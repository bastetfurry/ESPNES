// The few Arduino APIs ESPNES uses, on plain ESP-IDF (no Arduino core in the firmware):
// String, Serial (UART0), SPIFFS with File, millis/delay and the GPIO calls. Only what main.cpp
// needs, with the same behaviour where it matters (Serial.parseInt's timeout, File's methods).
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"


// --- time, GPIO ---

inline uint32_t millis()                   { return (uint32_t)(esp_timer_get_time() / 1000); }
void            delay(uint32_t ms);
void            delayMicroseconds(uint32_t us);

enum { LOW = 0, HIGH = 1 };
enum { INPUT, OUTPUT, INPUT_PULLUP };
void pinMode(int pin, int mode);
inline void digitalWrite(int pin, int level) { gpio_set_level((gpio_num_t) pin, level); }
inline int  digitalRead(int pin)             { return gpio_get_level((gpio_num_t) pin); }


// --- String: std::string with Arduino's methods ---

class String : public std::string {
public:
  String() { }
  String(char const * s) : std::string(s ? s : "") { }
  String(std::string const & s) : std::string(s) { }
  String(char c) : std::string(1, c) { }
  String(int v) : std::string(std::to_string(v)) { }
  String(unsigned v) : std::string(std::to_string(v)) { }
  String(long v) : std::string(std::to_string(v)) { }

  int    length() const                         { return (int) size(); }
  bool   isEmpty() const                        { return empty(); }
  String substring(int from) const              { return from >= length() ? String() : String(substr(from)); }
  String substring(int from, int to) const
  {
    if (from > to)
      std::swap(from, to);
    return from >= length() ? String() : String(substr(from, to - from));
  }
  int    indexOf(char c) const                  { size_t p = find(c); return p == npos ? -1 : (int) p; }
  int    indexOf(char c, int from) const        { size_t p = find(c, from); return p == npos ? -1 : (int) p; }
  int    indexOf(char const * s) const          { size_t p = find(s); return p == npos ? -1 : (int) p; }
  int    lastIndexOf(char c) const              { size_t p = rfind(c); return p == npos ? -1 : (int) p; }
  bool   startsWith(String const & s) const     { return compare(0, s.size(), s) == 0; }
  bool   endsWith(String const & s) const       { return size() >= s.size() && compare(size() - s.size(), s.size(), s) == 0; }
  void   remove(int from)                       { if (from < length()) erase(from); }
  void   trim();
  void   toLowerCase();
  long   toInt() const                          { return strtol(c_str(), nullptr, 10); }

  String & operator+=(String const & s)         { append(s); return *this; }
  String & operator+=(char const * s)           { append(s); return *this; }
  String & operator+=(char c)                   { push_back(c); return *this; }
};

inline String operator+(String const & a, String const & b)  { String r(a); r += b; return r; }
inline String operator+(String const & a, char const * b)    { String r(a); r += b; return r; }
inline String operator+(char const * a, String const & b)    { String r(a); r += b; return r; }


// --- Serial: the console on UART0 ---

class SerialPort {
public:
  void   begin(int baud);
  void   setTxBufferSize(size_t size)          { m_txBuffer = size; }
  int    available();
  int    read();
  int    peek();
  long   parseInt();                            // skips to a number, waits up to 1 s for it
  void   printf(char const * format, ...) __attribute__((format(printf, 2, 3)));
  void   print(char const * s);
  void   println(char const * s = "");
  void   println(String const & s)             { println(s.c_str()); }
private:
  int    timedRead();
  size_t m_txBuffer = 256;
  int    m_peeked = -1;
};

extern SerialPort Serial;


// --- SPIFFS and its files, on the IDF's SPIFFS (mounted at /spiffs) ---

class File {
public:
  File() { }
  File(FILE * f, String const & name, bool dir = false) : m_f(f), m_name(name), m_dir(dir) { }
  File(File && o) : m_f(o.m_f), m_name(o.m_name), m_dir(o.m_dir), m_dirHandle(o.m_dirHandle) { o.m_f = nullptr; o.m_dirHandle = nullptr; }
  File & operator=(File && o);
  ~File()                                       { close(); }

  explicit operator bool() const                { return m_f || m_dirHandle; }
  size_t read(uint8_t * buf, size_t size)       { return m_f ? fread(buf, 1, size, m_f) : 0; }
  size_t write(uint8_t const * buf, size_t size){ return m_f ? fwrite(buf, 1, size, m_f) : 0; }
  size_t print(String const & s)                { return write((uint8_t const *) s.c_str(), s.size()); }
  int    available();
  String readStringUntil(char end);
  size_t size();
  char const * name() const                     { return m_name.c_str(); }
  void   close();

  File   openNextFile();                        // for the root directory

private:
  friend class SpiffsFS;
  FILE * m_f = nullptr;
  String m_name;                                // without the mount point: "/name"
  bool   m_dir = false;
  void * m_dirHandle = nullptr;
};

class SpiffsFS {
public:
  bool   begin(bool formatOnFail);
  File   open(String const & path, char const * mode = "r");
  bool   exists(String const & path);
  bool   remove(String const & path);
  bool   rename(String const & from, String const & to);
  size_t totalBytes();
  size_t usedBytes();
};

extern SpiffsFS SPIFFS;
