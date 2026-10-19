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
//   Game Genie     6 or 8 letters of APZLGITYEOXUKSVN, e.g. SXIOPO
//   hex            AAAA VV (a RAM poke below $8000, a ROM patch from $8000) or AAAA VV CC (a ROM
//                  patch only where the ROM holds CC); Action Replay's AAAA:VV and 00AAAAVV too
// Spaces, dashes and colons are ignored, letters may be lower case; a code typed with spaces or
// colons is read as hex first (EEEEEE is both). Returns false if it is neither.
bool decodeCheat(char const * text, Cheat * out);

// The code in the canonical hex form, for display and saving: "AAAA VV" or "AAAA VV CC".
void formatCheat(Cheat const & c, char * out, int outSize);
