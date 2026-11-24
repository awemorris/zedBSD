/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * SFTP browser view tab: lifecycle, worker integration, directory tree
 * and listing management, transfer actions, U8 text sanitization, and
 * terminal_view_ops wiring.
 */

#include "sftp-view.h"
#include "sftp-local.h"
#include "sftp-transfer.h"

#include "userland/desktop/terminal/terminal.h"

#include <windows.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Next post number assigned to an SFTP view tab. */
static uint32_t view_next_number = 1U;

static size_t view_conn_read(struct terminal_connection *connection, unsigned char *buffer, size_t size);
static int view_conn_write(struct terminal_connection *connection, const unsigned char *bytes, size_t size, size_t *written);
static int view_conn_resize(struct terminal_connection *connection, unsigned columns, unsigned rows, unsigned width, unsigned height);
static int view_conn_mode(const struct terminal_connection *connection, unsigned *mode);
static unsigned view_conn_event(struct terminal_connection *connection, const struct kl_app_event *event);
static unsigned view_conn_pending(const struct terminal_connection *connection);
static void view_conn_close(struct terminal_connection *connection);
static int view_op_draw(struct terminal_connection *connection, const struct terminal_view_state *state, uint32_t *pixels, uint32_t width, uint32_t height);
static void view_op_title(struct terminal_connection *connection, char *title, size_t size);
static void view_op_input(struct terminal_connection *connection, const struct terminal_picture_event *event);
static unsigned view_op_drop(struct terminal_connection *connection, struct terminal_window *window, const struct terminal_drop_event *event);
static void view_op_shown(struct terminal_connection *connection, struct terminal_window *window, int visible);
static int view_op_confirm_close(struct terminal_connection *connection, struct terminal_window *window);
static int view_op_can_copy(const struct terminal_connection *connection);
static size_t view_op_copy(struct terminal_connection *connection, char *text, size_t size);
static int view_op_can_paste(const struct terminal_connection *connection);
static void view_op_paste(struct terminal_connection *connection, const char *text, size_t length);
static void view_op_refuse(struct terminal_connection *connection);
static void view_worker_notify(void *data);
static void view_poll_worker(struct sftp_view *view);
static void view_free_tree(struct sftp_tree_node *node);
static struct sftp_tree_node *view_alloc_node(const char *name, const char *path, int depth, int is_link, struct sftp_tree_node *parent);
static void view_populate_tree_children(struct sftp_view *view, struct sftp_tree_node *node);
static void view_append_flat(struct sftp_view *view, struct sftp_tree_node *node);
static int view_compare_rows(const void *a, const void *b);
static void view_history_push(struct sftp_history *hist, const char *path);
static void view_history_free(struct sftp_history *hist);
static size_t view_decode_utf8(const unsigned char *s, size_t len, uint32_t *cp);
static int view_is_bidi_control(uint32_t cp);

/* Thread-local sort state used by view_compare_rows. */
static enum sftp_sort_column view_sort_col;
static int view_sort_rev;

/* View operations table registered on the connection. */
static const struct terminal_view_ops view_ops = {
	view_op_draw,
	view_op_title,
	view_op_input,
	view_op_drop,
	view_op_shown,
	view_op_confirm_close,
	view_op_can_copy,
	view_op_copy,
	view_op_can_paste,
	view_op_paste,
	view_op_refuse
};

/* Connection operations table for an SFTP view tab. */
static const struct terminal_connection_ops view_conn_ops = {
	view_conn_read,
	view_conn_write,
	view_conn_resize,
	view_conn_mode,
	view_conn_event,
	view_conn_pending,
	view_conn_close,
	NULL,
	NULL,
	&view_ops
};

/*
 * Creates an SFTP browser view tab connection and starts its worker
 * thread on link.
 */
int
sftp_view_create(
	struct kl_app *app,
	struct mikuterm_session *session,
	struct sftp_link *link,
	const char *host,
	int preserve_times,
	int show_hidden,
	const char *font_path,
	const char *fallback_path,
	struct terminal_connection **connection_out)
{
	struct sftp_view *view;
	int error;

	if (app == NULL || link == NULL || connection_out == NULL)
		return EINVAL;

	view = calloc(1U, sizeof(*view));
	if (view == NULL)
		return ENOMEM;

	view->connection.ops = &view_conn_ops;
	view->app = app;
	view->session = session;
	view->link = link;
	sftp_link_retain(link);
	view->number = view_next_number++;
	view->show_hidden = show_hidden ? 1 : 0;
	view->split_width = sftp_view_load_split();
	view->focus_row = -1;
	view->anchor_row = -1;
	view->ctx_hover = -1;
	view->scale = KL_SCALE_BASE;
	if (host != NULL && host[0] != '\0') {
		(void)snprintf(view->host, sizeof(view->host), "%s", host);
		(void)snprintf(view->title_text, sizeof(view->title_text), "SFTP: %s", host);
	} else {
		(void)snprintf(view->host, sizeof(view->host), "SFTP");
		(void)snprintf(view->title_text, sizeof(view->title_text), "SFTP");
	}

	error = kl_mutex_init(&view->lock);
	if (error != 0) {
		sftp_link_release(link);
		free(view);
		return error;
	}

	view->ui = kl_ui_create();
	(void)kl_list_init(&view->tree_list);
	(void)kl_list_init(&view->file_list);
	(void)kl_list_init(&view->transfer_list);
	if (font_path != NULL && font_path[0] != '\0') {
		if (kl_text_open(&view->text, font_path, fallback_path) == 0)
			view->text_ready = 1;
	}

	view->tree_root = view_alloc_node("/", "/", 0, 0, NULL);
	if (view->tree_root != NULL) {
		view->tree_root->expanded = 1;
		sftp_view_rebuild_tree_flat(view);
	}

	error = sftp_worker_start(link, preserve_times, view_worker_notify, view, &view->worker);
	if (error != 0) {
		if (view->text_ready)
			kl_text_close(&view->text);
		kl_list_release(&view->tree_list);
		kl_list_release(&view->file_list);
		kl_list_release(&view->transfer_list);
		if (view->ui != NULL)
			kl_ui_destroy(view->ui);
		view_free_tree(view->tree_root);
		kl_mutex_destroy(&view->lock);
		sftp_link_release(link);
		free(view);
		return error;
	}

	view->dirty = 1;
	*connection_out = &view->connection;
	return 0;
}

/*
 * Asks the user for confirmation before closing the tab or window when
 * transfers are in progress; returns 1 to allow close, 0 to keep open.
 */
int
sftp_view_confirm_close(
	struct terminal_connection *connection,
	struct terminal_window *window)
{
	struct sftp_view *view;
	struct sftp_transfer_snapshot *items;
	size_t count;
	size_t active;
	wchar_t msg[256];
	int answer;

	(void)window;
	if (connection == NULL || connection->ops != &view_conn_ops)
		return 1;
	view = (struct sftp_view *)connection;
	if (view->worker == NULL)
		return 1;

	items = NULL;
	count = 0U;
	active = 0U;
	if (sftp_worker_transfers(view->worker, &items, &count, &active) == 0)
		sftp_worker_transfers_free(items);
	if (active == 0U)
		return 1;

	(void)swprintf(msg, sizeof(msg) / sizeof(msg[0]),
		L"%zu transfer%s in progress. Close and cancel them?",
		active, active == 1U ? L" is" : L"s are");
	answer = MessageBoxW(NULL, msg, L"mikuterm SFTP", MB_YESNO | MB_ICONWARNING);
	if (answer == IDYES) {
		sftp_worker_cancel_all(view->worker);
		return 1;
	}
	return 0;
}

/*
 * Sanitizes server-supplied text for display: invalid UTF-8 becomes
 * U+FFFD, C0/DEL/C1 control characters and bidirectional override
 * characters (U+202A..U+202E, U+2066..U+2069) become visible markers.
 */
void
sftp_view_sanitize_text(
	const char *raw,
	char *out,
	size_t out_cap)
{
	size_t i;
	size_t len;
	size_t pos;
	uint32_t cp;
	size_t step;
	char marker[16];
	size_t mlen;

	if (out == NULL || out_cap == 0U)
		return;
	out[0] = '\0';
	if (raw == NULL)
		return;

	len = strlen(raw);
	i = 0U;
	pos = 0U;
	while (i < len && pos + 1U < out_cap) {
		step = view_decode_utf8((const unsigned char *)raw + i, len - i, &cp);
		if (step == 0U) {
			/* Invalid UTF-8 byte -> U+FFFD (EF BF BD). */
			if (pos + 3U >= out_cap)
				break;
			out[pos++] = (char)0xefU;
			out[pos++] = (char)0xbfU;
			out[pos++] = (char)0xbdU;
			i++;
			continue;
		}
		if (cp < 0x20U || cp == 0x7fU || (cp >= 0x80U && cp <= 0x9fU)) {
			(void)snprintf(marker, sizeof(marker), "[x%02X]", (unsigned)cp);
			mlen = strlen(marker);
			if (pos + mlen >= out_cap)
				break;
			memcpy(out + pos, marker, mlen);
			pos += mlen;
		} else if (view_is_bidi_control(cp)) {
			(void)snprintf(marker, sizeof(marker), "[U+%04X]", (unsigned)cp);
			mlen = strlen(marker);
			if (pos + mlen >= out_cap)
				break;
			memcpy(out + pos, marker, mlen);
			pos += mlen;
		} else {
			if (pos + step >= out_cap)
				break;
			memcpy(out + pos, raw + i, step);
			pos += step;
		}
		i += step;
	}
	out[pos] = '\0';
}

/*
 * Marks the view dirty and posts a wakeup to the UI thread.
 */
void
sftp_view_request_redraw(
	struct sftp_view *view)
{
	view_worker_notify(view);
}

/*
 * Navigates the file list to path and refreshes the folder tree.
 */
void
sftp_view_navigate(
	struct sftp_view *view,
	const char *path,
	int record_history,
	int force_refresh)
{
	struct sftp_dir_listing *listing;
	size_t i;
	size_t cap;
	struct sftp_list_row *rows;

	if (view == NULL || view->worker == NULL || path == NULL || path[0] == '\0')
		return;

	listing = NULL;
	if (sftp_worker_list_dir(view->worker, path, force_refresh, &listing) != 0 || listing == NULL)
		return;

	if (listing->error != 0) {
		sftp_view_sanitize_text(listing->error_text, view->list_error, sizeof(view->list_error));
		sftp_dir_listing_free(listing);
		view->dirty = 1;
		return;
	}

	view->list_error[0] = '\0';
	view->list_truncated = listing->truncated;
	(void)snprintf(view->current_path, sizeof(view->current_path), "%s", listing->path);
	if (record_history)
		view_history_push(&view->history, view->current_path);

	free(view->rows);
	view->rows = NULL;
	view->row_count = 0U;
	view->focus_row = -1;
	view->anchor_row = -1;
	view->renaming = 0;

	cap = listing->count;
	if (cap > 0U) {
		rows = calloc(cap, sizeof(*rows));
		if (rows != NULL) {
			for (i = 0U; i < listing->count; i++) {
				const struct sftp_dir_entry *e;

				e = &listing->entries[i];
				if (!view->show_hidden && e->name[0] == '.')
					continue;
				rows[view->row_count].entry = *e;
				sftp_view_sanitize_text(e->name, rows[view->row_count].display_name, sizeof(rows[view->row_count].display_name));
				sftp_view_sanitize_text(e->link_target, rows[view->row_count].display_target, sizeof(rows[view->row_count].display_target));
				if (e->is_dir || e->link_is_dir)
					(void)snprintf(rows[view->row_count].size_text, sizeof(rows[view->row_count].size_text), "--");
				else
					sftp_view_format_size(e->size, rows[view->row_count].size_text, sizeof(rows[view->row_count].size_text));
				sftp_view_format_mtime(e->mtime, rows[view->row_count].mtime_text, sizeof(rows[view->row_count].mtime_text));
				sftp_view_format_perm(e->permissions, e->is_dir, e->is_link, rows[view->row_count].perm_text, sizeof(rows[view->row_count].perm_text));
				view->row_count++;
			}
			view->rows = rows;
			sftp_view_sort_rows(view);
		}
	}

	kl_scroll_move_to(&view->file_list.scroll, 0.0, 0.0, 0, kl_clock_us());
	sftp_dir_listing_free(listing);
	sftp_view_reveal_tree_path(view, view->current_path);
	view->dirty = 1;
}

/*
 * Navigates to the parent of current_path.
 */
void
sftp_view_navigate_up(
	struct sftp_view *view)
{
	char parent[SSH_SFTP_PATH_MAX];
	char *slash;

	if (view == NULL || view->current_path[0] == '\0')
		return;
	if (strcmp(view->current_path, "/") == 0)
		return;
	(void)snprintf(parent, sizeof(parent), "%s", view->current_path);
	slash = strrchr(parent, '/');
	if (slash == NULL || slash == parent)
		(void)snprintf(parent, sizeof(parent), "/");
	else
		*slash = '\0';
	sftp_view_navigate(view, parent, 1, 0);
}

/*
 * Navigates back in folder history.
 */
void
sftp_view_navigate_back(
	struct sftp_view *view)
{
	if (view == NULL || view->history.index == 0U || view->history.count == 0U)
		return;
	view->history.index--;
	sftp_view_navigate(view, view->history.paths[view->history.index], 0, 0);
}

/*
 * Navigates forward in folder history.
 */
void
sftp_view_navigate_forward(
	struct sftp_view *view)
{
	if (view == NULL || view->history.index + 1U >= view->history.count)
		return;
	view->history.index++;
	sftp_view_navigate(view, view->history.paths[view->history.index], 0, 0);
}

/*
 * Refreshes the current directory listing and tree.
 */
void
sftp_view_refresh_all(
	struct sftp_view *view)
{
	if (view == NULL || view->current_path[0] == '\0')
		return;
	sftp_view_navigate(view, view->current_path, 0, 1);
}

/*
 * Expands or collapses a folder tree node, loading its children if needed.
 */
void
sftp_view_toggle_tree_node(
	struct sftp_view *view,
	struct sftp_tree_node *node)
{
	if (view == NULL || node == NULL)
		return;
	if (node->expanded) {
		node->expanded = 0;
		sftp_view_rebuild_tree_flat(view);
		view->dirty = 1;
		return;
	}
	if (!node->loaded)
		view_populate_tree_children(view, node);
	node->expanded = 1;
	sftp_view_rebuild_tree_flat(view);
	view->dirty = 1;
}

/*
 * Expands the folder tree along path so every ancestor is visible.
 */
void
sftp_view_reveal_tree_path(
	struct sftp_view *view,
	const char *path)
{
	struct sftp_tree_node *cur;
	const char *p;
	const char *next;
	size_t seg_len;
	size_t i;
	struct sftp_tree_node *found;

	if (view == NULL || view->tree_root == NULL || path == NULL || path[0] != '/')
		return;

	cur = view->tree_root;
	if (!cur->loaded)
		view_populate_tree_children(view, cur);
	cur->expanded = 1;

	p = path + 1;
	while (*p != '\0') {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		next = strchr(p, '/');
		seg_len = next != NULL ? (size_t)(next - p) : strlen(p);
		if (!cur->loaded)
			view_populate_tree_children(view, cur);
		cur->expanded = 1;
		found = NULL;
		for (i = 0U; i < cur->child_count; i++) {
			if (strlen(cur->children[i]->name) == seg_len &&
			    strncmp(cur->children[i]->name, p, seg_len) == 0) {
				found = cur->children[i];
				break;
			}
		}
		if (found == NULL)
			break;
		cur = found;
		p += seg_len;
	}
	if (!cur->loaded)
		view_populate_tree_children(view, cur);
	cur->expanded = 1;
	sftp_view_rebuild_tree_flat(view);
}

/*
 * Rebuilds the flattened visible tree array used for drawing and hits.
 */
void
sftp_view_rebuild_tree_flat(
	struct sftp_view *view)
{
	free(view->tree_flat);
	view->tree_flat = NULL;
	view->tree_flat_count = 0U;
	if (view->tree_root != NULL)
		view_append_flat(view, view->tree_root);
}

/*
 * Sorts view->rows according to view->sort_column and view->sort_reverse.
 */
void
sftp_view_sort_rows(
	struct sftp_view *view)
{
	if (view == NULL || view->row_count < 2U)
		return;
	view_sort_col = view->sort_column;
	view_sort_rev = view->sort_reverse;
	qsort(view->rows, view->row_count, sizeof(view->rows[0]), view_compare_rows);
}

/*
 * Starts inline rename for the focused or first selected row.
 */
void
sftp_view_begin_rename(
	struct sftp_view *view)
{
	size_t i;
	long target;

	if (view == NULL || view->row_count == 0U)
		return;
	target = view->focus_row;
	if (target < 0 || (size_t)target >= view->row_count || !view->rows[target].selected) {
		target = -1;
		for (i = 0U; i < view->row_count; i++) {
			if (view->rows[i].selected) {
				target = (long)i;
				break;
			}
		}
	}
	if (target < 0)
		return;

	view->renaming = 1;
	view->rename_row = (size_t)target;
	memset(&view->rename_field, 0, sizeof(view->rename_field));
	kl_field_set(&view->rename_field, view->rows[target].entry.name);
	if (view->ui != NULL)
		kl_ui_set_focus(view->ui, SFTP_UI_RENAME_FIELD, 0U);
	view->dirty = 1;
}

/*
 * Commits an active inline rename via sftp_worker_rename.
 */
void
sftp_view_commit_rename(
	struct sftp_view *view)
{
	char old_path[SSH_SFTP_PATH_MAX];
	char new_path[SSH_SFTP_PATH_MAX];
	char err[256];
	char clean[512];

	if (view == NULL || !view->renaming || view->rename_row >= view->row_count)
		return;
	view->renaming = 0;
	if (view->rename_field.text[0] == '\0' ||
	    strcmp(view->rename_field.text, view->rows[view->rename_row].entry.name) == 0 ||
	    strchr(view->rename_field.text, '/') != NULL) {
		view->dirty = 1;
		return;
	}
	if (sftp_transfer_join_remote(old_path, sizeof(old_path), view->current_path, view->rows[view->rename_row].entry.name) != 0 ||
	    sftp_transfer_join_remote(new_path, sizeof(new_path), view->current_path, view->rename_field.text) != 0)
		return;

	err[0] = '\0';
	if (sftp_worker_rename(view->worker, old_path, new_path, err, sizeof(err)) != 0) {
		sftp_view_sanitize_text(err, clean, sizeof(clean));
		sftp_view_show_error(view, "Rename failed", clean);
		return;
	}
	sftp_view_refresh_all(view);
}

/*
 * Opens the New Folder modal dialog.
 */
void
sftp_view_begin_new_folder(
	struct sftp_view *view)
{
	if (view == NULL || view->worker_state != SFTP_WORKER_READY)
		return;
	view->dialog = SFTP_DIALOG_NEW_FOLDER;
	(void)snprintf(view->dialog_title, sizeof(view->dialog_title), "New folder");
	(void)snprintf(view->dialog_body, sizeof(view->dialog_body), "Folder name in %s:", view->current_path);
	memset(&view->dialog_field, 0, sizeof(view->dialog_field));
	if (view->ui != NULL)
		kl_ui_set_focus(view->ui, SFTP_UI_DIALOG_FIELD, 0U);
	view->dirty = 1;
}

/*
 * Counts selected items and opens the Delete confirmation dialog.
 */
void
sftp_view_begin_delete(
	struct sftp_view *view)
{
	size_t i;
	size_t total_items;
	size_t total_folders;
	int any_symlink;
	char full[SSH_SFTP_PATH_MAX];
	char **paths;
	size_t count;

	if (view == NULL || view->worker == NULL || view->row_count == 0U)
		return;
	paths = calloc(view->row_count, sizeof(*paths));
	if (paths == NULL)
		return;

	count = 0U;
	total_items = 0U;
	total_folders = 0U;
	any_symlink = 0;
	for (i = 0U; i < view->row_count; i++) {
		size_t items;
		size_t folders;
		int is_link;

		if (!view->rows[i].selected)
			continue;
		if (sftp_transfer_join_remote(full, sizeof(full), view->current_path, view->rows[i].entry.name) != 0)
			continue;
		paths[count] = strdup(full);
		if (paths[count] == NULL)
			continue;
		count++;
		items = 0U;
		folders = 0U;
		is_link = 0;
		if (sftp_worker_delete_count(view->worker, full, &items, &folders, &is_link) == 0) {
			total_items += items;
			total_folders += folders;
			if (is_link)
				any_symlink = 1;
		} else {
			total_items++;
		}
	}
	if (count == 0U) {
		free(paths);
		return;
	}

	for (i = 0U; i < view->dialog_delete_count; i++)
		free(view->dialog_delete_paths[i]);
	free(view->dialog_delete_paths);
	view->dialog_delete_paths = paths;
	view->dialog_delete_count = count;
	view->dialog = SFTP_DIALOG_DELETE;
	(void)snprintf(view->dialog_title, sizeof(view->dialog_title), "Confirm delete");
	(void)snprintf(view->dialog_body, sizeof(view->dialog_body),
		"Delete %zu item%s (%zu folder%s)?%s",
		total_items, total_items == 1U ? "" : "s",
		total_folders, total_folders == 1U ? "" : "s",
		any_symlink ? " Symbolic links will be unlinked without deleting their targets." : "");
	view->dirty = 1;
}

/*
 * Enqueues downloads for all selected items into the user's Downloads folder.
 */
void
sftp_view_download_selected(
	struct sftp_view *view)
{
	char downloads[MIKUTERM_PATH_MAX];
	char remote[SSH_SFTP_PATH_MAX];
	size_t i;

	if (view == NULL || view->worker == NULL)
		return;
	if (sftp_local_downloads_dir(downloads, sizeof(downloads)) != 0) {
		sftp_view_show_error(view, "Download failed", "Could not locate the Downloads folder.");
		return;
	}
	for (i = 0U; i < view->row_count; i++) {
		int is_dir;

		if (!view->rows[i].selected)
			continue;
		if (sftp_transfer_join_remote(remote, sizeof(remote), view->current_path, view->rows[i].entry.name) != 0)
			continue;
		is_dir = view->rows[i].entry.is_dir || view->rows[i].entry.link_is_dir;
		(void)sftp_worker_enqueue_download(view->worker, remote, downloads, is_dir, NULL);
	}
	view->dirty = 1;
}

/*
 * Selects all visible rows in the file list.
 */
void
sftp_view_select_all(
	struct sftp_view *view)
{
	size_t i;

	if (view == NULL)
		return;
	for (i = 0U; i < view->row_count; i++)
		view->rows[i].selected = 1;
	view->dirty = 1;
}

/*
 * Toggles display of hidden dot-files and refreshes the current listing.
 */
void
sftp_view_toggle_hidden(
	struct sftp_view *view)
{
	if (view == NULL)
		return;
	view->show_hidden = !view->show_hidden;
	sftp_view_navigate(view, view->current_path, 0, 0);
}

/*
 * Shows a modal error dialog with title and sanitized message.
 */
void
sftp_view_show_error(
	struct sftp_view *view,
	const char *title,
	const char *message)
{
	if (view == NULL)
		return;
	view->dialog = SFTP_DIALOG_ERROR;
	(void)snprintf(view->dialog_title, sizeof(view->dialog_title), "%s", title != NULL ? title : "Error");
	(void)snprintf(view->dialog_body, sizeof(view->dialog_body), "%s", message != NULL ? message : "Unknown error.");
	view->dirty = 1;
}

/*
 * Moves all selected rows into target_dir using SFTP RENAME.
 */
void
sftp_view_move_selected_to(
	struct sftp_view *view,
	const char *target_dir)
{
	char old_path[SSH_SFTP_PATH_MAX];
	char new_path[SSH_SFTP_PATH_MAX];
	char err[256];
	char clean[512];
	size_t i;
	int moved;

	if (view == NULL || view->worker == NULL || target_dir == NULL || target_dir[0] == '\0')
		return;
	if (strcmp(target_dir, view->current_path) == 0)
		return;

	moved = 0;
	for (i = 0U; i < view->row_count; i++) {
		if (!view->rows[i].selected)
			continue;
		if (sftp_transfer_join_remote(old_path, sizeof(old_path), view->current_path, view->rows[i].entry.name) != 0 ||
		    sftp_transfer_join_remote(new_path, sizeof(new_path), target_dir, view->rows[i].entry.name) != 0)
			continue;
		if (strcmp(old_path, target_dir) == 0)
			continue;
		err[0] = '\0';
		if (sftp_worker_rename(view->worker, old_path, new_path, err, sizeof(err)) != 0) {
			sftp_view_sanitize_text(err, clean, sizeof(clean));
			sftp_view_show_error(view, "Move failed", clean);
			return;
		}
		moved = 1;
	}
	if (moved)
		sftp_view_refresh_all(view);
}

/*
 * Executes a context menu action by ID.
 */
void
sftp_view_execute_ctx(
	struct sftp_view *view,
	int id)
{
	char copy[SSH_SFTP_PATH_MAX];
	size_t i;

	if (view == NULL)
		return;
	view->ctx_open = 0;
	switch (id) {
	case SFTP_CTX_DOWNLOAD:
		sftp_view_download_selected(view);
		break;
	case SFTP_CTX_NEW_FOLDER:
		sftp_view_begin_new_folder(view);
		break;
	case SFTP_CTX_RENAME:
		sftp_view_begin_rename(view);
		break;
	case SFTP_CTX_DELETE:
		sftp_view_begin_delete(view);
		break;
	case SFTP_CTX_COPY_PATH:
		copy[0] = '\0';
		for (i = 0U; i < view->row_count; i++) {
			if (view->rows[i].selected) {
				(void)sftp_transfer_join_remote(copy, sizeof(copy), view->current_path, view->rows[i].entry.name);
				break;
			}
		}
		if (copy[0] == '\0')
			(void)snprintf(copy, sizeof(copy), "%s", view->current_path);
		if (view->window != NULL)
			terminal_clipboard_set(view->window, copy, strlen(copy));
		break;
	case SFTP_CTX_REFRESH:
		sftp_view_refresh_all(view);
		break;
	case SFTP_CTX_TOGGLE_HIDDEN:
		sftp_view_toggle_hidden(view);
		break;
	default:
		break;
	}
}

/*
 * Saves the splitter width into config\state.conf.
 */
void
sftp_view_save_split(
	const struct sftp_view *view)
{
	char path[MIKUTERM_PATH_MAX];
	char value[32];
	char *text;
	char *made;
	size_t text_size;
	size_t made_size;
	int error;

	if (view == NULL)
		return;
	if (mikuterm_path(path, sizeof(path), "config/state.conf") != 0)
		return;
	text = NULL;
	text_size = 0U;
	error = mikuterm_file_read(path, MIKUTERM_CONF_MAX, &text, &text_size);
	if (error != 0 && error != ENOENT)
		return;
	(void)snprintf(value, sizeof(value), "%d", view->split_width);
	error = mikuterm_conf_set(text != NULL ? text : "", text_size, "SftpSplit", value, &made, &made_size);
	free(text);
	if (error == 0) {
		(void)mikuterm_file_replace(path, made, made_size);
		free(made);
	}
}

/*
 * Loads SftpSplit from config\state.conf, returning SFTP_SPLIT_DEFAULT
 * when absent or invalid.
 */
int
sftp_view_load_split(void)
{
	char path[MIKUTERM_PATH_MAX];
	char *text;
	size_t text_size;
	const char *p;
	const char *end;
	int val;

	if (mikuterm_path(path, sizeof(path), "config/state.conf") != 0)
		return SFTP_SPLIT_DEFAULT;
	text = NULL;
	text_size = 0U;
	if (mikuterm_file_read(path, MIKUTERM_CONF_MAX, &text, &text_size) != 0 || text == NULL)
		return SFTP_SPLIT_DEFAULT;
	val = SFTP_SPLIT_DEFAULT;
	p = text;
	end = text + text_size;
	while (p < end) {
		const char *eol;

		eol = memchr(p, '\n', (size_t)(end - p));
		if (eol == NULL)
			eol = end;
		if ((size_t)(eol - p) > 10U && strncmp(p, "SftpSplit ", 10U) == 0) {
			int parsed;

			parsed = atoi(p + 10);
			if (parsed >= SFTP_SPLIT_MIN && parsed <= 800)
				val = parsed;
			break;
		}
		p = eol + 1;
	}
	free(text);
	return val;
}

/*
 * Formats a byte count into a human-readable string.
 */
void
sftp_view_format_size(
	uint64_t bytes,
	char *out,
	size_t cap)
{
	if (bytes < 1024ULL) {
		(void)snprintf(out, cap, "%llu B", (unsigned long long)bytes);
	} else if (bytes < 1024ULL * 1024ULL) {
		(void)snprintf(out, cap, "%.1f KB", (double)bytes / 1024.0);
	} else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
		(void)snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
	} else {
		(void)snprintf(out, cap, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
	}
}

/*
 * Formats a Unix timestamp in local time as YYYY-MM-DD HH:MM.
 */
void
sftp_view_format_mtime(
	uint32_t mtime,
	char *out,
	size_t cap)
{
	time_t t;
	struct tm tm_val;
	struct tm *res;

	t = (time_t)mtime;
	res = localtime(&t);
	if (res == NULL) {
		(void)snprintf(out, cap, "--");
		return;
	}
	tm_val = *res;
	(void)snprintf(out, cap, "%04d-%02d-%02d %02d:%02d",
		tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
		tm_val.tm_hour, tm_val.tm_min);
}

/*
 * Formats Unix permission bits into an ls -l 10-character mode string.
 */
void
sftp_view_format_perm(
	uint32_t mode,
	int is_dir,
	int is_link,
	char *out,
	size_t cap)
{
	if (cap < 11U) {
		if (cap > 0U)
			out[0] = '\0';
		return;
	}
	out[0] = is_link ? 'l' : (is_dir ? 'd' : '-');
	out[1] = (mode & 0400U) ? 'r' : '-';
	out[2] = (mode & 0200U) ? 'w' : '-';
	out[3] = (mode & 0100U) ? 'x' : '-';
	out[4] = (mode & 0040U) ? 'r' : '-';
	out[5] = (mode & 0020U) ? 'w' : '-';
	out[6] = (mode & 0010U) ? 'x' : '-';
	out[7] = (mode & 0004U) ? 'r' : '-';
	out[8] = (mode & 0002U) ? 'w' : '-';
	out[9] = (mode & 0001U) ? 'x' : '-';
	out[10] = '\0';
}

/* Reads nothing; view tabs use view->draw instead. */
static size_t
view_conn_read(
	struct terminal_connection *connection,
	unsigned char *buffer,
	size_t size)
{
	(void)connection;
	(void)buffer;
	(void)size;
	return 0U;
}

/* Accepts no terminal byte writes. */
static int
view_conn_write(
	struct terminal_connection *connection,
	const unsigned char *bytes,
	size_t size,
	size_t *written)
{
	(void)connection;
	(void)bytes;
	(void)size;
	if (written != NULL)
		*written = 0U;
	return ENOTSUP;
}

/* Notes pixel resize for the view tab. */
static int
view_conn_resize(
	struct terminal_connection *connection,
	unsigned columns,
	unsigned rows,
	unsigned width,
	unsigned height)
{
	struct sftp_view *view;

	(void)columns;
	(void)rows;
	view = (struct sftp_view *)connection;
	view->width = (int)width;
	view->height = (int)height;
	view->dirty = 1;
	return 0;
}

/* Reports no terminal line mode. */
static int
view_conn_mode(
	const struct terminal_connection *connection,
	unsigned *mode)
{
	(void)connection;
	(void)mode;
	return ENOTSUP;
}

/* Dispatches wakeup posts from the SFTP worker thread. */
static unsigned
view_conn_event(
	struct terminal_connection *connection,
	const struct kl_app_event *event)
{
	struct sftp_view *view;

	view = (struct sftp_view *)connection;
	if (event->kind != KL_APP_POST || event->code != SFTP_VIEW_POST)
		return 0U;
	if ((uintptr_t)event->data != (uintptr_t)view->number)
		return 0U;

	kl_mutex_lock(&view->lock);
	view->posted = 0;
	kl_mutex_unlock(&view->lock);
	view_poll_worker(view);
	return TERMINAL_READY_OUTPUT;
}

/* Reports whether the view needs redrawing. */
static unsigned
view_conn_pending(
	const struct terminal_connection *connection)
{
	const struct sftp_view *view;
	int dirty;

	view = (const struct sftp_view *)connection;
	kl_mutex_lock((struct kl_mutex *)&view->lock);
	dirty = view->dirty;
	kl_mutex_unlock((struct kl_mutex *)&view->lock);
	return dirty ? TERMINAL_READY_OUTPUT : 0U;
}

/* Closes the SFTP view tab, stopping its worker and releasing resources. */
static void
view_conn_close(
	struct terminal_connection *connection)
{
	struct sftp_view *view;
	size_t i;

	view = (struct sftp_view *)connection;
	view->closed = 1;
	if (view->session != NULL) {
		mikuterm_session_detach_sftp(view->session, connection);
		view->session = NULL;
	}
	if (view->worker != NULL) {
		sftp_worker_stop(view->worker);
		view->worker = NULL;
	}
	if (view->link != NULL) {
		sftp_link_release(view->link);
		view->link = NULL;
	}
	sftp_worker_transfers_free(view->transfers);
	view->transfers = NULL;
	free(view->rows);
	view->rows = NULL;
	free(view->tree_flat);
	view->tree_flat = NULL;
	view_free_tree(view->tree_root);
	view->tree_root = NULL;
	view_history_free(&view->history);
	for (i = 0U; i < view->dialog_delete_count; i++)
		free(view->dialog_delete_paths[i]);
	free(view->dialog_delete_paths);
	view->dialog_delete_paths = NULL;
	if (view->canvas_ready)
		kl_canvas_release(&view->canvas);
	if (view->text_ready)
		kl_text_close(&view->text);
	kl_list_release(&view->tree_list);
	kl_list_release(&view->file_list);
	kl_list_release(&view->transfer_list);
	if (view->ui != NULL)
		kl_ui_destroy(view->ui);
	kl_mutex_destroy(&view->lock);
	free(view);
}

/* Draws the SFTP view frame. */
static int
view_op_draw(
	struct terminal_connection *connection,
	const struct terminal_view_state *state,
	uint32_t *pixels,
	uint32_t width,
	uint32_t height)
{
	struct sftp_view *view;

	view = (struct sftp_view *)connection;
	view_poll_worker(view);
	return sftp_view_draw_frame(view, state, pixels, width, height);
}

/* Copies the tab title. */
static void
view_op_title(
	struct terminal_connection *connection,
	char *title,
	size_t size)
{
	struct sftp_view *view;

	view = (struct sftp_view *)connection;
	(void)snprintf(title, size, "%s", view->title_text);
}

/* Handles keyboard, pointer, and text input. */
static void
view_op_input(
	struct terminal_connection *connection,
	const struct terminal_picture_event *event)
{
	sftp_view_handle_input((struct sftp_view *)connection, event);
}

/* Handles drag-and-drop events. */
static unsigned
view_op_drop(
	struct terminal_connection *connection,
	struct terminal_window *window,
	const struct terminal_drop_event *event)
{
	return sftp_view_handle_drop((struct sftp_view *)connection, window, event);
}

/* Tracks tab visibility and stores the active window handle. */
static void
view_op_shown(
	struct terminal_connection *connection,
	struct terminal_window *window,
	int visible)
{
	struct sftp_view *view;

	view = (struct sftp_view *)connection;
	view->visible = visible ? 1 : 0;
	view->window = visible ? window : NULL;
	if (visible)
		view->dirty = 1;
}

/* Checks whether closing is allowed. */
static int
view_op_confirm_close(
	struct terminal_connection *connection,
	struct terminal_window *window)
{
	return sftp_view_confirm_close(connection, window);
}

/* Reports whether Copy is available. */
static int
view_op_can_copy(
	const struct terminal_connection *connection)
{
	const struct sftp_view *view;
	size_t i;

	view = (const struct sftp_view *)connection;
	for (i = 0U; i < view->row_count; i++) {
		if (view->rows[i].selected)
			return 1;
	}
	return view->current_path[0] != '\0';
}

/* Copies the selected path(s) to text. */
static size_t
view_op_copy(
	struct terminal_connection *connection,
	char *text,
	size_t size)
{
	struct sftp_view *view;
	char full[SSH_SFTP_PATH_MAX];
	size_t i;
	size_t pos;

	view = (struct sftp_view *)connection;
	if (size == 0U)
		return 0U;
	pos = 0U;
	text[0] = '\0';
	for (i = 0U; i < view->row_count; i++) {
		size_t flen;

		if (!view->rows[i].selected)
			continue;
		if (sftp_transfer_join_remote(full, sizeof(full), view->current_path, view->rows[i].entry.name) != 0)
			continue;
		flen = strlen(full);
		if (pos > 0U && pos + 1U < size)
			text[pos++] = '\n';
		if (pos + flen >= size)
			break;
		memcpy(text + pos, full, flen);
		pos += flen;
	}
	if (pos == 0U && view->current_path[0] != '\0') {
		pos = strlen(view->current_path);
		if (pos >= size)
			pos = size - 1U;
		memcpy(text, view->current_path, pos);
	}
	text[pos] = '\0';
	return pos;
}

/* Reports whether Paste is available (for active text fields). */
static int
view_op_can_paste(
	const struct terminal_connection *connection)
{
	const struct sftp_view *view;

	view = (const struct sftp_view *)connection;
	return view->editing_path || view->renaming || view->dialog == SFTP_DIALOG_NEW_FOLDER;
}

/* Pastes text into the active text field. */
static void
view_op_paste(
	struct terminal_connection *connection,
	const char *text,
	size_t length)
{
	struct sftp_view *view;
	struct kl_field *field;
	char buf[KL_FIELD_MAX];
	size_t copy_len;

	view = (struct sftp_view *)connection;
	field = NULL;
	if (view->dialog == SFTP_DIALOG_NEW_FOLDER)
		field = &view->dialog_field;
	else if (view->renaming)
		field = &view->rename_field;
	else if (view->editing_path)
		field = &view->path_field;
	if (field == NULL || text == NULL || length == 0U)
		return;

	copy_len = length;
	if (copy_len >= sizeof(buf))
		copy_len = sizeof(buf) - 1U;
	memcpy(buf, text, copy_len);
	buf[copy_len] = '\0';
	kl_field_set(field, buf);
	view->dirty = 1;
}

/* Handles tab refusal when no tab slot is available. */
static void
view_op_refuse(
	struct terminal_connection *connection)
{
	struct sftp_view *view;

	view = (struct sftp_view *)connection;
	if (view->session != NULL) {
		mikuterm_session_detach_sftp(view->session, connection);
		view->session = NULL;
	}
}

/* Worker thread notification callback. */
static void
view_worker_notify(
	void *data)
{
	struct sftp_view *view;
	int should_post;

	view = data;
	kl_mutex_lock(&view->lock);
	view->dirty = 1;
	should_post = !view->posted && !view->closed;
	if (should_post)
		view->posted = 1;
	kl_mutex_unlock(&view->lock);
	if (should_post)
		(void)kl_app_post(view->app, SFTP_VIEW_POST, (void *)(uintptr_t)view->number);
}

/* Polls worker status and transfer queue snapshots on the UI thread. */
static void
view_poll_worker(
	struct sftp_view *view)
{
	enum sftp_worker_state st;
	char home[SSH_SFTP_PATH_MAX];
	char err[256];
	struct sftp_transfer_snapshot *items;
	size_t count;
	size_t active;
	size_t i;
	uint64_t now_us;

	if (view == NULL || view->worker == NULL)
		return;

	sftp_worker_status(view->worker, &st, home, sizeof(home), err, sizeof(err));
	view->worker_state = st;
	if (home[0] != '\0')
		(void)snprintf(view->home_path, sizeof(view->home_path), "%s", home);
	sftp_view_sanitize_text(err, view->worker_error, sizeof(view->worker_error));

	if (st == SFTP_WORKER_READY && !view->initial_loaded) {
		view->initial_loaded = 1;
		sftp_view_navigate(view, home[0] != '\0' ? home : "/", 1, 0);
	}

	items = NULL;
	count = 0U;
	active = 0U;
	if (sftp_worker_transfers(view->worker, &items, &count, &active) == 0) {
		sftp_worker_transfers_free(view->transfers);
		view->transfers = items;
		view->transfer_count = count;
		view->active_transfers = active;

		/* Computes instantaneous transfer speed and checks for overwrite prompts. */
		now_us = kl_clock_us();
		for (i = 0U; i < count; i++) {
			if (items[i].state == SFTP_TRANSFER_ACTIVE) {
				if (view->last_progress_id == items[i].id && now_us > view->last_progress_us + 200000ULL) {
					double dt;

					dt = (double)(now_us - view->last_progress_us) / 1000000.0;
					if (items[i].bytes_done >= view->last_progress_bytes && dt > 0.0)
						view->transfer_speed = (double)(items[i].bytes_done - view->last_progress_bytes) / dt;
					view->last_progress_bytes = items[i].bytes_done;
					view->last_progress_us = now_us;
				} else if (view->last_progress_id != items[i].id) {
					view->last_progress_id = items[i].id;
					view->last_progress_bytes = items[i].bytes_done;
					view->last_progress_us = now_us;
					view->transfer_speed = 0.0;
				}
			}
			if (items[i].state == SFTP_TRANSFER_WAITING_OVERWRITE && view->dialog == SFTP_DIALOG_NONE) {
				char clean_name[512];
				char rsize[32];
				char lsize[32];
				char rmtime[32];
				char lmtime[32];
				const char *base;

				base = strrchr(items[i].remote_path, '/');
				base = base != NULL ? base + 1 : items[i].remote_path;
				sftp_view_sanitize_text(base, clean_name, sizeof(clean_name));
				sftp_view_format_size(items[i].conflict_remote_size, rsize, sizeof(rsize));
				sftp_view_format_size(items[i].conflict_local_size, lsize, sizeof(lsize));
				sftp_view_format_mtime(items[i].conflict_remote_mtime, rmtime, sizeof(rmtime));
				sftp_view_format_mtime(items[i].conflict_local_mtime, lmtime, sizeof(lmtime));
				view->dialog = SFTP_DIALOG_OVERWRITE;
				view->dialog_transfer_id = items[i].id;
				(void)snprintf(view->dialog_title, sizeof(view->dialog_title), "File already exists");
				(void)snprintf(view->dialog_body, sizeof(view->dialog_body),
					"\"%s\" already exists.\nRemote: %s, %s\nLocal: %s, %s%s",
					clean_name, rsize, rmtime, lsize, lmtime,
					items[i].direction == SFTP_TRANSFER_UPLOAD
					? "\nCancelling an upload overwrite leaves a partially written file."
					: "");
				break;
			}
		}
	}
}

/* Recursively frees a tree node and its children. */
static void
view_free_tree(
	struct sftp_tree_node *node)
{
	size_t i;

	if (node == NULL)
		return;
	for (i = 0U; i < node->child_count; i++)
		view_free_tree(node->children[i]);
	free(node->children);
	free(node);
}

/* Allocates one tree node. */
static struct sftp_tree_node *
view_alloc_node(
	const char *name,
	const char *path,
	int depth,
	int is_link,
	struct sftp_tree_node *parent)
{
	struct sftp_tree_node *node;

	node = calloc(1U, sizeof(*node));
	if (node == NULL)
		return NULL;
	(void)snprintf(node->name, sizeof(node->name), "%s", name);
	sftp_view_sanitize_text(name, node->display, sizeof(node->display));
	(void)snprintf(node->path, sizeof(node->path), "%s", path);
	node->depth = depth;
	node->is_link = is_link;
	node->parent = parent;
	return node;
}

/* Populates child directory nodes for node using sftp_worker_list_dir. */
static void
view_populate_tree_children(
	struct sftp_view *view,
	struct sftp_tree_node *node)
{
	struct sftp_dir_listing *listing;
	size_t i;
	size_t count;
	struct sftp_tree_node **children;
	char full[SSH_SFTP_PATH_MAX];

	if (view == NULL || view->worker == NULL || node == NULL)
		return;
	listing = NULL;
	if (sftp_worker_list_dir(view->worker, node->path, 0, &listing) != 0 || listing == NULL)
		return;
	node->loaded = 1;
	if (listing->error != 0 || listing->count == 0U) {
		sftp_dir_listing_free(listing);
		return;
	}

	children = calloc(listing->count, sizeof(*children));
	if (children == NULL) {
		sftp_dir_listing_free(listing);
		return;
	}

	count = 0U;
	for (i = 0U; i < listing->count; i++) {
		const struct sftp_dir_entry *e;

		e = &listing->entries[i];
		if (!e->is_dir && !e->link_is_dir)
			continue;
		if (!view->show_hidden && e->name[0] == '.')
			continue;
		if (sftp_transfer_join_remote(full, sizeof(full), node->path, e->name) != 0)
			continue;
		children[count] = view_alloc_node(e->name, full, node->depth + 1, e->is_link, node);
		if (children[count] != NULL)
			count++;
	}

	for (i = 0U; i < node->child_count; i++)
		view_free_tree(node->children[i]);
	free(node->children);
	node->children = children;
	node->child_count = count;
	sftp_dir_listing_free(listing);
}

/* Appends node and its expanded descendants to view->tree_flat. */
static void
view_append_flat(
	struct sftp_view *view,
	struct sftp_tree_node *node)
{
	struct sftp_tree_node **grown;
	size_t i;

	grown = realloc(view->tree_flat, (view->tree_flat_count + 1U) * sizeof(*grown));
	if (grown == NULL)
		return;
	view->tree_flat = grown;
	view->tree_flat[view->tree_flat_count++] = node;
	if (!node->expanded)
		return;
	for (i = 0U; i < node->child_count; i++)
		view_append_flat(view, node->children[i]);
}

/* Compares two file list rows for qsort. */
static int
view_compare_rows(
	const void *a,
	const void *b)
{
	const struct sftp_list_row *ra;
	const struct sftp_list_row *rb;
	int da;
	int db;
	int cmp;

	ra = a;
	rb = b;
	da = ra->entry.is_dir || ra->entry.link_is_dir;
	db = rb->entry.is_dir || rb->entry.link_is_dir;
	if (da != db)
		return da ? -1 : 1;

	cmp = 0;
	switch (view_sort_col) {
	case SFTP_SORT_SIZE:
		if (ra->entry.size < rb->entry.size)
			cmp = -1;
		else if (ra->entry.size > rb->entry.size)
			cmp = 1;
		else
			cmp = _stricmp(ra->entry.name, rb->entry.name);
		break;
	case SFTP_SORT_MTIME:
		if (ra->entry.mtime < rb->entry.mtime)
			cmp = -1;
		else if (ra->entry.mtime > rb->entry.mtime)
			cmp = 1;
		else
			cmp = _stricmp(ra->entry.name, rb->entry.name);
		break;
	case SFTP_SORT_NAME:
	default:
		cmp = _stricmp(ra->entry.name, rb->entry.name);
		break;
	}
	return view_sort_rev ? -cmp : cmp;
}

/* Pushes path onto navigation history, discarding forward entries. */
static void
view_history_push(
	struct sftp_history *hist,
	const char *path)
{
	char **grown;
	size_t i;

	if (hist->count > 0U && hist->index < hist->count &&
	    strcmp(hist->paths[hist->index], path) == 0)
		return;

	if (hist->count > 0U && hist->index + 1U < hist->count) {
		for (i = hist->index + 1U; i < hist->count; i++)
			free(hist->paths[i]);
		hist->count = hist->index + 1U;
	}
	if (hist->count >= hist->capacity) {
		size_t new_cap;

		new_cap = hist->capacity == 0U ? 16U : hist->capacity * 2U;
		grown = realloc(hist->paths, new_cap * sizeof(*grown));
		if (grown == NULL)
			return;
		hist->paths = grown;
		hist->capacity = new_cap;
	}
	hist->paths[hist->count] = strdup(path);
	if (hist->paths[hist->count] == NULL)
		return;
	hist->index = hist->count;
	hist->count++;
}

/* Frees navigation history. */
static void
view_history_free(
	struct sftp_history *hist)
{
	size_t i;

	for (i = 0U; i < hist->count; i++)
		free(hist->paths[i]);
	free(hist->paths);
	hist->paths = NULL;
	hist->count = 0U;
	hist->capacity = 0U;
	hist->index = 0U;
}

/* Decodes one UTF-8 code point, returning byte length or 0 if invalid. */
static size_t
view_decode_utf8(
	const unsigned char *s,
	size_t len,
	uint32_t *cp)
{
	uint32_t v;

	if (len == 0U)
		return 0U;
	if (s[0] < 0x80U) {
		*cp = s[0];
		return 1U;
	}
	if ((s[0] & 0xe0U) == 0xc0U) {
		if (len < 2U || (s[1] & 0xc0U) != 0x80U)
			return 0U;
		v = ((uint32_t)(s[0] & 0x1fU) << 6) | (uint32_t)(s[1] & 0x3fU);
		if (v < 0x80U)
			return 0U;
		*cp = v;
		return 2U;
	}
	if ((s[0] & 0xf0U) == 0xe0U) {
		if (len < 3U || (s[1] & 0xc0U) != 0x80U || (s[2] & 0xc0U) != 0x80U)
			return 0U;
		v = ((uint32_t)(s[0] & 0x0fU) << 12) | ((uint32_t)(s[1] & 0x3fU) << 6) | (uint32_t)(s[2] & 0x3fU);
		if (v < 0x800U || (v >= 0xd800U && v <= 0xdfffU))
			return 0U;
		*cp = v;
		return 3U;
	}
	if ((s[0] & 0xf8U) == 0xf0U) {
		if (len < 4U || (s[1] & 0xc0U) != 0x80U || (s[2] & 0xc0U) != 0x80U || (s[3] & 0xc0U) != 0x80U)
			return 0U;
		v = ((uint32_t)(s[0] & 0x07U) << 18) | ((uint32_t)(s[1] & 0x3fU) << 12) |
		    ((uint32_t)(s[2] & 0x3fU) << 6) | (uint32_t)(s[3] & 0x3fU);
		if (v < 0x10000U || v > 0x10ffffU)
			return 0U;
		*cp = v;
		return 4U;
	}
	return 0U;
}

/* Reports whether cp is a Unicode bidirectional override/isolate control. */
static int
view_is_bidi_control(
	uint32_t cp)
{
	return (cp >= 0x202aU && cp <= 0x202eU) || (cp >= 0x2066U && cp <= 0x2069U);
}
