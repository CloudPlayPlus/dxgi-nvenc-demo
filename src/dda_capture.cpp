#include "dda_capture.h"
#include <dxgi1_6.h>
#include <stdio.h>

// Find the adapter that owns the specified output (monitor).
// On Optimus laptops, the primary display is owned by the iGPU adapter.
// DDA must be initialized on THAT adapter, not the dGPU.
static bool FindAdapterForOutput(int output_idx,
                                  ComPtr<IDXGIAdapter1>& out_adapter,
                                  ComPtr<IDXGIOutput>&   out_output)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
        fprintf(stderr, "[DDA] Failed to create DXGI factory\n");
        return false;
    }

    int global_output = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
        DXGI_ADAPTER_DESC1 adesc;
        adapter->GetDesc1(&adesc);
        wprintf(L"[DDA] Adapter %u: %s\n", ai, adesc.Description);

        ComPtr<IDXGIOutput> output;
        for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC odesc;
            output->GetDesc(&odesc);
            wprintf(L"[DDA]   Output %u: %s attached=%d\n", oi, odesc.DeviceName, odesc.AttachedToDesktop);

            if (odesc.AttachedToDesktop && global_output == output_idx) {
                out_adapter = adapter;
                out_output  = output;
                return true;
            }
            if (odesc.AttachedToDesktop) global_output++;
        }
    }

    fprintf(stderr, "[DDA] Output index %d not found\n", output_idx);
    return false;
}

bool DDACapture::Init(int output_idx)
{
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput>   output;

    if (!FindAdapterForOutput(output_idx, adapter, output)) {
        return false;
    }

    // Create D3D11 device on the adapter that owns this output
    D3D_FEATURE_LEVEL feature_level;
    HRESULT hr = D3D11CreateDevice(
        adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,  // must be UNKNOWN when adapter is specified
        nullptr,
        0,
        nullptr, 0,
        D3D11_SDK_VERSION,
        &device_,
        &feature_level,
        &context_
    );
    if (FAILED(hr)) {
        fprintf(stderr, "[DDA] D3D11CreateDevice failed: 0x%08X\n", hr);
        return false;
    }
    printf("[DDA] D3D11 device created (feature level 0x%X)\n", (unsigned)feature_level);

    // Get IDXGIOutput1 for duplication
    ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) {
        fprintf(stderr, "[DDA] Failed to get IDXGIOutput1: 0x%08X\n", hr);
        return false;
    }

    // Create the duplication
    hr = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
        fprintf(stderr, "[DDA] DuplicateOutput failed: 0x%08X\n", hr);
        if (hr == DXGI_ERROR_UNSUPPORTED) {
            fprintf(stderr, "[DDA] DXGI_ERROR_UNSUPPORTED: this adapter cannot duplicate this output.\n");
            fprintf(stderr, "[DDA] This usually means you're running on the wrong adapter.\n");
        }
        return false;
    }

    // Get output dimensions
    DXGI_OUTDUPL_DESC dupl_desc;
    duplication_->GetDesc(&dupl_desc);
    width_  = dupl_desc.ModeDesc.Width;
    height_ = dupl_desc.ModeDesc.Height;
    printf("[DDA] Desktop duplication ready: %dx%d\n", width_, height_);
    return true;
}

bool DDACapture::AcquireFrame(CaptureFrame& frame, uint32_t timeout_ms)
{
    if (frame_acquired_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }

    DXGI_OUTDUPL_FRAME_INFO frame_info = {};
    ComPtr<IDXGIResource> resource;

    HRESULT hr = duplication_->AcquireNextFrame(timeout_ms, &frame_info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;  // no update this frame
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[DDA] AcquireNextFrame failed: 0x%08X\n", hr);
        return false;
    }
    frame_acquired_ = true;

    // Get the texture
    hr = resource.As(&frame.texture);
    if (FAILED(hr)) {
        fprintf(stderr, "[DDA] Failed to get texture from resource: 0x%08X\n", hr);
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
        return false;
    }

    frame.width  = (uint32_t)width_;
    frame.height = (uint32_t)height_;
    return true;
}

void DDACapture::ReleaseFrame()
{
    if (frame_acquired_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }
}

void DDACapture::Release()
{
    ReleaseFrame();
    duplication_.Reset();
    context_.Reset();
    device_.Reset();
}
