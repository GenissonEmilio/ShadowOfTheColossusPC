package com.genisson.sotc;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.SharedPreferences;
import android.view.View;

final class LauncherLanguageInstrumentation {
    static void verify(Context context) {
        SharedPreferences settings = context.getSharedPreferences(
                "sotc-launcher-language-instrumentation", Context.MODE_PRIVATE);
        try {
            check(settings.edit().clear().commit(), "could not initialize language test preferences");
            Context test = isolated(context, settings);
            LauncherScreen launcher = new LauncherScreen(context);
            launcher.playButton.setEnabled(false);
            launcher.progress.setVisibility(View.VISIBLE);

            String[] playLabels = { "Jogar", "Play", "プレイ", "Jugar" };
            String[] languageLabels = { "Idioma", "Language", "言語", "Idioma" };
            for (int i = 0; i < LauncherLanguage.CODES.length; i++) {
                String code = LauncherLanguage.CODES[i];
                check(LauncherLanguage.set(test, code), "could not store language " + code);
                Context next = isolated(context, settings);
                check(LauncherLanguage.current(next).equals(code), "language was not retained in preferences");
                Context translated = LauncherLanguage.localized(next);
                check(translated.getString(R.string.launcher_play).equals(playLabels[i]),
                        "missing play label in " + code);
                check(translated.getString(R.string.launcher_prepare_percent, 42).contains("42%"),
                        "preparation progress could not be formatted in " + code);
                launcher.applyLanguage(translated, true);
                check(launcher.languageButton.getText().toString().equals(languageLabels[i]),
                        "language label did not update in place");
                check(!launcher.playButton.isEnabled() && launcher.progress.getVisibility() == View.VISIBLE,
                        "language change reset the launcher operation");
            }
            check(!LauncherLanguage.set(test, "fr") && LauncherLanguage.current(test).equals("es"),
                    "unsupported language replaced the selected language");
        } finally {
            settings.edit().clear().commit();
        }
    }

    private static Context isolated(Context context, SharedPreferences settings) {
        return new ContextWrapper(context) {
            @Override public SharedPreferences getSharedPreferences(String name, int mode) {
                return settings;
            }
        };
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
