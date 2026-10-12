#ifndef MIITUNE_MII_H
#define MIITUNE_MII_H

#include <SDL2/SDL.h>

// Image of the console's active account Mii (pre-rendered by the nn_act accounts service).
// nullptr if unavailable (no account, Cemu without support, etc.). Call from a background thread.
SDL_Surface* LoadUserMii();

#endif
