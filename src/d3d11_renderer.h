#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>

using Microsoft::WRL::ComPtr;

class D3D11Renderer {
public:
    D3D11Renderer()  = default;
    ~D3D11Renderer() { Release(); }

    // Init renderer: creates swapchain on the given window using the given device.
    // The device should be the same one used for decoding (NVIDIA) for zero-copy render.
    bool Init(HWND hwnd, ID3D11Device* device, int width, int height);

    // Render a decoded NV12 texture to the window.
    // nv12_tex: texture with DXGI_FORMAT_NV12, subresource 0=Y, 1=UV
    bool RenderNV12(ID3D11Texture2D* nv12_tex);

    // Update window title with perf stats
    void SetTitle(const std::string& title);

    // Handle resize
    void Resize(int width, int height);

    HWND Hwnd() const { return hwnd_; }

private:
    void Release();
    bool CreateShadersAndLayout();
    bool CreateSRVs(ID3D11Texture2D* nv12_tex);

    HWND hwnd_ = nullptr;

    ComPtr<ID3D11Device>           device_;
    ComPtr<ID3D11DeviceContext>    context_;
    ComPtr<IDXGISwapChain1>        swapchain_;
    ComPtr<ID3D11RenderTargetView> rtv_;

    ComPtr<ID3D11VertexShader>     vs_;
    ComPtr<ID3D11PixelShader>      ps_;
    ComPtr<ID3D11SamplerState>     sampler_;

    // SRVs for NV12: Y plane and UV plane
    ComPtr<ID3D11ShaderResourceView> srv_y_;
    ComPtr<ID3D11ShaderResourceView> srv_uv_;
    ID3D11Texture2D* last_tex_ = nullptr;  // track if SRVs need rebuild

    int width_  = 0;
    int height_ = 0;
};
