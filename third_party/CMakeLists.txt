# Vendored third-party dependencies.

# --- quirc: QR code decoder (ISC license) -----------------------------------
add_library(quirc STATIC
    quirc/quirc.c
    quirc/decode.c
    quirc/identify.c
    quirc/version_db.c
)
target_include_directories(quirc PUBLIC quirc)
set_target_properties(quirc PROPERTIES
    C_STANDARD 99
    POSITION_INDEPENDENT_CODE ON
)

# --- OpenXR: PICO OpenXR loader + headers -----------------------------------
# These come from the PICO OpenXR SDK, not the Khronos release: the headers
# carry PICO vendor extensions (XR_PICO_camera_image, ...) and the loader
# knows how to find the PICO runtime. Do not swap them for the Khronos AAR.
add_library(openxr_loader SHARED IMPORTED GLOBAL)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/lib/${ANDROID_ABI}/libopenxr_loader.so"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}"
)
