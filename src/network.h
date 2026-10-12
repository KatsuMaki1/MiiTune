#ifndef MIITUNE_NETWORK_H
#define MIITUNE_NETWORK_H

#include "models.h"
#include <string>
#include <vector>

class NetworkManager {
public:
    static bool Initialize();
    static void Cleanup();

    // Direct YouTube Music API calls (InnerTube)
    static std::vector<Song> GetTrending(const std::string& region = "FR");
    static std::vector<Song> SearchSongs(const std::string& query);
    static std::vector<Song> SearchSongsPage(const std::string& query, const std::string& mode,
                                             const std::string& token, std::string* nextToken);
    static std::vector<Song> GetRadio(const std::string& videoId);
    static void SetRegion(const std::string& region);
    static std::string GetAudioStreamUrl(const std::string& videoId);
    static LyricsData GetLyrics(const std::string& artist, const std::string& title, int durationSeconds = 0);

    // Artist profile & Last.fm
    static ArtistProfile GetArtistDetails(const std::string& artistName, const std::string& channelId = "");
    static bool ScrobbleLastFm(const std::string& artist, const std::string& track);
    static std::string EscapeJsonString(const std::string& input);

    // HTTP requests
    static std::string HttpGet(const std::string& url, long timeoutSeconds = 8);
    static std::vector<uint8_t> HttpGetBytes(const std::string& url, long timeoutSeconds = 6);
    static std::string HttpPost(const std::string& url, const std::string& jsonBody,
                                const std::string& origin = "", long timeoutSeconds = 8,
                                const std::vector<std::string>& extraHeaders = {});
    static std::string UrlEncode(const std::string& value);

private:
    static LyricsData ParseLrc(const std::string& lrcContent);
};

#endif // MIITUNE_NETWORK_H
