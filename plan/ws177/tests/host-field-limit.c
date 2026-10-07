/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of a field's limit (ws177-p004,
 * userland/desktop/libkeiland/ui/field.c, KL_VERSION 64) with stand-ins for
 * the drawing and for the keys and texts a window takes: a zeroed field
 * holds KL_FIELD_MAX less one bytes; one limited to 4 takes four keys and
 * no fifth, an input method's commit only what fits, cut at a character's
 * start; a text set longer is cut; a limit set below the text cuts it; and
 * a limit past the room is the room.
 */

#include "userland/desktop/libkeiland/ui/internal.h"

#include <stdio.h>
#include <string.h>

/* The inputs the window gives the field in the next draw, and how many are left. */
static struct keiui_input test_inputs[8];
static unsigned test_input_count;
static unsigned test_input_next;

/* The number of failed checks. */
static int test_failures;

static void check(int condition, const char *what);
static void test_key(uint32_t code);
static void test_commit(const char *text);
static void test_draw(struct kl_field *field);

/* Stands in for ui.c: the field has the keyboard. */
unsigned
keiui_ui_widget(
	struct kl_ui *ui,
	uint32_t id,
	uint32_t index,
	const struct kl_rect *rect,
	unsigned flags)
{
	(void)ui;
	(void)id;
	(void)index;
	(void)rect;
	(void)flags;

	/* Succeeded: focused. */
	return KL_HIT_FOCUSED;
}

/* Stands in for ui.c: the inputs the test queued, in order. */
int
keiui_ui_take_input(
	struct kl_ui *ui,
	uint32_t id,
	uint32_t index,
	keiui_wants_key wants,
	struct keiui_input *input)
{
	(void)ui;
	(void)id;
	(void)index;
	(void)wants;

	/* None left. */
	if (test_input_next == test_input_count)
		return 0;

	/* Succeeded: the next one. */
	*input = test_inputs[test_input_next];
	test_input_next++;
	return 1;
}

/* Stands in for ui.c: nothing is being composed. */
const char *
keiui_ui_preedit(
	struct kl_ui *ui,
	uint32_t id,
	uint32_t index,
	int focused,
	int32_t *begin,
	int32_t *end)
{
	(void)ui;
	(void)id;
	(void)index;
	(void)focused;
	(void)begin;
	(void)end;

	/* Succeeded: none. */
	return NULL;
}

/* Stands in for ui.c. */
void
keiui_ui_text_caret(
	struct kl_ui *ui,
	const struct kl_rect *caret)
{
	(void)ui;
	(void)caret;
}

/* Stands in for the keys: the test's codes are the characters themselves. */
uint32_t
kl_key_character(
	uint32_t key,
	unsigned modifiers)
{
	(void)modifiers;

	/* Succeeded: the character. */
	return key;
}

/* Stands in for the text: 8 pixels a byte. */
int
kl_text_width(
	struct kl_text *text,
	const char *string,
	size_t length,
	unsigned pixels,
	int bold)
{
	(void)text;
	(void)string;
	(void)pixels;
	(void)bold;

	/* Succeeded: the width. */
	return (int)length * 8;
}

/* The drawing stands in as nothing: a text's end. */
int
kl_text_draw(
	struct kl_text *text,
	struct kl_canvas *canvas,
	int x,
	int baseline,
	const char *string,
	size_t length,
	unsigned pixels,
	int bold,
	kl_color color)
{
	(void)text;
	(void)canvas;
	(void)baseline;
	(void)string;
	(void)length;
	(void)pixels;
	(void)bold;
	(void)color;
	return x;
}

int
kl_text_center(
	unsigned pixels,
	int top,
	int height)
{
	(void)pixels;
	(void)height;
	return top;
}

void
kl_canvas_round(
	struct kl_canvas *canvas,
	float x,
	float y,
	float width,
	float height,
	float radius,
	kl_color color)
{
	(void)canvas;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	(void)radius;
	(void)color;
}

void
kl_canvas_round_border(
	struct kl_canvas *canvas,
	float x,
	float y,
	float width,
	float height,
	float radius,
	float thickness,
	kl_color color)
{
	(void)canvas;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	(void)radius;
	(void)thickness;
	(void)color;
}

void
kl_canvas_line(
	struct kl_canvas *canvas,
	float x0,
	float y0,
	float x1,
	float y1,
	float thickness,
	kl_color color)
{
	(void)canvas;
	(void)x0;
	(void)y0;
	(void)x1;
	(void)y1;
	(void)thickness;
	(void)color;
}

void
kl_canvas_clip_push(
	struct kl_canvas *canvas,
	const struct kl_rect *rect)
{
	(void)canvas;
	(void)rect;
}

void
kl_canvas_clip_pop(
	struct kl_canvas *canvas)
{
	(void)canvas;
}

void
kl_ui_pointer(
	const struct kl_ui *ui,
	double *x,
	double *y)
{
	(void)ui;
	*x = 0.0;
	*y = 0.0;
}

/*
 * Runs the steps.
 */
int
main(void)
{
	struct kl_field field;
	char long_text[KL_FIELD_MAX + 16U];

	/* A zeroed field holds the room. */
	memset(&field, 0, sizeof(field));
	memset(long_text, 'x', sizeof(long_text) - 1U);
	long_text[sizeof(long_text) - 1U] = '\0';
	kl_field_set(&field, long_text);
	check(field.length == KL_FIELD_MAX - 1U, "a zeroed field holds KL_FIELD_MAX less one bytes");

	/* Limited to 4: four keys go in, the fifth does not. */
	memset(&field, 0, sizeof(field));
	kl_field_set_limit(&field, 4U);
	test_key('a');
	test_key('b');
	test_key('c');
	test_key('d');
	test_key('e');
	test_draw(&field);
	check(field.length == 4U && strcmp(field.text, "abcd") == 0, "four keys in, the fifth refused");

	/* A commit takes what fits, cut at a character's start: two bytes left, a three-byte character does not fit. */
	memset(&field, 0, sizeof(field));
	kl_field_set_limit(&field, 4U);
	kl_field_set(&field, "ab");
	test_commit("\xe3\x81\x82z");
	test_draw(&field);
	check(field.length == 2U && strcmp(field.text, "ab") == 0, "a character that does not fit is not cut in half");
	test_commit("yz");
	test_draw(&field);
	check(field.length == 4U && strcmp(field.text, "abyz") == 0, "a commit takes what fits");

	/* A text set longer is cut; a limit set below the text cuts it, the caret at the end. */
	memset(&field, 0, sizeof(field));
	kl_field_set_limit(&field, 3U);
	kl_field_set(&field, "a\xc3\xa9z");
	check(field.length == 3U && strcmp(field.text, "a\xc3\xa9") == 0, "a text set longer is cut at the limit");
	kl_field_set_limit(&field, 2U);
	check(field.length == 1U && field.caret == 1U && strcmp(field.text, "a") == 0, "a lower limit cuts at a character's start");

	/* A limit past the room is the room. */
	kl_field_set_limit(&field, KL_FIELD_MAX * 2U);
	kl_field_set(&field, long_text);
	check(field.limit == KL_FIELD_MAX - 1U && field.length == KL_FIELD_MAX - 1U, "a limit past the room is the room");

	/* The verdict. */
	if (test_failures != 0) {
		printf("host-field-limit: %d FAILED\n", test_failures);
		return 1;
	}

	/* Succeeded: every step held. */
	printf("host-field-limit: PASS\n");
	return 0;
}

/* Counts and names a failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* A failure is named. */
	if (!condition) {
		printf("FAIL: %s\n", what);
		test_failures++;
	}
}

/* Queues a key for the next draw. */
static void
test_key(
	uint32_t code)
{
	/* The key. */
	memset(&test_inputs[test_input_count], 0, sizeof(test_inputs[0]));
	test_inputs[test_input_count].kind = KEIUI_INPUT_KEY;
	test_inputs[test_input_count].code = code;
	test_input_count++;
}

/* Queues an input method's commit for the next draw. */
static void
test_commit(
	const char *text)
{
	/* The commit. */
	memset(&test_inputs[test_input_count], 0, sizeof(test_inputs[0]));
	test_inputs[test_input_count].kind = KEIUI_INPUT_COMMIT;
	(void)snprintf(test_inputs[test_input_count].text, sizeof(test_inputs[0].text), "%s", text);
	test_input_count++;
}

/* Draws the field once, which takes the queued inputs. */
static void
test_draw(
	struct kl_field *field)
{
	struct kl_theme theme;
	struct kl_style style;
	struct kl_rect rect;

	/* A style of nothing, a field of 300 by 32. */
	memset(&theme, 0, sizeof(theme));
	memset(&style, 0, sizeof(style));
	style.theme = &theme;
	rect.x = 0;
	rect.y = 0;
	rect.width = 300;
	rect.height = 32;
	(void)kl_field(NULL, &style, 1U, &rect, field, NULL);

	/* The queue is used up. */
	test_input_count = 0U;
	test_input_next = 0U;
}
