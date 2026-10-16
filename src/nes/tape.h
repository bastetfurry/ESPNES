// The Famicom Data Recorder: a cassette recorder on the Family BASIC keyboard's jacks.
//
// The console has no tape logic; the CPU drives the signal itself, a game times its own bits:
//   $4016 write bit 0   1 bit "DAC" to the recorder
//   $4016 write bit 2   when 0, the recorder's input reads 0
//   $4016 read  bit 1   1 bit "ADC" from the recorder
// Every game has its own encoding, so the tape holds just the signal: the times between its level
// changes, in CPU cycles, played back exactly as they were recorded.
//
// The tape image ("FTAP"): a header, then runs of square wave periods.
//   0  "FTAP"
//   4  version (1)
//   5  the level the tape starts with (0 or 1)
//   6  the CPU clock in Hz (uint32, little endian), for converting to sound
//   10 runs of periods: n periods of a cycles at the starting level, then b cycles at the other;
//      a last b of 0 ends a tape after a half period. A run is a token byte: its high nibble is
//      an index into a list of the last 15 periods (a, b) used, most recent first, or 15 for a
//      new one, whose a and b follow as varints (7 bits per byte, low first, bit 7 = more); its
//      low nibble is n - 1, or 15 for an n that follows as a varint. The period used moves to
//      the front of the list.
// Games time their bits with counted loops, so the same few periods repeat exactly: Mach Rider's
// save, 27 seconds and 2400 runs, takes 2.8 KB.
#pragma once

#include <stddef.h>
#include "platform.h"

class Tape {
public:
  enum Mode { STOPPED, PLAYING, RECORDING };

  static constexpr int HEADER_SIZE = 10;

  Tape()                                     { }
  ~Tape();
  Tape(Tape const &) = delete;
  Tape & operator=(Tape const &) = delete;

  Mode     mode() const                      { return m_mode; }
  bool     empty() const                     { return m_size <= HEADER_SIZE; }
  bool     overflowed() const                { return m_overflow; }   // the last recording did not fit

  // the tape image (valid while stopped)
  uint8_t const * data() const               { return m_data; }
  size_t   size() const                      { return m_size; }
  // Takes an image (copies it). Returns nullptr or an error message.
  char const * load(uint8_t const * image, size_t size);
  // a tape of 'capacity' bytes to be filled with load data via loadBuffer(), then checked with
  // load(nullptr, size): for reading an image straight into place
  uint8_t * loadBuffer(size_t capacity);

  // played time in seconds and the tape's length (for the display)
  uint32_t playedSeconds() const             { return m_clock ? (uint32_t)(m_elapsed / m_clock) : 0; }

  // --- for the console (Nes) ---
  // 'cycle' is the console's frame relative CPU cycle; rebase() moves the stored ones at the end
  // of every frame.
  void     record(int32_t cycle, int level, uint32_t clock, size_t capacity);
  void     play(int32_t cycle);
  void     stop();
  void     output(int32_t cycle, int level)
  {
    if (m_mode == RECORDING && level != m_outLevel)
      edge(cycle, level);
  }
  int      input(int32_t cycle)
  {
    if (m_mode != PLAYING)
      return 0;
    while (cycle >= m_next && m_mode == PLAYING)
      advance();
    return m_mode == PLAYING ? m_inLevel : 0;
  }
  void     rebase(int32_t cycles);

private:
  void     edge(int32_t cycle, int level);
  void     run(uint32_t a, uint32_t b);
  void     flushRun();
  void     putVarint(uint32_t v);
  bool     getVarint(uint32_t * v);
  bool     nextRun();
  void     advance();

  Mode      m_mode = STOPPED;
  uint8_t * m_data = nullptr;
  size_t    m_size = 0;
  size_t    m_capacity = 0;
  bool      m_overflow = false;
  uint32_t  m_clock = 0;
  uint64_t  m_elapsed = 0;          // cycles played or recorded

  // recording
  int       m_outLevel = 0;
  int32_t   m_lastEdge = 0;
  int32_t   m_half = -1;            // the first half of a period, waiting for its second
  uint32_t  m_runA = 0, m_runB = 0, m_runN = 0;
  static constexpr int RECENT = 15;
  struct Period { uint32_t a, b; };
  Period    m_recent[RECENT];
  int       m_recentCount = 0;
  void      useRecent(int i, uint32_t a, uint32_t b);

  // playing
  size_t    m_pos = 0;
  int       m_inLevel = 0;
  int       m_startLevel = 0;
  int       m_phase = 0;            // 0: in a, 1: in b
  int32_t   m_next = 0;             // the cycle of the next level change
};
