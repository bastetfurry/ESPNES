// Namco 163 sound (nesdev wiki "Namco 163 audio"): up to 8 wavetable channels in 128 bytes of
// sound RAM, which also hold the waves (4 bit samples, the low nibble first). $F800 sets the RAM
// address (bit 7: increment after each access), $4800 reads and writes it. Channel n's registers
// are 8 bytes at $40 + 8 * n (channel 7 at $78): frequency (18 bit) at +0/+2/+4, phase (24 bit)
// at +1/+3/+5, the length (256 - 4 * (+4 >> 2) samples), the wave address +6, volume +7 (bits
// 0-3); $7F bits 4-6 are the number of channels - 1, counted down from channel 7.
//
// The chip updates one channel every 15 CPU cycles, in turn, and outputs them one after the other;
// it is mixed as the wiki suggests: the channels' held outputs summed and divided by their number.
// Synthesized lazily: advance(n) runs the updates of n CPU cycles and adds the mix * cycles to
// acc, and each channel's level (sample * volume, 0-225) * cycles to level[] for the meters.
#pragma once

#include "platform.h"

class N163Audio {
public:
  void    reset();
  void    setAddress(uint8_t value);     // $F800
  void    write(uint8_t value);          // $4800
  uint8_t read();                        // $4800
  void    advance(int32_t n);
  int     channels() const               { return ((m_ram[0x7f] >> 4) & 7) + 1; }

  int32_t acc = 0;                       // mix: sum of (sample - 8) * volume over channels / count
  int32_t level[8] = { };                // per channel (7 is the first): sample * volume

private:
  void    update(int channel);

  uint8_t  m_ram[128];
  uint8_t  m_address;
  bool     m_increment;
  int32_t  m_timer;                      // cycles to the next channel update
  int      m_next;                       // the channel updated next
  int32_t  m_out[8];                     // each channel's held output, (sample - 8) * volume
  int32_t  m_shown[8];                   // and as sample * volume, for the meters
};
