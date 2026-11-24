/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Input and drag-and-drop handling for the SFTP browser view tab:
 * keyboard shortcuts, multi-selection, rubber-band drag, splitter drag,
 * internal drag-move, context menu, and Explorer-to-mikuterm URI drops.
 */

#include "sftp-view.h"
#include "sftp-local.h"
#include "sftp-transfer.h"

#include "userland/desktop/terminal/terminal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Evdev key codes not defined in keiland_key.h. */
#define INPUT_KEY_A		30U
#define INPUT_KEY_L		38U
#define INPUT_KEY_F2		60U
#define INPUT_KEY_F5		63U

/* Evdev mouse button codes. */
#define INPUT_BTN_LEFT		0x110U
#define INPUT_BTN_RIGHT		0x111U

static int input_to_logical(const struct sftp_view *view, int px);
static int input_point_in_rect(const struct kl_rect *r, int x, int y);
static void input_handle_key(struct sftp_view *view, uint32_t key, unsigned mods);
static void input_handle_left_down(struct sftp_view *view, int ux, int uy);
static void input_handle_left_up(struct sftp_view *view, int ux, int uy);
static void input_handle_right_down(struct sftp_view *view, int ux, int uy);
static void input_handle_motion(struct sftp_view *view, int ux, int uy);
static void input_open_ctx_menu(struct sftp_view *view, int ux, int uy);
static void input_activate_row(struct sftp_view *view, size_t row_idx);
static void input_select_row(struct sftp_view *view, size_t row_idx, unsigned mods);
static void input_update_band_selection(struct sftp_view *view);
static void input_enqueue_uri_list(struct sftp_view *view, const char *data, size_t length, const char *target_dir);
static int input_uri_to_local_path(const char *uri, size_t len, char *out, size_t out_cap);

/*
 * Handles a keyboard, pointer, wheel, focus, or text event for the view.
 */
void
sftp_view_handle_input(
	struct sftp_view *view,
	const struct terminal_picture_event *event)
{
	int ux;
	int uy;

	if (view == NULL || event == NULL)
		return;

	ux = input_to_logical(view, event->x);
	uy = input_to_logical(view, event->y);
	view->modifiers = event->modifiers;

	if (view->ui != NULL) {
		switch (event->kind) {
		case TERMINAL_PICTURE_MOTION:
			(void)kl_ui_pointer_motion(view->ui, (double)ux, (double)uy);
			break;
		case TERMINAL_PICTURE_LEAVE:
			(void)kl_ui_pointer_leave(view->ui);
			break;
		case TERMINAL_PICTURE_BUTTON:
			(void)kl_ui_pointer_motion(view->ui, (double)ux, (double)uy);
			if (event->code == INPUT_BTN_LEFT)
				(void)kl_ui_pointer_button(view->ui, event->pressed, kl_clock_us());
			break;
		case TERMINAL_PICTURE_AXIS:
			(void)kl_ui_wheel(view->ui, event->dx, event->dy, kl_clock_us());
			break;
		case TERMINAL_PICTURE_KEY:
			(void)kl_ui_key(view->ui, event->code, event->pressed, event->modifiers);
			break;
		case TERMINAL_PICTURE_TEXT:
		{
			struct kl_window_event wev;

			memset(&wev, 0, sizeof(wev));
			wev.kind = KL_WINDOW_TEXT_COMMIT;
			(void)snprintf(wev.text, sizeof(wev.text), "%s", event->text);
			(void)kl_ui_text(view->ui, &wev);
			break;
		}
		default:
			break;
		}
	}

	switch (event->kind) {
	case TERMINAL_PICTURE_MOTION:
		view->pointer_x = ux;
		view->pointer_y = uy;
		input_handle_motion(view, ux, uy);
		break;
	case TERMINAL_PICTURE_BUTTON:
		view->pointer_x = ux;
		view->pointer_y = uy;
		if (event->code == INPUT_BTN_LEFT) {
			if (event->pressed)
				input_handle_left_down(view, ux, uy);
			else
				input_handle_left_up(view, ux, uy);
		} else if (event->code == INPUT_BTN_RIGHT && event->pressed) {
			input_handle_right_down(view, ux, uy);
		}
		break;
	case TERMINAL_PICTURE_KEY:
		if (event->pressed)
			input_handle_key(view, event->code, event->modifiers);
		break;
	case TERMINAL_PICTURE_AXIS:
		view->dirty = 1;
		break;
	default:
		break;
	}
	view->dirty = 1;
}

/*
 * Handles drag-and-drop events (URI upload from Explorer) for the view.
 */
unsigned
sftp_view_handle_drop(
	struct sftp_view *view,
	struct terminal_window *window,
	const struct terminal_drop_event *event)
{
	int ux;
	int uy;
	char target[SSH_SFTP_PATH_MAX];
	char *data;
	size_t length;
	unsigned type;
	int error;

	if (view == NULL || event == NULL)
		return 0U;

	ux = input_to_logical(view, event->x);
	uy = input_to_logical(view, event->y);

	switch (event->kind) {
	case TERMINAL_DROP_ENTER:
	case TERMINAL_DROP_MOTION:
		if ((event->types & KL_DROP_URIS) == 0U || view->worker_state != SFTP_WORKER_READY) {
			view->drop_target_path[0] = '\0';
			view->ext_drop_active = 0;
			view->dirty = 1;
			return 0U;
		}
		view->ext_drop_active = 1;
		if (!sftp_view_hit_folder_at(view, ux, uy, target, sizeof(target)))
			(void)snprintf(target, sizeof(target), "%s", view->current_path);
		(void)snprintf(view->drop_target_path, sizeof(view->drop_target_path), "%s", target);
		view->dirty = 1;
		return KL_DND_COPY;

	case TERMINAL_DROP_LEAVE:
		view->ext_drop_active = 0;
		view->drop_target_path[0] = '\0';
		view->dirty = 1;
		return 0U;

	case TERMINAL_DROP_DROP:
		view->ext_drop_active = 0;
		if (!sftp_view_hit_folder_at(view, ux, uy, target, sizeof(target)))
			(void)snprintf(target, sizeof(target), "%s", view->current_path);
		view->drop_target_path[0] = '\0';
		if (window == NULL || window->kui == NULL || (event->types & KL_DROP_URIS) == 0U) {
			if (window != NULL && window->kui != NULL)
				kl_window_finish_drop(window->kui, 0U);
			return 0U;
		}
		data = NULL;
		length = 0U;
		type = 0U;
		error = kl_window_receive_drop(window->kui, &data, &length, &type);
		if (error == E2BIG) {
			kl_window_finish_drop(window->kui, 0U);
			sftp_view_show_error(view, "Drop failed", "too many files; drop fewer at once");
			return 0U;
		}
		if (error != 0 || data == NULL || (type & KL_DROP_URIS) == 0U) {
			free(data);
			kl_window_finish_drop(window->kui, 0U);
			return 0U;
		}
		input_enqueue_uri_list(view, data, length, target);
		free(data);
		kl_window_finish_drop(window->kui, KL_DND_COPY);
		view->dirty = 1;
		return KL_DND_COPY;

	default:
		return 0U;
	}
}

/*
 * Hit-tests (ux, uy) against tree folder nodes and list folder rows,
 * returning 1 and filling path_out if a folder is under the pointer.
 */
int
sftp_view_hit_folder_at(
	struct sftp_view *view,
	int ux,
	int uy,
	char *path_out,
	size_t path_cap)
{
	int idx;

	int rh;

	if (view == NULL || path_out == NULL || path_cap == 0U)
		return 0;
	rh = kl_theme_default()->row_height;
	if (rh <= 0)
		rh = SFTP_ROW_HEIGHT;

	if (input_point_in_rect(&view->rect_tree, ux, uy)) {
		idx = (uy - view->rect_tree.y + (int)view->tree_list.scroll.y) / rh;
		if (idx >= 0 && (size_t)idx < view->tree_flat_count) {
			(void)snprintf(path_out, path_cap, "%s", view->tree_flat[idx]->path);
			return 1;
		}
	}
	if (input_point_in_rect(&view->rect_list, ux, uy)) {
		idx = (uy - view->rect_list.y + (int)view->file_list.scroll.y) / rh;
		if (idx >= 0 && (size_t)idx < view->row_count &&
		    (view->rows[idx].entry.is_dir || view->rows[idx].entry.link_is_dir)) {
			return sftp_transfer_join_remote(path_out, path_cap, view->current_path, view->rows[idx].entry.name) == 0;
		}
	}
	return 0;
}

/* Converts pixel coordinates to logical units. */
static int
input_to_logical(
	const struct sftp_view *view,
	int px)
{
	unsigned sc;

	sc = view->scale != 0U ? view->scale : KL_SCALE_BASE;
	return (int)(((int64_t)px * (int64_t)KL_SCALE_BASE) / (int64_t)sc);
}

/* Returns 1 if (x, y) lies inside r. */
static int
input_point_in_rect(
	const struct kl_rect *r,
	int x,
	int y)
{
	return x >= r->x && x < r->x + r->width && y >= r->y && y < r->y + r->height;
}

/* Handles keyboard shortcuts when no text field owns the key. */
static void
input_handle_key(
	struct sftp_view *view,
	uint32_t key,
	unsigned mods)
{
	size_t i;

	if (view->ctx_open) {
		if (key == KL_KEY_ESC)
			view->ctx_open = 0;
		return;
	}
	if (view->dialog != SFTP_DIALOG_NONE || view->renaming || view->editing_path)
		return;

	if (key == INPUT_KEY_F5) {
		sftp_view_refresh_all(view);
		return;
	}
	if (key == INPUT_KEY_F2) {
		sftp_view_begin_rename(view);
		return;
	}
	if (key == KL_KEY_DELETE) {
		sftp_view_begin_delete(view);
		return;
	}
	if (key == KL_KEY_BACKSPACE || (key == KL_KEY_UP && (mods & KL_MOD_ALT) != 0U)) {
		sftp_view_navigate_up(view);
		return;
	}
	if (key == KL_KEY_LEFT && (mods & KL_MOD_ALT) != 0U) {
		sftp_view_navigate_back(view);
		return;
	}
	if (key == KL_KEY_RIGHT && (mods & KL_MOD_ALT) != 0U) {
		sftp_view_navigate_forward(view);
		return;
	}
	if (key == INPUT_KEY_A && (mods & KL_MOD_CTRL) != 0U) {
		sftp_view_select_all(view);
		return;
	}
	if (key == INPUT_KEY_L && (mods & KL_MOD_CTRL) != 0U) {
		view->editing_path = 1;
		memset(&view->path_field, 0, sizeof(view->path_field));
		kl_field_set(&view->path_field, view->current_path);
		if (view->ui != NULL)
			kl_ui_set_focus(view->ui, SFTP_UI_PATH_FIELD, 0U);
		return;
	}
	if (key == KL_KEY_ENTER || key == KL_KEY_KPENTER) {
		if (view->focus_row >= 0 && (size_t)view->focus_row < view->row_count)
			input_activate_row(view, (size_t)view->focus_row);
		return;
	}
	if (key == KL_KEY_UP && view->row_count > 0U) {
		long next;

		next = view->focus_row > 0 ? view->focus_row - 1 : 0;
		for (i = 0U; i < view->row_count; i++)
			view->rows[i].selected = 0;
		view->rows[next].selected = 1;
		view->focus_row = next;
		view->anchor_row = next;
		return;
	}
	if (key == KL_KEY_DOWN && view->row_count > 0U) {
		long next;

		next = view->focus_row + 1 < (long)view->row_count ? view->focus_row + 1 : (long)view->row_count - 1;
		for (i = 0U; i < view->row_count; i++)
			view->rows[i].selected = 0;
		view->rows[next].selected = 1;
		view->focus_row = next;
		view->anchor_row = next;
		return;
	}
}

/* Handles left mouse button press. */
static void
input_handle_left_down(
	struct sftp_view *view,
	int ux,
	int uy)
{
	static uint64_t last_click_us;
	static long last_click_row = -1;
	uint64_t now_us;
	int idx;

	if (view->ctx_open) {
		if (input_point_in_rect(&view->rect_ctx, ux, uy) &&
		    view->ctx_hover >= 0 && (size_t)view->ctx_hover < view->ctx_count &&
		    view->ctx_items[view->ctx_hover].enabled) {
			sftp_view_execute_ctx(view, view->ctx_items[view->ctx_hover].id);
		} else {
			view->ctx_open = 0;
		}
		return;
	}
	if (view->dialog != SFTP_DIALOG_NONE)
		return;

	/* Breadcrumb click enters path editing mode. */
	if (!view->editing_path && input_point_in_rect(&view->rect_breadcrumb, ux, uy)) {
		view->editing_path = 1;
		memset(&view->path_field, 0, sizeof(view->path_field));
		kl_field_set(&view->path_field, view->current_path);
		if (view->ui != NULL)
			kl_ui_set_focus(view->ui, SFTP_UI_PATH_FIELD, 0U);
		return;
	}
	if (view->editing_path && !input_point_in_rect(&view->rect_breadcrumb, ux, uy))
		view->editing_path = 0;

	/* Splitter drag. */
	if (input_point_in_rect(&view->rect_splitter, ux, uy)) {
		view->dragging_splitter = 1;
		view->split_drag_start_x = ux;
		view->split_drag_start_w = view->split_width;
		return;
	}

	/* Column header click for sorting. */
	if (input_point_in_rect(&view->rect_header, ux, uy)) {
		int rel_x;
		int perm_w;
		int mtime_w;
		int size_w;
		int name_w;
		enum sftp_sort_column col;

		rel_x = ux - view->rect_header.x - 8;
		perm_w = 96;
		mtime_w = 136;
		size_w = 84;
		name_w = view->rect_header.width - perm_w - mtime_w - size_w - 16;
		if (name_w < 100)
			name_w = 100;
		if (rel_x < name_w)
			col = SFTP_SORT_NAME;
		else if (rel_x < name_w + size_w)
			col = SFTP_SORT_SIZE;
		else if (rel_x < name_w + size_w + mtime_w)
			col = SFTP_SORT_MTIME;
		else
			return;
		if (view->sort_column == col)
			view->sort_reverse = !view->sort_reverse;
		else {
			view->sort_column = col;
			view->sort_reverse = 0;
		}
		sftp_view_sort_rows(view);
		return;
	}

	/* Left tree click. */
	if (input_point_in_rect(&view->rect_tree, ux, uy)) {
		int rh;

		rh = kl_theme_default()->row_height;
		if (rh <= 0)
			rh = SFTP_TREE_ROW_HEIGHT;
		idx = (uy - view->rect_tree.y + (int)view->tree_list.scroll.y) / rh;
		if (idx >= 0 && (size_t)idx < view->tree_flat_count) {
			struct sftp_tree_node *node;
			int chev_x;

			node = view->tree_flat[idx];
			chev_x = view->rect_tree.x + 8 + node->depth * 14;
			if (ux >= chev_x - 4 && ux <= chev_x + 14)
				sftp_view_toggle_tree_node(view, node);
			else
				sftp_view_navigate(view, node->path, 1, 0);
		}
		return;
	}

	/* Right file list click. */
	if (input_point_in_rect(&view->rect_list, ux, uy)) {
		int rh;

		rh = kl_theme_default()->row_height;
		if (rh <= 0)
			rh = SFTP_ROW_HEIGHT;
		idx = (uy - view->rect_list.y + (int)view->file_list.scroll.y) / rh;
		if (idx >= 0 && (size_t)idx < view->row_count) {
			now_us = kl_clock_us();
			if (last_click_row == idx && now_us - last_click_us < 400000ULL &&
			    (view->modifiers & (KL_MOD_CTRL | KL_MOD_SHIFT)) == 0U) {
				last_click_row = -1;
				input_activate_row(view, (size_t)idx);
				return;
			}
			last_click_row = idx;
			last_click_us = now_us;
			if (!view->rows[idx].selected || (view->modifiers & (KL_MOD_CTRL | KL_MOD_SHIFT)) != 0U)
				input_select_row(view, (size_t)idx, view->modifiers);
			view->drag_pending = 1;
			view->drag_start_x = ux;
			view->drag_start_y = uy;
		} else {
			size_t i;

			for (i = 0U; i < view->row_count; i++)
				view->rows[i].selected = 0;
			view->band_active = 1;
			view->band_x0 = ux;
			view->band_y0 = uy;
			view->band_x1 = ux;
			view->band_y1 = uy;
		}
	}
}

/* Handles left mouse button release. */
static void
input_handle_left_up(
	struct sftp_view *view,
	int ux,
	int uy)
{
	if (view->dragging_splitter) {
		view->dragging_splitter = 0;
		sftp_view_save_split(view);
		return;
	}
	if (view->band_active) {
		view->band_active = 0;
		return;
	}
	if (view->drag_active) {
		char target[SSH_SFTP_PATH_MAX];

		view->drag_active = 0;
		view->drag_pending = 0;
		if (sftp_view_hit_folder_at(view, ux, uy, target, sizeof(target)))
			sftp_view_move_selected_to(view, target);
		view->drop_target_path[0] = '\0';
		return;
	}
	view->drag_pending = 0;
}

/* Handles right mouse button press to open the context menu. */
static void
input_handle_right_down(
	struct sftp_view *view,
	int ux,
	int uy)
{
	int idx;

	if (view->dialog != SFTP_DIALOG_NONE)
		return;
	if (input_point_in_rect(&view->rect_list, ux, uy)) {
		int rh;

		rh = kl_theme_default()->row_height;
		if (rh <= 0)
			rh = SFTP_ROW_HEIGHT;
		idx = (uy - view->rect_list.y + (int)view->file_list.scroll.y) / rh;
		if (idx >= 0 && (size_t)idx < view->row_count && !view->rows[idx].selected)
			input_select_row(view, (size_t)idx, 0U);
	}
	input_open_ctx_menu(view, ux, uy);
}

/* Handles pointer motion for splitter drag, rubber band, internal drag, and context menu hover. */
static void
input_handle_motion(
	struct sftp_view *view,
	int ux,
	int uy)
{
	if (view->dragging_splitter) {
		int nw;

		nw = view->split_drag_start_w + (ux - view->split_drag_start_x);
		if (nw < SFTP_SPLIT_MIN)
			nw = SFTP_SPLIT_MIN;
		if (nw > view->width - 180)
			nw = view->width - 180;
		view->split_width = nw;
		return;
	}
	if (view->band_active) {
		view->band_x1 = ux;
		view->band_y1 = uy;
		input_update_band_selection(view);
		return;
	}
	if (view->drag_pending && !view->drag_active) {
		if (abs(ux - view->drag_start_x) > 5 || abs(uy - view->drag_start_y) > 5)
			view->drag_active = 1;
	}
	if (view->drag_active) {
		char target[SSH_SFTP_PATH_MAX];

		if (sftp_view_hit_folder_at(view, ux, uy, target, sizeof(target)))
			(void)snprintf(view->drop_target_path, sizeof(view->drop_target_path), "%s", target);
		else
			view->drop_target_path[0] = '\0';
		return;
	}
	if (view->ctx_open) {
		if (input_point_in_rect(&view->rect_ctx, ux, uy)) {
			int idx;

			idx = (uy - view->rect_ctx.y - 4) / 26;
			view->ctx_hover = (idx >= 0 && (size_t)idx < view->ctx_count) ? idx : -1;
		} else {
			view->ctx_hover = -1;
		}
	}
}

/* Populates and opens the context menu at (ux, uy). */
static void
input_open_ctx_menu(
	struct sftp_view *view,
	int ux,
	int uy)
{
	size_t sel;
	size_t i;
	int ready;

	sel = 0U;
	for (i = 0U; i < view->row_count; i++) {
		if (view->rows[i].selected)
			sel++;
	}
	ready = view->worker_state == SFTP_WORKER_READY;

	view->ctx_count = 0U;
	view->ctx_items[view->ctx_count].id = SFTP_CTX_DOWNLOAD;
	view->ctx_items[view->ctx_count].label = "Download";
	view->ctx_items[view->ctx_count].enabled = ready && sel > 0U;
	view->ctx_count++;

	view->ctx_items[view->ctx_count].id = SFTP_CTX_NEW_FOLDER;
	view->ctx_items[view->ctx_count].label = "New folder";
	view->ctx_items[view->ctx_count].enabled = ready;
	view->ctx_count++;

	view->ctx_items[view->ctx_count].id = SFTP_CTX_RENAME;
	view->ctx_items[view->ctx_count].label = "Rename";
	view->ctx_items[view->ctx_count].enabled = ready && sel == 1U;
	view->ctx_count++;

	view->ctx_items[view->ctx_count].id = SFTP_CTX_DELETE;
	view->ctx_items[view->ctx_count].label = "Delete";
	view->ctx_items[view->ctx_count].enabled = ready && sel > 0U;
	view->ctx_count++;

	view->ctx_items[view->ctx_count].id = SFTP_CTX_COPY_PATH;
	view->ctx_items[view->ctx_count].label = "Copy path";
	view->ctx_items[view->ctx_count].enabled = view->current_path[0] != '\0';
	view->ctx_count++;

	view->ctx_items[view->ctx_count].id = SFTP_CTX_REFRESH;
	view->ctx_items[view->ctx_count].label = "Refresh";
	view->ctx_items[view->ctx_count].enabled = ready;
	view->ctx_count++;

	view->ctx_items[view->ctx_count].id = SFTP_CTX_TOGGLE_HIDDEN;
	view->ctx_items[view->ctx_count].label = view->show_hidden ? "Hide dot-files" : "Show dot-files";
	view->ctx_items[view->ctx_count].enabled = ready;
	view->ctx_count++;

	view->ctx_x = ux;
	view->ctx_y = uy;
	view->ctx_hover = -1;
	view->ctx_open = 1;
}

/* Activates a row (enters folder or downloads file). */
static void
input_activate_row(
	struct sftp_view *view,
	size_t row_idx)
{
	const struct sftp_list_row *r;
	char next[SSH_SFTP_PATH_MAX];

	if (row_idx >= view->row_count)
		return;
	r = &view->rows[row_idx];
	if (r->entry.is_dir || r->entry.link_is_dir) {
		if (sftp_transfer_join_remote(next, sizeof(next), view->current_path, r->entry.name) == 0)
			sftp_view_navigate(view, next, 1, 0);
	}
}

/* Updates row selection on click with optional Ctrl/Shift modifiers. */
static void
input_select_row(
	struct sftp_view *view,
	size_t row_idx,
	unsigned mods)
{
	size_t i;

	if (row_idx >= view->row_count)
		return;
	if ((mods & KL_MOD_SHIFT) != 0U && view->anchor_row >= 0 && (size_t)view->anchor_row < view->row_count) {
		size_t lo;
		size_t hi;

		lo = (size_t)view->anchor_row < row_idx ? (size_t)view->anchor_row : row_idx;
		hi = (size_t)view->anchor_row > row_idx ? (size_t)view->anchor_row : row_idx;
		for (i = 0U; i < view->row_count; i++)
			view->rows[i].selected = (i >= lo && i <= hi);
		view->focus_row = (long)row_idx;
		return;
	}
	if ((mods & KL_MOD_CTRL) != 0U) {
		view->rows[row_idx].selected = !view->rows[row_idx].selected;
		view->focus_row = (long)row_idx;
		view->anchor_row = (long)row_idx;
		return;
	}
	for (i = 0U; i < view->row_count; i++)
		view->rows[i].selected = (i == row_idx);
	view->focus_row = (long)row_idx;
	view->anchor_row = (long)row_idx;
}

/* Updates row selection from the active rubber-band rectangle. */
static void
input_update_band_selection(
	struct sftp_view *view)
{
	int top;
	int bottom;
	int rh;
	size_t i;

	rh = kl_theme_default()->row_height;
	if (rh <= 0)
		rh = SFTP_ROW_HEIGHT;
	top = view->band_y0 < view->band_y1 ? view->band_y0 : view->band_y1;
	bottom = view->band_y0 > view->band_y1 ? view->band_y0 : view->band_y1;
	for (i = 0U; i < view->row_count; i++) {
		int ry;

		ry = view->rect_list.y + (int)i * rh - (int)view->file_list.scroll.y;
		view->rows[i].selected = (ry + rh > top && ry < bottom);
	}
}

/* Parses a text/uri-list payload and enqueues uploads to target_dir. */
static void
input_enqueue_uri_list(
	struct sftp_view *view,
	const char *data,
	size_t length,
	const char *target_dir)
{
	size_t pos;
	char local_path[MIKUTERM_PATH_MAX];
	struct sftp_local_entry st;

	pos = 0U;
	while (pos < length) {
		size_t end;
		size_t line_len;

		end = pos;
		while (end < length && data[end] != '\n' && data[end] != '\r')
			end++;
		line_len = end - pos;
		if (line_len > 0U && data[pos] != '#') {
			if (input_uri_to_local_path(data + pos, line_len, local_path, sizeof(local_path)) == 0) {
				if (sftp_local_stat(local_path, &st) == 0)
					(void)sftp_worker_enqueue_upload(view->worker, local_path, target_dir, st.is_dir, NULL);
			}
		}
		pos = end;
		while (pos < length && (data[pos] == '\n' || data[pos] == '\r'))
			pos++;
	}
}

/* Decodes a file:// URI into a Windows local path. */
static int
input_uri_to_local_path(
	const char *uri,
	size_t len,
	char *out,
	size_t out_cap)
{
	size_t i;
	size_t pos;
	const char *p;
	size_t rem;

	if (len < 7U || strncmp(uri, "file://", 7U) != 0)
		return EINVAL;
	p = uri + 7U;
	rem = len - 7U;
	/* file:///C:/... -> skip leading '/' before drive letter. */
	if (rem >= 3U && p[0] == '/' && p[2] == ':') {
		p++;
		rem--;
	} else if (rem > 0U && p[0] != '/') {
		/* UNC path file://server/share -> \\server\share */
		if (out_cap < 3U)
			return ENAMETOOLONG;
		out[0] = '\\';
		out[1] = '\\';
		pos = 2U;
		goto decode;
	}
	pos = 0U;

decode:
	for (i = 0U; i < rem && pos + 1U < out_cap; i++) {
		unsigned char ch;

		if (p[i] == '%' && i + 2U < rem) {
			unsigned hi;
			unsigned lo;
			int ok;

			ok = 1;
			if (p[i + 1U] >= '0' && p[i + 1U] <= '9')
				hi = (unsigned)(p[i + 1U] - '0');
			else if (p[i + 1U] >= 'a' && p[i + 1U] <= 'f')
				hi = (unsigned)(p[i + 1U] - 'a' + 10);
			else if (p[i + 1U] >= 'A' && p[i + 1U] <= 'F')
				hi = (unsigned)(p[i + 1U] - 'A' + 10);
			else
				ok = 0;
			if (p[i + 2U] >= '0' && p[i + 2U] <= '9')
				lo = (unsigned)(p[i + 2U] - '0');
			else if (p[i + 2U] >= 'a' && p[i + 2U] <= 'f')
				lo = (unsigned)(p[i + 2U] - 'a' + 10);
			else if (p[i + 2U] >= 'A' && p[i + 2U] <= 'F')
				lo = (unsigned)(p[i + 2U] - 'A' + 10);
			else
				ok = 0;
			if (ok) {
				ch = (unsigned char)((hi << 4) | lo);
				i += 2U;
			} else {
				ch = (unsigned char)p[i];
			}
		} else {
			ch = (unsigned char)p[i];
		}
		if (ch == '/')
			ch = '\\';
		out[pos++] = (char)ch;
	}
	out[pos] = '\0';
	return 0;
}
