#include "favorites.h"
#include <fstream>
#include <iostream>
#include <jansson.h>
#include <coreinit/debug.h>

static const char* FAVORITES_PATHS[] = {
    "fs:/vol/external01/wiiu/apps/MiiTune/favorites.json",
    "favorites.json",
    nullptr
};

FavoritesManager& FavoritesManager::Instance() {
    static FavoritesManager instance;
    return instance;
}

void FavoritesManager::Initialize() {
    LoadFromFile();
}

bool FavoritesManager::IsFavorite(const std::string& songId) {
    if (songId.empty()) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& s : m_favorites) {
        if (s.id == songId) return true;
    }
    return false;
}

void FavoritesManager::ToggleFavorite(const Song& song) {
    if (song.id.empty()) return;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_favorites.begin(); it != m_favorites.end(); ++it) {
            if (it->id == song.id) {
                m_favorites.erase(it);
                found = true;
                OSReport("[MiiTune] Track removed from favorites: %s\n", song.title.c_str());
                break;
            }
        }
        if (!found) {
            Song fav = song;
            fav.isFavorite = true;
            m_favorites.insert(m_favorites.begin(), fav);
            OSReport("[MiiTune] Track added to favorites: %s\n", song.title.c_str());
        }
    }
    SaveToFile();
}

std::vector<Song> FavoritesManager::GetFavorites() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_favorites;
}

void FavoritesManager::LoadFromFile() {
    std::string content;
    for (int i = 0; FAVORITES_PATHS[i] != nullptr; ++i) {
        std::ifstream file(FAVORITES_PATHS[i]);
        if (file.is_open()) {
            content.assign((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
            file.close();
            if (!content.empty()) break;
        }
    }

    if (content.empty()) return;

    json_error_t err;
    json_t* root = json_loads(content.c_str(), 0, &err);
    if (!root || !json_is_array(root)) {
        if (root) json_decref(root);
        return;
    }

    std::vector<Song> list;
    size_t count = json_array_size(root);
    for (size_t i = 0; i < count; i++) {
        json_t* item = json_array_get(root, i);
        if (!json_is_object(item)) continue;

        Song s;
        json_t* jId = json_object_get(item, "id");
        json_t* jTitle = json_object_get(item, "title");
        json_t* jArtist = json_object_get(item, "artist");
        json_t* jThumb = json_object_get(item, "thumbnailUrl");
        json_t* jDur = json_object_get(item, "durationSeconds");
        json_t* jDurFmt = json_object_get(item, "durationFormatted");
        json_t* jChan = json_object_get(item, "channelTitle");

        if (jId && json_string_value(jId)) s.id = json_string_value(jId);
        if (jTitle && json_string_value(jTitle)) s.title = json_string_value(jTitle);
        if (jArtist && json_string_value(jArtist)) s.artist = json_string_value(jArtist);
        if (jThumb && json_string_value(jThumb)) s.thumbnailUrl = json_string_value(jThumb);
        if (jDur && json_is_integer(jDur)) s.durationSeconds = (int)json_integer_value(jDur);
        if (jDurFmt && json_string_value(jDurFmt)) s.durationFormatted = json_string_value(jDurFmt);
        if (jChan && json_string_value(jChan)) s.channelTitle = json_string_value(jChan);
        s.isFavorite = true;

        if (!s.id.empty()) {
            list.push_back(s);
        }
    }

    json_decref(root);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_favorites = list;
    OSReport("[MiiTune] %u favorites loaded from storage.\n", (unsigned int)m_favorites.size());
}

void FavoritesManager::SaveToFile() {
    json_t* root = json_array();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& s : m_favorites) {
            json_t* obj = json_object();
            json_object_set_new(obj, "id", json_string(s.id.c_str()));
            json_object_set_new(obj, "title", json_string(s.title.c_str()));
            json_object_set_new(obj, "artist", json_string(s.artist.c_str()));
            json_object_set_new(obj, "thumbnailUrl", json_string(s.thumbnailUrl.c_str()));
            json_object_set_new(obj, "durationSeconds", json_integer(s.durationSeconds));
            json_object_set_new(obj, "durationFormatted", json_string(s.durationFormatted.c_str()));
            json_object_set_new(obj, "channelTitle", json_string(s.channelTitle.c_str()));
            json_array_append_new(root, obj);
        }
    }

    char* dumped = json_dumps(root, JSON_INDENT(2));
    if (dumped) {
        for (int i = 0; FAVORITES_PATHS[i] != nullptr; ++i) {
            std::ofstream file(FAVORITES_PATHS[i]);
            if (file.is_open()) {
                file << dumped;
                file.close();
                break;
            }
        }
        free(dumped);
    }
    json_decref(root);
}
