package com.genisson.sotc;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.SharedPreferences;
import android.graphics.PixelFormat;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import java.io.File;

public final class NativeGameActivity extends NativeActivity {
    private static final String ADAPTIVE_WIDESCREEN = "adaptive_widescreen";
    static { System.loadLibrary("ps2EntryRunner"); }
    public static native boolean nativeHasGame();
    public static native void nativeConfigure(String disc, String bootElf, String saves, String log, int resolutionPercent);
    public static native void nativeSetInput(int buttons, float moveX, float moveY, float cameraX, float cameraY);
    public static native void nativeReleaseInput();
    public static native void nativePause(boolean paused);
    private static native void nativeSetWidescreen(boolean enabled, float aspect);
    private TouchControlsView controls;
    private boolean editing;
    private AlertDialog menu;
    private boolean overlayAttached;
    private boolean resumed;
    private SharedPreferences displayPreferences;
    private android.window.OnBackInvokedCallback backCallback;

    @Override protected void onCreate(Bundle state) {
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
        super.onCreate(state);
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            backCallback = this::showMenu;
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
        controls = new TouchControlsView(this);
        controls.setListener(new TouchControlsView.Listener() {
            @Override public void onInput(int buttons, float mx, float my, float cx, float cy) {
                nativeSetInput(buttons, mx, my, cx, cy);
            }
            @Override public void onMenu() {
                if (editing) {
                    editing = false;
                    controls.setEditing(false);
                    updatePauseState();
                } else showMenu();
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

    private void showMenu() {
        if (controls == null || isFinishing() || isDestroyed() || (menu != null && menu.isShowing())) return;
        controls.releaseAll();
        nativePause(true);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        int padding = (int) (20 * getResources().getDisplayMetrics().density);
        content.setPadding(padding, padding, padding, padding);
        CheckBox widescreen = new CheckBox(this);
        widescreen.setText(R.string.game_adaptive_widescreen);
        widescreen.setChecked(displayPreferences.getBoolean(ADAPTIVE_WIDESCREEN, true));
        content.addView(widescreen);
        widescreen.setOnCheckedChangeListener((button, value) -> {
            displayPreferences.edit().putBoolean(ADAPTIVE_WIDESCREEN, value).apply();
            View gameWindow = getWindow().getDecorView();
            updateDisplayMode(gameWindow.getWidth(), gameWindow.getHeight());
        });
        slider(content, "Tamanho dos controles", controls.getControlScale(), .7f, 1.3f, controls::setControlScale);
        slider(content, "Opacidade", controls.getOpacity(), .2f, 1f, controls::setOpacity);
        slider(content, "Sensibilidade da câmera", controls.getCameraSensitivity(), .35f, 2.5f, controls::setCameraSensitivity);
        CheckBox x = new CheckBox(this); x.setText("Inverter câmera horizontal");
        CheckBox y = new CheckBox(this); y.setText("Inverter câmera vertical");
        x.setChecked(controls.isCameraXInverted()); y.setChecked(controls.isCameraYInverted());
        content.addView(x); content.addView(y);
        x.setOnCheckedChangeListener((button, value) -> controls.setInvertCamera(value, y.isChecked()));
        y.setOnCheckedChangeListener((button, value) -> controls.setInvertCamera(x.isChecked(), value));
        android.widget.Button reset = new android.widget.Button(this);
        reset.setText("RESTAURAR CONTROLES");
        reset.setOnClickListener(view -> {
            controls.resetSettings();
            menu.dismiss();
        });
        content.addView(reset);
        android.widget.ScrollView scrolling = new android.widget.ScrollView(this);
        scrolling.addView(content);
        menu = new AlertDialog.Builder(this).setTitle("Seus controles").setView(scrolling)
                .setPositiveButton("VOLTAR", (dialog, which) -> { })
                .setNeutralButton("MOVER BOTÕES", (dialog, which) -> {
                    editing = true; controls.setEditing(true);
                })
                .setNegativeButton("SAIR DO JOGO", (dialog, which) -> finish()).create();
        menu.setOnDismissListener(dialog -> {
            nativeReleaseInput();
            updatePauseState();
            immersive();
        });
        menu.show();
    }

    private interface FloatSetting { void set(float value); }
    private void updatePauseState() {
        nativePause(!resumed || editing || (menu != null && menu.isShowing()));
    }
    private void slider(LinearLayout content, String name, float value, float min, float max, FloatSetting setter) {
        TextView label = new TextView(this); label.setText(name); content.addView(label);
        SeekBar slider = new SeekBar(this); slider.setMax(100);
        slider.setProgress(Math.round((value - min) * 100 / (max - min)));
        content.addView(slider);
        slider.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
                if (fromUser) setter.set(min + (max - min) * progress / 100);
            }
            public void onStartTrackingTouch(SeekBar bar) { }
            public void onStopTrackingTouch(SeekBar bar) { }
        });
    }
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (!focused) { if (controls != null) controls.releaseAll(); nativeReleaseInput(); }
        else immersive();
    }
    @Override protected void onPause() {
        resumed = false;
        if (controls != null) controls.releaseAll();
        nativeReleaseInput(); nativePause(true);
        super.onPause();
    }
    @Override protected void onResume() {
        super.onResume();
        resumed = true;
        updatePauseState();
    }
    @Override public void onBackPressed() { showMenu(); }
    @Override protected void onDestroy() {
        resumed = false;
        if (menu != null) {
            menu.setOnDismissListener(null);
            menu.dismiss();
        }
        if (controls != null) controls.releaseAll();
        nativeReleaseInput();
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
