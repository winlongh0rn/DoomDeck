cmake_minimum_required(VERSION 3.20)

project(DoomDeckCapture)

if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(DoomDeckCapture
    src/main.cpp
    src/WindowFinder.cpp
    src/CaptureEngine.cpp
)

target_link_libraries(DoomDeckCapture
    user32
    kernel32
    psapi
    windowsapp
    d3d11
    dxgi
)