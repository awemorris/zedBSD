/* XSI 48-bit linear congruential random number family. SPDX-License-Identifier: Zlib */
#include <stdint.h>
#include <stdlib.h>

#define MASK48 0xffffffffffffULL

static unsigned short global_state[3] = { 0x330e, 0xabcd, 0x1234 };
static unsigned short saved_state[3];
static uint64_t multiplier = 0x5deece66dULL;
static unsigned short addend = 0x000b;
static volatile unsigned global_lock;

static void
lock_state(void)
{
	while (__atomic_exchange_n(&global_lock, 1U, __ATOMIC_ACQUIRE) != 0)
		;
}

static void
unlock_state(void)
{
	__atomic_store_n(&global_lock, 0U, __ATOMIC_RELEASE);
}

static uint64_t
unpack(const unsigned short state[3])
{
	return (uint64_t)state[0] | ((uint64_t)state[1] << 16) |
	    ((uint64_t)state[2] << 32);
}

static void
pack(unsigned short state[3], uint64_t value)
{
	state[0] = (unsigned short)value;
	state[1] = (unsigned short)(value >> 16);
	state[2] = (unsigned short)(value >> 32);
}

static uint64_t
step(unsigned short state[3])
{
	uint64_t value = (unpack(state) * multiplier + addend) & MASK48;
	pack(state, value);
	return value;
}

double
erand48(unsigned short state[3])
{
	return (double)step(state) / 281474976710656.0;
}

long
nrand48(unsigned short state[3])
{
	return (long)(step(state) >> 17);
}

long
jrand48(unsigned short state[3])
{
	return (long)(int32_t)(step(state) >> 16);
}

double
drand48(void)
{
	double result;
	lock_state();
	result = erand48(global_state);
	unlock_state();
	return result;
}

long
lrand48(void)
{
	long result;
	lock_state();
	result = nrand48(global_state);
	unlock_state();
	return result;
}

long
mrand48(void)
{
	long result;
	lock_state();
	result = jrand48(global_state);
	unlock_state();
	return result;
}

void
srand48(long seed)
{
	lock_state();
	global_state[0] = 0x330e;
	global_state[1] = (unsigned short)seed;
	global_state[2] = (unsigned short)((unsigned long)seed >> 16);
	multiplier = 0x5deece66dULL;
	addend = 0x000b;
	unlock_state();
}

unsigned short *
seed48(unsigned short seed[3])
{
	lock_state();
	saved_state[0] = global_state[0];
	saved_state[1] = global_state[1];
	saved_state[2] = global_state[2];
	global_state[0] = seed[0];
	global_state[1] = seed[1];
	global_state[2] = seed[2];
	multiplier = 0x5deece66dULL;
	addend = 0x000b;
	unlock_state();
	return saved_state;
}

void
lcong48(unsigned short parameters[7])
{
	lock_state();
	global_state[0] = parameters[0];
	global_state[1] = parameters[1];
	global_state[2] = parameters[2];
	multiplier = unpack(parameters + 3);
	addend = parameters[6];
	unlock_state();
}
