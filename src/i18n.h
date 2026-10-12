#ifndef MIITUNE_I18N_H
#define MIITUNE_I18N_H

#include <string>

// UI languages. Source strings in the code are English: T("English text") returns the translation for the
// current language (key = the English text, table in i18n_table.hpp). Text without an entry is returned as is.
// Thread-safe.
enum Lang { LANG_FR = 0, LANG_EN = 1, LANG_ES = 2, LANG_COUNT = 3 };

extern int g_lang;
const char* T(const char* en);
const char* LangName(int lang);                   // "Français", "English", "Español" (each in its own language)
const char* LangCode(int lang);                   // "fr", "en", "es"
int LangFromCode(const std::string& code);        // unknown -> LANG_EN

#endif
