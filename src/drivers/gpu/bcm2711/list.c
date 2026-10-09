/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoding of a BCM2711 compositor display list.
 *
 * The compositor composes each output channel from a display list: a run of
 * planes in the compositor's own list memory, each plane a few 32-bit words
 * that start with a control word, and the run closed by an end word.  This
 * file reads a list and prepares its unchanged words for relocation.  It
 * touches no register and allocates nothing, so the host test drives it
 * with ordinary arrays.
 *
 * The word layout is the BCM2711 compositor's (the fifth generation of the
 * VideoCore compositor); the bit positions are hardware facts.
 */

#include <stdbool.h>
#include <stdint.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/* The control word: set on the word that closes the list. */
#define LIST_CONTROL_END		0x80000000U

/* The control word: set on the first word of every plane. */
#define LIST_CONTROL_VALID		0x40000000U

/* The control word: the plane's length in words, bits 29:24. */
#define LIST_CONTROL_WORDS_SHIFT	24U
#define LIST_CONTROL_WORDS_MASK		0x3fU

/* The control word: set when the plane is shown at its own size. */
#define LIST_CONTROL_UNSCALED		0x00008000U

/* The control word: the order of the colour components, bits 14:13. */
#define LIST_CONTROL_ORDER_SHIFT	13U
#define LIST_CONTROL_ORDER_MASK		0x3U

/* The control word: the pixel format, bits 4:0. */
#define LIST_CONTROL_FORMAT_MASK	0x1fU

/* The last pixel format that keeps all colour in one plane of memory. */
#define LIST_FORMAT_LAST_SINGLE		7U

/* The position word: x in bits 13:0, y in bits 27:16. */
#define LIST_POSITION_X_MASK		0x3fffU
#define LIST_POSITION_Y_SHIFT		16U
#define LIST_POSITION_Y_MASK		0xfffU

/* The source size word: width in bits 12:0, height in bits 28:16. */
#define LIST_SIZE_WIDTH_MASK		0x1fffU
#define LIST_SIZE_HEIGHT_SHIFT		16U
#define LIST_SIZE_HEIGHT_MASK		0x1fffU

/* The pitch word of a single-plane format: bytes per line, bits 15:0. */
#define LIST_PITCH_MASK			0xffffU

/*
 * Where the words of an unscaled plane sit, counted from its control word.
 * A scaled plane inserts one word (its output size) after the alpha word, so
 * every later word moves down by one.
 */
#define LIST_WORD_POSITION		1U
#define LIST_WORD_SOURCE_SIZE		3U
#define LIST_WORD_POINTER		5U
#define LIST_WORD_PITCH			7U

/* The shortest unscaled plane: control, position, alpha, size, context, pointer, context, pitch. */
#define LIST_PLANE_MIN_WORDS		8U

/* The most planes one list may hold before the walk calls it malformed. */
#define LIST_PLANE_LIMIT		256U

static void decode_plane(const volatile uint32_t *memory, uint32_t first, uint32_t words, struct bcm2711_list_plane *plane);
static uint32_t choose_copy_start(uint32_t words, uint32_t source, const struct bcm2711_list_range *reserved, unsigned reserved_count);

/*
 * Decodes the display list that starts at a word of the list memory.
 *
 * memory is the list memory, BCM2711_LIST_WORDS words long.  The walk follows
 * the planes' lengths to the end word; it stops and leaves valid false at a
 * word that is neither a plane nor the end, at a plane that would run past
 * the memory, and after LIST_PLANE_LIMIT planes.
 */
void
bcm2711_list_decode(
	const volatile uint32_t *memory,
	uint32_t start,
	struct bcm2711_list *list)
{
	uint32_t index;
	uint32_t control;
	uint32_t words;

	/* Starts from an empty, invalid list. */
	list->valid = false;
	list->start = start;
	list->end = start;
	list->plane_count = 0;

	/* Walks the planes until the end word. */
	index = start;
	while (index < BCM2711_LIST_WORDS) {
		/* Ends the walk at the word that closes the list. */
		control = memory[index];
		if ((control & LIST_CONTROL_END) != 0) {
			list->end = index;
			list->valid = true;
			return;
		}

		/* Refuses a word that does not start a plane. */
		if ((control & LIST_CONTROL_VALID) == 0)
			return;

		/* Refuses a plane with no length or one that runs past the memory. */
		words = (control >> LIST_CONTROL_WORDS_SHIFT) & LIST_CONTROL_WORDS_MASK;
		if (words == 0)
			return;
		if (words > BCM2711_LIST_WORDS - index)
			return;

		/* Refuses a list with more planes than any real one has. */
		if (list->plane_count == LIST_PLANE_LIMIT)
			return;

		/* Keeps the first planes in full; the rest are only counted. */
		if (list->plane_count < BCM2711_LIST_PLANES)
			decode_plane(memory, index, words, &list->planes[list->plane_count]);

		/* Moves past the plane. */
		list->plane_count++;
		index += words;
	}
}

/*
 * Prepares an unchanged display list at an unoccupied SRAM position.
 *
 * snapshot contains BCM2711_LIST_WORDS stable words.  reserved must describe
 * every other current and pending list, filter table and firmware-owned
 * region; this routine cannot infer those reservations from one list.
 * The source list, including its end marker, is protected automatically.
 *
 * image is separate caller-owned storage, never the hardware SRAM or part
 * of snapshot.  On refusal it remains unchanged and copy has zero words.
 * On success its raw words preserve opaque plane fields and filter pointers.
 * The caller must revalidate firmware ownership before any hardware write.
 */
bool
bcm2711_list_copy_prepare(
	const uint32_t *snapshot,
	uint32_t source,
	const struct bcm2711_list_range *reserved,
	unsigned reserved_count,
	uint32_t *image,
	uint32_t image_words,
	struct bcm2711_list_copy *copy)
{
	struct bcm2711_list list;
	uint32_t words;
	uint32_t destination;
	uint32_t index;
	unsigned reservation;

	/* Leaves no relocation that a caller could publish after a refusal. */
	copy->source = 0;
	copy->destination = 0;
	copy->words = 0;

	/* Rejects occupied intervals that cannot describe this SRAM. */
	for (reservation = 0; reservation < reserved_count; reservation++) {
		/* A reservation must contain at least one word. */
		if (reserved[reservation].words == 0)
			return false;

		/* Bounds the start before subtracting it from SRAM capacity. */
		if (reserved[reservation].first >= BCM2711_LIST_WORDS)
			return false;

		/* Refuses a length that crosses SRAM's end, including wraparound. */
		if (reserved[reservation].words >
		    BCM2711_LIST_WORDS - reserved[reservation].first)
			return false;
	}

	/* Finds the complete source run, including its closing word. */
	bcm2711_list_decode(snapshot, source, &list);
	if (!list.valid)
		return false;

	/* Keeps the closing word even when there are no planes. */
	words = list.end - list.start + 1U;
	if (words > image_words)
		return false;

	/* Finds an independent run before changing the caller's image. */
	destination = choose_copy_start(words, source, reserved, reserved_count);
	if (destination == BCM2711_LIST_WORDS)
		return false;

	/* Preserves all plane words rather than rebuilding only decoded fields. */
	for (index = 0; index < words; index++)
		image[index] = snapshot[source + index];

	/* Publishes the prepared run only after its complete image exists. */
	copy->source = source;
	copy->destination = destination;
	copy->words = words;

	/* Succeeded: the caller owns an exact image and a nonoverlapping position. */
	return true;
}

/* Decodes the words of one plane that starts at a control word. */
static void
decode_plane(
	const volatile uint32_t *memory,
	uint32_t first,
	uint32_t words,
	struct bcm2711_list_plane *plane)
{
	uint32_t control;
	uint32_t shift;
	uint32_t position;
	uint32_t size;

	/* Records what the control word says. */
	control = memory[first];
	plane->control = control;
	plane->words = words;
	plane->format = control & LIST_CONTROL_FORMAT_MASK;
	plane->order = (control >> LIST_CONTROL_ORDER_SHIFT) & LIST_CONTROL_ORDER_MASK;
	plane->x = 0;
	plane->y = 0;
	plane->width = 0;
	plane->height = 0;
	plane->pointer = 0;
	plane->pitch = 0;

	/* A scaled plane carries its output size after the alpha word. */
	if ((control & LIST_CONTROL_UNSCALED) != 0) {
		plane->scaled = false;
		shift = 0;
	} else {
		plane->scaled = true;
		shift = 1;
	}

	/* Leaves the rest empty for a plane too short to hold the usual words. */
	if (words < LIST_PLANE_MIN_WORDS + shift)
		return;

	/* Reads where the plane sits on the output. */
	position = memory[first + LIST_WORD_POSITION];
	plane->x = position & LIST_POSITION_X_MASK;
	plane->y = (position >> LIST_POSITION_Y_SHIFT) & LIST_POSITION_Y_MASK;

	/* Reads the size of the image the plane shows. */
	size = memory[first + LIST_WORD_SOURCE_SIZE + shift];
	plane->width = size & LIST_SIZE_WIDTH_MASK;
	plane->height = (size >> LIST_SIZE_HEIGHT_SHIFT) & LIST_SIZE_HEIGHT_MASK;

	/* Reads the bus address of the image's first pixel. */
	plane->pointer = memory[first + LIST_WORD_POINTER + shift];

	/* Reads the pitch, which follows one pointer only in a single-plane format. */
	if (plane->format <= LIST_FORMAT_LAST_SINGLE)
		plane->pitch = memory[first + LIST_WORD_PITCH + shift] & LIST_PITCH_MASK;
}

/* Chooses the earliest complete gap while protecting the source and reservations. */
static uint32_t
choose_copy_start(
	uint32_t words,
	uint32_t source,
	const struct bcm2711_list_range *reserved,
	unsigned reserved_count)
{
	uint32_t candidate;
	uint32_t end;
	uint32_t next;
	uint32_t occupied_end;
	unsigned reservation;

	/* Advances past intersecting intervals until a complete run fits. */
	candidate = 0;
	while (candidate <= BCM2711_LIST_WORDS - words) {
		/* Keeps the source live until hardware has accepted another list. */
		end = candidate + words;
		next = candidate;
		occupied_end = source + words;
		if (candidate < occupied_end) {
			/* A candidate ending at the source start is still disjoint. */
			if (source < end)
				next = occupied_end;
		}

		/* Handles unsorted and overlapping reservations without allocating. */
		for (reservation = 0; reservation < reserved_count; reservation++) {
			/* Ignores an interval wholly before the candidate. */
			occupied_end = reserved[reservation].first;
			occupied_end += reserved[reservation].words;
			if (candidate >= occupied_end)
				continue;

			/* Ignores an interval wholly after the candidate. */
			if (reserved[reservation].first >= end)
				continue;

			/* Moves forward, even when a later reservation starts earlier. */
			if (occupied_end > next)
				next = occupied_end;
		}

		/* A gap must fit the closing word as well as every plane word. */
		if (next == candidate)
			break;

		/* Every collision advances the walk; full SRAM terminates it. */
		candidate = next;
	}

	/* Refuses relocation when no separate complete run remains. */
	if (candidate > BCM2711_LIST_WORDS - words)
		return BCM2711_LIST_WORDS;

	/* Succeeded: the complete run is separate from all occupied intervals. */
	return candidate;
}
