/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Prints the V3D binning sequences one encoder (native-bin.c or
 * native71-bin.c, chosen at compile time) makes for a set of draws, and
 * attribute records of native-shader.c, as hex lines that
 * bin-draw-host-test.py decodes with Mesa's packet description.  Refused
 * draws are checked here.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/native-bin.h"
#include "drivers/gpu/bcm2711/native-state.h"

static void base_state(struct bcm2711_native_bin *state);
static void print_bytes(const char *name, const uint8_t *bytes, size_t count);
static void print_draw(const char *name, const struct bcm2711_native_bin *state);

int
main(void)
{
	struct bcm2711_native_bin state;
	struct bcm2711_native_attribute attribute;
	uint8_t bytes[BCM2711_NATIVE_BIN_BYTES];
	int error;

	/* A single-instance vertex array from vertex 7. */
	base_state(&state);
	state.vertices = 36;
	state.instances = 1;
	state.first = 7;
	print_draw("array", &state);

	/* An instanced vertex array with a first instance. */
	base_state(&state);
	state.vertices = 3;
	state.instances = 5000;
	state.base_instance = 3;
	print_draw("array-instanced", &state);

	/* A single-instance 32-bit indexed list from index 12 with vertex offset -2. */
	base_state(&state);
	state.indexed = 1;
	state.vertices = 300;
	state.instances = 1;
	state.first = 48;
	state.base_vertex = 0xfffffffeU;
	state.index_address = 0x10000;
	state.index_bytes = 4096;
	state.index_size = 4;
	print_draw("indexed32", &state);

	/* An instanced 16-bit indexed list. */
	base_state(&state);
	state.indexed = 1;
	state.vertices = 1000;
	state.instances = 6500;
	state.first = 4;
	state.base_instance = 1;
	state.index_address = 0x20002;
	state.index_bytes = 2004;
	state.index_size = 2;
	print_draw("indexed16-instanced", &state);

	/* Refused: no instance, a misaligned first index, indices past the interval, a wrong index size. */
	base_state(&state);
	state.instances = 0;
	error = bcm2711_native_bin_encode(&state, bytes, sizeof(bytes));
	assert(error == EINVAL);
	base_state(&state);
	state.indexed = 1;
	state.index_address = 0x10000;
	state.index_bytes = 4096;
	state.index_size = 4;
	state.first = 2;
	error = bcm2711_native_bin_encode(&state, bytes, sizeof(bytes));
	assert(error == EINVAL);
	state.first = 4096 - 4 * 35;
	error = bcm2711_native_bin_encode(&state, bytes, sizeof(bytes));
	assert(error == EINVAL);
	state.first = 4096 - 4 * 36;
	error = bcm2711_native_bin_encode(&state, bytes, sizeof(bytes));
	assert(error == 0);
	state.index_size = 1;
	error = bcm2711_native_bin_encode(&state, bytes, sizeof(bytes));
	assert(error == EINVAL);

	/* Attribute records: per vertex, and per instance with divisor one. */
	memset(&attribute, 0, sizeof(attribute));
	attribute.address = 0x123450;
	attribute.stride = 32;
	attribute.maximum_index = 4095;
	attribute.components = 3;
	attribute.coordinate_values = 3;
	attribute.vertex_values = 3;
	error = bcm2711_native_attribute_encode(&attribute, bytes, BCM2711_NATIVE_ATTRIBUTE_BYTES);
	assert(error == 0);
	print_bytes("attribute-vertex", bytes, BCM2711_NATIVE_ATTRIBUTE_BYTES);
	attribute.components = 4;
	attribute.coordinate_values = 2;
	attribute.vertex_values = 2;
	attribute.stride = 16;
	attribute.divisor = 1;
	error = bcm2711_native_attribute_encode(&attribute, bytes, BCM2711_NATIVE_ATTRIBUTE_BYTES);
	assert(error == 0);
	print_bytes("attribute-instance", bytes, BCM2711_NATIVE_ATTRIBUTE_BYTES);
	attribute.divisor = 0x10000;
	error = bcm2711_native_attribute_encode(&attribute, bytes, BCM2711_NATIVE_ATTRIBUTE_BYTES);
	assert(error == EINVAL);

	/* Succeeded: the refusals held and every sequence was printed. */
	return 0;
}

/* Fills the draw-independent state with valid values. */
static void
base_state(
	struct bcm2711_native_bin *state)
{
	/* Valid shader, viewport and clipper values; every test is off. */
	memset(state, 0, sizeof(*state));
	state->shader = 0x40000;
	state->attributes = 4;
	state->vertices = 36;
	state->instances = 1;
	state->window[2] = 1920;
	state->window[3] = 1080;
	state->viewport.x_scale = 0x44700000U;
	state->viewport.y_scale = 0x44070000U;
	state->clipper.depth_scale = 0x3f800000U;
	state->forward = 1;
	state->reverse = 1;
	state->depth_compare = 7;

	/* Succeeded: the state encodes. */
	return;
}

/* Prints one named line of hex bytes. */
static void
print_bytes(
	const char *name,
	const uint8_t *bytes,
	size_t count)
{
	size_t index;

	/* The name, then each byte. */
	printf("%s ", name);
	for (index = 0; index < count; index++)
		printf("%02x", bytes[index]);
	printf("\n");

	/* Succeeded: one line was printed. */
	return;
}

/* Encodes one draw and prints its whole sequence. */
static void
print_draw(
	const char *name,
	const struct bcm2711_native_bin *state)
{
	uint8_t bytes[BCM2711_NATIVE_BIN_BYTES];
	int error;

	/* The encoder must accept the draw. */
	error = bcm2711_native_bin_encode(state, bytes, sizeof(bytes));
	assert(error == 0);
	print_bytes(name, bytes, sizeof(bytes));

	/* Succeeded: the sequence was printed. */
	return;
}
