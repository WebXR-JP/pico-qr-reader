#include "pico_camera.h"

#include <android/log.h>
#include <cmath>
#include <cstdlib>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PicoQrCamera", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "PicoQrCamera", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PicoQrCamera", __VA_ARGS__)

#define LOAD_XR_FN(inst, name)                                                              \
    do {                                                                                    \
        XrResult _r = xrGetInstanceProcAddr(inst, #name, reinterpret_cast<PFN_xrVoidFunction*>(&name##_)); \
        if (XR_FAILED(_r) || name##_ == nullptr) {                                          \
            LOGE("Failed to load %s (res=%d)", #name, (int)_r);                             \
            return false;                                                                   \
        }                                                                                   \
    } while (0)

bool PicoCamera::LoadFunctions(XrInstance instance) {
    LOAD_XR_FN(instance, xrPollFutureEXT);
    LOAD_XR_FN(instance, xrCancelFutureEXT);
    LOAD_XR_FN(instance, xrEnumerateAvailableCamerasPICO);
    LOAD_XR_FN(instance, xrGetCameraSupportedCapabilitiesPICO);
    LOAD_XR_FN(instance, xrCreateCameraDeviceAsyncPICO);
    LOAD_XR_FN(instance, xrCreateCameraDeviceCompletePICO);
    LOAD_XR_FN(instance, xrDestroyCameraDevicePICO);
    LOAD_XR_FN(instance, xrCreateCameraCaptureSessionAsyncPICO);
    LOAD_XR_FN(instance, xrCreateCameraCaptureSessionCompletePICO);
    LOAD_XR_FN(instance, xrDestroyCameraCaptureSessionPICO);
    LOAD_XR_FN(instance, xrBeginCameraCapturePICO);
    LOAD_XR_FN(instance, xrEndCameraCapturePICO);
    LOAD_XR_FN(instance, xrAcquireCameraImagePICO);
    LOAD_XR_FN(instance, xrGetCameraImageDataPICO);
    LOAD_XR_FN(instance, xrReleaseCameraImagePICO);

    // Optional calibration queries (may be missing on some runtime versions).
    xrGetInstanceProcAddr(instance, "xrGetCameraIntrinsicsPICO",
                          reinterpret_cast<PFN_xrVoidFunction*>(&xrGetCameraIntrinsicsPICO_));
    xrGetInstanceProcAddr(instance, "xrGetCameraExtrinsicsPICO",
                          reinterpret_cast<PFN_xrVoidFunction*>(&xrGetCameraExtrinsicsPICO_));
    LOGI("All XR_PICO_camera_image functions loaded (intrinsics=%d extrinsics=%d)",
         (int)(xrGetCameraIntrinsicsPICO_ != nullptr),
         (int)(xrGetCameraExtrinsicsPICO_ != nullptr));
    return true;
}

void PicoCamera::QueryIntrinsics() {
    intrinsics_ = {};
    if (!xrGetCameraIntrinsicsPICO_ || captureSession_ == XR_NULL_HANDLE) {
        LOGW("Camera intrinsics unavailable; renderer will use a fallback FOV");
        return;
    }

    XrCameraIntrinsicsPICO in{XR_TYPE_CAMERA_INTRINSICS_PICO};
    XrResult res = xrGetCameraIntrinsicsPICO_(captureSession_, &in);
    if (XR_FAILED(res)) {
        LOGW("xrGetCameraIntrinsicsPICO failed: %d", (int)res);
        return;
    }

    LOGI("Camera intrinsics: focal=(%.2f, %.2f) principal=(%.2f, %.2f) fov=(%.2f, %.2f) deg",
         in.focalLength.x, in.focalLength.y,
         in.principalPoint.x, in.principalPoint.y,
         in.fov.x, in.fov.y);

    intrinsics_.valid = true;
    intrinsics_.fx = in.focalLength.x;
    intrinsics_.fy = in.focalLength.y;
    intrinsics_.cx = in.principalPoint.x;
    intrinsics_.cy = in.principalPoint.y;
    intrinsics_.fovX = in.fov.x;
    intrinsics_.fovY = in.fov.y;
    intrinsics_.width = chosenWidth_;
    intrinsics_.height = chosenHeight_;

    if (xrGetCameraExtrinsicsPICO_) {
        XrCameraExtrinsicsPICO ex{XR_TYPE_CAMERA_EXTRINSICS_PICO};
        XrResult exRes = xrGetCameraExtrinsicsPICO_(captureSession_, &ex);
        if (XR_SUCCEEDED(exRes)) {
            LOGI("Camera extrinsics: pos=(%.3f, %.3f, %.3f) quat=(%.3f, %.3f, %.3f, %.3f)",
                 ex.pose.position.x, ex.pose.position.y, ex.pose.position.z,
                 ex.pose.orientation.x, ex.pose.orientation.y,
                 ex.pose.orientation.z, ex.pose.orientation.w);
        } else {
            LOGW("xrGetCameraExtrinsicsPICO failed: %d", (int)exRes);
        }
    }
}

bool PicoCamera::QueryAndPickConfig() {
    XrAvailableCamerasEnumerateInfoPICO enumInfo{XR_TYPE_AVAILABLE_CAMERAS_ENUMERATE_INFO_PICO};
    uint32_t camCount = 0;
    XrResult res = xrEnumerateAvailableCamerasPICO_(instance_, &enumInfo, 0, &camCount, nullptr);
    if (XR_FAILED(res) || camCount == 0) {
        LOGE("xrEnumerateAvailableCamerasPICO count failed: res=%d count=%u", (int)res, camCount);
        statusText_ = "カメラ未検出 (res=" + std::to_string((int)res) + ")";
        return false;
    }

    std::vector<XrCameraIdPICO> camIds(camCount);
    res = xrEnumerateAvailableCamerasPICO_(instance_, &enumInfo, camCount, &camCount, camIds.data());
    if (XR_FAILED(res) || camCount == 0) {
        LOGE("xrEnumerateAvailableCamerasPICO failed: res=%d", (int)res);
        statusText_ = "カメラ列挙失敗";
        return false;
    }

    cameraId_ = camIds[0];
    for (uint32_t i = 0; i < camCount; ++i) {
        LOGI("Available camera[%u]: id=%llu", i, (unsigned long long)camIds[i]);
    }
    if (preferredCameraId_ != 0) {
        bool found = false;
        for (uint32_t i = 0; i < camCount; ++i) {
            if (camIds[i] == preferredCameraId_) {
                cameraId_ = camIds[i];
                found = true;
                break;
            }
        }
        if (!found) {
            LOGE("Preferred camera id=%llu not found",
                 (unsigned long long)preferredCameraId_);
            statusText_ = "カメラID " + std::to_string((unsigned long long)preferredCameraId_) + " が見つかりません";
            return false;
        }
    } else {
        for (uint32_t i = 0; i < camCount; ++i) {
            if (camIds[i] == 1) {
                cameraId_ = camIds[i]; // Prefer RGB Left (id=1)
            }
        }
    }
    LOGI("Selected camera id=%llu (preferred=%llu)",
         (unsigned long long)cameraId_, (unsigned long long)preferredCameraId_);

    // Query supported resolutions and FPS
    XrCameraSupportedCapabilitiesGetInfoPICO capGetInfo{XR_TYPE_CAMERA_SUPPORTED_CAPABILITIES_GET_INFO_PICO};
    capGetInfo.id = cameraId_;

    XrCameraSupportedCapabilityImageResolutionPICO resCap{XR_TYPE_CAMERA_SUPPORTED_CAPABILITY_IMAGE_RESOLUTION_PICO};
    XrCameraSupportedCapabilityImageFpsPICO fpsCap{XR_TYPE_CAMERA_SUPPORTED_CAPABILITY_IMAGE_FPS_PICO};
    XrCameraSupportedCapabilityBaseHeaderPICO* capPtrs[2] = {
        reinterpret_cast<XrCameraSupportedCapabilityBaseHeaderPICO*>(&resCap),
        reinterpret_cast<XrCameraSupportedCapabilityBaseHeaderPICO*>(&fpsCap),
    };
    XrCameraSupportedCapabilitiesPICO caps{XR_TYPE_CAMERA_SUPPORTED_CAPABILITIES_PICO};
    caps.capabilityCount = 2;
    caps.capabilities = capPtrs;

    res = xrGetCameraSupportedCapabilitiesPICO_(instance_, &capGetInfo, &caps);
    if (XR_FAILED(res)) {
        LOGW("Query capability counts failed (%d), using fallback 1024x768 @ 30fps", (int)res);
        chosenWidth_ = 1024;
        chosenHeight_ = 768;
        chosenFps_ = XR_CAMERA_IMAGE_FPS_30_PICO;
        return true;
    }

    std::vector<XrExtent2Di> resolutions(resCap.resolutionCountOutput);
    std::vector<XrCameraImageFpsPICO> fpsList(fpsCap.fpsCountOutput);
    resCap.resolutionCapacityInput = (uint32_t)resolutions.size();
    resCap.resolutions = resolutions.data();
    fpsCap.fpsCapacityInput = (uint32_t)fpsList.size();
    fpsCap.fps = fpsList.data();

    res = xrGetCameraSupportedCapabilitiesPICO_(instance_, &capGetInfo, &caps);
    if (XR_SUCCEEDED(res) && !resolutions.empty()) {
        // Pick resolution closest to ~1152px wide for crisp QR decoding & fast speed
        int bestIdx = 0;
        int bestDist = 1000000;
        for (size_t i = 0; i < resolutions.size(); ++i) {
            LOGI("Supported resolution[%zu]: %dx%d", i, resolutions[i].width, resolutions[i].height);
            int dist = std::abs(resolutions[i].width - 1152);
            if (dist < bestDist) {
                bestDist = dist;
                bestIdx = (int)i;
            }
        }
        chosenWidth_ = (uint32_t)resolutions[bestIdx].width;
        chosenHeight_ = (uint32_t)resolutions[bestIdx].height;
    } else {
        chosenWidth_ = 1024;
        chosenHeight_ = 768;
    }

    chosenFps_ = XR_CAMERA_IMAGE_FPS_30_PICO;
    if (!fpsList.empty()) {
        chosenFps_ = fpsList[0];
        for (auto f : fpsList) {
            LOGI("Supported fps enum: %d", (int)f);
            if (f == XR_CAMERA_IMAGE_FPS_30_PICO) {
                chosenFps_ = f;
            }
        }
    }

    LOGI("Chosen config: %ux%u fpsEnum=%d", chosenWidth_, chosenHeight_, (int)chosenFps_);
    return true;
}

bool PicoCamera::Start(XrInstance instance, XrSession session, XrCameraIdPICO preferredCameraId) {
    if (state_ != State::Idle && state_ != State::Error) {
        return true;
    }
    instance_ = instance;
    session_ = session;
    preferredCameraId_ = preferredCameraId;

    if (!QueryAndPickConfig()) {
        state_ = State::Error;
        return false;
    }

    XrCameraDeviceCreateInfoPICO createInfo{XR_TYPE_CAMERA_DEVICE_CREATE_INFO_PICO};
    createInfo.cameraId = cameraId_;
    XrResult res = xrCreateCameraDeviceAsyncPICO_(instance_, &createInfo, &deviceFuture_);
    if (XR_FAILED(res)) {
        LOGE("xrCreateCameraDeviceAsyncPICO failed: %d", (int)res);
        statusText_ = "カメラ起動失敗 (res=" + std::to_string((int)res) + ")";
        state_ = State::Error;
        return false;
    }

    LOGI("xrCreateCameraDeviceAsyncPICO started, future=%llu", (unsigned long long)deviceFuture_);
    state_ = State::CreatingDevice;
    statusText_ = "カメラデバイス初期化中...";
    return true;
}

void PicoCamera::Update(const FrameCallback& onFrame) {
    if (state_ == State::CreatingDevice) {
        XrFuturePollInfoEXT pollInfo{XR_TYPE_FUTURE_POLL_INFO_EXT};
        pollInfo.future = deviceFuture_;
        XrFuturePollResultEXT pollResult{XR_TYPE_FUTURE_POLL_RESULT_EXT};
        XrResult res = xrPollFutureEXT_(instance_, &pollInfo, &pollResult);
        if (XR_SUCCEEDED(res) && pollResult.state == XR_FUTURE_STATE_READY_EXT) {
            XrCreateCameraDeviceCompletionPICO completion{XR_TYPE_CREATE_CAMERA_DEVICE_COMPLETION_PICO};
            res = xrCreateCameraDeviceCompletePICO_(instance_, deviceFuture_, &completion);
            deviceFuture_ = XR_NULL_FUTURE_EXT;
            if (XR_FAILED(res) || XR_FAILED(completion.futureResult)) {
                LOGE("CreateCameraDeviceComplete failed: res=%d futureRes=%d",
                     (int)res, (int)completion.futureResult);
                statusText_ = "カメラ初期化失敗 (" + std::to_string((int)completion.futureResult) + ")";
                state_ = State::Error;
                return;
            }
            cameraDevice_ = completion.device;
            LOGI("Camera device created: %p", (void*)cameraDevice_);

            // Create capture session
            XrCameraCapabilityImageResolutionPICO capRes{XR_TYPE_CAMERA_CAPABILITY_IMAGE_RESOLUTION_PICO};
            capRes.resolution = {(int32_t)chosenWidth_, (int32_t)chosenHeight_};

            XrCameraCapabilityImageFormatPICO capFmt{XR_TYPE_CAMERA_CAPABILITY_IMAGE_FORMAT_PICO};
            capFmt.format = XR_CAMERA_IMAGE_FORMAT_RGBA_8888_PICO;

            XrCameraCapabilityDataTransferTypePICO capXfer{XR_TYPE_CAMERA_CAPABILITY_DATA_TRANSFER_TYPE_PICO};
            capXfer.transferType = XR_CAMERA_DATA_TRANSFER_TYPE_RAW_BUFFER_PICO;

            XrCameraCapabilityCameraModelPICO capModel{XR_TYPE_CAMERA_CAPABILITY_CAMERA_MODEL_PICO};
            capModel.model = XR_CAMERA_MODEL_PINHOLE_PICO;

            XrCameraCapabilityImageFpsPICO capFps{XR_TYPE_CAMERA_CAPABILITY_IMAGE_FPS_PICO};
            capFps.fps = chosenFps_;

            const XrCameraCapabilityBaseHeaderPICO* configs[5] = {
                reinterpret_cast<const XrCameraCapabilityBaseHeaderPICO*>(&capRes),
                reinterpret_cast<const XrCameraCapabilityBaseHeaderPICO*>(&capFmt),
                reinterpret_cast<const XrCameraCapabilityBaseHeaderPICO*>(&capXfer),
                reinterpret_cast<const XrCameraCapabilityBaseHeaderPICO*>(&capModel),
                reinterpret_cast<const XrCameraCapabilityBaseHeaderPICO*>(&capFps),
            };

            XrCameraCaptureSessionCreateInfoPICO sessInfo{XR_TYPE_CAMERA_CAPTURE_SESSION_CREATE_INFO_PICO};
            sessInfo.camera = cameraDevice_;
            sessInfo.configCount = 5;
            sessInfo.configs = configs;

            res = xrCreateCameraCaptureSessionAsyncPICO_(session_, &sessInfo, &sessionFuture_);
            if (XR_FAILED(res)) {
                LOGE("xrCreateCameraCaptureSessionAsyncPICO failed: %d", (int)res);
                statusText_ = "セッション作成失敗 (" + std::to_string((int)res) + ")";
                state_ = State::Error;
                return;
            }
            LOGI("xrCreateCameraCaptureSessionAsyncPICO started");
            state_ = State::CreatingSession;
            statusText_ = "キャプチャセッション作成中...";
        }
        return;
    }

    if (state_ == State::CreatingSession) {
        XrFuturePollInfoEXT pollInfo{XR_TYPE_FUTURE_POLL_INFO_EXT};
        pollInfo.future = sessionFuture_;
        XrFuturePollResultEXT pollResult{XR_TYPE_FUTURE_POLL_RESULT_EXT};
        XrResult res = xrPollFutureEXT_(instance_, &pollInfo, &pollResult);
        if (XR_SUCCEEDED(res) && pollResult.state == XR_FUTURE_STATE_READY_EXT) {
            XrCreateCameraCaptureSessionCompletionPICO completion{
                XR_TYPE_CREATE_CAMERA_CAPTURE_SESSION_COMPLETION_PICO};
            res = xrCreateCameraCaptureSessionCompletePICO_(session_, sessionFuture_, &completion);
            sessionFuture_ = XR_NULL_FUTURE_EXT;
            if (XR_FAILED(res) || XR_FAILED(completion.futureResult)) {
                LOGE("CreateCameraCaptureSessionComplete failed: res=%d futureRes=%d",
                     (int)res, (int)completion.futureResult);
                statusText_ = "セッション完了失敗 (" + std::to_string((int)completion.futureResult) + ")";
                state_ = State::Error;
                return;
            }
            captureSession_ = completion.captureSession;
            LOGI("Capture session created: %p", (void*)captureSession_);

            QueryIntrinsics();

            XrCameraCaptureBeginInfoPICO beginInfo{XR_TYPE_CAMERA_CAPTURE_BEGIN_INFO_PICO};
            res = xrBeginCameraCapturePICO_(captureSession_, &beginInfo);
            if (XR_FAILED(res)) {
                LOGE("xrBeginCameraCapturePICO failed: %d", (int)res);
                statusText_ = "キャプチャ開始失敗 (" + std::to_string((int)res) + ")";
                state_ = State::Error;
                return;
            }
            capturing_ = true;
            lastCaptureTime_ = 0;
            state_ = State::Capturing;
            statusText_ = "スキャン中 (" + std::to_string(chosenWidth_) + "x" + std::to_string(chosenHeight_) + ")";
            LOGI("Camera capture started!");
        }
        return;
    }

    if (state_ == State::Capturing && captureSession_ != XR_NULL_HANDLE) {
        XrCameraImageAcquireInfoPICO acqInfo{XR_TYPE_CAMERA_IMAGE_ACQUIRE_INFO_PICO};
        acqInfo.lastCaptureTime = lastCaptureTime_;
        XrCameraImagePICO img{XR_TYPE_CAMERA_IMAGE_PICO};

        XrResult res = xrAcquireCameraImagePICO_(captureSession_, &acqInfo, &img);
        if (res == XR_CAMERA_IMAGE_NO_UPDATE_PICO) {
            return;
        }
        if (XR_FAILED(res)) {
            return;
        }
        if (img.imageId == 0) {
            return;
        }
        lastCaptureTime_ = img.captureTime;

        XrCameraImageDataRawBufferPICO rawBuf{XR_TYPE_CAMERA_IMAGE_DATA_RAW_BUFFER_PICO};
        res = xrGetCameraImageDataPICO_(
            captureSession_,
            img.imageId,
            reinterpret_cast<XrCameraImageDataBaseHeaderPICO*>(&rawBuf));
        if (XR_SUCCEEDED(res) && rawBuf.buffer != nullptr && rawBuf.width > 0 && rawBuf.height > 0) {
            if (onFrame) {
                onFrame(rawBuf.buffer, rawBuf.width, rawBuf.height, rawBuf.stride,
                        rawBuf.bytesPerPixel, img.captureTime);
            }
        }
        xrReleaseCameraImagePICO_(captureSession_, img.imageId);
    }
}

void PicoCamera::Shutdown() {
    if (capturing_ && captureSession_ != XR_NULL_HANDLE && xrEndCameraCapturePICO_) {
        xrEndCameraCapturePICO_(captureSession_);
        capturing_ = false;
    }
    if (captureSession_ != XR_NULL_HANDLE && xrDestroyCameraCaptureSessionPICO_) {
        xrDestroyCameraCaptureSessionPICO_(captureSession_);
        captureSession_ = XR_NULL_HANDLE;
    }
    if (cameraDevice_ != XR_NULL_HANDLE && xrDestroyCameraDevicePICO_) {
        xrDestroyCameraDevicePICO_(cameraDevice_);
        cameraDevice_ = XR_NULL_HANDLE;
    }
    state_ = State::Idle;
    lastCaptureTime_ = 0;
    intrinsics_ = {};
}
