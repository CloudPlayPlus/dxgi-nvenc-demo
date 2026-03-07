#include "nvenc_encoder.h"
#include <dxgi.h>
#include <stdio.h>

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
    DXGI_ADAPTER_DESC da_desc, db_desc;
    if (FAILED(a->QueryInterface(IID_PPV_ARGS(&da)))) return false;
    if (FAILED(b->QueryInterface(IID_PPV_ARGS(&db)))) return false;
    da->GetAdapter(&aa); db->GetAdapter(&ab);
    aa->GetDesc(&da_desc); ab->GetDesc(&db_desc);
    return da_desc.AdapterLuid.LowPart  == db_desc.AdapterLuid.LowPart &&
           da_desc.AdapterLuid.HighPart == db_desc.AdapterLuid.HighPart;
}

bool NvencEncoder::Init(ID3D11Device* capture_device, int width, int height, int fps)
{
    width_  = width;
    height_ = height;
    fps_    = fps;
    cap_device_ = capture_device;

    // --- D3D11 device on NVIDIA ---
    ComPtr<IDXGIAdapter> nv_adapter;
    if (!FindNvidiaAdapter(nv_adapter)) {
        fprintf(stderr, "[NVENC] No NVIDIA adapter found\n");
        return false;
    }
    HRESULT hr = D3D11CreateDevice(nv_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &enc_device_, nullptr, &enc_context_);
    if (FAILED(hr)) { fprintf(stderr, "[NVENC] D3D11CreateDevice failed: 0x%08X\n", hr); return false; }

    same_adapter_ = SameAdapter(capture_device, enc_device_.Get());
    printf("[NVENC] Same adapter: %s\n", same_adapter_ ? "yes" : "no (cross-adapter CPU roundtrip)");

    if (!same_adapter_ && !SetupCrossAdapter(capture_device)) return false;

    // --- FFmpeg D3D11VA hw device (wrap our NVIDIA device) ---
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

    // --- HW frames ctx (BGRA from DDA) ---
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

    // --- h264_nvenc codec (Sunshine low-latency params) ---
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
    codec_ctx_->max_b_frames   = 0;   // no B-frames for low latency
    codec_ctx_->gop_size       = fps_; // keyframe every 1s

    // Sunshine-aligned low-latency params
    // Ref: https://github.com/LizardByte/Sunshine/blob/master/src/nvenc/nvenc_base.cpp
    auto* priv = codec_ctx_->priv_data;
    av_opt_set(priv, "preset",        "p4",   0);  // balanced quality/speed (p1=fastest, p7=best)
    av_opt_set(priv, "tune",          "ull",  0);  // ultra-low latency
    av_opt_set(priv, "rc",            "cbr",  0);  // constant bitrate
    av_opt_set(priv, "profile",       "high", 0);
    av_opt_set(priv, "level",         "auto", 0);
    av_opt_set_int(priv, "b",         10000000, 0); // 10 Mbps
    av_opt_set_int(priv, "bufsize",   10000000, 0); // buffer = 1s at bitrate
    av_opt_set_int(priv, "rc-lookahead", 0,    0);  // no lookahead
    av_opt_set_int(priv, "no-scenecut", 1,     0);  // no scene cut detection
    av_opt_set_int(priv, "forced-idr",  1,     0);  // allow forced IDR

    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        char e[128]; av_strerror(ret, e, sizeof(e));
        fprintf(stderr, "[NVENC] avcodec_open2: %s\n", e);
        return false;
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

bool NvencEncoder::SetupCrossAdapter(ID3D11Device* cap_dev)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = width_; d.Height = height_;
    d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HRESULT hr = cap_dev->CreateTexture2D(&d, nullptr, &staging_tex_);
    if (FAILED(hr)) { fprintf(stderr, "[NVENC] staging texture failed: 0x%08X\n", hr); return false; }
    printf("[NVENC] Cross-adapter staging texture ready\n");
    return true;
}

bool NvencEncoder::CopyToEncoderTexture(ID3D11Texture2D* src)
{
    ComPtr<ID3D11DeviceContext> cap_ctx;
    cap_device_->GetImmediateContext(&cap_ctx);
    auto* dst = (ID3D11Texture2D*)hw_frame_->data[0];
    UINT  idx = (UINT)(intptr_t)hw_frame_->data[1];

    if (same_adapter_) {
        enc_context_->CopySubresourceRegion(dst, idx, 0, 0, 0, src, 0, nullptr);
    } else {
        cap_ctx->CopyResource(staging_tex_.Get(), src);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(cap_ctx->Map(staging_tex_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        enc_context_->UpdateSubresource(dst, idx, nullptr, m.pData, m.RowPitch, 0);
        cap_ctx->Unmap(staging_tex_.Get(), 0);
    }
    return true;
}

AVPacket* NvencEncoder::EncodeFrame(ID3D11Texture2D* src)
{
    if (!CopyToEncoderTexture(src)) return nullptr;
    hw_frame_->pts = pts_++;
    if (avcodec_send_frame(codec_ctx_, hw_frame_) < 0) return nullptr;

    AVPacket* pkt = av_packet_alloc();
    int ret = avcodec_receive_packet(codec_ctx_, pkt);
    if (ret == 0) return pkt;
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
    staging_tex_.Reset();
    enc_context_.Reset();
    enc_device_.Reset();
    cap_device_.Reset();
    if (hw_frame_)      av_frame_free(&hw_frame_);
    if (codec_ctx_)     avcodec_free_context(&codec_ctx_);
    if (hw_frame_ctx_)  av_buffer_unref(&hw_frame_ctx_);
    if (hw_device_ctx_) av_buffer_unref(&hw_device_ctx_);
}
