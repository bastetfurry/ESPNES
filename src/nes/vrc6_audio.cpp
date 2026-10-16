#include <string.h>

#include "vrc6_audio.h"


void Vrc6Audio::reset()
{
  memset(m_pulse, 0, sizeof(m_pulse));
  memset(&m_saw, 0, sizeof(m_saw));
  for (auto & p : m_pulse) {
    p.step = 15;
    p.timer = 1;
  }
  m_saw.timer = 1;
  m_halt = false;
  m_shift = 0;
  acc[0] = acc[1] = acc[2] = 0;
}


void Vrc6Audio::write(uint16_t addr, uint8_t v)
{
  if (addr == 0x9003) {
    m_halt = v & 1;
    m_shift = v & 4 ? 8 : v & 2 ? 4 : 0;
    return;
  }
  int reg = addr & 3;
  if (addr < 0xb000) {
    Pulse & p = m_pulse[addr >= 0xa000];
    switch (reg) {
      case 0:
        p.mode = v & 0x80;
        p.duty = (v >> 4) & 7;
        p.volume = v & 0x0f;
        break;
      case 1:
        p.period = (p.period & 0xf00) | v;
        break;
      case 2:
        p.period = (p.period & 0x0ff) | (v & 0x0f) << 8;
        p.enabled = v & 0x80;
        if (!p.enabled)
          p.step = 15;                           // disabled: the sequence starts over
        break;
    }
    return;
  }
  switch (reg) {
    case 0:
      m_saw.rate = v & 0x3f;
      break;
    case 1:
      m_saw.period = (m_saw.period & 0xf00) | v;
      break;
    case 2:
      m_saw.period = (m_saw.period & 0x0ff) | (v & 0x0f) << 8;
      m_saw.enabled = v & 0x80;
      if (!m_saw.enabled) {
        m_saw.accumulator = 0;
        m_saw.step = 0;
      }
      break;
  }
}


NES_HOT void Vrc6Audio::advance(int32_t n)
{
  if (m_halt) {
    acc[0] += m_pulse[0].level() * n;
    acc[1] += m_pulse[1].level() * n;
    acc[2] += m_saw.level() * n;
    return;
  }
  // a disabled channel's output does not change: its timer stands still
  bool on0 = m_pulse[0].enabled, on1 = m_pulse[1].enabled, onSaw = m_saw.enabled;
  if (!on0 && !on1 && !onSaw) {
    acc[0] += m_pulse[0].level() * n;
    acc[1] += m_pulse[1].level() * n;
    acc[2] += m_saw.level() * n;
    return;
  }
  while (n > 0) {
    int32_t step = n;
    if (on0 && m_pulse[0].timer < step)
      step = m_pulse[0].timer;
    if (on1 && m_pulse[1].timer < step)
      step = m_pulse[1].timer;
    if (onSaw && m_saw.timer < step)
      step = m_saw.timer;
    if (step < 1)
      step = 1;

    acc[0] += m_pulse[0].level() * step;
    acc[1] += m_pulse[1].level() * step;
    acc[2] += m_saw.level() * step;
    n -= step;

    for (auto & p : m_pulse) {
      if (!p.enabled)
        continue;
      p.timer -= step;
      if (p.timer <= 0) {
        p.timer = reload(p.period);
        p.step = (p.step - 1) & 15;
      }
    }
    if (onSaw) {
      m_saw.timer -= step;
      if (m_saw.timer <= 0) {
        m_saw.timer = reload(m_saw.period);
        // 14 steps: the rate is added on every second one, the 14th starts over
        if (++m_saw.step == 14) {
          m_saw.step = 0;
          m_saw.accumulator = 0;
        } else if (!(m_saw.step & 1)) {
          m_saw.accumulator += m_saw.rate;
        }
      }
    }
  }
}
