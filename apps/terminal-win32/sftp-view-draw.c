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
	uint64_t now_us;
	int w;
	int h;
	int transfer_h;
	int banner_h;
	int body_y;
	int body_h;
	int split_x;
	int error;

	if (view == NULL || pixels == NULL || width == 0U || height == 0U)
		return EINVAL;

	if (view->canvas_ready) {
		kl_canvas_release(&view->canvas);
		view->canvas_ready = 0;
	}
	error = kl_canvas_init(&view->canvas, pixels, (size_t)width, (int)width, (int)height);
	if (error != 0)
		return error;
	view->canvas_ready = 1;
	view->scale = state != NULL && state->scale != 0U ? state->scale : KL_SCALE_BASE;
	kl_canvas_set_scale(&view->canvas, view->scale);
	view->width = (int)width;
	view->height = (int)height;

	frame.x = 0;
	frame.y = 0;
	frame.width = (int)width;
	frame.height = (int)height;
	kl_canvas_logical(&view->canvas, &frame, &units);
	w = units.width;
	h = units.height;

	theme = kl_theme_default();
	style.canvas = &view->canvas;
	style.text = view->text_ready ? &view->text : NULL;
	style.theme = theme;
	style.glass = 0;

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
		transfer_h = SFTP_TRANSFER_BAR_H + (view->transfers_expanded ? SFTP_TRANSFER_PANEL_H : 0);
		if (transfer_h > h - SFTP_BAR_HEIGHT - banner_h - 80)
			transfer_h = SFTP_TRANSFER_BAR_H;
	}

	body_y = SFTP_BAR_HEIGHT + banner_h;
	body_h = h - body_y - transfer_h;
	if (body_h < 40)
		body_h = 40;

	split_x = view->split_width;
	if (split_x < SFTP_SPLIT_MIN)
		split_x = SFTP_SPLIT_MIN;
	if (split_x > w - 180)
		split_x = w > 300 ? w - 180 : w / 3;

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

	draw_tree_pane(view, &style, &view->rect_tree, now_us);
	kl_canvas_fill(&view->canvas, &split_rect, theme->separator);
	draw_file_pane(view, &style, &view->rect_list, now_us);

	if (transfer_h > 0)
		draw_transfer_area(view, &style, w, h - transfer_h, now_us);
	else {
		memset(&view->rect_transfer_bar, 0, sizeof(view->rect_transfer_bar));
		memset(&view->rect_transfer_panel, 0, sizeof(view->rect_transfer_panel));
	}

	/* Rubber-band selection rectangle. */
	if (view->band_active) {
		struct kl_rect band;

		band.x = view->band_x0 < view->band_x1 ? view->band_x0 : view->band_x1;
		band.y = view->band_y0 < view->band_y1 ? view->band_y0 : view->band_y1;
		band.width = abs(view->band_x1 - view->band_x0);
		band.height = abs(view->band_y1 - view->band_y0);
		kl_band(&style, &band);
	}

	/* Context menu and modal dialog on top. */
	if (view->ctx_open)
		draw_context_menu(view, &style);
	if (view->dialog != SFTP_DIALOG_NONE)
		draw_modal_dialog(view, &style, w, h);

	if (view->ui != NULL)
		(void)kl_ui_end(view->ui, now_us);

	kl_mutex_lock(&view->lock);
	view->dirty = 0;
	kl_mutex_unlock(&view->lock);
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
	int x;
	int right_x;
	int btn_w_refresh;
	int btn_w_new;
	int cy;
	unsigned flags;

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
	flags = (view->history.index == 0U || view->history.count == 0U) ? KL_BUTTON_DISABLED : 0U;
	if (kl_icon_button(view->ui, style, SFTP_UI_BACK, 0U, &btn, KL_ICON_BACK, 16, flags))
		sftp_view_navigate_back(view);
	x += 32;

	btn.x = x;
	flags = (view->history.index + 1U >= view->history.count) ? KL_BUTTON_DISABLED : 0U;
	if (kl_icon_button(view->ui, style, SFTP_UI_FWD, 0U, &btn, KL_ICON_FORWARD, 16, flags))
		sftp_view_navigate_forward(view);
	x += 32;

	btn.x = x;
	flags = (view->current_path[0] == '\0' || strcmp(view->current_path, "/") == 0) ? KL_BUTTON_DISABLED : 0U;
	if (kl_icon_button(view->ui, style, SFTP_UI_UP, 0U, &btn, KL_ICON_UP, 16, flags))
		sftp_view_navigate_up(view);
	x += 36;

	/* Right-aligned Refresh and New folder buttons. */
	btn_w_new = style->text != NULL ? kl_button_width(style, "New folder") : 96;
	btn_w_refresh = style->text != NULL ? kl_button_width(style, "Refresh") : 74;
	right_x = width - 8 - btn_w_new;
	btn.x = right_x;
	btn.y = cy;
	btn.width = btn_w_new;
	btn.height = 28;
	if (kl_button(view->ui, style, SFTP_UI_NEW_FOLDER, &btn, "New folder",
		      view->worker_state == SFTP_WORKER_READY ? 0U : KL_BUTTON_DISABLED))
		sftp_view_begin_new_folder(view);

	right_x -= 6 + btn_w_refresh;
	btn.x = right_x;
	btn.width = btn_w_refresh;
	if (kl_button(view->ui, style, SFTP_UI_REFRESH, &btn, "Refresh",
		      view->worker_state == SFTP_WORKER_READY ? 0U : KL_BUTTON_DISABLED))
		sftp_view_refresh_all(view);

	crumb_area.x = x;
	crumb_area.y = cy;
	crumb_area.width = right_x - 8 - x;
	crumb_area.height = 28;
	if (crumb_area.width < 40)
		crumb_area.width = 40;
	view->rect_breadcrumb = crumb_area;

	if (view->editing_path) {
		unsigned f_res;

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
		kl_canvas_round(style->canvas, (float)crumb_area.x, (float)crumb_area.y,
			(float)crumb_area.width, (float)crumb_area.height,
			style->theme->control_radius, style->theme->control);
		kl_canvas_round_border(style->canvas, (float)crumb_area.x, (float)crumb_area.y,
			(float)crumb_area.width, (float)crumb_area.height,
			style->theme->control_radius, 1.0f, style->theme->control_edge);

		if (style->text != NULL && view->current_path[0] != '\0') {
			char clean[SSH_SFTP_PATH_MAX * 2U];
			const char *p;
			int cx;
			int baseline;

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
				const char *next;
				size_t seg_len;
				char seg_raw[SSH_SFTP_PATH_MAX];

				next = strchr(p, '/');
				seg_len = next != NULL ? (size_t)(next - p) : strlen(p);
				if (seg_len > 0U && seg_len < sizeof(seg_raw)) {
					memcpy(seg_raw, p, seg_len);
					seg_raw[seg_len] = '\0';
					sftp_view_sanitize_text(seg_raw, clean, sizeof(clean));
					(void)kl_text_draw(style->text, style->canvas, cx, baseline, ">", 1U, style->theme->text_small, 0, style->theme->text_faint);
					cx += kl_text_width(style->text, ">", 1U, style->theme->text_small, 0) + 6;
					(void)kl_text_draw(style->text, style->canvas, cx, baseline, clean, strlen(clean), style->theme->text_body, 0, style->theme->text);
					cx += kl_text_width(style->text, clean, strlen(clean), style->theme->text_body, 0) + 6;
				}
				p += seg_len;
				if (*p == '/')
					p++;
			}
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

	bar.x = 0;
	bar.y = y;
	bar.width = width;
	bar.height = h;
	kl_canvas_fill(style->canvas, &bar, style->theme->card);

	ink = style->theme->text;
	if (view->worker_state == SFTP_WORKER_OPENING || view->worker_state == SFTP_WORKER_IDLE) {
		msg = "Opening SFTP session...";
	} else if (view->worker_state == SFTP_WORKER_FAILED) {
		msg = view->worker_error[0] != '\0' ? view->worker_error : "SFTP session failed.";
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

	if (style->text != NULL) {
		baseline = kl_text_center(style->theme->text_small, y, h);
		(void)kl_text_draw_fit(style->text, style->canvas, 12, baseline, msg, style->theme->text_small, 0, width - 100, ink);
	}

	if (view->worker_state == SFTP_WORKER_FAILED && !sftp_link_is_ended(view->link)) {
		btn.x = width - 80;
		btn.y = y + (h - 24) / 2;
		btn.width = 68;
		btn.height = 24;
		if (kl_button(view->ui, style, SFTP_UI_RETRY, &btn, "Retry", KL_BUTTON_PRIMARY)) {
			view->initial_loaded = 0;
			sftp_worker_retry(view->worker);
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

	(void)now_us;
	kl_panel(style, rect, 1);

	view->tree_list.count = view->tree_flat_count;
	first = 0U;
	last = 0U;
	(void)kl_list_begin(view->ui, style, SFTP_UI_TREE_LIST, rect, &view->tree_list, view->tree_flat_count, &first, &last);

	kl_canvas_clip_push(style->canvas, rect);
	for (i = first; i < last && i < view->tree_flat_count; i++) {
		const struct sftp_tree_node *node;
		unsigned state_bits;
		int indent;
		int baseline;
		int icon_x;
		int text_x;
		kl_color faint;

		node = view->tree_flat[i];
		(void)kl_list_row(view->ui, style, SFTP_UI_TREE_LIST, rect, &view->tree_list, i, &row, &ink);

		state_bits = 0U;
		if (strcmp(node->path, view->current_path) == 0)
			state_bits |= KL_ITEM_SELECTED;
		if (view->drop_target_path[0] != '\0' && strcmp(node->path, view->drop_target_path) == 0)
			state_bits |= KL_ITEM_HOVER | KL_ITEM_SELECTED;

		kl_list_item(style, &row, state_bits, &ink, &faint);

		indent = 8 + node->depth * 14;
		icon_x = row.x + indent;
		if (style->text != NULL) {
			const char *chev;

			chev = node->expanded ? "v" : ">";
			baseline = kl_text_center(style->theme->text_small, row.y, row.height);
			(void)kl_text_draw(style->text, style->canvas, icon_x, baseline, chev, 1U, style->theme->text_small, 0, faint);
		}
		kl_icon_folder(style->canvas, (float)(icon_x + 14), (float)(row.y + (row.height - 16) / 2), 16.0f, style->theme->folder);

		text_x = icon_x + 34;
		if (style->text != NULL && text_x < row.x + row.width - 4) {
			char label[SSH_SFTP_PATH_MAX * 2U + 8U];

			if (node->is_link)
				(void)snprintf(label, sizeof(label), "[L] %s", node->display);
			else
				(void)snprintf(label, sizeof(label), "%s", node->display);
			baseline = kl_text_center(style->theme->text_body, row.y, row.height);
			(void)kl_text_draw_fit(style->text, style->canvas, text_x, baseline, label,
				style->theme->text_body, 0, row.x + row.width - text_x - 4, ink);
		}
	}
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
	int w;
	int size_w;
	int mtime_w;
	int perm_w;
	int name_w;
	size_t first;
	size_t last;
	size_t i;
	struct kl_rect row;
	kl_color ink;

	(void)now_us;
	w = view->rect_header.width;
	perm_w = 96;
	mtime_w = 136;
	size_w = 84;
	name_w = w - perm_w - mtime_w - size_w - 16;
	if (name_w < 100)
		name_w = 100;

	cols[0].title = "Name";
	cols[0].x = view->rect_header.x + 8;
	cols[0].width = name_w;
	cols[0].flags = view->sort_column == SFTP_SORT_NAME ? (KL_COLUMN_SORTED | (view->sort_reverse ? KL_COLUMN_REVERSED : 0U)) : 0U;

	cols[1].title = "Size";
	cols[1].x = cols[0].x + name_w;
	cols[1].width = size_w;
	cols[1].flags = view->sort_column == SFTP_SORT_SIZE ? (KL_COLUMN_SORTED | (view->sort_reverse ? KL_COLUMN_REVERSED : 0U)) : 0U;

	cols[2].title = "Modified";
	cols[2].x = cols[1].x + size_w;
	cols[2].width = mtime_w;
	cols[2].flags = view->sort_column == SFTP_SORT_MTIME ? (KL_COLUMN_SORTED | (view->sort_reverse ? KL_COLUMN_REVERSED : 0U)) : 0U;

	cols[3].title = "Perm.";
	cols[3].x = cols[2].x + mtime_w;
	cols[3].width = perm_w;
	cols[3].flags = 0U;

	kl_list_header(style, &view->rect_header, cols, 4U);

	view->file_list.count = view->row_count;
	first = 0U;
	last = 0U;
	(void)kl_list_begin(view->ui, style, SFTP_UI_FILE_LIST, rect, &view->file_list, view->row_count, &first, &last);

	kl_canvas_clip_push(style->canvas, rect);
	for (i = first; i < last && i < view->row_count; i++) {
		const struct sftp_list_row *r;
		unsigned state_bits;
		kl_color faint;
		int baseline;
		char full_row[SSH_SFTP_PATH_MAX];
		char label[SSH_SFTP_PATH_MAX * 4U];

		r = &view->rows[i];
		(void)kl_list_row(view->ui, style, SFTP_UI_FILE_LIST, rect, &view->file_list, i, &row, &ink);

		state_bits = 0U;
		if (r->selected)
			state_bits |= KL_ITEM_SELECTED | KL_ITEM_FOCUSED;
		if (view->drop_target_path[0] != '\0' && (r->entry.is_dir || r->entry.link_is_dir) &&
		    sftp_transfer_join_remote(full_row, sizeof(full_row), view->current_path, r->entry.name) == 0 &&
		    strcmp(full_row, view->drop_target_path) == 0)
			state_bits |= KL_ITEM_HOVER | KL_ITEM_SELECTED;

		kl_list_item(style, &row, state_bits, &ink, &faint);

		if (r->entry.is_dir || r->entry.link_is_dir)
			kl_icon_folder(style->canvas, (float)(cols[0].x + 2), (float)(row.y + (row.height - 16) / 2), 16.0f, style->theme->folder);
		else
			kl_icon_file(style->canvas, style->text, (float)(cols[0].x + 2), (float)(row.y + (row.height - 16) / 2), 16.0f, style->theme->accent, "");

		if (view->renaming && view->rename_row == i) {
			struct kl_rect frect;
			unsigned fres;

			frect.x = cols[0].x + 24;
			frect.y = row.y + 1;
			frect.width = cols[0].width - 28;
			frect.height = row.height - 2;
			fres = kl_field(view->ui, style, SFTP_UI_RENAME_FIELD, &frect, &view->rename_field, "");
			if ((fres & KL_FIELD_SUBMITTED) != 0U)
				sftp_view_commit_rename(view);
			else if ((fres & KL_FIELD_CANCELLED) != 0U) {
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
	const struct sftp_transfer_snapshot *active_item;
	size_t i;
	char summary[256];
	char done_s[32];
	char total_s[32];
	char clean_name[256];
	double frac;
	int baseline;

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

	active_item = NULL;
	for (i = 0U; i < view->transfer_count; i++) {
		if (view->transfers[i].state == SFTP_TRANSFER_ACTIVE ||
		    view->transfers[i].state == SFTP_TRANSFER_WAITING_OVERWRITE) {
			active_item = &view->transfers[i];
			break;
		}
	}
	if (active_item == NULL && view->transfer_count > 0U)
		active_item = &view->transfers[view->transfer_count - 1U];

	frac = 0.0;
	if (active_item != NULL) {
		const char *base;

		base = strrchr(active_item->remote_path, '/');
		base = base != NULL ? base + 1 : active_item->remote_path;
		sftp_view_sanitize_text(base, clean_name, sizeof(clean_name));
		sftp_view_format_size(active_item->bytes_done, done_s, sizeof(done_s));
		sftp_view_format_size(active_item->bytes_total, total_s, sizeof(total_s));
		if (active_item->bytes_total > 0ULL)
			frac = (double)active_item->bytes_done / (double)active_item->bytes_total;
		if (active_item->state == SFTP_TRANSFER_ACTIVE && view->transfer_speed > 1.0) {
			char spd[32];
			unsigned eta;

			sftp_view_format_size((uint64_t)view->transfer_speed, spd, sizeof(spd));
			eta = active_item->bytes_total > active_item->bytes_done
				? (unsigned)((double)(active_item->bytes_total - active_item->bytes_done) / view->transfer_speed)
				: 0U;
			(void)snprintf(summary, sizeof(summary), "%s %s  %s / %s  %s/s  %u s left",
				active_item->direction == SFTP_TRANSFER_UPLOAD ? "Uploading" : "Downloading",
				clean_name, done_s, total_s, spd, eta);
		} else {
			(void)snprintf(summary, sizeof(summary), "%s %s  (%s)",
				active_item->direction == SFTP_TRANSFER_UPLOAD ? "Upload" : "Download",
				clean_name, draw_transfer_state_text(active_item->state));
		}
	} else {
		(void)snprintf(summary, sizeof(summary), "No transfers");
	}

	if (style->text != NULL) {
		baseline = kl_text_center(style->theme->text_small, top_y, SFTP_TRANSFER_BAR_H);
		(void)kl_text_draw_fit(style->text, style->canvas, 12, baseline, summary,
			style->theme->text_small, 0, width - 360, style->theme->text);
	}

	prog.x = width - 340;
	prog.y = top_y + (SFTP_TRANSFER_BAR_H - 8) / 2;
	prog.width = 120;
	prog.height = 8;
	if (prog.x > 160 && active_item != NULL && active_item->state == SFTP_TRANSFER_ACTIVE)
		kl_progress(style, &prog, frac, now_us);

	if (view->active_transfers > 0U) {
		btn.x = width - 208;
		btn.y = top_y + (SFTP_TRANSFER_BAR_H - 26) / 2;
		btn.width = 92;
		btn.height = 26;
		if (kl_button(view->ui, style, SFTP_UI_TRANSFER_CANCEL_ALL, &btn, "Cancel all", KL_BUTTON_DANGER))
			sftp_worker_cancel_all(view->worker);
	}

	btn.x = width - 108;
	btn.y = top_y + (SFTP_TRANSFER_BAR_H - 26) / 2;
	btn.width = 96;
	btn.height = 26;
	{
		char tlabel[32];

		if (view->active_transfers > 1U)
			(void)snprintf(tlabel, sizeof(tlabel), "%zu more", view->active_transfers - 1U);
		else
			(void)snprintf(tlabel, sizeof(tlabel), view->transfers_expanded ? "Hide" : "Queue");
		if (kl_button(view->ui, style, SFTP_UI_TRANSFER_TOGGLE, &btn, tlabel, 0U)) {
			view->transfers_expanded = !view->transfers_expanded;
			view->dirty = 1;
		}
	}

	if (view->transfers_expanded) {
		struct kl_rect panel;
		size_t first;
		size_t last;
		struct kl_rect row;
		kl_color ink;

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
			const struct sftp_transfer_snapshot *t;
			const char *base;
			char line[512];
			char clean_err[256];
			kl_color faint;

			t = &view->transfers[i];
			(void)kl_list_row(view->ui, style, SFTP_UI_TRANSFER_LIST, &panel, &view->transfer_list, i, &row, &ink);
			kl_list_item(style, &row, 0U, &ink, &faint);

			base = strrchr(t->remote_path, '/');
			base = base != NULL ? base + 1 : t->remote_path;
			sftp_view_sanitize_text(base, clean_name, sizeof(clean_name));
			sftp_view_sanitize_text(t->error_text, clean_err, sizeof(clean_err));
			sftp_view_format_size(t->bytes_done, done_s, sizeof(done_s));
			sftp_view_format_size(t->bytes_total, total_s, sizeof(total_s));
			if (clean_err[0] != '\0')
				(void)snprintf(line, sizeof(line), "%s %s - %s (%s)",
					t->direction == SFTP_TRANSFER_UPLOAD ? "[UP]" : "[DN]",
					clean_name, draw_transfer_state_text(t->state), clean_err);
			else
				(void)snprintf(line, sizeof(line), "%s %s - %s (%s / %s)",
					t->direction == SFTP_TRANSFER_UPLOAD ? "[UP]" : "[DN]",
					clean_name, draw_transfer_state_text(t->state), done_s, total_s);

			baseline = kl_text_center(style->theme->text_small, row.y, row.height);
			if (style->text != NULL)
				(void)kl_text_draw_fit(style->text, style->canvas, row.x + 12, baseline, line,
					style->theme->text_small, 0, row.width - 96, ink);

			if (t->state == SFTP_TRANSFER_QUEUED || t->state == SFTP_TRANSFER_ACTIVE || t->state == SFTP_TRANSFER_WAITING_OVERWRITE) {
				btn.x = row.x + row.width - 76;
				btn.y = row.y + 2;
				btn.width = 64;
				btn.height = row.height - 4;
				if (kl_button(view->ui, style, SFTP_UI_TRANSFER_ITEM_BASE + (uint32_t)i, &btn, "Cancel", KL_BUTTON_QUIET))
					sftp_worker_cancel_transfer(view->worker, t->id);
			}
		}
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
	struct kl_rect area;
	int chosen;

	area.x = 0;
	area.y = 0;
	area.width = width;
	area.height = height;

	if (view->dialog == SFTP_DIALOG_OVERWRITE) {
		static const char *const labels[5] = {
			"Overwrite", "Skip", "Overwrite all", "Skip all", "Cancel"
		};

		chosen = kl_dialog(view->ui, style, SFTP_UI_DIALOG_BASE, &area, view->dialog_title, view->dialog_body, labels, 5);
		if (chosen >= 0) {
			static const enum sftp_conflict_action actions[5] = {
				SFTP_CONFLICT_OVERWRITE,
				SFTP_CONFLICT_SKIP,
				SFTP_CONFLICT_OVERWRITE_ALL,
				SFTP_CONFLICT_SKIP_ALL,
				SFTP_CONFLICT_CANCEL
			};

			sftp_worker_resolve_conflict(view->worker, view->dialog_transfer_id, actions[chosen]);
			view->dialog = SFTP_DIALOG_NONE;
			view->dirty = 1;
		}
	} else if (view->dialog == SFTP_DIALOG_DELETE) {
		static const char *const labels[2] = { "Delete", "Cancel" };

		chosen = kl_dialog(view->ui, style, SFTP_UI_DIALOG_BASE, &area, view->dialog_title, view->dialog_body, labels, 2);
		if (chosen == 0) {
			size_t i;
			char err[256];
			char clean[512];

			for (i = 0U; i < view->dialog_delete_count; i++) {
				err[0] = '\0';
				if (sftp_worker_delete(view->worker, view->dialog_delete_paths[i], err, sizeof(err)) != 0) {
					sftp_view_sanitize_text(err, clean, sizeof(clean));
					sftp_view_show_error(view, "Delete failed", clean);
					return;
				}
			}
			view->dialog = SFTP_DIALOG_NONE;
			sftp_view_refresh_all(view);
		} else if (chosen == 1) {
			view->dialog = SFTP_DIALOG_NONE;
			view->dirty = 1;
		}
	} else if (view->dialog == SFTP_DIALOG_NEW_FOLDER) {
		struct kl_rect box;
		struct kl_rect fld;
		struct kl_rect ok_btn;
		struct kl_rect cancel_btn;
		unsigned fres;
		int create_now;

		box.width = 380;
		box.height = 148;
		box.x = (width - box.width) / 2;
		box.y = (height - box.height) / 2;
		kl_canvas_fill(style->canvas, &area, KL_RGBA(0x000000U, 128U));
		kl_canvas_shadow(style->canvas, (float)box.x, (float)box.y, (float)box.width, (float)box.height,
			style->theme->card_radius, 18.0f, style->theme->shadow);
		kl_canvas_round(style->canvas, (float)box.x, (float)box.y, (float)box.width, (float)box.height,
			style->theme->card_radius, style->theme->card);
		kl_canvas_round_border(style->canvas, (float)box.x, (float)box.y, (float)box.width, (float)box.height,
			style->theme->card_radius, 1.0f, style->theme->card_edge);

		if (style->text != NULL) {
			(void)kl_text_draw(style->text, style->canvas, box.x + 16, box.y + 28,
				view->dialog_title, strlen(view->dialog_title), style->theme->text_title, 1, style->theme->text);
			(void)kl_text_draw_fit(style->text, style->canvas, box.x + 16, box.y + 52,
				view->dialog_body, style->theme->text_small, 0, box.width - 32, style->theme->text_secondary);
		}

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

		create_now = kl_button(view->ui, style, SFTP_UI_DIALOG_BASE, &ok_btn, "Create", KL_BUTTON_PRIMARY) ||
			     (fres & KL_FIELD_SUBMITTED) != 0U;
		if (create_now && view->dialog_field.text[0] != '\0') {
			char new_path[SSH_SFTP_PATH_MAX];
			char err[256];
			char clean[512];

			if (strchr(view->dialog_field.text, '/') != NULL) {
				sftp_view_show_error(view, "New folder failed", "Folder name cannot contain '/'.");
				return;
			}
			if (sftp_transfer_join_remote(new_path, sizeof(new_path), view->current_path, view->dialog_field.text) == 0) {
				err[0] = '\0';
				if (sftp_worker_mkdir(view->worker, new_path, err, sizeof(err)) != 0) {
					sftp_view_sanitize_text(err, clean, sizeof(clean));
					sftp_view_show_error(view, "New folder failed", clean);
					return;
				}
			}
			view->dialog = SFTP_DIALOG_NONE;
			sftp_view_refresh_all(view);
		} else if (kl_button(view->ui, style, SFTP_UI_DIALOG_BASE + 1U, &cancel_btn, "Cancel", 0U) ||
			   (fres & KL_FIELD_CANCELLED) != 0U) {
			view->dialog = SFTP_DIALOG_NONE;
			view->dirty = 1;
		}
	} else if (view->dialog == SFTP_DIALOG_ERROR) {
		static const char *const labels[1] = { "OK" };

		chosen = kl_dialog(view->ui, style, SFTP_UI_DIALOG_BASE, &area, view->dialog_title, view->dialog_body, labels, 1);
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
	int baseline;

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

	kl_canvas_shadow(style->canvas, (float)r.x, (float)r.y, (float)r.width, (float)r.height,
		style->theme->control_radius, 12.0f, style->theme->shadow);
	kl_canvas_round(style->canvas, (float)r.x, (float)r.y, (float)r.width, (float)r.height,
		style->theme->control_radius, style->theme->card);
	kl_canvas_round_border(style->canvas, (float)r.x, (float)r.y, (float)r.width, (float)r.height,
		style->theme->control_radius, 1.0f, style->theme->card_edge);

	for (i = 0U; i < view->ctx_count; i++) {
		struct kl_rect ir;
		kl_color ink;

		ir.x = r.x + 4;
		ir.y = r.y + 4 + (int)i * item_h;
		ir.width = menu_w - 8;
		ir.height = item_h;
		if ((int)i == view->ctx_hover && view->ctx_items[i].enabled) {
			kl_canvas_round(style->canvas, (float)ir.x, (float)ir.y, (float)ir.width, (float)ir.height,
				style->theme->control_radius, style->theme->selection);
		}
		ink = view->ctx_items[i].enabled ? style->theme->text : style->theme->text_faint;
		if (style->text != NULL) {
			baseline = kl_text_center(style->theme->text_body, ir.y, ir.height);
			(void)kl_text_draw(style->text, style->canvas, ir.x + 10, baseline,
				view->ctx_items[i].label, strlen(view->ctx_items[i].label),
				style->theme->text_body, 0, ink);
		}
	}
}

/* Returns human-readable text for a transfer state. */
static const char *
draw_transfer_state_text(
	enum sftp_transfer_state st)
{
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
