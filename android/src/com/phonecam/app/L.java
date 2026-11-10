package com.phonecam.app;

import java.util.HashMap;
import java.util.Locale;

/** UI language: follows the phone (Russian / Ukrainian / English for everything else). */
final class L {
    private static HashMap<String, String[]> table;
    private static int lang = -1;  // 0 ru, 1 en, 2 uk

    static int lang() {
        if (lang < 0) {
            String l = Locale.getDefault().getLanguage();
            lang = ("ru".equals(l) || "be".equals(l) || "kk".equals(l)) ? 0 : "uk".equals(l) ? 2 : 1;
        }
        return lang;
    }

    /** Text in the phone's language (source strings are Russian). */
    static synchronized String t(String ru) {
        int l = lang();
        if (l == 0 || ru == null) return ru;
        if (table == null) table = Strings.load();
        String[] tr = table.get(ru);
        return tr == null ? ru : tr[l == 1 ? 0 : 1];
    }
}
