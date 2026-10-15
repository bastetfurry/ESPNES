// The firmware's fonts, from fonts/*.png (see tools/nesfont.py), generated into
// src/generated/fonts.cpp by tools/embed_roms.py. Code page 437, like FabGL's own.
#pragma once

#include "fabgl.h"

extern const fabgl::FontInfo FONT_8x8;     // menu, manuals, key mapper, messages over the game
extern const fabgl::FontInfo FONT_4x6;     // the small labels on the drawn pad
