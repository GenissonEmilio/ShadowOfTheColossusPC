# Adjust raylib 5.5's Android lifecycle, window size and audio shutdown.
function(replace_android_source old new)
    string(FIND "${source}" "${old}" old_pos)
    if(old_pos EQUAL -1)
        string(FIND "${source}" "${new}" new_pos)
        if(new_pos EQUAL -1)
            message(FATAL_ERROR "Unexpected raylib source in ${path}: ${old}")
        endif()
    else()
        string(REPLACE "${old}" "${new}" source "${source}")
        set(source "${source}" PARENT_SCOPE)
    endif()
endfunction()

set(path "${RAYLIB_SOURCE_DIR}/src/platforms/rcore_android.c")
file(READ "${path}" original)
string(REPLACE "\r\n" "\n" source "${original}")

replace_android_source([=[    while (!app->destroyRequested)
    {
        // Poll all events until we reach return value TIMEOUT, meaning no events left to process
        while ((pollResult = ALooper_pollOnce(0, NULL, &pollEvents, (void **)&platform.source)) > ALOOPER_POLL_TIMEOUT)
        {
            if (platform.source != NULL) platform.source->process(app, platform.source);
        }
    }]=] [=[    while (!app->destroyRequested)
    {
        pollResult = ALooper_pollOnce(-1, NULL, &pollEvents, (void **)&platform.source);
        if (pollResult >= 0 && platform.source != NULL) platform.source->process(app, platform.source);
    }]=])

replace_android_source([=[    // NOTE: Activity is paused if not enabled (platform.appEnabled)
    while ((pollResult = ALooper_pollOnce(platform.appEnabled? 0 : -1, NULL, &pollEvents, (void**)&platform.source)) > ALOOPER_POLL_TIMEOUT)]=] [=[    // The application may pause its guest while continuing to dispatch window events.
    while ((pollResult = ALooper_pollOnce(0, NULL, &pollEvents, (void**)&platform.source)) >= 0)]=])

replace_android_source([=[        // NOTE: Allow closing the window in case a configuration change happened.
        // The android_main function should be allowed to return to its caller in order for the
        // Android OS to relaunch the activity.
        if (platform.app->destroyRequested != 0)
        {
            CORE.Window.shouldClose = true;
        }]=] [=[        if (platform.app->destroyRequested != 0)
        {
            CORE.Window.shouldClose = true;
            return;
        }]=])

replace_android_source([=[    while (!CORE.Window.ready)
    {
        // Process events until we reach TIMEOUT, which indicates no more events queued.
        while ((pollResult = ALooper_pollOnce(0, NULL, &pollEvents, (void**)&platform.source)) > ALOOPER_POLL_TIMEOUT)
        {
            // Process this event
            if (platform.source != NULL) platform.source->process(platform.app, platform.source);

            // NOTE: It's highly likely destroyRequested will never be non-zero at the start of the activity lifecycle.
            //if (platform.app->destroyRequested != 0) CORE.Window.shouldClose = true;
        }
    }

    return 0;]=] [=[    while (!CORE.Window.ready && !CORE.Window.shouldClose && !platform.app->destroyRequested)
    {
        pollResult = ALooper_pollOnce(-1, NULL, &pollEvents, (void **)&platform.source);
        if (pollResult >= 0 && platform.source != NULL) platform.source->process(platform.app, platform.source);
    }
    if (!CORE.Window.ready)
    {
        ClosePlatform();
        return -1;
    }

    return 0;]=])

replace_android_source([=[                    InitGraphicsDevice();]=] [=[                    if (InitGraphicsDevice() != 0)
                    {
                        CORE.Window.shouldClose = true;
                        return;
                    }]=])

replace_android_source([=[                    eglMakeCurrent(platform.device, platform.surface, platform.surface, platform.context);

                    platform.contextRebindRequired = false;]=] [=[                    if (platform.surface == EGL_NO_SURFACE ||
                        !eglMakeCurrent(platform.device, platform.surface, platform.surface, platform.context))
                    {
                        TRACELOG(LOG_ERROR, "DISPLAY: Failed to rebind the Android surface");
                        CORE.Window.shouldClose = true;
                        return;
                    }
                    platform.contextRebindRequired = false;]=])

replace_android_source([=[    // Display data
    EGLDisplay device;]=] [=[    bool nativeWindowSize;               // Let Android size the window's buffers

    EGLDisplay device;]=])

replace_android_source([=[void PollInputEvents(void)
{
#if defined(SUPPORT_GESTURES_SYSTEM)]=] [=[void PollInputEvents(void)
{
    CORE.Window.resizedLastFrame = false;
#if defined(SUPPORT_GESTURES_SYSTEM)]=])

replace_android_source([=[    // Initialize display basic configuration
    //----------------------------------------------------------------------------]=] [=[    platform.nativeWindowSize = (CORE.Window.screen.width == 0 && CORE.Window.screen.height == 0);

    // Initialize display configuration
    //----------------------------------------------------------------------------]=])

replace_android_source([=[    ANativeWindow_setBuffersGeometry(platform.app->window, CORE.Window.render.width, CORE.Window.render.height, displayFormat);
    //ANativeWindow_setBuffersGeometry(platform.app->window, 0, 0, displayFormat);       // Force use of native display size]=] [=[    ANativeWindow_setBuffersGeometry(platform.app->window,
        platform.nativeWindowSize ? 0 : CORE.Window.render.width,
        platform.nativeWindowSize ? 0 : CORE.Window.render.height, displayFormat);]=])

replace_android_source([=[// ANDROID: Process activity lifecycle commands
static void AndroidCommandCallback(struct android_app *app, int32_t cmd)]=] [=[// Refresh native-size raylib windows on the thread owning the EGL context.
static void AndroidUpdateNativeViewport(void)
{
    if (!platform.nativeWindowSize || platform.app->window == NULL ||
        platform.surface == EGL_NO_SURFACE || eglGetCurrentSurface(EGL_DRAW) != platform.surface)
        return;

    const int width = ANativeWindow_getWidth(platform.app->window);
    const int height = ANativeWindow_getHeight(platform.app->window);
    if (width <= 0 || height <= 0 || (width == CORE.Window.screen.width && height == CORE.Window.screen.height))
        return;

    CORE.Window.display = CORE.Window.screen = (Size){ width, height };
    CORE.Window.currentFbo = CORE.Window.screen;
    CORE.Window.renderOffset = (Point){ 0, 0 };
    CORE.Window.screenScale = MatrixIdentity();
    SetupViewport(width, height);
    rlSetFramebufferWidth(width);
    rlSetFramebufferHeight(height);
    CORE.Window.resizedLastFrame = true;
}

// ANDROID: Process lifecycle and window-size commands
static void AndroidCommandCallback(struct android_app *app, int32_t cmd)]=])

replace_android_source([=[                        CORE.Window.render.width + CORE.Window.renderOffset.x,
                        CORE.Window.render.height + CORE.Window.renderOffset.y,
                        displayFormat);]=] [=[                        platform.nativeWindowSize ? 0 : CORE.Window.render.width + CORE.Window.renderOffset.x,
                        platform.nativeWindowSize ? 0 : CORE.Window.render.height + CORE.Window.renderOffset.y,
                        displayFormat);]=])

replace_android_source([=[                    platform.contextRebindRequired = false;
                }
                else]=] [=[                    platform.contextRebindRequired = false;
                    AndroidUpdateNativeViewport();
                }
                else]=])

replace_android_source([=[        } break;
        case APP_CMD_GAINED_FOCUS:
        {
            platform.appEnabled = true;]=] [=[        } break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONTENT_RECT_CHANGED:
            AndroidUpdateNativeViewport();
            break;
        case APP_CMD_GAINED_FOCUS:
        {
            platform.appEnabled = true;]=])

if(NOT source STREQUAL original)
    file(WRITE "${path}" "${source}")
endif()

set(path "${RAYLIB_SOURCE_DIR}/src/rcore.c")
file(READ "${path}" original)
string(REPLACE "\r\n" "\n" source "${original}")

replace_android_source([=[    InitPlatform();
    //--------------------------------------------------------------]=] [=[    if (InitPlatform() != 0) return;
    //--------------------------------------------------------------]=])

if(NOT source STREQUAL original)
    file(WRITE "${path}" "${source}")
endif()

set(path "${RAYLIB_SOURCE_DIR}/src/raudio.c")
file(READ "${path}" original)
string(REPLACE "\r\n" "\n" source "${original}")

# Backport raylib #6039. Audio callbacks can lock the mixer until the device stops.
replace_android_source([=[        ma_mutex_uninit(&AUDIO.System.lock);
        ma_device_uninit(&AUDIO.System.device);
        ma_context_uninit(&AUDIO.System.context);]=] [=[        ma_device_uninit(&AUDIO.System.device);
        ma_mutex_uninit(&AUDIO.System.lock);
        ma_context_uninit(&AUDIO.System.context);]=])

if(NOT source STREQUAL original)
    file(WRITE "${path}" "${source}")
endif()
