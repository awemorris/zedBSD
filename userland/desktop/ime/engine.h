/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The interface between the input method's Wayland side and its language
 * engines (plan/ws095/design.md sections 5 and 6).
 *
 * An engine knows no Wayland: it is given one key at a time and fills an
 * output that says what to commit, what to show as the preedit, which
 * candidates to offer, and whether the key was not used and goes back to
 * the application.  The host tests drive the engines through this header
 * alone.
 */

#ifndef IME_ENGINE_H
#define IME_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Marks an argument a function is given but does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/*
 * The longest text one message may carry (input-method-unstable-v2 limits
 * commit_string and set_preedit_string to 4000 bytes), with room for the
 * terminating NUL.
 */
#define IME_TEXT_MAX		4000U

/* The most candidates offered for one segment, and the longest one. */
#define IME_CANDIDATES_MAX	40U
#define IME_CANDIDATE_MAX	160U

/*
 * The content hints and purposes of text-input-unstable-v3 an engine acts
 * on: a field whose text is secret teaches the engine nothing.
 */
#define IME_HINT_HIDDEN_TEXT	0x40U
#define IME_HINT_SENSITIVE_DATA	0x80U
#define IME_PURPOSE_PASSWORD	8U
#define IME_PURPOSE_PIN		9U

/* The modifiers held with a key. */
#define IME_MOD_SHIFT		0x01U
#define IME_MOD_CTRL		0x02U
#define IME_MOD_ALT		0x04U
#define IME_MOD_SUPER		0x08U

/*
 * The evdev codes of the keys the engines know by name.  They are the
 * codes of <uapi/input.h>, repeated here so that the engines build on the
 * host as well.
 */
#define IME_KEY_ESCAPE		1U
#define IME_KEY_1		2U
#define IME_KEY_9		10U
#define IME_KEY_BACKSPACE	14U
#define IME_KEY_TAB		15U
#define IME_KEY_ENTER		28U
#define IME_KEY_SPACE		57U
#define IME_KEY_F6		64U
#define IME_KEY_F7		65U
#define IME_KEY_F8		66U
#define IME_KEY_F9		67U
#define IME_KEY_F10		68U
#define IME_KEY_HENKAN		92U
#define IME_KEY_KP_ENTER	96U
#define IME_KEY_HOME		102U
#define IME_KEY_UP		103U
#define IME_KEY_PAGE_UP		104U
#define IME_KEY_LEFT		105U
#define IME_KEY_RIGHT		106U
#define IME_KEY_END		107U
#define IME_KEY_DOWN		108U
#define IME_KEY_PAGE_DOWN	109U
#define IME_KEY_DELETE		111U

struct ime_engine;

/*
 * One key pressed on the keyboard, as the Wayland side hands it to an
 * engine.
 *
 * The character is what the key types on the US layout with the shift
 * state it was pressed in (0 for a key that types none), so that an
 * engine need not carry a layout of its own.
 */
struct ime_key {
	uint32_t code;
	uint32_t character;
	uint32_t modifiers;
};

/*
 * What one key, or one reset, made an engine do.
 *
 * The Wayland side turns it into input-method requests: the commit text
 * into commit_string, the preedit into set_preedit_string with its cursor
 * range (the segment in focus while converting), the candidates into the
 * popup, and pass_key into giving the key back on the virtual keyboard.
 * The preedit is the whole of what is being composed after this key, not
 * a change to the last one.
 */
struct ime_output {
	char commit[IME_TEXT_MAX];
	size_t commit_length;
	char preedit[IME_TEXT_MAX];
	size_t preedit_length;
	int32_t cursor_begin;
	int32_t cursor_end;
	char candidates[IME_CANDIDATES_MAX][IME_CANDIDATE_MAX];
	size_t candidate_count;
	size_t candidate_selected;
	bool candidates_shown;
	bool composing;
	bool pass_key;
};

/*
 * The functions one language engine provides.
 *
 * key interprets a key and fills the output; reset ends whatever is being
 * composed, either committing it into the output (on switching languages)
 * or dropping it (on deactivation, when the compositor has already committed the
 * preedit itself); surrounding tells the engine the text around the
 * cursor; content_type tells it what the field holds (the hints and the
 * purpose of text-input-v3); save writes what the engine has learned to
 * its file, away from the keys (the Wayland side calls it once no key has
 * come for a while); destroy writes what is still unsaved and frees the
 * engine.  An engine with modes that are languages of their own to the
 * desktop (SKK's kana, katakana, Latin and wide Latin, WS154) gives the ID
 * and label of the mode it is in (mode), and takes a mode by its ID
 * (select: true when the ID is one of its modes); the others leave both
 * NULL and are one language, id and label.  An engine that predicts words
 * for the on-screen keyboard (the Japanese one, ws166-p002) gives the
 * words a reading of kana starts as "WORD\tREADING" lines in list
 * (predict: the list's length, 0 for none) and learns the word chosen for
 * a reading (learn); the others leave both NULL.
 */
struct ime_engine_ops {
	const char *id;
	const char *label;
	void (*key)(struct ime_engine *engine, const struct ime_key *key, struct ime_output *out);
	void (*reset)(struct ime_engine *engine, bool commit, struct ime_output *out);
	void (*surrounding)(struct ime_engine *engine, const char *text, uint32_t cursor, uint32_t anchor);
	void (*content_type)(struct ime_engine *engine, uint32_t hint, uint32_t purpose);
	void (*save)(struct ime_engine *engine);
	void (*destroy)(struct ime_engine *engine);
	const char *(*mode)(struct ime_engine *engine, const char **label);
	bool (*select)(struct ime_engine *engine, const char *id);
	size_t (*predict)(struct ime_engine *engine, const char *reading, char *list, size_t size);
	void (*learn)(struct ime_engine *engine, const char *reading, const char *word);
};

/*
 * One language engine: its functions and its own state.
 *
 * The Wayland side keeps one per language named in ime.conf, for the
 * input method's lifetime.
 */
struct ime_engine {
	const struct ime_engine_ops *ops;
	void *state;
};

/*
 * Where the Japanese engine finds its dictionaries.
 *
 * The user dictionary is read at start and rewritten when the engine is
 * told to save and when it is destroyed, not at each learned choice
 * (BUG-143); the supplement is optional (NULL for none) and is looked in
 * before the system dictionary.  Without one, the system dictionary may be
 * Kei's file of two parts (ws095-p017, SKK-JISYO.ja), whose first part is
 * the supplement; the input method gives that one file alone.
 */
struct ja_config {
	const char *system_dictionary;
	const char *supplement_dictionary;
	const char *user_dictionary;
};

void ime_output_clear(struct ime_output *out);
int ime_output_append_commit(struct ime_output *out, const char *text, size_t length);

int ime_direct_create(struct ime_engine *engine);
int ja_engine_create(struct ime_engine *engine, const struct ja_config *config);

#endif
