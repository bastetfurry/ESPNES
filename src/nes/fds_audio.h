// The Famicom Disk System's sound: one 64 step wavetable channel with a pitch modulator, and a
// volume and a modulation envelope (nesdev wiki "FDS audio"; the pitch formula as Mesen and
// NSFPlay have it). Registers $4040-$408A, reads $4040-$407F, $4090, $4092.
//
// Like the APU's channels it is synthesized lazily: advance(n) runs n CPU cycles, from event to
// event (a wave step, a modulator step, an envelope tick), and adds level * cycles to acc. The
// level is 0-63 (the 6 bit DAC); the APU mixes it, low-pass filtered.
#pragma once

#include "platform.h"

class FdsAudio {
public:
  void    reset();
  void    write(uint16_t addr, uint8_t value);
  int     read(uint16_t addr) const;        // -1: not one of its registers
  void    advance(int32_t n);

  int32_t acc = 0;                          // level * CPU cycles since the APU last took it

private:
  struct Envelope {
    bool     off, up;
    uint8_t  speed, gain;
    int32_t  timer;                         // CPU cycles to the next tick
  };

  void    resetTimer(Envelope & e) const    { e.timer = 8 * (e.speed + 1) * m_masterSpeed; }
  bool    tick(Envelope & e) const;         // a tick that changes the gain: true
  void    stepModulator();
  void    updateModulation();
  void    updateLevel();
  bool    envelopesRun() const              { return !m_waveHalt && !m_envHalt && m_masterSpeed; }

  uint8_t  m_wave[64];
  uint8_t  m_modTable[64];

  // wave unit
  uint16_t m_pitch;                         // $4082/$4083, 12 bit
  uint32_t m_waveAcc;                       // 16 bit: a step at each overflow
  uint8_t  m_wavePos;
  bool     m_waveHalt, m_envHalt;           // $4083 bits 7 and 6
  bool     m_waveWrite;                     // $4089 bit 7
  uint8_t  m_masterVolume;                  // $4089 bits 0-1
  uint8_t  m_masterSpeed;                   // $408A

  Envelope m_vol, m_mod;

  // modulator
  uint16_t m_modPitch;                      // $4086/$4087, 12 bit
  uint32_t m_modAcc;
  uint8_t  m_modPos;
  int8_t   m_modCounter;                    // 7 bit signed
  bool     m_modHalt;                       // $4087 bit 7
  int32_t  m_modOffset;                     // what the modulator adds to the wave pitch

  int32_t  m_level;
};
