#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"

class PicoCamera {
public:
    enum class State {
        Idle,
        CreatingDevice,
        CreatingSession,
        Capturing,
        Error,
    };

    using FrameCallback = std::function<void(
        const uint8_t* rgba,
        uint32_t width,
        uint32_t height,
        uint32_t stride,
        uint32_t bytesPerPixel)>;

    PicoCamera() = default;
    ~PicoCamera() { Shutdown(); }

    bool LoadFunctions(XrInstance instance);
    bool Start(XrInstance instance, XrSession session);
    void Update(const FrameCallback& onFrame);
    void Shutdown();

    State GetState() const { return state_; }
    std::string GetStatusText() const { return statusText_; }
    uint32_t GetWidth() const { return chosenWidth_; }
    uint32_t GetHeight() const { return chosenHeight_; }

private:
    bool QueryAndPickConfig();

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSession session_ = XR_NULL_HANDLE;

    State state_ = State::Idle;
    std::string statusText_ = "待機中";

    XrCameraIdPICO cameraId_ = 0;
    uint32_t chosenWidth_ = 0;
    uint32_t chosenHeight_ = 0;
    XrCameraImageFpsPICO chosenFps_ = XR_CAMERA_IMAGE_FPS_30_PICO;

    XrFutureEXT deviceFuture_ = XR_NULL_FUTURE_EXT;
    XrCameraDevicePICO cameraDevice_ = XR_NULL_HANDLE;

    XrFutureEXT sessionFuture_ = XR_NULL_FUTURE_EXT;
    XrCameraCaptureSessionPICO captureSession_ = XR_NULL_HANDLE;

    XrTime lastCaptureTime_ = 0;
    bool capturing_ = false;

    // OpenXR function pointers
    PFN_xrPollFutureEXT xrPollFutureEXT_ = nullptr;
    PFN_xrCancelFutureEXT xrCancelFutureEXT_ = nullptr;

    PFN_xrEnumerateAvailableCamerasPICO xrEnumerateAvailableCamerasPICO_ = nullptr;
    PFN_xrGetCameraSupportedCapabilitiesPICO xrGetCameraSupportedCapabilitiesPICO_ = nullptr;
    PFN_xrCreateCameraDeviceAsyncPICO xrCreateCameraDeviceAsyncPICO_ = nullptr;
    PFN_xrCreateCameraDeviceCompletePICO xrCreateCameraDeviceCompletePICO_ = nullptr;
    PFN_xrDestroyCameraDevicePICO xrDestroyCameraDevicePICO_ = nullptr;
    PFN_xrCreateCameraCaptureSessionAsyncPICO xrCreateCameraCaptureSessionAsyncPICO_ = nullptr;
    PFN_xrCreateCameraCaptureSessionCompletePICO xrCreateCameraCaptureSessionCompletePICO_ = nullptr;
    PFN_xrDestroyCameraCaptureSessionPICO xrDestroyCameraCaptureSessionPICO_ = nullptr;
    PFN_xrBeginCameraCapturePICO xrBeginCameraCapturePICO_ = nullptr;
    PFN_xrEndCameraCapturePICO xrEndCameraCapturePICO_ = nullptr;
    PFN_xrAcquireCameraImagePICO xrAcquireCameraImagePICO_ = nullptr;
    PFN_xrGetCameraImageDataPICO xrGetCameraImageDataPICO_ = nullptr;
    PFN_xrReleaseCameraImagePICO xrReleaseCameraImagePICO_ = nullptr;
};
