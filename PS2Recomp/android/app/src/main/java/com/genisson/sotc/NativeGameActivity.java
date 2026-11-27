package com.genisson.sotc;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.PixelFormat;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.view.Gravity;
import android.view.Display;
import android.view.View;
import android.view.WindowManager;
import java.io.File;
import java.io.IOException;

public final class NativeGameActivity extends NativeActivity {
    static final String ADAPTIVE_WIDESCREEN = "adaptive_widescreen";
    static { System.loadLibrary(BuildConfig.PORTABLE ? "SotcBootstrap" : "ps2EntryRunner"); }
    private static native String nativeAttachRunner(String path);
    public static native boolean nativeHasGame();
    public static native void nativeConfigure(String disc, String bootElf, String saves, String log, int resolutionPercent);
    public static native void nativeSetInput(int buttons, float moveX, float moveY, float cameraX, float cameraY);
    public static native void nativeReleaseInput();
    public static native void nativePause(boolean paused);
    private static native void nativeSetWidescreen(boolean enabled, float aspect);
    private static native void nativeSetRefreshRate(float hz);
    static native float[] nativeOptionValues();
    static native void nativeApplyOptions(float[] options);
    static native int nativeOptionCapabilities();
    static native String[] nativeStateSlots();
    static native boolean nativeRequestState(int action, int slot);
    private TouchControlsView controls;
    private boolean editing;
    private boolean gameAttached;
    private GameMenu menu;
    private boolean overlayAttached;
    private boolean resumed;
    private SharedPreferences displayPreferences;
    private DisplayManager displays;
    private boolean refreshRateSupported;
    private final DisplayManager.DisplayListener refreshListener = new DisplayManager.DisplayListener() {
        @Override public void onDisplayAdded(int id) {}
        @Override public void onDisplayRemoved(int id) {}
        @Override public void onDisplayChanged(int id) { updateRefreshRate(); }
    };
    private android.window.OnBackInvokedCallback backCallback;

    @Override protected void onCreate(Bundle state) {
        String launchLanguage = getIntent().getStringExtra(LauncherLanguage.EXTRA_LANGUAGE);
        Context labels = LauncherLanguage.localized(this, launchLanguage);
        if (BuildConfig.PORTABLE && !attachRunner()) {
            // NativeActivity still needs its lifecycle. The bootstrap has no
            // callbacks to forward when a portable runner failed to attach.
            super.onCreate(state);
            new AlertDialog.Builder(this).setTitle(labels.getString(R.string.game_start_failed))
                    .setMessage(labels.getString(OnDevicePreparation.isAvailable(this)
                            ? R.string.game_runner_incompatible : R.string.game_player_incompatible))
                    .setPositiveButton(labels.getString(R.string.game_close), (dialog, which) -> finish())
                    .setOnCancelListener(dialog -> finish()).show();
            return;
        }
        displayPreferences = getSharedPreferences("sotc-display", MODE_PRIVATE);
        android.util.DisplayMetrics metrics = getResources().getDisplayMetrics();
        updateDisplayMode(metrics.widthPixels, metrics.heightPixels);
        String disc = getIntent().getStringExtra("disc");
        if (disc != null) {
            File saves = new File(getFilesDir(), "memcards");
            saves.mkdirs();
            nativeConfigure(disc, new File(disc, DiscImageImporter.BOOT_FILE).getAbsolutePath(),
                    saves.getAbsolutePath(), new File(getFilesDir(), "last-game.log").getAbsolutePath(), 100);
        }
        GameOptions options = new GameOptions(getSharedPreferences("sotc-game-options", MODE_PRIVATE));
        super.onCreate(state);
        gameAttached = true;
        refreshRateSupported = options.supported();
        updateRefreshRate();
        if (refreshRateSupported) {
            displays = getSystemService(DisplayManager.class);
            if (displays != null) {
                displays.registerDisplayListener(refreshListener, new Handler(getMainLooper()));
            }
        }
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            backCallback = this::menuAction;
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback);
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        immersive();
        View gameWindow = getWindow().getDecorView();
        gameWindow.addOnLayoutChangeListener((view, left, top, right, bottom,
                oldLeft, oldTop, oldRight, oldBottom) -> {
            int width = right - left;
            int height = bottom - top;
            if (width != oldRight - oldLeft || height != oldBottom - oldTop) {
                updateDisplayMode(width, height);
            }
        });
        controls = new TouchControlsView(labels);
        menu = new GameMenu(this, controls, options, displayPreferences, launchLanguage,
                this::refreshDisplayMode, this::editControls, () -> {
                    nativeReleaseInput();
                    updatePauseState();
                    immersive();
                });
        controls.setListener(new TouchControlsView.Listener() {
            @Override public void onInput(int buttons, float mx, float my, float cx, float cy) {
                nativeSetInput(buttons, mx, my, cx, cy);
            }
            @Override public void onMenu() {
                menuAction();
            }
        });
        // NativeActivity owns its surface and input queue. A separate application
        // panel gives the HUD its own drawable surface and Java touch dispatch.
        getWindow().getDecorView().post(() -> {
            if (isFinishing() || isDestroyed()) return;
            WindowManager.LayoutParams params = new WindowManager.LayoutParams(-1, -1,
                    WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                    PixelFormat.TRANSLUCENT);
            params.token = getWindow().getDecorView().getWindowToken();
            params.gravity = Gravity.TOP | Gravity.START;
            params.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            if (android.os.Build.VERSION.SDK_INT >= 30) params.setFitInsetsTypes(0);
            getWindowManager().addView(controls, params);
            overlayAttached = true;
        });
    }

    private boolean attachRunner() {
        try {
            File runner = OnDevicePreparation.installedRunner(this);
            if (runner == null) runner = new PlayerPackage(getFilesDir()).installedRunner();
            if (runner != null && getIntent().getStringExtra("disc") != null) {
                String error = nativeAttachRunner(runner.getCanonicalPath());
                if (error == null) return true;
                Log.e("SOTC", "Could not attach the imported runner: " + error);
            }
        } catch (IOException | LinkageError | RuntimeException error) {
            Log.e("SOTC", "Could not attach the imported runner", error);
        }
        return false;
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (menu != null) menu.setLaunchLanguage(intent.getStringExtra(LauncherLanguage.EXTRA_LANGUAGE));
    }

    private void immersive() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    private void updateDisplayMode(int width, int height) {
        if (width > 0 && height > 0) {
            nativeSetWidescreen(displayPreferences.getBoolean(ADAPTIVE_WIDESCREEN, true),
                    (float) width / height);
        }
    }

    private void refreshDisplayMode() {
        View gameWindow = getWindow().getDecorView();
        updateDisplayMode(gameWindow.getWidth(), gameWindow.getHeight());
    }

    private void updateRefreshRate() {
        if (!refreshRateSupported) return;
        Display display = Build.VERSION.SDK_INT >= 30 ? getDisplay() : getWindowManager().getDefaultDisplay();
        if (display == null) return;
        try {
            nativeSetRefreshRate(display.getRefreshRate());
        } catch (UnsatisfiedLinkError legacyRunner) {
            refreshRateSupported = false;
        }
    }

    private void menuAction() {
        if (editing) {
            editing = false;
            controls.setEditing(false);
            updatePauseState();
        } else {
            showMenu();
        }
    }

    private void editControls() {
        editing = true;
        controls.setEditing(true);
        updatePauseState();
    }

    private void showMenu() {
        if (controls == null || isFinishing() || isDestroyed() || (menu != null && menu.isShowing())) return;
        controls.releaseAll();
        nativePause(true);
        menu.show();
    }

    private void updatePauseState() {
        nativePause(!resumed || editing || (menu != null && menu.isShowing()));
    }
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (!gameAttached) return;
        if (!focused) { if (controls != null) controls.releaseAll(); nativeReleaseInput(); }
        else immersive();
    }
    @Override protected void onPause() {
        resumed = false;
        if (controls != null) controls.releaseAll();
        if (gameAttached) {
            nativeReleaseInput();
            nativePause(true);
        }
        super.onPause();
    }
    @Override protected void onResume() {
        super.onResume();
        resumed = true;
        if (gameAttached) updatePauseState();
    }
    @Override public void onBackPressed() {
        if (gameAttached) menuAction();
        else super.onBackPressed();
    }
    @Override protected void onDestroy() {
        resumed = false;
        if (displays != null) displays.unregisterDisplayListener(refreshListener);
        if (menu != null) menu.dispose();
        if (controls != null) controls.releaseAll();
        if (gameAttached) nativeReleaseInput();
        if (overlayAttached) {
            getWindowManager().removeViewImmediate(controls);
            overlayAttached = false;
        }
        if (android.os.Build.VERSION.SDK_INT >= 33 && backCallback != null) {
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backCallback);
        }
        super.onDestroy();
        // Each game session has its own process. NativeActivity has already joined
        // the game thread here; release its global caches before the next session.
        // Android may destroy this Activity without finish(). A recreated session
        // must also get fresh native globals and hook tables in a new process.
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
