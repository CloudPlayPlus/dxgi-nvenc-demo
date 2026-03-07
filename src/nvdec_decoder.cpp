#include "nvdec_decoder.h"
#include <stdio.h>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/error.h>
}

bool NvdecDecoder::Init(ID3D11Device* device, int width, int height)
{
    width_  = width;
    height_ = height;
    device_ = device;
    device_->GetImmediateContext(&context_);

    // --- FFmpeg D3D11VA hw device context (wrap our existing device) ---
    hw_device_ctx_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hw_device_ctx_) {
        fprintf(stderr, "[NVDEC] Failed to alloc hw device ctx\n");
        return false;
    }

    auto* hw_device  = (AVHWDeviceContext*)hw_device_ctx_->data;
    auto* d3d11va    = (AVD3D11VADeviceContext*)hw_device->hwctx;
    d3d11va->device         = device_.Get();
    d3d11va->device_context = context_.Get();
    d3d11va->device->AddRef();
    d3d11va->device_context->AddRef();

    int ret = av_hwdevice_ctx_init(hw_device_ctx_);
    if (ret < 0) {
        char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[NVDEC] av_hwdevice_ctx_init: %s\n", errbuf);
        return false;
    }

    // Use standard h264 decoder with d3d11va hw acceleration
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "[NVDEC] No H264 decoder found\n");
        return false;
    }
    printf("[NVDEC] Using decoder '%s' with d3d11va hwaccel\n", codec->name);

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) return false;

    codec_ctx_->width              = width_;
    codec_ctx_->height             = height_;
    codec_ctx_->hw_device_ctx      = av_buffer_ref(hw_device_ctx_);
    codec_ctx_->thread_count       = 1;
    codec_ctx_->flags             |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx_->flags2            |= AV_CODEC_FLAG2_FAST;

    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[NVDEC] avcodec_open2: %s\n", errbuf);
        return false;
    }
    printf("[NVDEC] Decoder '%s' ready (%dx%d)\n", codec->name, width_, height_);

    hw_frame_ = av_frame_alloc();
    sw_frame_ = av_frame_alloc();
    return true;
}

bool NvdecDecoder::EnsureShaderTexture(int w, int h)
{
    if (nv12_tex_ && nv12_w_ == w && nv12_h_ == h) return true;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width          = w;
    desc.Height         = h;
    desc.MipLevels      = 1;
    desc.ArraySize      = 1;
    desc.Format         = DXGI_FORMAT_NV12;
    desc.SampleDesc     = {1, 0};
    desc.Usage          = D3D11_USAGE_DEFAULT;
    desc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &nv12_tex_);
    if (FAILED(hr)) {
        fprintf(stderr, "[NVDEC] Failed to create NV12 shader texture: 0x%08X\n", hr);
        return false;
    }
    nv12_w_ = w;
    nv12_h_ = h;
    return true;
}

bool NvdecDecoder::CopyFrameToShaderTexture(AVFrame* frame, DecodedFrame& out)
{
    // frame->data[0] = ID3D11Texture2D* (NV12 array texture)
    // frame->data[1] = array index (as intptr_t)
    auto*  src_tex   = (ID3D11Texture2D*)frame->data[0];
    UINT   array_idx = (UINT)(intptr_t)frame->data[1];

    int w = frame->width;
    int h = frame->height;

    if (!EnsureShaderTexture(w, h)) return false;

    // For NV12 array textures, subresource layout is:
    //   Y  plane of slice k: subresource = k
    //   UV plane of slice k: subresource = k + ArraySize
    D3D11_TEXTURE2D_DESC src_desc;
    src_tex->GetDesc(&src_desc);
    UINT array_size = src_desc.ArraySize;

    context_->CopySubresourceRegion(
        nv12_tex_.Get(), 0,              // dst: Y plane (subresource 0)
        0, 0, 0,
        src_tex, array_idx,              // src: Y plane of this slice
        nullptr
    );
    context_->CopySubresourceRegion(
        nv12_tex_.Get(), 1,              // dst: UV plane (subresource 1)
        0, 0, 0,
        src_tex, array_idx + array_size, // src: UV plane of this slice
        nullptr
    );

    out.texture = nv12_tex_;
    out.width   = w;
    out.height  = h;
    out.valid   = true;
    return true;
}

bool NvdecDecoder::Decode(AVPacket* packet, DecodedFrame& out)
{
    out.valid = false;

    int ret = avcodec_send_packet(codec_ctx_, packet);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[NVDEC] avcodec_send_packet: %s\n", errbuf);
        return false;
    }

    ret = avcodec_receive_frame(codec_ctx_, hw_frame_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return false; // need more data
    }
    if (ret < 0) {
        char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[NVDEC] avcodec_receive_frame: %s\n", errbuf);
        return false;
    }

    // hw_frame_->format should be AV_PIX_FMT_D3D11
    if (hw_frame_->format != AV_PIX_FMT_D3D11) {
        fprintf(stderr, "[NVDEC] Unexpected frame format: %d\n", hw_frame_->format);
        av_frame_unref(hw_frame_);
        return false;
    }

    bool ok = CopyFrameToShaderTexture(hw_frame_, out);
    av_frame_unref(hw_frame_);
    return ok;
}

void NvdecDecoder::Flush()
{
    if (!codec_ctx_) return;
    avcodec_send_packet(codec_ctx_, nullptr);
    DecodedFrame dummy;
    while (avcodec_receive_frame(codec_ctx_, hw_frame_) >= 0) {
        av_frame_unref(hw_frame_);
    }
}

void NvdecDecoder::Release()
{
    Flush();
    nv12_tex_.Reset();
    if (hw_frame_)    av_frame_free(&hw_frame_);
    if (sw_frame_)    av_frame_free(&sw_frame_);
    if (codec_ctx_)   avcodec_free_context(&codec_ctx_);
    if (hw_device_ctx_) av_buffer_unref(&hw_device_ctx_);
    context_.Reset();
    device_.Reset();
}
