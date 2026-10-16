// Cartridge: iNES / NES 2.0 parsing and the mapper interface.
//
// The CPU and PPU never see a mapper directly. They read through bank pointer tables in Nes
// (8 KB PRG pages at $8000-$FFFF, 1 KB CHR pages at PPU $0000-$1FFF, 1 KB nametable pages), and a
// mapper only rewrites those pointers. That keeps the hot read paths free of virtual calls and
// lets later mappers point PRG banks straight into memory-mapped flash instead of RAM.
#pragma once

#include "platform.h"

class Nes;
class StateIO;

enum class Mirroring : uint8_t { Horizontal, Vertical, SingleA, SingleB, FourScreen };

enum class Region : uint8_t { NTSC, PAL };

struct RomInfo {
  int       mapper;
  int       submapper;     // NES 2.0 only, else 0
  int       prgSize;       // bytes
  int       chrSize;       // bytes, 0 = the cartridge has 8 KB CHR RAM instead
  Mirroring mirroring;
  bool      battery;
  bool      nes20;
  bool      regionKnown;   // the header says which TV system the game is for
  Region    region;
};

// A parsed ROM image. PRG and CHR point into the caller's buffer, which must outlive the cart.
struct Cart {
  RomInfo         info;
  uint8_t const * prg;
  uint8_t const * chr;
};

// Parses an iNES / NES 2.0 image. Returns nullptr on success or an error message.
char const * parseRom(uint8_t const * data, size_t size, Cart * cart);


class Mapper {
public:
  explicit Mapper(Nes & nes) : m_nes(nes) { }
  virtual ~Mapper() { }

  // Sets up the bank pointers for power on.
  virtual void reset() = 0;

  // Savestates (see state.h): state() saves or loads the registers, restore() then rebuilds the
  // bank pointers (PRG, CHR, nametables) from them.
  virtual void state(StateIO & io) { }
  virtual void restore() = 0;

  // CPU write to $8000-$FFFF (mapper registers).
  virtual void writeRegister(uint16_t addr, uint8_t value) { }

  // CPU access to $4020-$5FFF (MMC5 registers and ExRAM). readLow returns -1 for open bus.
  virtual int  readLow(uint16_t addr)                    { return -1; }
  virtual void writeLow(uint16_t addr, uint8_t value)   { }

  // Called once per rendered scanline at about dot 260, where the MMC3 sees PPU A12 rise.
  virtual void hblank() { }

  // Mappers that set wantsLineStart get lineStart(y) at about dot 4 of each visible line while
  // rendering is on (the MMC5's scanline detection), and vblank() once per frame.
  bool         wantsLineStart = false;
  virtual void lineStart(int y) { }
  virtual void vblank() { }

  // An IRQ counter that counts CPU cycles (the VRCs) tells Nes the cycle of its next IRQ
  // (Nes::setMapperIrqAt); Nes calls irqTime(cycle) when the CPU gets there, and rebase() at the
  // end of each frame, when the frame relative cycle numbers go back by 'cycles'.
  virtual void irqTime(int32_t cycle) { }
  virtual void rebase(int32_t cycles) { }

  // A CPU write to PPUCTRL ($2000): the MMC5 picks its CHR bank sets by the sprite size.
  bool         wantsPpuCtrl = false;
  virtual void ppuCtrl(uint8_t value) { }

protected:
  Nes & m_nes;
};

// Returns nullptr when the mapper is not supported (yet).
Mapper * createMapper(int number, Nes & nes);

bool mapperSupported(int number);
