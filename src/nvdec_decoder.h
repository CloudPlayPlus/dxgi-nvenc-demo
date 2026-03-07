#pragma once
#include <d3d11.h>
#include <wrl/client.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

using Microsoft::WRL::ComPtr;

struct DecodedFrame {
    ComPtr<ID3D11Texture2D> texture;   // NV12 texture (shader-readable, NOT array)
    int width  = 0;
    int height = 0;
    bool valid = false;
};

class NvdecDecoder {
public:
    NvdecDecoder() = default;
    ~NvdecDecoder() { Release(); }

    // Init decoder on specified D3D11 device (should be NVIDIA dGPU)
    bool Init(ID3D11Device* device, int width, int height);

    // Feed encoded packet (AVPacket*), decode into DecodedFrame.
    // Returns false if no frame ready yet (need_more).
    bool Decode(AVPacket* packet, DecodedFrame& out);

    // Flush decoder
    void Flush();

    int Width()  const { return width_; }
    int Height() const { return height_; }

private:
    void Release();
    bool CopyFrameToShaderTexture(AVFrame* frame, DecodedFrame& out);
    bool EnsureShaderTexture(int w, int h);

    AVBufferRef*    hw_device_ctx_ = nullptr;
    AVCodecContext* codec_ctx_     = nullptr;
    AVFrame*        hw_frame_      = nullptr;
    AVFrame*        sw_frame_      = nullptr;

    // The device we operate on
    ComPtr<ID3D11Device>        device_;
    ComPtr<ID3D11DeviceContext> context_;

    // Single NV12 texture for shader rendering (recreated if size changes)
    ComPtr<ID3D11Texture2D>     nv12_tex_;
    int nv12_w_ = 0, nv12_h_ = 0;

    int width_  = 0;
    int height_ = 0;
};
