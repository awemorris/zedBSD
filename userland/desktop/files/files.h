/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of files, the file manager of the zedBSD desktop.
 *
 * The interface (ui*.c) draws the window's frame on a CPU canvas and reads
 * the window's input as fm_event values; it knows nothing of Wayland or
 * Vulkan.  window.c turns the Wayland window's input into those events,
 * present.c shows each drawn frame through Vulkan, menu.c gives the compositor
 * the menus through libkeiland, and main.c ties them together.  The model
 * (dir.c, nav.c, mime.c, places.c and the files after them) keeps what the
 * interface shows: the listed places, the history, the selection, the file
 * operations.
 */

#ifndef FILES_H
#define FILES_H

#include <keiland/keiland.h>
#include "ops.h"

#include "userland/desktop/paths.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

/* The longest path and name the file manager handles, with the terminating NUL. */
#define FM_PATH_MAX		1024
#define FM_NAME_MAX		256

/* How many tabs a window has, how far back a tab's history goes. */
#define FM_TABS			8
#define FM_HISTORY		64

/* The most parts the path in the toolbar has. */
#define FM_CRUMBS		32

/* How many clickable regions one frame records. */
#define FM_HITS			1024

/* How many places the sidebar lists. */
#define FM_PLACES		40

/* The list view's header and row heights (ui-list.c; the keyboard and the rubber band use them too). */
#define FM_LIST_HEADER		30
#define FM_LIST_ROW		28

/* The desktop's wallpaper, which the dashboard's hero card shows (a PNG, ws138-p002). */
#define FM_WALLPAPER		KEILAND_DATADIR "/keiland/wallpaper.png"

/* The window's size when the compositor leaves it to the program. */
#define FM_WIDTH		1120
#define FM_HEIGHT		720

/*
 * The colors of the interface (spec §38: quiet, color only for the
 * selection), in two sets: the light appearance's and the dark one's
 * (ws089-p017, palette.c).  fm_palette is the set of the appearance the
 * compositor told last, and every colour is read through it.
 */
struct fm_palette {
	/* The window's ground, at the top and at the bottom (a gradient). */
	kl_color background_top;
	kl_color background_bottom;

	/* A panel, its edge and its rim on glass, and the sidebar on the plain ground. */
	kl_color panel;
	kl_color panel_edge;
	kl_color panel_rim;
	kl_color sidebar;

	/* The tints a glass window lays over the compositor's frosted glass: the sidebar's veil, and the content's and the preview's a little stronger one (the glass shows through; the items stay easy to read). */
	kl_color glass_sidebar;
	kl_color glass_content;

	/* A card's shadow. */
	kl_color shadow;

	/* The text: the main ink, the secondary, the faint, a heading's, and an icon's. */
	kl_color text;
	kl_color text_secondary;
	kl_color text_faint;
	kl_color title;
	kl_color icon;

	/* The accent, a selection with and without the keyboard, the pointer's hover, a folder and a separator. */
	kl_color accent;
	kl_color selection;
	kl_color selection_inactive;
	kl_color hover;
	kl_color folder;
	kl_color separator;

	/* A button's ground and under the pointer, a card's inner ground, a tile behind an icon, and a progress bar's rail. */
	kl_color button;
	kl_color button_lit;
	kl_color inner;
	kl_color tile;
	kl_color rail;

	/* The ink of text and marks on the accent, and the accent as the colour of text (ws179-p001). */
	kl_color accent_ink;
	kl_color accent_text;
};

/* The set in use, and the choice of the appearance's (KL_APPEARANCE_*; palette.c). */
extern const struct fm_palette *fm_palette;
void fm_palette_set(unsigned appearance);
void fm_palette_take(unsigned appearance, const struct kl_accent *values);
const struct fm_palette *fm_palette_of(unsigned appearance);

#define FM_COLOR_BACKGROUND_TOP	(fm_palette->background_top)
#define FM_COLOR_BACKGROUND_BOTTOM	(fm_palette->background_bottom)
#define FM_COLOR_PANEL		(fm_palette->panel)
#define FM_COLOR_PANEL_EDGE	(fm_palette->panel_edge)
#define FM_COLOR_PANEL_RIM	(fm_palette->panel_rim)
#define FM_COLOR_SIDEBAR		(fm_palette->sidebar)
#define FM_COLOR_GLASS_SIDEBAR	(fm_palette->glass_sidebar)
#define FM_COLOR_GLASS_CONTENT	(fm_palette->glass_content)
#define FM_COLOR_SHADOW		(fm_palette->shadow)
#define FM_COLOR_TEXT		(fm_palette->text)
#define FM_COLOR_TEXT_SECONDARY	(fm_palette->text_secondary)
#define FM_COLOR_TEXT_FAINT	(fm_palette->text_faint)
#define FM_COLOR_TITLE		(fm_palette->title)
#define FM_COLOR_ICON		(fm_palette->icon)
#define FM_COLOR_ACCENT		(fm_palette->accent)
#define FM_COLOR_ACCENT_INK	(fm_palette->accent_ink)
#define FM_COLOR_ACCENT_TEXT	(fm_palette->accent_text)
#define FM_COLOR_SELECTION	(fm_palette->selection)
#define FM_COLOR_SELECTION_INACTIVE	(fm_palette->selection_inactive)
#define FM_COLOR_HOVER		(fm_palette->hover)
#define FM_COLOR_FOLDER		(fm_palette->folder)
#define FM_COLOR_SEPARATOR	(fm_palette->separator)
#define FM_COLOR_BUTTON		(fm_palette->button)
#define FM_COLOR_BUTTON_LIT	(fm_palette->button_lit)
#define FM_COLOR_INNER		(fm_palette->inner)
#define FM_COLOR_TILE		(fm_palette->tile)
#define FM_COLOR_RAIL		(fm_palette->rail)

/* The modifier keys held with an input, the file manager's own bits. */
#define FM_MOD_SHIFT		0x01U
#define FM_MOD_CTRL		0x02U
#define FM_MOD_ALT		0x04U
#define FM_MOD_SUPER		0x08U

/* The pointer buttons, as evdev codes (BTN_LEFT, BTN_RIGHT, BTN_MIDDLE). */
#define FM_BUTTON_LEFT		0x110U
#define FM_BUTTON_RIGHT		0x111U
#define FM_BUTTON_MIDDLE	0x112U

/*
 * The kinds of input the window gives the interface.  The drop events are
 * a drag and drop from the compositor (ws035-p084): one comes over the window
 * (pressed: it is this window's own drag; focused: it carries file names),
 * moves, leaves, or is dropped; the part of the titlebar's path it is over
 * (action: the control, button: the part; action 0 for none); the action
 * the compositor chose (action); and the end of this window's own drag that left
 * it (pressed: dropped somewhere).
 */
enum fm_event_type {
	FM_EVENT_MOTION,
	FM_EVENT_BUTTON,
	FM_EVENT_AXIS,
	FM_EVENT_LEAVE,
	FM_EVENT_KEY,
	FM_EVENT_FOCUS,
	FM_EVENT_ACTION,
	FM_EVENT_DROP_ENTER,
	FM_EVENT_DROP_MOTION,
	FM_EVENT_DROP_LEAVE,
	FM_EVENT_DROP,
	FM_EVENT_DROP_PART,
	FM_EVENT_DROP_ACTION,
	FM_EVENT_DRAG_DONE,
	FM_EVENT_TEXT,
	FM_EVENT_TEXT_DELETE,
	FM_EVENT_PREEDIT
};

/* The longest text an input method sends at once, with its NUL (KL_WINDOW_TEXT_MAX). */
#define FM_TEXT_INPUT_MAX	256U

/* The drag and drop actions the compositor chooses between (wl_data_device_manager's dnd_action). */
#define FM_DND_COPY		1U
#define FM_DND_MOVE		2U
#define FM_DND_ASK		4U

/*
 * One input: where the pointer is, which button or key, and the modifiers
 * held.  serial is the compositor's serial of a button press (a context
 * menu answers it).
 */
struct fm_event {
	unsigned type;
	int x;
	int y;
	uint32_t button;
	int pressed;
	int scroll;
	uint32_t key;
	uint32_t modifiers;
	uint32_t serial;
	uint64_t time;
	int focused;
	uint32_t action;
	char text[FM_TEXT_INPUT_MAX];
	uint32_t before;
};

/*
 * What an entry is, which decides its icon and how it opens.
 */
enum fm_category {
	FM_CATEGORY_FOLDER,
	FM_CATEGORY_FILE,
	FM_CATEGORY_TEXT,
	FM_CATEGORY_CODE,
	FM_CATEGORY_IMAGE,
	FM_CATEGORY_AUDIO,
	FM_CATEGORY_VIDEO,
	FM_CATEGORY_ARCHIVE,
	FM_CATEGORY_PDF,
	FM_CATEGORY_EXECUTABLE,
	FM_CATEGORY_DOCUMENT,
	FM_CATEGORY_FONT,
	FM_CATEGORY_MODEL
};

/*
 * A file's type: its MIME name, what the interface calls it, and its
 * category.  The table of them lives in mime.c for the whole run.
 */
struct fm_mime {
	const char *type;
	const char *kind;
	unsigned category;
};

/*
 * One item of a listed place: a file or a folder with what was learned
 * of it when it was listed.
 *
 * The name and the path are allocated with the listing and freed with it.
 * detail is a second line some places show (a search result's folder, a
 * trashed file's original place); NULL otherwise.
 */
struct fm_entry {
	char *name;
	char *path;
	char *detail;
	const struct fm_mime *mime;
	uint64_t size;
	mode_t mode;
	uid_t uid;
	gid_t gid;
	time_t modified;
	time_t changed;
	time_t accessed;
	time_t extra_time;
	int folder;
	int link;
	int child_count;
	int selected;
	int cut;
};

/*
 * The items of one place, as last read.
 *
 * error is the errno value of a place that could not be read (0 when it
 * was); modified is the folder's own modification time when it was read,
 * which tells whether it changed since.
 */
struct fm_listing {
	struct fm_entry *entries;
	size_t count;
	size_t capacity;
	int error;
	time_t modified;
};

/*
 * The kinds of place a tab can show.
 */
enum fm_location_kind {
	FM_LOCATION_TODAY,
	FM_LOCATION_FOLDER,
	FM_LOCATION_RECENTS,
	FM_LOCATION_TRASH,
	FM_LOCATION_SEARCH
};

/*
 * A place a tab shows: its kind and its path (a folder) or query.
 */
struct fm_location {
	unsigned kind;
	char path[FM_PATH_MAX];
};

/*
 * One step of a tab's history, with how the place was left: its scroll
 * and the item that had the keyboard's cursor.
 */
struct fm_visit {
	struct fm_location location;
	int scroll;
	char cursor[FM_NAME_MAX];
};

/*
 * One item's place on the desktop (ws094-p004): its cell, the column
 * counted from the right edge (0 the rightmost) and the row from the top;
 * column -1 when the desktop has no cell left for it.
 */
struct fm_desktop_place {
	int column;
	int row;
};

/*
 * A place the user gave an item (by moving it), kept in the desktop's
 * layout file by the item's name.
 */
struct fm_desktop_saved {
	char name[FM_NAME_MAX];
	int column;
	int row;
};

/*
 * What a desktop cell was last drawn with (ws094-p009): its place, whether
 * it was selected or faded (cut), and its thumbnail (NULL for none).  A
 * frame redraws only the cells whose record changed, when nothing else
 * did.
 */
struct fm_desktop_painted {
	const struct kl_image *thumb;
	int column;
	int row;
	int selected;
	int cut;
};

/*
 * The desktop's state (files --desktop, ws094-p004, p005): each item's
 * place (places, one an entry of the tab's listing, remade when the
 * listing or the size changes), the saved places read from the layout file
 * (loaded says they were read), the places the items were shown at by name
 * (shown, so that a new item takes a free cell without moving the others;
 * only in memory), the size laid out
 * for, the listing laid out (its count, time and a hash of its names in
 * order, which a rename changes), the rubber band being
 * dragged (band, from its start to the pointer; painted_band says the kept
 * frame has the band drawn at painted_band_rect, which the next frame
 * draws again only where it changed, BUG-221), the last left click (for
 * a double click: its item and time), and the number of items the last
 * logged layout had plus one (0 before the first).
 *
 * The drag of items (ws094-p006): a left press held on an item (pressing,
 * the item and where; press_alone when a plain click on a selected item
 * selects it alone at the release instead of at the press), which becomes the compositor's drag and drop once it
 * moves (dragging, until the drag's end); a drop over the desktop has its
 * target in drop_item (a folder item, or -1 for the desktop itself) and
 * drop_column and drop_row (the cell under it, -1 for none); drop_place
 * says a drop of the desktop's own items on the desktop moves them to
 * cells (no file moves), carried out with the drop.
 *
 * The frame kept in the canvas (ws094-p009): painted says the canvas holds
 * a whole frame of the listing (its count, time and names' hash) at
 * painted_width by painted_height with nothing over the items (no band,
 * drop target, message, question or field), and painted_cells what each
 * item's cell was drawn with; fm_desktop_repaint forgets it.
 */
struct fm_desktop {
	struct fm_desktop_place *places;
	size_t place_count;
	struct fm_desktop_saved *saved;
	size_t saved_count;
	struct fm_desktop_saved *shown;
	size_t shown_count;
	int loaded;
	int width;
	int height;
	size_t laid_count;
	time_t laid_modified;
	uint32_t laid_names;
	/* A recovered directory read must refresh pruning even when its names did not change. */
	int laid_error;
	int band;
	int band_x;
	int band_y;
	int pointer_x;
	int pointer_y;
	int click_index;
	uint64_t click_ms;
	int logged;
	/* Selection press time for the next successful frame log; zero means no sample pending. */
	uint64_t select_ms;
	int pressing;
	int press_alone;
	int press_index;
	int press_x;
	int press_y;
	int dragging;
	int drop_item;
	int drop_column;
	int drop_row;
	int drop_place;
	int painted;
	int painted_band;
	struct kl_rect painted_band_rect;
	int painted_width;
	int painted_height;
	size_t painted_count;
	time_t painted_modified;
	uint32_t painted_names;
	struct fm_desktop_painted *painted_cells;
};

/*
 * One tab: its history (like a browser's), the place it shows now and
 * where in it the user is.
 */
struct fm_tab {
	struct fm_visit history[FM_HISTORY];
	int history_count;
	int history_index;
	struct fm_listing listing;
	int scroll;
	int cursor;
	int anchor;
	uint64_t checked_at;
};

/*
 * The sections of the sidebar.
 */
enum fm_place_section {
	FM_SECTION_FAVORITES,
	FM_SECTION_LOCATIONS,
	FM_SECTION_DEVICES
};

/* The most removable devices shown, and how long and how often a new one blinks (ws132-p005). */
#define FM_DEVICES_MAX		16
#define FM_DEVICE_BLINK_MS	1800U
#define FM_DEVICE_BLINK_PERIOD	600U

/*
 * One removable device (a USB stick's volume, ws132-p005) as the desktop
 * tells it: its ID, the name shown (its label), where it is mounted ("" when
 * it is not), whether it is new (inserted and never mounted since), and
 * when its blinks began (0: not blinking).
 */
struct fm_device {
	char id[64];
	char name[64];
	char path[FM_PATH_MAX];
	int mounted;
	int fresh;
	uint64_t blink_at;
	/* Its file system ("fat", "ufs"; "" when not told) and size in bytes (0 when not told), for the mount's question (ws132-p009). */
	char fs[8];
	uint64_t bytes;
};

/*
 * One place in the sidebar: its section, icon, label and where it leads;
 * missing marks a favorite whose folder is not there, and fixed one of the
 * two places that head the Favorites (Today and Home, ws127-p011), which
 * the user's list of favorite folders does not hold: they cannot be taken
 * off, dragged in the list or written to it.
 */
struct fm_place {
	unsigned section;
	unsigned icon;
	char label[64];
	struct fm_location location;
	int missing;
	int fixed;
	int device;
};

/*
 * One part of the path in the toolbar: its label and the place it leads to.
 */
struct fm_crumb {
	char label[FM_NAME_MAX];
	struct fm_location location;
};

/*
 * The sidebar's places, in their order.
 */
struct fm_places {
	struct fm_place items[FM_PLACES];
	int count;

	/* The removable devices, which fm_places_init keeps and shows in the Devices section (device is index + 1). */
	struct fm_device devices[FM_DEVICES_MAX];
	int device_count;
};

/*
 * How the items of a folder are shown.
 */
enum fm_view {
	FM_VIEW_ICONS,
	FM_VIEW_LIST
};

/*
 * What the items are sorted by (FM_SORT_COUNT is none).
 */
enum fm_sort {
	FM_SORT_NAME,
	FM_SORT_KIND,
	FM_SORT_SIZE,
	FM_SORT_MODIFIED,
	FM_SORT_COUNT
};

/*
 * The columns of the list view; the app's columns are a mask of their
 * bits.  Location and Date Deleted are added by the places that need them.
 */
enum fm_column {
	FM_COLUMN_NAME,
	FM_COLUMN_KIND,
	FM_COLUMN_SIZE,
	FM_COLUMN_MODIFIED,
	FM_COLUMN_CHANGED,
	FM_COLUMN_OWNER,
	FM_COLUMN_LOCATION,
	FM_COLUMN_DELETED,
	FM_COLUMN_COUNT
};

/* The columns shown unless the user chooses others. */
#define FM_COLUMNS_DEFAULT	((1U << FM_COLUMN_KIND) | (1U << FM_COLUMN_SIZE) | (1U << FM_COLUMN_MODIFIED))

/*
 * Where the keyboard's input goes inside the window.
 */
enum fm_focus {
	FM_FOCUS_CONTENT,
	FM_FOCUS_LOCATION,
	FM_FOCUS_SEARCH,
	FM_FOCUS_RENAME
};

/*
 * What a key did to a text field.
 */
enum fm_field_result {
	FM_FIELD_NONE,
	FM_FIELD_MOVED,
	FM_FIELD_CHANGED,
	FM_FIELD_ENTER,
	FM_FIELD_CANCEL
};

/*
 * A one-line text field: UTF-8 text, the cursor and the other end of the
 * selection (byte offsets on character boundaries).
 */
struct fm_field {
	char text[FM_PATH_MAX];
	size_t length;
	size_t cursor;
	size_t anchor;
};

/*
 * The kinds of clickable region a frame records, which the pointer's
 * input is matched against.
 */
enum fm_hit_kind {
	FM_HIT_NONE,
	FM_HIT_PLACE,
	FM_HIT_ITEM,
	FM_HIT_CONTENT,
	FM_HIT_HEADER,
	FM_HIT_CARD,
	FM_HIT_RECENT,
	FM_HIT_SHOW_ALL,
	FM_HIT_TAB,
	FM_HIT_TAB_CLOSE,
	FM_HIT_SCOPE,
	FM_HIT_OVERLAY,
	FM_HIT_BUTTON,
	FM_HIT_SECTION,
	FM_HIT_COLUMN_EDGE
};

/*
 * One clickable region of the last frame: where it is, what it is and
 * which one (an item's index, a place's, a crumb's).
 */
struct fm_hit {
	struct kl_rect rect;
	unsigned kind;
	int index;
};

/* How many glass panels a frame has at most: the sidebar, the content (with its tabs) and the preview. */
#define FM_PANELS		3

/* The kind of glass panel the window has: a card floating in the window. */
#define FM_PANEL_CARD		0U

/*
 * One part of the window that stands on the compositor's frosted glass: its
 * rectangle in the window, its corners' radius and its kind.  The frame's
 * panels are worked out from its layout (fm_ui_panels) and handed to
 * the compositor with the frame.
 */
struct fm_panel {
	struct kl_rect rect;
	int radius;
	unsigned kind;
};

/*
 * The panels of the last frame, where the drawing put them.  The content's
 * card holds the row of tabs (when there are two or more) over the
 * content, whose items are drawn under the row.
 */
struct fm_layout {
	struct kl_rect tabbar;
	struct kl_rect sidebar;
	struct kl_rect card;
	struct kl_rect content;
	struct kl_rect preview;
	struct kl_rect items;
	int grid_left;
	int columns;
	int cell_width;
	int cell_height;
	int content_height;
	int sidebar_height;
};

/* A search's longest query, how many words of a kind it keeps and how long a word is, and its most results. */
#define FM_SEARCH_QUERY		256
#define FM_SEARCH_WORDS		8
#define FM_SEARCH_WORD		64
#define FM_SEARCH_RESULTS	5000U

/*
 * A folder a search is walking: its open directory and its path.
 */
struct search_walk {
	void *directory;
	char path[FM_PATH_MAX];
};

/*
 * A search in progress: what it wants (names, extensions, kinds),
 * where it looks, and the folders it is walking.
 */
struct fm_search {
	int active;
	char names[FM_SEARCH_WORDS][FM_SEARCH_WORD];
	int name_count;
	char extensions[FM_SEARCH_WORDS][FM_SEARCH_WORD];
	int extension_count;
	unsigned categories;
	char base[FM_PATH_MAX];
	int hidden;
	unsigned long visited;
	struct search_walk *walks;
	size_t walk_count;
	size_t walk_capacity;
};

/*
 * Where a search looks: the folder it was started in, the home folder, or
 * the whole computer.
 */
enum fm_scope {
	FM_SCOPE_FOLDER,
	FM_SCOPE_HOME,
	FM_SCOPE_COMPUTER
};

/* How many recent files and folders the dashboard shows. */
#define FM_HOME_RECENTS		6
#define FM_HOME_FOLDERS		6
#define FM_HOME_CARDS		6

/*
 * A folder card of the dashboard: the folder, its name and how many items
 * it holds.
 */
struct fm_home_card {
	char path[FM_PATH_MAX];
	char label[64];
	int count;
};

/*
 * A recent file of the dashboard: its path and when it was opened.
 */
struct fm_home_recent {
	char path[FM_PATH_MAX];
	time_t time;
};

/*
 * What the home dashboard shows, gathered when it is shown: the folder
 * cards, the recent files and folders, and the hero card's words.
 */
struct fm_dashboard {
	struct fm_home_card cards[FM_HOME_CARDS];
	int card_count;
	struct fm_home_recent recents[FM_HOME_RECENTS];
	int recent_count;
	char folders[FM_HOME_FOLDERS][FM_PATH_MAX];
	int folder_count;
	char greeting[128];
	char summary[160];
};

/* How many thumbnails are kept, and the longest side of one. */
#define FM_THUMBS		64
#define FM_THUMB_SIDE		256

/* How many records the cache on disk holds before it is trimmed, and how many the trim leaves (ws127-p004). */
#define FM_THUMB_RECORDS_MAX	2000U
#define FM_THUMB_RECORDS_KEEP	1800U

/*
 * One kept thumbnail: the file it was made from as the file was then (its
 * path and modification time), the small picture, and when it was last
 * drawn.
 *
 * failed marks a file that could not be read as a picture, so that it is
 * not read again every frame; a file changed since is another file;
 * pending marks the one a child is making (ws168-p004).  An empty path is
 * a free slot.
 */
struct fm_thumb {
	char path[FM_PATH_MAX];
	time_t modified;
	struct kl_image image;
	uint64_t used;
	int failed;
	int pending;
};

/* How much of a file is read to preview it, and how many of its lines are kept. */
#define FM_PEEK_BYTES		16384U
#define FM_PEEK_LINES		60

/*
 * What was read of one file to show it in the preview and Quick Look: its
 * type as its contents tell it, its first lines when it is text, and its
 * picture when Quick Look shows it large.
 *
 * It is kept for one file (its path and modification time) and read again
 * when another is shown.  text holds up to FM_PEEK_LINES lines and is
 * allocated with it; NULL when the file is not text.
 */
struct fm_peek {
	char path[FM_PATH_MAX];
	time_t modified;
	const struct fm_mime *mime;
	char *text;
	size_t text_length;
	int line_count;
	struct kl_image picture;
	int picture_tried;
};

/* The longest name and command of a way to open files, and how many ways a file is offered. */
#define FM_OPENER_NAME		64
#define FM_OPENER_COMMAND	512
#define FM_OPENERS		8

/*
 * One way to open a file: the name the window shows ("Terminal (less)")
 * and the command, where %f stands for the file's path.  A command that
 * starts with "@terminal " runs the rest in a new terminal window, and
 * "@quicklook" shows the file in Quick Look (apps.c).
 */
struct fm_opener {
	char name[FM_OPENER_NAME];
	char command[FM_OPENER_COMMAND];
};

/* How many extended attributes the information lists, and the longest name it keeps of one. */
#define FM_INFO_ATTRIBUTES	12
#define FM_INFO_ATTRIBUTE_NAME	96

/*
 * One extended attribute of a file: its name (the information shows the
 * names only, WS131 D14).
 */
struct fm_attribute {
	char name[FM_INFO_ATTRIBUTE_NAME];
};

/*
 * Where the checksum of the information's file is: not asked for, being
 * computed, done, or failed.
 */
enum fm_checksum_state {
	FM_CHECKSUM_NONE,
	FM_CHECKSUM_RUNNING,
	FM_CHECKSUM_DONE,
	FM_CHECKSUM_FAILED
};

/*
 * What the information card (Get Info) shows of one file or folder,
 * gathered when the card opens: its status as lstat sees it (and the
 * target of a link), its type, extended attributes, and the ways it
 * can be opened; its SHA-256 checksum is computed only when asked for, a
 * piece each round of the main loop.
 *
 * checksum_descriptor is the open file while the checksum runs (-1
 * otherwise), and checksum_context its hash state, allocated with it; both
 * go when the checksum ends or the card closes (fm_info_release).
 */
struct fm_info {
	char path[FM_PATH_MAX];
	int error;
	mode_t mode;
	uid_t uid;
	gid_t gid;
	uint64_t size;
	long links;
	time_t modified;
	time_t changed;
	time_t accessed;
	int folder;
	int child_count;
	char target[FM_PATH_MAX];
	const struct fm_mime *mime;
	struct fm_attribute attributes[FM_INFO_ATTRIBUTES];
	int attribute_count;
	int attributes_more;
	struct fm_opener openers[FM_OPENERS];
	int opener_count;
	unsigned checksum_state;
	char checksum[72];
	int checksum_error;
	int checksum_descriptor;
	void *checksum_context;
	uint64_t checksum_done;
};

/*
 * What the menus ask the window to do (ui-menu.c).  Each menu item
 * reports one; the ranges at the end carry an index (a list column, a way
 * to open the selection, an app to always open with, a place to move to).
 */
enum fm_action {
	FM_ACTION_NONE,
	FM_ACTION_NEW_WINDOW,
	FM_ACTION_NEW_FOLDER,
	FM_ACTION_OPEN,
	FM_ACTION_GET_INFO,
	FM_ACTION_TRASH,
	FM_ACTION_CLOSE_WINDOW,
	FM_ACTION_UNDO,
	FM_ACTION_REDO,
	FM_ACTION_CUT,
	FM_ACTION_COPY,
	FM_ACTION_PASTE,
	FM_ACTION_DUPLICATE,
	FM_ACTION_SELECT_ALL,
	FM_ACTION_RENAME,
	FM_ACTION_VIEW_ICONS,
	FM_ACTION_VIEW_LIST,
	FM_ACTION_SORT_NAME,
	FM_ACTION_SORT_KIND,
	FM_ACTION_SORT_SIZE,
	FM_ACTION_SORT_MODIFIED,
	FM_ACTION_SHOW_SIDEBAR,
	FM_ACTION_SHOW_PREVIEW,
	FM_ACTION_SHOW_HIDDEN,
	FM_ACTION_BACK,
	FM_ACTION_FORWARD,
	FM_ACTION_ENCLOSING,
	FM_ACTION_GO_HOME,
	FM_ACTION_GO_DESKTOP,
	FM_ACTION_GO_DOCUMENTS,
	FM_ACTION_GO_DOWNLOADS,
	FM_ACTION_GO_RECENTS,
	FM_ACTION_GO_COMPUTER,
	FM_ACTION_GO_TRASH,
	FM_ACTION_GO_LOCATION,
	FM_ACTION_FIND,
	FM_ACTION_MINIMIZE,
	FM_ACTION_ZOOM,
	FM_ACTION_HELP,
	FM_ACTION_SHORTCUTS,
	FM_ACTION_ABOUT,
	FM_ACTION_NEW_TAB,
	FM_ACTION_CLOSE_TAB,
	FM_ACTION_NEXT_TAB,
	FM_ACTION_PREVIOUS_TAB,
	FM_ACTION_OPEN_IN_NEW_TAB,
	FM_ACTION_PUT_BACK,
	FM_ACTION_DELETE_NOW,
	FM_ACTION_EMPTY_TRASH,
	FM_ACTION_PLACE_NEW_TAB,
	FM_ACTION_PLACE_REMOVE,
	FM_ACTION_DROP_MOVE,
	FM_ACTION_DROP_COPY,
	FM_ACTION_DROP_LINK,
	FM_ACTION_DROP_CANCEL,
	FM_ACTION_SHOW_IN_FILES,
	FM_ACTION_CLEAN_UP,
	FM_ACTION_CHANGE_WALLPAPER,
	FM_ACTION_OPEN_IN_NEW_WINDOW,
	FM_ACTION_COLUMN_FIRST = 100,
	FM_ACTION_OPEN_WITH_FIRST = 200,
	FM_ACTION_ALWAYS_WITH_FIRST = 400,
	FM_ACTION_USE_SYSTEM_DEFAULT = 450,
	/* The context menu's Move To: the selection moved to the sidebar's place of this index (ws127-p002). */
	FM_ACTION_MOVE_TO_FIRST = 500
};

/*
 * What the window asks of its Wayland side after an action (main.c):
 * nothing, a new window, minimizing, zooming (maximizing or back), or
 * closing.
 */
enum fm_request {
	FM_REQUEST_NONE,
	FM_REQUEST_NEW_WINDOW,
	FM_REQUEST_MINIMIZE,
	FM_REQUEST_ZOOM,
	FM_REQUEST_CLOSE,
	FM_REQUEST_CONTEXT,
	FM_REQUEST_DRAG_OUT,
	FM_REQUEST_DROP,
	FM_REQUEST_DROP_ASK,
	FM_REQUEST_DROP_CANCEL,
	FM_REQUEST_DEVICE_MOUNT,
	FM_REQUEST_DEVICE_EJECT
};

/* How many rows a context menu has at most, and the longest label. */
#define FM_CONTEXT_ROWS		64
#define FM_CONTEXT_LABEL	64

/* Where a context menu was asked for: on items, on the empty part of a folder, on a place of the sidebar. */
#define FM_CONTEXT_ITEMS	0U
#define FM_CONTEXT_EMPTY	1U
#define FM_CONTEXT_PLACE	2U
#define FM_CONTEXT_DROP		3U

/* What a drag of items is over (ui-drag.c): nothing that takes them, a folder, the Trash. */
#define FM_DRAG_NONE		0U
#define FM_DRAG_FOLDER		1U
#define FM_DRAG_TRASH		3U

/* Also: the Favorites' title (the dragged folders are added), and a favorite that a dragged favorite goes to. */
#define FM_DRAG_FAVORITES	4U
#define FM_DRAG_REORDER		5U

/* The kinds of a context menu's row. */
#define FM_ROW_ITEM		0U
#define FM_ROW_LINE		1U
#define FM_ROW_CHECK		2U
#define FM_ROW_SUBMENU		4U

/*
 * One row of a context menu: its number (from 1), the submenu it is in
 * (0 for the top level), its kind, its label, the action it carries out,
 * and whether it can be chosen and is checked.
 */
struct fm_context_row {
	unsigned id;
	unsigned parent;
	unsigned kind;
	char label[FM_CONTEXT_LABEL];
	unsigned action;
	int enabled;
	int checked;
};

/*
 * A context menu worked out for a right press (fm_ui_context): its rows,
 * in order.  menu.c hands it to the compositor, which shows it at the press.
 */
struct fm_context {
	unsigned count;
	struct fm_context_row rows[FM_CONTEXT_ROWS];
};

/*
 * What the menus show of the window's state: which items do something now,
 * which are checked, and the names of the variable items (the ways to open
 * the selection).  menu.c sends it to the compositor when it differs
 * from what the menus show.
 */
struct fm_menu_state {
	int selection;
	int folder;
	int trash;
	int field;
	int can_paste;
	int can_undo;
	int can_redo;
	int can_back;
	int can_forward;
	int can_enclose;
	unsigned view;
	unsigned sort;
	unsigned columns;
	int sidebar;
	int preview;
	int hidden;
	int tabs;
	int opener_count;
	char openers[FM_OPENERS][FM_OPENER_NAME];
	int user_default;
};

/*
 * The controls of the window's titlebar (WS070's CONTROLS presentation,
 * drawn by the compositor): their IDs in the model titlebar.c gives the compositor.
 */
enum fm_control {
	FM_CONTROL_NONE,
	FM_CONTROL_BACK,
	FM_CONTROL_FORWARD,
	FM_CONTROL_HOME,
	FM_CONTROL_PATH,
	FM_CONTROL_SEARCH,
	FM_CONTROL_ICONS,
	FM_CONTROL_LIST,
	FM_CONTROL_PREVIEW,
	FM_CONTROL_PROGRESS
};

/* The bytes of a titlebar's text with its NUL (the compositor takes 1023), and of a path's part. */
#define FM_TITLEBAR_TEXT	1024
#define FM_TITLEBAR_PART	65

/* The most folders the path's field suggests at once (ws127-p010). */
#define FM_SUGGESTIONS		12

/* The progress control's value while nothing runs (it is then not in the titlebar). */
#define FM_TITLEBAR_NO_PROGRESS	(-1)

/*
 * What the titlebar shows of the window's state: whether the history can
 * go back and forward, the parts of the path shown (each cut to
 * FM_TITLEBAR_PART bytes on a character's boundary), the folder the path's
 * field starts from (Ctrl+L), the query of the search shown, the view,
 * the preview, the share done of the running operations in thousandths
 * (FM_TITLEBAR_NO_PROGRESS when none run), and the text control the
 * window last asked to give the keyboard to (FM_CONTROL_SEARCH or
 * FM_CONTROL_PATH) with the count of such requests, and the folders the
 * path's field suggests (ws127-p010: each a label and the text it puts in
 * the field) with the count of the lists made.  titlebar.c sends it to
 * the compositor when it differs from what the titlebar shows.
 */
struct fm_titlebar_state {
	int can_back;
	int can_forward;
	int part_count;
	char parts[FM_CRUMBS][FM_TITLEBAR_PART];
	char path[FM_TITLEBAR_TEXT];
	char query[FM_TITLEBAR_TEXT];
	unsigned view;
	int preview;
	int progress;
	unsigned focus;
	unsigned focus_serial;
	unsigned suggest_serial;
	int suggest_count;
	char suggest_labels[FM_SUGGESTIONS][FM_NAME_MAX];
	char suggest_texts[FM_SUGGESTIONS][FM_TITLEBAR_TEXT];
};

/*
 * What the titlebar tells the window.
 */
enum fm_titlebar_kind {
	FM_TITLEBAR_ACTIVATED,
	FM_TITLEBAR_CHANGED,
	FM_TITLEBAR_DONE
};

/*
 * One thing done with the titlebar: a control chosen (with the path's part
 * for the path), a text control's text as typed, or its editing ended
 * (how: KL_TEXT_SUBMITTED, _CANCELLED or _LEFT), with its text.
 */
struct fm_titlebar_event {
	unsigned kind;
	uint32_t id;
	uint32_t detail;
	char text[FM_TITLEBAR_TEXT];
};

/*
 * The cards of text the Help menu shows over the window.
 */
enum fm_help {
	FM_HELP_NONE,
	FM_HELP_GUIDE,
	FM_HELP_SHORTCUTS,
	FM_HELP_ABOUT
};

/*
 * The file manager of one window: its settings, its tabs, what the last
 * frame drew and what the pointer and the keyboard are doing.
 *
 * One lives for the whole run.  The tabs are allocated (each holds its
 * history) and freed with the app.
 */
struct fm_app {
	/* The fonts, the window's size, the time of the input being handled, and whether a new frame is due. */
	struct kl_text *text;
	int width;
	int height;
	uint64_t now;
	time_t wall;
	int dirty;
	int focused;

	/*
	 * A change of the lit region or of a rubber band alone (BUG-221,
	 * BUG-226): the next frame is drawn only within damage, clipped to it
	 * (fm_ui_damage adds to it), the rest keeping the last frame's pixels.
	 * Any other change (dirty) draws everything.
	 */
	int damage_pending;
	struct kl_rect damage;

	/*
	 * What the last frame drawn changed (BUG-221): frame_partial when it
	 * was drawn by parts, which frame_part holds all of (empty when no
	 * part changed); the frame is then shown by that part alone
	 * (kl_window_present_part).  0 for a frame drawn whole.
	 */
	int frame_partial;
	struct kl_rect frame_part;

	/*
	 * Whether the window is glass: the compositor draws frosted glass under the
	 * panels and shows the desktop between them, so the frame leaves its
	 * ground clear and tints the panels only lightly.  Set once, before the
	 * first frame, when the compositor can show the window see-through.
	 */
	int glass;

	/*
	 * The desktop mode (files --desktop, ws094-p003): the icons of the tab's
	 * folder (~/Desktop) on the compositor's desktop surface instead of the
	 * window, and their places, the saved places and the pointer's state
	 * (ui-desktop.c, desktop-layout.c).
	 */
	int desktop;
	struct fm_desktop desk;

	/* The user's home folder, and the name the Home page greets (the display name, ws035-p120). */
	char home[FM_PATH_MAX];
	char user[64];

	/* How folders are shown. */
	unsigned view;
	unsigned sort;
	int sort_reverse;
	int show_sidebar;
	int show_preview;
	int show_hidden;
	unsigned columns;

	/*
	 * The list view's columns' widths the user dragged (BUG-220: 0 for a
	 * column's own width), kept while Files runs; a drag of the edge before
	 * a column (its index, -1 for none), where it started, and the widths
	 * of the columns on either side of the edge then.
	 */
	int column_widths[FM_COLUMN_COUNT];
	int column_drag;
	int column_drag_x;
	int column_drag_left;
	int column_drag_right;

	/*
	 * The removable device a mount or an eject was asked for
	 * (FM_REQUEST_DEVICE_*, ws132-p005), and the one to be opened once the
	 * desktop says it is mounted.
	 */
	char device_asked[64];
	char device_open[64];
	/* The device whose mount is being asked about (FM_DIALOG_MOUNT, ws132-p009). */
	char device_confirm[64];

	/* Whether the devices' rows and Today's cards were logged since the list changed (for the tests). */
	int device_rows_logged;
	int device_cards_logged;

	/* The sidebar and the tabs. */
	struct fm_places places;
	struct fm_tab *tabs[FM_TABS];
	int tab_count;
	int tab_index;

	/* The last frame's panels and clickable regions. */
	struct fm_layout layout;
	struct fm_hit hits[FM_HITS];
	int hit_count;

	/* The pointer: where it is, the region under it, and the press in progress. */
	int pointer_x;
	int pointer_y;
	int pointer_inside;
	unsigned hover_kind;
	int hover_index;
	unsigned press_kind;
	int press_index;
	int pressing;

	/* The last click, which a second one soon after on the same region makes a double click. */
	uint64_t click_time;
	unsigned click_kind;
	int click_index;

	/* The modifiers held. */
	uint32_t modifiers;

	/*
	 * Where typing goes, and the location field (Ctrl+L); the titlebar's
	 * text control the window last asked for the keyboard for, and how many
	 * times it asked (fm_titlebar_state).
	 */
	unsigned focus;
	struct fm_field location;
	unsigned control_focus;
	unsigned control_focus_serial;

	/* A rubber band being dragged over the items: its corners in the items' coordinates (scroll included). */
	int band;
	int band_x0;
	int band_y0;
	int band_x1;
	int band_y1;

	/*
	 * A drag of the selected items (ui-drag.c): where the left press was;
	 * a press on a selected item leaves its selection change for the
	 * release (press_deferred, with the modifiers of the press) so that the
	 * whole selection can be dragged; whether the drag has started, how many
	 * items it carries (or the favorite dragged in the sidebar, else -1),
	 * and its target (FM_DRAG_*) with the region under the pointer and the
	 * folder it stands for.
	 */
	int press_x;
	int press_y;
	int press_deferred;
	uint32_t press_deferred_modifiers;
	int drag;
	size_t drag_count;
	int drag_place;
	unsigned drag_target;
	unsigned drag_hit_kind;
	int drag_hit_index;
	char drag_folder[FM_PATH_MAX];

	/*
	 * Spring-loaded folders and the scroll at the edges under a drag
	 * (ws127-p002, F-039).  A folder the drag rests on opens after a
	 * moment, which changes the listing and its selection, so a drag keeps
	 * the paths it started with (drag_paths, drag_path_count, freed at its
	 * end) and the folder they came from (drag_source, whose items are no
	 * target).  spring_ms is when the drag came to rest on a folder that
	 * springs (0 for none); edge_ms is when the content last scrolled under
	 * the drag at an edge (0 when it did not); drag_wait_ms is the time to
	 * the next of these steps (-1 for none), for the loop's sleep.  The
	 * times are fm_ui_tick's (app->now).
	 */
	char **drag_paths;
	size_t drag_path_count;
	char drag_source[FM_PATH_MAX];
	uint64_t spring_ms;
	uint64_t edge_ms;
	int drag_wait_ms;

	/*
	 * Drag and drop with other windows (ws035-p084): whether the dragged
	 * items left the window (the compositor carries them from then on).  A drop
	 * coming in: whether one is over the window, whether it is this
	 * window's own drag, whether it carries file names, where it is, the
	 * part of the titlebar's path it is over (-1 for none), the action
	 * the compositor chose (FM_DND_*), whether the Wayland side must answer a
	 * changed target, and the folder and operation (FM_TASK_*) of a drop
	 * made (FM_REQUEST_DROP).  Its target is drag_target and drag_folder.
	 */
	int drag_outside;
	int drop_active;
	int drop_self;
	int drop_files;
	int drop_x;
	int drop_y;
	int drop_part;
	uint32_t drop_action;
	int drop_answer;
	char drop_folder[FM_PATH_MAX];
	unsigned drop_operation;

	/* Whether a drop dropped with "ask" waits for its choice in the context menu (ws035-p088). */
	int drop_asking;

	/* What was typed to find an item by its name, and when it was typed last. */
	char typed[64];
	size_t typed_length;
	uint64_t typed_at;

	/* A short message in the status pill, and until when it shows. */
	char message[160];
	uint64_t message_until;

	/* The tasks running, oldest first, whether their list is open, and when their progress was last drawn. */
	struct fm_task *tasks[FM_TASKS];
	int task_count;
	int show_tasks;
	uint64_t task_drawn_at;

	/* The undo and redo histories. */
	struct fm_undo undo;

	/*
	 * libkeiland's widgets' input (ui-widgets.c, ws090-p023), and the name
	 * being changed: libkeiland's field in it (rename.c, ws090-p010), what
	 * its last frame did
	 * (KL_FIELD_*), where it was drawn (window pixels; shown 0 until it
	 * is), and the item's path.
	 */
	struct kl_field rename;
	struct kl_ui *ui;
	unsigned rename_flags;
	struct kl_rect rename_rect;
	int rename_shown;
	char rename_path[FM_PATH_MAX];

	/*
	 * Whether the recent list was stopped when Recents was read last
	 * (Settings' "Keep recent items" off, ws177-p008): its empty view then
	 * says so and where to turn it on.
	 */
	int recents_off;

	/* A question being asked (FM_DIALOG_*), and the paths it is about. */
	unsigned dialog;
	char **dialog_paths;
	size_t dialog_count;

	/*
	 * A copy or a move held back while the names its destination already
	 * has are asked about (FM_DIALOG_COLLISION): the task, the source being
	 * asked about, and whether the next answer is for all the rest.  The
	 * task is queued once every such source has its answer.
	 */
	struct fm_task *collision_task;
	size_t collision_index;
	int collision_all;

	/*
	 * Whether the held task is the paste of a cut: the clipboard is
	 * emptied only when the task is queued, so that Esc on the question
	 * keeps the cut items on it (ws035-p110, F-050).
	 */
	int collision_cut;

	/* The paths to select once the folder is read again (a finished task's outcome). */
	char **select_paths;
	size_t select_count;

	/* The search: the field, when it was last typed in (0 when the search is up to date), the scope, the folder it was started from, and the walk. */
	struct fm_field search_field;
	uint64_t search_typed_at;

	/*
	 * The path's field's suggestions (ws127-p010): when it was last typed
	 * in (0 when its suggestions are up to date), and the folders it
	 * suggests (a label and the text it puts in the field each), made a
	 * second after the typing stops; suggest_serial counts the lists made,
	 * so that the titlebar sends each once.
	 */
	uint64_t location_typed_at;
	unsigned suggest_serial;
	int suggest_count;
	char suggest_labels[FM_SUGGESTIONS][FM_NAME_MAX];
	char suggest_texts[FM_SUGGESTIONS][FM_TITLEBAR_TEXT];
	unsigned search_scope;
	char search_folder[FM_PATH_MAX];
	struct fm_search search;

	/* How far the sidebar is scrolled (when its places do not fit). */
	int sidebar_scroll;

	/* The dashboard: what it shows, the hero's picture as read and as scaled for the card, and whether it was read. */
	struct fm_dashboard dashboard;
	struct kl_image hero_source;
	struct kl_image hero;
	int hero_tried;
	char wallpaper[FM_PATH_MAX];

	/* The thumbnails: the kept ones, the clock their use is ordered by, and the file asked for next (empty when none). */
	struct fm_thumb thumbs[FM_THUMBS];
	uint64_t thumb_clock;
	char thumb_wanted[FM_PATH_MAX];
	time_t thumb_wanted_modified;

	/* What was read of the file the preview and Quick Look show, and whether Quick Look is open. */
	struct fm_peek peek;
	int quicklook;

	/* The information card (Get Info): whether it is open, and what it shows. */
	int info_open;
	struct fm_info info;

	/* The Help card shown (FM_HELP_NONE when none), and what the Wayland side is asked to do next. */
	unsigned help;
	unsigned request;

	/*
	 * The folder a new window is to show (FM_REQUEST_NEW_WINDOW), set by
	 * Open in New Window and emptied when the window is started; empty
	 * means the folder this window shows (New Window, Ctrl+N).
	 */
	char new_window_folder[FM_PATH_MAX];

	/*
	 * The last right press, for its context menu (FM_REQUEST_CONTEXT):
	 * where in the window, on what (FM_CONTEXT_*), and the sidebar's place
	 * pressed (-1 for none), which the place's actions work on.
	 */
	int context_x;
	int context_y;
	unsigned context_where;
	int context_place;

	/* The ways to open the selection the menus last showed, kept for the file they were read for (its path and time). */
	struct fm_opener menu_openers[FM_OPENERS];
	int menu_opener_count;
	char menu_openers_path[FM_PATH_MAX];
	time_t menu_openers_modified;

	/* Whether the user chose the default of that file's type (Always Open With), kept with its ways. */
	int menu_user_default;
};

/*
 * The questions the window asks before an action that cannot be undone.
 */
enum fm_dialog {
	FM_DIALOG_NONE,
	FM_DIALOG_DELETE,
	FM_DIALOG_EMPTY_TRASH,
	FM_DIALOG_COLLISION,
	FM_DIALOG_MOUNT,
	FM_DIALOG_CLEAR_RECENTS
};

/* The indexes of the buttons (FM_HIT_BUTTON) the frame records. */
#define FM_BUTTON_CANCEL	0
#define FM_BUTTON_CONFIRM	1
#define FM_BUTTON_PUT_BACK	10
#define FM_BUTTON_EMPTY_TRASH	11
#define FM_BUTTON_LOOK_CLOSE	12
#define FM_BUTTON_INFO_CLOSE	13
#define FM_BUTTON_INFO_CHECKSUM	14
#define FM_BUTTON_HELP_CLOSE	15
#define FM_BUTTON_REPLACE	16
#define FM_BUTTON_SKIP		17
#define FM_BUTTON_KEEP_BOTH	18
#define FM_BUTTON_APPLY_ALL	19
#define FM_BUTTON_MERGE		20
#define FM_BUTTON_CLEAR_RECENTS	21
#define FM_BUTTON_TASK_CANCEL	100
#define FM_BUTTON_OPENER	180

/* The indexes of the regions over the window (FM_HIT_OVERLAY): a card that takes clicks, and the dimmed grounds of Quick Look, the information and Help. */
#define FM_OVERLAY_CARD		0
#define FM_OVERLAY_LOOK_GROUND	1
#define FM_OVERLAY_INFO_GROUND	2
#define FM_OVERLAY_HELP_GROUND	3
#define FM_BUTTON_REMOVE_PLACE	200
#define FM_BUTTON_EJECT_PLACE	300

/* The interface (ui.c). */
int fm_app_init(struct fm_app *app, struct kl_text *text, const char *start);
void fm_app_release(struct fm_app *app);
void fm_ui_event(struct fm_app *app, const struct fm_event *event);
void fm_ui_tick(struct fm_app *app, uint64_t now);
void fm_ui_draw(struct fm_app *app, struct kl_canvas *canvas);
void fm_ui_damage(struct fm_app *app, const struct kl_rect *rect);
void fm_ui_frame_part(struct fm_app *app, const struct kl_rect *rect, int margin);
int fm_ui_hit_rect(const struct fm_app *app, unsigned kind, int index, struct kl_rect *rect);

/* The desktop's icons and their input (ui-desktop.c, ws094-p003, p004). */
void fm_desktop_draw(struct fm_app *app, struct kl_canvas *canvas);
void fm_desktop_repaint(struct fm_desktop *desk);
void fm_desktop_event(struct fm_app *app, const struct fm_event *event);
void fm_desktop_open_selected(struct fm_app *app);
int fm_desktop_item_at(struct fm_app *app, int x, int y);

/* The desktop's drag of items and the drops on it (ui-desktop-drag.c, ws094-p006). */
void fm_desktop_drag_press(struct fm_app *app, int index, int x, int y);
int fm_desktop_drag_motion(struct fm_app *app, int x, int y);
void fm_desktop_drag_release(struct fm_app *app);
void fm_desktop_drop_event(struct fm_app *app, const struct fm_event *event);
void fm_desktop_drop_draw(struct fm_app *app, struct kl_canvas *canvas);
int fm_desktop_drop_place(struct fm_app *app);
void fm_desktop_dropped(struct fm_app *app, char *const *paths, size_t count);

/* The desktop's context menus' actions and its keys for the file operations (ui-desktop-actions.c, ws094-p005). */
void fm_desktop_action(struct fm_app *app, unsigned action);
int fm_desktop_operation_key(struct fm_app *app, const struct fm_event *event);
void fm_desktop_rename_end(struct fm_app *app, int commit);
int fm_desktop_can_change_wallpaper(void);

/* The desktop's grid and its layout file (desktop-layout.c, ws094-p004). */
void fm_desktop_grid(int width, int height, int *columns, int *rows);
int fm_desktop_cell_rect(int column, int row, int width, int height, struct kl_rect *rect);
int fm_desktop_cell_at(int x, int y, int width, int height, int *column, int *row);
void fm_desktop_arrange(const char *const *names, size_t count, const struct fm_desktop_saved *saved, size_t saved_count, int width, int height, struct fm_desktop_place *places);

/* An item's name as the desktop shows it under its icon: one or two lines, the middle left out of a longer one (desktop-layout.c, ws094-p010). */
#define FM_DESKTOP_LABEL_MAX	256U
void fm_desktop_label(struct kl_text *text, const char *name, int width, unsigned pixels, char *first, char *second);
int fm_desktop_layout_path(char *path, size_t size);
int fm_desktop_layout_read(const char *path, struct fm_desktop_saved **saved, size_t *count);
int fm_desktop_layout_write(const char *path, const struct fm_desktop_saved *saved, size_t count);
int fm_desktop_layout_set(struct fm_desktop *desk, const char *name, int column, int row);
void fm_desktop_layout_prune(struct fm_desktop *desk, const char *const *names, size_t count);
int fm_desktop_clean_up(struct fm_desktop *desk);
int fm_desktop_layout_rename(struct fm_desktop *desk, const char *old_name, const char *new_name);
int fm_desktop_remember(struct fm_desktop *desk, const char *const *names, size_t count);
void fm_desktop_release(struct fm_desktop *desk);
void fm_ui_hit(struct fm_app *app, const struct kl_rect *rect, unsigned kind, int index);
size_t fm_ui_panels(struct fm_app *app, struct fm_panel *panels, size_t capacity);
struct fm_tab *fm_ui_tab(struct fm_app *app);
void fm_ui_go(struct fm_app *app, const struct fm_location *location);
void fm_log(const char *format, ...);
int fm_ui_crumbs(struct fm_app *app, struct fm_crumb *crumbs, int capacity);
void fm_ui_reload(struct fm_app *app, struct fm_tab *tab);
void fm_ui_back(struct fm_app *app);
void fm_ui_forward(struct fm_app *app);
void fm_ui_open(struct fm_app *app, int index);
void fm_ui_message(struct fm_app *app, const char *message);
int fm_ui_wait(struct fm_app *app);

/* The pointer and the keyboard (ui-input.c). */
void fm_input_motion(struct fm_app *app, const struct fm_event *event);
void fm_input_button(struct fm_app *app, const struct fm_event *event);
void fm_input_scroll(struct fm_app *app, int amount);

/* The content's overlay scroll bar (ui-scrollbar.c, ws127-p002). */
void fm_scrollbar_moved(struct fm_app *app);
int fm_scrollbar_motion(struct fm_app *app, int x, int y);
int fm_scrollbar_press(struct fm_app *app, int x, int y);
int fm_scrollbar_release(struct fm_app *app);
void fm_scrollbar_leave(struct fm_app *app);
void fm_scrollbar_draw(struct fm_app *app, struct kl_canvas *canvas);
int fm_scrollbar_busy(struct fm_app *app);
void fm_input_key(struct fm_app *app, const struct fm_event *event);
int fm_input_hit_at(struct fm_app *app, int x, int y, unsigned *kind, int *index);
void fm_input_sort_by(struct fm_app *app, unsigned sort, int reverse);
void fm_input_open_selection(struct fm_app *app);
void fm_input_enclosing(struct fm_app *app);
void fm_input_location(struct fm_app *app);
void fm_input_location_go(struct fm_app *app);
void fm_location_tick(struct fm_app *app);

/* The content panel and the icon view (ui-grid.c). */
void fm_grid_draw(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *area);
void fm_grid_entry_icon(struct fm_app *app, struct kl_canvas *canvas, const struct fm_entry *entry, float x, float y, float size);
void fm_view_item_rect(struct fm_app *app, int index, struct kl_rect *rect);

/* The list view (ui-list.c). */
void fm_list_draw(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *inner);
void fm_time_text(time_t when, time_t now, char *text, size_t size);
int fm_list_sort_at(struct fm_app *app, int index);
void fm_list_edge_press(struct fm_app *app, int column, int x);
int fm_list_edge_motion(struct fm_app *app, int x);
int fm_list_edge_release(struct fm_app *app);
void fm_list_widths_load(struct fm_app *app);

/* The text fields (ui-field.c). */
char fm_key_character(uint32_t key, uint32_t modifiers);
void fm_field_set(struct fm_field *field, const char *text);
void fm_field_select(struct fm_field *field, size_t start, size_t end);
unsigned fm_field_key(struct fm_field *field, uint32_t key, uint32_t modifiers);
void fm_field_insert(struct fm_field *field, const char *text, size_t length);
int fm_widgets_begin(struct fm_app *app);
void fm_widgets_end(struct fm_app *app);
void fm_widgets_input(struct fm_app *app, const struct fm_event *event);
void fm_widgets_release(struct fm_app *app);
void fm_style(struct fm_app *app, struct kl_canvas *canvas, struct kl_style *style);
int fm_button_width(struct fm_app *app, const char *label);
void fm_button(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *rect, const char *label, int index, unsigned flags);
void fm_icon_button(struct fm_app *app, struct kl_canvas *canvas, uint32_t id, int index, const struct kl_rect *rect, enum kl_icon icon, int pixels, unsigned flags);

/* files' buttons' IDs among libkeiland's widgets (ui-widgets.c), the button's index after it. */
#define FM_WIDGET_BUTTON	0x20000000U

/* The IDs of files' buttons that are a picture alone: a button's (its FM_BUTTON_* index), and a tab's close button (the tab's index). */
#define FM_WIDGET_ICON		0x30000000U
#define FM_WIDGET_TAB_CLOSE	0x30000001U
int fm_rename_start(struct fm_app *app, const char *name, size_t stem);
void fm_rename_select_all(struct fm_app *app);
int fm_rename_input(struct fm_app *app, const struct fm_event *event);
void fm_rename_draw(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *rect);
int fm_rename_hit(const struct fm_app *app, int x, int y);
void fm_rename_take(struct fm_app *app);
void fm_field_draw(struct fm_app *app, struct kl_canvas *canvas, const struct fm_field *field, const struct kl_rect *rect, unsigned pixels, const char *placeholder);

/* The search (search.c). */
void fm_search_start(struct fm_search *search, const char *query, const char *base, int hidden);
int fm_search_step(struct fm_search *search, struct fm_listing *listing, uint64_t budget_ms);
void fm_search_stop(struct fm_search *search);

/* Today, the dashboard (ui-home.c). */
void fm_home_gather(struct fm_app *app);
void fm_home_folder_opened(const char *folder);
void fm_home_draw(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *inner);
void fm_mark_draw(struct kl_canvas *canvas, int x, int y, unsigned pixels, float opacity);
void fm_home_click(struct fm_app *app, unsigned kind, int index, int double_click);

/* The places that are not one folder and the search field (ui-search.c). */
void fm_search_focus(struct fm_app *app);
void fm_search_key(struct fm_app *app, const struct fm_event *event);
void fm_search_cancel(struct fm_app *app);
void fm_search_tick(struct fm_app *app);
void fm_search_scope(struct fm_app *app, unsigned scope);
void fm_search_load(struct fm_app *app, struct fm_tab *tab);
void fm_recent_add(const char *path);

/* The file operations (actions.c). */
void fm_action_copy(struct fm_app *app, int cut);
void fm_action_paste(struct fm_app *app);
void fm_action_duplicate(struct fm_app *app);
void fm_action_trash(struct fm_app *app);
void fm_action_delete(struct fm_app *app);
void fm_action_empty_trash(struct fm_app *app);
void fm_action_clear_recents(struct fm_app *app);
void fm_action_confirm(struct fm_app *app, int confirmed);
void fm_action_put_back(struct fm_app *app);
void fm_action_new_folder(struct fm_app *app);
void fm_action_rename_begin(struct fm_app *app);
void fm_action_rename_end(struct fm_app *app, int commit);
void fm_action_undo(struct fm_app *app, int redo);
void fm_action_cancel_task(struct fm_app *app, int index);
void fm_action_add_favorite(struct fm_app *app);
int fm_action_transfer(struct fm_app *app, unsigned kind, char *const *paths, size_t count, const char *destination);
void fm_action_collision(struct fm_app *app, unsigned answer);
void fm_action_collision_cancel(struct fm_app *app);
void fm_action_collision_all(struct fm_app *app);
size_t fm_action_collision_left(const struct fm_app *app);
void fm_action_remove_favorite(struct fm_app *app, int place);
int fm_actions_tick(struct fm_app *app);
void fm_actions_release(struct fm_app *app);
const char *fm_current_folder(struct fm_app *app);
int fm_selected_paths(struct fm_app *app, char ***paths, size_t *count);

/* The dialogs and the tasks' list (ui-overlay.c). */
void fm_overlay_draw(struct fm_app *app, struct kl_canvas *canvas);
void fm_tasks_draw(struct fm_app *app, struct kl_canvas *canvas, int x, int y);
void fm_task_text(const struct fm_task *task, char *text, size_t size);

/* The selection (select.c). */
void fm_select_none(struct fm_tab *tab);
void fm_select_only(struct fm_tab *tab, int index);
void fm_select_toggle(struct fm_tab *tab, int index);
void fm_select_range(struct fm_tab *tab, int from, int to);
void fm_select_all(struct fm_tab *tab);
size_t fm_select_count(struct fm_tab *tab, uint64_t *bytes);
int fm_select_first(struct fm_tab *tab);
int fm_select_find(struct fm_tab *tab, const char *name);

/* The listing of a place (dir.c). */
int fm_dir_read(struct fm_listing *listing, const char *path, int hidden);
void fm_dir_sort(struct fm_listing *listing, unsigned sort, int reverse);
void fm_dir_free(struct fm_listing *listing);
struct fm_entry *fm_dir_add(struct fm_listing *listing, const char *folder, const char *name);
int fm_dir_count(const char *path, int hidden);
int fm_dir_read_trash(struct fm_listing *listing);
void fm_dir_size_text(uint64_t size, char *text, size_t length);
void fm_dir_items_text(long count, char *text, size_t length);
void fm_owner_text(uid_t uid, gid_t gid, char *text, size_t length);

/* The file types (mime.c). */
const struct fm_mime *fm_mime_guess(const char *name, mode_t mode);
const struct fm_mime *fm_mime_sniff(const char *path, const struct fm_mime *guess);
kl_color fm_mime_color(unsigned category);
void fm_mime_label(const char *name, char *label, size_t size);
int fm_mime_text(const unsigned char *bytes, size_t length);

/* The pictures and the thumbnails (thumb.c). */
int fm_image_load(const char *path, struct kl_image *image);
int fm_image_thumbnail(const char *path, int side, struct kl_image *thumbnail);
const struct kl_image *fm_thumb_get(struct fm_app *app, const char *path, time_t modified);
int fm_thumb_tick(struct fm_app *app);
int fm_thumb_busy(void);
void fm_thumb_release(struct fm_app *app);
void fm_image_fit(int width, int height, int box_width, int box_height, int *fit_width, int *fit_height);

/* What is read of a file to show it (peek.c). */
void fm_peek_read(struct fm_peek *peek, const struct fm_entry *entry);
void fm_peek_picture(struct fm_peek *peek, int side);
void fm_peek_release(struct fm_peek *peek);

/* The preview pane and Quick Look (ui-preview.c). */
void fm_preview_draw(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *area);
void fm_look_draw(struct fm_app *app, struct kl_canvas *canvas);
void fm_look_toggle(struct fm_app *app);
void fm_look_close(struct fm_app *app);
void fm_look_step(struct fm_app *app, int step);
int fm_preview_item(struct fm_app *app);

/* The applications that open files (apps.c). */
int fm_apps_for(const char *path, const struct fm_mime *mime, mode_t mode, struct fm_opener *openers, int capacity);
int fm_apps_is_quicklook(const struct fm_opener *opener);
int fm_apps_set_default(const char *type, const struct fm_opener *opener);
int fm_apps_clear_default(const char *type);
int fm_apps_has_default(const char *type);
int fm_apps_launch(const struct fm_opener *opener, const char *path);
int fm_apps_spawn(char *const arguments[]);

/* What the information card shows of a file (info.c). */
int fm_info_gather(struct fm_info *info, const char *path);
int fm_info_checksum_start(struct fm_info *info);
int fm_info_checksum_step(struct fm_info *info, uint64_t budget_ms);
void fm_info_release(struct fm_info *info);
void fm_mode_text(mode_t mode, char *text, size_t size);

/* The information card (ui-info.c). */
void fm_info_open(struct fm_app *app);
void fm_info_close(struct fm_app *app);
void fm_info_draw(struct fm_app *app, struct kl_canvas *canvas);
void fm_info_tick(struct fm_app *app);
void fm_info_button(struct fm_app *app, int index);
void fm_open_entry(struct fm_app *app, int index, int opener);
void fm_open_with(struct fm_app *app, const struct fm_opener *opener, const char *path);

/* The menus' actions and state (ui-menu.c). */
void fm_ui_action(struct fm_app *app, unsigned action);
void fm_ui_menu_state(struct fm_app *app, struct fm_menu_state *state);
void fm_ui_context(struct fm_app *app, struct fm_context *context);
int fm_ui_context_action(struct fm_app *app, unsigned action);

/* Dragging the selected items within the window (ui-drag.c). */
int fm_drag_motion(struct fm_app *app, int x, int y);
int fm_drag_release(struct fm_app *app);
void fm_drag_cancel(struct fm_app *app);
void fm_drag_draw(struct fm_app *app, struct kl_canvas *canvas);
void fm_drop_event(struct fm_app *app, const struct fm_event *event);
int fm_drag_tick(struct fm_app *app, uint64_t now);

/* The thumbnails kept on disk (thumb-cache.c, ws127-p002; ws168-p004: written by keiland-preview). */
int fm_thumb_kind(const struct fm_entry *entry);
int fm_thumb_cache_read(const char *path, struct kl_image *image);
int fm_thumb_cache_target(const char *path, char *record, size_t record_size, char *stamp, size_t stamp_size);
int fm_thumb_cache_trim(unsigned maximum, unsigned keep);
int fm_drop_accepts(const struct fm_app *app);
void fm_drop_perform(struct fm_app *app, char *const *paths, size_t count);

/* The tabs (ui-tabs.c). */
void fm_tabs_new(struct fm_app *app, const struct fm_location *location);
void fm_tabs_duplicate(struct fm_app *app);
void fm_tabs_close(struct fm_app *app, int index);
void fm_tabs_select(struct fm_app *app, int index);
void fm_tabs_step(struct fm_app *app, int step);
int fm_tabs_layout(struct fm_app *app, int top, int left, int right);
void fm_tabs_draw(struct fm_app *app, struct kl_canvas *canvas);

/* The titlebar's state and what is done with it (ui-titlebar.c). */
void fm_ui_titlebar_state(struct fm_app *app, struct fm_titlebar_state *state);
void fm_ui_titlebar(struct fm_app *app, const struct fm_titlebar_event *event);

/* The Help cards (ui-help.c). */
void fm_help_open(struct fm_app *app, unsigned help);
void fm_help_close(struct fm_app *app);
void fm_help_draw(struct fm_app *app, struct kl_canvas *canvas);

/* The sidebar's places (places.c). */
void fm_places_init(struct fm_places *places, const char *home);
int fm_places_add_favorite(struct fm_places *places, const char *path);
int fm_places_remove_favorite(struct fm_places *places, int removed);
int fm_places_move_favorite(struct fm_places *places, int moved, int to);
const char *fm_location_name(const struct fm_location *location, const char *home);
int fm_place_is_favorite_folder(const struct fm_place *place);

/* The removable devices (devices.c, ws132-p005). */
void fm_devices_set(struct fm_app *app, const struct fm_device *list, int count, int blink_all);
float fm_devices_blink(const struct fm_app *app, const struct fm_device *device);
int fm_devices_blinking(const struct fm_app *app);
void fm_devices_mount(struct fm_app *app, int index);
void fm_devices_mount_answer(struct fm_app *app, int confirmed);
const struct fm_device *fm_devices_find(const struct fm_app *app, const char *id);
void fm_devices_eject(struct fm_app *app, int index);

#endif
