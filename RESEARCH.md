# DDA + NVENC/QSV 串流 Pipeline 调研记录

> 环境：Intel Arc（iGPU，主显） + RTX 4060 Laptop（dGPU）@ 2560x1600 60fps
> 日期：2026-03-07 ~ 2026-03-08

---

## 一、最终结论

| 指标 | NVENC (delay=0) | QSV (async_depth=1) |
|---|---|---|
| 端到端延迟（帧就绪→解码输出） | **10.94 ms** | 16.36 ms |
| 编码组成 | CPU xfer 3ms + RTX4060 HW 6ms | CPU xfer 3ms + 上传 3ms + IntelArc HW 10ms |
| 跨 Adapter 拷贝 | CPU roundtrip（不可避免） | CPU roundtrip（同上） |

**NVENC 胜出约 5ms。RTX 4060 硬件编码速度是 Intel Arc 的约 1.6x。**

---

## 二、调研过程中的错误与纠正

### 错误 1：zero-copy 跨 adapter 可行性（过于乐观）

**最初假设：** 可以用 D3D11 shared handle 把 Intel DDA 纹理直接共享给 NVIDIA NVENC，避免 CPU 拷贝。

**实际情况：**

| 方案 | 结果 | 原因 |
|---|---|---|
| `SHARED` legacy handle + `OpenSharedResource` | ❌ E_INVALIDARG | D3D11 legacy handle 仅限同一 adapter |
| `SHARED_NTHANDLE + KEYEDMUTEX` + `OpenSharedResource1` | ❌ E_INVALIDARG | Intel Arc 驱动未实现 |
| `SHARED_NTHANDLE + SHARED_CROSS_ADAPTER` + `CreateTexture2D` | ❌ E_INVALIDARG | Intel Arc WDDM 驱动不支持此 flag |

**结论：** 在 Optimus 笔记本（Intel Arc + NVIDIA）上，D3D11 任何形式的 cross-adapter resource sharing 均被驱动层拒绝。CPU roundtrip 是唯一可行路径。Sunshine 在同类硬件上也是如此处理。

---

### 错误 2：QSV 编码慢（初判错误原因）

**第一轮 bench（QSV 走 swscale NV12 转换路径）：**
```
NVENC encode: 5.6ms
QSV  encode: 22.9ms  ← 误判为"Intel Arc 编码能力差 4 倍"
```

**实际原因：** 22ms 里有 ~7ms 是 CPU swscale BGRA→NV12 转换，不是硬件编码慢。

**用户指出：** "为啥要 swscale？不能直接喂给 FFmpeg encoder？"

**修正：** 改 `sw_format = BGRA`（与 NVENC 一样），h264_qsv 通过 Intel VPP 内部完成色彩转换。修正后 QSV encode 降至 16ms。

**QSV BGRA 路径 breakdown：**
```
map=3.1ms  conv=0ms  upload=2.6ms  send=10.7ms  total=16.4ms
```

---

### 错误 3：cap_ms 把 DDA 等待混入计时（误导性对比）

**问题：** bench 测到：
```
NVENC cap: 11.75ms
QSV  cap:  1.72ms
```

用户问："为啥 NVENC capture 这么久？"

**实际原因：** `AcquireFrame(timeout=16ms)` 会阻塞等待下一次桌面刷新。
- NVENC 编码快（~5ms）→ 完成后距下次刷新还有 ~11ms → 等了 11ms
- QSV 编码慢（~15ms）→ 完成后刷新快到了 → 等了 1ms

两者 cap+enc 都约等于 16ms（一个 vsync 周期），这是正常的，不代表 NVENC 抓帧慢。

**修正：** 把 `dda_wait_ms`（DDA 阻塞等待时间）和 `encode_ms`（帧就绪→码流就绪）拆开，分别报告。

---

### 错误 4：NVENC "encode=4ms" 是虚假数字（最关键的错误）

用户问："应该端到端从拿到一帧到解码器解码出一帧来测。"

**问题：** 不加 `delay=0` 时 NVENC 的 breakdown：
```
xfer=3.5ms  send=0.37ms  recv=0.00ms  total=4ms
```

看起来 NVENC 只要 4ms，但实际上：
- `avcodec_send_frame` 异步提交帧到 NVENC 队列，**立即返回**（0.37ms）
- `avcodec_receive_packet` 拿到的是 **N 帧之前**已经编好的包（0ms）
- 本帧的 GPU 编码在后台流水线中进行，完全不计入测量

**真实 e2e 验证（用 pts 追踪每帧从 DDA 就绪到解码输出）：**
```
不加 delay=0：NVENC e2e = 38ms  （流水线堆积了 ~2帧 × 16ms）
加了 delay=0：NVENC e2e = 10.94ms （同步，无流水线堆积）
QSV async_depth=1：e2e = 16.36ms  （本来就同步）
```

**根本原因：** h264_nvenc 默认 `delay = INT_MAX`，FFmpeg 会将其设为 `surfaces - 1`（通常 2~4），造成多帧流水线延迟。

**Sunshine 的处理：** PR #507 专门修了这个问题，明确说明：

> "If delay is not set manually, it defaults to INT_MAX, which is then reduced to
> number of surfaces - 1, which can potentially cause encode delay."

Sunshine 显式设置 `delay=0`（对应 FFmpeg opt `delay=0`），确保每帧同步编码。

**正确 NVENC 低延迟参数（与 Sunshine 对齐）：**
```
preset=p4
tune=ull
rc=cbr
profile=high
b=10000000 (10Mbps)
bufsize=10000000
rc-lookahead=0
no-scenecut=1
forced-idr=1
max_b_frames=0
gop=fps
delay=0          ← 关键！必须显式设置，否则流水线堆积造成高延迟
```

---

## 三、正确的延迟测量方法

**错误方法（只测 encode_ms）：**
```
cap_ms = AcquireFrame 耗时（含 DDA 等待，随编码速度变化）
enc_ms = EncodeFrame 耗时（NVENC 异步时严重低估真实延迟）
```

**正确方法（E2E pts 追踪）：**
1. DDA `AcquireFrame` 完成时记录 `t_cap` 和 `pts`
2. `pts` 作为编码器输入 pts
3. `avcodec_receive_packet` 拿到 packet，读取 `pkt->pts`
4. 送给解码器，`avcodec_receive_frame` 完成时记录 `t_dec`
5. `e2e = t_dec - cap_time[pkt->pts]`

此方法正确处理异步编码器的 pts 错位问题。

---

## 四、各组件实测性能（2560×1600 @ 60fps）

### 编码器
| 步骤 | NVENC (delay=0) | QSV BGRA (async_depth=1) |
|---|---|---|
| CPU xfer (Intel staging map) | ~3ms | ~3ms |
| 色彩转换 | GPU 内部（BGRA→YUV） | Intel VPP 内部（BGRA→NV12） |
| 上传到编码器 | UpdateSubresource ~0.5ms | hwframe_transfer ~2.6ms |
| 硬件编码 | RTX 4060 ~6ms | Intel Arc ~10ms |
| **总编码延迟** | **~9ms** | **~15ms** |

### 解码器
| 解码器 | 耗时 |
|---|---|
| h264 + d3d11va (NVIDIA) | ~0.2ms |
| h264_cuvid | 不兼容 D3D11VA device context，放弃 |

### 端到端（含 DDA 等待）
| Pipeline | E2E avg |
|---|---|
| NVENC + NVDEC (delay=0) | **10.94 ms** |
| QSV BGRA + NVDEC | 16.36 ms |

---

## 五、NV12 纹理相关坑

### SRV UV plane 创建
```cpp
// 错误写法：
sd.Texture2D.MostDetailedMip = 1;  // 以为 UV plane 是 subresource 1

// 正确写法：
sd.Format = DXGI_FORMAT_R8G8_UNORM;
sd.Texture2D.MostDetailedMip = 0;  // D3D 根据 format 自动选 plane
// R8_UNORM   → Y plane
// R8G8_UNORM → UV plane（自动）
```

### NV12 Array Texture 子资源索引
```cpp
// 错误写法（之前用的）：
src_tex, array_idx * 2      // Y plane
src_tex, array_idx * 2 + 1 // UV plane（错误！）

// 正确写法（D3D11 NV12 planar 格式）：
D3D11_TEXTURE2D_DESC desc; src_tex->GetDesc(&desc);
UINT array_size = desc.ArraySize;
src_tex, array_idx              // Y plane
src_tex, array_idx + array_size // UV plane
```

---

## 七、Live Pipeline vs Benchmark 的延迟差异

**现象：**
- 纯 benchmark（无 renderer）: NVENC e2e = 10.94ms
- Live pipeline（有 renderer）: NVENC e2e = 14~20ms，encode 从 9ms 抖动到 17ms

**原因：** Renderer 和 NVENC 共用 RTX 4060，互相竞争 GPU 资源。
- NVENC `send` 时间从 6ms 增到 6~17ms（GPU 调度等待）
- CPU xfer 从 3ms 增到 4~8ms（GPU 命令队列更长）

**Sunshine 的解决方案：** Capture+Encode 在独立的 server 进程，不含本地 renderer，
本地桌面仍由 iGPU 渲染，NVENC 独占 dGPU。

**结论：** 实际串流产品里 Encoder 不应和 Renderer 共 GPU。此 demo 的 renderer
仅用于验证，实测端到端延迟应以 `--e2e` benchmark 结果（10.94ms）为准。

---

## 六、硬件与驱动限制汇总

| 限制 | 描述 |
|---|---|
| D3D11 cross-adapter shared handle | Intel Arc + NVIDIA Optimus 下全部方案均不可用（3种都试过） |
| h264_cuvid + D3D11VA device context | 不兼容，需改用标准 `h264` decoder + d3d11va hwaccel |
| QSV derive from D3D11VA | `av_hwdevice_ctx_create_derived` QSV 失败（Error setting child device handle: -16）；需直接 `av_hwdevice_ctx_create` |
| Intel Arc `SHARED_CROSS_ADAPTER` | CreateTexture2D 直接失败，驱动不支持 |
