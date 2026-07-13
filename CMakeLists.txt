cmake_minimum_required(VERSION 3.20)
project(Crash LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Phase 0 spike: a single windowed executable that renders a synthetic
# 100k-row virtualized list on Direct2D via a DirectComposition flip-model
# swapchain, with a live perf overlay. No disk I/O yet (that is Phase 1).
add_executable(crash WIN32
    src/main.cpp
    src/GraphicsDevice.cpp
    src/VirtualListView.cpp
    src/Enumerator.cpp
    src/IconCache.cpp
    src/ShellContextMenu.cpp
    src/ThumbnailCache.cpp
    src/DragDrop.cpp
    src/Session.cpp
    src/Settings.cpp
    src/License.cpp
)

target_compile_definitions(crash PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    $<$<CONFIG:Release>:NDEBUG>
)

target_link_libraries(crash PRIVATE
    d3d11 d2d1 dwrite dcomp dxgi dwmapi shell32 ole32
)

if (MSVC)
    target_compile_options(crash PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()
