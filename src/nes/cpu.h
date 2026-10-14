// Ricoh 2A03 CPU core: a 6502 without decimal mode, all official and the stable unofficial
// opcodes, instruction granular with exact cycle counts (page crossings and branches included).
#pragma once

#include "platform.h"

class Nes;

class Cpu {
public:

  // interrupt request sources, ORed into irqLines; the IRQ line is level triggered
  enum : uint8_t { IRQ_MAPPER = 1, IRQ_FRAME = 2, IRQ_DMC = 4 };

  void init(Nes * nes) { m_nes = nes; }
  void reset();

  // Runs the instructions that end by 'until' (a few cycles more with page crossings or taken
  // branches), so 'cycles' stops at most one instruction short of it. With single, runs exactly
  // one instruction (or interrupt entry) regardless.
  void run(int32_t until, bool single = false);

  // Runs exactly one instruction (or interrupt entry).
  void step()                     { run(cycles, true); }

  void nmi()                      { nmiPending = true; }
  void setIrq(uint8_t source)     { irqLines |= source; }
  void clearIrq(uint8_t source)   { irqLines &= ~source; }

  uint8_t getP() const;
  void    setP(uint8_t p);

  // registers, public for the test harness and the debugger
  uint16_t PC;
  uint8_t  A, X, Y, S;

  // CPU cycles, relative to the start of the current frame (Nes::runFrame rebases it)
  int32_t  cycles;

  bool     nmiPending;
  uint8_t  irqLines;
  bool     jammed;                // hit a KIL opcode

private:

  void interrupt(uint16_t vector, bool brk);

  Nes *    m_nes;

  // flags: N is bit 7 of n, Z is set when z == 0, the others are 0/1
  uint8_t  n, z, C, V, I, D;
};
