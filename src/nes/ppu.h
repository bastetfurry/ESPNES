// Ricoh 2C02 / 2C07 PPU, scanline based.
//
// Each visible line is drawn in one go at its start, from the scroll registers as they are
// at that moment (the "loopy" v/t/x/w model, so $2005/$2006 splits work). The fine Y increment
// and horizontal reload happen at dot 256, as on hardware. Sprite 0 hit is computed while drawing
// and becomes visible to $2002 only once the CPU has reached the dot where it happens, so games
// that time a status bar split on it see it on the right line.
#pragma once

#include "platform.h"

class Nes;

class Ppu {
public:

  void init(Nes * nes) { m_nes = nes; }
  void reset();

  uint8_t readRegister(uint16_t addr);
  void    writeRegister(uint16_t addr, uint8_t value);
  void    oamDma(uint8_t const * page);

  // Draws visible line y into 'out' (256 NES colour indices, 0-63) when out != nullptr. With
  // nullptr nothing is drawn, but sprite 0 hit is still evaluated (for skipped frames).
  // Returns the x position of the sprite 0 hit on this line, or -1.
  int  renderLine(int y, uint8_t * out);

  // dot 256-257 of a rendered line: fine Y increment and horizontal copy from t
  void endOfLine();
  // dots 280-304 of the pre-render line: vertical copy from t
  void copyVertical();

  void setVBlank();
  void startFrame();                // pre-render line, dot 1: clears the status flags

  bool renderingEnabled() const     { return m_mask & 0x18; }
  bool nmiEnabled() const           { return m_ctrl & 0x80; }

  // PPU address space: CHR in 1 KB pages, nametables in 1 KB pages
  uint8_t * chrMap[8];
  uint8_t * ntMap[4];
  bool      chrWritable;

  uint8_t   vram[2048];             // the console's 2 KB of nametable RAM
  uint8_t   palette[32];
  uint8_t   oam[256];

  // CPU cycle (frame relative) at which the pending sprite 0 hit becomes visible
  int32_t   spr0HitCycle;

private:

  uint8_t  vramRead(uint16_t addr);
  void     vramWrite(uint16_t addr, uint8_t value);

  Nes *    m_nes;

  uint8_t  m_ctrl, m_mask, m_status, m_oamAddr;
  uint16_t m_v, m_t;                // loopy registers
  uint8_t  m_x;                     // fine X
  bool     m_w;                     // write toggle
  uint8_t  m_readBuffer;
  uint8_t  m_openBus;
};
