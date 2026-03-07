#include "d3d11_renderer.h"
#include <d3dcompiler.h>
#include <stdio.h>

#pragma comment(lib, "d3dcompiler.lib")

// ---- HLSL shaders (embedded) -----------------------------------------------

// Fullscreen triangle (no vertex buffer, 3 vertices via SV_VertexID)
static const char* kVS = R"hlsl(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD;
};
VSOut main(uint id : SV_VertexID) {
    // Fullscreen triangle trick
    float2 uv  = float2((id & 1) * 2.0, (id >> 1) * 2.0);
    float4 pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    VSOut o;
    o.pos = pos;
    o.uv  = uv;
    return o;
}
)hlsl";

// NV12 → RGB conversion
// Y plane:  R8_UNORM  (srv t0)
// UV plane: R8G8_UNORM (srv t1) - Cb in R, Cr in G
static const char* kPS = R"hlsl(
Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState      samp  : register(s0);

struct PSIn {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD;
};

float4 main(PSIn i) : SV_TARGET {
    float  y  = texY.Sample(samp, i.uv).r;
    float2 uv = texUV.Sample(samp, i.uv).rg - 0.5;
    // BT.601 YCbCr -> RGB
    float r = clamp(y + 1.402  * uv.y,                    0, 1);
    float g = clamp(y - 0.3441 * uv.x - 0.7141 * uv.y,   0, 1);
    float b = clamp(y + 1.772  * uv.x,                    0, 1);
    return float4(r, g, b, 1.0);
}
)hlsl";

// ---- Helpers ----------------------------------------------------------------

static ComPtr<ID3DBlob> CompileShader(const char* src, const char* entry, const char* target)
{
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                             entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                             &blob, &err);
    if (FAILED(hr)) {
        if (err) fprintf(stderr, "[Renderer] Shader error: %s\n", (char*)err->GetBufferPointer());
        return nullptr;
    }
    return blob;
}

// ---- D3D11Renderer ----------------------------------------------------------

bool D3D11Renderer::Init(HWND hwnd, ID3D11Device* device, int width, int height)
{
    hwnd_    = hwnd;
    device_  = device;
    width_   = width;
    height_  = height;
    device_->GetImmediateContext(&context_);

    // --- Create swap chain ---
    ComPtr<IDXGIDevice2>  dxgi_device;
    ComPtr<IDXGIAdapter>  adapter;
    ComPtr<IDXGIFactory2> factory;
    device_->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    dxgi_device->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width       = width_;
    sd.Height      = height_;
    sd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc  = {1, 0};
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags       = 0;

    HRESULT hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &sd, nullptr, nullptr, &swapchain_);
    if (FAILED(hr)) {
        fprintf(stderr, "[Renderer] CreateSwapChain failed: 0x%08X\n", hr);
        return false;
    }

    // --- Create RTV ---
    ComPtr<ID3D11Texture2D> back_buf;
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&back_buf));
    hr = device_->CreateRenderTargetView(back_buf.Get(), nullptr, &rtv_);
    if (FAILED(hr)) { fprintf(stderr, "[Renderer] CreateRTV failed\n"); return false; }

    // --- Shaders ---
    if (!CreateShadersAndLayout()) return false;

    // --- Sampler ---
    D3D11_SAMPLER_DESC sd2 = {};
    sd2.Filter   = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd2.AddressU = sd2.AddressV = sd2.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device_->CreateSamplerState(&sd2, &sampler_);

    printf("[Renderer] D3D11 renderer ready (%dx%d)\n", width_, height_);
    return true;
}

bool D3D11Renderer::CreateShadersAndLayout()
{
    auto vs_blob = CompileShader(kVS, "main", "vs_5_0");
    auto ps_blob = CompileShader(kPS, "main", "ps_5_0");
    if (!vs_blob || !ps_blob) return false;

    HRESULT hr = device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs_);
    if (FAILED(hr)) { fprintf(stderr, "[Renderer] CreateVertexShader failed\n"); return false; }
    hr = device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &ps_);
    if (FAILED(hr)) { fprintf(stderr, "[Renderer] CreatePixelShader failed\n"); return false; }
    return true;
}

bool D3D11Renderer::CreateSRVs(ID3D11Texture2D* nv12_tex)
{
    if (nv12_tex == last_tex_) return true;  // already created
    srv_y_.Reset();
    srv_uv_.Reset();

    // Y plane: subresource 0, format R8_UNORM
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels       = 1;
    sd.Texture2D.MostDetailedMip = 0;

    sd.Format = DXGI_FORMAT_R8_UNORM;
    HRESULT hr = device_->CreateShaderResourceView(nv12_tex, &sd, &srv_y_);
    if (FAILED(hr)) {
        fprintf(stderr, "[Renderer] CreateSRV (Y) failed: 0x%08X\n", hr);
        return false;
    }

    // UV plane: same MipLevels/MostDetailedMip, just different format.
    // D3D11 selects the correct plane based on format:
    //   R8_UNORM   → Y plane
    //   R8G8_UNORM → UV plane (Cb in R, Cr in G)
    sd.Format = DXGI_FORMAT_R8G8_UNORM;
    sd.Texture2D.MostDetailedMip = 0;
    sd.Texture2D.MipLevels = 1;
    hr = device_->CreateShaderResourceView(nv12_tex, &sd, &srv_uv_);
    if (FAILED(hr)) {
        fprintf(stderr, "[Renderer] CreateSRV (UV) failed: 0x%08X\n", hr);
        return false;
    }

    last_tex_ = nv12_tex;
    return true;
}

bool D3D11Renderer::RenderNV12(ID3D11Texture2D* nv12_tex)
{
    if (!CreateSRVs(nv12_tex)) return false;

    // Set render target
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);

    D3D11_VIEWPORT vp = { 0, 0, (float)width_, (float)height_, 0, 1 };
    context_->RSSetViewports(1, &vp);

    // Clear (optional)
    float black[4] = {0, 0, 0, 1};
    context_->ClearRenderTargetView(rtv_.Get(), black);

    // Draw fullscreen quad (3 vertices, no vertex buffer)
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetInputLayout(nullptr);

    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);

    ID3D11ShaderResourceView* srvs[] = { srv_y_.Get(), srv_uv_.Get() };
    context_->PSSetShaderResources(0, 2, srvs);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());

    context_->Draw(3, 0);

    // Present
    HRESULT hr = swapchain_->Present(0, 0);  // vsync=0 for latency measurement
    if (FAILED(hr)) {
        fprintf(stderr, "[Renderer] Present failed: 0x%08X\n", hr);
        return false;
    }
    return true;
}

void D3D11Renderer::SetTitle(const std::string& title)
{
    if (hwnd_) SetWindowTextA(hwnd_, title.c_str());
}

void D3D11Renderer::Resize(int width, int height)
{
    if (width == width_ && height == height_) return;
    width_  = width;
    height_ = height;
    rtv_.Reset();
    swapchain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    ComPtr<ID3D11Texture2D> back_buf;
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&back_buf));
    device_->CreateRenderTargetView(back_buf.Get(), nullptr, &rtv_);
}

void D3D11Renderer::Release()
{
    srv_y_.Reset(); srv_uv_.Reset();
    sampler_.Reset();
    vs_.Reset(); ps_.Reset();
    rtv_.Reset();
    swapchain_.Reset();
    context_.Reset();
    device_.Reset();
}
