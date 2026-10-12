#include "models.h"
#include "network.h"
#include "account.h"
#include "audio.h"
#include "favorites.h"
#include "library.h"
#include "lastfm.h"
#include "storage.h"
#include "threadutil.h"
#include "mii.h"
#include "icons.h"
#include "renderer.h"
#include "i18n.h"
#include "embedded_assets.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>
#include <functional>
#include <cmath>

#include <whb/proc.h>
#include <whb/log.h>
#include <vpad/input.h>
#include <coreinit/thread.h>
#include <coreinit/debug.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#if defined(__WIIU__)
#include <SDL2/SDL_system.h>
#include <SDL2/SDL_syswm.h>
#endif

// ------------------------------------------------------------------ threads

struct SafeTaskData {
    std::function<void()> task;
};

static int SafeTaskRunner(void* data) {
    BackgroundThread(4);   // core 2: away from rendering (core 1) and audio decoding (core 0)
    auto* td = (SafeTaskData*)data;
    if (td) {
        if (td->task) td->task();
        delete td;
    }
    return 0;
}

// All network work goes through here: SDL threads with a 512 KB stack (std::thread crashes on Wii U)
static void RunAsync(const char* name, std::function<void()> task) {
    auto* td = new SafeTaskData{ std::move(task) };
    SDL_Thread* th = SDL_CreateThreadWithStackSize(SafeTaskRunner, name, 512 * 1024, td);
    if (th) {
        SDL_DetachThread(th);
    } else {
        if (td->task) td->task();
        delete td;
    }
}

static std::atomic<uint32_t> s_play_sequence{0};

// ------------------------------------------------------------------ toast / keyboard

static void Toast(AppContext& ctx, const std::string& msg) {
    ctx.toast = msg;
    ctx.toast_until = SDL_GetTicks() + 2600;
}

// From a thread: shown on the next frame
static void PostToast(AppContext& ctx, const std::string& msg) {
    std::lock_guard<std::mutex> lock(ctx.data_mutex);
    ctx.pending_toast = msg;
}

// playlist = true: the typed text is the name of a new playlist
static void OpenWiiUKeyboard(AppContext& ctx, bool playlist = false) {
    ctx.text_mode = playlist ? 1 : 0;
#if defined(__WIIU__)
    SDL_WiiUSetSWKBDKeyboardMode(SDL_WIIU_SWKBD_KEYBOARD_MODE_FULL);
    SDL_WiiUSetSWKBDOKLabel(playlist ? T("Create") : T("Search"));
    SDL_WiiUSetSWKBDHintText(playlist ? T("Playlist name...") : T("Search for a song or artist..."));
    if (!playlist && !ctx.search_query.empty()) {
        SDL_WiiUSetSWKBDInitialText(ctx.search_query.c_str());
        SDL_WiiUSetSWKBDHighlightInitialText(SDL_TRUE);
    }
#endif
    SDL_StopTextInput();   // start from a clean state: a cancelled input left SDL "already active" and the keyboard never reopened
    SDL_StartTextInput();
}

// ------------------------------------------------------------------ lists / selection

static const std::vector<Song>* GetList(AppContext& c, int id) {
    switch (id) {
        case LIST_CATALOG: return &c.catalog;
        case LIST_SEARCH:  return &c.search_results;
        case LIST_ARTIST:  return &c.artist_profile.topSongs;
        case LIST_FAVS:    return &c.favorites;
        case LIST_HISTORY: return &c.history;
        case LIST_LIKED:   return &c.liked;
        case LIST_QUEUE:   return &c.queue;
        case LIST_PLAYLIST: return &c.pl_songs;
    }
    return nullptr;
}

static void SetListSel(AppContext& c, int id, int idx) {
    HomeSec secs[6];
    int n = BuildHomeSections(c, secs);
    switch (id) {
        case LIST_CATALOG: c.selected_song_idx = idx; break;
        case LIST_SEARCH:  c.search_selected_idx = idx; break;
        case LIST_ARTIST:  c.artist_profile.selected_idx = idx; break;
        case LIST_FAVS:    c.fav_selected_idx = idx; break;
        case LIST_HISTORY: c.hist_sel = idx; break;
        case LIST_LIKED:   c.liked_sel = idx; break;
        case LIST_QUEUE:   c.queue_sel = idx; break;
        case LIST_PLAYLIST: c.pl_view_sel = idx; break;
    }
    for (int i = 0; i < n; i++) if (secs[i].listId == id) c.home_row = i;
}

// ------------------------------------------------------------------ favorites / artist / playback

static void ToggleFavorite(AppContext& ctx, Song& song) {
    if (song.id.empty()) return;
    FavoritesManager::Instance().ToggleFavorite(song);
    song.isFavorite = FavoritesManager::Instance().IsFavorite(song.id);
    if (ctx.current_song.id == song.id) {
        ctx.current_song.isFavorite = song.isFavorite;
    }
    ctx.favorites = FavoritesManager::Instance().GetFavorites();
    int n = (int)ctx.favorites.size();
    if (ctx.fav_selected_idx >= n) ctx.fav_selected_idx = std::max(0, n - 1);
}

static void OpenArtistProfile(AppContext& ctx, const std::string& artistName, const std::string& channelId = "") {
    if (artistName.empty()) return;
    if (ctx.state != STATE_ARTIST) {
        ctx.previous_state = ctx.state;
    }
    ctx.state = STATE_ARTIST;
    ctx.artist_profile.name = artistName;
    ctx.artist_profile.channelId = channelId;
    ctx.artist_profile.isLoading = true;
    ctx.artist_profile.topSongs.clear();
    ctx.artist_profile.selected_idx = 0;
    ctx.artist_profile.scroll_x = 0;

    RunAsync("ArtistProfile", [&ctx, artistName, channelId]() {
        ArtistProfile profile = NetworkManager::GetArtistDetails(artistName, channelId);
        {
            std::lock_guard<std::mutex> lock(ctx.data_mutex);
            ctx.pending_artist_profile = std::move(profile);
            ctx.has_pending_artist = true;
        }
    });
}

static void OpenArtistOfSong(AppContext& ctx, const Song& s) {
    std::string creator = s.channelTitle.empty() ? s.artist : s.channelTitle;
    if (!creator.empty()) OpenArtistProfile(ctx, creator, s.channelId);
}

static void PlaySong(AppContext& ctx, const Song& song) {
    if (song.id.empty()) return;
    ctx.current_song = song;
    ctx.current_song.isFavorite = FavoritesManager::Instance().IsFavorite(song.id);
    ctx.play_pending = true;
    ctx.play_started = false;
    ctx.play_started_ms = SDL_GetTicks();
    uint32_t playId = ++s_play_sequence;
    ctx.current_play_req_id = playId;
    OSReport("[MiiTune] Starting playback: %s - %s\n", song.title.c_str(), song.artist.c_str());

    ctx.prefetch_id.clear();

    // History: disk write outside the render thread
    RunAsync("HistorySave", [&ctx, song]() {
        Library::Instance().AddHistory(song);
        std::lock_guard<std::mutex> lock(ctx.data_mutex);
        ctx.history_dirty = true;
    });

    RunAsync("PlayExtract", [&ctx, song, playId]() {
        if (playId != s_play_sequence.load()) return;
        // Track already prefetched? start immediately, no extraction or download
        bool started = AudioPlayer::Instance().PlayPrefetched(song.id, song.durationSeconds);
        if (!started) {
            std::string streamUrl = NetworkManager::GetAudioStreamUrl(song.id);
            if (playId != s_play_sequence.load()) return;
            if (!streamUrl.empty()) {
                AudioPlayer::Instance().PlayStream(streamUrl, song.durationSeconds);
                started = true;
            }
        }
        if (started) {
            {
                std::lock_guard<std::mutex> lock(ctx.data_mutex);
                ctx.pending_started = true;
            }
            LastFm::NowPlaying(song.artist, song.title);
        } else {
            OSReport("[MiiTune] Stream extraction error for %s\n", song.id.c_str());
            std::lock_guard<std::mutex> lock(ctx.data_mutex);
            ctx.pending_fail = true;
            ctx.pending_toast = T("Cannot play: stream not found");
        }
    });

    RunAsync("LyricsFetch", [&ctx, song, playId]() {
        LyricsData lyrics = NetworkManager::GetLyrics(song.artist, song.title, song.durationSeconds);
        if (playId != s_play_sequence.load()) return;
        {
            std::lock_guard<std::mutex> lock(ctx.data_mutex);
            ctx.pending_lyrics = std::move(lyrics);
            ctx.has_pending_lyrics = true;
        }
    });
}

// ------------------------------------------------------------------ queue

static void PlayQueueIdx(AppContext& ctx, int i) {
    if (i < 0 || i >= (int)ctx.queue.size()) return;
    ctx.queue_idx = i;
    ctx.queue_sel = i;
    PlaySong(ctx, ctx.queue[i]);
}

// Start a track from a list: the list becomes the queue
static void PlayFromList(AppContext& ctx, int listId, int idx) {
    const std::vector<Song>* l = GetList(ctx, listId);
    if (!l || idx < 0 || idx >= (int)l->size()) return;
    if (listId == LIST_QUEUE) { PlayQueueIdx(ctx, idx); return; }
    SetListSel(ctx, listId, idx);
    std::vector<Song> copy = *l;
    ctx.queue = std::move(copy);
    PlayQueueIdx(ctx, idx);
}

static void QueueEnsureCurrent(AppContext& ctx) {
    if (ctx.queue.empty() && !ctx.current_song.id.empty()) {
        ctx.queue.push_back(ctx.current_song);
        ctx.queue_idx = 0;
    }
}

// Insert after the current track; returns the position
static int QueueInsertNext(AppContext& ctx, const Song& s) {
    QueueEnsureCurrent(ctx);
    int pos = std::min((int)ctx.queue.size(), ctx.queue_idx + 1);
    ctx.queue.insert(ctx.queue.begin() + pos, s);
    return pos;
}

static void StartRadio(AppContext& ctx, const Song& seed, bool thenNext) {
    if (ctx.radio_loading || seed.id.empty()) return;
    ctx.radio_loading = true;
    ctx.radio_then_next = thenNext;
    std::string id = seed.id;
    RunAsync("Radio", [&ctx, id]() {
        std::vector<Song> songs = NetworkManager::GetRadio(id);
        std::lock_guard<std::mutex> lock(ctx.data_mutex);
        ctx.pending_radio = std::move(songs);
        ctx.has_pending_radio = true;
    });
}

static void NextTrack(AppContext& ctx, bool manual) {
    if (ctx.queue.empty()) { if (manual) Toast(ctx, T("Queue is empty")); return; }
    if (!manual && ctx.repeat_mode == RepeatMode::ONE) { PlayQueueIdx(ctx, std::max(0, ctx.queue_idx)); return; }
    if (ctx.shuffle_enabled && ctx.queue.size() > 1) {
        int n;
        do { n = rand() % (int)ctx.queue.size(); } while (n == ctx.queue_idx);
        PlayQueueIdx(ctx, n);
    } else if (ctx.queue_idx + 1 < (int)ctx.queue.size()) {
        PlayQueueIdx(ctx, ctx.queue_idx + 1);
    } else if (ctx.repeat_mode == RepeatMode::ALL) {
        PlayQueueIdx(ctx, 0);
    } else if (ctx.settings.autoplay && !ctx.current_song.id.empty()) {
        Toast(ctx, T("Auto radio..."));
        StartRadio(ctx, ctx.current_song, true);
    } else if (manual) {
        Toast(ctx, T("End of queue"));
    }
}

static void PrevTrack(AppContext& ctx) {
    if (AudioPlayer::Instance().GetCurrentPosition() > 5 || ctx.queue_idx <= 0) {
        AudioPlayer::Instance().Seek(0);
    } else {
        PlayQueueIdx(ctx, ctx.queue_idx - 1);
    }
}

// ------------------------------------------------------------------ search

static void TriggerSearch(AppContext& ctx, const std::string& query) {
    if (query.empty()) return;
    ctx.search_is_loading = true;
    ctx.search_query = query;
    ctx.search_title.clear();
    ctx.search_tag_idx = -1;
    ctx.search_selected_idx = 0;
    ctx.search_scroll_x = 0;
    ctx.search_next_token.clear();
    ctx.search_loading_more = false;
    std::string mode = ctx.settings.search_mode;

    RunAsync("SearchReq", [&ctx, query, mode]() {
        std::string next;
        std::vector<Song> results = NetworkManager::SearchSongsPage(query, mode, "", &next);
        if (mode != "all" && results.size() < 4) results = NetworkManager::SearchSongsPage(query, "all", "", &next);
        {
            std::lock_guard<std::mutex> lock(ctx.data_mutex);
            ctx.pending_search_results = std::move(results);
            ctx.pending_search_token = next;
            ctx.has_pending_search = true;
        }
    });
}

static void LoadMoreSearch(AppContext& ctx) {
    ctx.search_loading_more = true;
    std::string q = ctx.search_query, tok = ctx.search_next_token, mode = ctx.settings.search_mode;
    RunAsync("SearchMore", [&ctx, q, tok, mode]() {
        std::string next;
        std::vector<Song> more = NetworkManager::SearchSongsPage(q, mode, tok, &next);
        std::lock_guard<std::mutex> lock(ctx.data_mutex);
        ctx.pending_more = std::move(more);
        ctx.pending_more_token = next;
        ctx.has_pending_more = true;
    });
}

static void SubmitSearch(AppContext& ctx, const std::string& q) {
    Library::Instance().AddSearch(q);
    ctx.search_history = Library::Instance().Searches();
    ctx.state = STATE_SEARCH;
    TriggerSearch(ctx, q);
}

static void SelectChip(AppContext& ctx, int chip) {
    ctx.state = STATE_SEARCH;
    ctx.search_selected_idx = 0;
    ctx.search_scroll_x = 0;
    if (chip == 0) {
        ctx.search_results = ctx.favorites;
        ctx.search_is_loading = false;
        ctx.search_query.clear();
        ctx.search_next_token.clear();
        ctx.search_title = T("My favorites");
        ctx.search_tag_idx = 0;
    } else {
        TriggerSearch(ctx, QUICK_MUSIC_TAGS[chip - 1]);
        ctx.search_title = QUICK_MUSIC_TAGS[chip - 1];
        ctx.search_tag_idx = chip;
    }
}

static void LoadInitialTrending(AppContext& ctx) {
    std::string region = ctx.settings.region;
    RunAsync("TrendingReq", [&ctx, region]() {
        std::vector<Song> liveTrending = NetworkManager::GetTrending(region);
        if (!liveTrending.empty()) {
            std::lock_guard<std::mutex> lock(ctx.data_mutex);
            ctx.pending_catalog = std::move(liveTrending);
            ctx.has_pending_catalog = true;
        }
    });
}

// ------------------------------------------------------------------ actions (touch + gamepad)

static void RefreshPlaylists(AppContext& ctx) {
    ctx.playlists = Library::Instance().Playlists();
    if (ctx.pl_open >= (int)ctx.playlists.size()) ctx.pl_open = -1;
    if (ctx.pl_open >= 0) ctx.pl_songs = ctx.playlists[ctx.pl_open].songs;
    else ctx.pl_songs.clear();
    ctx.pl_sel = std::max(0, std::min(ctx.pl_sel, (int)ctx.playlists.size() - 1));
    ctx.pl_view_sel = std::max(0, std::min(ctx.pl_view_sel, (int)ctx.pl_songs.size() - 1));
}

// Name typed on the Wii U keyboard -> new playlist (+ the pending track if we came from the menu)
static void CreatePlaylistNamed(AppContext& ctx, std::string name) {
    size_t a = name.find_first_not_of(" \t\r\n"), b = name.find_last_not_of(" \t\r\n");
    name = (a == std::string::npos) ? "" : name.substr(a, b - a + 1);
    if (name.empty()) { Toast(ctx, T("Empty playlist name")); ctx.pl_pending_valid = false; return; }
    int idx = Library::Instance().CreatePlaylist(name);
    if (ctx.pl_pending_valid) Library::Instance().AddToPlaylist(idx, ctx.pl_pending_song);
    Toast(ctx, ctx.pl_pending_valid ? T("Playlist created, track added") : T("Playlist created: ") + name);
    ctx.pl_pending_valid = false;
    RefreshPlaylists(ctx);
}

static void OpenPicker(AppContext& ctx, const Song& s) {
    if (s.id.empty()) return;
    RefreshPlaylists(ctx);
    ctx.picker_open = true;
    ctx.picker_song = s;
    ctx.picker_sel = 0;
    ctx.picker_scroll = 0;
}

// a = -1: new playlist (keyboard); otherwise the playlist index
static void PickerChoose(AppContext& ctx, int a) {
    ctx.picker_open = false;
    if (a < 0) {
        ctx.pl_pending_song = ctx.picker_song;
        ctx.pl_pending_valid = true;
        OpenWiiUKeyboard(ctx, true);
    } else if (a < (int)ctx.playlists.size()) {
        bool added = Library::Instance().AddToPlaylist(a, ctx.picker_song);
        Toast(ctx, added ? T("Added to ") + ctx.playlists[a].name : T("Already in ") + ctx.playlists[a].name);
        RefreshPlaylists(ctx);
    }
}

static void GoTo(AppContext& ctx, AppState st) {
    if (st == STATE_PLAYLISTS) RefreshPlaylists(ctx);
    if ((st == STATE_PLAYER || st == STATE_QUEUE) && ctx.state != STATE_PLAYER && ctx.state != STATE_QUEUE) ctx.previous_state = ctx.state;
    ctx.state = st;
    ctx.rail_focused = false;
}

static void TogglePlay(AppContext& ctx) {
    if (AudioPlayer::Instance().GetState() == PlaybackState::STOPPED) PlaySong(ctx, ctx.current_song);
    else AudioPlayer::Instance().TogglePlayPause();
}

static void LoadPlaylist(AppContext& ctx, const std::string& plId) {
    RunAsync("PlaylistLoad", [&ctx, plId]() {
        std::vector<Song> songs = AccountManager::Instance().LoadPlaylistSongs(plId);
        if (!songs.empty()) {
            std::lock_guard<std::mutex> lock(ctx.data_mutex);
            ctx.pending_catalog = std::move(songs);
            ctx.has_pending_catalog = true;
        }
    });
}

static void CycleRepeat(AppContext& ctx) {
    ctx.repeat_mode = (ctx.repeat_mode == RepeatMode::OFF) ? RepeatMode::ALL :
                      (ctx.repeat_mode == RepeatMode::ALL ? RepeatMode::ONE : RepeatMode::OFF);
}

static void OpenMenu(AppContext& ctx, const Song& s) {
    if (s.id.empty()) return;
    ctx.menu_open = true;
    ctx.menu_song = s;
    ctx.menu_sel = 0;
}

static void SetScreenOff(AppContext& ctx, bool off) {
    ctx.screen_off = off;
#if defined(__WIIU__)
    VPADSetLcdMode(VPAD_CHAN_0, off ? VPAD_LCD_OFF : VPAD_LCD_ON);
#endif
}

static void MenuAct(AppContext& ctx, Act a) {
    Song s = ctx.menu_song;
    ctx.menu_open = false;
    switch (a) {
        case Act::MENU_PLAY: {
            if (ctx.queue.empty()) { ctx.queue.push_back(s); PlayQueueIdx(ctx, 0); }
            else { int pos = QueueInsertNext(ctx, s); PlayQueueIdx(ctx, pos); }
            break;
        }
        case Act::MENU_NEXT:
            if (ctx.current_song.id.empty()) { ctx.queue = { s }; PlayQueueIdx(ctx, 0); }
            else { QueueInsertNext(ctx, s); Toast(ctx, T("Will play next")); }
            break;
        case Act::MENU_QUEUE:
            if (ctx.current_song.id.empty()) { ctx.queue = { s }; PlayQueueIdx(ctx, 0); }
            else { QueueEnsureCurrent(ctx); ctx.queue.push_back(s); Toast(ctx, T("Added to queue (") + std::to_string(ctx.queue.size()) + ")"); }
            break;
        case Act::MENU_FAV:
            ToggleFavorite(ctx, s);
            Toast(ctx, FavoritesManager::Instance().IsFavorite(s.id) ? T("Added to favorites") : T("Removed from favorites"));
            break;
        case Act::MENU_CHANNEL: OpenArtistOfSong(ctx, s); break;
        case Act::MENU_PLAYLIST: OpenPicker(ctx, s); break;
        case Act::MENU_RADIO:
            ctx.queue = { s };
            PlayQueueIdx(ctx, 0);
            Toast(ctx, T("Preparing radio..."));
            StartRadio(ctx, s, false);
            break;
        default: break;
    }
}

static const char* kRegions[] = { "FR", "US", "GB", "CA", "DE", "ES", "IT", "BR", "JP" };

static void QueueEnsureRowVisible(AppContext& ctx) {
    int top = ctx.queue_sel * L::QUEUE_ROW, visible = ctx.content_bottom - L::QUEUE_TOP;
    if (top < ctx.queue_scroll_y) ctx.queue_scroll_y = top;
    else if (top + 80 > ctx.queue_scroll_y + visible) ctx.queue_scroll_y = top + 80 - visible;
    ctx.queue_scroll_y = std::max(0, std::min(ctx.queue_scroll_y, QueueMaxScrollY(ctx)));
}

static void Trigger(AppContext& ctx, Act act, int a, int b, int tx = 0, const SDL_Rect* rect = nullptr) {
    switch (act) {
        case Act::NAV:      GoTo(ctx, (AppState)a); break;
        case Act::KEYBOARD: ctx.state = STATE_SEARCH; OpenWiiUKeyboard(ctx); break;
        case Act::CHIP:     SelectChip(ctx, a); break;
        case Act::PLAY_SONG: PlayFromList(ctx, a, b); break;
        case Act::OPEN_ARTIST: {
            const std::vector<Song>* l = GetList(ctx, a);
            if (l && b >= 0 && b < (int)l->size()) OpenArtistOfSong(ctx, (*l)[b]);
            break;
        }
        case Act::OPEN_ARTIST_CUR: OpenArtistOfSong(ctx, ctx.current_song); break;
        case Act::BACK:
            if (ctx.state == STATE_PLAYLIST_VIEW) { ctx.state = STATE_PLAYLISTS; break; }
            ctx.state = (ctx.previous_state != STATE_ARTIST && ctx.previous_state != STATE_QUEUE && ctx.previous_state != STATE_PLAYER)
                            ? ctx.previous_state : STATE_CATALOG;
            break;
        case Act::DOCK_OPEN: GoTo(ctx, STATE_PLAYER); break;
        case Act::PLAY_PAUSE: TogglePlay(ctx); break;
        case Act::SEEK_REL: {
            int cur = AudioPlayer::Instance().GetCurrentPosition();
            AudioPlayer::Instance().Seek(std::max(0, cur + a));
            break;
        }
        case Act::SEEK_BAR: case Act::VOLUME_BAR: {
            if (!rect) break;
            int x0 = rect->x + 16, w = rect->w - 32;
            if (w <= 0) break;
            float ratio = std::min(1.0f, std::max(0.0f, (float)(tx - x0) / (float)w));
            if (act == Act::VOLUME_BAR) {
                AudioPlayer::Instance().SetVolume((int)(ratio * 100.0f + 0.5f));
            } else {
                int dur = AudioPlayer::Instance().GetDuration();
                if (dur > 0) AudioPlayer::Instance().Seek((int)(ratio * dur));
            }
            break;
        }
        case Act::PREV_TRACK: PrevTrack(ctx); break;
        case Act::NEXT_TRACK: NextTrack(ctx, true); break;
        case Act::SHUFFLE: ctx.shuffle_enabled = !ctx.shuffle_enabled; break;
        case Act::REPEAT:  CycleRepeat(ctx); break;
        case Act::LYRICS:  ctx.show_lyrics = !ctx.show_lyrics; break;
        case Act::FAVORITE: ToggleFavorite(ctx, ctx.current_song); break;
        case Act::LASTFM:
            if (!LastFm::HasSession()) { Toast(ctx, T("Last.fm not connected: see Settings")); }
            else if (!ctx.current_song.title.empty()) {
                Song s = ctx.current_song;
                RunAsync("ScrobbleBtn", [&ctx, s]() {
                    bool ok = LastFm::Scrobble(s.artist, s.title);
                    PostToast(ctx, ok ? T("Scrobble sent") : T("Scrobble failed"));
                });
            }
            break;
        case Act::ARTIST_SHUFFLE:
            if (!ctx.artist_profile.topSongs.empty()) {
                ctx.shuffle_enabled = true;
                PlayFromList(ctx, LIST_ARTIST, rand() % ctx.artist_profile.topSongs.size());
            }
            break;
        case Act::ACC_NEW_CODE: AccountManager::Instance().StartDeviceLogin(); break;
        case Act::ACC_CANCEL:
            AccountManager::Instance().CancelDeviceLogin();
            ctx.state = STATE_CATALOG;
            break;
        case Act::ACC_LOGOUT:
            AccountManager::Instance().Logout();
            ctx.liked.clear();
            ctx.liked_requested = false;
            break;
        case Act::ACC_REFRESH: AccountManager::Instance().LoadUserPlaylists(); break;
        case Act::ACC_PLAYLIST: {
            UserAccount acc = AccountManager::Instance().GetAccount();
            if (a >= 0 && a < (int)acc.playlists.size()) {
                ctx.account_selected_playlist_idx = a;
                LoadPlaylist(ctx, acc.playlists[a].id);
            }
            break;
        }
        case Act::OPEN_QUEUE:
            GoTo(ctx, STATE_QUEUE);
            ctx.queue_sel = std::max(0, ctx.queue_idx);
            ctx.content_bottom = L::DOCK_Y;
            QueueEnsureRowVisible(ctx);
            break;
        case Act::SCREEN_OFF: SetScreenOff(ctx, true); break;
        case Act::QUEUE_REMOVE:
            if (a >= 0 && a < (int)ctx.queue.size()) {
                ctx.queue.erase(ctx.queue.begin() + a);
                if (a <= ctx.queue_idx) ctx.queue_idx--;   // the next track takes the place of the removed one
                ctx.queue_sel = std::min(ctx.queue_sel, std::max(0, (int)ctx.queue.size() - 1));
            }
            break;
        case Act::QUEUE_UP:
            if (a > 0 && a < (int)ctx.queue.size()) {
                std::swap(ctx.queue[a], ctx.queue[a - 1]);
                if (ctx.queue_idx == a) ctx.queue_idx = a - 1;
                else if (ctx.queue_idx == a - 1) ctx.queue_idx = a;
                ctx.queue_sel = a - 1;
            }
            break;
        case Act::QUEUE_CLEAR:
            ctx.queue.clear();
            ctx.queue_idx = -1;
            ctx.queue_sel = 0;
            ctx.queue_scroll_y = 0;
            Toast(ctx, T("Queue cleared"));
            break;
        case Act::HISTORY_SEARCH:
            if (a >= 0 && a < (int)ctx.search_history.size()) SubmitSearch(ctx, ctx.search_history[a]);
            break;
        case Act::CLEAR_SEARCHES: case Act::CLEAR_SEARCH_HISTORY:
            Library::Instance().ClearSearches();
            ctx.search_history.clear();
            Toast(ctx, T("Searches cleared"));
            break;
        case Act::CLEAR_HISTORY:
            Library::Instance().ClearHistory();
            ctx.history.clear();
            ctx.hist_sel = ctx.hist_scroll = 0;
            ctx.home_row = 0;
            Toast(ctx, T("History cleared"));
            break;
        case Act::LANG_SET:
            g_lang = std::max(0, std::min(a, (int)LANG_COUNT - 1));
            ctx.settings.language = LangCode(g_lang);
            Library::Instance().SetSettings(ctx.settings);
            if (ctx.state == STATE_LANGUAGE) ctx.state = STATE_CATALOG;
            break;
        case Act::SET_LANGUAGE:
            g_lang = (g_lang + 1) % LANG_COUNT;
            ctx.settings.language = LangCode(g_lang);
            Library::Instance().SetSettings(ctx.settings);
            break;
        case Act::SET_REGION: {
            int n = (int)(sizeof(kRegions) / sizeof(kRegions[0])), i = 0;
            for (int k = 0; k < n; k++) if (ctx.settings.region == kRegions[k]) i = k;
            ctx.settings.region = kRegions[(i + 1) % n];
            Library::Instance().SetSettings(ctx.settings);
            NetworkManager::SetRegion(ctx.settings.region);
    g_lang = LangFromCode(ctx.settings.language);
    if (ctx.settings.language.empty()) { ctx.state = STATE_LANGUAGE; ctx.focus_visible = false; }   // first launch
            LoadInitialTrending(ctx);
            Toast(ctx, T("Region: ") + ctx.settings.region);
            break;
        }
        case Act::SET_AUTOPLAY:
            ctx.settings.autoplay = !ctx.settings.autoplay;
            Library::Instance().SetSettings(ctx.settings);
            break;
        case Act::SET_SEARCHMODE:
            ctx.settings.search_mode = ctx.settings.search_mode == "songs" ? "videos" : ctx.settings.search_mode == "videos" ? "all" : "songs";
            Library::Instance().SetSettings(ctx.settings);
            break;
        case Act::SET_SLEEP: {
            static const int steps[] = { 0, 15, 30, 60, 90 };
            int i = 0;
            for (int k = 0; k < 5; k++) if (ctx.settings.sleep_min == steps[k]) i = k;
            ctx.settings.sleep_min = steps[(i + 1) % 5];
            ctx.sleep_deadline_ms = ctx.settings.sleep_min ? SDL_GetTicks() + (uint32_t)ctx.settings.sleep_min * 60000u : 0;
            break;
        }
        case Act::LFM_ACTION:
            RunAsync("LastFmAuth", [&ctx]() {
                if (!LastFm::HasKeys()) { PostToast(ctx, T("Add api_key and api_secret to lastfm.json")); return; }
                if (LastFm::HasSession() && LastFm::AuthUrl().empty()) { PostToast(ctx, T("Last.fm already connected")); return; }
                if (LastFm::AuthUrl().empty()) {
                    PostToast(ctx, LastFm::BeginAuth() ? T("Scan the QR code, then confirm") : T("Last.fm: request failed"));
                } else {
                    PostToast(ctx, LastFm::FinishAuth() ? T("Last.fm connected!") : T("Not yet authorized on Last.fm"));
                }
            });
            break;
        case Act::MENU_CLOSE: ctx.menu_open = false; break;
        case Act::MENU_PLAY: case Act::MENU_NEXT: case Act::MENU_QUEUE: case Act::MENU_FAV:
        case Act::MENU_CHANNEL: case Act::MENU_RADIO: case Act::MENU_PLAYLIST:
            MenuAct(ctx, act);
            break;
        case Act::PL_NEW:
            ctx.pl_pending_valid = false;
            OpenWiiUKeyboard(ctx, true);
            break;
        case Act::PL_OPEN:
            if (a >= 0 && a < (int)ctx.playlists.size()) {
                ctx.pl_open = a;
                ctx.pl_songs = ctx.playlists[a].songs;
                ctx.pl_view_sel = 0;
                ctx.pl_view_scroll_y = 0;
                ctx.state = STATE_PLAYLIST_VIEW;
            }
            break;
        case Act::PL_PLAY:
            if (!ctx.pl_songs.empty()) { ctx.shuffle_enabled = false; PlayFromList(ctx, LIST_PLAYLIST, 0); }
            break;
        case Act::PL_SHUFFLE:
            if (!ctx.pl_songs.empty()) { ctx.shuffle_enabled = true; PlayFromList(ctx, LIST_PLAYLIST, rand() % ctx.pl_songs.size()); }
            break;
        case Act::PL_DELETE:
            if (ctx.pl_open >= 0) {
                Toast(ctx, T("Playlist deleted: ") + ctx.playlists[ctx.pl_open].name);
                Library::Instance().DeletePlaylist(ctx.pl_open);
                ctx.pl_open = -1;
                RefreshPlaylists(ctx);
                ctx.state = STATE_PLAYLISTS;
            }
            break;
        case Act::PL_REMOVE:
            if (ctx.pl_open >= 0) {
                Library::Instance().RemoveFromPlaylist(ctx.pl_open, a);
                RefreshPlaylists(ctx);
            }
            break;
        case Act::PLAYER_PL_ADD: OpenPicker(ctx, ctx.current_song); break;
        case Act::PICK_PLAYLIST: PickerChoose(ctx, a); break;
        case Act::PICKER_CLOSE: ctx.picker_open = false; break;
        case Act::NOP: case Act::NONE: break;
    }
}

// ------------------------------------------------------------------ touch

// Current finger state (regions are copied because ctx.hits is rebuilt every frame)
struct TouchState {
    bool active = false, dragging = false, lp_fired = false;
    int sx = 0, sy = 0, lx = 0, ly = 0;
    int axis = 0;                 // 1 = horizontal, 2 = vertical
    uint32_t t0 = 0;
    float vel = 0.0f;
    bool has_act = false;
    HitRegion act_region;
    int* hscroll = nullptr; int hmax = 0;
    int* vscroll = nullptr; int vmax = 0;
};

// Inertia after a swipe
struct Fling {
    int* p = nullptr;
    int max = 0;
    float v = 0.0f;
};

static bool Inside(const SDL_Rect& r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void WakeScreen(AppContext& ctx) { if (ctx.screen_off) SetScreenOff(ctx, false); }

static void TouchDown(AppContext& ctx, TouchState& t, Fling& fl, int x, int y) {
    fl.p = nullptr;
    if (ctx.screen_off) { WakeScreen(ctx); t = TouchState(); return; }
    t = TouchState();
    t.active = true;
    t.sx = t.lx = x;
    t.sy = t.ly = y;
    t.t0 = SDL_GetTicks();
    ctx.focus_visible = false;

    // Scrolling-band regions only respond inside the band (not under the top bar / dock)
    int bandTop = ctx.state == STATE_QUEUE ? L::QUEUE_TOP - 6 : ctx.state == STATE_PLAYLIST_VIEW ? L::PL_ROWS_TOP - 6 : L::CONTENT_TOP;
    auto allowed = [&](const HitRegion& h) { return !h.content || (y >= bandTop && y < ctx.content_bottom); };

    for (auto it = ctx.hits.rbegin(); it != ctx.hits.rend(); ++it) {
        if (it->act != Act::NONE && Inside(it->r, x, y) && allowed(*it)) { t.has_act = true; t.act_region = *it; break; }
    }
    for (auto it = ctx.hits.rbegin(); it != ctx.hits.rend(); ++it) {
        if (it->scroll && Inside(it->r, x, y) && allowed(*it)) {
            if (it->vert && !t.vscroll) { t.vscroll = it->scroll; t.vmax = it->scroll_max; }
            else if (!it->vert && !t.hscroll) { t.hscroll = it->scroll; t.hmax = it->scroll_max; }
        }
    }
    if (t.has_act && t.act_region.act != Act::SEEK_BAR && t.act_region.act != Act::VOLUME_BAR &&
        t.act_region.act != Act::MENU_CLOSE && t.act_region.act != Act::NOP) {
        ctx.press_valid = true;
        ctx.press_rect = t.act_region.r;
    }
    if (t.has_act && t.act_region.act == Act::VOLUME_BAR) Trigger(ctx, Act::VOLUME_BAR, 0, 0, x, &t.act_region.r);
}

static void TouchMove(AppContext& ctx, TouchState& t, int x, int y) {
    if (!t.active) return;
    int tdx = x - t.sx, tdy = y - t.sy;
    if (!t.dragging && std::abs(tdx) + std::abs(tdy) > 16) {
        t.dragging = true;
        t.axis = std::abs(tdx) >= std::abs(tdy) ? 1 : 2;
        ctx.press_valid = false;
    }
    if (t.has_act && t.act_region.act == Act::VOLUME_BAR) Trigger(ctx, Act::VOLUME_BAR, 0, 0, x, &t.act_region.r);
    if (t.dragging) {
        int* p = t.axis == 1 ? t.hscroll : t.vscroll;
        int mx = t.axis == 1 ? t.hmax : t.vmax;
        int d = t.axis == 1 ? (x - t.lx) : (y - t.ly);
        if (p) {
            *p = std::max(0, std::min(*p - d, mx));
            t.vel = 0.6f * t.vel + 0.4f * (float)(-d);
        }
    }
    t.lx = x;
    t.ly = y;
}

static void TouchUp(AppContext& ctx, TouchState& t, Fling& fl, int x, int y) {
    if (!t.active) return;
    t.lx = x;
    t.ly = y;
    ctx.press_valid = false;
    bool fired = t.lp_fired;
    bool seekLike = t.has_act && (t.act_region.act == Act::SEEK_BAR);
    if (!fired && t.has_act && (!t.dragging || seekLike) && t.act_region.act != Act::VOLUME_BAR) {
        Trigger(ctx, t.act_region.act, t.act_region.a, t.act_region.b, x, &t.act_region.r);
    } else if (t.dragging && !fired) {
        // Swipe down from the player = close it
        if (ctx.state == STATE_PLAYER && t.axis == 2 && (y - t.sy) > 140) {
            ctx.state = ctx.previous_state != STATE_PLAYER ? ctx.previous_state : STATE_CATALOG;
        } else {
            int* p = t.axis == 1 ? t.hscroll : t.vscroll;
            if (p && std::fabs(t.vel) > 4.0f) { fl.p = p; fl.max = t.axis == 1 ? t.hmax : t.vmax; fl.v = t.vel; }
        }
    }
    t = TouchState();
}

// ------------------------------------------------------------------ gamepad navigation

struct Pad {
    bool up = false, down = false, left = false, right = false;
    bool a = false, b = false, x = false, y = false;
    bool l = false, r = false, zl = false, zr = false, plus = false, minus = false;
    bool any() const { return up || down || left || right || a || b || x || y || l || r || zl || zr || plus || minus; }
};

static const AppState kRailStates[L::RAIL_ITEMS] = { STATE_CATALOG, STATE_SEARCH, STATE_PLAYER, STATE_PLAYLISTS, STATE_ACCOUNT, STATE_SETTINGS };

static void FocusRail(AppContext& ctx) {
    ctx.rail_focused = true;
    switch (ctx.state) {
        case STATE_SEARCH:   ctx.rail_selected_idx = 1; break;
        case STATE_PLAYER: case STATE_QUEUE: ctx.rail_selected_idx = 2; break;
        case STATE_PLAYLISTS: case STATE_PLAYLIST_VIEW: ctx.rail_selected_idx = 3; break;
        case STATE_ACCOUNT:  ctx.rail_selected_idx = 4; break;
        case STATE_SETTINGS: ctx.rail_selected_idx = 5; break;
        default:             ctx.rail_selected_idx = 0; break;
    }
}

// Move in a grid with `rows` rows. Returns false when leaving through the left edge.
static bool NavGrid(int& sel, int& scroll, int count, int rows, const Pad& p) {
    if (count <= 0) return true;
    int cols = (count + rows - 1) / rows;
    int n = sel;
    if (p.right && sel + rows < count) n = sel + rows;
    else if (p.left) {
        if (sel - rows < 0) return false;
        n = sel - rows;
    } else if (p.down && rows > 1 && sel % rows < rows - 1 && sel + 1 < count) n = sel + 1;
    else if (p.up && rows > 1 && sel % rows > 0) n = sel - 1;
    sel = n;
    EnsureColVisible(scroll, sel / rows, cols);
    return true;
}

static void VolumeKeys(const Pad& p) {
    if (p.r) AudioPlayer::Instance().SetVolume(std::min(100, AudioPlayer::Instance().GetVolume() + 5));
    if (p.l) AudioPlayer::Instance().SetVolume(std::max(0, AudioPlayer::Instance().GetVolume() - 5));
}

static const Act kMenuActs[7] = { Act::MENU_PLAY, Act::MENU_NEXT, Act::MENU_QUEUE, Act::MENU_FAV, Act::MENU_CHANNEL, Act::MENU_RADIO, Act::MENU_PLAYLIST };
static const Act kSettingsActs[9] = { Act::SET_LANGUAGE, Act::SET_REGION, Act::SET_SEARCHMODE, Act::SET_AUTOPLAY, Act::SET_SLEEP,
                                      Act::SCREEN_OFF, Act::LFM_ACTION, Act::CLEAR_HISTORY, Act::CLEAR_SEARCH_HISTORY };

// Song under the gamepad cursor (for the context menu)
static bool FocusedSong(AppContext& ctx, Song& out) {
    switch (ctx.state) {
        case STATE_CATALOG: {
            HomeSec secs[6];
            int n = BuildHomeSections(ctx, secs);
            if (ctx.home_row < n && *secs[ctx.home_row].sel < (int)secs[ctx.home_row].list->size()) {
                out = (*secs[ctx.home_row].list)[*secs[ctx.home_row].sel];
                return true;
            }
            return false;
        }
        case STATE_SEARCH:
            if (ctx.search_selected_idx < (int)ctx.search_results.size()) { out = ctx.search_results[ctx.search_selected_idx]; return true; }
            return false;
        case STATE_ARTIST:
            if (ctx.artist_profile.selected_idx < (int)ctx.artist_profile.topSongs.size()) { out = ctx.artist_profile.topSongs[ctx.artist_profile.selected_idx]; return true; }
            return false;
        case STATE_PLAYER: out = ctx.current_song; return !out.id.empty();
        case STATE_QUEUE:
            if (ctx.queue_sel < (int)ctx.queue.size()) { out = ctx.queue[ctx.queue_sel]; return true; }
            return false;
        case STATE_PLAYLIST_VIEW:
            if (ctx.pl_view_sel < (int)ctx.pl_songs.size()) { out = ctx.pl_songs[ctx.pl_view_sel]; return true; }
            return false;
        default: return false;
    }
}

static void HandlePad(AppContext& ctx, const Pad& p) {
    if (ctx.screen_off) { if (p.any()) WakeScreen(ctx); return; }
    if (p.any()) ctx.focus_visible = true;

    // Language choice (first launch): up/down + A
    if (ctx.state == STATE_LANGUAGE) {
        if (p.up) ctx.lang_sel = (ctx.lang_sel + LANG_COUNT - 1) % LANG_COUNT;
        else if (p.down) ctx.lang_sel = (ctx.lang_sel + 1) % LANG_COUNT;
        else if (p.a) Trigger(ctx, Act::LANG_SET, ctx.lang_sel, 0);
        return;
    }

    // Playlist picker: captures all keys
    if (ctx.picker_open) {
        int n = 1 + (int)ctx.playlists.size(), visible = std::min(n, 6);
        if (p.up) ctx.picker_sel = (ctx.picker_sel + n - 1) % n;
        else if (p.down) ctx.picker_sel = (ctx.picker_sel + 1) % n;
        else if (p.a) { PickerChoose(ctx, ctx.picker_sel - 1); return; }
        else if (p.b) { ctx.picker_open = false; return; }
        if (ctx.picker_sel * 64 < ctx.picker_scroll) ctx.picker_scroll = ctx.picker_sel * 64;
        else if ((ctx.picker_sel + 1) * 64 > ctx.picker_scroll + visible * 64) ctx.picker_scroll = (ctx.picker_sel + 1) * 64 - visible * 64;
        return;
    }

    // Context menu: captures all keys
    if (ctx.menu_open) {
        if (p.up) ctx.menu_sel = (ctx.menu_sel + 6) % 7;
        else if (p.down) ctx.menu_sel = (ctx.menu_sel + 1) % 7;
        else if (p.a) MenuAct(ctx, kMenuActs[ctx.menu_sel]);
        else if (p.b || p.minus) ctx.menu_open = false;
        return;
    }

    if (p.plus) TogglePlay(ctx);
    if (p.zl) AudioPlayer::Instance().Seek(std::max(0, AudioPlayer::Instance().GetCurrentPosition() - 10));
    if (p.zr) AudioPlayer::Instance().Seek(AudioPlayer::Instance().GetCurrentPosition() + 10);

    if (ctx.rail_focused) {
        if (p.up) ctx.rail_selected_idx = (ctx.rail_selected_idx + L::RAIL_ITEMS - 1) % L::RAIL_ITEMS;
        else if (p.down) ctx.rail_selected_idx = (ctx.rail_selected_idx + 1) % L::RAIL_ITEMS;
        else if (p.right || p.b) ctx.rail_focused = false;
        else if (p.a) GoTo(ctx, kRailStates[ctx.rail_selected_idx]);
        return;
    }

    Song focused;

    switch (ctx.state) {
    case STATE_CATALOG: {
        VolumeKeys(p);
        if (p.b) { FocusRail(ctx); break; }
        if (p.y) { ctx.state = STATE_SEARCH; OpenWiiUKeyboard(ctx); break; }
        HomeSec secs[6];
        int n = BuildHomeSections(ctx, secs);
        ctx.home_row = std::max(0, std::min(ctx.home_row, n - 1));
        if (p.down || p.up) {
            int dir = p.down ? 1 : -1;
            for (int i = ctx.home_row + dir; i >= 0 && i < n; i += dir) {
                if (!secs[i].list->empty()) { ctx.home_row = i; EnsureSectionVisible(ctx, i); break; }
            }
            break;
        }
        HomeSec& s = secs[ctx.home_row];
        if (!NavGrid(*s.sel, *s.scroll, (int)s.list->size(), 1, p)) FocusRail(ctx);
        if (*s.sel >= 0 && *s.sel < (int)s.list->size()) {
            if (p.a) PlayFromList(ctx, s.listId, *s.sel);
            else if (p.x) OpenArtistOfSong(ctx, (*s.list)[*s.sel]);
            else if (p.minus && FocusedSong(ctx, focused)) OpenMenu(ctx, focused);
        }
        break;
    }
    case STATE_SEARCH: {
        VolumeKeys(p);
        if (p.b) { FocusRail(ctx); break; }
        if (p.y) { OpenWiiUKeyboard(ctx); break; }
        if (ctx.search_results.empty()) { if (p.a) OpenWiiUKeyboard(ctx); if (p.left) FocusRail(ctx); break; }
        if (!NavGrid(ctx.search_selected_idx, ctx.search_scroll_x, (int)ctx.search_results.size(), 2, p)) FocusRail(ctx);
        if (ctx.search_selected_idx >= 0 && ctx.search_selected_idx < (int)ctx.search_results.size()) {
            if (p.a) PlayFromList(ctx, LIST_SEARCH, ctx.search_selected_idx);
            else if (p.x) OpenArtistOfSong(ctx, ctx.search_results[ctx.search_selected_idx]);
            else if (p.minus && FocusedSong(ctx, focused)) OpenMenu(ctx, focused);
        }
        break;
    }
    case STATE_PLAYER: {
        if (p.b) ctx.state = (ctx.previous_state != STATE_PLAYER) ? ctx.previous_state : STATE_CATALOG;
        else if (p.x) OpenArtistOfSong(ctx, ctx.current_song);
        else if (p.y) ToggleFavorite(ctx, ctx.current_song);
        else if (p.a) AudioPlayer::Instance().TogglePlayPause();
        else if (p.minus) ctx.show_lyrics = !ctx.show_lyrics;
        else if (p.l) PrevTrack(ctx);
        else if (p.r) NextTrack(ctx, true);
        else if (p.left) AudioPlayer::Instance().Seek(std::max(0, AudioPlayer::Instance().GetCurrentPosition() - 10));
        else if (p.right) AudioPlayer::Instance().Seek(AudioPlayer::Instance().GetCurrentPosition() + 10);
        else if (p.up) AudioPlayer::Instance().SetVolume(std::min(100, AudioPlayer::Instance().GetVolume() + 5));
        else if (p.down) AudioPlayer::Instance().SetVolume(std::max(0, AudioPlayer::Instance().GetVolume() - 5));
        break;
    }
    case STATE_ARTIST: {
        VolumeKeys(p);
        ArtistProfile& ap = ctx.artist_profile;
        if (p.b) { Trigger(ctx, Act::BACK, 0, 0); break; }
        if (p.x) { ctx.state = STATE_SEARCH; ctx.search_query = ap.name; TriggerSearch(ctx, ap.name); break; }
        if (!ap.topSongs.empty()) {
            if (!NavGrid(ap.selected_idx, ap.scroll_x, (int)ap.topSongs.size(), 1, p)) FocusRail(ctx);
            if (ap.selected_idx >= 0 && ap.selected_idx < (int)ap.topSongs.size()) {
                if (p.a) PlayFromList(ctx, LIST_ARTIST, ap.selected_idx);
                else if (p.y) ToggleFavorite(ctx, ap.topSongs[ap.selected_idx]);
                else if (p.minus && FocusedSong(ctx, focused)) OpenMenu(ctx, focused);
            }
        }
        break;
    }
    case STATE_QUEUE: {
        int n = (int)ctx.queue.size();
        if (p.b) { Trigger(ctx, Act::BACK, 0, 0); break; }
        if (p.up && ctx.queue_sel > 0) ctx.queue_sel--;
        else if (p.down && ctx.queue_sel < n - 1) ctx.queue_sel++;
        else if (p.a && n > 0) PlayQueueIdx(ctx, ctx.queue_sel);
        else if (p.x && n > 0) Trigger(ctx, Act::QUEUE_REMOVE, ctx.queue_sel, 0);
        else if (p.y && n > 0) Trigger(ctx, Act::QUEUE_UP, ctx.queue_sel, 0);
        else if (p.minus && FocusedSong(ctx, focused)) OpenMenu(ctx, focused);
        else if (p.left) FocusRail(ctx);
        QueueEnsureRowVisible(ctx);
        break;
    }
    case STATE_PLAYLISTS: {
        int n = (int)ctx.playlists.size();
        if (p.b) { ctx.state = STATE_CATALOG; break; }
        if (p.y) { Trigger(ctx, Act::PL_NEW, 0, 0); break; }
        if (n == 0) { if (p.left) FocusRail(ctx); break; }
        if (!NavGrid(ctx.pl_sel, ctx.pl_scroll_x, n, 2, p)) FocusRail(ctx);
        if (p.a) Trigger(ctx, Act::PL_OPEN, ctx.pl_sel, 0);
        break;
    }
    case STATE_PLAYLIST_VIEW: {
        int n = (int)ctx.pl_songs.size();
        if (p.b) { Trigger(ctx, Act::BACK, 0, 0); break; }
        if (p.up && ctx.pl_view_sel > 0) ctx.pl_view_sel--;
        else if (p.down && ctx.pl_view_sel < n - 1) ctx.pl_view_sel++;
        else if (p.a && n > 0) PlayFromList(ctx, LIST_PLAYLIST, ctx.pl_view_sel);
        else if (p.x && n > 0) Trigger(ctx, Act::PL_REMOVE, ctx.pl_view_sel, 0);
        else if (p.y) Trigger(ctx, Act::PL_PLAY, 0, 0);
        else if (p.minus && FocusedSong(ctx, focused)) OpenMenu(ctx, focused);
        else if (p.left) FocusRail(ctx);
        {
            int top = ctx.pl_view_sel * L::QUEUE_ROW, visible = ctx.content_bottom - (L::PL_ROWS_TOP - 6);
            if (top < ctx.pl_view_scroll_y) ctx.pl_view_scroll_y = top;
            else if (top + 80 > ctx.pl_view_scroll_y + visible) ctx.pl_view_scroll_y = top + 80 - visible;
            ctx.pl_view_scroll_y = std::max(0, std::min(ctx.pl_view_scroll_y, PlaylistViewMaxScrollY(ctx)));
        }
        break;
    }
    case STATE_SETTINGS: {
        if (p.b) { ctx.state = STATE_CATALOG; break; }
        if (p.up) ctx.settings_sel = (ctx.settings_sel + 8) % 9;
        else if (p.down) ctx.settings_sel = (ctx.settings_sel + 1) % 9;
        else if (p.a || p.right) Trigger(ctx, kSettingsActs[ctx.settings_sel], 0, 0);
        else if (p.left) FocusRail(ctx);
        break;
    }
    case STATE_ACCOUNT: {
        UserAccount acc = AccountManager::Instance().GetAccount();
        if (p.b) {
            if (!acc.is_logged_in) AccountManager::Instance().CancelDeviceLogin();
            ctx.state = STATE_CATALOG;
        } else if (!acc.is_logged_in) {
            if (p.a || p.x || p.y) AccountManager::Instance().StartDeviceLogin();
            if (p.left) FocusRail(ctx);
        } else {
            int n = (int)acc.playlists.size();
            if (p.y) AccountManager::Instance().LoadUserPlaylists();
            else if (p.x) Trigger(ctx, Act::ACC_LOGOUT, 0, 0);
            else if (p.a && n > 0 && ctx.account_selected_playlist_idx < n)
                LoadPlaylist(ctx, acc.playlists[ctx.account_selected_playlist_idx].id);
            else if (p.right && ctx.account_selected_playlist_idx < n - 1) {
                ctx.account_selected_playlist_idx++;
                int left = ctx.account_selected_playlist_idx * 220;
                if (left + 200 > ctx.account_scroll_x + L::VISIBLE_W) ctx.account_scroll_x = left + 200 - L::VISIBLE_W;
            } else if (p.left) {
                if (ctx.account_selected_playlist_idx > 0) {
                    ctx.account_selected_playlist_idx--;
                    int left = ctx.account_selected_playlist_idx * 220;
                    if (left < ctx.account_scroll_x) ctx.account_scroll_x = left;
                } else FocusRail(ctx);
            }
        }
        break;
    }
    }
}

// ------------------------------------------------------------------ main

int main(int argc, char* argv[]) {
    WHBProcInit();
    StorageInit();

    if (!NetworkManager::Initialize()) {
        std::cerr << "[MiiTune] Could not initialise networking." << std::endl;
        WHBProcShutdown();
        return -1;
    }

    Library::Instance().Init();
    LastFm::Init();
    AccountManager::Instance().Initialize();
    FavoritesManager::Instance().Initialize();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        std::cerr << "[MiiTune] SDL_Init error: " << SDL_GetError() << std::endl;
        NetworkManager::Cleanup();
        WHBProcShutdown();
        return -1;
    }

    if (TTF_Init() < 0) {
        std::cerr << "[MiiTune] TTF_Init error: " << TTF_GetError() << std::endl;
    }

    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP);
#if defined(__WIIU__)
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);   // start/end/cancel of Wii U keyboard input
#endif

    AppContext ctx;
    ctx.window = SDL_CreateWindow("MiiTune", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  L::W, L::H, SDL_WINDOW_SHOWN);
    if (!ctx.window) {
        std::cerr << "[MiiTune] SDL2 window creation error: " << SDL_GetError() << std::endl;
        NetworkManager::Cleanup();
        WHBProcShutdown();
        return -1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1"); // linear filtering: smooth shapes and posters
    ctx.renderer = SDL_CreateRenderer(ctx.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ctx.renderer) {
        ctx.renderer = SDL_CreateRenderer(ctx.window, -1, SDL_RENDERER_SOFTWARE);
    }

    SetScreenFormat(ctx, FORMAT_TV);
    IconsInit(ctx.renderer);   // Lucide icons (embedded atlas)

    auto LoadBestFont = [](int ptSize) -> TTF_Font* {
        const char* paths[] = {
            "content/font.ttf",
            "content/fonts/DejaVuSans.ttf",
            "romfs:/font.ttf",
            "romfs:/fonts/DejaVuSans.ttf",
            "/vol/content/font.ttf",
            "/vol/content/fonts/DejaVuSans.ttf",
            nullptr
        };
        for (int i = 0; paths[i]; ++i) {
            TTF_Font* f = TTF_OpenFont(paths[i], ptSize);
            if (f) return f;
        }
        // Font embedded in the binary: also works when launched as .rpx (Homebrew Launcher, no content folder)
        return TTF_OpenFontRW(SDL_RWFromConstMem(EMBEDDED_FONT_DATA, EMBEDDED_FONT_SIZE), 1, ptSize);
    };

    // Bigger fonts: the GamePad shows the 1280x720 frame scaled down to 854x480
    ctx.font_title  = LoadBestFont(30);
    ctx.font_normal = LoadBestFont(22);
    ctx.font_small  = LoadBestFont(18);

    const char* logo_paths[] = {
        "icon.png",
        "branding/iconTex.png",
        "content/images/icon.png",
        "romfs:/icon.png",
        "/vol/content/icon.png",
        nullptr
    };
    for (int i = 0; logo_paths[i]; ++i) {
        ctx.tex_logo = IMG_LoadTexture(ctx.renderer, logo_paths[i]);
        if (ctx.tex_logo) break;
    }
    if (!ctx.tex_logo) {
        SDL_Surface* surf = IMG_Load_RW(SDL_RWFromConstMem(EMBEDDED_LOGO_DATA, EMBEDDED_LOGO_SIZE), 1);
        if (surf) {
            ctx.tex_logo = SDL_CreateTextureFromSurface(ctx.renderer, surf);
            SDL_FreeSurface(surf);
        }
    }

    if (!AudioPlayer::Instance().Initialize()) {
        std::cerr << "[MiiTune] AudioPlayer initialisation error." << std::endl;
    }

    ctx.poster_mgr.init();
    VPADInit();

    ctx.settings = Library::Instance().GetSettings();
    g_debug_hud = !StorageRead("debug.txt").empty();
    NetworkManager::SetRegion(ctx.settings.region);
    ctx.favorites = FavoritesManager::Instance().GetFavorites();
    ctx.history = Library::Instance().History();
    ctx.playlists = Library::Instance().Playlists();
    ctx.search_history = Library::Instance().Searches();
    LoadInitialTrending(ctx);

    // Console Mii: loaded in the background, shown instead of the logo when available
    RunAsync("MiiLoad", [&ctx]() {
        SDL_Surface* s = LoadUserMii();
        if (!s) return;
        std::lock_guard<std::mutex> lock(ctx.data_mutex);
        if (ctx.pending_mii) SDL_FreeSurface(ctx.pending_mii);
        ctx.pending_mii = s;
    });

    bool running = true;
    TouchState touch;
    Fling fling;
    bool vpad_touch = false;
    uint32_t last_activity = SDL_GetTicks();   // last user action
    uint32_t last_render = 0;
    uint32_t kb_last_ms = 0;
    std::string kb_buffer;                    // Wii U keyboard text (SDL delivers it in chunks of <32 bytes)
    bool kb_collecting = false;                   // last time the Wii U keyboard was on screen

    // Three touch sources: VPAD (GamePad), SDL fingers, mouse. The first one to press "owns" the gesture
    // until release, which avoids double taps and still works if SDL consumes
    // the VPAD samples on its side.
    int touch_src = 0;
    uint32_t touch_last_ms = 0;
    int sdl_touch_events = 0;
    auto beginTouch = [&](int src, int x, int y) {
        if (touch_src != 0) return;
        touch_src = src; touch_last_ms = SDL_GetTicks();
        TouchDown(ctx, touch, fling, x, y);
    };
    auto moveTouch = [&](int src, int x, int y) {
        if (touch_src != src) return;
        touch_last_ms = SDL_GetTicks();
        TouchMove(ctx, touch, x, y);
    };
    auto endTouch = [&](int src, int x, int y) {
        if (touch_src != src) return;
        TouchUp(ctx, touch, fling, x, y);
        touch_src = 0;
    };

    while (running && WHBProcIsRunning()) {
        Pad pad;

        // 1. Gamepad (VPAD) + GamePad touch screen
        VPADStatus vpad;
        VPADReadError vpad_err;
        int read_count = VPADRead(VPAD_CHAN_0, &vpad, 1, &vpad_err);
        bool swkbd_consumed = false;
        VPADTouchData tp;
        tp.x = tp.y = 0; tp.touched = 0; tp.validity = 0;
        if (read_count > 0 && vpad_err == VPAD_READ_SUCCESS) {
            // Touch in 1280x720 for the UI, computed from the raw data
            VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_1280X720, &tp, &vpad.tpNormal);
#if defined(__WIIU__)
            // The Wii U keyboard wants an ALREADY calibrated point in tpNormal (otherwise it ignores touch)
            // The SDK reads touch from tpFiltered1 (and SDL mentions tpNormal): calibrate all three
            {
                VPADTouchData raw0 = vpad.tpNormal, raw1 = vpad.tpFiltered1, raw2 = vpad.tpFiltered2;
                VPADGetTPCalibratedPoint(VPAD_CHAN_0, &vpad.tpNormal, &raw0);
                VPADGetTPCalibratedPoint(VPAD_CHAN_0, &vpad.tpFiltered1, &raw1);
                VPADGetTPCalibratedPoint(VPAD_CHAN_0, &vpad.tpFiltered2, &raw2);
            }
            if (SDL_WiiUSetSWKBDVPAD(&vpad)) { swkbd_consumed = true; kb_last_ms = SDL_GetTicks(); }
#endif
            if (!swkbd_consumed) {
                uint32_t trg = vpad.trigger, hold = vpad.hold;

                // Repeat on the d-pad left/right
                static uint32_t s_last_rep = 0, s_hold_start = 0;
                uint32_t now = SDL_GetTicks();
                bool rep_l = false, rep_r = false;
                if (hold & (VPAD_BUTTON_LEFT | VPAD_BUTTON_RIGHT)) {
                    if (s_hold_start == 0) { s_hold_start = now; s_last_rep = now; }
                    else if (now - s_hold_start > 250 && now - s_last_rep > 90) {
                        s_last_rep = now;
                        if (hold & VPAD_BUTTON_LEFT) rep_l = true;
                        if (hold & VPAD_BUTTON_RIGHT) rep_r = true;
                    }
                } else {
                    s_hold_start = 0;
                }

                pad.minus = trg & VPAD_BUTTON_MINUS;
                pad.a = trg & VPAD_BUTTON_A;
                pad.b = trg & VPAD_BUTTON_B;
                pad.x = trg & VPAD_BUTTON_X;
                pad.y = trg & VPAD_BUTTON_Y;
                pad.plus = trg & VPAD_BUTTON_PLUS;
                pad.l = trg & VPAD_BUTTON_L;
                pad.r = trg & VPAD_BUTTON_R;
                pad.zl = trg & VPAD_BUTTON_ZL;
                pad.zr = trg & VPAD_BUTTON_ZR;
                pad.left = (trg & VPAD_BUTTON_LEFT) || rep_l;
                pad.right = (trg & VPAD_BUTTON_RIGHT) || rep_r;
                pad.up = trg & VPAD_BUTTON_UP;
                pad.down = trg & VPAD_BUTTON_DOWN;

                // Stick = scroll the row under the cursor
                float stickX = 0.0f;
                if (std::abs(vpad.leftStick.x) > 0.18f) stickX = vpad.leftStick.x;
                else if (std::abs(vpad.rightStick.x) > 0.18f) stickX = vpad.rightStick.x;
                if (std::abs(stickX) > 0.18f && !ctx.menu_open) {
                    int d = (int)(stickX * 20.0f);
                    int* s = nullptr;
                    int cols = 0, rows = 1;
                    if (ctx.state == STATE_SEARCH) { s = &ctx.search_scroll_x; cols = (int)ctx.search_results.size(); rows = 2; }
                    else if (ctx.state == STATE_CATALOG) {
                        HomeSec secs[6];
                        int n = BuildHomeSections(ctx, secs);
                        int row = std::max(0, std::min(ctx.home_row, n - 1));
                        s = secs[row].scroll; cols = (int)secs[row].list->size();
                    } else if (ctx.state == STATE_ARTIST) { s = &ctx.artist_profile.scroll_x; cols = (int)ctx.artist_profile.topSongs.size(); }
                    if (s) *s = std::max(0, std::min(*s + d, ShelfMaxScroll((cols + rows - 1) / rows)));
                    ctx.focus_visible = true;
                }

                // GamePad touch screen, read directly through VPAD (independent of SDL events)
                // (validity is not tested: some emulators fill it in wrongly)
                bool touching = tp.touched && tp.x < 1400 && tp.y < 800;
                if (touching && !vpad_touch) beginTouch(1, tp.x, tp.y);
                else if (touching) moveTouch(1, tp.x, tp.y);
                else if (!touching && vpad_touch) endTouch(1, touch.lx, touch.ly);
                vpad_touch = touching;
            }
        }

        // 2. SDL events (mouse/Cemu, Wii U keyboard text)
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_SYSWMEVENT) {
#if defined(__WIIU__)
                if (e.syswm.msg && e.syswm.msg->subsystem == SDL_SYSWM_WIIU) {
                    unsigned ev = e.syswm.msg->msg.wiiu.event;
                    if (ev == SDL_WIIU_SYSWM_SWKBD_OK_START_EVENT) {
                        kb_buffer.clear();
                        kb_collecting = true;
                    } else if (ev == SDL_WIIU_SYSWM_SWKBD_OK_FINISH_EVENT) {
                        kb_collecting = false;
                        std::string in = kb_buffer;
                        kb_buffer.clear();
                        SDL_StopTextInput();
                        kb_last_ms = 0;
                        if (!in.empty()) {
                            if (ctx.text_mode == 1) { CreatePlaylistNamed(ctx, in); ctx.text_mode = 0; }
                            else SubmitSearch(ctx, in);
                        }
                    } else if (ev == SDL_WIIU_SYSWM_SWKBD_CANCEL_EVENT) {
                        kb_collecting = false;
                        kb_buffer.clear();
                        SDL_StopTextInput();
                        kb_last_ms = 0;
                        ctx.text_mode = 0;
                        ctx.pl_pending_valid = false;
                    }
                }
#endif
            } else if (e.type == SDL_TEXTINPUT) {
                std::string in = e.text.text;
                if (kb_collecting) {
                    kb_buffer += in;     // chunk of a longer text: wait for the end (OK_FINISH)
                } else if (!in.empty()) {
                    if (ctx.text_mode == 1) { CreatePlaylistNamed(ctx, in); ctx.text_mode = 0; }
                    else SubmitSearch(ctx, in);
                    SDL_StopTextInput();
                }
            } else if (e.type == SDL_FINGERDOWN) {
                sdl_touch_events++;
                beginTouch(2, (int)(e.tfinger.x * L::W), (int)(e.tfinger.y * L::H));
            } else if (e.type == SDL_FINGERMOTION) {
                moveTouch(2, (int)(e.tfinger.x * L::W), (int)(e.tfinger.y * L::H));
            } else if (e.type == SDL_FINGERUP) {
                endTouch(2, (int)(e.tfinger.x * L::W), (int)(e.tfinger.y * L::H));
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.which != SDL_TOUCH_MOUSEID) {
                int ww = L::W, wh = L::H; SDL_GetWindowSize(ctx.window, &ww, &wh);
                sdl_touch_events++;
                beginTouch(3, e.button.x * L::W / std::max(1, ww), e.button.y * L::H / std::max(1, wh));
            } else if (e.type == SDL_MOUSEMOTION && e.motion.which != SDL_TOUCH_MOUSEID) {
                int ww = L::W, wh = L::H; SDL_GetWindowSize(ctx.window, &ww, &wh);
                moveTouch(3, e.motion.x * L::W / std::max(1, ww), e.motion.y * L::H / std::max(1, wh));
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.which != SDL_TOUCH_MOUSEID) {
                int ww = L::W, wh = L::H; SDL_GetWindowSize(ctx.window, &ww, &wh);
                endTouch(3, e.button.x * L::W / std::max(1, ww), e.button.y * L::H / std::max(1, wh));
            } else if (e.type == SDL_KEYDOWN) {
                if (SDL_IsTextInputActive()) {
                    if (e.key.keysym.sym == SDLK_BACKSPACE && !ctx.search_query.empty()) {
                        ctx.search_query.pop_back();
                    } else if (e.key.keysym.sym == SDLK_RETURN) {
                        SDL_StopTextInput();
                        SubmitSearch(ctx, ctx.search_query);
                    } else if (e.key.keysym.sym == SDLK_ESCAPE) {
                        SDL_StopTextInput();
                    }
                } else {
                    switch (e.key.keysym.sym) {
                        case SDLK_RETURN: case SDLK_SPACE: pad.a = true; break;
                        case SDLK_ESCAPE: case SDLK_BACKSPACE: pad.b = true; break;
                        case SDLK_x: pad.x = true; break;
                        case SDLK_y: pad.y = true; break;
                        case SDLK_p: pad.plus = true; break;
                        case SDLK_m: pad.minus = true; break;
                        case SDLK_LEFT: pad.left = true; break;
                        case SDLK_RIGHT: pad.right = true; break;
                        case SDLK_UP: pad.up = true; break;
                        case SDLK_DOWN: pad.down = true; break;
                        default: break;
                    }
                }
            }
        }

        // Lost gesture (release never received): free it so touch does not stay blocked
        if (touch_src != 0 && SDL_GetTicks() - touch_last_ms > (touch_src == 1 ? 700u : 4000u)) {
            TouchUp(ctx, touch, fling, touch.lx, touch.ly);
            touch_src = 0;
        }
        {
            static uint32_t s_dbg_t = 0;
            static int s_dbg_src = -1;
            if (SDL_GetTicks() - s_dbg_t > 250 || s_dbg_src != touch_src) {
                s_dbg_t = SDL_GetTicks();
                s_dbg_src = touch_src;
                snprintf(ctx.touch_dbg, sizeof(ctx.touch_dbg), "touch src%d  vpad t%d (%d,%d)  sdl%d  kb%d",
                         touch_src, (int)tp.touched, (int)tp.x, (int)tp.y, sdl_touch_events, (int)swkbd_consumed);
            }
        }

        // Long press on a card = context menu
        if (touch.active && !touch.dragging && !touch.lp_fired && touch.has_act && touch.act_region.act == Act::PLAY_SONG &&
            !ctx.menu_open && SDL_GetTicks() - touch.t0 > 550) {
            const std::vector<Song>* l = GetList(ctx, touch.act_region.a);
            if (l && touch.act_region.b >= 0 && touch.act_region.b < (int)l->size()) {
                OpenMenu(ctx, (*l)[touch.act_region.b]);
                touch.lp_fired = true;
                ctx.press_valid = false;
            }
        }

        // Scroll inertia
        if (fling.p && !touch.active) {
            int nv = *fling.p + (int)fling.v;
            *fling.p = std::max(0, std::min(nv, fling.max));
            fling.v *= 0.93f;
            if (std::fabs(fling.v) < 1.0f || nv < 0 || nv > fling.max) fling.p = nullptr;
        }

        HandlePad(ctx, pad);

        // Track loading state
        bool failed = false;
        {
            std::lock_guard<std::mutex> lock(ctx.data_mutex);
            if (ctx.pending_started) { ctx.play_started = true; ctx.pending_started = false; }
            if (ctx.pending_fail) { failed = true; ctx.pending_fail = false; }
            if (ctx.history_dirty) { ctx.history = Library::Instance().History(); ctx.history_dirty = false; }
            if (!ctx.pending_toast.empty()) { Toast(ctx, ctx.pending_toast); ctx.pending_toast.clear(); }
        }
        if (ctx.play_pending) {
            PlaybackState ps = AudioPlayer::Instance().GetState();
            if (ctx.play_started && ps == PlaybackState::PLAYING) {
                ctx.play_pending = false;
                ctx.fail_count = 0;
            } else if (ctx.play_started && ps == PlaybackState::STOPPED) {
                failed = true;
                Toast(ctx, T("Cannot play (unreadable stream)"));
            } else if (SDL_GetTicks() - ctx.play_started_ms > 60000) {
                failed = true;
                Toast(ctx, T("Playback took too long: giving up"));
            }
        }
        if (failed) {
            ctx.play_pending = false;
            ctx.fail_count++;
            if (ctx.fail_count < 3 && ctx.queue_idx + 1 < (int)ctx.queue.size()) NextTrack(ctx, true);
        }

        // Automatic advance (end of track)
        if (!ctx.play_pending && AudioPlayer::Instance().GetState() == PlaybackState::STOPPED &&
            !ctx.current_song.id.empty() && AudioPlayer::Instance().GetDuration() > 0 &&
            AudioPlayer::Instance().GetCurrentPosition() >= AudioPlayer::Instance().GetDuration() - 2 &&
            !ctx.queue.empty() && !ctx.radio_loading) {
            NextTrack(ctx, false);
        }

        // Last.fm scrobble at the halfway point
        if (AudioPlayer::Instance().GetState() == PlaybackState::PLAYING && !ctx.current_song.id.empty()) {
            int cur = AudioPlayer::Instance().GetCurrentPosition();
            int dur = AudioPlayer::Instance().GetDuration();
            static std::string s_last_scrobbled_id;
            if (dur > 30 && cur >= dur / 2 && s_last_scrobbled_id != ctx.current_song.id) {
                s_last_scrobbled_id = ctx.current_song.id;
                Song sc = ctx.current_song;
                RunAsync("ScrobbleReq", [sc]() { NetworkManager::ScrobbleLastFm(sc.artist, sc.title); });
            }
        }

        // Prefetch of the next queued track: it will start without waiting
        if (AudioPlayer::Instance().GetState() == PlaybackState::PLAYING && !ctx.shuffle_enabled &&
            ctx.repeat_mode != RepeatMode::ONE && ctx.queue_idx >= 0 && ctx.queue_idx + 1 < (int)ctx.queue.size() &&
            !ctx.play_pending) {
            int cur = AudioPlayer::Instance().GetCurrentPosition();
            int dur = AudioPlayer::Instance().GetDuration();
            const Song& nxt = ctx.queue[ctx.queue_idx + 1];
            if ((cur >= 12 || (dur > 0 && cur * 5 >= dur)) && ctx.prefetch_id != nxt.id && !AudioPlayer::Instance().HasPrefetched(nxt.id)) {
                ctx.prefetch_id = nxt.id;
                std::string nid = nxt.id;
                RunAsync("Prefetch", [nid]() {
                    std::string url = NetworkManager::GetAudioStreamUrl(nid);
                    if (!url.empty()) AudioPlayer::Instance().Prefetch(nid, url);
                });
            }
        }

        // Sleep timer
        if (ctx.sleep_deadline_ms && SDL_GetTicks() > ctx.sleep_deadline_ms) {
            ctx.sleep_deadline_ms = 0;
            ctx.settings.sleep_min = 0;
            AudioPlayer::Instance().Pause();
            Toast(ctx, T("Sleep timer: music paused"));
        }

        // YouTube liked songs (signed-in account)
        if (AccountManager::Instance().IsLoggedIn()) {
            if (!ctx.liked_requested) {
                ctx.liked_requested = true;
                RunAsync("LikedLoad", [&ctx]() {
                    std::vector<Song> songs = AccountManager::Instance().LoadPlaylistSongs("LL");
                    std::lock_guard<std::mutex> lock(ctx.data_mutex);
                    ctx.pending_liked = std::move(songs);
                    ctx.has_pending_liked = true;
                });
            }
        } else if (ctx.liked_requested || !ctx.liked.empty()) {
            ctx.liked_requested = false;
            ctx.liked.clear();
        }

        // Search: load more when nearing the end
        if (ctx.state == STATE_SEARCH && !ctx.search_next_token.empty() && !ctx.search_loading_more &&
            !ctx.search_is_loading && !ctx.search_query.empty()) {
            int cols = ((int)ctx.search_results.size() + 1) / 2;
            if (ctx.search_scroll_x > ShelfMaxScroll(cols) - 260) LoadMoreSearch(ctx);
        }

        // Asynchronous data
        {
            std::lock_guard<std::mutex> lock(ctx.data_mutex);
            if (ctx.has_pending_catalog) {
                ctx.catalog = std::move(ctx.pending_catalog);
                ctx.has_pending_catalog = false;
                if (ctx.state == STATE_ACCOUNT) {
                    ctx.selected_song_idx = 0;
                    ctx.scroll_x = 0;
                    if (!ctx.catalog.empty()) {
                        ctx.queue = ctx.catalog;
                        PlayQueueIdx(ctx, 0);
                    }
                    ctx.state = STATE_CATALOG;
                }
            }
            if (ctx.has_pending_search) {
                ctx.search_results = std::move(ctx.pending_search_results);
                ctx.search_next_token = ctx.pending_search_token;
                ctx.search_is_loading = false;
                ctx.has_pending_search = false;
            }
            if (ctx.has_pending_more) {
                for (auto& s : ctx.pending_more) {
                    bool dup = false;
                    for (auto& o : ctx.search_results) if (o.id == s.id) { dup = true; break; }
                    if (!dup) ctx.search_results.push_back(s);
                }
                ctx.pending_more.clear();
                ctx.search_next_token = ctx.pending_more_token;
                ctx.search_loading_more = false;
                ctx.has_pending_more = false;
            }
            if (ctx.has_pending_lyrics) {
                ctx.current_lyrics = std::move(ctx.pending_lyrics);
                ctx.has_pending_lyrics = false;
            }
            if (ctx.has_pending_artist) {
                ctx.artist_profile = std::move(ctx.pending_artist_profile);
                ctx.artist_profile.isLoading = false;
                ctx.has_pending_artist = false;
            }
            if (ctx.pending_mii) {
                if (ctx.tex_mii) SDL_DestroyTexture(ctx.tex_mii);
                ctx.tex_mii = SDL_CreateTextureFromSurface(ctx.renderer, ctx.pending_mii);
                SDL_FreeSurface(ctx.pending_mii);
                ctx.pending_mii = nullptr;
            }
            if (ctx.has_pending_liked) {
                ctx.liked = std::move(ctx.pending_liked);
                ctx.has_pending_liked = false;
            }
            if (ctx.has_pending_radio) {
                std::vector<Song> radio = std::move(ctx.pending_radio);
                ctx.has_pending_radio = false;
                ctx.radio_loading = false;
                bool then = ctx.radio_then_next;
                ctx.radio_then_next = false;
                QueueEnsureCurrent(ctx);
                size_t before = ctx.queue.size();
                for (auto& s : radio) {
                    bool dup = false;
                    for (auto& o : ctx.queue) if (o.id == s.id) { dup = true; break; }
                    if (!dup) ctx.queue.push_back(s);
                }
                if (ctx.queue.size() == before) {
                    ctx.toast = T("Radio unavailable");
                    ctx.toast_until = SDL_GetTicks() + 2600;
                } else if (then) {
                    NextTrack(ctx, true);
                }
            }
        }

        // 3. Rendering
        // Rendering is the big CPU consumer (and on Cemu it starves the audio thread: crackling sound).
        // At rest the screen hardly changes: we only redraw 10 times per second, and
        // at full rate only during (and 0.7 s after) an interaction.
        {
            uint32_t t = SDL_GetTicks();
            // Wii U keyboard on screen: SDL only draws it during SDL_RenderPresent. So we present every
            // frame (otherwise it lags), with a minimal render so we do not steal its CPU.
            if (t - kb_last_ms < 400) {
                DrawBackground(ctx);
                SDL_RenderPresent(ctx.renderer);
                AdvanceRendererFrame();
                SDL_Delay(1);
                last_render = t;
                continue;
            }
            if (pad.any() || touch.active || fling.p || ctx.menu_open || ctx.picker_open || ctx.press_valid) last_activity = t;
            bool animated = (ctx.state == STATE_PLAYER && AudioPlayer::Instance().GetState() == PlaybackState::PLAYING);
            bool active = (t - last_activity < 700) || ctx.toast_until > t;
            // 30 frames/s during a drag/inertia (60 fps of heavy drawing starved the audio of CPU)
            uint32_t interval = (touch.dragging || fling.p) ? 33 : (active ? 0 : (animated ? 33 : 100));
            if (!ctx.screen_off && t - last_render < interval) {
                SDL_Delay(6);
                continue;
            }
            last_render = t;
        }
        if (ctx.screen_off) {
            SDL_SetRenderDrawColor(ctx.renderer, 0, 0, 0, 255);
            SDL_RenderClear(ctx.renderer);
            SDL_RenderPresent(ctx.renderer);
            AdvanceRendererFrame();
            SDL_Delay(40);
            continue;
        }

        ctx.poster_mgr.update_textures(ctx);
        DrawBackground(ctx);

        switch (ctx.state) {
            case STATE_CATALOG:  RenderCatalog(ctx, ctx.poster_mgr); break;
            case STATE_SEARCH:   RenderSearch(ctx, ctx.poster_mgr); break;
            case STATE_PLAYER:   RenderPlayer(ctx, ctx.poster_mgr); break;
            case STATE_ACCOUNT:  RenderAccount(ctx, ctx.poster_mgr); break;
            case STATE_ARTIST:   RenderArtist(ctx, ctx.poster_mgr); break;
            case STATE_QUEUE:    RenderQueue(ctx, ctx.poster_mgr); break;
            case STATE_SETTINGS: RenderSettings(ctx, ctx.poster_mgr); break;
            case STATE_PLAYLISTS: RenderPlaylists(ctx, ctx.poster_mgr); break;
            case STATE_LANGUAGE:  RenderLanguage(ctx, ctx.poster_mgr); break;
            case STATE_PLAYLIST_VIEW: RenderPlaylistView(ctx, ctx.poster_mgr); break;
        }
        RenderOverlays(ctx, ctx.poster_mgr);

        SDL_RenderPresent(ctx.renderer);
        AdvanceRendererFrame();
        // Player: 30 frames/s is enough (bar, lyrics) and leaves CPU to audio decoding
        SDL_Delay(ctx.state == STATE_PLAYER ? 16 : 1);
    }

    ctx.poster_mgr.shutdown();
    AudioPlayer::Instance().Cleanup();
    AccountManager::Instance().Shutdown();
    ClearTextCache();
    IconsShutdown();
    if (ctx.screen_off) SetScreenOff(ctx, false);

    if (ctx.font_title) TTF_CloseFont(ctx.font_title);
    if (ctx.font_normal) TTF_CloseFont(ctx.font_normal);
    if (ctx.font_small) TTF_CloseFont(ctx.font_small);
    if (ctx.tex_logo) SDL_DestroyTexture(ctx.tex_logo);
    if (ctx.tex_mii) SDL_DestroyTexture(ctx.tex_mii);
    for (auto& pair : ctx.poster_cache) {
        if (pair.second) SDL_DestroyTexture(pair.second);
    }

    if (ctx.renderer) SDL_DestroyRenderer(ctx.renderer);
    if (ctx.window) SDL_DestroyWindow(ctx.window);

    IMG_Quit();
    TTF_Quit();
    SDL_Quit();

    NetworkManager::Cleanup();
    WHBProcShutdown();

    return 0;
}
