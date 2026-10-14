// Cheat codes: Game Genie (patches what the CPU reads from ROM) and Action Replay (pokes RAM).
#pragma once

#include "platform.h"

struct Cheat {
  bool     rom;          // Game Genie: a ROM patch at $8000-$FFFF, else Action Replay: a RAM poke
  uint16_t addr;
  uint8_t  value;
  int16_t  compare;      // Game Genie 8 letter codes: patch only where ROM holds this, else -1
};

// Decodes a code as typed. Accepted:
//   Game Genie   6 or 8 letters of APZLGITYEOXUKSVN, e.g. SXIOPO
//   Action Replay  address and value in hex: AAAA:VV, AAAAVV, or the 8 digit 00AAAAVV form,
//                  for addresses below $8000 (RAM, save RAM)
// Spaces and dashes are ignored, letters may be lower case. Returns false if it is neither.
bool decodeCheat(char const * text, Cheat * out);

// The code in a canonical form for display and saving (upper case, no separators; AR as AAAA:VV).
void formatCheat(Cheat const & c, char const * typed, char * out, int outSize);
