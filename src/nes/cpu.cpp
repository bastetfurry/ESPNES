#include "cpu.h"
#include "nes.h"


// Base cycles per opcode. Page crossing penalties for reads and branch penalties are added at
// run time; stores and read-modify-write instructions already include their fixed extra cycle.
static const uint8_t NES_FASTDATA s_cycles[256] = {
//0 1 2 3 4 5 6 7 8 9 A B C D E F
  7,6,2,8,3,3,5,5,3,2,2,2,4,4,6,6,  // 0
  2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,  // 1
  6,6,2,8,3,3,5,5,4,2,2,2,4,4,6,6,  // 2
  2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,  // 3
  6,6,2,8,3,3,5,5,3,2,2,2,3,4,6,6,  // 4
  2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,  // 5
  6,6,2,8,3,3,5,5,4,2,2,2,5,4,6,6,  // 6
  2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,  // 7
  2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,  // 8
  2,6,2,6,4,4,4,4,2,5,2,5,5,5,5,5,  // 9
  2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,  // A
  2,5,2,5,4,4,4,4,2,4,2,4,4,4,4,4,  // B
  2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,  // C
  2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,  // D
  2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,  // E
  2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,  // F
};


void Cpu::reset()
{
  A = X = Y = 0;
  S = 0xfd;
  n = 0; z = 1; C = V = D = 0;
  I = 1;
  nmiPending = 0;
  irqLines   = 0;
  jammed     = false;
  PC = m_nes->read(0xfffc) | (m_nes->read(0xfffd) << 8);
  cycles += 7;
}


uint8_t Cpu::getP() const
{
  return (n & 0x80) | (V << 6) | 0x20 | (D << 3) | (I << 2) | ((z == 0) << 1) | C;
}


void Cpu::setP(uint8_t p)
{
  n = p;
  V = (p >> 6) & 1;
  D = (p >> 3) & 1;
  I = (p >> 2) & 1;
  z = ~p & 2;
  C = p & 1;
}


void Cpu::interrupt(uint16_t vector, bool brk)
{
  Nes & nes = *m_nes;
  nes.ram[0x100 | S--] = PC >> 8;
  nes.ram[0x100 | S--] = PC;
  nes.ram[0x100 | S--] = nes.bus = getP() | (brk ? 0x10 : 0);
  I = 1;
  PC = nes.read(vector) | (nes.read(vector + 1) << 8);
}


// addressing modes: each declares 'a', the effective address
#define RD(addr)     nes.read(addr)
#define WR(addr, v)  nes.write(addr, v)
// Operand fetches leave their byte on the bus: it is the last value there before the data access
// (what an open bus read returns). Opcode fetches and RAM/ROM data reads do not need to.
#define OPERAND(addr) (nes.bus = RD(addr))
#define FETCH16()    (RD(PC) | (OPERAND((uint16_t)(PC + 1)) << 8))

// Dummy reads have side effects only on I/O registers ($2000-$401F: $2002 clears vblank, $2007
// advances the address, $4015 and $4016 too), so they are only made there and cost nothing
// for RAM and ROM.
// The accesses themselves are out of line (and in flash): inlined, the memory access code at every
// dummy access made the core 8 KB bigger, too much for the IRAM.
#define IS_IO(addr)  ((uint16_t)((addr) - 0x2000) < 0x2020)
#define DUMMY_READ(addr) { uint16_t d_ = (addr); if (NES_UNLIKELY(IS_IO(d_))) ioDummyRead(nes, d_); }

static void __attribute__((noinline, cold)) ioDummyRead(Nes & nes, uint16_t addr)
{
  nes.read(addr);
}

static void __attribute__((noinline, cold)) ioDummyWrite(Nes & nes, uint16_t addr, uint8_t value)
{
  nes.write(addr, value);
}

#define AM_IMM   uint16_t a = PC++;
#define AM_ZP    uint16_t a = OPERAND(PC++);
#define AM_ZPX   uint16_t a = (uint8_t)(OPERAND(PC++) + X);
#define AM_ZPY   uint16_t a = (uint8_t)(OPERAND(PC++) + Y);
#define AM_ABS   uint16_t a = FETCH16(); PC += 2;
#define AM_ABSX  uint16_t a; { uint16_t b = FETCH16(); PC += 2; a = b + X; DUMMY_READ((b & 0xff00) | (a & 0xff)); }
#define AM_ABSY  uint16_t a; { uint16_t b = FETCH16(); PC += 2; a = b + Y; DUMMY_READ((b & 0xff00) | (a & 0xff)); }
// read forms: one more cycle when indexing crosses a page
#define AM_ABSXP uint16_t a; { uint16_t b = FETCH16(); PC += 2; a = b + X; \
                               if ((a ^ b) & 0xff00) { ++cycles; DUMMY_READ((b & 0xff00) | (a & 0xff)); } }
#define AM_ABSYP uint16_t a; { uint16_t b = FETCH16(); PC += 2; a = b + Y; \
                               if ((a ^ b) & 0xff00) { ++cycles; DUMMY_READ((b & 0xff00) | (a & 0xff)); } }
#define AM_IZX   uint16_t a; { uint8_t zp = RD(PC++) + X; a = nes.ram[zp] | ((nes.bus = nes.ram[(uint8_t)(zp + 1)]) << 8); }
#define AM_IZY   uint16_t a; { uint8_t zp = RD(PC++); uint16_t b = nes.ram[zp] | ((nes.bus = nes.ram[(uint8_t)(zp + 1)]) << 8); \
                               a = b + Y; DUMMY_READ((b & 0xff00) | (a & 0xff)); }
#define AM_IZYP  uint16_t a; { uint8_t zp = RD(PC++); uint16_t b = nes.ram[zp] | ((nes.bus = nes.ram[(uint8_t)(zp + 1)]) << 8); \
                               a = b + Y; if ((a ^ b) & 0xff00) { ++cycles; DUMMY_READ((b & 0xff00) | (a & 0xff)); } }

// operations on a value m
#define NZ(v)      { n = z = (v); }
#define OP_ORA(m)  { A |= (m); NZ(A); }
#define OP_AND(m)  { A &= (m); NZ(A); }
#define OP_EOR(m)  { A ^= (m); NZ(A); }
#define OP_ADC(m)  { uint8_t mv = (m); unsigned s = A + mv + C; V = ((~(A ^ mv) & (A ^ s)) >> 7) & 1; \
                     C = s >> 8; A = s; NZ(A); }
#define OP_SBC(m)  OP_ADC((uint8_t) ~(m))
#define OP_CMP(r, m) { uint8_t mv = (m); C = (r) >= mv; NZ((uint8_t)((r) - mv)); }
#define OP_CMPA(m) OP_CMP(A, m)
#define OP_CMPX(m) OP_CMP(X, m)
#define OP_CMPY(m) OP_CMP(Y, m)
#define OP_LDA(m)  { A = (m); NZ(A); }
#define OP_LDX(m)  { X = (m); NZ(X); }
#define OP_LDY(m)  { Y = (m); NZ(Y); }
#define OP_LAX(m)  { A = X = (m); NZ(A); }
#define OP_BIT(m)  { uint8_t mv = (m); z = A & mv; n = mv; V = (mv >> 6) & 1; }
#define OP_NOP(m)  { (void)(m); }
// read-modify-write: operate on the lvalue m
#define OP_ASL(m)  { C = (m) >> 7; (m) <<= 1; NZ(m); }
#define OP_LSR(m)  { C = (m) & 1; (m) >>= 1; NZ(m); }
#define OP_ROL(m)  { uint8_t c = (m) >> 7; (m) = ((m) << 1) | C; C = c; NZ(m); }
#define OP_ROR(m)  { uint8_t c = (m) & 1; (m) = ((m) >> 1) | (C << 7); C = c; NZ(m); }
#define OP_INC(m)  { ++(m); NZ(m); }
#define OP_DEC(m)  { --(m); NZ(m); }
#define OP_SLO(m)  { OP_ASL(m); OP_ORA(m); }
#define OP_RLA(m)  { OP_ROL(m); OP_AND(m); }
#define OP_SRE(m)  { OP_LSR(m); OP_EOR(m); }
#define OP_RRA(m)  { OP_ROR(m); OP_ADC(m); }
#define OP_DCP(m)  { --(m); OP_CMP(A, m); }
#define OP_ISB(m)  { ++(m); OP_SBC(m); }

#define READ(op, mode, OP)    case op: { mode OP(RD(a)); break; }
#define READ_IMM(op, OP)      case op: { OP(OPERAND(PC++)); break; }
#define STORE(op, mode, val)  case op: { mode WR(a, val); break; }
#define RMW(op, mode, OP)     case op: { mode uint8_t m = RD(a); if (NES_UNLIKELY(IS_IO(a))) ioDummyWrite(nes, a, m); \
                                         OP(m); WR(a, m); break; }

// the eight addressing modes of the ALU group (ORA AND EOR ADC STA LDA CMP SBC)
#define ALU_READ(base, OP)  READ(base + 0x01, AM_IZX, OP)  READ(base + 0x05, AM_ZP, OP) \
                            READ_IMM(base + 0x09, OP)      READ(base + 0x0d, AM_ABS, OP) \
                            READ(base + 0x11, AM_IZYP, OP) READ(base + 0x15, AM_ZPX, OP) \
                            READ(base + 0x19, AM_ABSYP, OP) READ(base + 0x1d, AM_ABSXP, OP)

// the shift/inc group (ASL ROL LSR ROR DEC INC): zp, abs, zp,x, abs,x
#define SHIFT_RMW(base, OP) RMW(base + 0x06, AM_ZP, OP)  RMW(base + 0x0e, AM_ABS, OP) \
                            RMW(base + 0x16, AM_ZPX, OP) RMW(base + 0x1e, AM_ABSX, OP)

// the unofficial combined group (SLO RLA SRE RRA DCP ISB): all seven RMW modes
#define COMBO_RMW(base, OP) RMW(base + 0x03, AM_IZX, OP) RMW(base + 0x07, AM_ZP, OP)   \
                            RMW(base + 0x0f, AM_ABS, OP) RMW(base + 0x13, AM_IZY, OP)  \
                            RMW(base + 0x17, AM_ZPX, OP) RMW(base + 0x1b, AM_ABSY, OP) \
                            RMW(base + 0x1f, AM_ABSX, OP)

// a taken branch dummy reads the next byte, and on a page crossing the address before the
// high byte is fixed (only made on I/O, like the other dummy reads)
#define BRANCH(op, cond)  case op: { int8_t off = OPERAND(PC++); if (cond) { uint16_t t = PC + off; \
                                     DUMMY_READ(PC); \
                                     if ((t ^ PC) & 0xff00) { cycles += 2; DUMMY_READ((PC & 0xff00) | (t & 0xff)); } \
                                     else ++cycles; \
                                     PC = t; } break; }

#define PUSH(v)  (nes.ram[0x100 | S--] = nes.bus = (v))
#define POP()    (nes.bus = nes.ram[0x100 | ++S])


NES_HOT void Cpu::run(int32_t until, bool single)
{
  Nes & nes = *m_nes;

  // An instruction runs only if it ends by 'until': the 6502 accesses memory in its last cycle,
  // so a store that would land after the PPU event the caller is heading for (dot 256, say)
  // must happen after it, not before. Deciding by the start instead made SMB3's status bar jump
  // a line from frame to frame, depending on how the instructions happened to line up.
  for (;;) {

    if (NES_UNLIKELY(nmiPending == NMI_NOW)) {
      if (!single && cycles + 7 > until)
        break;
      nmiPending = 0;
      interrupt(0xfffa, false);
      cycles += 7;
      if (single)
        break;
      continue;
    }
    // right after CLI/SEI/PLP the poll still saw the old I flag (it changes in their last cycle)
    if (NES_UNLIKELY(irqLines) && !(cycles == m_iChanged ? m_iOld : I)) {
      if (!single && cycles + 7 > until)
        break;
      interrupt(0xfffe, false);
      cycles += 7;
      if (single)
        break;
      continue;
    }

    uint8_t op = RD(PC);
    if (!single && cycles + s_cycles[op] > until)
      break;
    ++PC;
    cycles += s_cycles[op];

    switch (op) {

      ALU_READ(0x00, OP_ORA)
      ALU_READ(0x20, OP_AND)
      ALU_READ(0x40, OP_EOR)
      ALU_READ(0x60, OP_ADC)
      ALU_READ(0xa0, OP_LDA)
      ALU_READ(0xc0, OP_CMPA)
      ALU_READ(0xe0, OP_SBC)

      STORE(0x81, AM_IZX, A)  STORE(0x85, AM_ZP, A)   STORE(0x8d, AM_ABS, A)  STORE(0x91, AM_IZY, A)
      STORE(0x95, AM_ZPX, A)  STORE(0x99, AM_ABSY, A) STORE(0x9d, AM_ABSX, A)
      STORE(0x86, AM_ZP, X)   STORE(0x8e, AM_ABS, X)  STORE(0x96, AM_ZPY, X)
      STORE(0x84, AM_ZP, Y)   STORE(0x8c, AM_ABS, Y)  STORE(0x94, AM_ZPX, Y)

      READ_IMM(0xa2, OP_LDX) READ(0xa6, AM_ZP, OP_LDX) READ(0xae, AM_ABS, OP_LDX)
      READ(0xb6, AM_ZPY, OP_LDX) READ(0xbe, AM_ABSYP, OP_LDX)
      READ_IMM(0xa0, OP_LDY) READ(0xa4, AM_ZP, OP_LDY) READ(0xac, AM_ABS, OP_LDY)
      READ(0xb4, AM_ZPX, OP_LDY) READ(0xbc, AM_ABSXP, OP_LDY)

      READ_IMM(0xe0, OP_CMPX) READ(0xe4, AM_ZP, OP_CMPX) READ(0xec, AM_ABS, OP_CMPX)
      READ_IMM(0xc0, OP_CMPY) READ(0xc4, AM_ZP, OP_CMPY) READ(0xcc, AM_ABS, OP_CMPY)
      READ(0x24, AM_ZP, OP_BIT)   READ(0x2c, AM_ABS, OP_BIT)

      SHIFT_RMW(0x00, OP_ASL)
      SHIFT_RMW(0x20, OP_ROL)
      SHIFT_RMW(0x40, OP_LSR)
      SHIFT_RMW(0x60, OP_ROR)
      SHIFT_RMW(0xc0, OP_DEC)
      SHIFT_RMW(0xe0, OP_INC)
      case 0x0a: OP_ASL(A); break;
      case 0x2a: OP_ROL(A); break;
      case 0x4a: OP_LSR(A); break;
      case 0x6a: OP_ROR(A); break;


      BRANCH(0x10, !(n & 0x80))
      BRANCH(0x30, n & 0x80)
      BRANCH(0x50, !V)
      BRANCH(0x70, V)
      BRANCH(0x90, !C)
      BRANCH(0xb0, C)
      BRANCH(0xd0, z)
      BRANCH(0xf0, !z)

      case 0x00:  // BRK
        ++PC;
        interrupt(0xfffe, true);
        break;
      case 0x20: { // JSR
        uint8_t lo = OPERAND(PC++);
        PUSH(PC >> 8);
        PUSH(PC);
        PC = lo | (OPERAND(PC) << 8);
        break;
      }
      case 0x60: { // RTS
        uint8_t lo = POP();
        PC = (lo | (POP() << 8)) + 1;
        break;
      }
      case 0x40: { // RTI
        setP(POP());
        uint8_t lo = POP();
        PC = lo | (POP() << 8);
        break;
      }
      case 0x4c:  // JMP abs
        PC = FETCH16();
        break;
      case 0x6c: { // JMP (ind), the pointer's high byte is fetched without carry into the page
        uint16_t p = FETCH16();
        PC = RD(p) | (RD((p & 0xff00) | ((p + 1) & 0xff)) << 8);
        break;
      }
      case 0x08: PUSH(getP() | 0x10); break;   // PHP
      case 0x28: m_iOld = I; m_iChanged = cycles; setP(POP()); break;   // PLP
      case 0x48: PUSH(A); break;               // PHA
      case 0x68: A = POP(); NZ(A); break;      // PLA

      case 0x18: C = 0; break;
      case 0x38: C = 1; break;
      case 0x58: m_iOld = I; m_iChanged = cycles; I = 0; break;   // CLI
      case 0x78: m_iOld = I; m_iChanged = cycles; I = 1; break;   // SEI
      case 0xb8: V = 0; break;
      case 0xd8: D = 0; break;
      case 0xf8: D = 1; break;

      case 0xaa: X = A; NZ(X); break;
      case 0x8a: A = X; NZ(A); break;
      case 0xa8: Y = A; NZ(Y); break;
      case 0x98: A = Y; NZ(A); break;
      case 0xba: X = S; NZ(X); break;
      case 0x9a: S = X; break;
      case 0xe8: ++X; NZ(X); break;
      case 0xca: --X; NZ(X); break;
      case 0xc8: ++Y; NZ(Y); break;
      case 0x88: --Y; NZ(Y); break;

      // NOPs, official and unofficial; the ones with an operand do read it (which matters on I/O)
      case 0xea: case 0x1a: case 0x3a: case 0x5a: case 0x7a: case 0xda: case 0xfa:
        break;
      case 0x80: case 0x82: case 0x89: case 0xc2: case 0xe2:
        PC += 1;
        break;

      // everything else (unofficial opcodes, rare in games) runs from flash, see runCold()
      default:
        runCold(op, until);
        break;
    }
    if (NES_UNLIKELY(nmiPending)) {
      if (nmiPending == NMI_EDGE)       // this instruction's last cycle was cycles - 1
        nmiPending = cycles - 1 <= m_nmiEdge ? NMI_AFTER_THIS : NMI_NOW;
      else
        nmiPending = nmiPending == NMI_AFTER_NEXT ? NMI_AFTER_THIS : NMI_NOW;
    }
    if (single)
      break;
  }
}

// The unofficial opcodes: games hardly use them, so they live in flash, keeping the hot switch in
// run() small enough for the IRAM (the combined read-modify-write ones alone are 42 cases).
void Cpu::runCold(uint8_t op, int32_t until)
{
  Nes & nes = *m_nes;
  switch (op) {
      READ(0xeb, AM_IMM, OP_SBC)        // unofficial duplicate of E9
      STORE(0x83, AM_IZX, A & X) STORE(0x87, AM_ZP, A & X) STORE(0x8f, AM_ABS, A & X) STORE(0x97, AM_ZPY, A & X)
      READ(0xa3, AM_IZX, OP_LAX) READ(0xa7, AM_ZP, OP_LAX) READ(0xaf, AM_ABS, OP_LAX)
      READ(0xb3, AM_IZYP, OP_LAX) READ(0xb7, AM_ZPY, OP_LAX) READ(0xbf, AM_ABSYP, OP_LAX)
      READ(0xab, AM_IMM, OP_LAX)
      COMBO_RMW(0x00, OP_SLO)
      COMBO_RMW(0x20, OP_RLA)
      COMBO_RMW(0x40, OP_SRE)
      COMBO_RMW(0x60, OP_RRA)
      COMBO_RMW(0xc0, OP_DCP)
      COMBO_RMW(0xe0, OP_ISB)
      READ(0x04, AM_ZP, OP_NOP)  READ(0x44, AM_ZP, OP_NOP)  READ(0x64, AM_ZP, OP_NOP)
      READ(0x14, AM_ZPX, OP_NOP) READ(0x34, AM_ZPX, OP_NOP) READ(0x54, AM_ZPX, OP_NOP)
      READ(0x74, AM_ZPX, OP_NOP) READ(0xd4, AM_ZPX, OP_NOP) READ(0xf4, AM_ZPX, OP_NOP)
      READ(0x0c, AM_ABS, OP_NOP)
      READ(0x1c, AM_ABSXP, OP_NOP) READ(0x3c, AM_ABSXP, OP_NOP) READ(0x5c, AM_ABSXP, OP_NOP)
      READ(0x7c, AM_ABSXP, OP_NOP) READ(0xdc, AM_ABSXP, OP_NOP) READ(0xfc, AM_ABSXP, OP_NOP)
      // unofficial immediates
      case 0x0b: case 0x2b: { AM_IMM OP_AND(RD(a)); C = A >> 7; break; }          // ANC
      case 0x4b: { AM_IMM OP_AND(RD(a)); OP_LSR(A); break; }                      // ALR
      case 0x6b: { AM_IMM A &= RD(a); A = (A >> 1) | (C << 7); NZ(A);             // ARR
                   C = (A >> 6) & 1; V = ((A >> 6) ^ (A >> 5)) & 1; break; }
      case 0xcb: { AM_IMM uint8_t m = RD(a); uint8_t ax = A & X;                  // AXS
                   C = ax >= m; X = ax - m; NZ(X); break; }
      case 0x8b: { AM_IMM A = X & RD(a); NZ(A); break; }                          // XAA (unstable)

      // unofficial stores of "register AND (high address byte + 1)", unstable on hardware
      case 0x9c: { AM_ABSX WR(a, Y & (((a - X) >> 8) + 1)); break; }               // SHY
      case 0x9e: { AM_ABSY WR(a, X & (((a - Y) >> 8) + 1)); break; }               // SHX
      case 0x9f: { AM_ABSY WR(a, A & X & (((a - Y) >> 8) + 1)); break; }           // AHX
      case 0x93: { AM_IZY  WR(a, A & X & (((a - Y) >> 8) + 1)); break; }           // AHX
      case 0x9b: { AM_ABSY S = A & X; WR(a, S & (((a - Y) >> 8) + 1)); break; }    // TAS
      case 0xbb: { AM_ABSYP A = X = S = RD(a) & S; NZ(A); break; }                 // LAS

      // KIL: the CPU locks up until reset
      case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52:
      case 0x62: case 0x72: case 0x92: case 0xb2: case 0xd2: case 0xf2:
        --PC;
        jammed = true;
        cycles = until;
        break;
  }
}
