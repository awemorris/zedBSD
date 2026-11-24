/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit test for view tab input delivery (run.c, window.c): verifies
 * that pointer motion, button press/release, wheel axis, key, and text
 * events queued for an active view tab are delivered to view->input and
 * trigger a redraw without being discarded by run_picture_inputs().
 */

#include "run.h"
#include "connection.h"
#include "display.h"
#include "settings-page.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
	} \
} while (0)

#define MAX_SCRIPT_EVENTS 32U
#define MAX_RECORDED_INPUTS 32U

struct kl_app {
	int dummy;
};

struct kl_window {
	int dummy;
};

struct terminal_display {
	int dummy;
};

static struct kl_app mock_app_obj;
static struct kl_window mock_win_obj;
static struct terminal_display mock_disp_obj;
static uint64_t mock_clock_val = 1000000ULL;

static struct kl_window_event script_events[MAX_SCRIPT_EVENTS];
static unsigned script_count;
static unsigned script_pos;
static int dispatch_budget;
static int display_page_calls;
static int g_view_taken;

/* ---- Mock libkeiland, display, font, page, touch ---- */

uint64_t kl_monotonic_us(void) { return mock_clock_val; }
uint64_t kl_clock_us(void) { return mock_clock_val; }
struct kl_app *kl_app_open(const struct kl_app_options *o) { (void)o; return &mock_app_obj; }
struct kl_window *kl_app_window_create(struct kl_app *a, const struct kl_window_options *o) { (void)a; (void)o; return &mock_win_obj; }
void kl_app_close(struct kl_app *a) { (void)a; }

int
kl_app_dispatch(struct kl_app *a, int t)
{
	(void)a;
	(void)t;
	if (script_pos < script_count)
		dispatch_budget = 1;
	else
		mock_clock_val += 500000ULL;
	return 0;
}

int
kl_app_take(struct kl_app *a, struct kl_app_event *e)
{
	(void)a;
	if (dispatch_budget <= 0 || !g_view_taken || script_pos >= script_count)
		return 0;
	dispatch_budget--;
	memset(e, 0, sizeof(*e));
	e->kind = KL_APP_WINDOW;
	e->window = &mock_win_obj;
	e->input = script_events[script_pos];
	script_pos++;
	return 1;
}

int kl_app_post(struct kl_app *a, uint32_t c, void *d) { (void)a; (void)c; (void)d; return 0; }
void kl_app_wake(struct kl_app *a) { (void)a; }
int kl_app_process_poll(struct kl_app *a) { (void)a; return 0; }
int kl_app_has_timers(const struct kl_app *a) { (void)a; return 0; }
int kl_app_timer_wait(const struct kl_app *a, uint64_t n) { (void)a; (void)n; return -1; }
int kl_app_run_timers(struct kl_app *a, uint64_t n) { (void)a; (void)n; return 0; }
int kl_app_open_socket(struct kl_app *a, int f, int t, int p) { (void)a; (void)f; (void)t; (void)p; return -1; }
struct kl_window *kl_window_open(const struct kl_window_options *o) { (void)o; return &mock_win_obj; }
void kl_window_close(struct kl_window *w) { (void)w; }
int kl_window_dispatch(struct kl_window *w, int t) { (void)w; (void)t; return 0; }
int kl_window_take(struct kl_window *w, struct kl_window_event *e) { (void)w; (void)e; return 0; }
void kl_window_set_title(struct kl_window *w, const char *t) { (void)w; (void)t; }
void kl_window_size(const struct kl_window *w, uint32_t *pw, uint32_t *ph) { (void)w; *pw = 800U; *ph = 600U; }
unsigned kl_window_scale(const struct kl_window *w) { (void)w; return 96U; }
int kl_window_follow_scale(struct kl_window *w) { (void)w; return 0; }
int kl_window_present_resize(struct kl_window *w, uint32_t *pw, uint32_t *ph) { (void)w; (void)pw; (void)ph; return 0; }
int kl_window_present(struct kl_window *w, const uint32_t *p, size_t s) { (void)w; (void)p; (void)s; return 0; }
int kl_window_present_part(struct kl_window *w, const uint32_t *p, size_t s, const struct kl_rect *r) { (void)w; (void)p; (void)s; (void)r; return 0; }
int kl_window_present_parts(struct kl_window *w, const uint32_t *p, size_t s, const struct kl_rect *r, unsigned c) { (void)w; (void)p; (void)s; (void)r; (void)c; return 0; }
int kl_window_set_pacing(struct kl_window *w, unsigned f, unsigned m) { (void)w; (void)f; (void)m; return 0; }
int kl_window_frame_wait(const struct kl_window *w, uint64_t n) { (void)w; (void)n; return 0; }
int kl_window_set_configure_deferred(struct kl_window *w, int d) { (void)w; (void)d; return 0; }
void kl_window_copy(struct kl_window *w, const char *t, size_t l) { (void)w; (void)t; (void)l; }
size_t kl_window_paste(struct kl_window *w, char *t, size_t s) { (void)w; (void)t; (void)s; return 0U; }
int kl_window_can_paste(const struct kl_window *w) { (void)w; return 0; }
void kl_window_text_input(struct kl_window *w, int e) { (void)w; (void)e; }
void kl_window_text_cursor(struct kl_window *w, int x, int y, int width, int height) { (void)w; (void)x; (void)y; (void)width; (void)height; }
void kl_window_select(struct kl_window *w, const char *t, size_t l) { (void)w; (void)t; (void)l; }
int kl_window_selecting(const struct kl_window *w) { (void)w; return 0; }
size_t kl_window_paste_primary(struct kl_window *w, char *t, size_t s) { (void)w; (void)t; (void)s; return 0U; }
void kl_window_set_fullscreen(struct kl_window *w, int f) { (void)w; (void)f; }
int kl_window_fullscreen(const struct kl_window *w) { (void)w; return 0; }
int kl_window_set_menu(struct kl_window *w, const struct kl_menu_entry *e, size_t c) { (void)w; (void)e; (void)c; return 0; }
int kl_window_set_action_state(struct kl_window *w, uint32_t a, unsigned s) { (void)w; (void)a; (void)s; return 0; }
int kl_window_set_tabs(struct kl_window *w, const struct kl_tab_entry *t, size_t c, unsigned o) { (void)w; (void)t; (void)c; (void)o; return 0; }
int kl_window_set_repeat(struct kl_window *w, int e) { (void)w; (void)e; return 0; }
void kl_window_answer_drop(struct kl_window *w, unsigned a, unsigned p) { (void)w; (void)a; (void)p; }
int kl_window_receive_drop(struct kl_window *w, char **d, size_t *l, unsigned *t) { (void)w; (void)d; (void)l; (void)t; return 0; }
void kl_window_finish_drop(struct kl_window *w, unsigned a) { (void)w; (void)a; }
int kl_window_accept_drops(struct kl_window *w, unsigned t) { (void)w; (void)t; return 0; }
int kl_window_repeat(struct kl_window *w, uint64_t n) { (void)w; (void)n; return 0; }
int kl_window_repeat_wait(const struct kl_window *w, uint64_t n) { (void)w; (void)n; return -1; }
void kl_window_edit_by_keys(struct kl_window *w) { (void)w; }
int kl_window_set_opaque(struct kl_window *w, int o) { (void)w; (void)o; return 0; }
int kl_window_drag_text(struct kl_window *w, const char *t, size_t l, uint32_t s) { (void)w; (void)t; (void)l; (void)s; return 0; }
int kl_window_selection_own(const struct kl_window *w, unsigned wh) { (void)w; (void)wh; return 0; }
size_t kl_window_take_drop(struct kl_window *w, char *t, size_t s, unsigned *tp) { (void)w; (void)t; (void)s; (void)tp; return 0U; }

int terminal_font_open(struct terminal_font *f, const char *p, const char *c, unsigned px) { (void)p; (void)c; (void)px; memset(f, 0, sizeof(*f)); f->cell_width = 8U; f->cell_height = 16U; return 0; }
int terminal_font_fallback(struct terminal_font *f, const char *p) { (void)f; (void)p; return 0; }
void terminal_font_emoji(struct terminal_font *f, const char *p) { (void)f; (void)p; }
int terminal_font_resize(struct terminal_font *f, unsigned px) { (void)f; (void)px; return 0; }
void terminal_font_close(struct terminal_font *f) { (void)f; }
int terminal_display_open(struct terminal_display **d, struct terminal_window *w, struct terminal_font *f) { (void)w; (void)f; *d = &mock_disp_obj; return 0; }
void terminal_display_close(struct terminal_display *d) { (void)d; }
int terminal_display_draw(struct terminal_display *d, struct terminal_screen *s, struct terminal_font *f, const struct terminal_window *w) { (void)d; (void)s; (void)f; (void)w; return 0; }

int
terminal_display_page(struct terminal_display *d, const uint32_t *px, uint32_t w, uint32_t h)
{
	(void)d;
	(void)px;
	(void)w;
	(void)h;
	display_page_calls++;
	return 0;
}

int terminal_display_idle(struct terminal_display *d) { (void)d; return 1; }
void terminal_display_size(const struct terminal_display *d, uint32_t *w, uint32_t *h) { (void)d; *w = 800U; *h = 600U; }
const char *terminal_display_operation(const struct terminal_display *d, int *r) { (void)d; if (r != NULL) *r = 0; return "ok"; }
int terminal_display_resize(struct terminal_display *d, uint32_t w, uint32_t h) { (void)d; (void)w; (void)h; return 0; }
int terminal_display_present_wait(struct terminal_display *d) { (void)d; return 0; }
void terminal_settings_load(struct terminal_settings *s) { memset(s, 0, sizeof(*s)); }
int terminal_settings_save(const struct terminal_settings *s) { (void)s; return 0; }
int terminal_page_open(const struct terminal_page_program *p, const char *f, const char *fb) { (void)p; (void)f; (void)fb; return 0; }
void terminal_page_close(void) {}
void terminal_page_shown(struct terminal_window *w) { (void)w; }
void terminal_page_paste(const char *t, size_t l) { (void)t; (void)l; }
size_t terminal_page_copy_text(char *t, size_t s) { (void)t; (void)s; return 0U; }
int terminal_page_copy_emptied(void) { return 0; }
int terminal_page_draw(struct terminal_window *w, const struct terminal_page_state *st, uint32_t width, uint32_t height, const uint32_t **px) { (void)w; (void)st; (void)width; (void)height; (void)px; return 0; }
int terminal_page_input(struct terminal_window *w, const struct kl_window_event *e) { (void)w; (void)e; return 0; }
int terminal_page_text_wanted(void) { return 0; }
int terminal_page_moving(void) { return 0; }
void terminal_page_reset_scroll(void) {}
int terminal_touch_open(struct terminal_touch *t) { (void)t; return 0; }
void terminal_touch_close(struct terminal_touch *t) { (void)t; }
void terminal_touch_layout(struct terminal_touch *t, const void *s, unsigned ch, unsigned h, unsigned gh, unsigned v, int o) { (void)t; (void)s; (void)ch; (void)h; (void)gh; (void)v; (void)o; }
void terminal_touch_event(struct terminal_touch *t, const struct terminal_touch_event *e) { (void)t; (void)e; }
int terminal_touch_tick(struct terminal_touch *t, uint64_t n) { (void)t; (void)n; return 0; }
int terminal_touch_view(struct terminal_touch *t, unsigned *v, int *o) { (void)t; (void)v; (void)o; return 0; }
int terminal_touch_take_pointer(struct terminal_touch *t, struct terminal_touch_pointer *p) { (void)t; (void)p; return 0; }
uint64_t terminal_touch_clock(void) { return mock_clock_val; }

/* ---- Mock view & shell connections ---- */

struct mock_view_conn {
	struct terminal_connection base;
	int dirty;
	int draw_calls;
	unsigned input_count;
	struct terminal_picture_event inputs[MAX_RECORDED_INPUTS];
};

static struct mock_view_conn g_view;
static struct terminal_connection g_shell;

static size_t mock_view_read(struct terminal_connection *c, unsigned char *b, size_t n) { (void)c; (void)b; (void)n; return 0U; }
static int mock_view_write(struct terminal_connection *c, const unsigned char *b, size_t n, size_t *w) { (void)c; (void)b; *w = n; return 0; }
static int mock_view_resize(struct terminal_connection *c, unsigned cols, unsigned rows, unsigned w, unsigned h) { (void)c; (void)cols; (void)rows; (void)w; (void)h; return 0; }
static int mock_view_mode(const struct terminal_connection *c, unsigned *m) { (void)c; (void)m; return ENOTSUP; }
static unsigned mock_view_event(struct terminal_connection *c, const struct kl_app_event *ev) { (void)c; (void)ev; return 0U; }
static unsigned mock_view_pending(const struct terminal_connection *c)
{
	const struct mock_view_conn *v = (const struct mock_view_conn *)c;
	if (v->dirty)
		return TERMINAL_READY_OUTPUT;
	return 0U;
}
static void mock_view_close(struct terminal_connection *c) { (void)c; }

static int mock_view_draw(struct terminal_connection *c, const struct terminal_view_state *s, uint32_t *px, uint32_t w, uint32_t h)
{
	struct mock_view_conn *v = (struct mock_view_conn *)c;
	(void)s;
	(void)px;
	(void)w;
	(void)h;
	v->draw_calls++;
	v->dirty = 0;
	return 0;
}

static void mock_view_title(struct terminal_connection *c, char *t, size_t sz)
{
	(void)c;
	(void)snprintf(t, sz, "SFTP: test");
}

static void mock_view_input(struct terminal_connection *c, const struct terminal_picture_event *ev)
{
	struct mock_view_conn *v = (struct mock_view_conn *)c;
	if (v->input_count < MAX_RECORDED_INPUTS)
		v->inputs[v->input_count++] = *ev;
	v->dirty = 1;
}

static const struct terminal_view_ops mock_view_ops = {
	mock_view_draw,
	mock_view_title,
	mock_view_input,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

static const struct terminal_connection_ops mock_view_conn_ops = {
	mock_view_read,
	mock_view_write,
	mock_view_resize,
	mock_view_mode,
	mock_view_event,
	mock_view_pending,
	mock_view_close,
	NULL,
	NULL,
	&mock_view_ops
};

static unsigned mock_shell_pending(const struct terminal_connection *c)
{
	(void)c;
	if (g_view_taken)
		return 0U;
	return TERMINAL_READY_TAB;
}

static struct terminal_connection *mock_shell_take(struct terminal_connection *c)
{
	(void)c;
	if (g_view_taken)
		return NULL;
	g_view_taken = 1;
	return &g_view.base;
}

static const struct terminal_connection_ops mock_shell_ops = {
	mock_view_read,
	mock_view_write,
	mock_view_resize,
	mock_view_mode,
	mock_view_event,
	mock_shell_pending,
	mock_view_close,
	mock_shell_take,
	NULL,
	NULL
};

static int
mock_connect(void *data, struct kl_app *app, uint32_t id, unsigned cols, unsigned rows,
    unsigned w, unsigned h, char *title, size_t size, struct terminal_connection **out)
{
	(void)data;
	(void)app;
	(void)id;
	(void)cols;
	(void)rows;
	(void)w;
	(void)h;
	(void)snprintf(title, size, "Shell");
	*out = &g_shell;
	return 0;
}

static void
push_event(const struct kl_window_event *ev)
{
	assert(script_count < MAX_SCRIPT_EVENTS);
	script_events[script_count++] = *ev;
}

int
main(void)
{
	struct terminal_run_options opts;
	struct terminal_connector conn;
	struct terminal_run_result res;
	struct kl_window_event ev;
	int st;

	/* Initializes the mock shell and view connections. */
	memset(&g_view, 0, sizeof(g_view));
	g_view.base.ops = &mock_view_conn_ops;
	memset(&g_shell, 0, sizeof(g_shell));
	g_shell.ops = &mock_shell_ops;
	g_view_taken = 0;

	/* 1. Motion */
	memset(&ev, 0, sizeof(ev));
	ev.kind = KL_WINDOW_MOTION;
	ev.x = 120.0;
	ev.y = 80.0;
	push_event(&ev);

	/* 2. Left mouse button press */
	memset(&ev, 0, sizeof(ev));
	ev.kind = KL_WINDOW_BUTTON;
	ev.code = 0x110U;
	ev.pressed = 1;
	ev.x = 120.0;
	ev.y = 80.0;
	ev.modifiers = KL_MOD_CTRL;
	push_event(&ev);

	/* 3. Left mouse button release */
	memset(&ev, 0, sizeof(ev));
	ev.kind = KL_WINDOW_BUTTON;
	ev.code = 0x110U;
	ev.pressed = 0;
	ev.x = 120.0;
	ev.y = 80.0;
	push_event(&ev);

	/* 4. Mouse wheel axis */
	memset(&ev, 0, sizeof(ev));
	ev.kind = KL_WINDOW_AXIS;
	ev.dy = 60.0;
	push_event(&ev);

	/* 5. Key press (F5) */
	memset(&ev, 0, sizeof(ev));
	ev.kind = KL_WINDOW_KEY;
	ev.code = 63U;
	ev.pressed = 1;
	ev.modifiers = KL_MOD_SHIFT;
	push_event(&ev);

	/* 6. Committed text */
	memset(&ev, 0, sizeof(ev));
	ev.kind = KL_WINDOW_TEXT_COMMIT;
	(void)snprintf(ev.text, sizeof(ev.text), "abc");
	push_event(&ev);

	/* Configures the mock connector and terminal run options. */
	memset(&conn, 0, sizeof(conn));
	conn.open = mock_connect;
	memset(&opts, 0, sizeof(opts));
	opts.title = "test";
	opts.present = KL_PRESENT_SHM;
	opts.pixels = 16U;
	opts.columns = 80U;
	opts.rows = 24U;
	opts.token = "view-input";
	opts.connector = &conn;
	opts.timeout = 1U;

	/* Runs the terminal loop and verifies all 6 view inputs and redraws. */
	st = terminal_run(&opts, &res);
	CHECK(st == 0);
	CHECK(strcmp(res.reason, "timeout") == 0);
	CHECK(g_view.input_count == 7U);
	CHECK(g_view.inputs[0].kind == TERMINAL_PICTURE_FOCUS);
	CHECK(g_view.inputs[1].kind == TERMINAL_PICTURE_MOTION);
	CHECK(g_view.inputs[1].x == 120 && g_view.inputs[1].y == 80);
	CHECK(g_view.inputs[2].kind == TERMINAL_PICTURE_BUTTON);
	CHECK(g_view.inputs[2].code == 0x110U && g_view.inputs[2].pressed == 1);
	CHECK(g_view.inputs[2].x == 120 && g_view.inputs[2].y == 80);
	CHECK(g_view.inputs[2].modifiers == TERMINAL_MODIFIER_CONTROL);
	CHECK(g_view.inputs[3].kind == TERMINAL_PICTURE_BUTTON);
	CHECK(g_view.inputs[3].code == 0x110U && g_view.inputs[3].pressed == 0);
	CHECK(g_view.inputs[4].kind == TERMINAL_PICTURE_AXIS);
	CHECK(g_view.inputs[4].dy == 60.0);
	CHECK(g_view.inputs[5].kind == TERMINAL_PICTURE_KEY);
	CHECK(g_view.inputs[5].code == 63U && g_view.inputs[5].pressed == 1);
	CHECK(g_view.inputs[5].modifiers == TERMINAL_MODIFIER_SHIFT);
	CHECK(g_view.inputs[6].kind == TERMINAL_PICTURE_TEXT);
	CHECK(strcmp(g_view.inputs[6].text, "abc") == 0);
	CHECK(g_view.draw_calls >= 2);
	CHECK(display_page_calls >= 2);

	/* Reports test summary and status. */
	printf("checks %d, failures %d\n", checks, failures);
	if (failures == 0) {
		printf("terminal-view-input-host-test: PASS\n");
		return 0;
	}

	/* Reports failure status. */
	printf("terminal-view-input-host-test: FAIL\n");
	return 1;
}
