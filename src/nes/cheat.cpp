#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cheat.h"


static int ggDigit(char c)
{
  static const char letters[] = "APZLGITYEOXUKSVN";
  char const * p = strchr(letters, toupper((unsigned char) c));
  return (p && *p) ? (int)(p - letters) : -1;
}


static int hexDigit(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  c = toupper((unsigned char) c);
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}


// hex: AAAAVV, AAAAVVCC from $8000, or Action Replay's 00AAAAVV (and short forms like AVV)
static bool decodeHex(char const * s, int len, Cheat * out)
{
  char buf[16];
  memcpy(buf, s, len + 1);
  if (len == 8 && buf[0] == '0' && buf[1] == '0')
    memmove(buf, buf + 2, (len -= 2) + 1);
  if (len < 3 || len > 8 || len == 7)
    return false;
  uint32_t v = 0;
  for (int i = 0; i < len; ++i) {
    int d = hexDigit(buf[i]);
    if (d < 0)
      return false;
    v = (v << 4) | d;
  }
  int compare = -1;
  if (len == 8) {
    compare = v & 0xff;
    v >>= 8;
  }
  uint16_t addr = v >> 8;
  if (compare >= 0 && addr < 0x8000)
    return false;               // a compare value only for ROM
  out->rom = addr >= 0x8000;
  out->addr = addr;
  out->value = v & 0xff;
  out->compare = compare;
  return true;
}


bool decodeCheat(char const * text, Cheat * out)
{
  // the code without separators
  char s[16];
  int len = 0;
  bool separated = false;
  for (; *text; ++text) {
    if (*text == ' ' || *text == '-' || *text == ':') {
      separated |= *text != '-';
      continue;
    }
    if (len == (int) sizeof(s) - 1)
      return false;
    s[len++] = *text;
  }
  s[len] = 0;
  if (separated && decodeHex(s, len, out))
    return true;

  // Game Genie: the letters are 4 bit values, scrambled into address, value and compare
  if (len == 6 || len == 8) {
    int n[8];
    bool gg = true;
    for (int i = 0; i < len && gg; ++i)
      gg = (n[i] = ggDigit(s[i])) >= 0;
    if (gg) {
      out->rom = true;
      out->addr = 0x8000 | ((n[3] & 7) << 12) | ((n[5] & 7) << 8) | ((n[4] & 8) << 8) |
                  ((n[2] & 7) << 4) | ((n[1] & 8) << 4) | (n[4] & 7) | (n[3] & 8);
      if (len == 6) {
        out->value = ((n[1] & 7) << 4) | ((n[0] & 8) << 4) | (n[0] & 7) | (n[5] & 8);
        out->compare = -1;
      } else {
        out->value = ((n[1] & 7) << 4) | ((n[0] & 8) << 4) | (n[0] & 7) | (n[7] & 8);
        out->compare = ((n[7] & 7) << 4) | ((n[6] & 8) << 4) | (n[6] & 7) | (n[5] & 8);
      }
      return true;
    }
  }
  return decodeHex(s, len, out);
}


void formatCheat(Cheat const & c, char * out, int outSize)
{
  if (c.compare >= 0)
    snprintf(out, outSize, "%04X %02X %02X", c.addr, c.value, c.compare);
  else
    snprintf(out, outSize, "%04X %02X", c.addr, c.value);
}
