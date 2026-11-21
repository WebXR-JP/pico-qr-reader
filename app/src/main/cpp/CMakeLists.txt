cmake_minimum_required(VERSION 3.22.1)

project(pico_qr_reader LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party")
add_subdirectory("${THIRD_PARTY_DIR}" third_party)

add_library(pico_qr_reader SHARED
    main.cpp
    pico_camera.cpp
)

target_compile_options(pico_qr_reader PRIVATE
    -Wall
    -fno-exceptions
    -fno-rtti
)

target_link_options(pico_qr_reader PRIVATE
    # 16 KB page alignment (default since NDK r28; explicit for older NDKs).
    -Wl,-z,max-page-size=16384
)

target_link_libraries(pico_qr_reader PRIVATE
    quirc
    openxr_loader
    android
    EGL
    GLESv3
    log
    m
)
