/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * SFTP browser view tab for mikuterm: folder tree, detailed file list,
 * breadcrumb bar, transfer bar, modal dialogs, internal drag-move, and
 * drop upload.
 */

#ifndef MIKUTERM_SFTP_VIEW_H
#define MIKUTERM_SFTP_VIEW_H

#include "mikuterm.h"
#include "sftp-link.h"
#include "sftp-worker.h"

#include "userland/desktop/terminal/connection.h"

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

/* Post code used to wake the UI thread when the SFTP worker updates state. */
#define SFTP_VIEW_POST		0x73667470U
/* Post code used to launch OLE virtual-file drag-and-drop after pointer leave. */
#define SFTP_VIEW_DRAG_POST	0x73667464U

/* Sort columns for the file list. */
enum sftp_sort_column {
	SFTP_SORT_NAME = 0,
	SFTP_SORT_SIZE = 1,
	SFTP_SORT_MTIME = 2
};

/* Active modal dialog kind. */
enum sftp_dialog_kind {
	SFTP_DIALOG_NONE = 0,
	SFTP_DIALOG_NEW_FOLDER = 1,
	SFTP_DIALOG_DELETE = 2,
	SFTP_DIALOG_OVERWRITE = 3,
	SFTP_DIALOG_ERROR = 4
};

/* One node in the left folder tree. */
struct sftp_tree_node {
	char name[SSH_SFTP_PATH_MAX];
	char display[SSH_SFTP_PATH_MAX * 2U];
	char path[SSH_SFTP_PATH_MAX];
	int depth;
	int is_link;
	int expanded;
	int loaded;
	int loading;
	struct sftp_tree_node *parent;
	struct sftp_tree_node **children;
	size_t child_count;
};

/* One row in the right file list (points to listing entry with sanitized text). */
struct sftp_list_row {
	struct sftp_dir_entry entry;
	char display_name[SSH_SFTP_PATH_MAX * 2U];
	char display_target[SSH_SFTP_PATH_MAX * 2U];
	char size_text[32];
	char mtime_text[32];
	char perm_text[16];
	int selected;
};

/* Navigation history entry. */
struct sftp_history {
	char **paths;
	size_t count;
	size_t capacity;
	size_t index;
};

/* In-view context menu item. */
struct sftp_context_item {
	int id;
	const char *label;
	int enabled;
	int separator;
};

/* Context menu action IDs. */
#define SFTP_CTX_DOWNLOAD	1
#define SFTP_CTX_NEW_FOLDER	2
#define SFTP_CTX_RENAME		3
#define SFTP_CTX_DELETE		4
#define SFTP_CTX_COPY_PATH	5
#define SFTP_CTX_REFRESH	6
#define SFTP_CTX_TOGGLE_HIDDEN	7

/* Widget IDs for kl_ui. */
#define SFTP_UI_BACK		1U
#define SFTP_UI_FWD		2U
#define SFTP_UI_UP		3U
#define SFTP_UI_REFRESH		4U
#define SFTP_UI_NEW_FOLDER	5U
#define SFTP_UI_PATH_FIELD	6U
#define SFTP_UI_TREE_LIST	7U
#define SFTP_UI_FILE_LIST	8U
#define SFTP_UI_RENAME_FIELD	9U
#define SFTP_UI_TRANSFER_TOGGLE	10U
#define SFTP_UI_TRANSFER_CANCEL	11U
#define SFTP_UI_TRANSFER_CANCEL_ALL	12U
#define SFTP_UI_TRANSFER_LIST	13U
#define SFTP_UI_RETRY		14U
#define SFTP_UI_DIALOG_BASE	100U
#define SFTP_UI_DIALOG_FIELD	110U
#define SFTP_UI_TRANSFER_ITEM_BASE	200U

/* Layout constants (logical units at 96 dpi). */
#define SFTP_BAR_HEIGHT		42
#define SFTP_HEADER_HEIGHT	28
#define SFTP_ROW_HEIGHT		26
#define SFTP_TREE_ROW_HEIGHT	24
#define SFTP_SPLITTER_WIDTH	6
#define SFTP_SPLIT_DEFAULT	240
#define SFTP_SPLIT_MIN		120
#define SFTP_TRANSFER_BAR_H	38
#define SFTP_TRANSFER_PANEL_H	160
#define SFTP_STATUS_BAR_H	34

/*
 * State of one SFTP browser view tab.
 */
struct sftp_view {
	struct terminal_connection connection;
	struct kl_app *app;
	struct terminal_window *window;
	struct mikuterm_session *session;
	struct sftp_link *link;
	struct sftp_worker *worker;
	struct sftp_drag_context *drag_ctx;
	uint32_t number;
	char host[MIKUTERM_HOST_MAX];
	char title_text[MIKUTERM_HOST_MAX + 32U];

	struct kl_mutex lock;
	int dirty;
	int posted;
	int closed;
	int visible;

	/* Rendering state. */
	struct kl_ui *ui;
	struct kl_text text;
	int text_ready;
	struct kl_canvas canvas;
	int canvas_ready;
	unsigned scale;
	int width;
	int height;

	/* Worker status snapshot. */
	enum sftp_worker_state worker_state;
	char home_path[SSH_SFTP_PATH_MAX];
	char worker_error[256];
	int initial_loaded;

	/* Current directory and sorted/filtered rows. */
	char current_path[SSH_SFTP_PATH_MAX];
	struct sftp_list_row *rows;
	size_t row_count;
	long focus_row;
	long anchor_row;
	enum sftp_sort_column sort_column;
	int sort_reverse;
	int show_hidden;
	int list_truncated;
	char list_error[256];

	/* Navigation history and breadcrumb edit. */
	struct sftp_history history;
	int editing_path;
	struct kl_field path_field;

	/* Left folder tree. */
	struct sftp_tree_node *tree_root;
	struct sftp_tree_node **tree_flat;
	size_t tree_flat_count;
	struct kl_list tree_list;
	struct kl_list file_list;
	struct kl_list transfer_list;
	int split_width;
	int dragging_splitter;
	int split_drag_start_x;
	int split_drag_start_w;

	/* Inline rename. */
	int renaming;
	size_t rename_row;
	struct kl_field rename_field;

	/* Rubber-band selection. */
	int band_active;
	int band_x0;
	int band_y0;
	int band_x1;
	int band_y1;

	/* Internal drag-and-drop move. */
	int drag_pending;
	int drag_active;
	int drag_start_x;
	int drag_start_y;
	int pointer_x;
	int pointer_y;
	uint32_t modifiers;
	char drop_target_path[SSH_SFTP_PATH_MAX];

	/* External drag-over state. */
	int ext_drop_active;

	/* Outbound virtual-file drag state. */
	int drag_start_pending;
	int drag_error_pending;
	char drag_error[256];

	/* Transfer bar and snapshot. */
	struct sftp_transfer_snapshot *transfers;
	size_t transfer_count;
	size_t active_transfers;
	int transfers_expanded;
	uint64_t last_progress_id;
	uint64_t last_progress_bytes;
	uint64_t last_progress_us;
	double transfer_speed;

	/* Modal dialog. */
	enum sftp_dialog_kind dialog;
	char dialog_title[128];
	char dialog_body[512];
	struct kl_field dialog_field;
	uint64_t dialog_transfer_id;
	char dialog_delete_path[SSH_SFTP_PATH_MAX];
	size_t dialog_delete_count;
	char **dialog_delete_paths;

	/* Context menu. */
	int ctx_open;
	int ctx_x;
	int ctx_y;
	struct sftp_context_item ctx_items[8];
	size_t ctx_count;
	int ctx_hover;

	/* Cached hit rectangles (logical units) for input and drop hit-testing. */
	struct kl_rect rect_tree;
	struct kl_rect rect_splitter;
	struct kl_rect rect_header;
	struct kl_rect rect_list;
	struct kl_rect rect_breadcrumb;
	struct kl_rect rect_transfer_bar;
	struct kl_rect rect_transfer_panel;
	struct kl_rect rect_ctx;
};

int sftp_view_create(struct kl_app *app, struct mikuterm_session *session, struct sftp_link *link, const char *host, int preserve_times, int show_hidden, const char *font_path, const char *fallback_path, struct terminal_connection **connection_out);
int sftp_view_confirm_close(struct terminal_connection *connection, struct terminal_window *window);
void sftp_view_sanitize_text(const char *raw, char *out, size_t out_cap);

/* Internal helpers shared across sftp-view*.c. */
void sftp_view_request_redraw(struct sftp_view *view);
void sftp_view_navigate(struct sftp_view *view, const char *path, int record_history, int force_refresh);
void sftp_view_navigate_up(struct sftp_view *view);
void sftp_view_navigate_back(struct sftp_view *view);
void sftp_view_navigate_forward(struct sftp_view *view);
void sftp_view_refresh_all(struct sftp_view *view);
void sftp_view_toggle_tree_node(struct sftp_view *view, struct sftp_tree_node *node);
void sftp_view_reveal_tree_path(struct sftp_view *view, const char *path);
void sftp_view_rebuild_tree_flat(struct sftp_view *view);
void sftp_view_sort_rows(struct sftp_view *view);
void sftp_view_begin_rename(struct sftp_view *view);
void sftp_view_commit_rename(struct sftp_view *view);
void sftp_view_begin_new_folder(struct sftp_view *view);
void sftp_view_begin_delete(struct sftp_view *view);
void sftp_view_download_selected(struct sftp_view *view);
void sftp_view_select_all(struct sftp_view *view);
void sftp_view_toggle_hidden(struct sftp_view *view);
void sftp_view_show_error(struct sftp_view *view, const char *title, const char *message);
void sftp_view_save_split(const struct sftp_view *view);
int sftp_view_load_split(void);
void sftp_view_format_size(uint64_t bytes, char *out, size_t cap);
void sftp_view_format_mtime(uint32_t mtime, char *out, size_t cap);
void sftp_view_format_perm(uint32_t mode, int is_dir, int is_link, char *out, size_t cap);
void sftp_view_execute_ctx(struct sftp_view *view, int id);
void sftp_view_move_selected_to(struct sftp_view *view, const char *target_dir);
void sftp_view_request_external_drag(struct sftp_view *view);
int sftp_view_hit_folder_at(struct sftp_view *view, int ux, int uy, char *path_out, size_t path_cap);

int sftp_view_draw_frame(struct sftp_view *view, const struct terminal_view_state *state, uint32_t *pixels, uint32_t width, uint32_t height);
void sftp_view_handle_input(struct sftp_view *view, const struct terminal_picture_event *event);
unsigned sftp_view_handle_drop(struct sftp_view *view, struct terminal_window *window, const struct terminal_drop_event *event);

#endif
