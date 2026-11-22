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
 *   Hosts       the ssh_config the program reads, edited and saved;
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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The widgets' ids (each widget's, or its first with an index). */
#define PAGE_ID_PANES		1U
#define PAGE_ID_SCROLL		2U
#define PAGE_ID_CONFIG		3U
#define PAGE_ID_CONFIG_SAVE	4U
#define PAGE_ID_CONFIG_REVERT	5U
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

/* The widest a known host's words are drawn, its removal's button after them. */
#define PAGE_KNOWN_LINE	480

/* The lowest the ssh_config's text area stands, and the most lines of a note. */
#define PAGE_AREA_MIN		220
#define PAGE_NOTE_LINES	4

/*
 * The evdev codes of the keys the page answers itself: C, V and X with
 * Control, and Up, Down, Page Up and Page Down with Control (the panes).
 */
#define PAGE_KEY_X		45U
#define PAGE_KEY_C		46U
#define PAGE_KEY_V		47U
#define PAGE_KEY_UP		103U
#define PAGE_KEY_PAGEUP	104U
#define PAGE_KEY_DOWN		108U
#define PAGE_KEY_PAGEDOWN	109U

/* The spaces a tab of the ssh_config is shown as (the text area draws no tab). */
#define PAGE_TAB_SPACES	4U

/* How many pixels a line of the page's scroll is, for the keys. */
#define PAGE_SCROLL_LINE	40.0

/* The largest ssh_config the Hosts pane edits (the text area's room) and the largest known_hosts read. */
#define PAGE_CONFIG_MAX	(KL_TEXT_AREA_MAX - 1U)
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

	/* Hosts: the ssh_config's text, whether it was read (and was too large to edit), and the last save's word. */
	struct kl_text_area config;
	int config_read;
	int config_large;
	int config_crlf;
	struct page_message config_message;

	/* The program's settings: their fields (read when items_read), and the last save's word. */
	struct kl_field items[TERMINAL_ITEMS_MAX];
	int items_read;
	struct page_message items_message;

	/* Keys: the fields of a new key, the key shown (its name, whether it is there, its line and fingerprint) and the last word. */
	struct kl_field key_name;
	struct kl_field key_comment;
	struct kl_field key_pass;
	struct kl_field key_again;
	char key_shown[TERMINAL_KEY_NAME_MAX];
	int key_looked;
	int key_found;
	char key_line[TERMINAL_KEY_LINE_MAX];
	char key_fingerprint[64];
	struct page_message key_message;

	/* The text the next Copy takes in place of the focused field's selection (a cut's, or the public key), when copied_set. */
	char copied[KL_TEXT_AREA_MAX];
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

/* The page (large: the text area and the known hosts; one for the run). */
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

static int page_frame(uint32_t width, uint32_t height, unsigned scale);
static double page_units(double pixels);
static int page_pixels(int units);
static void page_layout(struct kl_rect *sidebar, struct kl_rect *content);
static void page_sidebar(const struct kl_rect *sidebar);
static int page_hosts(struct terminal_window *window, int top, int left, int width);
static int page_appearance(struct terminal_window *window, const struct terminal_page_state *state, int top, int left, int width);
static int page_behavior(struct terminal_window *window, const struct terminal_page_state *state, int top, int left, int width);
static int page_keys(struct terminal_window *window, int top, int left, int width);
static int page_known(int top, int left, int width);
static int page_items(unsigned pane, int top, int left, int width);
static void page_items_read(void);
static void page_items_save(unsigned pane);
static void page_config_read(void);
static void page_config_save(void);
static int page_config_shown(const char *bytes, size_t size);
static void page_key_look(void);
static void page_key_make(void);
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
static int page_focused(struct kl_field **field, struct kl_text_area **area);
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
 * Pastes text into the focused field or text area (Edit > Paste, or
 * Control with V: the run receives the clipboard's text), as the input
 * method commits text.
 */
void
terminal_page_paste(
	const char *text,
	size_t length)
{
	struct kl_field *field;
	struct kl_text_area *area;
	int focused;

	/* Only into a field that has the keyboard. */
	focused = page_focused(&field, &area);
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
 * else the focused field's or text area's selection; a secret field's is
 * never given.  Returns its length (0: nothing to copy), the text cut to
 * size - 1 bytes and ended.
 */
size_t
terminal_page_copy_text(
	char *text,
	size_t size)
{
	struct kl_field *field;
	struct kl_text_area *area;
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

	/* Only from a field that has the keyboard. */
	focused = page_focused(&field, &area);
	if (!focused)
		return 0U;

	/* The field's selection, unless it is a secret, or the text area's. */
	if (field != NULL) {
		if (field->secret)
			return 0U;
		page_selection(field->text, field->caret, field->anchor, &from, &to);

		/* Copies the selection, cut at the buffer's size. */
		if (to - from > size - 1U)
			to = from + size - 1U;
		memcpy(text, field->text + from, to - from);
	} else {
		page_selection(area->text, area->caret, area->anchor, &from, &to);

		/* Copies the selection, cut at the buffer's size. */
		if (to - from > size - 1U)
			to = from + size - 1U;
		memcpy(text, area->text + from, to - from);
	}

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

	/* The canvas over it, drawn at the scale. */
	error = kl_canvas_init(&page.canvas, page.pixels, (size_t)width, (int)width, (int)height);
	if (error != 0)
		return ENOMEM;
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

/* Draws the Hosts pane: the ssh_config's text, Save and Revert, and what the last save found; reports its bottom. */
static int
page_hosts(
	struct terminal_window *window,
	int top,
	int left,
	int width)
{
	const struct kl_theme *theme;
	struct kl_rect card;
	struct kl_rect control;
	char subtitle[PAGE_MESSAGE];
	unsigned flags;
	int area_height;
	int pressed;
	int y;

	(void)window;

	/* The colours and sizes of the page's widgets. */
	theme = page.style.theme;

	/* The file, the first time. */
	if (!page.config_read)
		page_config_read();

	/* The card: as tall as the panel allows, with the area, the buttons, the note and the last word. */
	area_height = (int)page.viewport.height - 2 * PAGE_PAD - PAGE_CARD_SUBTITLED - theme->control_height - 3 * PAGE_NOTE_LINE - 40;
	if (area_height < PAGE_AREA_MIN)
		area_height = PAGE_AREA_MIN;
	card.x = left;
	card.y = top;
	card.width = width;
	card.height = PAGE_CARD_SUBTITLED + area_height + 12 + theme->control_height + 12 + page_note_height(width - 40, "Takes effect from the next connection.") +
	    page_note_height(width - 40, page.config_message.text) + PAGE_CARD_FOOT;
	(void)snprintf(subtitle, sizeof(subtitle), "%s", page.program->config_shown);
	y = page_card(&card, "Hosts (ssh_config)", subtitle);

	/* The text, or why it cannot be edited here; the area draws its placeholder on one line. */
	control.x = card.x + 20;
	control.y = y;
	control.width = card.width - 40;
	control.height = area_height;
	if (page.config_large) {
		(void)page_note(y, control.x, control.width, "The file is too large to edit here: edit it with a text editor.", theme->text_secondary);
	} else {
		(void)kl_text_area(page.ui, &page.style, PAGE_ID_CONFIG, &control, &page.config, "Host myserver, then its HostName, User and other lines");
	}

	/* Moves down past the area. */
	y += area_height + 12;

	/* Save and Revert. */
	control.y = y;
	control.height = theme->control_height;
	control.width = kl_button_width(&page.style, "Save");
	flags = KL_BUTTON_PRIMARY;
	if (page.config_large)
		flags |= KL_BUTTON_DISABLED;
	pressed = kl_button(page.ui, &page.style, PAGE_ID_CONFIG_SAVE, &control, "Save", flags);
	if (pressed)
		page_config_save();

	/* Revert, beside Save, reads the file again. */
	control.x += control.width + 8;
	control.width = kl_button_width(&page.style, "Revert");
	pressed = kl_button(page.ui, &page.style, PAGE_ID_CONFIG_REVERT, &control, "Revert", 0U);
	if (pressed) {
		page_config_read();
		page_say(&page.config_message, 0, "The file was read again.");
	}

	/* Moves down past the buttons. */
	y += control.height + 12;

	/* When it takes effect, and the last word. */
	y = page_note(y, card.x + 20, card.width - 40, "Takes effect from the next connection.", theme->text_secondary);
	y = page_message_draw(y, card.x + 20, card.width - 40, &page.config_message);

	/* Reports the pane's bottom. */
	return card.y + card.height;
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

	/* Larger beside Smaller, and the size in use after them. */
	control.x += control.width + 6;
	control.width = kl_button_width(&page.style, "Larger");
	pressed = kl_button(page.ui, &page.style, PAGE_ID_LARGER, &control, "Larger", 0U);
	if (pressed)
		page_action(window, TERMINAL_ACTION_ZOOM_IN);
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

	/* The ambiguous width: a switch, and what it does after it. */
	value_x = page_label(y, card.x, card.width, "Ambiguous width", 1);
	wide = state->ambiguous_wide;
	pressed = kl_switch(page.ui, &page.style, PAGE_ID_AMBIGUOUS, value_x, y + (PAGE_ROW - theme->switch_height) / 2, &wide, 0U);
	if (pressed)
		page_action(window, TERMINAL_ACTION_AMBIGUOUS_WIDE);
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

	/* Copy-on-select: a switch, and what it does after it. */
	value_x = page_label(y, card.x, card.width, "Copy on select", 1);
	on = state->copy_on_select;
	changed = kl_switch(page.ui, &page.style, PAGE_ID_COPY_SELECT, value_x, y + (PAGE_ROW - theme->switch_height) / 2, &on, 0U);
	if (changed)
		page_action(window, TERMINAL_ACTION_COPY_ON_SELECT);
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
			y = page_field_row(y, card.x, card.width, item->label, PAGE_ID_ITEM + index, &page.items[index], item->keyword);
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
	char subtitle[PAGE_MESSAGE];
	int pressed;
	int rows;
	int same;
	int y;

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
		card.height = PAGE_CARD_SUBTITLED + PAGE_ROW + page_note_height(width - 40, page.key_line) + page_note_height(width - 40, page.key_fingerprint) + 8 +
		    theme->control_height + 12;
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
		y = page_note(y + 4, card.x + 20, card.width - 40, page.key_line, theme->text);
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

	/* known_hosts under it. */
	y = page_known(card.y + card.height + PAGE_GAP, left, width);

	/* Reports the pane's bottom. */
	return y;
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

/* Reads the ssh_config into the text area (empty when it is not there). */
static void
page_config_read(void)
{
	char text[PAGE_MESSAGE];
	char *bytes;
	size_t size;
	int error;

	/* The file, as large as the area holds. */
	page.config_read = 1;
	page.config_large = 0;
	error = page.program->read(page.program->data, page.program->config_path, PAGE_CONFIG_MAX, &bytes, &size);
	if (error == ENOENT) {
		kl_text_area_set(&page.config, "");
		return;
	}

	/* A file too large for the area is not edited here. */
	if (error == EFBIG) {
		page.config_large = 1;
		return;
	}

	/* Any other failure is told, and the area is left empty. */
	if (error != 0) {
		kl_text_area_set(&page.config, "");
		(void)snprintf(text, sizeof(text), "%s could not be read (%s).", page.program->config_shown, strerror(error));
		page_say(&page.config_message, 1, text);
		return;
	}

	/* Its text in the area: its line breaks as LF (CR LF told apart for the save), its tabs as spaces. */
	error = page_config_shown(bytes, size);
	free(bytes);
	if (error != 0)
		page.config_large = 1;
}

/*
 * Puts an ssh_config's text in the area as the area shows it: each CR LF
 * as LF (config_crlf remembers it, and the save writes CR LF again) and
 * each tab as PAGE_TAB_SPACES spaces (ssh reads spaces as it reads tabs).
 * Returns 0, or EFBIG when the text does not fit the area.
 */
static int
page_config_shown(
	const char *bytes,
	size_t size)
{
	char *shown;
	size_t at;
	size_t index;
	unsigned space;

	/* The room of the area. */
	shown = malloc(KL_TEXT_AREA_MAX);
	if (shown == NULL)
		return ENOMEM;

	/* Each byte, a CR before an LF left out and a tab spread out. */
	at = 0U;
	page.config_crlf = 0;
	for (index = 0U; index < size; index++) {
		/* A CR before an LF is dropped, and the file is marked as one with CR LF. */
		if (bytes[index] == '\r' &&
		    index + 1U < size &&
		    bytes[index + 1U] == '\n') {
			page.config_crlf = 1;
			continue;
		}

		/* A text too long for the area is not edited here. */
		if (at + PAGE_TAB_SPACES >= KL_TEXT_AREA_MAX) {
			free(shown);
			return EFBIG;
		}

		/* A tab shows as spaces. */
		if (bytes[index] == '\t') {
			for (space = 0U; space < PAGE_TAB_SPACES; space++)
				shown[at++] = ' ';
			continue;
		}

		/* Any other byte as it is. */
		shown[at++] = bytes[index];
	}

	/* The text ends as a C string. */
	shown[at] = '\0';

	/* Succeeded: the area shows it. */
	kl_text_area_set(&page.config, shown);
	free(shown);
	return 0;
}

/*
 * Saves the text area as the ssh_config, then has the program check it:
 * what is wrong is shown (the file is kept all the same, as an editor
 * keeps a file), else that it takes effect from the next connection.
 */
static void
page_config_save(void)
{
	char report[PAGE_MESSAGE];
	char text[PAGE_MESSAGE];
	char *bytes;
	size_t size;
	size_t index;
	int error;

	/* The text, its line breaks CR LF again when the file had them. */
	bytes = malloc(2U * page.config.length + 1U);
	if (bytes == NULL) {
		page_say(&page.config_message, 1, "There is no memory to save the file.");
		return;
	}

	/* Each LF becomes CR LF again when the file had them. */
	size = 0U;
	for (index = 0U; index < page.config.length; index++) {
		if (page.config.text[index] == '\n' && page.config_crlf)
			bytes[size++] = '\r';
		bytes[size++] = page.config.text[index];
	}

	/* The file, written whole. */
	error = page.program->replace(page.program->data, page.program->config_path, bytes, size);
	free(bytes);
	if (error != 0) {
		(void)snprintf(text, sizeof(text), "%s could not be written (%s).", page.program->config_shown, strerror(error));
		page_say(&page.config_message, 1, text);
		page_log("config", "save-failed");
		return;
	}

	/* What the program finds wrong in it. */
	report[0] = '\0';
	if (page.program->check != NULL)
		(void)page.program->check(page.program->data, page.config.text, page.config.length, report, sizeof(report));

	/* A problem the program found is told; the file stays saved. */
	if (report[0] != '\0') {
		(void)snprintf(text, sizeof(text), "Saved, but: %s", report);
		page_say(&page.config_message, 1, text);
		page_log("config", "saved-problems");
		return;
	}

	/* Succeeded. */
	page_say(&page.config_message, 0, "Saved. It takes effect from the next connection.");
	page_log("config", "saved");
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
	if (error == 0)
		page.key_found = 1;
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

/* Finds the field or the text area with the keyboard's focus; reports 1 with one of them, 0 for none. */
static int
page_focused(
	struct kl_field **field,
	struct kl_text_area **area)
{
	unsigned index;
	int has;

	/* None yet. */
	*field = NULL;
	*area = NULL;

	/* The ssh_config's text. */
	has = kl_ui_has_focus(page.ui, PAGE_ID_CONFIG, 0U);
	if (has) {
		*area = &page.config;
		return 1;
	}

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
 * the selection aside for the Copy, then takes it out); reports 1 when it
 * took the key.
 */
static int
page_key(
	struct terminal_window *window,
	const struct kl_window_event *event)
{
	struct kl_field *field;
	struct kl_text_area *area;
	char text[KL_TEXT_AREA_MAX];
	size_t length;
	unsigned held;
	int focused;

	/* Only a press with Control alone, of C, X or V. */
	held = event->modifiers & (KL_MOD_CTRL | KL_MOD_ALT | KL_MOD_SHIFT | KL_MOD_SUPER);
	if (!event->pressed || held != KL_MOD_CTRL)
		return 0;
	if (event->code != PAGE_KEY_C &&
	    event->code != PAGE_KEY_X &&
	    event->code != PAGE_KEY_V)
		return 0;

	/* Without a field with the keyboard the key is no one's. */
	focused = page_focused(&field, &area);
	if (!focused)
		return 1;

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
	if (field != NULL && field->secret)
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
	struct kl_field *field;
	struct kl_text_area *area;
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

	/* Only a field takes text (Space presses a button). */
	focused = page_focused(&field, &area);
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
