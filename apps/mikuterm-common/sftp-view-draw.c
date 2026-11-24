/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * CPU rendering for the SFTP browser view tab: breadcrumb bar, folder
 * tree, splitter, detailed file list, inline rename field, rubber-band
 * selection, transfer bar and queue, modal dialogs, and context menu.
 */

#include "sftp-view.h"
#include "sftp-transfer.h"

#include "userland/desktop/terminal/terminal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void draw_top_bar(struct sftp_view *view, const struct kl_style *style, int width);
static void draw_status_banner(struct sftp_view *view, const struct kl_style *style, int width, int y, int h);
static void draw_tree_pane(struct sftp_view *view, const struct kl_style *style, const struct kl_rect *rect, uint64_t now_us);
static void draw_file_pane(struct sftp_view *view, const struct kl_style *style, const struct kl_rect *rect, uint64_t now_us);
static void draw_transfer_area(struct sftp_view *view, const struct kl_style *style, int width, int bottom_y, uint64_t now_us);
static void draw_modal_dialog(struct sftp_view *view, const struct kl_style *style, int width, int height);
static void draw_context_menu(struct sftp_view *view, const struct kl_style *style);
static const char *draw_transfer_state_text(enum sftp_transfer_state st);

/*
 * Draws one frame of the SFTP browser view into pixels.
 */
int
sftp_view_draw_frame(
	struct sftp_view *view,
	const struct terminal_view_state *state,
	uint32_t *pixels,
	uint32_t width,
	uint32_t height)
{
	const struct kl_theme *theme;
	struct kl_style style;
	struct kl_rect frame;
	struct kl_rect units;
	struct kl_rect split_rect;
	struct kl_rect band;
	uint64_t now_us;
	int w;
	int h;
	int transfer_h;
	int banner_h;
	int body_y;
	int body_h;
	int split_x;
	int moving;
	int error;

	/* Validates frame dimensions and output pixel buffer. */
	if (view == NULL || pixels == NULL || width == 0U || height == 0U)
		return EINVAL;

	/* Reinitializes the software canvas for the current pixel buffer. */
	if (view->canvas_ready) {
		kl_canvas_release(&view->canvas);
		view->canvas_ready = 0;
	}

	/* Initializes the canvas wrapper over pixels. */
	error = kl_canvas_init(&view->canvas, pixels, (size_t)width, (int)width, (int)height);
	if (error != 0)
		return error;
	view->canvas_ready = 1;
	if (state != NULL && state->scale != 0U)
		view->scale = state->scale;
	else
		view->scale = KL_SCALE_BASE;
	kl_canvas_set_scale(&view->canvas, view->scale);
	view->width = (int)width;
	view->height = (int)height;

	now_us = kl_clock_us();
	sftp_view_poll_touch(view, now_us);

	/* Computes logical view dimensions from the pixel frame. */
	frame.x = 0;
	frame.y = 0;
	frame.width = (int)width;
	frame.height = (int)height;
	kl_canvas_logical(&view->canvas, &frame, &units);
	w = units.width;
	h = units.height;

	/* Prepares the drawing style for this frame. */
	theme = kl_theme_default();
	style.canvas = &view->canvas;
	if (view->text_ready)
		style.text = &view->text;
	else
		style.text = NULL;
	style.theme = theme;
	style.glass = 0;

	/* Begins the immediate-mode UI frame. */
	now_us = kl_clock_us();
	if (view->ui != NULL)
		kl_ui_begin(view->ui, now_us);

	/* Background gradient. */
	kl_canvas_gradient(&view->canvas, &units, theme->ground_top, theme->ground_bottom);

	/* Top breadcrumb and action bar. */
	draw_top_bar(view, &style, w);

	/* Optional status/error banner when worker is not ready or listing failed. */
	banner_h = 0;
	if (view->worker_state != SFTP_WORKER_READY || view->list_error[0] != '\0' || view->list_truncated) {
		banner_h = SFTP_STATUS_BAR_H;
		draw_status_banner(view, &style, w, SFTP_BAR_HEIGHT, banner_h);
	}

	/* Bottom transfer bar (+ expanded list if open). */
	transfer_h = 0;
	if (view->transfer_count > 0U || view->transfers_expanded) {
		if (view->transfers_expanded)
			transfer_h = SFTP_TRANSFER_BAR_H + SFTP_TRANSFER_PANEL_H;
		else
			transfer_h = SFTP_TRANSFER_BAR_H;
		if (transfer_h > h - SFTP_BAR_HEIGHT - banner_h - 80)
			transfer_h = SFTP_TRANSFER_BAR_H;
	}

	/* Computes layout rectangles for the tree, splitter, header, and file list. */
	body_y = SFTP_BAR_HEIGHT + banner_h;
	body_h = h - body_y - transfer_h;
	if (body_h < 40)
		body_h = 40;

	/* Clamps the splitter position within the available width. */
	split_x = view->split_width;
	if (split_x < SFTP_SPLIT_MIN)
		split_x = SFTP_SPLIT_MIN;
	if (split_x > w - 180) {
		if (w > 300)
			split_x = w - 180;
		else
			split_x = w / 3;
	}

	/* Stores hit-test rectangles for the tree, splitter, header, and list. */
	view->rect_tree.x = 0;
	view->rect_tree.y = body_y;
	view->rect_tree.width = split_x;
	view->rect_tree.height = body_h;
	view->rect_splitter.x = split_x;
	view->rect_splitter.y = body_y;
	view->rect_splitter.width = SFTP_SPLITTER_WIDTH;
	view->rect_splitter.height = body_h;
	split_rect.x = split_x + SFTP_SPLITTER_WIDTH / 2;
	split_rect.y = body_y;
	split_rect.width = 1;
	split_rect.height = body_h;
	view->rect_header.x = split_x + SFTP_SPLITTER_WIDTH;
	view->rect_header.y = body_y;
	view->rect_header.width = w - view->rect_header.x;
	view->rect_header.height = SFTP_HEADER_HEIGHT;
	view->rect_list.x = view->rect_header.x;
	view->rect_list.y = body_y + SFTP_HEADER_HEIGHT;
	view->rect_list.width = view->rect_header.width;
	view->rect_list.height = body_h - SFTP_HEADER_HEIGHT;
	if (view->rect_list.height < 0)
		view->rect_list.height = 0;

	/* Draws the tree pane, splitter line, and detailed file list. */
	draw_tree_pane(view, &style, &view->rect_tree, now_us);
	kl_canvas_fill(&view->canvas, &split_rect, theme->separator);
	draw_file_pane(view, &style, &view->rect_list, now_us);

	/* Draws the transfer area or clears its hit rectangles when hidden. */
	if (transfer_h > 0) {
		draw_transfer_area(view, &style, w, h - transfer_h, now_us);
	} else {
		memset(&view->rect_transfer_bar, 0, sizeof(view->rect_transfer_bar));
		memset(&view->rect_transfer_panel, 0, sizeof(view->rect_transfer_panel));
	}

	/* Rubber-band selection rectangle. */
	if (view->band_active) {
		if (view->band_x0 < view->band_x1)
			band.x = view->band_x0;
		else
			band.x = view->band_x1;
		if (view->band_y0 < view->band_y1)
			band.y = view->band_y0;
		else
			band.y = view->band_y1;
		band.width = abs(view->band_x1 - view->band_x0);
		band.height = abs(view->band_y1 - view->band_y0);
		kl_band(&style, &band);
	}

	/* Context menu and modal dialog on top. */
	if (view->ctx_open)
		draw_context_menu(view, &style);
	if (view->dialog != SFTP_DIALOG_NONE)
		draw_modal_dialog(view, &style, w, h);

	/* Ends the immediate-mode UI frame and keeps dirty while animations run. */
	moving = 0;
	if (view->ui != NULL)
		moving = kl_ui_end(view->ui, now_us);

	/* Updates the dirty flag under the view lock. */
	kl_mutex_lock(&view->lock);
	if (moving)
		view->dirty = 1;
	else
		view->dirty = 0;
	kl_mutex_unlock(&view->lock);

	/* Succeeded: frame rendered into pixels. */
	return 0;
}

/* Draws the top navigation buttons, breadcrumb / editable path, Refresh, and New folder. */
static void
draw_top_bar(
	struct sftp_view *view,
	const struct kl_style *style,
	int width)
{
	struct kl_rect bar;
	struct kl_rect sep;
	struct kl_rect btn;
	struct kl_rect crumb_area;
	char clean[SSH_SFTP_PATH_MAX * 2U];
	char seg_raw[SSH_SFTP_PATH_MAX];
	const char *p;
	const char *next;
	size_t seg_len;
	int cx;
	int baseline;
	int x;
	int right_x;
	int btn_w_refresh;
	int btn_w_new;
	int btn_w_upload;
	int btn_w_download;
	int btn_w_more;
	size_t i;
	size_t sel_count;
	int cy;
	int clicked;
	int at_root;
	int show_refresh;
	unsigned f_res;
	unsigned flags;

	/* Draws the top bar background and bottom separator. */
	bar.x = 0;
	bar.y = 0;
	bar.width = width;
	bar.height = SFTP_BAR_HEIGHT;
	kl_canvas_fill(style->canvas, &bar, style->theme->panel);
	sep.x = 0;
	sep.y = SFTP_BAR_HEIGHT - 1;
	sep.width = width;
	sep.height = 1;
	kl_canvas_fill(style->canvas, &sep, style->theme->separator);
	cy = (SFTP_BAR_HEIGHT - 28) / 2;
	x = 8;

	/* Back, Forward, Up buttons. */
	btn.x = x;
	btn.y = cy;
	btn.width = 28;
	btn.height = 28;
	if (view->history.index == 0U || view->history.count == 0U)
		flags = KL_BUTTON_DISABLED;
	else
		flags = 0U;
	clicked = kl_icon_button(view->ui, style, SFTP_UI_BACK, 0U, &btn, KL_ICON_BACK, 16, flags);
	if (clicked)
		sftp_view_navigate_back(view);
	x += 32;

	/* Draws the Forward button. */
	btn.x = x;
	if (view->history.index + 1U >= view->history.count)
		flags = KL_BUTTON_DISABLED;
	else
		flags = 0U;
	clicked = kl_icon_button(view->ui, style, SFTP_UI_FWD, 0U, &btn, KL_ICON_FORWARD, 16, flags);
	if (clicked)
		sftp_view_navigate_forward(view);
	x += 32;

	/* Draws the Up button. */
	btn.x = x;
	at_root = strcmp(view->current_path, "/");
	if (view->current_path[0] == '\0' || at_root == 0)
		flags = KL_BUTTON_DISABLED;
	else
		flags = 0U;
	clicked = kl_icon_button(view->ui, style, SFTP_UI_UP, 0U, &btn, KL_ICON_UP, 16, flags);
	if (clicked)
		sftp_view_navigate_up(view);
	x += 36;

	/* Counts selected rows for Download button enablement. */
	sel_count = 0U;
	for (i = 0U; i < view->row_count; i++) {
		if (view->rows[i].selected)
			sel_count++;
	}

	/* Right-aligned action buttons: Upload, Download, New folder, Refresh (wide), and overflow menu. */
	show_refresh = (width >= 760);
	if (style->text != NULL) {
		btn_w_upload = kl_button_width(style, "Upload");
		btn_w_download = kl_button_width(style, "Download");
		btn_w_new = kl_button_width(style, "New folder");
		btn_w_refresh = show_refresh ? kl_button_width(style, "Refresh") : 0;
	} else {
		btn_w_upload = 68;
		btn_w_download = 82;
		btn_w_new = 96;
		btn_w_refresh = show_refresh ? 74 : 0;
	}
	btn_w_more = 28;

	/* Overflow menu button (always includes Change Download Folder). */
	right_x = width - 8 - btn_w_more;
	btn.x = right_x;
	btn.y = cy;
	btn.width = btn_w_more;
	btn.height = 28;
	clicked = kl_button(view->ui, style, SFTP_UI_MORE, &btn, "...", 0U);
	if (clicked)
		sftp_view_open_more_menu(view, btn.x - 160, btn.y + btn.height + 2);

	if (view->worker_state == SFTP_WORKER_READY)
		flags = 0U;
	else
		flags = KL_BUTTON_DISABLED;

	/* Draws the New folder button. */
	right_x -= 6 + btn_w_new;
	btn.x = right_x;
	btn.width = btn_w_new;
	clicked = kl_button(view->ui, style, SFTP_UI_NEW_FOLDER, &btn, "New folder", flags);
	if (clicked)
		sftp_view_begin_new_folder(view);

	/* Draws the Refresh button when the bar is wide enough. */
	if (show_refresh) {
		right_x -= 6 + btn_w_refresh;
		btn.x = right_x;
		btn.width = btn_w_refresh;
		clicked = kl_button(view->ui, style, SFTP_UI_REFRESH, &btn, "Refresh", flags);
		if (clicked)
			sftp_view_refresh_all(view);
	}

	/* Draws the Download button (enabled when rows are selected). */
	right_x -= 6 + btn_w_download;
	btn.x = right_x;
	btn.width = btn_w_download;
	clicked = kl_button(
		view->ui,
		style,
		SFTP_UI_DOWNLOAD,
		&btn,
		"Download",
		(view->worker_state == SFTP_WORKER_READY && sel_count > 0U) ? 0U : KL_BUTTON_DISABLED);
	if (clicked)
		sftp_view_download_selected(view);

	/* Draws the Upload button. */
	right_x -= 6 + btn_w_upload;
	btn.x = right_x;
	btn.width = btn_w_upload;
	clicked = kl_button(view->ui, style, SFTP_UI_UPLOAD, &btn, "Upload", flags);
	if (clicked && view->on_upload != NULL)
		view->on_upload(view->platform_data, view);

	/* Computes the breadcrumb / path-edit area between the button groups. */
	crumb_area.x = x;
	crumb_area.y = cy;
	crumb_area.width = right_x - 8 - x;
	crumb_area.height = 28;
	if (crumb_area.width < 40)
		crumb_area.width = 40;
	view->rect_breadcrumb = crumb_area;

	/* Draws either the editable path field or clickable breadcrumb segments. */
	if (view->editing_path) {
		f_res = kl_field(view->ui, style, SFTP_UI_PATH_FIELD, &crumb_area, &view->path_field, "/path/to/folder");
		if ((f_res & KL_FIELD_SUBMITTED) != 0U) {
			view->editing_path = 0;
			if (view->path_field.text[0] != '\0')
				sftp_view_navigate(view, view->path_field.text, 1, 0);
		} else if ((f_res & KL_FIELD_CANCELLED) != 0U) {
			view->editing_path = 0;
			view->dirty = 1;
		}
	} else {
		/* Draw clickable breadcrumb segments. */
		kl_canvas_round(
			style->canvas,
			(float)crumb_area.x,
			(float)crumb_area.y,
			(float)crumb_area.width,
			(float)crumb_area.height,
			style->theme->control_radius,
			style->theme->control);
		kl_canvas_round_border(
			style->canvas,
			(float)crumb_area.x,
			(float)crumb_area.y,
			(float)crumb_area.width,
			(float)crumb_area.height,
			style->theme->control_radius,
			1.0f,
			style->theme->control_edge);

		/* Draws the root slash and each path segment inside the clipped box. */
		if (style->text != NULL && view->current_path[0] != '\0') {
			kl_canvas_clip_push(style->canvas, &crumb_area);
			cx = crumb_area.x + 10;
			baseline = kl_text_center(style->theme->text_body, crumb_area.y, crumb_area.height);
			sftp_view_sanitize_text("/", clean, sizeof(clean));
			(void)kl_text_draw(style->text, style->canvas, cx, baseline, "/", 1U, style->theme->text_body, 0, style->theme->text);
			cx += kl_text_width(style->text, "/", 1U, style->theme->text_body, 0) + 4;
			p = view->current_path;
			if (*p == '/')
				p++;
			while (*p != '\0') {
				next = strchr(p, '/');
				if (next != NULL)
					seg_len = (size_t)(next - p);
				else
					seg_len = strlen(p);
				if (seg_len > 0U && seg_len < sizeof(seg_raw)) {
					memcpy(seg_raw, p, seg_len);
					seg_raw[seg_len] = '\0';
					sftp_view_sanitize_text(seg_raw, clean, sizeof(clean));
					(void)kl_text_draw(style->text, style->canvas, cx, baseline, ">", 1U, style->theme->text_small, 0, style->theme->text_faint);
					cx += kl_text_width(style->text, ">", 1U, style->theme->text_small, 0) + 6;
					(void)kl_text_draw(style->text, style->canvas, cx, baseline, clean, strlen(clean), style->theme->text_body, 0, style->theme->text);
					cx += kl_text_width(style->text, clean, strlen(clean), style->theme->text_body, 0) + 6;
				}

				/* Advances to the next path segment. */
				p += seg_len;
				if (*p == '/')
					p++;
			}

			/* Restores clipping. */
			kl_canvas_clip_pop(style->canvas);
		}
	}
}

/* Draws status/error banner below the breadcrumb bar. */
static void
draw_status_banner(
	struct sftp_view *view,
	const struct kl_style *style,
	int width,
	int y,
	int h)
{
	struct kl_rect bar;
	struct kl_rect btn;
	const char *msg;
	kl_color ink;
	int baseline;
	int ended;
	int clicked;

	/* Fills the status banner background. */
	bar.x = 0;
	bar.y = y;
	bar.width = width;
	bar.height = h;
	kl_canvas_fill(style->canvas, &bar, style->theme->card);

	/* Selects the status message and ink color for the current worker state. */
	ink = style->theme->text;
	if (view->worker_state == SFTP_WORKER_OPENING || view->worker_state == SFTP_WORKER_IDLE) {
		msg = "Opening SFTP session...";
	} else if (view->worker_state == SFTP_WORKER_FAILED) {
		if (view->worker_error[0] != '\0')
			msg = view->worker_error;
		else
			msg = "SFTP session failed.";
		ink = style->theme->bad;
	} else if (view->worker_state == SFTP_WORKER_ENDED) {
		msg = "The connection ended.";
		ink = style->theme->bad;
	} else if (view->list_error[0] != '\0') {
		msg = view->list_error;
		ink = style->theme->bad;
	} else {
		msg = "Directory listing truncated (limit reached).";
	}

	/* Draws the banner message text. */
	if (style->text != NULL) {
		baseline = kl_text_center(style->theme->text_small, y, h);
		(void)kl_text_draw_fit(style->text, style->canvas, 12, baseline, msg, style->theme->text_small, 0, width - 100, ink);
	}

	/* Draws a Retry button when the SFTP channel failed on an open SSH connection. */
	if (view->worker_state == SFTP_WORKER_FAILED) {
		ended = sftp_link_is_ended(view->link);
		if (!ended) {
			btn.x = width - 80;
			btn.y = y + (h - 24) / 2;
			btn.width = 68;
			btn.height = 24;
			clicked = kl_button(view->ui, style, SFTP_UI_RETRY, &btn, "Retry", KL_BUTTON_PRIMARY);
			if (clicked) {
				view->initial_loaded = 0;
				sftp_worker_retry(view->worker);
			}
		}
	}
}

/* Draws the left folder tree pane. */
static void
draw_tree_pane(
	struct sftp_view *view,
	const struct kl_style *style,
	const struct kl_rect *rect,
	uint64_t now_us)
{
	size_t first;
	size_t last;
	size_t i;
	struct kl_rect row;
	kl_color ink;
	const struct sftp_tree_node *node;
	unsigned state_bits;
	int indent;
	int baseline;
	int icon_x;
	int text_x;
	kl_color faint;
	const char *chev;
	char label[SSH_SFTP_PATH_MAX * 2U + 8U];
	int cmp_cur;
	int cmp_drop;

	/* Draws the tree panel background and begins virtualized list scrolling. */
	(void)now_us;
	kl_panel(style, rect, 1);
	view->tree_list.count = view->tree_flat_count;
	first = 0U;
	last = 0U;
	(void)kl_list_begin(view->ui, style, SFTP_UI_TREE_LIST, rect, &view->tree_list, view->tree_flat_count, &first, &last);

	/* Draws each visible folder tree row. */
	kl_canvas_clip_push(style->canvas, rect);
	for (i = first; i < last && i < view->tree_flat_count; i++) {
		node = view->tree_flat[i];
		(void)kl_list_row(view->ui, style, SFTP_UI_TREE_LIST, rect, &view->tree_list, i, &row, &ink);
		state_bits = 0U;
		cmp_cur = strcmp(node->path, view->current_path);
		if (cmp_cur == 0)
			state_bits |= KL_ITEM_SELECTED;
		if (view->drop_target_path[0] != '\0') {
			cmp_drop = strcmp(node->path, view->drop_target_path);
			if (cmp_drop == 0)
				state_bits |= KL_ITEM_HOVER | KL_ITEM_SELECTED;
		}

		/* Draws the item highlight, expand chevron, and folder icon. */
		kl_list_item(style, &row, state_bits, &ink, &faint);
		indent = 8 + node->depth * 14;
		icon_x = row.x + indent;
		if (style->text != NULL) {
			if (node->expanded)
				chev = "v";
			else
				chev = ">";
			baseline = kl_text_center(style->theme->text_small, row.y, row.height);
			(void)kl_text_draw(style->text, style->canvas, icon_x, baseline, chev, 1U, style->theme->text_small, 0, faint);
		}

		/* Draws the folder icon. */
		kl_icon_folder(style->canvas, (float)(icon_x + 14), (float)(row.y + (row.height - 16) / 2), 16.0f, style->theme->folder);

		/* Draws the sanitized folder label. */
		text_x = icon_x + 34;
		if (style->text != NULL && text_x < row.x + row.width - 4) {
			if (node->is_link)
				(void)snprintf(label, sizeof(label), "[L] %s", node->display);
			else
				(void)snprintf(label, sizeof(label), "%s", node->display);
			baseline = kl_text_center(style->theme->text_body, row.y, row.height);
			(void)kl_text_draw_fit(
				style->text,
				style->canvas,
				text_x,
				baseline,
				label,
				style->theme->text_body,
				0,
				row.x + row.width - text_x - 4,
				ink);
		}
	}

	/* Restores clipping and finishes the tree list widget. */
	kl_canvas_clip_pop(style->canvas);
	kl_list_end(view->ui, style, rect, &view->tree_list);
}

/* Draws the right detailed file list pane (header + rows + inline rename). */
static void
draw_file_pane(
	struct sftp_view *view,
	const struct kl_style *style,
	const struct kl_rect *rect,
	uint64_t now_us)
{
	struct kl_column cols[4];
	const struct sftp_list_row *r;
	struct kl_rect row;
	struct kl_rect frect;
	kl_color ink;
	kl_color faint;
	int w;
	int size_w;
	int mtime_w;
	int perm_w;
	int name_w;
	int baseline;
	int joined;
	int cmp_drop;
	unsigned state_bits;
	unsigned fres;
	unsigned rev_flag;
	size_t first;
	size_t last;
	size_t i;
	char full_row[SSH_SFTP_PATH_MAX];
	char label[SSH_SFTP_PATH_MAX * 4U];

	/* Computes column widths for Name, Size, Modified, and Permissions. */
	(void)now_us;
	w = view->rect_header.width;
	perm_w = 96;
	mtime_w = 136;
	size_w = 84;
	name_w = w - perm_w - mtime_w - size_w - 16;
	if (name_w < 100)
		name_w = 100;
	if (view->sort_reverse)
		rev_flag = KL_COLUMN_REVERSED;
	else
		rev_flag = 0U;

	/* Populates column headers and sort indicators. */
	cols[0].title = "Name";
	cols[0].x = view->rect_header.x + 8;
	cols[0].width = name_w;
	if (view->sort_column == SFTP_SORT_NAME)
		cols[0].flags = KL_COLUMN_SORTED | rev_flag;
	else
		cols[0].flags = 0U;
	cols[1].title = "Size";
	cols[1].x = cols[0].x + name_w;
	cols[1].width = size_w;
	if (view->sort_column == SFTP_SORT_SIZE)
		cols[1].flags = KL_COLUMN_SORTED | rev_flag;
	else
		cols[1].flags = 0U;
	cols[2].title = "Modified";
	cols[2].x = cols[1].x + size_w;
	cols[2].width = mtime_w;
	if (view->sort_column == SFTP_SORT_MTIME)
		cols[2].flags = KL_COLUMN_SORTED | rev_flag;
	else
		cols[2].flags = 0U;
	cols[3].title = "Perm.";
	cols[3].x = cols[2].x + mtime_w;
	cols[3].width = perm_w;
	cols[3].flags = 0U;

	/* Draws the column header bar and begins the virtualized file list. */
	kl_list_header(style, &view->rect_header, cols, 4U);
	view->file_list.count = view->row_count;
	first = 0U;
	last = 0U;
	(void)kl_list_begin(view->ui, style, SFTP_UI_FILE_LIST, rect, &view->file_list, view->row_count, &first, &last);

	/* Draws each visible file or folder row. */
	kl_canvas_clip_push(style->canvas, rect);
	for (i = first; i < last && i < view->row_count; i++) {
		r = &view->rows[i];
		(void)kl_list_row(view->ui, style, SFTP_UI_FILE_LIST, rect, &view->file_list, i, &row, &ink);
		state_bits = 0U;
		if (r->selected)
			state_bits |= KL_ITEM_SELECTED | KL_ITEM_FOCUSED;
		if (view->drop_target_path[0] != '\0' && (r->entry.is_dir || r->entry.link_is_dir)) {
			joined = sftp_transfer_join_remote(full_row, sizeof(full_row), view->current_path, r->entry.name);
			if (joined == 0) {
				cmp_drop = strcmp(full_row, view->drop_target_path);
				if (cmp_drop == 0)
					state_bits |= KL_ITEM_HOVER | KL_ITEM_SELECTED;
			}
		}

		/* Draws row selection/hover background and file or folder icon. */
		kl_list_item(style, &row, state_bits, &ink, &faint);
		if (r->entry.is_dir || r->entry.link_is_dir)
			kl_icon_folder(style->canvas, (float)(cols[0].x + 2), (float)(row.y + (row.height - 16) / 2), 16.0f, style->theme->folder);
		else
			kl_icon_file(style->canvas, style->text, (float)(cols[0].x + 2), (float)(row.y + (row.height - 16) / 2), 16.0f, style->theme->accent, "");

		/* Draws either the inline rename field or the four detail cells. */
		if (view->renaming && view->rename_row == i) {
			frect.x = cols[0].x + 24;
			frect.y = row.y + 1;
			frect.width = cols[0].width - 28;
			frect.height = row.height - 2;
			fres = kl_field(view->ui, style, SFTP_UI_RENAME_FIELD, &frect, &view->rename_field, "");
			if ((fres & KL_FIELD_SUBMITTED) != 0U) {
				sftp_view_commit_rename(view);
			} else if ((fres & KL_FIELD_CANCELLED) != 0U) {
				view->renaming = 0;
				view->dirty = 1;
			}
		} else {
			baseline = kl_text_center(style->theme->text_body, row.y, row.height);
			if (r->entry.is_link && r->display_target[0] != '\0')
				(void)snprintf(label, sizeof(label), "[L] %s -> %s", r->display_name, r->display_target);
			else if (r->entry.is_link)
				(void)snprintf(label, sizeof(label), "[L] %s", r->display_name);
			else
				(void)snprintf(label, sizeof(label), "%s", r->display_name);
			kl_list_cell(style, cols[0].x + 24, cols[0].width - 28, baseline, label, style->theme->text_body, 0U, ink);
			kl_list_cell(style, cols[1].x, cols[1].width - 8, baseline, r->size_text, style->theme->text_small, KL_CELL_RIGHT, faint);
			kl_list_cell(style, cols[2].x + 4, cols[2].width - 8, baseline, r->mtime_text, style->theme->text_small, 0U, faint);
			kl_list_cell(style, cols[3].x + 4, cols[3].width - 8, baseline, r->perm_text, style->theme->text_small, 0U, faint);
		}
	}

	/* Restores clipping and finishes the file list widget. */
	kl_canvas_clip_pop(style->canvas);
	kl_list_end(view->ui, style, rect, &view->file_list);
}

/* Draws the bottom transfer bar and optional expanded transfer queue. */
static void
draw_transfer_area(
	struct sftp_view *view,
	const struct kl_style *style,
	int width,
	int top_y,
	uint64_t now_us)
{
	struct kl_rect bar;
	struct kl_rect sep;
	struct kl_rect prog;
	struct kl_rect btn;
	struct kl_rect panel;
	struct kl_rect row;
	const struct sftp_transfer_snapshot *active_item;
	const struct sftp_transfer_snapshot *t;
	const char *base;
	const char *dir_verb;
	const char *dir_tag;
	const char *toggle_str;
	size_t first;
	size_t last;
	size_t i;
	char summary[MIKUTERM_PATH_MAX + 512];
	char done_s[32];
	char total_s[32];
	char clean_name[256];
	char clean_dest[MIKUTERM_PATH_MAX];
	char spd[32];
	char tlabel[32];
	char line[MIKUTERM_PATH_MAX + 512];
	char clean_err[256];
	unsigned eta;
	double frac;
	kl_color ink;
	kl_color faint;
	int baseline;
	int clicked;

	/* Draws the bottom transfer bar background and top separator. */
	bar.x = 0;
	bar.y = top_y;
	bar.width = width;
	bar.height = SFTP_TRANSFER_BAR_H;
	view->rect_transfer_bar = bar;
	kl_canvas_fill(style->canvas, &bar, style->theme->panel);
	sep.x = 0;
	sep.y = top_y;
	sep.width = width;
	sep.height = 1;
	kl_canvas_fill(style->canvas, &sep, style->theme->separator);

	/* Selects the active or most recent transfer item for the summary bar. */
	active_item = NULL;
	for (i = 0U; i < view->transfer_count; i++) {
		if (view->transfers[i].state == SFTP_TRANSFER_ACTIVE ||
		    view->transfers[i].state == SFTP_TRANSFER_WAITING_OVERWRITE) {
			active_item = &view->transfers[i];
			break;
		}
	}

	/* Falls back to the last item in the queue when none is active. */
	if (active_item == NULL && view->transfer_count > 0U)
		active_item = &view->transfers[view->transfer_count - 1U];

	/* Formats the transfer summary string and progress fraction. */
	frac = 0.0;
	if (active_item != NULL) {
		base = strrchr(active_item->remote_path, '/');
		if (base != NULL)
			base = base + 1;
		else
			base = active_item->remote_path;
		sftp_view_sanitize_text(base, clean_name, sizeof(clean_name));
		clean_dest[0] = '\0';
		if (active_item->local_path[0] != '\0')
			sftp_view_sanitize_text(active_item->local_path, clean_dest, sizeof(clean_dest));
		sftp_view_format_size(active_item->bytes_done, done_s, sizeof(done_s));
		sftp_view_format_size(active_item->bytes_total, total_s, sizeof(total_s));
		if (active_item->bytes_total > 0ULL)
			frac = (double)active_item->bytes_done / (double)active_item->bytes_total;
		if (active_item->state == SFTP_TRANSFER_ACTIVE && view->transfer_speed > 1.0) {
			sftp_view_format_size((uint64_t)view->transfer_speed, spd, sizeof(spd));
			if (active_item->bytes_total > active_item->bytes_done)
				eta = (unsigned)((double)(active_item->bytes_total - active_item->bytes_done) / view->transfer_speed);
			else
				eta = 0U;
			if (active_item->direction == SFTP_TRANSFER_UPLOAD) {
				(void)snprintf(
					summary,
					sizeof(summary),
					"Uploading %s  %s / %s  %s/s  %u s left",
					clean_name,
					done_s,
					total_s,
					spd,
					eta);
			} else if (clean_dest[0] != '\0') {
				(void)snprintf(
					summary,
					sizeof(summary),
					"Downloading %s to %s  %s / %s  %s/s  %u s left",
					clean_name,
					clean_dest,
					done_s,
					total_s,
					spd,
					eta);
			} else {
				(void)snprintf(
					summary,
					sizeof(summary),
					"Downloading %s  %s / %s  %s/s  %u s left",
					clean_name,
					done_s,
					total_s,
					spd,
					eta);
			}
		} else if (active_item->direction == SFTP_TRANSFER_DOWNLOAD && clean_dest[0] != '\0') {
			if (active_item->state == SFTP_TRANSFER_DONE) {
				(void)snprintf(summary, sizeof(summary), "Saved to %s", clean_dest);
			} else {
				(void)snprintf(
					summary,
					sizeof(summary),
					"Download %s to %s  (%s)",
					clean_name,
					clean_dest,
					draw_transfer_state_text(active_item->state));
			}
		} else {
			if (active_item->direction == SFTP_TRANSFER_UPLOAD)
				dir_verb = "Upload";
			else
				dir_verb = "Download";
			(void)snprintf(
				summary,
				sizeof(summary),
				"%s %s  (%s)",
				dir_verb,
				clean_name,
				draw_transfer_state_text(active_item->state));
		}
	} else {
		(void)snprintf(summary, sizeof(summary), "No transfers");
	}

	/* Draws the summary text. */
	if (style->text != NULL) {
		baseline = kl_text_center(style->theme->text_small, top_y, SFTP_TRANSFER_BAR_H);
		(void)kl_text_draw_fit(
			style->text,
			style->canvas,
			12,
			baseline,
			summary,
			style->theme->text_small,
			0,
			width - 360,
			style->theme->text);
	}

	/* Draws the progress bar when an active transfer is running. */
	prog.x = width - 340;
	prog.y = top_y + (SFTP_TRANSFER_BAR_H - 8) / 2;
	prog.width = 120;
	prog.height = 8;
	if (prog.x > 160 && active_item != NULL && active_item->state == SFTP_TRANSFER_ACTIVE)
		kl_progress(style, &prog, frac, now_us);

	/* Draws the Cancel all button when any transfer is active or queued. */
	if (view->active_transfers > 0U) {
		btn.x = width - 208;
		btn.y = top_y + (SFTP_TRANSFER_BAR_H - 26) / 2;
		btn.width = 92;
		btn.height = 26;
		clicked = kl_button(view->ui, style, SFTP_UI_TRANSFER_CANCEL_ALL, &btn, "Cancel all", KL_BUTTON_DANGER);
		if (clicked)
			sftp_worker_cancel_all(view->worker);
	}

	/* Draws the Queue / Hide toggle button. */
	btn.x = width - 108;
	btn.y = top_y + (SFTP_TRANSFER_BAR_H - 26) / 2;
	btn.width = 96;
	btn.height = 26;
	if (view->active_transfers > 1U) {
		(void)snprintf(tlabel, sizeof(tlabel), "%zu more", view->active_transfers - 1U);
	} else {
		if (view->transfers_expanded)
			toggle_str = "Hide";
		else
			toggle_str = "Queue";
		(void)snprintf(tlabel, sizeof(tlabel), "%s", toggle_str);
	}

	/* Toggles the expanded queue panel when clicked. */
	clicked = kl_button(view->ui, style, SFTP_UI_TRANSFER_TOGGLE, &btn, tlabel, 0U);
	if (clicked) {
		view->transfers_expanded = !view->transfers_expanded;
		view->dirty = 1;
	}

	/* Draws the expanded transfer queue panel or clears its hit rectangle. */
	if (view->transfers_expanded) {
		panel.x = 0;
		panel.y = top_y + SFTP_TRANSFER_BAR_H;
		panel.width = width;
		panel.height = SFTP_TRANSFER_PANEL_H;
		view->rect_transfer_panel = panel;
		kl_canvas_fill(style->canvas, &panel, style->theme->card);
		view->transfer_list.count = view->transfer_count;
		first = 0U;
		last = 0U;
		(void)kl_list_begin(view->ui, style, SFTP_UI_TRANSFER_LIST, &panel, &view->transfer_list, view->transfer_count, &first, &last);
		kl_canvas_clip_push(style->canvas, &panel);
		for (i = first; i < last && i < view->transfer_count; i++) {
			t = &view->transfers[i];
			(void)kl_list_row(view->ui, style, SFTP_UI_TRANSFER_LIST, &panel, &view->transfer_list, i, &row, &ink);
			kl_list_item(style, &row, 0U, &ink, &faint);
			base = strrchr(t->remote_path, '/');
			if (base != NULL)
				base = base + 1;
			else
				base = t->remote_path;
			sftp_view_sanitize_text(base, clean_name, sizeof(clean_name));
			clean_dest[0] = '\0';
			if (t->local_path[0] != '\0')
				sftp_view_sanitize_text(t->local_path, clean_dest, sizeof(clean_dest));
			sftp_view_sanitize_text(t->error_text, clean_err, sizeof(clean_err));
			sftp_view_format_size(t->bytes_done, done_s, sizeof(done_s));
			sftp_view_format_size(t->bytes_total, total_s, sizeof(total_s));
			if (t->direction == SFTP_TRANSFER_UPLOAD)
				dir_tag = "[UP]";
			else
				dir_tag = "[DN]";
			if (clean_err[0] != '\0') {
				(void)snprintf(
					line,
					sizeof(line),
					"%s %s - %s (%s)",
					dir_tag,
					clean_name,
					draw_transfer_state_text(t->state),
					clean_err);
			} else if (t->direction == SFTP_TRANSFER_DOWNLOAD && clean_dest[0] != '\0') {
				(void)snprintf(
					line,
					sizeof(line),
					"%s %s -> %s - %s (%s / %s)",
					dir_tag,
					clean_name,
					clean_dest,
					draw_transfer_state_text(t->state),
					done_s,
					total_s);
			} else {
				(void)snprintf(
					line,
					sizeof(line),
					"%s %s - %s (%s / %s)",
					dir_tag,
					clean_name,
					draw_transfer_state_text(t->state),
					done_s,
					total_s);
			}

			/* Draws the transfer queue row text. */
			baseline = kl_text_center(style->theme->text_small, row.y, row.height);
			if (style->text != NULL) {
				(void)kl_text_draw_fit(
					style->text,
					style->canvas,
					row.x + 12,
					baseline,
					line,
					style->theme->text_small,
					0,
					row.width - 96,
					ink);
			}

			/* Draws a per-item Cancel button for queued or active items. */
			if (t->state == SFTP_TRANSFER_QUEUED || t->state == SFTP_TRANSFER_ACTIVE || t->state == SFTP_TRANSFER_WAITING_OVERWRITE) {
				btn.x = row.x + row.width - 76;
				btn.y = row.y + 2;
				btn.width = 64;
				btn.height = row.height - 4;
				clicked = kl_button(view->ui, style, SFTP_UI_TRANSFER_ITEM_BASE + (uint32_t)i, &btn, "Cancel", KL_BUTTON_QUIET);
				if (clicked)
					sftp_worker_cancel_transfer(view->worker, t->id);
			}
		}

		/* Restores clipping and finishes the transfer list widget. */
		kl_canvas_clip_pop(style->canvas);
		kl_list_end(view->ui, style, &panel, &view->transfer_list);
	} else {
		memset(&view->rect_transfer_panel, 0, sizeof(view->rect_transfer_panel));
	}
}

/* Draws the active modal dialog. */
static void
draw_modal_dialog(
	struct sftp_view *view,
	const struct kl_style *style,
	int width,
	int height)
{
	static const char *const overwrite_labels[5] = {
		"Overwrite", "Skip", "Overwrite all", "Skip all", "Cancel"
	};
	static const enum sftp_conflict_action overwrite_actions[5] = {
		SFTP_CONFLICT_OVERWRITE,
		SFTP_CONFLICT_SKIP,
		SFTP_CONFLICT_OVERWRITE_ALL,
		SFTP_CONFLICT_SKIP_ALL,
		SFTP_CONFLICT_CANCEL
	};
	static const char *const delete_labels[2] = {
		"Delete", "Cancel"
	};
	static const char *const error_labels[1] = {
		"OK"
	};
	struct kl_rect area;
	struct kl_rect box;
	struct kl_rect fld;
	struct kl_rect ok_btn;
	struct kl_rect cancel_btn;
	char err[256];
	char clean[512];
	char new_path[SSH_SFTP_PATH_MAX];
	const char *slash;
	size_t i;
	unsigned fres;
	int create_btn;
	int cancel_clicked;
	int create_now;
	int chosen;
	int error;

	/* Sets up the full-viewport modal backdrop rectangle. */
	area.x = 0;
	area.y = 0;
	area.width = width;
	area.height = height;

	/* Renders the active modal dialog kind. */
	if (view->dialog == SFTP_DIALOG_OVERWRITE) {
		chosen = kl_dialog(view->ui, style, SFTP_UI_DIALOG_BASE, &area, view->dialog_title, view->dialog_body, overwrite_labels, 5);
		if (chosen >= 0) {
			sftp_worker_resolve_conflict(view->worker, view->dialog_transfer_id, overwrite_actions[chosen]);
			view->dialog = SFTP_DIALOG_NONE;
			view->dirty = 1;
		}
	} else if (view->dialog == SFTP_DIALOG_DELETE) {
		chosen = kl_dialog(view->ui, style, SFTP_UI_DIALOG_BASE, &area, view->dialog_title, view->dialog_body, delete_labels, 2);
		if (chosen == 0) {
			for (i = 0U; i < view->dialog_delete_count; i++) {
				err[0] = '\0';
				error = sftp_worker_delete(view->worker, view->dialog_delete_paths[i], err, sizeof(err));
				if (error != 0) {
					sftp_view_sanitize_text(err, clean, sizeof(clean));
					sftp_view_show_error(view, "Delete failed", clean);
					return;
				}
			}

			/* Closes the dialog and refreshes the listing after deletion. */
			view->dialog = SFTP_DIALOG_NONE;
			sftp_view_refresh_all(view);
		} else if (chosen == 1) {
			view->dialog = SFTP_DIALOG_NONE;
			view->dirty = 1;
		}
	} else if (view->dialog == SFTP_DIALOG_NEW_FOLDER) {
		box.width = 380;
		box.height = 148;
		box.x = (width - box.width) / 2;
		box.y = (height - box.height) / 2;
		kl_canvas_fill(style->canvas, &area, KL_RGBA(0x000000U, 128U));
		kl_canvas_shadow(
			style->canvas,
			(float)box.x,
			(float)box.y,
			(float)box.width,
			(float)box.height,
			style->theme->card_radius,
			18.0f,
			style->theme->shadow);
		kl_canvas_round(
			style->canvas,
			(float)box.x,
			(float)box.y,
			(float)box.width,
			(float)box.height,
			style->theme->card_radius,
			style->theme->card);
		kl_canvas_round_border(
			style->canvas,
			(float)box.x,
			(float)box.y,
			(float)box.width,
			(float)box.height,
			style->theme->card_radius,
			1.0f,
			style->theme->card_edge);

		/* Draws the dialog title and prompt body. */
		if (style->text != NULL) {
			(void)kl_text_draw(
				style->text,
				style->canvas,
				box.x + 16,
				box.y + 28,
				view->dialog_title,
				strlen(view->dialog_title),
				style->theme->text_title,
				1,
				style->theme->text);
			(void)kl_text_draw_fit(
				style->text,
				style->canvas,
				box.x + 16,
				box.y + 52,
				view->dialog_body,
				style->theme->text_small,
				0,
				box.width - 32,
				style->theme->text_secondary);
		}

		/* Draws the folder name input field and Create/Cancel buttons. */
		fld.x = box.x + 16;
		fld.y = box.y + 64;
		fld.width = box.width - 32;
		fld.height = 30;
		fres = kl_field(view->ui, style, SFTP_UI_DIALOG_FIELD, &fld, &view->dialog_field, "folder name");
		ok_btn.x = box.x + box.width - 176;
		ok_btn.y = box.y + box.height - 40;
		ok_btn.width = 76;
		ok_btn.height = 28;
		cancel_btn.x = box.x + box.width - 92;
		cancel_btn.y = ok_btn.y;
		cancel_btn.width = 76;
		cancel_btn.height = 28;

		/* Handles Create or Cancel actions. */
		create_btn = kl_button(view->ui, style, SFTP_UI_DIALOG_BASE, &ok_btn, "Create", KL_BUTTON_PRIMARY);
		cancel_clicked = kl_button(view->ui, style, SFTP_UI_DIALOG_BASE + 1U, &cancel_btn, "Cancel", 0U);
		create_now = create_btn || (fres & KL_FIELD_SUBMITTED) != 0U;
		if (create_now && view->dialog_field.text[0] != '\0') {
			slash = strchr(view->dialog_field.text, '/');
			if (slash != NULL) {
				sftp_view_show_error(view, "New folder failed", "Folder name cannot contain '/'.");
				return;
			}

			/* Joins the new directory path and issues MKDIR. */
			error = sftp_transfer_join_remote(new_path, sizeof(new_path), view->current_path, view->dialog_field.text);
			if (error == 0) {
				err[0] = '\0';
				error = sftp_worker_mkdir(view->worker, new_path, err, sizeof(err));
				if (error != 0) {
					sftp_view_sanitize_text(err, clean, sizeof(clean));
					sftp_view_show_error(view, "New folder failed", clean);
					return;
				}
			}

			/* Closes the dialog and refreshes the listing. */
			view->dialog = SFTP_DIALOG_NONE;
			sftp_view_refresh_all(view);
		} else if (cancel_clicked || (fres & KL_FIELD_CANCELLED) != 0U) {
			view->dialog = SFTP_DIALOG_NONE;
			view->dirty = 1;
		}
	} else if (view->dialog == SFTP_DIALOG_ERROR) {
		chosen = kl_dialog(view->ui, style, SFTP_UI_DIALOG_BASE, &area, view->dialog_title, view->dialog_body, error_labels, 1);
		if (chosen >= 0) {
			view->dialog = SFTP_DIALOG_NONE;
			view->dirty = 1;
		}
	}
}

/* Draws the in-view right-click context menu. */
static void
draw_context_menu(
	struct sftp_view *view,
	const struct kl_style *style)
{
	int menu_w;
	int item_h;
	int menu_h;
	size_t i;
	struct kl_rect r;
	struct kl_rect ir;
	kl_color ink;
	int baseline;

	/* Computes and clamps the context menu popup rectangle. */
	menu_w = 180;
	item_h = 26;
	menu_h = (int)view->ctx_count * item_h + 8;
	r.x = view->ctx_x;
	r.y = view->ctx_y;
	if (r.x + menu_w > view->width)
		r.x = view->width - menu_w - 4;
	if (r.y + menu_h > view->height)
		r.y = view->height - menu_h - 4;
	if (r.x < 4)
		r.x = 4;
	if (r.y < 4)
		r.y = 4;
	r.width = menu_w;
	r.height = menu_h;
	view->rect_ctx = r;

	/* Draws the context menu card shadow, fill, and border. */
	kl_canvas_shadow(
		style->canvas,
		(float)r.x,
		(float)r.y,
		(float)r.width,
		(float)r.height,
		style->theme->control_radius,
		12.0f,
		style->theme->shadow);
	kl_canvas_round(
		style->canvas,
		(float)r.x,
		(float)r.y,
		(float)r.width,
		(float)r.height,
		style->theme->control_radius,
		KL_RGBA(style->theme->card, 0xf2U));
	kl_canvas_round_border(
		style->canvas,
		(float)r.x,
		(float)r.y,
		(float)r.width,
		(float)r.height,
		style->theme->control_radius,
		1.0f,
		style->theme->card_edge);

	/* Draws each context menu item and hover highlight. */
	for (i = 0U; i < view->ctx_count; i++) {
		ir.x = r.x + 4;
		ir.y = r.y + 4 + (int)i * item_h;
		ir.width = menu_w - 8;
		ir.height = item_h;
		if ((int)i == view->ctx_hover && view->ctx_items[i].enabled) {
			kl_canvas_round(
				style->canvas,
				(float)ir.x,
				(float)ir.y,
				(float)ir.width,
				(float)ir.height,
				style->theme->control_radius,
				style->theme->selection);
		}

		/* Draws the item label using active or faint ink. */
		if (view->ctx_items[i].enabled)
			ink = style->theme->text;
		else
			ink = style->theme->text_faint;
		if (style->text != NULL) {
			baseline = kl_text_center(style->theme->text_body, ir.y, ir.height);
			(void)kl_text_draw(
				style->text,
				style->canvas,
				ir.x + 10,
				baseline,
				view->ctx_items[i].label,
				strlen(view->ctx_items[i].label),
				style->theme->text_body,
				0,
				ink);
		}
	}
}

/* Returns human-readable text for a transfer state. */
static const char *
draw_transfer_state_text(
	enum sftp_transfer_state st)
{
	/* Maps transfer state enum to display text. */
	switch (st) {
	case SFTP_TRANSFER_QUEUED:
		return "Queued";
	case SFTP_TRANSFER_WAITING_OVERWRITE:
		return "Waiting";
	case SFTP_TRANSFER_ACTIVE:
		return "Transferring";
	case SFTP_TRANSFER_DONE:
		return "Done";
	case SFTP_TRANSFER_SKIPPED:
		return "Skipped";
	case SFTP_TRANSFER_CANCELLED:
		return "Cancelled";
	case SFTP_TRANSFER_FAILED:
		return "Failed";
	default:
		return "";
	}
}
