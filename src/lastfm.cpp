#include "lastfm.h"
#include "i18n.h"
#include "network.h"
#include "storage.h"
#include <jansson.h>
#include <mbedtls/md.h>
#include <map>
#include <mutex>
#include <ctime>
#include <coreinit/debug.h>

namespace {
std::mutex g_mutex;
std::string g_key, g_secret, g_session, g_token, g_status;

std::string Md5Hex(const std::string& in) {
    unsigned char out[16];
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    if (!info || mbedtls_md(info, (const unsigned char*)in.data(), in.size(), out) != 0) return "";
    static const char* H = "0123456789abcdef";
    std::string hex;
    for (int i = 0; i < 16; i++) { hex += H[out[i] >> 4]; hex += H[out[i] & 15]; }
    return hex;
}

// Signed call (api_sig = md5 of the sorted "namevalue" pairs + secret). post=true for writes.
std::string Call(std::map<std::string, std::string> p, bool post, bool signedCall) {
    std::string key, secret;
    { std::lock_guard<std::mutex> l(g_mutex); key = g_key; secret = g_secret; }
    p["api_key"] = key;
    if (signedCall) {
        std::string raw;
        for (auto& kv : p) raw += kv.first + kv.second;   // std::map = alphabetical order
        p["api_sig"] = Md5Hex(raw + secret);
    }
    p["format"] = "json";
    std::string body;
    for (auto& kv : p) {
        if (!body.empty()) body += "&";
        body += kv.first + "=" + NetworkManager::UrlEncode(kv.second);
    }
    if (post)
        return NetworkManager::HttpPost("https://ws.audioscrobbler.com/2.0/", body, "", 10,
                                        { "Content-Type: application/x-www-form-urlencoded" });
    return NetworkManager::HttpGet("https://ws.audioscrobbler.com/2.0/?" + body, 10);
}

void Save() {
    json_t* o = json_object();
    json_object_set_new(o, "api_key", json_string(g_key.c_str()));
    json_object_set_new(o, "api_secret", json_string(g_secret.c_str()));
    json_object_set_new(o, "session_key", json_string(g_session.c_str()));
    char* d = json_dumps(o, JSON_INDENT(2));
    if (d) { StorageWrite("lastfm.json", d); free(d); }
    json_decref(o);
}
}

namespace LastFm {

void Init() {
    std::lock_guard<std::mutex> l(g_mutex);
    std::string txt = StorageRead("lastfm.json");
    if (txt.empty()) { Save(); g_status = T("Not configured"); return; }
    json_error_t e;
    json_t* r = json_loads(txt.c_str(), 0, &e);
    if (!r) return;
    auto get = [&](const char* k) { json_t* v = json_object_get(r, k); return (v && json_string_value(v)) ? std::string(json_string_value(v)) : std::string(); };
    g_key = get("api_key"); g_secret = get("api_secret"); g_session = get("session_key");
    json_decref(r);
}

std::string ApiKey() { std::lock_guard<std::mutex> l(g_mutex); return g_key; }
bool HasKeys()    { std::lock_guard<std::mutex> l(g_mutex); return !g_key.empty() && !g_secret.empty(); }
bool HasSession() { std::lock_guard<std::mutex> l(g_mutex); return !g_session.empty(); }

std::string Status() {
    std::lock_guard<std::mutex> l(g_mutex);
    if (g_key.empty() || g_secret.empty()) return T("Add api_key and api_secret to lastfm.json (SD)");
    if (g_session.empty()) return g_token.empty() ? T("Key OK - touch to connect") : T("Allow in your browser, then confirm");
    return g_status.empty() ? T("Connected") : g_status;
}

bool BeginAuth() {
    if (!HasKeys()) return false;
    std::string resp = Call({ {"method", "auth.getToken"} }, false, true);
    json_error_t e;
    json_t* r = json_loads(resp.c_str(), 0, &e);
    if (!r) return false;
    json_t* t = json_object_get(r, "token");
    bool ok = t && json_string_value(t);
    if (ok) { std::lock_guard<std::mutex> l(g_mutex); g_token = json_string_value(t); }
    json_decref(r);
    return ok;
}

std::string AuthUrl() {
    std::lock_guard<std::mutex> l(g_mutex);
    if (g_token.empty()) return "";
    return "https://www.last.fm/api/auth/?api_key=" + g_key + "&token=" + g_token;
}

bool FinishAuth() {
    std::string token;
    { std::lock_guard<std::mutex> l(g_mutex); token = g_token; }
    if (token.empty()) return false;
    std::string resp = Call({ {"method", "auth.getSession"}, {"token", token} }, false, true);
    json_error_t e;
    json_t* r = json_loads(resp.c_str(), 0, &e);
    if (!r) return false;
    json_t* s = json_object_get(json_object_get(r, "session"), "key");
    bool ok = s && json_string_value(s);
    if (ok) {
        std::lock_guard<std::mutex> l(g_mutex);
        g_session = json_string_value(s);
        g_token.clear();
        g_status = T("Connected");
        Save();
    }
    json_decref(r);
    return ok;
}

bool Scrobble(const std::string& artist, const std::string& track) {
    if (artist.empty() || track.empty() || !HasKeys() || !HasSession()) return false;
    std::string sk;
    { std::lock_guard<std::mutex> l(g_mutex); sk = g_session; }
    std::string resp = Call({ {"method", "track.scrobble"}, {"artist", artist}, {"track", track},
                              {"timestamp", std::to_string((long long)time(nullptr))}, {"sk", sk} }, true, true);
    bool ok = resp.find("\"accepted\"") != std::string::npos;
    std::lock_guard<std::mutex> l(g_mutex);
    g_status = ok ? T("Last scrobble: ") + track : T("Scrobble failed");
    OSReport("[Last.fm] scrobble %s - %s: %s\n", artist.c_str(), track.c_str(), ok ? "ok" : "echec");
    return ok;
}

void NowPlaying(const std::string& artist, const std::string& track) {
    if (artist.empty() || track.empty() || !HasKeys() || !HasSession()) return;
    std::string sk;
    { std::lock_guard<std::mutex> l(g_mutex); sk = g_session; }
    Call({ {"method", "track.updateNowPlaying"}, {"artist", artist}, {"track", track}, {"sk", sk} }, true, true);
}

}
