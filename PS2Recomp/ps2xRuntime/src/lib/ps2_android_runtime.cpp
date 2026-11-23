#include "runtime/ps2_android_runtime.h"
#include "runtime/native_renderer.h"
#include "ps2_runtime.h"

#include <EGL/egl.h>
#include <android/log.h>
#include <jni.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>

namespace
{
    std::mutex stateMutex;
    std::condition_variable pauseChanged;
    ps2x::android::LaunchPaths paths;
    ps2x::android::PadInput input;
    std::atomic<bool> isPaused{false};

    EGLDisplay workerDisplay = EGL_NO_DISPLAY;
    EGLSurface workerSurface = EGL_NO_SURFACE;
    EGLContext workerContext = EGL_NO_CONTEXT;

    std::string javaString(JNIEnv *env, jstring value)
    {
        if (!value)
            return {};
        const char *chars = env->GetStringUTFChars(value, nullptr);
        if (!chars)
            return {};
        std::string result(chars);
        env->ReleaseStringUTFChars(value, chars);
        return result;
    }

    float stickAxis(float axis)
    {
        return std::isfinite(axis) ? std::clamp(axis, -1.0f, 1.0f) : 0.0f;
    }
}

namespace ps2x::android
{
    LaunchPaths launchPaths()
    {
        std::lock_guard lock(stateMutex);
        return paths;
    }

    PadInput padInput()
    {
        std::lock_guard lock(stateMutex);
        return input;
    }

    bool paused()
    {
        return isPaused.load(std::memory_order_relaxed);
    }

    std::chrono::steady_clock::duration waitWhilePaused(const std::atomic<bool> &stopRequested)
    {
        if (!paused())
            return {};
        std::unique_lock lock(stateMutex);
        const auto begin = std::chrono::steady_clock::now();
        pauseChanged.wait(lock, [&] { return !paused() || stopRequested.load(std::memory_order_relaxed); });
        return std::chrono::steady_clock::now() - begin;
    }

    void wakeGuest()
    {
        std::lock_guard lock(stateMutex);
        pauseChanged.notify_all();
    }

    bool initializeRenderer()
    {
        // raylib owns this display and has already initialized EGL. The native
        // renderer keeps its context current on its own worker thread.
        workerDisplay = eglGetCurrentDisplay();
        if (workerDisplay == EGL_NO_DISPLAY)
            return false;
        constexpr EGLint configAttributes[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_NONE};
        constexpr EGLint surfaceAttributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        constexpr EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint count = 0;
        if (eglChooseConfig(workerDisplay, configAttributes, &config, 1, &count) && count > 0)
        {
            workerSurface = eglCreatePbufferSurface(workerDisplay, config, surfaceAttributes);
            if (workerSurface != EGL_NO_SURFACE)
                workerContext = eglCreateContext(workerDisplay, config, EGL_NO_CONTEXT, contextAttributes);
        }
        if (workerContext == EGL_NO_CONTEXT)
        {
            __android_log_print(ANDROID_LOG_ERROR, "ps2x", "Could not create renderer EGL context (0x%x)", eglGetError());
            shutdownRenderer();
            return false;
        }
        native_renderer::setGlContext([]
        {
            if (!eglMakeCurrent(workerDisplay, workerSurface, workerSurface, workerContext))
                __android_log_print(ANDROID_LOG_ERROR, "ps2x", "Could not bind renderer EGL context (0x%x)", eglGetError());
        });
        return true;
    }

    void shutdownRenderer()
    {
        native_renderer::shutdownGl();
        if (workerContext != EGL_NO_CONTEXT)
            eglDestroyContext(workerDisplay, workerContext);
        if (workerSurface != EGL_NO_SURFACE)
            eglDestroySurface(workerDisplay, workerSurface);
        workerDisplay = EGL_NO_DISPLAY;
        workerSurface = EGL_NO_SURFACE;
        workerContext = EGL_NO_CONTEXT;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_genisson_sotc_NativeGameActivity_nativeConfigure(JNIEnv *env, jclass, jstring discDirectory, jstring bootElf,
                                                        jstring saveDirectory, jstring logFile, jint resolutionPercent)
{
    std::lock_guard lock(stateMutex);
    paths = {javaString(env, discDirectory), javaString(env, bootElf), javaString(env, saveDirectory),
             javaString(env, logFile), std::clamp<int>(resolutionPercent, 50, 400)};
    input = {};
    isPaused.store(false, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_genisson_sotc_NativeGameActivity_nativeSetInput(JNIEnv *, jclass, jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry)
{
    std::lock_guard lock(stateMutex);
    if (!isPaused.load(std::memory_order_relaxed))
        input = {static_cast<uint16_t>(buttons), stickAxis(lx), stickAxis(ly), stickAxis(rx), stickAxis(ry)};
}

extern "C" JNIEXPORT void JNICALL
Java_com_genisson_sotc_NativeGameActivity_nativeReleaseInput(JNIEnv *, jclass)
{
    std::lock_guard lock(stateMutex);
    input = {};
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_genisson_sotc_NativeGameActivity_nativeHasGame(JNIEnv *, jclass)
{
    return g_ps2RecompiledFunctionTableSlotCount != 0 ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_genisson_sotc_NativeGameActivity_nativePause(JNIEnv *, jclass, jboolean paused)
{
    {
        std::lock_guard lock(stateMutex);
        isPaused.store(paused != JNI_FALSE, std::memory_order_relaxed);
        if (paused)
            input = {};
    }
    pauseChanged.notify_all();
}
