#include "mii.h"
#include <SDL2/SDL_image.h>
#include <nn/act/client_cpp.h>
#include <coreinit/debug.h>
#include <vector>

SDL_Surface* LoadUserMii() {
    nn::act::Initialize();   // already initialised = harmless error
    nn::act::SlotNo slot = nn::act::GetSlotNo();

    std::vector<uint8_t> buf(128 * 1024);
    // The service "type" parameter is poorly documented: we try the first values.
    for (int type = 0; type < 4; ++type) {
        size_t size = 0;
        nn::Result r = nn::act::GetMiiImageEx(&size, buf.data(), buf.size(), type, slot);
        if (!r.IsSuccess() || size == 0 || size > buf.size()) continue;

        SDL_RWops* rw = SDL_RWFromConstMem(buf.data(), (int)size);
        SDL_Surface* s = IMG_Load_RW(rw, 0);                        // PNG / JPG
        if (!s) {
            SDL_RWseek(rw, 0, RW_SEEK_SET);
            s = IMG_LoadTyped_RW(rw, 0, "TGA");                     // usual format of Wii U Mii icons
        }
        SDL_RWclose(rw);
        if (s) {
            OSReport("[MiiTune] Mii loaded (type %d, %u bytes, %dx%d)\n", type, (unsigned int)size, s->w, s->h);
            return s;
        }
    }
    OSReport("[MiiTune] Mii image unavailable\n");
    return nullptr;
}
