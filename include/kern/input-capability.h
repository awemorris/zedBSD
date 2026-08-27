/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_INPUT_CAPABILITY_H
#define ZEDBSD_KERN_INPUT_CAPABILITY_H

#include <zedbsd/input.h>
#include <stddef.h>
#include <stdint.h>

#define INPUT_BITS_PER_WORD (sizeof(unsigned long) * 8U)
#define INPUT_BIT_WORDS(maximum)                                               \
	(((maximum) + 1U + INPUT_BITS_PER_WORD - 1U) / INPUT_BITS_PER_WORD)
#define INPUT_EVENT_BITS_SIZE                                                  \
	(INPUT_BIT_WORDS(EV_MAX) * sizeof(unsigned long))
#define INPUT_KEY_BITS_SIZE                                                    \
	(INPUT_BIT_WORDS(KEY_MAX) * sizeof(unsigned long))
#define INPUT_REL_BITS_SIZE                                                    \
	(INPUT_BIT_WORDS(REL_MAX) * sizeof(unsigned long))
#define INPUT_ABS_BITS_SIZE                                                    \
	(INPUT_BIT_WORDS(ABS_MAX) * sizeof(unsigned long))
#define INPUT_CAPABILITY_COUNT_MAX                                             \
	((EV_MAX + 1U) + (KEY_MAX + 1U) + (REL_MAX + 1U) +                 \
	 (ABS_MAX + 1U))

struct input_capability {
	uint16_t type;
	uint16_t code;
};

struct input_abs_axis {
	uint16_t code;
	struct input_absinfo info;
};

struct input_capability_state {
	unsigned long event_bits[INPUT_BIT_WORDS(EV_MAX)];
	unsigned long key_bits[INPUT_BIT_WORDS(KEY_MAX)];
	unsigned long rel_bits[INPUT_BIT_WORDS(REL_MAX)];
	unsigned long abs_bits[INPUT_BIT_WORDS(ABS_MAX)];
	unsigned long abs_configured[INPUT_BIT_WORDS(ABS_MAX)];
	unsigned long key_state[INPUT_BIT_WORDS(KEY_MAX)];
	struct input_absinfo abs_info[ABS_MAX + 1U];
};

int input_capability_state_init(struct input_capability_state *,
				const struct input_capability *, size_t,
				const struct input_abs_axis *, size_t);
int input_capability_bits(const struct input_capability_state *, unsigned,
			  const uint8_t **, size_t *);
int input_capability_key_state(const struct input_capability_state *,
			       const uint8_t **, size_t *);
int input_capability_copy(const uint8_t *, size_t, size_t, uint8_t *, size_t);
int input_capability_abs_info(const struct input_capability_state *, unsigned,
			      struct input_absinfo *);
int input_capability_event(struct input_capability_state *, uint16_t,
			   uint16_t, int32_t);

#endif
