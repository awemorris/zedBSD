/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SKK engine (WS154 p003): keys in, text and preedit out, through the
 * engine interface of engine.h.
 *
 * The keys, as ddskk binds them:
 *   kana mode      letters type kana straight in; q toggles katakana, l
 *                  goes to Latin, L to wide Latin; an upper-case letter
 *                  starts a reading (▽), Q starts one without a letter
 *   ▽ reading      letters add to it; an upper-case letter starts the
 *                  okurigana; Space converts; q puts it in as katakana;
 *                  Enter or C-j puts it in as typed; C-g drops it
 *   ▼ choosing     Space shows the next candidate and x the previous; from
 *                  the fifth the candidates are listed seven at a time and
 *                  a s d f j k l choose; Enter or C-j puts the candidate
 *                  in, as does typing on; C-g and Backspace go back to ▽
 *   registration   when the candidates run out: the word is typed (it may
 *                  be converted in turn, three registrations deep), Enter
 *                  keeps it in the user's dictionary and puts it in, C-g
 *                  gives up
 *   Latin mode     keys go to the application; C-j goes back to kana
 *   wide Latin     printable keys type their full-width forms; C-j goes
 *                  back to kana
 */

#include "skk.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The characters the preedit marks a reading and a choice with, and the registration's. */
#define SKK_MARK_READING	"\xe2\x96\xbd"
#define SKK_MARK_CHOOSING	"\xe2\x96\xbc"
#define SKK_MARK_OKURI		"*"
#define SKK_MARK_REGISTER	"[\xe7\x99\xbb\xe9\x8c\xb2]"

/* The keys that choose a listed candidate, in order. */
#define SKK_CHOICE_KEYS		"asdfjkl"

static void engine_key(struct ime_engine *engine, const struct ime_key *key, struct ime_output *out);
static void engine_reset(struct ime_engine *engine, bool commit, struct ime_output *out);
static void engine_surrounding(struct ime_engine *engine, const char *text, uint32_t cursor, uint32_t anchor);
static void engine_content_type(struct ime_engine *engine, uint32_t hint, uint32_t purpose);
static void engine_save(struct ime_engine *engine);
static void engine_destroy(struct ime_engine *engine);
static const char *engine_mode(struct ime_engine *engine, const char **label);
static bool engine_select(struct ime_engine *engine, const char *id);
static bool key_control(struct skk_state *state, const struct ime_key *key, struct ime_output *out);
static void key_wide(struct skk_state *state, const struct ime_key *key, struct ime_output *out);
static void key_enter(struct skk_state *state, struct ime_output *out);
static void key_backspace(struct skk_state *state, struct ime_output *out);
static void key_space(struct skk_state *state, struct ime_output *out);
static bool key_choosing(struct skk_state *state, uint32_t character, struct ime_output *out);
static void key_character(struct skk_state *state, uint32_t character, struct ime_output *out);
static bool key_command(struct skk_state *state, struct skk_frame *frame, char letter, struct ime_output *out);
static void feed(struct skk_state *state, char letter, struct ime_output *out);
static void flush_pending(struct skk_state *state, struct ime_output *out);
static void take_units(struct skk_state *state, const struct ja_romaji_result *result, struct ime_output *out);
static void convert(struct skk_state *state);
static void collect(struct skk_state *state, struct skk_frame *frame, const char *key, size_t key_length);
static void add_candidate(struct skk_frame *frame, const char *text, size_t length);
static void next_candidate(struct skk_state *state);
static void previous_candidate(struct skk_state *state);
static void commit_choice(struct skk_state *state, struct ime_output *out);
static void commit_reading(struct skk_state *state, bool katakana, struct ime_output *out);
static void commit_text(struct skk_state *state, const char *text, size_t length, struct ime_output *out);
static void register_start(struct skk_state *state);
static void register_finish(struct skk_state *state, struct ime_output *out);
static void register_abort(struct skk_state *state);
static void back_to_reading(struct skk_frame *frame);
static void frame_clear(struct skk_frame *frame);
static size_t lookup_key(const struct skk_frame *frame, char *key, size_t size);
static void learn(struct skk_state *state, const struct skk_frame *frame, const char *word, size_t length);
static bool text_append(char *buffer, size_t size, size_t *length, const char *text, size_t text_length);
static void text_pop(char *buffer, size_t *length);
static size_t form_of(const char *text, size_t length, bool katakana, char *out, size_t size);
static void render(struct skk_state *state, struct ime_output *out);
static void render_frame(const struct skk_state *state, const struct skk_frame *frame, struct ime_output *out);
static void render_append(struct ime_output *out, const char *text, size_t length);

/* The engine's functions as the input method calls them. */
static const struct ime_engine_ops engine_ops = {
	"skk",
	"\xe3\x81\x82",
	engine_key,
	engine_reset,
	engine_surrounding,
	engine_content_type,
	engine_save,
	engine_destroy,
	engine_mode,
	engine_select,
	NULL,
	NULL
};

/*
 * The modes as languages of their own to the desktop (WS154 D3): each one's
 * ID, its label on the system bar, and the mode.  The desktop remembers a
 * language for each application, so it remembers the mode too.
 */
struct skk_mode_name {
	const char *id;
	const char *label;
	enum skk_mode mode;
};

/* The modes' names, in the order of enum skk_mode. */
static const struct skk_mode_name mode_names[] = {
	{ "skk", "\xe3\x81\x82", SKK_MODE_KANA },
	{ "skk-katakana", "\xe3\x82\xa2", SKK_MODE_KATAKANA },
	{ "skk-latin", "A", SKK_MODE_LATIN },
	{ "skk-wide", "\xef\xbc\xa1", SKK_MODE_WIDE },
};

/*
 * Makes the SKK engine: reads its dictionaries and the user's.  A missing
 * system dictionary leaves the engine without its candidates; a missing
 * user dictionary starts an empty one.  Returns 0 or ENOMEM.
 */
int
skk_engine_create(
	struct ime_engine *engine,
	const struct skk_config *config)
{
	struct skk_state *state;
	size_t i;
	int error;

	/* The state, in kana mode with nothing composed. */
	state = calloc(1, sizeof(*state));
	if (state == NULL)
		return ENOMEM;

	/* Kana mode, learning, nothing pending. */
	state->mode = SKK_MODE_KANA;
	state->learning = true;
	ja_romaji_reset(&state->romaji);

	/* Each system dictionary there is. */
	for (i = 0; i < SKK_DICTS_MAX; i++) {
		/* A dictionary not named. */
		if (config->dictionaries[i] == NULL)
			continue;

		/* Read whole and indexed; one that cannot be read is left out. */
		error = ja_dict_load(&state->dicts[state->dict_count], config->dictionaries[i], JA_DICT_SIZE_MAX);
		if (error == ENOMEM) {
			engine->state = state;
			engine->ops = &engine_ops;
			engine_destroy(engine);
			return ENOMEM;
		}

		/* Counted when it was read. */
		if (error == 0)
			state->dict_count++;
	}

	/* The user's dictionary (an empty one when there is none yet). */
	error = ja_user_open(&state->user, config->user_dictionary);
	if (error == 0)
		state->user_open = true;

	/* Succeeded: the engine is ready. */
	engine->state = state;
	engine->ops = &engine_ops;
	return 0;
}

/*
 * Interprets one key.
 */
static void
engine_key(
	struct ime_engine *engine,
	const struct ime_key *key,
	struct ime_output *out)
{
	struct skk_state *state;
	bool handled;

	/* The output of this key alone. */
	state = engine->state;
	ime_output_clear(out);

	/* Keys with Alt or Super belong to the application or the desktop. */
	if ((key->modifiers & (IME_MOD_ALT | IME_MOD_SUPER)) != 0U) {
		out->pass_key = true;
		render(state, out);
		return;
	}

	/* C-g and C-j, and any other control key. */
	if ((key->modifiers & IME_MOD_CTRL) != 0U) {
		handled = key_control(state, key, out);
		if (!handled)
			out->pass_key = true;

		/* What is composed now. */
		render(state, out);
		return;
	}

	/* Latin mode lets every other key through. */
	if (state->mode == SKK_MODE_LATIN) {
		out->pass_key = true;
		render(state, out);
		return;
	}

	/* Wide Latin types the full-width forms. */
	if (state->mode == SKK_MODE_WIDE) {
		key_wide(state, key, out);
		render(state, out);
		return;
	}

	/* The keys with a meaning of their own, then the characters. */
	if (key->code == IME_KEY_ENTER || key->code == IME_KEY_KP_ENTER)
		key_enter(state, out);
	else if (key->code == IME_KEY_BACKSPACE)
		key_backspace(state, out);
	else if (key->code == IME_KEY_SPACE)
		key_space(state, out);
	else if (key->character >= 0x21U && key->character < 0x7fU)
		key_character(state, key->character, out);
	else
		out->pass_key = true;

	/* What is composed now. */
	render(state, out);
}

/*
 * Ends what is composed: committed into the output (switching languages)
 * or dropped (deactivation).
 */
static void
engine_reset(
	struct ime_engine *engine,
	bool commit,
	struct ime_output *out)
{
	struct skk_state *state;
	struct skk_frame *top;
	unsigned i;

	/* Only the text itself can be committed; a registration in progress is dropped. */
	state = engine->state;
	ime_output_clear(out);
	state->depth = 0;
	top = &state->frames[0];
	if (commit && top->phase == SKK_PHASE_CHOOSING)
		commit_choice(state, out);
	else if (commit && top->phase == SKK_PHASE_READING)
		commit_reading(state, state->mode == SKK_MODE_KATAKANA, out);
	else if (commit)
		flush_pending(state, out);

	/* Nothing is left composed. */
	ja_romaji_reset(&state->romaji);
	for (i = 0; i < SKK_FRAMES_MAX; i++)
		frame_clear(&state->frames[i]);

	/* What is composed now (nothing). */
	render(state, out);
}

/*
 * Hears the text around the cursor, which SKK does not use.
 */
static void
engine_surrounding(
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
 * Hears what the field holds: a secret one teaches the user's dictionary
 * nothing.
 */
static void
engine_content_type(
	struct ime_engine *engine,
	uint32_t hint,
	uint32_t purpose)
{
	struct skk_state *state;

	/* Learning unless the field is secret. */
	state = engine->state;
	state->learning = true;
	if ((hint & (IME_HINT_HIDDEN_TEXT | IME_HINT_SENSITIVE_DATA)) != 0U)
		state->learning = false;
	else if (purpose == IME_PURPOSE_PASSWORD || purpose == IME_PURPOSE_PIN)
		state->learning = false;
}

/*
 * Writes what was learned to the user's dictionary, by the writer thread.
 */
static void
engine_save(
	struct ime_engine *engine)
{
	struct skk_state *state;
	int error;

	/* Nothing learned since the last save. */
	state = engine->state;
	if (!state->user_open || !state->user_unsaved)
		return;

	/* Handed to the writer thread; a failure is tried again at the next save. */
	error = ja_user_save_later(&state->user);
	if (error == 0)
		state->user_unsaved = false;
}

/*
 * Writes what is unsaved and frees the engine.
 */
static void
engine_destroy(
	struct ime_engine *engine)
{
	struct skk_state *state;
	size_t i;

	/* The choices not yet in the file go to the writer thread, which ja_user_free waits for. */
	state = engine->state;
	if (state->user_open && state->user_unsaved)
		(void)ja_user_save_later(&state->user);

	/* The user's dictionary. */
	if (state->user_open)
		ja_user_free(&state->user);

	/* The system dictionaries and the state. */
	for (i = 0; i < state->dict_count; i++)
		ja_dict_free(&state->dicts[i]);

	/* The state. */
	free(state);
	engine->state = NULL;
}

/*
 * Gives the ID and label of the mode the engine is in.
 */
static const char *
engine_mode(
	struct ime_engine *engine,
	const char **label)
{
	struct skk_state *state;
	size_t i;

	/* The mode's name. */
	state = engine->state;
	for (i = 0; i < sizeof(mode_names) / sizeof(mode_names[0]); i++) {
		/* The entry of the mode. */
		if (mode_names[i].mode == state->mode) {
			*label = mode_names[i].label;
			return mode_names[i].id;
		}
	}

	/* Kana (not reached: every mode has its name). */
	*label = mode_names[0].label;
	return mode_names[0].id;
}

/*
 * Takes a mode by its ID.  Returns false for an ID that is not one of the
 * modes.  What is composed is kept (the input method commits it when the
 * language changes).
 */
static bool
engine_select(
	struct ime_engine *engine,
	const char *id)
{
	struct skk_state *state;
	size_t i;
	int differs;

	/* The mode of the ID. */
	state = engine->state;
	for (i = 0; i < sizeof(mode_names) / sizeof(mode_names[0]); i++) {
		/* The entry of the ID. */
		differs = strcmp(mode_names[i].id, id);
		if (differs == 0) {
			state->mode = mode_names[i].mode;
			return true;
		}
	}

	/* Not one of the modes. */
	return false;
}

/*
 * Interprets a key held with Control: C-g gives up the step being taken
 * and C-j puts in what is composed (or goes back to kana from Latin).
 * Returns false for a control key SKK does not use.
 */
static bool
key_control(
	struct skk_state *state,
	const struct ime_key *key,
	struct ime_output *out)
{
	struct skk_frame *frame;

	/* C-j: kana again, or what is composed put in. */
	frame = &state->frames[state->depth];
	if (key->character == 'j' || key->character == 'J') {
		/* From either Latin mode, kana. */
		if (state->mode == SKK_MODE_LATIN || state->mode == SKK_MODE_WIDE) {
			state->mode = SKK_MODE_KANA;
			return true;
		}

		/* Otherwise like Enter. */
		key_enter(state, out);
		return true;
	}

	/* Only C-g is left. */
	if (key->character != 'g' && key->character != 'G')
		return false;

	/* C-g while choosing goes back to the reading. */
	if (frame->phase == SKK_PHASE_CHOOSING) {
		back_to_reading(frame);
		return true;
	}

	/* C-g on a reading drops it. */
	if (frame->phase == SKK_PHASE_READING) {
		ja_romaji_reset(&state->romaji);
		frame_clear(frame);
		return true;
	}

	/* C-g in a registration gives it up. */
	if (state->depth != 0) {
		register_abort(state);
		return true;
	}

	/* C-g with letters typed drops them; with nothing, it is the application's. */
	if (state->romaji.pending_length != 0) {
		ja_romaji_reset(&state->romaji);
		return true;
	}

	/* Nothing to give up. */
	return false;
}

/*
 * Types the full-width form of a printable key in wide Latin mode; other
 * keys go to the application.
 */
static void
key_wide(
	struct skk_state *state,
	const struct ime_key *key,
	struct ime_output *out)
{
	char text[4];
	size_t length;
	uint32_t wide;

	/* Only printable characters (a space becomes the full-width space). */
	if (key->character < 0x20U || key->character >= 0x7fU || key->code == IME_KEY_ENTER) {
		out->pass_key = true;
		return;
	}

	/* The full-width form, put in. */
	wide = 0x3000U;
	if (key->character != 0x20U)
		wide = ja_to_full_ascii(key->character);

	/* Its bytes, put in. */
	length = ja_utf8_encode(wide, text);
	commit_text(state, text, length, out);
}

/*
 * Enter: puts in the candidate or the reading, finishes a registration, or
 * goes to the application after the pending letters are put in.
 */
static void
key_enter(
	struct skk_state *state,
	struct ime_output *out)
{
	struct skk_frame *frame;

	/* The candidate shown. */
	frame = &state->frames[state->depth];
	if (frame->phase == SKK_PHASE_CHOOSING) {
		commit_choice(state, out);
		return;
	}

	/* The reading as typed. */
	if (frame->phase == SKK_PHASE_READING) {
		commit_reading(state, state->mode == SKK_MODE_KATAKANA, out);
		return;
	}

	/* The word of a registration. */
	if (state->depth != 0) {
		register_finish(state, out);
		return;
	}

	/* The pending letters, and the key itself to the application. */
	flush_pending(state, out);
	out->pass_key = true;
}

/*
 * Backspace: back from choosing to the reading, or the last letter, kana
 * or okurigana taken back.
 */
static void
key_backspace(
	struct skk_state *state,
	struct ime_output *out)
{
	struct skk_frame *frame;
	bool taken;

	/* Choosing goes back to the reading. */
	frame = &state->frames[state->depth];
	if (frame->phase == SKK_PHASE_CHOOSING) {
		back_to_reading(frame);
		return;
	}

	/* A pending letter goes first. */
	taken = ja_romaji_backspace(&state->romaji);
	if (taken)
		return;

	/* On a reading: the okurigana, the okurigana's start, then the reading. */
	if (frame->phase == SKK_PHASE_READING) {
		if (frame->okuri_length != 0)
			text_pop(frame->okuri, &frame->okuri_length);
		else if (frame->okuri_letter != '\0')
			frame->okuri_letter = '\0';
		else
			text_pop(frame->reading, &frame->reading_length);

		/* An empty reading is no reading. */
		if (frame->reading_length == 0 && frame->okuri_letter == '\0')
			frame_clear(frame);

		/* The reading is shorter, or gone. */
		return;
	}

	/* In a registration, the word's last character. */
	if (state->depth != 0) {
		text_pop(frame->word, &frame->word_length);
		return;
	}

	/* Nothing composed: the application's. */
	out->pass_key = true;
}

/*
 * Space: converts the reading, shows the next candidate, or goes to the
 * application (into a registration's word).
 */
static void
key_space(
	struct skk_state *state,
	struct ime_output *out)
{
	struct skk_frame *frame;

	/* The next candidate. */
	frame = &state->frames[state->depth];
	if (frame->phase == SKK_PHASE_CHOOSING) {
		next_candidate(state);
		return;
	}

	/* The reading converted (a pending n is ん first). */
	if (frame->phase == SKK_PHASE_READING) {
		flush_pending(state, out);
		convert(state);
		return;
	}

	/* The pending letters, then a space into a registration's word or to the application. */
	flush_pending(state, out);
	if (state->depth != 0) {
		commit_text(state, " ", 1, out);
		return;
	}

	/* The application types the space. */
	out->pass_key = true;
}

/*
 * A character while choosing: x for the previous candidate, a listed
 * candidate's key, or any other character, which puts the candidate in
 * and is then typed.  Returns true when the character is used up.
 */
static bool
key_choosing(
	struct skk_state *state,
	uint32_t character,
	struct ime_output *out)
{
	struct skk_frame *frame;
	const char *choice;
	size_t page;
	size_t index;

	/* x: the previous candidate. */
	frame = &state->frames[state->depth];
	if (character == 'x') {
		previous_candidate(state);
		return true;
	}

	/* While the candidates are listed, their keys choose one. */
	choice = NULL;
	if (frame->selected >= SKK_INLINE)
		choice = strchr(SKK_CHOICE_KEYS, (int)character);

	/* A listed candidate's key. */
	if (choice != NULL && character != '\0') {
		/* The candidate under the key, when the page has one there. */
		page = (frame->selected - SKK_INLINE) / SKK_PAGE;
		index = SKK_INLINE + page * SKK_PAGE + (size_t)(choice - SKK_CHOICE_KEYS);
		if (index < frame->candidate_count) {
			frame->selected = index;
			commit_choice(state, out);
		}

		/* The key is used, whether its place had a candidate or not. */
		return true;
	}

	/* Any other character: the candidate goes in, and the character is typed after it. */
	commit_choice(state, out);
	return false;
}

/*
 * A printable character in kana or katakana mode.
 */
static void
key_character(
	struct skk_state *state,
	uint32_t character,
	struct ime_output *out)
{
	struct skk_frame *frame;
	bool used;
	char letter;

	/* While choosing: x, a listed candidate's key, or the candidate put in first. */
	frame = &state->frames[state->depth];
	if (frame->phase == SKK_PHASE_CHOOSING) {
		used = key_choosing(state, character, out);
		if (used)
			return;
	}

	/* The mode keys, when no letter is pending. */
	letter = (char)character;
	used = key_command(state, frame, letter, out);
	if (used)
		return;

	/* An upper-case letter outside a reading starts one. */
	if (letter >= 'A' && letter <= 'Z' && frame->phase == SKK_PHASE_INPUT) {
		flush_pending(state, out);
		frame->phase = SKK_PHASE_READING;
		feed(state, (char)(letter - 'A' + 'a'), out);
		return;
	}

	/* An upper-case letter on a reading starts its okurigana (once). */
	if (letter >= 'A' && letter <= 'Z' && frame->phase == SKK_PHASE_READING) {
		if (frame->okuri_letter == '\0' && frame->reading_length != 0) {
			flush_pending(state, out);
			frame->okuri_letter = (char)(letter - 'A' + 'a');
		}

		/* The letter types its kana. */
		feed(state, (char)(letter - 'A' + 'a'), out);
		return;
	}

	/* Any other character goes through the romaji. */
	feed(state, letter, out);
}

/*
 * The keys that change the mode or start a reading, when no letter is
 * pending: q, l, L and Q.  Returns true when the key was one of them.
 */
static bool
key_command(
	struct skk_state *state,
	struct skk_frame *frame,
	char letter,
	struct ime_output *out)
{
	/* A pending letter makes these keys letters (xq is no command). */
	if (state->romaji.pending_length != 0)
		return false;

	/* q on a reading puts it in as the other kana. */
	if (letter == 'q' && frame->phase == SKK_PHASE_READING) {
		commit_reading(state, state->mode != SKK_MODE_KATAKANA, out);
		return true;
	}

	/* The other commands apply only between readings. */
	if (frame->phase != SKK_PHASE_INPUT)
		return false;

	/* q toggles between hiragana and katakana. */
	if (letter == 'q') {
		if (state->mode == SKK_MODE_KATAKANA)
			state->mode = SKK_MODE_KANA;
		else
			state->mode = SKK_MODE_KATAKANA;

		/* The mode changed. */
		return true;
	}

	/* l goes to Latin, L to wide Latin. */
	if (letter == 'l') {
		state->mode = SKK_MODE_LATIN;
		return true;
	}

	/* Wide Latin. */
	if (letter == 'L') {
		state->mode = SKK_MODE_WIDE;
		return true;
	}

	/* Q starts a reading without a letter. */
	if (letter == 'Q') {
		frame->phase = SKK_PHASE_READING;
		return true;
	}

	/* Not a command. */
	return false;
}

/*
 * Gives one letter to the romaji and takes the kana it completes.
 */
static void
feed(
	struct skk_state *state,
	char letter,
	struct ime_output *out)
{
	struct ja_romaji_result result;
	struct skk_frame *frame;

	/* The letter, and what it completes. */
	ja_romaji_feed(&state->romaji, letter, &result);
	take_units(state, &result, out);

	/* With okurigana started, the reading converts once the okurigana's letters have all made kana. */
	frame = &state->frames[state->depth];
	if (frame->phase == SKK_PHASE_READING && frame->okuri_letter != '\0' &&
	    frame->okuri_length != 0 && state->romaji.pending_length == 0)
		convert(state);
}

/*
 * Ends the pending letters: a lone n becomes ん, others are kept as typed.
 */
static void
flush_pending(
	struct skk_state *state,
	struct ime_output *out)
{
	struct ja_romaji_result result;

	/* What the pending letters make (the flush adds to a result it does not empty). */
	result.count = 0;
	ja_romaji_flush(&state->romaji, &result);
	take_units(state, &result, out);
}

/*
 * Takes the characters the romaji made: into the reading or its okurigana,
 * or put in straight away.
 */
static void
take_units(
	struct skk_state *state,
	const struct ja_romaji_result *result,
	struct ime_output *out)
{
	struct skk_frame *frame;
	char text[8];
	char shown[32];
	size_t length;
	size_t shown_length;
	size_t i;

	/* Each character made, into the reading, its okurigana, or the text. */
	frame = &state->frames[state->depth];
	for (i = 0; i < result->count; i++) {
		/* The character's bytes. */
		length = ja_utf8_encode(result->units[i].code, text);

		/* On a reading: the okurigana once they started, else the reading. */
		if (frame->phase == SKK_PHASE_READING && frame->okuri_letter != '\0') {
			(void)text_append(frame->okuri, sizeof(frame->okuri), &frame->okuri_length, text, length);
			continue;
		}

		/* The reading itself. */
		if (frame->phase == SKK_PHASE_READING) {
			(void)text_append(frame->reading, sizeof(frame->reading), &frame->reading_length, text, length);
			continue;
		}

		/* Straight in, as katakana in katakana mode. */
		shown_length = form_of(text, length, state->mode == SKK_MODE_KATAKANA, shown, sizeof(shown));
		commit_text(state, shown, shown_length, out);
	}
}

/*
 * Converts the reading: its candidates are looked up (the user's first),
 * and the first one is shown; without any, a registration starts.
 */
static void
convert(
	struct skk_state *state)
{
	struct skk_frame *frame;
	char key[SKK_TEXT_MAX + 2U];
	size_t key_length;

	/* An empty reading converts nothing. */
	frame = &state->frames[state->depth];
	if (frame->reading_length == 0)
		return;

	/* The candidates of the reading (and the okurigana's letter). */
	key_length = lookup_key(frame, key, sizeof(key));
	frame->candidate_count = 0;
	frame->selected = 0;
	collect(state, frame, key, key_length);

	/* Choosing the first, or teaching the word when there is none. */
	frame->phase = SKK_PHASE_CHOOSING;
	if (frame->candidate_count == 0)
		register_start(state);
}

/*
 * Gathers a key's candidates: the user's choices, then each dictionary's,
 * each once.
 */
static void
collect(
	struct skk_state *state,
	struct skk_frame *frame,
	const char *key,
	size_t key_length)
{
	const struct ja_user_entry *learned;
	const struct ja_dict_entry *entry;
	const char *candidate;
	size_t position;
	size_t length;
	size_t i;
	bool more;

	/* The user's choices first, most recent first. */
	learned = NULL;
	if (state->user_open)
		learned = ja_user_find(&state->user, key, key_length);

	/* Added in the user's order. */
	if (learned != NULL) {
		for (i = 0; i < learned->candidate_count; i++)
			add_candidate(frame, learned->candidates[i], strlen(learned->candidates[i]));
	}

	/* Each dictionary's, in order. */
	for (i = 0; i < state->dict_count; i++) {
		/* The entry of the key, when the dictionary has one. */
		entry = ja_dict_find(&state->dicts[i], key, key_length);
		if (entry == NULL)
			continue;

		/* Its candidates, their annotations left out. */
		position = 0;
		for (;;) {
			/* The next candidate, or the end of the entry. */
			more = ja_dict_next_candidate(entry, &position, &candidate, &length);
			if (!more)
				break;

			/* Added unless it is there already. */
			add_candidate(frame, candidate, length);
		}
	}
}

/*
 * Adds a candidate unless it is there already, while there is room.
 */
static void
add_candidate(
	struct skk_frame *frame,
	const char *text,
	size_t length)
{
	size_t i;
	bool same;

	/* Too long, or no room left. */
	if (length == 0 || length >= IME_CANDIDATE_MAX || frame->candidate_count == IME_CANDIDATES_MAX)
		return;

	/* Once each. */
	for (i = 0; i < frame->candidate_count; i++) {
		/* The same text. */
		same = ja_bytes_equal(frame->candidates[i], strlen(frame->candidates[i]), text, length);
		if (same)
			return;
	}

	/* Added. */
	memcpy(frame->candidates[frame->candidate_count], text, length);
	frame->candidates[frame->candidate_count][length] = '\0';
	frame->candidate_count++;
}

/*
 * Shows the next candidate: one by one for the first four, then a page of
 * seven at a time; past the last, a registration.
 */
static void
next_candidate(
	struct skk_state *state)
{
	struct skk_frame *frame;
	size_t page;
	size_t next;

	/* One by one, or the first of the next page. */
	frame = &state->frames[state->depth];
	next = frame->selected + 1U;
	if (frame->selected >= SKK_INLINE) {
		page = (frame->selected - SKK_INLINE) / SKK_PAGE;
		next = SKK_INLINE + (page + 1U) * SKK_PAGE;
	}

	/* Past the last: the word is taught. */
	if (next >= frame->candidate_count) {
		register_start(state);
		return;
	}

	/* The candidate shown. */
	frame->selected = next;
}

/*
 * Shows the previous candidate (the previous page while they are listed);
 * before the first, back to the reading.
 */
static void
previous_candidate(
	struct skk_state *state)
{
	struct skk_frame *frame;
	size_t page;

	/* Before the first: the reading again. */
	frame = &state->frames[state->depth];
	if (frame->selected == 0) {
		back_to_reading(frame);
		return;
	}

	/* One by one before the pages. */
	if (frame->selected <= SKK_INLINE) {
		frame->selected--;
		return;
	}

	/* The previous page, or the last one shown one by one from the first page. */
	page = (frame->selected - SKK_INLINE) / SKK_PAGE;
	if (page == 0)
		frame->selected = SKK_INLINE - 1U;
	else
		frame->selected = SKK_INLINE + (page - 1U) * SKK_PAGE;
}

/*
 * Puts in the candidate shown and its okurigana, and learns the choice.
 */
static void
commit_choice(
	struct skk_state *state,
	struct ime_output *out)
{
	struct skk_frame *frame;
	char text[IME_CANDIDATE_MAX + SKK_TEXT_MAX];
	char okuri[SKK_TEXT_MAX * 2U];
	size_t length;
	size_t okuri_length;
	size_t candidate_length;

	/* The candidate, then the okurigana in the mode's kana. */
	frame = &state->frames[state->depth];
	candidate_length = strlen(frame->candidates[frame->selected]);
	memcpy(text, frame->candidates[frame->selected], candidate_length);
	length = candidate_length;
	okuri_length = form_of(frame->okuri, frame->okuri_length, state->mode == SKK_MODE_KATAKANA, okuri, sizeof(okuri));
	(void)text_append(text, sizeof(text), &length, okuri, okuri_length);

	/* The choice is learned, then put in. */
	learn(state, frame, frame->candidates[frame->selected], candidate_length);
	frame_clear(frame);
	commit_text(state, text, length, out);
}

/*
 * Puts in the reading as it was typed (with its okurigana), as katakana
 * when asked.
 */
static void
commit_reading(
	struct skk_state *state,
	bool katakana,
	struct ime_output *out)
{
	struct skk_frame *frame;
	char reading[SKK_TEXT_MAX * 2U];
	char text[SKK_TEXT_MAX * 4U];
	size_t length;
	size_t text_length;

	/* The pending letters first, then the reading and okurigana in the kana asked for. */
	flush_pending(state, out);
	frame = &state->frames[state->depth];
	memcpy(reading, frame->reading, frame->reading_length);
	length = frame->reading_length;
	(void)text_append(reading, sizeof(reading), &length, frame->okuri, frame->okuri_length);
	text_length = form_of(reading, length, katakana, text, sizeof(text));

	/* Put in, and the reading is done. */
	frame_clear(frame);
	commit_text(state, text, text_length, out);
}

/*
 * Puts text in: into the output at the top, into the registration's word
 * inside one.
 */
static void
commit_text(
	struct skk_state *state,
	const char *text,
	size_t length,
	struct ime_output *out)
{
	struct skk_frame *frame;

	/* The text itself. */
	if (state->depth == 0) {
		(void)ime_output_append_commit(out, text, length);
		return;
	}

	/* The word being registered. */
	frame = &state->frames[state->depth];
	(void)text_append(frame->word, sizeof(frame->word), &frame->word_length, text, length);
}

/*
 * Starts a registration for the reading being converted, in a new
 * composition; three deep at most (then the last candidate stays, or the
 * reading when there is none).
 */
static void
register_start(
	struct skk_state *state)
{
	struct skk_frame *frame;

	/* Too deep already: stays where it was. */
	frame = &state->frames[state->depth];
	if (state->depth + 1U >= SKK_FRAMES_MAX) {
		if (frame->candidate_count == 0)
			back_to_reading(frame);

		/* The registration does not start. */
		return;
	}

	/* A new composition for the word. */
	ja_romaji_reset(&state->romaji);
	state->depth++;
	frame_clear(&state->frames[state->depth]);
}

/*
 * Ends a registration with the word typed: it is learned for the reading,
 * and put in (with the okurigana) by the composition that asked.  An empty
 * word gives the registration up.
 */
static void
register_finish(
	struct skk_state *state,
	struct ime_output *out)
{
	struct skk_frame *frame;
	struct skk_frame *asking;
	char word[SKK_TEXT_MAX];
	size_t length;

	/* The pending letters into the word. */
	flush_pending(state, out);
	frame = &state->frames[state->depth];
	if (frame->word_length == 0) {
		register_abort(state);
		return;
	}

	/* The word, and back to the composition that asked. */
	memcpy(word, frame->word, frame->word_length);
	length = frame->word_length;
	frame_clear(frame);
	state->depth--;

	/* It becomes that composition's one candidate, put in and learned. */
	asking = &state->frames[state->depth];
	asking->candidate_count = 0;
	add_candidate(asking, word, length);
	if (asking->candidate_count == 0) {
		back_to_reading(asking);
		return;
	}

	/* Put in. */
	asking->selected = 0;
	commit_choice(state, out);
}

/*
 * Gives a registration up: the composition that asked goes back to its
 * reading.
 */
static void
register_abort(
	struct skk_state *state)
{
	/* Nothing to give up at the top. */
	if (state->depth == 0)
		return;

	/* The registration goes; the reading comes back. */
	ja_romaji_reset(&state->romaji);
	frame_clear(&state->frames[state->depth]);
	state->depth--;
	back_to_reading(&state->frames[state->depth]);
}

/*
 * Goes back from choosing to the reading: the okurigana become part of it
 * again, as ddskk does.
 */
static void
back_to_reading(
	struct skk_frame *frame)
{
	/* The reading with its okurigana, and no candidates. */
	(void)text_append(frame->reading, sizeof(frame->reading), &frame->reading_length, frame->okuri, frame->okuri_length);
	frame->okuri_length = 0;
	frame->okuri_letter = '\0';
	frame->candidate_count = 0;
	frame->selected = 0;
	frame->phase = SKK_PHASE_READING;
}

/*
 * Empties a composition.
 */
static void
frame_clear(
	struct skk_frame *frame)
{
	/* Back to typing kana, with nothing in it (the word of a registration too). */
	frame->phase = SKK_PHASE_INPUT;
	frame->reading_length = 0;
	frame->okuri_length = 0;
	frame->okuri_letter = '\0';
	frame->candidate_count = 0;
	frame->selected = 0;
	frame->word_length = 0;
}

/*
 * Gives the dictionary key of a reading: the reading, then the
 * okurigana's letter when there are okurigana (SKK's okuri-ari form).
 */
static size_t
lookup_key(
	const struct skk_frame *frame,
	char *key,
	size_t size)
{
	size_t length;

	/* The reading. */
	length = 0;
	(void)text_append(key, size, &length, frame->reading, frame->reading_length);

	/* The okurigana's letter. */
	if (frame->okuri_letter != '\0')
		(void)text_append(key, size, &length, &frame->okuri_letter, 1);

	/* The key's length. */
	key[length] = '\0';
	return length;
}

/*
 * Learns a choice for the composition's reading, unless the field is
 * secret.
 */
static void
learn(
	struct skk_state *state,
	const struct skk_frame *frame,
	const char *word,
	size_t length)
{
	char key[SKK_TEXT_MAX + 2U];
	char copy[IME_CANDIDATE_MAX];
	size_t key_length;
	int error;

	/* Not in a secret field, nor without a user dictionary. */
	if (!state->learning || !state->user_open || length >= sizeof(copy))
		return;

	/* The key and the word, terminated. */
	key_length = lookup_key(frame, key, sizeof(key));
	memcpy(copy, word, length);
	copy[length] = '\0';

	/* Learned; saved later. */
	error = ja_user_learn(&state->user, key, key_length, copy);
	if (error == 0)
		state->user_unsaved = true;
}

/*
 * Appends text to a buffer of a size, keeping room for a terminator.
 * Returns false (appending nothing) when it does not fit.
 */
static bool
text_append(
	char *buffer,
	size_t size,
	size_t *length,
	const char *text,
	size_t text_length)
{
	/* No room. */
	if (*length + text_length >= size)
		return false;

	/* Appended and terminated. */
	memcpy(buffer + *length, text, text_length);
	*length += text_length;
	buffer[*length] = '\0';
	return true;
}

/*
 * Takes the last UTF-8 character off a buffer.
 */
static void
text_pop(
	char *buffer,
	size_t *length)
{
	/* Nothing to take. */
	if (*length == 0)
		return;

	/* Back over the continuation bytes to the character's first byte. */
	(*length)--;
	while (*length != 0 && ((unsigned char)buffer[*length] & 0xc0U) == 0x80U)
		(*length)--;

	/* Terminated where the character began. */
	buffer[*length] = '\0';
}

/*
 * Writes text with its hiragana as katakana when asked, as it is
 * otherwise.  Returns the length written (what fits).
 */
static size_t
form_of(
	const char *text,
	size_t length,
	bool katakana,
	char *out,
	size_t size)
{
	char bytes[8];
	size_t position;
	size_t used;
	size_t written;
	size_t encoded;
	uint32_t code;
	bool hiragana;

	/* Each character in turn. */
	position = 0;
	written = 0;
	while (position < length) {
		/* The next character; a broken byte is kept as it is. */
		used = ja_utf8_decode(text + position, length - position, &code);
		if (used == 0) {
			used = 1;
			code = (unsigned char)text[position];
		}

		/* Its katakana when asked. */
		hiragana = ja_is_hiragana(code);
		if (katakana && hiragana)
			code = ja_to_katakana(code);

		/* Written when it fits. */
		encoded = ja_utf8_encode(code, bytes);
		if (written + encoded >= size)
			break;

		/* Written. */
		memcpy(out + written, bytes, encoded);
		written += encoded;
		position += used;
	}

	/* The length written. */
	out[written] = '\0';
	return written;
}

/*
 * Fills the output's preedit and candidates from what is composed: the
 * registrations' prompts and words, then the active composition.
 */
static void
render(
	struct skk_state *state,
	struct ime_output *out)
{
	const struct skk_frame *frame;
	char key[SKK_TEXT_MAX + 2U];
	size_t key_length;
	unsigned i;

	/* Each composition from the text inward. */
	out->preedit_length = 0;
	out->preedit[0] = '\0';
	for (i = 0; i <= state->depth; i++) {
		/* The composition itself. */
		frame = &state->frames[i];
		if (i == state->depth) {
			render_frame(state, frame, out);
			break;
		}

		/* A composition waiting for a registration: its reading in the prompt, then the word so far. */
		key_length = lookup_key(frame, key, sizeof(key));
		render_append(out, SKK_MARK_REGISTER, strlen(SKK_MARK_REGISTER));
		render_append(out, key, key_length);
		render_append(out, " ", 1);
		render_append(out, state->frames[i + 1U].word, state->frames[i + 1U].word_length);
	}

	/* The cursor at the end; composing while anything is shown. */
	out->cursor_begin = (int32_t)out->preedit_length;
	out->cursor_end = (int32_t)out->preedit_length;

	/* Composing while anything is shown. */
	if (out->preedit_length != 0)
		out->composing = true;
}

/*
 * Adds one composition to the preedit: the pending letters, the reading
 * (▽) with its okurigana, or the candidate (▼); the listed candidates go
 * to the candidate window.
 */
static void
render_frame(
	const struct skk_state *state,
	const struct skk_frame *frame,
	struct ime_output *out)
{
	char shown[SKK_TEXT_MAX * 2U];
	char *label;
	size_t shown_length;
	size_t length;
	size_t page;
	size_t first;
	size_t i;
	bool katakana;

	/* The reading. */
	katakana = state->mode == SKK_MODE_KATAKANA;
	if (frame->phase == SKK_PHASE_READING) {
		render_append(out, SKK_MARK_READING, strlen(SKK_MARK_READING));
		shown_length = form_of(frame->reading, frame->reading_length, katakana, shown, sizeof(shown));
		render_append(out, shown, shown_length);

		/* The okurigana after their mark. */
		if (frame->okuri_letter != '\0') {
			render_append(out, SKK_MARK_OKURI, strlen(SKK_MARK_OKURI));
			shown_length = form_of(frame->okuri, frame->okuri_length, katakana, shown, sizeof(shown));
			render_append(out, shown, shown_length);
		}
	}

	/* The candidate and its okurigana. */
	if (frame->phase == SKK_PHASE_CHOOSING) {
		render_append(out, SKK_MARK_CHOOSING, strlen(SKK_MARK_CHOOSING));
		render_append(out, frame->candidates[frame->selected], strlen(frame->candidates[frame->selected]));
		shown_length = form_of(frame->okuri, frame->okuri_length, katakana, shown, sizeof(shown));
		render_append(out, shown, shown_length);
	}

	/* The letters not yet a kana. */
	render_append(out, state->romaji.pending, state->romaji.pending_length);

	/* The listed candidates: a page of seven, each with its key. */
	if (frame->phase != SKK_PHASE_CHOOSING || frame->selected < SKK_INLINE)
		return;

	/* The page of the candidate shown. */
	page = (frame->selected - SKK_INLINE) / SKK_PAGE;
	first = SKK_INLINE + page * SKK_PAGE;
	out->candidate_count = 0;
	for (i = first; i < frame->candidate_count && i < first + SKK_PAGE; i++) {
		/* The key, a colon, and the candidate (cut to the room left). */
		label = out->candidates[out->candidate_count];
		label[0] = SKK_CHOICE_KEYS[i - first];
		label[1] = ':';
		length = strlen(frame->candidates[i]);
		if (length > IME_CANDIDATE_MAX - 3U)
			length = IME_CANDIDATE_MAX - 3U;

		/* The candidate, terminated. */
		memcpy(label + 2, frame->candidates[i], length);
		label[2U + length] = '\0';
		out->candidate_count++;
	}

	/* Shown, with the one in focus. */
	out->candidate_selected = frame->selected - first;
	out->candidates_shown = true;
}

/*
 * Appends text to the preedit, as far as it fits.
 */
static void
render_append(
	struct ime_output *out,
	const char *text,
	size_t length)
{
	/* What fits with the terminator. */
	if (out->preedit_length + length >= IME_TEXT_MAX)
		return;

	/* Appended. */
	memcpy(out->preedit + out->preedit_length, text, length);
	out->preedit_length += length;
	out->preedit[out->preedit_length] = '\0';
}
