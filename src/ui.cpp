// MiiTune screens. Rendering registers touch regions (ctx.hits),
// main.cpp runs them. Single 1280x720 layout, touch targets >= 56 px.
#include "renderer.h"
#include "i18n.h"
#include "account.h"
#include "favorites.h"
#include "lastfm.h"
#include "qr.h"
#include "icons.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <ctime>

void DrawVinylPlaceholder(SDL_Renderer* renderer, int x, int y, int w, int h, const std::string& title);

const std::vector<std::string> QUICK_MUSIC_TAGS = {
    "Top Hits FR", "Rap Francais", "Rock Classics", "Lofi Hip Hop", "Pop 2026", "Electro Dance", "Anime OST"
};

// ---------------------------------------------------------------- primitives

static void SetCol(SDL_Renderer* r, SDL_Color c) { SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a); }

static SDL_Color BgAt(int y) {
    float t = std::min(1.0f, std::max(0.0f, y / (float)L::H));
    return { (Uint8)(18 - 11 * t), (Uint8)(14 - 8 * t), (Uint8)(23 - 13 * t), 255 };
}

static void FillCircle(SDL_Renderer* r, int cx, int cy, int rad, SDL_Color c) { DrawFillCircle(r, cx, cy, rad, c); }



// Rounds an image corner by painting the outside of the quarter circle

static void RoundImage(SDL_Renderer* r, SDL_Rect a, int rad, SDL_Color top, SDL_Color bottom) {
    DrawRoundCorners(r, a, rad, top, bottom);
}

// ---------------------------------------------------------------- icons

static void IconHome(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c, SDL_Color) { DrawIcon(r, IC_HOUSE, cx, cy, s, c); }
static void IconSearch(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_SEARCH, cx, cy, s, c); }
static void IconNote(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_MUSIC, cx, cy, s, c); }
static void IconPerson(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_USER_ROUND, cx, cy, s, c); }
static void IconGear(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_SETTINGS, cx, cy, s, c); }
static void IconPlay(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_PLAY, cx, cy, s, c); }
static void IconPause(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_PAUSE, cx, cy, s, c); }
static void IconSeek(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c, bool forward) { DrawIcon(r, forward ? IC_ROTATE_CW : IC_ROTATE_CCW, cx, cy, s, c); }
static void IconTrack(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c, bool forward) { DrawIcon(r, forward ? IC_SKIP_FORWARD : IC_SKIP_BACK, cx, cy, s, c); }
static void IconHeart(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c, bool filled, SDL_Color) { DrawIcon(r, filled ? IC_HEART_FILLED : IC_HEART, cx, cy, s, c); }
static void IconShuffle(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_SHUFFLE, cx, cy, s, c); }
static void IconRepeat(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c, bool one) { DrawIcon(r, one ? IC_REPEAT_1 : IC_REPEAT, cx, cy, s, c); }
static void IconBack(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_ARROW_LEFT, cx, cy, s, c); }
static void IconLyrics(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_MIC_VOCAL, cx, cy, s, c); }
static void IconQueue(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_LIST_MUSIC, cx, cy, s, c); }
static void IconListEnd(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_LIST_END, cx, cy, s, c); }
static void IconListPlus(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_LIST_PLUS, cx, cy, s, c); }
static void IconMoon(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c, SDL_Color) { DrawIcon(r, IC_MOON, cx, cy, s, c); }
static void IconClose(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_X, cx, cy, s, c); }
static void IconUp(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_CHEVRON_UP, cx, cy, s, c); }
static void IconSpeaker(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_VOLUME_2, cx, cy, s, c); }
static void IconRadio(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_RADIO, cx, cy, s, c); }
static void IconPlus(SDL_Renderer* r, int cx, int cy, int s, SDL_Color c) { DrawIcon(r, IC_PLUS, cx, cy, s, c); }

// ---------------------------------------------------------------- text

static int FontH(TTF_Font* f) { return f ? TTF_FontHeight(f) : 20; }

static void TextC(AppContext& c, TTF_Font* f, const std::string& t, int cx, int cy, SDL_Color col) {
    RenderText(c.renderer, f, t, cx - MeasureTextWidth(f, t) / 2, cy - FontH(f) / 2, col);
}

static std::string Fit(TTF_Font* f, const std::string& s, int maxW) {
    static std::map<std::string, std::string> cache;
    char kb[48];
    snprintf(kb, sizeof(kb), "%p_%d#", (void*)f, maxW);
    std::string key = std::string(kb) + s;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 600) cache.clear();
    std::string out = s;
    if (MeasureTextWidth(f, s) > maxW) {
        std::string t = s;
        out.clear();
        while (!t.empty()) {
            size_t i = t.size() - 1;
            while (i > 0 && (t[i] & 0xC0) == 0x80) i--;
            t.erase(i);
            std::string cand = t + "...";
            if (MeasureTextWidth(f, cand) <= maxW) { out = cand; break; }
        }
    }
    cache[key] = out;
    return out;
}

static void TextFit(AppContext& c, TTF_Font* f, const std::string& t, int x, int y, int maxW, SDL_Color col) {
    RenderText(c.renderer, f, Fit(f, t, maxW), x, y, col);
}

// ---------------------------------------------------------------- touch regions

static bool g_content = false;   // true while drawing the scrolling band

static void Hit(AppContext& c, int x, int y, int w, int h, Act a, int aa = 0, int bb = 0) {
    HitRegion hr;
    hr.r = { x, y, w, h };
    hr.act = a; hr.a = aa; hr.b = bb;
    hr.content = g_content;
    c.hits.push_back(hr);
}

static void HitScroll(AppContext& c, SDL_Rect r, int* scroll, int maxScroll, bool vert = false) {
    HitRegion hr;
    hr.r = r; hr.scroll = scroll; hr.scroll_max = maxScroll; hr.vert = vert;
    hr.content = g_content;
    c.hits.push_back(hr);
}

int ShelfMaxScroll(int cols) {
    return std::max(0, cols * (L::CARD_W + L::CARD_GAP) - L::CARD_GAP - L::VISIBLE_W);
}

void EnsureColVisible(int& scroll, int col, int cols) {
    int left = col * (L::CARD_W + L::CARD_GAP);
    if (left < scroll) scroll = left;
    else if (left + L::CARD_W > scroll + L::VISIBLE_W) scroll = left + L::CARD_W - L::VISIBLE_W;
    scroll = std::max(0, std::min(scroll, ShelfMaxScroll(cols)));
}

static bool DockVisible(const AppContext& c) {
    return c.state != STATE_PLAYER && !c.current_song.id.empty();
}

int BuildHomeSections(AppContext& c, HomeSec out[6]) {
    int n = 0;
    out[n++] = { T("Trending"), &c.catalog, LIST_CATALOG, &c.selected_song_idx, &c.scroll_x };
    if (!c.history.empty()) out[n++] = { T("Recently played"), &c.history, LIST_HISTORY, &c.hist_sel, &c.hist_scroll };
    out[n++] = { T("My favorites"), &c.favorites, LIST_FAVS, &c.fav_selected_idx, &c.fav_scroll_x };
    if (!c.liked.empty()) out[n++] = { T("Liked on YouTube"), &c.liked, LIST_LIKED, &c.liked_sel, &c.liked_scroll };
    return n;
}

int HomeMaxScrollY(AppContext& c) {
    HomeSec s[6];
    int n = BuildHomeSections(c, s);
    int visible = c.content_bottom - L::ROW1_TITLE_Y;
    return std::max(0, n * L::SEC_H - 26 - visible);
}

void EnsureSectionVisible(AppContext& c, int sec) {
    int top = sec * L::SEC_H, bottom = top + 34 + L::CARD_H + 8;
    int visible = c.content_bottom - L::ROW1_TITLE_Y;
    if (top < c.home_scroll_y) c.home_scroll_y = top;
    else if (bottom > c.home_scroll_y + visible) c.home_scroll_y = bottom - visible;
    c.home_scroll_y = std::max(0, std::min(c.home_scroll_y, HomeMaxScrollY(c)));
}

int QueueMaxScrollY(AppContext& c) {
    int visible = c.content_bottom - L::QUEUE_TOP;
    return std::max(0, (int)c.queue.size() * L::QUEUE_ROW - visible + 8);
}

// ---------------------------------------------------------------- common building blocks

static void RenderRail(AppContext& ctx) {
    SDL_Renderer* r = ctx.renderer;
    SDL_Rect rail = { 0, 0, L::RAIL_W, L::H };
    SetCol(r, COLOR_BAR);
    SDL_RenderFillRect(r, &rail);
    SetCol(r, { 38, 33, 52, 255 });
    SDL_Rect div = { L::RAIL_W - 1, 0, 1, L::H };
    SDL_RenderFillRect(r, &div);

    SDL_Rect logo = { (L::RAIL_W - 52) / 2, 18, 52, 52 };
    DrawM3Pill(r, logo.x, logo.y, logo.w, logo.h, COLOR_PRIMARY, COLOR_NONE, 16);
    if (ctx.tex_mii) {
        // User Mii: round, on a neutral background
        DrawFillCircle(r, logo.x + 26, logo.y + 26, 26, COLOR_CARD_ACTIVE);
        SDL_RenderCopy(r, ctx.tex_mii, nullptr, &logo);
        RoundImage(r, logo, 26, COLOR_BAR, COLOR_BAR);
    } else {
        if (ctx.tex_logo) SDL_RenderCopy(r, ctx.tex_logo, nullptr, &logo);
        RoundImage(r, logo, 16, COLOR_BAR, COLOR_BAR);
    }

    struct Item { AppState st; const char* label; };
    const Item items[L::RAIL_ITEMS] = {
        { STATE_CATALOG, T("Home") }, { STATE_SEARCH, T("Search") }, { STATE_PLAYER, T("Player") },
        { STATE_PLAYLISTS, T("Playlists") }, { STATE_ACCOUNT, T("Account") }, { STATE_SETTINGS, T("Settings") }
    };
    for (int i = 0; i < L::RAIL_ITEMS; i++) {
        int y = 92 + i * 88;
        bool cur = (ctx.state == items[i].st) || (i == 2 && ctx.state == STATE_QUEUE) ||
                   (i == 3 && ctx.state == STATE_PLAYLIST_VIEW);
        bool foc = ctx.focus_visible && ctx.rail_focused && ctx.rail_selected_idx == i;
        SDL_Rect it = { 8, y, L::RAIL_W - 16, 82 };
        if (foc) DrawM3Pill(r, it.x, it.y, it.w, it.h, COLOR_CARD_ACTIVE, COLOR_PRIMARY, 22);
        else if (cur) DrawM3Pill(r, it.x, it.y, it.w, it.h, COLOR_CARD, COLOR_NONE, 22);
        SDL_Color col = cur ? COLOR_PRIMARY : (foc ? COLOR_WHITE : COLOR_MUTED);
        SDL_Color under = foc ? COLOR_CARD_ACTIVE : (cur ? COLOR_CARD : COLOR_BAR);
        int cx = L::RAIL_W / 2, cy = y + 30;
        if (i == 0) IconHome(r, cx, cy, 32, col, under);
        else if (i == 1) IconSearch(r, cx, cy, 32, col);
        else if (i == 2) IconNote(r, cx, cy, 32, col);
        else if (i == 3) IconQueue(r, cx, cy, 32, col);
        else if (i == 4) IconPerson(r, cx, cy, 32, col);
        else IconGear(r, cx, cy, 32, col);
        TextC(ctx, ctx.font_small, items[i].label, cx, y + 62, col);
        Hit(ctx, it.x, it.y, it.w, it.h, Act::NAV, (int)items[i].st);
    }
}

static void RenderTopBar(AppContext& ctx, const std::string& title, const std::string& placeholder, bool withSearch = true) {
    SDL_Renderer* r = ctx.renderer;
    int cy = L::TOPBAR_Y + L::TOPBAR_H / 2;
    RenderText(r, ctx.font_title, title, L::CONTENT_X, cy - FontH(ctx.font_title) / 2, COLOR_WHITE);

    if (withSearch) {
        int px = L::CONTENT_X + 210, pw = 660;
        DrawM3Pill(r, px, L::TOPBAR_Y, pw, L::TOPBAR_H, { 236, 234, 240, 255 }, COLOR_NONE, L::TOPBAR_H / 2);
        IconSearch(r, px + 34, cy, 26, COLOR_INK);
        TextFit(ctx, ctx.font_normal, placeholder, px + 64, cy - FontH(ctx.font_normal) / 2, pw - 90, { 92, 88, 106, 255 });
        Hit(ctx, px, L::TOPBAR_Y, pw, L::TOPBAR_H, Act::KEYBOARD);
    }

    time_t now = time(nullptr);
    struct tm* lt = localtime(&now);
    char buf[16];
    if (lt) snprintf(buf, sizeof(buf), "%02d:%02d", lt->tm_hour, lt->tm_min);
    else snprintf(buf, sizeof(buf), "--:--");
    int tw = MeasureTextWidth(ctx.font_normal, buf);
    RenderText(r, ctx.font_normal, buf, L::W - L::RIGHT_PAD - tw, cy - FontH(ctx.font_normal) / 2, COLOR_WHITE);
}

static void RenderChips(AppContext& ctx) {
    SDL_Renderer* r = ctx.renderer;
    int x = L::CONTENT_X;
    int cy = L::CHIPS_Y + L::CHIPS_H / 2;
    for (size_t i = 0; i <= QUICK_MUSIC_TAGS.size(); ++i) {
        std::string label = (i == 0) ? T("Favorites") : QUICK_MUSIC_TAGS[i - 1];
        int extra = (i == 0) ? 30 : 0;
        int tw = MeasureTextWidth(ctx.font_small, label);
        int w = tw + 44 + extra;
        if (x + w > L::W - L::RIGHT_PAD) break;
        bool sel = (ctx.search_tag_idx == (int)i);
        DrawM3Pill(r, x, L::CHIPS_Y, w, L::CHIPS_H, sel ? COLOR_WHITE : COLOR_CHIP, COLOR_NONE, L::CHIPS_H / 2);
        SDL_Color fg = sel ? COLOR_INK : COLOR_WHITE;
        int tx = x + 22;
        if (i == 0) {
            IconHeart(r, x + 34, cy, 18, sel ? COLOR_HEART : COLOR_MUTED, true, COLOR_NONE);
            tx += 30;
        }
        RenderText(r, ctx.font_small, label, tx, cy - FontH(ctx.font_small) / 2, fg);
        Hit(ctx, x, L::CHIPS_Y, w, L::CHIPS_H, Act::CHIP, (int)i);
        x += w + 12;
    }
}

static void RenderDock(AppContext& ctx, PosterManager& pm) {
    if (!DockVisible(ctx)) return;
    SDL_Renderer* r = ctx.renderer;
    int x = L::RAIL_W, w = L::W - L::RAIL_W, y = L::DOCK_Y, h = L::DOCK_H;
    SDL_Rect bg = { x, y, w, h };
    SetCol(r, COLOR_BAR);
    SDL_RenderFillRect(r, &bg);
    SetCol(r, { 38, 33, 52, 255 });
    SDL_Rect line = { x, y, w, 1 };
    SDL_RenderFillRect(r, &line);

    int cur = AudioPlayer::Instance().GetCurrentPosition();
    int dur = AudioPlayer::Instance().GetDuration();
    if (dur <= 0) dur = std::max(1, ctx.current_song.durationSeconds);
    float pct = ctx.play_pending ? 0.0f : std::min(1.0f, std::max(0.0f, (float)cur / (float)dur));
    SetCol(r, COLOR_PRIMARY);
    SDL_Rect pb = { x, y, (int)(w * pct), 4 };
    SDL_RenderFillRect(r, &pb);

    Hit(ctx, x, y, w, h, Act::DOCK_OPEN);

    SDL_Rect cover = { L::CONTENT_X, y + 12, 60, 60 };
    SDL_Texture* tex = GetPosterTexture(ctx, pm, ctx.current_song);
    if (tex) SDL_RenderCopy(r, tex, nullptr, &cover);
    else DrawVinylPlaceholder(r, cover.x, cover.y, cover.w, cover.h, ctx.current_song.title);
    RoundImage(r, cover, 10, COLOR_BAR, COLOR_BAR);

    int tx = cover.x + cover.w + 16, maxW = 880 - tx;
    TextFit(ctx, ctx.font_normal, ctx.current_song.title.empty() ? T("No track") : ctx.current_song.title,
            tx, y + 14, maxW, COLOR_WHITE);
    if (ctx.play_pending) {
        RenderText(r, ctx.font_small, T("Loading..."), tx, y + 48, COLOR_PRIMARY);
    } else {
        // Audio diagnostics refreshed twice per second (text that changes every frame forces a texture rebuild)
        static std::string s_dbg;
        static uint32_t s_dbg_t = 0;
        if (SDL_GetTicks() - s_dbg_t > 500) {
            s_dbg_t = SDL_GetTicks();
            float bufSec, speed; int und;
            AudioPlayer::Instance().GetDebugInfo(bufSec, speed, und);
            char b[80];
            snprintf(b, sizeof(b), T("   -   buffer %.0fs  x%.1f  dropouts %d"), bufSec, speed, und);
            s_dbg = b;
        }
        std::string artist = ctx.current_song.artist.empty() ? "YouTube Music" : ctx.current_song.artist;
        TextFit(ctx, ctx.font_small, g_debug_hud ? artist + s_dbg : artist, tx, y + 48, maxW, COLOR_MUTED);
    }

    int cy = y + 44;
    bool playing = (AudioPlayer::Instance().GetState() == PlaybackState::PLAYING);
    int qx = 930, bx = 1030, px = 1120, fx = 1210;
    FillCircle(r, qx, cy, 28, COLOR_CARD);
    IconQueue(r, qx, cy, 26, COLOR_WHITE);
    Hit(ctx, qx - 34, cy - 34, 68, 68, Act::OPEN_QUEUE);
    FillCircle(r, bx, cy, 28, COLOR_CARD);
    IconTrack(r, bx, cy, 22, COLOR_WHITE, false);
    Hit(ctx, bx - 34, cy - 34, 68, 68, Act::PREV_TRACK);
    FillCircle(r, px, cy, 34, COLOR_WHITE);
    if (playing) IconPause(r, px, cy, 24, COLOR_INK); else IconPlay(r, px + 2, cy, 26, COLOR_INK);
    Hit(ctx, px - 38, cy - 38, 76, 76, Act::PLAY_PAUSE);
    FillCircle(r, fx, cy, 28, COLOR_CARD);
    IconTrack(r, fx, cy, 22, COLOR_WHITE, true);
    Hit(ctx, fx - 34, cy - 34, 68, 68, Act::NEXT_TRACK);
}

// Song card: rounded poster, title, channel (tap = open channel)
static void DrawSongCard(AppContext& ctx, PosterManager& pm, const Song& s, int x, int y, bool focused,
                         int list, int idx, int rank = 0) {
    SDL_Renderer* r = ctx.renderer;
    const int rad = 14;
    if (focused) DrawM3Pill(r, x - 4, y - 4, L::CARD_W + 8, L::POSTER_H + 8, COLOR_PRIMARY, COLOR_NONE, rad + 4);

    SDL_Rect pr = { x, y, L::CARD_W, L::POSTER_H };
    SDL_Texture* tex = GetPosterTexture(ctx, pm, s);
    if (tex) SDL_RenderCopy(r, tex, nullptr, &pr);
    else DrawVinylPlaceholder(r, x, y, L::CARD_W, L::POSTER_H, s.title);
    RoundImage(r, pr, rad, focused ? COLOR_PRIMARY : BgAt(y), focused ? COLOR_PRIMARY : BgAt(y + L::POSTER_H));

    std::string dur = s.durationFormatted;
    if (!dur.empty()) {
        int bw = MeasureTextWidth(ctx.font_small, dur) + 16, bh = 26;
        int bx = x + L::CARD_W - bw - 8, by = y + L::POSTER_H - bh - 8;
        DrawM3Pill(r, bx, by, bw, bh, { 0, 0, 0, 200 }, COLOR_NONE, 8);
        RenderText(r, ctx.font_small, dur, bx + 8, by + (bh - FontH(ctx.font_small)) / 2, COLOR_WHITE);
    }
    if (rank > 0) {
        std::string rs = "#" + std::to_string(rank);
        int bw = MeasureTextWidth(ctx.font_small, rs) + 16;
        DrawM3Pill(r, x + 8, y + 8, bw, 26, COLOR_PRIMARY, COLOR_NONE, 8);
        RenderText(r, ctx.font_small, rs, x + 16, y + 8 + (26 - FontH(ctx.font_small)) / 2, COLOR_WHITE);
    }

    std::string creator = s.channelTitle.empty() ? s.artist : s.channelTitle;
    TextFit(ctx, ctx.font_normal, s.title, x, y + L::POSTER_H + 6, L::CARD_W, COLOR_WHITE);
    TextFit(ctx, ctx.font_small, creator, x, y + L::POSTER_H + 38, L::CARD_W, focused ? COLOR_CYAN : COLOR_MUTED);

    Hit(ctx, x, y, L::CARD_W, L::POSTER_H + 34, Act::PLAY_SONG, list, idx);
    if (!creator.empty())
        Hit(ctx, x, y + L::POSTER_H + 34, L::CARD_W, L::INFO_H - 34 + 8, Act::OPEN_ARTIST, list, idx);
}

// Scrollable row (or `rows`-row grid) of cards
static void RenderShelf(AppContext& ctx, PosterManager& pm, const std::vector<Song>& list, int listId,
                        int rowY, int rows, int sel, int& scroll, bool focusShelf, bool ranks = false) {
    int cols = ((int)list.size() + rows - 1) / rows;
    int maxS = ShelfMaxScroll(cols);
    scroll = std::max(0, std::min(scroll, maxS));
    int rowStride = L::CARD_H + 18;
    HitScroll(ctx, { L::RAIL_W, rowY - 6, L::W - L::RAIL_W, rows * rowStride }, &scroll, maxS);
    for (size_t i = 0; i < list.size(); ++i) {
        int col = (int)i / rows, row = (int)i % rows;
        int cx = L::CONTENT_X + col * (L::CARD_W + L::CARD_GAP) - scroll;
        if (cx + L::CARD_W < 0 || cx > L::W) continue;
        DrawSongCard(ctx, pm, list[i], cx, rowY + row * rowStride,
                     focusShelf && ctx.focus_visible && sel == (int)i, listId, (int)i, ranks ? (int)i + 1 : 0);
    }
}

// Clickable pill with a label
static void PillButton(AppContext& ctx, int x, int y, int w, int h, const std::string& label,
                       SDL_Color bg, SDL_Color fg, Act act, int a = 0) {
    DrawM3Pill(ctx.renderer, x, y, w, h, bg, COLOR_NONE, h / 2);
    TextC(ctx, ctx.font_normal, label, x + w / 2, y + h / 2, fg);
    Hit(ctx, x, y, w, h, act, a);
}

static void RoundBtn(AppContext& ctx, int cx, int cy, int d, SDL_Color bg, Act act, int a = 0) {
    FillCircle(ctx.renderer, cx, cy, d / 2, bg);
    int hp = std::max(d, 64) / 2;
    Hit(ctx, cx - hp, cy - hp, hp * 2, hp * 2, act, a);
}

// ---------------------------------------------------------------- screens

void RenderCatalog(AppContext& ctx, PosterManager& pm) {
    ctx.hits.clear();
    g_content = false;
    RenderTopBar(ctx, "MiiTune", T("Search YouTube Music"));
    RenderChips(ctx);

    ctx.content_bottom = DockVisible(ctx) ? L::DOCK_Y : L::H;
    int maxY = HomeMaxScrollY(ctx);
    ctx.home_scroll_y = std::max(0, std::min(ctx.home_scroll_y, maxY));

    SDL_Rect band = { 0, L::CONTENT_TOP, L::W, ctx.content_bottom - L::CONTENT_TOP };
    g_content = true;
    HitScroll(ctx, band, &ctx.home_scroll_y, maxY, true);
    SDL_RenderSetClipRect(ctx.renderer, &band);

    HomeSec secs[6];
    int n = BuildHomeSections(ctx, secs);
    for (int i = 0; i < n; i++) {
        int y0 = L::ROW1_TITLE_Y + i * L::SEC_H - ctx.home_scroll_y;
        if (y0 + L::SEC_H < L::CONTENT_TOP || y0 > ctx.content_bottom) continue;
        RenderText(ctx.renderer, ctx.font_normal, secs[i].title, L::CONTENT_X, y0, COLOR_WHITE);
        if (secs[i].list->empty()) {
            const char* msg = secs[i].listId == LIST_CATALOG ? T("Loading tracks...")
                                                             : T("Long-press a track then Favorite, or use the heart in the player.");
            RenderText(ctx.renderer, ctx.font_small, msg, L::CONTENT_X, y0 + 44, COLOR_MUTED);
        } else {
            RenderShelf(ctx, pm, *secs[i].list, secs[i].listId, y0 + 34, 1, *secs[i].sel, *secs[i].scroll,
                        !ctx.rail_focused && ctx.home_row == i);
        }
    }
    SDL_RenderSetClipRect(ctx.renderer, nullptr);
    g_content = false;

    RenderDock(ctx, pm);
    RenderRail(ctx);
}

void RenderSearch(AppContext& ctx, PosterManager& pm) {
    ctx.hits.clear();
    g_content = false;
    RenderTopBar(ctx, T("Search"), ctx.search_query.empty() ? T("Type a title or artist...") : ctx.search_query);
    RenderChips(ctx);

    std::string head;
    if (ctx.search_is_loading) head = T("Searching...");
    else if (ctx.search_results.empty()) head = ctx.search_history.empty() ? T("Results") : T("Recent searches");
    else head = (ctx.search_title.empty() ? std::string(T("Results")) : ctx.search_title)
                + " (" + std::to_string(ctx.search_results.size()) + (ctx.search_loading_more ? "+..." : "") + ")";
    RenderText(ctx.renderer, ctx.font_normal, head, L::CONTENT_X, L::ROW1_TITLE_Y,
               ctx.search_is_loading ? COLOR_PRIMARY : COLOR_WHITE);

    if (!ctx.search_is_loading && ctx.search_results.empty()) {
        if (ctx.search_history.empty()) {
            RenderText(ctx.renderer, ctx.font_normal, T("Touch the search bar to open the Wii U keyboard,"),
                       L::CONTENT_X, L::ROW1_Y + 30, COLOR_WHITE);
            RenderText(ctx.renderer, ctx.font_small, T("or pick a theme above."),
                       L::CONTENT_X, L::ROW1_Y + 66, COLOR_MUTED);
        } else {
            PillButton(ctx, L::W - L::RIGHT_PAD - 170, L::CHIPS_Y + L::CHIPS_H + 6, 170, 52, T("Clear"), COLOR_CARD, COLOR_MUTED, Act::CLEAR_SEARCHES);
            int x = L::CONTENT_X, y = L::ROW1_Y + 28;
            for (size_t i = 0; i < ctx.search_history.size(); ++i) {
                int w = MeasureTextWidth(ctx.font_normal, Fit(ctx.font_normal, ctx.search_history[i], 420)) + 56;
                if (x + w > L::W - L::RIGHT_PAD) { x = L::CONTENT_X; y += 68; }
                if (y + 56 > L::DOCK_Y - 10) break;
                DrawM3Pill(ctx.renderer, x, y, w, 56, COLOR_CHIP, COLOR_NONE, 28);
                RenderText(ctx.renderer, ctx.font_normal, Fit(ctx.font_normal, ctx.search_history[i], 420), x + 28,
                           y + 28 - FontH(ctx.font_normal) / 2, COLOR_WHITE);
                Hit(ctx, x, y, w, 56, Act::HISTORY_SEARCH, (int)i);
                x += w + 12;
            }
        }
    } else if (!ctx.search_results.empty()) {
        RenderShelf(ctx, pm, ctx.search_results, LIST_SEARCH, L::ROW1_Y, 2, ctx.search_selected_idx,
                    ctx.search_scroll_x, !ctx.rail_focused);
    }

    RenderDock(ctx, pm);
    RenderRail(ctx);
}

void RenderPlayer(AppContext& ctx, PosterManager& pm) {
    ctx.hits.clear();
    g_content = false;
    SDL_Renderer* r = ctx.renderer;
    const int cx0 = L::CONTENT_X + 20;
    const int coverS = 300, coverY = 70;
    const int colX = cx0 + coverS + 56, colW = L::W - L::RIGHT_PAD - colX;

    // Cover (rounded, subtle coral halo)
    DrawM3Pill(r, cx0 - 10, coverY - 10, coverS + 20, coverS + 20, { 255, 82, 110, 30 }, COLOR_NONE, 26);
    SDL_Rect cover = { cx0, coverY, coverS, coverS };
    SDL_Texture* tex = GetPosterTexture(ctx, pm, ctx.current_song);
    if (tex) SDL_RenderCopy(r, tex, nullptr, &cover);
    else DrawVinylPlaceholder(r, cover.x, cover.y, cover.w, cover.h, ctx.current_song.title);
    RoundImage(r, cover, 20, BgAt(coverY), BgAt(coverY + coverS));

    // Top-right buttons: queue, screen off
    int bxr = L::W - L::RIGHT_PAD - 28;
    RoundBtn(ctx, bxr, coverY + 28, 56, COLOR_CARD, Act::SCREEN_OFF);
    IconMoon(r, bxr, coverY + 28, 24, COLOR_WHITE, COLOR_CARD);
    RoundBtn(ctx, bxr - 68, coverY + 28, 56, COLOR_CARD, Act::OPEN_QUEUE);
    IconQueue(r, bxr - 68, coverY + 28, 26, COLOR_WHITE);
    RoundBtn(ctx, bxr - 136, coverY + 28, 56, COLOR_CARD, Act::PLAYER_PL_ADD);
    IconPlus(r, bxr - 136, coverY + 28, 24, COLOR_WHITE);

    // Title + channel
    int titleW = colW - 220;
    std::string title = ctx.current_song.title.empty() ? T("No track selected") : ctx.current_song.title;
    TextFit(ctx, ctx.font_title, title, colX, coverY, titleW, COLOR_WHITE);
    std::string creator = ctx.current_song.channelTitle.empty() ? ctx.current_song.artist : ctx.current_song.channelTitle;
    if (creator.empty()) creator = "YouTube Music";
    TextFit(ctx, ctx.font_normal, creator, colX, coverY + 52, titleW, COLOR_CYAN);
    Hit(ctx, colX, coverY + 44, titleW, 48, Act::OPEN_ARTIST_CUR);

    // Lyrics (3 lines), visualizer or help
    int lyrY = coverY + 108, lyrH = 112;
    bool playing = (AudioPlayer::Instance().GetState() == PlaybackState::PLAYING);
    if (ctx.show_lyrics && ctx.current_lyrics.hasLyrics && !ctx.current_lyrics.lines.empty()) {
        DrawM3Pill(r, colX, lyrY, colW, lyrH, COLOR_CARD, COLOR_NONE, 20);
        int posMs = AudioPlayer::Instance().GetCurrentPosition() * 1000;
        int active = 0;
        if (ctx.current_lyrics.isSynced) {
            for (size_t l = 0; l < ctx.current_lyrics.lines.size(); l++) {
                if (ctx.current_lyrics.lines[l].timeMs <= posMs) active = (int)l; else break;
            }
        }
        for (int k = -1; k <= 1; k++) {
            int li = active + k;
            if (li < 0 || li >= (int)ctx.current_lyrics.lines.size()) continue;
            bool cur = (k == 0);
            TTF_Font* f = cur ? ctx.font_normal : ctx.font_small;
            std::string t = Fit(f, ctx.current_lyrics.lines[li].text, colW - 40);
            TextC(ctx, f, t, colX + colW / 2, lyrY + lyrH / 2 + k * 34, cur ? COLOR_PRIMARY : COLOR_MUTED);
        }
    } else if (ctx.show_lyrics) {
        RenderText(r, ctx.font_small, T("Lyrics not available for this track."), colX, lyrY + 40, COLOR_MUTED_DARK);
    } else {
        // Spectrum visualizer
        std::vector<float> bars = AudioPlayer::Instance().GetSpectrumBars(32);
        int n = (int)bars.size(), gap = 6, bw = std::max(4, (colW - (n - 1) * gap) / n);
        for (int i = 0; i < n; i++) {
            float v = playing ? std::min(1.0f, bars[i]) : 0.0f;
            int h = std::max(4, (int)(v * lyrH));
            SDL_Rect bar = { colX + i * (bw + gap), lyrY + lyrH - h, bw, h };
            SetCol(r, { 255, 82, 110, 200 });
            SDL_RenderFillRect(r, &bar);
        }
    }

    // Actions: Lyrics / Favorite / Channel / Last.fm
    int ay = coverY + coverS - 60, ah = 60, ax = colX;
    bool fav = FavoritesManager::Instance().IsFavorite(ctx.current_song.id);
    struct A { const char* label; Act act; int kind; SDL_Color bg; SDL_Color fg; };
    A acts[4] = {
        { ctx.show_lyrics ? T("Lyrics ON") : T("Lyrics"), Act::LYRICS, 0, ctx.show_lyrics ? COLOR_CARD_ACTIVE : COLOR_CARD, COLOR_WHITE },
        { T("Favorite"), Act::FAVORITE, 1, fav ? COLOR_HEART : COLOR_CARD, COLOR_WHITE },
        { T("Channel"), Act::OPEN_ARTIST_CUR, 2, COLOR_CARD, COLOR_CYAN },
        { "Last.fm", Act::LASTFM, 3, COLOR_CARD, COLOR_LASTFM }
    };
    for (int i = 0; i < 4; i++) {
        int w = MeasureTextWidth(ctx.font_normal, acts[i].label) + (i == 3 ? 44 : 84);
        DrawM3Pill(r, ax, ay, w, ah, acts[i].bg, COLOR_NONE, ah / 2);
        int icx = ax + 34, icy = ay + ah / 2;
        int tx = ax + 62;
        if (acts[i].kind == 0) IconLyrics(r, icx, icy, 22, COLOR_WHITE);
        else if (acts[i].kind == 1) IconHeart(r, icx, icy, 24, fav ? COLOR_WHITE : COLOR_HEART, fav, COLOR_CARD);
        else if (acts[i].kind == 2) IconPerson(r, icx, icy, 24, COLOR_CYAN);
        else tx = ax + 22;
        RenderText(r, ctx.font_normal, acts[i].label, tx, icy - FontH(ctx.font_normal) / 2, acts[i].fg);
        Hit(ctx, ax, ay, w, ah, acts[i].act);
        ax += w + 14;
    }

    // Progress bar (tall touch zone)
    int sx = cx0, sw = L::W - L::RIGHT_PAD - sx, sy = 432;
    int cur = AudioPlayer::Instance().GetCurrentPosition();
    int dur = AudioPlayer::Instance().GetDuration();
    if (dur <= 0) dur = std::max(1, ctx.current_song.durationSeconds);
    if (dur <= 0) dur = 210;
    float pct = ctx.play_pending ? 0.0f : std::min(1.0f, std::max(0.0f, (float)cur / (float)dur));
    DrawM3Pill(r, sx, sy, sw, 10, COLOR_CARD_ACTIVE, COLOR_NONE, 5);
    int fw = std::max(10, (int)(sw * pct));
    DrawM3Pill(r, sx, sy, fw, 10, COLOR_PRIMARY, COLOR_NONE, 5);
    FillCircle(r, sx + (int)(sw * pct), sy + 5, 13, COLOR_WHITE);
    Hit(ctx, sx - 16, sy - 30, sw + 32, 70, Act::SEEK_BAR);

    char cs[16], ds[16];
    snprintf(cs, sizeof(cs), "%d:%02d", cur / 60, cur % 60);
    snprintf(ds, sizeof(ds), "%d:%02d", dur / 60, dur % 60);
    RenderText(r, ctx.font_small, cs, sx, sy + 26, COLOR_WHITE);
    if (ctx.play_pending) TextC(ctx, ctx.font_small, T("Loading..."), sx + sw / 2, sy + 36, COLOR_PRIMARY);
    RenderText(r, ctx.font_small, ds, sx + sw - MeasureTextWidth(ctx.font_small, ds), sy + 26, COLOR_MUTED);

    // Transport: shuffle | previous | -10 | play | +10 | next | repeat
    int mid = (L::RAIL_W + L::W) / 2, ty = 552;
    RoundBtn(ctx, mid - 330, ty, 64, ctx.shuffle_enabled ? COLOR_PRIMARY : COLOR_CARD, Act::SHUFFLE);
    IconShuffle(r, mid - 330, ty, 30, COLOR_WHITE);
    RoundBtn(ctx, mid - 236, ty, 64, COLOR_CARD, Act::PREV_TRACK);
    IconTrack(r, mid - 236, ty, 26, COLOR_WHITE, false);
    RoundBtn(ctx, mid - 140, ty, 80, COLOR_CARD, Act::SEEK_REL, -10);
    IconSeek(r, mid - 140, ty - 8, 26, COLOR_WHITE, false);
    TextC(ctx, ctx.font_small, "10", mid - 140, ty + 22, COLOR_WHITE);
    RoundBtn(ctx, mid, ty, 108, COLOR_WHITE, Act::PLAY_PAUSE);
    if (playing) IconPause(r, mid, ty, 40, COLOR_INK); else IconPlay(r, mid + 3, ty, 44, COLOR_INK);
    RoundBtn(ctx, mid + 140, ty, 80, COLOR_CARD, Act::SEEK_REL, 10);
    IconSeek(r, mid + 140, ty - 8, 26, COLOR_WHITE, true);
    TextC(ctx, ctx.font_small, "10", mid + 140, ty + 22, COLOR_WHITE);
    RoundBtn(ctx, mid + 236, ty, 64, COLOR_CARD, Act::NEXT_TRACK);
    IconTrack(r, mid + 236, ty, 26, COLOR_WHITE, true);
    RoundBtn(ctx, mid + 330, ty, 64, ctx.repeat_mode != RepeatMode::OFF ? COLOR_PRIMARY : COLOR_CARD, Act::REPEAT);
    IconRepeat(r, mid + 330, ty, 30, COLOR_WHITE, ctx.repeat_mode == RepeatMode::ONE);

    // Volume
    int vy = 664, vw = 300, vx = mid - vw / 2 + 20;
    IconSpeaker(r, vx - 34, vy + 5, 26, COLOR_MUTED);
    DrawM3Pill(r, vx, vy, vw, 10, COLOR_CARD_ACTIVE, COLOR_NONE, 5);
    int vol = AudioPlayer::Instance().GetVolume();
    int vf = std::max(10, vw * vol / 100);
    DrawM3Pill(r, vx, vy, vf, 10, COLOR_WHITE, COLOR_NONE, 5);
    FillCircle(r, vx + vw * vol / 100, vy + 5, 11, COLOR_WHITE);
    Hit(ctx, vx - 16, vy - 24, vw + 32, 58, Act::VOLUME_BAR);

    // Audio diagnostics (small text bottom right)
    {
        static std::string s_pdbg;
        static uint32_t s_pdbg_t = 0;
        if (SDL_GetTicks() - s_pdbg_t > 500) {
            s_pdbg_t = SDL_GetTicks();
            float bufSec, speed; int und;
            AudioPlayer::Instance().GetDebugInfo(bufSec, speed, und);
            char dbg[96];
            snprintf(dbg, sizeof(dbg), T("buffer %.0fs   decoding x%.1f   dropouts %d"), bufSec, speed, und);
            s_pdbg = dbg;
        }
        if (g_debug_hud) RenderText(r, ctx.font_small, s_pdbg, L::W - L::RIGHT_PAD - MeasureTextWidth(ctx.font_small, s_pdbg), L::H - 26, COLOR_MUTED_DARK);
    }

    RenderRail(ctx);
}

void RenderArtist(AppContext& ctx, PosterManager& pm) {
    ctx.hits.clear();
    g_content = false;
    SDL_Renderer* r = ctx.renderer;
    ArtistProfile& ap = ctx.artist_profile;

    DrawM3Pill(r, L::CONTENT_X, 16, 64, 56, COLOR_CARD, COLOR_NONE, 28);
    IconBack(r, L::CONTENT_X + 32, 44, 26, COLOR_WHITE);
    Hit(ctx, L::CONTENT_X, 16, 64, 56, Act::BACK);

    int nx = L::CONTENT_X + 84, nw = L::W - L::RIGHT_PAD - nx;
    TextFit(ctx, ctx.font_title, ap.name.empty() ? T("Artist") : ap.name, nx, 16, nw, COLOR_WHITE);
    std::string sub = ap.subscribers.empty() ? "YouTube Music" : ap.subscribers;
    if (!ap.channelId.empty()) sub += "   -   youtube.com/channel/" + ap.channelId;
    TextFit(ctx, ctx.font_small, sub, nx, 56, nw, COLOR_CYAN);
    if (!ap.bio.empty())
        TextFit(ctx, ctx.font_small, ap.bio, L::CONTENT_X, 96, L::W - L::RIGHT_PAD - L::CONTENT_X, COLOR_MUTED);

    PillButton(ctx, L::CONTENT_X, 132, 300, 56, T("Shuffle play"), COLOR_PRIMARY, COLOR_WHITE, Act::ARTIST_SHUFFLE);

    RenderText(r, ctx.font_normal, T("Channel tracks"), L::CONTENT_X, 206, COLOR_WHITE);
    int rowY = 244;
    if (ap.isLoading) {
        RenderText(r, ctx.font_normal, T("Loading..."), L::CONTENT_X, rowY + 20, COLOR_PRIMARY);
    } else if (ap.topSongs.empty()) {
        RenderText(r, ctx.font_normal, T("No tracks found for this artist."), L::CONTENT_X, rowY + 20, COLOR_MUTED);
    } else {
        RenderShelf(ctx, pm, ap.topSongs, LIST_ARTIST, rowY, 1, ap.selected_idx, ap.scroll_x, !ctx.rail_focused, true);
    }

    RenderDock(ctx, pm);
    RenderRail(ctx);
}

// ---------------------------------------------------------------- queue

// Song row (80 px): cover, title, channel, duration, [move up], [remove]
static void DrawSongRow(AppContext& ctx, PosterManager& pm, const Song& s, int y, int rw, bool cur, bool foc,
                        int listId, int idx, bool showUp, Act upAct, Act removeAct) {
    SDL_Renderer* r = ctx.renderer;
    SDL_Color base = cur ? SDL_Color{ 70, 34, 48, 255 } : COLOR_CARD;
    DrawM3Pill(r, L::CONTENT_X, y, rw, 80, base, foc ? COLOR_PRIMARY : COLOR_NONE, 20);
    SDL_Rect cv = { L::CONTENT_X + 12, y + 8, 64, 64 };
    SDL_Texture* tex = GetPosterTexture(ctx, pm, s);
    if (tex) SDL_RenderCopy(r, tex, nullptr, &cv);
    else DrawVinylPlaceholder(r, cv.x, cv.y, cv.w, cv.h, s.title);
    RoundImage(r, cv, 10, base, base);
    int tx = cv.x + cv.w + 18, tw = rw - 12 - 64 - 18 - 250;
    TextFit(ctx, ctx.font_normal, s.title, tx, y + 12, tw, cur ? COLOR_PRIMARY : COLOR_WHITE);
    std::string creator = s.channelTitle.empty() ? s.artist : s.channelTitle;
    TextFit(ctx, ctx.font_small, creator, tx, y + 46, tw, COLOR_MUTED);
    if (!s.durationFormatted.empty()) {
        int dw = MeasureTextWidth(ctx.font_small, s.durationFormatted);
        RenderText(r, ctx.font_small, s.durationFormatted, L::CONTENT_X + rw - 132 - dw - 16, y + 30, COLOR_MUTED);
    }
    Hit(ctx, L::CONTENT_X, y, rw, 80, Act::PLAY_SONG, listId, idx);
    int bx = L::CONTENT_X + rw - 66;
    FillCircle(r, bx, y + 40, 26, COLOR_CARD_ACTIVE);
    IconClose(r, bx, y + 40, 20, COLOR_WHITE);
    Hit(ctx, bx - 30, y + 10, 60, 60, removeAct, idx);
    if (showUp) {
        int ux = bx - 66;
        FillCircle(r, ux, y + 40, 26, COLOR_CARD_ACTIVE);
        IconUp(r, ux, y + 40, 22, COLOR_WHITE);
        Hit(ctx, ux - 30, y + 10, 60, 60, upAct, idx);
    }
}


void RenderQueue(AppContext& ctx, PosterManager& pm) {
    ctx.hits.clear();
    g_content = false;
    SDL_Renderer* r = ctx.renderer;

    DrawM3Pill(r, L::CONTENT_X, 16, 64, 56, COLOR_CARD, COLOR_NONE, 28);
    IconBack(r, L::CONTENT_X + 32, 44, 26, COLOR_WHITE);
    Hit(ctx, L::CONTENT_X, 16, 64, 56, Act::BACK);
    RenderText(r, ctx.font_title, T("Queue"), L::CONTENT_X + 84, 16, COLOR_WHITE);
    std::string cnt = std::to_string(ctx.queue.size()) + T(" tracks") + (ctx.radio_loading ? T("  -  loading radio...") : "");
    RenderText(r, ctx.font_small, cnt, L::CONTENT_X + 84, 56, COLOR_MUTED);
    PillButton(ctx, L::W - L::RIGHT_PAD - 150, 16, 150, 56, T("Clear"), COLOR_CARD, COLOR_MUTED, Act::QUEUE_CLEAR);

    ctx.content_bottom = DockVisible(ctx) ? L::DOCK_Y : L::H;
    int maxY = QueueMaxScrollY(ctx);
    ctx.queue_scroll_y = std::max(0, std::min(ctx.queue_scroll_y, maxY));
    SDL_Rect band = { L::RAIL_W, L::QUEUE_TOP - 6, L::W - L::RAIL_W, ctx.content_bottom - (L::QUEUE_TOP - 6) };

    if (ctx.queue.empty()) {
        RenderText(r, ctx.font_normal, T("The queue is empty."), L::CONTENT_X, L::QUEUE_TOP + 20, COLOR_WHITE);
        RenderText(r, ctx.font_small, T("Long-press a track: Play next / Add to queue / Radio."),
                   L::CONTENT_X, L::QUEUE_TOP + 60, COLOR_MUTED);
    } else {
        g_content = true;
        HitScroll(ctx, band, &ctx.queue_scroll_y, maxY, true);
        SDL_RenderSetClipRect(r, &band);
        const int rw = L::W - L::RIGHT_PAD - L::CONTENT_X;
        for (int i = 0; i < (int)ctx.queue.size(); i++) {
            int y = L::QUEUE_TOP + i * L::QUEUE_ROW - ctx.queue_scroll_y;
            if (y + 80 < band.y || y > ctx.content_bottom) continue;
            DrawSongRow(ctx, pm, ctx.queue[i], y, rw, i == ctx.queue_idx, ctx.focus_visible && ctx.queue_sel == i,
                        LIST_QUEUE, i, i > 0, Act::QUEUE_UP, Act::QUEUE_REMOVE);
        }
        SDL_RenderSetClipRect(r, nullptr);
        g_content = false;
    }

    RenderDock(ctx, pm);
    RenderRail(ctx);
}

// ---------------------------------------------------------------- account

static void DrawQrCode(SDL_Renderer* renderer, const std::string& text, int x, int y, int totalSize) {
    if (text.empty() || totalSize <= 0) return;
    static std::string s_last;
    static qr::Code s_qr;
    if (s_last != text) { s_qr = qr::encode(text); s_last = text; }
    if (s_qr.size <= 0) return;

    SDL_Rect bg = { x, y, totalSize, totalSize };
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderFillRect(renderer, &bg);
    int margin = 3, grid = s_qr.size + margin * 2;
    float mod = (float)totalSize / (float)grid;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    for (int rr = 0; rr < s_qr.size; rr++) {
        for (int c = 0; c < s_qr.size; c++) {
            if (!s_qr.at(c, rr)) continue;
            int px = x + (int)((c + margin) * mod), py = y + (int)((rr + margin) * mod);
            SDL_Rect m = { px, py, (int)((c + margin + 1) * mod) - (int)((c + margin) * mod),
                           (int)((rr + margin + 1) * mod) - (int)((rr + margin) * mod) };
            SDL_RenderFillRect(renderer, &m);
        }
    }
}

static void StepBadge(AppContext& ctx, const char* n, int x, int y) {
    FillCircle(ctx.renderer, x + 17, y + 17, 17, COLOR_PRIMARY);
    TextC(ctx, ctx.font_small, n, x + 17, y + 17, COLOR_WHITE);
}

void RenderAccount(AppContext& ctx, PosterManager& pm) {
    ctx.hits.clear();
    g_content = false;
    SDL_Renderer* r = ctx.renderer;
    RenderTopBar(ctx, T("Account"), "", false);

    UserAccount acc = AccountManager::Instance().GetAccount();
    DeviceCodeInfo code = AccountManager::Instance().GetDeviceCodeInfo();
    const int px = L::CONTENT_X, pw = L::W - L::RIGHT_PAD - px;

    if (!acc.is_logged_in) {
        if (!code.active && !code.is_requesting && code.user_code.empty()) {
            AccountManager::Instance().StartDeviceLogin();
            code = AccountManager::Instance().GetDeviceCodeInfo();
        }

        int py = 92, ph = 452;
        DrawM3Pill(r, px, py, pw, ph, COLOR_CARD, COLOR_NONE, 24);

        int qs = 260, qx = px + 60, qy = py + 36;
        if (!code.user_code.empty()) {
            DrawQrCode(r, "https://www.google.com/device?user_code=" + code.user_code, qx, qy, qs);
        } else {
            SDL_Rect q = { qx, qy, qs, qs };
            SetCol(r, { 236, 234, 240, 255 });
            SDL_RenderFillRect(r, &q);
            TextC(ctx, ctx.font_normal, code.is_requesting ? T("Loading...") : T("Code unavailable"), qx + qs / 2, qy + qs / 2, COLOR_INK);
        }
        TextC(ctx, ctx.font_small, T("Scan with your phone"), qx + qs / 2, qy + qs + 30, COLOR_WHITE);

        int dx = px + 380;
        SetCol(r, { 52, 48, 68, 255 });
        SDL_Rect dv = { dx, py + 30, 1, ph - 110 };
        SDL_RenderFillRect(r, &dv);
        DrawM3Pill(r, dx - 24, py + (ph - 80) / 2 - 15, 48, 30, COLOR_CARD, { 52, 48, 68, 255 }, 15);
        TextC(ctx, ctx.font_small, "ou", dx, py + (ph - 80) / 2, COLOR_MUTED);

        int rx = dx + 50, rw = px + pw - 40 - rx;
        int y1 = py + 34;
        StepBadge(ctx, "1", rx, y1);
        RenderText(r, ctx.font_normal, T("Go to"), rx + 50, y1 + 3, COLOR_WHITE);
        DrawM3Pill(r, rx + 150, y1 - 6, 300, 46, { 14, 16, 21, 255 }, COLOR_CYAN, 12);
        TextC(ctx, ctx.font_normal, "google.com/device", rx + 300, y1 + 17, COLOR_CYAN);

        int y2 = y1 + 76;
        StepBadge(ctx, "2", rx, y2);
        RenderText(r, ctx.font_normal, T("Enter this code:"), rx + 50, y2 + 3, COLOR_WHITE);
        DrawM3Pill(r, rx, y2 + 50, rw, 96, { 14, 16, 21, 255 }, COLOR_PRIMARY, 16);
        std::string spaced;
        if (code.is_requesting && code.user_code.empty()) spaced = T("LOADING...");
        else if (!code.user_code.empty()) {
            for (size_t i = 0; i < code.user_code.size(); ++i) { spaced += code.user_code[i]; if (i + 1 < code.user_code.size()) spaced += "  "; }
        } else spaced = "...";
        TextC(ctx, ctx.font_title, spaced, rx + rw / 2, y2 + 98, COLOR_WHITE);

        int y3 = y2 + 172;
        StepBadge(ctx, "3", rx, y3);
        TextFit(ctx, ctx.font_normal, T("Choose your Google account, then Allow."), rx + 50, y3 + 3, rw - 50, COLOR_WHITE);
        TextFit(ctx, ctx.font_small, T("Your playlists sync automatically."), rx + 50, y3 + 38, rw - 50, COLOR_MUTED);

        int by = py + ph - 56;
        SetCol(r, { 52, 48, 68, 255 });
        SDL_Rect ln = { px + 24, by, pw - 48, 1 };
        SDL_RenderFillRect(r, &ln);
        SDL_Color dot = code.is_requesting ? COLOR_CYAN : (code.active && !code.user_code.empty() ? COLOR_GREEN : COLOR_PRIMARY);
        FillCircle(r, px + 44, by + 28, 6, dot);
        TextFit(ctx, ctx.font_small, code.status_msg.empty() ? T("Waiting for approval...") : code.status_msg,
                px + 64, by + 28 - FontH(ctx.font_small) / 2, pw / 2, COLOR_MUTED);
        uint64_t now = (uint64_t)time(nullptr);
        if (code.expires_at > now) {
            char b[40];
            int rem = (int)(code.expires_at - now);
            snprintf(b, sizeof(b), T("Code valid for: %02d:%02d"), rem / 60, rem % 60);
            RenderText(r, ctx.font_small, b, px + pw - 30 - MeasureTextWidth(ctx.font_small, b), by + 28 - FontH(ctx.font_small) / 2, COLOR_CYAN);
        }

        int yb = py + ph + 14, hb = 60;
        RenderText(r, ctx.font_small, T("Google shares no password with MiiTune."), px + 4, yb + 20, COLOR_MUTED);
        PillButton(ctx, px + pw - 190, yb, 190, hb, T("Cancel"), COLOR_CARD, COLOR_MUTED, Act::ACC_CANCEL);
        PillButton(ctx, px + pw - 190 - 16 - 250, yb, 250, hb, T("New code"), COLOR_CARD_ACTIVE, COLOR_WHITE, Act::ACC_NEW_CODE);
    } else {
        int py = 92, ph = 96;
        DrawM3Pill(r, px, py, pw, ph, COLOR_CARD, COLOR_NONE, 24);
        FillCircle(r, px + 56, py + ph / 2, 30, COLOR_PRIMARY);
        std::string ini = acc.user_name.empty() ? "Y" : acc.user_name.substr(0, 1);
        TextC(ctx, ctx.font_title, ini, px + 56, py + ph / 2, COLOR_WHITE);
        int tx = px + 104, tw = pw - 104 - 470;
        TextFit(ctx, ctx.font_title, acc.user_name.empty() ? T("YouTube user") : acc.user_name, tx, py + 14, tw, COLOR_WHITE);
        TextFit(ctx, ctx.font_small, acc.user_email.empty() ? T("Signed in with Google") : acc.user_email, tx, py + 56, tw, COLOR_MUTED);
        PillButton(ctx, px + pw - 20 - 200, py + 20, 200, 56, T("Sign out"), { 180, 30, 44, 255 }, COLOR_WHITE, Act::ACC_LOGOUT);
        PillButton(ctx, px + pw - 20 - 200 - 14 - 210, py + 20, 210, 56, T("Refresh"), COLOR_CARD_ACTIVE, COLOR_WHITE, Act::ACC_REFRESH);

        RenderText(r, ctx.font_normal, T("Your playlists"), px, 208, COLOR_WHITE);
        RenderText(r, ctx.font_small, T("Touch a playlist to start listening."), px, 242, COLOR_MUTED);

        int cy0 = 282, cw = 200, gap = 20;
        if (acc.is_loading_playlists) {
            RenderText(r, ctx.font_normal, T("Loading playlists..."), px, cy0 + 20, COLOR_CYAN);
        } else if (acc.playlists.empty()) {
            RenderText(r, ctx.font_normal, T("No playlists found."), px, cy0 + 20, COLOR_MUTED);
        } else {
            int maxS = std::max(0, (int)acc.playlists.size() * (cw + gap) - gap - L::VISIBLE_W);
            ctx.account_scroll_x = std::max(0, std::min(ctx.account_scroll_x, maxS));
            HitScroll(ctx, { L::RAIL_W, cy0 - 6, L::W - L::RAIL_W, cw + 90 }, &ctx.account_scroll_x, maxS);
            for (size_t i = 0; i < acc.playlists.size(); ++i) {
                const Playlist& pl = acc.playlists[i];
                int cx = px + (int)i * (cw + gap) - ctx.account_scroll_x;
                if (cx + cw < 0 || cx > L::W) continue;
                bool sel = ctx.focus_visible && ctx.account_selected_playlist_idx == (int)i;
                if (sel) DrawM3Pill(r, cx - 4, cy0 - 4, cw + 8, cw + 8, COLOR_PRIMARY, COLOR_NONE, 18);
                if (!pl.thumbnailUrl.empty()) pm.queue_poster(pl.id, pl.thumbnailUrl);
                SDL_Rect pr = { cx, cy0, cw, cw };
                auto it = ctx.poster_cache.find(pl.id);
                if (it != ctx.poster_cache.end() && it->second) SDL_RenderCopy(r, it->second, nullptr, &pr);
                else DrawVinylPlaceholder(r, cx, cy0, cw, cw, pl.title);
                SDL_Color mc = sel ? COLOR_PRIMARY : BgAt(cy0 + cw / 2);
                RoundImage(r, pr, 14, mc, mc);
                std::string cnt = std::to_string(pl.itemCount) + T(" tracks");
                int bw = MeasureTextWidth(ctx.font_small, cnt) + 16;
                DrawM3Pill(r, cx + 8, cy0 + cw - 34, bw, 26, { 0, 0, 0, 200 }, COLOR_NONE, 8);
                RenderText(r, ctx.font_small, cnt, cx + 16, cy0 + cw - 34 + (26 - FontH(ctx.font_small)) / 2, COLOR_WHITE);
                TextFit(ctx, ctx.font_normal, pl.title, cx, cy0 + cw + 8, cw, COLOR_WHITE);
                Hit(ctx, cx, cy0, cw, cw + 44, Act::ACC_PLAYLIST, (int)i);
            }
        }
    }

    RenderDock(ctx, pm);
    RenderRail(ctx);
}

// ---------------------------------------------------------------- settings

void RenderSettings(AppContext& ctx, PosterManager& pm) {
    ctx.hits.clear();
    g_content = false;
    SDL_Renderer* r = ctx.renderer;
    RenderTopBar(ctx, T("Settings"), "", false);

    std::string lfmAuth = LastFm::AuthUrl();
    bool lfmPanel = !lfmAuth.empty();
    const int px = L::CONTENT_X;
    const int rowW = lfmPanel ? 740 : (L::W - L::RIGHT_PAD - px);

    std::string mode = ctx.settings.search_mode == "songs" ? T("Songs") : ctx.settings.search_mode == "videos" ? T("Videos") : T("All");
    std::string sleep = ctx.settings.sleep_min == 0 ? T("Off") : std::to_string(ctx.settings.sleep_min) + " min";
    if (ctx.sleep_deadline_ms > SDL_GetTicks()) {
        int rem = (int)((ctx.sleep_deadline_ms - SDL_GetTicks()) / 60000) + 1;
        sleep = std::to_string(rem) + T(" min left");
    }
    struct Row { const char* label; std::string value; Act act; SDL_Color valCol; };
    Row rows[9] = {
        { T("Language"), LangName(g_lang), Act::SET_LANGUAGE, COLOR_CYAN },
        { T("Trending region"),            ctx.settings.region,                      Act::SET_REGION,       COLOR_CYAN },
        { T("Search type"),               mode,                                     Act::SET_SEARCHMODE,   COLOR_CYAN },
        { T("Autoplay (radio)"),     ctx.settings.autoplay ? T("On") : T("Off"), Act::SET_AUTOPLAY, ctx.settings.autoplay ? COLOR_GREEN : COLOR_MUTED },
        { T("Sleep timer"),            sleep,                                    Act::SET_SLEEP,        COLOR_CYAN },
        { T("Turn off screen (music keeps playing)"), T("Turn off"),                           Act::SCREEN_OFF,       COLOR_PRIMARY },
        { "Last.fm",                         LastFm::Status(),                         Act::LFM_ACTION,       COLOR_LASTFM },
        { T("Clear listening history"),   T("Clear"),                                Act::CLEAR_HISTORY,    COLOR_MUTED },
        { T("Clear recent searches"), T("Clear"),                                Act::CLEAR_SEARCH_HISTORY, COLOR_MUTED }
    };
    for (int i = 0; i < 9; i++) {
        int y = 92 + i * 58;
        bool foc = ctx.focus_visible && ctx.settings_sel == i;
        DrawM3Pill(r, px, y, rowW, 52, COLOR_CARD, foc ? COLOR_PRIMARY : COLOR_NONE, 18);
        RenderText(r, ctx.font_normal, rows[i].label, px + 24, y + 26 - FontH(ctx.font_normal) / 2, COLOR_WHITE);
        std::string v = Fit(ctx.font_normal, rows[i].value, rowW / 2 - 24);
        RenderText(r, ctx.font_normal, v, px + rowW - 24 - MeasureTextWidth(ctx.font_normal, v),
                   y + 26 - FontH(ctx.font_normal) / 2, rows[i].valCol);
        Hit(ctx, px, y, rowW, 52, rows[i].act);
    }

    if (lfmPanel) {
        int qx = px + rowW + 24, qw = L::W - L::RIGHT_PAD - qx;
        DrawM3Pill(r, qx, 92, qw, 420, COLOR_CARD, COLOR_NONE, 24);
        RenderText(r, ctx.font_normal, T("Last.fm sign-in"), qx + 24, 108, COLOR_WHITE);
        int qs = std::min(240, qw - 48);
        DrawQrCode(r, lfmAuth, qx + (qw - qs) / 2, 154, qs);
        TextC(ctx, ctx.font_small, T("Scan, allow, then confirm"), qx + qw / 2, 154 + qs + 26, COLOR_MUTED);
        PillButton(ctx, qx + 24, 154 + qs + 52, qw - 48, 56, T("I have allowed it"), COLOR_PRIMARY, COLOR_WHITE, Act::LFM_ACTION);
    }

    RenderDock(ctx, pm);
    RenderRail(ctx);
}

// ---------------------------------------------------------------- overlays (menu, toast, touch feedback)

void RenderOverlays(AppContext& ctx, PosterManager& pm) {
    SDL_Renderer* r = ctx.renderer;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    if (ctx.menu_open) {
        // The background swallows taps (closes the menu): its regions sit above the screen
        Hit(ctx, 0, 0, L::W, L::H, Act::MENU_CLOSE);
        SetCol(r, { 0, 0, 0, 170 });
        SDL_Rect all = { 0, 0, L::W, L::H };
        SDL_RenderFillRect(r, &all);

        const int pw = 540, rowH = 64, ph = 96 + 7 * rowH + 16;
        int x = (L::W - pw) / 2, y = (L::H - ph) / 2;
        DrawM3Pill(r, x, y, pw, ph, COLOR_CARD, COLOR_CARD_ACTIVE, 26);
        Hit(ctx, x, y, pw, ph, Act::NOP);

        SDL_Rect cv = { x + 20, y + 16, 64, 64 };
        SDL_Texture* tex = GetPosterTexture(ctx, pm, ctx.menu_song);
        if (tex) SDL_RenderCopy(r, tex, nullptr, &cv);
        else DrawVinylPlaceholder(r, cv.x, cv.y, cv.w, cv.h, ctx.menu_song.title);
        RoundImage(r, cv, 10, COLOR_CARD, COLOR_CARD);
        TextFit(ctx, ctx.font_normal, ctx.menu_song.title, x + 100, y + 20, pw - 120, COLOR_WHITE);
        std::string cr = ctx.menu_song.channelTitle.empty() ? ctx.menu_song.artist : ctx.menu_song.channelTitle;
        TextFit(ctx, ctx.font_small, cr, x + 100, y + 52, pw - 120, COLOR_MUTED);

        bool fav = FavoritesManager::Instance().IsFavorite(ctx.menu_song.id);
        struct M { const char* label; Act act; int icon; };
        M items[7] = {
            { T("Play now"),       Act::MENU_PLAY,    0 },
            { T("Play next"),         Act::MENU_NEXT,    1 },
            { T("Add to queue"),     Act::MENU_QUEUE,   2 },
            { fav ? T("Remove from favorites") : T("Add to favorites"), Act::MENU_FAV, 3 },
            { T("View channel"),        Act::MENU_CHANNEL, 4 },
            { T("Start radio"),       Act::MENU_RADIO,   5 },
            { T("Add to a playlist"), Act::MENU_PLAYLIST, 6 }
        };
        for (int i = 0; i < 7; i++) {
            int ry = y + 96 + i * rowH;
            bool foc = ctx.focus_visible && ctx.menu_sel == i;
            if (foc) DrawM3Pill(r, x + 12, ry, pw - 24, rowH - 6, COLOR_CARD_ACTIVE, COLOR_PRIMARY, 18);
            int icx = x + 52, icy = ry + (rowH - 6) / 2;
            switch (items[i].icon) {
                case 0: IconPlay(r, icx + 2, icy, 22, COLOR_WHITE); break;
                case 1: IconTrack(r, icx, icy, 22, COLOR_WHITE, true); break;
                case 2: IconListEnd(r, icx, icy, 24, COLOR_WHITE); break;
                case 3: IconHeart(r, icx, icy, 24, COLOR_HEART, fav, COLOR_CARD); break;
                case 4: IconPerson(r, icx, icy, 24, COLOR_CYAN); break;
                case 6: IconListPlus(r, icx, icy, 24, COLOR_WHITE); break;
                default: IconRadio(r, icx, icy, 24, COLOR_WHITE); break;
            }
            RenderText(r, ctx.font_normal, items[i].label, x + 92, icy - FontH(ctx.font_normal) / 2, COLOR_WHITE);
            Hit(ctx, x + 12, ry, pw - 24, rowH - 6, items[i].act);
        }
    }


    if (ctx.picker_open) {
        Hit(ctx, 0, 0, L::W, L::H, Act::PICKER_CLOSE);
        SetCol(r, { 0, 0, 0, 170 });
        SDL_Rect all = { 0, 0, L::W, L::H };
        SDL_RenderFillRect(r, &all);

        const int pw = 540, rowH = 64;
        int n = 1 + (int)ctx.playlists.size();
        int visible = std::min(n, 6);
        int ph = 96 + visible * rowH + 16;
        int x = (L::W - pw) / 2, y = (L::H - ph) / 2;
        DrawM3Pill(r, x, y, pw, ph, COLOR_CARD, COLOR_CARD_ACTIVE, 26);
        Hit(ctx, x, y, pw, ph, Act::NOP);
        RenderText(r, ctx.font_normal, T("Add to a playlist"), x + 28, y + 18, COLOR_WHITE);
        TextFit(ctx, ctx.font_small, ctx.picker_song.title, x + 28, y + 54, pw - 56, COLOR_MUTED);

        SDL_Rect band = { x, y + 88, pw, visible * rowH };
        int maxS = std::max(0, n * rowH - visible * rowH);
        ctx.picker_scroll = std::max(0, std::min(ctx.picker_scroll, maxS));
        if (maxS > 0) HitScroll(ctx, band, &ctx.picker_scroll, maxS, true);
        SDL_RenderSetClipRect(r, &band);
        for (int i = 0; i < n; i++) {
            int ry = band.y + i * rowH - ctx.picker_scroll;
            if (ry + rowH < band.y || ry > band.y + band.h) continue;
            bool foc = ctx.focus_visible && ctx.picker_sel == i;
            if (foc) DrawM3Pill(r, x + 12, ry, pw - 24, rowH - 6, COLOR_CARD_ACTIVE, COLOR_PRIMARY, 18);
            int cy = ry + (rowH - 6) / 2;
            if (i == 0) {
                IconPlus(r, x + 52, cy, 24, COLOR_PRIMARY);
                RenderText(r, ctx.font_normal, T("New playlist"), x + 92, cy - FontH(ctx.font_normal) / 2, COLOR_PRIMARY);
                Hit(ctx, x + 12, ry, pw - 24, rowH - 6, Act::PICK_PLAYLIST, -1);
            } else {
                const LocalPlaylist& pl = ctx.playlists[i - 1];
                IconQueue(r, x + 52, cy, 24, COLOR_WHITE);
                std::string cnt = std::to_string(pl.songs.size()) + T(" tracks");
                int cw = MeasureTextWidth(ctx.font_small, cnt);
                TextFit(ctx, ctx.font_normal, pl.name, x + 92, cy - FontH(ctx.font_normal) / 2, pw - 92 - cw - 56, COLOR_WHITE);
                RenderText(r, ctx.font_small, cnt, x + pw - 28 - cw, cy - FontH(ctx.font_small) / 2, COLOR_MUTED);
                Hit(ctx, x + 12, ry, pw - 24, rowH - 6, Act::PICK_PLAYLIST, i - 1);
            }
        }
        SDL_RenderSetClipRect(r, nullptr);
    }

    if (!ctx.toast.empty() && SDL_GetTicks() < ctx.toast_until) {
        int tw = MeasureTextWidth(ctx.font_normal, ctx.toast) + 56;
        tw = std::min(tw, 900);
        int bottom = DockVisible(ctx) ? L::DOCK_Y : L::H;
        int x = (L::W + L::RAIL_W - tw) / 2, y = bottom - 84;
        DrawM3Pill(r, x, y, tw, 56, { 40, 36, 54, 245 }, COLOR_CARD_ACTIVE, 28);
        TextC(ctx, ctx.font_normal, Fit(ctx.font_normal, ctx.toast, 840), x + tw / 2, y + 28, COLOR_WHITE);
    }

    // Touch diagnostics (shown with debug.txt)
    if (g_debug_hud && ctx.touch_dbg[0]) {
        int tw = MeasureTextWidth(ctx.font_small, ctx.touch_dbg);
        int by = (DockVisible(ctx) ? L::DOCK_Y : L::H) - 30;
        DrawM3Pill(r, L::CONTENT_X - 8, by, tw + 16, 26, { 0, 0, 0, 170 }, COLOR_NONE, 8);
        RenderText(r, ctx.font_small, ctx.touch_dbg, L::CONTENT_X, by + (26 - FontH(ctx.font_small)) / 2, COLOR_MUTED);
    }

    if (ctx.press_valid) {
        DrawM3Pill(r, ctx.press_rect.x, ctx.press_rect.y, ctx.press_rect.w, ctx.press_rect.h,
                   { 255, 255, 255, 38 }, COLOR_NONE, 18);
    }
}


// ---------------------------------------------------------------- local playlists

int PlaylistViewMaxScrollY(AppContext& c) {
    int visible = c.content_bottom - (L::PL_ROWS_TOP - 6);
    return std::max(0, (int)c.pl_songs.size() * L::QUEUE_ROW - visible + 8);
}

void RenderPlaylists(AppContext& ctx, PosterManager& pm) {
    ctx.hits.clear();
    g_content = false;
    SDL_Renderer* r = ctx.renderer;

    RenderText(r, ctx.font_title, T("Playlists"), L::CONTENT_X, 16 + (56 - FontH(ctx.font_title)) / 2, COLOR_WHITE);
    PillButton(ctx, L::W - L::RIGHT_PAD - 320, 14, 320, 56, T("+ New playlist"), COLOR_PRIMARY, COLOR_WHITE, Act::PL_NEW);

    int n = (int)ctx.playlists.size();
    if (n == 0) {
        RenderText(r, ctx.font_normal, T("No playlists yet."), L::CONTENT_X, 120, COLOR_WHITE);
        RenderText(r, ctx.font_small, T("Touch \"New playlist\", or long-press a track then \"Add to a playlist\"."),
                   L::CONTENT_X, 160, COLOR_MUTED);
    } else {
        const int rowY = 100, stride = L::CARD_H + 18;
        int cols = (n + 1) / 2;
        int maxS = ShelfMaxScroll(cols);
        ctx.pl_scroll_x = std::max(0, std::min(ctx.pl_scroll_x, maxS));
        HitScroll(ctx, { L::RAIL_W, rowY - 6, L::W - L::RAIL_W, 2 * stride }, &ctx.pl_scroll_x, maxS);
        for (int i = 0; i < n; i++) {
            int col = i / 2, row = i % 2;
            int x = L::CONTENT_X + col * (L::CARD_W + L::CARD_GAP) - ctx.pl_scroll_x;
            if (x + L::CARD_W < 0 || x > L::W) continue;
            int y = rowY + row * stride;
            const LocalPlaylist& pl = ctx.playlists[i];
            bool foc = ctx.focus_visible && !ctx.rail_focused && ctx.pl_sel == i;
            if (foc) DrawM3Pill(r, x - 4, y - 4, L::CARD_W + 8, L::POSTER_H + 8, COLOR_PRIMARY, COLOR_NONE, 18);
            SDL_Rect pr = { x, y, L::CARD_W, L::POSTER_H };
            SDL_Texture* tex = pl.songs.empty() ? nullptr : GetPosterTexture(ctx, pm, pl.songs[0]);
            if (tex) SDL_RenderCopy(r, tex, nullptr, &pr);
            else DrawVinylPlaceholder(r, x, y, L::CARD_W, L::POSTER_H, pl.name);
            RoundImage(r, pr, 14, foc ? COLOR_PRIMARY : BgAt(y), foc ? COLOR_PRIMARY : BgAt(y + L::POSTER_H));
            IconQueue(r, x + L::CARD_W - 34, y + 30, 26, COLOR_WHITE);
            TextFit(ctx, ctx.font_normal, pl.name, x, y + L::POSTER_H + 6, L::CARD_W, COLOR_WHITE);
            RenderText(r, ctx.font_small, std::to_string(pl.songs.size()) + T(" tracks"), x, y + L::POSTER_H + 38, COLOR_MUTED);
            Hit(ctx, x, y, L::CARD_W, L::CARD_H, Act::PL_OPEN, i);
        }
    }

    RenderDock(ctx, pm);
    RenderRail(ctx);
}

void RenderPlaylistView(AppContext& ctx, PosterManager& pm) {
    ctx.hits.clear();
    g_content = false;
    SDL_Renderer* r = ctx.renderer;
    std::string name = (ctx.pl_open >= 0 && ctx.pl_open < (int)ctx.playlists.size()) ? ctx.playlists[ctx.pl_open].name : T("Playlist");

    DrawM3Pill(r, L::CONTENT_X, 16, 64, 56, COLOR_CARD, COLOR_NONE, 28);
    IconBack(r, L::CONTENT_X + 32, 44, 26, COLOR_WHITE);
    Hit(ctx, L::CONTENT_X, 16, 64, 56, Act::BACK);
    int nx = L::CONTENT_X + 84, nw = L::W - L::RIGHT_PAD - nx;
    TextFit(ctx, ctx.font_title, name, nx, 12, nw, COLOR_WHITE);
    RenderText(r, ctx.font_small, std::to_string(ctx.pl_songs.size()) + T(" tracks"), nx, 52, COLOR_MUTED);

    PillButton(ctx, L::CONTENT_X, 84, 180, 56, T("Play"), COLOR_PRIMARY, COLOR_WHITE, Act::PL_PLAY);
    PillButton(ctx, L::CONTENT_X + 196, 84, 230, 56, T("Shuffle"), COLOR_CARD_ACTIVE, COLOR_WHITE, Act::PL_SHUFFLE);
    PillButton(ctx, L::W - L::RIGHT_PAD - 250, 84, 250, 56, T("Delete"), { 180, 30, 44, 255 }, COLOR_WHITE, Act::PL_DELETE);

    ctx.content_bottom = DockVisible(ctx) ? L::DOCK_Y : L::H;
    int maxY = PlaylistViewMaxScrollY(ctx);
    ctx.pl_view_scroll_y = std::max(0, std::min(ctx.pl_view_scroll_y, maxY));
    SDL_Rect band = { L::RAIL_W, L::PL_ROWS_TOP - 6, L::W - L::RAIL_W, ctx.content_bottom - (L::PL_ROWS_TOP - 6) };

    if (ctx.pl_songs.empty()) {
        RenderText(r, ctx.font_normal, T("This playlist is empty."), L::CONTENT_X, L::PL_ROWS_TOP + 20, COLOR_WHITE);
        RenderText(r, ctx.font_small, T("Long-press a track then \"Add to a playlist\"."), L::CONTENT_X, L::PL_ROWS_TOP + 60, COLOR_MUTED);
    } else {
        g_content = true;
        HitScroll(ctx, band, &ctx.pl_view_scroll_y, maxY, true);
        SDL_RenderSetClipRect(r, &band);
        const int rw = L::W - L::RIGHT_PAD - L::CONTENT_X;
        for (int i = 0; i < (int)ctx.pl_songs.size(); i++) {
            int y = L::PL_ROWS_TOP + i * L::QUEUE_ROW - ctx.pl_view_scroll_y;
            if (y + 80 < band.y || y > ctx.content_bottom) continue;
            bool cur = (ctx.pl_songs[i].id == ctx.current_song.id);
            DrawSongRow(ctx, pm, ctx.pl_songs[i], y, rw, cur, ctx.focus_visible && ctx.pl_view_sel == i,
                        LIST_PLAYLIST, i, false, Act::NONE, Act::PL_REMOVE);
        }
        SDL_RenderSetClipRect(r, nullptr);
        g_content = false;
    }

    RenderDock(ctx, pm);
    RenderRail(ctx);
}


// ---------------------------------------------------------------- language selection (first launch)

void RenderLanguage(AppContext& ctx, PosterManager&) {
    ctx.hits.clear();
    g_content = false;
    SDL_Renderer* r = ctx.renderer;

    const char* titles[LANG_COUNT] = { "Choisis ta langue", "Choose your language", "Elige tu idioma" };
    int cx = L::W / 2;
    for (int i = 0; i < LANG_COUNT; i++)
        TextC(ctx, i == 0 ? ctx.font_title : ctx.font_normal, titles[i], cx, 96 + i * 44, i == 0 ? COLOR_WHITE : COLOR_MUTED);

    const int bw = 520, bh = 84;
    for (int i = 0; i < LANG_COUNT; i++) {
        int y = 262 + i * 112;
        bool foc = ctx.focus_visible && ctx.lang_sel == i;
        DrawM3Pill(r, cx - bw / 2, y, bw, bh, COLOR_CARD, foc ? COLOR_PRIMARY : COLOR_NONE, bh / 2);
        TextC(ctx, ctx.font_title, LangName(i), cx, y + bh / 2, COLOR_WHITE);
        Hit(ctx, cx - bw / 2, y, bw, bh, Act::LANG_SET, i);
    }
    TextC(ctx, ctx.font_small, "Settings > Language", cx, 262 + 3 * 112 + 8, COLOR_MUTED_DARK);
}
