/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The memory manager: every allocation belongs to a pool and is freed
 * with it, JPOOL_IMAGE when an image ends (or is aborted), JPOOL_PERMANENT
 * when the object is destroyed.  The manager and the decoder's state
 * (cinfo->master) are made with the object; an allocation that fails is
 * an error (error_exit).
 */

#include "internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * The manager's methods and the pools, kept in the decoder's state; the
 * methods come first so that cinfo->mem points at them.
 */
struct jpeg_compat_memory {
	struct jpeg_memory_mgr pub;
	struct jpeg_decomp_master master;
};

static void *jpeg_compat_alloc_small(j_common_ptr cinfo, int pool_id, size_t sizeofobject);
static JSAMPARRAY jpeg_compat_alloc_sarray(j_common_ptr cinfo, int pool_id, JDIMENSION samplesperrow, JDIMENSION numrows);
static JBLOCKARRAY jpeg_compat_alloc_barray(j_common_ptr cinfo, int pool_id, JDIMENSION blocksperrow, JDIMENSION numrows);
static void jpeg_compat_free_pool(j_common_ptr cinfo, int pool_id);
static void jpeg_compat_self_destruct(j_common_ptr cinfo);
static struct jpeg_decomp_master *jpeg_compat_master(j_common_ptr cinfo);

/*
 * Makes an object's memory manager and its decoder state (cinfo->mem and,
 * for a decompression object, cinfo->master).  Fails through error_exit.
 */
void
jpeg_compat_memory_init(
	j_common_ptr cinfo)
{
	struct jpeg_compat_memory *memory;

	/* The manager and the state, zeroed. */
	memory = calloc(1, sizeof(*memory));
	if (memory == NULL)
		jpeg_compat_fail_number(cinfo, JERR_OUT_OF_MEMORY, 0, 0);

	/* The methods. */
	memory->pub.alloc_small = jpeg_compat_alloc_small;
	memory->pub.alloc_large = jpeg_compat_alloc_small;
	memory->pub.alloc_sarray = jpeg_compat_alloc_sarray;
	memory->pub.alloc_barray = jpeg_compat_alloc_barray;
	memory->pub.free_pool = jpeg_compat_free_pool;
	memory->pub.self_destruct = jpeg_compat_self_destruct;
	memory->pub.max_memory_to_use = 0;
	memory->pub.max_alloc_chunk = 1000000000L;

	/* The object holds both. */
	cinfo->mem = &memory->pub;
	if (cinfo->is_decompressor)
		((j_decompress_ptr)cinfo)->master = &memory->master;
}

/* Allocates in a pool for the decoder (fails through error_exit). */
void *
jpeg_compat_alloc(
	j_decompress_ptr cinfo,
	int pool,
	size_t size)
{
	void *made;

	/* The bytes, from the object's manager. */
	made = cinfo->mem->alloc_large((j_common_ptr)cinfo, pool, size);
	return made;
}

/*
 * Ends an object: frees everything it allocated, then the manager.
 */
void
jpeg_destroy(
	j_common_ptr cinfo)
{
	/* The pools and the manager, when there are any. */
	if (cinfo->mem != NULL)
		cinfo->mem->self_destruct(cinfo);
	cinfo->mem = NULL;
	cinfo->global_state = 0;
}

/*
 * Abandons the image in progress: frees what the image allocated and
 * makes the object ready for another jpeg_read_header.
 */
void
jpeg_abort(
	j_common_ptr cinfo)
{
	j_decompress_ptr decompress;

	/* Nothing was made. */
	if (cinfo->mem == NULL)
		return;

	/* The image's pool, and the object back at its start. */
	cinfo->mem->free_pool(cinfo, JPOOL_IMAGE);
	cinfo->global_state = JPEG_STATE_START;
	if (cinfo->is_decompressor) {
		decompress = (j_decompress_ptr)cinfo;
		decompress->marker_list = NULL;
	}
}

/* Allocates an object in a pool. */
static void *
jpeg_compat_alloc_small(
	j_common_ptr cinfo,
	int pool_id,
	size_t sizeofobject)
{
	struct jpeg_decomp_master *master;
	struct jpeg_pool_block *block;
	size_t header;

	/* A pool that does not exist, or a size that cannot be added to the block's header. */
	if (pool_id < 0 || pool_id >= JPOOL_NUMPOOLS)
		jpeg_compat_fail_number(cinfo, JERR_BAD_STATE, pool_id, 0);
	header = offsetof(struct jpeg_pool_block, align);
	if (sizeofobject > SIZE_MAX - header)
		jpeg_compat_fail_number(cinfo, JERR_OUT_OF_MEMORY, 1, 0);

	/* The block, at the front of its pool. */
	block = malloc(header + sizeofobject);
	if (block == NULL)
		jpeg_compat_fail_number(cinfo, JERR_OUT_OF_MEMORY, 2, 0);
	master = jpeg_compat_master(cinfo);
	block->next = master->pools[pool_id];
	master->pools[pool_id] = block;

	/* Succeeded: the caller's bytes follow the header. */
	return &block->align;
}

/* Allocates rows of samples: the row pointers, then the rows in one piece. */
static JSAMPARRAY
jpeg_compat_alloc_sarray(
	j_common_ptr cinfo,
	int pool_id,
	JDIMENSION samplesperrow,
	JDIMENSION numrows)
{
	JSAMPARRAY rows;
	JSAMPLE *samples;
	JDIMENSION row;

	/* The pointers and the samples. */
	if (numrows != 0 && samplesperrow > SIZE_MAX / numrows)
		jpeg_compat_fail_number(cinfo, JERR_OUT_OF_MEMORY, 3, 0);
	rows = jpeg_compat_alloc_small(cinfo, pool_id, (size_t)numrows * sizeof(JSAMPROW));
	samples = jpeg_compat_alloc_small(cinfo, pool_id, (size_t)samplesperrow * numrows);

	/* Each pointer to its row. */
	for (row = 0; row < numrows; row++)
		rows[row] = samples + (size_t)row * samplesperrow;

	/* Succeeded: the rows. */
	return rows;
}

/* Allocates rows of coefficient blocks, as alloc_sarray does samples. */
static JBLOCKARRAY
jpeg_compat_alloc_barray(
	j_common_ptr cinfo,
	int pool_id,
	JDIMENSION blocksperrow,
	JDIMENSION numrows)
{
	JBLOCKARRAY rows;
	JBLOCKROW blocks;
	JDIMENSION row;

	/* The pointers and the blocks. */
	if (numrows != 0 && blocksperrow > SIZE_MAX / sizeof(JBLOCK) / numrows)
		jpeg_compat_fail_number(cinfo, JERR_OUT_OF_MEMORY, 4, 0);
	rows = jpeg_compat_alloc_small(cinfo, pool_id, (size_t)numrows * sizeof(JBLOCKROW));
	blocks = jpeg_compat_alloc_small(cinfo, pool_id, (size_t)blocksperrow * numrows * sizeof(JBLOCK));

	/* Each pointer to its row. */
	for (row = 0; row < numrows; row++)
		rows[row] = blocks + (size_t)row * blocksperrow;

	/* Succeeded: the rows. */
	return rows;
}

/* Frees every allocation of a pool (the permanent pool also takes the image's). */
static void
jpeg_compat_free_pool(
	j_common_ptr cinfo,
	int pool_id)
{
	struct jpeg_decomp_master *master;
	struct jpeg_pool_block *block;
	struct jpeg_pool_block *next;
	int pool;

	/* The pools freed: the one named, or both for the permanent one. */
	if (pool_id < 0 || pool_id >= JPOOL_NUMPOOLS)
		return;
	master = jpeg_compat_master(cinfo);
	for (pool = pool_id; pool < JPOOL_NUMPOOLS; pool++) {
		for (block = master->pools[pool]; block != NULL; block = next) {
			next = block->next;
			free(block);
		}

		/* The pool is empty. */
		master->pools[pool] = NULL;
	}
}

/* Frees the pools and the manager itself. */
static void
jpeg_compat_self_destruct(
	j_common_ptr cinfo)
{
	struct jpeg_compat_memory *memory;

	/* Every allocation, then the manager and the state it holds. */
	jpeg_compat_free_pool(cinfo, JPOOL_PERMANENT);
	memory = (struct jpeg_compat_memory *)cinfo->mem;
	free(memory);
	cinfo->mem = NULL;
	if (cinfo->is_decompressor)
		((j_decompress_ptr)cinfo)->master = NULL;
}

/* The decoder state that holds an object's pools (it lives beside the manager). */
static struct jpeg_decomp_master *
jpeg_compat_master(
	j_common_ptr cinfo)
{
	struct jpeg_compat_memory *memory;

	/* The manager's container. */
	memory = (struct jpeg_compat_memory *)cinfo->mem;
	return &memory->master;
}
