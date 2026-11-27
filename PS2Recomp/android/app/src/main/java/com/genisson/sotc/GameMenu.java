package com.genisson.sotc;

import static com.genisson.sotc.GameOptions.Option.*;

import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.window.OnBackInvokedDispatcher;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;
import java.util.Arrays;
import java.util.function.Consumer;
import java.util.function.IntConsumer;
import java.util.function.IntSupplier;

final class GameMenu {
    private static final String MENU_LANGUAGE = "menu_language";
    private static final int SAVE = 0;
    private static final int DELETE = 2;

    private enum Page {
        HOME(R.string.game_options), QUALITY(R.string.game_quality), EFFECTS(R.string.game_effects),
        AV(R.string.game_av), CONTROLS(R.string.game_controls), STATES(R.string.game_states);

        final int title;
        Page(int title) { this.title = title; }
    }

    private final NativeGameActivity activity;
    private final TouchControlsView controls;
    private final GameOptions options;
    private final SharedPreferences display;
    private String launchLanguage;
    private final Runnable refreshDisplay;
    private final Runnable editControls;
    private final Runnable onClosed;

    private Context labels;
    private AlertDialog dialog;
    private AlertDialog prompt;
    private ScrollView scroller;
    private LinearLayout content;
    private Button presetButton;
    private Page currentPage;

    GameMenu(NativeGameActivity activity, TouchControlsView controls, GameOptions options,
            SharedPreferences display, String launchLanguage, Runnable refreshDisplay,
            Runnable editControls, Runnable onClosed) {
        this.activity = activity;
        this.controls = controls;
        this.options = options;
        this.display = display;
        this.launchLanguage = launchLanguage;
        this.refreshDisplay = refreshDisplay;
        this.editControls = editControls;
        this.onClosed = onClosed;
        applyLocale();
    }

    boolean isShowing() {
        return dialog != null && dialog.isShowing();
    }

    void setLaunchLanguage(String language) {
        launchLanguage = language;
        applyLocale();
        if (isShowing()) showPage(currentPage);
    }

    void show() {
        content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        int pad = Math.round(20 * activity.getResources().getDisplayMetrics().density);
        content.setPadding(pad, pad / 2, pad, pad / 2);
        scroller = new ScrollView(activity);
        scroller.setFillViewport(true);
        scroller.addView(content);
        dialog = new AlertDialog.Builder(activity).setTitle(text(Page.HOME.title)).setView(scroller)
                .setPositiveButton(text(R.string.game_resume), null)
                .setNegativeButton(text(R.string.game_menu_back), null).create();
        dialog.setOnDismissListener(ignored -> {
            dialog = null;
            onClosed.run();
        });
        dialog.show();
        dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener(view -> showPage(Page.HOME));
        if (Build.VERSION.SDK_INT >= 33 && dialog.getOnBackInvokedDispatcher() != null) {
            dialog.getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT, this::back);
        } else {
            dialog.setOnKeyListener((ignored, key, event) -> {
                if (key != android.view.KeyEvent.KEYCODE_BACK) return false;
                if (event.getAction() == android.view.KeyEvent.ACTION_UP) back();
                return true;
            });
        }
        showPage(Page.HOME);
    }

    void dispose() {
        if (prompt != null) prompt.dismiss();
        if (dialog != null) {
            dialog.setOnDismissListener(null);
            dialog.dismiss();
            dialog = null;
        }
    }

    private void back() {
        if (dialog == null) return;
        if (currentPage == Page.HOME) dialog.dismiss();
        else showPage(Page.HOME);
    }

    private void applyLocale() {
        String language = display.getString(MENU_LANGUAGE, launchLanguage);
        labels = LauncherLanguage.localized(activity, language);
        controls.setLabels(labels);
    }

    private void showPage(Page page) {
        currentPage = page;
        presetButton = null;
        content.removeAllViews();
        scroller.scrollTo(0, 0);
        dialog.setTitle(text(page.title));
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setText(text(R.string.game_resume));
        Button back = dialog.getButton(AlertDialog.BUTTON_NEGATIVE);
        back.setText(text(R.string.game_menu_back));
        back.setVisibility(page == Page.HOME ? android.view.View.GONE : android.view.View.VISIBLE);
        switch (page) {
            case HOME:
                button(R.string.game_quality, () -> showPage(Page.QUALITY));
                button(R.string.game_effects, () -> showPage(Page.EFFECTS));
                button(R.string.game_av, () -> showPage(Page.AV));
                button(R.string.game_controls, () -> showPage(Page.CONTROLS));
                button(R.string.game_states, () -> showPage(Page.STATES));
                button(R.string.game_exit, this::confirmExit);
                break;
            case CONTROLS:
                buildControls();
                break;
            case QUALITY: {
                float[] values = rendererValues();
                if (values != null) buildQuality(values);
                break;
            }
            case EFFECTS: {
                float[] values = rendererValues();
                if (values != null) buildEffects(values);
                break;
            }
            case AV:
                buildAudioVideo();
                break;
            case STATES:
                buildStates();
                break;
        }
    }

    private void confirmExit() {
        showPrompt(new AlertDialog.Builder(activity).setMessage(text(R.string.game_exit_confirm))
                .setNegativeButton(text(android.R.string.cancel), null)
                .setPositiveButton(text(R.string.game_exit), (ignored, which) -> activity.finish()));
    }

    private float[] rendererValues() {
        float[] values = options.values();
        if (values == null) {
            note(options.supported() ? R.string.game_starting_options : R.string.game_legacy_options);
        }
        return values;
    }

    private void buildQuality(float[] values) {
        String[] profiles = strings(R.array.game_profiles);
        presetButton = choice(R.string.game_preset, profiles, () -> selectedPreset(profiles), profiles.length - 1, index -> {
            options.set(PRESET, index);
            showPage(Page.QUALITY);
        });
        optionSlider(R.string.game_scale, R.string.game_times, values, RENDER_SCALE, .5f, 4f, .5f);
        int capabilities = options.capabilities();
        optionalToggle(R.string.game_fxaa, values, ANTI_ALIASING, capabilities, GameOptions.FXAA);
        String[] filters = strings(R.array.game_filters);
        int count = Math.max(3, Math.min(filters.length, (capabilities >> GameOptions.TEXTURE_LIMIT_SHIFT) + 1));
        optionChoice(R.string.game_texture_filter, values, TEXTURE_FILTER, Arrays.copyOf(filters, count));
        if (count < filters.length) note(R.string.game_anisotropy_limit);
        optionChoice(R.string.game_model_detail, values, MODEL_DETAIL, strings(R.array.game_model_levels));
        optionChoice(R.string.game_effect_detail, values, EFFECT_DETAIL, strings(R.array.game_effect_levels));
    }

    private void buildEffects(float[] values) {
        optionToggle(R.string.game_bloom, values, BLOOM);
        optionSlider(R.string.game_bloom_strength, R.string.game_times, values, BLOOM_STRENGTH, 0, 2, .02f);
        optionSlider(R.string.game_bloom_rounds, 0, values, BLOOM_ROUNDS, 1, 6, 1);
        optionSlider(R.string.game_bloom_spread, R.string.game_times, values, BLOOM_SPREAD, 0, 4, .1f);
        optionToggle(R.string.game_vertex_fog, values, VERTEX_FOG);
        optionSlider(R.string.game_fog_amount, R.string.game_times, values, FOG_AMOUNT, 0, 2, .05f);
        optionToggle(R.string.game_depth_fog, values, DEPTH_FOG);
        optionToggle(R.string.game_glow, values, GLOW);
        optionToggle(R.string.game_dof, values, DEPTH_OF_FIELD);
        optionToggle(R.string.game_shadows, values, SHADOWS);
        optionToggle(R.string.game_particles, values, PARTICLES);
        optionToggle(R.string.game_screen_blur, values, SCREEN_BLUR);
        optionToggle(R.string.game_feedback, values, FEEDBACK);
        optionToggle(R.string.game_reflections, values, REFLECTIONS);
        optionToggle(R.string.game_refraction, values, REFRACTION);
    }

    private void buildAudioVideo() {
        float[] values = rendererValues();
        if (values == null) {
            toggle(R.string.game_adaptive_widescreen,
                    display.getBoolean(NativeGameActivity.ADAPTIVE_WIDESCREEN, true), true, this::setWide);
        } else {
            String[] screenModes = strings(R.array.game_screen_modes);
            int selected = display.getBoolean(NativeGameActivity.ADAPTIVE_WIDESCREEN, true)
                    ? 0 : index(values, SCREEN_MODE) + 1;
            choice(R.string.game_screen, screenModes, selected, screenModes.length, index -> {
                options.set(SCREEN_MODE, Math.max(0, index - 1));
                setWide(index == 0);
            });
            int capabilities = options.capabilities();
            optionToggle(R.string.game_vsync, values, VSYNC);
            optionalToggle(R.string.game_fsr, values, UPSCALE_FSR, capabilities, GameOptions.FSR);
            optionalToggle(R.string.game_frame_generation, values, FRAME_GENERATION, capabilities,
                    GameOptions.FRAME_GENERATION);
            optionToggle(R.string.game_cap30, values, CAP_30);
            optionToggle(R.string.game_show_fps, values, SHOW_FPS);
            slider(R.string.game_volume, R.string.game_percent, values[VOLUME.ordinal()] / 10,
                    0f, 1f, .1f, value -> options.set(VOLUME, Math.round(value * 10)));
        }

        String[] languages = new String[LauncherLanguage.NAMES.length + 1];
        languages[0] = text(R.string.game_follow_launcher);
        System.arraycopy(LauncherLanguage.NAMES, 0, languages, 1, LauncherLanguage.NAMES.length);
        int language = LauncherLanguage.indexOf(display.getString(MENU_LANGUAGE, null)) + 1;
        choice(R.string.game_menu_language, languages, language, languages.length, index -> {
            SharedPreferences.Editor editor = display.edit();
            if (index == 0) editor.remove(MENU_LANGUAGE);
            else editor.putString(MENU_LANGUAGE, LauncherLanguage.CODES[index - 1]);
            editor.apply();
            applyLocale();
            showPage(Page.AV);
        });
    }

    private void setWide(boolean enabled) {
        display.edit().putBoolean(NativeGameActivity.ADAPTIVE_WIDESCREEN, enabled).apply();
        refreshDisplay.run();
    }

    private void buildControls() {
        button(R.string.game_move_buttons, () -> {
            editControls.run();
            dialog.dismiss();
        });
        slider(R.string.game_control_scale, R.string.game_percent, controls.getControlScale(),
                .7f, 1.3f, .01f, controls::setControlScale);
        slider(R.string.game_control_opacity, R.string.game_percent, controls.getOpacity(),
                .2f, 1f, .01f, controls::setOpacity);
        slider(R.string.game_camera_sensitivity, R.string.game_times, controls.getCameraSensitivity(),
                .35f, 2.5f, .05f, controls::setCameraSensitivity);
        toggle(R.string.game_invert_horizontal, controls.isCameraXInverted(), true,
                value -> controls.setInvertCamera(value, controls.isCameraYInverted()));
        toggle(R.string.game_invert_vertical, controls.isCameraYInverted(), true,
                value -> controls.setInvertCamera(controls.isCameraXInverted(), value));
        button(R.string.game_reset_controls, () -> {
            controls.resetSettings();
            showPage(Page.CONTROLS);
        });
    }

    private void buildStates() {
        if (!options.supported() || (options.capabilities() & GameOptions.SAVE_STATES) == 0) {
            note(R.string.game_states_unavailable);
            return;
        }
        String[] slots = NativeGameActivity.nativeStateSlots();
        if (slots == null) {
            note(R.string.game_states_busy);
            return;
        }
        for (int i = 0; i < slots.length; i++) {
            int slot = i;
            String title = slot == 0 ? text(R.string.game_state_quick) : text(R.string.game_state_slot, slot);
            boolean exists = !slots[slot].isEmpty();
            String detail = exists ? slots[slot] : text(R.string.game_state_empty);
            button(text(R.string.game_option_value, title, detail), () -> selectState(slot, title, exists));
        }
    }

    private void selectState(int slot, String title, boolean exists) {
        String[] actions = exists ? strings(R.array.game_state_actions)
                : new String[]{ text(R.string.game_state_save) };
        showPrompt(new AlertDialog.Builder(activity).setTitle(title)
                .setItems(actions, (ignored, action) -> {
                    if (exists) {
                        showPrompt(new AlertDialog.Builder(activity)
                                .setMessage(text(R.string.game_state_confirm, actions[action], title))
                                .setNegativeButton(text(android.R.string.cancel), null)
                                .setPositiveButton(actions[action], (confirm, which) -> runState(action, slot)));
                    } else {
                        runState(SAVE, slot);
                    }
                }));
    }

    private void runState(int action, int slot) {
        if (action < SAVE || action > DELETE || !NativeGameActivity.nativeRequestState(action, slot)) {
            Toast.makeText(activity, text(R.string.game_states_busy), Toast.LENGTH_SHORT).show();
            return;
        }
        dialog.dismiss(); // The state manager runs the queued request on the guest thread.
    }

    private void optionSlider(int name, int unit, float[] values, GameOptions.Option option,
            float min, float max, float step) {
        slider(name, unit, values[option.ordinal()], min, max, step, value -> setOption(option, value));
    }

    private void optionChoice(int name, float[] values, GameOptions.Option option, String[] choices) {
        choice(name, choices, Math.min(index(values, option), choices.length - 1), choices.length,
                value -> setOption(option, value));
    }

    private void optionToggle(int name, float[] values, GameOptions.Option option) {
        toggle(name, values[option.ordinal()] != 0, true, value -> options.set(option, value ? 1 : 0));
    }

    private void optionalToggle(int name, float[] values, GameOptions.Option option, int capabilities, int capability) {
        boolean available = (capabilities & capability) != 0;
        toggle(name, available && values[option.ordinal()] != 0, available,
                value -> setOption(option, value ? 1 : 0));
        if (!available) note(R.string.game_graphics_unavailable);
    }

    private static int index(float[] values, GameOptions.Option option) {
        return Math.round(values[option.ordinal()]);
    }

    private void setOption(GameOptions.Option option, float value) {
        options.set(option, value);
        if (presetButton != null) {
            String[] profiles = strings(R.array.game_profiles);
            presetButton.setText(text(R.string.game_option_value, text(R.string.game_preset),
                    profiles[selectedPreset(profiles)]));
        }
    }

    private int selectedPreset(String[] profiles) {
        float[] values = options.values();
        return values == null ? profiles.length - 1
                : Math.max(0, Math.min(index(values, PRESET), profiles.length - 1));
    }

    private void choice(int name, String[] choices, int selected, int selectable, IntConsumer change) {
        int[] selectedValue = {selected};
        choice(name, choices, () -> selectedValue[0], selectable, index -> {
            selectedValue[0] = index;
            change.accept(index);
        });
    }

    private Button choice(int name, String[] choices, IntSupplier selected, int selectable, IntConsumer change) {
        int initial = Math.max(0, Math.min(selected.getAsInt(), choices.length - 1));
        Button entry = button(text(R.string.game_option_value, text(name), choices[initial]), null);
        entry.setOnClickListener(view -> {
            int index = selected.getAsInt();
            AlertDialog.Builder builder = new AlertDialog.Builder(activity).setTitle(text(name))
                    .setSingleChoiceItems(Arrays.copyOf(choices, selectable),
                            index < selectable ? index : -1, (choice, value) -> {
                                choice.dismiss();
                                change.accept(value);
                                entry.setText(text(R.string.game_option_value, text(name), choices[value]));
                            }).setNegativeButton(text(android.R.string.cancel), null);
            showPrompt(builder);
        });
        return entry;
    }

    private void toggle(int name, boolean checked, boolean enabled, Consumer<Boolean> change) {
        CheckBox check = new CheckBox(activity);
        check.setText(text(name));
        check.setChecked(checked);
        check.setEnabled(enabled);
        check.setOnCheckedChangeListener((view, value) -> change.accept(value));
        content.addView(check);
    }

    private void slider(int name, int unit, float value, float min, float max, float step,
            Consumer<Float> change) {
        TextView label = new TextView(activity);
        SeekBar seek = new SeekBar(activity);
        seek.setContentDescription(text(name));
        seek.setMax(Math.round((max - min) / step));
        seek.setProgress(Math.round((value - min) / step));
        label.setText(sliderText(name, unit, value));
        content.addView(label);
        content.addView(seek);
        seek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
                if (fromUser) {
                    float next = min + step * progress;
                    label.setText(sliderText(name, unit, next));
                    change.accept(next);
                }
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
    }

    private String sliderText(int name, int unit, float value) {
        String amount;
        if (unit == R.string.game_percent) {
            amount = text(unit, Math.round(value * 100));
        } else if (unit == R.string.game_times) {
            amount = text(unit, value);
        } else {
            amount = Integer.toString(Math.round(value));
        }
        return text(R.string.game_option_value, text(name), amount);
    }

    private Button button(int title, Runnable click) { return button(text(title), click); }

    private Button button(String title, Runnable click) {
        Button button = new Button(activity);
        button.setText(title);
        button.setAllCaps(false);
        if (click != null) button.setOnClickListener(view -> click.run());
        content.addView(button);
        return button;
    }

    private void note(int message) {
        TextView text = new TextView(activity);
        text.setText(text(message));
        content.addView(text);
    }

    private void showPrompt(AlertDialog.Builder builder) {
        AlertDialog next = builder.create();
        prompt = next;
        next.setOnDismissListener(ignored -> {
            if (prompt == next) prompt = null;
        });
        next.show();
    }

    private String[] strings(int id) { return labels.getResources().getStringArray(id); }
    private String text(int id, Object... arguments) { return labels.getString(id, arguments); }
}
