// Platform glue for the emulator core. The core in src/nes/ has no ESP32 or FabGL dependencies,
// so it also builds on a PC (see host/) where it is tested against nestest and test ROMs.
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef ESP_PLATFORM          // any ESP32 build (ESP-IDF, with or without Arduino)
  #include "esp_attr.h"
  #include "xtensa/core-macros.h"
  // CPU clock cycles, for profiling
  static inline uint32_t nesClock() { return XTHAL_GET_CCOUNT(); }
  // Hot paths go to IRAM: from flash they would fight over the cache with every ROM byte fetch.
  #define NES_HOT IRAM_ATTR
  // Tables read on every pixel or instruction: internal RAM, not flash rodata.
  #define NES_FASTDATA DRAM_ATTR
#else
  #define NES_HOT
  #define NES_FASTDATA
  static inline uint32_t nesClock() { return 0; }
#endif

#define NES_LIKELY(x)   __builtin_expect(!!(x), 1)
#define NES_UNLIKELY(x) __builtin_expect(!!(x), 0)
