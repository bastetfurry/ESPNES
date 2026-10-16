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
#include "fds_audio.h"
#include "vrc6_audio.h"

class Nes;
class StateIO;

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

  // DMC DMA: the frame relative CPU cycle of the next sample fetch (INT32_MAX: none ahead), and
  // the CPU cycles the fetches made so far have stolen
  int32_t nextDmaCycle() const;
  int     takeDmaStall()           { int n = m_dmaStall; m_dmaStall = 0; return n; }

  // samples produced since the last takeSamples(), signed 16 bit
  int             sampleCount() const      { return m_sampleCount; }
  int16_t const * samples() const          { return m_samples; }
  void            takeSamples()            { m_sampleCount = 0; }

  static constexpr int MAX_SAMPLES = 1024;   // per frame; 48 kHz at 50 fps is 960

  // savestates (see state.h); the samples not yet taken are dropped, the sample rate is kept
  void state(StateIO & io);

  // the FDS's sound, mixed in while fdsOn (FDS music; Nes routes $4040-$4097 to it)
  FdsAudio fds;
  bool     fdsOn = false;

  // the VRC6's sound, mixed in while vrc6On (VRC6 music; Nes routes $9000-$B002 to it)
  Vrc6Audio vrc6;
  bool      vrc6On = false;

  // The MMC5's sound, mixed in while mmc5On (Nes routes $5000-$5015 to it): two pulse channels
  // as the 2A03's but without a sweep, whose envelopes and length counters the MMC5's own 240 Hz
  // counter clocks, and an 8 bit PCM level ($5011, written). $5015 enables the pulses.
  bool     mmc5On = false;
  void     writeMmc5(uint16_t addr, uint8_t value);
  uint8_t  readMmc5Status() const;

  // Level meters (while metersOn): how far each channel's output swung since the last call, per
  // sample: pulse 1, pulse 2, triangle, noise (0-15), DMC (0-127), FDS (0-63), VRC6 pulse 1 and 2
  // (0-15), VRC6 saw (0-31), MMC5 pulse 1 and 2 (0-15), MMC5 PCM (0-255)
  enum { METER_PULSE1, METER_PULSE2, METER_TRIANGLE, METER_NOISE, METER_DMC, METER_FDS,
         METER_VRC6_1, METER_VRC6_2, METER_SAW, METER_MMC5_1, METER_MMC5_2, METER_PCM, METERS };
  bool     metersOn = false;
  void     takeMeters(uint8_t out[METERS]);

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
  int      m_dmaStall = 0;
  bool     m_loadPending = false;   // a $4015 write started a sample: its first fetch is due
  int32_t  m_loadAt = 0;

  Pulse    m_pulse[2];
  Pulse    m_mmc5Pulse[2];
  uint8_t  m_mmc5Pcm;
  int32_t  m_mmc5PcmAcc;
  int32_t  m_mmc5Timer;          // CPU cycles to the MMC5's next 240 Hz tick
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
  int32_t  m_fdsLowpass;         // the FDS's output filter, about 2 kHz
  int32_t  m_fdsLowpassK;        // its coefficient at the sample rate, of 1024
  uint8_t  m_meterMin[METERS], m_meterMax[METERS];
  void     meter(int i, int32_t level)
  {
    if (level < m_meterMin[i]) m_meterMin[i] = level;
    if (level > m_meterMax[i]) m_meterMax[i] = level;
  }

  int16_t  m_samples[MAX_SAMPLES];
  int      m_sampleCount;
};
