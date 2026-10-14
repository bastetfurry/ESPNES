// Cartridge: iNES / NES 2.0 parsing and the mapper interface.
//
// The CPU and PPU never see a mapper directly. They read through bank pointer tables in Nes
// (8 KB PRG pages at $8000-$FFFF, 1 KB CHR pages at PPU $0000-$1FFF, 1 KB nametable pages), and a
// mapper only rewrites those pointers. That keeps the hot read paths free of virtual calls and
// lets later mappers point PRG banks straight into memory-mapped flash instead of RAM.
#pragma once

#include "platform.h"

class Nes;

enum class Mirroring : uint8_t { Horizontal, Vertical, SingleA, SingleB, FourScreen };

enum class Region : uint8_t { NTSC, PAL };

struct RomInfo {
  int       mapper;
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

  // CPU write to $8000-$FFFF (mapper registers).
  virtual void writeRegister(uint16_t addr, uint8_t value) { }

  // Called once per rendered scanline at about dot 260, where the MMC3 sees PPU A12 rise.
  virtual void hblank() { }

protected:
  Nes & m_nes;
};

// Returns nullptr when the mapper is not supported (yet).
Mapper * createMapper(int number, Nes & nes);

bool mapperSupported(int number);
