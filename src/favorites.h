#ifndef MIITUNE_FAVORITES_H
#define MIITUNE_FAVORITES_H

#include "models.h"
#include <vector>
#include <string>
#include <mutex>

class FavoritesManager {
public:
    static FavoritesManager& Instance();

    void Initialize();
    bool IsFavorite(const std::string& songId);
    void ToggleFavorite(const Song& song);
    std::vector<Song> GetFavorites();

private:
    FavoritesManager() = default;
    void LoadFromFile();
    void SaveToFile();

    std::vector<Song> m_favorites;
    std::mutex m_mutex;
};

#endif // MIITUNE_FAVORITES_H
