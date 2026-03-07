cmake_minimum_required(VERSION 3.20)
project(dxgi_nvenc_demo CXX)

set(CMAKE_CXX_STANDARD 17)

# FFmpeg - set FFMPEG_DIR to your FFmpeg install prefix
set(FFMPEG_DIR "" CACHE PATH "Path to FFmpeg install directory")
if(NOT FFMPEG_DIR)
  message(FATAL_ERROR "Set -DFFMPEG_DIR=<path to ffmpeg>")
endif()

add_executable(dxgi_nvenc_demo
  src/main.cpp
  src/dda_capture.cpp
  src/nvenc_encoder.cpp
  src/nvdec_decoder.cpp
  src/d3d11_renderer.cpp
)

target_include_directories(dxgi_nvenc_demo PRIVATE
  ${FFMPEG_DIR}/include
  src
)

target_link_directories(dxgi_nvenc_demo PRIVATE ${FFMPEG_DIR}/lib)

target_link_libraries(dxgi_nvenc_demo PRIVATE
  avcodec avformat avutil avdevice avfilter swscale swresample
  d3d11 dxgi dxguid d3dcompiler
)

# Copy FFmpeg DLLs to output dir
file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
foreach(DLL ${FFMPEG_DLLS})
  add_custom_command(TARGET dxgi_nvenc_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DLL} $<TARGET_FILE_DIR:dxgi_nvenc_demo>
  )
endforeach()
