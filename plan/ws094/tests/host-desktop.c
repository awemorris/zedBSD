/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws094-p003, p004: the desktop's grid and layout on the host
 * (userland/desktop/files/desktop-layout.c): cells from the top-right
 * corner down a column, then leftwards; saved places kept when they are in
 * the grid and free, the others filling the free cells; the layout file
 * read and written (a place set, Clean Up removing the file).
 *
 * ws094-p005: the places shown kept for the next layout (a new item takes
 * a free cell, the others stay), a rename carrying the place (shown and
 * saved) to the new name, Clean Up forgetting the places shown, and the
 * desktop's context menus (ui-context.c) on an item and on the empty
 * desktop, with the file manager's model over a folder of the temporary
 * folder.
 *
 * ws094-p006: the cell at a point; a drop of the desktop's own items on a
 * cell moving them there (a taken cell keeping them), and a drop of
 * another window's items placing them from the drop's cell.
 *
 * ws094-p009: a click's frame draws only the cells that changed; the
 * picture is the same as a whole frame's (and the rest of the canvas is
 * not drawn again).  BUG-221: so does a rubber band's drag, drawn again
 * only where the band and the cells it selects changed, and its end.
 *
 *   host-desktop TEMPORARY-FOLDER
 */

#include "files.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* How many checks failed. */
static int failures;

static void check(int condition, const char *text);
static void check_shown(void);
static void check_menus(const char *temporary);
static int context_has(const struct fm_context *context, const char *label);
static void check_drag(const char *temporary);
static void check_partial(const char *temporary);
static int partial_same(struct fm_app *app, struct kl_canvas *canvas, uint32_t *whole, const char *text);
static void band_input(struct fm_app *app, unsigned type, int x, int y, int pressed);
static int saved_at(const struct fm_desktop *desk, const char *name, int column, int row);
static void check_label(void);
static void check_resize(void);
static void check_prune(void);

/*
 * Runs the checks.
 */
int
main(
	int argc,
	char **argv)
{
	static const char *const names[] = { "a", "b", "c", "d" };
	struct fm_desktop_place places[4];
	struct fm_desktop_saved saved[3];
	struct fm_desktop_saved *read;
	struct fm_desktop desk;
	struct kl_rect rect;
	char config[1024];
	char path[1200];
	size_t count;
	int placed;
	int columns;
	int rows;
	int error;
	int read_error;

	/* The temporary folder is the configuration folder. */
	if (argc != 2) {
		fprintf(stderr, "usage: host-desktop TEMPORARY-FOLDER\n");
		return 2;
	}

	/* The configuration folder under it. */
	snprintf(config, sizeof(config), "%s/config", argv[1]);
	setenv("XDG_CONFIG_HOME", config, 1);

	/* The grid of a 1280x766 desktop: 13 columns of 7 rows. */
	fm_desktop_grid(1280, 766, &columns, &rows);
	check(columns == 13 && rows == 7, "grid 13 x 7");

	/* The top-right cell, the one under it, the first of the next column, none past the grid. */
	placed = fm_desktop_cell_rect(0, 0, 1280, 766, &rect);
	check(placed == 1 && rect.x == 1168 && rect.y == 16, "cell 0,0 at the top right (1168,16)");
	placed = fm_desktop_cell_rect(0, 1, 1280, 766, &rect);
	check(placed == 1 && rect.x == 1168 && rect.y == 120, "cell 0,1 under it (1168,120)");
	placed = fm_desktop_cell_rect(1, 0, 1280, 766, &rect);
	check(placed == 1 && rect.x == 1072 && rect.y == 16, "cell 1,0 to the left (1072,16)");
	placed = fm_desktop_cell_rect(13, 0, 1280, 766, &rect);
	check(placed == 0, "no cell past the grid");

	/* No saved places: in order down the first column. */
	fm_desktop_arrange(names, 4U, NULL, 0U, 1280, 766, places);
	check(places[0].column == 0 && places[0].row == 0 && places[3].column == 0 && places[3].row == 3, "in order down the first column");

	/* Saved places: c at 2,5; d at c's cell (taken, so filled in); x not an item; a outside the grid. */
	snprintf(saved[0].name, sizeof(saved[0].name), "c");
	saved[0].column = 2;
	saved[0].row = 5;
	snprintf(saved[1].name, sizeof(saved[1].name), "d");
	saved[1].column = 2;
	saved[1].row = 5;
	snprintf(saved[2].name, sizeof(saved[2].name), "a");
	saved[2].column = 40;
	saved[2].row = 0;
	fm_desktop_arrange(names, 4U, saved, 3U, 1280, 766, places);
	check(places[2].column == 2 && places[2].row == 5, "c where it was put (2,5)");
	check(places[0].column == 0 && places[0].row == 0, "a (saved outside the grid) fills the first free cell");
	check(places[1].column == 0 && places[1].row == 1, "b fills the next");
	check(places[3].column == 0 && places[3].row == 2, "d (its cell taken by c) fills the next");

	/* A tiny desktop has one cell: the second item has none. */
	fm_desktop_arrange(names, 2U, NULL, 0U, 50, 50, places);
	check(places[0].column == 0 && places[1].column == -1, "a tiny desktop has one cell");

	/* The file: a place set is written and read back; Clean Up removes it. */
	memset(&desk, 0, sizeof(desk));
	error = fm_desktop_layout_set(&desk, "photo.png", 3, 2);
	check(error == 0, "a place set");
	error = fm_desktop_layout_path(path, sizeof(path));
	check(error == 0, "the layout file's path");
	error = fm_desktop_layout_read(path, &read, &count);
	check(error == 0 && count == 1U && read[0].column == 3 && read[0].row == 2, "the place read back");
	free(read);
	error = fm_desktop_layout_set(&desk, "photo.png", 4, 1);
	if (error != 0)
		check(0, "the repeated placement writes its layout");

	/* Reads back the saved places even when the writer reported an error. */
	read_error = fm_desktop_layout_read(path, &read, &count);
	if (read_error != 0)
		error = read_error;
	check(error == 0 && count == 1U && read[0].column == 4 && read[0].row == 1, "the same item placed again: one line");
	free(read);
	error = fm_desktop_clean_up(&desk);
	if (error != 0)
		check(0, "Clean Up removes the saved layout");

	/* Reads the empty layout after the cleanup attempt. */
	read_error = fm_desktop_layout_read(path, &read, &count);
	if (read_error != 0)
		error = read_error;
	check(error == 0 && count == 0U && desk.saved_count == 0U, "Clean Up forgets every place");
	free(read);
	fm_desktop_release(&desk);

	/* ws094-p005: the places shown, the rename, and the context menus. */
	check_shown();
	check_menus(argv[1]);
	check_drag(argv[1]);
	check_partial(argv[1]);

	/* ws094-p010: the names in two lines, and a desktop of another size. */
	check_label();
	check_resize();

	/* Verify that stale names disappear only from the next saved layout. */
	check_prune();

	/* The outcome. */
	if (failures != 0) {
		printf("host-desktop: FAIL (%d)\n", failures);
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("host-desktop: PASS\n");
	return 0;
}

/* Prints a check's outcome and counts a failure. */
static void
check(
	int condition,
	const char *text)
{
	/* A failed check is counted. */
	if (!condition) {
		printf("FAIL: %s\n", text);
		failures++;
		return;
	}

	/* A passed check is printed. */
	printf("ok: %s\n", text);

	/* Succeeded: this check's outcome is reported. */
	return;
}

/* Checks pruning against the full listing and its deferred file write. */
static void
check_prune(void)
{
	static const char *const names[] = { "a.txt", "far.txt", "new.txt" };
	struct fm_desktop desk;
	struct fm_desktop_saved *read;
	char path[FM_PATH_MAX];
	size_t count;
	int error;
	int present;
	int differs;

	/* Start with one retained name, one stale name and one off-grid place. */
	memset(&desk, 0, sizeof(desk));
	desk.saved = calloc(3U, sizeof(desk.saved[0]));
	check(desk.saved != NULL, "prune: room for saved places");
	if (desk.saved == NULL)
		return;

	/* Populate the saved array in the order its compacted version must keep. */
	desk.saved_count = 3U;
	snprintf(desk.saved[0].name, sizeof(desk.saved[0].name), "a.txt");
	desk.saved[0].column = 2;
	desk.saved[0].row = 3;
	snprintf(desk.saved[1].name, sizeof(desk.saved[1].name), "gone.txt");
	desk.saved[1].column = 4;
	desk.saved[1].row = 4;
	snprintf(desk.saved[2].name, sizeof(desk.saved[2].name), "far.txt");
	desk.saved[2].column = 40;
	desk.saved[2].row = 0;

	/* Put the original saved names on disk before pruning memory. */
	error = fm_desktop_layout_path(path, sizeof(path));
	check(error == 0, "prune: layout path");
	if (error != 0) {
		fm_desktop_release(&desk);
		return;
	}

	/* Preserve the old file until an ordinary placement writes again. */
	error = fm_desktop_layout_write(path, desk.saved, desk.saved_count);
	check(error == 0, "prune: original layout written");
	if (error != 0) {
		fm_desktop_release(&desk);
		return;
	}

	/* Include far.txt even though its saved cell lies beyond the grid. */
	fm_desktop_layout_prune(&desk, names, 3U);
	check(desk.saved_count == 2U, "prune: only existing names retained");
	present = saved_at(&desk, "a.txt", 2, 3);
	check(present == 1, "prune: visible name retains its place");
	present = saved_at(&desk, "far.txt", 40, 0);
	check(present == 1, "prune: off-grid name retains its place");

	/* The disk still contains all three names before the next save. */
	error = fm_desktop_layout_read(path, &read, &count);
	check(error == 0 && count == 3U, "prune: pruning alone does not write the file");
	free(read);

	/* A second prune leaves the retained places unchanged. */
	fm_desktop_layout_prune(&desk, names, 3U);
	check(desk.saved_count == 2U, "prune: repeated listing preserves retained names");

	/* Move an existing item using the production layout-file writer. */
	error = fm_desktop_layout_set(&desk, "a.txt", 3, 3);
	check(error == 0, "prune: next placement written");
	if (error != 0) {
		fm_desktop_release(&desk);
		return;
	}

	/* The new file keeps the two listing names and has no stale row. */
	error = fm_desktop_layout_read(path, &read, &count);
	check(error == 0 && count == 2U, "prune: next save omits the stale name");
	if (count == 2U) {
		differs = strcmp(read[0].name, "a.txt");
		check(differs == 0 && read[0].column == 3, "prune: first saved name moved");
		differs = strcmp(read[1].name, "far.txt");
		check(differs == 0 && read[1].column == 40, "prune: overflow saved name persists");
	}

	/* A successful empty listing removes every saved name safely. */
	free(read);
	fm_desktop_layout_prune(&desk, NULL, 0U);
	check(desk.saved_count == 0U, "prune: empty listing removes remaining names");
	fm_desktop_release(&desk);

	/* Succeeded: pruning and the next save were exercised. */
	return;
}

/* Checks the places shown: kept for the next layout, carried by a rename, forgotten by Clean Up (ws094-p005). */
static void
check_shown(void)
{
	static const char *const before[] = { "b", "c", "d" };
	static const char *const after[] = { "a", "b", "c", "d" };
	struct fm_desktop_place places[4];
	struct fm_desktop_saved *read;
	struct fm_desktop desk;
	char path[1200];
	size_t count;
	int error;
	int operation_error;

	/* b, c and d laid out down the first column, and remembered. */
	memset(&desk, 0, sizeof(desk));
	desk.places = places;
	desk.place_count = 3U;
	fm_desktop_arrange(before, 3U, NULL, 0U, 1280, 766, places);
	error = fm_desktop_remember(&desk, before, 3U);
	check(error == 0 && desk.shown_count == 3U, "the places shown remembered");

	/* A new item a (before them by name) takes the first free cell; the others stay. */
	fm_desktop_arrange(after, 4U, desk.shown, desk.shown_count, 1280, 766, places);
	check(places[1].column == 0 && places[1].row == 0, "b stays at 0,0");
	check(places[3].column == 0 && places[3].row == 2, "d stays at 0,2");
	check(places[0].column == 0 && places[0].row == 3, "the new a takes the free cell 0,3");

	/* A renamed item keeps its place shown under the new name; c had a saved place, which follows too. */
	desk.place_count = 4U;
	error = fm_desktop_remember(&desk, after, 4U);
	if (error != 0)
		check(0, "the preceding layout operation succeeded");

	/* Writes the renamed item's original saved cell. */
	operation_error = fm_desktop_layout_set(&desk, "c", 5, 4);
	if (operation_error != 0)
		error = operation_error;
	if (error != 0)
		check(0, "the preceding layout operation succeeded");

	/* Renames the saved and shown places. */
	operation_error = fm_desktop_layout_rename(&desk, "c", "e");
	if (operation_error != 0)
		error = operation_error;
	check(error == 0 && strcmp(desk.shown[2].name, "e") == 0, "the rename carries the place shown");
	error = fm_desktop_layout_path(path, sizeof(path));
	if (error != 0)
		check(0, "the preceding layout operation succeeded");

	/* Reads back the renamed saved place. */
	operation_error = fm_desktop_layout_read(path, &read, &count);
	if (operation_error != 0)
		error = operation_error;
	check(error == 0 && count == 1U && strcmp(read[0].name, "e") == 0 && read[0].column == 5, "the rename carries the saved place in the file");
	free(read);

	/* Clean Up forgets the places shown too. */
	error = fm_desktop_clean_up(&desk);
	check(error == 0 && desk.shown_count == 0U && desk.shown == NULL, "Clean Up forgets the places shown");

	/* The places were the test's; the rest is freed. */
	desk.places = NULL;
	fm_desktop_release(&desk);

	/* Succeeded: saved/shown rename and cleanup were exercised. */
	return;
}

/* Checks the desktop's context menus and its rename over a folder of the temporary folder (ws094-p005). */
static void
check_menus(
	const char *temporary)
{
	static struct fm_context context;
	static struct fm_app app;
	struct fm_tab *tab;
	struct stat status;
	char folder[1024];
	char file[1200];
	char renamed[1200];
	FILE *made;
	int differs;
	int index;
	int error;

	/* A Desktop folder with a file and a folder, and the model over it in the desktop mode. */
	snprintf(folder, sizeof(folder), "%s/Desktop", temporary);
	(void)mkdir(folder, 0755);
	snprintf(file, sizeof(file), "%s/notes.txt", folder);
	made = fopen(file, "w");
	if (made != NULL)
		fclose(made);
	snprintf(renamed, sizeof(renamed), "%s/Projects", folder);
	(void)mkdir(renamed, 0755);
	setenv("HOME", temporary, 1);
	error = fm_app_init(&app, NULL, folder);
	check(error == 0, "the model over the desktop's folder");
	if (error != 0)
		return;
	app.desktop = 1;
	tab = fm_ui_tab(&app);
	check(tab->listing.count == 2U, "two items on the desktop");

	/* The empty desktop's menu: New Folder, Paste, Clean Up, Show Desktop in Files; no view or sort. */
	app.context_where = FM_CONTEXT_EMPTY;
	fm_select_none(tab);
	fm_ui_context(&app, &context);
	check(context_has(&context, "New Folder") && context_has(&context, "Paste") && context_has(&context, "Clean Up"), "the empty desktop's menu: New Folder, Paste, Clean Up");
	check(context_has(&context, "Show Desktop in Files") && !context_has(&context, "View") && !context_has(&context, "Sort By"), "... Show Desktop in Files, and no View or Sort By");

	/* An item's menu: the file manager's, with Show in Files instead of Get Info and no new tab. */
	index = 0;
	differs = strcmp(tab->listing.entries[0].name, "Projects");
	if (differs != 0)
		index = 1;
	fm_select_only(tab, index);
	app.context_where = FM_CONTEXT_ITEMS;
	fm_ui_context(&app, &context);
	check(context_has(&context, "Open") && context_has(&context, "Rename") && context_has(&context, "Move to Trash") && context_has(&context, "Copy"), "an item's menu: Open, Copy, Rename, Move to Trash");
	check(context_has(&context, "Show in Files") && !context_has(&context, "Get Info") && !context_has(&context, "Open in New Tab"), "... Show in Files, no Get Info or Open in New Tab");

	/* A rename on the desktop: F2's field, a new name typed, Enter; the file renamed. */
	fm_select_only(tab, 1 - index);
	tab->cursor = 1 - index;
	fm_desktop_action(&app, FM_ACTION_RENAME);
	check(app.focus == FM_FOCUS_RENAME, "Rename opens the field");
	kl_field_set(&app.rename, "todo.txt");
	fm_desktop_rename_end(&app, 1);
	snprintf(renamed, sizeof(renamed), "%s/todo.txt", folder);
	error = lstat(renamed, &status);
	check(error == 0 && app.focus != FM_FOCUS_RENAME, "the file renamed to todo.txt");

	/* The model is done with. */
	fm_desktop_release(&app.desk);
	fm_app_release(&app);

	/* Succeeded: desktop menu and rename checks finished. */
	return;
}

/* Tells whether a context menu has a row with a label. */
static int
context_has(
	const struct fm_context *context,
	const char *label)
{
	unsigned index;
	int differs;

	/* Each row's label. */
	for (index = 0; index < context->count; index++) {
		differs = strcmp(context->rows[index].label, label);
		if (differs == 0)
			return 1;
	}

	/* No such row. */
	return 0;
}

/* Checks the cell at a point, and the drops on the desktop (ws094-p006), over the Desktop folder check_menus made. */
static void
check_drag(
	const char *temporary)
{
	static struct fm_app app;
	static const char *names[4];
	static char *dropped[2] = { "/elsewhere/a.txt", "/elsewhere/b.txt" };
	struct fm_desktop_place places[4];
	struct fm_tab *tab;
	char folder[1024];
	size_t index;
	int column;
	int row;
	int found;
	int error;
	int todo;

	/* The cells at points: the top-right one, the one left of it, the margin. */
	found = fm_desktop_cell_at(1200, 50, 1280, 766, &column, &row);
	check(found == 1 && column == 0 && row == 0, "the cell at (1200,50) is 0,0");
	found = fm_desktop_cell_at(1100, 150, 1280, 766, &column, &row);
	check(found == 1 && column == 1 && row == 1, "the cell at (1100,150) is 1,1");
	found = fm_desktop_cell_at(1270, 50, 1280, 766, &column, &row);
	check(found == 0, "no cell in the right margin");

	/* The model over the Desktop folder (Projects and todo.txt), laid out down the first column. */
	snprintf(folder, sizeof(folder), "%s/Desktop", temporary);
	error = fm_app_init(&app, NULL, folder);
	check(error == 0, "the model for the drag");
	if (error != 0)
		return;
	app.desktop = 1;
	tab = fm_ui_tab(&app);
	for (index = 0; index < tab->listing.count && index < 4U; index++)
		names[index] = tab->listing.entries[index].name;
	fm_desktop_arrange(names, tab->listing.count, NULL, 0U, 1280, 766, places);
	app.desk.places = places;
	app.desk.place_count = tab->listing.count;
	app.desk.width = 1280;
	app.desk.height = 766;
	todo = 0;
	error = strcmp(tab->listing.entries[0].name, "todo.txt");
	if (error != 0)
		todo = 1;

	/* todo.txt dragged to the cell 3,2: it moves there, and the place is saved. */
	fm_select_only(tab, todo);
	app.desk.press_index = todo;
	app.desk.drop_place = 1;
	app.desk.drop_column = 3;
	app.desk.drop_row = 2;
	found = fm_desktop_drop_place(&app);
	check(found == 1 && saved_at(&app.desk, "todo.txt", 3, 2), "todo.txt moved to 3,2 and saved");

	/* Dragged onto Projects' cell 0,0: it stays. */
	app.desk.drop_place = 1;
	app.desk.drop_column = 0;
	app.desk.drop_row = 0;
	found = fm_desktop_drop_place(&app);
	check(found == 1 && saved_at(&app.desk, "todo.txt", 3, 2), "a drop on another item's cell keeps todo.txt where it was saved");

	/* Two items of another window dropped at 0,0: placed in the free cells from there (0,0 and 0,1 are the items'). */
	snprintf(app.drop_folder, sizeof(app.drop_folder), "%s", folder);
	app.desk.drop_item = -1;
	app.desk.drop_column = 0;
	app.desk.drop_row = 0;
	fm_desktop_dropped(&app, dropped, 2U);
	check(saved_at(&app.desk, "a.txt", 0, 2) && saved_at(&app.desk, "b.txt", 0, 3), "the dropped a.txt and b.txt placed at 0,2 and 0,3");

	/* The places were the test's; the rest is freed. */
	app.desk.places = NULL;
	fm_desktop_release(&app.desk);
	fm_app_release(&app);

	/* Succeeded: cell and drop checks finished. */
	return;
}

/* Tells whether a name has a saved place at a cell. */
static int
saved_at(
	const struct fm_desktop *desk,
	const char *name,
	int column,
	int row)
{
	size_t index;
	int differs;

	/* Each saved place. */
	for (index = 0; index < desk->saved_count; index++) {
		differs = strcmp(desk->saved[index].name, name);
		if (differs != 0)
			continue;

		/* The name's place. */
		if (desk->saved[index].column == column && desk->saved[index].row == row)
			return 1;
		return 0;
	}

	/* No place for the name. */
	return 0;
}

/*
 * ws094-p009: the desktop over a folder of files, folders and a long name
 * drawn whole, then with one item selected, another selected with Ctrl,
 * and none: each frame draws only the changed cells, and matches a whole
 * frame of the same state.
 */
static void
check_partial(
	const char *temporary)
{
	static struct fm_app app;
	static struct kl_text text;
	struct kl_canvas canvas;
	struct fm_tab *tab;
	char folder[1024];
	char file[1200];
	uint32_t *pixels;
	uint32_t *whole;
	const char *stem;
	FILE *made;
	int index;
	int error;
	int differs;
	int selected;
	int first;

	/* Nine items: files, folders and a name longer than a cell. */
	snprintf(folder, sizeof(folder), "%s/partial", temporary);
	(void)mkdir(folder, 0755);
	for (index = 0; index < 9; index++) {
		if (index % 3 == 2) {
			snprintf(file, sizeof(file), "%s/folder-%d", folder, index);
			(void)mkdir(file, 0755);
			continue;
		}

		/* A file, the fifth with a long name. */
		stem = "note";
		if (index == 4)
			stem = "a-name-much-longer-than-its-cell";
		snprintf(file, sizeof(file), "%s/%s-%d.txt", folder, stem, index);
		made = fopen(file, "w");
		if (made != NULL)
			fclose(made);
	}

	/* The fonts, a 1280x766 canvas and the model in the desktop mode. */
	kl_text_companions("userland/desktop/fonts/Mahora-Bold.ttf", "userland/desktop/fonts/JetBrainsMono-Regular.ttf");
	error = kl_text_open(&text, "userland/desktop/fonts/Mahora-Regular.ttf", NULL);
	check(error == 0, "partial: the font");
	if (error != 0)
		return;

	/* Allocates and checks the desktop canvas before the comparison image. */
	pixels = calloc(1280U * 766U, sizeof(uint32_t));
	if (pixels == NULL) {
		check(0, "partial: the canvas");
		return;
	}

	/* Allocates the reference image only after the canvas allocation succeeded. */
	whole = calloc(1280U * 766U, sizeof(uint32_t));
	if (whole == NULL) {
		check(0, "partial: the canvas");
		return;
	}

	/* Attaches the canvas to the allocated pixel storage. */
	error = kl_canvas_init(&canvas, pixels, 1280U, 1280, 766);
	check(error == 0, "partial: the canvas");
	if (error != 0)
		return;
	app.now = 1000U;
	error = fm_app_init(&app, &text, folder);
	check(error == 0, "partial: the model");
	if (error != 0)
		return;
	app.desktop = 1;
	tab = fm_ui_tab(&app);
	check(tab->listing.count == 9U, "partial: nine items");

	/* The first frame is whole, and kept. */
	fm_desktop_draw(&app, &canvas);
	check(app.desk.painted == 1, "partial: the first frame is kept");

	/* A failed directory read must not discard a saved name missing from its listing. */
	error = fm_desktop_layout_set(&app.desk, "missing.txt", 8, 6);
	check(error == 0, "prune: saved name prepared before a failed listing");
	tab->listing.error = EIO;
	fm_desktop_draw(&app, &canvas);
	check(app.desk.saved_count == 1U, "prune: failed listing preserves saved names");

	/* A later successful listing may remove that absent saved name. */
	tab->listing.error = 0;
	fm_desktop_draw(&app, &canvas);
	check(app.desk.saved_count == 0U, "prune: successful listing removes absent names");

	/* One item selected, another added with Ctrl (the long name), and none. */
	fm_select_only(tab, 0);
	partial_same(&app, &canvas, whole, "partial: one item selected");
	for (index = 0; index < (int)tab->listing.count; index++) {
		differs = strncmp(tab->listing.entries[index].name, "a-name", 6);
		if (differs == 0)
			fm_select_toggle(tab, index);
	}

	/* The long name's pill is kept in its cell. */
	partial_same(&app, &canvas, whole, "partial: the long name added");
	fm_select_none(tab);
	partial_same(&app, &canvas, whole, "partial: none selected");

	/*
	 * BUG-221: a rubber band from the empty middle across the items' column
	 * at the right, and back: each frame draws only where the band and the
	 * selection changed, and is the whole frame's picture.
	 */
	band_input(&app, FM_EVENT_MOTION, 800, 600, 0);
	band_input(&app, FM_EVENT_BUTTON, 800, 600, 1);
	partial_same(&app, &canvas, whole, "band: it starts");
	selected = 0;
	for (index = 1; index <= 8; index++) {
		band_input(&app, FM_EVENT_MOTION, 800 + 60 * index, 600 - 70 * index, 0);
		partial_same(&app, &canvas, whole, "band: it grows across the items");
		first = fm_select_first(tab);
		if (first >= 0)
			selected = 1;
	}

	/* Some of the items it crossed were selected. */
	check(selected, "band: it selects the items it crosses");

	/* Back towards where it started, past the items. */
	for (index = 1; index <= 4; index++) {
		band_input(&app, FM_EVENT_MOTION, 1280 - 110 * index, 40 + 90 * index, 0);
		partial_same(&app, &canvas, whole, "band: it shrinks");
	}

	/* The button's release ends it. */
	band_input(&app, FM_EVENT_BUTTON, 840, 400, 0);
	partial_same(&app, &canvas, whole, "band: it ends");

	/* The model and the canvas are done with. */
	fm_desktop_release(&app.desk);
	fm_app_release(&app);
	kl_canvas_release(&canvas);
	free(pixels);
	free(whole);
	kl_text_close(&text);

	/* Succeeded: all retained-canvas comparisons finished. */
	return;
}

/*
 * Draws a frame after a change of the selection, with a mark in a corner
 * no cell has: the mark stays (only cells were drawn), and without it the
 * picture is the one a whole frame draws.  Returns 1 when both hold.
 */
static int
partial_same(
	struct fm_app *app,
	struct kl_canvas *canvas,
	uint32_t *whole,
	const char *text)
{
	uint32_t *corner;
	size_t size;
	int kept;
	int same;
	int differs;

	/* The mark, in the bottom-left corner. */
	corner = canvas->pixels + (size_t)765 * canvas->stride;
	corner[0] = 0x12345678U;

	/* The changed cells only: the mark stays. */
	fm_desktop_draw(app, canvas);
	kept = 0;
	if (corner[0] == 0x12345678U)
		kept = 1;
	corner[0] = 0U;

	/* The same state drawn whole, for the comparison. */
	size = (size_t)canvas->height * canvas->stride * sizeof(uint32_t);
	memcpy(whole, canvas->pixels, size);
	fm_desktop_repaint(&app->desk);
	fm_desktop_draw(app, canvas);
	differs = memcmp(whole, canvas->pixels, size);
	same = 0;
	if (differs == 0)
		same = 1;
	check(kept && same, text);

	/* Refuses a frame that repainted the corner or differs from the whole image. */
	if (!kept || !same)
		return 0;

	/* Succeeded: the changed cells match the complete frame. */
	return 1;
}

/* Gives the desktop one pointer input at a point, the left button's for a button (BUG-221's band). */
static void
band_input(
	struct fm_app *app,
	unsigned type,
	int x,
	int y,
	int pressed)
{
	struct fm_event event;

	/* The input, a little later than the last. */
	memset(&event, 0, sizeof(event));
	event.type = type;
	event.x = x;
	event.y = y;
	event.button = FM_BUTTON_LEFT;
	event.pressed = pressed;
	app->now += 16U;
	event.time = app->now;
	fm_desktop_event(app, &event);
}

/*
 * ws094-p010 (a): a name that fits is one line; a longer one is broken
 * into two, after a space; a 40-character one keeps its start on the
 * first line and "..." with its end -- the extension -- on the second;
 * every line fits the 88 pixels of a cell at 13 pixels.
 */
static void
check_label(void)
{
	static const char *const names[] = {
		"notes.txt",
		"Budget notes 2026.pdf",
		"Quarterly report draft.pdf",
		"a_forty_character_file_name_for_tests.txt",
		"\xe9\x95\xb7\xe3\x81\x84\xe5\x90\x8d\xe5\x89\x8d\xe3\x81\xae\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab\xe3\x81\xae\xe4\xbe\x8b\xe3\x81\xa7\xe3\x81\x99\xe4\xbb\x8a\xe6\x97\xa5\xe3\x81\xae\xe4\xbc\x9a\xe8\xad\xb0\xe3\x81\xae\xe8\xa8\x98\xe9\x8c\xb2\xe3\x81\xa8\xe6\x9c\xab\xe5\xb0\xbe.txt",
	};
	char first[FM_DESKTOP_LABEL_MAX];
	char second[FM_DESKTOP_LABEL_MAX];
	struct kl_text text;
	size_t index;
	size_t length;
	int fits;
	int width;
	int error;

	/* The desktop's font (the fallback for the Japanese name). */
	kl_text_companions("userland/desktop/fonts/Mahora-Bold.ttf", "userland/desktop/fonts/JetBrainsMono-Regular.ttf");
	error = kl_text_open(&text, "userland/desktop/fonts/Mahora-Regular.ttf", "userland/desktop/fonts/DroidSansFallbackFull.ttf");
	check(error == 0, "label: the fonts");
	if (error != 0)
		return;

	/* Each name, as the desktop shows it. */
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		fm_desktop_label(&text, names[index], 88, 13U, first, second);

		/* Checks the second line only when the first fits, preserving the short circuit. */
		fits = 0;
		width = kl_text_width(&text, first, strlen(first), 13U, 0);
		if (width <= 88) {
			width = kl_text_width(&text, second, strlen(second), 13U, 0);
			if (width <= 88)
				fits = 1;
		}

		/* Reports the two rendered lines and verifies their fit. */
		printf("label %zu: \"%s\" / \"%s\"\n", index, first, second);
		check(fits, "label: both lines fit the cell");
		check(first[0] != '\0', "label: a first line");
	}

	/* The short name in one line. */
	fm_desktop_label(&text, names[0], 88, 13U, first, second);
	check(strcmp(first, "notes.txt") == 0 && second[0] == '\0', "label: a short name in one line");

	/* A longer one in two, whole, broken after a space. */
	fm_desktop_label(&text, names[1], 88, 13U, first, second);
	check(strcmp(first, "Budget notes") == 0 && strcmp(second, "2026.pdf") == 0, "label: a longer name in two lines, broken at a space");

	/* One whose rest does not fit after the space: broken where the first line is full, still whole. */
	fm_desktop_label(&text, names[2], 88, 13U, first, second);
	check(strlen(first) + strlen(second) == strlen(names[2]) && strncmp(second, "\xe2\x80\xa6", 3U) != 0, "label: broken where the first line is full, whole");

	/* The 40-character ones: the start, then the ellipsis and the end with its extension. */
	for (index = 3; index < 5; index++) {
		fm_desktop_label(&text, names[index], 88, 13U, first, second);
		length = strlen(second);
		check(strncmp(names[index], first, strlen(first)) == 0, "label: the first line is the name's start");
		check(strncmp(second, "\xe2\x80\xa6", 3U) == 0, "label: the second line starts with the ellipsis");
		check(length > 7U && strcmp(second + length - 4U, ".txt") == 0, "label: the second line ends with the extension");
		check(strlen(first) + length - 3U < strlen(names[index]), "label: the middle is left out");
	}

	/* The fonts are done with. */
	kl_text_close(&text);

	/* Succeeded: the label checks released their fonts. */
	return;
}

/*
 * ws094-p010 (b): a desktop made larger (1280x766 to 1920x1046) keeps the
 * places saved; made smaller, an item saved past its grid takes a free
 * cell, and made larger again it is back where it was saved (the layout
 * file is not changed by a size).
 */
static void
check_resize(void)
{
	static const char *const names[] = { "left.txt", "far.txt", "new.txt" };
	struct fm_desktop_place places[3];
	struct fm_desktop_saved saved[2];
	int columns;
	int rows;

	/* The grids: 13 x 7 and 19 x 9. */
	fm_desktop_grid(1920, 1046, &columns, &rows);
	check(columns == 19 && rows == 9, "resize: the 1920x1046 grid is 19 x 9");

	/* left.txt saved at 3,2 (in both grids), far.txt at 16,8 (only in the larger one). */
	snprintf(saved[0].name, sizeof(saved[0].name), "left.txt");
	saved[0].column = 3;
	saved[0].row = 2;
	snprintf(saved[1].name, sizeof(saved[1].name), "far.txt");
	saved[1].column = 16;
	saved[1].row = 8;

	/* Larger: both where they were saved, the new item in the first free cell. */
	fm_desktop_arrange(names, 3U, saved, 2U, 1920, 1046, places);
	check(places[0].column == 3 && places[0].row == 2, "resize: 1920, left.txt at its saved 3,2");
	check(places[1].column == 16 && places[1].row == 8, "resize: 1920, far.txt at its saved 16,8");
	check(places[2].column == 0 && places[2].row == 0, "resize: 1920, new.txt in the first free cell");

	/* Smaller: left.txt kept, far.txt (past the grid) in a free cell. */
	fm_desktop_arrange(names, 3U, saved, 2U, 1280, 766, places);
	check(places[0].column == 3 && places[0].row == 2, "resize: 1280, left.txt still at 3,2");
	check(places[1].column == 0 && places[1].row == 0, "resize: 1280, far.txt in the first free cell");
	check(places[2].column == 0 && places[2].row == 1, "resize: 1280, new.txt in the next");

	/* Larger again: far.txt back at its saved place. */
	fm_desktop_arrange(names, 3U, saved, 2U, 1920, 1046, places);
	check(places[1].column == 16 && places[1].row == 8, "resize: 1920 again, far.txt back at 16,8");

	/* Succeeded: the resized grids retained the saved places. */
	return;
}
