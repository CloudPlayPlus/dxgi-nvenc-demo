#include "nvenc_encoder.h"
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <stdio.h>

// D3D11_RESOURCE_MISC_SHARED_CROSS_ADAPTER may not be defined in older SDKs
#ifndef D3D11_RESOURCE_MISC_SHARED_CROSS_ADAPTER
#define D3D11_RESOURCE_MISC_SHARED_CROSS_ADAPTER 0x80000L
#endif

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/error.h>
}

static bool FindNvidiaAdapter(ComPtr<IDXGIAdapter>& out)
{
    ComPtr<IDXGIFactory1> f;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f);
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
        if (d.VendorId == 0x10DE) { out = a; return true; }
    }
    return false;
}

static bool SameAdapter(ID3D11Device* a, ID3D11Device* b)
{
    ComPtr<IDXGIDevice> da, db;
    ComPtr<IDXGIAdapter> aa, ab;
    DXGI_ADAPTER_DESC da_d, db_d;
    if (FAILED(a->QueryInterface(IID_PPV_ARGS(&da)))) return false;
    if (FAILED(b->QueryInterface(IID_PPV_ARGS(&db)))) return false;
    da->GetAdapter(&aa); db->GetAdapter(&ab);
    aa->GetDesc(&da_d); ab->GetDesc(&db_d);
    return da_d.AdapterLuid.LowPart  == db_d.AdapterLuid.LowPart &&
           da_d.AdapterLuid.HighPart == db_d.AdapterLuid.HighPart;
}

bool NvencEncoder::Init(ID3D11Device* capture_device, int width, int height, int fps)
{
    width_  = width;
    height_ = height;
    fps_    = fps;
    cap_device_ = capture_device;

    // --- NVIDIA D3D11 device ---
    ComPtr<IDXGIAdapter> nv_adapter;
    if (!FindNvidiaAdapter(nv_adapter)) {
        fprintf(stderr, "[NVENC] No NVIDIA adapter found\n"); return false;
    }
    HRESULT hr = D3D11CreateDevice(nv_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &enc_device_, nullptr, &enc_context_);
    if (FAILED(hr)) { fprintf(stderr, "[NVENC] D3D11CreateDevice: 0x%08X\n", hr); return false; }

    same_adapter_ = SameAdapter(capture_device, enc_device_.Get());

    if (same_adapter_) {
        xfer_mode_ = XferMode::SameAdapter;
        printf("[NVENC] Transfer: same adapter (direct copy)\n");
    } else if (SetupZeroCopy(capture_device)) {
        xfer_mode_ = XferMode::ZeroCopy;
        printf("[NVENC] Transfer: zero-copy (NT shared handle + keyed mutex)\n");
    } else {
        printf("[NVENC] Transfer: CPU roundtrip fallback\n");
        if (!SetupCpuRoundtrip(capture_device)) return false;
        xfer_mode_ = XferMode::CpuRoundtrip;
    }

    // --- FFmpeg D3D11VA hw device (NVIDIA) ---
    hw_device_ctx_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    auto* hw_dev  = (AVHWDeviceContext*)hw_device_ctx_->data;
    auto* d3d11va = (AVD3D11VADeviceContext*)hw_dev->hwctx;
    d3d11va->device         = enc_device_.Get();
    d3d11va->device_context = enc_context_.Get();
    d3d11va->device->AddRef();
    d3d11va->device_context->AddRef();
    if (av_hwdevice_ctx_init(hw_device_ctx_) < 0) {
        fprintf(stderr, "[NVENC] av_hwdevice_ctx_init failed\n"); return false;
    }

    // --- HW frames ctx (BGRA) ---
    hw_frame_ctx_ = av_hwframe_ctx_alloc(hw_device_ctx_);
    auto* fc = (AVHWFramesContext*)hw_frame_ctx_->data;
    fc->format    = AV_PIX_FMT_D3D11;
    fc->sw_format = AV_PIX_FMT_BGRA;
    fc->width     = width_;
    fc->height    = height_;
    fc->initial_pool_size = 4;
    if (av_hwframe_ctx_init(hw_frame_ctx_) < 0) {
        fprintf(stderr, "[NVENC] av_hwframe_ctx_init failed\n"); return false;
    }

    // --- h264_nvenc (Sunshine low-latency params) ---
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) { fprintf(stderr, "[NVENC] h264_nvenc not found\n"); return false; }

    codec_ctx_ = avcodec_alloc_context3(codec);
    codec_ctx_->width          = width_;
    codec_ctx_->height         = height_;
    codec_ctx_->time_base      = {1, fps_};
    codec_ctx_->framerate      = {fps_, 1};
    codec_ctx_->pix_fmt        = AV_PIX_FMT_D3D11;
    codec_ctx_->hw_device_ctx  = av_buffer_ref(hw_device_ctx_);
    codec_ctx_->hw_frames_ctx  = av_buffer_ref(hw_frame_ctx_);
    codec_ctx_->flags         |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx_->max_b_frames   = 0;
    codec_ctx_->gop_size       = fps_;

    auto* p = codec_ctx_->priv_data;
    av_opt_set    (p, "preset",         "p4",  0);
    av_opt_set    (p, "tune",           "ull", 0);
    av_opt_set    (p, "rc",             "cbr", 0);
    av_opt_set    (p, "profile",        "high",0);
    av_opt_set_int(p, "b",         10000000,   0);
    av_opt_set_int(p, "bufsize",   10000000,   0);
    av_opt_set_int(p, "rc-lookahead",   0,     0);
    av_opt_set_int(p, "no-scenecut",    1,     0);
    av_opt_set_int(p, "forced-idr",     1,     0);

    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        char e[128]; av_strerror(ret, e, sizeof(e));
        fprintf(stderr, "[NVENC] avcodec_open2: %s\n", e); return false;
    }
    printf("[NVENC] h264_nvenc ready %dx%d @ %dfps, 10Mbps CBR, preset=p4/ull\n",
           width_, height_, fps_);

    hw_frame_ = av_frame_alloc();
    hw_frame_->format = AV_PIX_FMT_D3D11;
    hw_frame_->width  = width_;
    hw_frame_->height = height_;
    av_hwframe_get_buffer(hw_frame_ctx_, hw_frame_, 0);
    return true;
}

bool NvencEncoder::SetupZeroCopy(ID3D11Device* cap_dev)
{
    // D3D11_RESOURCE_MISC_SHARED_CROSS_ADAPTER: WDDM 2.0+ cross-adapter sharing.
    // This is the only path that works between physically separate adapters (Optimus).
    // Constraints: BindFlags must be 0 (no shader/RT binding on the Intel side),
    //              texture is row-major layout for cross-adapter compatibility.
    D3D11_TEXTURE2D_DESC d = {};
    d.Width      = width_;
    d.Height     = height_;
    d.MipLevels  = 1;
    d.ArraySize  = 1;
    d.Format     = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc = {1, 0};
    d.Usage      = D3D11_USAGE_DEFAULT;
    d.BindFlags  = 0;  // required for SHARED_CROSS_ADAPTER
    d.MiscFlags  = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                   D3D11_RESOURCE_MISC_SHARED_CROSS_ADAPTER;

    HRESULT hr = cap_dev->CreateTexture2D(&d, nullptr, &shared_tex_intel_);
    if (FAILED(hr)) {
        fprintf(stderr, "[NVENC] Zero-copy: CreateTexture2D (CROSS_ADAPTER) failed: 0x%08X\n", hr);
        return false;
    }

    // Create NT shared handle with GENERIC_ALL access
    ComPtr<IDXGIResource1> res1;
    hr = shared_tex_intel_.As(&res1);
    if (FAILED(hr)) { fprintf(stderr, "[NVENC] Zero-copy: IDXGIResource1 failed\n"); return false; }

    HANDLE h = nullptr;
    hr = res1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h);
    if (FAILED(hr) || !h) {
        fprintf(stderr, "[NVENC] Zero-copy: CreateSharedHandle (CROSS_ADAPTER) failed: 0x%08X\n", hr);
        return false;
    }

    // Open on NVIDIA device
    ComPtr<ID3D11Device1> enc_dev1;
    enc_device_.As(&enc_dev1);
    hr = enc_dev1->OpenSharedResource1(h, IID_PPV_ARGS(&shared_tex_nvidia_));
    CloseHandle(h);
    if (FAILED(hr)) {
        fprintf(stderr, "[NVENC] Zero-copy: OpenSharedResource1 (CROSS_ADAPTER) failed: 0x%08X\n", hr);
        return false;
    }

    printf("[NVENC] Zero-copy: CROSS_ADAPTER shared texture ready\n");
    return true;
}

bool NvencEncoder::SetupCpuRoundtrip(ID3D11Device* cap_dev)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = width_; d.Height = height_;
    d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HRESULT hr = cap_dev->CreateTexture2D(&d, nullptr, &staging_tex_);
    if (FAILED(hr)) { fprintf(stderr, "[NVENC] staging texture: 0x%08X\n", hr); return false; }
    return true;
}

bool NvencEncoder::CopyToEncoderTexture(ID3D11Texture2D* src)
{
    auto* dst     = (ID3D11Texture2D*)hw_frame_->data[0];
    UINT  dst_idx = (UINT)(intptr_t)hw_frame_->data[1];

    ComPtr<ID3D11DeviceContext> cap_ctx;
    cap_device_->GetImmediateContext(&cap_ctx);

    switch (xfer_mode_) {

    case XferMode::SameAdapter:
        // Direct copy on same GPU
        enc_context_->CopySubresourceRegion(dst, dst_idx, 0, 0, 0, src, 0, nullptr);
        return true;

    case XferMode::ZeroCopy: {
        // Step 1: Intel GPU copies DDA tex → shared tex (GPU blit, same adapter, fast)
        cap_ctx->CopyResource(shared_tex_intel_.Get(), src);
        cap_ctx->Flush();  // ensure GPU commands submitted before NVIDIA reads

        // Step 2: NVIDIA reads shared tex directly (cross-adapter, no CPU involved)
        enc_context_->CopySubresourceRegion(dst, dst_idx, 0, 0, 0,
                                             shared_tex_nvidia_.Get(), 0, nullptr);
        return true;
    }

    case XferMode::CpuRoundtrip:
    default: {
        // Fallback: CPU map/unmap
        cap_ctx->CopyResource(staging_tex_.Get(), src);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(cap_ctx->Map(staging_tex_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        enc_context_->UpdateSubresource(dst, dst_idx, nullptr, m.pData, m.RowPitch, 0);
        cap_ctx->Unmap(staging_tex_.Get(), 0);
        return true;
    }
    }
}

AVPacket* NvencEncoder::EncodeFrame(ID3D11Texture2D* src)
{
    if (!CopyToEncoderTexture(src)) return nullptr;
    hw_frame_->pts = pts_++;
    if (avcodec_send_frame(codec_ctx_, hw_frame_) < 0) return nullptr;
    AVPacket* pkt = av_packet_alloc();
    if (avcodec_receive_packet(codec_ctx_, pkt) == 0) return pkt;
    av_packet_free(&pkt);
    return nullptr;
}

AVPacket* NvencEncoder::Flush()
{
    avcodec_send_frame(codec_ctx_, nullptr);
    AVPacket* pkt = av_packet_alloc();
    if (avcodec_receive_packet(codec_ctx_, pkt) == 0) return pkt;
    av_packet_free(&pkt);
    return nullptr;
}

void NvencEncoder::Release()
{
    mutex_nvidia_.Reset();
    mutex_intel_.Reset();
    shared_tex_nvidia_.Reset();
    shared_tex_intel_.Reset();
    staging_tex_.Reset();
    enc_context_.Reset();
    enc_device_.Reset();
    cap_device_.Reset();
    if (hw_frame_)      av_frame_free(&hw_frame_);
    if (codec_ctx_)     avcodec_free_context(&codec_ctx_);
    if (hw_frame_ctx_)  av_buffer_unref(&hw_frame_ctx_);
    if (hw_device_ctx_) av_buffer_unref(&hw_device_ctx_);
}
