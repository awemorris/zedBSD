/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SKK engine (WS154 p003): the way of typing Japanese of Emacs's SKK
 * (ddskk).  Kana are typed and go straight in; an upper-case letter starts
 * a word to convert (the ▽ reading), a second one marks where its
 * okurigana start, Space converts it (the ▼ candidates) and the word is
 * put in with Enter or by typing on.  A word no dictionary has is taught
 * in a registration, where the word is typed (and may itself be converted)
 * and kept in the user's dictionary.
 *
 * It uses the Japanese engine's romaji, kana and dictionary parts (ja.h):
 * the dictionaries are SKK's own format.
 */

#ifndef IME_SKK_H
#define IME_SKK_H

#include "ja.h"

/* The longest reading, okurigana or registered word, in bytes. */
#define SKK_TEXT_MAX		256U

/* The compositions one may be inside: the text itself and up to three registrations in each other. */
#define SKK_FRAMES_MAX		4U

/* The candidates shown one by one before the rest are listed, and how many are listed at a time. */
#define SKK_INLINE		4U
#define SKK_PAGE		7U

/* The most system dictionaries looked in. */
#define SKK_DICTS_MAX		2U

/* The input modes. */
enum skk_mode {
	SKK_MODE_KANA,
	SKK_MODE_KATAKANA,
	SKK_MODE_LATIN,
	SKK_MODE_WIDE
};

/* What a composition is doing: typing kana straight in, typing a reading (▽), or choosing (▼). */
enum skk_phase {
	SKK_PHASE_INPUT,
	SKK_PHASE_READING,
	SKK_PHASE_CHOOSING
};

/*
 * One composition: the text being typed and, inside a registration, the
 * word typed so far.  While a reading is typed, the reading (hiragana), the
 * okurigana and the letter that started them (0 for none); while choosing,
 * the candidates and the one shown.
 */
struct skk_frame {
	enum skk_phase phase;
	char reading[SKK_TEXT_MAX];
	size_t reading_length;
	char okuri[SKK_TEXT_MAX];
	size_t okuri_length;
	char okuri_letter;
	char candidates[IME_CANDIDATES_MAX][IME_CANDIDATE_MAX];
	size_t candidate_count;
	size_t selected;
	char word[SKK_TEXT_MAX];
	size_t word_length;
};

/*
 * The engine's state: the mode, the letters not yet a kana, the
 * compositions (frames[0] is the text, the others registrations, the last
 * one the active one), the dictionaries, the user's dictionary, and whether
 * this field may teach it (not a password's).
 */
struct skk_state {
	enum skk_mode mode;
	struct ja_romaji romaji;
	struct skk_frame frames[SKK_FRAMES_MAX];
	unsigned depth;
	struct ja_dict dicts[SKK_DICTS_MAX];
	size_t dict_count;
	struct ja_user user;
	bool user_open;
	bool user_unsaved;
	bool learning;
};

/*
 * Where the SKK engine finds its dictionaries: the system ones, looked in
 * in order (NULL for none), and the user's, read at the start and written
 * when the engine is told to save and when it goes.
 */
struct skk_config {
	const char *dictionaries[SKK_DICTS_MAX];
	const char *user_dictionary;
};

int skk_engine_create(struct ime_engine *engine, const struct skk_config *config);

#endif
