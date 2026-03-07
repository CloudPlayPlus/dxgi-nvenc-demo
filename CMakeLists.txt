cmake_minimum_required(VERSION 3.20)
project(dxgi_nvenc_demo CXX)

set(CMAKE_CXX_STANDARD 17)

# FFmpeg - set FFMPEG_DIR to your FFmpeg install prefix
set(FFMPEG_DIR "" CACHE PATH "Path to FFmpeg install directory")
if(NOT FFMPEG_DIR)
  message(FATAL_ERROR "Set -DFFMPEG_DIR=<path to ffmpeg>")
endif()

set(FFMPEG_INCLUDE "${FFMPEG_DIR}/include")
set(FFMPEG_LIB_DIR "${FFMPEG_DIR}/lib")

# D3D11 / DXGI from Windows SDK (auto-found via CMake)
find_package(directx-headers CONFIG QUIET)

add_executable(dxgi_nvenc_demo
  src/main.cpp
  src/dda_capture.cpp
  src/nvenc_encoder.cpp
)

target_include_directories(dxgi_nvenc_demo PRIVATE
  ${FFMPEG_INCLUDE}
  src
)

target_link_directories(dxgi_nvenc_demo PRIVATE ${FFMPEG_LIB_DIR})

target_link_libraries(dxgi_nvenc_demo PRIVATE
  avcodec
  avformat
  avutil
  avdevice
  avfilter
  swscale
  swresample
  d3d11
  dxgi
  dxguid
)

# Copy FFmpeg DLLs to output dir (Windows)
if(WIN32)
  file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
  foreach(DLL ${FFMPEG_DLLS})
    add_custom_command(TARGET dxgi_nvenc_demo POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DLL} $<TARGET_FILE_DIR:dxgi_nvenc_demo>
    )
  endforeach()
endif()
