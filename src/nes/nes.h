// The console: CPU and PPU on a common bus, the cartridge's bank maps, controllers, frame timing.
#pragma once

#include <atomic>
#include <limits.h>

#include "platform.h"
#include "cart.h"
#include "cpu.h"
#include "ppu.h"
#include "apu.h"
#include "cheat.h"

#ifdef NES_TRACE
class Nes;
// host debugging: called for every write to $2000-$401F (built with -DNES_TRACE only)
extern void (*nesTraceWrite)(Nes & nes, uint16_t addr, uint8_t value);
// host debugging: called for reads of $4015 (built with -DNES_TRACE only)
extern void (*nesTraceRead)(Nes & nes, uint16_t addr, uint8_t value);
#endif

// controller bits, in the order the shift register reports them
enum : uint8_t {
  BTN_A = 0x01, BTN_B = 0x02, BTN_SELECT = 0x04, BTN_START = 0x08,
  BTN_UP = 0x10, BTN_DOWN = 0x20, BTN_LEFT = 0x40, BTN_RIGHT = 0x80,
};

class Nes {
public:

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
  int    linesPerFrame() const                { return m_linesPerFrame + m_extraLines; }

  // Overclocking: gives the CPU 'percent' more cycles per frame (up to MAX_OVERCLOCK), as extra
  // lines before vblank in which the PPU idles and the APU is frozen. Helps games that slow down.
  // On the ESP32 each extra line costs ~23 us: 100% takes PAL to ~17.5 of its 20 ms per frame.
  static constexpr int MAX_OVERCLOCK = 100;
  void   setOverclock(int percent);
  int    overclock() const                    { return m_overclock; }

  // Emulates one frame. With draw = false the picture is not produced (frame skipping).
  void runFrame(bool draw);

  void setButtons(int port, uint8_t buttons)  { m_buttons[port] = buttons; }
  // Where the picture goes: 240 line buffers of 256 bytes (32 bit aligned), the output byte for
  // each of the 64 NES colours, and whether to use FabGL's raw scanline byte order.
  void setVideoOutput(uint8_t * const * lines, uint8_t const * colorMap, bool vgaOrder)
  {
    m_lines = lines;
    m_colorMap = colorMap;
    m_vgaOrder = vgaOrder;
  }

  // Called for every drawn line after the PPU wrote it, on the core that draws (for overlays).
  typedef void (*LineHook)(void * arg, int y, uint8_t * line);
  void setLineHook(LineHook hook, void * arg)  { m_lineHook = hook; m_lineHookArg = arg; }

  // --- drawing on another core ---
  // Without a kick function, runFrame() draws each line itself. With one, runFrame() only
  // snapshots the PPU state per line and calls kick(arg) at the start of each drawn frame; the
  // other core then calls drawQueuedLines(), which returns when the frame's 240 lines are done.
  // runFrame() waits for that before vblank, when games start changing VRAM and OAM.
  typedef void (*KickFunc)(void * arg);
  void setRenderKick(KickFunc kick, void * arg) { m_kick = kick; m_kickArg = arg; }
  void drawQueuedLines();

  // profiling, in CPU clock cycles (0 on the host), of the last drawn frame
  uint32_t   profWait;                  // runFrame waiting for the drawing to finish
  uint32_t   profDraw;                  // drawQueuedLines busy drawing (not waiting for lines)

  // frame relative position of a CPU cycle's start in fifths of a PPU dot (line * 1705 + dot * 5)
  int32_t unitsOfCycle(int32_t cycle) const   { return cycle * m_unitsPerCycle + m_baseUnits; }
  static int32_t unitsAt(int line, int dot)   { return line * UNITS_PER_LINE + dot * 5; }

  // PPU position of the CPU's current cycle (for tracing)
  int currentLine(int * dot) const
  {
    int32_t units = cpu.cycles * m_unitsPerCycle + m_baseUnits;
    if (dot)
      *dot = (units % UNITS_PER_LINE) / 5;
    return units / UNITS_PER_LINE;
  }
  uint32_t frameCount;

  // CPU cycles alternate between "get" (even) and "put" (odd) for the APU and the DMA units;
  // cpu.cycles is frame relative, so the absolute parity needs m_parityBase
  bool isGetCycle(int32_t cycle) const        { return !((cycle ^ m_parityBase) & 1); }

  // --- cheats ---
  // Game Genie codes patch what the CPU reads from ROM without slowing it down: an 8 KB page a
  // code applies to is copied to RAM, patched, and mapped instead (see patchedPage). Action
  // Replay codes are written to RAM at the start of every frame.
  static constexpr int MAX_CHEATS = 16;
  void setCheats(Cheat const * list, int count);
  void enableCheats(bool on);
  bool cheatsEnabled() const                  { return m_cheatsOn; }
  int  cheatCount() const                     { return m_cheatCount; }

  // --- for mappers ---
  Cart const & cart() const                   { return m_cart; }
  void mapPrg8K(int slot, uint8_t const * p)
  {
    m_prgSource[slot] = p;
    prgMap[slot] = NES_UNLIKELY(m_romCheats) ? patchedPage(slot, p) : p;
  }
  void mapChr1K(int slot, int bank);            // background and sprites alike
  void mapChrBg1K(int slot, int bank);          // background (and $2007) only
  void mapChrSpr1K(int slot, int bank);         // sprites only
  void mapChr8K(int bank);
  void setMirroring(Mirroring mirroring);
  void mapNametable(int slot, uint8_t * page)  { ppu.ntMap[slot] = page; }
  void mapperPpuCtrl(uint8_t value)            { if (m_mapper && m_mapper->wantsPpuCtrl) m_mapper->ppuCtrl(value); }

  // --- CPU bus ---
  inline uint8_t read(uint16_t addr);
  inline void    write(uint16_t addr, uint8_t value);

  Cpu             cpu;
  Ppu             ppu;
  Apu             apu;
  uint8_t         ram[2048];
  uint8_t const * prgMap[4];      // 8 KB pages at $8000, $A000, $C000, $E000
  uint8_t *       prgRam;         // 8 KB at $6000, battery backed on some carts
  uint8_t         bus = 0;        // the last value on the CPU data bus (open bus)
  static constexpr int PRG_RAM_SIZE = 0x2000;

private:

  uint8_t readIO(uint16_t addr);
  void    runCpu(int32_t until);
  void    dmaInRead(uint16_t addr);
  int32_t m_dmaArmed = INT32_MAX;    // DMC DMA cycle while its instruction runs (runCpu)
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
  int        m_overclock = 0;      // percent
  int        m_extraLines = 0;
  int32_t    m_baseUnits;        // position of CPU cycle 0 relative to the frame start, <= 0
  uint8_t    m_parityBase = 0;   // flips with every frame of an odd number of cycles

  uint8_t    m_buttons[2];
  uint8_t    m_shift[2];
  bool       m_strobe;

  uint8_t const * patchedPage(int slot, uint8_t const * page);
  void            remapPrg();
  void            freePatchedPages();

  Cheat           m_cheats[MAX_CHEATS];
  int             m_cheatCount = 0;
  bool            m_cheatsOn = false;
  bool            m_romCheats = false;          // on, with at least one Game Genie code
  uint8_t const * m_prgSource[4] = { };         // the pages the mapper selected, unpatched
  static constexpr int PATCH_PAGES = 6;         // 8 KB of RAM each
  struct { uint8_t const * source; uint8_t * copy; } m_patched[PATCH_PAGES] = { };

  uint8_t * const * m_lines;
  uint8_t const *   m_colorMap;
  bool              m_vgaOrder;
  LineHook   m_lineHook = nullptr;
  void *     m_lineHookArg = nullptr;
  KickFunc   m_kick;
  void *     m_kickArg;

  Ppu::LineState    m_lineStates[240];
  std::atomic<int>  m_linesQueued;   // written by runFrame
  std::atomic<int>  m_linesDrawn;    // written by drawQueuedLines
};


// Reads of unmapped addresses ("open bus") return the last value on the CPU data bus. Within an
// instruction, the access before a data read is always an operand (or pointer) fetch, so 'bus' is
// set there by the CPU, on stack accesses, writes and I/O reads, but not on every RAM/ROM read:
// that store cost ~9% of the CPU's time. readIO() leaves it alone for $4015, which only drives
// the CPU's internal bus.
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
  bus = value;
  if (addr < 0x2000)
    ram[addr & 0x7ff] = value;
  else
    writeIO(addr, value);
}
