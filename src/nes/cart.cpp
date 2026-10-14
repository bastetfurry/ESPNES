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
  info.submapper = 0;

  int prgUnits = data[4], chrUnits = data[5];
  if (info.nes20) {
    info.mapper |= (data[8] & 0x0f) << 8;
    info.submapper = data[8] >> 4;
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


///////////////////////////////////////////////////////////////////////////////
// Mapper 4: MMC3 (TxROM). Eight bank registers selected through $8000 and written through $8001:
// R0/R1 are 2 KB CHR banks, R2-R5 1 KB CHR banks, R6/R7 8 KB PRG banks. $8000 bit 6 swaps which
// PRG slot is fixed to the second last bank, bit 7 swaps the CHR halves. $A000 sets mirroring.
//
// The scanline counter is clocked by rising edges of PPU A12, which with the usual setup
// (background at $0000, sprites at $1000) happen once per rendered line at about dot 260. Nes
// calls hblank() there. When the counter reaches 0 with IRQs enabled, the IRQ line goes low until
// $E000 acknowledges it.

class MapperMMC3 : public Mapper {
public:
  using Mapper::Mapper;

  void reset() override
  {
    m_select = 0;
    for (int i = 0; i < 8; ++i)
      m_regs[i] = 0;
    m_regs[0] = 0; m_regs[1] = 2; m_regs[2] = 4; m_regs[3] = 5; m_regs[4] = 6; m_regs[5] = 7;
    m_regs[6] = 0; m_regs[7] = 1;
    m_irqLatch = m_irqCounter = 0;
    m_irqReload = m_irqEnabled = false;
    m_nes.cpu.clearIrq(Cpu::IRQ_MAPPER);
    Cart const & c = m_nes.cart();
    m_fourScreen = c.info.mirroring == Mirroring::FourScreen;
    m_nes.setMirroring(c.info.mirroring);
    updatePrg();
    updateChr();
  }

  void writeRegister(uint16_t addr, uint8_t value) override
  {
    bool odd = addr & 1;
    switch (addr & 0xe000) {
      case 0x8000:
        if (!odd) {
          m_select = value;
        } else {
          m_regs[m_select & 7] = value;
        }
        updatePrg();
        updateChr();
        break;
      case 0xa000:
        // odd: PRG RAM protect, not emulated (the RAM stays enabled and writable)
        if (!odd && !m_fourScreen)
          m_nes.setMirroring((value & 1) ? Mirroring::Horizontal : Mirroring::Vertical);
        break;
      case 0xc000:
        if (!odd)
          m_irqLatch = value;
        else {
          m_irqCounter = 0;
          m_irqReload = true;
        }
        break;
      case 0xe000:
        if (!odd) {
          m_irqEnabled = false;
          m_nes.cpu.clearIrq(Cpu::IRQ_MAPPER);
        } else {
          m_irqEnabled = true;
        }
        break;
    }
  }

  void hblank() override
  {
    if (m_irqCounter == 0 || m_irqReload) {
      m_irqCounter = m_irqLatch;
      m_irqReload = false;
    } else {
      --m_irqCounter;
    }
    if (m_irqCounter == 0 && m_irqEnabled)
      m_nes.cpu.setIrq(Cpu::IRQ_MAPPER);
  }

private:
  void updatePrg()
  {
    Cart const & c = m_nes.cart();
    int banks = c.info.prgSize / 0x2000;
    auto bank = [&](int b) { return c.prg + (b % banks) * 0x2000; };
    uint8_t const * secondLast = bank(banks - 2);
    if (m_select & 0x40) {
      m_nes.mapPrg8K(0, secondLast);
      m_nes.mapPrg8K(2, bank(m_regs[6]));
    } else {
      m_nes.mapPrg8K(0, bank(m_regs[6]));
      m_nes.mapPrg8K(2, secondLast);
    }
    m_nes.mapPrg8K(1, bank(m_regs[7]));
    m_nes.mapPrg8K(3, bank(banks - 1));
  }

  void updateChr()
  {
    // with bit 7 set the 2 KB banks go to $1000 and the 1 KB banks to $0000
    int flip = (m_select & 0x80) ? 4 : 0;
    m_nes.mapChr1K(0 ^ flip, m_regs[0] & 0xfe);
    m_nes.mapChr1K(1 ^ flip, m_regs[0] | 1);
    m_nes.mapChr1K(2 ^ flip, m_regs[1] & 0xfe);
    m_nes.mapChr1K(3 ^ flip, m_regs[1] | 1);
    m_nes.mapChr1K(4 ^ flip, m_regs[2]);
    m_nes.mapChr1K(5 ^ flip, m_regs[3]);
    m_nes.mapChr1K(6 ^ flip, m_regs[4]);
    m_nes.mapChr1K(7 ^ flip, m_regs[5]);
  }

  uint8_t m_select;
  uint8_t m_regs[8];
  uint8_t m_irqLatch, m_irqCounter;
  bool    m_irqReload, m_irqEnabled;
  bool    m_fourScreen;
};


///////////////////////////////////////////////////////////////////////////////
// Mapper 7: AxROM (Battletoads, Marble Madness, Rare's games). One register anywhere in
// $8000-$FFFF: bits 0-2 select a 32 KB PRG bank, bit 4 the single-screen nametable. 8 KB CHR RAM.
// AOROM boards (NES 2.0 submapper 2) have bus conflicts: the ROM drives the bus at the same time,
// so the value written is ANDed with the ROM byte at that address.

class MapperAxROM : public Mapper {
public:
  using Mapper::Mapper;

  void reset() override
  {
    m_banks = m_nes.cart().info.prgSize / 0x8000;
    m_busConflicts = m_nes.cart().info.submapper == 2;
    select(0);
    m_nes.mapChr8K(0);
  }

  void writeRegister(uint16_t addr, uint8_t value) override
  {
    if (m_busConflicts)
      value &= m_nes.read(addr);
    select(value);
  }

private:
  void select(uint8_t value)
  {
    uint8_t const * p = m_nes.cart().prg + ((value & 7) % m_banks) * 0x8000;
    for (int i = 0; i < 4; ++i)
      m_nes.mapPrg8K(i, p + i * 0x2000);
    m_nes.setMirroring((value & 0x10) ? Mirroring::SingleB : Mirroring::SingleA);
  }

  int  m_banks;
  bool m_busConflicts;
};


///////////////////////////////////////////////////////////////////////////////
// Mapper 71: Camerica / Codemasters BF909x (Fire Hawk, Micro Machines, Bee 52, the Dizzy games).
// $C000-$FFFF selects a 16 KB PRG bank at $8000, the last bank is fixed at $C000, 8 KB CHR RAM.
// Fire Hawk's board (submapper 1) also has single-screen mirroring at $9000-$9FFF, bit 4.

class MapperCamerica : public Mapper {
public:
  using Mapper::Mapper;

  void reset() override
  {
    Cart const & c = m_nes.cart();
    m_banks = c.info.prgSize / 0x4000;
    m_mirroringControl = c.info.submapper == 1;
    select(0);
    uint8_t const * last = c.prg + (m_banks - 1) * 0x4000;
    m_nes.mapPrg8K(2, last);
    m_nes.mapPrg8K(3, last + 0x2000);
    m_nes.mapChr8K(0);
    m_nes.setMirroring(m_mirroringControl ? Mirroring::SingleA : c.info.mirroring);
  }

  void writeRegister(uint16_t addr, uint8_t value) override
  {
    if (addr >= 0xc000)
      select(value);
    else if (m_mirroringControl && (addr & 0xf000) == 0x9000)
      m_nes.setMirroring((value & 0x10) ? Mirroring::SingleB : Mirroring::SingleA);
  }

private:
  void select(int bank)
  {
    uint8_t const * p = m_nes.cart().prg + (bank % m_banks) * 0x4000;
    m_nes.mapPrg8K(0, p);
    m_nes.mapPrg8K(1, p + 0x2000);
  }

  int  m_banks;
  bool m_mirroringControl;
};


Mapper * createMapper(int number, Nes & nes)
{
  switch (number) {
    case 0:  return new MapperNROM(nes);
    case 1:  return new MapperMMC1(nes);
    case 2:  return new MapperUxROM(nes);
    case 4:  return new MapperMMC3(nes);
    case 7:  return new MapperAxROM(nes);
    case 71: return new MapperCamerica(nes);
    default: return nullptr;
  }
}


bool mapperSupported(int number)
{
  return number <= 2 || number == 4 || number == 7 || number == 71;
}
