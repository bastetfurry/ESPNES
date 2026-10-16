// NSF: NES music files (nesdev wiki "NSF"). A 128 byte header, then the music's code and data:
//   $00 "NESM",$1A  $05 version  $06 songs  $07 first song (1 based)  $08 load  $0A init  $0C play
//   $0E title  $2E artist  $4E copyright (32 bytes each)  $6E play period NTSC (microseconds)
//   $70 initial banks for $8000-$FFFF (all 0: no bank switching)  $78 play period PAL
//   $7A bit 0: PAL, bit 1: both  $7B expansion chips (NSF_VRC6 ...)
//
// The data is loaded at the load address, or with bank switching in 4 KB banks (the load
// address's low 12 bits are the padding before it), switched by writes to $5FF8-$5FFF for
// $8000-$FFFF, and for FDS music also $5FF6-$5FF7 for $6000-$7FFF. FDS music has RAM from $6000
// to $FFFF, which it writes to.
//
// The player (Nes::nsfStart, Nes::runFrame) calls INIT with the song in A and PAL in X, then PLAY
// at the play rate. Neither has a way back, so the return address points to $4100, which reads as
// "JMP $4100": the CPU waits there between calls.
#pragma once

#include "platform.h"
#include "cart.h"

enum : uint8_t {
  NSF_VRC6 = 0x01, NSF_VRC7 = 0x02, NSF_FDS = 0x04, NSF_MMC5 = 0x08, NSF_N163 = 0x10, NSF_5B = 0x20,
};

struct NsfInfo {
  uint8_t         songs, firstSong;           // firstSong 1 based
  uint16_t        load, init, play;
  char            title[33], artist[33], copyright[33];
  uint32_t        periodNtsc, periodPal;      // microseconds between PLAY calls
  uint8_t         banks[8];
  bool            bankSwitched;
  bool            pal, dual;
  uint8_t         chips;
  uint8_t const * data;                       // after the header, in the caller's buffer
  size_t          dataSize;
};

static constexpr uint16_t NSF_IDLE = 0x4100;  // where the CPU waits between INIT/PLAY calls

bool         isNsf(uint8_t const * image, size_t size);
// Returns nullptr or an error message.
char const * parseNsf(uint8_t const * image, size_t size, NsfInfo * info);

// The NSF "mapper": $8000-$FFFF are 32 KB of RAM the banks are copied into.
Mapper *     createNsfMapper(Nes & nes, NsfInfo const & info);
