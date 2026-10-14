// Ricoh 2C02 / 2C07 PPU, scanline based.
//
// Each visible line is drawn in one go, from the registers as they are at the line's start (the
// "loopy" v/t/x/w model, so $2005/$2006 splits work). The fine Y increment and horizontal reload
// happen at dot 256, as on hardware.
//
// Drawing is split from the CPU's side of the line so it can run on the other core: at the line
// start the CPU side takes a LineState snapshot (scroll, control, mask, palette, bank pointers)
// and works out the sprite 0 hit and overflow itself, which only needs the pixels under sprite 0.
// drawLine() then turns the snapshot into pixels at leisure. Nametables, OAM and CHR RAM are not
// copied: games change them in vblank, and Nes waits for the drawing to finish before vblank.
//
// The sprite 0 hit becomes visible to $2002 only once the CPU has reached the dot where it
// happens, so games that time a status bar split on it see it on the right line.
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

  // everything drawLine needs that the CPU may change during the frame
  struct LineState {
    uint16_t  v;
    uint8_t   x, ctrl, mask;
    uint8_t   evalStart;            // OAM address sprite evaluation starts at
    uint8_t   palette[32];
    uint8_t * chrMap[8];
    uint8_t * ntMap[4];
  };

  void captureLine(LineState & s) const;

  // CPU side of visible line y, at its start: sets the sprite overflow flag and returns the x
  // position of the sprite 0 hit on this line, or -1.
  int  evaluateLine(int y);

  // Draws visible line y from a snapshot into 'out' (256 pixels, 32 bit aligned), each NES colour
  // (0-63) translated through colorMap. vgaOrder stores the pixels in FabGL's raw scanline byte
  // order. Touches no PPU state, so it may run on another core.
  void drawLine(LineState const & s, int y, uint8_t * out, uint8_t const * colorMap, bool vgaOrder) const;

  // dot 65 of a line: sprite evaluation for the next line starts at the current OAM address
  void latchEvalStart();

  // dot 256-257 of a rendered line: fine Y increment and horizontal copy from t, OAMADDR to 0
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
  // cpu.cycles of an instruction whose $2002 read races the vblank flag being set (see Nes)
  int32_t   vblRaceCycle;
  int32_t   nmiEdgeCycle;           // CPU cycle of the vblank NMI edge
  int32_t   vblSetUnits;            // the dot the vblank flag rises at (Nes::unitsAt)
  int32_t   nmiClearCycle;          // CPU cycle from which the NMI input sees vblank over

private:

  bool     bgOpaqueAt(int x) const;
  void     incrementY();
  void     incrementAfter2007();
  uint8_t  vramRead(uint16_t addr);
  void     vramWrite(uint16_t addr, uint8_t value);

  Nes *    m_nes;

  uint8_t  m_ctrl, m_mask, m_status, m_oamAddr;
  uint16_t m_v, m_t;                // loopy registers
  uint8_t  m_x;                     // fine X
  uint8_t  m_evalStart;             // OAMADDR at dot 65 of the line before
  bool     m_w;                     // write toggle
  uint8_t  m_readBuffer;
  uint8_t  m_openBus;
  uint32_t m_openBusFrame;          // frame of the latch's last refresh, for its decay
  bool     m_vblSuppressed;         // a $2002 read raced the vblank flag: it stays clear
  int32_t  m_vblCycle;              // CPU cycle of this frame's vblank edge
};
