/* XSI/BSD additive-feedback random generator and radix-64 conversion. SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(ZEDBSD_DYNAMIC_LIBC)
#define RANDOM_THREAD_LOCAL _Thread_local
#else
#define RANDOM_THREAD_LOCAL
#endif

enum { TYPE_0 = 0, TYPE_1, TYPE_2, TYPE_3, TYPE_4 };
static const int degrees[] = { 0, 7, 15, 31, 63 };
static const int separations[] = { 0, 3, 1, 3, 1 };
static int32_t default_state[32];
static int32_t *state = default_state + 1;
static int type = TYPE_3, degree = 31, separation = 3;
static int front = 3, rear;
static int initialized;
static volatile unsigned state_lock;

static void lock_random(void) { while (__atomic_exchange_n(&state_lock, 1U, __ATOMIC_ACQUIRE)); }
static void unlock_random(void) { __atomic_store_n(&state_lock, 0U, __ATOMIC_RELEASE); }

static long
next_value(void)
{
	uint32_t value;
	if (type == TYPE_0) {
		state[0] = (int32_t)((1103515245U * (uint32_t)state[0] + 12345U) & 0x7fffffffU);
		return state[0];
	}
	value = (uint32_t)state[front] + (uint32_t)state[rear];
	state[front] = (int32_t)value;
	if (++front >= degree) front = 0;
	if (++rear >= degree) rear = 0;
	return (long)((value >> 1) & 0x7fffffffU);
}

static void
seed_state(unsigned int seed)
{
	int index;
	if (seed == 0) seed = 1;
	state[0] = (int32_t)seed;
	if (type != TYPE_0) {
		for (index = 1; index < degree; index++)
			state[index] = (int32_t)((16807ULL * (uint32_t)state[index - 1]) % 2147483647ULL);
		front = separation;
		rear = 0;
		for (index = 0; index < degree * 10; index++) (void)next_value();
	}
}

static void
initialize(void)
{
	if (!initialized) {
		default_state[0] = TYPE_3;
		seed_state(1);
		initialized = 1;
	}
}

long
random(void)
{
	long result;
	lock_random();
	initialize();
	result = next_value();
	unlock_random();
	return result;
}

void
srandom(unsigned int seed)
{
	lock_random();
	initialize();
	seed_state(seed);
	unlock_random();
}

static int
type_for_size(size_t size)
{
	if (size < 8) return -1;
	if (size < 32) return TYPE_0;
	if (size < 64) return TYPE_1;
	if (size < 128) return TYPE_2;
	if (size < 256) return TYPE_3;
	return TYPE_4;
}

static void
save_header(void)
{
	state[-1] = type == TYPE_0 ? TYPE_0 : type + 5 * rear;
}

char *
initstate(unsigned int seed, char *buffer, size_t size)
{
	int new_type = type_for_size(size);
	char *old;
	if (buffer == NULL || new_type < 0) { errno = EINVAL; return NULL; }
	lock_random();
	initialize();
	save_header();
	old = (char *)(state - 1);
	type = new_type;
	degree = degrees[type];
	separation = separations[type];
	state = (int32_t *)buffer + 1;
	state[-1] = type;
	front = separation;
	rear = 0;
	seed_state(seed);
	unlock_random();
	return old;
}

char *
setstate(char *buffer)
{
	int32_t header;
	int new_type, new_rear;
	char *old;
	if (buffer == NULL) { errno = EINVAL; return NULL; }
	header = *(int32_t *)buffer;
	new_type = header % 5;
	new_rear = header / 5;
	if (new_type < TYPE_0 || new_type > TYPE_4 ||
	    (new_type != TYPE_0 && (new_rear < 0 || new_rear >= degrees[new_type]))) {
		errno = EINVAL;
		return NULL;
	}
	lock_random();
	initialize();
	save_header();
	old = (char *)(state - 1);
	type = new_type;
	degree = degrees[type];
	separation = separations[type];
	state = (int32_t *)buffer + 1;
	rear = type == TYPE_0 ? 0 : new_rear;
	front = type == TYPE_0 ? 0 : (rear + separation) % degree;
	unlock_random();
	return old;
}

long
a64l(const char *text)
{
	unsigned long result = 0;
	unsigned shift;
	for (shift = 0; shift < 36 && text != NULL && *text != '\0'; shift += 6, text++) {
		unsigned value;
		if (*text == '.' || *text == '/') value = (unsigned)(*text - '.');
		else if (*text >= '0' && *text <= '9') value = (unsigned)(*text - '0') + 2;
		else if (*text >= 'A' && *text <= 'Z') value = (unsigned)(*text - 'A') + 12;
		else if (*text >= 'a' && *text <= 'z') value = (unsigned)(*text - 'a') + 38;
		else break;
		result |= (unsigned long)value << shift;
	}
	return (long)result;
}

char *
l64a(long value)
{
	static RANDOM_THREAD_LOCAL char output[7];
	static const char alphabet[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	unsigned long bits = (unsigned long)value;
	unsigned index = 0;
	while (bits != 0 && index < 6) {
		output[index++] = alphabet[bits & 63U];
		bits >>= 6;
	}
	output[index] = '\0';
	return output;
}
