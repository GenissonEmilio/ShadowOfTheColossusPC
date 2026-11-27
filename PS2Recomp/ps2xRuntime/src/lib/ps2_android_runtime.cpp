#include "runtime/ps2_android_runtime.h"
#include "runtime/android_state_identity.h"
#include "runtime/desktop_video.h"
#include "runtime/host_menu.h"
#include "runtime/native_renderer.h"
#include "runtime/native_tuning.h"
#include "runtime/state_manager.h"
#include "ps2_runtime.h"

#include <EGL/egl.h>
#include <android/log.h>
#include <jni.h>
#include <link.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace
{
    std::mutex stateMutex;
    std::condition_variable pauseChanged;
    ps2x::android::LaunchPaths paths;
    ps2x::android::PadInput input;
    constexpr uint16_t kStart = 1 << 3;
    bool mapPressPending = false;
    std::atomic<bool> isPaused{false};
    std::atomic<float> requestedAspect{0.0f};
    std::atomic<float> activeAspect{0.0f};
    std::atomic<float> refreshRate{60.0f};

    // Keep this order aligned with GameOptions.Option in the Android UI.
    enum Option
    {
        Preset, RenderScale, Fxaa, TextureFilter, ModelDetail, EffectDetail,
        FrameGeneration, Cap30, ShowFps, Volume, ScreenMode, Vsync, UpscaleFsr,
        Bloom, BloomStrength, BloomRounds, BloomSpread, VertexFog, FogAmount, DepthFog,
        Glow, DepthOfField, Shadows, Particles, ScreenBlur, Feedback, Reflections, Refraction,
        OptionCount
    };
    using Options = std::array<float, OptionCount>;
    std::mutex optionsMutex;
    bool optionsReady = false;
    Options pendingOptions = [] { Options values; values.fill(NAN); return values; }();
    std::atomic<int> presentationOptions{0};
    std::atomic<int> maxTextureFilter{2};
    constexpr int kFxaaSupported = 1 << 0;
    constexpr int kFrameGenerationSupported = 1 << 1;
    constexpr int kUpscaleFsrSupported = 1 << 2;
    constexpr int kSaveStatesSupported = 1 << 3;

    constexpr native_renderer::Preset kPresets[] = {
        native_renderer::kPresetLow, native_renderer::kPresetMedium, native_renderer::kPresetBalanced,
        native_renderer::kPresetEnhanced, native_renderer::kPresetDesktopUltra,
    };
    struct NumberOption
    {
        Option option;
        float native_renderer::Tuning::*member;
        float min, max, step;
    };
    constexpr NumberOption kNumbers[] = {
        {RenderScale, &native_renderer::Tuning::renderScale, 0.5f, 4.0f, 0.5f},
        {TextureFilter, &native_renderer::Tuning::textureFilter, 0, 6, 1},
        {ModelDetail, &native_renderer::Tuning::modelDetail, 0, 2, 1},
        {EffectDetail, &native_renderer::Tuning::effectDetail, 0, 2, 1},
        {BloomStrength, &native_renderer::Tuning::bloomStrength, 0, 2, 0.02f},
        {BloomRounds, &native_renderer::Tuning::bloomRounds, 1, 6, 1},
        {BloomSpread, &native_renderer::Tuning::bloomSpread, 0, 4, 0.1f},
        {FogAmount, &native_renderer::Tuning::vertexFogAmount, 0, 2, 0.05f},
    };
    struct ToggleOption
    {
        Option option;
        bool native_renderer::Tuning::*member;
    };
    constexpr ToggleOption kToggles[] = {
        {Fxaa, &native_renderer::Tuning::fxaa}, {Cap30, &native_renderer::Tuning::frameCap30},
        {Bloom, &native_renderer::Tuning::bloom}, {VertexFog, &native_renderer::Tuning::vertexFog},
        {DepthFog, &native_renderer::Tuning::zFog}, {Glow, &native_renderer::Tuning::glow},
        {DepthOfField, &native_renderer::Tuning::depthOfField}, {Shadows, &native_renderer::Tuning::shadows},
        {Particles, &native_renderer::Tuning::particles}, {ScreenBlur, &native_renderer::Tuning::screenBlur},
        {Feedback, &native_renderer::Tuning::feedback}, {Reflections, &native_renderer::Tuning::reflections},
        {Refraction, &native_renderer::Tuning::refraction},
    };

    int optionIndex(float value, int max)
    {
        return static_cast<int>(std::round(std::clamp(value, 0.0f, static_cast<float>(max))));
    }

    void applyOptions(const Options &values)
    {
        const auto has = [&](Option option) { return std::isfinite(values[option]); };
        native_renderer::updateTuning([&](native_renderer::Tuning &tuning)
        {
            if (has(Preset))
                native_renderer::applyPreset(tuning, kPresets[optionIndex(values[Preset], std::size(kPresets) - 1)]);
            for (const auto &number : kNumbers)
                if (has(number.option))
                    tuning.*number.member = std::clamp(
                        std::round(std::clamp(values[number.option], number.min, number.max) / number.step) * number.step,
                        number.min, number.max);
            for (const auto &toggle : kToggles)
                if (has(toggle.option))
                    tuning.*toggle.member = values[toggle.option] >= 0.5f;
            tuning.preset = static_cast<float>(native_renderer::matchingPreset(tuning));
        });
        auto &settings = hostmenu::settings();
        if (has(ShowFps)) settings.showFps = values[ShowFps] >= 0.5f;
        if (has(Volume)) settings.volume = optionIndex(values[Volume], 10);
        if (has(ScreenMode)) settings.screenMode = optionIndex(values[ScreenMode], hostmenu::kScreenInteger);
#ifdef PS2X_DESKTOP_VIDEO
        if (has(FrameGeneration)) settings.frameGeneration = values[FrameGeneration] >= 0.5f;
        if (has(Vsync)) settings.vsync = values[Vsync] >= 0.5f;
        if (has(UpscaleFsr)) settings.upscaleFsr = values[UpscaleFsr] >= 0.5f;
#endif
    }

    Options readOptions()
    {
        Options values{};
        const auto &tuning = native_renderer::tuning();
        const auto *preset = std::find(std::begin(kPresets), std::end(kPresets), native_renderer::matchingPreset(tuning));
        values[Preset] = static_cast<float>(preset - std::begin(kPresets)); // the end index is Custom
        for (const auto &number : kNumbers)
            values[number.option] = tuning.*number.member;
        for (const auto &toggle : kToggles)
            values[toggle.option] = tuning.*toggle.member ? 1.0f : 0.0f;
        auto &settings = hostmenu::settings();
        values[ShowFps] = settings.showFps.load() ? 1.0f : 0.0f;
        values[Volume] = static_cast<float>(settings.volume.load());
        values[ScreenMode] = static_cast<float>(std::min(settings.screenMode.load(), static_cast<int>(hostmenu::kScreenInteger)));
#ifdef PS2X_DESKTOP_VIDEO
        values[FrameGeneration] = settings.frameGeneration.load() ? 1.0f : 0.0f;
        values[Vsync] = settings.vsync.load() ? 1.0f : 0.0f;
        values[UpscaleFsr] = settings.upscaleFsr.load() ? 1.0f : 0.0f;
#endif
        return values;
    }

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

    int findRunnerBuildId(dl_phdr_info *info, size_t, void *data)
    {
        // An APK may map the library straight from its ZIP entry. The
        // loaded ELF build ID identifies the same code in either location.
        const auto address = reinterpret_cast<uintptr_t>(&ps2x::android::runnerBuildId);
        bool containsRunner = false;
        for (unsigned i = 0; i < info->dlpi_phnum; ++i)
        {
            const auto &ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_LOAD)
                continue;
            const auto start = info->dlpi_addr + ph.p_vaddr;
            containsRunner |= address >= start && address - start < ph.p_memsz;
        }
        if (!containsRunner)
            return 0;

        auto &identity = *static_cast<std::vector<uint8_t> *>(data);
        for (unsigned i = 0; i < info->dlpi_phnum; ++i)
        {
            const auto &ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_NOTE)
                continue;
            const bool mapped = std::any_of(info->dlpi_phdr, info->dlpi_phdr + info->dlpi_phnum,
                [&](const auto &load)
                {
                    return load.p_type == PT_LOAD && (load.p_flags & PF_R) &&
                           ph.p_vaddr >= load.p_vaddr && ph.p_filesz <= load.p_filesz &&
                           ph.p_vaddr - load.p_vaddr <= load.p_filesz - ph.p_filesz;
                });
            if (!mapped)
                continue;
            const auto *note = reinterpret_cast<const uint8_t *>(info->dlpi_addr + ph.p_vaddr);
            size_t remaining = ph.p_filesz;
            while (remaining >= sizeof(ElfW(Nhdr)))
            {
                ElfW(Nhdr) header{};
                std::memcpy(&header, note, sizeof(header));
                note += sizeof(header);
                remaining -= sizeof(header);
                const size_t nameLength = (static_cast<size_t>(header.n_namesz) + 3) & ~size_t{3};
                const size_t descLength = (static_cast<size_t>(header.n_descsz) + 3) & ~size_t{3};
                if (nameLength > remaining || descLength > remaining - nameLength)
                    break;
                if (header.n_type == NT_GNU_BUILD_ID && header.n_namesz == 4 &&
                    std::memcmp(note, "GNU", 4) == 0 && header.n_descsz >= ps2state::android::MinBuildIdBytes &&
                    header.n_descsz <= ps2state::android::MaxBuildIdBytes)
                {
                    const auto *build = note + nameLength;
                    identity.assign(build, build + header.n_descsz);
                    return 1;
                }
                note += nameLength + descLength;
                remaining -= nameLength + descLength;
            }
        }
        return 1;
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
        PadInput sampled = input;
        if (mapPressPending)
            sampled.buttons |= kStart;
        mapPressPending = false;
        return sampled;
    }

    void initializeOptions()
    {
        std::lock_guard lock(optionsMutex);
        applyOptions(pendingOptions);
        optionsReady = true;
    }

    void setPresentationOptions(bool fxaa, bool frameGeneration, bool fsr)
    {
#ifdef PS2X_DESKTOP_VIDEO
        presentationOptions.store((fxaa ? kFxaaSupported : 0) |
                                  (frameGeneration ? kFrameGenerationSupported : 0) |
                                  (fsr ? kUpscaleFsrSupported : 0));
#endif
    }

    void setMaxAnisotropy(float amount)
    {
        int filter = 2; // Trilinear
        for (float level = 2; level <= amount && filter < 6; level *= 2)
            ++filter;
        maxTextureFilter.store(filter);
    }

    float displayRefreshRate()
    {
        return refreshRate.load(std::memory_order_relaxed);
    }

    const std::vector<uint8_t> &runnerBuildId()
    {
        static const auto identity = []
        {
            std::vector<uint8_t> result;
            dl_iterate_phdr(findRunnerBuildId, &result);
            return result;
        }();
        return identity;
    }

    float requestedWidescreenAspect()
    {
        return requestedAspect.load(std::memory_order_relaxed);
    }

    void setActiveWidescreenAspect(float aspect)
    {
        activeAspect.store(aspect, std::memory_order_relaxed);
    }

    float activeWidescreenAspect()
    {
        return activeAspect.load(std::memory_order_relaxed);
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
        const EGLContext presentationContext = eglGetCurrentContext();
        bool sharedPresentation = false;
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
            {
                const char *sharing = std::getenv("SOTC_SHARED_PRESENT");
                if (presentationContext != EGL_NO_CONTEXT && (!sharing || sharing[0] != '0'))
                {
                    workerContext = eglCreateContext(workerDisplay, config, presentationContext, contextAttributes);
                    sharedPresentation = workerContext != EGL_NO_CONTEXT;
                }
                if (!sharedPresentation)
                    workerContext = eglCreateContext(workerDisplay, config, EGL_NO_CONTEXT, contextAttributes);
            }
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
        }, sharedPresentation);
        __android_log_print(ANDROID_LOG_INFO, "ps2x", "Native GLES presentation: %s",
                            sharedPresentation ? "shared textures" : "CPU fallback");
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
    mapPressPending = false;
    isPaused.store(false, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_genisson_sotc_NativeGameActivity_nativeSetInput(JNIEnv *, jclass, jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry)
{
    std::lock_guard lock(stateMutex);
    if (!isPaused.load(std::memory_order_relaxed))
    {
        // Keep a quick map tap until the guest reads the next pad packet.
        mapPressPending |= (buttons & kStart) != 0 && (input.buttons & kStart) == 0;
        input = {static_cast<uint16_t>(buttons), stickAxis(lx), stickAxis(ly), stickAxis(rx), stickAxis(ry)};
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_genisson_sotc_NativeGameActivity_nativeSetWidescreen(JNIEnv *, jclass, jboolean enabled, jfloat aspect)
{
    requestedAspect.store(enabled != JNI_FALSE && std::isfinite(aspect) && aspect > ps2x::android::kOriginalAspect
                              ? aspect : 0.0f,
                          std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_genisson_sotc_NativeGameActivity_nativeSetRefreshRate(JNIEnv *, jclass, jfloat hz)
{
    refreshRate.store(std::isfinite(hz) ? std::clamp(hz, 30.0f, 240.0f) : 60.0f, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_genisson_sotc_NativeGameActivity_nativeReleaseInput(JNIEnv *, jclass)
{
    std::lock_guard lock(stateMutex);
    input = {};
    mapPressPending = false;
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
        {
            input = {};
            mapPressPending = false;
        }
    }
    pauseChanged.notify_all();
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_genisson_sotc_NativeGameActivity_nativeOptionValues(JNIEnv *env, jclass)
{
    std::lock_guard lock(optionsMutex);
    if (!optionsReady)
        return nullptr;
    const Options values = readOptions();
    jfloatArray result = env->NewFloatArray(OptionCount);
    if (result)
        env->SetFloatArrayRegion(result, 0, OptionCount, values.data());
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_genisson_sotc_NativeGameActivity_nativeApplyOptions(JNIEnv *env, jclass, jfloatArray options)
{
    if (!options || env->GetArrayLength(options) != OptionCount)
        return;
    Options values;
    env->GetFloatArrayRegion(options, 0, OptionCount, values.data());
    if (env->ExceptionCheck())
        return;
    std::lock_guard lock(optionsMutex);
    if (optionsReady)
        applyOptions(values);
    else
        pendingOptions = values;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_genisson_sotc_NativeGameActivity_nativeOptionCapabilities(JNIEnv *, jclass)
{
    const bool saves = !ps2x::android::runnerBuildId().empty();
    return presentationOptions.load() | (saves ? kSaveStatesSupported : 0) | (maxTextureFilter.load() << 8);
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_genisson_sotc_NativeGameActivity_nativeStateSlots(JNIEnv *env, jclass)
{
    if (!savestates::ready() || savestates::busy())
        return nullptr;
    jclass stringClass = env->FindClass("java/lang/String");
    if (!stringClass)
        return nullptr;
    jobjectArray result = env->NewObjectArray(savestates::SlotCount, stringClass, nullptr);
    if (!result)
        return nullptr;
    const auto slots = savestates::slots();
    for (int i = 0; i < savestates::SlotCount; ++i)
    {
        jstring description = env->NewStringUTF(slots[i].modified.c_str());
        if (!description)
            return nullptr;
        env->SetObjectArrayElement(result, i, description);
        env->DeleteLocalRef(description);
    }
    return result;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_genisson_sotc_NativeGameActivity_nativeRequestState(JNIEnv *, jclass, jint action, jint slot)
{
    if (action < 0 || action > 2 || ps2x::android::runnerBuildId().empty())
        return JNI_FALSE;
    constexpr savestates::Action actions[] = {savestates::Action::Save, savestates::Action::Load, savestates::Action::Delete};
    return savestates::tryRequest(actions[action], slot) ? JNI_TRUE : JNI_FALSE;
}
