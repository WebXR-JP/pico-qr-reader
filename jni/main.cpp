#include "pico_camera.h"

#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "quirc.h"
}

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PicoQrReader", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "PicoQrReader", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PicoQrReader", __VA_ARGS__)

namespace {

JavaVM* gJvm = nullptr;
std::atomic<bool> gStopRequested{false};
std::atomic<bool> gCameraPermGranted{false};

// Async QR decoder state
struct DecoderState {
    std::mutex mtx;
    std::vector<uint8_t> pendingGray;
    uint32_t pendingW = 0;
    uint32_t pendingH = 0;
    bool hasPending = false;
    bool stop = false;

    std::string lastQrText;
    std::chrono::steady_clock::time_point lastDetectTime{};
    bool hasQr = false;
    bool newQrForClipboard = false;
};

struct Mat4 {
    float m[16];
};

Mat4 MakeProjection(const XrFovf& fov, float nearZ, float farZ) {
    const float tanLeft = std::tan(fov.angleLeft);
    const float tanRight = std::tan(fov.angleRight);
    const float tanDown = std::tan(fov.angleDown);
    const float tanUp = std::tan(fov.angleUp);

    const float tanW = tanRight - tanLeft;
    const float tanH = tanUp - tanDown;

    Mat4 out{};
    out.m[0] = 2.0f / tanW;
    out.m[5] = 2.0f / tanH;
    out.m[8] = (tanRight + tanLeft) / tanW;
    out.m[9] = (tanUp + tanDown) / tanH;
    out.m[10] = -(farZ + nearZ) / (farZ - nearZ);
    out.m[11] = -1.0f;
    out.m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
    return out;
}

Mat4 MakeViewInverse(const XrPosef& pose) {
    const float x = pose.orientation.x;
    const float y = pose.orientation.y;
    const float z = pose.orientation.z;
    const float w = pose.orientation.w;

    // Rotation matrix R from quaternion
    const float r00 = 1.0f - 2.0f * (y * y + z * z);
    const float r01 = 2.0f * (x * y - z * w);
    const float r02 = 2.0f * (x * z + y * w);
    const float r10 = 2.0f * (x * y + z * w);
    const float r11 = 1.0f - 2.0f * (x * x + z * z);
    const float r12 = 2.0f * (y * z - x * w);
    const float r20 = 2.0f * (x * z - y * w);
    const float r21 = 2.0f * (y * z + x * w);
    const float r22 = 1.0f - 2.0f * (x * x + y * y);

    const float px = pose.position.x;
    const float py = pose.position.y;
    const float pz = pose.position.z;

    // View = R^T * T(-p), column-major
    Mat4 v{};
    v.m[0] = r00; v.m[4] = r10; v.m[8]  = r20; v.m[12] = -(r00 * px + r10 * py + r20 * pz);
    v.m[1] = r01; v.m[5] = r11; v.m[9]  = r21; v.m[13] = -(r01 * px + r11 * py + r21 * pz);
    v.m[2] = r02; v.m[6] = r12; v.m[10] = r22; v.m[14] = -(r02 * px + r12 * py + r22 * pz);
    v.m[3] = 0.0f; v.m[7] = 0.0f; v.m[11] = 0.0f; v.m[15] = 1.0f;
    return v;
}

Mat4 MulMat4(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            r.m[col * 4 + row] =
                a.m[0 * 4 + row] * b.m[col * 4 + 0] +
                a.m[1 * 4 + row] * b.m[col * 4 + 1] +
                a.m[2 * 4 + row] * b.m[col * 4 + 2] +
                a.m[3 * 4 + row] * b.m[col * 4 + 3];
        }
    }
    return r;
}

struct Vec3 {
    float x, y, z;
};

Vec3 RotateVec(const XrQuaternionf& q, const Vec3& v) {
    // v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
    const float tx = 2.0f * (q.y * v.z - q.z * v.y);
    const float ty = 2.0f * (q.z * v.x - q.x * v.z);
    const float tz = 2.0f * (q.x * v.y - q.y * v.x);
    return {
        v.x + q.w * tx + (q.y * tz - q.z * ty),
        v.y + q.w * ty + (q.z * tx - q.x * tz),
        v.z + q.w * tz + (q.x * ty - q.y * tx),
    };
}

XrQuaternionf QuatMul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

XrQuaternionf QuatFromAxisAngle(float ax, float ay, float az, float angle) {
    const float h = 0.5f * angle;
    const float s = std::sin(h);
    return {ax * s, ay * s, az * s, std::cos(h)};
}

XrQuaternionf QuatSlerp(const XrQuaternionf& a, const XrQuaternionf& b, float t) {
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    XrQuaternionf bb = b;
    if (dot < 0.0f) {
        dot = -dot;
        bb = {-b.x, -b.y, -b.z, -b.w};
    }
    float wa, wb;
    if (dot > 0.9995f) {
        wa = 1.0f - t;
        wb = t;
    } else {
        const float theta = std::acos(dot);
        const float sinTheta = std::sin(theta);
        wa = std::sin((1.0f - t) * theta) / sinTheta;
        wb = std::sin(t * theta) / sinTheta;
    }
    XrQuaternionf out{
        wa * a.x + wb * bb.x,
        wa * a.y + wb * bb.y,
        wa * a.z + wb * bb.z,
        wa * a.w + wb * bb.w,
    };
    const float n = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w);
    if (n > 1e-6f) {
        out.x /= n; out.y /= n; out.z /= n; out.w /= n;
    }
    return out;
}

// a * b : b is expressed in a's local frame
XrPosef PoseMul(const XrPosef& a, const XrPosef& b) {
    const Vec3 p = RotateVec(a.orientation, {b.position.x, b.position.y, b.position.z});
    XrPosef out;
    out.orientation = QuatMul(a.orientation, b.orientation);
    out.position = {a.position.x + p.x, a.position.y + p.y, a.position.z + p.z};
    return out;
}

// Model matrix for a quad in the XY plane (facing +Z) placed at `pose`.
Mat4 MakePoseScale(const XrPosef& pose, float sx, float sy) {
    const float x = pose.orientation.x;
    const float y = pose.orientation.y;
    const float z = pose.orientation.z;
    const float w = pose.orientation.w;

    const float r00 = 1.0f - 2.0f * (y * y + z * z);
    const float r01 = 2.0f * (x * y - z * w);
    const float r02 = 2.0f * (x * z + y * w);
    const float r10 = 2.0f * (x * y + z * w);
    const float r11 = 1.0f - 2.0f * (x * x + z * z);
    const float r12 = 2.0f * (y * z - x * w);
    const float r20 = 2.0f * (x * z - y * w);
    const float r21 = 2.0f * (y * z + x * w);
    const float r22 = 1.0f - 2.0f * (x * x + y * y);

    Mat4 m{};
    m.m[0] = r00 * sx; m.m[1] = r10 * sx; m.m[2] = r20 * sx; m.m[3] = 0.0f;
    m.m[4] = r01 * sy; m.m[5] = r11 * sy; m.m[6] = r21 * sy; m.m[7] = 0.0f;
    m.m[8] = r02;      m.m[9] = r12;      m.m[10] = r22;     m.m[11] = 0.0f;
    m.m[12] = pose.position.x;
    m.m[13] = pose.position.y;
    m.m[14] = pose.position.z;
    m.m[15] = 1.0f;
    return m;
}

GLuint CompileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("Shader compile error: %s", log);
    }
    return s;
}

GLuint CreateQuadProgram() {
    const char* vs = R"(#version 300 es
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aUv;
        uniform mat4 uMvp;
        uniform int uMode;        // 0 = world-space HUD quad, 1 = fullscreen reticle
        out vec2 vUv;
        out vec2 vScreen;
        void main() {
            vUv = aUv;
            vScreen = aPos + 0.5;
            gl_Position = uMode == 1 ? vec4(aPos * 2.0, 0.0, 1.0)
                                     : uMvp * vec4(aPos, 0.0, 1.0);
        }
    )";
    const char* fs = R"(#version 300 es
        precision highp int;
        precision mediump float;
        in vec2 vUv;
        in vec2 vScreen;
        uniform sampler2D uTex;
        uniform int uMode;        // 0 = HUD texture, 1 = reticle
        uniform float uDetected;
        out vec4 fragColor;
        void main() {
            if (uMode == 1) {
                // Small, unobtrusive aiming reticle at the center of view.
                // Output is premultiplied and transparent elsewhere, so the
                // system passthrough shows through.
                vec2 p = vScreen - 0.5;
                float crossH = (abs(p.x) < 0.0022 && abs(p.y) < 0.016) ? 1.0 : 0.0;
                float crossV = (abs(p.y) < 0.0022 && abs(p.x) < 0.016) ? 1.0 : 0.0;
                float cross = max(crossH, crossV);
                float dotMask = 1.0 - step(0.0035, length(p));
                vec3 retCol = mix(vec3(0.65, 0.85, 1.0), vec3(0.2, 0.95, 0.55), uDetected);
                float retA = clamp(cross * 0.55 + dotMask * 0.85, 0.0, 1.0);
                fragColor = vec4(retCol * retA, retA);
            } else {
                fragColor = texture(uTex, vUv);
            }
        }
    )";
    GLuint v = CompileShader(GL_VERTEX_SHADER, vs);
    GLuint f = CompileShader(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOGE("Program link error: %s", log);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

bool IsUrl(const std::string& s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

// JNI helpers
struct JniBridge {
    JNIEnv* env = nullptr;
    jobject activity = nullptr;
    jmethodID midHasCameraPerm = nullptr;
    jmethodID midCopyClipboard = nullptr;
    jmethodID midOpenUrl = nullptr;
    jmethodID midRenderHud = nullptr;

    bool Init(JNIEnv* e, jobject act) {
        env = e;
        activity = act;
        jclass cls = env->GetObjectClass(act);
        if (!cls) return false;
        midHasCameraPerm = env->GetMethodID(cls, "hasCameraPermission", "()Z");
        midCopyClipboard = env->GetMethodID(cls, "copyToClipboard", "(Ljava/lang/String;)V");
        midOpenUrl = env->GetMethodID(cls, "openUrl", "(Ljava/lang/String;)V");
        midRenderHud = env->GetMethodID(
            cls, "renderHudBitmap", "(IILjava/lang/String;Ljava/lang/String;ZZ)[B");
        env->DeleteLocalRef(cls);
        return midHasCameraPerm && midCopyClipboard && midOpenUrl && midRenderHud;
    }

    bool HasCameraPermission() {
        return env->CallBooleanMethod(activity, midHasCameraPerm) == JNI_TRUE;
    }

    void CopyToClipboard(const std::string& text) {
        jstring jtxt = env->NewStringUTF(text.c_str());
        env->CallVoidMethod(activity, midCopyClipboard, jtxt);
        env->DeleteLocalRef(jtxt);
    }

    void OpenUrl(const std::string& url) {
        jstring jurl = env->NewStringUTF(url.c_str());
        env->CallVoidMethod(activity, midOpenUrl, jurl);
        env->DeleteLocalRef(jurl);
    }

    bool RenderHudToTexture(GLuint tex, int w, int h,
                            const std::string& status,
                            const std::string& qrText,
                            bool isUrl,
                            bool detected) {
        jstring jStatus = env->NewStringUTF(status.c_str());
        jstring jQr = env->NewStringUTF(qrText.c_str());
        jbyteArray arr = static_cast<jbyteArray>(
            env->CallObjectMethod(activity, midRenderHud, w, h, jStatus, jQr,
                                  isUrl ? JNI_TRUE : JNI_FALSE,
                                  detected ? JNI_TRUE : JNI_FALSE));
        env->DeleteLocalRef(jStatus);
        env->DeleteLocalRef(jQr);
        if (env->ExceptionCheck()) {
            LOGE("renderHudBitmap threw an exception");
            env->ExceptionDescribe();
            env->ExceptionClear();
            return false;
        }
        if (!arr) {
            LOGE("renderHudBitmap returned null");
            return false;
        }

        jsize len = env->GetArrayLength(arr);
        if (len >= w * h * 4) {
            jbyte* bytes = env->GetByteArrayElements(arr, nullptr);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes);
            env->ReleaseByteArrayElements(arr, bytes, JNI_ABORT);
        }
        env->DeleteLocalRef(arr);
        return true;
    }
};

void RunDecoderWorker(DecoderState* st) {
    struct quirc* qr = quirc_new();
    if (!qr) {
        LOGE("quirc_new failed");
        return;
    }
    uint32_t curW = 0, curH = 0;

    while (true) {
        std::vector<uint8_t> gray;
        uint32_t w = 0, h = 0;
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            if (st->stop) break;
            if (st->hasPending) {
                gray.swap(st->pendingGray);
                w = st->pendingW;
                h = st->pendingH;
                st->hasPending = false;
            }
        }

        if (gray.empty() || w == 0 || h == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }

        if (w != curW || h != curH) {
            if (quirc_resize(qr, (int)w, (int)h) < 0) {
                LOGE("quirc_resize(%u, %u) failed", w, h);
                continue;
            }
            curW = w;
            curH = h;
        }

        int qw = 0, qh = 0;
        uint8_t* qbuf = quirc_begin(qr, &qw, &qh);
        if (qbuf && qw == (int)w && qh == (int)h) {
            std::memcpy(qbuf, gray.data(), (size_t)w * h);
        }
        quirc_end(qr);

        int count = quirc_count(qr);
        for (int i = 0; i < count; ++i) {
            struct quirc_code code;
            struct quirc_data data;
            quirc_extract(qr, i, &code);
            quirc_decode_error_t err = quirc_decode(&code, &data);
            if (err == QUIRC_SUCCESS && data.payload_len > 0) {
                std::string decoded(reinterpret_cast<const char*>(data.payload), data.payload_len);
                auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lk(st->mtx);
                bool isNew = !st->hasQr || st->lastQrText != decoded;
                st->lastQrText = decoded;
                st->lastDetectTime = now;
                st->hasQr = true;
                if (isNew) {
                    st->newQrForClipboard = true;
                    LOGI("QR DECODED: %s", decoded.c_str());
                }
                break;
            }
        }
    }

    quirc_destroy(qr);
}

} // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    gJvm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_pico_qrreader_MainActivity_nativeRequestStop(JNIEnv*, jobject) {
    gStopRequested.store(true);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_pico_qrreader_MainActivity_nativeOnCameraPermissionResult(JNIEnv*, jobject, jboolean granted) {
    gCameraPermGranted.store(granted == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_pico_qrreader_MainActivity_nativeMainLoop(JNIEnv* env, jobject thiz, jobject activity) {
    LOGI("android_main: entered (nativeMainLoop)");
    gStopRequested.store(false);

    JniBridge bridge;
    if (!bridge.Init(env, activity)) {
        LOGE("Failed to initialize JniBridge");
        return;
    }
    gCameraPermGranted.store(bridge.HasCameraPermission());

    // 1. Initialize OpenXR Android loader
    PFN_xrInitializeLoaderKHR pfnInitLoader = nullptr;
    XrResult res = xrGetInstanceProcAddr(
        XR_NULL_HANDLE, "xrInitializeLoaderKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&pfnInitLoader));
    if (XR_SUCCEEDED(res) && pfnInitLoader) {
        XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loaderInfo.applicationVM = gJvm;
        loaderInfo.applicationContext = activity;
        res = pfnInitLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo));
        LOGI("xrInitializeLoaderKHR -> %d", (int)res);
    }

    // 2. Enumerate extensions
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> extProps(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, extProps.data());

    auto hasExt = [&](const char* name) {
        for (const auto& e : extProps) {
            if (std::strcmp(e.extensionName, name) == 0) return true;
        }
        return false;
    };

    const bool hasPicoCamera = hasExt(XR_PICO_CAMERA_IMAGE_EXTENSION_NAME);
    const bool hasExtFuture = hasExt(XR_EXT_FUTURE_EXTENSION_NAME);
    const bool hasCameraExt = hasPicoCamera && hasExtFuture;
    LOGI("Extensions: total=%u pico_camera=%d ext_future=%d",
         extCount, (int)hasPicoCamera, (int)hasExtFuture);

    // Always attempt to request XR_PICO_camera_image + XR_EXT_future first
    // (some PICO runtimes accept vendor extensions even if omitted from enumeration).
    std::vector<const char*> enabledExts = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    };
    if (hasExtFuture) {
        enabledExts.push_back(XR_EXT_FUTURE_EXTENSION_NAME);
    }

    // 3. Create XrInstance
    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = gJvm;
    androidInfo.applicationActivity = activity;

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.next = &androidInfo;
    std::strncpy(createInfo.applicationInfo.applicationName, "PICO QR Reader", XR_MAX_APPLICATION_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    std::strncpy(createInfo.applicationInfo.engineName, "PicoQrNative", XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);

    XrInstance instance = XR_NULL_HANDLE;
    bool cameraExtEnabled = false;
    {
        auto tryExts = enabledExts;
        tryExts.push_back(XR_PICO_CAMERA_IMAGE_EXTENSION_NAME);
        createInfo.enabledExtensionCount = (uint32_t)tryExts.size();
        createInfo.enabledExtensionNames = tryExts.data();
        res = xrCreateInstance(&createInfo, &instance);
        LOGI("xrCreateInstance (with XR_PICO_camera_image) -> %d", (int)res);
        if (XR_SUCCEEDED(res)) {
            cameraExtEnabled = true;
        } else {
            createInfo.enabledExtensionCount = (uint32_t)enabledExts.size();
            createInfo.enabledExtensionNames = enabledExts.data();
            res = xrCreateInstance(&createInfo, &instance);
            LOGI("xrCreateInstance (fallback without XR_PICO_camera_image) -> %d", (int)res);
        }
    }
    if (XR_FAILED(res)) {
        LOGE("xrCreateInstance failed: %d", (int)res);
        return;
    }
    LOGI("xrCreateInstance succeeded (cameraExtEnabled=%d)", (int)cameraExtEnabled);

    // 4. Get System
    XrSystemGetInfo sysInfo{XR_TYPE_SYSTEM_GET_INFO};
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    res = xrGetSystem(instance, &sysInfo, &systemId);
    if (XR_FAILED(res)) {
        LOGE("xrGetSystem failed: %d", (int)res);
        xrDestroyInstance(instance);
        return;
    }

    // 4b. Environment blend modes: ALPHA_BLEND lets the runtime composite its
    //     own low-latency passthrough (same path as PICO's MR) behind our
    //     transparent layer. This is the only way the real world is shown.
    bool supportsAlphaBlend = false;
    {
        uint32_t modeCount = 0;
        xrEnumerateEnvironmentBlendModes(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                         0, &modeCount, nullptr);
        std::vector<XrEnvironmentBlendMode> modes(modeCount);
        if (modeCount > 0) {
            xrEnumerateEnvironmentBlendModes(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                             modeCount, &modeCount, modes.data());
        }
        for (auto m : modes) {
            LOGI("Supported environment blend mode: %d", (int)m);
            if (m == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) supportsAlphaBlend = true;
        }
        LOGI("ALPHA_BLEND passthrough supported: %d", (int)supportsAlphaBlend);
    }

    // 5. OpenGL ES requirements & EGL context
    PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetGlReqs = nullptr;
    xrGetInstanceProcAddr(instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetGlReqs));
    XrGraphicsRequirementsOpenGLESKHR glReqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (pfnGetGlReqs) {
        pfnGetGlReqs(instance, systemId, &glReqs);
    }

    EGLDisplay eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(eglDisplay, nullptr, nullptr);
    const EGLint cfgAttribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    EGLConfig eglConfig = nullptr;
    EGLint numConfigs = 0;
    eglChooseConfig(eglDisplay, cfgAttribs, &eglConfig, 1, &numConfigs);

    const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext eglContext = eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, ctxAttribs);
    const EGLint pbAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface eglSurface = eglCreatePbufferSurface(eglDisplay, eglConfig, pbAttribs);
    eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);

    // 6. Create XrSession
    XrGraphicsBindingOpenGLESAndroidKHR glBinding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    glBinding.display = eglDisplay;
    glBinding.config = eglConfig;
    glBinding.context = eglContext;

    XrSessionCreateInfo sessInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessInfo.next = &glBinding;
    sessInfo.systemId = systemId;

    XrSession session = XR_NULL_HANDLE;
    res = xrCreateSession(instance, &sessInfo, &session);
    if (XR_FAILED(res)) {
        LOGE("xrCreateSession failed: %d", (int)res);
        xrDestroyInstance(instance);
        return;
    }
    LOGI("xrCreateSession succeeded");

    // 7. Reference space (LOCAL so the HUD can be anchored in the air and
    //    smoothly trail the head; VIEW would make it rigidly head-locked)
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    XrSpace worldSpace = XR_NULL_HANDLE;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    res = xrCreateReferenceSpace(session, &spaceInfo, &worldSpace);
    if (XR_FAILED(res)) {
        LOGW("LOCAL reference space failed (%d), trying STAGE", (int)res);
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        res = xrCreateReferenceSpace(session, &spaceInfo, &worldSpace);
    }
    if (XR_FAILED(res)) {
        LOGW("STAGE reference space failed (%d), falling back to VIEW", (int)res);
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        res = xrCreateReferenceSpace(session, &spaceInfo, &worldSpace);
    }
    LOGI("Step 7 done: worldSpace=%p (type=%d)", (void*)worldSpace, (int)spaceInfo.referenceSpaceType);

    // 8. Controller action (Trigger / Select click to open URL or re-copy)
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction triggerAction = XR_NULL_HANDLE;
    {
        XrActionSetCreateInfo asInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strcpy(asInfo.actionSetName, "qr_actions");
        std::strcpy(asInfo.localizedActionSetName, "QR Actions");
        asInfo.priority = 0;
        if (XR_SUCCEEDED(xrCreateActionSet(instance, &asInfo, &actionSet))) {
            XrActionCreateInfo actInfo{XR_TYPE_ACTION_CREATE_INFO};
            actInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            std::strcpy(actInfo.actionName, "open_or_copy");
            std::strcpy(actInfo.localizedActionName, "Open URL or Copy");
            xrCreateAction(actionSet, &actInfo, &triggerAction);

            XrPath simpleProfile = XR_NULL_PATH, rightSelect = XR_NULL_PATH;
            xrStringToPath(instance, "/interaction_profiles/khr/simple_controller", &simpleProfile);
            xrStringToPath(instance, "/user/hand/right/input/select/click", &rightSelect);
            XrActionSuggestedBinding binding{triggerAction, rightSelect};
            XrInteractionProfileSuggestedBinding sugg{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            sugg.interactionProfile = simpleProfile;
            sugg.countSuggestedBindings = 1;
            sugg.suggestedBindings = &binding;
            xrSuggestInteractionProfileBindings(instance, &sugg);

            XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
            attachInfo.countActionSets = 1;
            attachInfo.actionSets = &actionSet;
            xrAttachSessionActionSets(session, &attachInfo);
        }
    }

    // 9. Views & Swapchains
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> viewConfigs(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, viewConfigs.data());

    std::vector<XrSwapchain> swapchains(viewCount, XR_NULL_HANDLE);
    std::vector<std::vector<XrSwapchainImageOpenGLESKHR>> swapchainImages(viewCount);
    for (uint32_t i = 0; i < viewCount; ++i) {
        XrSwapchainCreateInfo scInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        scInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        scInfo.format = GL_RGBA8;
        scInfo.sampleCount = 1;
        scInfo.width = viewConfigs[i].recommendedImageRectWidth;
        scInfo.height = viewConfigs[i].recommendedImageRectHeight;
        scInfo.faceCount = 1;
        scInfo.arraySize = 1;
        scInfo.mipCount = 1;
        xrCreateSwapchain(session, &scInfo, &swapchains[i]);

        uint32_t imgCount = 0;
        xrEnumerateSwapchainImages(swapchains[i], 0, &imgCount, nullptr);
        swapchainImages[i].resize(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(swapchains[i], imgCount, &imgCount,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchainImages[i].data()));
    }

    // 10. GL resources (FBO, quad VAO, shader, HUD texture)
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    GLuint program = CreateQuadProgram();
    GLint locMvp = glGetUniformLocation(program, "uMvp");
    GLint locTex = glGetUniformLocation(program, "uTex");
    GLint locMode = glGetUniformLocation(program, "uMode");
    GLint locDetected = glGetUniformLocation(program, "uDetected");

    const float quadVerts[] = {
        -0.5f, -0.5f, 0.0f, 1.0f,
         0.5f, -0.5f, 1.0f, 1.0f,
         0.5f,  0.5f, 1.0f, 0.0f,
        -0.5f, -0.5f, 0.0f, 1.0f,
         0.5f,  0.5f, 1.0f, 0.0f,
        -0.5f,  0.5f, 0.0f, 0.0f,
    };
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);

    GLuint hudTex = 0;
    glGenTextures(1, &hudTex);
    glBindTexture(GL_TEXTURE_2D, hudTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    uint32_t clearPx = 0x00000000;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &clearPx);

    const int kHudW = 1024;
    const int kHudH = 320;

    // 11. PICO Camera (left RGB, used only for QR decoding) + QR Decoder worker
    PicoCamera picoCam;
    bool camFnsLoaded = cameraExtEnabled && picoCam.LoadFunctions(instance);
    LOGI("Step 11 done: entering main loop (camFnsLoaded=%d alphaBlend=%d)",
         (int)camFnsLoaded, (int)supportsAlphaBlend);
    DecoderState decoderState;
    std::thread decoderThread(RunDecoderWorker, &decoderState);

    std::string lastHudStatus;
    std::string lastHudQr;
    bool lastHudDetected = false;
    auto lastDecodeSubmit = std::chrono::steady_clock::now();

    bool sessionBegun = false;
    bool exitLoop = false;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool prevTriggerState = false;

    // Floating HUD: world-anchored target pose relative to the head, with a
    // damped follow so it drifts behind the view and glides back into place.
    const float kHudPitch = -14.0f * 3.14159265358979f / 180.0f;
    const XrPosef kHudOffsetPose = {
        QuatFromAxisAngle(1.0f, 0.0f, 0.0f, kHudPitch),
        {0.0f, -0.30f, -1.10f},
    };
    const float kHudWidth = 0.72f;
    const float kHudHeight = 0.225f;
    XrPosef hudPose{};
    bool hudPoseInitialized = false;
    XrTime lastFrameTime = 0;

    while (!exitLoop && !gStopRequested.load()) {
        // Poll OpenXR events
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                sessionState = sc->state;
                LOGI("Session state -> %d", (int)sessionState);
                // PICO PxrRuntime requires calling xrBeginSession in IDLE (with begin-once guard)
                if ((sessionState == XR_SESSION_STATE_IDLE || sessionState == XR_SESSION_STATE_READY) && !sessionBegun) {
                    XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XrResult br = xrBeginSession(session, &beginInfo);
                    LOGI("xrBeginSession (state=%d) -> %d", (int)sessionState, (int)br);
                    if (XR_SUCCEEDED(br)) {
                        sessionBegun = true;
                    }
                } else if (sessionState == XR_SESSION_STATE_STOPPING) {
                    picoCam.Shutdown();
                    if (sessionBegun) {
                        xrEndSession(session);
                        sessionBegun = false;
                    }
                } else if (sessionState == XR_SESSION_STATE_EXITING ||
                           sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                    exitLoop = true;
                }
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }

        if (!sessionBegun) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        // Start the camera once session is VISIBLE/FOCUSED and CAMERA permission is granted.
        const bool sessionVisible = (sessionState == XR_SESSION_STATE_VISIBLE ||
                                     sessionState == XR_SESSION_STATE_FOCUSED);
        if (sessionVisible && (gCameraPermGranted.load() || bridge.HasCameraPermission())) {
            gCameraPermGranted.store(true);
            if (camFnsLoaded && picoCam.GetState() == PicoCamera::State::Idle) {
                picoCam.Start(instance, session, 1);  // RGB left
            }
        }

        // Sync controller actions
        if (actionSet != XR_NULL_HANDLE && sessionState == XR_SESSION_STATE_FOCUSED) {
            XrActiveActionSet active{actionSet, XR_NULL_PATH};
            XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
            syncInfo.countActiveActionSets = 1;
            syncInfo.activeActionSets = &active;
            if (XR_SUCCEEDED(xrSyncActions(session, &syncInfo))) {
                XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
                getInfo.action = triggerAction;
                XrActionStateBoolean bState{XR_TYPE_ACTION_STATE_BOOLEAN};
                if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &getInfo, &bState))) {
                    bool pressed = (bState.isActive == XR_TRUE && bState.currentState == XR_TRUE);
                    if (pressed && !prevTriggerState) {
                        std::string qr;
                        {
                            std::lock_guard<std::mutex> lk(decoderState.mtx);
                            if (decoderState.hasQr) qr = decoderState.lastQrText;
                        }
                        if (!qr.empty()) {
                            if (IsUrl(qr)) {
                                bridge.OpenUrl(qr);
                            } else {
                                bridge.CopyToClipboard(qr);
                            }
                        }
                    }
                    prevTriggerState = pressed;
                }
            }
        }

        // Wait for the frame first, so the camera image acquired below is as
        // fresh as possible for decoding.
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(session, &waitInfo, &frameState))) {
            continue;
        }

        // Pull the latest camera frame and feed the QR decoder
        if (camFnsLoaded && picoCam.GetState() != PicoCamera::State::Idle) {
            static bool loggedFirstFrame = false;
            picoCam.Update([&](const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t stride, uint32_t bpp, XrTime) {
                if (bpp != 4) return;
                if (!loggedFirstFrame) {
                    loggedFirstFrame = true;
                    const uint8_t* mid = rgba + (size_t)(h / 2) * stride + (size_t)(w / 2) * 4;
                    LOGI("First camera frame: %ux%u stride=%u bpp=%u centerRGBA=(%u,%u,%u,%u)",
                         w, h, stride, bpp, mid[0], mid[1], mid[2], mid[3]);
                }

                // Throttle QR decode to ~8 Hz
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDecodeSubmit).count() >= 120) {
                    std::lock_guard<std::mutex> lk(decoderState.mtx);
                    if (!decoderState.hasPending) {
                        decoderState.pendingGray.resize((size_t)w * h);
                        uint8_t* dst = decoderState.pendingGray.data();
                        for (uint32_t y = 0; y < h; ++y) {
                            const uint8_t* row = rgba + (size_t)y * stride;
                            uint8_t* drow = dst + (size_t)y * w;
                            for (uint32_t x = 0; x < w; ++x) {
                                const uint8_t r = row[x * 4 + 0];
                                const uint8_t g = row[x * 4 + 1];
                                const uint8_t b = row[x * 4 + 2];
                                drow[x] = (uint8_t)((77u * r + 150u * g + 29u * b) >> 8);
                            }
                        }
                        decoderState.pendingW = w;
                        decoderState.pendingH = h;
                        decoderState.hasPending = true;
                        lastDecodeSubmit = now;
                    }
                }
            });
        }

        // Check decoded QR status & update HUD texture if changed
        std::string curStatus;
        if (!cameraExtEnabled) {
            curStatus = "XR_PICO_camera_image 非対応";
        } else if (!gCameraPermGranted.load()) {
            curStatus = "カメラ権限を許可してください";
        } else {
            curStatus = picoCam.GetStatusText();
        }
        if (!supportsAlphaBlend) {
            curStatus += "  |  パススルー非対応";
        }

        std::string curQr;
        bool recentlyDetected = false;
        bool shouldCopy = false;
        {
            std::lock_guard<std::mutex> lk(decoderState.mtx);
            if (decoderState.hasQr) {
                curQr = decoderState.lastQrText;
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - decoderState.lastDetectTime).count();
                recentlyDetected = (elapsed < 2500);
                if (decoderState.newQrForClipboard) {
                    shouldCopy = true;
                    decoderState.newQrForClipboard = false;
                }
            }
        }
        if (shouldCopy && !curQr.empty()) {
            bridge.CopyToClipboard(curQr);
        }

        if (curStatus != lastHudStatus || curQr != lastHudQr || recentlyDetected != lastHudDetected) {
            bool hudOk = bridge.RenderHudToTexture(hudTex, kHudW, kHudH, curStatus, curQr, IsUrl(curQr), recentlyDetected);
            static int hudUpdateLogs = 0;
            if (hudUpdateLogs < 3) {
                hudUpdateLogs++;
                LOGI("HUD update: ok=%d status='%s' qrLen=%zu", (int)hudOk,
                     curStatus.c_str(), curQr.size());
            }
            lastHudStatus = curStatus;
            lastHudQr = curQr;
            lastHudDetected = recentlyDetected;
        }

        // OpenXR Frame (xrWaitFrame already done above)
        XrFrameBeginInfo beginFrameInfo{XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(session, &beginFrameInfo);

        std::vector<XrCompositionLayerProjectionView> projViews(viewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
        bool rendered = false;

        if (frameState.shouldRender == XR_TRUE) {
            XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locateInfo.displayTime = frameState.predictedDisplayTime;
            locateInfo.space = worldSpace;

            XrViewState viewState{XR_TYPE_VIEW_STATE};
            std::vector<XrView> views(viewCount, {XR_TYPE_VIEW});
            uint32_t outViews = 0;
            if (XR_SUCCEEDED(xrLocateViews(session, &locateInfo, &viewState, viewCount, &outViews, views.data()))) {
                rendered = true;
                {
                    static bool loggedEyeFov = false;
                    if (!loggedEyeFov && outViews > 0) {
                        loggedEyeFov = true;
                        LOGI("Eye FOV deg: L=%.1f R=%.1f D=%.1f U=%.1f",
                             views[0].fov.angleLeft * 57.29578f,
                             views[0].fov.angleRight * 57.29578f,
                             views[0].fov.angleDown * 57.29578f,
                             views[0].fov.angleUp * 57.29578f);
                    }
                }

                // ---- Head pose + damped floating HUD pose ----
                XrPosef headPose = views[0].pose;
                if (outViews > 1) {
                    float px = 0.0f, py = 0.0f, pz = 0.0f;
                    for (uint32_t i = 0; i < outViews; ++i) {
                        px += views[i].pose.position.x;
                        py += views[i].pose.position.y;
                        pz += views[i].pose.position.z;
                    }
                    const float inv = 1.0f / (float)outViews;
                    headPose.position = {px * inv, py * inv, pz * inv};
                }
                const XrPosef hudTarget = PoseMul(headPose, kHudOffsetPose);

                float dt = 0.0f;
                if (lastFrameTime != 0 && frameState.predictedDisplayTime > lastFrameTime) {
                    dt = (float)(frameState.predictedDisplayTime - lastFrameTime) * 1e-9f;
                }
                lastFrameTime = frameState.predictedDisplayTime;
                if (dt < 0.0f) dt = 0.0f;
                if (dt > 0.05f) dt = 0.05f;

                if (!hudPoseInitialized) {
                    hudPose = hudTarget;
                    hudPoseInitialized = true;
                } else if (dt > 0.0f) {
                    const float kFollowTau = 0.16f;  // larger = lazier follow
                    const float k = 1.0f - std::exp(-dt / kFollowTau);
                    hudPose.position.x += (hudTarget.position.x - hudPose.position.x) * k;
                    hudPose.position.y += (hudTarget.position.y - hudPose.position.y) * k;
                    hudPose.position.z += (hudTarget.position.z - hudPose.position.z) * k;
                    hudPose.orientation = QuatSlerp(hudPose.orientation, hudTarget.orientation, k);
                }

                for (uint32_t i = 0; i < viewCount; ++i) {
                    uint32_t imgIdx = 0;
                    XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    xrAcquireSwapchainImage(swapchains[i], &acq, &imgIdx);
                    XrSwapchainImageWaitInfo waitImg{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    waitImg.timeout = XR_INFINITE_DURATION;
                    xrWaitSwapchainImage(swapchains[i], &waitImg);
                    const int w = (int)viewConfigs[i].recommendedImageRectWidth;
                    const int h = (int)viewConfigs[i].recommendedImageRectHeight;

                    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                           swapchainImages[i][imgIdx].image, 0);
                    glViewport(0, 0, w, h);
                    glEnable(GL_BLEND);
                    // Keep destination alpha meaningful (premultiplied) so the
                    // compositor can blend our layer over system passthrough.
                    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    glDisable(GL_DEPTH_TEST);

                    // Transparent clear so the system passthrough shows through
                    // (dark slate if the runtime cannot do ALPHA_BLEND).
                    if (supportsAlphaBlend) {
                        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                    } else {
                        glClearColor(0.02f, 0.025f, 0.035f, 1.0f);
                    }
                    glClear(GL_COLOR_BUFFER_BIT);

                    Mat4 proj = MakeProjection(views[i].fov, 0.05f, 100.0f);
                    Mat4 view = MakeViewInverse(views[i].pose);
                    Mat4 vp = MulMat4(proj, view);

                    glUseProgram(program);
                    glBindVertexArray(vao);
                    glActiveTexture(GL_TEXTURE0);
                    glUniform1i(locTex, 0);

                    // 1) Aiming reticle at the center of view
                    glUniform1f(locDetected, recentlyDetected ? 1.0f : 0.0f);
                    glUniform1i(locMode, 1);
                    glDrawArrays(GL_TRIANGLES, 0, 6);

                    // 2) Floating HUD text panel (world space, damped follow)
                    Mat4 mHud = MulMat4(vp, MakePoseScale(hudPose, kHudWidth, kHudHeight));
                    glUniformMatrix4fv(locMvp, 1, GL_FALSE, mHud.m);
                    glUniform1i(locMode, 0);
                    glBindTexture(GL_TEXTURE_2D, hudTex);
                    glDrawArrays(GL_TRIANGLES, 0, 6);

                    glBindVertexArray(0);
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);

                    XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    xrReleaseSwapchainImage(swapchains[i], &rel);

                    projViews[i].pose = views[i].pose;
                    projViews[i].fov = views[i].fov;
                    projViews[i].subImage.swapchain = swapchains[i];
                    projViews[i].subImage.imageRect = {{0, 0}, {w, h}};
                    projViews[i].subImage.imageArrayIndex = 0;
                }
            }
        }

        std::vector<const XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerProjection projLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        if (rendered) {
            projLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            projLayer.space = worldSpace;
            projLayer.viewCount = viewCount;
            projLayer.views = projViews.data();
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projLayer));
        }

        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = supportsAlphaBlend ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
                                                          : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = (uint32_t)layers.size();
        endInfo.layers = layers.empty() ? nullptr : layers.data();
        XrResult endRes = xrEndFrame(session, &endInfo);
        if (XR_FAILED(endRes)) {
            static int endFrameErrLogs = 0;
            if (endFrameErrLogs < 3) {
                endFrameErrLogs++;
                LOGE("xrEndFrame failed: %d", (int)endRes);
            }
        }
    }

    // Cleanup
    {
        std::lock_guard<std::mutex> lk(decoderState.mtx);
        decoderState.stop = true;
    }
    if (decoderThread.joinable()) decoderThread.join();

    picoCam.Shutdown();
    for (auto sc : swapchains) {
        if (sc != XR_NULL_HANDLE) xrDestroySwapchain(sc);
    }
    if (worldSpace != XR_NULL_HANDLE) xrDestroySpace(worldSpace);
    if (actionSet != XR_NULL_HANDLE) xrDestroyActionSet(actionSet);
    if (session != XR_NULL_HANDLE) xrDestroySession(session);
    if (instance != XR_NULL_HANDLE) xrDestroyInstance(instance);

    glDeleteTextures(1, &hudTex);
    glDeleteProgram(program);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteFramebuffers(1, &fbo);
    eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(eglDisplay, eglSurface);
    eglDestroyContext(eglDisplay, eglContext);
    eglTerminate(eglDisplay);

    LOGI("nativeMainLoop exited cleanly");
}
