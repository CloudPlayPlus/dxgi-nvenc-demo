#include "dda_capture.h"
#include "nvenc_encoder.h"
#include "nvdec_decoder.h"
#include "d3d11_renderer.h"
#include "perf_stats.h"

#include <windows.h>
#include <stdio.h>
#include <string>

// ---- Config ----------------------------------------------------------------
static const int TARGET_FPS   = 60;
static const int FRAME_BUDGET = 1000 / TARGET_FPS;  // ms

// ---- Globals ---------------------------------------------------------------
static bool           g_running  = true;
static HWND           g_hwnd     = nullptr;
static D3D11Renderer* g_renderer = nullptr;

// ---- Win32 window ----------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (g_renderer) {
            g_renderer->Resize(LOWORD(lp), HIWORD(lp));
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            g_running = false;
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND CreateAppWindow(int w, int h)
{
    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "DxgiNvencDemo";
    RegisterClassExA(&wc);

    RECT rc = {0, 0, w, h};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExA(
        0, "DxgiNvencDemo",
        "DXGI-NVENC Demo | ESC to quit",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    return hwnd;
}

// ---- Main pipeline loop ----------------------------------------------------
static int RunPipeline(HWND hwnd)
{
    printf("\n=== Initializing Pipeline ===\n");

    // 1. Capture (Intel iGPU DDA)
    DDACapture capture;
    printf("[1/4] Desktop Duplication...\n");
    if (!capture.Init(0)) {
        MessageBoxA(hwnd, "Failed to initialize DDA capture", "Error", MB_OK|MB_ICONERROR);
        return 1;
    }

    int W = capture.Width();
    int H = capture.Height();

    // 2. Encoder (NVENC on NVIDIA dGPU)
    NvencEncoder encoder;
    printf("[2/4] NVENC Encoder...\n");
    if (!encoder.Init(capture.GetDevice(), W, H, TARGET_FPS)) {
        MessageBoxA(hwnd, "Failed to initialize NVENC", "Error", MB_OK|MB_ICONERROR);
        return 1;
    }

    // 3. Decoder (NVDEC on same NVIDIA dGPU device as encoder)
    NvdecDecoder decoder;
    printf("[3/4] NVDEC Decoder...\n");
    if (!decoder.Init(encoder.GetEncDevice(), W, H)) {
        MessageBoxA(hwnd, "Failed to initialize NVDEC", "Error", MB_OK|MB_ICONERROR);
        return 1;
    }

    // 4. Renderer (D3D11 on same NVIDIA device, renders to window)
    D3D11Renderer renderer;
    g_renderer = &renderer;
    printf("[4/4] D3D11 Renderer...\n");
    // Use actual window client area dimensions
    RECT rc; GetClientRect(hwnd, &rc);
    int disp_w = rc.right  - rc.left;
    int disp_h = rc.bottom - rc.top;
    if (disp_w == 0) disp_w = W / 2;
    if (disp_h == 0) disp_h = H / 2;
    if (!renderer.Init(hwnd, encoder.GetEncDevice(), disp_w, disp_h)) {
        MessageBoxA(hwnd, "Failed to initialize renderer", "Error", MB_OK|MB_ICONERROR);
        return 1;
    }

    printf("\n=== Pipeline Ready. Press ESC to quit. ===\n");
    printf("%-8s %-10s %-10s %-10s %-10s %-10s\n",
           "Frame", "Capture", "Encode", "Decode", "Render", "Total");

    PerfStats stats;
    int frame_count = 0;
    int decode_fails = 0;

    // Pipeline loop
    while (g_running) {
        // Process Windows messages
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        stats.OnFrameStart();
        double t_start = NowMs();

        // ---- Capture -------------------------------------------------------
        double t0 = NowMs();
        CaptureFrame cap_frame;
        if (!capture.AcquireFrame(cap_frame, FRAME_BUDGET)) {
            // No desktop update, still try to encode last frame if needed
            // (skip this frame)
            continue;
        }
        double capture_ms = NowMs() - t0;
        stats.capture_ms.Add(capture_ms);

        // ---- Encode --------------------------------------------------------
        double t1 = NowMs();
        AVPacket* pkt = encoder.EncodeFrame(cap_frame.texture.Get());
        capture.ReleaseFrame();
        double encode_ms = NowMs() - t1;
        stats.encode_ms.Add(encode_ms);

        if (!pkt) {
            // Encoder buffering (first few frames)
            continue;
        }

        // ---- Decode --------------------------------------------------------
        double t2 = NowMs();
        DecodedFrame dec_frame;
        bool decoded = decoder.Decode(pkt, dec_frame);
        av_packet_free(&pkt);
        double decode_ms = NowMs() - t2;
        stats.decode_ms.Add(decode_ms);

        if (!decoded) {
            decode_fails++;
            if (decode_fails % 30 == 1)
                printf("[Warn] Decode returned no frame (pts=%lld)\n", (long long)frame_count);
            continue;
        }
        decode_fails = 0;

        // ---- Render --------------------------------------------------------
        double t3 = NowMs();
        renderer.RenderNV12(dec_frame.texture.Get());
        double render_ms = NowMs() - t3;
        stats.render_ms.Add(render_ms);

        double total_ms = NowMs() - t_start;
        stats.total_ms.Add(total_ms);

        frame_count++;

        // Print stats every 60 frames
        if (frame_count % 60 == 0) {
            printf("%-8d %-10.2f %-10.2f %-10.2f %-10.2f %-10.2f\n",
                   frame_count,
                   stats.capture_ms.Avg(),
                   stats.encode_ms.Avg(),
                   stats.decode_ms.Avg(),
                   stats.render_ms.Avg(),
                   stats.total_ms.Avg());
        }

        // Update window title every 10 frames
        if (frame_count % 10 == 0) {
            renderer.SetTitle(stats.Summary() + " | ESC=quit");
        }

        // Frame pacing: sleep remaining budget
        double elapsed = NowMs() - t_start;
        if (elapsed < FRAME_BUDGET) {
            Sleep((DWORD)(FRAME_BUDGET - elapsed));
        }
    }

    printf("\n=== Shutting down ===\n");
    printf("Total frames: %d\n", frame_count);
    printf("Final stats: %s\n", stats.Summary().c_str());

    g_renderer = nullptr;
    return 0;
}

// ---- Entry point -----------------------------------------------------------
int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered stdout
    setvbuf(stderr, nullptr, _IONBF, 0);

    printf("DXGI Desktop Duplication + NVENC/NVDEC Full Pipeline Demo\n");
    printf("==========================================================\n");
    printf("Pipeline: Capture(Intel DDA) -> Encode(NVENC) -> Decode(NVDEC) -> Render(D3D11)\n");
    printf("Encoding: H264, 10Mbps CBR, p4/ull (Sunshine low-latency profile)\n\n");

    // Create a small preview window (quarter of screen)
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int win_w    = screen_w / 2;
    int win_h    = screen_h / 2;

    g_hwnd = CreateAppWindow(win_w, win_h);
    if (!g_hwnd) {
        fprintf(stderr, "Failed to create window\n");
        return 1;
    }

    int ret = RunPipeline(g_hwnd);

    DestroyWindow(g_hwnd);
    return ret;
}
