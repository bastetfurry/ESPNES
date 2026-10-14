#include <string.h>

#include "cart.h"
#include "nes.h"


char const * parseRom(uint8_t const * data, size_t size, Cart * cart)
{
  if (size < 16 || memcmp(data, "NES\x1a", 4) != 0)
    return "Not an iNES file";

  RomInfo & info = cart->info;
  uint8_t f6 = data[6], f7 = data[7];

  info.nes20 = (f7 & 0x0c) == 0x08;
  info.mapper = (f6 >> 4) | (f7 & 0xf0);

  int prgUnits = data[4], chrUnits = data[5];
  if (info.nes20) {
    info.mapper |= (data[8] & 0x0f) << 8;
    // exponent-multiplier sizes (nibble 0xF) are for odd homebrew sizes; not supported
    if ((data[9] & 0x0f) == 0x0f || (data[9] & 0xf0) == 0xf0)
      return "Unsupported NES 2.0 ROM size";
    prgUnits |= (data[9] & 0x0f) << 8;
    chrUnits |= (data[9] & 0xf0) << 4;
  } else if (data[7] & 0x0c) {
    // "DiskDude!" and other junk in bytes 7-15 of old dumps: the upper mapper nibble is garbage
    info.mapper &= 0x0f;
  }

  info.prgSize = prgUnits * 16384;
  info.chrSize = chrUnits * 8192;
  info.battery = f6 & 0x02;

  if (f6 & 0x08)
    info.mirroring = Mirroring::FourScreen;
  else
    info.mirroring = (f6 & 0x01) ? Mirroring::Vertical : Mirroring::Horizontal;

  if (info.nes20) {
    // 0 NTSC, 1 PAL, 2 multi-region, 3 Dendy (PAL timing is the closer match)
    int t = data[12] & 3;
    info.regionKnown = t != 2;
    info.region      = (t == 1 || t == 3) ? Region::PAL : Region::NTSC;
  } else {
    // iNES byte 9 bit 0 is almost never set by dumpers, so only trust a "PAL" there
    info.regionKnown = data[9] & 1;
    info.region      = info.regionKnown ? Region::PAL : Region::NTSC;
  }

  size_t offset = 16 + ((f6 & 0x04) ? 512 : 0);   // skip the trainer
  if (info.prgSize == 0)
    return "No PRG ROM";
  if (offset + info.prgSize + info.chrSize > size)
    return "File is truncated";

  cart->prg = data + offset;
  cart->chr = info.chrSize ? data + offset + info.prgSize : nullptr;
  return nullptr;
}


///////////////////////////////////////////////////////////////////////////////
// Mapper 0: NROM. 16 or 32 KB PRG (16 KB is mirrored), 8 KB CHR ROM or RAM, fixed mirroring.

class MapperNROM : public Mapper {
public:
  using Mapper::Mapper;

  void reset() override
  {
    Cart const & c = m_nes.cart();
    int prgMask = c.info.prgSize - 1;          // 16 KB -> $C000 mirrors $8000
    for (int i = 0; i < 4; ++i)
      m_nes.mapPrg8K(i, c.prg + ((i * 0x2000) & prgMask));
    m_nes.mapChr8K(0);
    m_nes.setMirroring(c.info.mirroring);
  }
};


///////////////////////////////////////////////////////////////////////////////
// Mapper 2: UxROM (UNROM, UOROM). A switchable 16 KB PRG bank at $8000, the last bank fixed at
// $C000, 8 KB CHR RAM, fixed mirroring. Real boards have bus conflicts (the written value is ANDed
// with the ROM byte at the address); games avoid them by writing to a byte holding the same
// value, so they are not emulated.

class MapperUxROM : public Mapper {
public:
  using Mapper::Mapper;

  void reset() override
  {
    Cart const & c = m_nes.cart();
    m_banks = c.info.prgSize / 0x4000;
    select(0);
    uint8_t const * last = c.prg + (m_banks - 1) * 0x4000;
    m_nes.mapPrg8K(2, last);
    m_nes.mapPrg8K(3, last + 0x2000);
    m_nes.mapChr8K(0);
    m_nes.setMirroring(c.info.mirroring);
  }

  void writeRegister(uint16_t addr, uint8_t value) override
  {
    select(value);
  }

private:
  void select(int bank)
  {
    uint8_t const * p = m_nes.cart().prg + (bank % m_banks) * 0x4000;
    m_nes.mapPrg8K(0, p);
    m_nes.mapPrg8K(1, p + 0x2000);
  }

  int m_banks;
};


///////////////////////////////////////////////////////////////////////////////
// Mapper 1: MMC1 (SxROM). Registers are written a bit at a time through a 5 bit shift register;
// a write with bit 7 set resets it. Address bits 13-14 of the fifth write pick the register:
//   $8000 control: mirroring (bits 0-1), PRG mode (2-3), CHR mode (4)
//   $A000 CHR bank 0, $C000 CHR bank 1 (4 KB units, or 8 KB with bank 0 when CHR mode is 0)
//   $E000 PRG bank (16 KB units)
// SUROM (512 KB PRG) uses bit 4 of the CHR bank registers as the 256 KB PRG half.

class MapperMMC1 : public Mapper {
public:
  using Mapper::Mapper;

  void reset() override
  {
    m_shift = 0;
    m_count = 0;
    m_control = 0x0c;     // PRG mode 3: the last bank fixed at $C000
    m_chr0 = m_chr1 = m_prg = 0;
    update();
  }

  void writeRegister(uint16_t addr, uint8_t value) override
  {
    if (value & 0x80) {
      m_shift = 0;
      m_count = 0;
      m_control |= 0x0c;
      update();
      return;
    }
    m_shift |= (value & 1) << m_count;
    if (++m_count < 5)
      return;
    switch ((addr >> 13) & 3) {
      case 0: m_control = m_shift; break;
      case 1: m_chr0    = m_shift; break;
      case 2: m_chr1    = m_shift; break;
      case 3: m_prg     = m_shift; break;
    }
    m_shift = 0;
    m_count = 0;
    update();
  }

private:
  void update()
  {
    Cart const & c = m_nes.cart();

    static const Mirroring mirroring[4] = { Mirroring::SingleA, Mirroring::SingleB, Mirroring::Vertical, Mirroring::Horizontal };
    m_nes.setMirroring(mirroring[m_control & 3]);

    // PRG, in 16 KB banks within a 256 KB half
    int banks = c.info.prgSize / 0x4000;
    int outer = (banks > 16) ? (m_chr0 & 0x10) : 0;
    int bankLo, bankHi;
    switch ((m_control >> 2) & 3) {
      case 0: case 1:          // 32 KB
        bankLo = (m_prg & 0x0e);
        bankHi = bankLo | 1;
        break;
      case 2:                  // first bank fixed at $8000
        bankLo = 0;
        bankHi = m_prg & 0x0f;
        break;
      default:                 // last bank fixed at $C000
        bankLo = m_prg & 0x0f;
        bankHi = 0x0f;
        break;
    }
    mapPrg16K(0, (outer | bankLo) % banks);
    mapPrg16K(1, (outer | bankHi) % banks);

    // CHR, in 4 KB banks
    if (m_control & 0x10) {
      mapChr4K(0, m_chr0);
      mapChr4K(1, m_chr1);
    } else {
      mapChr4K(0, m_chr0 & 0x1e);
      mapChr4K(1, (m_chr0 & 0x1e) | 1);
    }
  }

  void mapPrg16K(int half, int bank)
  {
    uint8_t const * p = m_nes.cart().prg + bank * 0x4000;
    m_nes.mapPrg8K(half * 2, p);
    m_nes.mapPrg8K(half * 2 + 1, p + 0x2000);
  }

  void mapChr4K(int half, int bank)
  {
    for (int i = 0; i < 4; ++i)
      m_nes.mapChr1K(half * 4 + i, bank * 4 + i);
  }

  uint8_t m_shift, m_count;
  uint8_t m_control, m_chr0, m_chr1, m_prg;
};


Mapper * createMapper(int number, Nes & nes)
{
  switch (number) {
    case 0:  return new MapperNROM(nes);
    case 1:  return new MapperMMC1(nes);
    case 2:  return new MapperUxROM(nes);
    default: return nullptr;
  }
}


bool mapperSupported(int number)
{
  return number <= 2;
}
