#include <string.h>
#include <limits.h>

#include "ppu.h"
#include "nes.h"


// The 8 bits of a pattern plane byte spread to one byte per pixel (0 or 1), leftmost pixel in
// the lowest byte, as two 32 bit halves: s_spread[n] covers 4 pixels of nibble n.
static uint32_t NES_FASTDATA s_spread[16];
// bit reversal, for horizontally flipped sprites
static uint8_t  NES_FASTDATA s_reverse[256];
static bool s_tablesReady = false;

static void initTables()
{
  for (int n = 0; n < 16; ++n) {
    uint32_t w = 0;
    for (int i = 0; i < 4; ++i)
      if (n & (8 >> i))
        w |= 1u << (i * 8);
    s_spread[n] = w;
  }
  for (int b = 0; b < 256; ++b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i)
      if (b & (1 << i))
        r |= 0x80 >> i;
    s_reverse[b] = r;
  }
  s_tablesReady = true;
}


void Ppu::reset()
{
  if (!s_tablesReady)
    initTables();
  m_ctrl = m_mask = m_status = m_oamAddr = 0;
  m_v = m_t = 0;
  m_x = 0;
  m_w = false;
  m_readBuffer = 0;
  m_openBus = 0;
  spr0HitCycle = INT32_MAX;
  memset(vram, 0, sizeof(vram));
  memset(oam, 0xff, sizeof(oam));
  // a typical power on palette; the real one is semi random
  static const uint8_t initialPalette[32] = {
    0x09, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x0d, 0x08, 0x10, 0x08, 0x24, 0x00, 0x00, 0x04, 0x2c,
    0x09, 0x01, 0x34, 0x03, 0x00, 0x04, 0x00, 0x14, 0x08, 0x3a, 0x00, 0x02, 0x00, 0x20, 0x2c, 0x08,
  };
  memcpy(palette, initialPalette, sizeof(palette));
}


static inline int paletteIndex(uint16_t addr)
{
  int i = addr & 0x1f;
  // $3F10/$3F14/$3F18/$3F1C mirror $3F00/$3F04/$3F08/$3F0C
  return (i & 0x13) == 0x10 ? i & 0x0f : i;
}


NES_HOT uint8_t Ppu::vramRead(uint16_t addr)
{
  addr &= 0x3fff;
  if (addr < 0x2000)
    return chrMap[addr >> 10][addr & 0x3ff];
  if (addr < 0x3f00)
    return ntMap[(addr >> 10) & 3][addr & 0x3ff];
  return palette[paletteIndex(addr)];
}


NES_HOT void Ppu::vramWrite(uint16_t addr, uint8_t value)
{
  addr &= 0x3fff;
  if (addr < 0x2000) {
    if (chrWritable)
      chrMap[addr >> 10][addr & 0x3ff] = value;
  } else if (addr < 0x3f00) {
    ntMap[(addr >> 10) & 3][addr & 0x3ff] = value;
  } else {
    palette[paletteIndex(addr)] = value & 0x3f;
  }
}


NES_HOT uint8_t Ppu::readRegister(uint16_t addr)
{
  switch (addr & 7) {
    case 2: {
      if (spr0HitCycle <= m_nes->cpu.cycles)
        m_status |= 0x40;
      uint8_t r = (m_status & 0xe0) | (m_openBus & 0x1f);
      m_status &= ~0x80;
      m_w = false;
      m_openBus = r;
      return r;
    }
    case 4:
      m_openBus = oam[m_oamAddr];
      return m_openBus;
    case 7: {
      uint16_t a = m_v & 0x3fff;
      uint8_t r;
      if (a < 0x3f00) {
        r = m_readBuffer;
        m_readBuffer = vramRead(a);
      } else {
        // palette reads are immediate; the buffer gets the nametable byte "under" the palette
        r = (vramRead(a) & 0x3f) | (m_openBus & 0xc0);
        m_readBuffer = vramRead(a - 0x1000);
      }
      m_v = (m_v + ((m_ctrl & 0x04) ? 32 : 1)) & 0x7fff;
      m_openBus = r;
      return r;
    }
    default:
      return m_openBus;   // write only registers
  }
}


NES_HOT void Ppu::writeRegister(uint16_t addr, uint8_t value)
{
  m_openBus = value;
  switch (addr & 7) {
    case 0: {
      bool wasEnabled = m_ctrl & 0x80;
      m_ctrl = value;
      m_t = (m_t & 0xf3ff) | ((value & 3) << 10);
      // enabling NMI while the vblank flag is still set fires one straight away
      if (!wasEnabled && (value & 0x80) && (m_status & 0x80))
        m_nes->cpu.nmi();
      break;
    }
    case 1:
      m_mask = value;
      break;
    case 3:
      m_oamAddr = value;
      break;
    case 4:
      oam[m_oamAddr++] = value;
      break;
    case 5:
      if (!m_w) {
        m_t = (m_t & 0xffe0) | (value >> 3);
        m_x = value & 7;
      } else {
        m_t = (m_t & 0x8c1f) | ((value & 7) << 12) | ((value & 0xf8) << 2);
      }
      m_w = !m_w;
      break;
    case 6:
      if (!m_w) {
        m_t = (m_t & 0x00ff) | ((value & 0x3f) << 8);
      } else {
        m_t = (m_t & 0xff00) | value;
        m_v = m_t;
      }
      m_w = !m_w;
      break;
    case 7:
      vramWrite(m_v, value);
      m_v = (m_v + ((m_ctrl & 0x04) ? 32 : 1)) & 0x7fff;
      break;
  }
}


void Ppu::oamDma(uint8_t const * page)
{
  for (int i = 0; i < 256; ++i)
    oam[(uint8_t)(m_oamAddr + i)] = page[i];
}


void Ppu::setVBlank()
{
  m_status |= 0x80;
  if (m_ctrl & 0x80)
    m_nes->cpu.nmi();
}


void Ppu::startFrame()
{
  m_status &= 0x1f;
  spr0HitCycle = INT32_MAX;
}


NES_HOT void Ppu::endOfLine()
{
  if (!renderingEnabled())
    return;
  // fine Y increment, carrying into coarse Y and the vertical nametable bit
  if ((m_v & 0x7000) != 0x7000) {
    m_v += 0x1000;
  } else {
    m_v &= ~0x7000;
    int y = (m_v & 0x03e0) >> 5;
    if (y == 29) {
      y = 0;
      m_v ^= 0x0800;
    } else if (y == 31) {
      y = 0;
    } else {
      ++y;
    }
    m_v = (m_v & ~0x03e0) | (y << 5);
  }
  // horizontal position from t
  m_v = (m_v & ~0x041f) | (m_t & 0x041f);
}


void Ppu::copyVertical()
{
  if (renderingEnabled())
    m_v = (m_v & ~0x7be0) | (m_t & 0x7be0);
}


NES_HOT int Ppu::renderLine(int y, uint8_t * out)
{
  if (!renderingEnabled()) {
    if (out)
      memset(out, palette[0], 256);
    return -1;
  }

  // --- sprite evaluation (on hardware this happens during the previous line) ---
  int height = (m_ctrl & 0x20) ? 16 : 8;
  int found[8];
  int count = 0;
  bool spr0OnLine = false;
  if (m_mask & 0x10) {
    for (int i = 0; i < 64; ++i) {
      unsigned row = (unsigned)(y - 1 - oam[i * 4]);
      if (row < (unsigned)height) {
        if (count == 8) {
          m_status |= 0x20;     // overflow (without the hardware's evaluation bug)
          break;
        }
        if (i == 0)
          spr0OnLine = true;
        found[count++] = i;
      }
    }
  }

  // a skipped frame only needs the lines that can produce the sprite 0 hit
  if (!out && !(spr0OnLine && (m_mask & 0x08)))
    return -1;

  // --- background, into bg[] as (palette << 2 | pixel), pixel 0 being transparent ---
  uint8_t bg[256 + 16] __attribute__((aligned(4)));
  if (m_mask & 0x08) {
    uint16_t v = m_v;
    int fineY = (v >> 12) & 7;
    uint16_t patternBase = ((m_ctrl & 0x10) << 8) | fineY;
    uint32_t * dst = (uint32_t *) bg;
    for (int tile = 0; tile < 33; ++tile) {
      uint8_t const * nt = ntMap[(v >> 10) & 3];
      int coarseX = v & 0x1f, coarseY = (v >> 5) & 0x1f;
      uint8_t name = nt[v & 0x3ff];
      uint8_t attr = nt[0x3c0 | ((coarseY >> 2) << 3) | (coarseX >> 2)];
      uint32_t pal = ((attr >> (((coarseY & 2) << 1) | (coarseX & 2))) & 3) * 0x04040404u;
      uint16_t pa = patternBase | (name << 4);
      uint8_t const * chr = chrMap[pa >> 10] + (pa & 0x3ff);
      uint8_t lo = chr[0], hi = chr[8];
      dst[0] = s_spread[lo >> 4]   | (s_spread[hi >> 4] << 1)   | pal;
      dst[1] = s_spread[lo & 0x0f] | (s_spread[hi & 0x0f] << 1) | pal;
      dst += 2;
      // coarse X increment, wrapping into the horizontally adjacent nametable
      if ((v & 0x1f) == 0x1f)
        v = (v & ~0x1f) ^ 0x0400;
      else
        ++v;
    }
    if (!(m_mask & 0x02))
      memset(bg + m_x, 0, 8);   // leftmost 8 pixels hidden
  } else {
    memset(bg, 0, sizeof(bg));
  }
  uint8_t const * bgLine = bg + m_x;

  // --- sprites, into spr[]: 0x10 | palette << 2 | pixel, 0x20 = behind background, 0x40 = sprite 0
  uint8_t spr[256 + 8];
  int hitX = -1;
  if (count) {
    memset(spr, 0, sizeof(spr));
    int minX = (m_mask & 0x04) ? 0 : 8;
    for (int k = 0; k < count; ++k) {
      int i = found[k];
      uint8_t const * s = oam + i * 4;
      int row = y - 1 - s[0];
      uint8_t tile = s[1], attr = s[2];
      int sx = s[3];
      uint16_t pa;
      if (height == 8) {
        if (attr & 0x80)
          row = 7 - row;
        pa = ((m_ctrl & 0x08) << 9) | (tile << 4) | row;
      } else {
        if (attr & 0x80)
          row = 15 - row;
        pa = ((tile & 1) << 12) | ((tile & 0xfe) << 4) | ((row & 8) << 1) | (row & 7);
      }
      uint8_t const * chr = chrMap[pa >> 10] + (pa & 0x3ff);
      uint8_t lo = chr[0], hi = chr[8];
      if (attr & 0x40) {
        lo = s_reverse[lo];
        hi = s_reverse[hi];
      }
      if (!(lo | hi))
        continue;
      uint8_t tag = 0x10 | ((attr & 3) << 2) | ((attr & 0x20) ? 0x20 : 0) | (i == 0 ? 0x40 : 0);
      for (int px = 0; px < 8; ++px) {
        int x = sx + px;
        int p = ((lo >> (7 - px)) & 1) | (((hi >> (7 - px)) & 1) << 1);
        // lower OAM index wins: only the first opaque sprite pixel is kept
        if (p && x >= minX && !spr[x])
          spr[x] = tag | p;
      }
    }

    if (spr0OnLine && (m_mask & 0x08)) {
      for (int x = 0; x < 255; ++x)
        if ((spr[x] & 0x40) && (bgLine[x] & 3)) {
          hitX = x;
          break;
        }
    }
  }

  if (!out)
    return hitX;

  // --- composition ---
  uint8_t mono = (m_mask & 0x01) ? 0x30 : 0x3f;
  if (!count) {
    for (int x = 0; x < 256; ++x) {
      uint8_t b = bgLine[x];
      out[x] = palette[(b & 3) ? b : 0] & mono;
    }
  } else {
    for (int x = 0; x < 256; ++x) {
      uint8_t b = bgLine[x], s = spr[x];
      int c = (b & 3) ? b : 0;
      if (s && (!(s & 0x20) || !(b & 3)))
        c = s & 0x1f;
      out[x] = palette[c] & mono;
    }
  }
  return hitX;
}
