// 2A03 APU: two pulse channels, triangle, noise, DMC and the frame counter.
//
// Synthesis is lazy: run(cycle) catches the APU up to a CPU cycle, and Nes calls it before every
// APU register access and once per scanline. Each output sample is the average of every
// channel's level over the CPU cycles it covers (a box filter), which keeps high notes from
// aliasing badly at a low output rate. The channels are mixed with the linear approximation of
// the NES mixer and high-pass filtered like the console's output stage.
#pragma once

#include "platform.h"
#include "cart.h"

class Nes;

class Apu {
public:

  Apu() : m_nes(nullptr), m_region(Region::NTSC), m_sampleRate(22050), m_sampleCount(0) { }

  void init(Nes * nes) { m_nes = nes; }
  void reset(Region region);

  // Output sample rate. adjust slightly speeds up (> 1) or slows down sample production, to keep
  // a playback buffer from running dry or overflowing.
  void setSampleRate(int rate, float adjust = 1.0f);

  void    run(int32_t cycle);

  // Frozen, run() lets time pass without the APU advancing (overclocking's extra CPU cycles)
  void    setFrozen(bool value)    { m_frozen = value; }
  void    write(uint16_t addr, uint8_t value);
  uint8_t readStatus();

  // end of frame: the frame relative cycle counter goes back by 'cycles'
  void    rebase(int32_t cycles);

  // samples produced since the last takeSamples(), signed 16 bit
  int             sampleCount() const      { return m_sampleCount; }
  int16_t const * samples() const          { return m_samples; }
  void            takeSamples()            { m_sampleCount = 0; }

  static constexpr int MAX_SAMPLES = 1024;   // per frame; 48 kHz at 50 fps is 960

private:

  struct Envelope {
    bool    start, loop, constant;
    uint8_t volume, divider, decay;
    void    clock();
    uint8_t output() const     { return constant ? volume : decay; }
  };

  struct Pulse {
    Envelope env;
    uint8_t  duty, seq;
    uint16_t period;           // 11 bit timer reload
    int32_t  timer;            // CPU cycles to the next sequencer step
    uint8_t  length;
    bool     enabled;
    bool     sweepEnabled, sweepNegate, sweepReload;
    uint8_t  sweepPeriod, sweepShift, sweepDivider;
    bool     onesComplement;   // pulse 1 negates with one's complement
    int32_t  acc;              // level * cycles since the last sample

    uint16_t sweepTarget() const;
    bool     muted() const     { return period < 8 || sweepTarget() > 0x7ff; }
    int      level() const;
    void     clockSweep();
    void     advance(int32_t n);
  };

  struct Triangle {
    uint16_t period;
    int32_t  timer;
    uint8_t  seq;
    uint8_t  length;
    bool     enabled, control, linearReload;
    uint8_t  linearReloadValue, linear;
    int32_t  acc;

    void     advance(int32_t n);
  };

  struct Noise {
    Envelope env;
    uint16_t period;           // CPU cycles
    int32_t  timer;
    uint16_t lfsr;
    bool     mode;
    uint8_t  length;
    bool     enabled;
    int32_t  acc;

    int      level() const     { return (length && !(lfsr & 1)) ? env.output() : 0; }
    void     advance(int32_t n);
  };

  struct Dmc {
    bool     irqEnabled, loop, irqFlag;
    uint16_t period;
    int32_t  timer;
    uint8_t  level;
    uint16_t startAddr, addr;
    uint16_t startLength, remaining;
    uint8_t  shift, bitsLeft;
    bool     silent;
    uint8_t  buffer;
    bool     bufferFull;
    int32_t  acc;
  };

  void advance(int32_t n);       // all channels, no frame counter events inside
  void advanceDmc(int32_t n);
  void dmcFetch();
  void quarterFrame();
  void halfFrame();
  void frameCounterStep();
  void emitSample();

  Nes *    m_nes;
  Region   m_region;
  bool     m_frozen = false;

  Pulse    m_pulse[2];
  Triangle m_tri;
  Noise    m_noise;
  Dmc      m_dmc;

  int32_t  m_cycle;              // frame relative CPU cycle the APU has reached
  uint8_t  m_parity;             // makes (m_cycle ^ m_parity) & 1 the absolute cycle parity
  int32_t  m_accCycles;          // cycles accumulated into the current sample

  // frame counter
  bool     m_fiveStep, m_irqInhibit, m_frameIrq;
  int32_t  m_seqCycle;           // cycles since the sequence started
  int      m_seqStep;
  int32_t const * m_seqPositions;

  // sample clock, 16.16 fixed point CPU cycles
  int      m_sampleRate;
  int64_t  m_cyclesPerSample;
  int64_t  m_nextSample;

  // output stage
  int32_t  m_hpPrevIn, m_hpPrevOut;

  int16_t  m_samples[MAX_SAMPLES];
  int      m_sampleCount;
};
