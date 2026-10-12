#include "embedded_assets.hpp"

// The files are pulled in by the assembler at build time, so the repository does not need
// multi-megabyte generated headers. Paths are relative to the project root (where make runs).
#define EMBED_FILE(NAME, PATH)                    \
    ".balign 4\n"                                 \
    ".global " #NAME "_DATA\n"                    \
    #NAME "_DATA:\n"                              \
    ".incbin \"" PATH "\"\n"                      \
    #NAME "_END:\n"                               \
    ".balign 4\n"                                 \
    ".global " #NAME "_SIZE\n"                    \
    #NAME "_SIZE:\n"                              \
    ".long " #NAME "_END - " #NAME "_DATA\n"

__asm__(
    ".section .rodata\n"
    EMBED_FILE(EMBEDDED_FONT, "content/font.ttf")
    EMBED_FILE(EMBEDDED_LOGO, "icon.png")
    EMBED_FILE(ICON_ATLAS, "assets/icons_atlas.png")
    ".text\n"
);
