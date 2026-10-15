#include <string.h>
#include <limits.h>

#include "ppu.h"
#include "nes.h"
#include "state.h"


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
  m_evalStart = 0;
  m_openBusFrame = 0;
  spr0HitCycle = INT32_MAX;
  vblRaceCycle = INT32_MIN;
  m_vblCycle = INT32_MIN;
  nmiEdgeCycle = INT32_MIN;
  nmiClearCycle = INT32_MAX;
  vblSetUnits = 0;
  m_vblSuppressed = false;
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


// After a $2007 access: normally +1 or +32, but while the PPU renders (a visible or the pre-render
// line, rendering on) the access glitches into its coarse X and Y increments at once.
void Ppu::incrementAfter2007()
{
  if (renderingEnabled()) {
    int line = m_nes->currentLine(nullptr);
    if (line < 240 || line >= m_nes->linesPerFrame() - 1) {
      if ((m_v & 0x1f) == 0x1f)
        m_v = (m_v & ~0x1f) ^ 0x0400;
      else
        ++m_v;
      incrementY();
      return;
    }
  }
  m_v = (m_v + ((m_ctrl & 0x04) ? 32 : 1)) & 0x7fff;
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
  // the PPU's I/O latch is a capacitance: unrefreshed for about half a second, it reads 0
  if (m_nes->frameCount - m_openBusFrame > 30)
    m_openBus = 0;
  switch (addr & 7) {
    case 2: {
      if (spr0HitCycle <= m_nes->cpu.cycles)
        m_status |= 0x40;
      if (NES_UNLIKELY(m_nes->cpu.cycles == vblRaceCycle))
        m_vblSuppressed = true;
      else if (NES_UNLIKELY(m_nes->cpu.cycles == m_vblCycle + 1) &&
               m_nes->unitsOfCycle(m_nes->cpu.cycles - 1) - vblSetUnits <= 5)
        m_nes->cpu.cancelNmi();          // read on the dot after the flag rose: NMI suppressed too
      uint8_t r = (m_status & 0xe0) | (m_openBus & 0x1f);
      m_status &= ~0x80;
      m_w = false;
      m_openBus = r;
      m_openBusFrame = m_nes->frameCount;
      return r;
    }
    case 4: {
      int dot, line = m_nes->currentLine(&dot);
      if (renderingEnabled() && line < 240 && ((dot >= 1 && dot <= 64) || (dot >= 257 && dot <= 320)))
        m_openBus = 0xff;
      else
        m_openBus = oam[m_oamAddr];
      m_openBusFrame = m_nes->frameCount;
      return m_openBus;
    }
    case 7: {
      uint16_t a = m_v & 0x3fff;
      uint8_t r;
      if (a < 0x3f00) {
        r = m_readBuffer;
        m_readBuffer = vramRead(a);
      } else {
        // palette reads are immediate; the buffer gets the nametable byte "under" the palette
        r = (vramRead(a) & ((m_mask & 0x01) ? 0x30 : 0x3f)) | (m_openBus & 0xc0);
        m_readBuffer = vramRead(a - 0x1000);
      }
      incrementAfter2007();
      m_openBus = r;
      m_openBusFrame = m_nes->frameCount;
      return r;
    }
    default:
      return m_openBus;   // write only registers
  }
}


NES_HOT void Ppu::writeRegister(uint16_t addr, uint8_t value)
{
  m_openBus = value;
  m_openBusFrame = m_nes->frameCount;
  switch (addr & 7) {
    case 0: {
      m_nes->mapperPpuCtrl(value);
      bool wasEnabled = m_ctrl & 0x80;
      // the NMI line is "vblank AND enabled": turned off within a cycle of the edge, the CPU's
      // edge detector never sees it
      if (wasEnabled && !(value & 0x80) && m_nes->cpu.cycles <= nmiEdgeCycle + 1)
        m_nes->cpu.cancelNmi();
      m_ctrl = value;
      m_t = (m_t & 0xf3ff) | ((value & 3) << 10);
      // enabling NMI while the vblank flag is still set fires one straight away
      if (!wasEnabled && (value & 0x80) && (m_status & 0x80) && m_nes->cpu.cycles <= nmiClearCycle)
        m_nes->cpu.nmiDelayed();
      break;
    }
    case 1:
      m_mask = value;
      break;
    case 3:
      m_oamAddr = value;
      break;
    case 4:
      if (renderingEnabled() && m_nes->currentLine(nullptr) < 240) {
        m_oamAddr = (m_oamAddr + 4) & 0xfc;
      } else {
        // the attribute byte has no bits 2-4
        oam[m_oamAddr] = (m_oamAddr & 3) == 2 ? value & 0xe3 : value;
        ++m_oamAddr;
      }
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
      incrementAfter2007();
      break;
  }
}


void Ppu::oamDma(uint8_t const * page)
{
  for (int i = 0; i < 256; ++i) {
    uint8_t a = m_oamAddr + i;
    oam[a] = (a & 3) == 2 ? page[i] & 0xe3 : page[i];   // the attribute byte has no bits 2-4
  }
}


void Ppu::setVBlank()
{
  m_vblCycle = vblRaceCycle;
  vblRaceCycle = INT32_MIN;
  if (m_vblSuppressed) {
    m_vblSuppressed = false;
    return;
  }
  m_status |= 0x80;
  if (m_ctrl & 0x80)
    m_nes->cpu.nmiAtCycle(nmiEdgeCycle);
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
  // dots 257-320 fetch the next line's sprites, and leave OAMADDR at 0
  m_oamAddr = 0;
  incrementY();
  // horizontal position from t
  m_v = (m_v & ~0x041f) | (m_t & 0x041f);
}


// fine Y increment, carrying into coarse Y and the vertical nametable bit
inline void Ppu::incrementY()
{
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
}


void Ppu::copyVertical()
{
  if (renderingEnabled())
    m_v = (m_v & ~0x7be0) | (m_t & 0x7be0);
}


// PPU address of the pattern row 'row' (0 = top, before flipping) of a sprite
static inline uint16_t spritePatternAddr(uint8_t ctrl, uint8_t tile, uint8_t attr, int row)
{
  if (!(ctrl & 0x20)) {
    if (attr & 0x80)
      row = 7 - row;
    return ((ctrl & 0x08) << 9) | (tile << 4) | row;
  }
  if (attr & 0x80)
    row = 15 - row;
  return ((tile & 1) << 12) | ((tile & 0xfe) << 4) | ((row & 8) << 1) | (row & 7);
}


// Sprite evaluation for line y, as the PPU does it during the line before: walks OAM from the
// address in OAMADDR at dot 65 ('start', normally 0) and collects up to 8 objects in range of the
// line as copies of their 4 bytes. Like the hardware:
//   - an object in range is copied byte by byte, so the address goes up by 4 from wherever it was
//   - one out of range moves the address by 4 and masks it with $FC, which re-aligns a misaligned
//     address
//   - with 8 found, the overflow search adds 5 instead (the famous hardware bug: the byte index
//     within the object is incremented along with the object, without carrying)
//   - evaluation ends when the address passes 255
// *spr0 tells whether the first object evaluated is among them: that one is "sprite 0" for the
// hit, whichever OAM slot it is in. Returns the number of objects found.
static int evaluateSprites(uint8_t const * oam, int start, int y, int height,
                           uint8_t found[8][4], bool * spr0, bool * overflow)
{
  int count = 0;
  *spr0 = *overflow = false;
  for (int a = start; a < 256; ) {
    bool inRange = (unsigned)(y - 1 - oam[a]) < (unsigned) height;
    if (count < 8) {
      if (inRange) {
        for (int k = 0; k < 4; ++k)
          found[count][k] = oam[(a + k) & 0xff];
        if (a == start)
          *spr0 = true;
        ++count;
        a += 4;
      } else {
        a = (a + 4) & 0x1fc;
      }
    } else {
      if (inRange) {
        *overflow = true;
        break;
      }
      a = ((a & ~3) + 4) | ((a + 1) & 3);
    }
  }
  return count;
}


void Ppu::latchEvalStart()
{
  m_evalStart = m_oamAddr;
}


void Ppu::captureLine(LineState & s) const
{
  s.v    = m_v;
  s.x    = m_x;
  s.ctrl = m_ctrl;
  s.mask = m_mask;
  s.evalStart = m_evalStart;
  memcpy(s.palette, palette, sizeof(palette));
  memcpy(s.chrMap, chrMap, sizeof(chrMap));
  memcpy(s.sprChrMap, sprChrMap, sizeof(sprChrMap));
  memcpy(s.ntMap, ntMap, sizeof(ntMap));
}


// Is the background pixel at screen x (0-255) on the current line opaque? Uses the live
// registers, so it answers for the line about to be drawn.
inline bool Ppu::bgOpaqueAt(int x) const
{
  int pos = m_x + x;
  int coarseX = (m_v & 0x1f) + (pos >> 3);
  int nt = (m_v >> 10) & 3;
  if (coarseX >= 32) {
    coarseX -= 32;
    nt ^= 1;
  }
  uint8_t name = ntMap[nt][(m_v & 0x03e0) | coarseX];
  uint16_t pa = ((m_ctrl & 0x10) << 8) | (name << 4) | ((m_v >> 12) & 7);
  uint8_t const * chr = chrMap[pa >> 10] + (pa & 0x3ff);
  int bit = 7 - (pos & 7);
  return ((chr[0] | chr[8]) >> bit) & 1;
}


NES_HOT int Ppu::evaluateLine(int y)
{
  // evaluation runs whenever rendering is on, background or sprites
  if (!(m_mask & 0x18))
    return -1;

  int height = (m_ctrl & 0x20) ? 16 : 8;
  uint8_t found[8][4];
  bool spr0, overflow;
  evaluateSprites(oam, m_evalStart, y, height, found, &spr0, &overflow);
  if (overflow)
    m_status |= 0x20;

  // sprite 0 hit: needs "sprite 0" (the first object evaluated) on this line, and both the
  // background and the sprites on
  if (!spr0 || (m_mask & 0x18) != 0x18)
    return -1;
  unsigned row = (unsigned)(y - 1 - found[0][0]);
  uint8_t tile = found[0][1], attr = found[0][2];
  int sx = found[0][3];
  uint16_t pa = spritePatternAddr(m_ctrl, tile, attr, row);
  uint8_t const * chr = sprChrMap[pa >> 10] + (pa & 0x3ff);
  uint8_t bits = chr[0] | chr[8];
  if (attr & 0x40)
    bits = s_reverse[bits];
  if (!bits)
    return -1;
  // either clip bit hides the leftmost 8 pixels from the hit test; x = 255 never hits
  int minX = ((m_mask & 0x06) == 0x06) ? 0 : 8;
  for (int px = 0; px < 8; ++px) {
    int x = sx + px;
    if (x > 254)
      break;
    if (x >= minX && ((bits >> (7 - px)) & 1) && bgOpaqueAt(x))
      return x;
  }
  return -1;
}


// Stores pixels 4 at a time: c0 is the leftmost. In VGA order the DMA sends byte 2 of a word
// first, then 3, 0, 1 (FabGL's raw scanline layout).
#define PACK4(c0, c1, c2, c3)  (vgaOrder ? ((c2) | ((c3) << 8) | ((c0) << 16) | ((c1) << 24)) \
                                         : ((c0) | ((c1) << 8) | ((c2) << 16) | ((c3) << 24)))

void Ppu::drawLine(LineState const & st, int y, uint8_t * out, uint8_t const * colorMap, bool vgaOrder) const
{
  // the line's palette, already in output pixels; entries 4, 8 and 12 (transparent background
  // pixels of palettes 1-3) show the backdrop, so a background pixel indexes it directly
  uint8_t mono = (st.mask & 0x01) ? 0x30 : 0x3f;
  uint8_t rp[32];
  for (int i = 0; i < 32; ++i)
    rp[i] = colorMap[st.palette[i] & mono];
  rp[4] = rp[8] = rp[12] = rp[0];

  uint32_t * dst32 = (uint32_t *) out;

  if (!(st.mask & 0x18)) {
    uint32_t w = rp[0] * 0x01010101u;
    for (int i = 0; i < 64; ++i)
      dst32[i] = w;
    return;
  }

  // --- background, into bg[] as (palette << 2 | pixel), pixel 0 being transparent ---
  uint8_t bg[256 + 16] __attribute__((aligned(4)));
  bool bgOn = st.mask & 0x08;
  uint8_t const * bgLine = bg + st.x;
  if (bgOn) {
    uint16_t v = st.v;
    uint16_t patternBase = ((st.ctrl & 0x10) << 8) | ((v >> 12) & 7);
    uint32_t * dst = (uint32_t *) bg;
    for (int tile = 0; tile < 33; ++tile) {
      uint8_t const * nt = st.ntMap[(v >> 10) & 3];
      int coarseX = v & 0x1f, coarseY = (v >> 5) & 0x1f;
      uint8_t name = nt[v & 0x3ff];
      uint8_t attr = nt[0x3c0 | ((coarseY >> 2) << 3) | (coarseX >> 2)];
      uint32_t palBits = ((attr >> (((coarseY & 2) << 1) | (coarseX & 2))) & 3) * 0x04040404u;
      uint16_t pa = patternBase | (name << 4);
      uint8_t const * chr = st.chrMap[pa >> 10] + (pa & 0x3ff);
      uint8_t lo = chr[0], hi = chr[8];
      dst[0] = s_spread[lo >> 4]   | (s_spread[hi >> 4] << 1)   | palBits;
      dst[1] = s_spread[lo & 0x0f] | (s_spread[hi & 0x0f] << 1) | palBits;
      dst += 2;
      // coarse X increment, wrapping into the horizontally adjacent nametable
      if ((v & 0x1f) == 0x1f)
        v = (v & ~0x1f) ^ 0x0400;
      else
        ++v;
    }
    if (!(st.mask & 0x02))
      memset(bg + st.x, 0, 8);   // leftmost 8 pixels hidden

    uint8_t const * b = bgLine;
    for (int i = 0; i < 64; ++i, b += 4)
      dst32[i] = PACK4(rp[b[0]], rp[b[1]], rp[b[2]], rp[b[3]]);
  } else {
    uint32_t w = rp[0] * 0x01010101u;
    for (int i = 0; i < 64; ++i)
      dst32[i] = w;
  }

  if (!(st.mask & 0x10))
    return;

  // --- sprites, drawn over the background in OAM order. The first opaque sprite pixel at an x
  // claims it, even when it is behind the background and so not drawn: that hides any later
  // sprite there (the "mushroom coming out of the block" effect).
  int height = (st.ctrl & 0x20) ? 16 : 8;
  int minX = (st.mask & 0x04) ? 0 : 8;
  int xorX = vgaOrder ? 2 : 0;
  uint32_t taken[8] = { 0 };
  uint8_t found[8][4];
  bool spr0, overflow;
  int count = evaluateSprites(oam, st.evalStart, y, height, found, &spr0, &overflow);
  for (int k = 0; k < count; ++k) {
    uint8_t const * s = found[k];
    int row = y - 1 - s[0];
    uint8_t attr = s[2];
    int sx = s[3];
    uint16_t pa = spritePatternAddr(st.ctrl, s[1], attr, row);
    uint8_t const * chr = st.sprChrMap[pa >> 10] + (pa & 0x3ff);
    uint8_t lo = chr[0], hi = chr[8];
    if (attr & 0x40) {
      lo = s_reverse[lo];
      hi = s_reverse[hi];
    }
    if (!(lo | hi))
      continue;
    uint8_t const * spal = rp + 0x10 + ((attr & 3) << 2);
    bool behind = attr & 0x20;
    for (int px = 0; px < 8; ++px) {
      int x = sx + px;
      if (x > 255)
        break;
      int p = ((lo >> (7 - px)) & 1) | (((hi >> (7 - px)) & 1) << 1);
      if (!p || x < minX)
        continue;
      uint32_t bit = 1u << (x & 31);
      if (taken[x >> 5] & bit)
        continue;
      taken[x >> 5] |= bit;
      if (!behind || !bgOn || !(bgLine[x] & 3))
        out[x ^ xorX] = spal[p];
    }
  }
}


void Ppu::state(StateIO & io)
{
  io.pod(vram); io.pod(palette); io.pod(oam);
  io.pod(spr0HitCycle); io.pod(vblRaceCycle); io.pod(nmiEdgeCycle); io.pod(vblSetUnits); io.pod(nmiClearCycle);
  io.pod(m_ctrl); io.pod(m_mask); io.pod(m_status); io.pod(m_oamAddr);
  io.pod(m_v); io.pod(m_t); io.pod(m_x); io.pod(m_evalStart); io.pod(m_w);
  io.pod(m_readBuffer); io.pod(m_openBus); io.pod(m_openBusFrame);
  io.pod(m_vblSuppressed); io.pod(m_vblCycle);
}
