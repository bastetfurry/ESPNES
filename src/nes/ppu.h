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
class StateIO;

class Ppu {
public:

  void init(Nes * nes) { m_nes = nes; }
  void reset();

  uint8_t readRegister(uint16_t addr);
  void    writeRegister(uint16_t addr, uint8_t value);
  void    oamDma(uint8_t const * page);

  // savestates (see state.h); the bank pointers are the mapper's to restore
  void    state(StateIO & io);

  // everything drawLine needs that the CPU may change during the frame
  struct LineState {
    uint16_t  v;
    uint8_t   x, ctrl, mask;
    uint8_t   sprCount;             // sprites fetched for the line
    uint8_t   spr[8][4];            // their pattern bytes (flipped), attribute and x
    uint8_t   palette[32];
    uint8_t * chrMap[8];
    uint8_t * sprChrMap[8];
    uint8_t * ntMap[4];
  };

  void captureLine(LineState & s) const;

  // CPU side of visible line y, at its start: the x position of the sprite 0 hit on this line,
  // or -1.
  int  spriteZeroHit(int y);

  // Draws visible line y from a snapshot into 'out' (256 pixels, 32 bit aligned), each NES colour
  // (0-63) translated through colorMap. vgaOrder stores the pixels in FabGL's raw scanline byte
  // order. Touches no PPU state, so it may run on another core.
  void drawLine(LineState const & s, int y, uint8_t * out, uint8_t const * colorMap, bool vgaOrder) const;

  // dot 65 of a line: secondary OAM has been cleared (with rendering on), and sprite evaluation
  // for the next line starts at the current OAM address
  void latchEvalStart();
  // dots 65-256 of visible line y: sprite evaluation for the next line into secondary OAM (when
  // rendering is on; otherwise secondary OAM keeps what it had)
  void evaluateSprites(int y);
  // dots 257-320 of line y (pre-render line: linesPerFrame - 1): the next line's sprites fetched
  // from secondary OAM, with the sprite size and pattern table set then
  void fetchSprites(int y);

  // dot 256-257 of a rendered line: fine Y increment and horizontal copy from t, OAMADDR to 0
  void endOfLine();
  // dots 280-304 of the pre-render line: vertical copy from t
  void copyVertical();

  void setVBlank();
  void startFrame();                // pre-render line, dot 1: clears the status flags

  bool renderingEnabled() const     { return m_mask & 0x18; }
  bool nmiEnabled() const           { return m_ctrl & 0x80; }

  // PPU address space: CHR in 1 KB pages, nametables in 1 KB pages
  uint8_t * chrMap[8];               // background fetches, and $2007
  uint8_t * sprChrMap[8];            // sprite fetches; the same as chrMap except on the MMC5
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
  void     resolveHit();
  void     incrementY();
  void     incrementAfter2007();
  uint8_t  vramRead(uint16_t addr);
  void     vramWrite(uint16_t addr, uint8_t value);

  Nes *    m_nes;

  uint8_t  m_ctrl, m_mask, m_status, m_oamAddr;
  uint16_t m_v, m_t;                // loopy registers
  uint8_t  m_x;                     // fine X
  uint8_t  m_evalStart;             // OAMADDR at dot 65 of the line before
  uint8_t  m_oam2[8][4];            // secondary OAM: the objects found by the last evaluation
  uint8_t  m_oam2Count;
  bool     m_oam2Spr0;              // ... the first of them is the first object evaluated
  bool     m_evalOn;                // rendering was on at dot 65: evaluation runs
  uint8_t  m_spr[8][4];             // the sprites fetched for the next line (see LineState)
  uint8_t  m_sprCount;
  bool     m_sprSpr0;
  bool     m_w;                     // write toggle
  uint8_t  m_readBuffer;
  uint8_t  m_openBus;
  uint32_t m_openBusFrame;          // frame of the latch's last refresh, for its decay
  bool     m_vblSuppressed;         // a $2002 read raced the vblank flag: it stays clear
  int32_t  m_vblCycle;              // CPU cycle of this frame's vblank edge
};
