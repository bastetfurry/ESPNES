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
  if (m_cart.info.regionKnown)
    setRegion(m_cart.info.region);
  return nullptr;
}


void Nes::eject()
{
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
}


void Nes::powerOn()
{
  memset(ram, 0, sizeof(ram));
  memset(prgRam, 0, 0x2000);
  if (m_chrRam)
    memset(m_chrRam, 0, 0x2000);
  m_strobe = false;
  ppu.reset();
  apu.reset(m_region);
  m_mapper->reset();
  m_baseUnits = 0;
  cpu.cycles  = 0;
  cpu.reset();
}


void Nes::mapChr1K(int slot, int bank)
{
  uint8_t * base = m_chrRam ? m_chrRam : (uint8_t *) m_cart.chr;
  int size = m_chrRam ? 0x2000 : m_cart.info.chrSize;
  ppu.chrMap[slot] = base + ((bank * 0x400) & (size - 1));
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
  if (addr < 0x4000)
    return ppu.readRegister(addr);

  if (addr >= 0x6000)
    return prgRam[addr & 0x1fff];

  switch (addr) {
    case 0x4015:
      apu.run(cpu.cycles);
      return apu.readStatus();
    case 0x4016:
    case 0x4017: {
      int port = addr & 1;
      if (m_strobe)
        return 0x40 | (m_buttons[port] & 1);
      uint8_t bit = m_shift[port] & 1;
      m_shift[port] = (m_shift[port] >> 1) | 0x80;   // reads 1 after the eighth bit
      return 0x40 | bit;                         // the upper bits are open bus, usually $40
    }
    default:
      return addr >> 8;                          // open bus
  }
}


NES_HOT void Nes::writeIO(uint16_t addr, uint8_t value)
{
  if (addr < 0x4000) {
    ppu.writeRegister(addr, value);
    return;
  }

  if (addr >= 0x8000) {
    m_mapper->writeRegister(addr, value);
    return;
  }

  if (addr >= 0x6000) {
    prgRam[addr & 0x1fff] = value;
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
    cpu.cycles += 513 + (cpu.cycles & 1);
    return;
  }

  if (addr == 0x4016) {
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
    busy += nesClock() - t0;
    m_linesDrawn.store(y + 1, std::memory_order_release);
  }
  profDraw = busy;
}


NES_HOT void Nes::runFrame(bool draw)
{
  draw = draw && m_lines;
  bool async = draw && m_kick;
  if (async) {
    m_linesQueued.store(0, std::memory_order_relaxed);
    m_linesDrawn.store(0, std::memory_order_relaxed);
    m_kick(m_kickArg);
  }

  // visible lines 0-239
  for (int y = 0; y < 240; ++y) {
    cpu.run(cycleAt(y, 0));
    if (draw) {
      ppu.captureLine(m_lineStates[y]);
      if (async) {
        m_linesQueued.store(y + 1, std::memory_order_release);
      } else {
        ppu.drawLine(m_lineStates[y], y, m_lines[y], m_colorMap, m_vgaOrder);
      }
    }
    int hitX = ppu.evaluateLine(y);
    // the flag rises about two dots after the pixel goes out
    if (hitX >= 0 && ppu.spr0HitCycle == INT32_MAX)
      ppu.spr0HitCycle = cycleAt(y, hitX + 2);
    cpu.run(cycleAt(y, 256));
    ppu.endOfLine();
    if (ppu.renderingEnabled())
      m_mapper->hblank();
    apu.run(cpu.cycles);
  }

  // line 240 is idle, vblank starts at line 241 dot 1
  cpu.run(cycleAt(241, 1));
  if (async) {
    uint32_t t0 = nesClock();
    while (m_linesDrawn.load(std::memory_order_acquire) < 240)
      ;
    profWait = nesClock() - t0;
  }
  ppu.setVBlank();

  // vblank, a line at a time so the APU (frame IRQ, DMC) keeps up
  int pre = m_linesPerFrame - 1;
  for (int y = 242; y <= pre; ++y) {
    cpu.run(cycleAt(y, 0));
    apu.run(cpu.cycles);
  }

  // pre-render line: the last of the frame
  cpu.run(cycleAt(pre, 1));
  ppu.startFrame();
  cpu.run(cycleAt(pre, 256));
  ppu.endOfLine();
  ppu.copyVertical();
  if (ppu.renderingEnabled())
    m_mapper->hblank();
  cpu.run(cycleAt(m_linesPerFrame, 0));
  apu.run(cpu.cycles);

  // rebase the cycle counter to the start of the next frame
  int32_t frameUnits = m_linesPerFrame * UNITS_PER_LINE;
  int32_t c = (frameUnits - m_baseUnits) / m_unitsPerCycle;
  cpu.cycles -= c;
  apu.rebase(c);
  m_baseUnits += c * m_unitsPerCycle - frameUnits;
}
