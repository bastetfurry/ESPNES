#include <string.h>

#include "apu.h"
#include "nes.h"
#include "state.h"


static const uint8_t s_lengthTable[32] = {
  10, 254, 20,  2, 40,  4, 80,  6, 160,  8, 60, 10, 14, 12, 26, 14,
  12,  16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};

static const uint8_t NES_FASTDATA s_dutyTable[4][8] = {
  { 0, 1, 0, 0, 0, 0, 0, 0 },   // 12.5 %
  { 0, 1, 1, 0, 0, 0, 0, 0 },   // 25 %
  { 0, 1, 1, 1, 1, 0, 0, 0 },   // 50 %
  { 1, 0, 0, 1, 1, 1, 1, 1 },   // 25 % negated
};

static const uint8_t NES_FASTDATA s_triangleTable[32] = {
  15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  0,
   0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
};

// in CPU cycles
static const uint16_t s_noisePeriodNTSC[16] = { 4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 };
static const uint16_t s_noisePeriodPAL[16]  = { 4, 8, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708, 944, 1890, 3778 };
static const uint16_t s_dmcPeriodNTSC[16]   = { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 };
static const uint16_t s_dmcPeriodPAL[16]    = { 398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50 };

// frame counter: CPU cycles of each step since the sequence start, then the sequence length;
// the 4-step sequence sets the IRQ flag a cycle before its last step too (and a cycle after)
static const int32_t s_seq4NTSC[6] = { 7457, 14913, 22371, 29828, 29829, 29830 };
static const int32_t s_seq5NTSC[6] = { 7457, 14913, 22371, 29829, 37281, 37282 };
static const int32_t s_seq4PAL[6]  = { 8313, 16627, 24939, 33252, 33253, 33254 };
static const int32_t s_seq5PAL[6]  = { 8313, 16627, 24939, 33253, 41565, 41566 };


///////////////////////////////////////////////////////////////////////////////
// channels

void Apu::Envelope::clock()
{
  if (start) {
    start = false;
    decay = 15;
    divider = volume;
  } else if (divider == 0) {
    divider = volume;
    if (decay)
      --decay;
    else if (loop)
      decay = 15;
  } else {
    --divider;
  }
}


uint16_t Apu::Pulse::sweepTarget() const
{
  int change = period >> sweepShift;
  if (sweepNegate) {
    int t = period - change - (onesComplement ? 1 : 0);
    return t < 0 ? 0 : t;
  }
  return period + change;
}


void Apu::Pulse::clockSweep()
{
  if (sweepDivider == 0 && sweepEnabled && sweepShift && !muted())
    period = sweepTarget();
  if (sweepDivider == 0 || sweepReload) {
    sweepDivider = sweepPeriod;
    sweepReload = false;
  } else {
    --sweepDivider;
  }
}


NES_HOT void Apu::Pulse::advance(int32_t n)
{
  // silent: nothing to accumulate, and the sequencer phase is inaudible
  if (!length || muted())
    return;
  uint8_t const * d = s_dutyTable[duty];
  int vol = env.output();
  int lv = d[seq] ? vol : 0;
  while (n >= timer) {
    acc += lv * timer;
    n -= timer;
    timer = (period + 1) * 2;
    seq = (seq + 1) & 7;
    lv = d[seq] ? vol : 0;
  }
  acc += lv * n;
  timer -= n;
}


NES_HOT void Apu::Triangle::advance(int32_t n)
{
  int lv = s_triangleTable[seq];
  // Halted, or ultrasonic (games use periods 0 and 1 to silence it): hold the level, which
  // avoids the pop a real console makes and the cost of stepping every cycle or two.
  if (!length || !linear || period < 2) {
    acc += lv * n;
    return;
  }
  while (n >= timer) {
    acc += lv * timer;
    n -= timer;
    timer = period + 1;
    seq = (seq + 1) & 31;
    lv = s_triangleTable[seq];
  }
  acc += lv * n;
  timer -= n;
}


NES_HOT void Apu::Noise::advance(int32_t n)
{
  if (!length)
    return;
  int vol = env.output();
  int tap = mode ? 6 : 1;
  while (n >= timer) {
    if (!(lfsr & 1))
      acc += vol * timer;
    n -= timer;
    timer = period;
    uint16_t feedback = (lfsr ^ (lfsr >> tap)) & 1;
    lfsr = (lfsr >> 1) | (feedback << 14);
  }
  if (!(lfsr & 1))
    acc += vol * n;
  timer -= n;
}


void Apu::dmcFetch(int32_t at)
{
  Dmc & d = m_dmc;
  if (d.bufferFull || !d.remaining)
    return;
  // the DMA halts the CPU for 4 cycles, 3 if it starts on a "get" cycle; Nes adds them
  m_dmaStall += m_nes->isGetCycle(at) ? 3 : 4;
  d.buffer = m_nes->bus = m_nes->read(d.addr);     // the fetched byte is left on the data bus
  d.bufferFull = true;
  d.addr = d.addr == 0xffff ? 0x8000 : d.addr + 1;
  if (--d.remaining == 0) {
    if (d.loop) {
      d.addr = d.startAddr;
      d.remaining = d.startLength;
    } else if (d.irqEnabled) {
      d.irqFlag = true;
      m_nes->cpu.setIrq(Cpu::IRQ_DMC);
    }
  }
}


NES_HOT void Apu::advanceDmc(int32_t n)
{
  Dmc & d = m_dmc;
  int32_t at = m_cycle;                        // the cycle d.timer counts from
  while (n >= d.timer) {
    d.acc += d.level * d.timer;
    n -= d.timer;
    at += d.timer;
    d.timer = d.period;
    if (!d.silent) {
      if (d.shift & 1) {
        if (d.level <= 125)
          d.level += 2;
      } else if (d.level >= 2) {
        d.level -= 2;
      }
    }
    d.shift >>= 1;
    if (--d.bitsLeft == 0) {
      d.bitsLeft = 8;
      if (d.bufferFull) {
        d.silent = false;
        d.shift = d.buffer;
        d.bufferFull = false;
        dmcFetch(at);
      } else {
        d.silent = true;
      }
    }
  }
  d.acc += d.level * n;
  d.timer -= n;
}


///////////////////////////////////////////////////////////////////////////////

void Apu::reset(Region region)
{
  m_region = region;
  memset(m_pulse, 0, sizeof(m_pulse));
  memset(&m_tri, 0, sizeof(m_tri));
  memset(&m_noise, 0, sizeof(m_noise));
  memset(&m_dmc, 0, sizeof(m_dmc));

  for (int i = 0; i < 2; ++i)
    m_pulse[i].timer = 2;
  m_pulse[0].onesComplement = true;
  m_tri.timer   = 1;
  m_noise.lfsr  = 1;
  m_noise.period = m_noise.timer = region == Region::PAL ? s_noisePeriodPAL[0] : s_noisePeriodNTSC[0];
  m_dmc.period  = m_dmc.timer = region == Region::PAL ? s_dmcPeriodPAL[0] : s_dmcPeriodNTSC[0];
  // the timer runs on APU cycles and runs out on "put" cycles (odd, from power on; the periods
  // are even), so the DMA of a buffer reload halts the CPU on a put cycle and takes 4 cycles
  ++m_dmc.timer;
  m_dmc.bitsLeft = 8;
  m_dmc.silent  = true;
  m_dmc.startAddr = 0xc000;
  m_dmc.startLength = 1;

  m_cycle = 0;
  m_parity = 0;
  m_dmaStall = 0;
  m_loadPending = false;
  m_accCycles = 0;
  m_fiveStep = m_irqInhibit = m_frameIrq = false;
  m_frameIrqUntil = INT32_MIN;
  m_seqCycle = 0;
  m_seqStep = 0;
  m_seqPositions = region == Region::PAL ? s_seq4PAL : s_seq4NTSC;

  m_hpPrevIn = m_hpPrevOut = 0;
  m_fdsLowpass = 0;
  fds.reset();
  vrc6.reset();
  s5b.reset();
  n163.reset();
  vrc7.reset();
  memset(m_mmc5Pulse, 0, sizeof(m_mmc5Pulse));
  for (auto & p : m_mmc5Pulse) {
    p.timer = 2;
    p.sweepNegate = true;        // no sweep: a target of period - period never mutes
  }
  m_mmc5Pcm = 0;
  m_mmc5PcmAcc = 0;
  m_mmc5Timer = 7457;
  for (int i = 0; i < METERS; ++i) {
    m_meterMin[i] = 255;
    m_meterMax[i] = 0;
  }
  m_sampleCount = 0;
  setSampleRate(m_sampleRate);
  m_nextSample = m_cyclesPerSample;
}


void Apu::takeMeters(uint8_t out[METERS])
{
  for (int i = 0; i < METERS; ++i) {
    out[i] = m_meterMax[i] > m_meterMin[i] ? m_meterMax[i] - m_meterMin[i] : 0;
    m_meterMin[i] = 255;
    m_meterMax[i] = 0;
  }
}


void Apu::setSampleRate(int rate, float adjust)
{
  m_sampleRate = rate;
  double cpuHz = m_region == Region::PAL ? 1662607.0 : 1789773.0;
  m_cyclesPerSample = (int64_t)(cpuHz * 65536.0 / (rate * adjust));
  // one pole low-pass at 2 kHz: 1 - e^(-2 pi 2000 / rate)
  double x = 2 * 3.14159265 * 2000.0 / rate;
  m_fdsLowpassK = (int32_t)(1024 * (1 - 1 / (1 + x + x * x / 2 + x * x * x / 6)));
}


void Apu::quarterFrame()
{
  m_pulse[0].env.clock();
  m_pulse[1].env.clock();
  m_noise.env.clock();
  if (m_tri.linearReload)
    m_tri.linear = m_tri.linearReloadValue;
  else if (m_tri.linear)
    --m_tri.linear;
  if (!m_tri.control)
    m_tri.linearReload = false;
}


void Apu::halfFrame()
{
  for (int i = 0; i < 2; ++i) {
    Pulse & p = m_pulse[i];
    if (p.length && !p.env.loop)
      --p.length;
    p.clockSweep();
  }
  if (m_tri.length && !m_tri.control)
    --m_tri.length;
  if (m_noise.length && !m_noise.env.loop)
    --m_noise.length;
}


void Apu::frameCounterStep()
{
  if (!m_fiveStep) {
    if (m_seqStep != 3)
      quarterFrame();
    if (m_seqStep == 1 || m_seqStep == 4)
      halfFrame();
    // the flag is set in the last three cycles of the sequence: a read in the first two
    // clears it for a cycle only
    if (m_seqStep == 3 && !m_irqInhibit) {
      m_frameIrq = true;
      m_frameIrqUntil = m_cycle + 2;
      m_nes->cpu.setIrq(Cpu::IRQ_FRAME);
    }
    if (++m_seqStep == 5) {
      m_seqStep = 0;
      m_seqCycle -= m_seqPositions[5];
    }
  } else {
    if (m_seqStep != 3)
      quarterFrame();
    if (m_seqStep == 1 || m_seqStep == 4)
      halfFrame();
    if (++m_seqStep == 5) {
      m_seqStep = 0;
      m_seqCycle -= m_seqPositions[5];
    }
  }
}


NES_HOT void Apu::advance(int32_t n)
{
  m_pulse[0].advance(n);
  m_pulse[1].advance(n);
  m_tri.advance(n);
  m_noise.advance(n);
  advanceDmc(n);
  if (NES_UNLIKELY(fdsOn))
    fds.advance(n);
  if (NES_UNLIKELY(vrc6On))
    vrc6.advance(n);
  if (NES_UNLIKELY(s5bOn))
    s5b.advance(n);
  if (NES_UNLIKELY(n163On))
    n163.advance(n);
  if (NES_UNLIKELY(vrc7On))
    vrc7.advance(n);
  if (NES_UNLIKELY(mmc5On)) {
    m_mmc5Pulse[0].advance(n);
    m_mmc5Pulse[1].advance(n);
    m_mmc5PcmAcc += m_mmc5Pcm * n;
    // its own frame counter: envelopes and length counters at 240 Hz
    for (m_mmc5Timer -= n; m_mmc5Timer <= 0; m_mmc5Timer += 7457)
      for (auto & p : m_mmc5Pulse) {
        p.env.clock();
        if (p.length && !p.env.loop)
          --p.length;
      }
  }
  m_cycle += n;
  m_seqCycle += n;
  m_accCycles += n;
}


NES_HOT void Apu::emitSample()
{
  int32_t cycles = m_accCycles ? m_accCycles : 1;
  // linear approximation of the NES mixer, in units of 1/100000 of full scale
  int32_t mix = ((m_pulse[0].acc + m_pulse[1].acc) * 752 + m_tri.acc * 851 + m_noise.acc * 494 + m_dmc.acc * 335) / cycles;
  if (NES_UNLIKELY(metersOn)) {
    meter(METER_PULSE1, m_pulse[0].acc / cycles);
    meter(METER_PULSE2, m_pulse[1].acc / cycles);
    meter(METER_TRIANGLE, m_tri.acc / cycles);
    meter(METER_NOISE, m_noise.acc / cycles);
    meter(METER_DMC, m_dmc.acc / cycles);
    if (fdsOn)
      meter(METER_FDS, fds.acc / cycles);
    if (vrc6On) {
      meter(METER_VRC6_1, vrc6.acc[0] / cycles);
      meter(METER_VRC6_2, vrc6.acc[1] / cycles);
      meter(METER_SAW, vrc6.acc[2] / cycles);
    }
    if (s5bOn)
      for (int i = 0; i < 3; ++i)
        meter(METER_5B_A + i, s5b.acc[i] / cycles);
    if (n163On)
      for (int i = 0; i < 8; ++i)
        meter(METER_N163 + i, n163.level[7 - i] / cycles);
    if (vrc7On)
      for (int i = 0; i < 6; ++i) {
        // no wave to measure: the envelope's level, as a swing from 0
        meter(METER_VRC7 + i, 0);
        meter(METER_VRC7 + i, vrc7.channelLevel(i));
      }
    if (mmc5On) {
      meter(METER_MMC5_1, m_mmc5Pulse[0].acc / cycles);
      meter(METER_MMC5_2, m_mmc5Pulse[1].acc / cycles);
      meter(METER_PCM, m_mmc5PcmAcc / cycles);
    }
  }
  m_pulse[0].acc = m_pulse[1].acc = m_tri.acc = m_noise.acc = m_dmc.acc = 0;
  if (NES_UNLIKELY(fdsOn)) {
    // at its loudest (63) about 2.4 times a pulse channel's (15 * 752)
    int32_t level = fds.acc * 430 / cycles;
    fds.acc = 0;
    m_fdsLowpass += (level - m_fdsLowpass) * m_fdsLowpassK / 1024;
    mix += m_fdsLowpass;
  }
  if (NES_UNLIKELY(vrc6On)) {
    // its pulses about as loud as the 2A03's, the saw (0-31) on the same scale
    mix += (vrc6.acc[0] + vrc6.acc[1] + vrc6.acc[2]) * 752 / cycles;
    vrc6.acc[0] = vrc6.acc[1] = vrc6.acc[2] = 0;
  }
  if (NES_UNLIKELY(vrc7On)) {
    // DBOPL's channel at full level reaches about 4000: about a 2A03 pulse at 15
    mix += vrc7.acc * 3 / cycles;
    vrc7.acc = 0;
  }
  if (NES_UNLIKELY(n163On)) {
    // one channel at its loudest swings 225 (-120 to 105): about 12 dB above a 2A03 pulse
    mix += n163.acc * 200 / cycles;
    n163.acc = 0;
    for (auto & l : n163.level)
      l = 0;
  }
  if (NES_UNLIKELY(s5bOn)) {
    // a channel at its loudest (255) about twice a 2A03 pulse at 15: the chip is loud, and its
    // logarithmic volume puts the usual settings far below that
    mix += (s5b.acc[0] + s5b.acc[1] + s5b.acc[2]) * 88 / cycles;
    s5b.acc[0] = s5b.acc[1] = s5b.acc[2] = 0;
  }
  if (NES_UNLIKELY(mmc5On)) {
    // the pulses as loud as the 2A03's, the PCM (0-255) about like the DMC (0-127)
    mix += ((m_mmc5Pulse[0].acc + m_mmc5Pulse[1].acc) * 752 + m_mmc5PcmAcc * 167) / cycles;
    m_mmc5Pulse[0].acc = m_mmc5Pulse[1].acc = 0;
    m_mmc5PcmAcc = 0;
  }
  m_accCycles = 0;

  // first order high-pass, about 30 Hz at 22 kHz: removes the DC offset like the console's output
  // (a division, not a shift: a shift rounds negative values down and they never decay to 0)
  int32_t out = mix - m_hpPrevIn + m_hpPrevOut * 1014 / 1024;
  m_hpPrevIn = mix;
  m_hpPrevOut = out;

  // full scale (all channels at maximum) is about 85000
  out = out * 3 / 8;
  if (out > 32767)
    out = 32767;
  else if (out < -32768)
    out = -32768;
  if (m_sampleCount < MAX_SAMPLES)
    m_samples[m_sampleCount++] = out;
}


NES_HOT void Apu::run(int32_t until)
{
  if (NES_UNLIKELY(m_loadPending) && !m_frozen && until >= m_loadAt) {
    m_loadPending = false;
    run(m_loadAt);
    dmcFetch(m_loadAt);
  }
  if (NES_UNLIKELY(m_frozen)) {
    // skip the time: the sample clock and the frame counter (relative to m_cycle) move with it
    if (until > m_cycle) {
      m_nextSample += (int64_t)(until - m_cycle) << 16;
      m_cycle = until;
    }
    return;
  }
  while (m_cycle < until) {
    int32_t sampleAt = (int32_t)(m_nextSample >> 16);
    int32_t seqAt    = m_cycle + (m_seqPositions[m_seqStep] - m_seqCycle);
    int32_t target   = until;
    if (sampleAt < target)
      target = sampleAt;
    if (seqAt < target)
      target = seqAt;
    if (target > m_cycle)
      advance(target - m_cycle);
    if (m_cycle >= seqAt)
      frameCounterStep();
    if (m_cycle >= sampleAt) {
      emitSample();
      m_nextSample += m_cyclesPerSample;
    }
  }
}


int32_t Apu::nextDmaCycle() const
{
  Dmc const & d = m_dmc;
  if (m_frozen)
    return INT32_MAX;
  if (m_loadPending)
    return m_loadAt;
  if (!d.remaining || !d.bufferFull)
    return INT32_MAX;
  // the buffer empties into the shift register when the current byte's bits run out
  return m_cycle + d.timer + (d.bitsLeft - 1) * d.period;
}


void Apu::rebase(int32_t cycles)
{
  if (m_frameIrqUntil != INT32_MIN)
    m_frameIrqUntil -= cycles;
  if (m_loadPending)
    m_loadAt -= cycles;
  m_parity ^= cycles & 1;       // keeps (m_cycle ^ m_parity) & 1 the absolute cycle parity
  m_cycle -= cycles;
  m_nextSample -= (int64_t) cycles << 16;
}


uint8_t Apu::readStatus()
{
  uint8_t r = (m_pulse[0].length ? 0x01 : 0) | (m_pulse[1].length ? 0x02 : 0) |
              (m_tri.length ? 0x04 : 0) | (m_noise.length ? 0x08 : 0) |
              (m_dmc.remaining ? 0x10 : 0) | (m_frameIrq ? 0x40 : 0) | (m_dmc.irqFlag ? 0x80 : 0);
  if (m_cycle >= m_frameIrqUntil) {
    m_frameIrq = false;
    m_nes->cpu.clearIrq(Cpu::IRQ_FRAME);
  }
  return r;
}


void Apu::writeMmc5(uint16_t addr, uint8_t v)
{
  if (addr <= 0x5007) {
    Pulse & p = m_mmc5Pulse[(addr >> 2) & 1];
    switch (addr & 3) {
      case 0:
        p.duty = v >> 6;
        p.env.loop = v & 0x20;
        p.env.constant = v & 0x10;
        p.env.volume = v & 0x0f;
        break;
      case 2:
        p.period = (p.period & 0x700) | v;
        break;
      case 3:
        p.period = (p.period & 0xff) | ((v & 7) << 8);
        if (p.enabled)
          p.length = s_lengthTable[v >> 3];
        p.seq = 0;
        p.env.start = true;
        break;
    }                                // (1: there is no sweep)
  } else if (addr == 0x5011) {
    if (v)                           // write mode: 0 is ignored
      m_mmc5Pcm = v;
  } else if (addr == 0x5015) {
    for (int i = 0; i < 2; ++i) {
      m_mmc5Pulse[i].enabled = v & (1 << i);
      if (!m_mmc5Pulse[i].enabled)
        m_mmc5Pulse[i].length = 0;
    }
  }
}


uint8_t Apu::readMmc5Status() const
{
  return (m_mmc5Pulse[0].length ? 1 : 0) | (m_mmc5Pulse[1].length ? 2 : 0);
}


void Apu::write(uint16_t addr, uint8_t v)
{
  switch (addr) {
    case 0x4000: case 0x4004: {
      Pulse & p = m_pulse[(addr >> 2) & 1];
      p.duty = v >> 6;
      p.env.loop = v & 0x20;
      p.env.constant = v & 0x10;
      p.env.volume = v & 0x0f;
      break;
    }
    case 0x4001: case 0x4005: {
      Pulse & p = m_pulse[(addr >> 2) & 1];
      p.sweepEnabled = v & 0x80;
      p.sweepPeriod = (v >> 4) & 7;
      p.sweepNegate = v & 0x08;
      p.sweepShift = v & 7;
      p.sweepReload = true;
      break;
    }
    case 0x4002: case 0x4006: {
      Pulse & p = m_pulse[(addr >> 2) & 1];
      p.period = (p.period & 0x700) | v;
      break;
    }
    case 0x4003: case 0x4007: {
      Pulse & p = m_pulse[(addr >> 2) & 1];
      p.period = (p.period & 0xff) | ((v & 7) << 8);
      if (p.enabled)
        p.length = s_lengthTable[v >> 3];
      p.seq = 0;
      p.env.start = true;
      break;
    }
    case 0x4008:
      m_tri.control = v & 0x80;
      m_tri.linearReloadValue = v & 0x7f;
      break;
    case 0x400a:
      m_tri.period = (m_tri.period & 0x700) | v;
      break;
    case 0x400b:
      m_tri.period = (m_tri.period & 0xff) | ((v & 7) << 8);
      if (m_tri.enabled)
        m_tri.length = s_lengthTable[v >> 3];
      m_tri.linearReload = true;
      break;
    case 0x400c:
      m_noise.env.loop = v & 0x20;
      m_noise.env.constant = v & 0x10;
      m_noise.env.volume = v & 0x0f;
      break;
    case 0x400e:
      m_noise.mode = v & 0x80;
      m_noise.period = (m_region == Region::PAL ? s_noisePeriodPAL : s_noisePeriodNTSC)[v & 0x0f];
      break;
    case 0x400f:
      if (m_noise.enabled)
        m_noise.length = s_lengthTable[v >> 3];
      m_noise.env.start = true;
      break;
    case 0x4010:
      m_dmc.irqEnabled = v & 0x80;
      m_dmc.loop = v & 0x40;
      m_dmc.period = (m_region == Region::PAL ? s_dmcPeriodPAL : s_dmcPeriodNTSC)[v & 0x0f];
      if (!m_dmc.irqEnabled) {
        m_dmc.irqFlag = false;
        m_nes->cpu.clearIrq(Cpu::IRQ_DMC);
      }
      break;
    case 0x4011:
      m_dmc.level = v & 0x7f;
      break;
    case 0x4012:
      m_dmc.startAddr = 0xc000 | (v << 6);
      break;
    case 0x4013:
      m_dmc.startLength = (v << 4) | 1;
      break;
    case 0x4015:
      m_pulse[0].enabled = v & 0x01;
      m_pulse[1].enabled = v & 0x02;
      m_tri.enabled = v & 0x04;
      m_noise.enabled = v & 0x08;
      if (!m_pulse[0].enabled) m_pulse[0].length = 0;
      if (!m_pulse[1].enabled) m_pulse[1].length = 0;
      if (!m_tri.enabled)      m_tri.length = 0;
      if (!m_noise.enabled)    m_noise.length = 0;
      m_dmc.irqFlag = false;
      m_nes->cpu.clearIrq(Cpu::IRQ_DMC);
      if (v & 0x10) {
        if (!m_dmc.remaining) {
          m_dmc.addr = m_dmc.startAddr;
          m_dmc.remaining = m_dmc.startLength;
          if (!m_dmc.bufferFull) {
            // the "load DMA": 3 cycles after a write on a put cycle, 4 after one on a get cycle
            m_loadPending = true;
            m_loadAt = m_cycle - 1 + (m_nes->isGetCycle(m_cycle - 1) ? 4 : 3);
          }
        }
      } else {
        m_dmc.remaining = 0;
        m_loadPending = false;
      }
      break;
    case 0x4017:
      m_fiveStep = v & 0x80;
      m_irqInhibit = v & 0x40;
      if (m_irqInhibit) {
        m_frameIrq = false;
        m_nes->cpu.clearIrq(Cpu::IRQ_FRAME);
      }
      m_seqPositions = m_fiveStep ? (m_region == Region::PAL ? s_seq5PAL : s_seq5NTSC)
                                  : (m_region == Region::PAL ? s_seq4PAL : s_seq4NTSC);
      // The sequencer restarts 3 CPU cycles after a write on a "put" cycle, 4 after one on a
      // "get" cycle (gets are the even cycles). The write was in cycle m_cycle - 1, and
      // m_seqCycle counts from m_cycle.
      m_seqCycle = ((uint32_t)(m_cycle - 1) ^ m_parity) & 1 ? -3 : -4;
      m_seqStep = 0;
      if (m_fiveStep) {
        quarterFrame();
        halfFrame();
      }
      break;
  }
}


void Apu::state(StateIO & io)
{
  // the channels are plain structs
  io.pod(m_pulse); io.pod(m_tri); io.pod(m_noise); io.pod(m_dmc);
  io.pod(m_region); io.pod(m_dmaStall); io.pod(m_loadPending); io.pod(m_loadAt);
  io.pod(m_cycle); io.pod(m_parity); io.pod(m_accCycles);
  io.pod(m_fiveStep); io.pod(m_irqInhibit); io.pod(m_frameIrq); io.pod(m_seqCycle); io.pod(m_seqStep);
  m_frameIrqUntil = INT32_MIN;                  // a two cycle window, not worth a format change
  io.pod(m_nextSample); io.pod(m_hpPrevIn); io.pod(m_hpPrevOut);
  if (io.loading()) {
    m_seqPositions = m_fiveStep ? (m_region == Region::PAL ? s_seq5PAL : s_seq5NTSC)
                                : (m_region == Region::PAL ? s_seq4PAL : s_seq4NTSC);
    m_sampleCount = 0;
  }
}
