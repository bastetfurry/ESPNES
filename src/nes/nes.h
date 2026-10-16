// The console: CPU and PPU on a common bus, the cartridge's bank maps, controllers, frame timing.
#pragma once

#include <atomic>
#include <limits.h>
#include <string.h>

#include "platform.h"
#include "cart.h"
#include "cpu.h"
#include "ppu.h"
#include "apu.h"
#include "cheat.h"
#include "state.h"
#include "tape.h"
#include "famikey.h"
#include "nsf.h"

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

  // Takes a ROM image or an NSF (see nsf.h), which must stay valid until eject(). Returns nullptr
  // or an error message.
  char const * insert(uint8_t const * image, size_t size);
  void         eject();
  bool         inserted() const               { return m_mapper != nullptr; }

  void   powerOn();

  // --- NSF music (see nsf.h) ---
  bool           isNsf() const                { return m_isNsf; }
  NsfInfo const & nsf() const                 { return m_nsfInfo; }
  // Starts song 1-songs: power on, INIT; runFrame() calls PLAY at the play rate.
  void           nsfStart(int song);
  int            nsfSong() const              { return m_nsfSong; }
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
  // The Famicom's microphone on the second pad: while on, $4016 bit 2 reads noise, like a voice.
  void setMic(bool on)                        { m_mic = on; }
  // The Data Recorder (see tape.h), nullptr for none. Its record, play and stop go through here,
  // between frames: the tape counts time in the console's cycles.
  void setTape(Tape * tape)                   { m_tape = tape; }
  Tape * tape() const                         { return m_tape; }
  void tapeRecord(size_t capacity)
  {
    m_tape->record(cpu.cycles, m_expOut & 1, m_region == Region::PAL ? 1662607 : 1789773, capacity);
  }
  void tapePlay()                             { m_tape->play(cpu.cycles); }
  // The Family BASIC keyboard (see famikey.h): plugged in or not, and its keys.
  void setFamiKeyboard(bool on)               { m_famiKeyboard = on; releaseFamiKeys(); }
  bool famiKeyboard() const                   { return m_famiKeyboard; }
  void setFamiKey(FamiKey key, bool down)
  {
    if (key >= FK_COUNT)
      return;
    uint8_t bit = 2 << (key & 3);
    if (down)
      m_famiKeys[key >> 2] |= bit;
    else
      m_famiKeys[key >> 2] &= ~bit;
  }
  void releaseFamiKeys()                      { memset(m_famiKeys, 0, sizeof(m_famiKeys)); }
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

  // --- savestates (see state.h) ---
  // Between frames only. A state loads only into the game it was saved from (the caller makes sure
  // of that, e.g. by the file name); the cheats, the overclock and the buttons stay as they are,
  // the TV system becomes the state's. loadState returns nullptr or an error message; after an
  // error the console is powered on afresh.
  bool         saveState(StateSink & sink);
  char const * loadState(StateSource & source);
  size_t       stateSizeLimit() const;        // an upper bound of a saved state's size, in bytes

  // --- cheats ---
  // Game Genie codes patch what the CPU reads from ROM without slowing it down: an 8 KB page a
  // code applies to is copied to RAM, patched, and mapped instead (see patchedPage). Action
  // Replay codes are written to RAM at the start of every frame.
  static constexpr int MAX_CHEATS = 16;
  void setCheats(Cheat const * list, int count);
  void enableCheats(bool on);
  bool cheatsEnabled() const                  { return m_cheatsOn; }
  int  cheatCount() const                     { return m_cheatCount; }

  // --- ROM patches: Game Genie style, but always on (not cheats), e.g. the tape traps' stubs ---
  static constexpr int MAX_PATCHES = 48;
  void setRomPatches(Cheat const * list, int count);

  // --- the Wii Virtual Console's call ---
  // A game writes $96 to $6000, then $C3 to $6001, and waits until the two are equal; the
  // emulator does its work in between (Mach Rider's VC versions save and load their courses so).
  // While enabled, vcCall() gives the return address on top of the stack at that moment (which
  // of the game's calls it is), 0 for none; finishVcCall() lets the game go on. Between frames.
  void     enableVcCall(bool on)              { m_vcCallOn = on; m_vcCall = 0; }
  uint16_t vcCall() const                     { return m_vcCall; }
  void     finishVcCall()                     { prgRam[0] = prgRam[1]; m_vcCall = 0; }

  // --- ESPNES's own call, for patched games whose RAM at $6000 is in use (Family BASIC) ---
  // The game writes an operation (1-255) to $4018 (the APU's unused test range) and waits while
  // $4018 reads $80; finishPortCall(result) makes it read the result (below $80). While enabled,
  // portCall() gives the waiting operation, 0 for none. Between frames.
  void     enablePortCall(bool on)            { m_portOn = on; m_portCall = 0; m_portResult = 0; }
  uint8_t  portCall() const                   { return m_portCall; }
  void     finishPortCall(uint8_t result)     { m_portResult = result & 0x7f; m_portCall = 0; }

  // --- for mappers ---
  Cart const & cart() const                   { return m_cart; }
  void mapPrg8K(int slot, uint8_t const * p)
  {
    m_prgSource[slot] = p;
    prgMap[slot] = NES_UNLIKELY(m_romPatched) ? patchedPage(slot, p) : p;
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
  bool       m_mic = false;
  bool       m_isNsf = false;
  NsfInfo    m_nsfInfo;
  int        m_nsfSong = 0;
  int64_t    m_nsfTime = 0;      // NSF: microseconds of play time not yet given to PLAY
  bool       m_fdsAudio = false; // $4040-$4097 are the FDS sound's
  bool       m_vrc6Audio = false; // $9000-$B002 are (also) the VRC6 sound's
  bool       m_mmc5Audio = false; // $5000-$5015 are the MMC5 sound's
  bool       m_s5bAudio = false;  // $C000/$E000 are (also) the 5B sound's
  bool       m_n163Audio = false; // $4800 and $F800 are the N163 sound's
  void       nsfCall(uint16_t addr);
  uint16_t   m_micNoise = 1;     // LFSR
  uint8_t    m_expOut = 0;       // the last $4016 write's bits 0-2 (the expansion port's outputs)
  bool       m_famiKeyboard = false;
  uint8_t    m_famiKeys[20] = { };  // per row and column the held keys, in bits 1-4; row 9 has none
  uint8_t    m_famiRow = 0;
  Tape *     m_tape = nullptr;

  bool            state(StateIO & io);

  uint8_t const * patchedPage(int slot, uint8_t const * page);
  void            remapPrg();
  void            freePatchedPages();

  Cheat           m_cheats[MAX_CHEATS];
  int             m_cheatCount = 0;
  bool            m_cheatsOn = false;
  bool            m_romPatched = false;         // cheats on with a Game Genie code, or ROM patches
  Cheat           m_patches[MAX_PATCHES];
  int             m_patchCount = 0;
  bool            m_vcCallOn = false;
  uint16_t        m_vcCall = 0;
  bool            m_portOn = false;
  uint8_t         m_portCall = 0;
  uint8_t         m_portResult = 0;
  bool            patches(Cheat const & c, uint16_t base, uint8_t const * page) const
  {
    return c.rom && (uint16_t)(c.addr - base) < 0x2000 && (c.compare < 0 || page[c.addr - base] == c.compare);
  }
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
