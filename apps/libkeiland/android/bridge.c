/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The queue between Java's threads and the native thread, and the UTF-16 and
 * UTF-8 conversions (see bridge.h).  The queue is a singly linked list of
 * events in memory from malloc, with a head, a tail and one lock.
 */

#include "apps/libkeiland/android/bridge.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* The code point that stands for text that could not be converted. */
#define BRIDGE_REPLACEMENT	0xFFFDU

/* The queue's two ends, and the lock that guards them. */
static struct keiland_bridge_event *bridge_head;
static struct keiland_bridge_event *bridge_tail;
static pthread_mutex_t bridge_lock = PTHREAD_MUTEX_INITIALIZER;

/* The function that wakes M, or NULL. */
static void (*bridge_wake)(void);

/* How many events were dropped for lack of memory; changed under the lock. */
static unsigned bridge_dropped;

static void bridge_drop(void);
static size_t bridge_utf8_decode(const char *in, size_t length, uint32_t *code_point);
static size_t bridge_utf8_encode(uint32_t code_point, char *out);

/*
 * Sets the function that wakes M after an event is pushed.
 */
void
keiland_bridge_set_wake(
	void (*wake)(void))
{
	/* Remembers the function for the pushes that follow. */
	bridge_wake = wake;
}

/*
 * Appends a copy of the event and of its text to the queue and wakes M.
 */
int
keiland_bridge_push(
	const struct keiland_bridge_event *event,
	const char *text,
	size_t length)
{
	struct keiland_bridge_event *copy;

	/* Refuses an event that is missing, or text of which only the length is known. */
	if (event == NULL)
		return EINVAL;
	if (text == NULL && length > 0)
		return EINVAL;

	/* Allocates the copy of the event. */
	copy = malloc(sizeof(*copy));
	if (copy == NULL) {
		bridge_drop();

		return ENOMEM;
	}

	/* Fills the copy; the text is added below. */
	*copy = *event;
	copy->next = NULL;
	copy->text = NULL;

	/* Copies the text with a NUL after it. */
	if (text != NULL) {
		copy->text = malloc(length + 1);
		if (copy->text == NULL) {
			free(copy);
			bridge_drop();

			return ENOMEM;
		}

		memcpy(copy->text, text, length);
		copy->text[length] = '\0';
	}

	/* Appends the copy after the newest event. */
	pthread_mutex_lock(&bridge_lock);

	if (bridge_tail == NULL)
		bridge_head = copy;
	else
		bridge_tail->next = copy;
	bridge_tail = copy;

	pthread_mutex_unlock(&bridge_lock);

	/* Wakes M, which takes the event on its next look at the queue. */
	if (bridge_wake != NULL)
		bridge_wake();

	/* Succeeded: the event is queued. */
	return 0;
}

/*
 * Takes every queued event, oldest first.
 */
struct keiland_bridge_event *
keiland_bridge_take_all(void)
{
	struct keiland_bridge_event *list;

	/* Detaches the whole list under the lock. */
	pthread_mutex_lock(&bridge_lock);

	list = bridge_head;
	bridge_head = NULL;
	bridge_tail = NULL;

	pthread_mutex_unlock(&bridge_lock);

	/* Succeeded: the events in the order they were pushed. */
	return list;
}

/*
 * Frees a list of events with their texts.
 */
void
keiland_bridge_free(
	struct keiland_bridge_event *list)
{
	struct keiland_bridge_event *next;

	/* Frees each event after its text. */
	while (list != NULL) {
		next = list->next;
		free(list->text);
		free(list);
		list = next;
	}
}

/*
 * Returns how many events were dropped.
 */
unsigned
keiland_bridge_dropped(void)
{
	unsigned count;

	/* Reads the count under the lock that changes it. */
	pthread_mutex_lock(&bridge_lock);

	count = bridge_dropped;

	pthread_mutex_unlock(&bridge_lock);

	/* Succeeded: the number dropped so far. */
	return count;
}

/*
 * Converts UTF-16 to UTF-8.
 */
size_t
keiland_utf16_to_utf8(
	const uint16_t *in,
	size_t count,
	char *out,
	size_t size)
{
	char encoded[4];
	size_t used;
	size_t i;
	size_t length;
	uint32_t code_point;
	uint32_t low;

	/* Walks the text one character at a time. */
	used = 0;
	i = 0;
	while (i < count) {
		code_point = in[i];
		i++;

		/* A high surrogate followed by a low one makes one character; any other surrogate is unpaired. */
		if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
			if (i < count && in[i] >= 0xDC00U && in[i] <= 0xDFFFU) {
				low = in[i];
				i++;
				code_point = 0x10000U + ((code_point - 0xD800U) << 10) + (low - 0xDC00U);
			} else {
				code_point = BRIDGE_REPLACEMENT;
			}
		} else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
			code_point = BRIDGE_REPLACEMENT;
		}

		/* Encodes the character, and counts it. */
		length = bridge_utf8_encode(code_point, encoded);

		/* Without a buffer, only the size the whole text needs is wanted. */
		if (out == NULL) {
			used += length;
			continue;
		}

		/* Stops before a character that does not fit with the NUL after it. */
		if (used + length + 1 > size)
			break;

		memcpy(out + used, encoded, length);
		used += length;
	}

	/* Ends the text. */
	if (out != NULL && size > 0)
		out[used] = '\0';

	/* Succeeded: the bytes written, or needed. */
	return used;
}

/*
 * Converts UTF-8 to UTF-16.
 */
size_t
keiland_utf8_to_utf16(
	const char *in,
	size_t length,
	uint16_t *out,
	size_t count)
{
	size_t used;
	size_t i;
	size_t consumed;
	size_t units;
	uint32_t code_point;

	/* Walks the text one character at a time. */
	used = 0;
	i = 0;
	while (i < length) {
		consumed = bridge_utf8_decode(in + i, length - i, &code_point);
		i += consumed;

		/* One unit, or two for a character above U+FFFF. */
		units = 1;
		if (code_point >= 0x10000U)
			units = 2;

		/* Without a buffer, only the size the whole text needs is wanted. */
		if (out == NULL) {
			used += units;
			continue;
		}

		/* Stops before a character that does not fit. */
		if (used + units > count)
			break;

		if (units == 1) {
			out[used] = (uint16_t)code_point;
		} else {
			code_point -= 0x10000U;
			out[used] = (uint16_t)(0xD800U + (code_point >> 10));
			out[used + 1] = (uint16_t)(0xDC00U + (code_point & 0x3FFU));
		}
		used += units;
	}

	/* Succeeded: the units written, or needed. */
	return used;
}

/*
 * Counts an event that was dropped.
 */
static void
bridge_drop(void)
{
	/* Adds one under the lock. */
	pthread_mutex_lock(&bridge_lock);

	bridge_dropped++;

	pthread_mutex_unlock(&bridge_lock);
}

/*
 * Decodes one character of UTF-8.  Returns how many bytes it used, at least
 * 1; bytes that are not valid UTF-8 (a stray byte, an overlong form, a
 * surrogate, a value above U+10FFFF, a cut-off sequence) give U+FFFD and use
 * the bytes up to where the sequence stopped being valid.
 */
static size_t
bridge_utf8_decode(
	const char *in,
	size_t length,
	uint32_t *code_point)
{
	const unsigned char *bytes;
	uint32_t value;
	uint32_t minimum;
	size_t needed;
	size_t i;

	bytes = (const unsigned char *)in;

	/* One byte for ASCII. */
	if (bytes[0] < 0x80U) {
		*code_point = bytes[0];

		return 1;
	}

	/* The lead byte says how many bytes follow, and what the value cannot be below. */
	if (bytes[0] >= 0xC2U && bytes[0] <= 0xDFU) {
		needed = 1;
		value = bytes[0] & 0x1FU;
		minimum = 0x80U;
	} else if (bytes[0] >= 0xE0U && bytes[0] <= 0xEFU) {
		needed = 2;
		value = bytes[0] & 0x0FU;
		minimum = 0x800U;
	} else if (bytes[0] >= 0xF0U && bytes[0] <= 0xF4U) {
		needed = 3;
		value = bytes[0] & 0x07U;
		minimum = 0x10000U;
	} else {
		*code_point = BRIDGE_REPLACEMENT;

		return 1;
	}

	/* Reads the continuation bytes, stopping at the first one that is missing or wrong. */
	for (i = 1;
	     i <= needed;
	     i++) {
		if (i >= length || (bytes[i] & 0xC0U) != 0x80U) {
			*code_point = BRIDGE_REPLACEMENT;

			return i;
		}
		value = (value << 6) | (bytes[i] & 0x3FU);
	}

	/* Refuses an overlong form, a surrogate and a value above the last code point. */
	if (value < minimum || (value >= 0xD800U && value <= 0xDFFFU) || value > 0x10FFFFU) {
		*code_point = BRIDGE_REPLACEMENT;

		return needed + 1;
	}

	/* Succeeded: the character and the bytes it took. */
	*code_point = value;

	return needed + 1;
}

/*
 * Encodes one character as UTF-8 into out (4 bytes at most).  Returns the
 * number of bytes.
 */
static size_t
bridge_utf8_encode(
	uint32_t code_point,
	char *out)
{
	/* One byte for ASCII. */
	if (code_point < 0x80U) {
		out[0] = (char)code_point;

		return 1;
	}

	/* Two bytes up to U+07FF. */
	if (code_point < 0x800U) {
		out[0] = (char)(0xC0U | (code_point >> 6));
		out[1] = (char)(0x80U | (code_point & 0x3FU));

		return 2;
	}

	/* Three bytes up to U+FFFF. */
	if (code_point < 0x10000U) {
		out[0] = (char)(0xE0U | (code_point >> 12));
		out[1] = (char)(0x80U | ((code_point >> 6) & 0x3FU));
		out[2] = (char)(0x80U | (code_point & 0x3FU));

		return 3;
	}

	/* Four bytes above that. */
	out[0] = (char)(0xF0U | (code_point >> 18));
	out[1] = (char)(0x80U | ((code_point >> 12) & 0x3FU));
	out[2] = (char)(0x80U | ((code_point >> 6) & 0x3FU));
	out[3] = (char)(0x80U | (code_point & 0x3FU));

	return 4;
}
