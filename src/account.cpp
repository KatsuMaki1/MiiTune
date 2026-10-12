#include "account.h"
#include "network.h"
#include "storage.h"
#include "i18n.h"

#include <curl/curl.h>
#include <jansson.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>

static const char* YOUTUBE_SCOPE         = "https://www.googleapis.com/auth/youtube";

static const char* DEVICE_CODE_URL      = "https://www.youtube.com/o/oauth2/device/code";
static const char* TOKEN_URL            = "https://www.youtube.com/o/oauth2/token";
static const char* USERINFO_URL         = "https://www.googleapis.com/oauth2/v3/userinfo";
static const char* PLAYLISTS_URL        = "https://www.googleapis.com/youtube/v3/playlists";
static const char* PLAYLIST_ITEMS_URL   = "https://www.googleapis.com/youtube/v3/playlistItems";


// Google API credentials are read from google.json on the SD card (see README). Nothing is hardcoded.
static std::string g_google_id, g_google_secret, g_google_key;

static void LoadGoogleConfig() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    std::string txt = StorageRead("google.json");
    if (txt.empty()) {
        StorageWrite("google.json", "{\n  \"client_id\": \"\",\n  \"client_secret\": \"\",\n  \"api_key\": \"\"\n}\n");
        return;
    }
    json_error_t err;
    json_t* r = json_loads(txt.c_str(), 0, &err);
    if (!r) return;
    auto get = [&](const char* k) {
        json_t* v = json_object_get(r, k);
        return (v && json_string_value(v)) ? std::string(json_string_value(v)) : std::string();
    };
    g_google_id = get("client_id");
    g_google_secret = get("client_secret");
    g_google_key = get("api_key");
    json_decref(r);
}

static const std::string& GoogleClientId()     { LoadGoogleConfig(); return g_google_id; }
static const std::string& GoogleClientSecret() { LoadGoogleConfig(); return g_google_secret; }
static const std::string& GoogleApiKey()       { LoadGoogleConfig(); return g_google_key; }

static const char* SESSION_FILE_PATHS[] = {
    "fs:/vol/external01/wiiu/apps/MiiTune/miitune_account.json",
    "/vol/external01/wiiu/apps/MiiTune/miitune_account.json",
    "miitune_account.json",
    nullptr
};

AccountManager& AccountManager::Instance() {
    static AccountManager instance;
    return instance;
}

AccountManager::~AccountManager() {
    Shutdown();
}

bool AccountManager::Initialize() {
    bool loaded = LoadSession();
    if (loaded && !m_account.refresh_token.empty()) {
        std::cout << "[Account] Session loaded for " << m_account.user_name << std::endl;
        RefreshSession();
    }
    return true;
}

void AccountManager::Shutdown() {
    CancelDeviceLogin();
}

UserAccount AccountManager::GetAccount() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_account;
}

DeviceCodeInfo AccountManager::GetDeviceCodeInfo() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_codeInfo;
}

bool AccountManager::IsLoggedIn() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_account.is_logged_in;
}

void AccountManager::StartDeviceLogin() {
    CancelDeviceLogin();

    if (GoogleClientId().empty() || GoogleClientSecret().empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_codeInfo = DeviceCodeInfo();
        m_codeInfo.active = true;
        m_codeInfo.status_msg = T("Add your Google credentials to google.json (see the README)");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_codeInfo = DeviceCodeInfo();
        m_codeInfo.is_requesting = true;
        m_codeInfo.status_msg = T("Connecting to Google...");
    }

    std::thread([this]() {
        std::string form = "client_id=" + NetworkManager::UrlEncode(GoogleClientId()) +
                           "&client_secret=" + NetworkManager::UrlEncode(GoogleClientSecret()) +
                           "&scope=" + NetworkManager::UrlEncode(YOUTUBE_SCOPE) +
                           "&device_id=wiiu-" + std::to_string((long long)time(nullptr)) +
                           "&device_model=Wii%20U";

        std::vector<std::string> headers = {
            "Content-Type: application/x-www-form-urlencoded"
        };

        std::string response = NetworkManager::HttpPost(DEVICE_CODE_URL, form, "", 12, headers);

        json_error_t jerr;
        json_t* root = json_loads(response.c_str(), 0, &jerr);
        if (!root) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_codeInfo.active = true;
            m_codeInfo.is_requesting = false;
            m_codeInfo.status_msg = T("Network error. Check your WiFi connection.");
            return;
        }

        json_t* jDevCode = json_object_get(root, "device_code");
        json_t* jUsrCode = json_object_get(root, "user_code");
        json_t* jUrl     = json_object_get(root, "verification_url");
        json_t* jInt     = json_object_get(root, "interval");
        json_t* jExp     = json_object_get(root, "expires_in");

        if (jDevCode && jUsrCode && json_string_value(jDevCode) && json_string_value(jUsrCode)) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_codeInfo.active = true;
                m_codeInfo.is_requesting = false;
                m_codeInfo.device_code = json_string_value(jDevCode);
                m_codeInfo.user_code = json_string_value(jUsrCode);
                if (jUrl && json_string_value(jUrl)) {
                    m_codeInfo.verification_url = json_string_value(jUrl);
                } else {
                    m_codeInfo.verification_url = "https://www.google.com/device";
                }
                m_codeInfo.interval = (jInt && json_is_integer(jInt)) ? (int)json_integer_value(jInt) : 5;
                if (m_codeInfo.interval < 5) m_codeInfo.interval = 5;
                m_codeInfo.expires_in = (jExp && json_is_integer(jExp)) ? (int)json_integer_value(jExp) : 1800;
                m_codeInfo.expires_at = (uint64_t)time(nullptr) + m_codeInfo.expires_in;
                m_codeInfo.status_msg = T("Waiting for approval on your phone...");
            }

            m_isPolling = false;
            if (m_pollingThread.joinable()) {
                m_pollingThread.join();
            }
            m_isPolling = true;
            m_pollingThread = std::thread(&AccountManager::PollingWorker, this);
        } else {
            json_t* jErr = json_object_get(root, "error");
            std::string errDetail;
            if (jErr && json_string_value(jErr)) errDetail = json_string_value(jErr);

            std::lock_guard<std::mutex> lock(m_mutex);
            m_codeInfo.active = true;
            m_codeInfo.is_requesting = false;
            m_codeInfo.status_msg = errDetail.empty() ? T("Could not get the YouTube code.") : (T("Error: ") + errDetail);
        }

        json_decref(root);
    }).detach();
}

void AccountManager::CancelDeviceLogin() {
    m_isPolling = false;
    if (m_pollingThread.joinable()) {
        m_pollingThread.join();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_codeInfo.active = false;
    m_codeInfo.is_polling = false;
    m_codeInfo.is_requesting = false;
}

void AccountManager::PollingWorker() {
    std::string deviceCode;
    int interval = 5;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        deviceCode = m_codeInfo.device_code;
        interval = m_codeInfo.interval;
        m_codeInfo.is_polling = true;
    }

    std::cout << "[Account] Starting OAuth polling (interval " << interval << "s)" << std::endl;

    while (m_isPolling) {
        for (int i = 0; i < interval * 10; ++i) {
            if (!m_isPolling) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::string form = "client_id=" + NetworkManager::UrlEncode(GoogleClientId()) +
                           "&client_secret=" + NetworkManager::UrlEncode(GoogleClientSecret()) +
                           "&code=" + NetworkManager::UrlEncode(deviceCode) +
                           "&grant_type=" + NetworkManager::UrlEncode("http://oauth.net/grant_type/device/1.0");

        std::vector<std::string> headers = {
            "Content-Type: application/x-www-form-urlencoded"
        };

        std::string response = NetworkManager::HttpPost(TOKEN_URL, form, "", 12, headers);

        json_error_t jerr;
        json_t* root = json_loads(response.c_str(), 0, &jerr);
        if (!root) continue;

        json_t* jErr = json_object_get(root, "error");
        if (jErr && json_string_value(jErr)) {
            std::string errStr = json_string_value(jErr);
            if (errStr == "authorization_pending") {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_codeInfo.status_msg = T("Waiting for approval on google.com/device...");
            } else if (errStr == "slow_down") {
                interval += 5;
            } else if (errStr == "expired_token") {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_codeInfo.active = true;
                m_codeInfo.status_msg = T("The code has expired. Touch 'New code'.");
                m_isPolling = false;
                json_decref(root);
                break;
            } else if (errStr == "access_denied") {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_codeInfo.active = true;
                m_codeInfo.status_msg = T("Sign-in denied by the user.");
                m_isPolling = false;
                json_decref(root);
                break;
            } else {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_codeInfo.status_msg = "Statut : " + errStr;
            }
            json_decref(root);
            continue;
        }

        json_t* jAccess = json_object_get(root, "access_token");
        json_t* jRefresh = json_object_get(root, "refresh_token");
        json_t* jExpires = json_object_get(root, "expires_in");

        if (jAccess && json_string_value(jAccess)) {
            std::string accessToken = json_string_value(jAccess);
            std::string refreshToken = (jRefresh && json_string_value(jRefresh)) ? json_string_value(jRefresh) : "";
            int expIn = (jExpires && json_is_integer(jExpires)) ? (int)json_integer_value(jExpires) : 3600;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_account.access_token = accessToken;
                if (!refreshToken.empty()) m_account.refresh_token = refreshToken;
                auto now = std::chrono::system_clock::now().time_since_epoch();
                m_account.token_expires_at = std::chrono::duration_cast<std::chrono::seconds>(now).count() + expIn;
                m_account.is_logged_in = true;
                m_codeInfo.active = false;
                m_codeInfo.is_polling = false;
                m_codeInfo.status_msg = T("Signed in!");
            }

            std::cout << "[Account] Token received successfully" << std::endl;
            json_decref(root);

            // Fetch the user name and avatar
            FetchUserProfile(accessToken);
            SaveSession();
            LoadUserPlaylists();
            m_isPolling = false;
            break;
        }

        json_decref(root);
    }
}

bool AccountManager::FetchUserProfile(const std::string& accessToken) {
    if (accessToken.empty()) return false;

    std::string authHdr = "Authorization: Bearer " + accessToken;
    std::vector<std::string> headers = { authHdr };

    CURL* curl = curl_easy_init();
    std::string response;
    if (curl) {
        struct curl_slist* hList = nullptr;
        hList = curl_slist_append(hList, authHdr.c_str());
        hList = curl_slist_append(hList, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)");

        curl_easy_setopt(curl, CURLOPT_URL, USERINFO_URL);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hList);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t sz, size_t nm, void* u) -> size_t {
            ((std::string*)u)->append((char*)ptr, sz * nm);
            return sz * nm;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        curl_easy_perform(curl);
        curl_slist_free_all(hList);
        curl_easy_cleanup(curl);
    }

    if (response.empty()) return false;

    json_error_t jerr;
    json_t* root = json_loads(response.c_str(), 0, &jerr);
    if (!root) return false;

    json_t* jName  = json_object_get(root, "name");
    json_t* jEmail = json_object_get(root, "email");
    json_t* jPic   = json_object_get(root, "picture");

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (jName && json_string_value(jName))   m_account.user_name = json_string_value(jName);
        if (jEmail && json_string_value(jEmail)) m_account.user_email = json_string_value(jEmail);
        if (jPic && json_string_value(jPic))     m_account.avatar_url = json_string_value(jPic);
    }

    json_decref(root);
    return true;
}

bool AccountManager::RefreshAccessToken() {
    std::string refreshToken;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        refreshToken = m_account.refresh_token;
    }
    if (refreshToken.empty()) return false;

    std::string form = "client_id=" + NetworkManager::UrlEncode(GoogleClientId()) +
                       "&client_secret=" + NetworkManager::UrlEncode(GoogleClientSecret()) +
                       "&refresh_token=" + NetworkManager::UrlEncode(refreshToken) +
                       "&grant_type=refresh_token";

    std::vector<std::string> headers = {
        "Content-Type: application/x-www-form-urlencoded"
    };

    std::string response = NetworkManager::HttpPost(TOKEN_URL, form, "", 12, headers);

    json_error_t jerr;
    json_t* root = json_loads(response.c_str(), 0, &jerr);
    if (!root) return false;

    json_t* jAccess = json_object_get(root, "access_token");
    json_t* jExp    = json_object_get(root, "expires_in");

    bool success = false;
    if (jAccess && json_string_value(jAccess)) {
        std::string newAccess = json_string_value(jAccess);
        int expIn = (jExp && json_is_integer(jExp)) ? (int)json_integer_value(jExp) : 3600;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_account.access_token = newAccess;
            auto now = std::chrono::system_clock::now().time_since_epoch();
            m_account.token_expires_at = std::chrono::duration_cast<std::chrono::seconds>(now).count() + expIn;
            m_account.is_logged_in = true;
        }

        FetchUserProfile(newAccess);
        SaveSession();
        LoadUserPlaylists();
        success = true;
    }

    json_decref(root);
    return success;
}

void AccountManager::RefreshSession() {
    std::thread([this]() {
        RefreshAccessToken();
    }).detach();
}

void AccountManager::Logout() {
    CancelDeviceLogin();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_account = UserAccount();
    }

    // Delete the session file
    for (int i = 0; SESSION_FILE_PATHS[i] != nullptr; ++i) {
        std::remove(SESSION_FILE_PATHS[i]);
    }
    std::cout << "[Account] Signed out." << std::endl;
}

void AccountManager::SaveSession() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_account.refresh_token.empty()) return;

    json_t* root = json_object();
    json_object_set_new(root, "refresh_token", json_string(m_account.refresh_token.c_str()));
    json_object_set_new(root, "access_token", json_string(m_account.access_token.c_str()));
    json_object_set_new(root, "user_name", json_string(m_account.user_name.c_str()));
    json_object_set_new(root, "user_email", json_string(m_account.user_email.c_str()));
    json_object_set_new(root, "avatar_url", json_string(m_account.avatar_url.c_str()));
    json_object_set_new(root, "token_expires_at", json_integer(m_account.token_expires_at));

    char* dumped = json_dumps(root, JSON_INDENT(2));
    if (dumped) {
        for (int i = 0; SESSION_FILE_PATHS[i] != nullptr; ++i) {
            std::ofstream out(SESSION_FILE_PATHS[i]);
            if (out.is_open()) {
                out << dumped;
                out.close();
                break;
            }
        }
        free(dumped);
    }

    json_decref(root);
}

bool AccountManager::LoadSession() {
    std::string content;
    for (int i = 0; SESSION_FILE_PATHS[i] != nullptr; ++i) {
        std::ifstream in(SESSION_FILE_PATHS[i]);
        if (in.is_open()) {
            std::stringstream ss;
            ss << in.rdbuf();
            content = ss.str();
            in.close();
            break;
        }
    }

    if (content.empty()) return false;

    json_error_t jerr;
    json_t* root = json_loads(content.c_str(), 0, &jerr);
    if (!root) return false;

    json_t* jRefresh = json_object_get(root, "refresh_token");
    json_t* jAccess  = json_object_get(root, "access_token");
    json_t* jName    = json_object_get(root, "user_name");
    json_t* jEmail   = json_object_get(root, "user_email");
    json_t* jPic     = json_object_get(root, "avatar_url");
    json_t* jExp     = json_object_get(root, "token_expires_at");

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (jRefresh && json_string_value(jRefresh)) m_account.refresh_token = json_string_value(jRefresh);
        if (jAccess && json_string_value(jAccess))   m_account.access_token = json_string_value(jAccess);
        if (jName && json_string_value(jName))       m_account.user_name = json_string_value(jName);
        if (jEmail && json_string_value(jEmail))     m_account.user_email = json_string_value(jEmail);
        if (jPic && json_string_value(jPic))         m_account.avatar_url = json_string_value(jPic);
        if (jExp && json_is_integer(jExp))           m_account.token_expires_at = json_integer_value(jExp);

        if (!m_account.refresh_token.empty()) {
            m_account.is_logged_in = true;
        }
    }

    json_decref(root);
    return true;
}

void AccountManager::LoadUserPlaylists() {
    std::string accessToken;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        accessToken = m_account.access_token;
        m_account.is_loading_playlists = true;
    }

    if (accessToken.empty()) return;

    std::thread([this, accessToken]() {
        std::string url = std::string(PLAYLISTS_URL) +
                          "?part=snippet,contentDetails&mine=true&maxResults=25&key=" + GoogleApiKey();

        std::string authHdr = "Authorization: Bearer " + accessToken;
        CURL* curl = curl_easy_init();
        std::string response;
        if (curl) {
            struct curl_slist* hList = nullptr;
            hList = curl_slist_append(hList, authHdr.c_str());
            hList = curl_slist_append(hList, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)");

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hList);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t sz, size_t nm, void* u) -> size_t {
                ((std::string*)u)->append((char*)ptr, sz * nm);
                return sz * nm;
            });
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

            curl_easy_perform(curl);
            curl_slist_free_all(hList);
            curl_easy_cleanup(curl);
        }

        std::vector<Playlist> playlists;
        json_error_t jerr;
        json_t* root = json_loads(response.c_str(), 0, &jerr);
        if (root) {
            json_t* items = json_object_get(root, "items");
            if (items && json_is_array(items)) {
                size_t count = json_array_size(items);
                for (size_t i = 0; i < count; i++) {
                    json_t* item = json_array_get(items, i);
                    json_t* jId = json_object_get(item, "id");
                    json_t* snippet = json_object_get(item, "snippet");
                    json_t* details = json_object_get(item, "contentDetails");

                    Playlist p;
                    if (jId && json_string_value(jId)) p.id = json_string_value(jId);

                    if (snippet) {
                        json_t* jTitle = json_object_get(snippet, "title");
                        if (jTitle && json_string_value(jTitle)) p.title = json_string_value(jTitle);

                        json_t* thumbs = json_object_get(snippet, "thumbnails");
                        if (thumbs) {
                            json_t* med = json_object_get(thumbs, "medium");
                            if (!med) med = json_object_get(thumbs, "default");
                            if (med) {
                                json_t* u = json_object_get(med, "url");
                                if (u && json_string_value(u)) p.thumbnailUrl = json_string_value(u);
                            }
                        }
                    }

                    if (details) {
                        json_t* jCount = json_object_get(details, "itemCount");
                        if (jCount && json_is_integer(jCount)) p.itemCount = (int)json_integer_value(jCount);
                    }

                    if (!p.id.empty() && !p.title.empty()) {
                        playlists.push_back(p);
                    }
                }
            }
            json_decref(root);
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_account.playlists = playlists;
            m_account.is_loading_playlists = false;
        }

        std::cout << "[Account] " << playlists.size() << " playlists fetched." << std::endl;
    }).detach();
}

std::vector<Song> AccountManager::LoadPlaylistSongs(const std::string& playlistId) {
    std::vector<Song> songs;
    if (playlistId.empty()) return songs;

    std::string accessToken;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        accessToken = m_account.access_token;
    }

    std::string url = std::string(PLAYLIST_ITEMS_URL) +
                      "?part=snippet&playlistId=" + NetworkManager::UrlEncode(playlistId) +
                      "&maxResults=50&key=" + GoogleApiKey();

    std::string authHdr = "Authorization: Bearer " + accessToken;
    CURL* curl = curl_easy_init();
    std::string response;
    if (curl) {
        struct curl_slist* hList = nullptr;
        if (!accessToken.empty()) {
            hList = curl_slist_append(hList, authHdr.c_str());
        }
        hList = curl_slist_append(hList, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hList);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t sz, size_t nm, void* u) -> size_t {
            ((std::string*)u)->append((char*)ptr, sz * nm);
            return sz * nm;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        curl_easy_perform(curl);
        curl_slist_free_all(hList);
        curl_easy_cleanup(curl);
    }

    json_error_t jerr;
    json_t* root = json_loads(response.c_str(), 0, &jerr);
    if (!root) return songs;

    json_t* items = json_object_get(root, "items");
    if (items && json_is_array(items)) {
        size_t count = json_array_size(items);
        for (size_t i = 0; i < count; i++) {
            json_t* item = json_array_get(items, i);
            json_t* snippet = json_object_get(item, "snippet");
            if (!snippet) continue;

            json_t* jTitle = json_object_get(snippet, "title");
            json_t* jArtist = json_object_get(snippet, "videoOwnerChannelTitle");
            json_t* resId = json_object_get(snippet, "resourceId");
            json_t* jVid = resId ? json_object_get(resId, "videoId") : nullptr;

            if (jVid && json_string_value(jVid)) {
                std::string vid = json_string_value(jVid);
                if (vid == "null" || vid.empty()) continue;

                Song s;
                s.id = vid;
                s.title = (jTitle && json_string_value(jTitle)) ? json_string_value(jTitle) : T("YouTube track");
                s.artist = (jArtist && json_string_value(jArtist)) ? json_string_value(jArtist) : "YouTube Music";
                s.durationSeconds = 210;
                s.durationFormatted = "03:30";
                s.thumbnailUrl = "https://i.ytimg.com/vi/" + vid + "/mqdefault.jpg";
                songs.push_back(s);
            }
        }
    }

    json_decref(root);
    return songs;
}
