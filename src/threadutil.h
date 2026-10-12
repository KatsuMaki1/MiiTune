#ifndef MIITUNE_THREADUTIL_H
#define MIITUNE_THREADUTIL_H

#include <coreinit/thread.h>

// Background threads (network, decoding): moved off core 1 (the render core) and given slightly
// lower priority, so a download or decode no longer makes the screen lag.
// Masks: 1 = core 0, 2 = core 1, 4 = core 2.
inline void BackgroundThread(uint32_t coreMask) {
    OSThread* t = OSGetCurrentThread();
    if (!t) return;
    OSSetThreadAffinity(t, coreMask);
    OSSetThreadPriority(t, OSGetThreadPriority(t) + 2);
}

#endif
