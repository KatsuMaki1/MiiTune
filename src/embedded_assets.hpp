#ifndef MIITUNE_EMBEDDED_ASSETS_H
#define MIITUNE_EMBEDDED_ASSETS_H

// Binary assets linked into the executable (see embedded_assets.cpp).
extern const unsigned char EMBEDDED_FONT_DATA[];   // content/font.ttf (DejaVu Sans)
extern const unsigned int  EMBEDDED_FONT_SIZE;
extern const unsigned char EMBEDDED_LOGO_DATA[];   // icon.png
extern const unsigned int  EMBEDDED_LOGO_SIZE;
extern const unsigned char ICON_ATLAS_DATA[];      // assets/icons_atlas.png (Lucide icons, see tools/gen_icons.py)
extern const unsigned int  ICON_ATLAS_SIZE;

#endif
