#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

struct CaptureFrame {
    ComPtr<ID3D11Texture2D> texture;  // captured texture (on capture device)
    uint32_t width;
    uint32_t height;
};

class DDACapture {
public:
    DDACapture() = default;
    ~DDACapture() { Release(); }

    // Initialize: enumerate adapters to find the one rendering the desktop.
    // output_idx: which monitor (0 = primary)
    bool Init(int output_idx = 0);

    // Acquire next frame. Returns false on timeout (no update).
    bool AcquireFrame(CaptureFrame& frame, uint32_t timeout_ms = 100);

    // Release the current acquired frame back to the duplication API.
    void ReleaseFrame();

    ID3D11Device* GetDevice() const { return device_.Get(); }
    ID3D11DeviceContext* GetContext() const { return context_.Get(); }

    int Width() const { return width_; }
    int Height() const { return height_; }

private:
    void Release();

    ComPtr<ID3D11Device>            device_;
    ComPtr<ID3D11DeviceContext>     context_;
    ComPtr<IDXGIOutputDuplication>  duplication_;
    int width_  = 0;
    int height_ = 0;
    bool frame_acquired_ = false;
};
