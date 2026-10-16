// Konami's VRC6 sound (nesdev wiki "VRC6 audio"): two pulse channels with 8 duty steps of 16
// and a 4 bit volume, and a sawtooth from a 6 bit accumulator rate. Registers (VRC6a, as NSF
// music has them):
//   $9000/$A000  mode (bit 7: always high), duty (6-4), volume (3-0)   $B000  accumulator rate
//   $x001        period bits 7-0                                       $x002  enable (7), bits 11-8
//   $9003        bit 0: halt all, bit 1: periods / 16, bit 2: periods / 256
//
// Synthesized lazily like the APU's channels: advance(n) runs n CPU cycles from timer event to
// timer event and adds each channel's level * cycles to its acc (pulses 0-15, saw 0-31).
#pragma once

#include "platform.h"

class Vrc6Audio {
public:
  void    reset();
  void    write(uint16_t addr, uint8_t value);   // addr already reduced to $9000-$B002 form
  void    advance(int32_t n);

  int32_t acc[3] = { };                          // pulse 1, pulse 2, saw: level * cycles

private:
  struct Pulse {
    uint8_t  duty, volume, step;
    bool     mode, enabled;
    uint16_t period;
    int32_t  timer;
    int      level() const { return enabled && (mode || step <= duty) ? volume : 0; }
  };
  struct Saw {
    uint8_t  rate, step, accumulator;
    bool     enabled;
    uint16_t period;
    int32_t  timer;
    int      level() const { return enabled ? accumulator >> 3 : 0; }
  };

  int32_t  reload(uint16_t period) const { return (period >> m_shift) + 1; }

  Pulse    m_pulse[2];
  Saw      m_saw;
  bool     m_halt;
  uint8_t  m_shift;
};
