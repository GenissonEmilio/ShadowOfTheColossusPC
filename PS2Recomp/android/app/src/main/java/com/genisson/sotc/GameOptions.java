package com.genisson.sotc;

import android.content.SharedPreferences;
import java.util.Arrays;

final class GameOptions {
    static final int FXAA = 1;
    static final int FRAME_GENERATION = 1 << 1;
    static final int FSR = 1 << 2;
    static final int SAVE_STATES = 1 << 3;
    static final int TEXTURE_LIMIT_SHIFT = 8;

    // Native index order lives in ps2_android_runtime.cpp. Preference keys
    // are the names instead of the indices so settings survive additions.
    enum Option {
        PRESET, RENDER_SCALE, ANTI_ALIASING, TEXTURE_FILTER, MODEL_DETAIL, EFFECT_DETAIL,
        FRAME_GENERATION, CAP_30, SHOW_FPS, VOLUME, SCREEN_MODE, VSYNC, UPSCALE_FSR,
        BLOOM, BLOOM_STRENGTH, BLOOM_ROUNDS, BLOOM_SPREAD, VERTEX_FOG, FOG_AMOUNT, DEPTH_FOG,
        GLOW, DEPTH_OF_FIELD, SHADOWS, PARTICLES, SCREEN_BLUR, FEEDBACK, REFLECTIONS, REFRACTION
    }

    private static final Option[] OPTIONS = Option.values();
    private static final Option[] PRESET_OPTIONS = {
        Option.RENDER_SCALE, Option.ANTI_ALIASING, Option.TEXTURE_FILTER,
        Option.MODEL_DETAIL, Option.EFFECT_DETAIL
    };

    private final SharedPreferences preferences;
    private boolean supported;

    GameOptions(SharedPreferences preferences) {
        this.preferences = preferences;
        float[] values = emptyOptions();
        for (Option option : OPTIONS) {
            values[option.ordinal()] = preferences.getFloat(option.name(), Float.NaN);
        }
        try {
            NativeGameActivity.nativeApplyOptions(values);
            supported = true;
        } catch (UnsatisfiedLinkError legacyRunner) {
            supported = false;
        }
    }

    boolean supported() {
        return supported;
    }

    float[] values() {
        if (!supported) return null;
        float[] values = NativeGameActivity.nativeOptionValues();
        return values != null && values.length == OPTIONS.length ? values : null;
    }

    int capabilities() {
        return supported ? NativeGameActivity.nativeOptionCapabilities() : 0;
    }

    void set(Option option, float value) {
        if (!supported) return;
        float[] changed = emptyOptions();
        changed[option.ordinal()] = value;
        NativeGameActivity.nativeApplyOptions(changed);
        SharedPreferences.Editor edit = preferences.edit().putFloat(option.name(), value);
        if (option == Option.PRESET) {
            for (Option override : PRESET_OPTIONS) {
                edit.remove(override.name());
            }
        }
        edit.apply();
    }

    private static float[] emptyOptions() {
        float[] values = new float[OPTIONS.length];
        Arrays.fill(values, Float.NaN);
        return values;
    }
}
