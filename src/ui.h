// The menus' screen: a grid of 32 x 30 characters over the NES's 256 x 240 picture.
//
// A screen writes what it wants into the grid (put, box, ...), every time in full, and flush()
// draws only the cells that differ from what is on the monitor, straight into the frame buffer.
// So nothing flickers, and a popup needs nothing saved under it: once it closes, the screen
// writes itself again and flush() paints back what the popup covered.
//
// Also here: the messages in the top row (green, yellow, red, each with its sound) and the
// sounds themselves, mixed into the APU's stream.
#pragma once

#include <stdint.h>

namespace ui {

static constexpr int COLS = 32, ROWS = 30;

enum Colour : uint8_t {
  BLACK, LINE, TEXT, GREY, SEL_BG, SEL_TEXT, RED, GREEN, YELLOW, BRIGHT, CYAN, MAGENTA,
  COLOURS
};

// the font's glyphs (code page 437, fonts/font8x8.png)
enum : uint8_t {
  G_PLAY = 0x10, G_UP = 0x1e, G_DOWN = 0x1f, G_CROSS = 0x2a, G_NOTE = 0xea, G_MODULE = 0xeb, G_CHECK = 0xfb,
  G_V = 0xb3, G_H = 0xc4, G_TL = 0xda, G_TR = 0xbf, G_BL = 0xc0, G_BR = 0xd9, G_LT = 0xc3, G_RT = 0xb4, G_UT = 0xc1,
};

// lines: the frame buffer's 240 lines, pixels in the DMA's byte order (x ^ 2); raw: the raw pixel
// of each Colour
void init(uint8_t * const * lines, uint8_t const raw[COLOURS]);

// what is on the monitor is unknown (a game or another screen drew there): the next flush draws all
void invalidate();

// writing into the grid; everything is clipped to it
void clear();
void put(int col, int row, char const * text, uint8_t fg = TEXT, uint8_t bg = BLACK);
void putc(int col, int row, uint8_t ch, uint8_t fg = TEXT, uint8_t bg = BLACK);
void fill(int col, int row, int w, int h, uint8_t ch = ' ', uint8_t fg = TEXT, uint8_t bg = BLACK);
// a frame in LINE colour, the inside cleared
void box(int col, int row, int w, int h);
// a line across a box, joined to its sides
void divider(int col, int row, int w);
// usage hints, "A: OK  B: Back": the word before each ':' in red, the rest in white
void hint(int col, int row, char const * text, uint8_t bg = BLACK);
// cells a picture is drawn over; flush leaves them alone and tells when they need the picture again
void picture(int col, int row, int w, int h);

// whether a cell is a picture's on the monitor (not covered by a popup), in pixels
bool pictureShown(int x, int y);

// Draws what changed. Returns true when picture cells were covered by something else before, so
// their picture is to be drawn again (also after invalidate()).
bool flush();

// messages in the top row, for a few seconds, with a sound
enum Kind : uint8_t { GOOD, NEUTRAL, BAD };
void message(Kind kind, char const * text);
void messageArea(int col, int width);      // where in row 0; the whole row by default
bool messageShowing();

// the sounds alone, at the DAC's rate
void soundRate(int sampleRate);
void sound(Kind kind);
// the sounds' next sample, from the sound interrupt; 0 when silent
int  soundSample();

}
