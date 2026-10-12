#include "network.h"
#include "i18n.h"
#include "lastfm.h"
#include <curl/curl.h>
#include <jansson.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <map>
#include <cstring>
#if defined(__WIIU__)
#include <coreinit/debug.h>
#else
#define OSReport printf
#endif

static size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

bool NetworkManager::Initialize() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return true;
}

void NetworkManager::Cleanup() {
    curl_global_cleanup();
}

std::string NetworkManager::UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : value) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << ((int)(unsigned char)c);
        }
    }

    return escaped.str();
}

std::string NetworkManager::HttpGet(const std::string& url, long timeoutSeconds) {
    CURL* curl = curl_easy_init();
    std::string responseBuffer;

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "[Network] GET failed: " << curl_easy_strerror(res) << " for URL: " << url << std::endl;
        }
        curl_easy_cleanup(curl);
    }

    return responseBuffer;
}

static size_t CurlWriteBytesCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* vec = (std::vector<uint8_t>*)userp;
    size_t total = size * nmemb;
    size_t cur = vec->size();
    vec->resize(cur + total);
    memcpy(vec->data() + cur, contents, total);
    return total;
}

std::vector<uint8_t> NetworkManager::HttpGetBytes(const std::string& url, long timeoutSeconds) {
    CURL* curl = curl_easy_init();
    std::vector<uint8_t> responseBuffer;

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteBytesCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }

    return responseBuffer;
}

std::string NetworkManager::HttpPost(const std::string& url, const std::string& jsonBody,
                                   const std::string& origin, long timeoutSeconds,
                                   const std::vector<std::string>& extraHeaders) {
    CURL* curl = curl_easy_init();
    std::string responseBuffer;

    if (curl) {
        struct curl_slist* headers = nullptr;
        bool hasCustomCt = false;
        bool hasCustomUa = false;
        for (const auto& h : extraHeaders) {
            if (h.rfind("User-Agent:", 0) == 0) hasCustomUa = true;
            if (h.rfind("Content-Type:", 0) == 0) hasCustomCt = true;
            headers = curl_slist_append(headers, h.c_str());
        }
        if (!hasCustomCt) {
            headers = curl_slist_append(headers, "Content-Type: application/json");
        }
        if (!hasCustomUa) {
            headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        }
        if (!origin.empty()) {
            std::string origHdr = "Origin: " + origin;
            headers = curl_slist_append(headers, origHdr.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "[Network] POST failed: " << curl_easy_strerror(res) << " for URL: " << url << std::endl;
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    return responseBuffer;
}

[[maybe_unused]] static std::string FormatSeconds(int totalSeconds) {
    if (totalSeconds < 0) totalSeconds = 0;
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, seconds);
    return std::string(buffer);
}

std::string NetworkManager::EscapeJsonString(const std::string& input) {
    std::string output;
    output.reserve(input.size() + 16);
    for (char c : input) {
        if (c == '"') output += "\\\"";
        else if (c == '\\') output += "\\\\";
        else if (c == '\b') output += "\\b";
        else if (c == '\f') output += "\\f";
        else if (c == '\n') output += "\\n";
        else if (c == '\r') output += "\\r";
        else if (c == '\t') output += "\\t";
        else output += c;
    }
    return output;
}

// "4:42" / "1:02:03" -> seconds (0 if not a duration)
static int ParseClock(const std::string& v) {
    if (v.empty() || v.size() > 8) return 0;
    int total = 0, cur = 0, groups = 0;
    for (char c : v) {
        if (c >= '0' && c <= '9') cur = cur * 10 + (c - '0');
        else if (c == ':') { total = total * 60 + cur; cur = 0; groups++; }
        else return 0;
    }
    if (groups == 0) return 0;
    return (total * 60 + cur);
}

// Parses a musicResponsiveListItemRenderer (search or channel page)
static bool ParseMusicListItem(json_t* itemRenderer, Song& out) {

    json_t* flexCols = json_object_get(itemRenderer, "flexColumns");
    if (!flexCols || !json_is_array(flexCols) || json_array_size(flexCols) < 2) return false;

    // Track title
    std::string title;
    json_t* col0 = json_array_get(flexCols, 0);
    json_t* flexCol0 = json_object_get(col0, "musicResponsiveListItemFlexColumnRenderer");
    json_t* text0 = flexCol0 ? json_object_get(flexCol0, "text") : nullptr;
    json_t* runs0 = text0 ? json_object_get(text0, "runs") : nullptr;
    if (runs0 && json_is_array(runs0) && json_array_size(runs0) > 0) {
        json_t* r0 = json_array_get(runs0, 0);
        json_t* tVal = json_object_get(r0, "text");
        if (tVal && json_string_value(tVal)) title = json_string_value(tVal);
    }

    // Artist and creator channel of the track
    std::string artist;
    std::string channelTitle;
    std::string channelId;
    int durationSec = 0;
    json_t* col1 = json_array_get(flexCols, 1);
    json_t* flexCol1 = json_object_get(col1, "musicResponsiveListItemFlexColumnRenderer");
    json_t* text1 = flexCol1 ? json_object_get(flexCol1, "text") : nullptr;
    json_t* runs1 = text1 ? json_object_get(text1, "runs") : nullptr;
    // Line 2 = "Type  •  Artist, Artist  •  views  •  duration": the first run is often the type
    // ("Song", "Album"...) with no link. The real creator = runs pointing to a channel (UC...).
    if (runs1 && json_is_array(runs1)) {
        std::vector<std::string> plain;
        size_t runCount = json_array_size(runs1);
        for (size_t r = 0; r < runCount; r++) {
            json_t* rItem = json_array_get(runs1, r);
            json_t* rText = json_object_get(rItem, "text");
            if (!rText || !json_string_value(rText)) continue;
            std::string val = json_string_value(rText);
            json_t* bId = json_object_get(json_object_get(json_object_get(rItem, "navigationEndpoint"), "browseEndpoint"), "browseId");
            if (bId && json_string_value(bId) && std::string(json_string_value(bId)).rfind("UC", 0) == 0) {
                if (channelTitle.empty()) { channelTitle = val; channelId = json_string_value(bId); }
                if (!artist.empty()) artist += ", ";
                artist += val;
            } else if (ParseClock(val) > 0) {
                durationSec = ParseClock(val);
            } else if (val != " \xE2\x80\xA2 " && val != " et " && val != ", ") {
                plain.push_back(val);
            }
        }
        // No channel link: take the first text after the type
        if (artist.empty() && !plain.empty()) artist = plain.size() > 1 ? plain[1] : plain[0];
    }
    if (channelTitle.empty()) channelTitle = artist;

    // YouTube video ID
    std::string videoId;
    json_t* overlay = json_object_get(itemRenderer, "overlay");
    json_t* thumbOverlay = overlay ? json_object_get(overlay, "musicItemThumbnailOverlayRenderer") : nullptr;
    json_t* overlayContent = thumbOverlay ? json_object_get(thumbOverlay, "content") : nullptr;
    json_t* playBtn = overlayContent ? json_object_get(overlayContent, "musicPlayButtonRenderer") : nullptr;
    json_t* nav = playBtn ? json_object_get(playBtn, "playNavigationEndpoint") : nullptr;
    json_t* watchEp = nav ? json_object_get(nav, "watchEndpoint") : nullptr;
    json_t* vidVal = watchEp ? json_object_get(watchEp, "videoId") : nullptr;
    if (vidVal && json_string_value(vidVal)) {
        videoId = json_string_value(vidVal);
    } else {
        json_t* pData = json_object_get(itemRenderer, "playlistItemData");
        json_t* pVid = pData ? json_object_get(pData, "videoId") : nullptr;
        if (pVid && json_string_value(pVid)) {
            videoId = json_string_value(pVid);
        }
    }

    if (!title.empty() && !videoId.empty()) {
        Song song;
        song.title = title;
        song.artist = artist.empty() ? "YouTube Music" : artist;
        song.channelTitle = channelTitle.empty() ? song.artist : channelTitle;
        song.channelId = channelId;
        song.id = videoId;
        song.durationSeconds = durationSec;
        if (durationSec > 0) {
            char db[16];
            snprintf(db, sizeof(db), "%d:%02d", durationSec / 60, durationSec % 60);
            song.durationFormatted = db;
        }
        song.thumbnailUrl = "https://i.ytimg.com/vi/" + videoId + "/mqdefault.jpg";
        out = song;
    return true;
    }
    return false;
}

// Carousel videos (musicTwoRowItemRenderer): only those with a watchEndpoint
static bool ParseTwoRowItem(json_t* r, Song& out) {
    json_t* runs = json_object_get(json_object_get(r, "title"), "runs");
    json_t* vid = json_object_get(json_object_get(json_object_get(r, "navigationEndpoint"), "watchEndpoint"), "videoId");
    if (!runs || !json_is_array(runs) || json_array_size(runs) == 0 || !vid || !json_string_value(vid)) return false;
    json_t* t = json_object_get(json_array_get(runs, 0), "text");
    if (!t || !json_string_value(t)) return false;
    out.title = json_string_value(t);
    out.id = json_string_value(vid);
    out.thumbnailUrl = "https://i.ytimg.com/vi/" + out.id + "/mqdefault.jpg";
    return true;
}

// 2. Live YouTube Music trends (top hits by region)
std::vector<Song> NetworkManager::GetTrending(const std::string& region) {
    static const std::map<std::string, std::string> Q = {
        {"FR", "top hits France"}, {"US", "top hits USA"}, {"GB", "UK top hits"}, {"CA", "top hits Canada"},
        {"DE", "Top Hits Deutschland"}, {"ES", "exitos Espana"}, {"IT", "hit Italia"}, {"BR", "sucessos Brasil"}, {"JP", "J-POP hits"}
    };
    auto it = Q.find(region);
    std::vector<Song> trending = SearchSongs(it != Q.end() ? it->second : "top hits 2026");
    if (trending.empty()) {
        trending = SearchSongs("top hits 2026");
    }
    return trending;
}

// 3. Direct extraction of the googlevideo audio stream (unbridged Android client, no bot challenge)
std::string NetworkManager::GetAudioStreamUrl(const std::string& videoId) {
    if (videoId.empty()) return "";

    std::cout << "[YouTube Music] Direct audio stream extraction for ID: " << videoId << std::endl;

    std::string url = "https://www.youtube.com/youtubei/v1/player?prettyPrint=false";
    std::string payload = "{\"context\":{\"client\":{\"clientName\":\"ANDROID\",\"clientVersion\":\"21.26.364\",\"androidSdkVersion\":30,\"osName\":\"Android\",\"osVersion\":\"11\",\"gl\":\"FR\",\"hl\":\"fr\"}},\"videoId\":\"" + videoId + "\"}";

    std::vector<std::string> extraHeaders = {
        "User-Agent: com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip",
        "X-YouTube-Client-Name: 3",
        "X-YouTube-Client-Version: 21.26.364"
    };

    std::string response = HttpPost(url, payload, "", 8, extraHeaders);
    if (response.empty()) {
        std::cerr << "[YouTube Music] Empty response from YouTube for " << videoId << std::endl;
        return "";
    }

    json_error_t error;
    json_t* root = json_loads(response.c_str(), 0, &error);
    if (!root) {
        std::cerr << "[YouTube Music] JSON error: " << error.text << std::endl;
        return "";
    }

    std::string chosenStreamUrl;
    json_t* streamingData = json_object_get(root, "streamingData");
    if (streamingData) {
        // 1. Look for a direct audio stream in adaptiveFormats (itag 140 m4a / 251 opus)
        json_t* adaptiveFormats = json_object_get(streamingData, "adaptiveFormats");
        if (adaptiveFormats && json_is_array(adaptiveFormats)) {
            size_t count = json_array_size(adaptiveFormats);
            for (size_t i = 0; i < count; i++) {
                json_t* fmt = json_array_get(adaptiveFormats, i);
                json_t* mimeVal = json_object_get(fmt, "mimeType");
                json_t* urlVal = json_object_get(fmt, "url");

                if (mimeVal && urlVal && json_string_value(mimeVal) && json_string_value(urlVal)) {
                    std::string mime = json_string_value(mimeVal);
                    std::string sUrl = json_string_value(urlVal);

                    if (mime.find("audio/mp4") != std::string::npos) {
                        chosenStreamUrl = sUrl;
                        break;
                    }
                    if (chosenStreamUrl.empty() && mime.find("audio") != std::string::npos) {
                        chosenStreamUrl = sUrl;
                    }
                }
            }
        }

        // 2. If adaptive streams have no direct URL, use the muxed 'formats' stream
        // (itag 18: MP4 360p with AAC 44.1 kHz stereo audio, no bot restriction)
        if (chosenStreamUrl.empty()) {
            json_t* formats = json_object_get(streamingData, "formats");
            if (formats && json_is_array(formats)) {
                size_t count = json_array_size(formats);
                for (size_t i = 0; i < count; i++) {
                    json_t* fmt = json_array_get(formats, i);
                    json_t* urlVal = json_object_get(fmt, "url");
                    if (urlVal && json_string_value(urlVal)) {
                        chosenStreamUrl = json_string_value(urlVal);
                        break;
                    }
                }
            }
        }
    }

    json_decref(root);
    if (!chosenStreamUrl.empty()) {
        std::cout << "[YouTube Music] Direct googlevideo audio stream obtained" << std::endl;
    } else {
        std::cerr << "[YouTube Music] No audio stream available for " << videoId << std::endl;
    }
    return chosenStreamUrl;
}

LyricsData NetworkManager::ParseLrc(const std::string& lrcContent) {
    LyricsData data;
    data.hasLyrics = true;
    data.isSynced = true;

    std::istringstream stream(lrcContent);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty()) continue;

        size_t openBracket = line.find('[');
        size_t closeBracket = line.find(']');

        if (openBracket != std::string::npos && closeBracket != std::string::npos && closeBracket > openBracket) {
            std::string timeStr = line.substr(openBracket + 1, closeBracket - openBracket - 1);
            std::string text = line.substr(closeBracket + 1);

            int min = 0, sec = 0, ms = 0;
            if (sscanf(timeStr.c_str(), "%d:%d.%d", &min, &sec, &ms) >= 2) {
                LyricsLine lLine;
                lLine.timeMs = (min * 60 + sec) * 1000 + ms * 10;
                lLine.text = text;
                data.lines.push_back(lLine);
            }
        }
    }

    if (data.lines.empty()) {
        data.isSynced = false;
    }

    return data;
}

LyricsData NetworkManager::GetLyrics(const std::string& artist, const std::string& title, int durationSeconds) {
    LyricsData lyrics;
    std::string encArtist = UrlEncode(artist);
    std::string encTitle = UrlEncode(title);

    std::string url = "https://lrclib.net/api/get?artist_name=" + encArtist + "&track_name=" + encTitle;
    if (durationSeconds > 0) {
        url += "&duration=" + std::to_string(durationSeconds);
    }

    std::string response = HttpGet(url, 5);
    if (response.empty()) return lyrics;

    json_error_t error;
    json_t* root = json_loads(response.c_str(), 0, &error);
    if (!root) return lyrics;

    json_t* syncedVal = json_object_get(root, "syncedLyrics");
    json_t* plainVal = json_object_get(root, "plainLyrics");

    if (syncedVal && json_is_string(syncedVal)) {
        std::string lrc = json_string_value(syncedVal);
        lyrics = ParseLrc(lrc);
    } else if (plainVal && json_is_string(plainVal)) {
        std::string plain = json_string_value(plainVal);
        lyrics.hasLyrics = true;
        lyrics.isSynced = false;
        std::istringstream stream(plain);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty()) {
                LyricsLine l;
                l.timeMs = 0;
                l.text = line;
                lyrics.lines.push_back(l);
            }
        }
    }

    json_decref(root);
    return lyrics;
}

// Real channel page (InnerTube browse): header + all videos/tracks found in the response
static void CollectSongs(json_t* node, std::vector<Song>& out, const std::string& channelId, const std::string& channelName, size_t limit = 40) {
    if (out.size() >= limit) return;
    if (json_is_object(node)) {
        const char* key; json_t* val;
        json_object_foreach(node, key, val) {
            Song s;
            bool ok = false;
            if (!strcmp(key, "musicResponsiveListItemRenderer")) ok = ParseMusicListItem(val, s);
            else if (!strcmp(key, "musicTwoRowItemRenderer")) ok = ParseTwoRowItem(val, s);
            if (ok) {
                bool dup = false;
                for (auto& o : out) if (o.id == s.id) { dup = true; break; }
                if (!dup) {
                    if (s.channelId.empty()) s.channelId = channelId;
                    if (s.artist.empty()) s.artist = channelName;
                    if (s.channelTitle.empty()) s.channelTitle = channelName;
                    out.push_back(s);
                }
            } else {
                CollectSongs(val, out, channelId, channelName, limit);
            }
        }
    } else if (json_is_array(node)) {
        size_t i; json_t* v;
        json_array_foreach(node, i, v) CollectSongs(v, out, channelId, channelName, limit);
    }
}

// First object carrying `key` (depth-first)
static json_t* FindKey(json_t* node, const char* key) {
    if (json_is_object(node)) {
        json_t* v = json_object_get(node, key);
        if (v) return v;
        const char* k; json_t* c;
        json_object_foreach(node, k, c) { json_t* r = FindKey(c, key); if (r) return r; }
    } else if (json_is_array(node)) {
        size_t i; json_t* c;
        json_array_foreach(node, i, c) { json_t* r = FindKey(c, key); if (r) return r; }
    }
    return nullptr;
}

static std::string FirstRunText(json_t* textObj) {
    json_t* runs = textObj ? json_object_get(textObj, "runs") : nullptr;
    if (!runs || !json_is_array(runs) || json_array_size(runs) == 0) return "";
    json_t* t = json_object_get(json_array_get(runs, 0), "text");
    return (t && json_string_value(t)) ? json_string_value(t) : "";
}

static bool FillChannelProfile(ArtistProfile& profile) {
    std::string payload = "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"1.20230501.01.00\",\"gl\":\"FR\",\"hl\":\"fr\"}},\"browseId\":\"" +
                          NetworkManager::EscapeJsonString(profile.channelId) + "\"}";
    std::string resp = NetworkManager::HttpPost("https://music.youtube.com/youtubei/v1/browse?prettyPrint=false", payload, "https://music.youtube.com", 8);
    if (resp.empty()) return false;
    json_error_t err;
    json_t* root = json_loads(resp.c_str(), 0, &err);
    if (!root) return false;

    json_t* header = json_object_get(root, "header");
    std::string name = FirstRunText(FindKey(header, "title"));
    if (!name.empty()) profile.name = name;
    std::string subs = FirstRunText(FindKey(header, "subscriberCountText"));
    if (!subs.empty()) profile.subscribers = subs + T(" subscribers");
    std::string desc = FirstRunText(FindKey(header, "description"));
    if (!desc.empty()) profile.bio = desc;

    std::vector<Song> songs;
    CollectSongs(json_object_get(root, "contents"), songs, profile.channelId, profile.name);
    json_decref(root);
    if (songs.empty()) return false;
    profile.topSongs = std::move(songs);
    return true;
}

static std::string g_region = "FR";
void NetworkManager::SetRegion(const std::string& r) { if (!r.empty()) g_region = r; }

static std::string ClientCtx() {
    return "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"1.20230501.01.00\",\"gl\":\"" +
           g_region + "\",\"hl\":\"fr\"}}";
}

// Search (mode "songs" | "videos" | "all"). token != "" = next page; nextToken receives the following one.
std::vector<Song> NetworkManager::SearchSongsPage(const std::string& query, const std::string& mode,
                                                  const std::string& token, std::string* nextToken) {
    std::vector<Song> songs;
    if (nextToken) nextToken->clear();
    std::string url = "https://music.youtube.com/youtubei/v1/search?prettyPrint=false";
    std::string payload;
    if (!token.empty()) {
        std::string t = UrlEncode(token);
        url = "https://music.youtube.com/youtubei/v1/search?ctoken=" + t + "&continuation=" + t + "&type=next&prettyPrint=false";
        payload = ClientCtx() + "}";
    } else {
        // params = Songs / Videos filter (the only filters that give following pages)
        std::string params = mode == "songs" ? ",\"params\":\"EgWKAQIIAWoMEA4QChADEAQQCRAF\""
                           : mode == "videos" ? ",\"params\":\"EgWKAQIQAWoMEA4QChADEAQQCRAF\"" : "";
        payload = ClientCtx() + ",\"query\":\"" + EscapeJsonString(query) + "\"" + params + "}";
    }
    std::string response = HttpPost(url, payload, "https://music.youtube.com", 8);
    if (response.empty()) return songs;
    json_error_t error;
    json_t* root = json_loads(response.c_str(), 0, &error);
    if (!root) return songs;
    CollectSongs(root, songs, "", "", 60);
    if (nextToken) {
        json_t* c = FindKey(root, "continuation");
        if (c && json_string_value(c)) *nextToken = json_string_value(c);
    }
    json_decref(root);
    return songs;
}

std::vector<Song> NetworkManager::SearchSongs(const std::string& query) {
    if (query.empty()) return {};
    std::vector<Song> songs = SearchSongsPage(query, "songs", "", nullptr);
    if (songs.size() < 4) songs = SearchSongsPage(query, "all", "", nullptr);
    return songs;
}

// Radio / mix from a track (playlist "RDAMVM<id>")
std::vector<Song> NetworkManager::GetRadio(const std::string& videoId) {
    std::vector<Song> out;
    if (videoId.empty()) return out;
    std::string payload = ClientCtx() + ",\"videoId\":\"" + videoId + "\",\"playlistId\":\"RDAMVM" + videoId + "\",\"isAudioOnly\":true}";
    std::string resp = HttpPost("https://music.youtube.com/youtubei/v1/next?prettyPrint=false", payload, "https://music.youtube.com", 8);
    if (resp.empty()) return out;
    json_error_t err;
    json_t* root = json_loads(resp.c_str(), 0, &err);
    if (!root) return out;

    std::vector<json_t*> stack{ root };
    while (!stack.empty() && out.size() < 50) {
        json_t* n = stack.back();
        stack.pop_back();
        if (json_is_object(n)) {
            json_t* pr = json_object_get(n, "playlistPanelVideoRenderer");
            if (pr) {
                json_t* vid = json_object_get(pr, "videoId");
                std::string title = FirstRunText(json_object_get(pr, "title"));
                if (vid && json_string_value(vid) && !title.empty()) {
                    Song s;
                    s.id = json_string_value(vid);
                    s.title = title;
                    json_t* runs = json_object_get(json_object_get(pr, "longBylineText"), "runs");
                    size_t i; json_t* r;
                    if (runs) json_array_foreach(runs, i, r) {
                        json_t* bid = json_object_get(json_object_get(json_object_get(r, "navigationEndpoint"), "browseEndpoint"), "browseId");
                        json_t* tx = json_object_get(r, "text");
                        if (bid && json_string_value(bid) && tx && json_string_value(tx) && std::string(json_string_value(bid)).rfind("UC", 0) == 0) {
                            if (s.channelId.empty()) { s.channelId = json_string_value(bid); s.channelTitle = json_string_value(tx); }
                            if (!s.artist.empty()) s.artist += ", ";
                            s.artist += json_string_value(tx);
                        }
                    }
                    if (s.artist.empty()) s.artist = "YouTube Music";
                    if (s.channelTitle.empty()) s.channelTitle = s.artist;
                    s.durationSeconds = ParseClock(FirstRunText(json_object_get(pr, "lengthText")));
                    if (s.durationSeconds > 0) {
                        char db[16];
                        snprintf(db, sizeof(db), "%d:%02d", s.durationSeconds / 60, s.durationSeconds % 60);
                        s.durationFormatted = db;
                    }
                    s.thumbnailUrl = "https://i.ytimg.com/vi/" + s.id + "/mqdefault.jpg";
                    out.push_back(s);
                }
                continue;
            }
            const char* k; json_t* c;
            json_object_foreach(n, k, c) stack.push_back(c);
        } else if (json_is_array(n)) {
            // reverse order to keep the playlist order when popping
            for (size_t i = json_array_size(n); i > 0; --i) stack.push_back(json_array_get(n, i - 1));
        }
    }
    json_decref(root);
    return out;
}

ArtistProfile NetworkManager::GetArtistDetails(const std::string& artistName, const std::string& channelId) {
    ArtistProfile profile;
    profile.name = artistName;
    profile.channelId = channelId;
    profile.isLoading = false;

    if (artistName.empty()) return profile;

    // 1. Real YouTube Music channel if its id is known, otherwise search by name
    if (!channelId.empty()) FillChannelProfile(profile);
    if (profile.topSongs.empty()) profile.topSongs = SearchSongs(artistName + " official");
    if (profile.topSongs.empty()) {
        profile.topSongs = SearchSongs(artistName);
    }

    // 2. Bio and listener count from Last.fm (needs the user's own API key from lastfm.json)
    std::string lfmKey = LastFm::ApiKey();
    std::string lfmResp;
    if (!lfmKey.empty()) {
        std::string lastFmUrl = "https://ws.audioscrobbler.com/2.0/?method=artist.getinfo&artist=" + UrlEncode(artistName) +
                                "&api_key=" + lfmKey + "&format=json";
        lfmResp = HttpGet(lastFmUrl, 6);
    }
    if (!lfmResp.empty()) {
        json_error_t jerr;
        json_t* lfmRoot = json_loads(lfmResp.c_str(), 0, &jerr);
        if (lfmRoot) {
            json_t* art = json_object_get(lfmRoot, "artist");
            if (art) {
                json_t* bio = json_object_get(art, "bio");
                if (bio) {
                    json_t* summary = json_object_get(bio, "summary");
                    if (summary && json_string_value(summary)) {
                        std::string s = json_string_value(summary);
                        size_t aPos = s.find("<a href");
                        if (aPos != std::string::npos) s = s.substr(0, aPos);
                        if (profile.bio.empty()) profile.bio = s;
                    }
                }
                json_t* stats = json_object_get(art, "stats");
                if (stats) {
                    json_t* listeners = json_object_get(stats, "listeners");
                    if (listeners && json_string_value(listeners) && profile.subscribers.empty()) {
                        profile.subscribers = std::string(json_string_value(listeners)) + T(" Last.fm listeners");
                    }
                }
            }
            json_decref(lfmRoot);
        }
    }

    if (profile.subscribers.empty()) {
        profile.subscribers = std::string(T("Official artist")) + " - YouTube Music";
    }

    return profile;
}

bool NetworkManager::ScrobbleLastFm(const std::string& artist, const std::string& track) {
    return LastFm::Scrobble(artist, track);
}
