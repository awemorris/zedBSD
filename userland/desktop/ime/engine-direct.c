/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The direct-input engine: every key goes back to the application as it
 * was pressed (plan/ws095/design.md section 5).
 *
 * The compositor does not send keys to the input method while this language is
 * chosen, so the engine is seldom asked; it exists so that the direct
 * language is a language like any other.
 */

#include "engine.h"


static void direct_key(struct ime_engine *engine, const struct ime_key *key, struct ime_output *out);
static void direct_reset(struct ime_engine *engine, bool commit, struct ime_output *out);
static void direct_surrounding(struct ime_engine *engine, const char *text, uint32_t cursor, uint32_t anchor);
static void direct_content_type(struct ime_engine *engine, uint32_t hint, uint32_t purpose);
static void direct_save(struct ime_engine *engine);
static void direct_destroy(struct ime_engine *engine);

/*
 * The direct engine's functions.
 */
static const struct ime_engine_ops direct_ops = {
	"direct",
	"A",
	direct_key,
	direct_reset,
	direct_surrounding,
	direct_content_type,
	direct_save,
	direct_destroy,
	NULL,
	NULL,
	NULL,
	NULL
};

/*
 * Makes the direct-input engine.
 *
 * Returns 0; it has no state that can fail to be made.
 */
int
ime_direct_create(
	struct ime_engine *engine)
{
	/* Succeeded: the engine has functions and no state. */
	engine->ops = &direct_ops;
	engine->state = NULL;
	return 0;
}

/*
 * Gives every key back.
 */
static void
direct_key(
	struct ime_engine *engine,
	const struct ime_key *key,
	struct ime_output *out)
{
	UNUSED_PARAMETER(engine);
	UNUSED_PARAMETER(key);

	/* The key is the application's. */
	ime_output_clear(out);
	out->pass_key = true;
}

/*
 * Has nothing composed to end.
 */
static void
direct_reset(
	struct ime_engine *engine,
	bool commit,
	struct ime_output *out)
{
	UNUSED_PARAMETER(engine);
	UNUSED_PARAMETER(commit);

	/* An empty output. */
	ime_output_clear(out);
}

/*
 * Does not use the text around the cursor.
 */
static void
direct_surrounding(
	struct ime_engine *engine,
	const char *text,
	uint32_t cursor,
	uint32_t anchor)
{
	UNUSED_PARAMETER(engine);
	UNUSED_PARAMETER(text);
	UNUSED_PARAMETER(cursor);
	UNUSED_PARAMETER(anchor);
}

/*
 * Does not use what the field holds.
 */
static void
direct_content_type(
	struct ime_engine *engine,
	uint32_t hint,
	uint32_t purpose)
{
	UNUSED_PARAMETER(engine);
	UNUSED_PARAMETER(hint);
	UNUSED_PARAMETER(purpose);
}

/*
 * Has learned nothing to save.
 */
static void
direct_save(
	struct ime_engine *engine)
{
	UNUSED_PARAMETER(engine);
}

/*
 * Has no state to free.
 */
static void
direct_destroy(
	struct ime_engine *engine)
{
	/* Only the functions are forgotten. */
	engine->ops = NULL;
	engine->state = NULL;
}
