# dxgi-nvenc-demo

Demo: D3D11 Desktop Duplication → FFmpeg NVENC H.264 encoding on Optimus laptops.

## Problem

On Optimus (hybrid GPU) laptops, the desktop is rendered by the iGPU (Intel).
DXGI Desktop Duplication API (DDA) must run on the same adapter that renders the desktop.
Directly using DDA on the NVIDIA dGPU yields `DXGI_ERROR_UNSUPPORTED`.

## Approach

1. **Adapter enumeration**: Find the adapter that actually owns the desktop output
2. **DDA capture**: Run Desktop Duplication on the iGPU adapter
3. **Cross-adapter transfer**: Share texture to NVIDIA dGPU via NT shared handle (zero CPU copy)
4. **NVENC encode**: Use FFmpeg `h264_nvenc` with D3D11 hardware context

## Fallback (ddagrab)

FFmpeg's built-in `ddagrab` filter (FFmpeg 5.0+) handles adapter selection automatically.
Try this first before manual cross-adapter logic.

## Build

Requirements:
- Visual Studio 2022
- FFmpeg (prebuilt, with NVENC support)
- Windows SDK

```bash
cmake -B build -DFFMPEG_DIR=path/to/ffmpeg
cmake --build build --config Release
```
