/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit test for Shell > Open File in vi... (menu.c), open_vi
 * (run.h / run.c), and TERMINAL_READY_TAB opening a normal terminal grid
 * connection via run_tab_connection_open().
 */

#include "run.h"
#include "connection.h"
#include "display.h"
#include "settings-page.h"

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

struct kl_app { int dummy; };
struct kl_window { int dummy; };
struct terminal_display { int dummy; };

static struct kl_app mock_app_obj;
static struct kl_window mock_win_obj;
static struct terminal_display mock_disp_obj;
static uint64_t mock_clock_val = 1000000ULL;

static int menu_has_open_vi;
static unsigned menu_open_vi_mod;
static uint32_t menu_open_vi_key;
static int mock_dispatch_budget;

struct step_item {
	int is_event;
	struct kl_window_event event;
	int ready_tab;
};

static struct step_item steps[16];
static unsigned step_count;
static unsigned step_pos;

struct mock_conn {
	struct terminal_connection base;
	char output[64];
	size_t output_len;
	int closed;
	unsigned cols;
	unsigned rows;
	struct terminal_connection *pending_tab;
};

static struct mock_conn *root_conn;
static struct mock_conn *vi_conn_obj;
static struct mock_conn *take_conn_obj;
static int open_vi_calls;
static int open_vi_cancel_next;

static size_t
conn_read(struct terminal_connection *c, unsigned char *buf, size_t sz)
{
	struct mock_conn *mc = (struct mock_conn *)c;
	size_t take = mc->output_len;

	/* Clamps the read size and copies any buffered mock output. */
	if (take > sz)
		take = sz;
	if (take > 0U) {
		memcpy(buf, mc->output, take);
		mc->output_len -= take;
	}

	/* Returns the byte count transferred. */
	return take;
}

static int
conn_write(struct terminal_connection *c, const unsigned char *b, size_t sz, size_t *w)
{
	(void)c;
	(void)b;
	*w = sz;
	return 0;
}

static int
conn_resize(struct terminal_connection *c, unsigned cols, unsigned rows, unsigned w, unsigned h)
{
	struct mock_conn *mc = (struct mock_conn *)c;

	/* Updates the recorded mock terminal dimensions. */
	(void)w;
	(void)h;
	mc->cols = cols;
	mc->rows = rows;
	return 0;
}

static int
conn_mode(const struct terminal_connection *c, unsigned *m)
{
	(void)c;
	*m = 0U;
	return 0;
}

static unsigned
conn_event(struct terminal_connection *c, const struct kl_app_event *ev)
{
	struct mock_conn *mc = (struct mock_conn *)c;
	unsigned r = 0U;

	/* Signals a pending tab when posted from the test loop. */
	if (ev->kind == KL_APP_POST && mc->pending_tab != NULL)
		r |= TERMINAL_READY_TAB;
	return r;
}

static unsigned
conn_pending(const struct terminal_connection *c)
{
	struct mock_conn *mc = (struct mock_conn *)c;
	unsigned r = 0U;

	/* Reports any queued output or pending tab readiness. */
	if (mc->output_len > 0U)
		r |= TERMINAL_READY_OUTPUT;
	if (mc->pending_tab != NULL)
		r |= TERMINAL_READY_TAB;
	return r;
}

static void
conn_close(struct terminal_connection *c)
{
	struct mock_conn *mc = (struct mock_conn *)c;

	/* Marks the mock connection as closed. */
	mc->closed = 1;
}

static struct terminal_connection *
conn_take(struct terminal_connection *c)
{
	struct mock_conn *mc = (struct mock_conn *)c;
	struct terminal_connection *ret = mc->pending_tab;

	/* Hands off and clears the pending tab connection. */
	mc->pending_tab = NULL;
	return ret;
}

static const struct terminal_connection_ops mock_conn_ops = {
	conn_read, conn_write, conn_resize, conn_mode,
	conn_event, conn_pending, conn_close, conn_take, NULL, NULL
};

static int
mock_open(void *data, struct kl_app *app, uint32_t id, unsigned cols, unsigned rows,
    unsigned w, unsigned h, char *title, size_t size, struct terminal_connection **out)
{
	(void)data;
	(void)app;
	(void)id;
	/* Updates the recorded mock terminal dimensions. */
	(void)w;
	(void)h;
	root_conn = calloc(1U, sizeof(*root_conn));
	root_conn->base.ops = &mock_conn_ops;
	root_conn->cols = cols;
	root_conn->rows = rows;
	snprintf(title, size, "Shell");
	*out = &root_conn->base;
	return 0;
}

static int
mock_open_vi(void *data, struct terminal_window *window, unsigned cols, unsigned rows,
    const char *path, char *title, size_t size, struct terminal_connection **connection)
{
	(void)data;
	(void)window;
	(void)path;
	open_vi_calls++;
	if (open_vi_cancel_next) {
		open_vi_cancel_next = 0;
		return ECANCELED;
	}

	/* Allocates and populates the mock vi connection. */
	vi_conn_obj = calloc(1U, sizeof(*vi_conn_obj));
	vi_conn_obj->base.ops = &mock_conn_ops;
	vi_conn_obj->cols = cols;
	vi_conn_obj->rows = rows;
	snprintf(vi_conn_obj->output, sizeof(vi_conn_obj->output), "VI-READY");
	vi_conn_obj->output_len = 8U;
	snprintf(title, size, "vi: test.txt");
	*connection = &vi_conn_obj->base;
	return 0;
}

/* ---- Libkeiland & terminal mocks ---- */
uint64_t kl_monotonic_us(void) { return mock_clock_val; }
uint64_t kl_clock_us(void) { return mock_clock_val; }
struct kl_app *kl_app_open(const struct kl_app_options *o) { (void)o; return &mock_app_obj; }
struct kl_window *kl_app_window_create(struct kl_app *a, const struct kl_window_options *o) { (void)a; (void)o; return &mock_win_obj; }
void kl_app_close(struct kl_app *a) { (void)a; }
int kl_app_dispatch(struct kl_app *a, int t) { (void)a; (void)t; mock_dispatch_budget = 1; return 0; }
int kl_app_take(struct kl_app *a, struct kl_app_event *e)
{
	(void)a;
	if (mock_dispatch_budget <= 0 || step_pos >= step_count)
		return 0;
	mock_dispatch_budget--;
	memset(e, 0, sizeof(*e));
	if (steps[step_pos].ready_tab) {
		take_conn_obj = calloc(1U, sizeof(*take_conn_obj));
		take_conn_obj->base.ops = &mock_conn_ops;
		root_conn->pending_tab = &take_conn_obj->base;
		e->kind = KL_APP_POST;
		step_pos++;
		return 1;
	}

	/* Delivers the next scripted window input event. */
	if (steps[step_pos].is_event) {
		e->kind = KL_APP_WINDOW;
		e->window = &mock_win_obj;
		e->input = steps[step_pos].event;
		step_pos++;
		return 1;
	}

	/* Advances past idle steps. */
	step_pos++;
	return 0;
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
int kl_window_set_menu(struct kl_window *w, const struct kl_menu_entry *e, size_t c)
{
	size_t i;
	/* Updates the recorded mock terminal dimensions. */
	(void)w;
	if (e == NULL || c == 0U)
		return 0;
	menu_has_open_vi = 0;
	menu_open_vi_mod = 0U;
	menu_open_vi_key = 0U;
	for (i = 0U; i < c; i++) {
		if (e[i].action == TERMINAL_ACTION_OPEN_VI) {
			menu_has_open_vi = 1;
			menu_open_vi_mod = e[i].modifiers;
			menu_open_vi_key = e[i].keysym;
		}
	}

	/* Menu update succeeds. */
	return 0;
}
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

int terminal_font_open(struct terminal_font *f, const char *p, const char *c, unsigned px)
{
	(void)p; (void)c; (void)px;
	memset(f, 0, sizeof(*f));
	f->cell_width = 8U;
	f->cell_height = 16U;
	return 0;
}
int terminal_font_fallback(struct terminal_font *f, const char *p) { (void)f; (void)p; return 0; }
void terminal_font_emoji(struct terminal_font *f, const char *p) { (void)f; (void)p; }
int terminal_font_resize(struct terminal_font *f, unsigned px) { (void)f; (void)px; return 0; }
void terminal_font_close(struct terminal_font *f) { (void)f; }
int terminal_display_open(struct terminal_display **d, struct terminal_window *w, struct terminal_font *f)
{
	/* Updates the recorded mock terminal dimensions. */
	(void)w; (void)f;
	*d = &mock_disp_obj;
	return 0;
}
void terminal_display_close(struct terminal_display *d) { (void)d; }
int terminal_display_draw(struct terminal_display *d, struct terminal_screen *s, struct terminal_font *f, const struct terminal_window *w)
{
	(void)d; (void)s; (void)f; (void)w;
	return 0;
}
int terminal_display_page(struct terminal_display *d, const uint32_t *px, uint32_t w, uint32_t h) { (void)d; (void)px; (void)w; (void)h; return 0; }
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
int terminal_page_draw(struct terminal_window *w, const struct terminal_page_state *st, uint32_t width, uint32_t height, const uint32_t **px)
{
	/* Updates the recorded mock terminal dimensions. */
	(void)w; (void)st; (void)width; (void)height; (void)px;
	return 0;
}
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

int
main(void)
{
	struct terminal_connector connector;
	struct terminal_run_options options;
	struct terminal_run_result result;
	int status;

	/* Initializes the mock connector. */
	memset(&connector, 0, sizeof(connector));
	connector.open = mock_open;
	connector.operation = "mock_open";

	/* 1. open_vi_menu = 0 hides Open File in vi...; open_vi_menu = 1 shows it with Ctrl+Shift+E. */
	memset(&options, 0, sizeof(options));
	options.application = "test";
	options.title = "test";
	options.columns = 80U;
	options.rows = 24U;
	options.token = "t1";
	options.connector = &connector;
	options.open_vi_menu = 0;

	/* Scripts a single close event to verify hidden menu state. */
	step_count = 1U;
	step_pos = 0U;
	memset(steps, 0, sizeof(steps));
	steps[0].is_event = 1;
	steps[0].event.kind = KL_WINDOW_CLOSE;

	/* Runs the terminal loop and checks open_vi_menu = 0. */
	status = terminal_run(&options, &result);
	CHECK(status == 0);
	CHECK(menu_has_open_vi == 0);
	if (root_conn != NULL) { free(root_conn); root_conn = NULL; }

	/* 2. open_vi_menu = 1: cancel (ECANCELED) + open vi tab + TAKE normal grid tab. */
	options.open_vi_menu = 1;
	options.open_vi = mock_open_vi;
	open_vi_calls = 0;
	open_vi_cancel_next = 1;

	/* Scripts cancel, open vi, ready tab, and close events. */
	step_count = 4U;
	step_pos = 0U;
	memset(steps, 0, sizeof(steps));
	steps[0].is_event = 1;
	steps[0].event.kind = KL_WINDOW_ACTION;
	steps[0].event.code = TERMINAL_ACTION_OPEN_VI;
	steps[1].is_event = 1;
	steps[1].event.kind = KL_WINDOW_ACTION;
	steps[1].event.code = TERMINAL_ACTION_OPEN_VI;
	steps[2].ready_tab = 1;
	steps[3].is_event = 1;
	steps[3].event.kind = KL_WINDOW_CLOSE;

	/* Runs the terminal loop and validates open_vi and tab handoff. */
	status = terminal_run(&options, &result);
	CHECK(status == 0);
	CHECK(menu_has_open_vi == 1);
	CHECK(menu_open_vi_mod == (KL_MENU_CTRL | KL_MENU_SHIFT));
	CHECK(menu_open_vi_key == 'e');
	CHECK(open_vi_calls == 2);
	CHECK(vi_conn_obj != NULL && vi_conn_obj->closed == 1);
	CHECK(take_conn_obj != NULL && take_conn_obj->closed == 1);

	/* Frees remaining mock allocations. */
	free(root_conn);
	free(vi_conn_obj);
	free(take_conn_obj);

	/* Reports test totals and exit status. */
	printf("checks %d, failures %d\n", checks, failures);
	if (failures != 0) {
		printf("terminal-open-vi-host-test: FAIL\n");
		return 1;
	}

	/* All checks passed. */
	printf("terminal-open-vi-host-test: PASS\n");
	return 0;
}
