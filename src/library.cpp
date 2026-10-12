#include "library.h"
#include "storage.h"
#include <jansson.h>
#include <algorithm>

static json_t* SongToJson(const Song& s) {
    json_t* o = json_object();
    json_object_set_new(o, "id", json_string(s.id.c_str()));
    json_object_set_new(o, "title", json_string(s.title.c_str()));
    json_object_set_new(o, "artist", json_string(s.artist.c_str()));
    json_object_set_new(o, "channelId", json_string(s.channelId.c_str()));
    json_object_set_new(o, "channelTitle", json_string(s.channelTitle.c_str()));
    json_object_set_new(o, "durationSeconds", json_integer(s.durationSeconds));
    json_object_set_new(o, "durationFormatted", json_string(s.durationFormatted.c_str()));
    json_object_set_new(o, "thumbnailUrl", json_string(s.thumbnailUrl.c_str()));
    return o;
}

static std::string Str(json_t* o, const char* k);
static Song SongFromJson(json_t* it) {
    Song s;
    s.id = Str(it, "id"); s.title = Str(it, "title"); s.artist = Str(it, "artist");
    s.channelId = Str(it, "channelId"); s.channelTitle = Str(it, "channelTitle");
    s.durationFormatted = Str(it, "durationFormatted"); s.thumbnailUrl = Str(it, "thumbnailUrl");
    json_t* d = json_object_get(it, "durationSeconds");
    if (d && json_is_integer(d)) s.durationSeconds = (int)json_integer_value(d);
    return s;
}

static std::string Str(json_t* o, const char* k) {
    json_t* v = json_object_get(o, k);
    return (v && json_string_value(v)) ? json_string_value(v) : "";
}

static void Dump(const char* file, json_t* root) {
    char* d = json_dumps(root, JSON_INDENT(1));
    if (d) { StorageWrite(file, d); free(d); }
    json_decref(root);
}

Library& Library::Instance() {
    static Library l;
    return l;
}

void Library::Init() {
    std::lock_guard<std::mutex> lock(m_mutex);
    json_error_t e;

    std::string h = StorageRead("history.json");
    if (json_t* r = h.empty() ? nullptr : json_loads(h.c_str(), 0, &e)) {
        size_t i; json_t* it;
        json_array_foreach(r, i, it) {
            Song s;
            s.id = Str(it, "id"); s.title = Str(it, "title"); s.artist = Str(it, "artist");
            s.channelId = Str(it, "channelId"); s.channelTitle = Str(it, "channelTitle");
            s.durationFormatted = Str(it, "durationFormatted"); s.thumbnailUrl = Str(it, "thumbnailUrl");
            json_t* d = json_object_get(it, "durationSeconds");
            if (d && json_is_integer(d)) s.durationSeconds = (int)json_integer_value(d);
            if (!s.id.empty()) m_history.push_back(s);
        }
        json_decref(r);
    }

    std::string pl = StorageRead("playlists.json");
    if (json_t* r = pl.empty() ? nullptr : json_loads(pl.c_str(), 0, &e)) {
        size_t i; json_t* it;
        json_array_foreach(r, i, it) {
            LocalPlaylist p;
            p.name = Str(it, "name");
            json_t* songs = json_object_get(it, "songs");
            size_t k; json_t* sj;
            if (songs) json_array_foreach(songs, k, sj) {
                Song s = SongFromJson(sj);
                if (!s.id.empty()) p.songs.push_back(s);
            }
            if (!p.name.empty()) m_playlists.push_back(p);
        }
        json_decref(r);
    }

    std::string q = StorageRead("searches.json");
    if (json_t* r = q.empty() ? nullptr : json_loads(q.c_str(), 0, &e)) {
        size_t i; json_t* it;
        json_array_foreach(r, i, it) if (json_string_value(it)) m_searches.push_back(json_string_value(it));
        json_decref(r);
    }

    std::string st = StorageRead("settings.json");
    if (json_t* r = st.empty() ? nullptr : json_loads(st.c_str(), 0, &e)) {
        std::string reg = Str(r, "region"), mode = Str(r, "search_mode");
        if (!reg.empty()) m_settings.region = reg;
        std::string lang = Str(r, "language");
        if (lang == "fr" || lang == "en" || lang == "es") m_settings.language = lang;
        if (mode == "songs" || mode == "videos" || mode == "all") m_settings.search_mode = mode;
        json_t* ap = json_object_get(r, "autoplay");
        if (ap) m_settings.autoplay = json_is_true(ap);
        json_decref(r);
    }
}

std::vector<Song> Library::History() { std::lock_guard<std::mutex> l(m_mutex); return m_history; }

void Library::AddHistory(const Song& s) {
    if (s.id.empty()) return;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_history.erase(std::remove_if(m_history.begin(), m_history.end(), [&](const Song& x) { return x.id == s.id; }), m_history.end());
        m_history.insert(m_history.begin(), s);
        if (m_history.size() > 30) m_history.resize(30);
    }
    SaveHistory();
}

void Library::ClearHistory() {
    { std::lock_guard<std::mutex> l(m_mutex); m_history.clear(); }
    SaveHistory();
}

std::vector<std::string> Library::Searches() { std::lock_guard<std::mutex> l(m_mutex); return m_searches; }

void Library::AddSearch(const std::string& q) {
    if (q.empty()) return;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_searches.erase(std::remove(m_searches.begin(), m_searches.end(), q), m_searches.end());
        m_searches.insert(m_searches.begin(), q);
        if (m_searches.size() > 12) m_searches.resize(12);
    }
    SaveSearches();
}

void Library::ClearSearches() {
    { std::lock_guard<std::mutex> l(m_mutex); m_searches.clear(); }
    SaveSearches();
}

std::vector<LocalPlaylist> Library::Playlists() { std::lock_guard<std::mutex> l(m_mutex); return m_playlists; }

int Library::CreatePlaylist(const std::string& name) {
    int idx;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        LocalPlaylist p;
        p.name = name.substr(0, 40);
        m_playlists.push_back(p);
        idx = (int)m_playlists.size() - 1;
    }
    SavePlaylists();
    return idx;
}

void Library::DeletePlaylist(int i) {
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (i < 0 || i >= (int)m_playlists.size()) return;
        m_playlists.erase(m_playlists.begin() + i);
    }
    SavePlaylists();
}

bool Library::AddToPlaylist(int i, const Song& s) {
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (i < 0 || i >= (int)m_playlists.size() || s.id.empty()) return false;
        for (auto& x : m_playlists[i].songs) if (x.id == s.id) return false;
        m_playlists[i].songs.push_back(s);
    }
    SavePlaylists();
    return true;
}

void Library::RemoveFromPlaylist(int i, int songIdx) {
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (i < 0 || i >= (int)m_playlists.size()) return;
        auto& v = m_playlists[i].songs;
        if (songIdx < 0 || songIdx >= (int)v.size()) return;
        v.erase(v.begin() + songIdx);
    }
    SavePlaylists();
}

void Library::SavePlaylists() {
    json_t* a = json_array();
    {
        std::lock_guard<std::mutex> l(m_mutex);
        for (auto& p : m_playlists) {
            json_t* o = json_object();
            json_object_set_new(o, "name", json_string(p.name.c_str()));
            json_t* songs = json_array();
            for (auto& s : p.songs) json_array_append_new(songs, SongToJson(s));
            json_object_set_new(o, "songs", songs);
            json_array_append_new(a, o);
        }
    }
    Dump("playlists.json", a);
}

Settings Library::GetSettings() { std::lock_guard<std::mutex> l(m_mutex); return m_settings; }

void Library::SetSettings(const Settings& s) {
    { std::lock_guard<std::mutex> l(m_mutex); m_settings = s; }
    SaveSettings();
}

void Library::SaveHistory() {
    json_t* a = json_array();
    { std::lock_guard<std::mutex> l(m_mutex); for (auto& s : m_history) json_array_append_new(a, SongToJson(s)); }
    Dump("history.json", a);
}

void Library::SaveSearches() {
    json_t* a = json_array();
    { std::lock_guard<std::mutex> l(m_mutex); for (auto& q : m_searches) json_array_append_new(a, json_string(q.c_str())); }
    Dump("searches.json", a);
}

void Library::SaveSettings() {
    json_t* o = json_object();
    { std::lock_guard<std::mutex> l(m_mutex);
      json_object_set_new(o, "region", json_string(m_settings.region.c_str()));
      json_object_set_new(o, "search_mode", json_string(m_settings.search_mode.c_str()));
      json_object_set_new(o, "language", json_string(m_settings.language.c_str()));
      json_object_set_new(o, "autoplay", json_boolean(m_settings.autoplay)); }
    Dump("settings.json", o);
}
