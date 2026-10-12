#ifndef MIITUNE_ACCOUNT_H
#define MIITUNE_ACCOUNT_H

#include "models.h"
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

class AccountManager {
public:
    static AccountManager& Instance();

    bool Initialize();
    void Shutdown();

    // Device Authorization Code flow (YouTube TV style)
    void StartDeviceLogin();
    void CancelDeviceLogin();
    void RefreshSession();
    void Logout();

    // User YouTube data
    void LoadUserPlaylists();
    std::vector<Song> LoadPlaylistSongs(const std::string& playlistId);

    // Thread-safe getters
    UserAccount GetAccount();
    DeviceCodeInfo GetDeviceCodeInfo();

    bool IsLoggedIn() const;

private:
    AccountManager() = default;
    ~AccountManager();
    AccountManager(const AccountManager&) = delete;
    AccountManager& operator=(const AccountManager&) = delete;

    void SaveSession();
    bool LoadSession();

    void PollingWorker();
    bool FetchUserProfile(const std::string& accessToken);
    bool RefreshAccessToken();

    mutable std::mutex m_mutex;
    UserAccount m_account;
    DeviceCodeInfo m_codeInfo;

    std::atomic<bool> m_isPolling{false};
    std::thread m_pollingThread;
};

#endif // MIITUNE_ACCOUNT_H
