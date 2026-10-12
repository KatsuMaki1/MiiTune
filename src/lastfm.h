#ifndef MIITUNE_LASTFM_H
#define MIITUNE_LASTFM_H

#include <string>

// Real Last.fm scrobbling. Needs your own API key (https://www.last.fm/api/account/create) in
// wiiu/apps/MiiTune/lastfm.json: {"api_key":"...","api_secret":"...","session_key":""}
// Without session_key: BeginAuth() -> the user opens AuthUrl() (QR) -> FinishAuth().
namespace LastFm {
    void Init();                      // loads the config (creates an empty template if missing)
    bool HasKeys();
    std::string ApiKey();             // "" if not configured
    bool HasSession();
    std::string Status();             // text for the Settings screen
    bool BeginAuth();                 // requests a token
    std::string AuthUrl();            // "" until BeginAuth has succeeded
    bool FinishAuth();                // exchanges the token for a session and saves it
    bool Scrobble(const std::string& artist, const std::string& track);
    void NowPlaying(const std::string& artist, const std::string& track);
}

#endif
