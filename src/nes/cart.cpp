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


Mapper * createMapper(int number, Nes & nes)
{
  switch (number) {
    case 0:  return new MapperNROM(nes);
    default: return nullptr;
  }
}


bool mapperSupported(int number)
{
  return number == 0;
}
