// Force-included into the game code by the native build (game/CMakeLists.txt).
//
// The decomp's own C library headers (CodeWarrior's MSL) sit behind
// -idirafter so Apple's win. This declares the MSL extras the game relies on
// that Apple's headers don't have.
#ifndef FRONTLINE_MSL_COMPAT_H
#define FRONTLINE_MSL_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

// Defined by the game itself (ea/sys/stricmp.cpp).
int stricmp(const char* a, const char* b);

#ifdef __cplusplus
}
#endif

// MSL names its stdio struct _FILE; Apple's is struct __sFILE.
#define _FILE __sFILE

// EA's random() (ea/sys/random.cpp) has the C library's name but another
// type. Every game reference becomes EA_random, after the system header has
// declared its own.
#include <stdlib.h>
#define random EA_random

// CodeWarrior's rand() and qsort() (tools/gen_sources.py compiles them): the
// game's random sequences and sort orders depend on them.
#define rand MSL_rand
#define srand MSL_srand
#define qsort MSL_qsort
#ifdef __cplusplus
extern "C" {
#endif
int MSL_rand(void);
void MSL_srand(unsigned int seed);
void MSL_qsort(void* base, size_t count, size_t size, int (*compare)(const void*, const void*));
#ifdef __cplusplus
}
#endif

#endif
