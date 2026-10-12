#ifndef MIITUNE_LIBRARY_H
#define MIITUNE_LIBRARY_H

#include "models.h"
#include <mutex>
#include <string>
#include <vector>

struct Settings {
    std::string region = "FR";     // trending region
    bool autoplay = true;          // automatic radio when the queue ends
    std::string search_mode = "songs"; // songs | videos | all
    std::string language;          // "fr" | "en" | "es"; empty = not chosen yet (first launch)
    int sleep_min = 0;             // sleep timer (0 = off), not persisted
};

// Local playlist (stored in playlists.json)
struct LocalPlaylist {
    std::string name;
    std::vector<Song> songs;
};

// Listening history, recent searches and settings (JSON files in wiiu/apps/MiiTune/)
class Library {
public:
    static Library& Instance();
    void Init();

    std::vector<Song> History();
    void AddHistory(const Song& s);
    void ClearHistory();

    std::vector<std::string> Searches();
    void AddSearch(const std::string& q);
    void ClearSearches();

    std::vector<LocalPlaylist> Playlists();
    int CreatePlaylist(const std::string& name);        // returns the index
    void DeletePlaylist(int i);
    bool AddToPlaylist(int i, const Song& s);           // false if already in the playlist
    void RemoveFromPlaylist(int i, int songIdx);

    Settings GetSettings();
    void SetSettings(const Settings& s);

private:
    void SaveHistory();
    void SaveSearches();
    void SaveSettings();
    void SavePlaylists();

    std::mutex m_mutex;
    std::vector<Song> m_history;
    std::vector<std::string> m_searches;
    std::vector<LocalPlaylist> m_playlists;
    Settings m_settings;
};

#endif
