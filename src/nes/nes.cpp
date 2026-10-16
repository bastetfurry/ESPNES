#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "nes.h"


Nes::Nes()
  : prgRam(nullptr),
    m_mapper(nullptr),
    m_chrRam(nullptr),
    m_fourScreenRam(nullptr),
    m_strobe(false),
    m_lines(nullptr),
    m_colorMap(nullptr),
    m_vgaOrder(false),
    m_kick(nullptr),
    m_kickArg(nullptr),
    m_linesQueued(0),
    m_linesDrawn(0)
{
  cpu.init(this);
  ppu.init(this);
  apu.init(this);
  profWait = profDraw = 0;
  m_buttons[0] = m_buttons[1] = 0;
  m_shift[0] = m_shift[1] = 0;
  setRegion(Region::NTSC);
}


Nes::~Nes()
{
  eject();
}


char const * Nes::insert(uint8_t const * image, size_t size)
{
  eject();
  if (::isNsf(image, size)) {
    // music: no CHR, the NSF mapper's RAM at $8000-$FFFF
    if (char const * err = parseNsf(image, size, &m_nsfInfo))
      return err;
    memset(&m_cart, 0, sizeof(m_cart));
    m_cart.info.mapper = -1;
    m_cart.info.mirroring = Mirroring::Vertical;
    m_cart.info.regionKnown = true;
    m_cart.info.region = m_nsfInfo.pal && !m_nsfInfo.dual ? Region::PAL : Region::NTSC;
    m_cart.prg = m_nsfInfo.data;
    m_cart.info.prgSize = m_nsfInfo.dataSize;
    prgRam = (uint8_t *) malloc(0x2000);
    m_chrRam = (uint8_t *) malloc(0x2000);
    m_mapper = prgRam && m_chrRam ? createNsfMapper(*this, m_nsfInfo) : nullptr;
    if (!m_mapper) {
      eject();
      return "Out of memory";
    }
    m_isNsf = true;
    m_fdsAudio = m_nsfInfo.chips & NSF_FDS;
    m_vrc6Audio = m_nsfInfo.chips & NSF_VRC6;
    m_mmc5Audio = m_nsfInfo.chips & NSF_MMC5;
    ppu.chrWritable = true;
    memset(prgRam, 0, 0x2000);
    setRegion(m_cart.info.region);
    return nullptr;
  }
  char const * err = parseRom(image, size, &m_cart);
  if (err)
    return err;
  if (!mapperSupported(m_cart.info.mapper))
    return "Mapper not supported yet";

  prgRam = (uint8_t *) malloc(0x2000);
  if (!m_cart.chr)
    m_chrRam = (uint8_t *) malloc(0x2000);
  if (m_cart.info.mirroring == Mirroring::FourScreen)
    m_fourScreenRam = (uint8_t *) malloc(0x800);
  m_mapper = createMapper(m_cart.info.mapper, *this);
  if (!prgRam || (!m_cart.chr && !m_chrRam) || (m_cart.info.mirroring == Mirroring::FourScreen && !m_fourScreenRam) || !m_mapper) {
    eject();
    return "Out of memory";
  }
  ppu.chrWritable = m_chrRam != nullptr;
  // PRG RAM keeps its contents over a reset (and a battery keeps it over power off: the caller
  // loads the save into prgRam after insert())
  memset(prgRam, 0, 0x2000);
  if (m_cart.info.regionKnown)
    setRegion(m_cart.info.region);
  return nullptr;
}


void Nes::eject()
{
  freePatchedPages();
  m_cheatCount = 0;
  m_patchCount = 0;
  m_cheatsOn = m_romPatched = false;
  m_vcCallOn = false;
  m_vcCall = 0;
  m_isNsf = false;
  m_fdsAudio = false;
  m_vrc6Audio = false;
  m_mmc5Audio = false;
  m_portOn = false;
  m_portCall = 0;
  delete m_mapper;
  m_mapper = nullptr;
  free(prgRam);
  prgRam = nullptr;
  free(m_chrRam);
  m_chrRam = nullptr;
  free(m_fourScreenRam);
  m_fourScreenRam = nullptr;
}


void Nes::setRegion(Region region)
{
  m_region        = region;
  m_unitsPerCycle = region == Region::PAL ? 16 : 15;
  m_linesPerFrame = region == Region::PAL ? 312 : 262;
  m_extraLines    = m_linesPerFrame * m_overclock / 100;
}


void Nes::setOverclock(int percent)
{
  m_overclock  = percent < 0 ? 0 : percent > MAX_OVERCLOCK ? MAX_OVERCLOCK : percent;
  m_extraLines = m_linesPerFrame * m_overclock / 100;
}


void Nes::powerOn()
{
  memset(ram, 0, sizeof(ram));
  if (m_chrRam)
    memset(m_chrRam, 0, 0x2000);
  m_strobe = false;
  m_expOut = 0;
  m_famiRow = 0;
  ppu.reset();
  apu.reset(m_region);
  m_mapper->reset();
  m_baseUnits = 0;
  m_parityBase = 0;
  cpu.cycles  = 0;
  frameCount  = 0;
  cpu.reset();
}


// The state's header: a format version (raise it whenever a state() function changes), then
// what the console must look like for the state to fit.
static constexpr uint32_t STATE_MAGIC = 0x53534e45;     // "ENSS"
static constexpr uint16_t STATE_VERSION = 1;

bool Nes::state(StateIO & io)
{
  uint32_t magic = STATE_MAGIC;
  uint16_t version = STATE_VERSION, mapper = m_cart.info.mapper;
  uint8_t  layout = (m_chrRam ? 1 : 0) | (m_fourScreenRam ? 2 : 0);
  Region   region = m_region;
  io.pod(magic); io.pod(version); io.pod(mapper); io.pod(layout); io.pod(region);
  if (io.loading()) {
    if (!io.ok() || magic != STATE_MAGIC || version != STATE_VERSION || mapper != m_cart.info.mapper ||
        layout != ((m_chrRam ? 1 : 0) | (m_fourScreenRam ? 2 : 0)) || (region != Region::NTSC && region != Region::PAL))
      return false;
    setRegion(region);
  }

  cpu.state(io);
  ppu.state(io);
  apu.state(io);
  m_mapper->state(io);
  io.pod(ram);
  io.bytes(prgRam, PRG_RAM_SIZE);
  io.optional(m_chrRam, 0x2000, m_chrRam);
  io.optional(m_fourScreenRam, 0x800, m_fourScreenRam);
  io.pod(bus); io.pod(frameCount); io.pod(m_baseUnits); io.pod(m_parityBase);
  io.pod(m_shift); io.pod(m_strobe); io.pod(m_micNoise);
  return io.ok();
}


bool Nes::saveState(StateSink & sink)
{
  StateWriter w(sink);
  state(w);
  return w.finish();
}


char const * Nes::loadState(StateSource & source)
{
  StateReader r(source);
  if (!state(r)) {
    // a half loaded state is no state: start the game afresh
    powerOn();
    return r.ok() ? "Saved by another version, or another game" : "The state is damaged";
  }
  m_mapper->restore();
  m_dmaArmed = INT32_MAX;
  return nullptr;
}


// everything stored, with the worst case of the compression (a literal byte per 128) and the
// registers generously
size_t Nes::stateSizeLimit() const
{
  size_t raw = 2048 + 2048 + 32 + 256 + PRG_RAM_SIZE + (m_chrRam ? 0x2000 : 0) + (m_fourScreenRam ? 0x800 : 0) + 4096;
  return raw + raw / 64;
}


void Nes::setCheats(Cheat const * list, int count)
{
  m_cheatCount = count < MAX_CHEATS ? count : MAX_CHEATS;
  for (int i = 0; i < m_cheatCount; ++i)
    m_cheats[i] = list[i];
  enableCheats(m_cheatsOn);
}


void Nes::enableCheats(bool on)
{
  m_cheatsOn = on;
  bool rom = false;
  for (int i = 0; i < m_cheatCount; ++i)
    rom |= m_cheats[i].rom;
  m_romPatched = (on && rom) || m_patchCount;
  freePatchedPages();
  remapPrg();
}


void Nes::setRomPatches(Cheat const * list, int count)
{
  m_patchCount = count < MAX_PATCHES ? count : MAX_PATCHES;
  for (int i = 0; i < m_patchCount; ++i)
    m_patches[i] = list[i];
  enableCheats(m_cheatsOn);
}


void Nes::remapPrg()
{
  for (int i = 0; i < 4; ++i)
    if (m_prgSource[i])
      mapPrg8K(i, m_prgSource[i]);
}


void Nes::freePatchedPages()
{
  for (auto & p : m_patched) {
    free(p.copy);
    p.copy = nullptr;
    p.source = nullptr;
  }
  // nothing may point into a freed copy any more
  for (int i = 0; i < 4; ++i)
    if (m_prgSource[i])
      prgMap[i] = m_prgSource[i];
}


// The page to map at 'slot' for the ROM page 'page': a patched RAM copy if a Game Genie code or
// a ROM patch applies to it (with a compare value: only where the ROM holds it), else the page.
uint8_t const * Nes::patchedPage(int slot, uint8_t const * page)
{
  uint16_t base = 0x8000 + slot * 0x2000;
  int cheats = m_cheatsOn ? m_cheatCount : 0;
  bool applies = false;
  for (int i = 0; i < cheats && !applies; ++i)
    applies = patches(m_cheats[i], base, page);
  for (int i = 0; i < m_patchCount && !applies; ++i)
    applies = patches(m_patches[i], base, page);
  if (!applies)
    return page;

  for (auto & p : m_patched)
    if (p.source == page)
      return p.copy;

  // a new copy: a free entry, or one that is not mapped at the moment
  for (auto & p : m_patched) {
    bool mapped = false;
    for (int i = 0; i < 4; ++i)
      mapped |= p.copy && prgMap[i] == p.copy;
    if (p.copy && mapped)
      continue;
    if (!p.copy && !(p.copy = (uint8_t *) malloc(0x2000)))
      return page;                                // out of RAM: unpatched
    memcpy(p.copy, page, 0x2000);
    for (int i = 0; i < m_patchCount; ++i)
      if (patches(m_patches[i], base, page))
        p.copy[m_patches[i].addr - base] = m_patches[i].value;
    for (int i = 0; i < cheats; ++i)
      if (patches(m_cheats[i], base, page))
        p.copy[m_cheats[i].addr - base] = m_cheats[i].value;
    p.source = page;
    return p.copy;
  }
  return page;                                    // all entries mapped: unpatched
}


static inline uint8_t * chrPage(Cart const & cart, uint8_t * chrRam, int bank)
{
  uint8_t * base = chrRam ? chrRam : (uint8_t *) cart.chr;
  int size = chrRam ? 0x2000 : cart.info.chrSize;
  return base + ((bank * 0x400) & (size - 1));
}


void Nes::mapChr1K(int slot, int bank)
{
  ppu.chrMap[slot] = ppu.sprChrMap[slot] = chrPage(m_cart, m_chrRam, bank);
}


void Nes::mapChrBg1K(int slot, int bank)
{
  ppu.chrMap[slot] = chrPage(m_cart, m_chrRam, bank);
}


void Nes::mapChrSpr1K(int slot, int bank)
{
  ppu.sprChrMap[slot] = chrPage(m_cart, m_chrRam, bank);
}


void Nes::mapChr8K(int bank)
{
  for (int i = 0; i < 8; ++i)
    mapChr1K(i, bank * 8 + i);
}


void Nes::setMirroring(Mirroring mirroring)
{
  static const uint8_t layout[4][4] = {
    { 0, 0, 1, 1 },   // horizontal
    { 0, 1, 0, 1 },   // vertical
    { 0, 0, 0, 0 },   // single screen A
    { 1, 1, 1, 1 },   // single screen B
  };
  if (mirroring == Mirroring::FourScreen) {
    ppu.ntMap[0] = ppu.vram;
    ppu.ntMap[1] = ppu.vram + 0x400;
    ppu.ntMap[2] = m_fourScreenRam;
    ppu.ntMap[3] = m_fourScreenRam + 0x400;
  } else {
    for (int i = 0; i < 4; ++i)
      ppu.ntMap[i] = ppu.vram + layout[(int)mirroring][i] * 0x400;
  }
}


NES_HOT uint8_t Nes::readIO(uint16_t addr)
{
  if (NES_UNLIKELY(cpu.cycles - 1 >= m_dmaArmed))
    dmaInRead(addr);

  if (addr < 0x4000)
    return bus = ppu.readRegister(addr);

  if (addr >= 0x6000)
    return bus = prgRam[addr & 0x1fff];

  switch (addr) {
    case 0x4015: {
      apu.run(cpu.cycles);
      // bit 5 is open bus, and the read does not change the external data bus
      uint8_t r = (apu.readStatus() & ~0x20) | (bus & 0x20);
#ifdef NES_TRACE
      if (nesTraceRead)
        nesTraceRead(*this, addr, r);
#endif
      return r;
    }
    case 0x4016:
    case 0x4017: {
      int port = addr & 1;
      uint8_t bit;
      if (m_strobe) {
        bit = m_buttons[port] & 1;
      } else {
        bit = m_shift[port] & 1;
        m_shift[port] = (m_shift[port] >> 1) | 0x80;   // reads 1 after the eighth bit
      }
      if (!port && m_mic) {
        m_micNoise = (m_micNoise >> 1) ^ (-(m_micNoise & 1) & 0xb400);
        bit |= m_micNoise & 4;
      }
      if (!port && m_tape && (m_expOut & 4))
        bit |= m_tape->input(cpu.cycles) << 1;
      if (port && m_famiKeyboard && (m_expOut & 4))
        bit |= ~m_famiKeys[m_famiRow * 2 + ((m_expOut >> 1) & 1)] & 0x1e;
#ifdef NES_TRACE
      if (nesTraceRead)
        nesTraceRead(*this, addr, (bus & 0xe0) | bit);
#endif
      // bit 0 is driven (and the mic's bit 2, the tape's bit 1, the keyboard's bits 1-4), bits 5-7
      // are open bus; bits 1-4 read 0 without anything on the expansion port
      return bus = (bus & 0xe0) | bit;
    }
    case 0x4018:
      if (m_portOn)
        return bus = m_portCall ? 0x80 : m_portResult;
      return bus;
    default:
      if (NES_UNLIKELY(m_mmc5Audio) && addr == 0x5015) {
        apu.run(cpu.cycles);
        return bus = (bus & ~3) | apu.readMmc5Status();
      }
      if (NES_UNLIKELY(m_fdsAudio) && addr >= 0x4040 && addr < 0x4098) {
        apu.run(cpu.cycles);
        int r = apu.fds.read(addr);
        if (r >= 0)
          return bus = r;
      }
      if (addr >= 0x4020) {
        int r = m_mapper->readLow(addr);
        if (r >= 0)
          return bus = r;
      }
      return bus;                                // open bus: nothing drives it
  }
}


#ifdef NES_TRACE
void (*nesTraceWrite)(Nes & nes, uint16_t addr, uint8_t value);
void (*nesTraceRead)(Nes & nes, uint16_t addr, uint8_t value);
#endif

NES_HOT void Nes::writeIO(uint16_t addr, uint8_t value)
{
#ifdef NES_TRACE
  if (nesTraceWrite && (addr < 0x4020 || (addr >= 0x5000 && addr < 0x6000)))
    nesTraceWrite(*this, addr, value);
#endif
  if (addr < 0x4000) {
    ppu.writeRegister(addr, value);
    return;
  }

  if (addr >= 0x8000) {
    if (NES_UNLIKELY(m_vrc6Audio)) {
      uint16_t a = addr & 0xf003;
      if (a >= 0x9000 && a < 0xc000 && ((a & 3) != 3 || a == 0x9003)) {
        apu.run(cpu.cycles);
        apu.vrc6.write(a, value);
      }
    }
    m_mapper->writeRegister(addr, value);
    return;
  }

  if (addr >= 0x6000) {
    prgRam[addr & 0x1fff] = value;
    if (NES_UNLIKELY(m_vcCallOn) && addr == 0x6001 && value == 0xc3 && prgRam[0] == 0x96)
      m_vcCall = ram[0x100 | (uint8_t)(cpu.S + 1)] | ram[0x100 | (uint8_t)(cpu.S + 2)] << 8;
    return;
  }

  if (addr >= 0x4020) {
    if (NES_UNLIKELY(m_mmc5Audio) && addr >= 0x5000 && addr <= 0x5015) {
      apu.run(cpu.cycles);
      apu.writeMmc5(addr, value);
      return;
    }
    if (NES_UNLIKELY(m_fdsAudio) && addr >= 0x4040 && addr < 0x4098) {
      apu.run(cpu.cycles);
      apu.fds.write(addr, value);
      return;
    }
    m_mapper->writeLow(addr, value);
    return;
  }

  if (addr == 0x4014) {
    // OAM DMA: 256 bytes from page 'value', the CPU is halted for 513 or 514 cycles
    uint16_t src = value << 8;
    if (src < 0x2000) {
      ppu.oamDma(ram + (src & 0x7ff));
    } else {
      uint8_t page[256];
      for (int i = 0; i < 256; ++i)
        page[i] = read(src + i);
      ppu.oamDma(page);
    }
    // a halt cycle, one more if needed so the reads land on "get" (even) cycles, then 256
    // get/put pairs: the CPU always resumes on a get cycle
    cpu.cycles += 513 + (isGetCycle(cpu.cycles) ? 1 : 0);
    return;
  }

  if (addr == 0x4016) {
    if (m_famiKeyboard) {
      if (value & 1)
        m_famiRow = 0;
      else if ((m_expOut & 2) && !(value & 2))   // column 1 -> 0: the next row
        m_famiRow = m_famiRow == 9 ? 0 : m_famiRow + 1;
    }
    m_expOut = value & 7;
    if (m_tape)
      m_tape->output(cpu.cycles, value & 1);
    m_strobe = value & 1;
    if (m_strobe) {
      m_shift[0] = m_buttons[0];
      m_shift[1] = m_buttons[1];
    }
    return;
  }

  if (addr < 0x4018) {
    apu.run(cpu.cycles);
    apu.write(addr, value);
  } else if (addr == 0x4018 && m_portOn && value) {
    m_portCall = value;
  }
}


void Nes::drawQueuedLines()
{
  uint32_t busy = 0;
  for (int y = 0; y < 240; ++y) {
    while (m_linesQueued.load(std::memory_order_acquire) <= y)
      ;
    uint32_t t0 = nesClock();
    ppu.drawLine(m_lineStates[y], y, m_lines[y], m_colorMap, m_vgaOrder);
    if (m_lineHook)
      m_lineHook(m_lineHookArg, y, m_lines[y]);
    busy += nesClock() - t0;
    m_linesDrawn.store(y + 1, std::memory_order_release);
  }
  profDraw = busy;
}


// Runs the CPU to 'until', halting it for the DMC's sample fetches (DMA) on the way. The
// instruction the DMA falls into runs "armed": if it reads I/O or open bus on or after the DMA's
// cycle, the DMA lands on that read (see dmaInRead). Otherwise the DMA happens after it. That is a
// best effort: the halt on RAM/ROM reads is not placed exactly, which only matters for timing.
// It costs nothing while the DMC is idle.
NES_HOT void Nes::runCpu(int32_t until)
{
  // NSF: waiting for the next PLAY in the idle loop changes nothing, so the time is skipped
  if (NES_UNLIKELY(m_isNsf) && cpu.PC == NSF_IDLE) {
    if (cpu.cycles < until)
      cpu.cycles = until;
    return;
  }
  for (;;) {
    int32_t dma = apu.nextDmaCycle();
    if (NES_LIKELY(dma >= until)) {
      cpu.run(until);
      return;
    }
    cpu.run(dma);
    // the instruction the DMA falls into, unless that could run past 'until' (an event)
    if (cpu.cycles < dma && dma + 8 <= until) {
      m_dmaArmed = dma;
      cpu.step();
      m_dmaArmed = INT32_MAX;
    }
    if (apu.nextDmaCycle() == dma) {         // did not land on a read: right after the instruction
      apu.run(cpu.cycles > dma ? cpu.cycles : dma);
      cpu.cycles += apu.takeDmaStall();
    }
  }
}


// A DMA landing on a read of 'addr': the CPU halts on that read, and the halt and alignment
// cycles read the address again (side effects on $2002, $2007, $4015, $4016), then the DMA fetches
// the sample byte, which ends up on the data bus. The actual read comes after that.
// Best effort: a DMA due anywhere in the instruction lands on its I/O read. Strictly it would only
// land on it when due in that very cycle (an earlier one halts an opcode or operand fetch), but
// that needs the halt placed exactly in every instruction, including the delay past write cycles,
// which the instruction-granular CPU does not model; tried, it scored worse (39 against 42).
void Nes::dmaInRead(uint16_t addr)
{
  int32_t at = m_dmaArmed;
  m_dmaArmed = INT32_MAX;
  int stall = isGetCycle(at) ? 3 : 4;
  for (int i = 1; i < stall; ++i)
    readIO(addr);
  apu.run(at);                               // the fetch, its byte stays on the data bus
  apu.takeDmaStall();
  cpu.cycles += stall;
}


// NSF: a call of INIT or PLAY, returning to the idle loop
void Nes::nsfCall(uint16_t addr)
{
  uint16_t back = NSF_IDLE - 1;                // RTS adds 1
  ram[0x100 | cpu.S--] = back >> 8;
  ram[0x100 | cpu.S--] = back & 0xff;
  cpu.PC = addr;
}


void Nes::nsfStart(int song)
{
  if (!m_isNsf)
    return;
  m_nsfSong = song < 1 ? 1 : song > m_nsfInfo.songs ? m_nsfInfo.songs : song;
  memset(prgRam, 0, 0x2000);
  powerOn();                                   // RAM cleared, the mapper loads the banks
  apu.fdsOn = m_fdsAudio;
  apu.vrc6On = m_vrc6Audio;
  apu.mmc5On = m_mmc5Audio;
  // the APU as an NSF expects it
  for (uint16_t a = 0x4000; a < 0x4014; ++a)
    write(a, 0);
  write(0x4015, 0x0f);
  write(0x4017, 0x40);
  if (m_fdsAudio) {
    write(0x4089, 0x80);                       // the BIOS's setup: wave writable, envelopes
    write(0x408a, 0xe8);
  }
  cpu.A = m_nsfSong - 1;
  cpu.X = region() == Region::PAL ? 1 : 0;
  cpu.Y = 0;
  cpu.S = 0xfd;
  cpu.setP(0x04);
  nsfCall(m_nsfInfo.init);
  m_nsfTime = 0;
}


NES_HOT void Nes::runFrame(bool draw)
{
  if (NES_UNLIKELY(m_isNsf)) {
    // PLAY at its rate, once INIT (or the last PLAY) has returned to the idle loop
    m_nsfTime += m_region == Region::PAL ? 19997 : 16639;
    uint32_t period = m_region == Region::PAL ? m_nsfInfo.periodPal : m_nsfInfo.periodNtsc;
    if (cpu.PC >= NSF_IDLE && cpu.PC < NSF_IDLE + 3 && m_nsfTime >= period) {
      m_nsfTime -= period;
      if (m_nsfTime > period)
        m_nsfTime = period;                    // behind: no catching up
      nsfCall(m_nsfInfo.play);
    }
  }

  if (NES_UNLIKELY(m_cheatsOn))
    for (int i = 0; i < m_cheatCount; ++i)
      if (!m_cheats[i].rom)
        write(m_cheats[i].addr, m_cheats[i].value);

  draw = draw && m_lines;
  bool async = draw && m_kick;
  if (async) {
    m_linesQueued.store(0, std::memory_order_relaxed);
    m_linesDrawn.store(0, std::memory_order_relaxed);
    m_kick(m_kickArg);
  }

  // visible lines 0-239
  for (int y = 0; y < 240; ++y) {
    runCpu(cycleAt(y, 0));
    if (draw) {
      ppu.captureLine(m_lineStates[y]);
      if (async) {
        m_linesQueued.store(y + 1, std::memory_order_release);
      } else {
        ppu.drawLine(m_lineStates[y], y, m_lines[y], m_colorMap, m_vgaOrder);
        if (m_lineHook)
          m_lineHook(m_lineHookArg, y, m_lines[y]);
      }
    }
    int hitX = ppu.evaluateLine(y);
    if (NES_UNLIKELY(m_mapper->wantsLineStart) && ppu.renderingEnabled()) {
      runCpu(cycleAt(y, 4));
      m_mapper->lineStart(y);
    }
    // the flag rises about two dots after the pixel goes out
    if (hitX >= 0 && ppu.spr0HitCycle == INT32_MAX)
      ppu.spr0HitCycle = cycleAt(y, hitX + 2);
    runCpu(cycleAt(y, 65));
    ppu.latchEvalStart();
    runCpu(cycleAt(y, 256));
    ppu.endOfLine();
    if (ppu.renderingEnabled())
      m_mapper->hblank();
    apu.run(cpu.cycles);
  }

  // line 240 is idle
  runCpu(cycleAt(241, 0));
  apu.run(cpu.cycles);

  // Overclocking: extra lines here, before vblank, in which only the CPU runs. The picture's
  // timing and the vblank length stay as they are; the APU is frozen so the music keeps its
  // tempo (register writes still take effect).
  int extra = m_extraLines;
  if (extra) {
    apu.setFrozen(true);
    for (int k = 1; k <= extra; ++k)
      runCpu(cycleAt(241 + k, 0));
    apu.run(cpu.cycles);
    apu.setFrozen(false);
  }

  // vblank starts at (what would be) line 241, dot 1
  int vbl = 241 + extra;
  // a $2002 read in the very CPU cycle the flag would be set reads 0 and suppresses the flag
  // and the NMI for this frame; that is the read of an instruction ending on this cycle count
  ppu.vblRaceCycle = cycleAt(vbl, 1);
  ppu.vblSetUnits = unitsAt(vbl, 1);
  // the CPU's NMI input sees the edge a dot before $2002 does (measured with AccuracyCoin)
  ppu.nmiEdgeCycle = cycleAt(vbl, 0);
  runCpu(ppu.vblRaceCycle);
  if (async) {
    uint32_t t0 = nesClock();
    while (m_linesDrawn.load(std::memory_order_acquire) < 240)
      ;
    profWait = nesClock() - t0;
  }
  ppu.setVBlank();
  if (m_mapper->wantsLineStart)
    m_mapper->vblank();

  // vblank, a line at a time so the APU (frame IRQ, DMC) keeps up
  int lines = m_linesPerFrame + extra;
  int pre = lines - 1;
  for (int y = vbl + 1; y <= pre; ++y) {
    runCpu(cycleAt(y, 0));
    apu.run(cpu.cycles);
  }

  // pre-render line: the last of the frame. As at the start of vblank, the NMI input sees the
  // flag drop a dot before $2002 does.
  ppu.nmiClearCycle = cycleAt(pre, 0);
  runCpu(cycleAt(pre, 1));
  ppu.startFrame();
  runCpu(cycleAt(pre, 65));
  ppu.latchEvalStart();
  runCpu(cycleAt(pre, 256));
  ppu.endOfLine();
  ppu.copyVertical();
  if (ppu.renderingEnabled())
    m_mapper->hblank();
  runCpu(cycleAt(lines, 0));
  apu.run(cpu.cycles);
  ppu.nmiClearCycle = INT32_MAX;
  ppu.nmiEdgeCycle = INT32_MIN;

  // rebase the cycle counter to the start of the next frame
  int32_t frameUnits = lines * UNITS_PER_LINE;
  int32_t c = (frameUnits - m_baseUnits) / m_unitsPerCycle;
  cpu.cycles -= c;
  m_parityBase ^= c & 1;
  apu.rebase(c);
  if (m_tape)
    m_tape->rebase(c);
  ++frameCount;
  m_baseUnits += c * m_unitsPerCycle - frameUnits;
}
