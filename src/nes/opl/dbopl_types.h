/*
  ESPPC: the DOSBox type names and macros DBOPL uses, for building it outside DOSBox.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

typedef intptr_t  Bits;
typedef uintptr_t Bitu;

#define DB_FASTCALL
#define INLINE inline
#define GCC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define GCC_LIKELY(x)   __builtin_expect(!!(x), 1)
