#ifndef MIITUNE_MODELS_H
#define MIITUNE_MODELS_H

#include <string>
#include <vector>

enum class PlaybackState {
    STOPPED,
    BUFFERING,
    PLAYING,
    PAUSED
};

enum class RepeatMode {
    OFF,
    ALL,
    ONE
};

enum AppState {
    STATE_CATALOG,
    STATE_SEARCH,
    STATE_PLAYER,
    STATE_ACCOUNT,
    STATE_ARTIST,
    STATE_QUEUE,
    STATE_SETTINGS,
    STATE_PLAYLISTS,
    STATE_PLAYLIST_VIEW,
    STATE_LANGUAGE        // language selection (first launch)
};

enum ScreenFormat {
    FORMAT_TV = 0,       // 1280x720 (Sharp widescreen for TV)
    FORMAT_GAMEPAD = 1   // 854x480 (Native 1:1 pixel mapping for Wii U GamePad)
};

enum class AppView {
    TRENDING,
    SEARCH,
    NOW_PLAYING,
    QUEUE,
    SETTINGS
};

struct Song {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    int durationSeconds = 0;
    std::string durationFormatted;
    std::string thumbnailUrl;
    std::string streamUrl;
    std::string channelId;
    std::string channelTitle;
    bool isLive = false;
    bool isFavorite = false;
};

struct LyricsLine {
    int timeMs = 0;
    std::string text;
};

struct LyricsData {
    bool hasLyrics = false;
    bool isSynced = false;
    std::vector<LyricsLine> lines;
    int currentLineIndex = -1;
};

struct VisualizerBar {
    float height = 0.0f;
    float targetHeight = 0.0f;
    float peak = 0.0f;
};

struct Playlist {
    std::string id;
    std::string title;
    int itemCount = 0;
    std::string thumbnailUrl;
};

struct UserAccount {
    bool is_logged_in = false;
    std::string user_name;
    std::string user_email;
    std::string avatar_url;
    std::string access_token;
    std::string refresh_token;
    uint64_t token_expires_at = 0;
    std::vector<Playlist> playlists;
    bool is_loading_playlists = false;
};

struct DeviceCodeInfo {
    bool active = false;
    std::string device_code;
    std::string user_code;
    std::string verification_url = "https://www.google.com/device";
    int interval = 5;
    int expires_in = 1800;
    uint64_t expires_at = 0;
    std::string status_msg;
    bool is_polling = false;
    bool is_requesting = false;
};

struct ArtistProfile {
    std::string name;
    std::string channelId;
    std::string subscribers;
    std::string bio;
    std::vector<Song> topSongs;
    int selected_idx = 0;
    int scroll_x = 0;
    bool isLoading = false;
};

struct LastFmInfo {
    bool enabled = false;
    std::string username;
    std::string sessionKey;
    std::string status_msg;
    int scrobbleCount = 0;
};

#endif // MIITUNE_MODELS_H
