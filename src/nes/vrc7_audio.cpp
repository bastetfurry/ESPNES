#include <new>
#include <string.h>

#include "vrc7_audio.h"
#include "opl/dbopl.h"


// the built-in instruments 1-15, bytes as $00-$07 (nesdev wiki)
static const uint8_t PATCHES[15][8] = {
  { 0x03, 0x21, 0x05, 0x06, 0xe8, 0x81, 0x42, 0x27 },   // buzzy bell
  { 0x13, 0x41, 0x14, 0x0d, 0xd8, 0xf6, 0x23, 0x12 },   // guitar
  { 0x11, 0x11, 0x08, 0x08, 0xfa, 0xb2, 0x20, 0x12 },   // wurly
  { 0x31, 0x61, 0x0c, 0x07, 0xa8, 0x64, 0x61, 0x27 },   // flute
  { 0x32, 0x21, 0x1e, 0x06, 0xe1, 0x76, 0x01, 0x28 },   // clarinet
  { 0x02, 0x01, 0x06, 0x00, 0xa3, 0xe2, 0xf4, 0xf4 },   // synth
  { 0x21, 0x61, 0x1d, 0x07, 0x82, 0x81, 0x11, 0x07 },   // trumpet
  { 0x23, 0x21, 0x22, 0x17, 0xa2, 0x72, 0x01, 0x17 },   // organ
  { 0x35, 0x11, 0x25, 0x00, 0x40, 0x73, 0x72, 0x01 },   // bells
  { 0xb5, 0x01, 0x0f, 0x0f, 0xa8, 0xa5, 0x51, 0x02 },   // vibes
  { 0x17, 0xc1, 0x24, 0x07, 0xf8, 0xf8, 0x22, 0x12 },   // vibraphone
  { 0x71, 0x23, 0x11, 0x06, 0x65, 0x74, 0x18, 0x16 },   // tutti
  { 0x01, 0x02, 0xd3, 0x05, 0xc9, 0x95, 0x03, 0x02 },   // fretless
  { 0x61, 0x63, 0x0c, 0x00, 0x94, 0xc0, 0x33, 0xf6 },   // synth bass
  { 0x21, 0x72, 0x0d, 0x00, 0xc1, 0xd5, 0x56, 0x06 },   // sweep
};

// the OPL2's operator register offsets of channels 0-5 (the modulator; the carrier is 3 more)
static const uint8_t MODULATOR[6] = { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0a };


bool Vrc7Audio::start()
{
  if (!m_chip) {
    static bool tables;
    if (!tables) {
      DBOPL::InitTables();
      tables = true;
    }
    m_chip = new (std::nothrow) DBOPL::Chip(false);   // an OPL2
    if (!m_chip)
      return false;
    m_chip->Setup(1789773 / CYCLES);
  }
  reset();
  return true;
}


void Vrc7Audio::stop()
{
  delete m_chip;
  m_chip = nullptr;
}


void Vrc7Audio::opl(uint32_t reg, uint8_t value)
{
  m_chip->WriteReg(reg, value);
}


void Vrc7Audio::reset()
{
  memset(m_reg, 0, sizeof(m_reg));
  m_select = 0;
  m_pos = BLOCK;
  m_sample = 0;
  m_timer = CYCLES;
  acc = 0;
  if (!m_chip)
    return;
  for (uint32_t r = 0x20; r < 0x100; ++r)
    opl(r, 0);
  opl(0x01, 0x20);                       // the waveform select on (half sine: wave 1)
  opl(0x08, 0x40);                       // key rate scaling from f-number bit 9 (the OPLL's bit 8)
  opl(0xbd, 0xc0);                       // the deep tremolo (4.8 dB) and vibrato, as the OPLL's
  for (int ch = 0; ch < 6; ++ch) {
    updateInstrument(ch);
    updateFrequency(ch);
  }
}


// KSL: the OPLL's 0, 1.5, 3, 6 dB per octave are the OPL2's 0, 2, 1, 3
static uint8_t ksl(uint8_t bits)
{
  return (bits & 0x80) >> 1 | (bits & 0x40) << 1;
}


void Vrc7Audio::updateInstrument(int ch)
{
  uint8_t inst = m_reg[0x30 + ch] >> 4, volume = m_reg[0x30 + ch] & 0x0f;
  uint8_t const * p = inst ? PATCHES[inst - 1] : m_reg;
  bool sustain = m_reg[0x20 + ch] & 0x20;
  uint8_t mod = MODULATOR[ch], car = mod + 3;
  opl(0x20 + mod, p[0]);
  opl(0x20 + car, p[1]);
  opl(0x40 + mod, ksl(p[2]) | (p[2] & 0x3f));
  opl(0x40 + car, ksl(p[3]) | volume << 2);           // 3 dB a step: 4 of the OPL2's 0.75 dB
  opl(0x60 + mod, p[4]);
  opl(0x60 + car, p[5]);
  opl(0x80 + mod, p[6]);
  opl(0x80 + car, sustain ? (p[7] & 0xf0) | 5 : p[7]);
  opl(0xe0 + mod, (p[3] >> 3) & 1);
  opl(0xe0 + car, (p[3] >> 4) & 1);
  opl(0xc0 + ch, (p[3] & 7) << 1);                    // feedback, FM
}


void Vrc7Audio::updateFrequency(int ch)
{
  uint8_t hi = m_reg[0x20 + ch];
  uint32_t fnum = ((hi & 1) << 8 | m_reg[0x10 + ch]) << 1;   // 9 bits to the OPL2's 10
  opl(0xa0 + ch, fnum & 0xff);
  opl(0xb0 + ch, (hi & 0x10 ? 0x20 : 0) | ((hi >> 1) & 7) << 2 | fnum >> 8);
}


void Vrc7Audio::write(uint8_t value)
{
  uint8_t r = m_select;
  m_reg[r] = value;
  if (!m_chip)
    return;
  if (r < 0x08) {
    for (int ch = 0; ch < 6; ++ch)
      if (!(m_reg[0x30 + ch] >> 4))
        updateInstrument(ch);                         // the custom instrument changed
  } else if (r >= 0x10 && r < 0x16) {
    updateFrequency(r & 7);
  } else if (r >= 0x20 && r < 0x26) {
    updateInstrument(r & 7);                          // the sustain bit is the release rate
    updateFrequency(r & 7);
  } else if (r >= 0x30 && r < 0x36) {
    updateInstrument(r & 7);
  }
}


int Vrc7Audio::channelLevel(int ch) const
{
  if (!m_chip)
    return 0;
  DBOPL::Operator const & car = m_chip->chan[ch].op[1];
  int att = car.volume + (int) car.currentLevel;      // 0 (loud) to 511 (silent)
  return att >= 511 ? 0 : (511 - att) >> 1;
}


NES_HOT void Vrc7Audio::advance(int32_t n)
{
  if (!m_chip)
    return;
  while (n > 0) {
    int32_t step = m_timer < n ? m_timer : n;
    acc += m_sample * step;
    n -= step;
    m_timer -= step;
    if (m_timer <= 0) {
      m_timer += CYCLES;
      if (m_pos == BLOCK) {
        m_chip->GenerateBlock2(BLOCK, m_block);
        m_pos = 0;
      }
      m_sample = m_block[m_pos++];
    }
  }
}
