#include <string.h>

#include "cart.h"
#include "nes.h"
#include "state.h"


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

  void restore() override  { reset(); }
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
    m_bank = 0;
    restore();
  }

  void restore() override
  {
    Cart const & c = m_nes.cart();
    select(m_bank);
    uint8_t const * last = c.prg + (m_banks - 1) * 0x4000;
    m_nes.mapPrg8K(2, last);
    m_nes.mapPrg8K(3, last + 0x2000);
    m_nes.mapChr8K(0);
    m_nes.setMirroring(c.info.mirroring);
  }

  void state(StateIO & io) override  { io.pod(m_bank); }

  void writeRegister(uint16_t addr, uint8_t value) override
  {
    select(value);
  }

private:
  void select(uint8_t bank)
  {
    m_bank = bank;
    uint8_t const * p = m_nes.cart().prg + (bank % m_banks) * 0x4000;
    m_nes.mapPrg8K(0, p);
    m_nes.mapPrg8K(1, p + 0x2000);
  }

  int     m_banks;
  uint8_t m_bank;
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

  void state(StateIO & io) override
  {
    io.pod(m_shift); io.pod(m_count); io.pod(m_control); io.pod(m_chr0); io.pod(m_chr1); io.pod(m_prg);
  }

  void restore() override  { update(); }

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
    m_mirroring = c.info.mirroring;
    m_nes.setMirroring(c.info.mirroring);
    updatePrg();
    updateChr();
  }

  void state(StateIO & io) override
  {
    io.pod(m_select); io.pod(m_regs); io.pod(m_irqLatch); io.pod(m_irqCounter);
    io.pod(m_irqReload); io.pod(m_irqEnabled); io.pod(m_mirroring);
  }

  void restore() override
  {
    m_nes.setMirroring(m_fourScreen ? Mirroring::FourScreen : m_mirroring);
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
          m_nes.setMirroring(m_mirroring = (value & 1) ? Mirroring::Horizontal : Mirroring::Vertical);
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
  Mirroring m_mirroring;
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
    m_value = 0;
    restore();
  }

  void state(StateIO & io) override  { io.pod(m_value); }

  void restore() override
  {
    select(m_value);
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
    m_value = value;
    uint8_t const * p = m_nes.cart().prg + ((value & 7) % m_banks) * 0x8000;
    for (int i = 0; i < 4; ++i)
      m_nes.mapPrg8K(i, p + i * 0x2000);
    m_nes.setMirroring((value & 0x10) ? Mirroring::SingleB : Mirroring::SingleA);
  }

  int     m_banks;
  bool    m_busConflicts;
  uint8_t m_value;
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
    m_bank = 0;
    m_mirroring = m_mirroringControl ? Mirroring::SingleA : c.info.mirroring;
    restore();
  }

  void state(StateIO & io) override  { io.pod(m_bank); io.pod(m_mirroring); }

  void restore() override
  {
    Cart const & c = m_nes.cart();
    select(m_bank);
    uint8_t const * last = c.prg + (m_banks - 1) * 0x4000;
    m_nes.mapPrg8K(2, last);
    m_nes.mapPrg8K(3, last + 0x2000);
    m_nes.mapChr8K(0);
    m_nes.setMirroring(m_mirroring);
  }

  void writeRegister(uint16_t addr, uint8_t value) override
  {
    if (addr >= 0xc000)
      select(value);
    else if (m_mirroringControl && (addr & 0xf000) == 0x9000)
      m_nes.setMirroring(m_mirroring = (value & 0x10) ? Mirroring::SingleB : Mirroring::SingleA);
  }

private:
  void select(uint8_t bank)
  {
    m_bank = bank;
    uint8_t const * p = m_nes.cart().prg + (bank % m_banks) * 0x4000;
    m_nes.mapPrg8K(0, p);
    m_nes.mapPrg8K(1, p + 0x2000);
  }

  int       m_banks;
  bool      m_mirroringControl;
  uint8_t   m_bank;
  Mirroring m_mirroring;
};


///////////////////////////////////////////////////////////////////////////////
// Mapper 5: MMC5 (Castlevania III, the Koei games, Just Breed). Registers at $5100-$5206:
//   $5100 PRG mode (32/16/16+8/8 KB), $5114-$5117 PRG banks (bit 7: ROM, else PRG RAM)
//   $5101 CHR mode (8/4/2/1 KB), $5120-$5127 CHR set A, $5128-$512B set B, $5130 upper bits.
//     With 8x16 sprites, sprites use set A and the background set B; with 8x8 sprites both use
//     the set written last. (So the MMC5 watches PPUCTRL.)
//   $5104 ExRAM mode, $5105 nametable mapping (per slot: CIRAM A, CIRAM B, ExRAM, fill),
//   $5106/$5107 fill tile and attribute, $5C00-$5FFF ExRAM (1 KB)
//   $5203 IRQ scanline, $5204 IRQ enable (write) / pending + in frame (read), $5205/6 multiplier
// The scanline counter: the MMC5 sees a frame start at line 0 and counts lines from there; it
// fires at about dot 4 of the compared line (lineStart).
// Not emulated: ExRAM's extended attribute mode, the vertical split, the sound channels (an NES
// cannot play them anyway), PRG RAM beyond the 8 KB at $6000.

class MapperMMC5 : public Mapper {
public:
  MapperMMC5(Nes & nes) : Mapper(nes)
  {
    wantsLineStart = true;
    wantsPpuCtrl = true;
  }

  void reset() override
  {
    m_prgMode = 3;
    m_chrMode = 3;
    m_exramMode = 0;
    for (int i = 0; i < 4; ++i)
      m_prg[i] = 0xff;
    for (int i = 0; i < 12; ++i)
      m_chr[i] = i < 8 ? i : i - 8;
    m_chrUpper = 0;
    m_lastSetB = false;
    m_sprites8x16 = false;
    m_irqCompare = 0;
    m_irqEnabled = m_irqPending = m_inFrame = false;
    m_line = 0;
    m_mulA = m_mulB = 0xff;
    memset(m_exram, 0, sizeof(m_exram));
    m_fillTile = m_fillAttr = 0;
    updateFill();
    m_nt = 0;
    updatePrg();
    updateChr();
    updateNametables();
  }

  void state(StateIO & io) override
  {
    io.pod(m_prgMode); io.pod(m_chrMode); io.pod(m_exramMode); io.pod(m_prg); io.pod(m_chr);
    io.pod(m_chrUpper); io.pod(m_lastSetB); io.pod(m_sprites8x16); io.pod(m_irqCompare);
    io.pod(m_irqEnabled); io.pod(m_irqPending); io.pod(m_inFrame); io.pod(m_line);
    io.pod(m_mulA); io.pod(m_mulB); io.pod(m_nt); io.pod(m_fillTile); io.pod(m_fillAttr); io.pod(m_exram);
  }

  void restore() override
  {
    updateFill();
    updatePrg();
    updateChr();
    updateNametables();
  }

  int readLow(uint16_t addr) override
  {
    switch (addr) {
      case 0x5204: {
        int r = (m_irqPending ? 0x80 : 0) | (m_inFrame ? 0x40 : 0);
        m_irqPending = false;
        m_nes.cpu.clearIrq(Cpu::IRQ_MAPPER);
        return r;
      }
      case 0x5205: return (m_mulA * m_mulB) & 0xff;
      case 0x5206: return (m_mulA * m_mulB) >> 8;
    }
    if (addr >= 0x5c00 && addr <= 0x5fff && m_exramMode >= 2)
      return m_exram[addr - 0x5c00];
    return -1;
  }

  void writeLow(uint16_t addr, uint8_t v) override
  {
    if (addr >= 0x5c00 && addr <= 0x5fff) {
      if (m_exramMode != 3)
        m_exram[addr - 0x5c00] = v;
      return;
    }
    switch (addr) {
      case 0x5100: m_prgMode = v & 3; updatePrg(); break;
      case 0x5101: m_chrMode = v & 3; updateChr(); break;
      case 0x5104: m_exramMode = v & 3; updateNametables(); break;
      case 0x5105: m_nt = v; updateNametables(); break;
      case 0x5106: m_fillTile = v; updateFill(); break;
      case 0x5107: m_fillAttr = v & 3; updateFill(); break;
      case 0x5114: case 0x5115: case 0x5116: case 0x5117:
        m_prg[addr - 0x5114] = v;
        updatePrg();
        break;
      case 0x5130: m_chrUpper = v & 3; break;
      case 0x5203: m_irqCompare = v; break;
      case 0x5204:
        m_irqEnabled = v & 0x80;
        if (m_irqEnabled && m_irqPending)
          m_nes.cpu.setIrq(Cpu::IRQ_MAPPER);
        else if (!m_irqEnabled)
          m_nes.cpu.clearIrq(Cpu::IRQ_MAPPER);
        break;
      case 0x5205: m_mulA = v; break;
      case 0x5206: m_mulB = v; break;
      default:
        if (addr >= 0x5120 && addr <= 0x512b) {
          m_chr[addr - 0x5120] = v | (m_chrUpper << 8);
          m_lastSetB = addr >= 0x5128;
          updateChr();
        }
        break;
    }
  }

  // PRG RAM selected into $8000-$DFFF: writes go there
  void writeRegister(uint16_t addr, uint8_t value) override
  {
    int slot = (addr >> 13) & 3;
    if (m_prgIsRam[slot])
      m_nes.prgRam[addr & 0x1fff] = value;
  }

  void ppuCtrl(uint8_t value) override
  {
    bool tall = value & 0x20;
    if (tall != m_sprites8x16) {
      m_sprites8x16 = tall;
      updateChr();
    }
  }

  void lineStart(int y) override
  {
    if (y == 0) {
      m_inFrame = true;
      m_line = 0;
      return;
    }
    if (!m_inFrame)
      return;
    if (++m_line == m_irqCompare && m_irqCompare) {
      m_irqPending = true;
      if (m_irqEnabled)
        m_nes.cpu.setIrq(Cpu::IRQ_MAPPER);
    }
  }

  void vblank() override
  {
    m_inFrame = false;
  }

private:
  void mapPrg(int slot, uint8_t reg, bool romOnly)
  {
    Cart const & c = m_nes.cart();
    int banks = c.info.prgSize / 0x2000;
    m_prgIsRam[slot] = !romOnly && !(reg & 0x80);
    if (m_prgIsRam[slot])
      m_nes.mapPrg8K(slot, m_nes.prgRam);        // only 8 KB of PRG RAM: every RAM bank is it
    else
      m_nes.mapPrg8K(slot, c.prg + ((reg & 0x7f) % banks) * 0x2000);
  }

  void updatePrg()
  {
    uint8_t const * r = m_prg;
    switch (m_prgMode) {
      case 0:                                      // 32 KB
        for (int i = 0; i < 4; ++i)
          mapPrg(i, ((r[3] & 0x7c) | i) | 0x80, true);
        break;
      case 1:                                      // 16 + 16 KB
        mapPrg(0, (r[1] & 0xfe), false);
        mapPrg(1, (r[1] & 0xfe) | 1, false);
        mapPrg(2, (r[3] & 0x7e) | 0x80, true);
        mapPrg(3, (r[3] & 0x7e) | 0x81, true);
        break;
      case 2:                                      // 16 + 8 + 8 KB
        mapPrg(0, (r[1] & 0xfe), false);
        mapPrg(1, (r[1] & 0xfe) | 1, false);
        mapPrg(2, r[2], false);
        mapPrg(3, r[3] | 0x80, true);
        break;
      default:                                     // 4 x 8 KB
        mapPrg(0, r[0], false);
        mapPrg(1, r[1], false);
        mapPrg(2, r[2], false);
        mapPrg(3, r[3] | 0x80, true);
        break;
    }
    // with only RAM bit 7 meaning "ROM", writes to ROM slots are ignored
  }

  // the 1 KB banks a set maps to $0000-$1FFF (set B only has 4 registers, repeated)
  void setBanks(bool setB, int out[8])
  {
    int const * r = m_chr + (setB ? 8 : 0);
    int n = setB ? 4 : 8;
    auto reg = [&](int i) { return setB ? r[i & 3] : r[i]; };
    (void) n;
    switch (m_chrMode) {
      case 0: for (int i = 0; i < 8; ++i) out[i] = reg(7) * 8 + i; break;
      case 1: for (int i = 0; i < 8; ++i) out[i] = reg(i < 4 ? 3 : 7) * 4 + (i & 3); break;
      case 2: for (int i = 0; i < 8; ++i) out[i] = reg((i | 1)) * 2 + (i & 1); break;
      default: for (int i = 0; i < 8; ++i) out[i] = reg(i); break;
    }
  }

  void updateChr()
  {
    int a[8], b[8];
    setBanks(false, a);
    setBanks(true, b);
    // 8x16 sprites: sprites from A, background from B; 8x8: both from the set written last
    int const * spr = m_sprites8x16 ? a : (m_lastSetB ? b : a);
    int const * bg  = m_sprites8x16 ? b : (m_lastSetB ? b : a);
    for (int i = 0; i < 8; ++i) {
      m_nes.mapChrSpr1K(i, spr[i]);
      m_nes.mapChrBg1K(i, bg[i]);
    }
  }

  void updateFill()
  {
    memset(m_fill, m_fillTile, 0x3c0);
    memset(m_fill + 0x3c0, m_fillAttr * 0x55, 0x40);
  }

  void updateNametables()
  {
    for (int i = 0; i < 4; ++i) {
      int sel = (m_nt >> (i * 2)) & 3;
      uint8_t * page;
      switch (sel) {
        case 0:  page = m_nes.ppu.vram; break;
        case 1:  page = m_nes.ppu.vram + 0x400; break;
        case 2:  page = m_exramMode <= 1 ? m_exram : m_zeros; break;
        default: page = m_fill; break;
      }
      m_nes.mapNametable(i, page);
    }
  }

  uint8_t m_prgMode, m_chrMode, m_exramMode;
  uint8_t m_prg[4];
  bool    m_prgIsRam[4] = { };
  int     m_chr[12];
  uint8_t m_chrUpper;
  bool    m_lastSetB, m_sprites8x16;
  uint8_t m_irqCompare;
  bool    m_irqEnabled, m_irqPending, m_inFrame;
  int     m_line;
  uint8_t m_mulA, m_mulB;
  uint8_t m_nt;
  uint8_t m_fillTile, m_fillAttr;
  uint8_t m_exram[0x400];
  uint8_t m_fill[0x400];
  uint8_t m_zeros[0x400] = { };
};


Mapper * createMapper(int number, Nes & nes)
{
  switch (number) {
    case 0:  return new MapperNROM(nes);
    case 1:  return new MapperMMC1(nes);
    case 2:  return new MapperUxROM(nes);
    case 4:  return new MapperMMC3(nes);
    case 5:  return new MapperMMC5(nes);
    case 7:  return new MapperAxROM(nes);
    case 71: return new MapperCamerica(nes);
    default: return nullptr;
  }
}


bool mapperSupported(int number)
{
  return number <= 2 || number == 4 || number == 5 || number == 7 || number == 71;
}
