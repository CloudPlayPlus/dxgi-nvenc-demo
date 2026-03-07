#pragma once
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string>

// High-resolution timer helpers
inline double NowMs() {
    LARGE_INTEGER t, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / freq.QuadPart;
}

// Rolling average over N samples
template<int N = 30>
struct RollingAvg {
    double samples[N] = {};
    int    idx        = 0;
    int    count      = 0;

    void Add(double v) {
        samples[idx] = v;
        idx = (idx + 1) % N;
        if (count < N) count++;
    }

    double Avg() const {
        if (count == 0) return 0;
        double sum = 0;
        for (int i = 0; i < count; i++) sum += samples[i];
        return sum / count;
    }
};

struct PerfStats {
    RollingAvg<60> capture_ms;
    RollingAvg<60> encode_ms;
    RollingAvg<60> decode_ms;
    RollingAvg<60> render_ms;
    RollingAvg<60> total_ms;
    RollingAvg<60> fps;

    double last_frame_time = 0;
    int    frame_count     = 0;

    void OnFrameStart() {
        double now = NowMs();
        if (last_frame_time > 0) {
            fps.Add(1000.0 / (now - last_frame_time));
        }
        last_frame_time = now;
        frame_count++;
    }

    std::string Summary() const {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "FPS:%.1f | Capture:%.1fms | Encode:%.1fms | Decode:%.1fms | Render:%.1fms | Total:%.1fms",
            fps.Avg(),
            capture_ms.Avg(),
            encode_ms.Avg(),
            decode_ms.Avg(),
            render_ms.Avg(),
            total_ms.Avg()
        );
        return buf;
    }
};
