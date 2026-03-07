@echo off
REM Quick build script
REM Usage: build.bat <path-to-ffmpeg>
REM Example: build.bat D:\ffmpeg

if "%1"=="" (
    echo Usage: build.bat ^<ffmpeg_dir^>
    echo Example: build.bat D:\ffmpeg
    exit /b 1
)

set FFMPEG_DIR=%1

cmake -B build -DFFMPEG_DIR=%FFMPEG_DIR% -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

echo.
echo Build done. Run: build\Release\dxgi_nvenc_demo.exe
