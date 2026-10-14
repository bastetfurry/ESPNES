// The console: CPU and PPU on a common bus, the cartridge's bank maps, controllers, frame timing.
#pragma once

#include "platform.h"
#include "cart.h"
#include "cpu.h"
#include "ppu.h"

// controller bits, in the order the shift register reports them
enum : uint8_t {
  BTN_A = 0x01, BTN_B = 0x02, BTN_SELECT = 0x04, BTN_START = 0x08,
  BTN_UP = 0x10, BTN_DOWN = 0x20, BTN_LEFT = 0x40, BTN_RIGHT = 0x80,
};

class Nes {
public:

  // receives each visible line as 256 NES colour indices (0-63)
  typedef void (*LineSink)(void * arg, int y, uint8_t const * colors);

  Nes();
  ~Nes();

  // Takes a ROM image, which must stay valid until eject(). Returns nullptr or an error message.
  char const * insert(uint8_t const * image, size_t size);
  void         eject();
  bool         inserted() const               { return m_mapper != nullptr; }

  void   powerOn();
  void   setRegion(Region region);
  Region region() const                       { return m_region; }
  int    frameRate() const                    { return m_region == Region::PAL ? 50 : 60; }

  // Emulates one frame. With draw = false the picture is not produced (frame skipping).
  void runFrame(bool draw);

  void setButtons(int port, uint8_t buttons)  { m_buttons[port] = buttons; }
  void setLineSink(LineSink sink, void * arg) { m_sink = sink; m_sinkArg = arg; }

  // --- for mappers ---
  Cart const & cart() const                   { return m_cart; }
  void mapPrg8K(int slot, uint8_t const * p)  { prgMap[slot] = p; }
  void mapChr1K(int slot, int bank);
  void mapChr8K(int bank);
  void setMirroring(Mirroring mirroring);

  // --- CPU bus ---
  inline uint8_t read(uint16_t addr);
  inline void    write(uint16_t addr, uint8_t value);

  Cpu             cpu;
  Ppu             ppu;
  uint8_t         ram[2048];
  uint8_t const * prgMap[4];      // 8 KB pages at $8000, $A000, $C000, $E000
  uint8_t *       prgRam;         // 8 KB at $6000

private:

  uint8_t readIO(uint16_t addr);
  void    writeIO(uint16_t addr, uint8_t value);

  // frame relative CPU cycle at which the PPU reaches (line, dot)
  int32_t cycleAt(int line, int dot) const
  {
    return (line * UNITS_PER_LINE + dot * 5 - m_baseUnits + m_unitsPerCycle - 1) / m_unitsPerCycle;
  }

  // Time is counted in fifths of a PPU dot, so that both CPU/PPU clock ratios are whole numbers:
  // a CPU cycle is 3 dots on NTSC (15 units) and 3.2 dots on PAL (16 units).
  static constexpr int UNITS_PER_LINE = 341 * 5;

  Cart       m_cart;
  Mapper *   m_mapper;
  uint8_t *  m_chrRam;
  uint8_t *  m_fourScreenRam;

  Region     m_region;
  int        m_unitsPerCycle;
  int        m_linesPerFrame;
  int32_t    m_baseUnits;        // position of CPU cycle 0 relative to the frame start, <= 0

  uint8_t    m_buttons[2];
  uint8_t    m_shift[2];
  bool       m_strobe;
  uint8_t    m_apuRegs[0x18];    // stored, not yet played

  LineSink   m_sink;
  void *     m_sinkArg;
  uint8_t    m_line[256];
};


inline uint8_t Nes::read(uint16_t addr)
{
  if (addr >= 0x8000)
    return prgMap[(addr >> 13) & 3][addr & 0x1fff];
  if (addr < 0x2000)
    return ram[addr & 0x7ff];
  return readIO(addr);
}


inline void Nes::write(uint16_t addr, uint8_t value)
{
  if (addr < 0x2000)
    ram[addr & 0x7ff] = value;
  else
    writeIO(addr, value);
}
