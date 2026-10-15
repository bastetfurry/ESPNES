// Savestates: the emulator's state as a compressed stream.
//
// Each part of the emulator has one state(StateIO &) function that both saves and loads: it
// passes every field to io.bytes() (or io.pod()), which either writes it or fills it in. Pointers
// are not saved; after loading, the bank pointers are rebuilt from the registers they come from.
//
// The stream is compressed field by field with a small LZ: a back reference may only point into
// the field itself, so neither side needs a window buffer (the field in memory is the window).
// Tokens: a byte c < 0x80 is followed by c + 1 literal bytes; c >= 0x80 copies (c & 0x7f) + 3
// bytes from a 16 bit distance back. 8 KB of zeros take about 200 bytes.
#pragma once

#include <stddef.h>
#include "platform.h"

// where a saved state goes, and where a loaded one comes from (a file, memory)
class StateSink {
public:
  virtual bool write(uint8_t const * data, size_t size) = 0;   // false: full, failed
};

class StateSource {
public:
  virtual size_t read(uint8_t * data, size_t size) = 0;        // bytes read, fewer at the end
};


class StateIO {
public:
  bool loading() const       { return m_source != nullptr; }
  bool ok() const            { return m_ok; }
  void fail()                { m_ok = false; }

  void bytes(void * data, size_t size);
  template <typename T> void pod(T & v)  { bytes(&v, sizeof(v)); }

  // a field that is saved only when 'present' says so; both sides must agree on it
  void optional(void * data, size_t size, bool present)  { if (present) bytes(data, size); }

  bool finish();             // after saving: writes what is buffered. Returns ok().

protected:
  StateIO(StateSink * sink, StateSource * source) : m_sink(sink), m_source(source) { }
  ~StateIO();

private:
  void   save(uint8_t const * p, size_t n);
  void   load(uint8_t * p, size_t n);
  void   put(uint8_t b);
  int    get();
  void   flushLiterals(uint8_t const * p, size_t n);

  StateSink *   m_sink;
  StateSource * m_source;
  bool          m_ok = true;
  uint16_t *    m_hash = nullptr;       // saving: positions by the hash of 3 bytes
  uint8_t       m_buf[256];
  size_t        m_bufLen = 0, m_bufPos = 0;
};

class StateWriter : public StateIO {
public:
  explicit StateWriter(StateSink & sink) : StateIO(&sink, nullptr) { }
};

class StateReader : public StateIO {
public:
  explicit StateReader(StateSource & source) : StateIO(nullptr, &source) { }
};
