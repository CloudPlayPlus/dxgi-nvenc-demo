#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libavformat/avformat.h>
}

using Microsoft::WRL::ComPtr;

class NvencEncoder {
public:
    NvencEncoder() = default;
    ~NvencEncoder() { Release(); }

    // Initialize encoder.
    // capture_device: the D3D11 device used for DDA capture (may be iGPU)
    // width, height: frame dimensions
    // fps: target frame rate
    bool Init(ID3D11Device* capture_device, int width, int height, int fps = 30);

    // Encode a captured D3D11 texture.
    // The texture may be on a different adapter; we handle cross-adapter copy internally.
    // outfile: write encoded H264 to this file (nullptr = don't write)
    bool EncodeFrame(ID3D11Texture2D* src_texture, FILE* outfile);

    void Flush(FILE* outfile);

private:
    void Release();
    bool SetupCrossAdapterIfNeeded(ID3D11Device* capture_device);
    bool CopyToEncoderTexture(ID3D11Texture2D* src);
    void WritePackets(AVCodecContext* ctx, AVFrame* frame, FILE* outfile);

    // FFmpeg encoder context (always on NVIDIA dGPU via d3d11va)
    AVBufferRef*    hw_device_ctx_  = nullptr;
    AVBufferRef*    hw_frame_ctx_   = nullptr;
    AVCodecContext* codec_ctx_      = nullptr;
    AVFrame*        hw_frame_       = nullptr;
    AVPacket*       packet_         = nullptr;

    int width_  = 0;
    int height_ = 0;
    int fps_    = 30;
    int64_t pts_ = 0;

    // Encoder D3D11 device (NVIDIA dGPU)
    ComPtr<ID3D11Device>        enc_device_;
    ComPtr<ID3D11DeviceContext> enc_context_;

    // Cross-adapter: staging texture on capture device (iGPU)
    // Used to CPU-copy from iGPU texture to upload into encoder device
    ComPtr<ID3D11Texture2D>     staging_texture_;   // on capture_device (iGPU), CPU-readable
    ComPtr<ID3D11Device>        capture_device_;    // reference, not owned

    bool same_adapter_ = false;  // true if capture and encode are on same GPU
};
