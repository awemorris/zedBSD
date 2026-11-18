/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arena: memory handed out in pieces from large blocks and freed all at
 * once.
 */

#include "base/base.h"

#include <stdlib.h>
#include <string.h>

/* The block size used when the caller asks for none. */
#define ARENA_DEFAULT_BLOCK	(64U * 1024U)

/* The alignment of every piece, enough for any scalar and a pointer. */
#define ARENA_ALIGN		16U

static size_t arena_round(size_t size);
static struct wb_arena_block *arena_add_block(struct wb_arena *arena, size_t size);

/*
 * Prepares an empty arena.
 *
 * block_size is the size of the blocks it takes from malloc; zero picks
 * the default.  A piece larger than a block gets a block of its own.
 */
void
wb_arena_init(
	struct wb_arena *arena,
	size_t block_size)
{
	/* Starts with no blocks, which is what release leaves too. */
	arena->head = NULL;
	arena->block_size = block_size;
	if (block_size == 0)
		arena->block_size = ARENA_DEFAULT_BLOCK;
}

/*
 * Hands out size bytes of the arena, aligned to 16 bytes.
 *
 * Returns NULL when memory runs out.  The bytes are not cleared.
 */
void *
wb_arena_alloc(
	struct wb_arena *arena,
	size_t size)
{
	struct wb_arena_block *block;
	size_t rounded;
	unsigned char *piece;

	/* Refuses a size whose rounding would overflow. */
	if (size > (size_t)-1 - ARENA_ALIGN)
		return NULL;
	rounded = arena_round(size);

	/* Takes a new block when the newest one has no room left. */
	block = arena->head;
	if (block == NULL || block->size - block->used < rounded) {
		block = arena_add_block(arena, rounded);
		if (block == NULL)
			return NULL;
	}

	/* Carves the piece from the free end of the block. */
	piece = (unsigned char *)block + arena_round(sizeof(*block)) + block->used;
	block->used += rounded;

	/* Succeeded: the piece lives until the arena is released. */
	return piece;
}

/*
 * Hands out size bytes of the arena, cleared to zero.
 */
void *
wb_arena_zalloc(
	struct wb_arena *arena,
	size_t size)
{
	void *piece;

	/* Takes the piece. */
	piece = wb_arena_alloc(arena, size);
	if (piece == NULL)
		return NULL;

	/* Clears it. */
	memset(piece, 0, size);

	/* Succeeded: the piece is zero. */
	return piece;
}

/*
 * Copies length bytes of a string into the arena and ends the copy with a
 * NUL.
 */
char *
wb_arena_strndup(
	struct wb_arena *arena,
	const char *string,
	size_t length)
{
	char *copy;

	/* Takes room for the bytes and the NUL. */
	copy = wb_arena_alloc(arena, length + 1);
	if (copy == NULL)
		return NULL;

	/* Copies the bytes and ends them. */
	memcpy(copy, string, length);
	copy[length] = '\0';

	/* Succeeded: the copy lives as long as the arena. */
	return copy;
}

/*
 * Frees every block of the arena; every piece it handed out is gone.
 */
void
wb_arena_release(
	struct wb_arena *arena)
{
	struct wb_arena_block *block;
	struct wb_arena_block *next;

	/* Frees the blocks from the newest to the oldest. */
	block = arena->head;
	while (block != NULL) {
		next = block->next;
		free(block);
		block = next;
	}

	/* An arena with no head owns nothing and can be used again. */
	arena->head = NULL;
}

/*
 * Takes over every block of another arena: what it handed out lives as long
 * as this arena does, and the other owns nothing afterwards.  The blocks go
 * behind this arena's own, so that new pieces still come from its newest one.
 */
void
wb_arena_adopt(
	struct wb_arena *arena,
	struct wb_arena *other)
{
	struct wb_arena_block *block;

	/* Nothing to take. */
	if (other->head == NULL)
		return;

	/* Behind the last block, or as the only ones. */
	if (arena->head == NULL) {
		arena->head = other->head;
	} else {
		block = arena->head;
		while (block->next != NULL)
			block = block->next;
		block->next = other->head;
	}
	other->head = NULL;
}

/* Tells how many bytes the blocks of the arena hold in all (what it took from malloc, less the blocks' own headers). */
size_t
wb_arena_bytes(
	const struct wb_arena *arena)
{
	const struct wb_arena_block *block;
	size_t bytes;

	bytes = 0;
	for (block = arena->head; block != NULL; block = block->next)
		bytes += block->size;
	return bytes;
}

/* Rounds a size up to the arena's alignment. */
static size_t
arena_round(
	size_t size)
{
	/* Reports the size rounded up to the next multiple of the alignment. */
	return (size + ARENA_ALIGN - 1U) & ~(size_t)(ARENA_ALIGN - 1U);
}

/* Adds a block with room for at least size bytes in front of the arena's list. */
static struct wb_arena_block *
arena_add_block(
	struct wb_arena *arena,
	size_t size)
{
	struct wb_arena_block *block;
	size_t capacity;
	size_t header;

	/* A piece larger than a block gets a block of exactly its size. */
	capacity = arena->block_size;
	if (size > capacity)
		capacity = size;
	header = arena_round(sizeof(*block));

	/* Takes the block from the C library. */
	block = malloc(header + capacity);
	if (block == NULL)
		return NULL;

	/* Puts the empty block at the head of the list. */
	block->next = arena->head;
	block->size = capacity;
	block->used = 0;
	arena->head = block;

	/* Succeeded: the new block is the arena's newest. */
	return block;
}
