/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The file chooser's model (ws090-p006, libkeiland's chooser-model.c of
 * ws092-p003 moved here): the folder shown and its items, the sidebar's
 * places, the filters, the name and the path typed, and the checks and the
 * question before the answer.
 *
 * Nothing here knows Wayland or draws; the view (chooser-view.c) carries
 * out what the widgets report through these calls.  The list's selection
 * and scroll are a kl_list's, the name and the path kl_fields.
 */

#include "chooser.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* How many items the array grows by at first. */
#define MODEL_ENTRIES_FIRST	64U

static void model_places(struct keiui_chooser *chooser);
static void model_add_place(struct keiui_chooser *chooser, const char *label, const char *path, enum kl_icon icon);
static int model_is_folder(const char *path);
static int model_read(struct keiui_chooser *chooser, const char *folder);
static int model_add_entry(struct keiui_chooser *chooser, const char *name, const char *path, const struct stat *status);
static void model_clear(struct keiui_chooser *chooser);
static int model_compare(const void *left, const void *right);
static int model_matches(const struct keiui_chooser *chooser, const char *name);
static void model_reload(struct keiui_chooser *chooser);
static void model_shown(struct keiui_chooser *chooser);
static void model_accept_save(struct keiui_chooser *chooser);
static void model_answer(struct keiui_chooser *chooser, const char *path);
static void model_join(const char *folder, const char *name, char *out, size_t size);
static void model_message(struct keiui_chooser *chooser, const char *format, const char *name);
static void model_field_stem(struct kl_field *field, const char *text);
static const char *model_base(const char *path);

/*
 * Makes a chooser's model from the options an application gave, and lists
 * the folder it starts in.
 *
 * Returns 0, EINVAL for options it cannot follow, or an errno value of the
 * list's scroll.
 */
int
keiui_chooser_init(
	struct keiui_chooser *chooser,
	const struct kl_file_chooser_options *options)
{
	const struct kl_file_filter *filter;
	const char *start;
	size_t index;
	int folder;
	int error;

	/* Nothing is set yet. */
	memset(chooser, 0, sizeof(*chooser));

	/* Only documented modes and filters within the bounds. */
	if (options == NULL)
		return EINVAL;
	if (options->mode != KL_FILE_CHOOSER_OPEN &&
	    options->mode != KL_FILE_CHOOSER_SAVE &&
	    options->mode != KL_FILE_CHOOSER_MEDIA &&
	    options->mode != KL_FILE_CHOOSER_FOLDER)
		return EINVAL;
	if (options->mode == KL_FILE_CHOOSER_MEDIA && options->filter_count != 0U)
		return EINVAL;
	if (options->filter_count > KL_FILE_CHOOSER_FILTERS_MAX)
		return EINVAL;
	if (options->filter_count > 0U && options->filters == NULL)
		return EINVAL;
	if (options->filter_count > 0U && options->filter >= options->filter_count)
		return EINVAL;
	chooser->mode = options->mode;

	/* The list's selection and scroll. */
	error = kl_list_init(&chooser->list);
	if (error != 0)
		return error;

	/* The window's title, given or the mode's. */
	if (options->title != NULL) {
		snprintf(chooser->title, sizeof(chooser->title), "%s", options->title);
	} else if (chooser->mode == KL_FILE_CHOOSER_SAVE) {
		snprintf(chooser->title, sizeof(chooser->title), "Save As");
	} else {
		snprintf(chooser->title, sizeof(chooser->title), "Open");
	}

	/* The filters, copied (the application's strings need not outlive the call). */
	for (index = 0; index < options->filter_count; index++) {
		filter = &options->filters[index];
		if (filter->label != NULL)
			snprintf(chooser->filters[index].label, sizeof(chooser->filters[index].label), "%s", filter->label);
		if (filter->extensions != NULL)
			snprintf(chooser->filters[index].extensions, sizeof(chooser->filters[index].extensions), "%s", filter->extensions);
	}

	/* How many there are, and the one chosen first. */
	chooser->filter_count = options->filter_count;
	chooser->filter = options->filter;

	/* A media chooser has one virtual location and receives its entries as metadata. */
	if (chooser->mode == KL_FILE_CHOOSER_MEDIA) {
		model_add_place(chooser, "Media Library", "Media Library", KL_ICON_PICTURES);
		snprintf(chooser->folder, sizeof(chooser->folder), "Media Library");
		chooser->want_focus = KEIUI_CHOOSER_ID_LIST;
		return 0;
	}

	/* The places of the sidebar. */
	model_places(chooser);

	/* Save starts with the name given, selected up to its extension, and types into it; Open types into the list. */
	chooser->want_focus = KEIUI_CHOOSER_ID_LIST;
	if (chooser->mode == KL_FILE_CHOOSER_SAVE) {
		if (options->name != NULL)
			model_field_stem(&chooser->name, options->name);
		chooser->want_focus = KEIUI_CHOOSER_ID_NAME;
	}

	/* The folder given when it is one, else the home folder, else the root. */
	start = options->folder;
	folder = 0;
	if (start != NULL)
		folder = model_is_folder(start);
	if (!folder)
		start = keiui_chooser_home();

	/* Its items. */
	error = keiui_chooser_go(chooser, start);
	if (error != 0)
		(void)keiui_chooser_go(chooser, "/");

	/* Succeeded: the chooser shows its first folder. */
	return 0;
}

/*
 * Frees what a chooser's model holds.
 */
void
keiui_chooser_fini(
	struct keiui_chooser *chooser)
{
	/* The items and their array. */
	model_clear(chooser);
	free(chooser->entries);
	chooser->entries = NULL;
	chooser->capacity = 0;

	/* The list's scroll. */
	kl_list_release(&chooser->list);
}

/*
 * Shows a folder: its items, none selected, scrolled to the top.
 *
 * Returns 0, or the errno value of finding the folder (the folder shown
 * stays then).  A folder found but not readable is shown empty with the
 * reason.
 */
int
keiui_chooser_go(
	struct keiui_chooser *chooser,
	const char *path)
{
	char resolved[KEIUI_CHOOSER_PATH_MAX];
	char *found;
	int folder;

	/* Media entries come only from the compositor snapshot. */
	if (chooser->mode == KL_FILE_CHOOSER_MEDIA)
		return ENOTSUP;

	/* The folder's real path. */
	found = realpath(path, resolved);
	if (found == NULL)
		return errno;

	/* Only a folder can be shown. */
	folder = model_is_folder(resolved);
	if (!folder)
		return ENOTDIR;

	/* The folder, then its items. */
	snprintf(chooser->folder, sizeof(chooser->folder), "%s", resolved);
	chooser->recent = 0;
	chooser->list_error = model_read(chooser, chooser->folder);

	/* Nothing selected, at the top, and no message left from before. */
	model_shown(chooser);

	/* Succeeded: the folder is shown. */
	return 0;
}

/*
 * Shows the recent files (for Open): the ones that still exist and pass
 * the filter, newest first.
 */
void
keiui_chooser_go_recent(
	struct keiui_chooser *chooser)
{
	struct kl_recent_item *items;
	struct stat status;
	size_t count;
	size_t index;
	int regular;
	int matches;
	int error;

	/* Media entries come only from the compositor snapshot. */
	if (chooser->mode == KL_FILE_CHOOSER_MEDIA ||
	    chooser->mode == KL_FILE_CHOOSER_FOLDER)
		return;

	/* The folder's items go; Recent is shown even when it cannot be read. */
	model_clear(chooser);
	chooser->recent = 1;
	chooser->list_error = 0;
	model_shown(chooser);

	/* Room for the list. */
	items = calloc(KEIUI_CHOOSER_RECENT_MAX, sizeof(*items));
	if (items == NULL) {
		chooser->list_error = ENOMEM;
		return;
	}

	/* The list, newest first. */
	count = 0;
	error = kl_recent_list(items, KEIUI_CHOOSER_RECENT_MAX, &count);
	if (error != 0) {
		free(items);
		return;
	}

	/* Each file that is still a file and passes the filter. */
	for (index = 0; index < count; index++) {
		error = stat(items[index].path, &status);
		if (error != 0)
			continue;
		regular = S_ISREG(status.st_mode);
		if (!regular)
			continue;
		matches = model_matches(chooser, model_base(items[index].path));
		if (!matches)
			continue;
		error = model_add_entry(chooser, model_base(items[index].path), items[index].path, &status);
		if (error != 0)
			break;
	}

	/* The list read is not needed any more. */
	free(items);
}

/*
 * Shows the folder that holds the one shown (nothing above the root or
 * above Recent).
 */
void
keiui_chooser_go_up(
	struct keiui_chooser *chooser)
{
	char parent[KEIUI_CHOOSER_PATH_MAX];
	char *slash;
	int above;

	/* Recent and the root have nothing above them. */
	above = keiui_chooser_can_go_up(chooser);
	if (!above)
		return;

	/* The path without its last part. */
	snprintf(parent, sizeof(parent), "%s", chooser->folder);
	slash = strrchr(parent, '/');
	if (slash == NULL)
		return;
	if (slash == parent)
		slash[1] = '\0';
	else
		*slash = '\0';

	/* That folder. */
	(void)keiui_chooser_go(chooser, parent);
}

/*
 * Shows a place of the sidebar (Recent or a folder).
 */
void
keiui_chooser_go_place(
	struct keiui_chooser *chooser,
	size_t place)
{
	/* Media entries come only from the compositor snapshot. */
	if (chooser->mode == KL_FILE_CHOOSER_MEDIA)
		return;

	/* No such place. */
	if (place >= chooser->place_count)
		return;

	/* Recent has no folder. */
	if (chooser->places[place].path[0] == '\0') {
		keiui_chooser_go_recent(chooser);
		return;
	}

	/* A folder. */
	(void)keiui_chooser_go(chooser, chooser->places[place].path);
}

/*
 * Selects an item (-1: none) and scrolls it into sight; a file selected in
 * Save gives its name.
 */
void
keiui_chooser_select(
	struct keiui_chooser *chooser,
	long index)
{
	const struct kl_theme *theme;
	struct keiui_chooser_entry *entry;
	struct kl_rect row;

	/* Only an item of the list, or none. */
	if (index < -1L || index >= (long)chooser->count)
		return;

	/* The selection. */
	chooser->list.selected = index;
	if (index < 0L)
		return;

	/* In sight. */
	theme = kl_theme_default();
	row.x = 0;
	row.y = (int)index * theme->row_height;
	row.width = 1;
	row.height = theme->row_height;
	kl_scroll_reveal(&chooser->list.scroll, &row, chooser->now_us);

	/* Save takes a file's name as the name to save as. */
	entry = &chooser->entries[index];
	if (chooser->mode == KL_FILE_CHOOSER_SAVE && !entry->folder)
		model_field_stem(&chooser->name, entry->name);
}

/*
 * Activates an item (a double click, a double tap, Enter): a folder is
 * gone into, a file is chosen (Save asks first when it would be replaced).
 */
void
keiui_chooser_activate(
	struct keiui_chooser *chooser,
	long index)
{
	char path[KEIUI_CHOOSER_PATH_MAX];

	/* Only an item. */
	if (index < 0L || index >= (long)chooser->count)
		return;

	/* A folder is gone into. */
	if (chooser->entries[index].folder) {
		snprintf(path, sizeof(path), "%s", chooser->entries[index].path);
		(void)keiui_chooser_go(chooser, path);
		return;
	}

	/* A file is chosen. */
	keiui_chooser_select(chooser, index);
	keiui_chooser_accept(chooser);
}

/*
 * The main button (Open, Save) or Enter: into a folder selected, or the
 * answer.
 */
void
keiui_chooser_accept(
	struct keiui_chooser *chooser)
{
	struct keiui_chooser_entry *entry;
	char path[KEIUI_CHOOSER_PATH_MAX];
	long selected;

	/* Folder selection accepts the highlighted directory, or the directory being browsed. */
	selected = chooser->list.selected;
	if (chooser->mode == KL_FILE_CHOOSER_FOLDER) {
		if (chooser->recent || chooser->list_error != 0)
			return;
		if (selected >= 0L && selected < (long)chooser->count) {
			if (!chooser->entries[selected].folder)
				return;
			model_answer(chooser, chooser->entries[selected].path);
		} else {
			model_answer(chooser, chooser->folder);
		}

		/* Succeeded: the chosen directory is passed as a path, not a file's parent. */
		return;
	}

	/* A folder selected is gone into. */
	if (selected >= 0L && selected < (long)chooser->count) {
		entry = &chooser->entries[selected];
		if (entry->folder) {
			snprintf(path, sizeof(path), "%s", entry->path);
			(void)keiui_chooser_go(chooser, path);
			return;
		}
	}

	/* Save checks the name and the folder. */
	if (chooser->mode == KL_FILE_CHOOSER_SAVE) {
		model_accept_save(chooser);
		return;
	}

	/* Open chooses the file selected. */
	if (selected < 0L || selected >= (long)chooser->count)
		return;
	model_answer(chooser, chooser->entries[selected].path);
}

/*
 * The path typed (Ctrl+L): a folder is shown, a file is chosen (Open) or
 * named (Save).
 */
void
keiui_chooser_accept_path(
	struct keiui_chooser *chooser)
{
	char path[KEIUI_CHOOSER_PATH_MAX];
	char folder[KEIUI_CHOOSER_PATH_MAX];
	const char *typed;
	struct stat status;
	char *slash;
	int folder_named;
	int regular;
	int home;
	int error;

	/* The path: ~ is home, and a relative one is from the folder shown. */
	typed = chooser->path.text;
	home = 0;
	if (typed[0] == '~') {
		if (typed[1] == '/' || typed[1] == '\0')
			home = 1;
	}

	/* The path made whole. */
	if (home) {
		snprintf(path, sizeof(path), "%s%s", keiui_chooser_home(), typed + 1);
	} else if (typed[0] == '/' || chooser->recent) {
		snprintf(path, sizeof(path), "%s", typed);
	} else {
		model_join(chooser->folder, typed, path, sizeof(path));
	}

	/* What the path names. */
	error = stat(path, &status);
	folder_named = 0;
	regular = 0;
	if (error == 0) {
		folder_named = S_ISDIR(status.st_mode);
		regular = S_ISREG(status.st_mode);
	}

	/* A directory typed is a valid folder answer; files cannot be imported as folders. */
	if (chooser->mode == KL_FILE_CHOOSER_FOLDER) {
		if (!folder_named) {
			model_message(
			    chooser, "There is no folder \"%s\".", path);
			return;
		}

		/* A typed directory is returned directly rather than opened as a file. */
		model_answer(chooser, path);
		return;
	}

	/* A folder is shown, and the keyboard goes back. */
	if (folder_named) {
		(void)keiui_chooser_go(chooser, path);
		keiui_chooser_close_path(chooser);
		return;
	}

	/* Open chooses a file that exists. */
	if (chooser->mode == KL_FILE_CHOOSER_OPEN) {
		if (!regular) {
			model_message(chooser, "There is no file \"%s\".", path);
			return;
		}

		/* Succeeded: the file typed is the answer. */
		/* A typed directory is returned directly rather than opened as a file. */
		model_answer(chooser, path);
		return;
	}

	/* Save: the folder part is shown and the last part becomes the name. */
	snprintf(folder, sizeof(folder), "%s", path);
	slash = strrchr(folder, '/');
	if (slash == NULL) {
		model_message(chooser, "There is no folder for \"%s\".", path);
		return;
	}

	/* The folder part: the root keeps its slash. */
	if (slash == folder)
		slash[1] = '\0';
	else
		*slash = '\0';
	error = keiui_chooser_go(chooser, folder);
	if (error != 0) {
		model_message(chooser, "There is no folder \"%s\".", folder);
		return;
	}

	/* The name, and Save goes on as if it was typed there. */
	kl_field_set(&chooser->name, model_base(path));
	keiui_chooser_close_path(chooser);
	model_accept_save(chooser);
}

/*
 * Opens the field for a path, holding the folder shown (the home folder
 * for Recent), with the keyboard.
 */
void
keiui_chooser_open_path(
	struct keiui_chooser *chooser)
{
	char text[KEIUI_CHOOSER_PATH_MAX];
	const char *folder;

	/* Media entries come only from the compositor snapshot. */
	if (chooser->mode == KL_FILE_CHOOSER_MEDIA)
		return;

	/* The folder with a slash after it, ready for a name. */
	folder = chooser->folder;
	if (chooser->recent)
		folder = keiui_chooser_home();
	model_join(folder, "", text, sizeof(text));

	/* The field has the keyboard, its caret at the end. */
	kl_field_set(&chooser->path, text);
	chooser->typing_path = 1;
	chooser->want_focus = KEIUI_CHOOSER_ID_PATH;
}

/*
 * Closes the path's field; the keyboard goes back to the name (Save) or
 * the list (Open).
 */
void
keiui_chooser_close_path(
	struct keiui_chooser *chooser)
{
	/* The location shows again. */
	chooser->typing_path = 0;
	chooser->want_focus = KEIUI_CHOOSER_ID_LIST;
	if (chooser->mode == KL_FILE_CHOOSER_SAVE)
		chooser->want_focus = KEIUI_CHOOSER_ID_NAME;
}

/*
 * Shows the next filter (round to the first).
 */
void
keiui_chooser_next_filter(
	struct keiui_chooser *chooser)
{
	/* Only a choice of filters. */
	if (chooser->filter_count < 2U)
		return;

	/* The next, and the place listed again with it. */
	chooser->filter = (chooser->filter + 1U) % chooser->filter_count;
	model_reload(chooser);
}

/*
 * Shows or hides the hidden items (Ctrl+H).
 */
void
keiui_chooser_toggle_hidden(
	struct keiui_chooser *chooser)
{
	/* The other way, and the place listed again. */
	chooser->show_hidden = !chooser->show_hidden;
	model_reload(chooser);
}

/*
 * Selects the next item after the one selected whose name starts with a
 * character.
 */
void
keiui_chooser_type_select(
	struct keiui_chooser *chooser,
	uint32_t character)
{
	size_t tried;
	size_t index;
	char letter[2];
	int same;

	/* Nothing to find in an empty list, and only a character of one byte. */
	if (chooser->count == 0U || character == 0U || character > 0x7fU)
		return;

	/* The character as a one-letter string. */
	letter[0] = (char)character;
	letter[1] = '\0';

	/* Each item after the one selected, round to it. */
	index = 0;
	if (chooser->list.selected >= 0L)
		index = (size_t)chooser->list.selected + 1U;
	for (tried = 0; tried < chooser->count; tried++) {
		index %= chooser->count;
		same = strncasecmp(chooser->entries[index].name, letter, 1U);
		if (same == 0) {
			keiui_chooser_select(chooser, (long)index);
			return;
		}

		/* The item after it. */
		index++;
	}
}

/*
 * Answers the question with Replace: the file is the answer.
 */
void
keiui_chooser_replace(
	struct keiui_chooser *chooser)
{
	/* Only while it is asked. */
	if (!chooser->confirm)
		return;
	model_answer(chooser, chooser->confirm_path);
}

/*
 * Answers the question with Cancel: the file is kept, and the name can be
 * changed.
 */
void
keiui_chooser_keep(
	struct keiui_chooser *chooser)
{
	/* The question goes; the name has the keyboard again. */
	chooser->confirm = 0;
	chooser->want_focus = KEIUI_CHOOSER_ID_NAME;
}

/*
 * Ends the chooser without a path.
 */
void
keiui_chooser_cancel(
	struct keiui_chooser *chooser)
{
	/* An answer is given once. */
	if (chooser->answered)
		return;

	/* Cancelled, with no path. */
	chooser->answered = 1;
	chooser->result = KL_FILE_CHOOSER_CANCELLED;
	chooser->answer[0] = '\0';
}

/*
 * Tells whether the main button (Open, Save) can be pressed now.
 */
int
keiui_chooser_can_accept(
	const struct keiui_chooser *chooser)
{
	long selected;

	/* A folder chooser can accept the current directory even with no highlighted child. */
	selected = chooser->list.selected;
	if (chooser->mode == KL_FILE_CHOOSER_FOLDER) {
		if (chooser->recent || chooser->list_error != 0)
			return 0;
		if (selected >= 0L && selected < (long)chooser->count) {
			if (!chooser->entries[selected].folder)
				return 0;
		}

		/* Succeeded: a directory is available without requiring a file. */
		return 1;
	}

	/* A selected folder can always be gone into. */
	if (selected >= 0L && selected < (long)chooser->count && chooser->entries[selected].folder)
		return 1;

	/* Save needs a name. */
	if (chooser->mode == KL_FILE_CHOOSER_SAVE) {
		if (chooser->name.length == 0U)
			return 0;
		return 1;
	}

	/* Open needs a file selected. */
	if (selected < 0L || selected >= (long)chooser->count)
		return 0;
	return 1;
}

/*
 * Tells whether there is a folder above the one shown (none above the root
 * or Recent).
 */
int
keiui_chooser_can_go_up(
	const struct keiui_chooser *chooser)
{
	int root;

	/* Media entries come only from the compositor snapshot. */
	if (chooser->mode == KL_FILE_CHOOSER_MEDIA)
		return 0;

	/* Recent has nothing above it. */
	if (chooser->recent)
		return 0;

	/* Nor has the root. */
	root = strcmp(chooser->folder, "/");
	if (root == 0)
		return 0;
	return 1;
}

/*
 * Tells whether a place of the sidebar is the one shown.
 */
int
keiui_chooser_is_place(
	const struct keiui_chooser *chooser,
	size_t place)
{
	int same;

	/* Recent is shown by its own flag. */
	if (chooser->places[place].path[0] == '\0')
		return chooser->recent;

	/* A folder, when it is the one shown. */
	if (chooser->recent)
		return 0;
	same = strcmp(chooser->places[place].path, chooser->folder);
	if (same == 0)
		return 1;
	return 0;
}

/*
 * Reports the home folder: $HOME, or the root without one.
 */
const char *
keiui_chooser_home(void)
{
	const char *home;

	/* The environment's. */
	home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		return "/";

	/* Succeeded: the home folder. */
	return home;
}

/*
 * Lists media names and absolute paths from a compositor metadata snapshot.
 */
int
keiui_chooser_media(
	struct keiui_chooser *chooser,
	int descriptor)
{
	struct stat status;
	FILE *input;
	char line[16384];
	char *fields[12];
	char *part;
	char *got;
	size_t count;
	size_t length;
	int copy;
	int same;
	int error;

	/* The caller retains its descriptor; the stream owns a separate duplicate. */
	if (chooser->mode != KL_FILE_CHOOSER_MEDIA)
		return EINVAL;
	copy = dup(descriptor);
	if (copy < 0)
		return errno;
	input = fdopen(copy, "r");
	if (input == NULL) {
		error = errno;
		close(copy);
		return error;
	}

	/* Requires the versioned metadata header before accepting any path. */
	got = fgets(line, sizeof(line), input);
	error = EPROTO;
	if (got != NULL) {
		same = strncmp(line, "# keiland-media 1\t", 18U);
		if (same == 0)
			error = 0;
	}

	/* Replaces the virtual list with complete metadata rows; media bytes stay in files. */
	model_clear(chooser);
	while (error == 0) {
		got = fgets(line, sizeof(line), input);
		if (got == NULL)
			break;
		length = strlen(line);
		if (length == 0U || line[length - 1U] != '\n') {
			error = EPROTO;
			break;
		}

		/* Only media rows become selectable; album rows belong to the Photos view. */
		if (line[0] != 'P' || line[1] != '\t')
			continue;
		line[length - 1U] = '\0';
		count = 0U;
		part = line;
		while (part != NULL && count < 12U) {
			fields[count++] = part;
			part = strchr(part, '\t');
			if (part != NULL)
				*part++ = '\0';
		}

		/* The snapshot's twelve fields include the original name and an absolute path. */
		if (count != 12U || part != NULL || fields[2][0] != '/') {
			error = EPROTO;
			break;
		}

		/* Sortable size and capture time come from metadata, not another database read. */
		memset(&status, 0, sizeof(status));
		status.st_mode = S_IFREG;
		status.st_size = (off_t)strtoll(fields[4], NULL, 10);
		status.st_mtime = (time_t)strtoll(fields[5], NULL, 10);
		error = model_add_entry(chooser, fields[11], fields[2], &status);
	}

	/* Stream failures or malformed snapshots never leave a selectable partial list. */
	same = ferror(input);
	if (same != 0 && error == 0)
		error = EIO;
	fclose(input);
	if (error != 0) {
		model_clear(chooser);
		model_shown(chooser);
		return error;
	}

	/* Shows the complete library in the ordinary chooser's name order. */
	if (chooser->count > 1U)
		qsort(chooser->entries, chooser->count, sizeof(chooser->entries[0]), model_compare);
	model_shown(chooser);

	/* Succeeded: selecting a row returns its original file path. */
	return 0;
}

/* Makes the sidebar's places: Recent (Open), Home and its usual folders, and the root. */
static void
model_places(
	struct keiui_chooser *chooser)
{
	static const char *const names[] = { "Desktop", "Documents", "Downloads" };
	static const enum kl_icon icons[] = { KL_ICON_DESKTOP, KL_ICON_DOCUMENTS, KL_ICON_DOWNLOADS };
	char path[KEIUI_CHOOSER_PATH_MAX];
	const char *home;
	size_t index;
	int folder;
	int root;

	/* Recent files, which only Open can choose from. */
	chooser->place_count = 0;
	if (chooser->mode == KL_FILE_CHOOSER_OPEN)
		model_add_place(chooser, "Recent", "", KL_ICON_RECENTS);

	/* Home, when there is one. */
	home = keiui_chooser_home();
	folder = model_is_folder(home);
	root = strcmp(home, "/");
	if (folder && root != 0) {
		model_add_place(chooser, "Home", home, KL_ICON_HOME);

		/* Its usual folders that exist. */
		for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
			model_join(home, names[index], path, sizeof(path));
			folder = model_is_folder(path);
			if (folder)
				model_add_place(chooser, names[index], path, icons[index]);
		}
	}

	/* The whole computer. */
	model_add_place(chooser, "Computer", "/", KL_ICON_COMPUTER);
}

/* Adds a place to the sidebar, when there is room. */
static void
model_add_place(
	struct keiui_chooser *chooser,
	const char *label,
	const char *path,
	enum kl_icon icon)
{
	struct keiui_chooser_place *place;

	/* No room, no place. */
	if (chooser->place_count >= KEIUI_CHOOSER_PLACES_MAX)
		return;

	/* The next place. */
	place = &chooser->places[chooser->place_count];
	snprintf(place->label, sizeof(place->label), "%s", label);
	snprintf(place->path, sizeof(place->path), "%s", path);
	place->icon = icon;
	chooser->place_count++;
}

/* Tells whether a path names a folder. */
static int
model_is_folder(
	const char *path)
{
	struct stat status;
	int folder;
	int error;

	/* What the path names. */
	error = stat(path, &status);
	if (error != 0)
		return 0;

	/* Only a folder. */
	folder = S_ISDIR(status.st_mode);
	if (!folder)
		return 0;

	/* Succeeded: a folder. */
	return 1;
}

/* Reads a folder's items that pass the filter, sorted; returns 0 or why it could not be read. */
static int
model_read(
	struct keiui_chooser *chooser,
	const char *folder)
{
	char path[KEIUI_CHOOSER_PATH_MAX];
	struct dirent *item;
	struct stat status;
	DIR *directory;
	int matches;
	int directory_item;
	int same;
	int error;

	/* The items of the folder shown before go. */
	model_clear(chooser);

	/* The folder. */
	directory = opendir(folder);
	if (directory == NULL)
		return errno;

	/* Each item but the folder itself and its parent. */
	for (;;) {
		item = readdir(directory);
		if (item == NULL)
			break;
		same = strcmp(item->d_name, ".");
		if (same == 0)
			continue;
		same = strcmp(item->d_name, "..");
		if (same == 0)
			continue;

		/* A hidden item only when hidden items show. */
		if (item->d_name[0] == '.' && !chooser->show_hidden)
			continue;

		/* What it is (an item that went meanwhile is skipped). */
		model_join(folder, item->d_name, path, sizeof(path));
		error = stat(path, &status);
		if (error != 0)
			continue;

		/* A file shows only when it passes the filter. */
		directory_item = S_ISDIR(status.st_mode);
		if (!directory_item) {
			/* Directory mode never offers a regular file as a selectable folder. */
			if (chooser->mode == KL_FILE_CHOOSER_FOLDER)
				continue;

			/* File filtering applies only after folder-only mode has excluded files. */
			matches = model_matches(chooser, item->d_name);
			if (!matches)
				continue;
		}

		/* The item. */
		error = model_add_entry(chooser, item->d_name, path, &status);
		if (error != 0)
			break;
	}

	/* The folder is read. */
	closedir(directory);

	/* Folders first, then by name. */
	if (chooser->count > 1U)
		qsort(chooser->entries, chooser->count, sizeof(chooser->entries[0]), model_compare);

	/* Succeeded: the folder's items are listed. */
	return 0;
}

/* Adds one item to the list; 0 or ENOMEM. */
static int
model_add_entry(
	struct keiui_chooser *chooser,
	const char *name,
	const char *path,
	const struct stat *status)
{
	struct keiui_chooser_entry *grown;
	struct keiui_chooser_entry *entry;
	size_t capacity;
	int folder;

	/* Room for one more. */
	if (chooser->count == chooser->capacity) {
		capacity = chooser->capacity * 2U;
		if (capacity == 0U)
			capacity = MODEL_ENTRIES_FIRST;
		grown = realloc(chooser->entries, capacity * sizeof(chooser->entries[0]));
		if (grown == NULL)
			return ENOMEM;
		chooser->entries = grown;
		chooser->capacity = capacity;
	}

	/* The item's name. */
	entry = &chooser->entries[chooser->count];
	entry->name = strdup(name);
	if (entry->name == NULL)
		return ENOMEM;

	/* Its whole path. */
	entry->path = strdup(path);
	if (entry->path == NULL) {
		free(entry->name);
		return ENOMEM;
	}

	/* What it is, how large, and when it changed. */
	entry->folder = 0;
	folder = S_ISDIR(status->st_mode);
	if (folder)
		entry->folder = 1;
	entry->size = (int64_t)status->st_size;
	entry->modified = (int64_t)status->st_mtime;
	chooser->count++;

	/* Succeeded: listed. */
	return 0;
}

/* Forgets the items listed. */
static void
model_clear(
	struct keiui_chooser *chooser)
{
	size_t index;

	/* Each item's strings. */
	for (index = 0; index < chooser->count; index++) {
		free(chooser->entries[index].name);
		free(chooser->entries[index].path);
	}

	/* No items. */
	chooser->count = 0;
}

/* Orders items: folders before files, then by name without regard to case. */
static int
model_compare(
	const void *left,
	const void *right)
{
	const struct keiui_chooser_entry *first;
	const struct keiui_chooser_entry *second;
	int order;

	/* A folder goes before a file. */
	first = left;
	second = right;
	if (first->folder != second->folder) {
		if (first->folder)
			return -1;
		return 1;
	}

	/* Then the names, ignoring case. */
	order = strcasecmp(first->name, second->name);
	if (order != 0)
		return order;

	/* Names that differ only in case, by their bytes. */
	order = strcmp(first->name, second->name);
	return order;
}

/* Tells whether a file's name passes the filter chosen. */
static int
model_matches(
	const struct keiui_chooser *chooser,
	const char *name)
{
	const char *extensions;
	const char *extension;
	const char *word;
	size_t length;
	size_t size;
	int same;

	/* Without filters every file passes, and so it does with an empty filter. */
	if (chooser->filter_count == 0U)
		return 1;
	extensions = chooser->filters[chooser->filter].extensions;
	if (extensions[0] == '\0')
		return 1;

	/* The name's extension: after its last dot, which is not its first character. */
	extension = strrchr(name, '.');
	if (extension == NULL || extension == name)
		return 0;
	extension++;
	length = strlen(extension);

	/* Each word of the filter, compared with it. */
	word = extensions;
	while (*word != '\0') {
		/* The spaces between words. */
		while (*word == ' ')
			word++;
		size = strcspn(word, " ");
		if (size == 0U)
			break;

		/* The same letters, whatever their case. */
		if (size == length) {
			same = strncasecmp(word, extension, size);
			if (same == 0)
				return 1;
		}

		/* The next word. */
		word += size;
	}

	/* No word matched. */
	return 0;
}

/* Lists the place shown again (after the filter or the hidden items changed). */
static void
model_reload(
	struct keiui_chooser *chooser)
{
	char folder[KEIUI_CHOOSER_PATH_MAX];

	/* Media entries come only from the compositor snapshot. */
	if (chooser->mode == KL_FILE_CHOOSER_MEDIA)
		return;

	/* Recent, or the folder. */
	if (chooser->recent) {
		keiui_chooser_go_recent(chooser);
		return;
	}

	/* The folder, from a copy (showing it rewrites chooser->folder). */
	snprintf(folder, sizeof(folder), "%s", chooser->folder);
	(void)keiui_chooser_go(chooser, folder);
}

/* A place was listed: nothing selected, at the top at once, and no message left from before. */
static void
model_shown(
	struct keiui_chooser *chooser)
{
	/* The selection and the scroll. */
	chooser->list.selected = -1;
	chooser->list.count = chooser->count;
	kl_scroll_move_to(&chooser->list.scroll, 0.0, 0.0, 0, chooser->now_us);

	/* The message. */
	chooser->message[0] = '\0';
}

/* Save's answer: the name in the folder shown, after the checks and the question. */
static void
model_accept_save(
	struct keiui_chooser *chooser)
{
	char path[KEIUI_CHOOSER_PATH_MAX];
	struct stat status;
	const char *slash;
	int writable;
	int regular;
	int folder;
	int dots;
	int error;

	/* A name is needed. */
	if (chooser->name.length == 0U)
		return;

	/* One part of a path. */
	slash = strchr(chooser->name.text, '/');
	if (slash != NULL) {
		model_message(chooser, "A name can't contain \"/\".", NULL);
		return;
	}

	/* Not the folder or its parent. */
	dots = 0;
	if (chooser->name.text[0] == '.') {
		if (chooser->name.text[1] == '\0')
			dots = 1;
		else if (chooser->name.text[1] == '.' && chooser->name.text[2] == '\0')
			dots = 1;
	}

	/* Such a name is refused. */
	if (dots) {
		model_message(chooser, "\"%s\" can't be used as a name.", chooser->name.text);
		return;
	}

	/* Recent is not a folder to save in. */
	if (chooser->recent) {
		model_message(chooser, "Choose a folder to save in.", NULL);
		return;
	}

	/* The path the name would have. */
	model_join(chooser->folder, chooser->name.text, path, sizeof(path));
	error = stat(path, &status);

	/* What already has that name: a folder, a file, or something else. */
	folder = 0;
	regular = 0;
	if (error == 0) {
		folder = S_ISDIR(status.st_mode);
		regular = S_ISREG(status.st_mode);
	}

	/* A folder of that name is gone into. */
	if (folder) {
		(void)keiui_chooser_go(chooser, path);
		kl_field_set(&chooser->name, "");
		return;
	}

	/* Anything else of that name but a file cannot be replaced. */
	if (error == 0 && !regular) {
		model_message(chooser, "\"%s\" isn't a file.", chooser->name.text);
		return;
	}

	/* A folder that cannot be written to cannot take the file. */
	writable = access(chooser->folder, W_OK);
	if (writable != 0) {
		model_message(chooser, "You can't save in \"%s\".", model_base(chooser->folder));
		return;
	}

	/* A file of that name: asked first. */
	if (error == 0) {
		snprintf(chooser->confirm_path, sizeof(chooser->confirm_path), "%s", path);
		chooser->confirm = 1;
		return;
	}

	/* Succeeded: a new file's path. */
	model_answer(chooser, path);
}

/* Gives the answer: the path chosen. */
static void
model_answer(
	struct keiui_chooser *chooser,
	const char *path)
{
	/* An answer is given once. */
	if (chooser->answered)
		return;

	/* Chosen, with its path. */
	snprintf(chooser->answer, sizeof(chooser->answer), "%s", path);
	chooser->answered = 1;
	chooser->result = KL_FILE_CHOOSER_CHOSEN;
	chooser->confirm = 0;
}

/* Writes a folder and a name joined by one slash. */
static void
model_join(
	const char *folder,
	const char *name,
	char *out,
	size_t size)
{
	size_t length;

	/* The root already ends with its slash. */
	length = strlen(folder);
	if (length > 0U && folder[length - 1U] == '/')
		snprintf(out, size, "%s%s", folder, name);
	else
		snprintf(out, size, "%s/%s", folder, name);
}

/* Shows why something was refused (format has one %s for name, or none). */
static void
model_message(
	struct keiui_chooser *chooser,
	const char *format,
	const char *name)
{
	/* The line, with the name when it has one. */
	if (name != NULL)
		snprintf(chooser->message, sizeof(chooser->message), format, name);
	else
		snprintf(chooser->message, sizeof(chooser->message), "%s", format);
}

/* Sets a field's text with the name up to its extension selected (all of a name without one). */
static void
model_field_stem(
	struct kl_field *field,
	const char *text)
{
	const char *dot;

	/* The text, the caret at its end. */
	kl_field_set(field, text);

	/* The name up to its extension selected. */
	field->anchor = 0;
	dot = strrchr(field->text, '.');
	if (dot != NULL && dot != field->text)
		field->caret = (size_t)(dot - field->text);
}

/* Reports a path's last part (the root's is "/"). */
static const char *
model_base(
	const char *path)
{
	const char *slash;

	/* After the last slash, unless that is all there is. */
	slash = strrchr(path, '/');
	if (slash == NULL)
		return path;
	if (slash[1] == '\0')
		return path;

	/* Reports the last part. */
	return slash + 1;
}
