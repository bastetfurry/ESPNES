#include <string.h>

#include "s5b_audio.h"


// the DAC: 5 bit levels at 1.5 dB per step, 31 = 255 (255 * 10^(-(31 - L) * 1.5 / 20)); 0 and 1
// are silent
static const uint8_t AMPLITUDE[32] = {
    0,  0,  2,  2,  2,  3,  3,  4,  5,  6,  7,  8, 10, 11, 14, 16,
   19, 23, 27, 32, 38, 45, 54, 64, 76, 90,108,128,152,181,215,255,
};


void S5bAudio::reset()
{
  memset(m_reg, 0, sizeof(m_reg));
  m_reg[7] = 0x3f;                       // everything off
  m_select = 0;
  m_writable = true;
  for (int i = 0; i < 3; ++i) {
    m_toneTimer[i] = 16;
    m_tone[i] = false;
    m_level[i] = 0;
    acc[i] = 0;
  }
  m_noiseTimer = 32;
  m_lfsr = 1;
  m_envTimer = 16;
  m_envStep = 31;
  m_envAttack = 0;
  m_envHolding = true;
  m_envHold = m_envAlternate = false;
  m_envLevel = 0;
}


void S5bAudio::select(uint8_t value)
{
  m_select = value & 0x0f;
  m_writable = !(value & 0xf0);          // as the YM2149F: high bits set, writes are ignored
}


void S5bAudio::write(uint8_t value)
{
  if (!m_writable)
    return;
  m_reg[m_select] = value;
  if (m_select == 0x0d) {
    // a new shape restarts the envelope; without "continue" it ends at 0 like hold + attack
    bool cont = value & 8, attack = value & 4;
    m_envHold = cont ? value & 1 : true;
    m_envAlternate = cont ? value & 2 : attack;
    m_envAttack = attack ? 31 : 0;
    m_envStep = 31;
    m_envHolding = false;
    m_envLevel = m_envStep ^ m_envAttack;
    m_envTimer = 16 * (((m_reg[0x0c] << 8) | m_reg[0x0b]) ? ((m_reg[0x0c] << 8) | m_reg[0x0b]) : 1);
  }
  updateLevels();
}


void S5bAudio::updateLevels()
{
  uint8_t mixer = m_reg[7];
  bool noise = m_lfsr & 1;
  for (int i = 0; i < 3; ++i) {
    bool toneOn = !(mixer & (1 << i)), noiseOn = !(mixer & (8 << i));
    bool high = (!toneOn || m_tone[i]) && (!noiseOn || noise);
    uint8_t v = m_reg[8 + i];
    // the fixed volume uses every second DAC level (3 dB a step), the envelope all of them
    int level = v & 0x10 ? m_envLevel : (v & 0x0f) ? (v & 0x0f) * 2 + 1 : 0;
    m_level[i] = high ? AMPLITUDE[level] : 0;
  }
}


void S5bAudio::envelopeStep()
{
  if (m_envHolding)
    return;
  if (--m_envStep < 0) {
    if (m_envHold) {
      // the end of the ramp (attack: 31, decay: 0), or its other end when alternating
      m_envLevel = m_envAlternate ? m_envAttack ^ 31 : m_envAttack;
      m_envHolding = true;
      return;
    }
    if (m_envAlternate)
      m_envAttack ^= 31;
    m_envStep = 31;
  }
  m_envLevel = m_envStep ^ m_envAttack;
}


NES_HOT void S5bAudio::advance(int32_t n)
{
  // generators the mixer has off do not change the output: their timers stand still
  uint8_t mixer = m_reg[7];
  bool toneUsed[3], noiseUsed = false;
  int32_t half[3];
  for (int i = 0; i < 3; ++i) {
    toneUsed[i] = !(mixer & (1 << i));
    noiseUsed |= !(mixer & (8 << i));
    int period = ((m_reg[i * 2 + 1] & 0x0f) << 8) | m_reg[i * 2];
    half[i] = 16 * (period ? period : 1);
  }
  bool envUsed = !m_envHolding && ((m_reg[8] | m_reg[9] | m_reg[10]) & 0x10);
  while (n > 0) {
    int32_t step = n;
    for (int i = 0; i < 3; ++i)
      if (toneUsed[i] && m_toneTimer[i] < step)
        step = m_toneTimer[i];
    if (noiseUsed && m_noiseTimer < step)
      step = m_noiseTimer;
    if (envUsed && m_envTimer < step)
      step = m_envTimer;
    if (step < 1)
      step = 1;

    for (int i = 0; i < 3; ++i)
      acc[i] += m_level[i] * step;
    n -= step;

    bool changed = false;
    for (int i = 0; i < 3; ++i) {
      if (!toneUsed[i])
        continue;
      m_toneTimer[i] -= step;
      if (m_toneTimer[i] <= 0) {
        m_toneTimer[i] = half[i];
        m_tone[i] = !m_tone[i];
        changed = true;
      }
    }
    if (noiseUsed) {
      m_noiseTimer -= step;
      if (m_noiseTimer <= 0) {
        int period = m_reg[6] & 0x1f;
        m_noiseTimer = 32 * (period ? period : 1);
        // 17 bit LFSR, taps 16 and 13
        uint32_t bit = (m_lfsr ^ (m_lfsr >> 3)) & 1;
        m_lfsr = (m_lfsr >> 1) | (bit << 16);
        changed = true;
      }
    }
    if (envUsed) {
      m_envTimer -= step;
      if (m_envTimer <= 0) {
        int period = (m_reg[0x0c] << 8) | m_reg[0x0b];
        m_envTimer = 16 * (period ? period : 1);
        envelopeStep();
        envUsed = !m_envHolding;
        changed = true;
      }
    }
    if (changed)
      updateLevels();
  }
}
