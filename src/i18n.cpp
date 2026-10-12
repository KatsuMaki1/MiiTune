#include "i18n.h"
#include "i18n_table.hpp"
#include <mutex>
#include <unordered_map>

int g_lang = LANG_EN;

const char* T(const char* en) {
    if (!en || g_lang == LANG_EN) return en;
    static std::mutex mtx;
    static std::unordered_map<std::string, const I18nEntry*> index;
    static std::unordered_map<const char*, const char*> cache[LANG_COUNT];   // keyed by literal address: no allocation
    std::lock_guard<std::mutex> lock(mtx);
    auto& c = cache[g_lang];
    auto hit = c.find(en);
    if (hit != c.end()) return hit->second;
    if (index.empty()) for (const auto& e : I18N_TABLE) index[e.en] = &e;
    auto it = index.find(en);
    const char* r = en;
    if (it != index.end()) r = (g_lang == LANG_FR) ? it->second->fr : it->second->es;
    c[en] = r;
    return r;
}

const char* LangName(int lang) {
    return lang == LANG_EN ? "English" : lang == LANG_ES ? "Español" : "Français";
}

const char* LangCode(int lang) {
    return lang == LANG_EN ? "en" : lang == LANG_ES ? "es" : "fr";
}

int LangFromCode(const std::string& code) {
    return code == "fr" ? LANG_FR : code == "es" ? LANG_ES : LANG_EN;
}
