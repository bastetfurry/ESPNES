// Konami's VRC7 sound (nesdev wiki "VRC7 audio"): a cut-down YM2413 (OPLL) with six FM channels
// and its own 15 instrument patches, written through $9010 (register select) and $9030 (data):
//   $00-$07  the custom instrument (instrument 0)   $10-$15  f-number bits 7-0
//   $20-$25  sustain (5), key on (4), block (3-1), f-number bit 8   $30-$35  instrument, volume
//
// The OPLL is Yamaha's cut-down OPL2, at the same clock (3.58 MHz / 72 = 49716 Hz), and a patch is
// nearly the OPL2's operator registers. So the VRC7 is emulated as a translator in front of an
// OPL2 (DBOPL, src/nes/opl): each write becomes the OPL2 writes for that channel (the patch's
// bytes, KSL's two bits swapped, volume as the carrier's level in 0.75 dB steps, the 9 bit
// f-number doubled to the OPL2's 10, the sustain bit as release rate 5). An OPLL's envelope steps
// and sine resolution differ slightly from the OPL2's; the difference is small.
//
// Lazily like the other chips: the OPL2 makes a sample every 72 CPU cycles (24858 Hz), in blocks;
// advance(n) holds each one for its cycles and adds sample * cycles to acc.
#pragma once

#include "platform.h"

namespace DBOPL { struct Chip; }

class Vrc7Audio {
public:
  ~Vrc7Audio()                           { stop(); }
  // The OPL2 (about 5 KB) exists only while VRC7 music plays: start() makes it (false: no
  // memory), stop() frees it.
  bool    start();
  void    stop();
  void    reset();
  void    select(uint8_t value)          { m_select = value & 0x3f; }    // $9010
  void    write(uint8_t value);                                          // $9030
  void    advance(int32_t n);
  int     channelLevel(int ch) const;    // the carrier's envelope, 0 (silent) - 255, for meters

  int32_t acc = 0;                       // the six channels' sum * cycles

private:
  void    updateInstrument(int ch);
  void    updateFrequency(int ch);
  void    opl(uint32_t reg, uint8_t value);

  static constexpr int CYCLES = 72;      // CPU cycles per sample
  static constexpr int BLOCK = 16;       // samples made at a time

  DBOPL::Chip * m_chip = nullptr;
  uint8_t  m_select;
  uint8_t  m_reg[0x40];
  int32_t  m_block[BLOCK];
  int      m_pos;                        // in m_block; BLOCK: make the next
  int32_t  m_sample;
  int32_t  m_timer;                      // CPU cycles to the next sample
};
