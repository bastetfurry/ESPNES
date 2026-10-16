// Sunsoft 5B sound (nesdev wiki "Sunsoft 5B audio"): a YM2149F, the AY-3-8910's sibling, clocked
// so that it runs at half an AY's speed. Three square tone channels, a noise generator and an
// envelope, each channel at a 4 bit volume or the envelope's 5 bit level, through a logarithmic
// DAC of 1.5 dB per 5 bit step. Written through $C000 (register select) and $E000 (data):
//   $00-$05  tone periods A, B, C (12 bit)   $06  noise period (5 bit)
//   $07      mixer: bits 0-2 tone off, 3-5 noise off (A, B, C)
//   $08-$0A  volume A, B, C (bit 4: the envelope's)   $0B/$0C  envelope period   $0D  its shape
//
// Synthesized lazily like the other channels: advance(n) runs n CPU cycles from event to event
// (a tone half period, a noise bit, an envelope step) and adds each channel's amplitude * cycles
// to its acc (0-255 per channel).
#pragma once

#include "platform.h"

class S5bAudio {
public:
  void    reset();
  void    select(uint8_t value);         // $C000
  void    write(uint8_t value);          // $E000
  void    advance(int32_t n);

  int32_t acc[3] = { };

private:
  void    updateLevels();
  void    envelopeStep();

  uint8_t  m_reg[16];
  uint8_t  m_select;
  bool     m_writable;

  // the generators: cycles to their next step
  int32_t  m_toneTimer[3];
  bool     m_tone[3];                    // the square's state
  int32_t  m_noiseTimer;
  uint32_t m_lfsr;
  int32_t  m_envTimer;
  int8_t   m_envStep;                    // 31 down to 0 in a ramp
  uint8_t  m_envAttack;                  // 0 or 31: the level is step ^ attack
  bool     m_envHolding, m_envHold, m_envAlternate;
  uint8_t  m_envLevel;

  int32_t  m_level[3];                   // the channels' amplitudes now
};
