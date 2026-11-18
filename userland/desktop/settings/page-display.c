/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Display page (ws113-p006; the design is plan/ws113/phase006/phase.md):
 *
 *   Displays     Extend or Mirror (D-MODES: the two choices only), the
 *                arrangement of the displays in the desktop's plane, whose
 *                cards are dragged in the extended mode and snap next to
 *                each other on release (arrange.c), and a row for each
 *                display: its size, or that it is held back by the limit
 *                of the displays shown at once.  The choice is a draft
 *                until Apply; Revert takes the desktop's again.
 *   Resolution   each display shown that offers more than one mode: its
 *                sizes (the larger first, with their shape, the
 *                recommended one marked and the one shown stressed), the
 *                refreshes of the size shown, and Use Recommended.  A
 *                press shows the new mode at once as a trial: a popup
 *                counts the seconds the desktop gives it, and Keep keeps
 *                it; Revert, Esc, the popup left to its time or the window
 *                closed puts the old mode back (the desktop does that by
 *                itself).  The popup is in this window, which is on the
 *                display changed: only the display the desktop is on, while
 *                it is the only one shown, can change its resolution.  The
 *                display's scale will be chosen in the same card.
 *   Rotation     each display shown, with four pictures of how it stands
 *                (upright, on its left side, upside down, on its right
 *                side; ws217-p004): pressing one turns the display's picture
 *                at once and the desktop keeps it.  An angle the display may
 *                not take is faint, and the reason is said below.
 *   Brightness   the built-in panel's light, when the desktop can set it:
 *                a slider sent while dragged (every DISPLAY_LIGHT_MS) and
 *                at its release; the light keys move it too, which the
 *                page follows.
 *
 * Everything goes through the desktop's system (kl_system_displays_*).  A
 * choice made from displays that changed meanwhile is refused stale: the
 * page takes the new displays and says so.
 */

#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * The page's controls (hit indices): the two modes, Apply, Revert, the
 * light, the displays' cards from DISPLAY_CARD, their on/off switches from
 * DISPLAY_SHOWN (ws113-p014), and their angles' pictures from
 * DISPLAY_ROTATE, SE_ROTATE_ANGLES a display (ws217-p004).
 */
#define DISPLAY_EXTEND		1
#define DISPLAY_MIRROR		2
#define DISPLAY_APPLY		3
#define DISPLAY_REVERT		4
#define DISPLAY_LIGHT		5
#define DISPLAY_CARD		10
#define DISPLAY_SHOWN		30
#define DISPLAY_ROTATE		40

/*
 * The resolutions' controls: a display's sizes from DISPLAY_SIZES and its
 * refreshes from DISPLAY_REFRESHES, DISPLAY_STRIDE a display (room for
 * KL_DISPLAY_MODES_MAX each), and its Use Recommended at
 * DISPLAY_RECOMMENDED and the display's index.
 */
#define DISPLAY_SIZES		1000
#define DISPLAY_REFRESHES	3000
#define DISPLAY_RECOMMENDED	5000
#define DISPLAY_STRIDE		100

/*
 * A display's part of the resolutions' card: its label's row, a line of
 * a reason or a trial, a size's tile and the space between the tiles,
 * its narrowest, and the room of the refreshes' label.
 */
#define DISPLAY_RES_LABEL	36
#define DISPLAY_RES_LINE	22
#define DISPLAY_SIZE_HEIGHT	56
#define DISPLAY_SIZE_GAP	8
#define DISPLAY_SIZE_WIDTH	140
#define DISPLAY_REFRESH_LABEL	120

/* What the page asked of a display's resolution: a size or a refresh, the default, keeping the trial, putting it back. */
#define DISPLAY_VERB_SET	1U
#define DISPLAY_VERB_RESET	2U
#define DISPLAY_VERB_KEEP	3U
#define DISPLAY_VERB_REVERT	4U

/* How long the page waits to look again at a trial whose time ran out until the desktop tells it ended (ms). */
#define DISPLAY_TRIAL_LOOK_MS	250

/* A display's row of the angles, a picture's square, the space between the pictures, and the panel drawn in it (its long and short sides, its edge's mark). */
#define DISPLAY_ROTATE_ROW	72
#define DISPLAY_TILE		56
#define DISPLAY_TILE_GAP	8
#define DISPLAY_PANEL_LONG	30
#define DISPLAY_PANEL_SHORT	20
#define DISPLAY_PANEL_MARK	10

/* The cards' margin, the space between cards, the arrangement's height, a row's height and the text sizes. */
#define DISPLAY_PAD		18
#define DISPLAY_GAP		16
#define DISPLAY_BOX		200
#define DISPLAY_ROW		40
#define DISPLAY_BUTTONS		44
#define DISPLAY_TITLED		70
#define DISPLAY_TEXT_TITLE	15U
#define DISPLAY_TEXT_SMALL	13U

/* The room a row's switch takes at its right, and the switch's size (widgets.c's). */
#define DISPLAY_SWITCH		64
#define DISPLAY_TOGGLE		44
#define DISPLAY_TOGGLE_HEIGHT	24

/* How often a light being dragged is sent (ms). */
#define DISPLAY_LIGHT_MS	100U

static int display_plain(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int display_arrangement(struct se_app *app, struct kl_canvas *canvas, int x, int y, int width);
static void display_card(struct se_app *app, struct kl_canvas *canvas, unsigned index, const struct se_arrange_rect *rect, unsigned arranged);
static int display_rows(struct se_app *app, struct kl_canvas *canvas, int x, int y, int width);
static int display_light(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int display_light_index(const struct se_display *display);
static unsigned display_rects(const struct se_display *display, struct se_arrange_rect *rects, unsigned *slots);
static void display_spread(struct se_display *display);
static void display_take(struct se_app *app, int keep_draft);
static int display_same_set(const struct se_display *display, const struct kl_display *displays, size_t count);
static void display_apply(struct se_app *app);
static void display_send_light(struct se_app *app, int final);
static void display_send_shown(struct se_app *app, unsigned index);
static int display_rotation(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static unsigned display_rotation_rows(const struct se_display *display);
static void display_rotation_row(struct se_app *app, struct kl_canvas *canvas, unsigned index, int x, int y, int width);
static void display_rotation_tile(struct se_app *app, struct kl_canvas *canvas, unsigned index, unsigned slot, int x, int y);
static void display_rotation_panel(struct kl_canvas *canvas, int center_x, int center_y, unsigned angle, kl_color ink);
static const char *display_rotation_reason(unsigned reason);
static int display_take_rotations(struct se_app *app);
static void display_send_rotation(struct se_app *app, unsigned index, unsigned angle);
static unsigned display_on_count(const struct se_display *display);
static void display_message(struct se_display *display, const char *text, int bad);
static uint64_t display_now_ms(void);
static int display_resolution(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int display_resolution_listed(const struct se_display *display, unsigned index);
static int display_resolution_height(const struct se_display *display, unsigned index, int width);
static int display_resolution_part(struct se_app *app, struct kl_canvas *canvas, unsigned index, int x, int y, int width);
static void display_resolution_size(struct se_app *app, struct kl_canvas *canvas, unsigned index, unsigned slot, const struct se_resolution_size *size, int x, int y, int width);
static int display_resolution_refreshes(struct se_app *app, struct kl_canvas *canvas, unsigned index, int x, int y, int width);
static const char *display_resolution_reason(unsigned reason);
static int display_resolution_columns(int width);
static int display_resolution_free(const struct se_app *app, unsigned index);
static int display_take_resolutions(struct se_app *app);
static void display_resolution_log(const struct se_display *display, unsigned index);
static void display_resolution_press(struct se_app *app, int index);
static void display_send_resolution(struct se_app *app, unsigned index, const struct kl_display_mode *mode, unsigned verb);
static void display_send_trial(struct se_app *app, unsigned verb);
static void display_trial_begin(struct se_app *app);
static void display_trial_act(struct se_app *app, unsigned action);
static void display_trial_close(struct se_app *app);
static void display_trial_text(const struct se_display *display, char *text, size_t size);
static void display_trial_follow(struct se_app *app);
static int display_trial_remaining(const struct se_app *app, uint32_t *remaining);
static void display_mode_text(const struct kl_display_mode *mode, char *text, size_t size);
static void display_resolution_said(struct se_display *display, int error);
static void display_resolution_message(struct se_display *display, const char *text, int bad);
static const char *display_verb_word(unsigned verb);

/*
 * Draws the Display page: the displays' card and, for a built-in panel,
 * the brightness card.  Returns the edge below them.
 */
int
se_display_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct se_display *display;
	const char *subtitle;
	kl_color ink;
	unsigned capable;
	int height;
	int y;
	int bx;

	/* Without the desktop's displays there is nothing to arrange: the screen's mode, read only. */
	display = &app->display;
	capable = 0U;
	if (app->system != NULL)
		capable = kl_system_capabilities(app->system) & KL_SYSTEM_HAS_DISPLAYS;
	if (capable == 0U || display->count == 0U)
		return display_plain(app, canvas, x, top, width);

	/* The displays' card: the modes, the arrangement, a row a display, Apply and Revert. */
	subtitle = kl_tr("Extend spreads the desktop over the displays. Mirror shows it on each.");
	height = DISPLAY_TITLED + DISPLAY_BUTTONS + 12 + DISPLAY_BOX + 12 + (int)display->count * DISPLAY_ROW + DISPLAY_BUTTONS + DISPLAY_PAD;
	y = se_card_begin(app, canvas, x, top, width, height, kl_tr("Displays"), subtitle);

	/* The two modes, the chosen one stressed. */
	bx = x + DISPLAY_PAD;
	bx += se_button_draw(app, canvas, bx, y + 6, kl_tr("Extend"), display->draft_mode == KL_DISPLAYS_EXTENDED, display->request == 0U, DISPLAY_EXTEND) + 8;
	bx += se_button_draw(app, canvas, bx, y + 6, kl_tr("Mirror"), display->draft_mode == KL_DISPLAYS_MIRROR, display->request == 0U, DISPLAY_MIRROR) + 16;

	/* With one display both modes show the same, which is said beside them. */
	if (display->count < 2U) {
		(void)kl_text_draw_fit(app->text, canvas, bx, y + 27, kl_tr("With one display both show the same."), DISPLAY_TEXT_SMALL, 0, x + width - DISPLAY_PAD - bx,
		    SE_COLOR_TEXT_SECONDARY);
	}

	/* Below the modes. */
	y += DISPLAY_BUTTONS + 12;

	/* The arrangement, then the rows. */
	y = display_arrangement(app, canvas, x + DISPLAY_PAD, y, width - 2 * DISPLAY_PAD);
	y = display_rows(app, canvas, x, y + 12, width);

	/* Apply and Revert, while the draft differs and nothing is awaited. */
	bx = x + width - DISPLAY_PAD - se_button_width(app, kl_tr("Apply"));
	(void)se_button_draw(app, canvas, bx, y + 6, kl_tr("Apply"), 1, display->edited && display->request == 0U, DISPLAY_APPLY);
	bx -= se_button_width(app, kl_tr("Revert")) + 8;
	(void)se_button_draw(app, canvas, bx, y + 6, kl_tr("Revert"), 0, display->edited && display->request == 0U, DISPLAY_REVERT);

	/* The last answer, under the card. */
	y = top + height;
	if (display->message[0] != '\0') {
		ink = SE_COLOR_TEXT_SECONDARY;
		if (display->message_bad)
			ink = SE_COLOR_BAD;
		(void)kl_text_draw_fit(app->text, canvas, x + 2, y + 18, display->message, DISPLAY_TEXT_SMALL, 0, width, ink);
		y += 30;
	}

	/* The displays' resolutions, their angles (ws217-p004), then the light of a built-in panel, when the desktop can set it. */
	y = display_resolution(app, canvas, x, y + DISPLAY_GAP, width);
	y = display_rotation(app, canvas, x, y, width);
	y = display_light(app, canvas, x, y, width);

	/* The edge below the cards. */
	return y;
}

/*
 * Carries out a click on a control: a mode chosen, the draft applied or
 * reverted.  The displays' cards and the light are dragged instead.
 */
void
se_display_press(
	struct se_app *app,
	int index)
{
	struct se_display *display;
	unsigned slot;

	/* Nothing while a request is awaited, or without the desktop's system. */
	display = &app->display;
	if (display->request != 0U || app->system == NULL)
		return;

	/* A mode: the draft's. */
	if (index == DISPLAY_EXTEND || index == DISPLAY_MIRROR) {
		display->draft_mode = KL_DISPLAYS_EXTENDED;
		if (index == DISPLAY_MIRROR)
			display->draft_mode = KL_DISPLAYS_MIRROR;
		if (display->draft_mode == KL_DISPLAYS_EXTENDED)
			display_spread(display);
		display->edited = 1U;
		if (display->draft_mode == KL_DISPLAYS_MIRROR && display->mode == KL_DISPLAYS_MIRROR)
			display->edited = 0U;
		if (display->draft_mode == KL_DISPLAYS_EXTENDED && display->mode == KL_DISPLAYS_EXTENDED)
			display->edited = (unsigned)(memcmp(display->draft, display->displays, display->count * sizeof(display->draft[0])) != 0);
		display->message[0] = '\0';
		app->dirty = 1;
		return;
	}

	/* The draft sent. */
	if (index == DISPLAY_APPLY && display->edited) {
		display_apply(app);
		return;
	}

	/* A display turned off or on, at once (ws113-p014). */
	if (index >= DISPLAY_SHOWN && (size_t)(index - DISPLAY_SHOWN) < display->count) {
		display_send_shown(app, (unsigned)(index - DISPLAY_SHOWN));
		return;
	}

	/* A display's size, refresh or default: tried at once. */
	if (index >= DISPLAY_SIZES) {
		display_resolution_press(app, index);
		return;
	}

	/* A display's angle chosen, at once (ws217-p004): the display, and the picture's angle. */
	if (index >= DISPLAY_ROTATE && (size_t)(index - DISPLAY_ROTATE) < display->count * SE_ROTATE_ANGLES) {
		slot = (unsigned)(index - DISPLAY_ROTATE);
		display_send_rotation(app, slot / SE_ROTATE_ANGLES, se_rotate_angle(slot % SE_ROTATE_ANGLES));
		return;
	}

	/* The desktop's choice again. */
	if (index == DISPLAY_REVERT) {
		display_take(app, 0);
		display->message[0] = '\0';
		app->dirty = 1;
	}
}

/*
 * Follows a drag on a display's card (in the extended mode: the card moves
 * with the pointer and snaps next to the others) or on the light's slider.
 */
void
se_display_drag(
	struct se_app *app,
	int index,
	int x,
	unsigned phase)
{
	struct se_display *display;
	struct se_arrange_rect rects[SE_ARRANGE_MAX];
	unsigned slots[SE_ARRANGE_MAX];
	unsigned arranged;
	unsigned moving;
	unsigned slot;
	int32_t plane_x;
	int32_t plane_y;
	int32_t snapped_x;
	int32_t snapped_y;
	float fraction;

	/* The light: the value under the pointer, sent now and then while held and at the release. */
	display = &app->display;
	if (index == DISPLAY_LIGHT) {
		fraction = se_slider_fraction(&display->slider, x);
		display->light = (unsigned)(fraction * 100.0f + 0.5f);
		display->light_dragging = 1;
		app->dirty = 1;
		if (phase == SE_DRAG_END) {
			display->light_dragging = 0;
			display_send_light(app, 1);
			return;
		}

		/* While held: now and then. */
		display_send_light(app, 0);
		return;
	}

	/* Only a display's card, in the extended mode, while nothing is awaited. */
	if (index < DISPLAY_CARD || (size_t)(index - DISPLAY_CARD) >= display->count)
		return;
	if (display->draft_mode != KL_DISPLAYS_EXTENDED || display->request != 0U)
		return;
	moving = (unsigned)(index - DISPLAY_CARD);

	/* The plane's point under the pointer, in the last frame's mapping. */
	se_arrange_from_box(&display->view, x, app->drag_y, &plane_x, &plane_y);

	/* The press: where on the card it was taken. */
	if (phase == SE_DRAG_START) {
		display->dragging = 1;
		display->drag_index = (int)moving;
		display->grab_x = plane_x - display->draft[moving].x;
		display->grab_y = plane_y - display->draft[moving].y;
		return;
	}

	/* Each move and the release: the card next to the others, nearest the pointer. */
	if (!display->dragging || display->drag_index != (int)moving)
		return;
	arranged = display_rects(display, rects, slots);
	for (slot = 0U; slot < arranged && slots[slot] != moving; slot++)
		continue;
	if (slot == arranged)
		return;
	se_arrange_snap(rects, arranged, slot, plane_x - display->grab_x, plane_y - display->grab_y, &snapped_x, &snapped_y);
	if (snapped_x != display->draft[moving].x || snapped_y != display->draft[moving].y) {
		display->draft[moving].x = snapped_x;
		display->draft[moving].y = snapped_y;
		display->edited = 1U;
		display->message[0] = '\0';
		app->dirty = 1;
	}

	/* The release ends the drag where it snapped. */
	if (phase == SE_DRAG_END) {
		display->dragging = 0;
		display->drag_index = -1;
		se_log("DISPLAY place %s x=%ld y=%ld", display->draft[moving].key, (long)display->draft[moving].x, (long)display->draft[moving].y);
	}
}

/*
 * Follows the displays: a new snapshot (a display plugged or unplugged, a
 * choice applied, the light changed) becomes the page's, keeping a draft
 * of the same displays.
 */
void
se_display_poll(
	struct se_app *app)
{
	struct se_display *display;

	/* Nothing without the desktop's system. */
	display = &app->display;
	if (app->system == NULL)
		return;

	/* The first snapshot, then each new one. */
	if (!display->taken || (app->system_changed & KL_SYSTEM_CHANGED_DISPLAYS) != 0U) {
		display->taken = 1;
		display_take(app, 1);
	}

	/* A trial of a resolution: its popup's seconds, and its end. */
	display_trial_follow(app);
}

/*
 * Reports how long the main loop may sleep for the resolutions: until the
 * second a trial's count shows changes (the popup's, or the card's while
 * the page is shown), -1 when no trial is under way.
 */
int
se_display_wait(
	const struct se_app *app)
{
	uint32_t remaining;
	int trial;

	/* Only a trial the page shows. */
	if (!app->display.trial_shown && app->page != SE_PAGE_DISPLAY)
		return -1;
	trial = display_trial_remaining(app, &remaining);
	if (!trial)
		return -1;

	/* Its time ran out: the desktop tells the end soon. */
	if (remaining == 0U)
		return DISPLAY_TRIAL_LOOK_MS;

	/* Reports the rest of the second shown now. */
	return (int)((remaining - 1U) % 1000U) + 1;
}

/*
 * Takes the answer of a request the page asked.  Returns 1 when it was
 * one.
 */
int
se_display_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	struct se_display *display;

	/* A light's answer: only its failure is told. */
	display = &app->display;
	if (display->light_request != 0U && request == display->light_request) {
		display->light_request = 0U;
		se_log("DISPLAY light result errno=%d", error);
		if (error != 0)
			display_message(display, kl_tr("The brightness could not be changed."), 1);
		app->dirty = 1;
		return 1;
	}

	/* A display turned off or on (ws113-p014): only its failure is told. */
	if (display->shown_request != 0U && request == display->shown_request) {
		display->shown_request = 0U;
		se_log("DISPLAY shown result errno=%d", error);
		app->dirty = 1;
		switch (error) {
		case 0:
			display_message(display, "", 0);
			break;
		case EINVAL:
			display_message(display, kl_tr("At least one display stays on."), 1);
			break;
		case ENODEV:
			display_message(display, kl_tr("The display is not connected."), 1);
			break;
		case EPERM:
			display_message(display, kl_tr("The displays can be changed only in an unlocked session."), 1);
			break;
		default:
			display_message(display, kl_tr("The display could not be turned off or on."), 1);
			break;
		}

		/* The answer was the page's. */
		return 1;
	}

	/* A display's angle (ws217-p004): only its failure is told; the snapshot that follows shows the new angle. */
	if (display->rotation_request != 0U && request == display->rotation_request) {
		display->rotation_request = 0U;
		se_log("DISPLAY rotation result errno=%d", error);
		app->dirty = 1;
		switch (error) {
		case 0:
			display_message(display, "", 0);
			break;
		case ENOTSUP:
			display_message(display, kl_tr("This display cannot be rotated that way."), 1);
			break;
		case ENODEV:
			display_message(display, kl_tr("The display is not connected."), 1);
			break;
		case EPERM:
			display_message(display, kl_tr("The displays can be changed only in an unlocked session."), 1);
			break;
		default:
			display_message(display, kl_tr("The display could not be rotated."), 1);
			break;
		}

		/* The answer was the page's. */
		return 1;
	}

	/* A resolution set or reset: its trial began, or why not is said. */
	if (display->resolution_request != 0U && request == display->resolution_request) {
		display->resolution_request = 0U;
		se_log("DISPLAY resolution result verb=%s errno=%d", display_verb_word(display->resolution_verb), error);
		app->dirty = 1;
		if (error == 0) {
			display_trial_begin(app);
			return 1;
		}

		/* The popup goes, and the card says why. */
		display_trial_close(app);
		display_resolution_said(display, error);
		return 1;
	}

	/* A trial kept or put back: only a keep that came too late is told. */
	if (display->trial_request != 0U && request == display->trial_request) {
		display->trial_request = 0U;
		se_log("DISPLAY trial result verb=%s errno=%d", display_verb_word(display->trial_verb), error);
		app->dirty = 1;
		if (display->trial_verb == DISPLAY_VERB_KEEP && error == EINVAL)
			display_resolution_message(display, kl_tr("The time ran out: the previous resolution is back."), 1);
		else if (display->trial_verb == DISPLAY_VERB_KEEP && error != 0)
			display_resolution_message(display, kl_tr("The resolution could not be kept."), 1);
		return 1;
	}

	/* Only the choice the page sent. */
	if (display->request == 0U || request != display->request)
		return 0;
	display->request = 0U;
	se_log("DISPLAY apply result errno=%d", error);
	app->dirty = 1;

	/* What it says; a choice applied (or refused) leaves the desktop's displays in the draft. */
	switch (error) {
	case 0:
		display_message(display, "", 0);
		display_take(app, 0);
		break;
	case ESTALE:
		display_take(app, 0);
		display_message(display, kl_tr("The displays changed. Arrange them again."), 1);
		break;
	case EINVAL:
		display_message(display, kl_tr("The displays must touch along an edge and not overlap."), 1);
		break;
	case EPERM:
		display_message(display, kl_tr("The displays can be changed only in an unlocked session."), 1);
		break;
	case ENOTSUP:
		display_message(display, kl_tr("This desktop cannot change the displays."), 1);
		break;
	default:
		display_message(display, kl_tr("The displays could not be changed."), 1);
		break;
	}

	/* Succeeded: the answer was the page's. */
	return 1;
}

/* Draws the screen's mode and the graphics device, read only, for a desktop that does not tell its displays; returns the edge below. */
static int
display_plain(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const char *mode;
	const char *graphics;
	int y;

	/* The values, or a dash for what is not known. */
	mode = app->about.display;
	if (mode[0] == '\0')
		mode = "-";
	graphics = app->about.graphics;
	if (graphics[0] == '\0')
		graphics = "-";

	/* The screen's card. */
	y = se_card_begin(app, canvas, x, top, width, se_card_height(2, 1), kl_tr("Screen"), NULL);
	y = se_row_value(app, canvas, x, y, width, kl_tr("Mode"), mode, 0);
	(void)se_row_value(app, canvas, x, y, width, kl_tr("Graphics"), graphics, 1);

	/* The edge below the card. */
	return top + se_card_height(2, 1);
}

/* Draws the arrangement's box: each display's card at its draft place, scaled; returns the edge below it. */
static int
display_arrangement(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int width)
{
	struct se_display *display;
	struct se_arrange_rect rects[SE_ARRANGE_MAX];
	unsigned slots[SE_ARRANGE_MAX];
	unsigned arranged;
	unsigned slot;

	/* The box's ground. */
	display = &app->display;
	kl_canvas_round(canvas, (float)x, (float)y, (float)width, (float)DISPLAY_BOX, 10.0f, SE_COLOR_FIELD);

	/* The mapping: fitted again unless a card is being dragged (it would move under the pointer). */
	arranged = display_rects(display, rects, slots);
	if (!display->dragging)
		se_arrange_fit(rects, arranged, x, y, width, DISPLAY_BOX, &display->view);

	/* Each arranged display's card (one held back is told in its row only). */
	for (slot = 0U; slot < arranged; slot++)
		display_card(app, canvas, slots[slot], &rects[slot], arranged);

	/* The edge below the box. */
	return y + DISPLAY_BOX;
}

/*
 * Draws one display's card in the arrangement at its rectangle, a control
 * to drag in the extended mode when there is another arranged display.
 */
static void
display_card(
	struct se_app *app,
	struct kl_canvas *canvas,
	unsigned index,
	const struct se_arrange_rect *rect,
	unsigned arranged)
{
	struct se_display *display;
	const struct kl_display *shown;
	struct kl_rect hit;
	struct kl_text_line line;
	kl_color ground;
	kl_color ink;
	int x;
	int y;
	int width;
	int height;

	/* Its place in the box. */
	display = &app->display;
	shown = &display->draft[index];
	se_arrange_to_box(&display->view, rect, &x, &y, &width, &height);

	/* Its ground: the accent for the desktop's anchor. */
	ground = SE_COLOR_TILE;
	ink = SE_COLOR_TEXT;
	if ((shown->flags & KL_DISPLAY_ANCHOR) != 0U) {
		ground = SE_COLOR_ACCENT;
		ink = SE_COLOR_ACCENT_TEXT;
	}

	/* The card and its edge. */
	kl_canvas_round(canvas, (float)x, (float)y, (float)width, (float)height, 6.0f, ground);
	kl_canvas_round_border(canvas, (float)x, (float)y, (float)width, (float)height, 6.0f, 1.0f, SE_COLOR_CARD_EDGE);

	/* Its label, in the middle. */
	kl_text_metrics(app->text, DISPLAY_TEXT_SMALL, &line);
	(void)kl_text_draw_fit(app->text, canvas, x + 6, y + height / 2 + line.ascent / 2, shown->label, DISPLAY_TEXT_SMALL, 1, width - 12, ink);

	/* A card dragged in the extended mode, when there is another to arrange it with. */
	if (display->draft_mode != KL_DISPLAYS_EXTENDED || arranged < 2U)
		return;
	hit.x = x;
	hit.y = y;
	hit.width = width;
	hit.height = height;
	se_ui_hit(app, &hit, SE_HIT_CONTROL, DISPLAY_CARD + (int)index);
}

/* Draws a row for each display: its label, and its size or that it is held back; returns the edge below them. */
static int
display_rows(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int width)
{
	struct se_display *display;
	const struct kl_display *shown;
	char value[96];
	char label[KL_DISPLAY_LABEL_MAX + 32];
	size_t index;
	unsigned on_count;
	int switches;
	int row_width;
	int on;
	int enabled;
	int top;

	/*
	 * Each display; in the extended mode each has a switch that turns it off
	 * or on at once (ws113-p014), the last one on kept on.
	 */
	display = &app->display;
	switches = display->mode == KL_DISPLAYS_EXTENDED && display->count > 1U;
	row_width = width;
	if (switches)
		row_width = width - DISPLAY_SWITCH;
	on_count = display_on_count(display);
	for (index = 0U; index < display->count; index++) {
		shown = &display->displays[index];

		/* Its label, marked when it is the machine's own. */
		(void)snprintf(label, sizeof(label), "%s", shown->label);
		if ((shown->flags & KL_DISPLAY_INTERNAL) != 0U)
			(void)snprintf(label, sizeof(label), "%s (%s)", shown->label, kl_tr("built in"));

		/* Its size and refresh, or why it is not shown. */
		(void)snprintf(value, sizeof(value), "%u x %u, %u Hz", shown->width, shown->height, (shown->refresh_mhz + 500U) / 1000U);
		if ((shown->flags & KL_DISPLAY_LIMITED) != 0U)
			(void)snprintf(value, sizeof(value), "%s", kl_tr("Not shown: the computer shows no more displays at once"));
		else if ((shown->flags & KL_DISPLAY_SHOWN) == 0U)
			(void)snprintf(value, sizeof(value), "%s", kl_tr("Off"));
		top = y;
		y = se_row_value(app, canvas, x, y, row_width, label, value, index + 1U == display->count);
		if (!switches)
			continue;

		/* Its switch: on unless turned off; the last one on, and any while a request is awaited, cannot be flipped. */
		on = (shown->flags & KL_DISPLAY_OFF) == 0U;
		enabled = display->shown_request == 0U && display->request == 0U;
		if (on && on_count <= 1U)
			enabled = 0;
		se_toggle_draw(app, canvas, x + width - DISPLAY_PAD - DISPLAY_TOGGLE, top + (DISPLAY_ROW - DISPLAY_TOGGLE_HEIGHT) / 2, on, enabled, DISPLAY_SHOWN + (int)index);
	}

	/* The edge below the rows. */
	return y;
}

/* Draws the brightness card when a display has a light the desktop can set; returns the edge below it (the top without one). */
static int
display_light(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct se_display *display;
	char percent[16];
	int index;
	int y;

	/* Only a display with a light. */
	display = &app->display;
	index = display_light_index(display);
	if (index < 0)
		return top;

	/* The card: the slider and the value. */
	y = se_card_begin(app, canvas, x, top, width, se_card_height(0, 1) + 60, kl_tr("Brightness"), display->displays[index].label);
	se_slider_draw(app, canvas, x + DISPLAY_PAD + 12, y + 18, width - 2 * DISPLAY_PAD - 90, (float)display->light / 100.0f, 1, DISPLAY_LIGHT, &display->slider);
	(void)snprintf(percent, sizeof(percent), "%u%%", display->light);
	(void)kl_text_draw_fit(app->text, canvas, x + width - DISPLAY_PAD - 50, y + 30, percent, DISPLAY_TEXT_TITLE, 0, 50, SE_COLOR_TEXT);

	/* The edge below the card. */
	return top + se_card_height(0, 1) + 60 + DISPLAY_GAP;
}

/* Finds the display whose light the desktop can set: its index, or -1. */
static int
display_light_index(
	const struct se_display *display)
{
	size_t index;

	/* The first with a light. */
	for (index = 0U; index < display->count; index++) {
		if ((display->displays[index].flags & KL_DISPLAY_BACKLIGHT) != 0U)
			return (int)index;
	}

	/* None. */
	return -1;
}

/*
 * Gives the rectangles of the displays arranged (each one not held back,
 * in the mirror mode all at the origin) and their indices in the draft;
 * returns how many.
 */
static unsigned
display_rects(
	const struct se_display *display,
	struct se_arrange_rect *rects,
	unsigned *slots)
{
	const struct kl_display *draft;
	unsigned count;
	size_t index;

	/* Each display not held back. */
	count = 0U;
	for (index = 0U; index < display->count && count < SE_ARRANGE_MAX; index++) {
		draft = &display->draft[index];
		if ((draft->flags & KL_DISPLAY_LIMITED) != 0U)
			continue;
		if ((draft->flags & KL_DISPLAY_OFF) != 0U && display->draft_mode == KL_DISPLAYS_EXTENDED)
			continue;

		/* Its draft place and size; the mirror's at the origin. */
		rects[count].x = draft->x;
		rects[count].y = draft->y;
		rects[count].width = draft->width;
		rects[count].height = draft->height;
		if (display->draft_mode == KL_DISPLAYS_MIRROR) {
			rects[count].x = 0;
			rects[count].y = 0;
		}

		/* Its index in the draft. */
		slots[count] = (unsigned)index;
		count++;
	}

	/* Succeeded: how many. */
	return count;
}

/*
 * Lays the arranged displays in a row, the anchor first, when any two of
 * them overlap (as the places of a mirror do): the extended mode chosen
 * from the mirror starts from places the compositor takes.
 */
static void
display_spread(
	struct se_display *display)
{
	struct se_arrange_rect rects[SE_ARRANGE_MAX];
	unsigned slots[SE_ARRANGE_MAX];
	unsigned arranged;
	unsigned first;
	unsigned second;
	unsigned slot;
	unsigned pass;
	int overlap;
	int64_t right;
	struct kl_display *draft;

	/* Whether any two overlap. */
	arranged = display_rects(display, rects, slots);
	overlap = 0;
	for (first = 0U; first < arranged; first++) {
		for (second = first + 1U; second < arranged; second++) {
			if ((int64_t)rects[first].x + rects[first].width <= rects[second].x ||
			    (int64_t)rects[second].x + rects[second].width <= rects[first].x)
				continue;
			if ((int64_t)rects[first].y + rects[first].height <= rects[second].y ||
			    (int64_t)rects[second].y + rects[second].height <= rects[first].y)
				continue;
			overlap = 1;
		}
	}

	/* Places that do not overlap are kept. */
	if (!overlap)
		return;

	/* A row from the origin: the anchor in the first pass, the others in the second. */
	right = 0;
	for (pass = 0U; pass < 2U; pass++) {
		for (slot = 0U; slot < arranged; slot++) {
			draft = &display->draft[slots[slot]];
			if (((draft->flags & KL_DISPLAY_ANCHOR) != 0U) != (pass == 0U))
				continue;
			draft->x = (int32_t)right;
			draft->y = 0;
			right += draft->width;
		}
	}
}

/*
 * Takes the desktop's displays.  keep_draft keeps an edited draft when
 * the displays are the same ones (only their light or a place someone
 * else applied changed); otherwise the draft is the desktop's.
 */
static void
display_take(
	struct se_app *app,
	int keep_draft)
{
	struct se_display *display;
	struct kl_display displays[KL_DISPLAYS_MAX];
	size_t count;
	size_t slot;
	unsigned mode;
	int32_t x;
	int32_t y;
	int same;
	int changed;
	int turned;
	int resized;
	int differs;
	int index;

	/* The snapshot (cleared first, so that an unchanged one compares equal byte for byte). */
	display = &app->display;
	memset(displays, 0, sizeof(displays));
	count = kl_system_displays_get(app->system, displays, KL_DISPLAYS_MAX);
	if (count > KL_DISPLAYS_MAX)
		count = KL_DISPLAYS_MAX;
	mode = kl_system_displays_mode(app->system);
	same = display_same_set(display, displays, count);

	/*
	 * The page is drawn again while it is shown, when the snapshot differs
	 * from the last or the page itself asked for it (keep_draft 0: a
	 * choice applied or put back).
	 */
	changed = 0;
	if (count != display->count) {
		changed = 1;
	} else if (mode != display->mode) {
		changed = 1;
	} else {
		differs = memcmp(display->displays, displays, count * sizeof(displays[0]));
		if (differs != 0)
			changed = 1;
	}
	if (keep_draft == 0)
		changed = 1;

	/* The snapshot becomes the page's, with the displays' angles (ws217-p004), which may change alone. */
	memcpy(display->displays, displays, count * sizeof(displays[0]));
	display->count = count;
	display->mode = mode;
	turned = display_take_rotations(app);
	if (turned)
		changed = 1;
	resized = display_take_resolutions(app);
	if (resized)
		changed = 1;
	if (changed != 0 && app->page == SE_PAGE_DISPLAY)
		app->dirty = 1;

	/* The light shown, unless the user holds the slider. */
	index = display_light_index(display);
	if (index >= 0 && !display->light_dragging)
		display->light = display->displays[index].brightness;

	/* An edited draft of the same displays is kept (a card being dragged too), with what the desktop tells of each now but its place. */
	if (keep_draft && display->edited && same) {
		for (slot = 0U; slot < count; slot++) {
			x = display->draft[slot].x;
			y = display->draft[slot].y;
			display->draft[slot] = displays[slot];
			display->draft[slot].x = x;
			display->draft[slot].y = y;
		}

		/* Kept. */
		return;
	}

	/* The draft is the desktop's choice; a draft lost to other displays is said. */
	if (keep_draft && display->edited && !same)
		display_message(display, kl_tr("The displays changed. Arrange them again."), 1);
	memcpy(display->draft, displays, count * sizeof(displays[0]));
	display->draft_mode = display->mode;
	display->edited = 0U;
	display->dragging = 0;
	display->drag_index = -1;
}

/* Tells whether a snapshot names the same displays (by their keys, in the same order) as the page's. */
static int
display_same_set(
	const struct se_display *display,
	const struct kl_display *displays,
	size_t count)
{
	size_t index;
	int differs;

	/* The same number. */
	if (count != display->count)
		return 0;

	/* The same keys. */
	for (index = 0U; index < count; index++) {
		differs = strcmp(displays[index].key, display->displays[index].key);
		if (differs != 0)
			return 0;
	}

	/* The same displays. */
	return 1;
}

/* Sends the draft: the mode and, for the extended mode, every display's place. */
static void
display_apply(
	struct se_app *app)
{
	struct se_display *display;
	struct kl_display_place places[KL_DISPLAYS_MAX];
	size_t count;
	size_t index;
	int error;

	/* The places of the displays shown (a display held back keeps its own). */
	display = &app->display;
	count = 0U;
	for (index = 0U; index < display->count && display->draft_mode == KL_DISPLAYS_EXTENDED; index++) {
		if ((display->draft[index].flags & (KL_DISPLAY_LIMITED | KL_DISPLAY_OFF)) != 0U)
			continue;
		places[count].key = display->draft[index].key;
		places[count].x = display->draft[index].x;
		places[count].y = display->draft[index].y;
		count++;
	}

	/* Asked of the desktop. */
	error = kl_system_displays_apply(app->system, display->draft_mode, places, count, &display->request);
	se_log("DISPLAY apply mode=%u places=%u error=%d", display->draft_mode, (unsigned)count, error);
	if (error != 0) {
		display->request = 0U;
		display_message(display, kl_tr("This desktop cannot change the displays."), 1);
	}

	/* The page shows it. */
	app->dirty = 1;
}

/* Sends the light shown: at the release (final), or while dragged at most every DISPLAY_LIGHT_MS. */
static void
display_send_light(
	struct se_app *app,
	int final)
{
	struct se_display *display;
	uint64_t now;
	int index;
	int error;

	/* The display with the light, and not too often while dragged. */
	display = &app->display;
	index = display_light_index(display);
	if (index < 0 || app->system == NULL)
		return;
	now = display_now_ms();
	if (!final && now - display->light_sent_ms < DISPLAY_LIGHT_MS)
		return;
	display->light_sent_ms = now;

	/* Asked of the desktop; only the last request's answer is followed. */
	error = kl_system_displays_set_brightness(app->system, display->displays[index].key, display->light, &display->light_request);
	if (error != 0) {
		display->light_request = 0U;
		display_message(display, kl_tr("The brightness could not be changed."), 1);
	}

	/* The release is logged. */
	if (final)
		se_log("DISPLAY light percent=%u error=%d", display->light, error);
}

/* Keeps the page's last message. */
static void
display_message(
	struct se_display *display,
	const char *text,
	int bad)
{
	/* The text and whether it tells of a failure. */
	(void)snprintf(display->message, sizeof(display->message), "%s", text);
	display->message_bad = bad;
}

/* The monotonic time in milliseconds. */
static uint64_t
display_now_ms(void)
{
	struct timespec now;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Turns a display off when it is on, on when it is off (ws113-p014): asked of the desktop at once. */
static void
display_send_shown(
	struct se_app *app,
	unsigned index)
{
	struct se_display *display;
	unsigned shown;
	int error;

	/* Not while another request is awaited. */
	display = &app->display;
	if (display->shown_request != 0U || app->system == NULL)
		return;

	/* The other way from now. */
	display->message[0] = '\0';
	shown = (unsigned)((display->displays[index].flags & KL_DISPLAY_OFF) != 0U);
	error = kl_system_displays_set_shown(app->system, display->displays[index].key, shown, &display->shown_request);
	se_log("DISPLAY shown %s shown=%u error=%d", display->displays[index].key, shown, error);
	if (error != 0) {
		display->shown_request = 0U;
		display_message(display, kl_tr("This desktop cannot turn displays off."), 1);
	}

	/* The page shows it. */
	app->dirty = 1;
}

/* Counts the displays on: connected, not turned off, not held back (ws113-p014). */
static unsigned
display_on_count(
	const struct se_display *display)
{
	unsigned count;
	size_t index;

	/* Each display. */
	count = 0U;
	for (index = 0U; index < display->count; index++) {
		if ((display->displays[index].flags & (KL_DISPLAY_OFF | KL_DISPLAY_LIMITED)) == 0U)
			count++;
	}

	/* Succeeded: how many. */
	return count;
}

/*
 * Draws the card of the displays' angles (ws217-p004): a row for each
 * display shown whose angle the desktop told; returns the edge below it
 * and the space after it (the top without such a display).
 */
static int
display_rotation(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct se_display *display;
	unsigned rows;
	unsigned index;
	int height;
	int y;

	/* Only displays shown whose angle the desktop told. */
	display = &app->display;
	rows = display_rotation_rows(display);
	if (rows == 0U)
		return top;

	/* The card, then a row for each such display. */
	height = DISPLAY_TITLED + (int)rows * DISPLAY_ROTATE_ROW + DISPLAY_PAD;
	y = se_card_begin(app, canvas, x, top, width, height, kl_tr("Rotation"), kl_tr("Turn the picture to match how the display stands."));
	for (index = 0U; index < display->count; index++) {
		if (!display->rotation_told[index])
			continue;
		if ((display->displays[index].flags & KL_DISPLAY_SHOWN) == 0U)
			continue;
		display_rotation_row(app, canvas, index, x, y, width);
		y += DISPLAY_ROTATE_ROW;
	}

	/* The edge below the card, and the space after it. */
	return top + height + DISPLAY_GAP;
}

/* Counts the displays the card of the angles has a row for: shown, their angle told. */
static unsigned
display_rotation_rows(
	const struct se_display *display)
{
	unsigned count;
	size_t index;

	/* Each display shown whose angle the desktop told. */
	count = 0U;
	for (index = 0U; index < display->count; index++) {
		if (!display->rotation_told[index])
			continue;
		if ((display->displays[index].flags & KL_DISPLAY_SHOWN) == 0U)
			continue;
		count++;
	}

	/* Succeeded: how many rows. */
	return count;
}

/* Draws one display's row of the angles: its label, the reason some cannot be chosen, and the four pictures at the right. */
static void
display_rotation_row(
	struct se_app *app,
	struct kl_canvas *canvas,
	unsigned index,
	int x,
	int y,
	int width)
{
	struct se_display *display;
	const char *reason;
	unsigned said;
	unsigned slot;
	int pictures;
	int text_width;
	int left;

	/* The pictures' room at the right, and the text's at the left. */
	display = &app->display;
	pictures = (int)SE_ROTATE_ANGLES * DISPLAY_TILE + ((int)SE_ROTATE_ANGLES - 1) * DISPLAY_TILE_GAP;
	left = x + width - DISPLAY_PAD - pictures;
	text_width = left - (x + DISPLAY_PAD) - DISPLAY_TILE_GAP;

	/* The display's label, and the reason below it. */
	(void)kl_text_draw_fit(app->text, canvas, x + DISPLAY_PAD, y + 26, display->displays[index].label, DISPLAY_TEXT_TITLE, 0, text_width, SE_COLOR_TEXT);
	said = se_rotate_reason(display->rotation_why[index]);
	reason = display_rotation_reason(said);
	if (reason != NULL)
		(void)kl_text_draw_fit(app->text, canvas, x + DISPLAY_PAD, y + 48, reason, DISPLAY_TEXT_SMALL, 0, text_width, SE_COLOR_TEXT_SECONDARY);

	/* The four pictures. */
	for (slot = 0U; slot < SE_ROTATE_ANGLES; slot++)
		display_rotation_tile(app, canvas, index, slot, left + (int)slot * (DISPLAY_TILE + DISPLAY_TILE_GAP), y + 4);
}

/*
 * Draws one picture of a display's angle: the accent for the angle chosen,
 * faint for one the display may not take now, a control for the others
 * (while no angle's request is awaited).
 */
static void
display_rotation_tile(
	struct se_app *app,
	struct kl_canvas *canvas,
	unsigned index,
	unsigned slot,
	int x,
	int y)
{
	const struct kl_theme *theme;
	struct se_display *display;
	struct kl_rect hit;
	char degrees[16];
	kl_color ground;
	kl_color ink;
	unsigned angle;
	int pressable;

	/* The picture's angle, and whether pressing it turns the display. */
	display = &app->display;
	angle = se_rotate_angle(slot);
	pressable = se_rotate_pressable(angle, display->rotation[index], display->rotation_mask[index]);

	/*
	 * Its colors: the angle chosen, its picture and degrees in the ink that
	 * reads on the accent (BUG-306: the accent's text colour is the accent
	 * itself, which hid them); one that can be chosen; one that cannot.
	 */
	ground = SE_COLOR_TILE;
	ink = SE_COLOR_TEXT;
	if (angle == display->rotation[index]) {
		theme = kl_theme_default();
		ground = SE_COLOR_ACCENT;
		ink = theme->accent_ink;
	} else if (!pressable) {
		ground = SE_COLOR_FIELD;
		ink = SE_COLOR_TEXT_FAINT;
	}

	/* The square and its edge. */
	kl_canvas_round(canvas, (float)x, (float)y, (float)DISPLAY_TILE, (float)DISPLAY_TILE, 8.0f, ground);
	kl_canvas_round_border(canvas, (float)x, (float)y, (float)DISPLAY_TILE, (float)DISPLAY_TILE, 8.0f, 1.0f, SE_COLOR_CARD_EDGE);

	/* The panel as it stands, and its angle in degrees below it. */
	display_rotation_panel(canvas, x + DISPLAY_TILE / 2, y + 22, angle, ink);
	(void)snprintf(degrees, sizeof(degrees), "%u\xc2\xb0", angle);
	(void)kl_text_draw_fit(app->text, canvas, x, y + DISPLAY_TILE - 6, degrees, DISPLAY_TEXT_SMALL - 2U, 1, DISPLAY_TILE, ink);

	/* A control, while nothing is awaited. */
	if (!pressable ||
	    display->rotation_request != 0U ||
	    display->request != 0U)
		return;
	hit.x = x;
	hit.y = y;
	hit.width = DISPLAY_TILE;
	hit.height = DISPLAY_TILE;
	se_ui_hit(app, &hit, SE_HIT_CONTROL, DISPLAY_ROTATE + (int)(index * SE_ROTATE_ANGLES + slot));
}

/*
 * Draws a display's panel as it stands at an angle, around a point: its
 * outline, lying (0, 180) or standing (90, 270), and a mark on the edge
 * that is the panel's top when it is upright: at the top, on the left
 * (the panel on its left side, the picture turned clockwise by 90), at
 * the bottom, or on the right (270).
 */
static void
display_rotation_panel(
	struct kl_canvas *canvas,
	int center_x,
	int center_y,
	unsigned angle,
	kl_color ink)
{
	float width;
	float height;
	float left;
	float top;

	/* The outline, lying or standing. */
	width = (float)DISPLAY_PANEL_LONG;
	height = (float)DISPLAY_PANEL_SHORT;
	if (angle == KL_DISPLAY_ROTATE_90 || angle == KL_DISPLAY_ROTATE_270) {
		width = (float)DISPLAY_PANEL_SHORT;
		height = (float)DISPLAY_PANEL_LONG;
	}
	left = (float)center_x - width / 2.0f;
	top = (float)center_y - height / 2.0f;
	kl_canvas_round_border(canvas, left, top, width, height, 3.0f, 2.0f, ink);

	/* The mark of the panel's own top edge, inside the outline. */
	switch (angle) {
	case KL_DISPLAY_ROTATE_90:
		kl_canvas_round(canvas, left + 3.0f, (float)center_y - (float)DISPLAY_PANEL_MARK / 2.0f, 3.0f, (float)DISPLAY_PANEL_MARK, 1.5f, ink);
		break;
	case KL_DISPLAY_ROTATE_180:
		kl_canvas_round(canvas, (float)center_x - (float)DISPLAY_PANEL_MARK / 2.0f, top + height - 6.0f, (float)DISPLAY_PANEL_MARK, 3.0f, 1.5f, ink);
		break;
	case KL_DISPLAY_ROTATE_270:
		kl_canvas_round(canvas, left + width - 6.0f, (float)center_y - (float)DISPLAY_PANEL_MARK / 2.0f, 3.0f, (float)DISPLAY_PANEL_MARK, 1.5f, ink);
		break;
	default:
		kl_canvas_round(canvas, (float)center_x - (float)DISPLAY_PANEL_MARK / 2.0f, top + 3.0f, (float)DISPLAY_PANEL_MARK, 3.0f, 1.5f, ink);
		break;
	}
}

/* Gives the words of a reason said below a display's pictures (SE_ROTATE_REASON_*), or NULL for none. */
static const char *
display_rotation_reason(
	unsigned reason)
{
	/* Each reason's sentence. */
	switch (reason) {
	case SE_ROTATE_REASON_FIXED:
		return kl_tr("This display cannot be rotated.");
	case SE_ROTATE_REASON_ALONE:
		return kl_tr("The rotation can be changed while this display is the only one shown.");
	case SE_ROTATE_REASON_RESOURCE:
		return kl_tr("The display could not be rotated (not enough memory).");
	case SE_ROTATE_REASON_GAME:
		return kl_tr("Rotation is paused while a game is shown.");
	case SE_ROTATE_REASON_QUARTER:
		return kl_tr("90 and 270 degrees are not available on this device yet.");
	default:
		break;
	}

	/* Nothing to say. */
	return NULL;
}

/*
 * Takes each display's angle from the desktop (ws217-p004): told or not,
 * the angle chosen, the angles it may take and why not.  Returns 1 when
 * any of them differs from what the page had.
 */
static int
display_take_rotations(
	struct se_app *app)
{
	struct se_display *display;
	unsigned rotation;
	unsigned mask;
	unsigned why;
	unsigned told;
	size_t index;
	int changed;
	int error;

	/* Each display of the snapshot. */
	display = &app->display;
	changed = 0;
	for (index = 0U; index < display->count; index++) {
		/* What the desktop told of it (nothing from a desktop before the angles). */
		rotation = KL_DISPLAY_ROTATE_0;
		mask = KL_DISPLAY_ROTATION_0;
		why = 0U;
		told = 0U;
		error = kl_system_displays_rotation(app->system, display->displays[index].key, &rotation);
		if (error == 0)
			error = kl_system_displays_rotations(app->system, display->displays[index].key, &mask, &why);
		if (error == 0)
			told = 1U;

		/* Whether it differs from the page's. */
		if (told != display->rotation_told[index] ||
		    rotation != display->rotation[index] ||
		    mask != display->rotation_mask[index] ||
		    why != display->rotation_why[index])
			changed = 1;

		/* The page's. */
		display->rotation_told[index] = told;
		display->rotation[index] = rotation;
		display->rotation_mask[index] = mask;
		display->rotation_why[index] = why;
	}

	/* Succeeded: whether anything changed. */
	return changed;
}

/* Turns a display's picture to an angle (ws217-p004): asked of the desktop at once. */
static void
display_send_rotation(
	struct se_app *app,
	unsigned index,
	unsigned angle)
{
	struct se_display *display;
	int pressable;
	int error;

	/* Not while another angle is awaited, nor for an angle that cannot be chosen now. */
	display = &app->display;
	if (display->rotation_request != 0U || app->system == NULL)
		return;
	pressable = se_rotate_pressable(angle, display->rotation[index], display->rotation_mask[index]);
	if (!pressable)
		return;

	/* Asked of the desktop. */
	display->message[0] = '\0';
	error = kl_system_displays_set_rotation(app->system, display->displays[index].key, angle, &display->rotation_request);
	se_log("DISPLAY rotation %s angle=%u error=%d", display->displays[index].key, angle, error);
	if (error != 0) {
		display->rotation_request = 0U;
		display_message(display, kl_tr("This desktop cannot rotate the displays."), 1);
	}

	/* The page shows it. */
	app->dirty = 1;
}


/*
 * Draws the card of the displays' resolutions: a part for each display
 * shown that offers more than one mode; returns the edge below it and the
 * space after it (the top without such a display).
 */
static int
display_resolution(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct se_display *display;
	kl_color ink;
	unsigned index;
	int listed;
	int height;
	int parts;
	int y;

	/* The card's height: each display listed. */
	display = &app->display;
	height = DISPLAY_TITLED + DISPLAY_PAD;
	parts = 0;
	for (index = 0U; index < display->count; index++) {
		listed = display_resolution_listed(display, index);
		if (!listed)
			continue;
		height += display_resolution_height(display, index, width);
		parts++;
	}

	/* No display to list. */
	if (parts == 0)
		return top;

	/* The card, then each display's part. */
	y = se_card_begin(app, canvas, x, top, width, height, kl_tr("Resolution"), kl_tr("A new resolution goes back by itself unless you keep it."));
	for (index = 0U; index < display->count; index++) {
		listed = display_resolution_listed(display, index);
		if (!listed)
			continue;
		y = display_resolution_part(app, canvas, index, x, y, width);
	}

	/* The last answer, under the card. */
	y = top + height;
	if (display->resolution_message[0] != '\0') {
		ink = SE_COLOR_TEXT_SECONDARY;
		if (display->resolution_bad)
			ink = SE_COLOR_BAD;
		(void)kl_text_draw_fit(app->text, canvas, x + 2, y + 18, display->resolution_message, DISPLAY_TEXT_SMALL, 0, width, ink);
		y += 30;
	}

	/* The edge below the card, and the space after it. */
	return y + DISPLAY_GAP;
}

/*
 * Tells whether the card of the resolutions lists a display: shown, its
 * modes told, and more than one of them (a display of one mode, as a
 * tablet's panel, keeps its size in its row only).
 */
static int
display_resolution_listed(
	const struct se_display *display,
	unsigned index)
{
	/* A display shown whose modes the desktop told. */
	if ((display->displays[index].flags & KL_DISPLAY_SHOWN) == 0U)
		return 0;
	if (!display->resolution_told[index])
		return 0;

	/* One mode: nothing to choose. */
	if ((display->resolution_why[index] & KL_DISPLAY_RESOLUTION_WHY_FIXED) != 0U)
		return 0;
	if (display->mode_count[index] < 2U)
		return 0;

	/* Listed. */
	return 1;
}

/*
 * Reports the height of a display's part of the card: its label, a line
 * of a reason or of a trial, the rows of its sizes, its refreshes when the
 * size shown has more than one, and Use Recommended.
 */
static int
display_resolution_height(
	const struct se_display *display,
	unsigned index,
	int width)
{
	struct se_resolution_size sizes[KL_DISPLAY_MODES_MAX];
	struct kl_display_mode current;
	uint32_t refreshes[KL_DISPLAY_MODES_MAX];
	unsigned count;
	unsigned reason;
	int columns;
	int rows;
	int height;
	int shown;

	/* The label, and a line for a reason or a trial. */
	height = DISPLAY_RES_LABEL;
	reason = se_resolution_reason(display->resolution_why[index]);
	if (reason != SE_RESOLUTION_REASON_NONE)
		height += DISPLAY_RES_LINE;
	if (display->resolution_state[index] != KL_DISPLAY_RESOLUTION_NONE)
		height += DISPLAY_RES_LINE;

	/* The rows of the sizes. */
	count = se_resolution_sizes(display->modes[index], display->mode_count[index], sizes, KL_DISPLAY_MODES_MAX);
	columns = display_resolution_columns(width);
	rows = ((int)count + columns - 1) / columns;
	height += rows * (DISPLAY_SIZE_HEIGHT + DISPLAY_SIZE_GAP);

	/* The refreshes of the size shown, when it has more than one. */
	shown = se_resolution_marked(display->modes[index], display->mode_count[index], KL_DISPLAY_MODE_CURRENT, &current);
	if (shown) {
		count = se_resolution_refreshes(display->modes[index], display->mode_count[index], current.width, current.height, refreshes, KL_DISPLAY_MODES_MAX);
		if (count > 1U)
			height += DISPLAY_BUTTONS;
	}

	/* Use Recommended. */
	height += DISPLAY_BUTTONS;

	/* The part's height. */
	return height;
}

/*
 * Draws a display's part of the card from a top edge: its label and the
 * mode shown, the reason none can be chosen, a trial under way, its sizes,
 * its refreshes and Use Recommended.  Returns the edge below it.
 */
static int
display_resolution_part(
	struct se_app *app,
	struct kl_canvas *canvas,
	unsigned index,
	int x,
	int y,
	int width)
{
	struct se_display *display;
	struct se_resolution_size sizes[KL_DISPLAY_MODES_MAX];
	struct kl_display_mode current;
	struct kl_display_mode native;
	char shown_text[64];
	char seconds_text[16];
	char line[SE_MESSAGE];
	const char *reason;
	uint32_t remaining;
	unsigned count;
	unsigned slot;
	unsigned state;
	unsigned why;
	int columns;
	int tile_width;
	int shown;
	int recommended;
	int enabled;
	int text_width;
	int error;
	int bx;

	/* The display's label, and the mode shown at the right. */
	display = &app->display;
	shown = se_resolution_marked(display->modes[index], display->mode_count[index], KL_DISPLAY_MODE_CURRENT, &current);
	shown_text[0] = '\0';
	if (shown)
		display_mode_text(&current, shown_text, sizeof(shown_text));
	text_width = kl_text_width(app->text, shown_text, strlen(shown_text), DISPLAY_TEXT_SMALL, 0);
	(void)kl_text_draw_fit(app->text, canvas, x + DISPLAY_PAD, y + 24, display->displays[index].label, DISPLAY_TEXT_TITLE, 0, width - 2 * DISPLAY_PAD - text_width - 16,
	    SE_COLOR_TEXT);
	(void)kl_text_draw_fit(app->text, canvas, x + width - DISPLAY_PAD - text_width, y + 24, shown_text, DISPLAY_TEXT_SMALL, 0, text_width, SE_COLOR_TEXT_SECONDARY);
	y += DISPLAY_RES_LABEL;

	/* Why none can be chosen now. */
	reason = display_resolution_reason(se_resolution_reason(display->resolution_why[index]));
	if (reason != NULL) {
		(void)kl_text_draw_fit(app->text, canvas, x + DISPLAY_PAD, y + 14, reason, DISPLAY_TEXT_SMALL, 0, width - 2 * DISPLAY_PAD, SE_COLOR_TEXT_SECONDARY);
		y += DISPLAY_RES_LINE;
	}

	/* A trial under way (the page's own is behind its popup; another program's is told here with its seconds). */
	if (display->resolution_state[index] != KL_DISPLAY_RESOLUTION_NONE) {
		state = KL_DISPLAY_RESOLUTION_NONE;
		remaining = 0U;
		why = 0U;
		error = kl_system_displays_resolution(app->system, display->displays[index].key, &state, &remaining, &why);
		if (error != 0)
			state = display->resolution_state[index];

		/* The words of its state. */
		switch (state) {
		case KL_DISPLAY_RESOLUTION_SWITCHING:
			(void)snprintf(line, sizeof(line), "%s", kl_tr("The resolution is being changed."));
			break;
		case KL_DISPLAY_RESOLUTION_TRIAL:
			(void)snprintf(seconds_text, sizeof(seconds_text), "%u", se_resolution_seconds(remaining));
			(void)kl_tr_format(line, sizeof(line),
			    kl_trn("A new resolution is being tried. It goes back in {1} second unless kept.", "A new resolution is being tried. It goes back in {1} seconds unless kept.",
				(unsigned long)se_resolution_seconds(remaining)),
			    seconds_text, (const char *)NULL);
			break;
		default:
			(void)snprintf(line, sizeof(line), "%s", kl_tr("The previous resolution is coming back."));
			break;
		}
		(void)kl_text_draw_fit(app->text, canvas, x + DISPLAY_PAD, y + 14, line, DISPLAY_TEXT_SMALL, 0, width - 2 * DISPLAY_PAD, SE_COLOR_ACCENT);
		y += DISPLAY_RES_LINE;
	}

	/* The sizes in rows, as wide as the card lets them be. */
	count = se_resolution_sizes(display->modes[index], display->mode_count[index], sizes, KL_DISPLAY_MODES_MAX);
	columns = display_resolution_columns(width);
	tile_width = (width - 2 * DISPLAY_PAD - (columns - 1) * DISPLAY_SIZE_GAP) / columns;
	for (slot = 0U; slot < count; slot++) {
		display_resolution_size(app, canvas, index, slot, &sizes[slot], x + DISPLAY_PAD + (int)(slot % (unsigned)columns) * (tile_width + DISPLAY_SIZE_GAP),
		    y + (int)(slot / (unsigned)columns) * (DISPLAY_SIZE_HEIGHT + DISPLAY_SIZE_GAP), tile_width);
	}
	y += ((int)count + columns - 1) / columns * (DISPLAY_SIZE_HEIGHT + DISPLAY_SIZE_GAP);

	/* The refreshes of the size shown. */
	y = display_resolution_refreshes(app, canvas, index, x, y, width);

	/* Use Recommended, when another mode than the recommended one is shown. */
	recommended = se_resolution_marked(display->modes[index], display->mode_count[index], KL_DISPLAY_MODE_NATIVE, &native);
	enabled = display_resolution_free(app, index);
	if (!recommended || (native.flags & KL_DISPLAY_MODE_CURRENT) != 0U)
		enabled = 0;
	bx = x + width - DISPLAY_PAD - se_button_width(app, kl_tr("Use Recommended"));
	(void)se_button_draw(app, canvas, bx, y + 4, kl_tr("Use Recommended"), 0, enabled, DISPLAY_RECOMMENDED + (int)index);

	/* The edge below the part. */
	return y + DISPLAY_BUTTONS;
}

/*
 * Draws one size of a display: the size and, below it, its shape and
 * whether it is the recommended one; the accent for the size shown, faint
 * when none can be chosen now, a control otherwise.
 */
static void
display_resolution_size(
	struct se_app *app,
	struct kl_canvas *canvas,
	unsigned index,
	unsigned slot,
	const struct se_resolution_size *size,
	int x,
	int y,
	int width)
{
	const struct kl_theme *theme;
	struct kl_rect hit;
	char size_text[32];
	char aspect[SE_RESOLUTION_ASPECT_MAX];
	char under[64];
	kl_color ground;
	kl_color ink;
	kl_color faint;
	int free_now;
	int text_width;

	/* Its colours: the size shown, one that can be chosen, or all faint while none can. */
	free_now = display_resolution_free(app, index);
	ground = SE_COLOR_TILE;
	ink = SE_COLOR_TEXT;
	faint = SE_COLOR_TEXT_SECONDARY;
	if ((size->flags & SE_RESOLUTION_CURRENT) != 0U) {
		theme = kl_theme_default();
		ground = SE_COLOR_ACCENT;
		ink = theme->accent_ink;
		faint = theme->accent_ink;
	} else if (!free_now) {
		ground = SE_COLOR_FIELD;
		ink = SE_COLOR_TEXT_FAINT;
		faint = SE_COLOR_TEXT_FAINT;
	}

	/* The tile and its edge. */
	kl_canvas_round(canvas, (float)x, (float)y, (float)width, (float)DISPLAY_SIZE_HEIGHT, 8.0f, ground);
	kl_canvas_round_border(canvas, (float)x, (float)y, (float)width, (float)DISPLAY_SIZE_HEIGHT, 8.0f, 1.0f, SE_COLOR_CARD_EDGE);

	/* The size, in the middle. */
	(void)snprintf(size_text, sizeof(size_text), "%lu x %lu", (unsigned long)size->width, (unsigned long)size->height);
	text_width = kl_text_width(app->text, size_text, strlen(size_text), DISPLAY_TEXT_TITLE, 1);
	if (text_width > width - 8)
		text_width = width - 8;
	(void)kl_text_draw_fit(app->text, canvas, x + (width - text_width) / 2, y + 25, size_text, DISPLAY_TEXT_TITLE, 1, width - 8, ink);

	/* Its shape, and Recommended for the display's own. */
	se_resolution_aspect(size->width, size->height, aspect, sizeof(aspect));
	(void)snprintf(under, sizeof(under), "%s", aspect);
	if ((size->flags & SE_RESOLUTION_NATIVE) != 0U && aspect[0] != '\0')
		(void)snprintf(under, sizeof(under), "%s \xc2\xb7 %s", aspect, kl_tr("Recommended"));
	else if ((size->flags & SE_RESOLUTION_NATIVE) != 0U)
		(void)snprintf(under, sizeof(under), "%s", kl_tr("Recommended"));
	text_width = kl_text_width(app->text, under, strlen(under), DISPLAY_TEXT_SMALL - 1U, 0);
	if (text_width > width - 8)
		text_width = width - 8;
	(void)kl_text_draw_fit(app->text, canvas, x + (width - text_width) / 2, y + 45, under, DISPLAY_TEXT_SMALL - 1U, 0, width - 8, faint);

	/* A control, unless it is the size shown or none can be chosen now. */
	if ((size->flags & SE_RESOLUTION_CURRENT) != 0U || !free_now)
		return;
	hit.x = x;
	hit.y = y;
	hit.width = width;
	hit.height = DISPLAY_SIZE_HEIGHT;
	se_ui_hit(app, &hit, SE_HIT_CONTROL, DISPLAY_SIZES + (int)(index * DISPLAY_STRIDE + slot));
}

/*
 * Draws the refreshes of the size a display shows, when it has more than
 * one: a label and a button for each, the one shown stressed.  Returns
 * the edge below them (the top when there are none to choose).
 */
static int
display_resolution_refreshes(
	struct se_app *app,
	struct kl_canvas *canvas,
	unsigned index,
	int x,
	int y,
	int width)
{
	struct se_display *display;
	struct kl_display_mode current;
	uint32_t refreshes[KL_DISPLAY_MODES_MAX];
	char hertz[SE_RESOLUTION_HERTZ_MAX];
	unsigned count;
	unsigned slot;
	int shown;
	int free_now;
	int stressed;
	int button;
	int bx;

	/* The size shown and its refreshes. */
	display = &app->display;
	shown = se_resolution_marked(display->modes[index], display->mode_count[index], KL_DISPLAY_MODE_CURRENT, &current);
	if (!shown)
		return y;
	count = se_resolution_refreshes(display->modes[index], display->mode_count[index], current.width, current.height, refreshes, KL_DISPLAY_MODES_MAX);
	if (count < 2U)
		return y;

	/* The label. */
	(void)kl_text_draw_fit(app->text, canvas, x + DISPLAY_PAD, y + 25, kl_tr("Refresh rate"), DISPLAY_TEXT_SMALL, 0, DISPLAY_REFRESH_LABEL - 8, SE_COLOR_TEXT);

	/* A button for each refresh, as many as fit in the row. */
	free_now = display_resolution_free(app, index);
	bx = x + DISPLAY_PAD + DISPLAY_REFRESH_LABEL;
	for (slot = 0U; slot < count; slot++) {
		se_resolution_hertz(refreshes[slot], hertz, sizeof(hertz));
		button = se_button_width(app, hertz);
		if (bx + button > x + width - DISPLAY_PAD)
			break;
		stressed = 0;
		if (refreshes[slot] == current.refresh_mhz)
			stressed = 1;
		(void)se_button_draw(app, canvas, bx, y + 6, hertz, stressed, free_now, DISPLAY_REFRESHES + (int)(index * DISPLAY_STRIDE + slot));
		bx += button + 8;
	}

	/* The edge below the row. */
	return y + DISPLAY_BUTTONS;
}

/* Gives the words of a reason said below a display's sizes (SE_RESOLUTION_REASON_*), or NULL for none. */
static const char *
display_resolution_reason(
	unsigned reason)
{
	/* Each reason's sentence (a display of one mode is not listed). */
	switch (reason) {
	case SE_RESOLUTION_REASON_ANCHOR:
		return kl_tr("The resolution can be changed on the display the desktop is shown on.");
	case SE_RESOLUTION_REASON_ALONE:
		return kl_tr("The resolution can be changed while this display is the only one shown.");
	case SE_RESOLUTION_REASON_GAME:
		return kl_tr("The resolution cannot be changed while a game is shown.");
	default:
		break;
	}

	/* Nothing to say. */
	return NULL;
}

/* Reports how many sizes a row of the card holds at its width (one at least). */
static int
display_resolution_columns(
	int width)
{
	int columns;

	/* As many of the narrowest tiles as fit between the margins. */
	columns = (width - 2 * DISPLAY_PAD + DISPLAY_SIZE_GAP) / (DISPLAY_SIZE_WIDTH + DISPLAY_SIZE_GAP);
	if (columns < 1)
		columns = 1;

	/* The columns. */
	return columns;
}

/*
 * Tells whether a display's resolution can be chosen now: nothing stands
 * in its way, it is in no trial, and the page awaits no answer and shows
 * no trial of its own.
 */
static int
display_resolution_free(
	const struct se_app *app,
	unsigned index)
{
	const struct se_display *display;

	/* The desktop's reasons and its trial. */
	display = &app->display;
	if (app->system == NULL)
		return 0;
	if (display->resolution_why[index] != 0U)
		return 0;
	if (display->resolution_state[index] != KL_DISPLAY_RESOLUTION_NONE)
		return 0;

	/* The page's own requests and trial. */
	if (display->resolution_request != 0U || display->trial_request != 0U)
		return 0;
	if (display->trial_shown || display->request != 0U)
		return 0;

	/* Free. */
	return 1;
}

/*
 * Takes each display's resolution from the desktop: told or not, its
 * modes, its trial and why none can be chosen.  Returns 1 when any of them
 * differs from what the page had.
 */
static int
display_take_resolutions(
	struct se_app *app)
{
	struct se_display *display;
	struct kl_display_mode modes[KL_DISPLAY_MODES_MAX];
	uint32_t remaining;
	size_t count;
	unsigned state;
	unsigned why;
	unsigned told;
	unsigned index;
	int changed;
	int differs;
	int error;

	/* Each display of the snapshot. */
	display = &app->display;
	changed = 0;
	for (index = 0U; index < display->count; index++) {
		/* What the desktop told of it (nothing from a desktop before the resolutions). */
		memset(modes, 0, sizeof(modes));
		state = KL_DISPLAY_RESOLUTION_NONE;
		remaining = 0U;
		why = 0U;
		told = 0U;
		count = 0U;
		error = kl_system_displays_resolution(app->system, display->displays[index].key, &state, &remaining, &why);
		if (error == 0) {
			count = kl_system_displays_modes(app->system, display->displays[index].key, modes, KL_DISPLAY_MODES_MAX);
			told = 1U;
		}

		/* Its modes, logged when they changed. */
		differs = memcmp(modes, display->modes[index], sizeof(modes));
		if (differs != 0 || count != display->mode_count[index]) {
			memcpy(display->modes[index], modes, sizeof(modes));
			display->mode_count[index] = (unsigned)count;
			changed = 1;
			display_resolution_log(display, index);
		}

		/* Its trial and its reasons. */
		if (told != display->resolution_told[index] ||
		    state != display->resolution_state[index] ||
		    why != display->resolution_why[index])
			changed = 1;
		if (state != display->resolution_state[index])
			se_log("DISPLAY resolution state key=%s state=%u remaining_ms=%lu", display->displays[index].key, state, (unsigned long)remaining);
		display->resolution_told[index] = told;
		display->resolution_state[index] = state;
		display->resolution_why[index] = why;
	}

	/* Succeeded: whether anything changed. */
	return changed;
}

/* Logs a display's sizes as the card shows them, with their controls (the tests press them). */
static void
display_resolution_log(
	const struct se_display *display,
	unsigned index)
{
	struct se_resolution_size sizes[KL_DISPLAY_MODES_MAX];
	uint32_t refreshes[KL_DISPLAY_MODES_MAX];
	unsigned count;
	unsigned refresh_count;
	unsigned slot;
	unsigned rate;

	/* The display, and each size with its refreshes. */
	count = se_resolution_sizes(display->modes[index], display->mode_count[index], sizes, KL_DISPLAY_MODES_MAX);
	se_log("DISPLAY modes key=%s modes=%u sizes=%u why=%u", display->displays[index].key, display->mode_count[index], count, display->resolution_why[index]);
	for (slot = 0U; slot < count; slot++) {
		se_log("DISPLAY size control=%u width=%lu height=%lu native=%u current=%u", DISPLAY_SIZES + index * DISPLAY_STRIDE + slot, (unsigned long)sizes[slot].width,
		    (unsigned long)sizes[slot].height, (unsigned)((sizes[slot].flags & SE_RESOLUTION_NATIVE) != 0U), (unsigned)((sizes[slot].flags & SE_RESOLUTION_CURRENT) != 0U));
		if ((sizes[slot].flags & SE_RESOLUTION_CURRENT) == 0U)
			continue;

		/* The refreshes of the size shown. */
		refresh_count = se_resolution_refreshes(display->modes[index], display->mode_count[index], sizes[slot].width, sizes[slot].height, refreshes, KL_DISPLAY_MODES_MAX);
		for (rate = 0U; rate < refresh_count; rate++)
			se_log("DISPLAY refresh control=%u refresh_mhz=%lu", DISPLAY_REFRESHES + index * DISPLAY_STRIDE + rate, (unsigned long)refreshes[rate]);
	}
}

/* Carries out a press on a display's size, refresh or Use Recommended. */
static void
display_resolution_press(
	struct se_app *app,
	int index)
{
	struct se_display *display;
	struct se_resolution_size sizes[KL_DISPLAY_MODES_MAX];
	struct kl_display_mode current;
	struct kl_display_mode wanted;
	uint32_t refreshes[KL_DISPLAY_MODES_MAX];
	unsigned which;
	unsigned slot;
	unsigned count;
	int free_now;
	int shown;

	/* The display and the control's place among its own. */
	display = &app->display;
	if (index >= DISPLAY_RECOMMENDED) {
		which = (unsigned)(index - DISPLAY_RECOMMENDED);
		slot = 0U;
	} else if (index >= DISPLAY_REFRESHES) {
		which = (unsigned)(index - DISPLAY_REFRESHES) / DISPLAY_STRIDE;
		slot = (unsigned)(index - DISPLAY_REFRESHES) % DISPLAY_STRIDE;
	} else {
		which = (unsigned)(index - DISPLAY_SIZES) / DISPLAY_STRIDE;
		slot = (unsigned)(index - DISPLAY_SIZES) % DISPLAY_STRIDE;
	}
	if ((size_t)which >= display->count)
		return;

	/* Nothing while a change is under way or something stands in its way. */
	free_now = display_resolution_free(app, which);
	if (!free_now)
		return;
	shown = se_resolution_marked(display->modes[which], display->mode_count[which], KL_DISPLAY_MODE_CURRENT, &current);
	if (!shown)
		memset(&current, 0, sizeof(current));

	/* Use Recommended: the display's default, tried the same way. */
	if (index >= DISPLAY_RECOMMENDED) {
		shown = se_resolution_marked(display->modes[which], display->mode_count[which], KL_DISPLAY_MODE_NATIVE, &wanted);
		if (!shown)
			return;
		display_send_resolution(app, which, &wanted, DISPLAY_VERB_RESET);
		return;
	}

	/* A refresh of the size shown, other than the one shown. */
	if (index >= DISPLAY_REFRESHES) {
		count = se_resolution_refreshes(display->modes[which], display->mode_count[which], current.width, current.height, refreshes, KL_DISPLAY_MODES_MAX);
		if (slot >= count || refreshes[slot] == current.refresh_mhz)
			return;
		wanted = current;
		wanted.refresh_mhz = refreshes[slot];
		display_send_resolution(app, which, &wanted, DISPLAY_VERB_SET);
		return;
	}

	/* A size other than the one shown, at the refresh shown when it has it (else its fastest). */
	count = se_resolution_sizes(display->modes[which], display->mode_count[which], sizes, KL_DISPLAY_MODES_MAX);
	if (slot >= count || (sizes[slot].flags & SE_RESOLUTION_CURRENT) != 0U)
		return;
	memset(&wanted, 0, sizeof(wanted));
	wanted.width = sizes[slot].width;
	wanted.height = sizes[slot].height;
	wanted.refresh_mhz = se_resolution_refresh_for(display->modes[which], display->mode_count[which], wanted.width, wanted.height, current.refresh_mhz);
	display_send_resolution(app, which, &wanted, DISPLAY_VERB_SET);
}

/*
 * Asks the desktop to try a mode on a display (or its default, for
 * DISPLAY_VERB_RESET), and opens the popup, busy until the display shows
 * the mode (the desktop answers then).
 */
static void
display_send_resolution(
	struct se_app *app,
	unsigned index,
	const struct kl_display_mode *mode,
	unsigned verb)
{
	struct se_display *display;
	const char *key;
	int shown;
	int error;

	/* The display, the mode it shows and the one tried. */
	display = &app->display;
	key = display->displays[index].key;
	(void)snprintf(display->trial_key, sizeof(display->trial_key), "%s", key);
	shown = se_resolution_marked(display->modes[index], display->mode_count[index], KL_DISPLAY_MODE_CURRENT, &display->trial_from);
	if (!shown)
		memset(&display->trial_from, 0, sizeof(display->trial_from));
	display->trial_to = *mode;

	/* Asked of the desktop. */
	if (verb == DISPLAY_VERB_RESET)
		error = kl_system_displays_reset_resolution(app->system, key, &display->resolution_request);
	else
		error = kl_system_displays_set_resolution(app->system, key, mode->width, mode->height, mode->refresh_mhz, &display->resolution_request);
	se_log("DISPLAY resolution %s key=%s width=%lu height=%lu refresh_mhz=%lu error=%d", display_verb_word(verb), key, (unsigned long)mode->width,
	    (unsigned long)mode->height, (unsigned long)mode->refresh_mhz, error);
	app->dirty = 1;
	if (error != 0) {
		display->resolution_request = 0U;
		display_resolution_message(display, kl_tr("This desktop cannot change the resolution."), 1);
		return;
	}

	/* Awaited, with the popup busy over the window while the display changes. */
	display->resolution_verb = verb;
	display->trial_dropped = 0;
	display_resolution_message(display, "", 0);
	se_dialog_open(app, display_trial_act, NULL);
	se_dialog_step(app, kl_tr("Changing the resolution"), 0U, 0U, kl_tr("The display may go dark for a moment."), NULL, 0);
	se_dialog_busy(app, kl_tr("Waiting for the display..."), 0);
}

/* Keeps the page's trial (DISPLAY_VERB_KEEP) or puts it back (DISPLAY_VERB_REVERT); the popup is gone. */
static void
display_send_trial(
	struct se_app *app,
	unsigned verb)
{
	struct se_display *display;
	int error;

	/* Asked of the desktop. */
	display = &app->display;
	display->trial_shown = 0;
	if (verb == DISPLAY_VERB_KEEP)
		error = kl_system_displays_keep_resolution(app->system, display->trial_key, &display->trial_request);
	else
		error = kl_system_displays_revert_resolution(app->system, display->trial_key, &display->trial_request);
	se_log("DISPLAY trial %s key=%s error=%d", display_verb_word(verb), display->trial_key, error);
	app->dirty = 1;
	if (error != 0) {
		display->trial_request = 0U;
		display_resolution_message(display, kl_tr("This desktop cannot change the resolution."), 1);
		return;
	}

	/* Awaited. */
	display->trial_verb = verb;
}

/*
 * Starts the popup's trial when the desktop answered that the display
 * shows the new mode: its seconds, Keep and Revert.  A popup that went
 * meanwhile (another page shown) puts the trial back at once.
 */
static void
display_trial_begin(
	struct se_app *app)
{
	struct se_display *display;
	char text[320];
	uint32_t remaining;
	unsigned state;
	unsigned why;
	int error;

	/* The popup went before the trial began: put back. */
	display = &app->display;
	if (display->trial_dropped || !app->dialog.open || app->dialog.act != display_trial_act) {
		display->trial_dropped = 0;
		display_send_trial(app, DISPLAY_VERB_REVERT);
		return;
	}

	/* The trial as the desktop tells it; one that ended already is said so. */
	state = KL_DISPLAY_RESOLUTION_NONE;
	remaining = 0U;
	why = 0U;
	error = kl_system_displays_resolution(app->system, display->trial_key, &state, &remaining, &why);
	if (error != 0 || state != KL_DISPLAY_RESOLUTION_TRIAL) {
		se_log("DISPLAY trial missed key=%s state=%u errno=%d", display->trial_key, state, error);
		se_dialog_close(app);
		display_resolution_message(display, kl_tr("The previous resolution is back."), 0);
		return;
	}

	/* The popup's step: the seconds left, Keep, and Revert in Cancel's place (Esc too). */
	display->trial_shown = 1;
	display->trial_seconds = se_resolution_seconds(remaining);
	display_trial_text(display, text, sizeof(text));
	se_dialog_step(app, kl_tr("Keep this resolution?"), 0U, 0U, text, kl_tr("Keep"), 0);
	se_dialog_cancel_word(app, kl_tr("Revert"));
	se_log("DISPLAY trial shown key=%s seconds=%u", display->trial_key, display->trial_seconds);
}

/* Hears the popup of the resolutions: Keep, Revert (or Esc, or another page), the rest ignored. */
static void
display_trial_act(
	struct se_app *app,
	unsigned action)
{
	struct se_display *display;

	/* Before the trial began only Cancel comes (another page shown): the trial is put back when it begins. */
	display = &app->display;
	if (!display->trial_shown) {
		if (action == SE_DIALOG_CANCEL) {
			display->trial_dropped = 1;
			se_dialog_close(app);
		}
		return;
	}

	/* Keep, or put back. */
	switch (action) {
	case SE_DIALOG_PRIMARY:
		se_dialog_close(app);
		display_send_trial(app, DISPLAY_VERB_KEEP);
		break;
	case SE_DIALOG_CANCEL:
		se_dialog_close(app);
		display_send_trial(app, DISPLAY_VERB_REVERT);
		break;
	default:
		break;
	}
}

/* Closes the popup when it is the resolutions' own. */
static void
display_trial_close(
	struct se_app *app)
{
	/* Another owner's popup stays. */
	app->display.trial_shown = 0;
	if (!app->dialog.open || app->dialog.act != display_trial_act)
		return;
	se_dialog_close(app);
}

/* Writes the popup's text: the mode shown now, the one that comes back, and the seconds left. */
static void
display_trial_text(
	const struct se_display *display,
	char *text,
	size_t size)
{
	char tried[64];
	char before[64];
	char seconds[16];

	/* The two modes and the seconds, in the language's words. */
	display_mode_text(&display->trial_to, tried, sizeof(tried));
	display_mode_text(&display->trial_from, before, sizeof(before));
	(void)snprintf(seconds, sizeof(seconds), "%u", display->trial_seconds);
	(void)kl_tr_format(text, size,
	    kl_trn("The display shows {1} now. Unless you keep it, {2} comes back in {3} second.", "The display shows {1} now. Unless you keep it, {2} comes back in {3} seconds.",
		(unsigned long)display->trial_seconds),
	    tried, before, seconds, (const char *)NULL);
}

/*
 * Follows the page's trial: its popup counts the seconds down and closes
 * when the desktop tells the trial ended (its time ran out, or something
 * else put it back); the card's line of a trial is drawn again each second.
 */
static void
display_trial_follow(
	struct se_app *app)
{
	struct se_display *display;
	char text[320];
	uint32_t remaining;
	unsigned seconds;
	unsigned state;
	unsigned why;
	int trial;
	int error;

	/* The popup's trial. */
	display = &app->display;
	if (display->trial_shown) {
		state = KL_DISPLAY_RESOLUTION_NONE;
		remaining = 0U;
		why = 0U;
		error = kl_system_displays_resolution(app->system, display->trial_key, &state, &remaining, &why);
		if (error != 0 || state != KL_DISPLAY_RESOLUTION_TRIAL) {
			/* It ended: the old mode is back. */
			se_log("DISPLAY trial ended key=%s state=%u errno=%d", display->trial_key, state, error);
			display_trial_close(app);
			display_resolution_message(display, kl_tr("The previous resolution is back."), 0);
			app->dirty = 1;
		} else {
			/* Its seconds, when they changed. */
			seconds = se_resolution_seconds(remaining);
			if (seconds != display->trial_seconds) {
				display->trial_seconds = seconds;
				display_trial_text(display, text, sizeof(text));
				se_dialog_text_set(app, text);
				se_log("DISPLAY trial seconds=%u", seconds);
			}
		}
	}

	/* The card's line of a trial, while the page is shown. */
	if (app->page != SE_PAGE_DISPLAY)
		return;
	seconds = 0U;
	trial = display_trial_remaining(app, &remaining);
	if (trial)
		seconds = se_resolution_seconds(remaining);
	if (seconds != display->card_seconds) {
		display->card_seconds = seconds;
		app->dirty = 1;
	}
}

/*
 * Finds a trial the page shows (its popup's, else any display's): the
 * time it has left.  Returns 1 with it, 0 when no display is in a trial.
 */
static int
display_trial_remaining(
	const struct se_app *app,
	uint32_t *remaining)
{
	const struct se_display *display;
	unsigned state;
	unsigned why;
	size_t index;
	int error;

	/* Nothing without the desktop's system. */
	display = &app->display;
	if (app->system == NULL)
		return 0;

	/* The popup's trial first. */
	if (display->trial_shown) {
		error = kl_system_displays_resolution(app->system, display->trial_key, &state, remaining, &why);
		if (error == 0 && state == KL_DISPLAY_RESOLUTION_TRIAL)
			return 1;
	}

	/* Any display's. */
	for (index = 0U; index < display->count; index++) {
		if (display->resolution_state[index] != KL_DISPLAY_RESOLUTION_TRIAL)
			continue;
		error = kl_system_displays_resolution(app->system, display->displays[index].key, &state, remaining, &why);
		if (error == 0 && state == KL_DISPLAY_RESOLUTION_TRIAL)
			return 1;
	}

	/* No trial. */
	return 0;
}

/* Writes a mode as the page says it ("1920 x 1080, 60 Hz"). */
static void
display_mode_text(
	const struct kl_display_mode *mode,
	char *text,
	size_t size)
{
	char hertz[SE_RESOLUTION_HERTZ_MAX];

	/* A mode not known is a dash. */
	if (mode->width == 0U || mode->height == 0U) {
		(void)snprintf(text, size, "-");
		return;
	}

	/* The size and the refresh. */
	se_resolution_hertz(mode->refresh_mhz, hertz, sizeof(hertz));
	(void)snprintf(text, size, "%lu x %lu, %s", (unsigned long)mode->width, (unsigned long)mode->height, hertz);
}

/* Says why a resolution was not tried (the desktop's answer to a set or a reset). */
static void
display_resolution_said(
	struct se_display *display,
	int error)
{
	/* Each answer's sentence. */
	switch (error) {
	case EIO:
		display_resolution_message(display, kl_tr("The display could not show that resolution. The previous one is back."), 1);
		break;
	case ENOTSUP:
		display_resolution_message(display, kl_tr("This display cannot show that resolution."), 1);
		break;
	case ENOBUFS:
		display_resolution_message(display, kl_tr("The resolution can be changed while this display is the only one shown."), 1);
		break;
	case EBUSY:
		display_resolution_message(display, kl_tr("The displays are being changed, or a game is shown. Try again later."), 1);
		break;
	case ENODEV:
		display_resolution_message(display, kl_tr("The display is not connected."), 1);
		break;
	case EPERM:
		display_resolution_message(display, kl_tr("The displays can be changed only in an unlocked session."), 1);
		break;
	default:
		display_resolution_message(display, kl_tr("The resolution could not be changed."), 1);
		break;
	}
}

/* Keeps the resolutions' last message. */
static void
display_resolution_message(
	struct se_display *display,
	const char *text,
	int bad)
{
	/* The text and whether it tells of a failure. */
	(void)snprintf(display->resolution_message, sizeof(display->resolution_message), "%s", text);
	display->resolution_bad = bad;
}

/* Gives the log's word of what the page asked of a resolution. */
static const char *
display_verb_word(
	unsigned verb)
{
	/* Each verb's word. */
	switch (verb) {
	case DISPLAY_VERB_SET:
		return "set";
	case DISPLAY_VERB_RESET:
		return "reset";
	case DISPLAY_VERB_KEEP:
		return "keep";
	case DISPLAY_VERB_REVERT:
		return "revert";
	default:
		break;
	}

	/* Not a verb. */
	return "none";
}
