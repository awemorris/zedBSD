/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host regression test for BUG-514: clicking and rendering the Behavior
 * pane in terminal settings-page.c when the program provides more than 12
 * items (matching apps/mikuterm-common/page.c's 13 items, with the 13th
 * item in TERMINAL_PANE_BEHAVIOR).
 */

#include "userland/desktop/terminal/settings-page.h"
#include "userland/desktop/terminal/settings-hosts.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int click_behavior_once = 1;
static unsigned behavior_fields_drawn;
static int thirteenth_item_drawn;

/* Minimal libkeiland and terminal stubs for host-side page rendering. */
struct kl_ui {
	int dummy;
};

static struct kl_ui stub_ui;
static struct kl_theme stub_theme;

const struct kl_theme *
kl_theme_default(
	void)
{
	stub_theme.control_height = 28;
	stub_theme.switch_width = 36;
	stub_theme.switch_height = 20;
	return &stub_theme;
}

struct kl_ui *
kl_ui_create(
	void)
{
	return &stub_ui;
}

void kl_ui_destroy(struct kl_ui *ui) { (void)ui; }
void kl_ui_begin(struct kl_ui *ui, uint64_t now_us) { (void)ui; (void)now_us; }
int kl_ui_end(struct kl_ui *ui, uint64_t now_us) { (void)ui; (void)now_us; return 0; }
int kl_ui_pointer_motion(struct kl_ui *ui, double x, double y) { (void)ui; (void)x; (void)y; return 0; }
int kl_ui_pointer_button(struct kl_ui *ui, int pressed, uint64_t now_us) { (void)ui; (void)pressed; (void)now_us; return 0; }
int kl_ui_pointer_leave(struct kl_ui *ui) { (void)ui; return 0; }
int kl_ui_axis(struct kl_ui *ui, const struct kl_window_event *event) { (void)ui; (void)event; return 0; }
int kl_ui_key(struct kl_ui *ui, uint32_t key, int pressed, unsigned mods) { (void)ui; (void)key; (void)pressed; (void)mods; return 0; }
int kl_ui_text(struct kl_ui *ui, const struct kl_window_event *event) { (void)ui; (void)event; return 0; }
int kl_ui_touch_down(struct kl_ui *ui, int32_t id, uint64_t t, uint64_t n, double x, double y) { (void)ui; (void)id; (void)t; (void)n; (void)x; (void)y; return 0; }
int kl_ui_touch_motion(struct kl_ui *ui, int32_t id, uint64_t t, uint64_t n, double x, double y) { (void)ui; (void)id; (void)t; (void)n; (void)x; (void)y; return 0; }
int kl_ui_touch_up(struct kl_ui *ui, int32_t id, uint64_t t, uint64_t n) { (void)ui; (void)id; (void)t; (void)n; return 0; }
int kl_ui_touch_cancel(struct kl_ui *ui, uint64_t n) { (void)ui; (void)n; return 0; }
int kl_ui_text_wanted(const struct kl_ui *ui, struct kl_rect *caret) { (void)ui; (void)caret; return 0; }
int kl_ui_has_focus(const struct kl_ui *ui, uint32_t id, uint32_t index) { (void)ui; (void)id; (void)index; return 0; }
void kl_ui_set_focus(struct kl_ui *ui, uint32_t id, uint32_t index) { (void)ui; (void)id; (void)index; }
void kl_ui_clear_focus(struct kl_ui *ui) { (void)ui; }
int kl_ui_take(struct kl_ui *ui, struct kl_event *event) { (void)ui; (void)event; return 0; }
void kl_ui_scroll_region(struct kl_ui *ui, uint32_t id, const struct kl_rect *rect, struct kl_scroll *scroll) { (void)ui; (void)id; (void)rect; (void)scroll; }

uint64_t kl_clock_us(void) { return 1000U; }

int kl_canvas_init(struct kl_canvas *canvas, uint32_t *pixels, size_t stride, int width, int height)
{
	memset(canvas, 0, sizeof(*canvas));
	canvas->pixels = pixels;
	canvas->stride = stride;
	canvas->width = width;
	canvas->height = height;
	return 0;
}

void kl_canvas_release(struct kl_canvas *canvas) { (void)canvas; }
void kl_canvas_set_scale(struct kl_canvas *canvas, unsigned scale) { (void)canvas; (void)scale; }
void kl_canvas_logical(const struct kl_canvas *canvas, const struct kl_rect *pixels, struct kl_rect *units) { (void)canvas; *units = *pixels; }
void kl_canvas_gradient(struct kl_canvas *canvas, const struct kl_rect *rect, kl_color top, kl_color bottom) { (void)canvas; (void)rect; (void)top; (void)bottom; }
void kl_canvas_clip_push(struct kl_canvas *canvas, const struct kl_rect *rect) { (void)canvas; (void)rect; }
void kl_canvas_clip_pop(struct kl_canvas *canvas) { (void)canvas; }

int kl_text_open(struct kl_text *text, const char *primary, const char *fallback) { (void)primary; (void)fallback; memset(text, 0, sizeof(*text)); return 0; }
void kl_text_close(struct kl_text *text) { (void)text; }
int kl_text_width(struct kl_text *text, const char *s, size_t len, unsigned px, int bold) { (void)text; (void)s; (void)px; (void)bold; return (int)(len * 8U); }
int kl_text_draw(struct kl_text *text, struct kl_canvas *canvas, int x, int y, const char *s, size_t len, unsigned px, int bold, kl_color c) { (void)text; (void)canvas; (void)x; (void)y; (void)s; (void)len; (void)px; (void)bold; (void)c; return 0; }
int kl_text_draw_fit(struct kl_text *text, struct kl_canvas *canvas, int x, int y, const char *s, unsigned px, int bold, int max_w, kl_color c) { (void)text; (void)canvas; (void)x; (void)y; (void)max_w; (void)s; (void)px; (void)bold; (void)c; return 0; }
int kl_text_center(unsigned px, int top, int height) { (void)px; return top + height / 2; }
size_t kl_text_break(struct kl_text *text, const char *s, unsigned px, int bold, int max_w) { (void)text; (void)px; (void)bold; (void)max_w; return strlen(s); }

int kl_scroll_init(struct kl_scroll *s, unsigned axes) { memset(s, 0, sizeof(*s)); s->axes = axes; return 0; }
void kl_scroll_release(struct kl_scroll *s) { (void)s; }
void kl_scroll_set_size(struct kl_scroll *s, double cw, double ch, double vw, double vh) { (void)s; (void)vw; (void)vh; (void)cw; (void)ch; }
int kl_scroll_step(struct kl_scroll *s, uint64_t now_us) { (void)s; (void)now_us; return 0; }
void kl_scroll_move_to(struct kl_scroll *s, double x, double y, int glide, uint64_t now_us) { (void)s; (void)x; (void)y; (void)glide; (void)now_us; }
int kl_scroll_key(struct kl_scroll *s, uint32_t key, unsigned mods, double line, uint64_t now_us) { (void)s; (void)key; (void)mods; (void)line; (void)now_us; return 0; }
int kl_scroll_draw_bars(const struct kl_scroll *s, struct kl_canvas *c, const struct kl_rect *vp, const struct kl_theme *t, uint64_t now_us) { (void)s; (void)c; (void)vp; (void)t; (void)now_us; return 0; }

void kl_panel(const struct kl_style *st, const struct kl_rect *r, int sidebar) { (void)st; (void)r; (void)sidebar; }
int kl_card(const struct kl_style *st, const struct kl_rect *r, const char *title, const char *sub) { (void)st; (void)title; (void)sub; return r->y + 36; }
int kl_row(const struct kl_style *st, int x, int y, int w, const char *lbl, const char *val, int last) { (void)st; (void)x; (void)w; (void)lbl; (void)val; (void)last; return y + 28; }
int kl_button_width(const struct kl_style *st, const char *label) { (void)st; return (int)(strlen(label) * 8U + 24U); }
int kl_button(struct kl_ui *ui, const struct kl_style *st, uint32_t id, const struct kl_rect *r, const char *label, unsigned flags) { (void)ui; (void)st; (void)id; (void)r; (void)label; (void)flags; return 0; }
int kl_icon_button(struct kl_ui *ui, const struct kl_style *st, uint32_t id, uint32_t idx, const struct kl_rect *r, enum kl_icon icon, int px, unsigned flags) { (void)ui; (void)st; (void)id; (void)idx; (void)r; (void)icon; (void)px; (void)flags; return 0; }
int kl_switch(struct kl_ui *ui, const struct kl_style *st, uint32_t id, int x, int y, int *on, unsigned flags) { (void)ui; (void)st; (void)id; (void)x; (void)y; (void)on; (void)flags; return 0; }

void
kl_field_set(
	struct kl_field *field,
	const char *text)
{
	size_t len;

	len = strlen(text);
	if (len >= sizeof(field->text))
		len = sizeof(field->text) - 1U;
	memcpy(field->text, text, len);
	field->text[len] = '\0';
	field->length = len;
	field->caret = len;
	field->anchor = len;
}

void kl_field_set_limit(struct kl_field *field, size_t limit) { (void)field; (void)limit; }

unsigned
kl_field(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	const struct kl_rect *rect,
	struct kl_field *field,
	const char *placeholder)
{
	(void)ui;
	(void)style;
	(void)rect;
	/* Verifies field->text is properly initialized and NUL-terminated. */
	assert(field->length < sizeof(field->text));
	assert(field->text[field->length] == '\0');
	assert(strlen(field->text) == field->length);
	behavior_fields_drawn++;
	if (placeholder != NULL && strcmp(placeholder, "Connect") == 0)
		thirteenth_item_drawn = 1;
	(void)id;
	return 0U;
}

unsigned kl_text_area(struct kl_ui *ui, const struct kl_style *st, uint32_t id, const struct kl_rect *r, struct kl_text_area *area, const char *ph) { (void)ui; (void)st; (void)id; (void)r; (void)area; (void)ph; return 0U; }
void kl_text_area_set(struct kl_text_area *area, const char *text) { (void)area; (void)text; }

int kl_sidebar_place(struct kl_ui *ui, const struct kl_style *st, uint32_t id, uint32_t idx, const struct kl_rect *r, enum kl_icon icon, const char *lbl, int cur, unsigned flags) { (void)ui; (void)st; (void)id; (void)idx; (void)r; (void)icon; (void)lbl; (void)cur; (void)flags; return 0; }
int kl_sidebar_section(const struct kl_style *st, int x, int y, int w, const char *lbl) { (void)st; (void)x; (void)w; (void)lbl; return y + 24; }

int
kl_sidebar_item(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	uint32_t index,
	const struct kl_rect *rect,
	enum kl_icon icon,
	const char *label,
	int current)
{
	(void)ui;
	(void)style;
	(void)id;
	(void)index;
	(void)rect;
	(void)icon;
	(void)current;
	if (click_behavior_once && strcmp(label, "Behavior") == 0) {
		click_behavior_once = 0;
		return 1;
	}
	return 0;
}

void kl_window_text_input(struct kl_window *w, int on) { (void)w; (void)on; }
void kl_window_text_cursor(struct kl_window *w, int x, int y, int width, int height) { (void)w; (void)x; (void)y; (void)width; (void)height; }

int terminal_key_name_valid(const char *n) { (void)n; return 1; }
int terminal_key_public(const struct terminal_page_program *p, const char *n, char *l, size_t ls, char *fp, size_t fps) { (void)p; (void)n; (void)l; (void)ls; (void)fp; (void)fps; return ENOENT; }
int terminal_key_make(const struct terminal_page_program *p, const char *n, const char *c, const char *pw, char *l, size_t ls, char *w, size_t ws) { (void)p; (void)n; (void)c; (void)pw; (void)l; (void)ls; (void)w; (void)ws; return 0; }
int terminal_key_import(const struct terminal_page_program *p, const char *n, const char *t, size_t s, char *l, size_t ls, char *w, size_t ws) { (void)p; (void)n; (void)t; (void)s; (void)l; (void)ls; (void)w; (void)ws; return 0; }
unsigned terminal_known_list(const char *t, size_t s, struct terminal_known_line *l, unsigned m, unsigned *tot) { (void)t; (void)s; (void)l; (void)m; *tot = 0U; return 0U; }
int terminal_known_remove(const char *t, size_t s, unsigned line, char **o, size_t *os) { (void)t; (void)s; (void)line; (void)o; (void)os; return ENOENT; }

static int
stub_read(
	void *data,
	const char *path,
	size_t limit,
	char **text,
	size_t *size)
{
	(void)data;
	(void)path;
	(void)limit;
	*text = NULL;
	*size = 0U;
	return ENOENT;
}

static int
stub_item_get(
	void *data,
	const char *keyword,
	char *value,
	size_t size)
{
	(void)data;
	(void)keyword;
	snprintf(value, size, "val");
	return 0;
}

static int
stub_item_set(
	void *data,
	const char *keyword,
	const char *value,
	char *why,
	size_t size)
{
	(void)data;
	(void)keyword;
	(void)value;
	if (size > 0U)
		why[0] = '\0';
	return 0;
}

int
main(
	void)
{
	static const struct terminal_page_item items[] = {
		{ "Font", "Font", TERMINAL_PANE_APPEARANCE, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_START, NULL },
		{ "FontFallback", "Fallback font", TERMINAL_PANE_APPEARANCE, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_START, NULL },
		{ "Columns", "Columns", TERMINAL_PANE_APPEARANCE, TERMINAL_ITEM_NUMBER, TERMINAL_EFFECT_START, NULL },
		{ "Rows", "Rows", TERMINAL_PANE_APPEARANCE, TERMINAL_ITEM_NUMBER, TERMINAL_EFFECT_START, NULL },
		{ "Term", "TERM", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_CONNECTION, NULL },
		{ "ConnectTimeout", "Connect timeout (s)", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_NUMBER, TERMINAL_EFFECT_CONNECTION, NULL },
		{ "WslPort", "WSL sshd port", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_NUMBER, TERMINAL_EFFECT_CONNECTION, NULL },
		{ "X11Scale", "X11 scale (%)", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_CONNECTION, NULL },
		{ "X11Render", "X11 drawing", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_CONNECTION, NULL },
		{ "X11PrimaryToClipboard", "X11 selection to clipboard", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_CONNECTION, NULL },
		{ "Sftp", "SFTP client", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_CONNECTION, NULL },
		{ "SftpPreserveTimes", "SFTP preserve times", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_CONNECTION, NULL },
		{ "Connect", "Host at start", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_START, NULL }
	};
	struct terminal_page_program program;
	struct terminal_page_state state;
	struct terminal_window window;
	const uint32_t *pixels;
	int status;

	memset(&program, 0, sizeof(program));
	program.config_path = "ssh/config";
	program.config_shown = "ssh/config";
	program.key_directory = "ssh/";
	program.key_directory_shown = "ssh/";
	program.known_hosts_path = "ssh/known_hosts";
	program.known_hosts_shown = "ssh/known_hosts";
	program.read = stub_read;
	program.items = items;
	program.item_count = (unsigned)(sizeof(items) / sizeof(items[0]));
	program.item_get = stub_item_get;
	program.item_set = stub_item_set;
	program.comment = "me@host";
	program.x11 = 1;

	memset(&state, 0, sizeof(state));
	state.pixels = 16U;
	state.theme = TERMINAL_THEME_DARK;
	state.scale = 1U;
	memset(&window, 0, sizeof(window));

	assert(terminal_page_open(&program, NULL, NULL) == 0);
	/* First draw: Hosts pane; stub clicks Behavior in the sidebar. */
	status = terminal_page_draw(&window, &state, 800U, 600U, &pixels);
	assert(status == 0 && pixels != NULL);
	/* Second draw: renders Behavior pane including the 13th item ("Connect"). */
	status = terminal_page_draw(&window, &state, 800U, 600U, &pixels);
	assert(status == 0 && pixels != NULL);
	assert(behavior_fields_drawn >= 9U);
	assert(thirteenth_item_drawn == 1);
	terminal_page_close();

	printf("settings-behavior-host-test: PASS\n");
	return 0;
}
