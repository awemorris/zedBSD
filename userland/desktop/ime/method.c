/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The input method's protocol side (ws095-p004, plan/ws095/design.md
 * sections 4 to 6): the input method object, its keyboard grab, the
 * virtual keyboard it gives unused keys back on, and the compositor's status.
 *
 * Each key press the grab hears goes to the language chosen; what the
 * engine makes becomes set_preedit_string, commit_string and a commit,
 * which is sent for every press so that compositor hears an answer.  A key
 * the engine does not use goes back on the virtual keyboard, and so does
 * its release.  Alt+Space (the compositor's next) commits what is composed and
 * chooses the next language; the compositor hears the language and whether text
 * is being composed on the status.  A deactivation drops what is composed:
 * The compositor has already committed the preedit to the application.  Nothing
 * typed is written to the log.
 */

#include "program.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The wl_keyboard key states. */
#define METHOD_KEY_RELEASED	0U
#define METHOD_KEY_PRESSED	1U

/* The evdev codes of the keys never repeated: the modifiers, Enter, keypad Enter and Escape. */
#define METHOD_KEY_LEFTCTRL	29U
#define METHOD_KEY_LEFTSHIFT	42U
#define METHOD_KEY_RIGHTSHIFT	54U
#define METHOD_KEY_LEFTALT	56U
#define METHOD_KEY_RIGHTCTRL	97U
#define METHOD_KEY_RIGHTALT	100U
#define METHOD_KEY_LEFTMETA	125U
#define METHOD_KEY_RIGHTMETA	126U

/*
 * The room of the on-screen keyboard's words for one reading (ws166-p002):
 * a Wayland message carries at most 4096 bytes with its header and the
 * serial.
 */
#define METHOD_PREDICT_LIST	3800U

static void method_activate(void *data, struct zwp_input_method_v2 *method);
static void method_deactivate(void *data, struct zwp_input_method_v2 *method);
static void method_surrounding_text(void *data, struct zwp_input_method_v2 *method, const char *text, uint32_t cursor, uint32_t anchor);
static void method_text_change_cause(void *data, struct zwp_input_method_v2 *method, uint32_t cause);
static void method_content_type(void *data, struct zwp_input_method_v2 *method, uint32_t hint, uint32_t purpose);
static void method_done(void *data, struct zwp_input_method_v2 *method);
static void method_unavailable(void *data, struct zwp_input_method_v2 *method);
static void grab_keymap(void *data, struct zwp_input_method_keyboard_grab_v2 *grab, uint32_t format, int32_t fd, uint32_t size);
static void grab_key(void *data, struct zwp_input_method_keyboard_grab_v2 *grab, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
static void grab_modifiers(void *data, struct zwp_input_method_keyboard_grab_v2 *grab, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
static void grab_repeat_info(void *data, struct zwp_input_method_keyboard_grab_v2 *grab, int32_t rate, int32_t delay);
static void status_next(void *data, struct kl_ime_status_v1 *status);
static void status_select(void *data, struct kl_ime_status_v1 *status, const char *id);
static void status_predict(void *data, struct kl_ime_status_v1 *status, uint32_t serial, const char *reading);
static void status_learn(void *data, struct kl_ime_status_v1 *status, const char *reading, const char *word);
static void method_send(struct program *program);
static void method_press(struct program *program, uint32_t time, uint32_t key, int repeated);
static int method_repeats(uint32_t key);
static void method_choose(struct program *program, unsigned index);
static void method_follow_mode(struct program *program);

/*
 * The input method's events.
 */
static const struct zwp_input_method_v2_listener method_listener = {
	method_activate,
	method_deactivate,
	method_surrounding_text,
	method_text_change_cause,
	method_content_type,
	method_done,
	method_unavailable
};

/*
 * The keyboard grab's events.
 */
static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = {
	grab_keymap,
	grab_key,
	grab_modifiers,
	grab_repeat_info
};

/*
 * The compositor's status events.
 */
static const struct kl_ime_status_v1_listener status_listener = {
	status_next,
	status_select,
	status_predict,
	status_learn
};

/*
 * Makes the input method, its keyboard grab, the virtual keyboard and the
 * status, and tells the compositor the language.
 *
 * Returns 0, or -1 when an object cannot be made.
 */
int
program_method_start(
	struct program *program)
{
	int status;

	/* The input method of the seat. */
	program->method = zwp_input_method_manager_v2_get_input_method(program->method_manager, program->seat);
	if (program->method == NULL)
		return -1;

	status = zwp_input_method_v2_add_listener(program->method, &method_listener, program);
	if (status != 0)
		return -1;

	/* The keyboard, whose keys the compositor sends while a text input is served. */
	program->grab = zwp_input_method_v2_grab_keyboard(program->method);
	if (program->grab == NULL)
		return -1;

	status = zwp_input_method_keyboard_grab_v2_add_listener(program->grab, &grab_listener, program);
	if (status != 0)
		return -1;

	/* The virtual keyboard the unused keys go back on. */
	program->keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(program->keyboard_manager, program->seat);
	if (program->keyboard == NULL)
		return -1;

	/* The compositor's status. */
	program->status = kl_ime_status_manager_v1_get_status(program->status_manager);
	if (program->status == NULL)
		return -1;

	status = kl_ime_status_v1_add_listener(program->status, &status_listener, program);
	if (status != 0)
		return -1;

	/* Succeeded: the compositor hears the language chosen. */
	program_announce_language(program);
	return 0;
}

/*
 * Tells the compositor the language chosen and its label.
 */
void
program_announce_language(
	struct program *program)
{
	const struct ime_engine_ops *ops;
	struct ime_engine *engine;
	const char *id;
	const char *label;

	/* The language's ID and its short label: the engine's mode's when it has modes (WS154). */
	engine = &program->engines[program->current];
	ops = engine->ops;
	id = ops->id;
	label = ops->label;
	if (ops->mode != NULL)
		id = ops->mode(engine, &label);

	/* Told to the compositor, and kept so that a change of mode is told too. */
	kl_ime_status_v1_language(program->status, id, label);
	snprintf(program->announced, sizeof(program->announced), "%s", id);
	printf("KEI-IME LANGUAGE id=%s\n", id);
}

/*
 * Gives how long main's loop may wait before a held key's next press is
 * due, in milliseconds: 0 when it is due, -1 when no key repeats.
 */
int
program_repeat_timeout(
	const struct program *program,
	unsigned long long now_ms)
{
	unsigned long long left;

	/* No key held, or no repeat. */
	if (program->repeat.key == 0U)
		return -1;

	/* Due already. */
	if (now_ms >= program->repeat.next_ms)
		return 0;

	/* The time left. */
	left = program->repeat.next_ms - now_ms;
	return (int)left;
}

/*
 * Presses the held key again when its next press is due, as the compositor would
 * repeat a key for an application.
 */
void
program_repeat_due(
	struct program *program,
	unsigned long long now_ms)
{
	uint32_t interval;

	/* Nothing held, or not yet due. */
	if (program->repeat.key == 0U)
		return;
	if (now_ms < program->repeat.next_ms)
		return;

	/* The next one an interval after this one was due (BUG-172), from now when the loop fell a whole interval behind. */
	interval = 1000U / (uint32_t)program->repeat.rate;
	program->repeat.next_ms += interval;
	if (program->repeat.next_ms <= now_ms)
		program->repeat.next_ms = now_ms + interval;
	program->repeat.time += interval;

	/* The press again. */
	method_press(program, program->repeat.time, program->repeat.key, 1);
}

/*
 * Gives how long main's loop may wait before the engines are due to save,
 * in milliseconds: 0 when it is due, -1 when nothing waits to be saved.
 */
int
program_save_timeout(
	const struct program *program,
	unsigned long long now_ms)
{
	unsigned long long left;

	/* No key came since the last save. */
	if (program->save_ms == 0U)
		return -1;

	/* Due already. */
	if (now_ms >= program->save_ms)
		return 0;

	/* The time left, at most PROGRAM_SAVE_IDLE_MS, which an int holds. */
	left = program->save_ms - now_ms;
	return (int)left;
}

/*
 * Tells every engine to save what it learned once no key has come for
 * PROGRAM_SAVE_IDLE_MS; the engines write by a thread of their own, so the
 * loop does not wait for the disk.
 */
void
program_save_due(
	struct program *program,
	unsigned long long now_ms)
{
	unsigned i;

	/* Nothing to save, or a key came too recently. */
	if (program->save_ms == 0U)
		return;
	if (now_ms < program->save_ms)
		return;

	/* Each engine writes what it learned. */
	for (i = 0; i < program->engine_count; i++)
		program->engines[i].ops->save(&program->engines[i]);

	/* Saved: the next key starts the wait again. */
	program->save_ms = 0;
}

/*
 * Gives a steady clock in milliseconds, for the repeat and the save.
 */
unsigned long long
program_clock_ms(void)
{
	struct timespec now;
	int status;

	/* The monotonic clock. */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return 0;

	/* Succeeded: the milliseconds. */
	return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)now.tv_nsec / 1000000ULL;
}

/*
 * Notes an activation, applied at done.
 */
static void
method_activate(
	void *data,
	struct zwp_input_method_v2 *method)
{
	struct program *program;

	UNUSED_PARAMETER(method);

	/* A text input is served from the next done. */
	program = data;
	program->pending_active = 1;
	program->pending_active_set = 1;
}

/*
 * Notes a deactivation, applied at done.
 */
static void
method_deactivate(
	void *data,
	struct zwp_input_method_v2 *method)
{
	struct program *program;

	UNUSED_PARAMETER(method);

	/* No text input is served from the next done. */
	program = data;
	program->pending_active = 0;
	program->pending_active_set = 1;
}

/*
 * Takes the text around the cursor, which the engines do not use yet.
 */
static void
method_surrounding_text(
	void *data,
	struct zwp_input_method_v2 *method,
	const char *text,
	uint32_t cursor,
	uint32_t anchor)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(method);
	UNUSED_PARAMETER(text);
	UNUSED_PARAMETER(cursor);
	UNUSED_PARAMETER(anchor);
}

/*
 * Takes what changed the text last, which the engines do not use yet.
 */
static void
method_text_change_cause(
	void *data,
	struct zwp_input_method_v2 *method,
	uint32_t cause)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(method);
	UNUSED_PARAMETER(cause);
}

/*
 * Notes the field's content type, applied at done.
 */
static void
method_content_type(
	void *data,
	struct zwp_input_method_v2 *method,
	uint32_t hint,
	uint32_t purpose)
{
	struct program *program;

	UNUSED_PARAMETER(method);

	/* The hints and the purpose wait for done. */
	program = data;
	program->pending_hint = hint;
	program->pending_purpose = purpose;
}

/*
 * Applies the state the compositor sent: an activation or a deactivation starts
 * the engines from nothing, and the content type tells them whether to
 * learn.
 */
static void
method_done(
	void *data,
	struct zwp_input_method_v2 *method)
{
	struct program *program;
	unsigned i;

	UNUSED_PARAMETER(method);

	/* The number of dones is the serial of the next commit. */
	program = data;
	program->done_count++;

	/* A change of the text input served drops what was being composed. */
	if (program->pending_active_set) {
		program->pending_active_set = 0;
		program->active = program->pending_active;
		for (i = 0; i < program->engine_count; i++)
			program->engines[i].ops->reset(&program->engines[i], false, program->out);
		program->composing = 0;
		kl_ime_status_v1_composing(program->status, 0);
		printf("KEI-IME %s\n", program->active ? "ACTIVATE" : "DEACTIVATE");

		/* The candidate window goes, and no key repeats into the new text input. */
		program->repeat.key = 0;
		program_popup_update(program, program->out);
	}

	/* A secret field teaches nothing (the engines' content_type). */
	for (i = 0; i < program->engine_count; i++)
		program->engines[i].ops->content_type(&program->engines[i], program->pending_hint, program->pending_purpose);
}

/*
 * Ends the program: another input method holds the seat.
 */
static void
method_unavailable(
	void *data,
	struct zwp_input_method_v2 *method)
{
	struct program *program;

	UNUSED_PARAMETER(method);

	/* main's loop ends on this. */
	program = data;
	program->unavailable = 1;
	printf("KEI-IME UNAVAILABLE\n");
}

/*
 * Gives the virtual keyboard the grab's keymap, as the protocol asks before
 * any key.
 */
static void
grab_keymap(
	void *data,
	struct zwp_input_method_keyboard_grab_v2 *grab,
	uint32_t format,
	int32_t fd,
	uint32_t size)
{
	struct program *program;

	UNUSED_PARAMETER(grab);

	/* The first keymap goes to the virtual keyboard; the descriptor is ours to close. */
	program = data;
	if (!program->keymap_sent && program->keyboard != NULL) {
		zwp_virtual_keyboard_v1_keymap(program->keyboard, format, fd, size);
		program->keymap_sent = 1;
	}

	close(fd);
}

/*
 * Gives one key to the language chosen, sends what it made, and gives the
 * key back when the language does not use it.
 */
static void
grab_key(
	void *data,
	struct zwp_input_method_keyboard_grab_v2 *grab,
	uint32_t serial,
	uint32_t time,
	uint32_t key,
	uint32_t state)
{
	struct program *program;

	UNUSED_PARAMETER(grab);
	UNUSED_PARAMETER(serial);

	program = data;

	/* A release goes back where its press went, and ends the repeat of its key. */
	if (state == METHOD_KEY_RELEASED) {
		if (key == program->repeat.key)
			program->repeat.key = 0;

		if (key < PROGRAM_KEYS && program->passed[key]) {
			program->passed[key] = 0;
			zwp_virtual_keyboard_v1_key(program->keyboard, time, key, METHOD_KEY_RELEASED);
		}

		return;
	}

	/* A press. */
	method_press(program, time, key, 0);
}

/*
 * Keeps the modifiers held.
 */
static void
grab_modifiers(
	void *data,
	struct zwp_input_method_keyboard_grab_v2 *grab,
	uint32_t serial,
	uint32_t depressed,
	uint32_t latched,
	uint32_t locked,
	uint32_t group)
{
	struct program *program;

	UNUSED_PARAMETER(grab);
	UNUSED_PARAMETER(serial);
	UNUSED_PARAMETER(latched);
	UNUSED_PARAMETER(locked);
	UNUSED_PARAMETER(group);

	/* Shift chooses the characters; the others tell the engines a shortcut. */
	program = data;
	program->modifiers = depressed;
}

/*
 * Keeps the repeat the compositor tells: presses per second (none when zero) and
 * the wait before the first.
 */
static void
grab_repeat_info(
	void *data,
	struct zwp_input_method_keyboard_grab_v2 *grab,
	int32_t rate,
	int32_t delay)
{
	struct program *program;

	UNUSED_PARAMETER(grab);

	/* Kept for the next key held. */
	program = data;
	program->repeat.rate = rate;
	program->repeat.delay = delay;
}

/*
 * Chooses the next language (Alt+Space).
 */
static void
status_next(
	void *data,
	struct kl_ime_status_v1 *status)
{
	struct program *program;

	UNUSED_PARAMETER(status);

	/* The one after the language chosen, round the list. */
	program = data;
	method_choose(program, (program->current + 1U) % program->engine_count);
}

/*
 * Chooses a language by its ID (the Japanese keyboard's keys).
 */
static void
status_select(
	void *data,
	struct kl_ime_status_v1 *status,
	const char *id)
{
	struct program *program;
	unsigned i;
	int order;
	bool taken;

	UNUSED_PARAMETER(status);

	/* Looks for the language, or an engine that has it as one of its modes (WS154). */
	program = data;
	for (i = 0; i < program->engine_count; i++) {
		/* The engine's own ID. */
		order = strcmp(program->engines[i].ops->id, id);
		if (order == 0) {
			method_choose(program, i);
			return;
		}

		/* Asks an engine with modes whether the ID is one of them. */
		taken = false;
		if (program->engines[i].ops->select != NULL)
			taken = program->engines[i].ops->select(&program->engines[i], id);

		/* One of its modes: chosen, then the mode told (method_choose tells only a change of engine). */
		if (taken) {
			method_choose(program, i);
			program_announce_language(program);
			return;
		}
	}

	/* An unknown language is ignored. */
	printf("KEI-IME SELECT unknown\n");
}

/*
 * Sends what the engine made: the text to commit, the preedit and its
 * cursor, and the commit that applies them; the compositor hears whether text is
 * being composed.
 */
static void
method_send(
	struct program *program)
{
	struct ime_output *out;
	unsigned composing;

	out = program->out;

	/* The text to commit, when there is some. */
	if (out->commit_length != 0U)
		zwp_input_method_v2_commit_string(program->method, out->commit);

	/* The preedit (empty for none) and the commit, naming the dones heard. */
	zwp_input_method_v2_set_preedit_string(program->method, out->preedit, out->cursor_begin, out->cursor_end);
	zwp_input_method_v2_commit(program->method, program->done_count);

	/* Whether text is being composed decides where the compositor puts the menu keys. */
	composing = 0;
	if (out->composing)
		composing = 1;
	if (composing != program->composing) {
		program->composing = composing;
		kl_ime_status_v1_composing(program->status, composing);
	}

	/* The candidate window shows the engine's candidates, or goes (popup.c). */
	program_popup_update(program, out);
}

/*
 * Tells the compositor the language again when the engine chosen has changed its
 * mode since it was last told (WS154).
 */
static void
method_follow_mode(
	struct program *program)
{
	struct ime_engine *engine;
	const char *id;
	const char *label;
	int same;

	/* Only an engine with modes. */
	engine = &program->engines[program->current];
	if (engine->ops->mode == NULL)
		return;

	/* The mode it is in, told when it is not the one told last. */
	id = engine->ops->mode(engine, &label);
	same = strcmp(id, program->announced);
	if (same != 0)
		program_announce_language(program);
}

/*
 * Chooses a language: what the old one composes is committed first.
 */
static void
method_choose(
	struct program *program,
	unsigned index)
{
	struct ime_engine *engine;

	/* The same language needs nothing. */
	if (index == program->current)
		return;

	/* The old language commits what it composes. */
	engine = &program->engines[program->current];
	engine->ops->reset(engine, true, program->out);
	if (program->active)
		method_send(program);

	/* The new language, told to the compositor. */
	program->current = index;
	program_announce_language(program);
}

/*
 * Gives one pressed key (or a repeat of the held one) to the language
 * chosen, sends what it made, gives the key back when the language does not
 * use it, and starts the key's repeat when the language used it.
 */
static void
method_press(
	struct program *program,
	uint32_t time,
	uint32_t key,
	int repeated)
{
	struct ime_engine *engine;
	struct ime_key typed;
	int repeats;

	/* The key as the engines see it. */
	typed.code = key;
	typed.character = program_key_character(key, program->modifiers);
	typed.modifiers = program_key_modifiers(program->modifiers);

	/* The language chosen acts on it; a key heard while no text input is served goes back. */
	engine = &program->engines[program->current];
	if (program->active) {
		engine->ops->key(engine, &typed, program->out);

		/* The engines save what they learned once no key has come for a while after this one. */
		program->save_ms = program_clock_ms() + PROGRAM_SAVE_IDLE_MS;
	} else {
		ime_output_clear(program->out);
		program->out->pass_key = true;
	}

	/* What it made (always sent, so that compositor hears an answer). */
	method_send(program);

	/* A key that changed the engine's mode (SKK's q, l, C-j) tells the compositor the new language. */
	method_follow_mode(program);

	/* A key the language does not use goes back to the application, which repeats it itself. */
	if (program->out->pass_key) {
		program->repeat.key = 0;
		if (repeated)
			return;

		zwp_virtual_keyboard_v1_key(program->keyboard, time, key, METHOD_KEY_PRESSED);
		if (key < PROGRAM_KEYS)
			program->passed[key] = 1;
		return;
	}

	/* A repeat goes on at its own pace. */
	if (repeated)
		return;

	/* A key the language used repeats while it is held, after the delay. */
	repeats = method_repeats(key);
	if (!repeats || program->repeat.rate <= 0) {
		program->repeat.key = 0;
		return;
	}

	program->repeat.key = key;
	program->repeat.time = time;
	program->repeat.next_ms = program_clock_ms() + (unsigned long long)program->repeat.delay;
}

/*
 * Tells whether a key repeats while held: not a modifier, Enter or Escape.
 */
static int
method_repeats(
	uint32_t key)
{
	/* The keys a held press must not do again. */
	switch (key) {
	case METHOD_KEY_LEFTCTRL:
	case METHOD_KEY_LEFTSHIFT:
	case METHOD_KEY_RIGHTSHIFT:
	case METHOD_KEY_LEFTALT:
	case METHOD_KEY_RIGHTCTRL:
	case METHOD_KEY_RIGHTALT:
	case METHOD_KEY_LEFTMETA:
	case METHOD_KEY_RIGHTMETA:
	case IME_KEY_ENTER:
	case IME_KEY_KP_ENTER:
	case IME_KEY_ESCAPE:
		/* Never repeated. */
		return 0;
	default:
		break;
	}

	/* Any other key repeats. */
	return 1;
}

/*
 * Answers the compositor's request for the on-screen keyboard's words for a
 * reading (ws166-p002): the first engine that predicts gives them, or none
 * when no engine does (SKK, or no input method chosen).
 */
static void
status_predict(
	void *data,
	struct kl_ime_status_v1 *status,
	uint32_t serial,
	const char *reading)
{
	struct program *program;
	struct ime_engine *engine;
	char list[METHOD_PREDICT_LIST];
	size_t length;
	unsigned i;

	/* No words yet. */
	program = data;
	list[0] = '\0';
	length = 0;

	/* The first engine that predicts. */
	for (i = 0; i < program->engine_count; i++) {
		engine = &program->engines[i];
		if (engine->ops->predict == NULL)
			continue;
		length = engine->ops->predict(engine, reading, list, sizeof(list));
		break;
	}

	/* The answer, with the request's serial. */
	kl_ime_status_v1_predictions(status, serial, list);
	printf("KEI-IME PREDICT serial=%u reading=%s bytes=%zu\n", serial, reading, length);
}

/*
 * Learns the word chosen on the on-screen keyboard for a reading
 * (ws166-p002), in the first engine that predicts; it is saved with the
 * engine's other choices once no key has come for a while.
 */
static void
status_learn(
	void *data,
	struct kl_ime_status_v1 *status,
	const char *reading,
	const char *word)
{
	struct program *program;
	struct ime_engine *engine;
	unsigned i;

	UNUSED_PARAMETER(status);

	/* The first engine that predicts. */
	program = data;
	for (i = 0; i < program->engine_count; i++) {
		engine = &program->engines[i];
		if (engine->ops->learn == NULL)
			continue;
		engine->ops->learn(engine, reading, word);
		program->save_ms = program_clock_ms() + PROGRAM_SAVE_IDLE_MS;
		break;
	}

	/* The log line the tests read. */
	printf("KEI-IME LEARN reading=%s word=%s\n", reading, word);
}
