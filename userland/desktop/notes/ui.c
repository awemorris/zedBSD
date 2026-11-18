/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The toolbar of Notes: a card of frosted glass floating at the top of the
 * window, drawn on the CPU into the picture the renderer shows over the
 * desk (design-input-notes.md section 5.2).
 *
 * It follows the look of the File Manager and the titlebar (the desktop's look,
 * plan/ws035/kei-identity-design.md): a translucent white card with a light
 * rim and a soft slate shadow, slate labels, and the desktop's blue for what is
 * chosen -- the tool and the width on a pale blue pill in blue, as the
 * File Manager marks its chosen place, and the colour with a blue ring.  The picture is B8G8R8A8 with straight
 * alpha: transparent around the card, so the desk shows there, and
 * translucent in the card, so the desk's colours show through the glass.
 *
 * ws216-p006: the pen and the marker are one button, the drawing tool
 * used last (Pen, Brush, Pencil, Water, Marker or Fill); pressed while a
 * drawing tool is chosen, it opens a strip of the six under the card, in
 * the rows of the notice (which waits while the strip is open).
 *
 * From the left the card holds the tools (Pen, Marker, Eraser), the
 * switch that lets one finger write (Finger, in blue while it is on), five
 * colours, three widths, Undo and Redo, the page's number between the
 * previous and next page buttons, a new page, and Save.  ws175-p008: the
 * tools go on with Select and Text; Select shows what it does to the PDF's
 * objects in place of the colours and the widths (and to a line of text:
 * Edit, its font, its size), and Text the font and the size of the words
 * it puts on the page before the colours.  The card is laid
 * out once to measure it and drawn centred; its width does not depend on
 * the status, so the buttons stay in place.  A status shows in a small
 * pill of its own beside the card when there is room, or else as a notice
 * centred under the card, in the rows of the picture below the toolbar's
 * band (NOTES_TOOLBAR_IMAGE_HEIGHT).  The labels are
 * drawn with libtruetype from the desktop's font; without the font the
 * buttons are drawn without them and still work.  Only ASCII is drawn (the
 * labels are ASCII; other characters of a status show as '?').
 */

#include "app.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <truetype/truetype.h>
#include <unistd.h>

/* The labels' size in pixels. */
#define UI_FONT_PIXELS		15U

/* The largest font file read, in bytes. */
#define UI_FONT_MAX		(32U * 1024U * 1024U)

/* The largest glyph drawn, in pixels a side. */
#define UI_GLYPH_MAX		64U

/* The glass card: its top, height and corner radius, and the room inside its ends, in pixels. */
#define UI_CARD_TOP		8
#define UI_CARD_HEIGHT		52
#define UI_CARD_RADIUS		18.0f
#define UI_CARD_PADDING		10

/* The card's shadow: how far it spreads, how far it drops, and its darkest alpha. */
#define UI_SHADOW_SOFT		12.0f
#define UI_SHADOW_DROP		4.0f
#define UI_SHADOW_ALPHA		52U

/* The buttons' height and top, and the gap between groups, in pixels. */
#define UI_BUTTON_HEIGHT	36
#define UI_BUTTON_TOP		(UI_CARD_TOP + (UI_CARD_HEIGHT - UI_BUTTON_HEIGHT) / 2)
#define UI_GAP			14

/* The strip of drawing tools (ws216-p006): its top (2 pixels under the toolbar's band), height and the room inside its ends, in pixels. */
#define UI_STRIP_TOP		((int32_t)NOTES_TOOLBAR_HEIGHT + 2)
#define UI_STRIP_HEIGHT		40
#define UI_STRIP_PADDING	4

/* The drawing tools, in the strip's order, with their labels. */
#define UI_DRAW_TOOLS		6U

/* The room the drawing tool button keeps at its end for the mark that it opens the strip. */
#define UI_MARK_ROOM		14

/* The status pill's height, the room around its text, and the gap above it when it shows under the card, in pixels. */
#define UI_STATUS_HEIGHT	32
#define UI_STATUS_PADDING	14
#define UI_NOTICE_GAP		6

/*
 * The desktop's colours, as 0xRRGGBB, with the alphas they are laid on with:
 * the glass's white veil, its rim and its slate edge, the slate text in
 * three strengths, the accent the user chose (ws179-p002) as a mark and as
 * text and its pale tint, and the shadow's slate
 * (the same values as the File Manager's).  The veil, its rim and the
 * text have the dark appearance's colours too (ws089-p017).
 */
#define UI_WHITE		kl_theme_choose(0xffffffU, 0x23272fU)
#define UI_RIM			kl_theme_choose(0xffffffU, 0x3a404bU)
#define UI_GLASS_ALPHA		210U
#define UI_RIM_ALPHA		235U
#define UI_SLATE_EDGE		0x1f3a66U
#define UI_EDGE_ALPHA		34U
#define UI_TEXT			kl_theme_choose(0x1e2632U, 0xe9edf3U)
#define UI_TEXT_SECONDARY	kl_theme_choose(0x6b7585U, 0xa9b2bfU)
#define UI_TEXT_DISABLED	kl_theme_choose(0xa3abb8U, 0x646d7aU)
#define UI_ACCENT		(kl_theme_default()->accent & 0xffffffU)
#define UI_ACCENT_TEXT		(kl_theme_default()->accent_text & 0xffffffU)
#define UI_ACCENT_TINT_ALPHA	52U
#define UI_SEPARATOR_ALPHA	40U

static int32_t ui_layout(struct notes_ui *ui, int32_t x, const struct notes_ui_state *state);
static int32_t ui_layout_rest(struct notes_ui *ui, int32_t x, int32_t left, const struct notes_ui_state *state);
static void ui_clear(struct notes_ui *ui);
static void ui_over(struct notes_ui *ui, int32_t x, int32_t y, uint32_t rgb, unsigned alpha);
static float ui_rounded_distance(float px, float py, float x, float y, float width, float height, float radius);
static void ui_rounded(struct notes_ui *ui, float x, float y, float width, float height, float radius, uint32_t rgb, unsigned alpha);
static void ui_rounded_edge(struct notes_ui *ui, float x, float y, float width, float height, float radius, uint32_t rgb, unsigned alpha);
static void ui_shadow(struct notes_ui *ui, float x, float y, float width, float height, float radius);
static void ui_glass(struct notes_ui *ui, float x, float y, float width, float height, float radius);
static void ui_dot(struct notes_ui *ui, float cx, float cy, float radius, uint32_t rgb, unsigned alpha);
static void ui_ring(struct notes_ui *ui, float cx, float cy, float radius, float thickness, uint32_t rgb);
static void ui_separator(struct notes_ui *ui, int32_t x);
static int32_t ui_text_width(struct notes_ui *ui, const char *text);
static void ui_text(struct notes_ui *ui, int32_t x, int32_t y, const char *text, uint32_t rgb);
static int32_t ui_label_button(struct notes_ui *ui, int32_t x, const char *label, uint32_t action, uint32_t chosen, int enabled);
static int32_t ui_label_button_at(struct notes_ui *ui, int32_t x, int32_t y, const char *label, uint32_t action, uint32_t chosen, int enabled);
static int32_t ui_draw_button(struct notes_ui *ui, int32_t x, const struct notes_ui_state *state);
static void ui_strip(struct notes_ui *ui, const struct notes_ui_state *state);
static const char *ui_draw_label(unsigned action);
static float ui_width_radius(unsigned kind, unsigned index);
static void ui_down_mark(struct notes_ui *ui, float cx, float cy, uint32_t rgb);
static int32_t ui_eraser_button(struct notes_ui *ui, int32_t x, const struct notes_ui_state *state);
static int32_t ui_text_controls(struct notes_ui *ui, int32_t x, const struct notes_ui_state *state);
static int32_t ui_eraser_slack(struct notes_ui *ui, const struct notes_ui_state *state);
static void ui_add_button(struct notes_ui *ui, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t action);
static uint32_t ui_codepoint(char byte);

/* The pen's colours as 0xRRGGBBAA: black, blue, red, green, orange. */
static const uint32_t ui_pen_colors[NOTES_COLORS] = {
	0x1f1f1fffU, 0x1d4ed8ffU, 0xdc2626ffU, 0x15803dffU, 0xea580cffU
};

/* The highlighter's colours, translucent: yellow, blue, pink, green, orange. */
static const uint32_t ui_marker_colors[NOTES_COLORS] = {
	0xfacc1566U, 0x60a5fa66U, 0xf472b666U, 0x4ade8066U, 0xfb923c66U
};

/* The pen's widths in points: fine, medium, bold. */
static const float ui_pen_widths[NOTES_WIDTHS] = { 1.5f, 3.0f, 6.0f };

/* The highlighter's widths in points: fine, medium, bold. */
static const float ui_marker_widths[NOTES_WIDTHS] = { 10.0f, 16.0f, 24.0f };

/* The brush's, the pencil's and the watercolour's widths in points (ws216-p006, plan/ws216/phase001/phase.md section 7). */
static const float ui_brush_widths[NOTES_WIDTHS] = { 3.0f, 6.0f, 12.0f };
static const float ui_pencil_widths[NOTES_WIDTHS] = { 0.8f, 1.4f, 2.4f };
static const float ui_water_widths[NOTES_WIDTHS] = { 8.0f, 16.0f, 28.0f };

/* The drawing tools' actions in the strip's order (ws216-p006). */
static const unsigned ui_draw_actions[UI_DRAW_TOOLS] = {
	NOTES_ACTION_PEN, NOTES_ACTION_BRUSH, NOTES_ACTION_PENCIL, NOTES_ACTION_WATERCOLOR, NOTES_ACTION_HIGHLIGHTER, NOTES_ACTION_FILL
};

/*
 * Opens the font the labels are drawn with.
 *
 * Returns 0, or an errno value; the toolbar works without its font, only
 * without labels.
 */
int
notes_ui_open(
	struct notes_ui *ui,
	const char *font_path)
{
	struct truetype_metrics metrics;
	struct stat status;
	ssize_t got;
	size_t done;
	int descriptor;
	int error;

	/* Nothing is open yet. */
	memset(ui, 0, sizeof(*ui));

	/* The font file, which must be of a sane size. */
	descriptor = open(font_path, O_RDONLY);
	if (descriptor < 0)
		return errno;
	error = fstat(descriptor, &status);
	if (error != 0 ||
	    status.st_size <= 0 ||
	    (unsigned long)status.st_size > UI_FONT_MAX) {
		(void)close(descriptor);
		return EINVAL;
	}

	/* Reads it whole; the face uses the bytes for as long as it is open. */
	ui->font_data = malloc((size_t)status.st_size);
	if (ui->font_data == NULL) {
		(void)close(descriptor);
		return ENOMEM;
	}

	/* Reads it all. */
	done = 0;
	while (done < (size_t)status.st_size) {
		got = read(descriptor, (unsigned char *)ui->font_data + done, (size_t)status.st_size - done);
		if (got <= 0)
			break;
		done += (size_t)got;
	}

	/* The file is not needed any more. */
	(void)close(descriptor);
	ui->font_size = done;

	/* The face at the labels' size. */
	error = truetype_open(ui->font_data, ui->font_size, 0U, &ui->face);
	if (error != 0) {
		notes_ui_close(ui);
		return error;
	}

	/* Its companions: Mahora Bold, and the monospaced fallback for the signs it lacks (ws090-p020). */
	(void)truetype_open_companions(ui->face, KEILAND_FONT_BOLD, KEILAND_FONT_FALLBACK_MONO);

	/* The labels' size. */
	error = truetype_set_pixel_size(ui->face, UI_FONT_PIXELS);
	if (error != 0) {
		notes_ui_close(ui);
		return error;
	}

	/* The ascent, which places the baseline in a button. */
	error = truetype_metrics(ui->face, &metrics);
	if (error != 0) {
		notes_ui_close(ui);
		return error;
	}

	/* Succeeded: labels can be drawn. */
	ui->font_pixels = UI_FONT_PIXELS;
	ui->ascent = metrics.ascent;
	return 0;
}

/*
 * Closes the font.
 */
void
notes_ui_close(
	struct notes_ui *ui)
{
	/* The face before the bytes it reads. */
	if (ui->face != NULL)
		truetype_close(ui->face);
	free(ui->font_data);
	memset(ui, 0, sizeof(*ui));
}

/*
 * Draws the toolbar into a picture (B8G8R8A8 rows, straight alpha) and lays
 * out its buttons.
 */
void
notes_ui_draw(
	struct notes_ui *ui,
	unsigned char *pixels,
	size_t pitch,
	uint32_t width,
	uint32_t height,
	const struct notes_ui_state *state)
{
	int32_t content_width;
	int32_t card_x;
	int32_t card_width;
	int32_t status_width;
	int32_t status_x;
	int32_t status_y;

	/* The picture being drawn, and no buttons yet, nor the strip. */
	ui->pixels = pixels;
	ui->pitch = pitch;
	ui->width = width;
	ui->height = height;
	ui->button_count = 0;
	ui->strip_shown = 0;
	if (pixels == NULL)
		return;

	/* Measures the card's content by laying it out without drawing. */
	ui->drawing = 0;
	content_width = ui_layout(ui, 0, state);

	/* The card, centred, or from the left edge when the window is narrower. */
	card_width = content_width + 2 * UI_CARD_PADDING;
	card_x = ((int32_t)width - card_width) / 2;
	if (card_x < 8)
		card_x = 8;

	/* The picture starts transparent: the desk shows around the card. */
	ui->drawing = 1;
	ui_clear(ui);

	/* The card's shadow, its glass and its rim. */
	ui_shadow(ui, (float)card_x, (float)UI_CARD_TOP, (float)card_width, (float)UI_CARD_HEIGHT, UI_CARD_RADIUS);
	ui_glass(ui, (float)card_x, (float)UI_CARD_TOP, (float)card_width, (float)UI_CARD_HEIGHT, UI_CARD_RADIUS);

	/* The buttons on the card, laid out again where they are drawn. */
	ui->button_count = 0;
	(void)ui_layout(ui, card_x + UI_CARD_PADDING, state);

	/* The strip of drawing tools under it, when open (ws216-p006). */
	if (state->draws_open)
		ui_strip(ui, state);

	/* A status shows in its own pill: right of the card when it fits, or left of it. */
	if (state->status == NULL || state->status[0] == '\0')
		return;
	status_width = ui_text_width(ui, state->status) + 2 * UI_STATUS_PADDING;
	status_y = UI_CARD_TOP + (UI_CARD_HEIGHT - UI_STATUS_HEIGHT) / 2;
	status_x = card_x + card_width + UI_GAP;
	if (status_x + status_width + 8 > (int32_t)width)
		status_x = card_x - UI_GAP - status_width;

	/* A status that fits on neither side shows as a notice, centred under the card, when the picture reaches there and the strip does not hold its rows (review-1 L7). */
	if (status_x < 8) {
		if (ui->strip_shown)
			return;
		status_x = ((int32_t)width - status_width) / 2;
		status_y = UI_CARD_TOP + UI_CARD_HEIGHT + UI_NOTICE_GAP;
	}

	/* A notice wider than the window, or below the picture, is not shown. */
	if (status_x < 8)
		return;
	if (status_y + UI_STATUS_HEIGHT > (int32_t)height)
		return;

	/* The pill and its text. */
	ui_shadow(ui, (float)status_x, (float)status_y, (float)status_width, (float)UI_STATUS_HEIGHT, (float)UI_STATUS_HEIGHT / 2.0f);
	ui_glass(ui, (float)status_x, (float)status_y, (float)status_width, (float)UI_STATUS_HEIGHT, (float)UI_STATUS_HEIGHT / 2.0f);
	ui_text(ui, status_x + UI_STATUS_PADDING, status_y - (UI_BUTTON_HEIGHT - UI_STATUS_HEIGHT) / 2, state->status, UI_TEXT_SECONDARY);
}

/*
 * Tells the action of the button at a point of the toolbar (NOTES_ACTION_NONE for none).
 */
uint32_t
notes_ui_hit(
	const struct notes_ui *ui,
	float x,
	float y)
{
	const struct notes_button *button;
	unsigned index;

	/* The first button whose rectangle holds the point. */
	for (index = 0; index < ui->button_count; index++) {
		button = &ui->buttons[index];
		if (x < (float)button->x || x >= (float)(button->x + button->width))
			continue;
		if (y < (float)button->y || y >= (float)(button->y + button->height))
			continue;
		return button->action;
	}

	/* No button there. */
	return NOTES_ACTION_NONE;
}

/*
 * Gives a tool's colour of an index, as 0xRRGGBBAA.
 */
uint32_t
notes_ui_color(
	unsigned tool,
	unsigned index)
{
	/* An index past the palette is its first colour. */
	if (index >= NOTES_COLORS)
		index = 0;

	/* The highlighter's translucent colours. */
	if (tool == NOTES_TOOL_HIGHLIGHTER)
		return ui_marker_colors[index];

	/* The pen's. */
	return ui_pen_colors[index];
}

/*
 * Gives a tool's width of an index, in points.
 */
float
notes_ui_width(
	unsigned tool,
	unsigned index)
{
	/* An index past the widths is the middle one. */
	if (index >= NOTES_WIDTHS)
		index = 1;

	/* Each tool's own (ws216-p006); the fill has none and takes the pen's. */
	switch (tool) {
	case NOTES_TOOL_HIGHLIGHTER:
		return ui_marker_widths[index];
	case NOTES_TOOL_BRUSH:
		return ui_brush_widths[index];
	case NOTES_TOOL_PENCIL:
		return ui_pencil_widths[index];
	case NOTES_TOOL_WATERCOLOR:
		return ui_water_widths[index];
	default:
		break;
	}

	/* The pen's. */
	return ui_pen_widths[index];
}

/*
 * Tells whether a point of the window is on the strip of drawing tools as
 * last drawn (ws216-p006).
 */
int
notes_ui_in_strip(
	const struct notes_ui *ui,
	float x,
	float y)
{
	/* No strip, no point on it. */
	if (!ui->strip_shown)
		return 0;

	/* Outside its rectangle. */
	if (x < (float)ui->strip_x || x >= (float)(ui->strip_x + ui->strip_width))
		return 0;
	if (y < (float)ui->strip_y || y >= (float)(ui->strip_y + ui->strip_height))
		return 0;

	/* On it. */
	return 1;
}

/*
 * Gives the kind of stroke (NOTES_TOOL_*) a drawing tool's action draws
 * (ws216-p006), or NOTES_UI_NOT_DRAWING for another action.
 */
unsigned
notes_ui_tool(
	unsigned action)
{
	/* Each drawing tool. */
	switch (action) {
	case NOTES_ACTION_PEN:
		return NOTES_TOOL_PEN;
	case NOTES_ACTION_HIGHLIGHTER:
		return NOTES_TOOL_HIGHLIGHTER;
	case NOTES_ACTION_BRUSH:
		return NOTES_TOOL_BRUSH;
	case NOTES_ACTION_PENCIL:
		return NOTES_TOOL_PENCIL;
	case NOTES_ACTION_WATERCOLOR:
		return NOTES_TOOL_WATERCOLOR;
	case NOTES_ACTION_FILL:
		return NOTES_TOOL_FILL;
	default:
		break;
	}

	/* Not a drawing tool. */
	return NOTES_UI_NOT_DRAWING;
}

/*
 * Carries out what an action does to the tools (ws216-p006), and tells
 * what changed (NOTES_TOOLS_*).
 *
 * The drawing tool button goes back to the drawing tool used last while
 * another tool is chosen, and opens or closes the strip while a drawing
 * tool is.  A drawing tool, the eraser, Select and Text are chosen (a
 * drawing tool is the one used last from then on); a colour chosen while
 * the eraser is goes back to the drawing tool used last.  Any other action
 * closes the strip.
 */
unsigned
notes_ui_tools_press(
	struct notes_tools *tools,
	unsigned action)
{
	unsigned changes;
	unsigned kind;

	/* The drawing tool button. */
	changes = 0;
	if (action == NOTES_ACTION_DRAW) {
		/* Another tool: back to the drawing tool used last, the strip closed. */
		kind = notes_ui_tool(tools->tool);
		if (kind == NOTES_UI_NOT_DRAWING) {
			tools->tool = tools->draw_tool;
			if (tools->draws_open)
				changes |= NOTES_TOOLS_CLOSED;
			tools->draws_open = 0;
			changes |= NOTES_TOOLS_CHOSEN;
			return changes;
		}

		/* A drawing tool: the strip opens or closes. */
		tools->draws_open = !tools->draws_open;
		if (tools->draws_open)
			return NOTES_TOOLS_OPENED;
		return NOTES_TOOLS_CLOSED;
	}

	/* Any other action closes the strip. */
	if (tools->draws_open) {
		tools->draws_open = 0;
		changes |= NOTES_TOOLS_CLOSED;
	}

	/* A drawing tool is chosen, and is the one used last from now on. */
	kind = notes_ui_tool(action);
	if (kind != NOTES_UI_NOT_DRAWING) {
		tools->tool = action;
		tools->draw_tool = action;
		changes |= NOTES_TOOLS_CHOSEN;
		return changes;
	}

	/* The other tools are chosen. */
	if (action == NOTES_ACTION_ERASER || action == NOTES_ACTION_SELECT || action == NOTES_ACTION_TEXT) {
		tools->tool = action;
		changes |= NOTES_TOOLS_CHOSEN;
		return changes;
	}

	/* A colour chosen with the eraser goes back to the drawing tool used last (review-1 L-1). */
	if (action >= NOTES_ACTION_COLOR && action < NOTES_ACTION_COLOR + NOTES_COLORS && tools->tool == NOTES_ACTION_ERASER)
		tools->tool = tools->draw_tool;

	/* Reports the changes. */
	return changes;
}

/*
 * Lays out (and, while ui->drawing is set, draws) the card's buttons from a
 * left edge, adds them, and returns the width they take.
 */
static int32_t
ui_layout(
	struct notes_ui *ui,
	int32_t left,
	const struct notes_ui_state *state)
{
	const uint32_t *colors;
	int32_t x;
	int32_t slack;
	uint32_t chosen;
	unsigned kind;
	unsigned index;
	float radius;
	float cx;
	float cy;

	/* The tools, the chosen one in the desktop's blue: the drawing tool, the eraser, Select and Text. */
	x = left;
	x = ui_draw_button(ui, x, state);
	x = ui_eraser_button(ui, x, state);
	x = ui_label_button(ui, x, "Select", NOTES_ACTION_SELECT, state->tool, 1);
	x = ui_label_button(ui, x, "Text", NOTES_ACTION_TEXT, state->tool, 1);

	/*
	 * The eraser's place keeps room for its longer label, so the buttons
	 * after it stay put when its mode changes.  While it says "Eraser",
	 * that room widens the gap after the tools, with the separator in its
	 * middle, instead of the gap before the label (ws035-p122).
	 */
	slack = ui_eraser_slack(ui, state);
	ui_separator(ui, x + (UI_GAP + slack) / 2 - 1);
	x += UI_GAP + slack;

	/* Writing with a finger, on a pale blue pill in blue while it is on (ws081-p015). */
	chosen = NOTES_ACTION_NONE;
	if (state->finger_write)
		chosen = NOTES_ACTION_FINGER;
	x = ui_label_button(ui, x, "Finger", NOTES_ACTION_FINGER, chosen, 1);
	ui_separator(ui, x + UI_GAP / 2 - 1);
	x += UI_GAP;

	/*
	 * With the Select tool and a line of text chosen (ws175-p008): its
	 * words edited in the box, its font and size, deleted or put back.
	 */
	if (state->tool == NOTES_ACTION_SELECT && state->text_selected) {
		x = ui_label_button(ui, x, "Edit", NOTES_ACTION_EDIT_TEXT, NOTES_ACTION_NONE, state->can_edit_text);
		x = ui_text_controls(ui, x, state);
		x = ui_label_button(ui, x, "Delete", NOTES_ACTION_DELETE_OBJECT, NOTES_ACTION_NONE, state->selected);
		x = ui_label_button(ui, x, "Reset", NOTES_ACTION_RESET_OBJECT, NOTES_ACTION_NONE, state->can_reset);
		ui_separator(ui, x + UI_GAP / 2 - 1);
		x += UI_GAP;
		return ui_layout_rest(ui, x, left, state);
	}

	/*
	 * With the Select tool (ws175-p008), what it does in place of the
	 * colours and the widths: an image inserted, and the chosen object's
	 * image replaced, deleted or put back as the page has it.
	 */
	if (state->tool == NOTES_ACTION_SELECT) {
		x = ui_label_button(ui, x, "Image", NOTES_ACTION_INSERT_IMAGE, NOTES_ACTION_NONE, state->can_insert);
		x = ui_label_button(ui, x, "Replace", NOTES_ACTION_REPLACE_IMAGE, NOTES_ACTION_NONE, state->can_replace);
		x = ui_label_button(ui, x, "Delete", NOTES_ACTION_DELETE_OBJECT, NOTES_ACTION_NONE, state->selected);
		x = ui_label_button(ui, x, "Reset", NOTES_ACTION_RESET_OBJECT, NOTES_ACTION_NONE, state->can_reset);
		ui_separator(ui, x + UI_GAP / 2 - 1);
		x += UI_GAP;
		return ui_layout_rest(ui, x, left, state);
	}

	/* With the Text tool (ws175-p008): the font and the size of the words put on the page, then the pen's colours. */
	if (state->tool == NOTES_ACTION_TEXT) {
		x = ui_text_controls(ui, x, state);
		ui_separator(ui, x + UI_GAP / 2 - 1);
		x += UI_GAP;
	}

	/*
	 * The colours and widths of the drawing tool chosen, or of the one used
	 * last while another tool is (the Text tool's colours are the pen's).
	 */
	kind = notes_ui_tool(state->tool);
	if (kind == NOTES_UI_NOT_DRAWING)
		kind = notes_ui_tool(state->draw_tool);
	if (state->tool == NOTES_ACTION_TEXT || kind == NOTES_UI_NOT_DRAWING)
		kind = NOTES_TOOL_PEN;
	colors = ui_pen_colors;
	if (kind == NOTES_TOOL_HIGHLIGHTER)
		colors = ui_marker_colors;

	/* Each colour, a dot; the chosen one has a blue ring. */
	for (index = 0; index < NOTES_COLORS; index++) {
		cx = (float)x + (float)UI_BUTTON_HEIGHT / 2.0f;
		cy = (float)UI_BUTTON_TOP + (float)UI_BUTTON_HEIGHT / 2.0f;
		if (index == state->color)
			ui_ring(ui, cx, cy, 15.0f, 2.0f, UI_ACCENT);
		ui_dot(ui, cx, cy, 10.5f, colors[index] >> 8, 255U);
		ui_add_button(ui, x, UI_BUTTON_TOP, UI_BUTTON_HEIGHT, UI_BUTTON_HEIGHT, NOTES_ACTION_COLOR + index);
		x += UI_BUTTON_HEIGHT + 2;
	}

	/* A separator before the next group. */
	ui_separator(ui, x + UI_GAP / 2 - 1);
	x += UI_GAP;

	/* The Text tool has no widths. */
	if (state->tool == NOTES_ACTION_TEXT)
		return ui_layout_rest(ui, x, left, state);

	/*
	 * The fill has none either, but keeps their room, so that the card and
	 * its buttons stay put from one drawing tool to another (review-1 C-4).
	 */
	if (kind == NOTES_TOOL_FILL) {
		x += (int32_t)NOTES_WIDTHS * (UI_BUTTON_HEIGHT + 2) + UI_GAP;
		return ui_layout_rest(ui, x, left, state);
	}

	/* The widths, each a dot of its size; the chosen one on a pale blue pill, in blue. */
	for (index = 0; index < NOTES_WIDTHS; index++) {
		cx = (float)x + (float)UI_BUTTON_HEIGHT / 2.0f;
		cy = (float)UI_BUTTON_TOP + (float)UI_BUTTON_HEIGHT / 2.0f;
		radius = ui_width_radius(kind, index);

		/* The chosen width stands out in the desktop's blue. */
		if (index == state->width) {
			ui_rounded(ui, (float)x, (float)UI_BUTTON_TOP, (float)UI_BUTTON_HEIGHT, (float)UI_BUTTON_HEIGHT,
				   (float)UI_BUTTON_HEIGHT / 2.0f, UI_ACCENT, UI_ACCENT_TINT_ALPHA);
			ui_dot(ui, cx, cy, radius, UI_ACCENT, 255U);
		} else {
			ui_dot(ui, cx, cy, radius, UI_TEXT, 255U);
		}

		/* The width's button. */
		ui_add_button(ui, x, UI_BUTTON_TOP, UI_BUTTON_HEIGHT, UI_BUTTON_HEIGHT, NOTES_ACTION_WIDTH + index);
		x += UI_BUTTON_HEIGHT + 2;
	}

	/* A separator before the next group. */
	ui_separator(ui, x + UI_GAP / 2 - 1);
	x += UI_GAP;

	/* Undo, Redo, the pages and Save. */
	return ui_layout_rest(ui, x, left, state);
}

/*
 * Lays out (and draws) the card's buttons after the tools' own: Undo and
 * Redo, the pages and Save, from x; returns the width all the buttons take
 * from left.
 */
static int32_t
ui_layout_rest(
	struct notes_ui *ui,
	int32_t x,
	int32_t left,
	const struct notes_ui_state *state)
{
	char page_text[48];
	int32_t text_width;
	int earlier;
	int later;

	/* Undo and Redo, pale when there is nothing to take back or make again. */
	x = ui_label_button(ui, x, "Undo", NOTES_ACTION_UNDO, NOTES_ACTION_NONE, state->can_undo);
	x = ui_label_button(ui, x, "Redo", NOTES_ACTION_REDO, NOTES_ACTION_NONE, state->can_redo);
	ui_separator(ui, x + UI_GAP / 2 - 1);
	x += UI_GAP;

	/* Whether there is a page before the current one, and one after. */
	earlier = 0;
	if (state->page > 0U)
		earlier = 1;
	later = 0;
	if (state->page + 1U < state->page_count)
		later = 1;

	/* The pages: previous, "3 / 12", next, and a new page. */
	x = ui_label_button(ui, x, "<", NOTES_ACTION_PREVIOUS_PAGE, NOTES_ACTION_NONE, earlier);
	(void)snprintf(page_text, sizeof(page_text), "%lu / %lu", (unsigned long)(state->page + 1U), (unsigned long)state->page_count);
	text_width = ui_text_width(ui, page_text);
	ui_text(ui, x + 4, UI_BUTTON_TOP, page_text, UI_TEXT_SECONDARY);
	x += text_width + 8;
	x = ui_label_button(ui, x, ">", NOTES_ACTION_NEXT_PAGE, NOTES_ACTION_NONE, later);
	x = ui_label_button(ui, x, "+ Page", NOTES_ACTION_NEW_PAGE, NOTES_ACTION_NONE, 1);
	ui_separator(ui, x + UI_GAP / 2 - 1);
	x += UI_GAP;

	/* Save; the last button leaves no gap after it. */
	x = ui_label_button(ui, x, "Save", NOTES_ACTION_SAVE, NOTES_ACTION_NONE, 1);

	/* Reports the width the buttons took. */
	return x - 2 - left;
}

/* Makes the whole picture transparent. */
static void
ui_clear(
	struct notes_ui *ui)
{
	uint32_t row;

	/* Each row, every byte zero. */
	for (row = 0; row < ui->height; row++)
		memset(ui->pixels + (size_t)row * ui->pitch, 0, (size_t)ui->width * 4U);
}

/*
 * Lays a colour with an alpha (0 to 255) over one pixel, by the "over"
 * rule on straight alpha: what was there shows through by what the colour
 * leaves.
 */
static void
ui_over(
	struct notes_ui *ui,
	int32_t x,
	int32_t y,
	uint32_t rgb,
	unsigned alpha)
{
	unsigned char *pixel;
	unsigned channel;
	unsigned source;
	unsigned below;
	unsigned below_alpha;
	unsigned result_alpha;
	unsigned value;

	/* Nothing to lay, or a pixel outside the picture. */
	if (alpha == 0U || !ui->drawing)
		return;
	if (x < 0 ||
	    y < 0 ||
	    x >= (int32_t)ui->width ||
	    y >= (int32_t)ui->height)
		return;

	/* The alpha of the result: the colour's, plus what the pixel's shows through it. */
	pixel = ui->pixels + (size_t)y * ui->pitch + (size_t)x * 4U;
	below_alpha = pixel[3];
	result_alpha = alpha + (below_alpha * (255U - alpha) + 127U) / 255U;
	if (result_alpha == 0U)
		return;

	/* Each of blue, green and red, weighted by the two alphas (the rounding of the alpha may carry it past 255). */
	for (channel = 0; channel < 3U; channel++) {
		source = (rgb >> (channel * 8U)) & 0xffU;
		below = pixel[channel];
		value = (source * alpha * 255U + below * below_alpha * (255U - alpha) + result_alpha * 127U) / (result_alpha * 255U);
		if (value > 255U)
			value = 255U;
		pixel[channel] = (unsigned char)value;
	}

	/* The pixel's new alpha. */
	pixel[3] = (unsigned char)result_alpha;
}

/* Measures how far a point is outside a rounded rectangle (negative inside), in pixels. */
static float
ui_rounded_distance(
	float px,
	float py,
	float x,
	float y,
	float width,
	float height,
	float radius)
{
	float cx;
	float cy;
	float dx;
	float dy;
	float outside;
	float inside;

	/* The point relative to the rectangle's centre, folded into one quarter. */
	cx = x + width / 2.0f;
	cy = y + height / 2.0f;
	dx = (float)fabs((double)(px - cx)) - (width / 2.0f - radius);
	dy = (float)fabs((double)(py - cy)) - (height / 2.0f - radius);

	/* The distance past the corner's circle, or into the straight sides. */
	outside = 0.0f;
	if (dx > 0.0f && dy > 0.0f)
		outside = (float)sqrt((double)(dx * dx + dy * dy));
	else if (dx > 0.0f)
		outside = dx;
	else if (dy > 0.0f)
		outside = dy;
	inside = dx;
	if (dy > inside)
		inside = dy;
	if (inside > 0.0f)
		inside = 0.0f;

	/* Reports the distance from the rounded edge. */
	return outside + inside - radius;
}

/* Fills a rounded rectangle with a colour at an alpha, its edge anti-aliased. */
static void
ui_rounded(
	struct notes_ui *ui,
	float x,
	float y,
	float width,
	float height,
	float radius,
	uint32_t rgb,
	unsigned alpha)
{
	int32_t column;
	int32_t row;
	float distance;
	float coverage;

	/* Each pixel of the rectangle, covered by how far inside the edge its centre is. */
	for (row = (int32_t)y; row <= (int32_t)(y + height); row++) {
		for (column = (int32_t)x; column <= (int32_t)(x + width); column++) {
			distance = ui_rounded_distance((float)column + 0.5f, (float)row + 0.5f, x, y, width, height, radius);
			coverage = 0.5f - distance;
			if (coverage <= 0.0f)
				continue;
			if (coverage > 1.0f)
				coverage = 1.0f;
			ui_over(ui, column, row, rgb, (unsigned)(coverage * (float)alpha + 0.5f));
		}
	}
}

/* Draws the one-pixel edge of a rounded rectangle in a colour at an alpha. */
static void
ui_rounded_edge(
	struct notes_ui *ui,
	float x,
	float y,
	float width,
	float height,
	float radius,
	uint32_t rgb,
	unsigned alpha)
{
	int32_t column;
	int32_t row;
	float distance;
	float coverage;

	/* Each pixel near the edge, covered by how close to the edge's middle its centre is. */
	for (row = (int32_t)y - 1; row <= (int32_t)(y + height) + 1; row++) {
		for (column = (int32_t)x - 1; column <= (int32_t)(x + width) + 1; column++) {
			distance = ui_rounded_distance((float)column + 0.5f, (float)row + 0.5f, x, y, width, height, radius);
			coverage = 1.0f - (float)fabs((double)(distance + 0.5f));
			if (coverage <= 0.0f)
				continue;
			ui_over(ui, column, row, rgb, (unsigned)(coverage * (float)alpha + 0.5f));
		}
	}
}

/* Draws the soft slate shadow a rounded card casts below it, outside the card only. */
static void
ui_shadow(
	struct notes_ui *ui,
	float x,
	float y,
	float width,
	float height,
	float radius)
{
	int32_t column;
	int32_t row;
	float distance;
	float card;
	float share;

	/* Each pixel around the dropped card, darker the closer it is. */
	for (row = (int32_t)(y - UI_SHADOW_SOFT); row <= (int32_t)(y + height + UI_SHADOW_DROP + UI_SHADOW_SOFT); row++) {
		for (column = (int32_t)(x - UI_SHADOW_SOFT); column <= (int32_t)(x + width + UI_SHADOW_SOFT); column++) {
			/* The card's own pixels are left to the glass. */
			card = ui_rounded_distance((float)column + 0.5f, (float)row + 0.5f, x, y, width, height, radius);
			if (card < 0.5f)
				continue;

			/* The shadow fades from the dropped card's edge out to its soft reach. */
			distance = ui_rounded_distance((float)column + 0.5f, (float)row + 0.5f, x, y + UI_SHADOW_DROP, width, height, radius);
			if (distance < 0.0f)
				distance = 0.0f;
			share = 1.0f - distance / UI_SHADOW_SOFT;
			if (share <= 0.0f)
				continue;
			ui_over(ui, column, row, UI_SLATE_EDGE, (unsigned)(share * share * (float)UI_SHADOW_ALPHA));
		}
	}
}

/* Draws a card of frosted glass: the white veil, the bright rim at its top and a faint slate edge. */
static void
ui_glass(
	struct notes_ui *ui,
	float x,
	float y,
	float width,
	float height,
	float radius)
{
	/* The veil, through which the desk shows. */
	ui_rounded(ui, x, y, width, height, radius, UI_WHITE, UI_GLASS_ALPHA);

	/* The faint edge all round, and the rim's light along the inside of the top. */
	ui_rounded_edge(ui, x, y, width, height, radius, UI_SLATE_EDGE, UI_EDGE_ALPHA);
	ui_rounded_edge(ui, x + 1.0f, y + 1.0f, width - 2.0f, height - 2.0f, radius - 1.0f, UI_RIM, UI_RIM_ALPHA / 2U);
}

/* Draws an anti-aliased disc in a colour at an alpha. */
static void
ui_dot(
	struct notes_ui *ui,
	float cx,
	float cy,
	float radius,
	uint32_t rgb,
	unsigned alpha)
{
	int32_t x;
	int32_t y;
	float dx;
	float dy;
	float distance;
	float coverage;

	/* Each pixel of the disc's box, covered by how far inside the edge its centre is. */
	for (y = (int32_t)(cy - radius - 1.0f); y <= (int32_t)(cy + radius + 1.0f); y++) {
		for (x = (int32_t)(cx - radius - 1.0f); x <= (int32_t)(cx + radius + 1.0f); x++) {
			dx = (float)x + 0.5f - cx;
			dy = (float)y + 0.5f - cy;
			distance = (float)sqrt((double)(dx * dx + dy * dy));
			coverage = radius + 0.5f - distance;
			if (coverage <= 0.0f)
				continue;
			if (coverage > 1.0f)
				coverage = 1.0f;
			ui_over(ui, x, y, rgb & 0xffffffU, (unsigned)(coverage * (float)alpha + 0.5f));
		}
	}
}

/* Draws an anti-aliased ring of a thickness whose outer edge has a radius. */
static void
ui_ring(
	struct notes_ui *ui,
	float cx,
	float cy,
	float radius,
	float thickness,
	uint32_t rgb)
{
	int32_t x;
	int32_t y;
	float dx;
	float dy;
	float distance;
	float coverage;
	float inner;

	/* Each pixel of the ring's box, covered by how far inside both edges its centre is. */
	for (y = (int32_t)(cy - radius - 1.0f); y <= (int32_t)(cy + radius + 1.0f); y++) {
		for (x = (int32_t)(cx - radius - 1.0f); x <= (int32_t)(cx + radius + 1.0f); x++) {
			dx = (float)x + 0.5f - cx;
			dy = (float)y + 0.5f - cy;
			distance = (float)sqrt((double)(dx * dx + dy * dy));
			coverage = radius + 0.5f - distance;
			inner = distance - (radius - thickness) + 0.5f;
			if (inner < coverage)
				coverage = inner;
			if (coverage <= 0.0f)
				continue;
			if (coverage > 1.0f)
				coverage = 1.0f;
			ui_over(ui, x, y, rgb, (unsigned)(coverage * 255.0f + 0.5f));
		}
	}
}

/* Draws the thin slate line that separates two groups of buttons. */
static void
ui_separator(
	struct notes_ui *ui,
	int32_t x)
{
	int32_t y;

	/* A column of faint pixels, shorter than the buttons. */
	for (y = UI_BUTTON_TOP + 8; y < UI_BUTTON_TOP + UI_BUTTON_HEIGHT - 8; y++)
		ui_over(ui, x, y, UI_SLATE_EDGE, UI_SEPARATOR_ALPHA);
}

/* Measures a text's width in pixels (0 without the font). */
static int32_t
ui_text_width(
	struct notes_ui *ui,
	const char *text)
{
	struct truetype_glyph glyph;
	uint32_t codepoint;
	unsigned index;
	int32_t width;
	int error;

	/* Nothing to measure without the font. */
	if (ui->face == NULL)
		return 0;

	/* The sum of the characters' advances. */
	width = 0;
	for (; *text != '\0'; text++) {
		codepoint = ui_codepoint(*text);
		index = truetype_glyph_index(ui->face, codepoint);
		error = truetype_glyph_metrics(ui->face, index, &glyph);
		if (error == 0)
			width += glyph.advance;
	}

	/* Reports the width. */
	return width;
}

/* Draws a text with its top at a point, in a colour. */
static void
ui_text(
	struct notes_ui *ui,
	int32_t x,
	int32_t y,
	const char *text,
	uint32_t rgb)
{
	unsigned char bitmap[UI_GLYPH_MAX * UI_GLYPH_MAX];
	struct truetype_glyph glyph;
	uint32_t codepoint;
	unsigned index;
	unsigned row;
	unsigned column;
	int32_t baseline;
	int error;

	/* Nothing to draw without the font, or while only measuring. */
	if (ui->face == NULL || !ui->drawing)
		return;

	/* The baseline, centring the text in a button's height. */
	baseline = y + (UI_BUTTON_HEIGHT - (int32_t)ui->font_pixels) / 2 + ui->ascent - 2;

	/* Each character, one after another. */
	for (; *text != '\0'; text++) {
		/* The glyph's coverage; one that does not fit is only advanced over. */
		codepoint = ui_codepoint(*text);
		index = truetype_glyph_index(ui->face, codepoint);
		error = truetype_render_glyph(ui->face, index, &glyph, bitmap, UI_GLYPH_MAX, sizeof(bitmap));
		if (error != 0) {
			error = truetype_glyph_metrics(ui->face, index, &glyph);
			if (error == 0)
				x += glyph.advance;
			continue;
		}

		/* Its pixels laid over the picture by their coverage. */
		for (row = 0; row < glyph.height; row++) {
			for (column = 0; column < glyph.width; column++) {
				ui_over(ui, x + glyph.left + (int32_t)column, baseline - glyph.top + (int32_t)row, rgb,
					bitmap[row * UI_GLYPH_MAX + column]);
			}
		}

		/* The next character's place. */
		x += glyph.advance;
	}
}

/*
 * Draws a button with a label -- on a pale blue pill with a blue label
 * when its action is the chosen one (a tool's), with a pale label when it
 * cannot be used -- adds it, and returns where the next one goes.
 */
static int32_t
ui_label_button(
	struct notes_ui *ui,
	int32_t x,
	const char *label,
	uint32_t action,
	uint32_t chosen,
	int enabled)
{
	int32_t next;

	/* On the card's row of buttons. */
	next = ui_label_button_at(ui, x, UI_BUTTON_TOP, label, action, chosen, enabled);

	/* Reports where the next button goes. */
	return next;
}

/* Draws a button with a label at a top (see ui_label_button()), adds it, and returns where the next one goes. */
static int32_t
ui_label_button_at(
	struct notes_ui *ui,
	int32_t x,
	int32_t y,
	const char *label,
	uint32_t action,
	uint32_t chosen,
	int enabled)
{
	int32_t width;
	uint32_t rgb;

	/* The label's width with room on both sides (a fixed width without the font). */
	width = ui_text_width(ui, label) + 24;
	if (ui->face == NULL)
		width = 44;

	/* The chosen tool's pale blue pill, and the label's colour. */
	rgb = UI_TEXT;
	if (action == chosen) {
		ui_rounded(ui, (float)x, (float)y, (float)width, (float)UI_BUTTON_HEIGHT,
			   (float)UI_BUTTON_HEIGHT / 2.0f, UI_ACCENT, UI_ACCENT_TINT_ALPHA);
		rgb = UI_ACCENT_TEXT;
	}

	/* A button that cannot be used is pale. */
	if (!enabled)
		rgb = UI_TEXT_DISABLED;
	ui_text(ui, x + 12, y, label, rgb);

	/* A pale button does nothing, so it is not added. */
	if (enabled)
		ui_add_button(ui, x, y, width, UI_BUTTON_HEIGHT, action);

	/* Reports where the next button goes. */
	return x + width + 2;
}

/*
 * Lays out (and draws) the text's controls (ws175-p008): the font's name,
 * which chooses the next font, and the size between its smaller and
 * larger buttons ("12 pt"; without a size, the buttons alone).
 */
static int32_t
ui_text_controls(
	struct notes_ui *ui,
	int32_t x,
	const struct notes_ui_state *state)
{
	char size_text[24];
	const char *label;
	int32_t text_width;
	int sized;

	/* The font's name. */
	label = state->font_label;
	if (label == NULL)
		label = "Font";
	x = ui_label_button(ui, x, label, NOTES_ACTION_FONT, NOTES_ACTION_NONE, 1);

	/* Smaller, the size, larger. */
	sized = state->text_size > 0.0f;
	x = ui_label_button(ui, x, "A-", NOTES_ACTION_SIZE_DOWN, NOTES_ACTION_NONE, 1);
	if (sized) {
		(void)snprintf(size_text, sizeof(size_text), "%g pt", (double)state->text_size);
		text_width = ui_text_width(ui, size_text);
		ui_text(ui, x + 4, UI_BUTTON_TOP, size_text, UI_TEXT_SECONDARY);
		x += text_width + 8;
	}

	/* Larger. */
	return ui_label_button(ui, x, "A+", NOTES_ACTION_SIZE_UP, NOTES_ACTION_NONE, 1);
}

/*
 * Draws the eraser's button: "Eraser" while it removes whole strokes,
 * "Part Eraser" while it cuts parts.  Like the other tools its label
 * starts 12 pixels in and its pill fits the label; the room its longer
 * label needs is left after it (ui_eraser_slack), so the buttons after it
 * do not move when the mode changes.
 */
static int32_t
ui_eraser_button(
	struct notes_ui *ui,
	int32_t x,
	const struct notes_ui_state *state)
{
	const char *label;
	int32_t width;
	uint32_t rgb;

	/* The label of the mode, with room on both sides (a fixed width without the font). */
	label = "Eraser";
	if (state->erase_parts)
		label = "Part Eraser";
	width = ui_text_width(ui, label) + 24;
	if (ui->face == NULL)
		width = 44;

	/* The chosen tool's pale blue pill, and the label's colour. */
	rgb = UI_TEXT;
	if (state->tool == NOTES_ACTION_ERASER) {
		ui_rounded(ui, (float)x, (float)UI_BUTTON_TOP, (float)width, (float)UI_BUTTON_HEIGHT,
			   (float)UI_BUTTON_HEIGHT / 2.0f, UI_ACCENT, UI_ACCENT_TINT_ALPHA);
		rgb = UI_ACCENT_TEXT;
	}

	/* The label, where the other tools have theirs. */
	ui_text(ui, x + 12, UI_BUTTON_TOP, label, rgb);
	ui_add_button(ui, x, UI_BUTTON_TOP, width, UI_BUTTON_HEIGHT, NOTES_ACTION_ERASER);

	/* Reports where the next button would go. */
	return x + width + 2;
}

/*
 * Draws the drawing tool button (ws216-p006): the drawing tool used last
 * by its name, on a pale blue pill while a drawing tool is chosen.  It is
 * as wide as its longest name, so the buttons after it stay put when the
 * tool changes.  Its left edge is kept for the strip.
 */
static int32_t
ui_draw_button(
	struct notes_ui *ui,
	int32_t x,
	const struct notes_ui_state *state)
{
	const char *label;
	int32_t width;
	int32_t widest;
	unsigned index;
	uint32_t rgb;

	/* The widest of the names, with room on both sides (a fixed width without the font). */
	widest = 0;
	for (index = 0; index < UI_DRAW_TOOLS; index++) {
		width = ui_text_width(ui, ui_draw_label(ui_draw_actions[index]));
		if (width > widest)
			widest = width;
	}
	width = widest + 24 + UI_MARK_ROOM;
	if (ui->face == NULL)
		width = 44 + UI_MARK_ROOM;

	/* The chosen tool's pale blue pill, and the label's colour. */
	label = ui_draw_label(state->draw_tool);
	rgb = UI_TEXT;
	if (notes_ui_tool(state->tool) != NOTES_UI_NOT_DRAWING) {
		ui_rounded(ui, (float)x, (float)UI_BUTTON_TOP, (float)width, (float)UI_BUTTON_HEIGHT,
			   (float)UI_BUTTON_HEIGHT / 2.0f, UI_ACCENT, UI_ACCENT_TINT_ALPHA);
		rgb = UI_ACCENT_TEXT;
	}

	/* The label, where the other tools have theirs, the mark that the button opens the strip at its end, and the button. */
	ui_text(ui, x + 12, UI_BUTTON_TOP, label, rgb);
	ui_down_mark(ui, (float)(x + width - 14), (float)UI_BUTTON_TOP + (float)UI_BUTTON_HEIGHT / 2.0f, rgb);
	ui_add_button(ui, x, UI_BUTTON_TOP, width, UI_BUTTON_HEIGHT, NOTES_ACTION_DRAW);
	ui->draw_x = x;

	/* Reports where the next button goes. */
	return x + width + 2;
}

/*
 * Draws the strip of drawing tools (ws216-p006): a small glass card in the
 * notice's rows under the toolbar's band, from the drawing tool button's
 * left (kept in the window), its six buttons, the chosen tool's in blue.
 * Its rectangle is kept for the presses.
 */
static void
ui_strip(
	struct notes_ui *ui,
	const struct notes_ui_state *state)
{
	int32_t width;
	int32_t x;
	int32_t button_top;
	unsigned index;

	/* Measures the strip from its buttons' labels. */
	width = 2 * UI_STRIP_PADDING - 2;
	for (index = 0; index < UI_DRAW_TOOLS; index++) {
		if (ui->face == NULL)
			width += 44 + 2;
		else
			width += ui_text_width(ui, ui_draw_label(ui_draw_actions[index])) + 24 + 2;
	}

	/* Its place: under the drawing tool button, in the window. */
	x = ui->draw_x - UI_STRIP_PADDING;
	if (x + width + 8 > (int32_t)ui->width)
		x = (int32_t)ui->width - width - 8;
	if (x < 8)
		x = 8;

	/* A picture too short for the strip shows none. */
	if (UI_STRIP_TOP + UI_STRIP_HEIGHT > (int32_t)ui->height)
		return;

	/*
	 * Its card, without the card's shadow: the shadow's reach (16 pixels
	 * under it) would be cut by the picture's last row, over the page
	 * (review-1 L-3); its faint edge sets it off instead.
	 */
	ui_glass(ui, (float)x, (float)UI_STRIP_TOP, (float)width, (float)UI_STRIP_HEIGHT, (float)UI_STRIP_HEIGHT / 2.0f);

	/* Its buttons, the chosen drawing tool's in blue. */
	button_top = UI_STRIP_TOP + (UI_STRIP_HEIGHT - UI_BUTTON_HEIGHT) / 2;
	ui->strip_x = x;
	x += UI_STRIP_PADDING;
	for (index = 0; index < UI_DRAW_TOOLS; index++)
		x = ui_label_button_at(ui, x, button_top, ui_draw_label(ui_draw_actions[index]), ui_draw_actions[index], state->tool, 1);

	/* The strip's rectangle, for the presses. */
	ui->strip_y = UI_STRIP_TOP;
	ui->strip_width = width;
	ui->strip_height = UI_STRIP_HEIGHT;
	ui->strip_shown = 1;
}

/*
 * Gives the radius of the dot a width's button shows (ws216-p006, review-1
 * L-2): each tool's three dots grow in even steps from its finest.
 */
static float
ui_width_radius(
	unsigned kind,
	unsigned index)
{
	/* The wide tools' larger dots, and the fine pencil's smaller ones. */
	switch (kind) {
	case NOTES_TOOL_HIGHLIGHTER:
	case NOTES_TOOL_WATERCOLOR:
		return 3.0f + (float)index * 3.0f;
	case NOTES_TOOL_BRUSH:
		return 2.5f + (float)index * 3.0f;
	case NOTES_TOOL_PENCIL:
		return 1.5f + (float)index * 1.5f;
	default:
		break;
	}

	/* The pen's. */
	return 2.0f + (float)index * 2.5f;
}

/* Draws the small mark pointing down that says a button opens a strip under it (the user's choice, 2026-10-13). */
static void
ui_down_mark(
	struct notes_ui *ui,
	float cx,
	float cy,
	uint32_t rgb)
{
	int32_t x;
	int32_t y;
	float half;
	float coverage;
	float row;

	/* A triangle 8 pixels wide and 4 high, its point down, each row narrower. */
	for (y = (int32_t)(cy - 2.0f); y <= (int32_t)(cy + 2.0f); y++) {
		row = (float)y + 0.5f - (cy - 2.0f);
		half = 4.0f - row;
		if (half <= 0.0f)
			continue;
		for (x = (int32_t)(cx - 5.0f); x <= (int32_t)(cx + 5.0f); x++) {
			coverage = half + 0.5f - (float)fabs((double)((float)x + 0.5f - cx));
			if (coverage <= 0.0f)
				continue;
			if (coverage > 1.0f)
				coverage = 1.0f;
			ui_over(ui, x, y, rgb, (unsigned)(coverage * 255.0f + 0.5f));
		}
	}
}

/* Gives a drawing tool's name on its button (the pen's for any other action). */
static const char *
ui_draw_label(
	unsigned action)
{
	/* Each drawing tool by its short name. */
	switch (action) {
	case NOTES_ACTION_BRUSH:
		return "Brush";
	case NOTES_ACTION_PENCIL:
		return "Pencil";
	case NOTES_ACTION_WATERCOLOR:
		return "Water";
	case NOTES_ACTION_HIGHLIGHTER:
		return "Marker";
	case NOTES_ACTION_FILL:
		return "Fill";
	default:
		break;
	}

	/* The pen. */
	return "Pen";
}

/* Measures the room the eraser's button leaves unused: how much wider "Part Eraser" is than the label shown. */
static int32_t
ui_eraser_slack(
	struct notes_ui *ui,
	const struct notes_ui_state *state)
{
	int32_t label_width;
	int32_t widest;

	/* Without the font every button is as wide, and so is the eraser's in either mode. */
	if (ui->face == NULL)
		return 0;

	/* "Part Eraser" is the longer label; while it shows there is no room left. */
	if (state->erase_parts)
		return 0;

	/* The two labels' widths. */
	label_width = ui_text_width(ui, "Eraser");
	widest = ui_text_width(ui, "Part Eraser");

	/* Reports the difference. */
	return widest - label_width;
}

/* Adds a button's rectangle and action, when there is room. */
static void
ui_add_button(
	struct notes_ui *ui,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height,
	uint32_t action)
{
	struct notes_button *button;

	/* A full table takes no more; the first button left out is logged (review-1 M6). */
	if (ui->button_count >= NOTES_BUTTONS) {
		if (!ui->overflow_logged) {
			printf("NOTES BUTTONS overflow action=%u\n", action);
			fflush(stdout);
			ui->overflow_logged = 1;
		}
		return;
	}

	/* Succeeded: the button is the last. */
	button = &ui->buttons[ui->button_count];
	button->x = x;
	button->y = y;
	button->width = width;
	button->height = height;
	button->action = action;
	ui->button_count++;
}

/* Gives the character a byte of a label draws: itself when it is ASCII, '?' otherwise. */
static uint32_t
ui_codepoint(
	char byte)
{
	/* A byte past ASCII is part of a character the toolbar does not draw. */
	if ((unsigned char)byte >= 0x80U)
		return (uint32_t)'?';

	/* Reports the ASCII character. */
	return (uint32_t)(unsigned char)byte;
}
