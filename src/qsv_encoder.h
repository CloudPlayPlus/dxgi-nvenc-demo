#pragma once
#include <d3d11.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswscale/swscale.h>
}

// QSV encoder: runs on the same Intel adapter as DDA capture.
// No cross-adapter copy needed — src texture is already on the Intel GPU.
class QsvEncoder {
public:
    bool Init(ID3D11Device* cap_device, int width, int height, int fps);
    AVPacket* EncodeFrame(ID3D11Texture2D* src);  // src on Intel device
    AVPacket* Flush();
    void Release();

    ID3D11Device* GetCapDevice() const { return cap_device_.Get(); }

private:
    ComPtr<ID3D11Device>        cap_device_;
    ComPtr<ID3D11DeviceContext> cap_context_;

    ComPtr<ID3D11Texture2D> staging_tex_;
    SwsContext*     sws_ctx_      = nullptr;

    AVBufferRef*    d3d11va_ctx_  = nullptr;
    AVBufferRef*    qsv_ctx_      = nullptr;
    AVBufferRef*    hw_frame_ctx_ = nullptr;
    AVCodecContext* codec_ctx_    = nullptr;
    AVFrame*        hw_frame_     = nullptr;
    AVFrame*        sw_frame_     = nullptr;
    int64_t         pts_          = 0;

    bool use_nv12_fallback_ = false;

    int width_  = 0;
    int height_ = 0;
    int fps_    = 60;
};
