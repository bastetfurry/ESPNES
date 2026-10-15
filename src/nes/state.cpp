#include <stdlib.h>
#include <string.h>

#include "state.h"


static constexpr int HASH_BITS = 12;
static constexpr int MIN_MATCH = 3, MAX_MATCH = 0x7f + MIN_MATCH;


StateIO::~StateIO()
{
  free(m_hash);
}


void StateIO::bytes(void * data, size_t size)
{
  if (!m_ok || !size)
    return;
  if (m_source)
    load((uint8_t *) data, size);
  else
    save((uint8_t const *) data, size);
}


void StateIO::put(uint8_t b)
{
  m_buf[m_bufLen++] = b;
  if (m_bufLen == sizeof(m_buf)) {
    m_ok &= m_sink->write(m_buf, m_bufLen);
    m_bufLen = 0;
  }
}


bool StateIO::finish()
{
  if (m_sink && m_bufLen && m_ok)
    m_ok = m_sink->write(m_buf, m_bufLen);
  m_bufLen = 0;
  return m_ok;
}


void StateIO::flushLiterals(uint8_t const * p, size_t n)
{
  while (n) {
    size_t run = n < 128 ? n : 128;
    put(run - 1);
    for (size_t i = 0; i < run; ++i)
      put(p[i]);
    p += run;
    n -= run;
  }
}


// A greedy LZ over one field: the hash table remembers the last position of each 3 byte string.
// Positions are 16 bit, so fields of up to 64 KB (the largest is 8 KB).
void StateIO::save(uint8_t const * p, size_t n)
{
  if (!m_hash && !(m_hash = (uint16_t *) malloc(sizeof(uint16_t) << HASH_BITS))) {
    m_ok = false;
    return;
  }
  memset(m_hash, 0xff, sizeof(uint16_t) << HASH_BITS);
  size_t lit = 0, i = 0;                      // literals start at i - lit
  while (i < n) {
    size_t len = 0, dist = 0;
    if (i + MIN_MATCH <= n) {
      unsigned h = ((p[i] << 8 | p[i + 1]) * 40503u + p[i + 2] * 2654435761u) >> (32 - HASH_BITS);
      h &= (1 << HASH_BITS) - 1;
      size_t j = m_hash[h];
      m_hash[h] = i;
      if (j != 0xffff && j < i) {
        size_t max = n - i < MAX_MATCH ? n - i : MAX_MATCH;
        while (len < max && p[j + len] == p[i + len])
          ++len;
        dist = i - j;
      }
      // a run of one byte value: copy from 1 back (overlapping), even if the hash missed it
      if (len < MIN_MATCH && i > 0 && p[i - 1] == p[i] && p[i] == p[i + 1] && p[i] == p[i + 2]) {
        size_t max = n - i < MAX_MATCH ? n - i : MAX_MATCH;
        len = 0;
        while (len < max && p[i + len] == p[i - 1])
          ++len;
        dist = 1;
      }
    }
    if (len >= MIN_MATCH) {
      flushLiterals(p + i - lit, lit);
      lit = 0;
      put(0x80 | (len - MIN_MATCH));
      put(dist & 0xff);
      put(dist >> 8);
      i += len;
    } else {
      ++lit;
      ++i;
    }
    if (!m_ok)
      return;
  }
  flushLiterals(p + i - lit, lit);
}


int StateIO::get()
{
  if (m_bufPos == m_bufLen) {
    m_bufLen = m_source->read(m_buf, sizeof(m_buf));
    m_bufPos = 0;
    if (!m_bufLen)
      return -1;
  }
  return m_buf[m_bufPos++];
}


void StateIO::load(uint8_t * p, size_t n)
{
  size_t i = 0;
  while (i < n) {
    int c = get();
    if (c < 0) {
      m_ok = false;
      return;
    }
    if (c < 0x80) {
      size_t run = c + 1;
      if (run > n - i) {
        m_ok = false;
        return;
      }
      for (size_t k = 0; k < run; ++k) {
        int b = get();
        if (b < 0) {
          m_ok = false;
          return;
        }
        p[i++] = b;
      }
    } else {
      size_t len = (c & 0x7f) + MIN_MATCH;
      int lo = get(), hi = get();
      size_t dist = (lo < 0 || hi < 0) ? 0 : (size_t)(lo | hi << 8);
      if (!dist || dist > i || len > n - i) {
        m_ok = false;
        return;
      }
      for (size_t k = 0; k < len; ++k, ++i)
        p[i] = p[i - dist];
    }
  }
}
