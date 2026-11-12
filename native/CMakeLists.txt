cmake_minimum_required(VERSION 3.20)

project(klive_capture LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 两个 DLL 随 .keel 分发，用户机器不一定装了 VC++ 运行时；统一静态链接 CRT。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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

# ---------------------------------------------------------------------------
# WebView2 外壳：Win32 + WebView2 + JSON 桥（见 webview/klive_webview.h）
# ---------------------------------------------------------------------------
set(WEBVIEW2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/webview/third_party/webview2")

add_library(klive_webview SHARED
    webview/klive_webview.cpp
    webview/klive_webview.rc
)

target_include_directories(klive_webview PRIVATE
    include
    webview
    webview/third_party
    webview/third_party/webview2
)

target_compile_definitions(klive_webview PRIVATE
    KLIVE_WEBVIEW_BUILD
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(klive_webview PRIVATE /utf-8 /W4 /permissive-)
endif()

target_link_libraries(klive_webview PRIVATE
    ole32
    oleaut32
    user32
    gdi32
    shell32
    shlwapi
    dwmapi
    "${WEBVIEW2_DIR}/WebView2Loader.dll.lib"
)

set_target_properties(klive_webview PROPERTIES OUTPUT_NAME "klive_webview")

add_custom_command(TARGET klive_webview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WEBVIEW2_DIR}/WebView2Loader.dll"
        "$<TARGET_FILE_DIR:klive_webview>/WebView2Loader.dll"
)

# 控制台自检：打开 smoke 页面，把桥协议日志打到 stdout
add_executable(klive_webview_smoke webview/smoke_main.cpp)
target_compile_definitions(klive_webview_smoke PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
if(MSVC)
    target_compile_options(klive_webview_smoke PRIVATE /utf-8 /W4 /permissive-)
endif()
target_link_libraries(klive_webview_smoke PRIVATE user32)
add_dependencies(klive_webview_smoke klive_webview)
add_custom_command(TARGET klive_webview_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/webview/smoke"
        "$<TARGET_FILE_DIR:klive_webview_smoke>/smoke"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/webview/smoke-mirror"
        "$<TARGET_FILE_DIR:klive_webview_smoke>/smoke-mirror"
)
