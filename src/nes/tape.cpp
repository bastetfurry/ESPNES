#include <stdlib.h>
#include <string.h>

#include "tape.h"


static constexpr uint8_t VERSION = 1;
// the longest time between two level changes that is kept; longer gaps (a recorder running on
// while nothing happens) are shortened to it, about 1 minute
static constexpr int32_t MAX_GAP = 1 << 27;


Tape::~Tape()
{
  free(m_data);
}


uint8_t * Tape::loadBuffer(size_t capacity)
{
  stop();
  m_resume = false;
  free(m_data);
  m_data = (uint8_t *) malloc(capacity);
  m_size = 0;
  m_capacity = m_data ? capacity : 0;
  return m_data;
}


void Tape::eject()
{
  stop();
  m_resume = false;
  free(m_data);
  m_data = nullptr;
  m_size = m_capacity = 0;
}


char const * Tape::load(uint8_t const * image, size_t size)
{
  if (image) {
    if (!loadBuffer(size))
      return "Not enough memory";
    memcpy(m_data, image, size);
  } else if (size > m_capacity) {
    return "Tape too long";
  }
  m_size = size;
  if (size < HEADER_SIZE || memcmp(m_data, "FTAP", 4) || m_data[4] != VERSION || m_data[5] > 1) {
    m_size = 0;
    return "Not a tape image";
  }
  m_clock = m_data[6] | m_data[7] << 8 | m_data[8] << 16 | (uint32_t) m_data[9] << 24;
  m_overflow = false;
  return nullptr;
}


///////////////////////////////////////////////////////////////////////////////
// recording

void Tape::record(int32_t cycle, int level, uint32_t clock, size_t capacity)
{
  stop();
  m_resume = false;
  free(m_data);
  m_data = (uint8_t *) malloc(capacity);
  m_capacity = m_data ? capacity : 0;
  m_size = 0;
  m_overflow = false;
  if (m_capacity < HEADER_SIZE + 16)
    return;
  memcpy(m_data, "FTAP", 4);
  m_data[4] = VERSION;
  m_data[5] = level;
  for (int i = 0; i < 4; ++i)
    m_data[6 + i] = clock >> (8 * i);
  m_size = HEADER_SIZE;
  m_clock = clock;
  m_elapsed = 0;
  m_outLevel = level;
  m_lastEdge = cycle;
  m_half = -1;
  m_runN = 0;
  m_recentCount = 0;
  m_mode = RECORDING;
}


void Tape::edge(int32_t cycle, int level)
{
  m_outLevel = level;
  int32_t gap = cycle - m_lastEdge;
  if (gap < 1)
    gap = 1;
  m_lastEdge = cycle;
  m_elapsed += gap;
  if (m_half < 0) {
    m_half = gap;
  } else {
    run(m_half, gap);
    m_half = -1;
  }
}


void Tape::run(uint32_t a, uint32_t b)
{
  if (m_runN && a == m_runA && b == m_runB && m_runN < 0x7fffffff) {
    ++m_runN;
    return;
  }
  flushRun();
  m_runA = a;
  m_runB = b;
  m_runN = 1;
}


void Tape::flushRun()
{
  if (!m_runN)
    return;
  // a token and three varints of up to 5 bytes each
  if (m_size + 16 > m_capacity) {
    m_overflow = true;
    m_mode = STOPPED;
    m_runN = 0;
    return;
  }
  int i = 0;
  while (i < m_recentCount && (m_recent[i].a != m_runA || m_recent[i].b != m_runB))
    ++i;
  int index = i < m_recentCount ? i : RECENT;
  m_data[m_size++] = index << 4 | (m_runN <= 15 ? m_runN - 1 : 15);
  if (index == RECENT) {
    putVarint(m_runA);
    putVarint(m_runB);
  }
  if (m_runN > 15)
    putVarint(m_runN);
  useRecent(i, m_runA, m_runB);
  m_runN = 0;
}


// moves the period at i (or a new one, i >= m_recentCount) to the front of the list
void Tape::useRecent(int i, uint32_t a, uint32_t b)
{
  if (i >= m_recentCount) {
    if (m_recentCount < RECENT)
      ++m_recentCount;
    i = m_recentCount - 1;
  }
  memmove(&m_recent[1], &m_recent[0], i * sizeof(Period));
  m_recent[0] = { a, b };
}


void Tape::putVarint(uint32_t v)
{
  while (v >= 0x80) {
    m_data[m_size++] = (v & 0x7f) | 0x80;
    v >>= 7;
  }
  m_data[m_size++] = v;
}


///////////////////////////////////////////////////////////////////////////////
// playing

bool Tape::getVarint(uint32_t * v)
{
  *v = 0;
  for (int shift = 0; shift < 35 && m_pos < m_size; shift += 7) {
    uint8_t c = m_data[m_pos++];
    *v |= (uint32_t)(c & 0x7f) << shift;
    if (!(c & 0x80))
      return true;
  }
  return false;
}


// the next run into m_runA/B/N; false at the end of the tape
bool Tape::nextRun()
{
  if (m_pos >= m_size)
    return false;
  uint8_t token = m_data[m_pos++];
  int i = token >> 4;
  if (i == RECENT) {
    if (!getVarint(&m_runA) || !getVarint(&m_runB))
      return false;
  } else if (i < m_recentCount) {
    m_runA = m_recent[i].a;
    m_runB = m_recent[i].b;
  } else {
    return false;
  }
  m_runN = (token & 15) + 1;
  if (m_runN == 16 && !getVarint(&m_runN))
    return false;
  useRecent(i, m_runA, m_runB);
  return m_runA && m_runN;
}


void Tape::play(int32_t cycle)
{
  stop();
  if (empty())
    return;
  if (m_resume) {
    // on from where it stopped, with the half period it was in from its start
    m_resume = false;
    m_next = cycle + (m_phase ? m_runB : m_runA);
    m_mode = PLAYING;
    return;
  }
  m_pos = HEADER_SIZE;
  m_startLevel = m_inLevel = m_data[5];
  m_elapsed = 0;
  m_recentCount = 0;
  if (!nextRun())
    return;
  m_phase = 0;
  m_next = cycle + m_runA;
  m_mode = PLAYING;
}


void Tape::advance()
{
  m_inLevel ^= 1;
  if (m_phase == 0) {
    m_elapsed += m_runA;
    if (!m_runB) {                 // the tape ends after a half period
      m_mode = STOPPED;
      return;
    }
    m_phase = 1;
    m_next += m_runB;
    return;
  }
  m_elapsed += m_runB;
  m_phase = 0;
  if (!--m_runN && !nextRun()) {
    m_mode = STOPPED;
    return;
  }
  m_next += m_runA;
}


///////////////////////////////////////////////////////////////////////////////

void Tape::stop()
{
  if (m_mode == RECORDING) {
    if (m_half >= 0)
      run(m_half, 0);
    flushRun();
    // give back what the recording did not use
    if (m_size && m_size < m_capacity)
      if (uint8_t * p = (uint8_t *) realloc(m_data, m_size)) {
        m_data = p;
        m_capacity = m_size;
      }
  }
  if (m_mode == PLAYING)
    m_resume = true;
  m_mode = STOPPED;
}


void Tape::rebase(int32_t cycles)
{
  if (m_mode == RECORDING) {
    m_lastEdge -= cycles;
    if (m_lastEdge < -MAX_GAP)
      m_lastEdge = -MAX_GAP;
  } else if (m_mode == PLAYING) {
    // the tape runs on whether the game reads it or not
    while (m_next <= cycles && m_mode == PLAYING)
      advance();
    m_next -= cycles;
  }
}
