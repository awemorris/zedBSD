/* Minimal musl libm atomic compatibility for the freestanding build. */
#ifndef ZEDBSD_MUSL_ATOMIC_H
#define ZEDBSD_MUSL_ATOMIC_H

#include <stdint.h>

static __inline int
a_clz_64(uint64_t value)
{
	return value != 0 ? __builtin_clzll(value) : 64;
}

#endif
