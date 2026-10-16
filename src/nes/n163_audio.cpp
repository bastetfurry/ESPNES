#include <string.h>

#include "n163_audio.h"


void N163Audio::reset()
{
  memset(m_ram, 0, sizeof(m_ram));
  m_address = 0;
  m_increment = false;
  m_timer = 15;
  m_next = 7;
  memset(m_out, 0, sizeof(m_out));
  memset(m_shown, 0, sizeof(m_shown));
  acc = 0;
  memset(level, 0, sizeof(level));
}


void N163Audio::setAddress(uint8_t value)
{
  m_address = value & 0x7f;
  m_increment = value & 0x80;
}


void N163Audio::write(uint8_t value)
{
  m_ram[m_address] = value;
  if (m_increment && m_address < 0x7f)   // it stops at $7F
    ++m_address;
}


uint8_t N163Audio::read()
{
  uint8_t v = m_ram[m_address];
  if (m_increment && m_address < 0x7f)
    ++m_address;
  return v;
}


void N163Audio::update(int ch)
{
  uint8_t * r = m_ram + 0x40 + ch * 8;
  uint32_t freq = r[0] | r[2] << 8 | (r[4] & 3) << 16;
  uint32_t phase = r[1] | r[3] << 8 | r[5] << 16;
  uint32_t length = (256 - (r[4] & 0xfc)) << 16;
  phase = (phase + freq) % length;
  r[1] = phase;
  r[3] = phase >> 8;
  r[5] = phase >> 16;
  uint8_t index = (uint8_t)((phase >> 16) + r[6]);
  uint8_t byte = m_ram[index >> 1];
  int sample = index & 1 ? byte >> 4 : byte & 0x0f;
  int volume = r[7] & 0x0f;
  m_out[ch] = (sample - 8) * volume;
  m_shown[ch] = sample * volume;
}


NES_HOT void N163Audio::advance(int32_t n)
{
  while (n > 0) {
    int32_t step = m_timer < n ? m_timer : n;
    int count = channels();
    int first = 8 - count;               // the enabled ones: first..7
    // the held outputs, summed (only the enabled channels sound)
    int32_t sum = 0;
    for (int ch = first; ch < 8; ++ch) {
      sum += m_out[ch];
      level[ch] += m_shown[ch] * step;
    }
    acc += sum * step / count;
    n -= step;
    m_timer -= step;
    if (m_timer <= 0) {
      m_timer += 15;
      if (m_next < first)
        m_next = 7;
      update(m_next);
      m_next = m_next == first ? 7 : m_next - 1;
    }
  }
}
