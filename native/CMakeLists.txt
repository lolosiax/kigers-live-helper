cmake_minimum_required(VERSION 3.20)

project(klive_capture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(klive_capture SHARED
    src/klive_exports.cpp
    src/klive_monitor.cpp
    src/klive_mirror.cpp
    src/frame_source.cpp
    src/klive_duplication.cpp
    src/klive_wgc.cpp
)

target_include_directories(klive_capture PUBLIC include PRIVATE src)

target_compile_definitions(klive_capture PRIVATE
    KLIVE_BUILD
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(klive_capture PRIVATE /utf-8 /W4 /permissive-)
endif()

target_link_libraries(klive_capture PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    user32
    gdi32
    # WGC 走 C++/WinRT；windowsapp.lib 提供 WinRT 激活与 IInspectable 相关符号
    windowsapp
)

set_target_properties(klive_capture PROPERTIES OUTPUT_NAME "klive_capture")
