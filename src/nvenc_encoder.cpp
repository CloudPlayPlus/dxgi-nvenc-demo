#include "nvenc_encoder.h"
#include <dxgi.h>
#include <stdio.h>
#include <vector>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/error.h>
}

// Helper: find the NVIDIA adapter LUID
static bool FindNvidiaAdapter(ComPtr<IDXGIAdapter>& out_adapter)
{
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        // NVIDIA vendor ID = 0x10DE
        if (desc.VendorId == 0x10DE) {
            wprintf(L"[NVENC] Found NVIDIA adapter: %s\n", desc.Description);
            out_adapter = adapter;
            return true;
        }
    }
    fprintf(stderr, "[NVENC] No NVIDIA adapter found, will use first available\n");
    return false;
}

// Check if two D3D11 devices are on the same adapter (by comparing LUID)
static bool SameAdapter(ID3D11Device* a, ID3D11Device* b)
{
    ComPtr<IDXGIDevice> da, db;
    ComPtr<IDXGIAdapter> aa, ab;
    DXGI_ADAPTER_DESC da_desc, db_desc;

    if (FAILED(a->QueryInterface(IID_PPV_ARGS(&da)))) return false;
    if (FAILED(b->QueryInterface(IID_PPV_ARGS(&db)))) return false;
    if (FAILED(da->GetAdapter(&aa))) return false;
    if (FAILED(db->GetAdapter(&ab))) return false;
    aa->GetDesc(&da_desc);
    ab->GetDesc(&db_desc);

    return da_desc.AdapterLuid.LowPart  == db_desc.AdapterLuid.LowPart &&
           da_desc.AdapterLuid.HighPart == db_desc.AdapterLuid.HighPart;
}

bool NvencEncoder::Init(ID3D11Device* capture_device, int width, int height, int fps)
{
    width_  = width;
    height_ = height;
    fps_    = fps;
    capture_device_ = capture_device;

    // --- Step 1: Create D3D11 device on NVIDIA adapter for encoding ---
    ComPtr<IDXGIAdapter> nvidia_adapter;
    FindNvidiaAdapter(nvidia_adapter);

    HRESULT hr = D3D11CreateDevice(
        nvidia_adapter.Get(),
        nvidia_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION,
        &enc_device_, nullptr, &enc_context_
    );
    if (FAILED(hr)) {
        fprintf(stderr, "[NVENC] Failed to create encoder D3D11 device: 0x%08X\n", hr);
        return false;
    }

    // Check if we need cross-adapter copy
    same_adapter_ = SameAdapter(capture_device, enc_device_.Get());
    printf("[NVENC] Same adapter: %s\n", same_adapter_ ? "yes (no cross-adapter copy needed)" : "no (cross-adapter copy required)");

    if (!same_adapter_) {
        if (!SetupCrossAdapterIfNeeded(capture_device)) {
            return false;
        }
    }

    // --- Step 2: Create FFmpeg D3D11VA hardware device context on encoder device ---
    hw_device_ctx_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hw_device_ctx_) {
        fprintf(stderr, "[NVENC] Failed to allocate D3D11VA hw device ctx\n");
        return false;
    }

    AVHWDeviceContext*    hw_device = (AVHWDeviceContext*)hw_device_ctx_->data;
    AVD3D11VADeviceContext* d3d11va = (AVD3D11VADeviceContext*)hw_device->hwctx;
    d3d11va->device         = enc_device_.Get();
    d3d11va->device_context = enc_context_.Get();
    // Prevent FFmpeg from releasing our device
    d3d11va->device->AddRef();
    d3d11va->device_context->AddRef();

    int ret = av_hwdevice_ctx_init(hw_device_ctx_);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[NVENC] av_hwdevice_ctx_init failed: %s\n", errbuf);
        return false;
    }

    // --- Step 3: Create hw frames context (BGRA d3d11) ---
    hw_frame_ctx_ = av_hwframe_ctx_alloc(hw_device_ctx_);
    if (!hw_frame_ctx_) {
        fprintf(stderr, "[NVENC] Failed to allocate hw frames context\n");
        return false;
    }

    AVHWFramesContext* frames_ctx = (AVHWFramesContext*)hw_frame_ctx_->data;
    frames_ctx->format    = AV_PIX_FMT_D3D11;
    frames_ctx->sw_format = AV_PIX_FMT_BGRA;  // DDA captures BGRA
    frames_ctx->width     = width_;
    frames_ctx->height    = height_;
    frames_ctx->initial_pool_size = 4;

    ret = av_hwframe_ctx_init(hw_frame_ctx_);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[NVENC] av_hwframe_ctx_init failed: %s\n", errbuf);
        return false;
    }

    // --- Step 4: Find and open h264_nvenc codec ---
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) {
        fprintf(stderr, "[NVENC] h264_nvenc not found. Is FFmpeg built with NVENC support?\n");
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        fprintf(stderr, "[NVENC] Failed to allocate codec context\n");
        return false;
    }

    codec_ctx_->width     = width_;
    codec_ctx_->height    = height_;
    codec_ctx_->time_base = {1, fps_};
    codec_ctx_->framerate = {fps_, 1};
    codec_ctx_->pix_fmt   = AV_PIX_FMT_D3D11;
    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
    codec_ctx_->hw_frames_ctx = av_buffer_ref(hw_frame_ctx_);

    // NVENC options: low-latency preset
    av_opt_set(codec_ctx_->priv_data, "preset",  "p4",        0);
    av_opt_set(codec_ctx_->priv_data, "tune",    "ull",       0);  // ultra-low latency
    av_opt_set(codec_ctx_->priv_data, "rc",      "cbr",       0);
    av_opt_set_int(codec_ctx_->priv_data, "b",   4000000,     0);  // 4 Mbps

    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[NVENC] avcodec_open2 failed: %s\n", errbuf);
        return false;
    }
    printf("[NVENC] h264_nvenc encoder ready (%dx%d @ %dfps)\n", width_, height_, fps_);

    // Allocate HW frame for encoding
    hw_frame_ = av_frame_alloc();
    hw_frame_->format = AV_PIX_FMT_D3D11;
    hw_frame_->width  = width_;
    hw_frame_->height = height_;
    ret = av_hwframe_get_buffer(hw_frame_ctx_, hw_frame_, 0);
    if (ret < 0) {
        fprintf(stderr, "[NVENC] av_hwframe_get_buffer failed\n");
        return false;
    }

    packet_ = av_packet_alloc();
    return true;
}

bool NvencEncoder::SetupCrossAdapterIfNeeded(ID3D11Device* capture_device)
{
    // Create a CPU-readable staging texture on the CAPTURE device (iGPU).
    // We'll copy: DDA texture → staging → CPU map → encoder upload.
    // This is the "CPU roundtrip" fallback. A zero-copy path using
    // IDXGIResource1::CreateSharedHandle is possible but more complex.
    D3D11_TEXTURE2D_DESC staging_desc = {};
    staging_desc.Width          = width_;
    staging_desc.Height         = height_;
    staging_desc.MipLevels      = 1;
    staging_desc.ArraySize      = 1;
    staging_desc.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
    staging_desc.SampleDesc     = {1, 0};
    staging_desc.Usage          = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = capture_device->CreateTexture2D(&staging_desc, nullptr, &staging_texture_);
    if (FAILED(hr)) {
        fprintf(stderr, "[NVENC] Failed to create staging texture: 0x%08X\n", hr);
        return false;
    }
    printf("[NVENC] Cross-adapter: staging texture created (CPU roundtrip mode)\n");
    return true;
}

bool NvencEncoder::CopyToEncoderTexture(ID3D11Texture2D* src)
{
    // Get capture device context
    ComPtr<ID3D11DeviceContext> cap_ctx;
    capture_device_->GetImmediateContext(&cap_ctx);

    if (same_adapter_) {
        // Same GPU: direct copy into hw_frame_ texture
        ID3D11Texture2D* dst_tex = (ID3D11Texture2D*)hw_frame_->data[0];
        UINT array_idx = (UINT)(intptr_t)hw_frame_->data[1];
        enc_context_->CopySubresourceRegion(dst_tex, array_idx, 0, 0, 0, src, 0, nullptr);
        return true;
    }

    // Cross-adapter: copy DDA texture → staging (on iGPU) → map → upload to encoder device
    cap_ctx->CopyResource(staging_texture_.Get(), src);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = cap_ctx->Map(staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        fprintf(stderr, "[NVENC] Failed to map staging texture: 0x%08X\n", hr);
        return false;
    }

    // Upload to encoder device texture
    ID3D11Texture2D* dst_tex = (ID3D11Texture2D*)hw_frame_->data[0];
    UINT array_idx = (UINT)(intptr_t)hw_frame_->data[1];
    enc_context_->UpdateSubresource(dst_tex, array_idx, nullptr,
                                     mapped.pData, mapped.RowPitch, 0);
    cap_ctx->Unmap(staging_texture_.Get(), 0);
    return true;
}

void NvencEncoder::WritePackets(AVCodecContext* ctx, AVFrame* frame, FILE* outfile)
{
    int ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[NVENC] avcodec_send_frame: %s\n", errbuf);
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx, packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "[NVENC] avcodec_receive_packet: %s\n", errbuf);
            break;
        }
        if (outfile) {
            fwrite(packet_->data, 1, packet_->size, outfile);
            printf("[NVENC] Encoded packet: %d bytes (pts=%lld)\n",
                   packet_->size, (long long)packet_->pts);
        }
        av_packet_unref(packet_);
    }
}

bool NvencEncoder::EncodeFrame(ID3D11Texture2D* src_texture, FILE* outfile)
{
    if (!CopyToEncoderTexture(src_texture)) return false;

    hw_frame_->pts = pts_++;
    WritePackets(codec_ctx_, hw_frame_, outfile);
    return true;
}

void NvencEncoder::Flush(FILE* outfile)
{
    WritePackets(codec_ctx_, nullptr, outfile);
}

void NvencEncoder::Release()
{
    Flush(nullptr);
    staging_texture_.Reset();
    enc_context_.Reset();
    enc_device_.Reset();

    if (hw_frame_)     { av_frame_free(&hw_frame_); }
    if (packet_)       { av_packet_free(&packet_); }
    if (codec_ctx_)    { avcodec_free_context(&codec_ctx_); }
    if (hw_frame_ctx_) { av_buffer_unref(&hw_frame_ctx_); }
    if (hw_device_ctx_){ av_buffer_unref(&hw_device_ctx_); }
}
