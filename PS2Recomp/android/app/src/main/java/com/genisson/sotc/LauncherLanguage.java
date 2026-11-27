package com.genisson.sotc;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import java.util.Locale;

final class LauncherLanguage {
    static final String EXTRA_LANGUAGE = "language";
    static final String[] CODES = { "pt", "en", "ja", "es" };
    static final String[] NAMES = { "Português", "English", "日本語", "Español" };

    private LauncherLanguage() {}

    static String current(Context context) {
        String stored = settings(context).getString(EXTRA_LANGUAGE, null);
        if (indexOf(stored) >= 0) return stored;
        Locale device = context.getResources().getConfiguration().getLocales().get(0);
        String deviceLanguage = device != null ? device.getLanguage() : null;
        return indexOf(deviceLanguage) >= 0 ? deviceLanguage : CODES[0];
    }

    static boolean set(Context context, String code) {
        return indexOf(code) >= 0 && settings(context).edit().putString(EXTRA_LANGUAGE, code).commit();
    }

    static Context localized(Context context) {
        return localized(context, current(context));
    }

    static Context localized(Context context, String code) {
        String language = indexOf(code) >= 0 ? code : current(context);
        Configuration override = new Configuration();
        override.setLocale(Locale.forLanguageTag(language));
        return context.createConfigurationContext(override);
    }

    static int indexOf(String code) {
        for (int i = 0; i < CODES.length; i++) {
            if (CODES[i].equals(code)) return i;
        }
        return -1;
    }

    private static SharedPreferences settings(Context context) {
        return context.getSharedPreferences("launcher_settings", Context.MODE_PRIVATE);
    }
}
