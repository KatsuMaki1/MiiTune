#include "icons.h"
#include "embedded_assets.hpp"
#include <SDL2/SDL_image.h>
#include <coreinit/debug.h>

static SDL_Texture* s_atlas = nullptr;

bool IconsInit(SDL_Renderer* r) {
    if (s_atlas) return true;
    SDL_Surface* s = IMG_Load_RW(SDL_RWFromConstMem(ICON_ATLAS_DATA, (int)ICON_ATLAS_SIZE), 1);
    if (!s) { OSReport("[MiiTune] Unreadable icon atlas: %s\n", IMG_GetError()); return false; }
    s_atlas = SDL_CreateTextureFromSurface(r, s);
    SDL_FreeSurface(s);
    if (!s_atlas) { OSReport("[MiiTune] Could not create the icon texture\n"); return false; }
    SDL_SetTextureBlendMode(s_atlas, SDL_BLENDMODE_BLEND);
    return true;
}

void IconsShutdown() {
    if (s_atlas) { SDL_DestroyTexture(s_atlas); s_atlas = nullptr; }
}

void DrawIcon(SDL_Renderer* r, IconId id, int cx, int cy, int size, SDL_Color c) {
    if (!s_atlas || id < 0 || id >= IC_COUNT) return;
    // A Lucide glyph fills ~20/24 of its cell: scale up so `size` is the visible size
    int d = size * 12 / 10;
    SDL_Rect src = { (id % ICON_COLS) * ICON_CELL, (id / ICON_COLS) * ICON_CELL, ICON_CELL, ICON_CELL };
    SDL_Rect dst = { cx - d / 2, cy - d / 2, d, d };
    SDL_SetTextureColorMod(s_atlas, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(s_atlas, c.a);
    SDL_RenderCopy(r, s_atlas, &src, &dst);
}
