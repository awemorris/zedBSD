/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The settings tab of terminal: a page of libkeiland's widgets, drawn on
 * the processor into a frame the drawing shows in place of a grid
 * (display.h's terminal_display_page), with a sidebar of four panes:
 *
 *   Hosts       the hosts of the ssh_config the program reads, each in a
 *               form (its name, address, port, user, command, identity
 *               files and forwards), its other lines shown; the file
 *               opens in the system's text editor for the rest;
 *   Appearance  the text's size, the theme and the ambiguous width (at
 *               once), and the program's own settings of the look;
 *   Behavior    copy-on-select (at once), and the program's own settings
 *               of the connections;
 *   Keys        a new ssh-ed25519 key, the public key of one there, and
 *               the lines of known_hosts, each of which can be removed.
 *
 * Each setting says when it takes effect: at once, from the next
 * connection, or from the program's next start.  The settings the run
 * carries out at once go to it as the menus' actions (window->actions);
 * the rest the program keeps (struct terminal_page_program).
 *
 * While the tab is active the window's input comes here (window.c): the
 * widgets take the pointer, the wheel, the keys and the input method's
 * text.  Copy, cut and paste in a field go through the terminal's own
 * clipboard calls (clipboard.c), so they work wherever the window does.
 * The page is one for the run (one window), made the first time the tab
 * opens.
 */

#include "settings-page.h"
#include "settings-hosts.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The widgets' ids (each widget's, or its first with an index). */
#define PAGE_ID_PANES		1U
#define PAGE_ID_SCROLL		2U
#define PAGE_ID_HOSTS_SAVE	4U
#define PAGE_ID_HOSTS_REVERT	5U
#define PAGE_ID_SMALLER		6U
#define PAGE_ID_LARGER		7U
#define PAGE_ID_AMBIGUOUS	9U
#define PAGE_ID_COPY_SELECT	10U
#define PAGE_ID_ITEMS_SAVE	11U
#define PAGE_ID_KEY_NAME	12U
#define PAGE_ID_KEY_COMMENT	13U
#define PAGE_ID_KEY_PASS	14U
#define PAGE_ID_KEY_AGAIN	15U
#define PAGE_ID_KEY_MAKE	16U
#define PAGE_ID_KEY_COPY	17U
#define PAGE_ID_KNOWN_REMOVE	18U
#define PAGE_ID_REMOVE_YES	19U
#define PAGE_ID_REMOVE_NO	20U
#define PAGE_ID_KNOWN_RELOAD	21U
#define PAGE_ID_ITEM		32U
#define PAGE_ID_SIZES		48U
#define PAGE_ID_THEMES		56U
#define PAGE_ID_HOST_ROWS	64U
#define PAGE_ID_HOST_NEW	72U
#define PAGE_ID_HOST_ADD	73U
#define PAGE_ID_HOST_EDITOR	74U
#define PAGE_ID_IDENTITY_NEW	75U
#define PAGE_ID_IDENTITY_ADD	76U
#define PAGE_ID_LINE_REMOVE	77U
#define PAGE_ID_FORWARD_LOCAL	78U
#define PAGE_ID_FORWARD_REMOTE	79U
#define PAGE_ID_FORWARD_LISTEN	80U
#define PAGE_ID_FORWARD_TO	81U
#define PAGE_ID_FORWARD_ADD	82U
#define PAGE_ID_HOST_REMOVE	83U
#define PAGE_ID_HOST_REMOVE_YES	84U
#define PAGE_ID_HOST_REMOVE_NO	85U
#define PAGE_ID_KEY_PASTE_AREA	86U
#define PAGE_ID_KEY_PASTE	87U
#define PAGE_ID_KEY_SAVE	88U
#define PAGE_ID_KEY_PUBLIC_AREA	89U
#define PAGE_ID_HOST_FIELD	96U

/* The layout: the margin round the panels, the sidebar's width, a pane's padding and the gap between cards, in pixels. */
#define PAGE_MARGIN		12
#define PAGE_SIDEBAR		196
#define PAGE_SIDEBAR_NARROW	150
#define PAGE_NARROW		640
#define PAGE_PAD		16
#define PAGE_GAP		14
#define PAGE_ITEM_HEIGHT	32

/* A card's row, the space a card's title takes (with a subtitle under it), the room below its last row, and a note's line. */
#define PAGE_ROW		40
#define PAGE_CARD_TITLE	46
#define PAGE_CARD_SUBTITLED	66
#define PAGE_CARD_FOOT		12
#define PAGE_NOTE_LINE		18

/*
 * A row's text inside a card: how far in from the card's left edge it
 * starts (a card's padding, as kl_row draws its label) and its size.
 */
#define PAGE_INSET		20
#define PAGE_TEXT_ROW		14U

/*
 * The label column of a pane's rows, from a row's text to its control: the
 * widest label of the pane and the gap after it, within a least and a most,
 * and in a narrow window at most a share of the row.  Then the widest a
 * field stands (a wide window does not stretch it), and the room between a
 * control and the words after it.
 */
#define PAGE_LABEL_GAP		24
#define PAGE_LABEL_MIN		96
#define PAGE_LABEL_MAX		220
#define PAGE_LABEL_SHARE	0.4f
#define PAGE_FIELD_MAX		420
#define PAGE_AFTER_GAP		12

/* The widest a known host's words (or a line of a host) are drawn, its removal's button after them. */
#define PAGE_KNOWN_LINE	480

/* The most lines of a note. */
#define PAGE_NOTE_LINES	4

/* A text area's line and the room it keeps above and below its lines (the widget's own), and how many lines the key's areas show. */
#define PAGE_AREA_LINE		20
#define PAGE_AREA_PAD		18
#define PAGE_AREA_PASTE_LINES	8
#define PAGE_AREA_PUBLIC_LINES	4

/* A removal's icon button, square, and its icon. */
#define PAGE_ICON_BUTTON	28
#define PAGE_ICON_PIXELS	16

/*
 * The fields of a host's form: Host, HostName, Port, User and
 * RemoteCommand, then the X11 lines from PAGE_HOST_X11 (ForwardX11,
 * ForwardX11Trusted, ForwardX11Timeout, X11RootWindow, X11WindowTabify;
 * only for a program that forwards X11).  Then the most of a block's
 * other lines shown.
 */
#define PAGE_HOST_FIELDS	10U
#define PAGE_HOST_NAME		0U
#define PAGE_HOST_PORT		2U
#define PAGE_HOST_COMMAND	4U
#define PAGE_HOST_X11		5U
#define PAGE_OTHER_LINES	40U

/* The longest line of the Hosts pane's words (a host's title, a line shown), with its NUL. */
#define PAGE_HOST_TEXT		256U

/*
 * What a widget of the Hosts pane asks for, carried out after the frame is
 * drawn (the text and its lines change only between frames).
 */
#define PAGE_HOSTS_NOTHING	0U
#define PAGE_HOSTS_SELECT	1U
#define PAGE_HOSTS_ADD_HOST	2U
#define PAGE_HOSTS_REMOVE_ASK	3U
#define PAGE_HOSTS_REMOVE	4U
#define PAGE_HOSTS_KEEP		5U
#define PAGE_HOSTS_ADD_IDENTITY	6U
#define PAGE_HOSTS_ADD_FORWARD	7U
#define PAGE_HOSTS_REMOVE_LINE	8U
#define PAGE_HOSTS_SAVE		9U
#define PAGE_HOSTS_REVERT	10U
#define PAGE_HOSTS_EDITOR	11U

/*
 * The evdev codes of the keys the page answers itself: C, V and X with
 * Control, R and S with Control in the Hosts pane (Revert and Save), and
 * Up, Down, Page Up and Page Down with Control (the panes).
 */
#define PAGE_KEY_R		19U
#define PAGE_KEY_S		31U
#define PAGE_KEY_X		45U
#define PAGE_KEY_C		46U
#define PAGE_KEY_V		47U
#define PAGE_KEY_UP		103U
#define PAGE_KEY_PAGEUP	104U
#define PAGE_KEY_DOWN		108U
#define PAGE_KEY_PAGEDOWN	109U

/* How many pixels a line of the page's scroll is, for the keys. */
#define PAGE_SCROLL_LINE	40.0

/* The largest known_hosts read. */
#define PAGE_KNOWN_MAX		(1024U * 1024U)

/* The longest message a pane shows. */
#define PAGE_MESSAGE		512U

/* The sizes of View > Text Size, which the Appearance pane steps through. */
#define PAGE_SIZES		4U

/*
 * A message a pane shows after the user did something: its text and
 * whether it tells of a failure (drawn in the theme's bad ink).
 */
struct page_message {
	char text[PAGE_MESSAGE];
	int bad;
};

/*
 * The page, for the run's whole life: the program's files, the widgets'
 * input and text, the frame and its canvas, the pane shown and its
 * scroll, and each pane's state.  The main thread's alone.
 */
struct page_state {
	/* The program, and whether the page was made (the rest is valid then). */
	const struct terminal_page_program *program;
	int made;

	/* The widgets' input, the text they draw with (text_open when its fonts were read) and their style. */
	struct kl_ui *ui;
	struct kl_text text;
	int text_open;
	struct kl_style style;

	/*
	 * The frame (frame_width by frame_height pixels), the scale it is drawn
	 * at (in 96ths), its size in the widgets' units at that scale (width by
	 * height), and the canvas over it (canvas_made when it is).
	 */
	uint32_t *pixels;
	uint32_t frame_width;
	uint32_t frame_height;
	unsigned scale;
	uint32_t width;
	uint32_t height;
	struct kl_canvas canvas;
	int canvas_made;

	/* The pane shown, whether a frame is wanted, and whether something moves by itself. */
	unsigned pane;
	int dirty;
	int moving;

	/* The label column of the pane drawn (page_column), in units. */
	int column;

	/* The pane's scroll, its viewport and how tall its content was drawn. */
	struct kl_scroll scroll;
	int scroll_made;
	struct kl_rect viewport;
	int content_height;

	/* Whether the window's text input was asked on, as last told (-1 before the first frame). */
	int text_wanted;

	/*
	 * Hosts: the ssh_config's bytes as last read or saved (original, which
	 * a save compares the file with, and which the check tells new problems
	 * from), the text with the pane's changes not saved yet (text, read into
	 * hosts), whether the file was read (and was too large or could not be
	 * read: nothing to edit), the block shown (TERMINAL_HOSTS_NONE: none)
	 * and the one the form's fields were filled from, with what each field
	 * was filled with (a field that still holds it changes nothing), the
	 * fields of a new host, identity file and forward (remote or local), a
	 * removal of the host asked, what a widget asked for in the frame drawn
	 * (PAGE_HOSTS_*, and the block or line it is about), and the last word.
	 */
	char *hosts_original;
	size_t hosts_original_size;
	char *hosts_text;
	size_t hosts_size;
	struct terminal_hosts hosts;
	int hosts_read;
	int hosts_unusable;
	unsigned hosts_block;
	unsigned hosts_filled;
	struct kl_field host_fields[PAGE_HOST_FIELDS];
	char host_filled[PAGE_HOST_FIELDS][KL_FIELD_MAX];
	struct kl_field host_new;
	struct kl_field identity_new;
	struct kl_field forward_listen;
	struct kl_field forward_to;
	int forward_remote;
	int host_remove_asked;
	unsigned hosts_later;
	unsigned hosts_later_what;
	struct page_message hosts_message;

	/* The program's settings: their fields (read when items_read), and the last save's word. */
	struct kl_field items[TERMINAL_ITEMS_MAX];
	int items_read;
	struct page_message items_message;

	/* Keys: the fields of a new key, the key shown (its name, whether it is there, its line and fingerprint) and the last word. */
	struct kl_field key_name;
	struct kl_field key_comment;
	struct kl_field key_pass;
	struct kl_field key_again;
	struct kl_text_area key_paste_area;
	struct kl_text_area key_public_area;
	struct page_message key_paste_message;
	char key_shown[TERMINAL_KEY_NAME_MAX];
	int key_looked;
	int key_found;
	char key_line[TERMINAL_KEY_LINE_MAX];
	char key_fingerprint[64];
	struct page_message key_message;

	/* The text the next Copy takes in place of the focused field's selection (a cut's, or the public key), when copied_set. */
	char copied[KL_FIELD_MAX + TERMINAL_KEY_LINE_MAX];
	size_t copied_length;
	int copied_set;

	/* Known hosts: the lines listed (known_count of known_total), whether read, the line asked to be removed (0: none) and the last word. */
	struct terminal_known_line known[TERMINAL_KNOWN_MAX];
	unsigned known_count;
	unsigned known_total;
	int known_read;
	unsigned remove_line;
	struct page_message known_message;
};

/* The page (large: the known hosts and the host's fields; one for the run). */
static struct page_state page;

/* The panes' names and icons, in their order. */
static const char *const page_pane_names[TERMINAL_PANES] = { "Hosts", "Appearance", "Behavior", "Keys" };
static const enum kl_icon page_pane_icons[TERMINAL_PANES] = { KL_ICON_COMPUTER, KL_ICON_PALETTE, KL_ICON_KEYBOARD, KL_ICON_LOCK };

/* The themes' names and their actions, in the order of TERMINAL_THEME_*. */
static const char *const page_theme_names[TERMINAL_THEMES] = { "Dark", "Light", "High Contrast" };
static const uint32_t page_theme_actions[TERMINAL_THEMES] = { TERMINAL_ACTION_THEME_DARK, TERMINAL_ACTION_THEME_LIGHT, TERMINAL_ACTION_THEME_CONTRAST };

/* The sizes of View > Text Size, their names and actions. */
static const unsigned page_sizes[PAGE_SIZES] = { TERMINAL_PIXELS_SMALL, TERMINAL_PIXELS_MEDIUM, TERMINAL_PIXELS_LARGE, TERMINAL_PIXELS_HUGE };
static const char *const page_size_names[PAGE_SIZES] = { "Small", "Medium", "Large", "Huge" };
static const uint32_t page_size_actions[PAGE_SIZES] = { TERMINAL_ACTION_SIZE_SMALL, TERMINAL_ACTION_SIZE_MEDIUM, TERMINAL_ACTION_SIZE_LARGE, TERMINAL_ACTION_SIZE_HUGE };

/* The labels of the rows each pane draws itself, which its label column is measured for (with the program's settings in it). */
static const char *const page_appearance_labels[] = { "Text size", "Size", "Theme", "Ambiguous width", "Font file" };
static const char *const page_behavior_labels[] = { "Copy on select" };
static const char *const page_key_labels[] = { "Name", "Comment", "Passphrase", "Again" };
static const char *const page_host_labels[] = {
	"Name", "Host address", "Port", "User", "Command", "Identity file", "Forward", "Listen", "Destination", "New host",
	"Forward X11", "Trusted", "Timeout", "Root window", "Window tabs"
};

/* How many of the Hosts pane's labels a program that does not forward X11 has (the X11 ones are last). */
#define PAGE_HOST_LABELS	10U

/* The keywords of a host's form, its labels and what each field shows empty, in the fields' order. */
static const char *const page_host_keywords[PAGE_HOST_FIELDS] = {
	"Host", "HostName", "Port", "User", "RemoteCommand",
	"ForwardX11", "ForwardX11Trusted", "ForwardX11Timeout", "X11RootWindow", "X11WindowTabify"
};
static const char *const page_host_field_labels[PAGE_HOST_FIELDS] = {
	"Name", "Host address", "Port", "User", "Command",
	"Forward X11", "Trusted", "Timeout", "Root window", "Window tabs"
};
static const char *const page_host_placeholders[PAGE_HOST_FIELDS] = {
	"myserver (Host)", "myserver.example.com (HostName)", "22 (Port)", "The login name (User)", "Run instead of a shell (RemoteCommand)",
	"yes or no (ForwardX11)", "yes or no (ForwardX11Trusted)", "A time, as 20m (ForwardX11Timeout)",
	"off, or a size: 1280x800 (X11RootWindow)", "yes or no (X11WindowTabify)"
};

static int page_frame(uint32_t width, uint32_t height, unsigned scale);
static double page_units(double pixels);
static int page_pixels(int units);
static void page_layout(struct kl_rect *sidebar, struct kl_rect *content);
static void page_sidebar(const struct kl_rect *sidebar);
static int page_hosts(struct terminal_window *window, int top, int left, int width);
static int page_hosts_list(int top, int left, int width);
static int page_hosts_form(int top, int left, int width);
static int page_hosts_identities(int top, int left, int width);
static int page_hosts_forwards(int top, int left, int width);
static int page_hosts_x11(int top, int left, int width);
static unsigned page_hosts_fields(void);
static int page_hosts_other(int top, int left, int width);
static int page_hosts_actions(int top, int left, int width);
static void page_hosts_read(void);
static int page_hosts_take(char *text, size_t size);
static void page_hosts_select(unsigned block);
static void page_hosts_fill(void);
static int page_hosts_apply(void);
static void page_hosts_add_host(void);
static void page_hosts_remove_host(void);
static void page_hosts_add_identity(void);
static void page_hosts_add_forward(void);
static void page_hosts_remove_line(unsigned line);
static void page_hosts_save(void);
static void page_hosts_editor(struct terminal_window *window);
static int page_hosts_listed(unsigned block);
static unsigned page_hosts_lines(unsigned block);
static void page_hosts_title(unsigned block, char *text, size_t size);
static void page_hosts_where(unsigned line, char *text, size_t size);
static void page_hosts_forward_text(unsigned line, char *text, size_t size);
static int page_hosts_other_line(unsigned line);
static int page_hosts_second(unsigned line);
static void page_hosts_later(unsigned action, unsigned what);
static void page_hosts_carry_out(struct terminal_window *window);
static const char *page_hosts_why(unsigned field, int error);
static int page_hosts_saved(void);
static unsigned page_hosts_first(void);
static int page_line_row(int top, int left, int width, const char *label, const char *text, kl_color ink, uint32_t index, int removable);
static int page_field_button(int top, int left, int width, const char *label, uint32_t id, struct kl_field *field, const char *placeholder, uint32_t button, const char *button_label);
static int page_appearance(struct terminal_window *window, const struct terminal_page_state *state, int top, int left, int width);
static int page_behavior(struct terminal_window *window, const struct terminal_page_state *state, int top, int left, int width);
static int page_keys(struct terminal_window *window, int top, int left, int width);
static int page_known(int top, int left, int width);
static int page_items(unsigned pane, int top, int left, int width);
static void page_items_read(void);
static void page_items_save(unsigned pane);
static void page_key_look(void);
static void page_key_make(void);
static void page_key_save(struct terminal_window *window);
static int page_focused_area(struct kl_text_area **area);
static int page_paste_card(struct terminal_window *window, int top, int left, int width);
static void page_known_read(void);
static void page_known_remove(void);
static int page_card(const struct kl_rect *rect, const char *title, const char *subtitle);
static void page_column(unsigned pane, const char *const *labels, unsigned count);
static int page_value_x(int left, int width);
static int page_label(int top, int left, int width, const char *label, int last);
static int page_control_width(int left, int width, int x);
static void page_row_text(int x, int top, int right, const char *text);
static int page_field_row(int top, int left, int width, const char *label, uint32_t id, struct kl_field *field, const char *placeholder);
static int page_note(int top, int left, int width, const char *text, kl_color ink);
static int page_note_height(int width, const char *text);
static int page_message_draw(int top, int left, int width, const struct page_message *message);
static void page_say(struct page_message *message, int bad, const char *text);
static void page_action(struct terminal_window *window, uint32_t action);
static void page_text_follow(struct terminal_window *window);
static int page_focused(struct kl_field **field);
static void page_selection(const char *text, size_t caret, size_t anchor, size_t *from, size_t *to);
static void page_commit(const char *text, size_t length);
static int page_key(struct terminal_window *window, const struct kl_window_event *event);
static void page_set_aside(const char *text, size_t length);
static int page_key_text(const struct kl_window_event *event);
static void page_pane(unsigned pane);
static void page_log(const char *what, const char *detail);
static const char *page_connection(void);

/*
 * Makes the settings tab's page the first time it is opened: the widgets'
 * input, the text in the given fonts (without them the page shows no
 * words) and each pane's fields.  Returns 0, or ENOMEM.
 */
int
terminal_page_open(
	const struct terminal_page_program *program,
	const char *font,
	const char *fallback)
{
	int error;

	/* Made once for the run; the program may only be told again. */
	page.program = program;
	if (page.made)
		return 0;

	/* The widgets' input. */
	page.ui = kl_ui_create();
	if (page.ui == NULL)
		return ENOMEM;

	/* The page's scroll, down only. */
	error = kl_scroll_init(&page.scroll, KL_SCROLL_Y);
	if (error != 0) {
		kl_ui_destroy(page.ui);
		page.ui = NULL;
		return ENOMEM;
	}

	/* The scroll is made: the page releases it when it closes. */
	page.scroll_made = 1;

	/* The text; a font that cannot be read leaves the page without words, which the log tells. */
	error = kl_text_open(&page.text, font, fallback);
	if (error == 0) {
		page.text_open = 1;
	} else {
		printf("ZTERM SETTINGS font-missing error=%d\n", error);
	}

	/* The style of the widgets: the desktop's theme, no glass. */
	page.style.text = &page.text;
	page.style.theme = kl_theme_default();
	page.style.glass = 0;

	/* The fields: a new key's name and comment by default, the secret ones shown as dots. */
	kl_field_set(&page.key_name, "id_ed25519");
	kl_field_set_limit(&page.key_name, TERMINAL_KEY_NAME_MAX - 1U);
	kl_field_set(&page.key_comment, program->comment);
	page.key_pass.secret = 1;
	page.key_again.secret = 1;
	kl_field_set_limit(&page.key_pass, 1024U);
	kl_field_set_limit(&page.key_again, 1024U);
	kl_text_area_set(&page.key_paste_area, "");
	kl_text_area_set(&page.key_public_area, "");

	/* No host shown before the file is read. */
	page.hosts_block = TERMINAL_HOSTS_NONE;
	page.hosts_filled = TERMINAL_HOSTS_NONE;

	/* Succeeded: the page is made, the Hosts pane first. */
	page.pane = TERMINAL_PANE_HOSTS;
	page.text_wanted = -1;
	page.dirty = 1;
	page.made = 1;
	return 0;
}

/*
 * Gives one input of the window to the page's widgets while the settings
 * tab is active: the pointer, the wheel, the keys (Control with C, X or V
 * copies, cuts or pastes in the field with the keyboard) and the input
 * method's text.  Returns 1 when the page is to be drawn again.
 */
int
terminal_page_input(
	struct terminal_window *window,
	const struct kl_window_event *event)
{
	struct kl_window_event scaled;
	int taken;

	/* Nothing before the page is made. */
	if (!page.made)
		return 0;

	/* The places and distances in the widgets' units (the window gives pixels). */
	scaled = *event;
	scaled.x = page_units(event->x);
	scaled.y = page_units(event->y);
	scaled.dx = page_units(event->dx);
	scaled.dy = page_units(event->dy);
	event = &scaled;

	/* What it is. */
	switch (event->kind) {
	case KL_WINDOW_MOTION:
		(void)kl_ui_pointer_motion(page.ui, event->x, event->y);
		break;
	case KL_WINDOW_LEAVE:
		(void)kl_ui_pointer_leave(page.ui);
		break;
	case KL_WINDOW_BUTTON:
		/* The main button only, where the pointer is. */
		(void)kl_ui_pointer_motion(page.ui, event->x, event->y);
		if (event->code == KL_BUTTON_LEFT)
			(void)kl_ui_pointer_button(page.ui, event->pressed, kl_clock_us());
		break;
	case KL_WINDOW_AXIS:
	case KL_WINDOW_AXIS_STOP:
		(void)kl_ui_axis(page.ui, event);
		break;
	case KL_WINDOW_KEY:
		/* The clipboard's keys are the page's own, a key's character the focused field's text; the rest go to the focused widget. */
		taken = page_key(window, event);
		if (!taken)
			taken = page_key_text(event);
		if (!taken)
			(void)kl_ui_key(page.ui, event->code, event->pressed, event->modifiers);
		break;
	case KL_WINDOW_TOUCH_DOWN:
		(void)kl_ui_touch_down(page.ui, event->id, event->time_us, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_MOTION:
		(void)kl_ui_touch_motion(page.ui, event->id, event->time_us, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_UP:
		(void)kl_ui_touch_up(page.ui, event->id, event->time_us, event->arrival_us);
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		(void)kl_ui_touch_cancel(page.ui, event->arrival_us);
		break;
	case KL_WINDOW_TEXT_COMMIT:
	case KL_WINDOW_TEXT_PREEDIT:
	case KL_WINDOW_TEXT_DELETE:
		(void)kl_ui_text(page.ui, event);
		break;
	default:
		/* Not the widgets' input. */
		return 0;
	}

	/* Succeeded: a frame shows what the input did. */
	page.dirty = 1;
	return 1;
}

/*
 * Draws the page into its frame at the size the drawing shows (the panes
 * read the run's state: the text's size, the theme, the ambiguous width,
 * copy-on-select), carries out what the widgets reported, and gives the
 * whole frame (*pixels, width words a row).  Returns 0, or ENOMEM.
 */
int
terminal_page_draw(
	struct terminal_window *window,
	const struct terminal_page_state *state,
	uint32_t width,
	uint32_t height,
	const uint32_t **pixels)
{
	const struct kl_theme *theme;
	struct kl_rect sidebar;
	struct kl_rect content;
	struct kl_rect whole;
	struct kl_event event;
	uint64_t now;
	int scrolled;
	int bottom;
	int left;
	int top;
	int inner;
	int taken;
	int error;

	/* A frame of the size, with its canvas. */
	error = page_frame(width, height, state->scale);
	if (error != 0)
		return error;

	/* The frame of the input, the files read the first time a pane needs them. */
	now = kl_clock_us();
	theme = kl_theme_default();
	page.style.theme = theme;
	page.style.canvas = &page.canvas;
	page.dirty = 0;
	kl_ui_begin(page.ui, now);

	/* The ground (the widgets' units, at the scale). */
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)page.width;
	whole.height = (int)page.height;
	kl_canvas_gradient(&page.canvas, &whole, theme->ground_top, theme->ground_bottom);

	/* The sidebar and the content's panel. */
	page_layout(&sidebar, &content);
	page_sidebar(&sidebar);
	kl_panel(&page.style, &content, 0);

	/* The pane, scrolled within the panel, its widgets over the scroll's region. */
	page.viewport = content;
	kl_canvas_clip_push(&page.canvas, &page.viewport);
	kl_ui_scroll_region(page.ui, PAGE_ID_SCROLL, &page.viewport, &page.scroll);
	left = content.x + PAGE_PAD;
	inner = content.width - 2 * PAGE_PAD;
	top = content.y + PAGE_PAD - (int)page.scroll.y;

	/* The pane shown. */
	switch (page.pane) {
	case TERMINAL_PANE_APPEARANCE:
		bottom = page_appearance(window, state, top, left, inner);
		break;
	case TERMINAL_PANE_BEHAVIOR:
		bottom = page_behavior(window, state, top, left, inner);
		break;
	case TERMINAL_PANE_KEYS:
		bottom = page_keys(window, top, left, inner);
		break;
	default:
		bottom = page_hosts(window, top, left, inner);
		break;
	}

	/* The content's height, which the scroll goes down to, and its bar. */
	page.content_height = bottom - top + PAGE_PAD;
	kl_scroll_set_size(&page.scroll, (double)content.width, (double)page.content_height, (double)content.width, (double)content.height);
	scrolled = kl_scroll_step(&page.scroll, now);
	(void)kl_scroll_draw_bars(&page.scroll, &page.canvas, &page.viewport, theme, now);
	kl_canvas_clip_pop(&page.canvas);

	/* The frame is drawn; another follows while the scroll or the input moves. */
	page.moving = kl_ui_end(page.ui, now);
	if (scrolled)
		page.moving = 1;

	/*
	 * The keys no widget took: Control with Up or Page Up shows the pane
	 * before, Control with Down or Page Down the pane after, the others
	 * scroll the pane.  Up and Down are there for the desktops whose
	 * titlebar takes Control with Page Up and Down for its tabs.
	 */
	for (;;) {
		taken = kl_ui_take(page.ui, &event);
		if (!taken)
			break;

		/* Only the keys move to another pane or scroll this one. */
		if (event.kind != KL_EVENT_KEY)
			continue;

		/* Control with Up or Page Up shows the pane before. */
		if ((event.modifiers & KL_MOD_CTRL) != 0U &&
		    (event.code == PAGE_KEY_UP ||
		     event.code == PAGE_KEY_PAGEUP)) {
			page_pane((page.pane + TERMINAL_PANES - 1U) % TERMINAL_PANES);
			continue;
		}

		/* Control with Down or Page Down shows the pane after. */
		if ((event.modifiers & KL_MOD_CTRL) != 0U &&
		    (event.code == PAGE_KEY_DOWN ||
		     event.code == PAGE_KEY_PAGEDOWN)) {
			page_pane((page.pane + 1U) % TERMINAL_PANES);
			continue;
		}

		/* Any other key scrolls the pane. */
		taken = kl_scroll_key(&page.scroll, event.code, event.modifiers, PAGE_SCROLL_LINE, now);
		if (taken)
			page.moving = 1;
	}

	/* The window's text input follows the focused field. */
	page_text_follow(window);

	/* Succeeded: the whole frame. */
	*pixels = page.pixels;
	return 0;
}

/*
 * Tells whether the page wants another frame without new input: a
 * scroll gliding, a press held, or what the last frame left to do.
 */
int
terminal_page_moving(void)
{
	/* A page not made draws nothing. */
	if (!page.made)
		return 0;

	/* Something moves, or an action asked for a frame. */
	if (page.moving || page.dirty)
		return 1;

	/* Nothing to draw. */
	return 0;
}

/*
 * Pastes text into the focused field (Edit > Paste, or
 * Control with V: the run receives the clipboard's text), as the input
 * method commits text.
 */
void
terminal_page_paste(
	const char *text,
	size_t length)
{
	struct kl_text_area *area;
	struct kl_field *field;
	int focused;

	/* Only into a field or an area that has the keyboard. */
	focused = page_focused(&field);
	if (!focused)
		focused = page_focused_area(&area);
	if (!focused)
		return;

	/* The text in place of the selection. */
	page_commit(text, length);
	page.dirty = 1;
	page_log("paste", "");
}

/*
 * Gives the text a Copy takes while the settings tab is active (Edit >
 * Copy, Control with C or X, Copy Public Key): what the page set aside,
 * else the focused field's selection; a secret field's is
 * never given.  Returns its length (0: nothing to copy), the text cut to
 * size - 1 bytes and ended.
 */
size_t
terminal_page_copy_text(
	char *text,
	size_t size)
{
	struct kl_text_area *area;
	struct kl_field *field;
	size_t from;
	size_t to;
	int focused;

	/* What the page set aside, taken once. */
	if (page.copied_set) {
		page.copied_set = 0;
		from = 0U;
		to = page.copied_length;
		if (to > size - 1U)
			to = size - 1U;
		memcpy(text, page.copied, to);
		text[to] = '\0';
		return to;
	}

	/* A text area's selection, when it has the keyboard. */
	focused = page_focused_area(&area);
	if (focused) {
		page_selection(area->text, area->caret, area->anchor, &from, &to);
		if (to - from > size - 1U)
			to = from + size - 1U;
		memcpy(text, area->text + from, to - from);
		text[to - from] = '\0';
		return to - from;
	}

	/* Only from a field that has the keyboard, never a secret one. */
	focused = page_focused(&field);
	if (!focused || field->secret)
		return 0U;

	/* The field's selection, cut at the buffer's size. */
	page_selection(field->text, field->caret, field->anchor, &from, &to);
	if (to - from > size - 1U)
		to = from + size - 1U;
	memcpy(text, field->text + from, to - from);

	/* The copy ends as a C string. */
	text[to - from] = '\0';

	/* Succeeded: the selection's text. */
	return to - from;
}

/*
 * Tells the page it is shown again (its tab became the active one): the
 * whole page is drawn, and the window's text input is told afresh.
 */
void
terminal_page_shown(
	struct terminal_window *window)
{
	(void)window;

	/* A frame, and the text input told on it. */
	page.dirty = 1;
	page.text_wanted = -1;
}

/*
 * Releases the page at the run's end.
 */
void
terminal_page_close(void)
{
	/* Nothing was made. */
	if (!page.made)
		return;

	/* The canvas, the frame, the text, the scroll and the input. */
	if (page.canvas_made)
		kl_canvas_release(&page.canvas);
	free(page.pixels);
	if (page.text_open)
		kl_text_close(&page.text);
	if (page.scroll_made)
		kl_scroll_release(&page.scroll);
	kl_ui_destroy(page.ui);

	/* The ssh_config's text. */
	terminal_hosts_release(&page.hosts);
	free(page.hosts_text);
	free(page.hosts_original);

	/* The secrets typed, and everything else. */
	memset(&page, 0, sizeof(page));
}

/* Makes the frame and its canvas the given size at a scale (kept while neither changes); 0 or ENOMEM. */
static int
page_frame(
	uint32_t width,
	uint32_t height,
	unsigned scale)
{
	struct kl_rect frame;
	struct kl_rect units;
	uint32_t *pixels;
	int error;

	/* A scale of 0 is 100 %. */
	if (scale == 0U)
		scale = KL_SCALE_BASE;

	/* The same size and scale keep the frame. */
	if (page.canvas_made &&
	    page.frame_width == width &&
	    page.frame_height == height &&
	    page.scale == scale)
		return 0;

	/* The old canvas goes; a new frame of the size. */
	if (page.canvas_made) {
		kl_canvas_release(&page.canvas);
		page.canvas_made = 0;
	}

	/* A window without an area has no frame. */
	if (width == 0U || height == 0U)
		return ENOMEM;

	/* The frame's pixels. */
	pixels = realloc(page.pixels, (size_t)width * height * sizeof(uint32_t));
	if (pixels == NULL)
		return ENOMEM;
	page.pixels = pixels;
	page.frame_width = width;
	page.frame_height = height;

	/* The canvas over it. */
	error = kl_canvas_init(&page.canvas, page.pixels, (size_t)width, (int)width, (int)height);
	if (error != 0)
		return ENOMEM;

	/* The canvas drawn at the scale, which the page's events are measured by too. */
	kl_canvas_set_scale(&page.canvas, scale);
	page.scale = scale;

	/* The frame's size in the widgets' units, which the panes are laid out in. */
	frame.x = 0;
	frame.y = 0;
	frame.width = (int)width;
	frame.height = (int)height;
	kl_canvas_logical(&page.canvas, &frame, &units);
	page.width = (uint32_t)units.width;
	page.height = (uint32_t)units.height;

	/* Succeeded: the frame and its canvas. */
	page.canvas_made = 1;
	return 0;
}

/* Gives a place or a distance of the window's pixels in the widgets' units (the frame's scale). */
static double
page_units(
	double pixels)
{
	double units;

	/* A page not drawn yet is at 100 %. */
	if (page.scale == 0U || page.scale == KL_SCALE_BASE)
		return pixels;

	/* The pixels over the scale. */
	units = pixels * (double)KL_SCALE_BASE / (double)page.scale;

	/* Reports the units. */
	return units;
}

/* Gives a place or a length of the widgets' units in the window's pixels, rounded to the nearest. */
static int
page_pixels(
	int units)
{
	double exact;
	int pixels;

	/* A page not drawn yet is at 100 %. */
	if (page.scale == 0U || page.scale == KL_SCALE_BASE)
		return units;

	/* The units times the scale, the half pixel up (the whole part below the value, a negative one too). */
	exact = (double)units * (double)page.scale / (double)KL_SCALE_BASE + 0.5;
	pixels = (int)exact;
	if ((double)pixels > exact)
		pixels--;

	/* Reports the pixels. */
	return pixels;
}

/* Places the sidebar on the left and the content's panel beside it, narrower in a narrow window. */
static void
page_layout(
	struct kl_rect *sidebar,
	struct kl_rect *content)
{
	int side;

	/* The sidebar's width for the window's. */
	side = PAGE_SIDEBAR;
	if ((int)page.width < PAGE_NARROW)
		side = PAGE_SIDEBAR_NARROW;

	/* The two panels, the margin round them. */
	sidebar->x = PAGE_MARGIN;
	sidebar->y = PAGE_MARGIN;
	sidebar->width = side;
	sidebar->height = (int)page.height - 2 * PAGE_MARGIN;
	content->x = PAGE_MARGIN + side + PAGE_MARGIN;
	content->y = PAGE_MARGIN;
	content->width = (int)page.width - content->x - PAGE_MARGIN;
	content->height = (int)page.height - 2 * PAGE_MARGIN;
}

/* Draws the sidebar: its section's title and the panes, the one shown lit; a click shows another. */
static void
page_sidebar(
	const struct kl_rect *sidebar)
{
	struct kl_rect item;
	unsigned index;
	int pressed;
	int top;

	/* The panel and its section. */
	kl_panel(&page.style, sidebar, 1);
	top = kl_sidebar_section(&page.style, sidebar->x + 8, sidebar->y + 8, sidebar->width - 16, "Settings");

	/* Each pane; a click shows it from its top. */
	for (index = 0U; index < TERMINAL_PANES; index++) {
		item.x = sidebar->x + 8;
		item.y = top + (int)index * PAGE_ITEM_HEIGHT;
		item.width = sidebar->width - 16;
		item.height = PAGE_ITEM_HEIGHT;
		pressed = kl_sidebar_item(page.ui, &page.style, PAGE_ID_PANES, index, &item, page_pane_icons[index], page_pane_names[index], index == page.pane);
		if (pressed)
			page_pane(index);
	}
}

/*
 * Draws the Hosts pane: the hosts of the ssh_config and Open in text
 * editor, the form of the host chosen (its fields, its identity files),
 * its forwards, its other lines, then Save, Revert and Remove host with
 * the last word; reports its bottom.
 */
static int
page_hosts(
	struct terminal_window *window,
	int top,
	int left,
	int width)
{
	const struct kl_theme *theme;
	unsigned labels;
	int y;

	/* The pane's label column (with the X11 lines' for a program that forwards X11), and the file the first time. */
	theme = page.style.theme;
	labels = PAGE_HOST_LABELS;
	if (page.program->x11)
		labels = sizeof(page_host_labels) / sizeof(page_host_labels[0]);
	page_column(TERMINAL_PANE_HOSTS, page_host_labels, labels);
	if (!page.hosts_read)
		page_hosts_read();

	/* The hosts, a new one, and the text editor. */
	y = page_hosts_list(top, left, width);

	/* A file that cannot be edited here shows the list's card alone, with Revert and the last word. */
	if (page.hosts_unusable) {
		y = page_hosts_actions(y + PAGE_GAP, left, width);
		page_hosts_carry_out(window);
		return y;
	}

	/* The host chosen: its form and identity files, its forwards, its X11 lines and its other lines. */
	if (page.hosts_block != TERMINAL_HOSTS_NONE) {
		y = page_hosts_form(y + PAGE_GAP, left, width);
		if (page.hosts.blocks[page.hosts_block].kind != TERMINAL_HOSTS_BLOCK_MATCH)
			y = page_hosts_forwards(y + PAGE_GAP, left, width);
		if (page.hosts.blocks[page.hosts_block].kind != TERMINAL_HOSTS_BLOCK_MATCH && page.program->x11)
			y = page_hosts_x11(y + PAGE_GAP, left, width);
		y = page_hosts_other(y + PAGE_GAP, left, width);
	} else {
		y += PAGE_GAP;
		y = page_note(y, left + 4, width - 8, "Add a host, or open the file in a text editor.", theme->text_secondary);
	}

	/* Save, Revert, Remove host and the last word. */
	y = page_hosts_actions(y + PAGE_GAP, left, width);

	/* What a widget asked for, now that the frame is drawn from the text as it was. */
	page_hosts_carry_out(window);

	/* Reports the pane's bottom. */
	return y;
}

/*
 * Draws the card of the hosts: a row for each (a click shows its form), a
 * new host's name with Add, and Open in text editor; reports its bottom.
 */
static int
page_hosts_list(
	int top,
	int left,
	int width)
{
	const struct kl_theme *theme;
	struct kl_rect card;
	struct kl_rect row;
	struct kl_rect control;
	char subtitle[PAGE_MESSAGE];
	char title[PAGE_HOST_TEXT];
	const char *editor_note;
	unsigned index;
	unsigned listed;
	unsigned flags;
	int shown;
	int pressed;
	int y;

	/* How many rows: the blocks listed, or a word that there is none. */
	theme = page.style.theme;
	listed = 0U;
	for (index = 0U; index < page.hosts.block_count; index++) {
		shown = page_hosts_listed(index);
		if (shown)
			listed++;
	}

	/* The card: the rows, the new host's row, the editor's button and its note. */
	editor_note = "The text editor's changes take effect from the next connection; Revert shows them here.";
	card.x = left;
	card.y = top;
	card.width = width;
	card.height = PAGE_CARD_SUBTITLED + (int)listed * PAGE_ITEM_HEIGHT + 8 + PAGE_ROW + 8 + theme->control_height + 6 +
	    page_note_height(width - 40, editor_note) + PAGE_CARD_FOOT;
	if (listed == 0U)
		card.height += PAGE_NOTE_LINE;
	if (page.hosts_unusable)
		card.height -= PAGE_ROW + 8;
	(void)snprintf(subtitle, sizeof(subtitle), "%s", page.program->config_shown);
	y = page_card(&card, "Hosts", subtitle);

	/* None yet. */
	if (listed == 0U)
		y = page_note(y, card.x + 20, card.width - 40, "There is no host in the file yet.", theme->text_secondary);

	/* Each block listed; a click shows its form (the changes of the one shown kept in the text). */
	for (index = 0U; index < page.hosts.block_count; index++) {
		shown = page_hosts_listed(index);
		if (!shown)
			continue;
		page_hosts_title(index, title, sizeof(title));
		row.x = card.x + 12;
		row.y = y;
		row.width = card.width - 24;
		row.height = PAGE_ITEM_HEIGHT;
		flags = 0U;
		if (page.hosts.blocks[index].kind == TERMINAL_HOSTS_BLOCK_MATCH)
			flags = KL_PLACE_QUIET;
		pressed = kl_sidebar_place(page.ui, &page.style, PAGE_ID_HOST_ROWS, index, &row, KL_ICON_COMPUTER, title, index == page.hosts_block, flags);
		if (pressed)
			page_hosts_later(PAGE_HOSTS_SELECT, index);
		y += PAGE_ITEM_HEIGHT;
	}

	/* A new host's name and Add, when the file can be edited here. */
	y += 8;
	if (!page.hosts_unusable) {
		pressed = page_field_button(y, card.x, card.width, "New host", PAGE_ID_HOST_NEW, &page.host_new, "myserver", PAGE_ID_HOST_ADD, "Add Host");
		if (pressed)
			page_hosts_later(PAGE_HOSTS_ADD_HOST, 0U);
		y += PAGE_ROW + 8;
	}

	/* The file in the system's text editor, and when its changes take effect. */
	control.x = card.x + 20;
	control.y = y;
	control.height = theme->control_height;
	control.width = kl_button_width(&page.style, "Open in Text Editor");
	pressed = kl_button(page.ui, &page.style, PAGE_ID_HOST_EDITOR, &control, "Open in Text Editor", 0U);
	if (pressed)
		page_hosts_later(PAGE_HOSTS_EDITOR, 0U);
	y += control.height + 6;
	(void)page_note(y, card.x + 20, card.width - 40, editor_note, theme->text_secondary);

	/* Reports the card's bottom. */
	return card.y + card.height;
}

/*
 * Draws the card of the host shown: its fields (each with the line of
 * another block that wins over it), then its identity files; a Match's
 * card only says it is edited in the text editor.  Reports its bottom.
 */
static int
page_hosts_form(
	int top,
	int left,
	int width)
{
	const struct kl_theme *theme;
	const struct terminal_hosts_block *block;
	struct kl_rect card;
	char title[PAGE_HOST_TEXT];
	char where[PAGE_HOST_TEXT];
	char note[PAGE_MESSAGE];
	unsigned overriding[PAGE_HOST_FIELDS];
	unsigned index;
	unsigned first;
	int named;
	int y;

	/* The block and its title. */
	theme = page.style.theme;
	block = &page.hosts.blocks[page.hosts_block];
	page_hosts_title(page.hosts_block, title, sizeof(title));
	card.x = left;
	card.y = top;
	card.width = width;

	/* A Match: its lines are under Other lines, and the text editor changes them. */
	if (block->kind == TERMINAL_HOSTS_BLOCK_MATCH) {
		card.height = PAGE_CARD_TITLE + page_note_height(width - 40, "A Match block is changed in the text editor.") + PAGE_CARD_FOOT;
		y = page_card(&card, title, NULL);
		(void)page_note(y, card.x + 20, card.width - 40, "A Match block is changed in the text editor.", theme->text_secondary);
		return card.y + card.height;
	}

	/* The fields shown: a Host's name only in a Host's block; the line that wins over each. */
	first = PAGE_HOST_NAME;
	if (block->kind != TERMINAL_HOSTS_BLOCK_HOST)
		first = PAGE_HOST_NAME + 1U;
	card.height = PAGE_CARD_SUBTITLED + (int)(PAGE_HOST_X11 - first) * PAGE_ROW + PAGE_CARD_FOOT;
	for (index = first; index < PAGE_HOST_X11; index++) {
		overriding[index] = TERMINAL_HOSTS_NONE;
		if (index != PAGE_HOST_NAME)
			overriding[index] = terminal_hosts_overrides(&page.hosts, page.hosts_block, page_host_keywords[index]);
		if (overriding[index] != TERMINAL_HOSTS_NONE)
			card.height += PAGE_NOTE_LINE;
	}

	/* A Host's name with a wildcard only: the blocks before are not compared. */
	named = 1;
	if (block->kind == TERMINAL_HOSTS_BLOCK_HOST)
		named = terminal_hosts_has_name(&page.hosts, page.hosts_block);
	if (!named)
		card.height += PAGE_NOTE_LINE;

	/* The card, the line its block starts at under the title. */
	note[0] = '\0';
	if (block->kind == TERMINAL_HOSTS_BLOCK_HOST)
		(void)snprintf(note, sizeof(note), "From line %u", page.hosts.lines[block->first].number);
	else
		(void)snprintf(note, sizeof(note), "These lines apply to every host");
	y = page_card(&card, title, note);

	/* Each field, and the line of another block that wins over it. */
	for (index = first; index < PAGE_HOST_X11; index++) {
		y = page_field_row(y, card.x, card.width, page_host_field_labels[index], PAGE_ID_HOST_FIELD + index, &page.host_fields[index], page_host_placeholders[index]);
		if (overriding[index] == TERMINAL_HOSTS_NONE)
			continue;
		page_hosts_where(overriding[index], where, sizeof(where));
		(void)snprintf(note, sizeof(note), "Overridden by %s", where);
		y = page_note(y, page_value_x(card.x, card.width), card.width - 40, note, theme->bad);
	}

	/* A wildcard's block: other blocks may give these too. */
	if (!named)
		(void)page_note(y, card.x + 20, card.width - 40, "Other blocks may set these too (the name has a wildcard).", theme->text_secondary);

	/* The identity files under the card. */
	y = page_hosts_identities(card.y + card.height + PAGE_GAP, left, width);

	/* Reports the bottom. */
	return y;
}

/*
 * Draws the card of the host's identity files: its own lines (each with a
 * removal), those other blocks add (read only), and a new one with Add;
 * reports its bottom.
 */
static int
page_hosts_identities(
	int top,
	int left,
	int width)
{
	const struct kl_theme *theme;
	struct kl_rect card;
	char text[PAGE_HOST_TEXT];
	char where[PAGE_HOST_TEXT];
	char word[PAGE_HOST_TEXT];
	unsigned line;
	unsigned rows;
	int pressed;
	int y;

	/* The rows: the block's lines and those others add. */
	theme = page.style.theme;
	rows = 0U;
	line = terminal_hosts_find(&page.hosts, page.hosts_block, "IdentityFile", TERMINAL_HOSTS_NONE);
	while (line != TERMINAL_HOSTS_NONE) {
		rows++;
		line = terminal_hosts_find(&page.hosts, page.hosts_block, "IdentityFile", line);
	}

	/* And those other blocks add. */
	line = terminal_hosts_other(&page.hosts, page.hosts_block, "IdentityFile", TERMINAL_HOSTS_NONE);
	while (line != TERMINAL_HOSTS_NONE) {
		rows++;
		line = terminal_hosts_other(&page.hosts, page.hosts_block, "IdentityFile", line);
	}

	/* The card: the rows and the new one's row. */
	card.x = left;
	card.y = top;
	card.width = width;
	card.height = PAGE_CARD_SUBTITLED + (int)rows * PAGE_ROW + PAGE_ROW + PAGE_CARD_FOOT;
	y = page_card(&card, "Identity files", "The keys tried, in order (IdentityFile)");

	/* The block's own, each with its removal. */
	line = terminal_hosts_find(&page.hosts, page.hosts_block, "IdentityFile", TERMINAL_HOSTS_NONE);
	while (line != TERMINAL_HOSTS_NONE) {
		(void)terminal_hosts_word(&page.hosts, line, 0U, word, sizeof(word));
		y = page_line_row(y, card.x, card.width, "", word, theme->text, line, 1);
		line = terminal_hosts_find(&page.hosts, page.hosts_block, "IdentityFile", line);
	}

	/* Those other blocks add, read only. */
	line = terminal_hosts_other(&page.hosts, page.hosts_block, "IdentityFile", TERMINAL_HOSTS_NONE);
	while (line != TERMINAL_HOSTS_NONE) {
		(void)terminal_hosts_word(&page.hosts, line, 0U, word, sizeof(word));
		page_hosts_where(line, where, sizeof(where));
		(void)snprintf(text, sizeof(text), "%s (%s)", word, where);
		y = page_line_row(y, card.x, card.width, "", text, theme->text_secondary, line, 0);
		line = terminal_hosts_other(&page.hosts, page.hosts_block, "IdentityFile", line);
	}

	/* A new one. */
	pressed = page_field_button(y, card.x, card.width, "Identity file", PAGE_ID_IDENTITY_NEW, &page.identity_new, "~/.ssh/id_ed25519", PAGE_ID_IDENTITY_ADD, "Add");
	if (pressed)
		page_hosts_later(PAGE_HOSTS_ADD_IDENTITY, 0U);

	/* Reports the card's bottom. */
	return card.y + card.height;
}

/*
 * Draws the card of the host's forwards: its own (each with a removal),
 * those other blocks add (read only), and a new one: Local or Remote,
 * where it listens, where it goes, and Add.  Reports its bottom.
 */
static int
page_hosts_forwards(
	int top,
	int left,
	int width)
{
	static const char *const keywords[2] = { "LocalForward", "RemoteForward" };
	const struct kl_theme *theme;
	struct kl_rect card;
	struct kl_rect control;
	char text[PAGE_HOST_TEXT];
	char where[PAGE_HOST_TEXT];
	char shown[PAGE_HOST_TEXT];
	const char *explained;
	const char *listen_hint;
	const char *to_hint;
	unsigned local_flags;
	unsigned remote_flags;
	unsigned kind;
	unsigned line;
	unsigned rows;
	int value_x;
	int pressed;
	int y;

	/* The rows of both kinds: the block's and those others add. */
	theme = page.style.theme;
	rows = 0U;
	for (kind = 0U; kind < 2U; kind++) {
		line = terminal_hosts_find(&page.hosts, page.hosts_block, keywords[kind], TERMINAL_HOSTS_NONE);
		while (line != TERMINAL_HOSTS_NONE) {
			rows++;
			line = terminal_hosts_find(&page.hosts, page.hosts_block, keywords[kind], line);
		}

		/* Those other blocks add. */
		line = terminal_hosts_other(&page.hosts, page.hosts_block, keywords[kind], TERMINAL_HOSTS_NONE);
		while (line != TERMINAL_HOSTS_NONE) {
			rows++;
			line = terminal_hosts_other(&page.hosts, page.hosts_block, keywords[kind], line);
		}
	}

	/* What the kind chosen does. */
	explained = "Local: a port on this computer reaches the destination from the server.";
	if (page.forward_remote)
		explained = "Remote: a port on the server reaches the destination from this computer (port 0: the server chooses).";

	/* The card: the rows, the kind, the two ends and Add, and the note. */
	card.x = left;
	card.y = top;
	card.width = width;
	card.height = PAGE_CARD_SUBTITLED + (int)rows * PAGE_ROW + 3 * PAGE_ROW + page_note_height(width - 40, explained) + PAGE_CARD_FOOT;
	y = page_card(&card, "Port forwarding", "LocalForward and RemoteForward");

	/* Each forward, the block's with a removal, the others' read only. */
	for (kind = 0U; kind < 2U; kind++) {
		line = terminal_hosts_find(&page.hosts, page.hosts_block, keywords[kind], TERMINAL_HOSTS_NONE);
		while (line != TERMINAL_HOSTS_NONE) {
			page_hosts_forward_text(line, shown, sizeof(shown));
			y = page_line_row(y, card.x, card.width, "", shown, theme->text, line, 1);
			line = terminal_hosts_find(&page.hosts, page.hosts_block, keywords[kind], line);
		}

		/* Those other blocks add. */
		line = terminal_hosts_other(&page.hosts, page.hosts_block, keywords[kind], TERMINAL_HOSTS_NONE);
		while (line != TERMINAL_HOSTS_NONE) {
			page_hosts_forward_text(line, shown, sizeof(shown));
			page_hosts_where(line, where, sizeof(where));
			(void)snprintf(text, sizeof(text), "%s (%s)", shown, where);
			y = page_line_row(y, card.x, card.width, "", text, theme->text_secondary, line, 0);
			line = terminal_hosts_other(&page.hosts, page.hosts_block, keywords[kind], line);
		}
	}

	/* The kind of a new one: Local or Remote, the one chosen marked. */
	value_x = page_label(y, card.x, card.width, "Forward", 0);
	control.x = value_x;
	control.y = y + (PAGE_ROW - theme->control_height) / 2;
	control.height = theme->control_height;
	control.width = kl_button_width(&page.style, "Local");
	local_flags = KL_BUTTON_PRIMARY;
	remote_flags = 0U;
	listen_hint = "8080, or 127.0.0.1:8080";
	to_hint = "db.internal:5432";
	if (page.forward_remote) {
		local_flags = 0U;
		remote_flags = KL_BUTTON_PRIMARY;
		listen_hint = "8080, or 0";
		to_hint = "localhost:3000";
	}

	/* Local. */
	pressed = kl_button(page.ui, &page.style, PAGE_ID_FORWARD_LOCAL, &control, "Local", local_flags);
	if (pressed) {
		page.forward_remote = 0;
		page.dirty = 1;
	}

	/* Remote beside it. */
	control.x += control.width + 6;
	control.width = kl_button_width(&page.style, "Remote");
	pressed = kl_button(page.ui, &page.style, PAGE_ID_FORWARD_REMOTE, &control, "Remote", remote_flags);
	if (pressed) {
		page.forward_remote = 1;
		page.dirty = 1;
	}

	/* The ends' rows under the kind. */
	y += PAGE_ROW;

	/* Where it listens, and where it goes with Add. */
	y = page_field_row(y, card.x, card.width, "Listen", PAGE_ID_FORWARD_LISTEN, &page.forward_listen, listen_hint);
	pressed = page_field_button(y, card.x, card.width, "Destination", PAGE_ID_FORWARD_TO, &page.forward_to, to_hint, PAGE_ID_FORWARD_ADD, "Add");
	if (pressed)
		page_hosts_later(PAGE_HOSTS_ADD_FORWARD, 0U);
	y += PAGE_ROW;

	/* What the kind does. */
	(void)page_note(y, card.x + 20, card.width - 40, explained, theme->text_secondary);

	/* Reports the card's bottom. */
	return card.y + card.height;
}

/*
 * Draws the card of the host's X11 lines (a program that forwards X11):
 * a field each, with the line of another block that wins over it, and a
 * note of what the root window and the tabs do; reports its bottom.
 */
static int
page_hosts_x11(
	int top,
	int left,
	int width)
{
	const struct kl_theme *theme;
	struct kl_rect card;
	char where[PAGE_HOST_TEXT];
	char note[PAGE_MESSAGE];
	const char *explained;
	unsigned overriding[PAGE_HOST_FIELDS];
	unsigned index;
	int y;

	/* The rows, and the line that wins over each. */
	theme = page.style.theme;
	explained = "Root window shows the remote X screen as one window of that size; Window tabs shows each remote window in a tab. "
	    "Off and no, each remote window is a window of its own.";
	card.x = left;
	card.y = top;
	card.width = width;
	card.height = PAGE_CARD_SUBTITLED + (int)(PAGE_HOST_FIELDS - PAGE_HOST_X11) * PAGE_ROW + page_note_height(width - 40, explained) + PAGE_CARD_FOOT;
	for (index = PAGE_HOST_X11; index < PAGE_HOST_FIELDS; index++) {
		overriding[index] = terminal_hosts_overrides(&page.hosts, page.hosts_block, page_host_keywords[index]);
		if (overriding[index] != TERMINAL_HOSTS_NONE)
			card.height += PAGE_NOTE_LINE;
	}
	y = page_card(&card, "X11", "The remote programs' windows on this computer");

	/* Each field, and the line of another block that wins over it. */
	for (index = PAGE_HOST_X11; index < PAGE_HOST_FIELDS; index++) {
		y = page_field_row(y, card.x, card.width, page_host_field_labels[index], PAGE_ID_HOST_FIELD + index, &page.host_fields[index], page_host_placeholders[index]);
		if (overriding[index] == TERMINAL_HOSTS_NONE)
			continue;
		page_hosts_where(overriding[index], where, sizeof(where));
		(void)snprintf(note, sizeof(note), "Overridden by %s", where);
		y = page_note(y, page_value_x(card.x, card.width), card.width - 40, note, theme->bad);
	}

	/* What the root window and the tabs do. */
	(void)page_note(y, card.x + 20, card.width - 40, explained, theme->text_secondary);

	/* Reports the card's bottom. */
	return card.y + card.height;
}

/* Reports how many fields a host's form has: the X11 lines' too for a program that forwards X11. */
static unsigned
page_hosts_fields(void)
{
	/* A program that forwards X11. */
	if (page.program->x11)
		return PAGE_HOST_FIELDS;

	/* One that does not. */
	return PAGE_HOST_X11;
}

/*
 * Draws the card of the block's other lines, which the form does not show
 * (comments, the keywords it does not know, Include, a second line of a
 * keyword that ssh passes over), read only; none: no card.  Reports its
 * bottom (top when there is no card).
 */
static int
page_hosts_other(
	int top,
	int left,
	int width)
{
	const struct terminal_hosts_block *block;
	const struct terminal_hosts_line *line;
	const struct kl_theme *theme;
	struct kl_rect card;
	char text[PAGE_HOST_TEXT];
	char shown[PAGE_HOST_TEXT];
	unsigned index;
	unsigned count;
	unsigned rows;
	size_t length;
	int other;
	int second;
	int baseline;
	int y;

	/* The lines not in the form. */
	theme = page.style.theme;
	block = &page.hosts.blocks[page.hosts_block];
	count = 0U;
	for (index = block->first; index < block->end; index++) {
		other = page_hosts_other_line(index);
		if (other)
			count++;
	}

	/* No card without them. */
	if (count == 0U)
		return top - PAGE_GAP;

	/* The card: a line a row, at most so many, and the note. */
	rows = count;
	if (rows > PAGE_OTHER_LINES)
		rows = PAGE_OTHER_LINES + 1U;
	card.x = left;
	card.y = top;
	card.width = width;
	card.height = PAGE_CARD_SUBTITLED + (int)rows * PAGE_NOTE_LINE + 6 + page_note_height(width - 40, "Open the file in a text editor to change these.") + PAGE_CARD_FOOT;
	y = page_card(&card, "Other lines", "As the file has them");

	/* Each line, its number first; a keyword's second line says ssh passes over it. */
	rows = 0U;
	for (index = block->first; index < block->end && rows < PAGE_OTHER_LINES; index++) {
		other = page_hosts_other_line(index);
		if (!other)
			continue;

		/* The line's bytes, cut to the row. */
		line = &page.hosts.lines[index];
		length = line->end - line->start;
		if (length > sizeof(shown) - 1U)
			length = sizeof(shown) - 1U;
		memcpy(shown, page.hosts_text + line->start, length);
		shown[length] = '\0';

		/* Its number, and why ssh passes over a second line of a field's keyword. */
		second = page_hosts_second(index);
		if (second) {
			(void)snprintf(text, sizeof(text), "%u: %s  (ignored: an earlier line sets it)", line->number, shown);
		} else {
			(void)snprintf(text, sizeof(text), "%u: %s", line->number, shown);
		}

		/* The row. */
		baseline = kl_text_center(theme->text_small, y, PAGE_NOTE_LINE);
		(void)kl_text_draw_fit(&page.text, &page.canvas, card.x + 20, baseline, text, theme->text_small, 0, card.width - 40, theme->text_secondary);
		y += PAGE_NOTE_LINE;
		rows++;
	}

	/* How many more there are. */
	if (count > PAGE_OTHER_LINES) {
		(void)snprintf(text, sizeof(text), "... and %u more", count - PAGE_OTHER_LINES);
		y = page_note(y, card.x + 20, card.width - 40, text, theme->text_secondary);
	}

	/* Where they are changed. */
	(void)page_note(y + 6, card.x + 20, card.width - 40, "Open the file in a text editor to change these.", theme->text_secondary);

	/* Reports the card's bottom. */
	return card.y + card.height;
}

/*
 * Draws Save, Revert and (for a Host) Remove Host, the question of a
 * removal, whether there are changes not saved, the last word and when a
 * save takes effect; reports the bottom.
 */
static int
page_hosts_actions(
	int top,
	int left,
	int width)
{
	const struct kl_theme *theme;
	struct kl_rect control;
	char title[PAGE_HOST_TEXT];
	char text[PAGE_MESSAGE];
	unsigned flags;
	int removable;
	int baseline;
	int pressed;
	int same;
	int y;

	/* Save, unless the file cannot be edited here. */
	theme = page.style.theme;
	control.x = left + 4;
	control.y = top;
	control.height = theme->control_height;
	control.width = kl_button_width(&page.style, "Save");
	flags = KL_BUTTON_PRIMARY;
	if (page.hosts_unusable)
		flags |= KL_BUTTON_DISABLED;
	pressed = kl_button(page.ui, &page.style, PAGE_ID_HOSTS_SAVE, &control, "Save", flags);
	if (pressed && !page.hosts_unusable)
		page_hosts_later(PAGE_HOSTS_SAVE, 0U);

	/* Revert, beside it, reads the file again. */
	control.x += control.width + 8;
	control.width = kl_button_width(&page.style, "Revert");
	pressed = kl_button(page.ui, &page.style, PAGE_ID_HOSTS_REVERT, &control, "Revert", 0U);
	if (pressed)
		page_hosts_later(PAGE_HOSTS_REVERT, 0U);

	/* Remove Host, for a Host's block shown. */
	removable = 0;
	if (!page.hosts_unusable &&
	    page.hosts_block != TERMINAL_HOSTS_NONE &&
	    page.hosts.blocks[page.hosts_block].kind == TERMINAL_HOSTS_BLOCK_HOST)
		removable = 1;
	if (removable) {
		control.x += control.width + 8;
		control.width = kl_button_width(&page.style, "Remove Host");
		pressed = kl_button(page.ui, &page.style, PAGE_ID_HOST_REMOVE, &control, "Remove Host", KL_BUTTON_DANGER);
		if (pressed)
			page_hosts_later(PAGE_HOSTS_REMOVE_ASK, 0U);
	}

	/* Under the buttons. */
	y = top + control.height + 8;

	/* The removal asked: Remove or Keep. */
	if (removable && page.host_remove_asked) {
		page_hosts_title(page.hosts_block, title, sizeof(title));
		(void)snprintf(text, sizeof(text), "Remove Host %s and its %u lines?", title, page_hosts_lines(page.hosts_block));
		control.x = left + 4;
		control.y = y;
		baseline = kl_text_center(theme->text_body, control.y, control.height);
		(void)kl_text_draw_fit(&page.text, &page.canvas, control.x, baseline, text, theme->text_body, 0, PAGE_KNOWN_LINE, theme->text);
		control.x += kl_text_width(&page.text, text, strlen(text), theme->text_body, 0) + 12;
		if (control.x > left + 4 + PAGE_KNOWN_LINE + 12)
			control.x = left + 4 + PAGE_KNOWN_LINE + 12;
		control.width = kl_button_width(&page.style, "Remove");
		pressed = kl_button(page.ui, &page.style, PAGE_ID_HOST_REMOVE_YES, &control, "Remove", KL_BUTTON_DANGER);
		if (pressed)
			page_hosts_later(PAGE_HOSTS_REMOVE, 0U);
		control.x += control.width + 8;
		control.width = kl_button_width(&page.style, "Keep");
		pressed = kl_button(page.ui, &page.style, PAGE_ID_HOST_REMOVE_NO, &control, "Keep", 0U);
		if (pressed)
			page_hosts_later(PAGE_HOSTS_KEEP, 0U);
		y += control.height + 8;
	}

	/* Changes the file does not have yet. */
	same = page_hosts_saved();
	if (!same)
		y = page_note(y, left + 4, width - 8, "There are changes not saved yet.", theme->text);

	/* The last word, and when a save takes effect. */
	y = page_message_draw(y, left + 4, width - 8, &page.hosts_message);
	y = page_note(y, left + 4, width - 8, "A save takes effect from the next connection.", theme->text_secondary);

	/* Reports the bottom. */
	return y;
}

/* Keeps what a widget of the Hosts pane asked for (the first of a frame), carried out once the frame is drawn. */
static void
page_hosts_later(
	unsigned action,
	unsigned what)
{
	/* One a frame: a click is one. */
	if (page.hosts_later != PAGE_HOSTS_NOTHING)
		return;
	page.hosts_later = action;
	page.hosts_later_what = what;
	page.dirty = 1;
}

/* Carries out what a widget of the Hosts pane asked for in the frame just drawn. */
static void
page_hosts_carry_out(
	struct terminal_window *window)
{
	unsigned action;
	unsigned what;

	/* The action, taken once. */
	action = page.hosts_later;
	what = page.hosts_later_what;
	page.hosts_later = PAGE_HOSTS_NOTHING;

	/* Which. */
	switch (action) {
	case PAGE_HOSTS_SELECT:
		page_hosts_select(what);
		break;
	case PAGE_HOSTS_ADD_HOST:
		page_hosts_add_host();
		break;
	case PAGE_HOSTS_REMOVE_ASK:
		page.host_remove_asked = 1;
		break;
	case PAGE_HOSTS_REMOVE:
		page_hosts_remove_host();
		break;
	case PAGE_HOSTS_KEEP:
		page.host_remove_asked = 0;
		break;
	case PAGE_HOSTS_ADD_IDENTITY:
		page_hosts_add_identity();
		break;
	case PAGE_HOSTS_ADD_FORWARD:
		page_hosts_add_forward();
		break;
	case PAGE_HOSTS_REMOVE_LINE:
		page_hosts_remove_line(what);
		break;
	case PAGE_HOSTS_SAVE:
		page_hosts_save();
		break;
	case PAGE_HOSTS_REVERT:
		page_hosts_read();
		page_say(&page.hosts_message, 0, "The file was read again.");
		break;
	case PAGE_HOSTS_EDITOR:
		page_hosts_editor(window);
		break;
	default:
		/* Nothing asked. */
		return;
	}

	/* A frame shows what it did. */
	page.dirty = 1;
}

/*
 * Reads the ssh_config into the pane, every change not saved dropped: the
 * bytes as they are (none when the file is not there), read into lines and
 * blocks, the first Host shown.  A file too large or that cannot be read
 * is not edited here (its word says why).
 */
static void
page_hosts_read(void)
{
	char text[PAGE_MESSAGE];
	char *bytes;
	char *copy;
	size_t size;
	int error;

	/* Nothing kept from before. */
	free(page.hosts_original);
	page.hosts_original = NULL;
	page.hosts_original_size = 0U;
	page.hosts_read = 1;
	page.hosts_unusable = 0;
	page.host_remove_asked = 0;
	page.hosts_block = TERMINAL_HOSTS_NONE;
	page.hosts_filled = TERMINAL_HOSTS_NONE;

	/* The file's bytes; one not there is empty. */
	bytes = NULL;
	size = 0U;
	error = page.program->read(page.program->data, page.program->config_path, TERMINAL_HOSTS_TEXT_MAX, &bytes, &size);
	if (error == ENOENT) {
		bytes = NULL;
		size = 0U;
		error = 0;
	}

	/* A file too large, or that cannot be read, is left to the text editor. */
	if (error != 0) {
		page.hosts_unusable = 1;
		if (error == EFBIG) {
			(void)snprintf(text, sizeof(text), "%s is larger than %u KiB: change it in a text editor.", page.program->config_shown, TERMINAL_HOSTS_TEXT_MAX / 1024U);
		} else {
			(void)snprintf(text, sizeof(text), "%s could not be read (%s).", page.program->config_shown, strerror(error));
		}

		/* Told, and nothing to edit. */
		page_say(&page.hosts_message, 1, text);
		size = 0U;
	}

	/* The bytes as read, which a save compares the file with. */
	page.hosts_original = malloc(size + 1U);
	if (page.hosts_original == NULL) {
		free(bytes);
		page.hosts_unusable = 1;
		page_say(&page.hosts_message, 1, "There is no memory to read the file.");
		return;
	}

	/* The copy the pane changes. */
	copy = malloc(size + 1U);
	if (copy == NULL) {
		free(page.hosts_original);
		free(bytes);
		page.hosts_original = NULL;
		page.hosts_unusable = 1;
		page_say(&page.hosts_message, 1, "There is no memory to read the file.");
		return;
	}

	/* Both hold the bytes. */
	if (size > 0U) {
		memcpy(page.hosts_original, bytes, size);
		memcpy(copy, bytes, size);
	}

	/* Both end as C strings; the bytes read go. */
	page.hosts_original[size] = '\0';
	copy[size] = '\0';
	page.hosts_original_size = size;
	free(bytes);

	/* The text the pane changes. */
	error = page_hosts_take(copy, size);
	if (error != 0) {
		page.hosts_unusable = 1;
		page_say(&page.hosts_message, 1, "There is no memory to read the file.");
		return;
	}

	/* The first Host shown, else the first block listed. */
	page_hosts_select(page_hosts_first());
	page_log("hosts", "read");
}

/*
 * Makes a text (malloc'd, a NUL after it; the pane's from now on) the
 * pane's text, read into its lines and blocks; the old one goes.  The
 * block shown stays (the caller moves it when the blocks changed).
 * Returns 0, or ENOMEM (the text is freed, the old one stays).
 */
static int
page_hosts_take(
	char *text,
	size_t size)
{
	struct terminal_hosts read;
	int error;

	/* Its lines and blocks. */
	error = terminal_hosts_read(&read, text, size);
	if (error != 0) {
		free(text);
		return error;
	}

	/* In place of the old text. */
	terminal_hosts_release(&page.hosts);
	free(page.hosts_text);
	page.hosts = read;
	page.hosts_text = text;
	page.hosts_size = size;
	page.host_remove_asked = 0;

	/* A block shown that is gone shows none. */
	if (page.hosts_block != TERMINAL_HOSTS_NONE && page.hosts_block >= page.hosts.block_count) {
		page.hosts_block = TERMINAL_HOSTS_NONE;
		page.hosts_filled = TERMINAL_HOSTS_NONE;
	}

	/* Succeeded: the text is the pane's. */
	page.dirty = 1;
	return 0;
}

/*
 * Shows a block's form: the changes of the form shown go into the text
 * first (a value that cannot be written keeps it shown, with why).
 */
static void
page_hosts_select(
	unsigned block)
{
	char title[PAGE_HOST_TEXT];
	int error;

	/* The form shown, kept. */
	error = page_hosts_apply();
	if (error != 0)
		return;

	/* The block, its fields filled from its lines. */
	page.hosts_block = block;
	page.host_remove_asked = 0;
	page_hosts_fill();
	kl_ui_clear_focus(page.ui);

	/* Logs which. */
	title[0] = '\0';
	if (block != TERMINAL_HOSTS_NONE)
		page_hosts_title(block, title, sizeof(title));
	page_log("hosts-shown", title);
}

/* Fills the form's fields from the block shown: the Host line's names, each keyword's first line (a word without its quotes; RemoteCommand as written). */
static void
page_hosts_fill(void)
{
	const struct terminal_hosts_block *block;
	char value[KL_FIELD_MAX];
	unsigned fields;
	unsigned index;
	unsigned line;

	/* Each field the form has. */
	fields = page_hosts_fields();
	for (index = 0U; index < PAGE_HOST_FIELDS; index++) {
		value[0] = '\0';
		if (index >= fields) {
			kl_field_set(&page.host_fields[index], value);
			page.host_filled[index][0] = '\0';
			continue;
		}
		if (page.hosts_block != TERMINAL_HOSTS_NONE) {
			block = &page.hosts.blocks[page.hosts_block];
			line = TERMINAL_HOSTS_NONE;
			if (index == PAGE_HOST_NAME && block->kind == TERMINAL_HOSTS_BLOCK_HOST)
				line = block->first;
			if (index != PAGE_HOST_NAME && block->kind != TERMINAL_HOSTS_BLOCK_MATCH)
				line = terminal_hosts_find(&page.hosts, page.hosts_block, page_host_keywords[index], TERMINAL_HOSTS_NONE);

			/* The Host's names and the command as written; a value of a word without its quotes. */
			if (line != TERMINAL_HOSTS_NONE) {
				if (index == PAGE_HOST_NAME || index == PAGE_HOST_COMMAND)
					(void)terminal_hosts_value(&page.hosts, line, value, sizeof(value));
				else
					(void)terminal_hosts_word(&page.hosts, line, 0U, value, sizeof(value));
			}
		}

		/* The field, and what it was filled with. */
		kl_field_set(&page.host_fields[index], value);
		(void)snprintf(page.host_filled[index], sizeof(page.host_filled[index]), "%s", page.host_fields[index].text);
	}

	/* The fields are the block's. */
	page.hosts_filled = page.hosts_block;
}

/*
 * Writes the form's changed fields into the text, each the least change
 * of its line (an empty one takes the keyword's lines out).  Returns 0, or
 * EINVAL (with why) for a value the file cannot hold, or ENOMEM.
 */
static int
page_hosts_apply(void)
{
	char written[TERMINAL_HOSTS_VALUE_MAX];
	char text[PAGE_MESSAGE];
	const char *field;
	const char *why;
	unsigned long port;
	unsigned changed;
	unsigned fields;
	unsigned index;
	char *out;
	char *end;
	size_t size;
	int same;
	int error;

	/* Only a form filled from the block shown (not a Match's). */
	if (page.hosts_block == TERMINAL_HOSTS_NONE || page.hosts_filled != page.hosts_block)
		return 0;
	if (page.hosts.blocks[page.hosts_block].kind == TERMINAL_HOSTS_BLOCK_MATCH)
		return 0;

	/* Each field of the form that changed. */
	changed = 0U;
	fields = page_hosts_fields();
	for (index = 0U; index < fields; index++) {
		field = page.host_fields[index].text;
		same = strcmp(field, page.host_filled[index]);
		if (same == 0)
			continue;

		/* The value as the file holds it (empty: the lines go). */
		written[0] = '\0';
		error = 0;
		why = NULL;
		if (index == PAGE_HOST_NAME) {
			error = terminal_hosts_put_names(field, written, sizeof(written));
		} else if (field[0] == '\0') {
			error = 0;
		} else if (index == PAGE_HOST_COMMAND) {
			error = terminal_hosts_put_command(field, written, sizeof(written));
		} else if (index >= PAGE_HOST_X11) {
			error = terminal_hosts_put_x11(page_host_keywords[index], field, written, sizeof(written), &why);
		} else {
			error = terminal_hosts_put_word(field, written, sizeof(written));
		}

		/* A port is a number from 1 to 65535. */
		if (error == 0 && index == PAGE_HOST_PORT && field[0] != '\0') {
			port = strtoul(field, &end, 10);
			if (*end != '\0' || port == 0UL || port > 65535UL || field[0] < '0' || field[0] > '9')
				error = EINVAL;
		}

		/* A value the file cannot hold is told, and the form stays. */
		if (error != 0) {
			if (why == NULL)
				why = page_hosts_why(index, error);
			(void)snprintf(text, sizeof(text), "%s: %s", page_host_field_labels[index], why);
			page_say(&page.hosts_message, 1, text);
			kl_ui_set_focus(page.ui, PAGE_ID_HOST_FIELD + index, 0U);
			return EINVAL;
		}

		/* The line changed in the text. */
		error = terminal_hosts_set(&page.hosts, page.hosts_block, page_host_keywords[index], written, &out, &size);
		if (error == 0)
			error = page_hosts_take(out, size);
		if (error != 0) {
			page_say(&page.hosts_message, 1, "There is no memory to change the text.");
			return error;
		}

		/* The field holds what the text has now. */
		(void)snprintf(page.host_filled[index], sizeof(page.host_filled[index]), "%s", field);
		changed++;
	}

	/* The form shows the values as the file holds them now. */
	if (changed > 0U) {
		page_hosts_fill();
		page_log("hosts", "applied");
	}

	/* Succeeded: the text has the form's values. */
	return 0;
}

/* Tells why a field's value cannot be written. */
static const char *
page_hosts_why(
	unsigned field,
	int error)
{
	/* Too long for a line. */
	if (error == ENAMETOOLONG)
		return "too long";

	/* What each field takes. */
	switch (field) {
	case PAGE_HOST_NAME:
		return "a host needs a name: words without quotes, none starting with # or =";
	case PAGE_HOST_PORT:
		return "a number from 1 to 65535";
	case PAGE_HOST_COMMAND:
		return "a command cannot start with = or hold a control character";
	default:
		break;
	}

	/* A word with a quote or a control character. */
	return "a value cannot hold a double quote or a control character";
}

/* Adds a Host of the new host's name where ssh reads it before what would win over it, and shows its form. */
static void
page_hosts_add_host(void)
{
	char names[TERMINAL_HOSTS_VALUE_MAX];
	char text[PAGE_MESSAGE];
	unsigned block;
	char *out;
	size_t size;
	int error;

	/* The names as the file holds them. */
	error = terminal_hosts_put_names(page.host_new.text, names, sizeof(names));
	if (error != 0) {
		(void)snprintf(text, sizeof(text), "New host: %s", page_hosts_why(PAGE_HOST_NAME, error));
		page_say(&page.hosts_message, 1, text);
		return;
	}

	/* The form shown kept first. */
	error = page_hosts_apply();
	if (error != 0)
		return;

	/* The block in the text, shown. */
	error = terminal_hosts_add_host(&page.hosts, names, &out, &size, &block);
	if (error == 0)
		error = page_hosts_take(out, size);
	if (error != 0) {
		page_say(&page.hosts_message, 1, "There is no memory to change the text.");
		return;
	}

	/* The new block shown, its address next to be typed. */
	page.hosts_block = block;
	page_hosts_fill();
	kl_field_set(&page.host_new, "");
	kl_ui_set_focus(page.ui, PAGE_ID_HOST_FIELD + 1U, 0U);
	(void)snprintf(text, sizeof(text), "Host %s was added at line %u. Fill in its address and the rest, then Save.", names, page.hosts.lines[page.hosts.blocks[block].first].number);
	page_say(&page.hosts_message, 0, text);
}

/* Takes the Host shown out of the text (its keywords' lines, its comments kept), and shows another. */
static void
page_hosts_remove_host(void)
{
	char title[PAGE_HOST_TEXT];
	char text[PAGE_MESSAGE];
	unsigned removed;
	unsigned block;
	char *out;
	size_t size;
	int listed;
	int error;

	/* The Host shown. */
	block = page.hosts_block;
	if (block == TERMINAL_HOSTS_NONE)
		return;
	page_hosts_title(block, title, sizeof(title));

	/* The text without it; its form's changes go with it. */
	error = terminal_hosts_remove_host(&page.hosts, block, &out, &size, &removed);
	if (error == 0)
		error = page_hosts_take(out, size);
	if (error != 0) {
		page_say(&page.hosts_message, 1, "The host could not be taken out.");
		return;
	}

	/* The block before it shown (the first listed when there is none). */
	page.hosts_filled = TERMINAL_HOSTS_NONE;
	page.hosts_block = TERMINAL_HOSTS_NONE;
	while (block > 0U) {
		block--;
		if (block >= page.hosts.block_count)
			continue;
		listed = page_hosts_listed(block);
		if (listed) {
			page.hosts_block = block;
			break;
		}
	}

	/* Else the first listed; its form, and the word. */
	if (page.hosts_block == TERMINAL_HOSTS_NONE)
		page.hosts_block = page_hosts_first();
	page_hosts_fill();
	(void)snprintf(text, sizeof(text), "Host %s was taken out (%u lines). Save writes the file.", title, removed);
	page_say(&page.hosts_message, 0, text);
	page_log("hosts", "host-removed");
}

/* Adds the new identity file to the Host shown, after its others. */
static void
page_hosts_add_identity(void)
{
	char written[TERMINAL_HOSTS_VALUE_MAX];
	char text[PAGE_MESSAGE];
	char *out;
	size_t size;
	int error;

	/* The path as the file holds it. */
	error = terminal_hosts_put_word(page.identity_new.text, written, sizeof(written));
	if (error != 0) {
		(void)snprintf(text, sizeof(text), "Identity file: %s", page_hosts_why(PAGE_HOST_FIELDS, error));
		page_say(&page.hosts_message, 1, text);
		return;
	}

	/* The form's changes first, so the lines follow in the order they were made. */
	error = page_hosts_apply();
	if (error != 0)
		return;

	/* The line in the text. */
	error = terminal_hosts_add(&page.hosts, page.hosts_block, "IdentityFile", written, &out, &size);
	if (error == 0)
		error = page_hosts_take(out, size);
	if (error != 0) {
		page_say(&page.hosts_message, 1, "There is no memory to change the text.");
		return;
	}

	/* The field emptied for the next. */
	kl_field_set(&page.identity_new, "");
	page_say(&page.hosts_message, 0, "The identity file was added. Save writes the file.");
}

/* Adds the new forward (Local or Remote) to the Host shown, after its others. */
static void
page_hosts_add_forward(void)
{
	char written[TERMINAL_HOSTS_VALUE_MAX];
	char text[PAGE_MESSAGE];
	const char *keyword;
	const char *why;
	char *out;
	size_t size;
	int error;

	/* Where it listens and where it goes, as the file holds them. */
	error = terminal_hosts_put_forward(page.forward_remote, page.forward_listen.text, page.forward_to.text, written, sizeof(written), &why);
	if (error != 0) {
		(void)snprintf(text, sizeof(text), "Forward: %s", why);
		page_say(&page.hosts_message, 1, text);
		return;
	}

	/* The form's changes first, so the lines follow in the order they were made. */
	error = page_hosts_apply();
	if (error != 0)
		return;

	/* The line in the text. */
	keyword = "LocalForward";
	if (page.forward_remote)
		keyword = "RemoteForward";
	error = terminal_hosts_add(&page.hosts, page.hosts_block, keyword, written, &out, &size);
	if (error == 0)
		error = page_hosts_take(out, size);
	if (error != 0) {
		page_say(&page.hosts_message, 1, "There is no memory to change the text.");
		return;
	}

	/* The fields emptied for the next. */
	kl_field_set(&page.forward_listen, "");
	kl_field_set(&page.forward_to, "");
	(void)snprintf(text, sizeof(text), "%s %s was added. Save writes the file.", keyword, written);
	page_say(&page.hosts_message, 0, text);
}

/* Takes one line (an identity file or a forward) out of the text. */
static void
page_hosts_remove_line(
	unsigned line)
{
	char text[PAGE_MESSAGE];
	unsigned number;
	char *out;
	size_t size;
	int error;

	/* The line's number before it goes. */
	if (line >= page.hosts.line_count)
		return;
	number = page.hosts.lines[line].number;

	/* The text without it. */
	error = terminal_hosts_remove(&page.hosts, line, &out, &size);
	if (error == 0)
		error = page_hosts_take(out, size);
	if (error != 0) {
		page_say(&page.hosts_message, 1, "There is no memory to change the text.");
		return;
	}

	/* Succeeded: the word. */
	(void)snprintf(text, sizeof(text), "Line %u was taken out. Save writes the file.", number);
	page_say(&page.hosts_message, 0, text);
}

/*
 * Saves the text as the ssh_config: the form's changes in it, the
 * program's check finding no problem the file did not have, and the file
 * as it was read (a change made outside is not written over: Revert reads
 * it again).
 */
static void
page_hosts_save(void)
{
	char report[PAGE_MESSAGE];
	char text[PAGE_MESSAGE];
	char *now;
	char *copy;
	size_t now_size;
	int differs;
	int fresh;
	int same;
	int error;

	/* The form's changes, then whether anything changed. */
	error = page_hosts_apply();
	if (error != 0)
		return;
	same = page_hosts_saved();
	if (same) {
		page_say(&page.hosts_message, 0, "Nothing changed.");
		return;
	}

	/* No problem the file did not have already. */
	if (page.program->check != NULL) {
		fresh = terminal_hosts_new_problem(page.program->check, page.program->data, page.hosts_original, page.hosts_original_size, page.hosts_text, page.hosts_size, report, sizeof(report));
		if (fresh) {
			(void)snprintf(text, sizeof(text), "Not saved: %s", report);
			page_say(&page.hosts_message, 1, text);
			page_log("hosts", "save-refused");
			return;
		}
	}

	/* The file as it was read (one not there then and now is the same). */
	now = NULL;
	now_size = 0U;
	error = page.program->read(page.program->data, page.program->config_path, TERMINAL_HOSTS_TEXT_MAX, &now, &now_size);
	if (error == ENOENT) {
		now = NULL;
		now_size = 0U;
		error = 0;
	}

	/* A file that cannot be read is not written over. */
	if (error != 0 && error != EFBIG) {
		(void)snprintf(text, sizeof(text), "%s could not be read (%s).", page.program->config_shown, strerror(error));
		page_say(&page.hosts_message, 1, text);
		return;
	}

	/* The same bytes as read (a file grown too large is not). */
	same = 0;
	if (error == 0 && now_size == page.hosts_original_size) {
		differs = 0;
		if (now_size > 0U)
			differs = memcmp(now, page.hosts_original, now_size);
		if (differs == 0)
			same = 1;
	}

	/* A file changed outside is not written over. */
	free(now);
	if (!same) {
		page_say(&page.hosts_message, 1, "The file was changed outside since it was read. Revert to read it again.");
		page_log("hosts", "changed-outside");
		return;
	}

	/* The file, written whole. */
	error = page.program->replace(page.program->data, page.program->config_path, page.hosts_text, page.hosts_size);
	if (error != 0) {
		(void)snprintf(text, sizeof(text), "%s could not be written (%s).", page.program->config_shown, strerror(error));
		page_say(&page.hosts_message, 1, text);
		page_log("hosts", "save-failed");
		return;
	}

	/* What the file holds now. */
	copy = malloc(page.hosts_size + 1U);
	if (copy != NULL) {
		memcpy(copy, page.hosts_text, page.hosts_size + 1U);
		free(page.hosts_original);
		page.hosts_original = copy;
		page.hosts_original_size = page.hosts_size;
	}

	/* Succeeded. */
	page_say(&page.hosts_message, 0, "Saved. It takes effect from the next connection.");
	page_log("hosts", "saved");
}

/* Opens the ssh_config in the system's text editor (an empty one made when it is not there). */
static void
page_hosts_editor(
	struct terminal_window *window)
{
	char text[PAGE_MESSAGE];
	int exists;
	int error;

	/* A program that cannot open one. */
	if (page.program->open_editor == NULL) {
		page_say(&page.hosts_message, 1, "There is no text editor to open the file with.");
		return;
	}

	/* An empty file when there is none, for the editor to open. */
	exists = page.program->exists(page.program->data, page.program->config_path);
	if (!exists) {
		error = page.program->create(page.program->data, page.program->config_path, "", 0U, 1);
		if (error != 0 && error != EEXIST) {
			(void)snprintf(text, sizeof(text), "%s could not be made (%s).", page.program->config_shown, strerror(error));
			page_say(&page.hosts_message, 1, text);
			return;
		}
	}

	/* The editor, on its way. */
	error = page.program->open_editor(page.program->data, window, page.program->config_path);
	if (error != 0) {
		(void)snprintf(text, sizeof(text), "The text editor could not be opened (%s).", strerror(error));
		page_say(&page.hosts_message, 1, text);
		page_log("hosts", "editor-failed");
		return;
	}

	/* Succeeded: its changes take effect from the next connection. */
	page_say(&page.hosts_message, 0, "The file is open in the text editor. Its changes take effect from the next connection; Revert shows them here.");
	page_log("hosts", "editor");
}

/* Reports whether the pane's text is the file as last read or saved. */
static int
page_hosts_saved(void)
{
	int differs;

	/* The same length. */
	if (page.hosts_size != page.hosts_original_size)
		return 0;

	/* The same bytes. */
	differs = 0;
	if (page.hosts_size > 0U)
		differs = memcmp(page.hosts_text, page.hosts_original, page.hosts_size);
	if (differs != 0)
		return 0;

	/* Saved. */
	return 1;
}

/* Finds the block shown first: the first Host, else the first block listed, else none. */
static unsigned
page_hosts_first(void)
{
	unsigned index;
	int listed;

	/* The first Host. */
	for (index = 0U; index < page.hosts.block_count; index++) {
		if (page.hosts.blocks[index].kind == TERMINAL_HOSTS_BLOCK_HOST)
			return index;
	}

	/* Else the first listed. */
	for (index = 0U; index < page.hosts.block_count; index++) {
		listed = page_hosts_listed(index);
		if (listed)
			return index;
	}

	/* None. */
	return TERMINAL_HOSTS_NONE;
}

/* Reports whether the list shows a block: every Host and Match, and the part before them when it has a keyword's line. */
static int
page_hosts_listed(
	unsigned block)
{
	const struct terminal_hosts_block *read;
	unsigned index;

	/* A Host's and a Match's. */
	read = &page.hosts.blocks[block];
	if (read->kind != TERMINAL_HOSTS_BLOCK_BEFORE)
		return 1;

	/* The part before them, with a keyword's line. */
	for (index = read->first; index < read->end; index++) {
		if (page.hosts.lines[index].kind == TERMINAL_HOSTS_LINE_KEYWORD ||
		    page.hosts.lines[index].kind == TERMINAL_HOSTS_LINE_BAD)
			return 1;
	}

	/* Only comments and blank lines. */
	return 0;
}

/* Counts a block's lines a removal takes out: all but its comments and blank lines. */
static unsigned
page_hosts_lines(
	unsigned block)
{
	unsigned index;
	unsigned count;

	/* Each line of a keyword. */
	count = 0U;
	for (index = page.hosts.blocks[block].first; index < page.hosts.blocks[block].end; index++) {
		if (page.hosts.lines[index].kind != TERMINAL_HOSTS_LINE_EMPTY &&
		    page.hosts.lines[index].kind != TERMINAL_HOSTS_LINE_COMMENT)
			count++;
	}

	/* Reports the count. */
	return count;
}

/*
 * Writes a block's title: a Host's names and what it connects to
 * (user@address:port from its own lines), "Match" and its condition, or
 * "Before any Host".
 */
static void
page_hosts_title(
	unsigned block,
	char *text,
	size_t size)
{
	const struct terminal_hosts_block *read;
	char names[PAGE_HOST_TEXT];
	char user[PAGE_HOST_TEXT];
	char address[PAGE_HOST_TEXT];
	char port[16];
	const char *at;
	const char *colon;
	unsigned line;

	/* The part before any Host. */
	read = &page.hosts.blocks[block];
	if (read->kind == TERMINAL_HOSTS_BLOCK_BEFORE) {
		(void)snprintf(text, size, "Before any Host");
		return;
	}

	/* A Match and its condition. */
	(void)terminal_hosts_value(&page.hosts, read->first, names, sizeof(names));
	if (read->kind == TERMINAL_HOSTS_BLOCK_MATCH) {
		(void)snprintf(text, size, "Match %s", names);
		return;
	}

	/* A Host's user, address and port, from its own lines. */
	line = terminal_hosts_find(&page.hosts, block, "User", TERMINAL_HOSTS_NONE);
	(void)terminal_hosts_word(&page.hosts, line, 0U, user, sizeof(user));
	line = terminal_hosts_find(&page.hosts, block, "HostName", TERMINAL_HOSTS_NONE);
	(void)terminal_hosts_word(&page.hosts, line, 0U, address, sizeof(address));
	line = terminal_hosts_find(&page.hosts, block, "Port", TERMINAL_HOSTS_NONE);
	(void)terminal_hosts_word(&page.hosts, line, 0U, port, sizeof(port));

	/* The names alone when it has none of them. */
	if (user[0] == '\0' && address[0] == '\0' && port[0] == '\0') {
		(void)snprintf(text, size, "%s", names);
		return;
	}

	/* The words between the parts that are there. */
	at = "";
	if (user[0] != '\0')
		at = "@";
	colon = "";
	if (port[0] != '\0')
		colon = ":";

	/* Without an address: the user and the port as words. */
	if (address[0] == '\0') {
		colon = "";
		if (port[0] != '\0')
			colon = "  port ";
		(void)snprintf(text, size, "%s   %s%s%s", names, user, colon, port);
		return;
	}

	/* The names and user@address:port, each part that is there. */
	(void)snprintf(text, size, "%s   %s%s%s%s%s", names, user, at, address, colon, port);
}

/* Writes where a line is: its number and its block ("line 3 (Host *)"). */
static void
page_hosts_where(
	unsigned line,
	char *text,
	size_t size)
{
	const struct terminal_hosts_block *block;
	char names[PAGE_HOST_TEXT];

	/* The part before any Host. */
	block = &page.hosts.blocks[page.hosts.lines[line].block];
	if (block->kind == TERMINAL_HOSTS_BLOCK_BEFORE) {
		(void)snprintf(text, size, "line %u, before any Host", page.hosts.lines[line].number);
		return;
	}

	/* A Host's or a Match's line. */
	(void)terminal_hosts_value(&page.hosts, block->first, names, sizeof(names));
	if (block->kind == TERMINAL_HOSTS_BLOCK_MATCH) {
		(void)snprintf(text, size, "line %u, Match %s", page.hosts.lines[line].number, names);
	} else {
		(void)snprintf(text, size, "line %u, Host %s", page.hosts.lines[line].number, names);
	}
}

/* Writes a forward as the card shows it: its kind, where it listens and where it goes. */
static void
page_hosts_forward_text(
	unsigned line,
	char *text,
	size_t size)
{
	char listen[PAGE_HOST_TEXT];
	char destination[PAGE_HOST_TEXT];
	const char *kind;
	int remote;

	/* The kind. */
	remote = terminal_hosts_keyword_is(&page.hosts, line, "RemoteForward");
	kind = "Local";
	if (remote)
		kind = "Remote";

	/* Its two ends (a value of another shape as written). */
	(void)terminal_hosts_word(&page.hosts, line, 0U, listen, sizeof(listen));
	(void)terminal_hosts_word(&page.hosts, line, 1U, destination, sizeof(destination));
	if (destination[0] == '\0') {
		(void)terminal_hosts_value(&page.hosts, line, listen, sizeof(listen));
		(void)snprintf(text, size, "%s  %s", kind, listen);
		return;
	}

	/* Where it listens, and where it goes. */
	(void)snprintf(text, size, "%s  %s  ->  %s", kind, listen, destination);
}

/*
 * Reports whether a line of the block shown is one of its Other lines:
 * a comment, a keyword the form does not show (Include among them), a
 * second line of a field's keyword, a line that cannot be read; every line
 * of a Match but its own.
 */
static int
page_hosts_other_line(
	unsigned line)
{
	static const char *const listed[] = { "IdentityFile", "LocalForward", "RemoteForward" };
	const struct terminal_hosts_line *read;
	unsigned index;
	unsigned fields;
	int same;
	int second;

	/* Blank lines and the block's own Host or Match line are not. */
	read = &page.hosts.lines[line];
	if (read->kind == TERMINAL_HOSTS_LINE_EMPTY ||
	    read->kind == TERMINAL_HOSTS_LINE_HOST ||
	    read->kind == TERMINAL_HOSTS_LINE_MATCH)
		return 0;

	/* Every other line of a Match, comments and lines that cannot be read. */
	if (page.hosts.blocks[read->block].kind == TERMINAL_HOSTS_BLOCK_MATCH)
		return 1;
	if (read->kind != TERMINAL_HOSTS_LINE_KEYWORD)
		return 1;

	/* A line of a list the form shows. */
	for (index = 0U; index < sizeof(listed) / sizeof(listed[0]); index++) {
		same = terminal_hosts_keyword_is(&page.hosts, line, listed[index]);
		if (same)
			return 0;
	}

	/* A field's keyword: its first line is in the form, a second one is not. */
	fields = page_hosts_fields();
	for (index = PAGE_HOST_NAME + 1U; index < fields; index++) {
		same = terminal_hosts_keyword_is(&page.hosts, line, page_host_keywords[index]);
		if (!same)
			continue;
		second = page_hosts_second(line);
		return second;
	}

	/* A keyword the form does not show. */
	return 1;
}

/* Reports whether a line is a second line of a field's keyword in its block (ssh passes over it). */
static int
page_hosts_second(
	unsigned line)
{
	unsigned index;
	unsigned first;
	int same;

	/* The field's keyword it is. */
	for (index = PAGE_HOST_NAME + 1U; index < PAGE_HOST_FIELDS; index++) {
		same = terminal_hosts_keyword_is(&page.hosts, line, page_host_keywords[index]);
		if (!same)
			continue;

		/* Not the block's first of it. */
		first = terminal_hosts_find(&page.hosts, page.hosts.lines[line].block, page_host_keywords[index], TERMINAL_HOSTS_NONE);
		if (first != line)
			return 1;
		return 0;
	}

	/* Not a field's keyword. */
	return 0;
}

/*
 * Draws a row of a card with a label, a line's words (fitted, at most
 * PAGE_KNOWN_LINE wide) and, when it may go, a button that takes the line
 * out; reports the edge below it.
 */
static int
page_line_row(
	int top,
	int left,
	int width,
	const char *label,
	const char *text,
	kl_color ink,
	uint32_t index,
	int removable)
{
	struct kl_rect control;
	int value_x;
	int words;
	int baseline;
	int pressed;

	/* The label, and the words after it. */
	value_x = page_label(top, left, width, label, 1);
	words = left + width - PAGE_INSET - value_x - PAGE_ICON_BUTTON - PAGE_AFTER_GAP;
	if (words > PAGE_KNOWN_LINE)
		words = PAGE_KNOWN_LINE;
	baseline = kl_text_center(PAGE_TEXT_ROW, top, PAGE_ROW);
	(void)kl_text_draw_fit(&page.text, &page.canvas, value_x, baseline, text, PAGE_TEXT_ROW, 0, words, ink);

	/* The removal's button after the words. */
	if (removable) {
		control.width = PAGE_ICON_BUTTON;
		control.height = PAGE_ICON_BUTTON;
		control.x = value_x + words + PAGE_AFTER_GAP;
		control.y = top + (PAGE_ROW - control.height) / 2;
		pressed = kl_icon_button(page.ui, &page.style, PAGE_ID_LINE_REMOVE, index, &control, KL_ICON_TRASH, PAGE_ICON_PIXELS, KL_BUTTON_QUIET);
		if (pressed)
			page_hosts_later(PAGE_HOSTS_REMOVE_LINE, index);
	}

	/* Reports the edge below the row. */
	return top + PAGE_ROW;
}

/* Draws a row of a label, a field and a button after it; reports whether the button was pressed. */
static int
page_field_button(
	int top,
	int left,
	int width,
	const char *label,
	uint32_t id,
	struct kl_field *field,
	const char *placeholder,
	uint32_t button,
	const char *button_label)
{
	const struct kl_theme *theme;
	struct kl_rect control;
	int value_x;
	int button_width;
	int pressed;

	/* The label in the pane's column. */
	theme = page.style.theme;
	value_x = page_label(top, left, width, label, 1);

	/* The field, leaving room for the button. */
	button_width = kl_button_width(&page.style, button_label);
	control.x = value_x;
	control.y = top + (PAGE_ROW - theme->control_height) / 2;
	control.width = page_control_width(left, width, value_x);
	if (control.width > left + width - PAGE_INSET - value_x - button_width - 8)
		control.width = left + width - PAGE_INSET - value_x - button_width - 8;
	control.height = theme->control_height;
	(void)kl_field(page.ui, &page.style, id, &control, field, placeholder);

	/* The button after it. */
	control.x += control.width + 8;
	control.width = button_width;
	pressed = kl_button(page.ui, &page.style, button, &control, button_label, 0U);

	/* Reports whether it was pressed. */
	return pressed;
}

/* Draws the Appearance pane: the text's size, the theme and the ambiguous width, then the program's own; reports its bottom. */
static int
page_appearance(
	struct terminal_window *window,
	const struct terminal_page_state *state,
	int top,
	int left,
	int width)
{
	const struct kl_theme *theme;
	struct kl_rect card;
	struct kl_rect control;
	char value[64];
	unsigned index;
	unsigned flags;
	int value_x;
	int right;
	int pressed;
	int wide;
	int y;

	/* The pane's label column. */
	page_column(TERMINAL_PANE_APPEARANCE, page_appearance_labels, sizeof(page_appearance_labels) / sizeof(page_appearance_labels[0]));

	/* The card of what takes effect at once: the size, the theme and the width, then a note. */
	theme = page.style.theme;
	card.x = left;
	card.y = top;
	card.width = width;
	card.height = PAGE_CARD_SUBTITLED + 4 * PAGE_ROW + page_note_height(width - 40, "Ambiguous-width characters already on the screen keep their width.") + PAGE_CARD_FOOT;
	y = page_card(&card, "At once", "The text's size, the colours and the width of ambiguous characters");
	right = card.x + card.width - PAGE_INSET;

	/* The text's size: smaller and larger after its label, then its value. */
	value_x = page_label(y, card.x, card.width, "Text size", 0);
	control.x = value_x;
	control.y = y + (PAGE_ROW - theme->control_height) / 2;
	control.height = theme->control_height;
	control.width = kl_button_width(&page.style, "Smaller");
	pressed = kl_button(page.ui, &page.style, PAGE_ID_SMALLER, &control, "Smaller", 0U);
	if (pressed)
		page_action(window, TERMINAL_ACTION_ZOOM_OUT);

	/* Larger beside Smaller. */
	control.x += control.width + 6;
	control.width = kl_button_width(&page.style, "Larger");
	pressed = kl_button(page.ui, &page.style, PAGE_ID_LARGER, &control, "Larger", 0U);
	if (pressed)
		page_action(window, TERMINAL_ACTION_ZOOM_IN);

	/* The size in use after the two buttons. */
	(void)snprintf(value, sizeof(value), "%u pixels", state->pixels);
	page_row_text(control.x + control.width + PAGE_AFTER_GAP, y, right, value);
	y += PAGE_ROW;

	/* The fixed sizes of View > Text Size, the one in use marked. */
	value_x = page_label(y, card.x, card.width, "Size", 0);
	control.x = value_x;
	control.y = y + (PAGE_ROW - theme->control_height) / 2;
	for (index = 0U; index < PAGE_SIZES; index++) {
		flags = 0U;
		if (state->pixels == page_sizes[index])
			flags = KL_BUTTON_PRIMARY;
		control.width = kl_button_width(&page.style, page_size_names[index]);
		pressed = kl_button(page.ui, &page.style, PAGE_ID_SIZES + index, &control, page_size_names[index], flags);
		if (pressed)
			page_action(window, page_size_actions[index]);
		control.x += control.width + 6;
	}

	/* The theme's row under the sizes. */
	y += PAGE_ROW;

	/* The theme, the one in use marked. */
	value_x = page_label(y, card.x, card.width, "Theme", 0);
	control.x = value_x;
	for (index = 0U; index < TERMINAL_THEMES; index++) {
		flags = 0U;
		if (state->theme == index)
			flags = KL_BUTTON_PRIMARY;
		control.y = y + (PAGE_ROW - theme->control_height) / 2;
		control.width = kl_button_width(&page.style, page_theme_names[index]);
		pressed = kl_button(page.ui, &page.style, PAGE_ID_THEMES + index, &control, page_theme_names[index], flags);
		if (pressed)
			page_action(window, page_theme_actions[index]);
		control.x += control.width + 6;
	}

	/* The switch's row under the themes. */
	y += PAGE_ROW;

	/* The ambiguous width: a switch. */
	value_x = page_label(y, card.x, card.width, "Ambiguous width", 1);
	wide = state->ambiguous_wide;
	pressed = kl_switch(page.ui, &page.style, PAGE_ID_AMBIGUOUS, value_x, y + (PAGE_ROW - theme->switch_height) / 2, &wide, 0U);
	if (pressed)
		page_action(window, TERMINAL_ACTION_AMBIGUOUS_WIDE);

	/* What the switch does, after it, and the note under the row. */
	page_row_text(value_x + theme->switch_width + PAGE_AFTER_GAP, y, right, "Wide (two cells, as in East Asian text)");
	y += PAGE_ROW;
	(void)page_note(y, card.x + 20, card.width - 40, "Ambiguous-width characters already on the screen keep their width.", theme->text_secondary);

	/* The program's own settings of the look, or the font the terminal draws with. */
	y = card.y + card.height + PAGE_GAP;
	if (page.program->font != NULL) {
		card.y = y;
		card.height = PAGE_CARD_TITLE + PAGE_ROW + page_note_height(width - 40, "The font is chosen with --font when the terminal starts.") + PAGE_CARD_FOOT;
		y = page_card(&card, "Font", NULL);
		value_x = page_label(y, card.x, card.width, "Font file", 1);
		page_row_text(value_x, y, card.x + card.width - PAGE_INSET, page.program->font);
		(void)page_note(y + PAGE_ROW, card.x + 20, card.width - 40, "The font is chosen with --font when the terminal starts.", theme->text_secondary);
		return card.y + card.height;
	}

	/* The program's own settings of the look. */
	y = page_items(TERMINAL_PANE_APPEARANCE, y, left, width);

	/* Reports the pane's bottom. */
	return y;
}

/* Draws the Behavior pane: copy-on-select, then the program's own; reports its bottom. */
static int
page_behavior(
	struct terminal_window *window,
	const struct terminal_page_state *state,
	int top,
	int left,
	int width)
{
	const struct kl_theme *theme;
	struct kl_rect card;
	int value_x;
	int changed;
	int on;
	int y;

	/* The pane's label column. */
	page_column(TERMINAL_PANE_BEHAVIOR, page_behavior_labels, sizeof(page_behavior_labels) / sizeof(page_behavior_labels[0]));

	/* The card of what takes effect at once. */
	theme = page.style.theme;
	card.x = left;
	card.y = top;
	card.width = width;
	card.height = PAGE_CARD_TITLE + PAGE_ROW + page_note_height(width - 40, "Off, only Edit > Copy puts the selected text on the clipboard.") + PAGE_CARD_FOOT;
	y = page_card(&card, "At once", NULL);

	/* Copy-on-select: a switch. */
	value_x = page_label(y, card.x, card.width, "Copy on select", 1);
	on = state->copy_on_select;
	changed = kl_switch(page.ui, &page.style, PAGE_ID_COPY_SELECT, value_x, y + (PAGE_ROW - theme->switch_height) / 2, &on, 0U);
	if (changed)
		page_action(window, TERMINAL_ACTION_COPY_ON_SELECT);

	/* What the switch does, after it, and the note under the row. */
	page_row_text(value_x + theme->switch_width + PAGE_AFTER_GAP, y, card.x + card.width - PAGE_INSET, "The selected text goes to the clipboard");
	(void)page_note(y + PAGE_ROW, card.x + 20, card.width - 40, "Off, only Edit > Copy puts the selected text on the clipboard.", theme->text_secondary);

	/* The program's own settings of the connections. */
	y = page_items(TERMINAL_PANE_BEHAVIOR, card.y + card.height + PAGE_GAP, left, width);

	/* Reports the pane's bottom. */
	return y;
}

/*
 * Draws the program's settings of a pane, a card for each effect (from the
 * next connection, from the next start), their fields and one Save under
 * the last, with the last save's word; reports the bottom (top when the
 * pane has none).
 */
static int
page_items(
	unsigned pane,
	int top,
	int left,
	int width)
{
	const struct terminal_page_item *item;
	const struct kl_theme *theme;
	struct kl_rect card;
	struct kl_rect control;
	char title[64];
	const char *hint;
	unsigned effect;
	unsigned index;
	unsigned rows;
	int pressed;
	int shown;
	int y;

	/* The colours and sizes of the page's widgets. */
	theme = page.style.theme;

	/* The fields' values, the first time. */
	if (!page.items_read)
		page_items_read();

	/* A card for each effect that has settings in the pane. */
	shown = 0;
	y = top;
	for (effect = TERMINAL_EFFECT_CONNECTION; effect <= TERMINAL_EFFECT_START; effect++) {
		/* How many rows it has. */
		rows = 0U;
		for (index = 0U; index < page.program->item_count; index++) {
			item = &page.program->items[index];
			if (item->pane == pane && item->effect == effect)
				rows++;
		}

		/* An effect without settings in the pane has no card. */
		if (rows == 0U)
			continue;

		/* The card's title: when its settings take effect. */
		if (effect == TERMINAL_EFFECT_START) {
			(void)snprintf(title, sizeof(title), "From the next start");
		} else {
			(void)snprintf(title, sizeof(title), "From the next %s", page_connection());
		}

		/* The card and its fields. */
		card.x = left;
		card.y = y;
		card.width = width;
		card.height = PAGE_CARD_TITLE + (int)rows * PAGE_ROW + PAGE_CARD_FOOT;
		y = page_card(&card, title, NULL);
		for (index = 0U; index < page.program->item_count; index++) {
			item = &page.program->items[index];
			if (item->pane != pane || item->effect != effect)
				continue;
			hint = item->keyword;
			if (item->hint != NULL)
				hint = item->hint;
			y = page_field_row(y, card.x, card.width, item->label, PAGE_ID_ITEM + index, &page.items[index], hint);
		}

		/* The next card goes under this one. */
		y = card.y + card.height + PAGE_GAP;
		shown = 1;
	}

	/* A pane without the program's settings. */
	if (!shown)
		return top;

	/* Save, and the last word. */
	control.x = left + 4;
	control.y = y;
	control.height = theme->control_height;
	control.width = kl_button_width(&page.style, "Save");
	pressed = kl_button(page.ui, &page.style, PAGE_ID_ITEMS_SAVE, &control, "Save", KL_BUTTON_PRIMARY);
	if (pressed)
		page_items_save(pane);
	y += control.height + 10;
	y = page_message_draw(y, left + 4, width - 8, &page.items_message);

	/* Reports the bottom. */
	return y;
}

/* Draws the Keys pane: a key by its name (its public key when it is there, else what makes it), then known_hosts; reports its bottom. */
static int
page_keys(
	struct terminal_window *window,
	int top,
	int left,
	int width)
{
	const struct kl_theme *theme;
	struct kl_rect card;
	struct kl_rect control;
	struct kl_rect area;
	char subtitle[PAGE_MESSAGE];
	unsigned changed;
	int pressed;
	int rows;
	int same;
	int y;
	int known_top;

	/* The pane's label column. */
	page_column(TERMINAL_PANE_KEYS, page_key_labels, sizeof(page_key_labels) / sizeof(page_key_labels[0]));

	/* The key named looked up whenever its name changed. */
	theme = page.style.theme;
	same = strcmp(page.key_shown, page.key_name.text);
	if (same != 0 || !page.key_looked)
		page_key_look();

	/* The card's height: the name, and the public key (two notes and Copy) or the fields of a new one and Make Key. */
	card.x = left;
	card.y = top;
	card.width = width;
	if (page.key_found) {
		card.height = PAGE_CARD_SUBTITLED + PAGE_ROW + PAGE_AREA_PUBLIC_LINES * PAGE_AREA_LINE + PAGE_AREA_PAD + 8 +
		    page_note_height(width - 40, page.key_fingerprint) + 8 + theme->control_height + 12;
	} else {
		rows = 4;
		card.height = PAGE_CARD_SUBTITLED + rows * PAGE_ROW + 8 + theme->control_height + 12 +
		    page_note_height(width - 40, "A passphrase is optional; with one, the key file is encrypted as ssh-keygen encrypts it.");
	}

	/* Room for the last word, and the card drawn with its subtitle. */
	card.height += page_note_height(width - 40, page.key_message.text) + PAGE_CARD_FOOT;
	(void)snprintf(subtitle, sizeof(subtitle), "In %s", page.program->key_directory_shown);
	y = page_card(&card, "SSH key (ed25519)", subtitle);

	/* The name. */
	y = page_field_row(y, card.x, card.width, "Name", PAGE_ID_KEY_NAME, &page.key_name, "id_ed25519");

	/* A key there: its public key, its fingerprint, and Copy. */
	control.x = card.x + 20;
	control.height = theme->control_height;
	if (page.key_found) {
		/* The public key in an area to select from; a change is undone (the area is for reading and copying). */
		area.x = card.x + 20;
		area.y = y + 4;
		area.width = card.width - 40;
		area.height = PAGE_AREA_PUBLIC_LINES * PAGE_AREA_LINE + PAGE_AREA_PAD;
		changed = kl_text_area(page.ui, &page.style, PAGE_ID_KEY_PUBLIC_AREA, &area, &page.key_public_area, NULL);
		if ((changed & KL_FIELD_CHANGED) != 0U)
			kl_text_area_set(&page.key_public_area, page.key_line);
		y = area.y + area.height + 8;
		y = page_note(y, card.x + 20, card.width - 40, page.key_fingerprint, theme->text_secondary);
		control.y = y + 4;
		control.width = kl_button_width(&page.style, "Copy Public Key");
		pressed = kl_button(page.ui, &page.style, PAGE_ID_KEY_COPY, &control, "Copy Public Key", KL_BUTTON_PRIMARY);
		if (pressed) {
			page_set_aside(page.key_line, strlen(page.key_line));
			page_action(window, TERMINAL_ACTION_COPY);
			page_say(&page.key_message, 0, "The public key is on the clipboard: add it to the server's ~/.ssh/authorized_keys.");
			page_log("key-copy", page.key_shown);
		}

		/* Moves down past Copy. */
		y = control.y + control.height + 8;
	} else {
		/* No key of the name: its comment, an optional passphrase twice, and Make Key. */
		y = page_field_row(y, card.x, card.width, "Comment", PAGE_ID_KEY_COMMENT, &page.key_comment, "user@host");
		y = page_field_row(y, card.x, card.width, "Passphrase", PAGE_ID_KEY_PASS, &page.key_pass, "Optional");
		y = page_field_row(y, card.x, card.width, "Again", PAGE_ID_KEY_AGAIN, &page.key_again, "The passphrase again");
		control.y = y + 8;
		control.width = kl_button_width(&page.style, "Make Key");
		pressed = kl_button(page.ui, &page.style, PAGE_ID_KEY_MAKE, &control, "Make Key", KL_BUTTON_PRIMARY);
		if (pressed)
			page_key_make();
		y = control.y + control.height + 4;
		y = page_note(y, card.x + 20, card.width - 40, "A passphrase is optional; with one, the key file is encrypted as ssh-keygen encrypts it.", theme->text_secondary);
	}

	/* The last word of the key's card. */
	(void)page_message_draw(y, card.x + 20, card.width - 40, &page.key_message);

	/* Under the key's card, a card to paste a private key into (a program that offers it, while no key has the name), then known_hosts. */
	known_top = card.y + card.height + PAGE_GAP;
	if (!page.key_found && page.program->key_paste)
		known_top = page_paste_card(window, known_top, left, width) + PAGE_GAP;
	y = page_known(known_top, left, width);

	/* Reports the pane's bottom. */
	return y;
}

/* Draws the card that keeps a pasted private key under the key's name: an area for its text, Paste and Save Key; reports the card's bottom. */
static int
page_paste_card(
	struct terminal_window *window,
	int top,
	int left,
	int width)
{
	static const char note[] = "Paste the text of an OpenSSH private key (ssh-keygen -t ed25519). Only ssh-ed25519 keys are supported.";
	const struct kl_theme *theme;
	struct kl_rect card;
	struct kl_rect control;
	struct kl_rect area;
	char subtitle[PAGE_MESSAGE];
	int pressed;
	int y;

	/* The card: the area, the two buttons, the note and the last word. */
	theme = page.style.theme;
	card.x = left;
	card.y = top;
	card.width = width;
	card.height = PAGE_CARD_SUBTITLED + PAGE_AREA_PASTE_LINES * PAGE_AREA_LINE + PAGE_AREA_PAD + 8 + theme->control_height + 4 +
	    page_note_height(width - 40, note) + page_note_height(width - 40, page.key_paste_message.text) + PAGE_CARD_FOOT;
	(void)snprintf(subtitle, sizeof(subtitle), "Kept in %s under the name above", page.program->key_directory_shown);
	y = page_card(&card, "Paste a private key", subtitle);

	/* The area the text is pasted or typed into. */
	area.x = card.x + 20;
	area.y = y;
	area.width = card.width - 40;
	area.height = PAGE_AREA_PASTE_LINES * PAGE_AREA_LINE + PAGE_AREA_PAD;
	(void)kl_text_area(page.ui, &page.style, PAGE_ID_KEY_PASTE_AREA, &area, &page.key_paste_area, "-----BEGIN OPENSSH PRIVATE KEY-----");
	y = area.y + area.height + 8;

	/* Paste puts the clipboard's text in the area in place of what it holds (the run reads the clipboard). */
	control.x = card.x + 20;
	control.y = y;
	control.width = kl_button_width(&page.style, "Paste");
	control.height = theme->control_height;
	pressed = kl_button(page.ui, &page.style, PAGE_ID_KEY_PASTE, &control, "Paste", 0U);
	if (pressed) {
		kl_text_area_set(&page.key_paste_area, "");
		kl_ui_set_focus(page.ui, PAGE_ID_KEY_PASTE_AREA, 0U);
		page_action(window, TERMINAL_ACTION_PASTE);
		page_log("key-paste", "");
	}

	/* Save Key keeps what the area holds. */
	control.x += control.width + 12;
	control.width = kl_button_width(&page.style, "Save Key");
	pressed = kl_button(page.ui, &page.style, PAGE_ID_KEY_SAVE, &control, "Save Key", KL_BUTTON_PRIMARY);
	if (pressed)
		page_key_save(window);
	y = control.y + control.height + 4;

	/* What the area takes, and the last word. */
	y = page_note(y, card.x + 20, card.width - 40, note, theme->text_secondary);
	(void)page_message_draw(y, card.x + 20, card.width - 40, &page.key_paste_message);

	/* Reports the card's bottom. */
	return card.y + card.height;
}

/* Draws the known_hosts card: each line's names, type and fingerprint with a button that removes it; reports its bottom. */
static int
page_known(
	int top,
	int left,
	int width)
{
	const struct terminal_known_line *line;
	const struct kl_theme *theme;
	struct kl_rect card;
	struct kl_rect control;
	char subtitle[PAGE_MESSAGE];
	char text[PAGE_MESSAGE];
	unsigned index;
	int baseline;
	int pressed;
	int confirm;
	int words;
	int y;

	/* The colours and sizes of the page's widgets. */
	theme = page.style.theme;

	/* The file, the first time. */
	if (!page.known_read)
		page_known_read();

	/* The card: a row a line, the question of a removal, Reload and the last word. */
	confirm = 0;
	if (page.remove_line != 0U)
		confirm = theme->control_height + 12;
	card.x = left;
	card.y = top;
	card.width = width;
	card.height = PAGE_CARD_SUBTITLED + (int)page.known_count * PAGE_ROW + confirm + theme->control_height + 12 +
	    page_note_height(width - 40, page.known_message.text) + PAGE_CARD_FOOT;
	if (page.known_count == 0U)
		card.height += PAGE_NOTE_LINE;
	(void)snprintf(subtitle, sizeof(subtitle), "%s: the servers' keys you accepted", page.program->known_hosts_shown);
	y = page_card(&card, "Known hosts", subtitle);

	/* None. */
	if (page.known_count == 0U)
		y = page_note(y, card.x + 20, card.width - 40, "No server is known yet.", theme->text_secondary);

	/* How wide a line's words are drawn: the card's row, no wider than the most, the removal's button after them. */
	words = card.width - 80;
	if (words > PAGE_KNOWN_LINE)
		words = PAGE_KNOWN_LINE;

	/* Each line: its names and type, its fingerprint, and the removal's button. */
	for (index = 0U; index < page.known_count; index++) {
		line = &page.known[index];
		if (line->marker[0] != '\0')
			(void)snprintf(text, sizeof(text), "%s %s", line->marker, line->names);
		else
			(void)snprintf(text, sizeof(text), "%s", line->names);
		baseline = kl_text_center(theme->text_body, y, PAGE_ROW / 2) + 2;
		(void)kl_text_draw_fit(&page.text, &page.canvas, card.x + 20, baseline, text, theme->text_body, 0, words, theme->text);
		(void)snprintf(text, sizeof(text), "%s  %s", line->type, line->fingerprint);
		(void)kl_text_draw_fit(&page.text, &page.canvas, card.x + 20, baseline + PAGE_ROW / 2 - 2, text, theme->text_small, 0, words, theme->text_secondary);
		control.width = 28;
		control.height = 28;
		control.x = card.x + 20 + words + PAGE_AFTER_GAP;
		control.y = y + (PAGE_ROW - control.height) / 2;
		pressed = kl_icon_button(page.ui, &page.style, PAGE_ID_KNOWN_REMOVE, index, &control, KL_ICON_TRASH, 16, KL_BUTTON_QUIET);
		if (pressed) {
			page.remove_line = line->line;
			page.dirty = 1;
		}

		/* The next line's row. */
		y += PAGE_ROW;
	}

	/* The removal asked: Remove or Keep. */
	control.x = card.x + 20;
	control.height = theme->control_height;
	if (page.remove_line != 0U) {
		control.y = y + 6;
		(void)snprintf(text, sizeof(text), "Remove line %u?", page.remove_line);
		baseline = kl_text_center(theme->text_body, control.y, control.height);
		(void)kl_text_draw(&page.text, &page.canvas, control.x, baseline, text, strlen(text), theme->text_body, 0, theme->text);
		control.x += kl_text_width(&page.text, text, strlen(text), theme->text_body, 0) + 12;
		control.width = kl_button_width(&page.style, "Remove");
		pressed = kl_button(page.ui, &page.style, PAGE_ID_REMOVE_YES, &control, "Remove", KL_BUTTON_DANGER);
		if (pressed)
			page_known_remove();
		control.x += control.width + 8;
		control.width = kl_button_width(&page.style, "Keep");
		pressed = kl_button(page.ui, &page.style, PAGE_ID_REMOVE_NO, &control, "Keep", 0U);
		if (pressed) {
			page.remove_line = 0U;
			page.dirty = 1;
		}

		/* Moves down past the question; Reload starts at the left again. */
		y += confirm;
		control.x = card.x + 20;
	}

	/* Reload, for a file another program changed. */
	control.y = y + 6;
	control.width = kl_button_width(&page.style, "Reload");
	pressed = kl_button(page.ui, &page.style, PAGE_ID_KNOWN_RELOAD, &control, "Reload", 0U);
	if (pressed) {
		page_known_read();
		page_say(&page.known_message, 0, "known_hosts was read again.");
	}

	/* The last word under the buttons. */
	y = control.y + control.height + 6;
	(void)page_message_draw(y, card.x + 20, card.width - 40, &page.known_message);

	/* Reports the card's bottom. */
	return card.y + card.height;
}

/* Reads the program's settings into their fields. */
static void
page_items_read(void)
{
	char value[TERMINAL_ITEM_VALUE];
	unsigned index;
	int error;

	/* Each setting's value as the program keeps it. */
	for (index = 0U; index < page.program->item_count && index < TERMINAL_ITEMS_MAX; index++) {
		value[0] = '\0';
		error = page.program->item_get(page.program->data, page.program->items[index].keyword, value, sizeof(value));
		if (error != 0)
			value[0] = '\0';
		kl_field_set(&page.items[index], value);
	}

	/* Read. */
	page.items_read = 1;
}

/*
 * Keeps the program's settings of a pane that changed (each a field whose
 * text differs from what the program keeps); a value it refuses is named
 * with why, and the others are kept all the same.
 */
static void
page_items_save(
	unsigned pane)
{
	const struct terminal_page_item *item;
	char value[TERMINAL_ITEM_VALUE];
	char why[256];
	char text[PAGE_MESSAGE];
	unsigned index;
	unsigned saved;
	unsigned effects;
	int same;
	int error;

	/* Each setting of the pane whose field changed. */
	saved = 0U;
	effects = 0U;
	text[0] = '\0';
	for (index = 0U; index < page.program->item_count && index < TERMINAL_ITEMS_MAX; index++) {
		item = &page.program->items[index];
		if (item->pane != pane)
			continue;
		value[0] = '\0';
		error = page.program->item_get(page.program->data, item->keyword, value, sizeof(value));
		if (error != 0)
			value[0] = '\0';
		same = strcmp(value, page.items[index].text);
		if (same == 0)
			continue;

		/* Kept, or refused with why. */
		why[0] = '\0';
		error = page.program->item_set(page.program->data, item->keyword, page.items[index].text, why, sizeof(why));
		if (error != 0) {
			(void)snprintf(text, sizeof(text), "%s: %s", item->label, why);
			page_say(&page.items_message, 1, text);
			page_log("save-failed", item->keyword);
			return;
		}

		/* Counts the saved item and the effect it has. */
		saved++;
		effects |= 1U << item->effect;
		page_log("saved", item->keyword);

		/* The field shows the value as kept (the program may have written it out in full). */
		error = page.program->item_get(page.program->data, item->keyword, value, sizeof(value));
		if (error == 0)
			kl_field_set(&page.items[index], value);
	}

	/* What was kept and when it takes effect. */
	(void)snprintf(text, sizeof(text), "Saved. It takes effect from the next %s.", page_connection());
	if (saved == 0U) {
		page_say(&page.items_message, 0, "Nothing changed.");
	} else if ((effects & (1U << TERMINAL_EFFECT_START)) != 0U) {
		page_say(&page.items_message, 0, "Saved. A setting of the next start takes effect when the program starts again.");
	} else {
		page_say(&page.items_message, 0, text);
	}
}

/* Looks up the key named in the name field: its public key when it is there. */
static void
page_key_look(void)
{
	int valid;
	int error;

	/* The name looked up. */
	(void)snprintf(page.key_shown, sizeof(page.key_shown), "%s", page.key_name.text);
	page.key_looked = 1;
	page.key_found = 0;
	page.key_line[0] = '\0';
	page.key_fingerprint[0] = '\0';

	/* A name a key cannot have is not looked for. */
	valid = terminal_key_name_valid(page.key_shown);
	if (valid != 0)
		return;

	/* Its public key. */
	error = terminal_key_public(page.program, page.key_shown, page.key_line, sizeof(page.key_line), page.key_fingerprint, sizeof(page.key_fingerprint));
	if (error == 0) {
		page.key_found = 1;
		kl_text_area_set(&page.key_public_area, page.key_line);
	}
}

/* Makes the key named, when the passphrase was typed the same twice, and shows its public key. */
static void
page_key_make(void)
{
	char line[TERMINAL_KEY_LINE_MAX];
	char why[PAGE_MESSAGE];
	int same;
	int error;

	/* The passphrase twice the same. */
	same = strcmp(page.key_pass.text, page.key_again.text);
	if (same != 0) {
		page_say(&page.key_message, 1, "The two passphrases differ.");
		page_log("key-make", "differ");
		return;
	}

	/* The key's files. */
	error = terminal_key_make(page.program, page.key_name.text, page.key_comment.text, page.key_pass.text, line, sizeof(line), why, sizeof(why));
	if (error != 0) {
		page_say(&page.key_message, 1, why);
		page_log("key-make", "failed");
		return;
	}

	/* The passphrase typed goes, and the key made is shown. */
	memset(&page.key_pass, 0, sizeof(page.key_pass));
	memset(&page.key_again, 0, sizeof(page.key_again));
	page.key_pass.secret = 1;
	page.key_again.secret = 1;
	kl_field_set_limit(&page.key_pass, 1024U);
	kl_field_set_limit(&page.key_again, 1024U);
	kl_ui_clear_focus(page.ui);
	page.key_looked = 0;
	page_say(&page.key_message, 0, "The key was made. Copy its public key to the server's ~/.ssh/authorized_keys.");
	page_log("key-made", page.key_name.text);
}

/*
 * Keeps the private key the area holds under the name, wipes the area, and
 * shows the key's public key.  The key's text is never logged, and the
 * clipboard it was pasted from is overwritten (the run keeps the clipboard
 * when it is told to copy nothing, so one space takes the key's place).
 */
static void
page_key_save(
	struct terminal_window *window)
{
	char line[TERMINAL_KEY_LINE_MAX];
	char why[PAGE_MESSAGE];
	int error;

	/* The key, as the Keys pane keeps a key it made. */
	error = terminal_key_import(page.program, page.key_name.text, page.key_paste_area.text, page.key_paste_area.length, line, sizeof(line), why,
	    sizeof(why));
	if (error != 0) {
		page_say(&page.key_paste_message, 1, why);
		page_log("key-import", "failed");
		return;
	}

	/* The text goes from the area (a write the compiler keeps: the area is the page's own), and the key saved is shown. */
	memset(&page.key_paste_area, 0, sizeof(page.key_paste_area));
	kl_text_area_set(&page.key_paste_area, "");
	kl_ui_clear_focus(page.ui);
	page.key_looked = 0;
	page_say(&page.key_paste_message, 0, "");
	page_say(&page.key_message, 0, why);

	/* The clipboard the key came from. */
	page_set_aside(" ", 1U);
	page_action(window, TERMINAL_ACTION_COPY);
	page_log("key-imported", page.key_name.text);
}

/* Reads known_hosts and lists its lines. */
static void
page_known_read(void)
{
	char *text;
	size_t size;
	int error;

	/* None until it is read. */
	page.known_read = 1;
	page.known_count = 0U;
	page.known_total = 0U;
	page.remove_line = 0U;

	/* The file. */
	error = page.program->read(page.program->data, page.program->known_hosts_path, PAGE_KNOWN_MAX, &text, &size);
	if (error != 0)
		return;

	/* Its lines. */
	page.known_count = terminal_known_list(text, size, page.known, TERMINAL_KNOWN_MAX, &page.known_total);
	free(text);
}

/* Removes the line asked for from known_hosts, writing the file again. */
static void
page_known_remove(void)
{
	char message[PAGE_MESSAGE];
	char *text;
	char *rest;
	size_t size;
	size_t rest_size;
	int error;

	/* The file as it is now. */
	error = page.program->read(page.program->data, page.program->known_hosts_path, PAGE_KNOWN_MAX, &text, &size);
	if (error != 0) {
		page_say(&page.known_message, 1, "known_hosts could not be read.");
		return;
	}

	/* Without the line. */
	error = terminal_known_remove(text, size, page.remove_line, &rest, &rest_size);
	free(text);
	if (error != 0) {
		page_say(&page.known_message, 1, "known_hosts has no such line any more.");
		page_known_read();
		return;
	}

	/* Written whole. */
	error = page.program->replace(page.program->data, page.program->known_hosts_path, rest, rest_size);
	free(rest);
	if (error != 0) {
		(void)snprintf(message, sizeof(message), "%s could not be written (%s).", page.program->known_hosts_shown, strerror(error));
		page_say(&page.known_message, 1, message);
		return;
	}

	/* Succeeded: listed again. */
	(void)snprintf(message, sizeof(message), "Line %u was removed.", page.remove_line);
	page_known_read();
	page_say(&page.known_message, 0, message);
	page_log("known-removed", "");
}

/* Draws a card with its title and subtitle (either may be NULL); reports the top of its content. */
static int
page_card(
	const struct kl_rect *rect,
	const char *title,
	const char *subtitle)
{
	int top;

	/* Settings' card. */
	top = kl_card(&page.style, rect, title, subtitle);

	/* Reports where its rows start. */
	return top;
}

/*
 * Sets the label column of the pane about to be drawn for the labels of
 * its own rows and of the program's settings in it: the widest label and
 * the gap after it, within the least and the most.
 */
static void
page_column(
	unsigned pane,
	const char *const *labels,
	unsigned count)
{
	const struct terminal_page_item *item;
	unsigned index;
	int widest;
	int width;

	/* The widest of the pane's own labels. */
	widest = 0;
	for (index = 0U; index < count; index++) {
		width = kl_text_width(&page.text, labels[index], strlen(labels[index]), PAGE_TEXT_ROW, 0);

		/* A wider label widens the column. */
		if (width > widest)
			widest = width;
	}

	/* And of the program's settings the pane shows. */
	for (index = 0U; index < page.program->item_count; index++) {
		item = &page.program->items[index];

		/* A setting of another pane is not in this one. */
		if (item->pane != pane)
			continue;

		/* A wider label widens the column. */
		width = kl_text_width(&page.text, item->label, strlen(item->label), PAGE_TEXT_ROW, 0);
		if (width > widest)
			widest = width;
	}

	/* The column: the widest label and the gap, within the least and the most. */
	page.column = widest + PAGE_LABEL_GAP;
	if (page.column < PAGE_LABEL_MIN)
		page.column = PAGE_LABEL_MIN;
	if (page.column > PAGE_LABEL_MAX)
		page.column = PAGE_LABEL_MAX;
}

/* Reports where a row's control starts in a card: after the pane's label column, which a narrow card holds to a share of its row. */
static int
page_value_x(
	int left,
	int width)
{
	int column;
	int most;

	/* The pane's column, no more than its share of a narrow row. */
	column = page.column;
	most = (int)((float)(width - 2 * PAGE_INSET) * PAGE_LABEL_SHARE);
	if (column > most)
		column = most;

	/* Reports the control's edge. */
	return left + PAGE_INSET + column;
}

/*
 * Draws a row's label, quiet, in the pane's label column, with the line
 * under the row unless it is the last (kl_row's); reports where the row's
 * control starts.
 */
static int
page_label(
	int top,
	int left,
	int width,
	const char *label,
	int last)
{
	int baseline;
	int value_x;
	int room;

	/* The row's line, without kl_row's own columns of a label and a value. */
	(void)kl_row(&page.style, left, top, width, "", "", last);

	/* The label, fitted to its column. */
	value_x = page_value_x(left, width);
	room = value_x - left - PAGE_INSET - PAGE_AFTER_GAP;
	baseline = kl_text_center(PAGE_TEXT_ROW, top, PAGE_ROW);
	(void)kl_text_draw_fit(&page.text, &page.canvas, left + PAGE_INSET, baseline, label, PAGE_TEXT_ROW, 0, room, page.style.theme->text_secondary);

	/* Reports the control's edge. */
	return value_x;
}

/* Reports how wide a field starting at x in a card's row stands: to the row's right, at most PAGE_FIELD_MAX. */
static int
page_control_width(
	int left,
	int width,
	int x)
{
	int room;

	/* The room to the row's right, no more than the most. */
	room = left + width - PAGE_INSET - x;
	if (room > PAGE_FIELD_MAX)
		room = PAGE_FIELD_MAX;

	/* Reports the width. */
	return room;
}

/* Draws a row's words (a value, or what a switch does) from x, fitted to the right edge given. */
static void
page_row_text(
	int x,
	int top,
	int right,
	const char *text)
{
	int baseline;

	/* The words, plain, on the row's line. */
	baseline = kl_text_center(PAGE_TEXT_ROW, top, PAGE_ROW);
	(void)kl_text_draw_fit(&page.text, &page.canvas, x, baseline, text, PAGE_TEXT_ROW, 0, right - x, page.style.theme->text);
}

/* Draws a row of a label and a field; reports the edge below it. */
static int
page_field_row(
	int top,
	int left,
	int width,
	const char *label,
	uint32_t id,
	struct kl_field *field,
	const char *placeholder)
{
	const struct kl_theme *theme;
	struct kl_rect control;
	int value_x;

	/* The label in the pane's column. */
	theme = page.style.theme;
	value_x = page_label(top, left, width, label, 1);

	/* The field after it, no wider than the most. */
	control.x = value_x;
	control.y = top + (PAGE_ROW - theme->control_height) / 2;
	control.width = page_control_width(left, width, value_x);
	control.height = theme->control_height;
	(void)kl_field(page.ui, &page.style, id, &control, field, placeholder);

	/* Reports the edge below the row. */
	return top + PAGE_ROW;
}

/* Draws a note, wrapped to the width in lines of the theme's small text (at most PAGE_NOTE_LINES); reports the edge below it. */
static int
page_note(
	int top,
	int left,
	int width,
	const char *text,
	kl_color ink)
{
	const struct kl_theme *theme;
	size_t length;
	size_t fit;
	unsigned lines;
	int baseline;

	/* Each line as much as fits, broken where the text allows. */
	theme = page.style.theme;
	lines = 0U;
	while (*text != '\0' && lines < PAGE_NOTE_LINES) {
		length = strlen(text);
		fit = kl_text_break(&page.text, text, theme->text_small, 0, width);
		if (fit == 0U || fit > length)
			fit = length;
		baseline = kl_text_center(theme->text_small, top, PAGE_NOTE_LINE);
		(void)kl_text_draw(&page.text, &page.canvas, left, baseline, text, fit, theme->text_small, 0, ink);
		text += fit;
		while (*text == ' ')
			text++;
		top += PAGE_NOTE_LINE;
		lines++;
	}

	/* Reports the edge below it. */
	return top;
}

/* Measures how tall page_note draws a text (0 for an empty one). */
static int
page_note_height(
	int width,
	const char *text)
{
	const struct kl_theme *theme;
	size_t length;
	size_t fit;
	unsigned lines;

	/* The lines it is broken into. */
	theme = page.style.theme;
	lines = 0U;
	while (*text != '\0' && lines < PAGE_NOTE_LINES) {
		length = strlen(text);
		fit = kl_text_break(&page.text, text, theme->text_small, 0, width);
		if (fit == 0U || fit > length)
			fit = length;
		text += fit;
		while (*text == ' ')
			text++;
		lines++;
	}

	/* Reports their height. */
	return (int)lines * PAGE_NOTE_LINE;
}

/* Draws a pane's last word, in the bad ink for a failure; reports the edge below it. */
static int
page_message_draw(
	int top,
	int left,
	int width,
	const struct page_message *message)
{
	kl_color ink;
	int bottom;

	/* A failure stands out. */
	ink = page.style.theme->good;
	if (message->bad)
		ink = page.style.theme->bad;

	/* The text. */
	bottom = page_note(top, left, width, message->text, ink);

	/* Reports the edge below it. */
	return bottom;
}

/* Sets a pane's last word, and logs it for the tests. */
static void
page_say(
	struct page_message *message,
	int bad,
	const char *text)
{
	/* The word, and a frame that shows it. */
	(void)snprintf(message->text, sizeof(message->text), "%s", text);
	message->bad = bad;
	page.dirty = 1;
	printf("ZTERM SETTINGS message bad=%d text=%s\n", bad, text);
	fflush(stdout);
}

/* Queues an action for the run, as a menu's choice is (what takes effect at once). */
static void
page_action(
	struct terminal_window *window,
	uint32_t action)
{
	/* A full queue drops it. */
	if (window->action_count >= TERMINAL_ACTIONS)
		return;

	/* The run carries it out on its next round, and the page shows the state it leaves. */
	window->actions[window->action_count] = action;
	window->action_count++;
	page.dirty = 1;
}

/* Asks for the window's text input while a field that takes text has the keyboard, at its caret (only a change is told). */
static void
page_text_follow(
	struct terminal_window *window)
{
	struct kl_rect caret;
	int wanted;
	int width;
	int height;
	int x;
	int y;

	/* What the focused widget wants. */
	wanted = kl_ui_text_wanted(page.ui, &caret);

	/* The window's text input on or off, when that changed. */
	if (wanted != page.text_wanted) {
		kl_window_text_input(window->kui, wanted);
		page.text_wanted = wanted;
	}

	/* The candidates' place at the caret, in the window's pixels. */
	if (wanted) {
		x = page_pixels(caret.x);
		y = page_pixels(caret.y);
		width = page_pixels(caret.width);
		height = page_pixels(caret.height);
		kl_window_text_cursor(window->kui, x, y, width, height);
	}
}

/* Finds the field with the keyboard's focus; reports 1 with it, 0 for none. */
static int
page_focused(
	struct kl_field **field)
{
	unsigned index;
	int has;

	/* None yet. */
	*field = NULL;

	/* A field of the Hosts pane's form. */
	for (index = 0U; index < PAGE_HOST_FIELDS; index++) {
		has = kl_ui_has_focus(page.ui, PAGE_ID_HOST_FIELD + index, 0U);
		if (has)
			*field = &page.host_fields[index];
	}

	/* The Hosts pane's fields of a new host, identity file and forward. */
	has = kl_ui_has_focus(page.ui, PAGE_ID_HOST_NEW, 0U);
	if (has)
		*field = &page.host_new;
	has = kl_ui_has_focus(page.ui, PAGE_ID_IDENTITY_NEW, 0U);
	if (has)
		*field = &page.identity_new;
	has = kl_ui_has_focus(page.ui, PAGE_ID_FORWARD_LISTEN, 0U);
	if (has)
		*field = &page.forward_listen;
	has = kl_ui_has_focus(page.ui, PAGE_ID_FORWARD_TO, 0U);
	if (has)
		*field = &page.forward_to;

	/* A field of the Keys pane. */
	has = kl_ui_has_focus(page.ui, PAGE_ID_KEY_NAME, 0U);
	if (has)
		*field = &page.key_name;
	has = kl_ui_has_focus(page.ui, PAGE_ID_KEY_COMMENT, 0U);
	if (has)
		*field = &page.key_comment;
	has = kl_ui_has_focus(page.ui, PAGE_ID_KEY_PASS, 0U);
	if (has)
		*field = &page.key_pass;
	has = kl_ui_has_focus(page.ui, PAGE_ID_KEY_AGAIN, 0U);
	if (has)
		*field = &page.key_again;

	/* A field of the program's settings. */
	for (index = 0U; index < TERMINAL_ITEMS_MAX; index++) {
		has = kl_ui_has_focus(page.ui, PAGE_ID_ITEM + index, 0U);
		if (has)
			*field = &page.items[index];
	}

	/* None has it. */
	if (*field == NULL)
		return 0;

	/* Succeeded: the field. */
	return 1;
}

/* Finds the text area with the keyboard's focus (the key's pasted text, or its public key); reports 1 with it, 0 for none. */
static int
page_focused_area(
	struct kl_text_area **area)
{
	int has;

	/* None yet. */
	*area = NULL;

	/* The Keys pane's two areas. */
	has = kl_ui_has_focus(page.ui, PAGE_ID_KEY_PASTE_AREA, 0U);
	if (has)
		*area = &page.key_paste_area;
	has = kl_ui_has_focus(page.ui, PAGE_ID_KEY_PUBLIC_AREA, 0U);
	if (has)
		*area = &page.key_public_area;

	/* Reports whether one has it. */
	if (*area == NULL)
		return 0;
	return 1;
}

/* Gives a selection's ends in order. */
static void
page_selection(
	const char *text,
	size_t caret,
	size_t anchor,
	size_t *from,
	size_t *to)
{
	(void)text;

	/* The smaller end first. */
	*from = caret;
	*to = anchor;
	if (anchor < caret) {
		*from = anchor;
		*to = caret;
	}
}

/* Puts text in place of the focused widget's selection, as the input method commits it (in pieces that fit an event). */
static void
page_commit(
	const char *text,
	size_t length)
{
	struct kl_window_event event;
	size_t piece;

	/* An empty commit takes the selection out. */
	memset(&event, 0, sizeof(event));
	event.kind = KL_WINDOW_TEXT_COMMIT;
	if (length == 0U) {
		(void)kl_ui_text(page.ui, &event);
		return;
	}

	/* Each piece, never cutting a UTF-8 character in two. */
	while (length > 0U) {
		piece = length;
		if (piece > KL_WINDOW_TEXT_MAX - 1U) {
			piece = KL_WINDOW_TEXT_MAX - 1U;
			while (piece > 0U && ((unsigned char)text[piece] & 0xc0U) == 0x80U)
				piece--;
		}

		/* One piece of the text, as one commit. */
		memcpy(event.text, text, piece);
		event.text[piece] = '\0';
		(void)kl_ui_text(page.ui, &event);
		text += piece;
		length -= piece;
	}
}

/*
 * Answers Control with C, X or V as the run's Copy and Paste (a cut sets
 * the selection aside for the Copy, then takes it out), and Control with S
 * or R in the Hosts pane as Save or Revert; reports 1 when it took the
 * key.
 */
static int
page_key(
	struct terminal_window *window,
	const struct kl_window_event *event)
{
	struct kl_text_area *area;
	struct kl_field *field;
	char text[KL_FIELD_MAX];
	size_t length;
	unsigned held;
	int focused;

	/* Only a press with Control alone. */
	held = event->modifiers & (KL_MOD_CTRL | KL_MOD_ALT | KL_MOD_SHIFT | KL_MOD_SUPER);
	if (!event->pressed || held != KL_MOD_CTRL)
		return 0;

	/* In the Hosts pane, S saves and R reverts (wherever the keyboard is). */
	if (page.pane == TERMINAL_PANE_HOSTS && event->code == PAGE_KEY_S) {
		page_hosts_later(PAGE_HOSTS_SAVE, 0U);
		return 1;
	}

	/* R reverts. */
	if (page.pane == TERMINAL_PANE_HOSTS && event->code == PAGE_KEY_R) {
		page_hosts_later(PAGE_HOSTS_REVERT, 0U);
		return 1;
	}

	/* Otherwise C, X or V. */
	if (event->code != PAGE_KEY_C &&
	    event->code != PAGE_KEY_X &&
	    event->code != PAGE_KEY_V)
		return 0;

	/* A text area with the keyboard: Copy and Paste are the run's as for a field, and a cut takes the selection out of the area that may change (the key's pasted text). */
	focused = page_focused(&field);
	if (!focused) {
		focused = page_focused_area(&area);
		if (!focused)
			return 1;
		if (event->code == PAGE_KEY_V) {
			page_action(window, TERMINAL_ACTION_PASTE);
			return 1;
		}
		page_action(window, TERMINAL_ACTION_COPY);
		if (event->code == PAGE_KEY_X && area == &page.key_paste_area)
			page_commit("", 0U);
		return 1;
	}

	/* Copy and paste are the run's (it keeps the terminal's clipboard). */
	if (event->code == PAGE_KEY_C) {
		page_action(window, TERMINAL_ACTION_COPY);
		return 1;
	}

	/* Paste is the run's too. */
	if (event->code == PAGE_KEY_V) {
		page_action(window, TERMINAL_ACTION_PASTE);
		return 1;
	}

	/* A cut: the selection set aside for the Copy, then taken out (never a secret's). */
	if (field->secret)
		return 1;
	length = terminal_page_copy_text(text, sizeof(text));
	if (length == 0U)
		return 1;
	page_set_aside(text, length);
	page_action(window, TERMINAL_ACTION_COPY);
	page_commit("", 0U);

	/* Succeeded: the page took the key. */
	return 1;
}

/* Shows a pane from its top, with no field holding the keyboard. */
static void
page_pane(
	unsigned pane)
{
	/* The pane shown already. */
	if (pane == page.pane)
		return;

	/* The pane, its scroll at the top, and a frame of it. */
	page.pane = pane;
	kl_scroll_move_to(&page.scroll, 0.0, 0.0, 0, kl_clock_us());
	kl_ui_clear_focus(page.ui);
	page.dirty = 1;
	page_log("pane", page_pane_names[pane]);
}

/*
 * Puts the character a key types into the field with the keyboard, as the
 * input method commits it: the window system's text of a press (or a held
 * key's repeat) in the keyboard's own layout, without Control, Alt or the
 * logo key, when it is not a control character.  Reports 1 when it took
 * the key; a key without text (the window system leaves it empty, and its
 * input method sends the characters) and every key while no field has the
 * keyboard go to the widgets as keys.
 */
static int
page_key_text(
	const struct kl_window_event *event)
{
	struct kl_window_event commit;
	struct kl_text_area *area;
	struct kl_field *field;
	unsigned char first;
	int focused;

	/* A press with its text, without the modifiers that make shortcuts. */
	if (!event->pressed)
		return 0;
	if ((event->modifiers & (KL_MOD_CTRL | KL_MOD_ALT | KL_MOD_SUPER)) != 0U)
		return 0;
	first = (unsigned char)event->text[0];
	if (first < 0x20U || first == 0x7fU)
		return 0;

	/* Only a field or a text area takes text (Space presses a button). */
	focused = page_focused(&field);
	if (!focused)
		focused = page_focused_area(&area);
	if (!focused)
		return 0;

	/* The text, committed in place of the selection. */
	memset(&commit, 0, sizeof(commit));
	commit.kind = KL_WINDOW_TEXT_COMMIT;
	(void)snprintf(commit.text, sizeof(commit.text), "%s", event->text);
	(void)kl_ui_text(page.ui, &commit);

	/* Succeeded: the field took the character. */
	return 1;
}

/* Sets text aside for the next Copy (it takes it in place of a selection). */
static void
page_set_aside(
	const char *text,
	size_t length)
{
	/* As much as the room holds. */
	if (length > sizeof(page.copied))
		length = sizeof(page.copied);
	memcpy(page.copied, text, length);
	page.copied_length = length;
	page.copied_set = 1;
}

/* Writes a ZTERM SETTINGS line the tests read. */
static void
page_log(
	const char *what,
	const char *detail)
{
	/* The line. */
	printf("ZTERM SETTINGS %s %s\n", what, detail);
	fflush(stdout);
}

/* Names what a setting of the next connection waits for: the program's word, else "connection". */
static const char *
page_connection(void)
{
	/* The program's own word. */
	if (page.program->connection != NULL)
		return page.program->connection;

	/* A connection, the word of a program whose tabs connect somewhere. */
	return "connection";
}
