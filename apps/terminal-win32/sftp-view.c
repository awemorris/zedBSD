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
#include "sftp-drag-win32.h"
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
static void view_drag_notify(void *data, const char *error_text);
static void view_run_external_drag(struct sftp_view *view);
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
	int text_err;
	int error;

	/* Validates required arguments. */
	if (app == NULL || link == NULL || connection_out == NULL)
		return EINVAL;

	/* Allocates and zeroes the view state. */
	view = calloc(1U, sizeof(*view));
	if (view == NULL)
		return ENOMEM;

	/* Initializes basic connection, host, and display defaults. */
	view->connection.ops = &view_conn_ops;
	view->app = app;
	view->session = session;
	view->link = link;
	sftp_link_retain(link);
	view->number = view_next_number++;
	if (show_hidden)
		view->show_hidden = 1;
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

	/* Initializes the view mutex protecting worker/UI shared flags. */
	error = kl_mutex_init(&view->lock);
	if (error != 0) {
		sftp_link_release(link);
		free(view);
		return error;
	}

	/* Initializes UI widgets and opens the text renderer when fonts are given. */
	view->ui = kl_ui_create();
	(void)kl_list_init(&view->tree_list);
	(void)kl_list_init(&view->file_list);
	(void)kl_list_init(&view->transfer_list);
	if (font_path != NULL && font_path[0] != '\0') {
		text_err = kl_text_open(&view->text, font_path, fallback_path);
		if (text_err == 0)
			view->text_ready = 1;
	}

	/* Builds the root node of the remote folder tree. */
	view->tree_root = view_alloc_node("/", "/", 0, 0, NULL);
	if (view->tree_root != NULL) {
		view->tree_root->expanded = 1;
		sftp_view_rebuild_tree_flat(view);
	}

	/* Starts the background SFTP worker thread. */
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

	/* Creates the outbound virtual-file drag context and publishes the view. */
	view->drag_ctx = sftp_drag_context_create(view->worker, view_drag_notify, view);
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
	const wchar_t *verb;
	int answer;
	int error;

	/* Ignores non-SFTP connections or views without an active worker. */
	(void)window;
	if (connection == NULL || connection->ops != &view_conn_ops)
		return 1;
	view = (struct sftp_view *)connection;
	if (view->worker == NULL)
		return 1;

	/* Counts active queued transfers and outbound virtual-file drags. */
	items = NULL;
	count = 0U;
	active = 0U;
	error = sftp_worker_transfers(view->worker, &items, &count, &active);
	if (error == 0)
		sftp_worker_transfers_free(items);
	active += sftp_drag_context_active_count(view->drag_ctx);
	if (active == 0U)
		return 1;

	/* Prompts the user to confirm cancelling active transfers. */
	if (active == 1U)
		verb = L" is";
	else
		verb = L"s are";
	(void)swprintf(
		msg,
		sizeof(msg) / sizeof(msg[0]),
		L"%zu transfer%s in progress. Close and cancel them?",
		active,
		verb);
	answer = MessageBoxW(NULL, msg, L"mikuterm SFTP", MB_YESNO | MB_ICONWARNING);
	if (answer == IDYES) {
		sftp_worker_cancel_all(view->worker);
		sftp_drag_context_cancel_all(view->drag_ctx);
		return 1;
	}

	/* User chose to keep the tab open. */
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
	int bidi;

	/* Clears the output buffer and validates inputs. */
	if (out == NULL || out_cap == 0U)
		return;
	out[0] = '\0';
	if (raw == NULL)
		return;

	/* Walks raw UTF-8 code points and replaces control or bidi characters. */
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

		/* Replaces C0/DEL/C1 control characters and bidi overrides with hex markers. */
		bidi = view_is_bidi_control(cp);
		if (cp < 0x20U || cp == 0x7fU || (cp >= 0x80U && cp <= 0x9fU)) {
			(void)snprintf(marker, sizeof(marker), "[x%02X]", (unsigned)cp);
			mlen = strlen(marker);
			if (pos + mlen >= out_cap)
				break;
			memcpy(out + pos, marker, mlen);
			pos += mlen;
		} else if (bidi) {
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

		/* Advances to the next UTF-8 code point. */
		i += step;
	}

	/* NUL-terminates the sanitized output string. */
	out[pos] = '\0';
}

/*
 * Marks the view dirty and posts a wakeup to the UI thread.
 */
void
sftp_view_request_redraw(
	struct sftp_view *view)
{
	/* Schedules a UI redraw via the worker notification path. */
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
	const struct sftp_dir_entry *e;
	size_t i;
	size_t cap;
	struct sftp_list_row *rows;
	int error;

	/* Validates the view, worker, and target path. */
	if (view == NULL || view->worker == NULL || path == NULL || path[0] == '\0')
		return;

	/* Requests the directory listing from the worker thread. */
	listing = NULL;
	error = sftp_worker_list_dir(view->worker, path, force_refresh, &listing);
	if (error != 0 || listing == NULL)
		return;

	/* Displays sanitized error text if listing the directory failed. */
	if (listing->error != 0) {
		sftp_view_sanitize_text(listing->error_text, view->list_error, sizeof(view->list_error));
		sftp_dir_listing_free(listing);
		view->dirty = 1;
		return;
	}

	/* Updates current path and navigation history. */
	view->list_error[0] = '\0';
	view->list_truncated = listing->truncated;
	(void)snprintf(view->current_path, sizeof(view->current_path), "%s", listing->path);
	if (record_history)
		view_history_push(&view->history, view->current_path);

	/* Clears previous file list rows and selection state. */
	free(view->rows);
	view->rows = NULL;
	view->row_count = 0U;
	view->focus_row = -1;
	view->anchor_row = -1;
	view->renaming = 0;

	/* Populates, sanitizes, formats, and sorts the new file list rows. */
	cap = listing->count;
	if (cap > 0U) {
		rows = calloc(cap, sizeof(*rows));
		if (rows != NULL) {
			for (i = 0U; i < listing->count; i++) {
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

			/* Sorts the populated rows. */
			view->rows = rows;
			sftp_view_sort_rows(view);
		}
	}

	/* Resets scroll, frees the listing, reveals the path in the tree, and redraws. */
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
	int at_root;

	/* Ignores requests when already at root or uninitialized. */
	if (view == NULL || view->current_path[0] == '\0')
		return;
	at_root = strcmp(view->current_path, "/");
	if (at_root == 0)
		return;

	/* Strips the trailing path component and navigates to the parent. */
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
	/* Moves back one entry in navigation history if available. */
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
	/* Moves forward one entry in navigation history if available. */
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
	/* Forces a fresh worker directory listing of current_path. */
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
	/* Validates arguments and collapses the node if already expanded. */
	if (view == NULL || node == NULL)
		return;
	if (node->expanded) {
		node->expanded = 0;
		sftp_view_rebuild_tree_flat(view);
		view->dirty = 1;
		return;
	}

	/* Loads child directories on first expand and rebuilds the flat tree. */
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
	size_t name_len;
	size_t i;
	struct sftp_tree_node *found;
	int match;

	/* Validates the view, root node, and leading slash. */
	if (view == NULL || view->tree_root == NULL || path == NULL || path[0] != '/')
		return;

	/* Ensures the root node's children are loaded and expanded. */
	cur = view->tree_root;
	if (!cur->loaded)
		view_populate_tree_children(view, cur);
	cur->expanded = 1;

	/* Walks each path segment down the tree, loading nodes as needed. */
	p = path + 1;
	while (*p != '\0') {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		next = strchr(p, '/');
		if (next != NULL)
			seg_len = (size_t)(next - p);
		else
			seg_len = strlen(p);
		if (!cur->loaded)
			view_populate_tree_children(view, cur);
		cur->expanded = 1;
		found = NULL;
		for (i = 0U; i < cur->child_count; i++) {
			name_len = strlen(cur->children[i]->name);
			if (name_len == seg_len) {
				match = strncmp(cur->children[i]->name, p, seg_len);
				if (match == 0) {
					found = cur->children[i];
					break;
				}
			}
		}

		/* Stops descending if a segment is missing from the tree. */
		if (found == NULL)
			break;
		cur = found;
		p += seg_len;
	}

	/* Expands the target directory node and rebuilds the flat visible list. */
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
	/* Discards the old flat array and walks expanded nodes from root. */
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
	/* Sorts rows when at least two entries are present. */
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

	/* Finds the focused selected row, or falls back to the first selected row. */
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

	/* Ignores rename when no row is selected. */
	if (target < 0)
		return;

	/* Activates the inline rename text field with the current filename. */
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
	const char *slash;
	int unchanged;
	int error;

	/* Validates active rename state and rejects empty, unchanged, or slash-containing names. */
	if (view == NULL || !view->renaming || view->rename_row >= view->row_count)
		return;
	view->renaming = 0;
	unchanged = strcmp(view->rename_field.text, view->rows[view->rename_row].entry.name);
	slash = strchr(view->rename_field.text, '/');
	if (view->rename_field.text[0] == '\0' || unchanged == 0 || slash != NULL) {
		view->dirty = 1;
		return;
	}

	/* Joins the old and new remote paths. */
	error = sftp_transfer_join_remote(old_path, sizeof(old_path), view->current_path, view->rows[view->rename_row].entry.name);
	if (error == 0)
		error = sftp_transfer_join_remote(new_path, sizeof(new_path), view->current_path, view->rename_field.text);
	if (error != 0)
		return;

	/* Performs the remote rename and shows an error dialog on failure. */
	err[0] = '\0';
	error = sftp_worker_rename(view->worker, old_path, new_path, err, sizeof(err));
	if (error != 0) {
		sftp_view_sanitize_text(err, clean, sizeof(clean));
		sftp_view_show_error(view, "Rename failed", clean);
		return;
	}

	/* Refreshes the directory listing to reflect the new name. */
	sftp_view_refresh_all(view);
}

/*
 * Opens the New Folder modal dialog.
 */
void
sftp_view_begin_new_folder(
	struct sftp_view *view)
{
	/* Ignores requests when the worker is not ready. */
	if (view == NULL || view->worker_state != SFTP_WORKER_READY)
		return;

	/* Opens the New Folder dialog and focuses its text input field. */
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
	size_t items;
	size_t folders;
	int is_link;
	const char *item_s;
	const char *folder_s;
	const char *sym_note;
	int error;

	/* Validates state and allocates the selected-path array. */
	if (view == NULL || view->worker == NULL || view->row_count == 0U)
		return;
	paths = calloc(view->row_count, sizeof(*paths));
	if (paths == NULL)
		return;

	/* Collects selected paths and counts items/folders recursively. */
	count = 0U;
	total_items = 0U;
	total_folders = 0U;
	any_symlink = 0;
	for (i = 0U; i < view->row_count; i++) {
		if (!view->rows[i].selected)
			continue;
		error = sftp_transfer_join_remote(full, sizeof(full), view->current_path, view->rows[i].entry.name);
		if (error != 0)
			continue;
		paths[count] = strdup(full);
		if (paths[count] == NULL)
			continue;
		count++;
		items = 0U;
		folders = 0U;
		is_link = 0;
		error = sftp_worker_delete_count(view->worker, full, &items, &folders, &is_link);
		if (error == 0) {
			total_items += items;
			total_folders += folders;
			if (is_link)
				any_symlink = 1;
		} else {
			total_items++;
		}
	}

	/* Aborts if no rows were selected. */
	if (count == 0U) {
		free(paths);
		return;
	}

	/* Replaces any prior delete target list and opens the confirmation dialog. */
	for (i = 0U; i < view->dialog_delete_count; i++)
		free(view->dialog_delete_paths[i]);
	free(view->dialog_delete_paths);
	view->dialog_delete_paths = paths;
	view->dialog_delete_count = count;
	view->dialog = SFTP_DIALOG_DELETE;
	if (total_items == 1U)
		item_s = "";
	else
		item_s = "s";
	if (total_folders == 1U)
		folder_s = "";
	else
		folder_s = "s";
	if (any_symlink)
		sym_note = " Symbolic links will be unlinked without deleting their targets.";
	else
		sym_note = "";
	(void)snprintf(view->dialog_title, sizeof(view->dialog_title), "Confirm delete");
	(void)snprintf(
		view->dialog_body,
		sizeof(view->dialog_body),
		"Delete %zu item%s (%zu folder%s)?%s",
		total_items,
		item_s,
		total_folders,
		folder_s,
		sym_note);
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
	int is_dir;
	int error;

	/* Resolves the local Downloads folder before queueing downloads. */
	if (view == NULL || view->worker == NULL)
		return;
	error = sftp_local_downloads_dir(downloads, sizeof(downloads));
	if (error != 0) {
		sftp_view_show_error(view, "Download failed", "Could not locate the Downloads folder.");
		return;
	}

	/* Enqueues a download for each selected row. */
	for (i = 0U; i < view->row_count; i++) {
		if (!view->rows[i].selected)
			continue;
		error = sftp_transfer_join_remote(remote, sizeof(remote), view->current_path, view->rows[i].entry.name);
		if (error != 0)
			continue;
		is_dir = view->rows[i].entry.is_dir || view->rows[i].entry.link_is_dir;
		(void)sftp_worker_enqueue_download(view->worker, remote, downloads, is_dir, NULL);
	}

	/* Marks the view dirty to show the transfer bar. */
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

	/* Marks every row selected and schedules a redraw. */
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
	/* Flips show_hidden and rebuilds the current view from cache. */
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
	const char *safe_title;
	const char *safe_msg;

	/* Populates the modal error dialog fields and requests a redraw. */
	if (view == NULL)
		return;
	if (title != NULL)
		safe_title = title;
	else
		safe_title = "Error";
	if (message != NULL)
		safe_msg = message;
	else
		safe_msg = "Unknown error.";
	view->dialog = SFTP_DIALOG_ERROR;
	(void)snprintf(view->dialog_title, sizeof(view->dialog_title), "%s", safe_title);
	(void)snprintf(view->dialog_body, sizeof(view->dialog_body), "%s", safe_msg);
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
	int same_dir;
	int same_item;
	int error;

	/* Validates target_dir and ignores moves onto the current directory. */
	if (view == NULL || view->worker == NULL || target_dir == NULL || target_dir[0] == '\0')
		return;
	same_dir = strcmp(target_dir, view->current_path);
	if (same_dir == 0)
		return;

	/* Renames each selected item into target_dir. */
	moved = 0;
	for (i = 0U; i < view->row_count; i++) {
		if (!view->rows[i].selected)
			continue;
		error = sftp_transfer_join_remote(old_path, sizeof(old_path), view->current_path, view->rows[i].entry.name);
		if (error == 0)
			error = sftp_transfer_join_remote(new_path, sizeof(new_path), target_dir, view->rows[i].entry.name);
		if (error != 0)
			continue;
		same_item = strcmp(old_path, target_dir);
		if (same_item == 0)
			continue;
		err[0] = '\0';
		error = sftp_worker_rename(view->worker, old_path, new_path, err, sizeof(err));
		if (error != 0) {
			sftp_view_sanitize_text(err, clean, sizeof(clean));
			sftp_view_show_error(view, "Move failed", clean);
			return;
		}

		/* Records that at least one item moved. */
		moved = 1;
	}

	/* Refreshes the view if any item was moved. */
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

	/* Closes the context menu and dispatches the selected command. */
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

		/* Falls back to current_path when no row is selected. */
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
	const char *base_text;
	size_t text_size;
	size_t made_size;
	int error;

	/* Resolves config/state.conf path. */
	if (view == NULL)
		return;
	error = mikuterm_path(path, sizeof(path), "config/state.conf");
	if (error != 0)
		return;

	/* Reads existing state.conf contents if present. */
	text = NULL;
	text_size = 0U;
	error = mikuterm_file_read(path, MIKUTERM_CONF_MAX, &text, &text_size);
	if (error != 0 && error != ENOENT)
		return;

	/* Updates the SftpSplit entry and writes the file back. */
	(void)snprintf(value, sizeof(value), "%d", view->split_width);
	if (text != NULL)
		base_text = text;
	else
		base_text = "";
	error = mikuterm_conf_set(base_text, text_size, "SftpSplit", value, &made, &made_size);
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
	const char *eol;
	int parsed;
	int match;
	int val;
	int error;

	/* Resolves config/state.conf path and reads its contents. */
	error = mikuterm_path(path, sizeof(path), "config/state.conf");
	if (error != 0)
		return SFTP_SPLIT_DEFAULT;
	text = NULL;
	text_size = 0U;
	error = mikuterm_file_read(path, MIKUTERM_CONF_MAX, &text, &text_size);
	if (error != 0 || text == NULL)
		return SFTP_SPLIT_DEFAULT;

	/* Scans lines for SftpSplit and clamps it to valid bounds. */
	val = SFTP_SPLIT_DEFAULT;
	p = text;
	end = text + text_size;
	while (p < end) {
		eol = memchr(p, '\n', (size_t)(end - p));
		if (eol == NULL)
			eol = end;
		if ((size_t)(eol - p) > 10U) {
			match = strncmp(p, "SftpSplit ", 10U);
			if (match == 0) {
				parsed = atoi(p + 10);
				if (parsed >= SFTP_SPLIT_MIN && parsed <= 800)
					val = parsed;
				break;
			}
		}

		/* Advances to the next line. */
		p = eol + 1;
	}

	/* Frees the file buffer and returns the splitter width. */
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
	/* Selects B, KB, MB, or GB units based on magnitude. */
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

	/* Converts the timestamp to local calendar time. */
	t = (time_t)mtime;
	res = localtime(&t);
	if (res == NULL) {
		(void)snprintf(out, cap, "--");
		return;
	}

	/* Formats the timestamp into out. */
	tm_val = *res;
	(void)snprintf(
		out,
		cap,
		"%04d-%02d-%02d %02d:%02d",
		tm_val.tm_year + 1900,
		tm_val.tm_mon + 1,
		tm_val.tm_mday,
		tm_val.tm_hour,
		tm_val.tm_min);
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
	/* Clears out when the buffer is too short for 10 mode chars plus NUL. */
	if (cap < 11U) {
		if (cap > 0U)
			out[0] = '\0';
		return;
	}

	/* Sets the file type prefix character. */
	if (is_link)
		out[0] = 'l';
	else if (is_dir)
		out[0] = 'd';
	else
		out[0] = '-';

	/* Sets owner, group, and other rwx permission characters. */
	if ((mode & 0400U) != 0U)
		out[1] = 'r';
	else
		out[1] = '-';
	if ((mode & 0200U) != 0U)
		out[2] = 'w';
	else
		out[2] = '-';
	if ((mode & 0100U) != 0U)
		out[3] = 'x';
	else
		out[3] = '-';
	if ((mode & 0040U) != 0U)
		out[4] = 'r';
	else
		out[4] = '-';
	if ((mode & 0020U) != 0U)
		out[5] = 'w';
	else
		out[5] = '-';
	if ((mode & 0010U) != 0U)
		out[6] = 'x';
	else
		out[6] = '-';
	if ((mode & 0004U) != 0U)
		out[7] = 'r';
	else
		out[7] = '-';
	if ((mode & 0002U) != 0U)
		out[8] = 'w';
	else
		out[8] = '-';
	if ((mode & 0001U) != 0U)
		out[9] = 'x';
	else
		out[9] = '-';
	out[10] = '\0';
}

/* Reads nothing; view tabs use view->draw instead. */
static size_t
view_conn_read(
	struct terminal_connection *connection,
	unsigned char *buffer,
	size_t size)
{
	/* View connections do not produce terminal byte streams. */
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
	/* View connections do not accept terminal byte writes. */
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

	/* Records the new pixel dimensions and requests a redraw. */
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
	/* View connections have no terminal line mode. */
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

	/* Filters for post events addressed to this view instance. */
	view = (struct sftp_view *)connection;
	if (event->kind != KL_APP_POST)
		return 0U;
	if ((uintptr_t)event->data != (uintptr_t)view->number)
		return 0U;

	/* Launches an outbound OLE virtual-file drag when posted. */
	if (event->code == SFTP_VIEW_DRAG_POST) {
		view->drag_start_pending = 0;
		view_run_external_drag(view);
		return TERMINAL_READY_OUTPUT;
	}

	/* Ignores unknown post codes. */
	if (event->code != SFTP_VIEW_POST)
		return 0U;

	/* Clears the posted flag and polls worker status/transfers. */
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

	/* Reads the dirty flag under the view lock. */
	view = (const struct sftp_view *)connection;
	kl_mutex_lock((struct kl_mutex *)&view->lock);

	dirty = view->dirty;

	kl_mutex_unlock((struct kl_mutex *)&view->lock);
	if (dirty)
		return TERMINAL_READY_OUTPUT;
	return 0U;
}

/* Closes the SFTP view tab, stopping its worker and releasing resources. */
static void
view_conn_close(
	struct terminal_connection *connection)
{
	struct sftp_view *view;
	size_t i;

	/* Detaches from the SSH session and stops outbound drags and worker thread. */
	view = (struct sftp_view *)connection;
	view->closed = 1;
	if (view->session != NULL) {
		mikuterm_session_detach_sftp(view->session, connection);
		view->session = NULL;
	}

	/* Detaches the drag context. */
	if (view->drag_ctx != NULL) {
		sftp_drag_context_detach(view->drag_ctx);
		view->drag_ctx = NULL;
	}

	/* Stops the worker thread. */
	if (view->worker != NULL) {
		sftp_worker_stop(view->worker);
		view->worker = NULL;
	}

	/* Releases the channel relay. */
	if (view->link != NULL) {
		sftp_link_release(view->link);
		view->link = NULL;
	}

	/* Frees view lists, tree, history, rendering resources, and mutex. */
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
	int drawn;

	/* Polls worker updates and renders the SFTP view frame. */
	view = (struct sftp_view *)connection;
	view_poll_worker(view);
	drawn = sftp_view_draw_frame(view, state, pixels, width, height);
	return drawn;
}

/* Copies the tab title. */
static void
view_op_title(
	struct terminal_connection *connection,
	char *title,
	size_t size)
{
	struct sftp_view *view;

	/* Copies the preformatted tab title. */
	view = (struct sftp_view *)connection;
	(void)snprintf(title, size, "%s", view->title_text);
}

/* Handles keyboard, pointer, and text input. */
static void
view_op_input(
	struct terminal_connection *connection,
	const struct terminal_picture_event *event)
{
	/* Forwards the input event to the SFTP view input handler. */
	sftp_view_handle_input((struct sftp_view *)connection, event);
}

/* Handles drag-and-drop events. */
static unsigned
view_op_drop(
	struct terminal_connection *connection,
	struct terminal_window *window,
	const struct terminal_drop_event *event)
{
	unsigned accepted;

	/* Forwards the drop event to the SFTP view drop handler. */
	accepted = sftp_view_handle_drop((struct sftp_view *)connection, window, event);
	return accepted;
}

/* Tracks tab visibility and stores the active window handle. */
static void
view_op_shown(
	struct terminal_connection *connection,
	struct terminal_window *window,
	int visible)
{
	struct sftp_view *view;

	/* Updates visibility and window handle, marking dirty when shown. */
	view = (struct sftp_view *)connection;
	if (visible) {
		view->visible = 1;
		view->window = window;
		view->dirty = 1;
	} else {
		view->visible = 0;
		view->window = NULL;
	}
}

/* Checks whether closing is allowed. */
static int
view_op_confirm_close(
	struct terminal_connection *connection,
	struct terminal_window *window)
{
	int confirmed;

	/* Prompts if transfers or outbound drags are active. */
	confirmed = sftp_view_confirm_close(connection, window);
	return confirmed;
}

/* Reports whether Copy is available. */
static int
view_op_can_copy(
	const struct terminal_connection *connection)
{
	const struct sftp_view *view;
	size_t i;

	/* Copy is available when any row is selected or a current path is open. */
	view = (const struct sftp_view *)connection;
	for (i = 0U; i < view->row_count; i++) {
		if (view->rows[i].selected)
			return 1;
	}

	/* Falls back to copying the current directory path. */
	if (view->current_path[0] != '\0')
		return 1;
	return 0;
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
	size_t flen;
	int error;

	/* Validates the output buffer. */
	view = (struct sftp_view *)connection;
	if (size == 0U)
		return 0U;

	/* Joins selected remote paths separated by newlines. */
	pos = 0U;
	text[0] = '\0';
	for (i = 0U; i < view->row_count; i++) {
		if (!view->rows[i].selected)
			continue;
		error = sftp_transfer_join_remote(full, sizeof(full), view->current_path, view->rows[i].entry.name);
		if (error != 0)
			continue;
		flen = strlen(full);
		if (pos > 0U && pos + 1U < size)
			text[pos++] = '\n';
		if (pos + flen >= size)
			break;
		memcpy(text + pos, full, flen);
		pos += flen;
	}

	/* Copies current_path when no row was selected. */
	if (pos == 0U && view->current_path[0] != '\0') {
		pos = strlen(view->current_path);
		if (pos >= size)
			pos = size - 1U;
		memcpy(text, view->current_path, pos);
	}

	/* NUL-terminates the copied string. */
	text[pos] = '\0';
	return pos;
}

/* Reports whether Paste is available (for active text fields). */
static int
view_op_can_paste(
	const struct terminal_connection *connection)
{
	const struct sftp_view *view;

	/* Paste is enabled only when a text input field is active. */
	view = (const struct sftp_view *)connection;
	if (view->editing_path || view->renaming || view->dialog == SFTP_DIALOG_NEW_FOLDER)
		return 1;
	return 0;
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

	/* Selects the currently active text field. */
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

	/* Copies the pasted text into the field and requests a redraw. */
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

	/* Detaches the refused view from its SSH session. */
	view = (struct sftp_view *)connection;
	if (view->session != NULL) {
		mikuterm_session_detach_sftp(view->session, connection);
		view->session = NULL;
	}
}

/*
 * Requests an outbound virtual-file OLE drag on the next UI event
 * dispatch.
 */
void
sftp_view_request_external_drag(
	struct sftp_view *view)
{
	/* Posts SFTP_VIEW_DRAG_POST when the worker is ready and no drag is pending. */
	if (view == NULL || view->closed || view->app == NULL)
		return;
	if (view->worker_state != SFTP_WORKER_READY || view->drag_ctx == NULL)
		return;
	if (view->drag_start_pending)
		return;
	view->drag_start_pending = 1;
	(void)kl_app_post(view->app, SFTP_VIEW_DRAG_POST, (void *)(uintptr_t)view->number);
}

/* Receives error notifications from outbound virtual-file drag operations. */
static void
view_drag_notify(
	void *data,
	const char *error_text)
{
	struct sftp_view *view;

	/* Validates arguments and stores the error under the view lock. */
	view = data;
	if (view == NULL || error_text == NULL)
		return;
	kl_mutex_lock(&view->lock);

	(void)snprintf(view->drag_error, sizeof(view->drag_error), "%s", error_text);
	view->drag_error_pending = 1;
	view->dirty = 1;
	if (!view->closed && !view->posted && view->app != NULL) {
		view->posted = 1;
		(void)kl_app_post(view->app, SFTP_VIEW_POST, (void *)(uintptr_t)view->number);
	}

	kl_mutex_unlock(&view->lock);
}

/* Collects selected rows and runs the OLE virtual-file DoDragDrop loop. */
static void
view_run_external_drag(
	struct sftp_view *view)
{
	struct sftp_drag_item *items;
	const struct sftp_dir_entry *ent;
	size_t count;
	size_t i;
	int error;

	/* Validates view and worker state before starting DoDragDrop. */
	if (view == NULL || view->closed || view->drag_ctx == NULL)
		return;
	if (view->worker_state != SFTP_WORKER_READY || view->row_count == 0U)
		return;

	/* Allocates the drag item descriptor array. */
	items = calloc(view->row_count, sizeof(*items));
	if (items == NULL)
		return;

	/* Populates drag items for all selected rows. */
	count = 0U;
	for (i = 0U; i < view->row_count; i++) {
		ent = &view->rows[i].entry;
		if (!view->rows[i].selected)
			continue;
		error = sftp_transfer_join_remote(
			items[count].remote_path,
			sizeof(items[count].remote_path),
			view->current_path,
			ent->name);
		if (error != 0)
			continue;
		(void)snprintf(items[count].name, sizeof(items[count].name), "%s", ent->name);
		items[count].size = ent->size;
		items[count].mtime = ent->mtime;
		if (ent->is_dir || ent->link_is_dir)
			items[count].is_dir = 1;
		if (ent->is_link)
			items[count].is_link = 1;
		count++;
	}

	/* Runs the synchronous OLE drag loop if at least one item was collected. */
	if (count > 0U)
		(void)sftp_drag_run(view->drag_ctx, items, count);
	free(items);
}

/* Worker thread notification callback. */
static void
view_worker_notify(
	void *data)
{
	struct sftp_view *view;
	int should_post;

	/* Marks the view dirty and posts one wakeup event if not already posted. */
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
	char drag_err[256];
	char clean_name[512];
	char rsize[32];
	char lsize[32];
	char rmtime[32];
	char lmtime[32];
	const char *base;
	const char *init_path;
	const char *ul_note;
	struct sftp_transfer_snapshot *items;
	size_t count;
	size_t active;
	size_t i;
	uint64_t now_us;
	double dt;
	int error;

	/* Ignores views without an active worker. */
	if (view == NULL || view->worker == NULL)
		return;

	/* Surfaces any pending outbound drag error dialog. */
	kl_mutex_lock(&view->lock);

	if (view->drag_error_pending) {
		(void)snprintf(drag_err, sizeof(drag_err), "%s", view->drag_error);
		view->drag_error_pending = 0;

		kl_mutex_unlock(&view->lock);
		sftp_view_show_error(view, "Drag and Drop", drag_err);
	} else {
		kl_mutex_unlock(&view->lock);
	}

	/* Polls the worker connection state and home directory. */
	sftp_worker_status(view->worker, &st, home, sizeof(home), err, sizeof(err));
	view->worker_state = st;
	if (home[0] != '\0')
		(void)snprintf(view->home_path, sizeof(view->home_path), "%s", home);
	sftp_view_sanitize_text(err, view->worker_error, sizeof(view->worker_error));

	/* Navigates to the remote home directory once the SFTP session becomes ready. */
	if (st == SFTP_WORKER_READY && !view->initial_loaded) {
		view->initial_loaded = 1;
		if (home[0] != '\0')
			init_path = home;
		else
			init_path = "/";
		sftp_view_navigate(view, init_path, 1, 0);
	}

	/* Fetches the latest transfer queue snapshot and updates speed and overwrite prompts. */
	items = NULL;
	count = 0U;
	active = 0U;
	error = sftp_worker_transfers(view->worker, &items, &count, &active);
	if (error == 0) {
		sftp_worker_transfers_free(view->transfers);
		view->transfers = items;
		view->transfer_count = count;
		view->active_transfers = active;

		/* Computes instantaneous transfer speed and checks for overwrite prompts. */
		now_us = kl_clock_us();
		for (i = 0U; i < count; i++) {
			if (items[i].state == SFTP_TRANSFER_ACTIVE) {
				if (view->last_progress_id == items[i].id && now_us > view->last_progress_us + 200000ULL) {
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

			/* Opens an overwrite confirmation dialog for the first waiting conflict. */
			if (items[i].state == SFTP_TRANSFER_WAITING_OVERWRITE && view->dialog == SFTP_DIALOG_NONE) {
				base = strrchr(items[i].remote_path, '/');
				if (base != NULL)
					base = base + 1;
				else
					base = items[i].remote_path;
				sftp_view_sanitize_text(base, clean_name, sizeof(clean_name));
				sftp_view_format_size(items[i].conflict_remote_size, rsize, sizeof(rsize));
				sftp_view_format_size(items[i].conflict_local_size, lsize, sizeof(lsize));
				sftp_view_format_mtime(items[i].conflict_remote_mtime, rmtime, sizeof(rmtime));
				sftp_view_format_mtime(items[i].conflict_local_mtime, lmtime, sizeof(lmtime));
				if (items[i].direction == SFTP_TRANSFER_UPLOAD)
					ul_note = "\nCancelling an upload overwrite leaves a partially written file.";
				else
					ul_note = "";
				view->dialog = SFTP_DIALOG_OVERWRITE;
				view->dialog_transfer_id = items[i].id;
				(void)snprintf(view->dialog_title, sizeof(view->dialog_title), "File already exists");
				(void)snprintf(
					view->dialog_body,
					sizeof(view->dialog_body),
					"\"%s\" already exists.\nRemote: %s, %s\nLocal: %s, %s%s",
					clean_name,
					rsize,
					rmtime,
					lsize,
					lmtime,
					ul_note);
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

	/* Ignores a NULL node and frees descendants post-order. */
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

	/* Allocates and initializes the tree node with sanitized display name. */
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
	const struct sftp_dir_entry *e;
	size_t i;
	size_t count;
	struct sftp_tree_node **children;
	char full[SSH_SFTP_PATH_MAX];
	int error;

	/* Lists the node's remote directory via the worker cache. */
	if (view == NULL || view->worker == NULL || node == NULL)
		return;
	listing = NULL;
	error = sftp_worker_list_dir(view->worker, node->path, 0, &listing);
	if (error != 0 || listing == NULL)
		return;
	node->loaded = 1;
	if (listing->error != 0 || listing->count == 0U) {
		sftp_dir_listing_free(listing);
		return;
	}

	/* Allocates the child node pointer array. */
	children = calloc(listing->count, sizeof(*children));
	if (children == NULL) {
		sftp_dir_listing_free(listing);
		return;
	}

	/* Creates tree nodes for visible subdirectories. */
	count = 0U;
	for (i = 0U; i < listing->count; i++) {
		e = &listing->entries[i];
		if (!e->is_dir && !e->link_is_dir)
			continue;
		if (!view->show_hidden && e->name[0] == '.')
			continue;
		error = sftp_transfer_join_remote(full, sizeof(full), node->path, e->name);
		if (error != 0)
			continue;
		children[count] = view_alloc_node(e->name, full, node->depth + 1, e->is_link, node);
		if (children[count] != NULL)
			count++;
	}

	/* Replaces the node's previous children and frees the listing. */
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

	/* Appends node to the flattened array and recurses when expanded. */
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

	/* Keeps directories grouped ahead of files regardless of sort column. */
	ra = a;
	rb = b;
	da = ra->entry.is_dir || ra->entry.link_is_dir;
	db = rb->entry.is_dir || rb->entry.link_is_dir;
	if (da != db) {
		if (da)
			return -1;
		return 1;
	}

	/* Compares by the active sort column, breaking ties by name. */
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

	/* Reverses the comparison sign when descending sort is active. */
	if (view_sort_rev)
		return -cmp;
	return cmp;
}

/* Pushes path onto navigation history, discarding forward entries. */
static void
view_history_push(
	struct sftp_history *hist,
	const char *path)
{
	char **grown;
	size_t new_cap;
	size_t i;
	int same;

	/* Avoids pushing a duplicate of the current history entry. */
	if (hist->count > 0U && hist->index < hist->count) {
		same = strcmp(hist->paths[hist->index], path);
		if (same == 0)
			return;
	}

	/* Discards forward entries after the current index. */
	if (hist->count > 0U && hist->index + 1U < hist->count) {
		for (i = hist->index + 1U; i < hist->count; i++)
			free(hist->paths[i]);
		hist->count = hist->index + 1U;
	}

	/* Grows the history path array if full. */
	if (hist->count >= hist->capacity) {
		if (hist->capacity == 0U)
			new_cap = 16U;
		else
			new_cap = hist->capacity * 2U;
		grown = realloc(hist->paths, new_cap * sizeof(*grown));
		if (grown == NULL)
			return;
		hist->paths = grown;
		hist->capacity = new_cap;
	}

	/* Appends the duplicated path and advances the active index. */
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

	/* Frees every stored path string and resets the history struct. */
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

	/* Decodes 1-byte ASCII. */
	if (len == 0U)
		return 0U;
	if (s[0] < 0x80U) {
		*cp = s[0];
		return 1U;
	}

	/* Decodes 2-byte UTF-8 sequence. */
	if ((s[0] & 0xe0U) == 0xc0U) {
		if (len < 2U || (s[1] & 0xc0U) != 0x80U)
			return 0U;
		v = ((uint32_t)(s[0] & 0x1fU) << 6) | (uint32_t)(s[1] & 0x3fU);
		if (v < 0x80U)
			return 0U;
		*cp = v;
		return 2U;
	}

	/* Decodes 3-byte UTF-8 sequence, rejecting surrogates and overlongs. */
	if ((s[0] & 0xf0U) == 0xe0U) {
		if (len < 3U || (s[1] & 0xc0U) != 0x80U || (s[2] & 0xc0U) != 0x80U)
			return 0U;
		v = ((uint32_t)(s[0] & 0x0fU) << 12) | ((uint32_t)(s[1] & 0x3fU) << 6) | (uint32_t)(s[2] & 0x3fU);
		if (v < 0x800U || (v >= 0xd800U && v <= 0xdfffU))
			return 0U;
		*cp = v;
		return 3U;
	}

	/* Decodes 4-byte UTF-8 sequence, rejecting out-of-range code points. */
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

	/* Invalid leading UTF-8 byte. */
	return 0U;
}

/* Reports whether cp is a Unicode bidirectional override/isolate control. */
static int
view_is_bidi_control(
	uint32_t cp)
{
	/* Matches U+202A..U+202E and U+2066..U+2069. */
	if (cp >= 0x202aU && cp <= 0x202eU)
		return 1;
	if (cp >= 0x2066U && cp <= 0x2069U)
		return 1;
	return 0;
}
