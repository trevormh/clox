#ifndef clox_common_h
#define clox_common_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// https://craftinginterpreters.com/optimization.html#conditional-support
#define NAN_BOXING

// #define DEBUG_PRINT_CODE
// #define DEBUG_TRACE_EXECUTION

// garbage collector debug flags
// "stress test" mode for the garbage collector
// when enabled, the GC runs as often as it possibly can
// #define DEBUG_STRESS_GC
// #define DEBUG_LOG_GC

#define UINT8_COUNT (UINT8_MAX + 1)

#endif
