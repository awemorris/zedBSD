/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of the Japanese engine (plan/ws095/design.md section 7).
 *
 * ja-romaji.c turns typed letters into kana; ja-kana.c knows the kana
 * (katakana, half width, rows and consonants); ja-dict.c reads the SKK
 * dictionaries; ja-user.c keeps the choices the user made; ja-inflect.c
 * knows how verbs and adjectives end; ja-segment.c splits a reading into
 * segments and lists each one's candidates; ja-engine.c holds what is
 * being composed and the operations on it; ja-keys.c binds keys to those
 * operations.  Only ja-keys.c knows which key does what, so that another
 * way of converting can replace it alone.
 */

#ifndef IME_JA_H
#define IME_JA_H

#include "engine.h"

/* The most characters one composition holds. */
#define JA_UNITS_MAX		200U

/* The longest run of letters one typed character may stand for. */
#define JA_RAW_MAX		8U

/* The longest reading the dictionaries are looked up with, in characters. */
#define JA_HEADWORD_MAX		16U

/* The largest system dictionary read, and the largest user dictionary. */
#define JA_DICT_SIZE_MAX	(8UL * 1024UL * 1024UL)
#define JA_USER_SIZE_MAX	(1024UL * 1024UL)

/* The most readings the user dictionary keeps, and candidates per reading. */
#define JA_USER_ENTRIES_MAX	10000U
#define JA_USER_CANDIDATES_MAX	8U

/*
 * What one character of a composition is.
 *
 * Kana are converted; a literal (a letter typed with shift, a digit, a
 * punctuation mark) is kept as it is and ends any segment it touches.
 */
enum ja_unit_kind {
	JA_UNIT_KANA,
	JA_UNIT_LITERAL
};

/*
 * How a segment is to be written when the user asks for one form of it
 * with a function key rather than for a candidate.
 */
enum ja_form {
	JA_FORM_HIRAGANA,
	JA_FORM_KATAKANA,
	JA_FORM_HALF_KATAKANA,
	JA_FORM_FULL_ASCII,
	JA_FORM_HALF_ASCII
};

/*
 * One character of what is being composed.
 *
 * The raw letters are the keys that typed it (the "kya" of きゃ is kept
 * on き, and ゃ has none), so that the composition can be given back as
 * the letters typed.
 */
struct ja_unit {
	uint32_t code;
	enum ja_unit_kind kind;
	char raw[JA_RAW_MAX];
};

/*
 * The letters typed that do not yet make a kana.
 *
 * It lives in the engine for as long as the composition does, and is
 * empty between compositions.
 */
struct ja_romaji {
	char pending[JA_RAW_MAX];
	size_t pending_length;
};

/*
 * The characters one romaji step produced.
 */
struct ja_romaji_result {
	struct ja_unit units[JA_RAW_MAX];
	size_t count;
};

/*
 * A composition as the segmenter reads it: its characters in UTF-8, where
 * each one starts, and which are kana.
 *
 * It is built from the composition before each split and not changed
 * while it is read.
 */
struct ja_text {
	char bytes[JA_UNITS_MAX * 4U + 1U];
	size_t length;
	size_t offsets[JA_UNITS_MAX + 1U];
	bool kana[JA_UNITS_MAX];
	size_t unit_count;
};

/*
 * How a verb's or an adjective's candidate conjugates, as the candidate's
 * SKK annotation says (たべr /食べ;一段/, かえr /帰;五段/変え;一段/).
 *
 * The supplement writes a verb's stem with every kana that does not
 * change, so that its headword letter is the row of the kana that
 * conjugates (五段), the る of an ichidan verb (一段) or the い of an
 * adjective (形容詞).  A candidate without such an annotation, as every
 * one of SKK-JISYO.X is, may take every okurigana the rules know.
 */
enum ja_conjugation {
	JA_CONJUGATION_ANY,
	JA_CONJUGATION_GODAN,
	JA_CONJUGATION_ICHIDAN,
	JA_CONJUGATION_ADJECTIVE
};

/*
 * One headword of an SKK dictionary: the reading (with the consonant of
 * the okurigana for a verb or an adjective) and its candidates as the
 * dictionary writes them ("/a/b/").
 */
struct ja_dict_entry {
	const char *key;
	size_t key_length;
	const char *candidates;
	size_t candidates_length;
};

/*
 * One SKK dictionary read into memory.
 *
 * The file's bytes are kept for the dictionary's lifetime; the entries
 * point into them, in a table hashed by headword.
 */
struct ja_dict {
	char *data;
	size_t size;
	struct ja_dict_entry *slots;
	size_t slot_count;
	size_t entry_count;
	size_t malformed_count;
};

/*
 * One reading the user has converted, and the candidates chosen for it,
 * most recent first.
 */
struct ja_user_entry {
	char *reading;
	char *candidates[JA_USER_CANDIDATES_MAX];
	size_t candidate_count;
	uint64_t stamp;
};

/*
 * The user's dictionary of choices.
 *
 * The entries are kept in a table hashed by reading.  The learned
 * choices are written back to the file, which is replaced as a whole,
 * when the engine is told to save and when it closes.
 */
struct ja_user {
	char path[1024];
	struct ja_user_entry *slots;
	size_t slot_count;
	size_t entry_count;
	uint64_t stamp;
	size_t malformed_count;
};

/* The most predictions offered for a reading (one page of the candidate window), and the longest reading kept with one. */
#define JA_PREDICT_MAX		9U

/* The most words offered to the on-screen keyboard for a reading (its candidates' tab, ws166-p002). */
#define JA_PREDICT_KEYBOARD_MAX	12U
#define JA_PREDICT_READING_MAX	(JA_HEADWORD_MAX * 4U + 1U)

/*
 * One prediction (ws166-p002): a word whose reading starts with what is
 * typed, and that reading (so that the choice can be learned).
 */
struct ja_prediction {
	char text[IME_CANDIDATE_MAX];
	char reading[JA_PREDICT_READING_MAX];
};

/*
 * A dictionary's headwords without okurigana, in the byte order of their
 * readings, for finding every reading that starts with a prefix by a binary
 * search (ws166-p002).  The entries are the dictionary's own and live as
 * long as it does.
 */
struct ja_predict_index {
	const struct ja_dict_entry **entries;
	size_t count;
};

/*
 * Everything a reading is looked up in, in the order it is looked up.
 */
struct ja_lexicon {
	const struct ja_user *user;
	const struct ja_dict *dicts[2];
	size_t dict_count;
};

/*
 * One segment of a conversion: where it lies in the composition, its
 * candidates and the one chosen.
 */
struct ja_segment {
	size_t start;
	size_t end;
	char candidates[IME_CANDIDATES_MAX][IME_CANDIDATE_MAX];
	size_t candidate_count;
	size_t selected;
	unsigned int presses;
};

/*
 * A span of a split, as the segmenter hands it back.
 */
struct ja_span {
	size_t start;
	size_t end;
};

/*
 * The Japanese engine's state: what is being composed, and, while it is
 * being converted, its segments.
 *
 * It lives for the input method's lifetime.  An empty composition with
 * nothing pending is the resting state between inputs.  user_unsaved
 * says the user dictionary's file is behind its table: set when a commit
 * learns a choice, cleared when ja_core_save hands the text to the writer.
 */
struct ja_core {
	struct ja_unit units[JA_UNITS_MAX];
	size_t unit_count;
	struct ja_romaji romaji;
	bool literal_latch;
	bool converting;
	struct ja_segment *segments;
	size_t segment_count;
	size_t focus;
	struct ja_dict system;
	struct ja_dict supplement;
	bool has_supplement;
	struct ja_user user;
	struct ja_lexicon lexicon;
	struct ja_predict_index indexes[2];
	size_t index_count;
	bool indexed;
	bool learning;
	bool user_unsaved;
	int system_error;
	int supplement_error;
	int user_error;
};

/* ja-kana.c */
size_t ja_utf8_encode(uint32_t code, char *out);
size_t ja_utf8_decode(const char *text, size_t length, uint32_t *code);
bool ja_bytes_equal(const char *left, size_t left_length, const char *right, size_t right_length);
bool ja_is_hiragana(uint32_t code);
bool ja_starts_no_word(uint32_t code);
bool ja_is_loanword_mark(uint32_t code);
uint32_t ja_to_katakana(uint32_t code);
const char *ja_to_half_katakana(uint32_t code);
uint32_t ja_to_full_ascii(uint32_t code);
char ja_kana_consonant(uint32_t code);
int ja_kana_vowel(uint32_t code);
uint32_t ja_godan_kana(char consonant, int vowel);

/* ja-romaji.c */
void ja_romaji_reset(struct ja_romaji *romaji);
void ja_romaji_feed(struct ja_romaji *romaji, char letter, struct ja_romaji_result *result);
void ja_romaji_flush(struct ja_romaji *romaji, struct ja_romaji_result *result);
bool ja_romaji_backspace(struct ja_romaji *romaji);

/* ja-dict.c */
int ja_dict_load(struct ja_dict *dict, const char *path, size_t size_max);
int ja_dict_load_parts(struct ja_dict *first, struct ja_dict *second, const char *path, size_t size_max, bool *split);
void ja_dict_free(struct ja_dict *dict);
const struct ja_dict_entry *ja_dict_find(const struct ja_dict *dict, const char *key, size_t key_length);
bool ja_dict_next_candidate(const struct ja_dict_entry *entry, size_t *position, const char **candidate, size_t *length);
bool ja_dict_next_conjugated(const struct ja_dict_entry *entry, size_t *position, const char **candidate, size_t *length, enum ja_conjugation *conjugation);

/* ja-user.c */
int ja_user_open(struct ja_user *user, const char *path);
void ja_user_free(struct ja_user *user);
const struct ja_user_entry *ja_user_find(const struct ja_user *user, const char *reading, size_t length);
int ja_user_learn(struct ja_user *user, const char *reading, size_t length, const char *candidate);
int ja_user_save(const struct ja_user *user);
int ja_user_save_later(const struct ja_user *user);

/* ja-inflect.c */
void ja_inflect_ends(const struct ja_text *text, size_t stem_end, char consonant, bool adjective, bool *ends);
void ja_inflect_conjugated_ends(const struct ja_text *text, size_t stem_end, char consonant, enum ja_conjugation conjugation, bool *ends);
void ja_inflect_suru_ends(const struct ja_text *text, size_t start, bool *ends);
void ja_inflect_kuru_ends(const struct ja_text *text, size_t start, bool *ends);
void ja_inflect_kana_verb_ends(const struct ja_text *text, size_t start, bool *ends);
size_t ja_inflect_kana_verb_stem(const struct ja_text *text, size_t start, size_t end);
void ja_particle_ends(const struct ja_text *text, size_t start, bool *ends);
bool ja_is_particle(const struct ja_text *text, size_t start, size_t end);
bool ja_text_match(const struct ja_text *text, size_t start, const char *suffix, size_t *end);

/* ja-segment.c */
void ja_text_build(struct ja_text *text, const struct ja_unit *units, size_t unit_count);
size_t ja_segment_split(const struct ja_lexicon *lexicon, const struct ja_text *text, size_t start, struct ja_span *spans, size_t spans_max);
void ja_segment_candidates(const struct ja_lexicon *lexicon, const struct ja_text *text, struct ja_segment *segment);
bool ja_segment_add_candidate(struct ja_segment *segment, const char *text, size_t length);

/* ja-engine.c */
int ja_core_open(struct ja_core *core, const struct ja_config *config);
void ja_core_close(struct ja_core *core);
bool ja_core_composing(const struct ja_core *core);
bool ja_core_type(struct ja_core *core, char letter);
bool ja_core_type_literal(struct ja_core *core, char letter);
void ja_core_latch_literal(struct ja_core *core);
bool ja_core_literal_latched(const struct ja_core *core);
void ja_core_backspace(struct ja_core *core);
void ja_core_clear(struct ja_core *core);
bool ja_core_convert(struct ja_core *core);
void ja_core_cancel_conversion(struct ja_core *core);
void ja_core_next_candidate(struct ja_core *core, int step);
bool ja_core_select_on_page(struct ja_core *core, size_t index);
void ja_core_move_focus(struct ja_core *core, int step);
void ja_core_resize_focus(struct ja_core *core, int step);
void ja_core_apply_form(struct ja_core *core, enum ja_form form);
void ja_core_commit(struct ja_core *core, struct ime_output *out);
void ja_core_output(const struct ja_core *core, struct ime_output *out);
void ja_core_set_learning(struct ja_core *core, bool learning);
void ja_core_save(struct ja_core *core);

/* ja-keys.c */
void ja_keys_handle(struct ja_core *core, const struct ime_key *key, struct ime_output *out);

/* ja-predict.c */
int ja_predict_index(const struct ja_dict *dict, struct ja_predict_index *index);
void ja_predict_index_free(struct ja_predict_index *index);
size_t ja_predict(const struct ja_user *user, const struct ja_predict_index *const *indexes, size_t index_count, const char *reading, size_t length, struct ja_prediction *out, size_t max);
size_t ja_predict_keyboard(const struct ja_user *user, const struct ja_predict_index *const *indexes, size_t index_count, const char *reading, size_t length, struct ja_prediction *out, size_t max);

#endif
