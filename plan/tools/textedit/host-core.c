/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws092: host tests of Text Editor's document, undo history, rows, files
 * and finding (plan/ws092/design.md section 15), built on Linux with the
 * editor's own sources:
 *
 *   sh plan/tools/textedit/host-core.sh
 *
 * Random edits are made both to the gap buffer and to a plain string and
 * compared after each, with the line table checked against the string;
 * undoing every change must give the first text back and redoing them the
 * last; files are read and written in their forms (LF, CR LF, BOM, bytes
 * that are not UTF-8, NUL refused); rows are laid out and positions mapped
 * both ways.  Each check prints ok or FAIL; the last line counts them.
 */

#include "textedit.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* How many random edits the buffer test makes. */
#define TEST_EDITS	100000

static int test_passed;
static int test_failed;
static char test_dir[256];
static struct te_text test_font;

static void check(int ok, const char *name);
static int buffer_matches(const struct te_buffer *buffer, const char *text, size_t length);
static void test_buffer(void);
static void test_undo(void);
static void test_files(void);
static void test_layout(void);
static void test_find(void);
static void test_edit(void);
static void test_replace(void);
static void test_recent(void);
static void test_touch_bar(void);
static void set_text(struct te_app *app, const char *text, const char *find);
static int text_is(struct te_app *app, const char *text);
static void write_file(const char *name, const char *bytes, size_t length);
static int read_back(const char *name, char *out, size_t size, size_t *length);

void
te_log(
	const char *format,
	...)
{
	(void)format;
}

uint64_t
te_clock(void)
{
	struct timespec now;

	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

int
main(
	void)
{
	char *made;

	snprintf(test_dir, sizeof(test_dir), "/tmp/ws092-host-XXXXXX");
	made = mkdtemp(test_dir);
	if (made == NULL)
		return 2;
	srand(92);
	if (te_text_open(&test_font, "userland/desktop/fonts/JetBrainsMono-Regular.ttf", "userland/desktop/fonts/DroidSansFallbackFull.ttf") != 0)
		return 2;
	test_buffer();
	test_undo();
	test_files();
	test_layout();
	test_find();
	test_edit();
	test_replace();
	test_recent();
	test_touch_bar();
	printf("host-core: %d/%d\n", test_passed, test_passed + test_failed);
	return test_failed == 0 ? 0 : 1;
}

static void
check(
	int ok,
	const char *name)
{
	if (ok)
		test_passed++;
	else
		test_failed++;
	printf("%s %s\n", ok ? "ok" : "FAIL", name);
}

static int
buffer_matches(
	const struct te_buffer *buffer,
	const char *text,
	size_t length)
{
	size_t index;
	size_t line;
	size_t lines;

	if (te_buffer_length(buffer) != length)
		return 0;
	for (index = 0; index < length; index++) {
		if (te_buffer_byte(buffer, index) != (unsigned char)text[index])
			return 0;
	}
	lines = 1;
	for (index = 0; index < length; index++) {
		if (text[index] != '\n')
			continue;
		if (buffer->lines[lines] != index + 1U)
			return 0;
		lines++;
	}
	if (lines != buffer->line_count)
		return 0;
	for (line = 0; line < buffer->line_count; line++) {
		index = te_buffer_line_start(buffer, line);
		if (te_buffer_line_of(buffer, index) != line)
			return 0;
	}
	return 1;
}

static void
test_buffer(void)
{
	static const char *const pieces[] = { "a", "bc", "\n", "x\ny\n", "日本", "\n\n", "語", "tab\t" };
	struct te_buffer buffer;
	char *text;
	size_t length;
	size_t capacity;
	size_t start;
	size_t end;
	size_t piece;
	int step;
	int good;

	capacity = 4U * 1024U * 1024U;
	text = malloc(capacity);
	(void)te_buffer_init(&buffer, "hello\nworld", 11);
	memcpy(text, "hello\nworld", 11);
	length = 11;
	good = buffer_matches(&buffer, text, length);
	for (step = 0; step < TEST_EDITS && good; step++) {
		start = length == 0 ? 0 : (size_t)rand() % (length + 1U);
		if (rand() % 3 != 0 && length + 16U < capacity) {
			piece = (size_t)rand() % (sizeof(pieces) / sizeof(pieces[0]));
			(void)te_buffer_insert(&buffer, start, pieces[piece], strlen(pieces[piece]));
			memmove(text + start + strlen(pieces[piece]), text + start, length - start);
			memcpy(text + start, pieces[piece], strlen(pieces[piece]));
			length += strlen(pieces[piece]);
		} else {
			end = start + (size_t)rand() % 8U;
			if (end > length)
				end = length;
			te_buffer_delete(&buffer, start, end);
			memmove(text + start, text + end, length - end);
			length -= end - start;
		}
		if (step % 97 == 0)
			good = buffer_matches(&buffer, text, length);
	}
	good = good && buffer_matches(&buffer, text, length);
	check(good, "buffer: 100000 random edits match a plain string and its line table");
	check(te_buffer_prev_char(&buffer, 0) == 0, "buffer: nothing before the start");
	te_buffer_free(&buffer);
	(void)te_buffer_init(&buffer, "a日b", 5);
	check(te_buffer_next_char(&buffer, 1) == 4 && te_buffer_prev_char(&buffer, 4) == 1, "buffer: a three-byte character is one step");
	te_buffer_free(&buffer);
	(void)te_buffer_init(&buffer, "a\xff\xfe" "b", 4);
	check(te_buffer_next_char(&buffer, 1) == 2 && te_buffer_prev_char(&buffer, 3) == 2, "buffer: a malformed byte is a character of its own");
	te_buffer_free(&buffer);
	free(text);
}

static void
test_undo(void)
{
	struct te_app app;
	char first[64];
	size_t first_length;
	char *last;
	size_t last_length;
	int step;
	int good;

	te_app_init(&app, &test_font, &test_font, 900, 680);
	(void)te_edit_insert_text(&app, "start\ntext", 10, TE_MERGE_NONE);
	te_undo_mark_saved(&app.undo);
	first_length = te_buffer_length(&app.buffer);
	te_buffer_copy(&app.buffer, 0, first_length, first);
	check(!te_app_modified(&app), "undo: saved text is not modified");
	for (step = 0; step < 300; step++) {
		app.now += 2000;
		app.cursor = (size_t)rand() % (te_buffer_length(&app.buffer) + 1U);
		app.cursor = te_buffer_prev_char(&app.buffer, te_buffer_next_char(&app.buffer, app.cursor));
		app.anchor = app.cursor;
		if (rand() % 2)
			(void)te_edit_insert_text(&app, "xy\n", 3, TE_MERGE_NONE);
		else
			(void)te_edit_delete(&app, app.cursor, te_buffer_next_char(&app.buffer, app.cursor), TE_MERGE_NONE);
	}
	last_length = te_buffer_length(&app.buffer);
	last = malloc(last_length + 1U);
	te_buffer_copy(&app.buffer, 0, last_length, last);
	check(te_app_modified(&app), "undo: edited text is modified");
	while (te_undo_can_undo(&app.undo) && te_buffer_length(&app.buffer) != first_length)
		te_edit_undo(&app);
	good = te_buffer_length(&app.buffer) == first_length && memcmp(first, app.buffer.data, 0) == 0;
	good = good && buffer_matches(&app.buffer, first, first_length);
	check(good, "undo: undoing the random changes gives the saved text");
	check(!te_app_modified(&app), "undo: back at the saved state is not modified");
	while (te_undo_can_redo(&app.undo))
		te_edit_redo(&app);
	check(buffer_matches(&app.buffer, last, last_length), "undo: redoing them all gives the last text");
	free(last);

	app.now += 5000;
	app.cursor = te_buffer_length(&app.buffer);
	app.anchor = app.cursor;
	(void)te_edit_insert_text(&app, "w", 1, TE_MERGE_TYPING);
	app.now += 100;
	(void)te_edit_insert_text(&app, "o", 1, TE_MERGE_TYPING);
	app.now += 100;
	(void)te_edit_insert_text(&app, "r", 1, TE_MERGE_TYPING);
	app.now += 100;
	(void)te_edit_insert_text(&app, " ", 1, TE_MERGE_TYPING);
	last_length = te_buffer_length(&app.buffer);
	te_edit_undo(&app);
	check(te_buffer_length(&app.buffer) == last_length - 1U, "undo: a blank after a word is a step of its own");
	te_edit_undo(&app);
	check(te_buffer_length(&app.buffer) == last_length - 4U, "undo: a word typed is undone at once");
	te_app_release(&app);
}

static void
test_files(void)
{
	struct te_file_info info;
	struct te_buffer buffer;
	struct stat status;
	char path[512];
	char out[256];
	size_t length;
	char *text;
	int error;

	write_file("lf.txt", "one\ntwo\n", 8);
	snprintf(path, sizeof(path), "%s/lf.txt", test_dir);
	error = te_file_read(path, &text, &length, &info);
	check(error == 0 && length == 8 && !info.crlf && !info.bom, "file: LF read as it is");
	(void)te_buffer_init(&buffer, text, length);
	free(text);
	chmod(path, 0640);
	info.mode = 0640;
	error = te_file_write(path, &buffer, &info);
	(void)read_back("lf.txt", out, sizeof(out), &length);
	stat(path, &status);
	check(error == 0 && length == 8 && memcmp(out, "one\ntwo\n", 8) == 0 && (status.st_mode & 0777) == 0640, "file: saved the same, permissions kept");
	te_buffer_free(&buffer);

	write_file("crlf.txt", "\xef\xbb\xbf" "a\r\nb\r\nc", 10);
	snprintf(path, sizeof(path), "%s/crlf.txt", test_dir);
	error = te_file_read(path, &text, &length, &info);
	check(error == 0 && info.crlf && info.bom && length == 5 && memcmp(text, "a\nb\nc", 5) == 0, "file: BOM and CR LF taken off");
	(void)te_buffer_init(&buffer, text, length);
	free(text);
	(void)te_buffer_insert(&buffer, 5, "\nd", 2);
	error = te_file_write(path, &buffer, &info);
	(void)read_back("crlf.txt", out, sizeof(out), &length);
	check(error == 0 && length == 13 && memcmp(out, "\xef\xbb\xbf" "a\r\nb\r\nc\r\nd", 13) == 0, "file: BOM and CR LF put back");
	te_buffer_free(&buffer);

	write_file("bad.txt", "ok\xff\xfe\n", 5);
	snprintf(path, sizeof(path), "%s/bad.txt", test_dir);
	error = te_file_read(path, &text, &length, &info);
	check(error == 0 && info.invalid && length == 5, "file: bytes that are not UTF-8 kept and noted");
	(void)te_buffer_init(&buffer, text, length);
	free(text);
	error = te_file_write(path, &buffer, &info);
	(void)read_back("bad.txt", out, sizeof(out), &length);
	check(error == 0 && length == 5 && memcmp(out, "ok\xff\xfe\n", 5) == 0, "file: malformed bytes saved as they were");
	te_buffer_free(&buffer);

	write_file("nul.bin", "a\0b", 3);
	snprintf(path, sizeof(path), "%s/nul.bin", test_dir);
	error = te_file_read(path, &text, &length, &info);
	check(error == EILSEQ, "file: a NUL byte is refused");
	snprintf(path, sizeof(path), "%s/none.txt", test_dir);
	error = te_file_read(path, &text, &length, &info);
	check(error == ENOENT, "file: a missing file says so");
	snprintf(path, sizeof(path), "%s/no-such-folder/x.txt", test_dir);
	(void)te_buffer_init(&buffer, "x", 1);
	memset(&info, 0, sizeof(info));
	error = te_file_write(path, &buffer, &info);
	check(error != 0, "file: saving into a missing folder fails");
	te_buffer_free(&buffer);
}

static void
test_layout(void)
{
	struct te_layout layout;
	struct te_buffer buffer;
	size_t row;
	size_t column;
	size_t position;
	size_t start;
	size_t end;
	int good;

	(void)te_buffer_init(&buffer, "aaa bbb ccc\n日本語テキスト\n\tx", strlen("aaa bbb ccc\n日本語テキスト\n\tx"));
	te_layout_init(&layout);
	(void)te_layout_reset(&layout, &buffer, 1, 8U);
	check(layout.rows[0] == 2 && layout.rows[1] == 2 && layout.rows[2] == 1, "layout: wrapped after a blank, wide characters two cells");
	te_layout_row_range(&layout, &buffer, 1, &start, &end);
	check(start == 8 && end == 11, "layout: the second row is the last word");
	te_layout_place(&layout, &buffer, 35, &row, &column);
	check(row == 4 && column == 4, "layout: a tab reaches the next stop");
	good = 1;
	for (position = 0; position <= te_buffer_length(&buffer); position = te_buffer_next_char(&buffer, position)) {
		te_layout_place(&layout, &buffer, position, &row, &column);
		if (te_layout_position(&layout, &buffer, row, column) != position)
			good = 0;
		if (position == te_buffer_length(&buffer))
			break;
	}
	check(good, "layout: place and position agree");
	(void)te_layout_reset(&layout, &buffer, 0, 8U);
	check(layout.total == 3 && layout.widest == 14, "layout: unwrapped, one row a line and the widest line");
	te_layout_free(&layout);
	te_buffer_free(&buffer);
}

static void
test_find(void)
{
	struct te_buffer buffer;
	size_t start;
	int wrapped;
	int found;

	(void)te_buffer_init(&buffer, "Alpha beta ALPHA gamma", 22);
	found = te_find(&buffer, "alpha", 5, 1, 1, &start, &wrapped);
	check(found && start == 11 && !wrapped, "find: forward, ignoring case");
	found = te_find(&buffer, "alpha", 5, 12, 1, &start, &wrapped);
	check(found && start == 0 && wrapped, "find: round the end");
	found = te_find(&buffer, "alpha", 5, 11, 0, &start, &wrapped);
	check(found && start == 0 && !wrapped, "find: backward");
	found = te_find(&buffer, "delta", 5, 0, 1, &start, &wrapped);
	check(!found, "find: nothing");
	te_buffer_free(&buffer);
}

static void
test_edit(void)
{
	struct te_app app;
	struct te_event event;
	char text[64];
	size_t length;

	te_app_init(&app, &test_font, &test_font, 900, 680);
	(void)te_edit_insert_text(&app, "    indented", 12, TE_MERGE_NONE);
	memset(&event, 0, sizeof(event));
	event.type = TE_EVENT_KEY;
	event.pressed = 1;
	event.key = TE_KEY_ENTER;
	te_app_event(&app, &event);
	length = te_buffer_length(&app.buffer);
	te_buffer_copy(&app.buffer, 0, length, text);
	check(length == 17 && memcmp(text + 12, "\n    ", 5) == 0, "edit: Enter keeps the indent");
	event.key = TE_KEY_HOME;
	te_app_event(&app, &event);
	check(app.cursor == 13, "edit: Home goes to the line's start from its indent's end");
	te_edit_select_all(&app);
	event.key = TE_KEY_TAB;
	te_app_event(&app, &event);
	length = te_buffer_length(&app.buffer);
	te_buffer_copy(&app.buffer, 0, length, text);
	check(text[0] == '\t' && text[14] == '\t', "edit: Tab over lines indents each");
	event.modifiers = TE_MOD_SHIFT;
	te_app_event(&app, &event);
	length = te_buffer_length(&app.buffer);
	check(length == 17, "edit: Shift+Tab takes the indent back");
	event.modifiers = TE_MOD_CTRL;
	event.key = TE_KEY_RIGHT;
	app.cursor = 0;
	app.anchor = 0;
	te_app_event(&app, &event);
	check(app.cursor == 12, "edit: Ctrl+Right goes to the end of the word");
	te_app_release(&app);
}

/* ws128-p003: Replace and Replace All (the undo group, an empty replacement, Japanese text); BUG-248: the Find panel. */
static void
test_replace(void)
{
	struct te_app app;
	size_t count;
	int replaced;

	/* Replace: the first selects, the next replaces and moves on, Undo puts one back. */
	te_app_init(&app, &test_font, &test_font, 900, 680);
	set_text(&app, "cat Cat cAT dog", "cat");
	replaced = te_edit_replace(&app, "fox", 3);
	check(!replaced && app.anchor == 0 && app.cursor == 3, "replace: the first Replace selects the place found");
	replaced = te_edit_replace(&app, "fox", 3);
	check(replaced && text_is(&app, "fox Cat cAT dog") && app.anchor == 4 && app.cursor == 7, "replace: the next replaces it and selects the next (either case)");
	te_edit_undo(&app);
	check(text_is(&app, "cat Cat cAT dog"), "replace: undo puts one replacement back");

	/* Replace All with Japanese text, undone and redone in one step each. */
	set_text(&app, "cat Cat cAT dog", "cat");
	count = te_edit_replace_all(&app, "\xe3\x81\xad\xe3\x81\x93", 6);
	check(count == 3 && text_is(&app, "\xe3\x81\xad\xe3\x81\x93 \xe3\x81\xad\xe3\x81\x93 \xe3\x81\xad\xe3\x81\x93 dog"), "replace all: every place, with Japanese text");
	te_edit_undo(&app);
	check(text_is(&app, "cat Cat cAT dog"), "replace all: one undo puts every place back");
	te_edit_redo(&app);
	check(text_is(&app, "\xe3\x81\xad\xe3\x81\x93 \xe3\x81\xad\xe3\x81\x93 \xe3\x81\xad\xe3\x81\x93 dog"), "replace all: one redo replaces them again");

	/* An empty replacement. */
	set_text(&app, "a-b--c", "-");
	count = te_edit_replace_all(&app, "", 0);
	check(count == 3 && text_is(&app, "abc"), "replace all: an empty replacement deletes");

	/* A Japanese find text. */
	set_text(&app, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xa8\xe6\x97\xa5\xe6\x9c\xac", "\xe6\x97\xa5\xe6\x9c\xac");
	count = te_edit_replace_all(&app, "\xe8\x8b\xb1", 3);
	check(count == 2 && text_is(&app, "\xe8\x8b\xb1\xe8\xaa\x9e\xe3\x81\xa8\xe8\x8b\xb1"), "replace all: a Japanese find text");

	/* A replacement holding the find text. */
	set_text(&app, "aaa", "a");
	count = te_edit_replace_all(&app, "aa", 2);
	check(count == 3 && text_is(&app, "aaaaaa"), "replace all: a replacement holding the find text is not searched again");

	/* The panel's Replace All, an empty find text, and the panel holding the other actions back. */
	set_text(&app, "one two one", "");
	te_app_replace(&app, "one", "1", 1);
	check(text_is(&app, "1 two 1") && strcmp(app.replace_with, "1") == 0 && strstr(app.message, "Replaced 2") != NULL, "replace: the panel's Replace All sets the find text and says how many");
	te_app_replace(&app, "", "x", 1);
	check(text_is(&app, "1 two 1") && strstr(app.message, "Type the text") != NULL, "replace: an empty find text changes nothing");
	te_app_action(&app, TE_ACTION_REPLACE);
	check(app.dialog == TE_DIALOG_REPLACE && app.panel_fresh, "replace: Edit > Replace opens the panel");
	te_app_action(&app, TE_ACTION_UNDO);
	check(text_is(&app, "1 two 1"), "replace: the panel holds the other actions back");
	te_app_replace_close(&app);
	check(app.dialog == TE_DIALOG_NONE, "replace: the panel closes");

	/* Edit > Find's panel (BUG-248): it opens, finds as it is typed, lets Find Next through, holds Undo back and closes. */
	set_text(&app, "one two one two", "");
	app.panel_fresh = 0;
	te_app_action(&app, TE_ACTION_FIND);
	check(app.dialog == TE_DIALOG_FIND && app.panel_fresh, "find: Edit > Find opens the panel");
	te_app_find_text(&app, "two");
	check(strcmp(app.find, "two") == 0 && app.anchor == 4U && app.cursor == 7U, "find: the panel's text is found as it is typed");
	te_app_action(&app, TE_ACTION_FIND_NEXT);
	check(app.anchor == 12U && app.cursor == 15U, "find: Find Next goes past the panel to the next place");
	te_app_action(&app, TE_ACTION_SELECT_ALL);
	check(app.anchor == 12U && app.cursor == 15U, "find: the panel holds the other actions back");
	te_app_find_close(&app);
	check(app.dialog == TE_DIALOG_NONE, "find: the panel closes");

	/* A text that occurs nowhere. */
	set_text(&app, "nothing here", "zebra");
	count = te_edit_replace_all(&app, "x", 1);
	check(count == 0 && text_is(&app, "nothing here") && !te_undo_can_undo(&app.undo), "replace all: no place, no change and no undo step");
	te_app_release(&app);
}

/* ws128-p003: File > Open Recent opens a file that is there, says so of one that is gone, and asks about unsaved changes first. */
static void
test_recent(void)
{
	struct te_app app;
	char path[512];

	/* Two recent files, one of them gone. */
	te_app_init(&app, &test_font, &test_font, 900, 680);
	write_file("recent.txt", "recent text\n", 12);
	snprintf(path, sizeof(path), "%s/recent.txt", test_dir);
	snprintf(app.recent[0], sizeof(app.recent[0]), "%s", path);
	app.recent_present[0] = 1;
	snprintf(app.recent[1], sizeof(app.recent[1]), "%s/gone.txt", test_dir);
	app.recent_present[1] = 0;
	app.recent_count = 2;
	/* The gone one, then the one that is there, then the one there over unsaved changes. */
	te_app_action(&app, (enum te_action)(TE_ACTION_RECENT_FIRST + 1U));
	check(strstr(app.message, "no longer there") != NULL && app.path[0] == '\0', "recent: a file that is gone is not opened");
	te_app_action(&app, (enum te_action)TE_ACTION_RECENT_FIRST);
	check(strcmp(app.path, path) == 0 && text_is(&app, "recent text\n"), "recent: the first item opens its file");
	(void)te_edit_insert_text(&app, "x", 1, TE_MERGE_NONE);
	te_app_new(&app);
	(void)te_edit_insert_text(&app, "unsaved", 7, TE_MERGE_NONE);
	te_app_action(&app, (enum te_action)TE_ACTION_RECENT_FIRST);
	check(app.dialog == TE_DIALOG_UNSAVED && app.path[0] == '\0', "recent: unsaved changes are asked about first");
	te_app_dialog_choose(&app, 1);
	check(strcmp(app.path, path) == 0, "recent: Don't Save goes on to open the file");
	te_app_release(&app);
}

/* Starts a document over with a text and a find text, the cursor at the start. */
static void
set_text(
	struct te_app *app,
	const char *text,
	const char *find)
{
	/* The text, without an undo step and with the cursor at the start. */
	te_app_new(app);
	(void)te_edit_insert_text(app, text, strlen(text), TE_MERGE_NONE);
	te_undo_free(&app->undo);
	te_undo_init(&app->undo);
	app->cursor = 0;
	app->anchor = 0;
	snprintf(app->find, sizeof(app->find), "%s", find);
	app->find_length = strlen(find);
}

/* Tells whether the document is a text. */
static int
text_is(
	struct te_app *app,
	const char *text)
{
	/* The buffer's bytes and lines against the text. */
	return buffer_matches(&app->buffer, text, strlen(text));
}

static void
write_file(
	const char *name,
	const char *bytes,
	size_t length)
{
	char path[512];
	FILE *file;

	snprintf(path, sizeof(path), "%s/%s", test_dir, name);
	file = fopen(path, "wb");
	fwrite(bytes, 1, length, file);
	fclose(file);
}

static int
read_back(
	const char *name,
	char *out,
	size_t size,
	size_t *length)
{
	char path[512];
	FILE *file;

	snprintf(path, sizeof(path), "%s/%s", test_dir, name);
	file = fopen(path, "rb");
	if (file == NULL)
		return -1;
	*length = fread(out, 1, size, file);
	fclose(file);
	return 0;
}

/* The appearance libkeiland's theme asks for: the light one (the host test has no compositor to ask, q796). */
unsigned
kl_appearance_get(
	const struct kl_appearance *appearance)
{
	(void)appearance;

	/* The light appearance. */
	return KL_APPEARANCE_LIGHT;
}

/* ws190-p003: the fingers' selection's bar in the text (libkeiland's kl_text_touch): a double tap's word with the bar, which a key takes away, also at a caret. */
static void
test_touch_bar(void)
{
	struct te_app app;
	struct te_event event;
	struct kl_rect place;

	te_app_init(&app, &test_font, &test_font, 900, 680);
	(void)te_edit_insert_text(&app, "hello world", 11, TE_MERGE_NONE);

	/* A double tap in "world": the word, its handles and the bar, the editor's selection. */
	app.touch.view->caret_rect(app.touch.data, 8, &place);
	kl_text_touch_tap(&app.touch, (double)place.x, (double)place.y + 2.0, 1);
	te_app_touch(&app);
	check(app.anchor == 6 && app.cursor == 11 && app.touch.bar == 1 && app.touch.handles == 1, "touch: a double tap selects the word with the bar");

	/* A key takes the handles and the bar away. */
	memset(&event, 0, sizeof(event));
	event.type = TE_EVENT_KEY;
	event.pressed = 1;
	event.key = TE_KEY_RIGHT;
	te_app_event(&app, &event);
	check(app.touch.bar == 0 && app.touch.handles == 0, "touch: a key takes the bar away");

	/* The bar at a caret (no handles, as after a double tap off the words), which a key takes away too. */
	kl_text_touch_select(&app.touch, 5, 5);
	te_app_touch(&app);
	check(app.touch.bar == 1 && app.touch.handles == 0 && app.cursor == 5, "touch: the bar at a caret");
	te_app_event(&app, &event);
	check(app.touch.bar == 0, "touch: a key takes the caret's bar away");
	te_app_release(&app);
}
