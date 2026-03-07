#include "qsv_encoder.h"
#include <stdio.h>

extern "C" {
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

bool QsvEncoder::Init(ID3D11Device* cap_device, int width, int height, int fps)
{
    width_  = width;
    height_ = height;
    fps_    = fps;
    cap_device_ = cap_device;
    cap_device_->GetImmediateContext(&cap_context_);

    // --- Create QSV device directly (Intel VPL/MFX runtime) ---
    // Preferred: try to derive from DDA's D3D11VA device (same handle = no copy needed for upload)
    // Fallback: direct QSV creation with d3d11va child (different handle, but works for CPU-upload path)
    int ret = -1;

    d3d11va_ctx_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (d3d11va_ctx_) {
        auto* hw_dev  = (AVHWDeviceContext*)d3d11va_ctx_->data;
        auto* d3d11va = (AVD3D11VADeviceContext*)hw_dev->hwctx;
        d3d11va->device         = cap_device;
        d3d11va->device_context = cap_context_.Get();
        d3d11va->device->AddRef();
        d3d11va->device_context->AddRef();
        if (av_hwdevice_ctx_init(d3d11va_ctx_) >= 0) {
            ret = av_hwdevice_ctx_create_derived(
                &qsv_ctx_, AV_HWDEVICE_TYPE_QSV, d3d11va_ctx_, 0);
            if (ret < 0) {
                char e[128]; av_strerror(ret, e, sizeof(e));
                printf("[QSV] derive from D3D11VA failed (%s), trying direct create\n", e);
                av_buffer_unref(&d3d11va_ctx_);
            }
        }
    }

    if (ret < 0) {
        // Direct QSV device creation (VPL picks the Intel adapter automatically)
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "child_device_type", "d3d11va", 0);
        ret = av_hwdevice_ctx_create(&qsv_ctx_, AV_HWDEVICE_TYPE_QSV, nullptr, opts, 0);
        av_dict_free(&opts);
        if (ret < 0) {
            char e[128]; av_strerror(ret, e, sizeof(e));
            fprintf(stderr, "[QSV] av_hwdevice_ctx_create QSV failed: %s\n", e);
            return false;
        }
        printf("[QSV] QSV device ready (direct, child=d3d11va)\n");
    } else {
        printf("[QSV] QSV device ready (derived from Intel D3D11VA)\n");
    }

    // --- QSV NV12 frames context ---
    hw_frame_ctx_ = av_hwframe_ctx_alloc(qsv_ctx_);
    auto* fc = (AVHWFramesContext*)hw_frame_ctx_->data;
    fc->format             = AV_PIX_FMT_QSV;
    fc->sw_format          = AV_PIX_FMT_NV12;
    fc->width              = width_;
    fc->height             = height_;
    fc->initial_pool_size  = 32;
    ret = av_hwframe_ctx_init(hw_frame_ctx_);
    if (ret < 0) {
        char e[128]; av_strerror(ret, e, sizeof(e));
        fprintf(stderr, "[QSV] hwframe_ctx_init: %s\n", e); return false;
    }

    // --- h264_qsv, Sunshine-aligned low-latency params ---
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_qsv");
    if (!codec) { fprintf(stderr, "[QSV] h264_qsv encoder not found\n"); return false; }

    codec_ctx_ = avcodec_alloc_context3(codec);
    codec_ctx_->width          = width_;
    codec_ctx_->height         = height_;
    codec_ctx_->time_base      = {1, fps_};
    codec_ctx_->framerate      = {fps_, 1};
    codec_ctx_->pix_fmt        = AV_PIX_FMT_QSV;
    codec_ctx_->hw_device_ctx  = av_buffer_ref(qsv_ctx_);
    codec_ctx_->hw_frames_ctx  = av_buffer_ref(hw_frame_ctx_);
    codec_ctx_->flags         |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx_->max_b_frames   = 0;
    codec_ctx_->gop_size       = fps_;
    codec_ctx_->bit_rate       = 10000000;
    codec_ctx_->rc_max_rate    = 10000000;
    codec_ctx_->rc_buffer_size = 10000000;

    auto* p = codec_ctx_->priv_data;
    av_opt_set    (p, "preset",       "veryfast", 0);
    av_opt_set    (p, "profile",      "high",     0);
    av_opt_set    (p, "rc_mode",      "cbr",      0);
    av_opt_set_int(p, "async_depth",  1,          0);  // critical for low latency
    av_opt_set_int(p, "look_ahead",   0,          0);

    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        char e[128]; av_strerror(ret, e, sizeof(e));
        fprintf(stderr, "[QSV] avcodec_open2: %s\n", e); return false;
    }
    printf("[QSV] h264_qsv ready %dx%d @ %dfps, 10Mbps CBR, veryfast async_depth=1\n",
           width_, height_, fps_);

    // --- Staging texture for CPU readback ---
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = width_; sd.Height = height_;
    sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    cap_device_->CreateTexture2D(&sd, nullptr, &staging_tex_);

    // --- swscale context: BGRA → NV12 (SIMD-accelerated) ---
    sws_ctx_ = sws_getContext(
        width_, height_, AV_PIX_FMT_BGRA,
        width_, height_, AV_PIX_FMT_NV12,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx_) { fprintf(stderr, "[QSV] sws_getContext failed\n"); return false; }

    // --- hw frame allocation ---
    hw_frame_ = av_frame_alloc();
    hw_frame_->format = AV_PIX_FMT_QSV;
    hw_frame_->width  = width_;
    hw_frame_->height = height_;
    av_hwframe_get_buffer(hw_frame_ctx_, hw_frame_, 0);

    // --- sw NV12 frame (reuse each frame) ---
    sw_frame_ = av_frame_alloc();
    sw_frame_->format = AV_PIX_FMT_NV12;
    sw_frame_->width  = width_;
    sw_frame_->height = height_;
    av_frame_get_buffer(sw_frame_, 64);  // 64-byte aligned

    return true;
}

AVPacket* QsvEncoder::EncodeFrame(ID3D11Texture2D* src)
{
    // Step 1: GPU copy DDA tex → staging (same Intel adapter)
    cap_context_->CopyResource(staging_tex_.Get(), src);

    // Step 2: CPU map staging → BGRA pointer
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(cap_context_->Map(staging_tex_.Get(), 0, D3D11_MAP_READ, 0, &m))) return nullptr;

    // Step 3: swscale BGRA → NV12 (SIMD, ~2-3ms at 2560x1600)
    const uint8_t* src_planes[1] = { (const uint8_t*)m.pData };
    int src_strides[1] = { (int)m.RowPitch };
    sws_scale(sws_ctx_, src_planes, src_strides, 0, height_,
              sw_frame_->data, sw_frame_->linesize);

    cap_context_->Unmap(staging_tex_.Get(), 0);

    // Step 4: upload NV12 sw frame → QSV hw frame
    if (av_hwframe_transfer_data(hw_frame_, sw_frame_, 0) < 0) return nullptr;

    // Step 5: QSV encode
    hw_frame_->pts = pts_++;
    if (avcodec_send_frame(codec_ctx_, hw_frame_) < 0) return nullptr;
    AVPacket* pkt = av_packet_alloc();
    if (avcodec_receive_packet(codec_ctx_, pkt) == 0) return pkt;
    av_packet_free(&pkt);
    return nullptr;
}

AVPacket* QsvEncoder::Flush()
{
    avcodec_send_frame(codec_ctx_, nullptr);
    AVPacket* pkt = av_packet_alloc();
    if (avcodec_receive_packet(codec_ctx_, pkt) == 0) return pkt;
    av_packet_free(&pkt);
    return nullptr;
}

void QsvEncoder::Release()
{
    if (sws_ctx_)       sws_freeContext(sws_ctx_);
    staging_tex_.Reset();
    cap_context_.Reset();
    cap_device_.Reset();
    if (sw_frame_)      av_frame_free(&sw_frame_);
    if (hw_frame_)      av_frame_free(&hw_frame_);
    if (codec_ctx_)     avcodec_free_context(&codec_ctx_);
    if (hw_frame_ctx_)  av_buffer_unref(&hw_frame_ctx_);
    if (qsv_ctx_)       av_buffer_unref(&qsv_ctx_);
    if (d3d11va_ctx_)   av_buffer_unref(&d3d11va_ctx_);
}
