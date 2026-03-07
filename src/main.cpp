#include "dda_capture.h"
#include "nvenc_encoder.h"
#include <stdio.h>
#include <signal.h>

// Output file: raw H264 annexb bitstream
// Play with: ffplay -f h264 output.h264
static const char* OUTPUT_FILE = "output.h264";
static const int   TARGET_FPS  = 30;
static const int   DURATION_S  = 10;  // capture 10 seconds

static volatile bool g_running = true;
static void on_signal(int) { g_running = false; }

int main(int argc, char* argv[])
{
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    printf("=== DXGI Desktop Duplication + NVENC Demo ===\n");
    printf("Captures primary monitor for %d seconds → %s\n\n", DURATION_S, OUTPUT_FILE);

    // --- Desktop Duplication capture (iGPU on Optimus) ---
    DDACapture capture;
    printf("[Step 1] Initializing Desktop Duplication...\n");
    if (!capture.Init(0)) {
        fprintf(stderr, "FATAL: Failed to initialize DDA capture.\n");
        return 1;
    }

    // --- NVENC encoder (NVIDIA dGPU) ---
    printf("\n[Step 2] Initializing NVENC encoder...\n");
    NvencEncoder encoder;
    if (!encoder.Init(capture.GetDevice(), capture.Width(), capture.Height(), TARGET_FPS)) {
        fprintf(stderr, "FATAL: Failed to initialize NVENC encoder.\n");
        return 1;
    }

    // --- Open output file ---
    FILE* outfile = fopen(OUTPUT_FILE, "wb");
    if (!outfile) {
        fprintf(stderr, "FATAL: Cannot open output file %s\n", OUTPUT_FILE);
        return 1;
    }
    printf("\n[Step 3] Capturing %dx%d @ %dfps for %d seconds...\n",
           capture.Width(), capture.Height(), TARGET_FPS, DURATION_S);
    printf("Press Ctrl+C to stop early.\n\n");

    int total_frames     = TARGET_FPS * DURATION_S;
    int frames_captured  = 0;
    int frames_skipped   = 0;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    while (g_running && frames_captured < total_frames) {
        CaptureFrame frame;
        if (capture.AcquireFrame(frame, 1000 / TARGET_FPS)) {
            encoder.EncodeFrame(frame.texture.Get(), outfile);
            capture.ReleaseFrame();
            frames_captured++;
        } else {
            // No new frame (desktop not updated) - skip or duplicate
            frames_skipped++;
        }

        // Pace to ~TARGET_FPS
        QueryPerformanceCounter(&t1);
        double elapsed_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
        double target_ms  = (double)(frames_captured + frames_skipped) * 1000.0 / TARGET_FPS;
        if (target_ms > elapsed_ms) {
            Sleep((DWORD)(target_ms - elapsed_ms));
        }
    }

    printf("\n[Done] Captured %d frames (%d skipped/no-update)\n", frames_captured, frames_skipped);

    encoder.Flush(outfile);
    fclose(outfile);

    printf("Output: %s\n", OUTPUT_FILE);
    printf("Play:   ffplay -f h264 %s\n", OUTPUT_FILE);
    return 0;
}
