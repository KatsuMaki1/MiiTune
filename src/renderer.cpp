#include "renderer.h"
#include "threadutil.h"
#include "account.h"
#include "favorites.h"
#include "qr.h"
#include <algorithm>
#include <vector>
#include <cmath>
#include <cstring>
#include <iostream>

#include "embedded_assets.hpp"

bool g_debug_hud = false;

static SDL_Texture* s_circle_tex = nullptr;
static SDL_Texture* s_corner_tex = nullptr;
static SDL_Texture* s_white_tex = nullptr;

struct CachedText {
    SDL_Texture* texture = nullptr;
    int w = 0;
    int h = 0;
    uint32_t last_used_frame = 0;
};

static std::map<std::string, CachedText> s_text_cache;
static uint32_t s_frame_counter = 0;
static TTF_Font* s_embedded_font = nullptr;

static TTF_Font* GetSafeFont(TTF_Font* font) {
    if (font) return font;
    if (!s_embedded_font) {
        s_embedded_font = TTF_OpenFontRW(SDL_RWFromConstMem(EMBEDDED_FONT_DATA, EMBEDDED_FONT_SIZE), 1, 16);
    }
    return s_embedded_font;
}

void ClearTextCache() {
    for (auto& pair : s_text_cache) {
        if (pair.second.texture) {
            SDL_DestroyTexture(pair.second.texture);
        }
    }
    s_text_cache.clear();
    if (s_circle_tex) { SDL_DestroyTexture(s_circle_tex); s_circle_tex = nullptr; }
    if (s_corner_tex) { SDL_DestroyTexture(s_corner_tex); s_corner_tex = nullptr; }
    if (s_white_tex) { SDL_DestroyTexture(s_white_tex); s_white_tex = nullptr; }
    if (s_embedded_font) {
        TTF_CloseFont(s_embedded_font);
        s_embedded_font = nullptr;
    }
}

void SetScreenFormat(AppContext& ctx, ScreenFormat fmt) {
    // Single 1280x720 layout: the DRC receives the same image scaled down
    (void)fmt;
    ctx.screen_format = FORMAT_TV;
    ctx.screen_w = L::W;
    ctx.screen_h = L::H;
    if (ctx.renderer) {
        // No SDL_RenderSetLogicalSize: its letterboxing shifted the whole display on the GamePad view.
        // We draw in 1280x720 and simply scale to the real output size.
        SDL_RenderSetLogicalSize(ctx.renderer, 0, 0);
        int ow = ctx.screen_w, oh = ctx.screen_h;
        SDL_GetRendererOutputSize(ctx.renderer, &ow, &oh);
        if (ow <= 0 || oh <= 0) { ow = ctx.screen_w; oh = ctx.screen_h; }
        SDL_RenderSetViewport(ctx.renderer, nullptr);
        SDL_RenderSetScale(ctx.renderer, (float)ow / (float)ctx.screen_w, (float)oh / (float)ctx.screen_h);
        SDL_Log("[MiiTune] sortie %dx%d, echelle %.3f x %.3f", ow, oh, (float)ow / ctx.screen_w, (float)oh / ctx.screen_h);
    }
}

void AdvanceRendererFrame() {
    s_frame_counter++;
    if (s_text_cache.size() >= 80) {
        for (auto cit = s_text_cache.begin(); cit != s_text_cache.end();) {
            if (s_frame_counter - cit->second.last_used_frame > 60) {
                if (cit->second.texture) SDL_DestroyTexture(cit->second.texture);
                cit = s_text_cache.erase(cit);
            } else {
                ++cit;
            }
        }
    }
}

static std::string SanitizeString(const std::string& input) {
    std::string clean;
    clean.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        unsigned char c = (unsigned char)input[i];
        if (c < 32 && c != '\n' && c != '\t') continue;
        clean += (char)c;
    }
    return clean;
}

void RenderText(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, int x, int y, SDL_Color color) {
    if (!renderer || text.empty()) return;

    TTF_Font* useFont = GetSafeFont(font);
    if (!useFont) return;

    std::string safeText = SanitizeString(text);
    if (safeText.empty()) return;

    char key_buf[64];
    snprintf(key_buf, sizeof(key_buf), "%p_%02x%02x%02x#", (void*)useFont, color.r, color.g, color.b);
    std::string key = std::string(key_buf) + safeText;

    auto it = s_text_cache.find(key);
    if (it != s_text_cache.end()) {
        it->second.last_used_frame = s_frame_counter;
        SDL_Rect dst = { x, y, it->second.w, it->second.h };
        SDL_RenderCopy(renderer, it->second.texture, nullptr, &dst);
        return;
    }

    SDL_Surface* surf = TTF_RenderUTF8_Blended(useFont, safeText.c_str(), color);
    if (!surf) return;

    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (tex) {
        CachedText entry;
        entry.texture = tex;
        entry.w = surf->w;
        entry.h = surf->h;
        entry.last_used_frame = s_frame_counter;
        s_text_cache[key] = entry;

        SDL_Rect dst = { x, y, surf->w, surf->h };
        SDL_RenderCopy(renderer, tex, nullptr, &dst);
    }
    SDL_FreeSurface(surf);
}

int MeasureTextWidth(TTF_Font* font, const std::string& text) {
    if (text.empty()) return 0;
    TTF_Font* useFont = GetSafeFont(font);
    if (!useFont) return (int)text.length() * 9;
    // TTF_SizeUTF8 is expensive and called dozens of times per frame: small cache
    static std::map<std::string, int> cache;
    char kb[24];
    snprintf(kb, sizeof(kb), "%p#", (void*)useFont);
    std::string key = std::string(kb) + text;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 1500) cache.clear();
    std::string safeText = SanitizeString(text);
    int w = 0, h = 0;
    TTF_SizeUTF8(useFont, safeText.c_str(), &w, &h);
    cache[key] = w;
    return w;
}

// PosterManager implementation
static int PosterWorkerEntry(void* ptr) {
    auto* pm = (PosterManager*)ptr;
    if (pm) pm->worker_loop();
    return 0;
}

void PosterManager::init() {
    m_running = true;
    m_worker = SDL_CreateThreadWithStackSize(PosterWorkerEntry, "PosterWorker", 512 * 1024, this);
}

void PosterManager::shutdown() {
    m_running = false;
    m_cv.notify_all();
    if (m_worker) {
        int status = 0;
        SDL_WaitThread(m_worker, &status);
        m_worker = nullptr;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& item : m_ready_surfaces) {
        if (item.second) SDL_FreeSurface(item.second);
    }
    m_ready_surfaces.clear();
}

void PosterManager::queue_poster(const std::string& id, const std::string& url) {
    if (id.empty() || url.empty()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queued_ids.count(id)) return;
    m_queued_ids.insert(id);
    m_queue.push_back({ id, url });
    m_cv.notify_one();
}

void PosterManager::update_textures(AppContext& ctx) {
    // Max 2 textures created per frame: otherwise 20 posters arriving at once freeze the screen
    std::vector<std::pair<std::string, SDL_Surface*>> finished;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t n = std::min<size_t>(2, m_ready_surfaces.size());
        finished.assign(m_ready_surfaces.begin(), m_ready_surfaces.begin() + n);
        m_ready_surfaces.erase(m_ready_surfaces.begin(), m_ready_surfaces.begin() + n);
    }

    for (auto& item : finished) {
        if (item.second) {
            SDL_Texture* tex = SDL_CreateTextureFromSurface(ctx.renderer, item.second);
            SDL_FreeSurface(item.second);
            if (tex) {
                if (ctx.poster_cache.count(item.first) && ctx.poster_cache[item.first]) {
                    SDL_DestroyTexture(ctx.poster_cache[item.first]);
                }
                ctx.poster_cache[item.first] = tex;

                auto lruIt = std::find(ctx.poster_cache_lru.begin(), ctx.poster_cache_lru.end(), item.first);
                if (lruIt != ctx.poster_cache_lru.end()) {
                    ctx.poster_cache_lru.erase(lruIt);
                }
                ctx.poster_cache_lru.push_back(item.first);

                // Strict GX2 VRAM limit on cached poster textures
                while (ctx.poster_cache.size() > 40 &&!ctx.poster_cache_lru.empty()) {
                    std::string oldId = ctx.poster_cache_lru.front();
                    ctx.poster_cache_lru.erase(ctx.poster_cache_lru.begin());
                    auto it = ctx.poster_cache.find(oldId);
                    if (it != ctx.poster_cache.end()) {
                        if (it->second) SDL_DestroyTexture(it->second);
                        ctx.poster_cache.erase(it);
                    }
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_queued_ids.erase(oldId);
                }
            }
        }
    }
}

void PosterManager::worker_loop() {
    while (m_running) {
        PosterDownloadRequest req;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return !m_running || !m_queue.empty();
            });
            if (!m_running) break;
            // Do not accumulate more than 8 pending requests
            if (m_queue.size() > 8) {
                // Dropped requests must be re-requestable (otherwise the poster stays empty forever)
                for (size_t k = 0; k < m_queue.size() - 8; ++k) m_queued_ids.erase(m_queue[k].song_id);
                m_queue.erase(m_queue.begin(), m_queue.begin() + (m_queue.size() - 8));
            }
            req = m_queue.front();
            m_queue.erase(m_queue.begin());
        }

        std::vector<uint8_t> bytes = NetworkManager::HttpGetBytes(req.image_url, 6);
        SDL_Surface* surf = nullptr;
        if (!bytes.empty()) {
            surf = IMG_Load_RW(SDL_RWFromMem(bytes.data(), (int)bytes.size()), 1);
        }

        if (surf) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ready_surfaces.push_back({ req.song_id, surf });
        }
    }
}

SDL_Texture* GetPosterTexture(AppContext& ctx, PosterManager& pmgr, const Song& song) {
    if (song.id.empty()) return nullptr;

    auto it = ctx.poster_cache.find(song.id);
    if (it != ctx.poster_cache.end()) {
        return it->second;
    }

    std::string thumbUrl = song.thumbnailUrl;
    if (thumbUrl.empty()) {
        thumbUrl = "https://i.ytimg.com/vi/" + song.id + "/mqdefault.jpg";
    }

    pmgr.queue_poster(song.id, thumbUrl);
    return nullptr;
}

void DrawBackground(AppContext& ctx) {
    SDL_SetRenderDrawBlendMode(ctx.renderer, SDL_BLENDMODE_BLEND);
    static SDL_Texture* s_bg_tex = nullptr;
    static int s_bg_h = 0;
    if (!s_bg_tex || s_bg_h != ctx.screen_h) {
        if (s_bg_tex) SDL_DestroyTexture(s_bg_tex);
        s_bg_h = ctx.screen_h;
        s_bg_tex = SDL_CreateTexture(ctx.renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STATIC, 1, 128);
        if (s_bg_tex) {
            uint32_t pixels[128];
            for (int y = 0; y < 128; y++) {
                float t = (float)y / 127.0f;
                // Background gradient: (18, 14, 23) to (7, 6, 10)
                uint8_t r = (uint8_t)(18 + (7 - 18) * t);
                uint8_t g = (uint8_t)(14 + (6 - 14) * t);
                uint8_t b = (uint8_t)(23 + (10 - 23) * t);
                pixels[y] = (r << 24) | (g << 16) | (b << 8) | 0xFF;
            }
            SDL_UpdateTexture(s_bg_tex, nullptr, pixels, 1 * sizeof(uint32_t));
        }
    }
    if (s_bg_tex) {
        SDL_RenderCopy(ctx.renderer, s_bg_tex, nullptr, nullptr);
    } else {
        SDL_SetRenderDrawColor(ctx.renderer, COLOR_BAR.r, COLOR_BAR.g, COLOR_BAR.b, 255);
        SDL_RenderClear(ctx.renderer);
    }
}

void DrawVinylPlaceholder(SDL_Renderer* renderer, int x, int y, int w, int h, const std::string& title) {
    // Velvety dark placeholder background
    SDL_SetRenderDrawColor(renderer, 28, 24, 38, 255);
    SDL_Rect bg = { x, y, w, h };
    SDL_RenderFillRect(renderer, &bg);

    // Subtle inner frame
    SDL_SetRenderDrawColor(renderer, 42, 36, 56, 255);
    SDL_Rect inner = { x + 6, y + 6, w - 12, h - 12 };
    SDL_RenderDrawRect(renderer, &inner);

    // Central decoration: stylised sound wave
    int cx = x + w / 2;
    int cy = y + h / 2;
    int barW = 4;
    int barSpacing = 8;
    int numBars = 7;
    int totalBW = numBars * barSpacing;
    int startBX = cx - totalBW / 2;

    int heights[] = { 10, 20, 32, 42, 28, 16, 8 };
    for (int b = 0; b < numBars; b++) {
        int bh = (int)(heights[b] * (h / 150.0f));
        if (bh < 4) bh = 4;
        SDL_Rect barR = { startBX + b * barSpacing, cy - bh / 2, barW, bh };
        if (b == 3) {
            SDL_SetRenderDrawColor(renderer, COLOR_PRIMARY.r, COLOR_PRIMARY.g, COLOR_PRIMARY.b, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, 80, 70, 100, 220);
        }
        SDL_RenderFillRect(renderer, &barR);
    }
}

// ---------------------------------------------------------------- precomputed shapes
// One anti-aliased circle (128x128) and one "inverse corner" (64x64) generated once: every rounded shape
// now costs a few RenderCopy calls instead of one SDL line per pixel of radius (thousands of
// GX2 draw calls per frame otherwise).
static float CircleCoverage(float px, float py, float cx, float cy, float rad) {
    int inside = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            float dx = px + (sx + 0.5f) / 4.0f - cx, dy = py + (sy + 0.5f) / 4.0f - cy;
            if (dx * dx + dy * dy <= rad * rad) inside++;
        }
    return inside / 16.0f;
}

static SDL_Texture* MakeAlphaTexture(SDL_Renderer* r, int size, bool inverseCorner) {
    SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STATIC, size, size);
    if (!t) return nullptr;
    std::vector<uint32_t> px((size_t)size * size);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float cov;
            if (inverseCorner) cov = 1.0f - CircleCoverage((float)x, (float)y, (float)size, (float)size, (float)size);
            else cov = CircleCoverage((float)x, (float)y, size / 2.0f, size / 2.0f, size / 2.0f);
            uint32_t a = (uint32_t)(cov * 255.0f + 0.5f);
            px[(size_t)y * size + x] = 0xFFFFFF00u | a;
        }
    }
    SDL_UpdateTexture(t, nullptr, px.data(), size * (int)sizeof(uint32_t));
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static void EnsureShapeTextures(SDL_Renderer* r) {
    if (!s_circle_tex) s_circle_tex = MakeAlphaTexture(r, 128, false);
    if (!s_corner_tex) s_corner_tex = MakeAlphaTexture(r, 64, true);
    if (!s_white_tex) {
        s_white_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STATIC, 2, 2);
        if (s_white_tex) {
            uint32_t w[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
            SDL_UpdateTexture(s_white_tex, nullptr, w, 2 * (int)sizeof(uint32_t));
            SDL_SetTextureBlendMode(s_white_tex, SDL_BLENDMODE_BLEND);
        }
    }
}

static void TintCopy(SDL_Renderer* r, SDL_Texture* t, SDL_Color c, const SDL_Rect* src, const SDL_Rect* dst,
                     double angle = 0.0, SDL_RendererFlip flip = SDL_FLIP_NONE) {
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    if (angle == 0.0 && flip == SDL_FLIP_NONE) SDL_RenderCopy(r, t, src, dst);
    else SDL_RenderCopyEx(r, t, src, dst, angle, nullptr, flip);
}

void DrawFillCircle(SDL_Renderer* r, int cx, int cy, int rad, SDL_Color c) {
    if (rad <= 0) return;
    EnsureShapeTextures(r);
    SDL_Rect dst = { cx - rad, cy - rad, rad * 2, rad * 2 };
    if (s_circle_tex) TintCopy(r, s_circle_tex, c, nullptr, &dst);
}

// Paints the outside of a rounded corner (x,y = top-left of the rad x rad square)
void DrawCornerMask(SDL_Renderer* r, int x, int y, int rad, bool right, bool bottom, SDL_Color c) {
    if (rad <= 0) return;
    EnsureShapeTextures(r);
    SDL_Rect dst = { x, y, rad, rad };
    int flip = SDL_FLIP_NONE;
    if (right) flip |= SDL_FLIP_HORIZONTAL;
    if (bottom) flip |= SDL_FLIP_VERTICAL;
    if (s_corner_tex) TintCopy(r, s_corner_tex, c, nullptr, &dst, 0.0, (SDL_RendererFlip)flip);
}

void DrawThickLine(SDL_Renderer* r, int x1, int y1, int x2, int y2, int t, SDL_Color c) {
    EnsureShapeTextures(r);
    if (!s_white_tex) return;
    float dx = (float)(x2 - x1), dy = (float)(y2 - y1);
    float len = std::sqrt(dx * dx + dy * dy) + (float)t;   // square ends, like the old stroke
    double angle = std::atan2(dy, dx) * 180.0 / 3.14159265358979;
    int mx = (x1 + x2) / 2, my = (y1 + y2) / 2;
    SDL_Rect dst = { mx - (int)(len / 2), my - t / 2, (int)len, t };
    TintCopy(r, s_white_tex, c, nullptr, &dst, angle);
}

// ---------------------------------------------------------------- geometry (1 draw call per shape)
static int s_geom = -1;   // -1 unknown, 1 ok, 0 unsupported -> fall back to textures

static bool GeomDraw(SDL_Renderer* r, const SDL_Vertex* v, int nv, const int* idx, int ni) {
    if (s_geom == 0) return false;
    int rc = SDL_RenderGeometry(r, nullptr, v, nv, idx, ni);
    s_geom = (rc == 0) ? 1 : 0;
    return rc == 0;
}

bool DrawGeometry(SDL_Renderer* r, const SDL_Vertex* v, int nv, const int* idx, int ni) {
    return GeomDraw(r, v, nv, idx, ni);
}

// Filled rounded rectangle: a single triangle fan
static bool PillGeometry(SDL_Renderer* r, int x, int y, int w, int h, int rad, SDL_Color c) {
    if (s_geom == 0) return false;
    const int S = rad >= 18 ? 8 : (rad >= 8 ? 5 : 3);
    const float PI = 3.14159265f;
    SDL_Vertex v[1 + 4 * 9];
    int idx[4 * 9 * 3];
    int nv = 0;
    auto vert = [&](float px, float py) { v[nv].position = { px, py }; v[nv].color = c; v[nv].tex_coord = { 0, 0 }; nv++; };
    vert(x + w / 2.0f, y + h / 2.0f);
    struct Corner { float cx, cy, a0; };
    Corner cs[4] = { { (float)(x + rad), (float)(y + rad), PI }, { (float)(x + w - rad), (float)(y + rad), 1.5f * PI },
                     { (float)(x + w - rad), (float)(y + h - rad), 0.0f }, { (float)(x + rad), (float)(y + h - rad), 0.5f * PI } };
    for (auto& k : cs)
        for (int i = 0; i <= S; i++) {
            float a = k.a0 + (PI / 2.0f) * i / S;
            vert(k.cx + rad * std::cos(a), k.cy + rad * std::sin(a));
        }
    int per = nv - 1, ni = 0;
    for (int i = 0; i < per; i++) { idx[ni++] = 0; idx[ni++] = 1 + i; idx[ni++] = 1 + ((i + 1) % per); }
    return GeomDraw(r, v, nv, idx, ni);
}

// The 4 "outer" corners of a rounded image in a single draw call
void DrawRoundCorners(SDL_Renderer* r, SDL_Rect a, int rad, SDL_Color top, SDL_Color bottom) {
    if (rad > 1 && s_geom != 0) {
        const int S = 4;
        const float PI = 3.14159265f;
        SDL_Vertex v[4 * (S + 2)];
        int idx[4 * S * 3];
        int nv = 0, ni = 0;
        struct K { float ax, ay, cx, cy, a0; SDL_Color col; };
        K ks[4] = {
            { (float)a.x, (float)a.y, (float)(a.x + rad), (float)(a.y + rad), PI, top },
            { (float)(a.x + a.w), (float)a.y, (float)(a.x + a.w - rad), (float)(a.y + rad), 1.5f * PI, top },
            { (float)(a.x + a.w), (float)(a.y + a.h), (float)(a.x + a.w - rad), (float)(a.y + a.h - rad), 0.0f, bottom },
            { (float)a.x, (float)(a.y + a.h), (float)(a.x + rad), (float)(a.y + a.h - rad), 0.5f * PI, bottom }
        };
        for (auto& k : ks) {
            int apex = nv;
            v[nv].position = { k.ax, k.ay }; v[nv].color = k.col; v[nv].tex_coord = { 0, 0 }; nv++;
            int first = nv;
            for (int i = 0; i <= S; i++) {
                float ang = k.a0 + (PI / 2.0f) * i / S;
                v[nv].position = { k.cx + rad * std::cos(ang), k.cy + rad * std::sin(ang) };
                v[nv].color = k.col; v[nv].tex_coord = { 0, 0 }; nv++;
            }
            for (int i = 0; i < S; i++) { idx[ni++] = apex; idx[ni++] = first + i; idx[ni++] = first + i + 1; }
        }
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        if (GeomDraw(r, v, nv, idx, ni)) return;
    }
    DrawCornerMask(r, a.x, a.y, rad, false, false, top);
    DrawCornerMask(r, a.x + a.w - rad, a.y, rad, true, false, top);
    DrawCornerMask(r, a.x, a.y + a.h - rad, rad, false, true, bottom);
    DrawCornerMask(r, a.x + a.w - rad, a.y + a.h - rad, rad, true, true, bottom);
}

void DrawM3Pill(SDL_Renderer* renderer, int x, int y, int w, int h, SDL_Color bg, SDL_Color border, int radius) {
    if (!renderer || w <= 0 || h <= 0) return;
    if (radius > h / 2) radius = h / 2;
    if (radius > w / 2) radius = w / 2;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, bg.a);

    if (radius > 1 && PillGeometry(renderer, x, y, w, h, radius, bg)) {
        // shape drawn in a single call
    } else if (radius <= 1) {
        SDL_Rect all = { x, y, w, h };
        SDL_RenderFillRect(renderer, &all);
    } else {
        SDL_Rect mid = { x + radius, y, w - 2 * radius, h };
        if (mid.w > 0) SDL_RenderFillRect(renderer, &mid);
        int sideH = h - 2 * radius;
        if (sideH > 0) {
            SDL_Rect left = { x, y + radius, radius, sideH };
            SDL_Rect right = { x + w - radius, y + radius, radius, sideH };
            SDL_RenderFillRect(renderer, &left);
            SDL_RenderFillRect(renderer, &right);
        }
        EnsureShapeTextures(renderer);
        if (s_circle_tex) {
            SDL_Rect tl = { 0, 0, 64, 64 }, tr = { 64, 0, 64, 64 }, bl = { 0, 64, 64, 64 }, br = { 64, 64, 64, 64 };
            SDL_Rect d1 = { x, y, radius, radius };
            SDL_Rect d2 = { x + w - radius, y, radius, radius };
            SDL_Rect d3 = { x, y + h - radius, radius, radius };
            SDL_Rect d4 = { x + w - radius, y + h - radius, radius, radius };
            TintCopy(renderer, s_circle_tex, bg, &tl, &d1);
            TintCopy(renderer, s_circle_tex, bg, &tr, &d2);
            TintCopy(renderer, s_circle_tex, bg, &bl, &d3);
            TintCopy(renderer, s_circle_tex, bg, &br, &d4);
        }
    }

    if (border.a > 0) {
        SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
        SDL_RenderDrawLine(renderer, x + radius, y, x + w - radius, y);
        SDL_RenderDrawLine(renderer, x + radius, y + h - 1, x + w - radius, y + h - 1);
        SDL_RenderDrawLine(renderer, x, y + radius, x, y + h - 1 - radius);
        SDL_RenderDrawLine(renderer, x + w - 1, y + radius, x + w - 1, y + h - 1 - radius);
    }
}
