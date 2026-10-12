#ifndef MIITUNE_ICONS_H
#define MIITUNE_ICONS_H

#include <SDL2/SDL.h>
#include "icons_ids.h"

// Lucide icons (https://lucide.dev, ISC license) rasterised into an embedded atlas (tools/gen_icons.py).
// One draw call per icon, tinted at draw time.
bool IconsInit(SDL_Renderer* r);
void IconsShutdown();
void DrawIcon(SDL_Renderer* r, IconId id, int cx, int cy, int size, SDL_Color color);   // size = glyph size in px

#endif
