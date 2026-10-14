// Platform glue for the emulator core. The core in src/nes/ has no ESP32 or FabGL dependencies,
// so it also builds on a PC (see host/) where it is tested against nestest and test ROMs.
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef ARDUINO
  #include "esp_attr.h"
  // Hot paths go to IRAM: from flash they would fight over the cache with every ROM byte fetch.
  #define NES_HOT IRAM_ATTR
  // Tables read on every pixel or instruction: internal RAM, not flash rodata.
  #define NES_FASTDATA DRAM_ATTR
#else
  #define NES_HOT
  #define NES_FASTDATA
#endif

#define NES_LIKELY(x)   __builtin_expect(!!(x), 1)
#define NES_UNLIKELY(x) __builtin_expect(!!(x), 0)
