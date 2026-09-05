/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/input-capability.h"

#include <errno.h>
#include <string.h>

static int
bit_test(const unsigned long *bits, unsigned bit)
{
	return (bits[bit / INPUT_BITS_PER_WORD] &
		(1UL << (bit % INPUT_BITS_PER_WORD))) != 0;
}

static void
bit_set(unsigned long *bits, unsigned bit)
{
	bits[bit / INPUT_BITS_PER_WORD] |=
	    1UL << (bit % INPUT_BITS_PER_WORD);
}

static void
bit_clear(unsigned long *bits, unsigned bit)
{
	bits[bit / INPUT_BITS_PER_WORD] &=
	    ~(1UL << (bit % INPUT_BITS_PER_WORD));
}

static int
capability_bits_mutable(struct input_capability_state *state, unsigned type,
			unsigned long **bits, size_t *size)
{
	if (state == NULL || bits == NULL || size == NULL)
		return EINVAL;
	switch (type) {
	case EV_KEY:
		*bits = state->key_bits;
		*size = sizeof(state->key_bits);
		return 0;
	case EV_REL:
		*bits = state->rel_bits;
		*size = sizeof(state->rel_bits);
		return 0;
	case EV_ABS:
		*bits = state->abs_bits;
		*size = sizeof(state->abs_bits);
		return 0;
	default:
		return EINVAL;
	}
}

static int
capability_code_valid(unsigned type, unsigned code)
{
	switch (type) {
	case EV_SYN: return code == SYN_REPORT;
	case EV_KEY: return code <= KEY_MAX;
	case EV_REL: return code <= REL_MAX;
	case EV_ABS: return code <= ABS_MAX;
	default: return 0;
	}
}

int
input_capability_state_init(struct input_capability_state *state,
			    const struct input_capability *capabilities,
			    size_t capability_count,
			    const struct input_abs_axis *absolute_axes,
			    size_t absolute_axis_count)
{
	size_t i;
	if (state == NULL ||
	    (capability_count != 0 && capabilities == NULL) ||
	    (absolute_axis_count != 0 && absolute_axes == NULL) ||
	    capability_count > INPUT_CAPABILITY_COUNT_MAX ||
	    absolute_axis_count > ABS_MAX + 1U)
		return EINVAL;
	memset(state, 0, sizeof(*state));
	for (i = 0; i < capability_count; i++) {
		const struct input_capability *capability = &capabilities[i];
		unsigned long *bits;
		size_t size;
		if (!capability_code_valid(capability->type, capability->code))
			return EINVAL;
		if (capability->type == EV_SYN) {
			if (bit_test(state->event_bits, EV_SYN))
				return EINVAL;
			bit_set(state->event_bits, EV_SYN);
			continue;
		}
		if (capability_bits_mutable(state, capability->type, &bits,
					    &size) != 0 ||
		    capability->code >= size * 8U ||
		    bit_test(bits, capability->code))
			return EINVAL;
		bit_set(state->event_bits, capability->type);
		bit_set(bits, capability->code);
	}
	for (i = 0; i < absolute_axis_count; i++) {
		const struct input_abs_axis *axis = &absolute_axes[i];
		const struct input_absinfo *info = &axis->info;
		if (axis->code > ABS_MAX ||
		    !bit_test(state->abs_bits, axis->code) ||
		    bit_test(state->abs_configured, axis->code) ||
		    info->minimum > info->maximum || info->value < info->minimum ||
		    info->value > info->maximum || info->fuzz < 0 || info->flat < 0 ||
		    info->resolution < 0)
			return EINVAL;
		state->abs_info[axis->code] = *info;
		bit_set(state->abs_configured, axis->code);
	}
	for (i = 0; i <= ABS_MAX; i++)
		if (bit_test(state->abs_bits, (unsigned)i) &&
		    !bit_test(state->abs_configured, (unsigned)i))
			return EINVAL;
	if (!bit_test(state->event_bits, EV_SYN))
		return EINVAL;
	return 0;
}

int
input_capability_bits(const struct input_capability_state *state,
		      unsigned type, const uint8_t **bits, size_t *size)
{
	if (state == NULL || bits == NULL || size == NULL)
		return EINVAL;
	if (type == EV_SYN) {
		*bits = (const uint8_t *)state->event_bits;
		*size = sizeof(state->event_bits);
		return 0;
	}
	switch (type) {
	case EV_KEY:
		*bits = (const uint8_t *)state->key_bits;
		*size = sizeof(state->key_bits);
		return 0;
	case EV_REL:
		*bits = (const uint8_t *)state->rel_bits;
		*size = sizeof(state->rel_bits);
		return 0;
	case EV_ABS:
		*bits = (const uint8_t *)state->abs_bits;
		*size = sizeof(state->abs_bits);
		return 0;
	default:
		return EINVAL;
	}
}

int
input_capability_key_state(const struct input_capability_state *state,
			   const uint8_t **bits, size_t *size)
{
	if (state == NULL || bits == NULL || size == NULL)
		return EINVAL;
	*bits = (const uint8_t *)state->key_state;
	*size = sizeof(state->key_state);
	return 0;
}

int
input_capability_copy(const uint8_t *source, size_t source_size, size_t offset,
		      uint8_t *destination, size_t capacity)
{
	size_t i;
	if ((source_size != 0 && source == NULL) ||
	    (capacity != 0 && destination == NULL) ||
	    capacity > SIZE_MAX - offset)
		return EINVAL;
	for (i = 0; i < capacity; i++) {
		size_t index = offset + i;
		destination[i] = index < source_size ? source[index] : 0;
	}
	return 0;
}

int
input_capability_abs_info(const struct input_capability_state *state,
			  unsigned axis, struct input_absinfo *info)
{
	if (state == NULL || info == NULL || axis > ABS_MAX)
		return EINVAL;
	if (!bit_test(state->abs_bits, axis))
		return ENOENT;
	*info = state->abs_info[axis];
	return 0;
}

int
input_capability_event(struct input_capability_state *state, uint16_t type,
		       uint16_t code, int32_t value)
{
	if (state == NULL)
		return 0;
	if (type == EV_SYN)
		return code == SYN_REPORT && value == 0 &&
		       bit_test(state->event_bits, EV_SYN);
	if (type == EV_KEY && code <= KEY_MAX &&
	    bit_test(state->key_bits, code)) {
		if (value == 0)
			bit_clear(state->key_state, code);
		else if (value == 1 || value == 2)
			bit_set(state->key_state, code);
		else
			return 0;
		return 1;
	} else if (type == EV_REL && code <= REL_MAX &&
		   bit_test(state->rel_bits, code)) {
		return 1;
	} else if (type == EV_ABS && code <= ABS_MAX &&
		   bit_test(state->abs_bits, code)) {
		state->abs_info[code].value = value;
		return 1;
	}
	return 0;
}
