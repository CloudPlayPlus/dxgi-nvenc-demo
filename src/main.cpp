#include "dda_capture.h"
#include "nvenc_encoder.h"
#include "qsv_encoder.h"
#include "nvdec_decoder.h"
#include "d3d11_renderer.h"
#include "perf_stats.h"

#include <windows.h>
#include <string>
#include <stdio.h>

static const int TARGET_FPS    = 60;
static const int FRAME_BUDGET  = 1000 / TARGET_FPS;
static const int BENCH_FRAMES  = 300;  // frames per encoder in benchmark mode

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
        if (g_renderer) g_renderer->Resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { g_running = false; PostQuitMessage(0); }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND CreateAppWindow(int w, int h, const char* title)
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
    return CreateWindowExA(0, "DxgiNvencDemo", title,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, wc.hInstance, nullptr);
}

// ---- Pump messages without blocking ----------------------------------------
static void PumpMessages()
{
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

// ---- NVENC live pipeline ---------------------------------------------------
static int RunNvenc(HWND hwnd)
{
    printf("\n=== NVENC Pipeline (Intel DDA -> NVENC -> d3d11va decode -> D3D11 render) ===\n");

    DDACapture capture;
    if (!capture.Init(0)) { fprintf(stderr, "DDA init failed\n"); return 1; }
    int W = capture.Width(), H = capture.Height();

    NvencEncoder encoder;
    if (!encoder.Init(capture.GetDevice(), W, H, TARGET_FPS)) { fprintf(stderr, "NVENC init failed\n"); return 1; }

    NvdecDecoder decoder;
    if (!decoder.Init(encoder.GetEncDevice(), W, H)) { fprintf(stderr, "NVDEC init failed\n"); return 1; }

    D3D11Renderer renderer;
    g_renderer = &renderer;
    RECT rc; GetClientRect(hwnd, &rc);
    int dw = rc.right - rc.left, dh = rc.bottom - rc.top;
    if (!dw) dw = W/2; if (!dh) dh = H/2;
    if (!renderer.Init(hwnd, encoder.GetEncDevice(), dw, dh)) { fprintf(stderr, "Renderer init failed\n"); return 1; }

    printf("%-8s %-10s %-10s %-10s %-10s %-10s\n",
           "Frame", "Cap(ms)", "Enc(ms)", "Dec(ms)", "Ren(ms)", "Total(ms)");

    PerfStats stats;
    int frame_count = 0;

    while (g_running) {
        PumpMessages();
        if (!g_running) break;

        stats.OnFrameStart();
        double t_start = NowMs();

        double t0 = NowMs();
        CaptureFrame cap_frame;
        if (!capture.AcquireFrame(cap_frame, FRAME_BUDGET)) continue;
        double capture_ms = NowMs() - t0;
        stats.capture_ms.Add(capture_ms);

        double t1 = NowMs();
        AVPacket* pkt = encoder.EncodeFrame(cap_frame.texture.Get());
        capture.ReleaseFrame();
        double encode_ms = NowMs() - t1;
        stats.encode_ms.Add(encode_ms);
        if (!pkt) continue;

        double t2 = NowMs();
        DecodedFrame dec_frame;
        bool decoded = decoder.Decode(pkt, dec_frame);
        av_packet_free(&pkt);
        double decode_ms = NowMs() - t2;
        stats.decode_ms.Add(decode_ms);
        if (!decoded) continue;

        double t3 = NowMs();
        renderer.RenderNV12(dec_frame.texture.Get());
        double render_ms = NowMs() - t3;
        stats.render_ms.Add(render_ms);

        double total_ms = NowMs() - t_start;
        stats.total_ms.Add(total_ms);
        frame_count++;

        if (frame_count % 60 == 0) {
            printf("%-8d %-10.2f %-10.2f %-10.2f %-10.2f %-10.2f\n",
                   frame_count,
                   stats.capture_ms.Avg(), stats.encode_ms.Avg(),
                   stats.decode_ms.Avg(), stats.render_ms.Avg(),
                   stats.total_ms.Avg());
        }
        if (frame_count % 10 == 0)
            renderer.SetTitle(stats.Summary() + " [NVENC] ESC=quit");

        double elapsed = NowMs() - t_start;
        if (elapsed < FRAME_BUDGET) Sleep((DWORD)(FRAME_BUDGET - elapsed));
    }

    printf("\nNVENC final: %s\n", stats.Summary().c_str());
    g_renderer = nullptr;
    return 0;
}

// ---- Benchmark: run N frames with NVENC, then N frames with QSV -----------
struct BenchResult {
    const char* name;
    double capture_ms, encode_ms, total_ms;
};

static BenchResult BenchmarkNvenc(DDACapture& capture)
{
    int W = capture.Width(), H = capture.Height();
    NvencEncoder encoder;
    encoder.Init(capture.GetDevice(), W, H, TARGET_FPS);

    PerfStats stats;
    int frame_count = 0;
    int skip = 60; // warmup frames

    while (frame_count < BENCH_FRAMES + skip) {
        PumpMessages();
        double t_start = NowMs();

        CaptureFrame cap_frame;
        if (!capture.AcquireFrame(cap_frame, FRAME_BUDGET)) continue;
        double t_enc0 = NowMs();
        double capture_ms = t_enc0 - t_start;

        AVPacket* pkt = encoder.EncodeFrame(cap_frame.texture.Get());
        capture.ReleaseFrame();
        double encode_ms = NowMs() - t_enc0;
        double total_ms  = NowMs() - t_start;

        if (pkt) av_packet_free(&pkt);

        if (frame_count >= skip) {
            stats.capture_ms.Add(capture_ms);
            stats.encode_ms.Add(encode_ms);
            stats.total_ms.Add(total_ms);
        }
        frame_count++;

        if (frame_count % 60 == 0)
            printf("  [NVENC] frame %d: cap=%.2f enc=%.2f total=%.2f ms\n",
                   frame_count, capture_ms, encode_ms, total_ms);
    }
    return {"NVENC (h264_nvenc p4/ull CBR)",
            stats.capture_ms.Avg(), stats.encode_ms.Avg(), stats.total_ms.Avg()};
}

static BenchResult BenchmarkQsv(DDACapture& capture)
{
    int W = capture.Width(), H = capture.Height();
    QsvEncoder encoder;
    if (!encoder.Init(capture.GetDevice(), W, H, TARGET_FPS)) {
        printf("  [QSV] Init failed — skipping QSV benchmark\n");
        return {"QSV (SKIPPED)", 0, 0, 0};
    }

    PerfStats stats;
    int frame_count = 0;
    int skip = 60;

    while (frame_count < BENCH_FRAMES + skip) {
        PumpMessages();
        double t_start = NowMs();

        CaptureFrame cap_frame;
        if (!capture.AcquireFrame(cap_frame, FRAME_BUDGET)) continue;
        double t_enc0 = NowMs();
        double capture_ms = t_enc0 - t_start;

        AVPacket* pkt = encoder.EncodeFrame(cap_frame.texture.Get());
        capture.ReleaseFrame();
        double encode_ms = NowMs() - t_enc0;
        double total_ms  = NowMs() - t_start;

        if (pkt) av_packet_free(&pkt);

        if (frame_count >= skip) {
            stats.capture_ms.Add(capture_ms);
            stats.encode_ms.Add(encode_ms);
            stats.total_ms.Add(total_ms);
        }
        frame_count++;

        if (frame_count % 60 == 0)
            printf("  [QSV]   frame %d: cap=%.2f enc=%.2f total=%.2f ms\n",
                   frame_count, capture_ms, encode_ms, total_ms);
    }
    return {"QSV  (h264_qsv veryfast CBR + CPU NV12)",
            stats.capture_ms.Avg(), stats.encode_ms.Avg(), stats.total_ms.Avg()};
}

static void RunBenchmark()
{
    printf("\n=== ENCODER BENCHMARK: NVENC vs QSV ===\n");
    printf("Resolution: screen res @ %d fps, CBR 10Mbps\n", TARGET_FPS);
    printf("Each encoder: %d warmup + %d measured frames\n\n", 60, BENCH_FRAMES);

    DDACapture capture;
    if (!capture.Init(0)) { fprintf(stderr, "DDA init failed\n"); return; }
    printf("Capture device: %dx%d\n\n", capture.Width(), capture.Height());

    printf("--- NVENC benchmark ---\n");
    BenchResult r_nvenc = BenchmarkNvenc(capture);

    printf("\n--- QSV benchmark ---\n");
    BenchResult r_qsv = BenchmarkQsv(capture);

    // Print comparison table (ASCII only, avoid MSVC GBK encoding issues)
    printf("\n");
    printf("+----------------------------------------------------------+\n");
    printf("|           ENCODER COMPARISON RESULT                     |\n");
    printf("+----------------------+----------+----------+--------------+\n");
    printf("| Encoder              | Cap (ms) | Enc (ms) | Total (ms)  |\n");
    printf("+----------------------+----------+----------+--------------+\n");
    printf("| %-20s | %8.2f | %8.2f | %11.2f |\n",
           "NVENC p4/ull CBR",
           r_nvenc.capture_ms, r_nvenc.encode_ms, r_nvenc.total_ms);
    printf("| %-20s | %8.2f | %8.2f | %11.2f |\n",
           r_qsv.total_ms > 0 ? "QSV veryfast CBR" : "QSV (FAILED)",
           r_qsv.capture_ms, r_qsv.encode_ms, r_qsv.total_ms);
    printf("+----------------------+----------+----------+--------------+\n");

    if (r_qsv.total_ms > 0) {
        double diff = r_nvenc.total_ms - r_qsv.total_ms;
        if (diff > 0)
            printf("\nNVENC is %.2f ms SLOWER (QSV wins in encode+copy total)\n", diff);
        else
            printf("\nNVENC is %.2f ms FASTER (NVENC wins in encode+copy total)\n", -diff);
        printf("\nNote: QSV encode path includes CPU BGRA->NV12 via swscale.\n");
        printf("      With D3D11 Video Processor (GPU color convert), QSV\n");
        printf("      total would drop by ~2-3ms (no CPU color conversion).\n");
    }
}

// ---- Entry point -----------------------------------------------------------
int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    bool benchmark = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--bench") == 0 || strcmp(argv[i], "-b") == 0)
            benchmark = true;
    }

    printf("DXGI Desktop Duplication + NVENC/QSV Pipeline Demo\n");
    printf("===================================================\n");
    printf("Usage: dxgi_nvenc_demo.exe [--bench]\n");
    printf("  (no args)  : NVENC live pipeline with window\n");
    printf("  --bench    : NVENC vs QSV benchmark (no decode/render)\n\n");

    if (benchmark) {
        // Create a hidden window just to have a valid message loop
        g_hwnd = CreateAppWindow(100, 100, "Benchmark running...");
        ShowWindow(g_hwnd, SW_HIDE);
        RunBenchmark();
        printf("\nBenchmark complete. Press Enter to exit.\n");
        getchar();
        DestroyWindow(g_hwnd);
        return 0;
    }

    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    g_hwnd = CreateAppWindow(screen_w / 2, screen_h / 2,
                              "NVENC Pipeline | ESC=quit");
    if (!g_hwnd) { fprintf(stderr, "CreateWindow failed\n"); return 1; }

    int ret = RunNvenc(g_hwnd);
    DestroyWindow(g_hwnd);
    return ret;
}
