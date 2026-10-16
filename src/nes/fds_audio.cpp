#include <string.h>

#include "fds_audio.h"


void FdsAudio::reset()
{
  memset(m_wave, 0, sizeof(m_wave));
  memset(m_modTable, 0, sizeof(m_modTable));
  m_pitch = 0;
  m_waveAcc = 0;
  m_wavePos = 0;
  m_waveHalt = true;
  m_envHalt = false;
  m_waveWrite = false;
  m_masterVolume = 0;
  m_masterSpeed = 0xe8;                     // as the BIOS sets it
  m_vol = { true, false, 0, 0, 0 };         // $4080 = $80
  m_mod = { true, false, 0, 0, 0 };
  resetTimer(m_vol);
  resetTimer(m_mod);
  m_modPitch = 0;
  m_modAcc = 0;
  m_modPos = 0;
  m_modCounter = 0;
  m_modHalt = true;
  m_modOffset = 0;
  m_level = 0;
  acc = 0;
}


int FdsAudio::read(uint16_t addr) const
{
  if (addr >= 0x4040 && addr < 0x4080)
    return m_wave[addr & 0x3f] | 0x40;      // bits 6-7: open bus ($40)
  if (addr == 0x4090)
    return m_vol.gain | 0x40;
  if (addr == 0x4092)
    return m_mod.gain | 0x40;
  return -1;
}


void FdsAudio::write(uint16_t addr, uint8_t v)
{
  if (addr >= 0x4040 && addr < 0x4080) {
    if (m_waveWrite) {
      m_wave[addr & 0x3f] = v & 0x3f;
      updateLevel();
    }
    return;
  }
  switch (addr) {
    case 0x4080:
    case 0x4084: {
      Envelope & e = addr == 0x4080 ? m_vol : m_mod;
      e.off = v & 0x80;
      e.up = v & 0x40;
      e.speed = v & 0x3f;
      if (e.off)
        e.gain = v & 0x3f;
      resetTimer(e);
      updateModulation();
      updateLevel();
      break;
    }
    case 0x4082:
      m_pitch = (m_pitch & 0xf00) | v;
      updateModulation();
      break;
    case 0x4083:
      m_pitch = (m_pitch & 0x0ff) | (v & 0x0f) << 8;
      m_waveHalt = v & 0x80;
      m_envHalt = v & 0x40;
      if (m_waveHalt) {
        m_waveAcc = 0;
        m_wavePos = 0;
      }
      if (m_envHalt) {
        resetTimer(m_vol);
        resetTimer(m_mod);
      }
      updateModulation();
      updateLevel();
      break;
    case 0x4085:
      m_modCounter = (int8_t)(v << 1) >> 1; // 7 bit signed
      updateModulation();
      break;
    case 0x4086:
      m_modPitch = (m_modPitch & 0xf00) | v;
      break;
    case 0x4087:
      m_modPitch = (m_modPitch & 0x0ff) | (v & 0x0f) << 8;
      m_modHalt = v & 0x80;
      if (m_modHalt)
        m_modAcc = 0;
      break;
    case 0x4088:
      // while halted: the entry at the position, twice (the table steps in half steps)
      if (m_modHalt) {
        m_modTable[m_modPos] = m_modTable[(m_modPos + 1) & 0x3f] = v & 7;
        m_modPos = (m_modPos + 2) & 0x3f;
      }
      break;
    case 0x4089:
      m_masterVolume = v & 3;
      m_waveWrite = v & 0x80;
      updateLevel();
      break;
    case 0x408a:
      m_masterSpeed = v;
      resetTimer(m_vol);
      resetTimer(m_mod);
      break;
  }
}


// The wave pitch's change: counter * gain, rounded in the hardware's odd way, times the pitch.
void FdsAudio::updateModulation()
{
  int32_t temp = m_modCounter * m_mod.gain;
  int32_t remainder = temp & 0xf;
  temp >>= 4;
  if (remainder > 0 && !(temp & 0x80))
    temp += m_modCounter < 0 ? -1 : 2;
  if (temp >= 192)
    temp -= 256;
  else if (temp < -64)
    temp += 256;
  temp *= m_pitch;
  remainder = temp & 0x3f;
  temp >>= 6;
  if (remainder >= 32)
    temp += 1;
  m_modOffset = temp;
}


void FdsAudio::updateLevel()
{
  // the master volume: 2/2, 2/3, 2/4, 2/5 (36, 24, 18, 14.4 of 36); gain is clamped to 32
  static const int32_t VOLUME[4] = { 36, 24, 17, 14 };
  int32_t gain = m_vol.gain < 32 ? m_vol.gain : 32;
  m_level = m_wave[m_wavePos] * gain * VOLUME[m_masterVolume] / 1152;
}


bool FdsAudio::tick(Envelope & e) const
{
  if (e.off || --e.timer > 0)
    return false;
  e.timer = 8 * (e.speed + 1) * m_masterSpeed;
  if (e.up && e.gain < 32)
    ++e.gain;
  else if (!e.up && e.gain > 0)
    --e.gain;
  else
    return false;
  return true;
}


void FdsAudio::stepModulator()
{
  static const int8_t STEP[8] = { 0, 1, 2, 4, 0, -4, -2, -1 };
  uint8_t entry = m_modTable[m_modPos];
  int counter = entry == 4 ? 0 : m_modCounter + STEP[entry];
  if (counter >= 64)
    counter -= 128;
  else if (counter < -64)
    counter += 128;
  m_modCounter = counter;
  m_modPos = (m_modPos + 1) & 0x3f;
  updateModulation();
}


NES_HOT void FdsAudio::advance(int32_t n)
{
  while (n > 0) {
    // the cycles to the next event
    int32_t step = n;
    bool envelopes = envelopesRun();
    if (envelopes) {
      if (!m_vol.off && m_vol.timer < step)
        step = m_vol.timer;
      if (!m_mod.off && m_mod.timer < step)
        step = m_mod.timer;
    }
    bool modRuns = !m_modHalt && m_modPitch;
    if (modRuns) {
      int32_t c = (0x10000 - m_modAcc + m_modPitch - 1) / m_modPitch;
      if (c < step)
        step = c;
    }
    int32_t pitch = m_pitch + m_modOffset;
    bool waveRuns = !m_waveHalt && !m_waveWrite && pitch > 0;
    if (waveRuns) {
      int32_t c = (0x10000 - m_waveAcc + pitch - 1) / pitch;
      if (c < step)
        step = c;
    }
    if (step < 1)
      step = 1;

    acc += m_level * step;
    n -= step;

    // the events at its end, in the hardware's order: envelopes, modulator, wave
    if (envelopes) {
      if (!m_vol.off) {
        m_vol.timer -= step - 1;
        if (tick(m_vol))
          updateLevel();
      }
      if (!m_mod.off) {
        m_mod.timer -= step - 1;
        if (tick(m_mod))
          updateModulation();
      }
    }
    if (modRuns) {
      m_modAcc += m_modPitch * step;
      if (m_modAcc >= 0x10000) {
        m_modAcc &= 0xffff;
        stepModulator();
      }
    }
    if (waveRuns) {
      m_waveAcc += pitch * step;
      if (m_waveAcc >= 0x10000) {
        m_waveAcc &= 0xffff;
        m_wavePos = (m_wavePos + 1) & 0x3f;
        updateLevel();
      }
    }
  }
}
