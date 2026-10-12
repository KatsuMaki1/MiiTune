#ifndef MIITUNE_RENDERER_H
#define MIITUNE_RENDERER_H

#include "models.h"
#include "audio.h"
#include "network.h"
#include "library.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include <string>
#include <vector>
#include <map>
#include <set>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>

// Palette (near-black background, coral accent)
const SDL_Color COLOR_BAR         = {  16,  14,  21, 255 };
const SDL_Color COLOR_CARD        = {  30,  28,  38, 255 };
const SDL_Color COLOR_CARD_ACTIVE = {  52,  48,  68, 255 };
const SDL_Color COLOR_CHIP        = {  36,  33,  46, 255 };
const SDL_Color COLOR_PRIMARY     = { 255,  82, 110, 255 };
const SDL_Color COLOR_CYAN        = {  56, 189, 248, 255 };
const SDL_Color COLOR_GREEN       = {  52, 211, 153, 255 };
const SDL_Color COLOR_WHITE       = { 246, 244, 248, 255 };
const SDL_Color COLOR_MUTED       = { 165, 160, 180, 255 };
const SDL_Color COLOR_MUTED_DARK  = { 110, 105, 125, 255 };
const SDL_Color COLOR_INK         = {  18,  14,  23, 255 }; // dark text on a light surface
const SDL_Color COLOR_HEART       = { 255,  64, 129, 255 };
const SDL_Color COLOR_LASTFM      = { 217,  35,  35, 255 };
const SDL_Color COLOR_NONE        = {   0,   0,   0,   0 };

// Single 1280x720 layout. The GamePad shows the same image scaled down (x0.667),
// so all touch targets are >= 56 logical px (~37 px on the DRC).
namespace L {
    constexpr int W = 1280, H = 720;
    constexpr int RAIL_W = 92;
    constexpr int CONTENT_X = RAIL_W + 36;
    constexpr int RIGHT_PAD = 28;
    constexpr int CARD_W = 224, CARD_GAP = 20;
    constexpr int POSTER_H = 126, INFO_H = 72, CARD_H = POSTER_H + INFO_H;
    constexpr int DOCK_H = 84, DOCK_Y = H - DOCK_H;
    constexpr int VISIBLE_W = W - CONTENT_X - RIGHT_PAD;
    constexpr int TOPBAR_Y = 14, TOPBAR_H = 56;
    constexpr int CHIPS_Y = 82, CHIPS_H = 48;
    constexpr int ROW1_TITLE_Y = 138, ROW1_Y = 172;
    constexpr int SEC_H = 258;             // height of a home-screen section (title + cards + margin)
    constexpr int CONTENT_TOP = 130;       // home scrolling band
    constexpr int QUEUE_TOP = 96, QUEUE_ROW = 86;
    constexpr int RAIL_ITEMS = 6;
    constexpr int PL_ROWS_TOP = 160;       // top of a playlist's list
}

extern const std::vector<std::string> QUICK_MUSIC_TAGS;
extern bool g_debug_hud;   // show audio/touch diagnostics (enabled by a debug.txt file next to the settings)

// Song lists addressable from a touch region
enum SongList { LIST_CATALOG = 0, LIST_SEARCH = 1, LIST_ARTIST = 2, LIST_FAVS = 3, LIST_HISTORY = 4, LIST_LIKED = 5, LIST_QUEUE = 6, LIST_PLAYLIST = 7 };

// Actions triggered by a tap. Rendering registers the regions, main.cpp runs them.
enum class Act {
    NONE,
    NOP,            // swallows the tap (menu background)
    NAV,            // a = AppState
    KEYBOARD,       // opens the Wii U keyboard
    CHIP,           // a = 0 favorites, 1.. = QUICK_MUSIC_TAGS[a-1]
    PLAY_SONG,      // a = SongList, b = index
    OPEN_ARTIST,    // a = SongList, b = index (the track's channel)
    OPEN_ARTIST_CUR,// channel of the current track
    BACK,
    DOCK_OPEN,
    PLAY_PAUSE,
    SEEK_REL,       // a = seconds (+/-)
    SEEK_BAR,       // position from the finger's x (resolved on release)
    VOLUME_BAR,     // volume from the finger's x (live)
    PREV_TRACK, NEXT_TRACK,
    SHUFFLE, REPEAT, LYRICS, FAVORITE, LASTFM,
    ARTIST_SHUFFLE,
    ACC_NEW_CODE, ACC_CANCEL, ACC_LOGOUT, ACC_REFRESH,
    ACC_PLAYLIST,   // a = index
    OPEN_QUEUE, SCREEN_OFF,
    QUEUE_REMOVE, QUEUE_UP, QUEUE_CLEAR,   // a = index
    HISTORY_SEARCH, CLEAR_SEARCHES,        // a = index
    MENU_PLAY, MENU_NEXT, MENU_QUEUE, MENU_FAV, MENU_CHANNEL, MENU_RADIO, MENU_CLOSE,
    PL_NEW, PL_OPEN, PL_PLAY, PL_SHUFFLE, PL_DELETE, PL_REMOVE,   // a = index
    MENU_PLAYLIST, PLAYER_PL_ADD, PICK_PLAYLIST, PICKER_CLOSE,    // PICK_PLAYLIST: a = -1 new, otherwise index
    LANG_SET, SET_LANGUAGE,               // LANG_SET: a = chosen language; SET_LANGUAGE: next language
    SET_REGION, SET_AUTOPLAY, SET_SLEEP, SET_SEARCHMODE, LFM_ACTION, CLEAR_HISTORY, CLEAR_SEARCH_HISTORY
};

struct HitRegion {
    SDL_Rect r;
    Act act = Act::NONE;
    int a = 0, b = 0;
    int* scroll = nullptr;   // scrollable region (drag)
    int scroll_max = 0;
    bool vert = false;       // vertical scrolling
    bool content = false;    // region inside the scrolling band: ignored outside the band
};

struct AppContext;

struct PosterDownloadRequest {
    std::string song_id;
    std::string image_url;
};

class PosterManager {
public:
    void init();
    void shutdown();
    void queue_poster(const std::string& id, const std::string& url);
    void update_textures(AppContext& ctx);
    void worker_loop();

private:
    std::atomic<bool> m_running{false};
    SDL_Thread* m_worker = nullptr;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::set<std::string> m_queued_ids;
    std::vector<PosterDownloadRequest> m_queue;
    std::vector<std::pair<std::string, SDL_Surface*>> m_ready_surfaces;
};

struct AppContext {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    TTF_Font* font_title = nullptr;
    TTF_Font* font_normal = nullptr;
    TTF_Font* font_small = nullptr;

    ScreenFormat screen_format = FORMAT_TV; // kept for compatibility, layout is always 1280x720
    int screen_w = L::W;
    int screen_h = L::H;

    SDL_Texture* tex_logo = nullptr;
    SDL_Texture* tex_mii = nullptr;      // console account Mii (top left)
    SDL_Surface* pending_mii = nullptr;  // (thread -> main) surface to turn into a texture
    std::map<std::string, SDL_Texture*> poster_cache;
    std::vector<std::string> poster_cache_lru;

    std::mutex data_mutex;

    AppState state = STATE_CATALOG;
    AppState previous_state = STATE_CATALOG;

    // Home: sections (trending / recent / favorites / liked) in a vertically scrolling page
    std::vector<Song> catalog;
    std::vector<Song> pending_catalog;
    bool has_pending_catalog = false;
    int selected_song_idx = 0, scroll_x = 0;
    std::vector<Song> history;
    int hist_sel = 0, hist_scroll = 0;
    std::vector<Song> favorites;
    int fav_selected_idx = 0, fav_scroll_x = 0;
    std::vector<Song> liked;             // "Liked songs" from YouTube (signed-in account)
    std::vector<Song> pending_liked;
    bool has_pending_liked = false;
    bool liked_requested = false;
    int liked_sel = 0, liked_scroll = 0;
    int home_row = 0;                    // focused section (index into BuildHomeSections)
    int home_scroll_y = 0;
    int content_bottom = L::H;           // bottom of the scrolling band (above the dock)

    // Search (2-row grid)
    std::string search_query;
    std::vector<Song> search_results;
    std::vector<Song> pending_search_results;
    bool has_pending_search = false;
    int search_selected_idx = 0, search_scroll_x = 0;
    int search_tag_idx = -1;
    bool search_is_loading = false;
    std::string search_title;
    std::string search_next_token, pending_search_token;
    bool search_loading_more = false;
    std::vector<Song> pending_more;
    std::string pending_more_token;
    bool has_pending_more = false;
    std::vector<std::string> search_history;

    // Playback + queue
    Song current_song;
    LyricsData current_lyrics;
    LyricsData pending_lyrics;
    bool has_pending_lyrics = false;
    bool show_lyrics = false;
    RepeatMode repeat_mode = RepeatMode::OFF;
    bool shuffle_enabled = false;
    uint32_t current_play_req_id = 0;
    std::vector<Song> queue;
    int queue_idx = -1;
    int queue_scroll_y = 0, queue_sel = 0;
    std::vector<Song> pending_radio;
    bool has_pending_radio = false;
    bool radio_loading = false;

    // Track loading state (spinner, errors)
    bool play_pending = false;
    bool play_started = false;      // PlayStream was called for the current track
    bool pending_started = false;   // (thread -> main) same, under data_mutex
    bool pending_fail = false;      // (thread -> main) stream extraction failed
    bool radio_then_next = false;
    std::string prefetch_id;         // next track already requested for prefetch
    bool history_dirty = false;      // (thread -> main) the on-disk history changed
    uint32_t play_started_ms = 0;
    int fail_count = 0;

    // Artist
    ArtistProfile artist_profile;
    ArtistProfile pending_artist_profile;
    bool has_pending_artist = false;

    // Account
    int account_selected_playlist_idx = 0;
    int account_scroll_x = 0;

    // Local playlists
    std::vector<LocalPlaylist> playlists;
    int pl_open = -1;                     // open playlist (STATE_PLAYLIST_VIEW)
    std::vector<Song> pl_songs;           // copy of its tracks
    int pl_sel = 0, pl_scroll_x = 0;      // playlist grid
    int pl_view_sel = 0, pl_view_scroll_y = 0;
    int text_mode = 0;                    // 0 = search, 1 = playlist name (Wii U keyboard)
    bool pl_pending_valid = false;        // track to add once the playlist is created
    Song pl_pending_song;
    bool picker_open = false;             // playlist picker
    Song picker_song;
    int picker_sel = 0, picker_scroll = 0;

    // Settings / sleep
    Settings settings;
    int settings_sel = 0;
    int lang_sel = 0;
    bool screen_off = false;
    uint32_t sleep_deadline_ms = 0;

    // Context menu (long press)
    bool menu_open = false;
    Song menu_song;
    int menu_sel = 0;

    // Toast
    std::string toast;
    uint32_t toast_until = 0;
    std::string pending_toast;

    // Rail (gamepad navigation)
    bool rail_focused = false;
    int rail_selected_idx = 0; // 0 Home, 1 Search, 2 Player, 3 Account, 4 Settings

    // Touch: regions rebuilt every frame by the renderer
    std::vector<HitRegion> hits;
    char touch_dbg[160] = "";      // touch diagnostics (shown bottom left)
    bool focus_visible = false; // focus ring only after button use
    bool press_valid = false;
    SDL_Rect press_rect = {0, 0, 0, 0};

    PosterManager poster_mgr;
};

struct HomeSec {
    const char* title;
    std::vector<Song>* list;
    int listId;
    int* sel;
    int* scroll;
};

void SetScreenFormat(AppContext& ctx, ScreenFormat fmt);
void AdvanceRendererFrame();
void ClearTextCache();
void RenderText(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, int x, int y, SDL_Color color);
int MeasureTextWidth(TTF_Font* font, const std::string& text);

SDL_Texture* GetPosterTexture(AppContext& ctx, PosterManager& pmgr, const Song& song);
void DrawBackground(AppContext& ctx);
void DrawM3Pill(SDL_Renderer* renderer, int x, int y, int w, int h, SDL_Color bg, SDL_Color border, int radius = 12);
bool DrawGeometry(SDL_Renderer* r, const SDL_Vertex* v, int nv, const int* idx, int ni);   // false if unsupported
void DrawRoundCorners(SDL_Renderer* r, SDL_Rect a, int rad, SDL_Color top, SDL_Color bottom);
void DrawFillCircle(SDL_Renderer* r, int cx, int cy, int rad, SDL_Color c);
void DrawCornerMask(SDL_Renderer* r, int x, int y, int rad, bool right, bool bottom, SDL_Color c);
void DrawThickLine(SDL_Renderer* r, int x1, int y1, int x2, int y2, int t, SDL_Color c);

// Scrolling of card rows and the home page
int ShelfMaxScroll(int cols);
void EnsureColVisible(int& scroll, int col, int cols);
int BuildHomeSections(AppContext& ctx, HomeSec out[6]);   // returns the number of sections
void EnsureSectionVisible(AppContext& ctx, int sec);
int HomeMaxScrollY(AppContext& ctx);
int QueueMaxScrollY(AppContext& ctx);

// Screens
void RenderCatalog(AppContext& ctx, PosterManager& pmgr);
void RenderSearch(AppContext& ctx, PosterManager& pmgr);
void RenderPlayer(AppContext& ctx, PosterManager& pmgr);
void RenderArtist(AppContext& ctx, PosterManager& pmgr);
void RenderAccount(AppContext& ctx, PosterManager& pmgr);
void RenderQueue(AppContext& ctx, PosterManager& pmgr);
void RenderSettings(AppContext& ctx, PosterManager& pmgr);
void RenderPlaylists(AppContext& ctx, PosterManager& pmgr);
void RenderLanguage(AppContext& ctx, PosterManager& pmgr);
void RenderPlaylistView(AppContext& ctx, PosterManager& pmgr);
int PlaylistViewMaxScrollY(AppContext& ctx);
void RenderOverlays(AppContext& ctx, PosterManager& pmgr);   // context menu, toast, touch feedback

#endif // MIITUNE_RENDERER_H
