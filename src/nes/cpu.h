// Ricoh 2A03 CPU core: a 6502 without decimal mode, all official and the stable unofficial
// opcodes, instruction granular with exact cycle counts (page crossings and branches included).
#pragma once

#include <limits.h>

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

  void nmi()                      { nmiPending = NMI_NOW; }
  // An NMI enabled by a register write is polled before the write cycle, so it is taken only
  // after the instruction following the write.
  void nmiDelayed()               { if (!nmiPending) nmiPending = NMI_AFTER_NEXT; }
  // The NMI edge at 'cycle', from outside the instruction stream: the CPU polls interrupts
  // before an instruction's last cycle, so the first instruction to end after the edge takes it
  // right after itself, unless the edge fell into its last cycle; then one instruction later.
  void nmiAtCycle(int32_t cycle)  { nmiPending = NMI_EDGE; m_nmiEdge = cycle; }
  void cancelNmi()                { nmiPending = 0; }
  void setIrq(uint8_t source)     { irqLines |= source; }
  void clearIrq(uint8_t source)   { irqLines &= ~source; }

  uint8_t getP() const;
  void    setP(uint8_t p);

  // registers, public for the test harness and the debugger
  uint16_t PC;
  uint8_t  A, X, Y, S;

  // CPU cycles, relative to the start of the current frame (Nes::runFrame rebases it)
  int32_t  cycles;

  // NMI_AFTER_NEXT turns into NMI_AFTER_THIS at the end of the instruction that set it, which
  // turns into NMI_NOW at the end of the next one
  enum : uint8_t { NMI_NOW = 1, NMI_AFTER_THIS = 2, NMI_AFTER_NEXT = 3, NMI_EDGE = 4 };
  uint8_t  nmiPending;
  uint8_t  irqLines;
  bool     jammed;                // hit a KIL opcode

private:

  void interrupt(uint16_t vector, bool brk);
  void runCold(uint8_t op, int32_t until);

  Nes *    m_nes;
  int32_t  m_nmiEdge;
  int32_t  m_iChanged = INT32_MIN;  // cycles after the last CLI/SEI/PLP, and the I flag before it
  uint8_t  m_iOld = 1;

  // flags: N is bit 7 of n, Z is set when z == 0, the others are 0/1
  uint8_t  n, z, C, V, I, D;
};
