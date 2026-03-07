#pragma once
#include <d3d11.h>
#include <wrl/client.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

using Microsoft::WRL::ComPtr;

class NvencEncoder {
public:
    NvencEncoder()  = default;
    ~NvencEncoder() { Release(); }

    // Init NVENC encoder on the NVIDIA dGPU.
    // capture_device: the device used for DDA (may be iGPU).
    bool Init(ID3D11Device* capture_device, int width, int height, int fps = 60);

    // Encode a DDA-captured texture. Returns an AVPacket* (caller must av_packet_free).
    // Returns nullptr if no packet ready (shouldn't happen in CBR low-latency mode).
    AVPacket* EncodeFrame(ID3D11Texture2D* src_texture);

    // Flush and return remaining packets. Returns nullptr when done.
    AVPacket* Flush();

    // Access the NVIDIA device (for decoder/renderer to share)
    ID3D11Device*        GetEncDevice()  const { return enc_device_.Get(); }
    ID3D11DeviceContext* GetEncContext() const { return enc_context_.Get(); }

    int Width()  const { return width_; }
    int Height() const { return height_; }

private:
    void Release();
    bool SetupZeroCopy(ID3D11Device* cap_dev);
    bool SetupCpuRoundtrip(ID3D11Device* cap_dev);
    bool CopyToEncoderTexture(ID3D11Texture2D* src);

    AVBufferRef*    hw_device_ctx_ = nullptr;
    AVBufferRef*    hw_frame_ctx_  = nullptr;
    AVCodecContext* codec_ctx_     = nullptr;
    AVFrame*        hw_frame_      = nullptr;
    int64_t         pts_           = 0;

    ComPtr<ID3D11Device>        enc_device_;
    ComPtr<ID3D11DeviceContext> enc_context_;
    ComPtr<ID3D11Device>        cap_device_;   // capture device (iGPU)

    // CPU roundtrip (fallback)
    ComPtr<ID3D11Texture2D>     staging_tex_;

    // Zero-copy shared handle path
    ComPtr<ID3D11Texture2D>     shared_tex_intel_;   // BGRA shared tex on Intel (write side)
    ComPtr<ID3D11Texture2D>     shared_tex_nvidia_;  // BGRA shared tex on NVIDIA (read side)
    ComPtr<IDXGIKeyedMutex>     mutex_intel_;        // sync: Intel writes with key 0
    ComPtr<IDXGIKeyedMutex>     mutex_nvidia_;       // sync: NVIDIA reads with key 1

    enum class XferMode { SameAdapter, ZeroCopy, CpuRoundtrip };
    XferMode xfer_mode_ = XferMode::CpuRoundtrip;

    bool    same_adapter_ = false;
    int     width_  = 0;
    int     height_ = 0;
    int     fps_    = 60;
};
