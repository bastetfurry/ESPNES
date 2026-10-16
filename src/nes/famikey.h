// The Family BASIC keyboard: 72 keys in a matrix of 9 rows x 2 columns x 4 bits, scanned through
// the expansion port (Nes::setFamiKey):
//   $4016 write  bit 0: back to row 0; bit 1: the column, a 1 -> 0 change goes to the next row;
//                bit 2: the matrix on (when off, it reads 0)
//   $4017 read   bits 1-4: the selected row's and column's keys, 0 = held
// A tenth row has no keys (Lode Runner checks for the keyboard there); after it, row 0 again.
// A key's code is row * 8 + column * 4 + (bit - 1).
#pragma once

#include "platform.h"

enum FamiKey : uint8_t {
  FK_F8 = 0,     FK_RETURN,   FK_LBRACKET, FK_RBRACKET,     // row 0, column 0: bits 1-4
  FK_KANA,       FK_RSHIFT,   FK_YEN,      FK_STOP,         // row 0, column 1
  FK_F7,         FK_AT,       FK_COLON,    FK_SEMICOLON,
  FK_UNDERSCORE, FK_SLASH,    FK_MINUS,    FK_CARET,
  FK_F6,         FK_O,        FK_L,        FK_K,
  FK_PERIOD,     FK_COMMA,    FK_P,        FK_0,
  FK_F5,         FK_I,        FK_U,        FK_J,
  FK_M,          FK_N,        FK_9,        FK_8,
  FK_F4,         FK_Y,        FK_G,        FK_H,
  FK_B,          FK_V,        FK_7,        FK_6,
  FK_F3,         FK_T,        FK_R,        FK_D,
  FK_F,          FK_C,        FK_5,        FK_4,
  FK_F2,         FK_W,        FK_S,        FK_A,
  FK_X,          FK_Z,        FK_E,        FK_3,
  FK_F1,         FK_ESC,      FK_Q,        FK_CTR,
  FK_LSHIFT,     FK_GRPH,     FK_1,        FK_2,
  FK_CLRHOME,    FK_UP,       FK_RIGHT,    FK_LEFT,
  FK_DOWN,       FK_SPACE,    FK_DEL,      FK_INS,
  FK_COUNT,
  FK_NONE = 0xff
};

// The key (and whether Shift goes with it) that types an ASCII character in Family BASIC, or
// FK_NONE. Letters type as capitals either way; '\n' is RETURN.
inline FamiKey famiKeyOfChar(char c, bool * shift)
{
  static const FamiKey letters[26] = {
    FK_A, FK_B, FK_C, FK_D, FK_E, FK_F, FK_G, FK_H, FK_I, FK_J, FK_K, FK_L, FK_M,
    FK_N, FK_O, FK_P, FK_Q, FK_R, FK_S, FK_T, FK_U, FK_V, FK_W, FK_X, FK_Y, FK_Z,
  };
  static const FamiKey digits[10] = { FK_0, FK_1, FK_2, FK_3, FK_4, FK_5, FK_6, FK_7, FK_8, FK_9 };
  // the shifted characters, on the Famicom's (JIS like) layout
  static const struct { char c; FamiKey key; bool shift; } others[] = {
    { ' ', FK_SPACE, false }, { '\n', FK_RETURN, false }, { '-', FK_MINUS, false },
    { '^', FK_CARET, false }, { '@', FK_AT, false }, { '[', FK_LBRACKET, false },
    { ']', FK_RBRACKET, false }, { ';', FK_SEMICOLON, false }, { ':', FK_COLON, false },
    { ',', FK_COMMA, false }, { '.', FK_PERIOD, false }, { '/', FK_SLASH, false },
    { '_', FK_UNDERSCORE, false }, { '\\', FK_YEN, false },
    { '!', FK_1, true }, { '"', FK_2, true }, { '#', FK_3, true }, { '$', FK_4, true },
    { '%', FK_5, true }, { '&', FK_6, true }, { '\'', FK_7, true }, { '(', FK_8, true },
    { ')', FK_9, true }, { '=', FK_MINUS, true }, { '+', FK_SEMICOLON, true },
    { '*', FK_COLON, true }, { '<', FK_COMMA, true }, { '>', FK_PERIOD, true },
    { '?', FK_SLASH, true },
  };
  *shift = false;
  if (c >= 'a' && c <= 'z')
    return letters[c - 'a'];
  if (c >= 'A' && c <= 'Z')
    return letters[c - 'A'];
  if (c >= '0' && c <= '9')
    return digits[c - '0'];
  for (auto const & o : others)
    if (o.c == c) {
      *shift = o.shift;
      return o.key;
    }
  return FK_NONE;
}
