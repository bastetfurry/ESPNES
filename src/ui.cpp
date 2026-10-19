// The menus' text grid, their messages and sounds (see ui.h).

#include "ui.h"

#include <string.h>
#include <math.h>
#include "esp_timer.h"
#include "esp_attr.h"
#include "fonts.h"

namespace ui {

// a cell: the character and its colours, foreground in the low nibble; PICTURE marks a picture's cell
struct Cell {
  uint8_t ch, attr;
  bool operator!=(Cell const & o) const { return ch != o.ch || attr != o.attr; }
};
static constexpr uint8_t PICTURE = 0xff;
static constexpr Cell    UNKNOWN = { 0, 0xfe };     // never wanted: shown cells after invalidate()

static Cell      s_want[ROWS][COLS];
static Cell      s_shown[ROWS][COLS];
static uint8_t * const * s_lines;
static uint8_t   s_raw[COLOURS];

static char      s_msgText[COLS + 1];
static Kind      s_msgKind;
static int64_t   s_msgUntil;                       // esp_timer time, 0: no message
static int       s_msgCol = 0, s_msgWidth = COLS;


void init(uint8_t * const * lines, uint8_t const raw[COLOURS])
{
  s_lines = lines;
  memcpy(s_raw, raw, COLOURS);
  clear();
  invalidate();
}


void invalidate()
{
  for (auto & row : s_shown)
    for (auto & c : row)
      c = UNKNOWN;
}


void clear()
{
  fill(0, 0, COLS, ROWS);
}


void putc(int col, int row, uint8_t ch, uint8_t fg, uint8_t bg)
{
  if (col >= 0 && col < COLS && row >= 0 && row < ROWS)
    s_want[row][col] = { ch, (uint8_t)(fg | bg << 4) };
}


void put(int col, int row, char const * text, uint8_t fg, uint8_t bg)
{
  for (; *text; ++text, ++col)
    putc(col, row, (uint8_t) *text, fg, bg);
}


void fill(int col, int row, int w, int h, uint8_t ch, uint8_t fg, uint8_t bg)
{
  for (int y = row; y < row + h; ++y)
    for (int x = col; x < col + w; ++x)
      putc(x, y, ch, fg, bg);
}


void box(int col, int row, int w, int h)
{
  fill(col + 1, row + 1, w - 2, h - 2);
  fill(col + 1, row, w - 2, 1, G_H, LINE);
  fill(col + 1, row + h - 1, w - 2, 1, G_H, LINE);
  fill(col, row + 1, 1, h - 2, G_V, LINE);
  fill(col + w - 1, row + 1, 1, h - 2, G_V, LINE);
  putc(col, row, G_TL, LINE);
  putc(col + w - 1, row, G_TR, LINE);
  putc(col, row + h - 1, G_BL, LINE);
  putc(col + w - 1, row + h - 1, G_BR, LINE);
}


void divider(int col, int row, int w)
{
  fill(col + 1, row, w - 2, 1, G_H, LINE);
  putc(col, row, G_LT, LINE);
  putc(col + w - 1, row, G_RT, LINE);
}


void hint(int col, int row, char const * text, uint8_t bg)
{
  // a word that ends in ':' is a button
  for (char const * p = text; *p; ) {
    char const * end = p;
    while (*end && *end != ' ')
      ++end;
    bool button = end > p && end[-1] == ':';
    for (; p < end; ++p, ++col)
      putc(col, row, (uint8_t) *p, button && p < end - 1 ? RED : TEXT, bg);
    for (; *p == ' '; ++p, ++col)
      putc(col, row, ' ', TEXT, bg);
  }
}


void picture(int col, int row, int w, int h)
{
  for (int y = row; y < row + h; ++y)
    for (int x = col; x < col + w; ++x)
      if (x >= 0 && x < COLS && y >= 0 && y < ROWS)
        s_want[y][x] = { 0, PICTURE };
}


bool pictureShown(int x, int y)
{
  return x >= 0 && x < COLS * 8 && y >= 0 && y < ROWS * 8 && s_shown[y / 8][x / 8].attr == PICTURE;
}


static void drawCell(int col, int row, Cell c)
{
  uint8_t fg = s_raw[c.attr & 15], bg = s_raw[c.attr >> 4];
  uint8_t const * glyph = FONT_8x8.data + c.ch * 8;
  for (int y = 0; y < 8; ++y) {
    uint8_t * line = s_lines[row * 8 + y];
    uint8_t bits = glyph[y];
    for (int x = 0; x < 8; ++x)
      line[(col * 8 + x) ^ 2] = bits & (0x80 >> x) ? fg : bg;   // the DMA's byte order
  }
}


bool flush()
{
  if (s_msgUntil && esp_timer_get_time() >= s_msgUntil)
    s_msgUntil = 0;
  bool picture = false;
  for (int row = 0; row < ROWS; ++row)
    for (int col = 0; col < COLS; ++col) {
      Cell c = s_want[row][col];
      if (row == 0 && s_msgUntil && col >= s_msgCol && col < s_msgCol + s_msgWidth) {
        char ch = s_msgText[col - s_msgCol];
        static const uint8_t colours[] = { GREEN, YELLOW, RED };
        c = { (uint8_t)(ch ? ch : ' '), colours[s_msgKind] };
      }
      if (!(c != s_shown[row][col]))
        continue;
      if (c.attr == PICTURE)
        picture = true;
      else
        drawCell(col, row, c);
      s_shown[row][col] = c;
    }
  return picture;
}


void message(Kind kind, char const * text)
{
  memset(s_msgText, 0, sizeof(s_msgText));
  strncpy(s_msgText, text, std::min(s_msgWidth, COLS));
  s_msgKind = kind;
  s_msgUntil = esp_timer_get_time() + 3000000;
  sound(kind);
}


void messageArea(int col, int width)
{
  s_msgCol = col;
  s_msgWidth = width;
}


bool messageShowing()
{
  return s_msgUntil != 0;
}


///////////////////////////////////////////////////////////////////////////////
// the sounds: a few decaying tones, started at set times

struct Voice {
  int8_t const * wave;
  uint32_t       phase, step;      // through the 256 samples of the wave, in 1/2^24
  int32_t        amp;              // 1/32768
  uint32_t       decay;            // amp's factor per sample, 1/65536
  int32_t        delay;            // samples before it starts
};

static constexpr int VOICES = 8;
static DRAM_ATTR Voice  s_voices[VOICES];
static DRAM_ATTR int8_t s_sine[256], s_bright[256];   // RAM: the interrupt runs while flash is busy
static int              s_rate = 22050;


void soundRate(int sampleRate)
{
  s_rate = sampleRate;
  // the bright wave: a few harmonics falling off like a struck string's
  for (int i = 0; i < 256; ++i) {
    float a = i * 2 * (float) M_PI / 256, b = 0;
    for (int h = 1; h <= 6; ++h)
      b += sinf(a * h) / h;
    s_sine[i]   = (int8_t) lrintf(sinf(a) * 127);
    s_bright[i] = (int8_t) lrintf(b * 70);
  }
}


struct Tone {
  float   freq, level, halfLife;   // Hz, 0-1, seconds
  int     startMs;
  bool    bright;
};


void sound(Kind kind)
{
  // positive: a soft rising ding-dong; neutral: one short ping; negative: a dissonant cluster
  // hammered twice, a piano's lowest patience
  static const Tone good[] = {
    { 1568, 0.40f, 0.10f, 0,  false }, { 3136, 0.10f, 0.05f, 0,  false },
    { 2093, 0.40f, 0.20f, 90, false }, { 4186, 0.08f, 0.08f, 90, false },
  };
  static const Tone neutral[] = {
    { 1760, 0.35f, 0.05f, 0, false },
  };
  static const Tone bad[] = {
    { 185, 0.30f, 0.12f, 0,   true }, { 196, 0.30f, 0.12f, 0,   true },
    { 277, 0.25f, 0.12f, 0,   true }, { 294, 0.25f, 0.12f, 0,   true },
    { 185, 0.30f, 0.18f, 140, true }, { 196, 0.30f, 0.18f, 140, true },
    { 277, 0.25f, 0.18f, 140, true }, { 294, 0.25f, 0.18f, 140, true },
  };
  Tone const * tones = kind == GOOD ? good : kind == NEUTRAL ? neutral : bad;
  int count = kind == GOOD ? 4 : kind == NEUTRAL ? 1 : 8;

  // silence first, so the interrupt never sees a half set voice playing
  for (auto & v : s_voices)
    v.amp = 0;
  for (int i = 0; i < count && i < VOICES; ++i) {
    Tone const & t = tones[i];
    Voice & v = s_voices[i];
    v.wave  = t.bright ? s_bright : s_sine;
    v.phase = 0;
    v.step  = (uint32_t)(t.freq * 16777216.0f / s_rate);
    v.decay = (uint32_t)(65536 * powf(0.5f, 1.0f / (t.halfLife * s_rate)));
    v.delay = t.startMs * s_rate / 1000;
    __sync_synchronize();
    v.amp   = (int32_t)(t.level * 32768);
  }
}


int IRAM_ATTR soundSample()
{
  int sum = 0;
  for (auto & v : s_voices) {
    if (!v.amp)
      continue;
    if (v.delay > 0) {
      --v.delay;
      continue;
    }
    sum += v.wave[(v.phase >> 16) & 255] * v.amp >> 15;
    v.phase += v.step;
    v.amp = (int32_t)((uint32_t) v.amp * v.decay >> 16);
    if (v.amp < 64)
      v.amp = 0;
  }
  return sum;
}

}
