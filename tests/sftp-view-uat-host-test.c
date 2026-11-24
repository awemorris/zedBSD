/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit test for SFTP view UAT fixes:
 * 1. Single wheel event keeps view->dirty=1 while scroll glide animates.
 * 2. Context menu card background uses ~95% opacity (alpha 0xf2).
 * 3. Pointer motion outside view bounds during drag triggers external OLE drag.
 * 4. Download transfer bar displays the local destination path.
 */

#include "sftp-view.h"
#include "sftp-local.h"
#include "sftp-transfer.h"
#include "userland/desktop/terminal/terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;
static uint64_t g_now_us = 1000000ULL;
static char g_navigated_path[256];
static unsigned g_download_btn_flags;
static int g_upload_clicked;
static int g_more_opened;
static char g_captured_texts[32][512];
static size_t g_captured_count;
static int g_dummy_gesture;
static int g_dummy_scroller;
static int g_dummy_worker;
static int g_change_download_dir_calls;
static int g_enqueue_download_calls;
static char g_last_download_remote[512];
static char g_last_download_local[512];

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
	} \
} while (0)

/* ---- Minimal mocks for libkeiland & sftp-view dependencies ---- */

uint64_t kl_clock_us(void) { return g_now_us; }
uint64_t kl_monotonic_us(void) { return g_now_us; }
int kl_mutex_init(struct kl_mutex *m) { (void)m; return 0; }
void kl_mutex_destroy(struct kl_mutex *m) { (void)m; }
void kl_mutex_lock(struct kl_mutex *m) { (void)m; }
void kl_mutex_unlock(struct kl_mutex *m) { (void)m; }
int kl_app_post(struct kl_app *a, unsigned c, void *d) { (void)a; (void)c; (void)d; return 0; }
int kl_text_open(struct kl_text *t, const char *p, const char *f) { (void)t; (void)p; (void)f; return 0; }
void kl_text_close(struct kl_text *t) { (void)t; }

int
kl_text_center(unsigned pixels, int y, int height)
{
	(void)pixels;
	return y + height / 2;
}

int
kl_text_draw(
	struct kl_text *t,
	struct kl_canvas *c,
	int x,
	int y,
	const char *s,
	size_t len,
	unsigned px,
	int bold,
	kl_color col)
{
	size_t n;

	/* Captures text drawn by the view. */
	(void)t;
	(void)c;
	(void)x;
	(void)y;
	(void)px;
	(void)bold;
	(void)col;
	if (s != NULL && g_captured_count < 32U) {
		n = len;
		if (n > 511U)
			n = 511U;
		memcpy(g_captured_texts[g_captured_count], s, n);
		g_captured_texts[g_captured_count][n] = '\0';
		g_captured_count++;
	}

	/* Returns success. */
	return 0;
}

int
kl_text_draw_fit(
	struct kl_text *t,
	struct kl_canvas *c,
	int x,
	int y,
	const char *s,
	unsigned px,
	int bold,
	int w,
	kl_color col)
{
	(void)t;
	(void)c;
	(void)x;
	(void)y;
	(void)px;
	(void)bold;
	(void)w;
	(void)col;
	if (s != NULL && g_captured_count < 32U) {
		(void)snprintf(g_captured_texts[g_captured_count], 512U, "%s", s);
		g_captured_count++;
	}

	/* Returns success. */
	return 0;
}

int kl_text_width(struct kl_text *t, const char *s, size_t len, unsigned px, int bold)
{
	(void)t;
	(void)s;
	(void)px;
	(void)bold;
	return (int)len * 7;
}

void kl_text_metrics(struct kl_text *text, unsigned pixels, struct kl_text_line *line)
{
	(void)text;
	if (line != NULL) {
		memset(line, 0, sizeof(*line));
		line->height = (int)pixels;
		line->ascent = (int)pixels;
	}
}

size_t
kl_text_break(struct kl_text *text, const char *string, unsigned pixels, int bold, int width)
{
	(void)text;
	(void)pixels;
	(void)bold;
	(void)width;
	if (string == NULL)
		return 0U;

	/* Returns full string length. */
	return strlen(string);
}

unsigned kl_appearance_get(const struct kl_appearance *appearance) { (void)appearance; return 0U; }
int kl_window_receive_drop(struct kl_window *w, char **d, size_t *l, unsigned *t) { (void)w; (void)d; (void)l; (void)t; return -1; }
void kl_window_finish_drop(struct kl_window *w, unsigned a) { (void)w; (void)a; }
int kl_window_can_paste(const struct kl_window *w) { (void)w; return 0; }
uint32_t kl_key_character(uint32_t key, unsigned modifiers) { (void)key; (void)modifiers; return 0U; }

struct kl_gesture *kl_gesture_create(void) { return (struct kl_gesture *)&g_dummy_gesture; }
void kl_gesture_destroy(struct kl_gesture *g) { (void)g; }
int kl_gesture_down(struct kl_gesture *g, int32_t id, uint64_t t, uint64_t a, double x, double y) { (void)g; (void)id; (void)t; (void)a; (void)x; (void)y; return 0; }
int kl_gesture_motion(struct kl_gesture *g, int32_t id, uint64_t t, uint64_t a, double x, double y) { (void)g; (void)id; (void)t; (void)a; (void)x; (void)y; return 0; }
int kl_gesture_up(struct kl_gesture *g, int32_t id, uint64_t t) { (void)g; (void)id; (void)t; return 0; }
void kl_gesture_cancel(struct kl_gesture *g) { (void)g; }
int kl_gesture_next(struct kl_gesture *g, uint64_t now_us, struct kl_gesture_event *ev) { (void)g; (void)now_us; (void)ev; return 0; }
int kl_gesture_drag_offset(struct kl_gesture *g, uint64_t t, double *dx, double *dy) { (void)g; (void)t; (void)dx; (void)dy; return -1; }

struct kl_scroller *kl_scroller_create(void) { return (struct kl_scroller *)&g_dummy_scroller; }
void kl_scroller_destroy(struct kl_scroller *s) { (void)s; }
int kl_scroller_set_bounds(struct kl_scroller *s, double a, double b, double c, double d, double e, double f) { (void)s; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return 0; }
void kl_scroller_set_position(struct kl_scroller *s, double x, double y) { (void)s; (void)x; (void)y; }
int kl_scroller_press(struct kl_scroller *s, uint64_t t) { (void)s; (void)t; return 0; }
void kl_scroller_drag(struct kl_scroller *s, double dx, double dy) { (void)s; (void)dx; (void)dy; }
void kl_scroller_cancel(struct kl_scroller *s, uint64_t t) { (void)s; (void)t; }
int kl_scroller_release(struct kl_scroller *s, uint64_t now_us, double vx, double vy) { (void)s; (void)now_us; (void)vx; (void)vy; return 0; }
int kl_scroller_tick(struct kl_scroller *s, uint64_t t, double *y, double *v) { (void)s; (void)t; (void)y; (void)v; return 0; }
int kl_scroller_axis(struct kl_scroller *s, double dx, double dy, uint64_t eu, uint64_t nu) { (void)s; (void)dx; (void)dy; (void)eu; (void)nu; return 0; }
int kl_scroller_axis_holding(const struct kl_scroller *s) { (void)s; return 0; }
int kl_scroller_axis_stop(struct kl_scroller *s, uint64_t eu, uint64_t nu, double *vx, double *vy) { (void)s; (void)eu; (void)nu; (void)vx; (void)vy; return 0; }
int kl_scroller_step(struct kl_scroller *s, uint64_t nu, double *x, double *y) { (void)s; (void)nu; (void)x; (void)y; return 0; }

struct kl_edit *kl_edit_create(struct wl_display *d, struct xdg_toplevel *t, kl_edit_fn cb, void *data) { (void)d; (void)t; (void)cb; (void)data; return NULL; }
void kl_edit_destroy(struct kl_edit *e) { (void)e; }
void kl_edit_set_state(struct kl_edit *e, uint32_t ops, uint32_t st) { (void)e; (void)ops; (void)st; }
void keiui_window_push(struct kl_ui *u, unsigned k, uint32_t c) { (void)u; (void)k; (void)c; }
const void *keiui_text_bar_calls = NULL;

void kl_text_touch_init(struct kl_text_touch *t, const struct kl_text_view *v, void *d) { (void)t; (void)v; (void)d; }
void kl_text_touch_tap(struct kl_text_touch *t, double x, double y, int tw) { (void)t; (void)x; (void)y; (void)tw; }
void kl_text_touch_long_press(struct kl_text_touch *t, double x, double y) { (void)t; (void)x; (void)y; }
int keiui_text_touch_hold(struct kl_text_touch *t, double x, double y) { (void)t; (void)x; (void)y; return 0; }
int keiui_text_touch_drag_begin(struct kl_text_touch *t, double x, double y) { (void)t; (void)x; (void)y; return 0; }
void kl_text_touch_drag(struct kl_text_touch *t, double x, double y) { (void)t; (void)x; (void)y; }
void kl_text_touch_drag_end(struct kl_text_touch *t) { (void)t; }
int keiui_text_touch_edge(struct kl_text_touch *t, uint64_t now, double *dx, double *dy) { (void)t; (void)now; (void)dx; (void)dy; return 0; }
void kl_text_touch_draw_handles(const struct kl_text_touch *t, struct kl_canvas *c, double ox, double oy, const struct kl_theme *th) { (void)t; (void)c; (void)ox; (void)oy; (void)th; }

void kl_list_header(const struct kl_style *s, const struct kl_rect *r, const struct kl_column *c, size_t n) { (void)s; (void)r; (void)c; (void)n; }
void kl_list_item(const struct kl_style *s, const struct kl_rect *r, unsigned st, kl_color *ink, kl_color *faint) { (void)s; (void)r; (void)st; if (ink) *ink = 0xff000000U; if (faint) *faint = 0xff666666U; }
void kl_list_cell(const struct kl_style *s, int x, int w, int b, const char *t, unsigned px, unsigned f, kl_color ink) { (void)s; (void)x; (void)w; (void)b; (void)t; (void)px; (void)f; (void)ink; }
void kl_band(const struct kl_style *s, const struct kl_rect *r) { (void)s; (void)r; }
void kl_panel(const struct kl_style *s, const struct kl_rect *r, int sb) { (void)s; (void)r; (void)sb; }
int kl_dialog(struct kl_ui *u, const struct kl_style *s, uint32_t id, const struct kl_rect *a, const char *t, const char *b, const char *const *l, int c) { (void)u; (void)s; (void)id; (void)a; (void)t; (void)b; (void)l; (void)c; return -1; }
int kl_icon_button(struct kl_ui *u, const struct kl_style *s, uint32_t id, uint32_t idx, const struct kl_rect *r, enum kl_icon i, int px, unsigned f) { (void)u; (void)s; (void)id; (void)idx; (void)r; (void)i; (void)px; (void)f; return 0; }
int kl_button_width(const struct kl_style *s, const char *l) { (void)s; if (l == NULL) return 40; return (int)strlen(l) * 8 + 16; }
void kl_icon_draw(struct kl_canvas *c, enum kl_icon i, float x, float y, float sz, kl_color col) { (void)c; (void)i; (void)x; (void)y; (void)sz; (void)col; }
void kl_icon_folder(struct kl_canvas *c, float x, float y, float sz, kl_color t) { (void)c; (void)x; (void)y; (void)sz; (void)t; }
void kl_icon_file(struct kl_canvas *c, struct kl_text *t, float x, float y, float sz, kl_color b, const char *l) { (void)c; (void)t; (void)x; (void)y; (void)sz; (void)b; (void)l; }
void kl_progress(const struct kl_style *s, const struct kl_rect *r, double f, uint64_t n) { (void)s; (void)r; (void)f; (void)n; }
int kl_button(struct kl_ui *u, const struct kl_style *s, uint32_t id, const struct kl_rect *r, const char *l, unsigned f) { (void)u; (void)s; (void)r; (void)l; if (id == SFTP_UI_DOWNLOAD) g_download_btn_flags = f; return 0; }
unsigned kl_field(struct kl_ui *u, const struct kl_style *s, uint32_t id, const struct kl_rect *r, struct kl_field *f, const char *p) { (void)u; (void)s; (void)id; (void)r; (void)f; (void)p; return 0U; }
void kl_field_set(struct kl_field *f, const char *t) { (void)f; (void)t; }
void kl_breadcrumb(struct kl_ui *u, const struct kl_style *s, uint32_t id, const struct kl_rect *r, const char *p, void *cb, void *d) { (void)u; (void)s; (void)id; (void)r; (void)p; (void)cb; (void)d; }

int sftp_view_platform_confirm_close(size_t active_transfers) { (void)active_transfers; return 1; }
int sftp_link_is_ended(struct sftp_link *l) { (void)l; return 0; }
void sftp_link_retain(struct sftp_link *l) { (void)l; }
void sftp_link_release(struct sftp_link *l) { (void)l; }
void sftp_dir_listing_free(struct sftp_dir_listing *l) { (void)l; }
int sftp_local_downloads_dir(char *p, size_t s) { (void)snprintf(p, s, "/tmp/downloads"); return 0; }
int sftp_local_stat(const char *p, struct sftp_local_entry *e) { (void)p; (void)e; return -1; }
int mikuterm_path(char *p, size_t s, const char *r) { (void)p; (void)s; (void)r; return -1; }
int mikuterm_file_read(const char *p, size_t m, char **o, size_t *s) { (void)p; (void)m; (void)o; (void)s; return -1; }
int mikuterm_file_replace(const char *p, const void *d, size_t s) { (void)p; (void)d; (void)s; return -1; }
int mikuterm_conf_set(const char *t, size_t ts, const char *k, const char *v, char **o, size_t *os) { (void)t; (void)ts; (void)k; (void)v; (void)o; (void)os; return -1; }
void mikuterm_session_detach_sftp(struct mikuterm_session *s, const struct terminal_connection *vc) { (void)s; (void)vc; }
void terminal_clipboard_set(struct terminal_window *w, const char *s, size_t l) { (void)w; (void)s; (void)l; }
int sftp_worker_start(struct sftp_link *l, int p, void (*cb)(void *), void *d, struct sftp_worker **out) { (void)l; (void)p; (void)cb; (void)d; *out = NULL; return -1; }
void sftp_worker_stop(struct sftp_worker *w) { (void)w; }
void sftp_worker_status(struct sftp_worker *w, enum sftp_worker_state *st, char *h, size_t hc, char *e, size_t ec) { (void)w; if (st) *st = SFTP_WORKER_READY; (void)h; (void)hc; (void)e; (void)ec; }
int sftp_worker_list_dir(struct sftp_worker *w, const char *p, int f, struct sftp_dir_listing **o) { (void)w; (void)f; (void)o; if (p != NULL) (void)snprintf(g_navigated_path, sizeof(g_navigated_path), "%s", p); return -1; }
int sftp_worker_rename(struct sftp_worker *w, const char *o, const char *n, char *e, size_t c) { (void)w; (void)o; (void)n; (void)e; (void)c; return 0; }
int sftp_worker_delete_count(struct sftp_worker *w, const char *p, size_t *items, size_t *folders, int *is_sym) { (void)w; (void)p; (void)items; (void)folders; (void)is_sym; return 0; }
int sftp_worker_enqueue_download(struct sftp_worker *w, const char *r, const char *l, int d, uint64_t *id) {
	(void)w;
	(void)d;
	(void)id;
	g_enqueue_download_calls++;
	if (r != NULL)
		(void)snprintf(g_last_download_remote, sizeof(g_last_download_remote), "%s", r);
	if (l != NULL)
		(void)snprintf(g_last_download_local, sizeof(g_last_download_local), "%s", l);
	return 0;
}
int sftp_worker_transfers(struct sftp_worker *w, struct sftp_transfer_snapshot **o, size_t *c, size_t *a) { (void)w; *o = NULL; *c = 0U; *a = 0U; return 0; }
void sftp_worker_transfers_free(struct sftp_transfer_snapshot *t) { (void)t; }
void sftp_worker_retry(struct sftp_worker *w) { (void)w; }
void sftp_worker_resolve_conflict(struct sftp_worker *w, uint64_t id, enum sftp_conflict_action a) { (void)w; (void)id; (void)a; }
int sftp_worker_delete(struct sftp_worker *w, const char *p, char *e, size_t c) { (void)w; (void)p; (void)e; (void)c; return 0; }
int sftp_worker_mkdir(struct sftp_worker *w, const char *p, char *e, size_t c) { (void)w; (void)p; (void)e; (void)c; return 0; }
int sftp_worker_enqueue_upload(struct sftp_worker *w, const char *l, const char *r, int d, uint64_t *id) { (void)w; (void)l; (void)r; (void)d; (void)id; return 0; }
void sftp_worker_cancel_transfer(struct sftp_worker *w, uint64_t id) { (void)w; (void)id; }
void sftp_worker_cancel_all(struct sftp_worker *w) { (void)w; }

static void
mock_on_change_download_dir(void *data, struct sftp_view *view)
{
	(void)data;
	(void)view;
	g_change_download_dir_calls++;
}

int
sftp_transfer_join_remote(char *out, size_t cap, const char *dir, const char *name)
{
	int w;

	/* Formats dir/name into out. */
	w = snprintf(out, cap, "%s/%s", dir, name);
	if (w < 0 || (size_t)w >= cap)
		return -1;
	return 0;
}

static int
has_captured_substring(const char *needle)
{
	const char *hit;
	size_t i;

	/* Scans captured text strings for needle. */
	for (i = 0U; i < g_captured_count; i++) {
		hit = strstr(g_captured_texts[i], needle);
		if (hit != NULL)
			return 1;
	}

	/* Not found. */
	return 0;
}

int
main(void)
{
	struct sftp_view view;
	struct terminal_view_state state;
	struct terminal_picture_event pev;
	struct sftp_list_row *rows;
	struct sftp_transfer_snapshot xfer;
	uint32_t *pixels;
	uint32_t w = 800U;
	uint32_t h = 600U;
	uint32_t px;
	uint32_t bg;
	size_t i;
	int mx;
	int my;
	int r_px;
	int r_bg;
	int found_menu_pixel;

	/* Allocates framebuffer and mock file rows. */
	pixels = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
	rows = calloc(60U, sizeof(*rows));
	CHECK(pixels != NULL && rows != NULL);
	if (pixels == NULL || rows == NULL)
		return 1;

	/* Initializes the SFTP view state. */
	memset(&view, 0, sizeof(view));
	view.ui = kl_ui_create();
	CHECK(view.ui != NULL);
	view.scale = KL_SCALE_BASE;
	view.split_width = 200;
	view.worker_state = SFTP_WORKER_READY;
	(void)snprintf(view.current_path, sizeof(view.current_path), "/home/user");
	view.rows = rows;
	view.row_count = 60U;
	for (i = 0U; i < 60U; i++) {
		(void)snprintf(rows[i].entry.name, sizeof(rows[i].entry.name), "file_%02zu.txt", i);
		(void)snprintf(rows[i].display_name, sizeof(rows[i].display_name), "file_%02zu.txt", i);
		rows[i].entry.size = 1024ULL;
	}

	/* Initializes list widgets. */
	(void)kl_list_init(&view.tree_list);
	(void)kl_list_init(&view.file_list);
	(void)kl_list_init(&view.transfer_list);

	/* Draws initial frame so UI scroll regions and rect_list are populated. */
	memset(&state, 0, sizeof(state));
	state.scale = KL_SCALE_BASE;
	CHECK(sftp_view_draw_frame(&view, &state, pixels, w, h) == 0);
	CHECK(view.dirty == 0);

	/* 1. Single wheel event over file list keeps view->dirty=1 while scroll glides. */
	/* Sends a single wheel event over the file list. */
	memset(&pev, 0, sizeof(pev));
	pev.kind = TERMINAL_PICTURE_MOTION;
	pev.x = view.rect_list.x + 20;
	pev.y = view.rect_list.y + 20;
	sftp_view_handle_input(&view, &pev);
	CHECK(sftp_view_draw_frame(&view, &state, pixels, w, h) == 0);

	/* Sends a single wheel event over the file list. */
	memset(&pev, 0, sizeof(pev));
	pev.kind = TERMINAL_PICTURE_AXIS;
	pev.x = view.rect_list.x + 20;
	pev.y = view.rect_list.y + 20;
	pev.dy = 60.0;
	sftp_view_handle_input(&view, &pev);
	CHECK(view.dirty == 1);
	CHECK(sftp_view_draw_frame(&view, &state, pixels, w, h) == 0);
	CHECK(view.dirty == 1);

	/* Advances clock past scroll glide and scrollbar fade: dirty clears to 0. */
	g_now_us += 2000000ULL;
	CHECK(sftp_view_draw_frame(&view, &state, pixels, w, h) == 0);
	g_now_us += 2000000ULL;
	CHECK(sftp_view_draw_frame(&view, &state, pixels, w, h) == 0);
	CHECK(view.dirty == 0);

	/* 2. Context menu opacity (~95% / 0xf2): verifies menu card pixel is near-opaque. */
	view.ctx_open = 1;
	view.ctx_x = 300;
	view.ctx_y = 200;
	view.ctx_items[0].id = SFTP_CTX_DOWNLOAD;
	view.ctx_items[0].label = "Download";
	view.ctx_items[0].enabled = 1;
	view.ctx_count = 1U;
	view.ctx_hover = -1;
	CHECK(sftp_view_draw_frame(&view, &state, pixels, w, h) == 0);
	found_menu_pixel = 0;
	mx = view.rect_ctx.x + view.rect_ctx.width - 10;
	my = view.rect_ctx.y + view.rect_ctx.height - 6;
	px = pixels[(size_t)my * w + (size_t)mx];
	bg = pixels[(size_t)my * w + (size_t)(view.rect_ctx.x - 10)];
	r_px = (int)((px >> 16) & 0xffU);
	r_bg = (int)((bg >> 16) & 0xffU);
	if (r_px >= 240 && r_px > r_bg)
		found_menu_pixel = 1;
	CHECK(found_menu_pixel);
	view.ctx_open = 0;

	/* 3. Drag-and-drop trigger on motion outside view bounds. */
	/* Sends a single wheel event over the file list. */
	memset(&pev, 0, sizeof(pev));
	pev.kind = TERMINAL_PICTURE_BUTTON;
	pev.code = 0x110U;
	pev.pressed = 1;
	pev.x = view.rect_list.x + 20;
	pev.y = view.rect_list.y + 10;
	sftp_view_handle_input(&view, &pev);
	CHECK(view.drag_pending == 1);

	/* Moves past drag threshold to activate drag. */
	pev.kind = TERMINAL_PICTURE_MOTION;
	pev.x = view.rect_list.x + 40;
	pev.y = view.rect_list.y + 10;
	sftp_view_handle_input(&view, &pev);
	CHECK(view.drag_active == 1);

	/* Moves outside view bounds to trigger external OLE drag. */
	pev.kind = TERMINAL_PICTURE_MOTION;
	pev.x = -10;
	pev.y = view.rect_list.y + 10;
	sftp_view_handle_input(&view, &pev);
	CHECK(view.drag_active == 0);
	CHECK(view.drag_pending == 0);

	/* 4. Download destination path shown in transfer bar (active and done). */
	view.text_ready = 1;
	memset(&xfer, 0, sizeof(xfer));
	xfer.id = 1U;
	xfer.direction = SFTP_TRANSFER_DOWNLOAD;
	xfer.state = SFTP_TRANSFER_ACTIVE;
	(void)snprintf(xfer.remote_path, sizeof(xfer.remote_path), "/home/user/alpha.txt");
	(void)snprintf(xfer.local_path, sizeof(xfer.local_path), "C:\\Users\\test\\Downloads\\alpha.txt");
	xfer.bytes_done = 512ULL;
	xfer.bytes_total = 1024ULL;
	view.transfers = &xfer;
	view.transfer_count = 1U;
	view.active_transfers = 1U;
	view.transfer_speed = 100.0;
	g_captured_count = 0U;
	CHECK(sftp_view_draw_frame(&view, &state, pixels, w, h) == 0);
	CHECK(has_captured_substring("C:\\Users\\test\\Downloads\\alpha.txt"));

	/* Verifies completed download shows Saved to <path>. */
	xfer.state = SFTP_TRANSFER_DONE;
	view.active_transfers = 0U;
	g_captured_count = 0U;
	CHECK(sftp_view_draw_frame(&view, &state, pixels, w, h) == 0);
	CHECK(has_captured_substring("Saved to C:\\Users\\test\\Downloads\\alpha.txt"));

	/* 5. Touch UX: swipe scroll direction, exclusive tap selection, folder nav, long-press. */
	view.worker = (struct sftp_worker *)&g_dummy_worker;
	view.worker_state = SFTP_WORKER_READY;
	view.file_list.scroll.y = 0.0;
	CHECK(sftp_view_draw_frame(&view, &state, pixels, w, h) == 0);
	{
		double scroll_before = view.file_list.scroll.y;

		/* Touch swipe up (uy < start_y) scrolls down (scroll.y increases). */
		memset(&pev, 0, sizeof(pev));
		pev.kind = TERMINAL_PICTURE_TOUCH_DOWN;
		pev.code = 1U;
		pev.x = view.rect_list.x + 20;
		pev.y = view.rect_list.y + 120;
		sftp_view_handle_input(&view, &pev);
		pev.kind = TERMINAL_PICTURE_TOUCH_MOVE;
		pev.y = view.rect_list.y + 120 - SFTP_TOUCH_SLOP - 40;
		sftp_view_handle_input(&view, &pev);
		pev.kind = TERMINAL_PICTURE_TOUCH_UP;
		sftp_view_handle_input(&view, &pev);
		g_now_us += 100000ULL;
		CHECK(sftp_view_draw_frame(&view, &state, pixels, w, h) == 0);
		CHECK(view.file_list.scroll.y > scroll_before);
	}

	view.row_count = 3U;
	rows[0].entry.is_dir = 0;
	(void)snprintf(rows[0].entry.name, sizeof(rows[0].entry.name), "file.txt");
	rows[0].selected = 0;
	rows[1].entry.is_dir = 0;
	(void)snprintf(rows[1].entry.name, sizeof(rows[1].entry.name), "other.txt");
	rows[1].selected = 0;
	rows[2].entry.is_dir = 1;
	(void)snprintf(rows[2].entry.name, sizeof(rows[2].entry.name), "docs");
	rows[2].selected = 0;
	view.file_list.scroll.y = 0.0;
	view.transfers = NULL;
	view.transfer_count = 0U;
	CHECK(sftp_view_draw_frame(&view, &state, pixels, w, h) == 0);
	CHECK((g_download_btn_flags & KL_BUTTON_DISABLED) != 0U);

	/* Single-tap file row 0: selects row 0 and enables Download button. */
	memset(&pev, 0, sizeof(pev));
	pev.kind = TERMINAL_PICTURE_TOUCH_DOWN;
	pev.code = 1U;
	pev.x = view.rect_list.x + 20;
	pev.y = view.rect_list.y + 8;
	sftp_view_handle_input(&view, &pev);
	pev.kind = TERMINAL_PICTURE_TOUCH_UP;
	sftp_view_handle_input(&view, &pev);
	CHECK(rows[0].selected == 1);
	CHECK(rows[1].selected == 0);
	CHECK(sftp_view_draw_frame(&view, &state, pixels, w, h) == 0);
	CHECK((g_download_btn_flags & KL_BUTTON_DISABLED) == 0U);

	/* Single-tap file row 1: exclusive selection moves to row 1 (deselects row 0). */
	pev.kind = TERMINAL_PICTURE_TOUCH_DOWN;
	pev.y = view.rect_list.y + SFTP_ROW_HEIGHT + 8;
	sftp_view_handle_input(&view, &pev);
	pev.kind = TERMINAL_PICTURE_TOUCH_UP;
	sftp_view_handle_input(&view, &pev);
	CHECK(rows[0].selected == 0);
	CHECK(rows[1].selected == 1);

	/* 6. First download with mediastore://downloads prompts SAF picker and completes after set_download_dir. */
	(void)snprintf(view.download_dir, sizeof(view.download_dir), "mediastore://downloads");
	view.on_change_download_dir = mock_on_change_download_dir;
	g_change_download_dir_calls = 0;
	g_enqueue_download_calls = 0;
	sftp_view_download_selected(&view);
	CHECK(g_change_download_dir_calls == 1);
	CHECK(view.pending_download == 1);
	CHECK(g_enqueue_download_calls == 0);

	/* Completing the SAF picker sets download_dir and enqueues the pending download. */
	sftp_view_set_download_dir(&view, "saf-tree://1");
	CHECK(view.pending_download == 0);
	CHECK(strcmp(view.download_dir, "saf-tree://1") == 0);
	CHECK(g_enqueue_download_calls == 1);
	CHECK(strcmp(g_last_download_remote, "/home/user/other.txt") == 0);
	CHECK(strcmp(g_last_download_local, "saf-tree://1") == 0);

	/* Cancelling the SAF picker clears pending_download without enqueuing. */
	(void)snprintf(view.download_dir, sizeof(view.download_dir), "mediastore://downloads");
	sftp_view_download_selected(&view);
	CHECK(view.pending_download == 1);
	sftp_view_set_download_dir(&view, NULL);
	CHECK(view.pending_download == 0);
	CHECK(g_enqueue_download_calls == 1);

	/* Single-tap on folder row 2: navigates into folder. */
	g_navigated_path[0] = '\0';
	pev.kind = TERMINAL_PICTURE_TOUCH_DOWN;
	pev.x = view.rect_list.x + 20;
	pev.y = view.rect_list.y + 2 * SFTP_ROW_HEIGHT + 8;
	sftp_view_handle_input(&view, &pev);
	pev.kind = TERMINAL_PICTURE_TOUCH_UP;
	sftp_view_handle_input(&view, &pev);
	CHECK(strcmp(g_navigated_path, "/home/user/docs") == 0);

	/* Touch move past slop cancels long-press. */
	view.ctx_open = 0;
	pev.kind = TERMINAL_PICTURE_TOUCH_DOWN;
	pev.x = view.rect_list.x + 20;
	pev.y = view.rect_list.y + 8;
	sftp_view_handle_input(&view, &pev);
	pev.kind = TERMINAL_PICTURE_TOUCH_MOVE;
	pev.y = view.rect_list.y + 8 + SFTP_TOUCH_SLOP + 4;
	sftp_view_handle_input(&view, &pev);
	g_now_us += SFTP_TOUCH_LONG_PRESS_US + 10000ULL;
	sftp_view_poll_touch(&view, g_now_us);
	CHECK(view.ctx_open == 0);
	pev.kind = TERMINAL_PICTURE_TOUCH_UP;
	sftp_view_handle_input(&view, &pev);

	/* Long-press within slop opens 7-item context menu. */
	pev.kind = TERMINAL_PICTURE_TOUCH_DOWN;
	pev.x = view.rect_list.x + 20;
	pev.y = view.rect_list.y + 8;
	sftp_view_handle_input(&view, &pev);
	g_now_us += SFTP_TOUCH_LONG_PRESS_US + 10000ULL;
	sftp_view_poll_touch(&view, g_now_us);
	CHECK(view.ctx_open == 1);
	CHECK(view.ctx_count == 7U);
	pev.kind = TERMINAL_PICTURE_TOUCH_UP;
	sftp_view_handle_input(&view, &pev);
	view.ctx_open = 0;
	(void)g_upload_clicked;
	(void)g_more_opened;

	/* Releases resources. */
	if (view.canvas_ready)
		kl_canvas_release(&view.canvas);
	kl_list_release(&view.tree_list);
	kl_list_release(&view.file_list);
	kl_list_release(&view.transfer_list);
	kl_ui_destroy(view.ui);
	free(rows);
	free(pixels);

	/* Reports test summary. */
	printf("checks %d, failures %d\n", checks, failures);
	if (failures == 0) {
		printf("sftp-view-uat-host-test: PASS\n");
		return 0;
	}

	/* Reports failure. */
	printf("sftp-view-uat-host-test: FAIL\n");
	return 1;
}
