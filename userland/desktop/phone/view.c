/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Phone's view (WS170 p000; phone.h): the contacts at the left -- a search
 * field over a list of people, each with the picture of their initials,
 * the latest item and its time -- and at the right the chosen person's
 * timeline in the way of iOS's Messages: a header with the name, the
 * number and a call button, the items oldest first (messages in bubbles,
 * the person's grey at the left and one's own at the right, blue for RCS
 * and green for the carrier's SMS and MMS; calls, pictures and files on
 * cards; each day under its date), and at the bottom the field to write
 * a message in, with buttons to attach and to send.
 *
 * A window narrower than PH_VIEW_NARROW shows either the contacts or the
 * timeline, with a back button on the timeline.
 *
 * Sending, calling and attaching show for a while that there is no
 * backend, and log a "PHONE NOBACKEND" line for the tests.
 */

#include "phone.h"

#include <stdio.h>
#include <string.h>

/* The width under which the window shows the contacts or the timeline, not both. */
#define PH_VIEW_NARROW		680

/* The contacts' column: its width's share of the window and its limits, the title's band, a row's height. */
#define PH_VIEW_SIDEBAR_SHARE	0.34
#define PH_VIEW_SIDEBAR_MIN	260
#define PH_VIEW_SIDEBAR_MAX	340
#define PH_VIEW_TITLE		96
#define PH_VIEW_ROW		68

/*
 * The cards on glass: the margin round them, the gap between, their corner.
 * As Settings has them (BUG-218): no margin, the window's own edges and the
 * titlebar's gap being the space round them, and the titlebar's gap between.
 */
#define PH_VIEW_MARGIN		0
#define PH_VIEW_GAP		8
#define PH_VIEW_CARD_RADIUS	16.0f

/* The timeline's header, the field's band, a bubble's margins and corner, and its widest share. */
#define PH_VIEW_HEADER		64
#define PH_VIEW_COMPOSER	60
#define PH_VIEW_PAD_X		12
#define PH_VIEW_PAD_Y		8
#define PH_VIEW_SIDE		16
#define PH_VIEW_RADIUS		14.0f
#define PH_VIEW_TAIL		4.0f
#define PH_VIEW_BUBBLE_SHARE	0.64
#define PH_VIEW_BUBBLE_MAX	520
#define PH_VIEW_BUBBLE_MIN	160

/* The text sizes: the title, a name, a message, a detail. */
#define PH_VIEW_TEXT_TITLE	22U
#define PH_VIEW_TEXT_NAME	14U
#define PH_VIEW_TEXT_HEADER	16U
#define PH_VIEW_TEXT_BODY	14U
#define PH_VIEW_TEXT_SMALL	12U

/* The most lines of a bubble, and the most contacts shown. */
#define PH_VIEW_LINES_MAX	32U
#define PH_VIEW_CONTACTS_MAX	64U

/* How long a notice shows. */
#define PH_VIEW_NOTICE_US	4000000U

/* The widgets' ids. */
#define PH_ID_CONTACT		1U
#define PH_ID_SEARCH		2U
#define PH_ID_CONTACTS		3U
#define PH_ID_TIMELINE		4U
#define PH_ID_CALL		5U
#define PH_ID_SEND		6U
#define PH_ID_MESSAGE		7U
#define PH_ID_ATTACH		8U
#define PH_ID_BACK		9U
#define PH_ID_ADD		10U
#define PH_ID_NEW_NAME		11U
#define PH_ID_NEW_NUMBER	12U
#define PH_ID_SAVE		13U
#define PH_ID_CANCEL		14U

/*
 * The colors: the person's bubbles and cards (on glass, see-through
 * white), one's own over RCS (the accent) and over the carrier (teal), a
 * call's card, and the contacts' ground on an opaque window.
 */
#define PH_COLOR_INCOMING	kl_theme_choose(KL_RGB(0xedf1f6), KL_RGB(0x2f3540))
#define PH_COLOR_INCOMING_GLASS	kl_theme_choose(KL_RGBA(0xffffff, 205), KL_RGBA(0x2f3540, 205))
#define PH_COLOR_RCS		KL_RGB(0x2f7cf6)
#define PH_COLOR_SMS		KL_RGB(0x0f9d8a)
#define PH_COLOR_CARD		kl_theme_choose(KL_RGB(0xf7f8fa), KL_RGB(0x2a2f38))
#define PH_COLOR_CARD_GLASS	kl_theme_choose(KL_RGBA(0xffffff, 170), KL_RGBA(0x2a2f38, 170))
#define PH_COLOR_SIDEBAR	kl_theme_choose(KL_RGB(0xf4f6f9), KL_RGB(0x1f232a))
#define PH_COLOR_WHITE		KL_RGB(0xffffff)
#define PH_COLOR_SURFACE	kl_theme_choose(KL_RGB(0xffffff), KL_RGB(0x23272f))

/* The handset's outline in an 18-pixel box (Material's "call"). */
static const float ph_view_handset[] = {
	17.01f, 12.38f, 16.40f, 12.36f, 15.79f, 12.31f, 15.20f, 12.23f, 14.62f, 12.13f, 14.04f, 11.99f,
	13.48f, 11.82f, 12.47f, 12.06f, 10.90f, 14.03f, 9.50f, 13.26f, 8.17f, 12.31f, 6.92f, 11.21f,
	5.79f, 9.97f, 4.81f, 8.63f, 4.01f, 7.20f, 5.96f, 5.54f, 6.08f, 5.39f, 6.17f, 5.23f,
	6.22f, 5.06f, 6.25f, 4.88f, 6.24f, 4.70f, 6.20f, 4.52f, 6.03f, 3.96f, 5.89f, 3.38f,
	5.78f, 2.80f, 5.70f, 2.21f, 5.66f, 1.60f, 5.64f, 0.99f, 5.60f, 0.73f, 5.50f, 0.49f,
	5.35f, 0.29f, 5.15f, 0.14f, 4.91f, 0.04f, 4.65f, 0.00f, 1.19f, 0.00f, 0.91f, 0.02f,
	0.64f, 0.09f, 0.39f, 0.21f, 0.19f, 0.40f, 0.05f, 0.66f, 0.00f, 0.99f, 0.62f, 5.48f,
	2.35f, 9.53f, 5.03f, 12.98f, 8.48f, 15.65f, 12.53f, 17.39f, 17.01f, 18.00f, 17.33f, 17.95f,
	17.58f, 17.82f, 17.77f, 17.62f, 17.90f, 17.37f, 17.98f, 17.10f, 18.00f, 16.82f, 18.00f, 13.37f,
	17.96f, 13.11f, 17.86f, 12.87f, 17.71f, 12.67f, 17.51f, 12.52f, 17.27f, 12.42f
};

static void view_layout(const struct ph_view *view, int width, int height, struct kl_rect *sidebar, struct kl_rect *conversation);
static size_t view_filtered(const struct ph_view *view, size_t *indices, size_t size);
static int view_contains(const char *text, const char *part);
static int view_lower(int c);
static void view_request(struct ph_view *view, unsigned action, long contact);
static void view_new_contact(struct ph_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_sidebar(struct ph_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_row(const struct kl_style *style, const struct ph_contact *contact, const struct kl_rect *row, int selected, int unread);
static void view_avatar(const struct kl_style *style, const struct ph_contact *contact, int cx, int cy, int radius);
static void view_conversation(struct ph_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_header(struct ph_view *view, struct kl_ui *ui, const struct kl_style *style, const struct ph_contact *contact, const struct kl_rect *area, uint64_t now_us);
static void view_timeline(struct ph_view *view, struct kl_ui *ui, const struct kl_style *style, const struct ph_contact *contact, const struct kl_rect *area, uint64_t now_us);
static int view_item(const struct kl_style *style, const struct ph_item *item, const struct ph_item *previous, int x, int y, int width, int draw);
static int view_text(const struct kl_style *style, const struct ph_item *item, int x, int y, int width, int draw);
static int view_call(const struct kl_style *style, const struct ph_item *item, int x, int y, int width, int draw);
static int view_photo(const struct kl_style *style, const struct ph_item *item, int x, int y, int width, int draw);
static int view_file(const struct kl_style *style, const struct ph_item *item, int x, int y, int width, int draw);
static void view_composer(struct ph_view *view, struct kl_ui *ui, const struct kl_style *style, const struct ph_contact *contact, const struct kl_rect *area, uint64_t now_us);
static void view_empty(const struct kl_style *style, const struct kl_rect *area, const char *title, const char *line);
static void view_handset(struct kl_canvas *canvas, float x, float y, float size, kl_color color);
static void view_centred(const struct kl_style *style, int cx, int baseline, const char *text, unsigned pixels, int bold, kl_color color);
static const char *view_channel_name(enum ph_channel channel);
static const char *view_preview(const struct ph_item *item);
static const struct ph_item *view_last_message(const struct ph_contact *contact);

/*
 * Makes the view's state: the fields empty, the scrolls at the top, the
 * first contact shown.
 */
int
ph_view_init(
	struct ph_view *view)
{
	int error;

	/* Nothing yet. */
	memset(view, 0, sizeof(view[0]));
	view->selected = -1;

	/* The contacts' scroll, down only. */
	error = kl_scroll_init(&view->contacts_scroll, KL_SCROLL_Y);
	if (error != 0)
		return error;

	/* The timeline's scroll, down only. */
	error = kl_scroll_init(&view->timeline_scroll, KL_SCROLL_Y);
	if (error != 0) {
		kl_scroll_release(&view->contacts_scroll);
		return error;
	}

	/* Succeeded: the first contact is shown (a narrow window shows the list first). */
	ph_view_select(view, 0);
	return 0;
}

/*
 * Frees what the view's state holds.
 */
void
ph_view_release(
	struct ph_view *view)
{
	/* Unsent temporary pictures and retained paths belong to the view. */
	ph_draft_release(view);

	/* The scrolls. */
	kl_scroll_release(&view->timeline_scroll);
	kl_scroll_release(&view->contacts_scroll);
}

/*
 * Shows a contact's timeline: at its end, its messages read, the field
 * emptied.
 */
void
ph_view_select(
	struct ph_view *view,
	long index)
{
	const struct ph_contact *contacts;
	size_t count;

	/* A contact that is not there. */
	contacts = ph_contacts(&count);
	if (index < 0 || (size_t)index >= count)
		return;

	/* Shown from its end, with the field emptied. */
	view->selected = index;
	view->attachment_page = 0U;
	view->to_end = 1;
	view->adding = 0;
	kl_field_set(&view->message, "");
	ph_log("SELECT contact=%ld", index);

	/* Its messages read: the window marks them so. */
	if (contacts[index].unread != 0U)
		view_request(view, PH_ACTION_READ, index);
}

/*
 * Carries out an action of the menu, a key or a button: sending, calling
 * and attaching show that there is no backend; Quit ends the program.
 */
void
ph_view_action(
	struct ph_view *view,
	unsigned action,
	uint64_t now_us)
{
	/* Each action. */
	switch (action) {
	case PH_ACTION_SEND:
		/* Nothing written, or nobody to send to: nothing to send. */
		if (view->selected < 0)
			break;

		/* The paired phone takes no text to send (ws197-p004b). */
		if (view->cannot_send) {
			ph_view_notice(view, "The phone does not take texts to send.", now_us);
			break;
		}

		/* Asked of the window. */
		view_request(view, PH_ACTION_SEND, view->selected);
		ph_log("REQUEST action=send contact=%ld length=%zu", view->selected, view->message.length);
		break;
	case PH_ACTION_CALL:
		/* The contact shown, called by the window. */
		if (view->selected < 0)
			break;
		view_request(view, PH_ACTION_CALL, view->selected);
		ph_log("REQUEST action=call contact=%ld", view->selected);
		break;
	case PH_ACTION_ATTACH:
		/* Selects media without sending or recording a timeline item. */
		if (view->selected >= 0)
			view_request(view, PH_ACTION_ATTACH, view->selected);
		break;
	case PH_ACTION_ADD:
		/* The form of a new contact, in place of the timeline. */
		view->adding = 1;
		view->opened = 1;
		kl_field_set(&view->new_name, "");
		kl_field_set(&view->new_number, "");
		ph_log("ADD open");
		break;
	case PH_ACTION_SAVE:
		/* The new contact, for the window to keep (a number is needed). */
		if (view->new_number.length == 0U) {
			ph_view_notice(view, "Write the contact's number.", now_us);
			break;
		}

		/* Asked of the window. */
		view_request(view, PH_ACTION_SAVE, -1);
		ph_log("REQUEST action=save name=%zu number=%zu", view->new_name.length, view->new_number.length);
		break;
	case PH_ACTION_CANCEL:
		view->adding = 0;
		ph_log("ADD cancel");
		break;
	case PH_ACTION_SYNC:
		/* Requests a complete sync without changing the selected conversation. */
		view_request(view, PH_ACTION_SYNC, -1);
		break;
	case PH_ACTION_QUIT:
		view->quit = 1;
		break;
	default:
		break;
	}
}

/*
 * Takes a key no widget took: Up and Down move through the contacts, Enter
 * opens the one chosen and Esc goes back to the list in a narrow window.
 */
void
ph_view_key(
	struct ph_view *view,
	uint32_t key,
	unsigned modifiers,
	uint64_t now_us)
{
	size_t indices[PH_VIEW_CONTACTS_MAX];
	size_t count;
	size_t at;
	size_t i;

	/* The keys with a modifier are the menu's. */
	(void)now_us;
	if ((modifiers & (KL_MOD_CTRL | KL_MOD_ALT | KL_MOD_SUPER)) != 0U)
		return;

	/* Esc goes back to the list of a narrow window. */
	if (key == KL_KEY_ESC) {
		view->opened = 0;
		return;
	}

	/* Enter opens the timeline of a narrow window. */
	if (key == KL_KEY_ENTER || key == KL_KEY_KPENTER) {
		view->opened = 1;
		return;
	}

	/* Up and Down alone move, through the contacts the filter shows. */
	count = view_filtered(view, indices, PH_VIEW_CONTACTS_MAX);
	if ((key != KL_KEY_UP && key != KL_KEY_DOWN) || count == 0U)
		return;

	/* Where the contact shown is among them (the first when it is not). */
	at = 0;
	for (i = 0; i < count; i++) {
		/* The contact shown. */
		if ((long)indices[i] == view->selected)
			at = i;
	}

	/* One up or down, within the list. */
	if (key == KL_KEY_UP && at > 0U)
		at--;
	else if (key == KL_KEY_DOWN && at + 1U < count)
		at++;
	ph_view_select(view, (long)indices[at]);
}

/*
 * Draws a frame of the view in a window of a size, between the caller's
 * kl_ui_begin and kl_ui_end, and takes what the input did to its widgets.
 */
void
ph_view_draw(
	struct ph_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	int width,
	int height,
	uint64_t now_us)
{
	struct kl_rect whole;
	struct kl_rect sidebar;
	struct kl_rect conversation;

	/* The ground: clear on glass (the desktop shows between the cards), else white. */
	whole.x = 0;
	whole.y = 0;
	whole.width = width;
	whole.height = height;
	if (view->glass) {
		kl_canvas_clear(style->canvas);
	} else {
		kl_canvas_fill(style->canvas, &whole, PH_COLOR_SURFACE);
	}

	/* Where the contacts and the timeline go (a narrow window shows one of them). */
	view->narrow = 0;
	if (width < PH_VIEW_NARROW)
		view->narrow = 1;
	view_layout(view, width, height, &sidebar, &conversation);

	/* On glass, each stands on a card of its own. */
	if (view->glass && sidebar.width > 0)
		kl_canvas_round(style->canvas, (float)sidebar.x, (float)sidebar.y, (float)sidebar.width, (float)sidebar.height, PH_VIEW_CARD_RADIUS, style->theme->glass_sidebar);
	if (view->glass && conversation.width > 0)
		kl_canvas_round(style->canvas, (float)conversation.x, (float)conversation.y, (float)conversation.width, (float)conversation.height, PH_VIEW_CARD_RADIUS, style->theme->glass_content);

	/* The contacts and the timeline, those shown. */
	if (sidebar.width > 0)
		view_sidebar(view, ui, style, &sidebar, now_us);
	if (conversation.width > 0)
		view_conversation(view, ui, style, &conversation, now_us);

	/* The notice over the bottom of the timeline (or of the window) while it shows. */
	if (view->notice != NULL && now_us < view->notice_until) {
		/* The timeline's card, or the window when it is not shown. */
		if (conversation.width <= 0)
			conversation = whole;

		/* The chip in the middle of it, above the field. */
		kl_chip(style, conversation.x + conversation.width / 2, conversation.y + conversation.height - PH_VIEW_COMPOSER - 14, view->notice);
	}
}

/*
 * Lists the parts of the view that stand on the compositor's glass (the cards
 * of the contacts and of the timeline) for a window of a size, into up to
 * capacity panels; returns how many there are.
 */
size_t
ph_view_panels(
	struct ph_view *view,
	int width,
	int height,
	struct kl_glass_panel *panels,
	size_t capacity)
{
	struct kl_rect cards[2];
	size_t count;
	size_t i;

	/* The two cards, as the frame draws them. */
	view_layout(view, width, height, &cards[0], &cards[1]);
	count = 0;
	for (i = 0; i < 2U && count < capacity; i++) {
		/* A card not shown has no panel. */
		if (cards[i].width <= 0 || cards[i].height <= 0)
			continue;

		/* The card's panel. */
		memset(&panels[count], 0, sizeof(panels[count]));
		panels[count].x = cards[i].x;
		panels[count].y = cards[i].y;
		panels[count].width = cards[i].width;
		panels[count].height = cards[i].height;
		panels[count].radius = (int32_t)PH_VIEW_CARD_RADIUS;
		panels[count].kind = KL_GLASS_CARD;
		count++;
	}

	/* The panels listed. */
	return count;
}

/*
 * Reports how long the window may wait for input (ms) before the next
 * frame is due by itself: until the notice goes, or -1 for no time.
 */
int
ph_view_wait(
	const struct ph_view *view,
	uint64_t now_us)
{
	uint64_t left;

	/* No notice: only input changes the view. */
	if (view->notice == NULL || now_us >= view->notice_until)
		return -1;

	/* The notice's time left, rounded up. */
	left = view->notice_until - now_us;
	return (int)(left / 1000U) + 1;
}

/*
 * Lays out the contacts and the timeline in a window of a size: side by
 * side, the contacts a third of the width within limits, or in a narrow
 * window the one shown across it (the other zero wide); on glass, as
 * cards with a margin round them and a gap between.
 */
static void
view_layout(
	const struct ph_view *view,
	int width,
	int height,
	struct kl_rect *sidebar,
	struct kl_rect *conversation)
{
	int margin;
	int gap;
	int share;

	/* The margin and the gap: only cards on glass have them. */
	margin = 0;
	gap = 0;
	if (view->glass) {
		margin = PH_VIEW_MARGIN;
		gap = PH_VIEW_GAP;
	}

	/* Both across the window within the margin, to begin with. */
	sidebar->x = margin;
	sidebar->y = margin;
	sidebar->width = width - 2 * margin;
	sidebar->height = height - 2 * margin;
	*conversation = *sidebar;

	/* A narrow window: the one shown alone. */
	if (view->narrow) {
		if (view->opened)
			sidebar->width = 0;
		else
			conversation->width = 0;
		return;
	}

	/* The contacts at the left, a third of the window within limits; the timeline the rest. */
	share = (int)((double)width * PH_VIEW_SIDEBAR_SHARE);
	if (share < PH_VIEW_SIDEBAR_MIN)
		share = PH_VIEW_SIDEBAR_MIN;
	else if (share > PH_VIEW_SIDEBAR_MAX)
		share = PH_VIEW_SIDEBAR_MAX;
	sidebar->width = share - margin;
	conversation->x = share + gap;
	conversation->width = width - share - gap - margin;
}

/*
 * Finds the contacts the search field's words match (all of them while it
 * is empty) and reports how many.
 */
static size_t
view_filtered(
	const struct ph_view *view,
	size_t *indices,
	size_t size)
{
	const struct ph_contact *contacts;
	size_t found;
	size_t count;
	size_t i;
	int name;
	int number;

	/* Each contact whose name or number holds the words. */
	contacts = ph_contacts(&count);
	found = 0;
	for (i = 0; i < count && found < size; i++) {
		/* The name, then the number. */
		name = view_contains(contacts[i].name, view->search.text);
		number = view_contains(contacts[i].number, view->search.text);
		if (name || number) {
			indices[found] = i;
			found++;
		}
	}

	/* The number found. */
	return found;
}

/*
 * Reports whether a text holds a part, the case of ASCII letters ignored
 * (an empty part is in every text).
 */
static int
view_contains(
	const char *text,
	const char *part)
{
	size_t length;
	size_t i;
	size_t j;
	int a;
	int b;

	/* Each place the part could start. */
	length = strlen(part);
	for (i = 0; text[i] != '\0' || length == 0U; i++) {
		/* The bytes from there, matched one by one. */
		for (j = 0; j < length; j++) {
			/* The two bytes, in lower case; a difference ends the match from there. */
			a = view_lower((unsigned char)text[i + j]);
			b = view_lower((unsigned char)part[j]);
			if (a != b)
				break;
		}

		/* Every byte of the part matched. */
		if (j == length)
			return 1;
	}

	/* Not held. */
	return 0;
}

/*
 * Reports a byte with an ASCII capital made small.
 */
static int
view_lower(
	int c)
{
	/* A capital, made small. */
	if (c >= 'A' && c <= 'Z')
		return c - 'A' + 'a';

	/* Anything else as it is. */
	return c;
}

/*
 * Shows a notice for a while (its words copied).
 */
void
ph_view_notice(
	struct ph_view *view,
	const char *message,
	uint64_t now_us)
{
	/* The words and until when. */
	(void)snprintf(view->notice_text, sizeof(view->notice_text), "%s", message);
	view->notice = view->notice_text;
	view->notice_until = now_us + PH_VIEW_NOTICE_US;
}

/*
 * Takes the oldest request the view queued: 1 with it, 0 when none waits.
 */
int
ph_view_take_request(
	struct ph_view *view,
	struct ph_request *request)
{
	size_t index;

	/* None. */
	if (view->request_count == 0U)
		return 0;

	/* The first, the rest moved up. */
	*request = view->requests[0];
	for (index = 1; index < view->request_count; index++)
		view->requests[index - 1U] = view->requests[index];
	view->request_count--;
	return 1;
}

/* Queues a request for the window (a full queue drops it: the user asks again). */
static void
view_request(
	struct ph_view *view,
	unsigned action,
	long contact)
{
	/* No room. */
	if (view->request_count == PH_REQUESTS_MAX)
		return;

	/* At the end. */
	view->requests[view->request_count].action = action;
	view->requests[view->request_count].contact = contact;
	view->request_count++;
}

/* Draws the form of a new contact: its name and number, Save and Cancel. */
static void
view_new_contact(
	struct ph_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	struct kl_rect field;
	struct kl_rect button;
	int clicked;
	int width;
	int x;
	int y;

	/* The title. */
	width = area->width - 64;
	if (width > 420)
		width = 420;
	x = area->x + (area->width - width) / 2;
	y = area->y + 70;
	(void)kl_text_draw(style->text, style->canvas, x, y, "New Contact", strlen("New Contact"), PH_VIEW_TEXT_TITLE, 1, style->theme->text);

	/* The name. */
	y += 24;
	(void)kl_text_draw(style->text, style->canvas, x, y + 21, "Name", strlen("Name"), PH_VIEW_TEXT_BODY, 0, style->theme->text_secondary);
	field.x = x + 90;
	field.y = y;
	field.width = width - 90;
	field.height = 32;
	(void)kl_field(ui, style, PH_ID_NEW_NAME, &field, &view->new_name, "Their name");

	/* The number. */
	y += 42;
	(void)kl_text_draw(style->text, style->canvas, x, y + 21, "Number", strlen("Number"), PH_VIEW_TEXT_BODY, 0, style->theme->text_secondary);
	field.y = y;
	(void)kl_field(ui, style, PH_ID_NEW_NUMBER, &field, &view->new_number, "+81 90 1234 5678");

	/* Save, and Cancel before it. */
	y += 52;
	button.width = 90;
	button.height = 34;
	button.x = x + width - button.width;
	button.y = y;
	clicked = kl_button(ui, style, PH_ID_SAVE, &button, "Save", KL_BUTTON_PRIMARY);
	if (clicked)
		ph_view_action(view, PH_ACTION_SAVE, now_us);
	button.x -= button.width + 8;
	clicked = kl_button(ui, style, PH_ID_CANCEL, &button, "Cancel", 0U);
	if (clicked)
		ph_view_action(view, PH_ACTION_CANCEL, now_us);
}

/*
 * Draws the contacts' column: the title, the search field and the list.
 */
static void
view_sidebar(
	struct ph_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	const struct ph_contact *contacts;
	size_t indices[PH_VIEW_CONTACTS_MAX];
	struct kl_rect field;
	struct kl_rect list;
	struct kl_rect edge;
	struct kl_rect row;
	struct kl_rect button;
	unsigned hit;
	unsigned changes;
	size_t shown;
	size_t total;
	size_t i;
	int selected;
	int unread;
	int clicked;

	/* The column's ground and its edge against the timeline (on glass, its card is drawn already). */
	if (!view->glass) {
		kl_canvas_fill(style->canvas, area, PH_COLOR_SIDEBAR);
		edge = *area;
		edge.x = area->x + area->width - 1;
		edge.width = 1;
		kl_canvas_fill(style->canvas, &edge, style->theme->separator);
	}

	/* The title. */
	(void)kl_text_draw(style->text, style->canvas, area->x + 20, area->y + 38, "Phone", strlen("Phone"), PH_VIEW_TEXT_TITLE, 1, style->theme->text);

	/* A new contact: "+" at the right of the title. */
	button.width = 32;
	button.height = 32;
	button.x = area->x + area->width - 16 - button.width;
	button.y = area->y + 14;
	clicked = kl_button(ui, style, PH_ID_ADD, &button, "+", 0U);
	if (clicked)
		ph_view_action(view, PH_ACTION_ADD, now_us);

	/* The search field; what it holds filters the list. */
	field.x = area->x + 16;
	field.y = area->y + 52;
	field.width = area->width - 32;
	field.height = 32;
	changes = kl_field(ui, style, PH_ID_SEARCH, &field, &view->search, "Search");
	if ((changes & KL_FIELD_CHANGED) != 0U)
		kl_scroll_move_to(&view->contacts_scroll, 0.0, 0.0, 0, now_us);

	/* The list's viewport, which scrolls. */
	contacts = ph_contacts(&total);
	shown = view_filtered(view, indices, PH_VIEW_CONTACTS_MAX);
	list.x = area->x;
	list.y = area->y + PH_VIEW_TITLE;
	list.width = area->width - 1;
	list.height = area->height - PH_VIEW_TITLE;
	kl_scroll_set_size(&view->contacts_scroll, (double)list.width, (double)shown * PH_VIEW_ROW + 8.0, (double)list.width, (double)list.height);
	kl_ui_scroll_region(ui, PH_ID_CONTACTS, &list, &view->contacts_scroll);
	kl_canvas_clip_push(style->canvas, &list);

	/* Each row: a click shows the contact (and opens it in a narrow window). */
	for (i = 0; i < shown; i++) {
		/* The row, inset. */
		row.x = list.x + 8;
		row.y = list.y + (int)i * PH_VIEW_ROW - (int)view->contacts_scroll.y;
		row.width = list.width - 16;
		row.height = PH_VIEW_ROW - 4;

		/* A row out of the viewport is not drawn. */
		if (row.y + row.height < list.y || row.y > list.y + list.height)
			continue;

		/* Its input. */
		hit = kl_ui_hit(ui, PH_ID_CONTACT, (uint32_t)indices[i], &row);
		if ((hit & KL_HIT_CLICKED) != 0U) {
			ph_view_select(view, (long)indices[i]);
			view->opened = 1;
		}

		/* The ground under the pointer. */
		if ((hit & KL_HIT_HOT) != 0U)
			kl_canvas_round(style->canvas, (float)row.x, (float)row.y, (float)row.width, (float)row.height, 10.0f, style->theme->hover);

		/* Chosen: the chosen row is not marked in a narrow window, where the list stands alone. */
		selected = 0;
		if ((long)indices[i] == view->selected && !view->narrow)
			selected = 1;

		/* Messages not read (the window marks them read when the timeline is shown). */
		unread = 0;
		if (contacts[indices[i]].unread != 0U)
			unread = 1;

		/* The row's content. */
		view_row(style, &contacts[indices[i]], &row, selected, unread);
	}

	/* The rows' clip goes, and the bar shows while the list moves. */
	kl_canvas_clip_pop(style->canvas);
	(void)kl_scroll_draw_bars(&view->contacts_scroll, style->canvas, &list, style->theme, now_us);

	/* Nothing matched. */
	if (shown == 0U)
		view_centred(style, area->x + area->width / 2, list.y + 40, "No contacts match.", PH_VIEW_TEXT_BODY, 0, style->theme->text_faint);
}

/*
 * Draws one contact's row: the picture, the name, the time of the latest
 * item and a line of it; a dot when there are messages not read.
 */
static void
view_row(
	const struct kl_style *style,
	const struct ph_contact *contact,
	const struct kl_rect *row,
	int selected,
	int unread)
{
	const struct ph_item *last;
	const char *when;
	const char *line;
	char preview[1024];
	size_t preview_length;
	kl_color ink;
	kl_color soft;
	int time_width;
	int preview_width;
	int left;
	int today;

	/* The colors of the words. */
	ink = style->theme->text;
	soft = style->theme->text_secondary;

	/* The selection under a chosen row, as Files shows it. */
	if (selected)
		kl_canvas_round(style->canvas, (float)row->x, (float)row->y, (float)row->width, (float)row->height, 10.0f, style->theme->selection);

	/* The dot of messages not read. */
	if (unread)
		kl_canvas_circle(style->canvas, (float)row->x + 7.0f, (float)row->y + (float)row->height / 2.0f, 4.0f, style->theme->accent);

	/* The picture. */
	view_avatar(style, contact, row->x + 14 + 22, row->y + row->height / 2, 22);

	/* The latest item, and a line of it ("No messages yet" without one). */
	left = row->x + 70;
	time_width = 0;
	line = "No messages yet";
	if (contact->item_count != 0U) {
		/* The latest item's line and the day it happened. */
		last = &contact->items[contact->item_count - 1U];
		line = view_preview(last);
		when = last->day;
		today = strcmp(last->day, "Today");

		/* Today's shows its time instead. */
		if (today == 0)
			when = last->time;

		/* The time at the right. */
		time_width = kl_text_width(style->text, when, strlen(when), PH_VIEW_TEXT_SMALL, 0);
		(void)kl_text_draw(style->text, style->canvas, row->x + row->width - 12 - time_width, row->y + 26, when, strlen(when), PH_VIEW_TEXT_SMALL, 0, soft);
	}

	/* Identifies a read-only name imported from the paired phone. */
	if (contact->phone_named) {
		(void)kl_text_draw(style->text, style->canvas, row->x + row->width - 45, row->y + 47, "Phone", 5U, PH_VIEW_TEXT_SMALL, 0, soft);
	}

	/* Reserves space for the imported-name label in the preview line. */
	preview_width = row->width - 82;
	if (contact->phone_named)
		preview_width -= 45;

	/* Shows only the first logical line, with a complete UTF-8 prefix. */
	if (line == NULL)
		line = "";
	preview_length = strcspn(line, "\r\n");
	if (preview_length >= sizeof(preview)) {
		preview_length = sizeof(preview) - 1U;
		while (preview_length > 0U && ((unsigned char)line[preview_length] & 0xc0U) == 0x80U)
			preview_length--;
	}

	/* Copies the bounded first line into the fitting renderer. */
	memcpy(preview, line, preview_length);
	preview[preview_length] = '\0';

	/* The name, and the line under it. */
	(void)kl_text_draw_fit(style->text, style->canvas, left, row->y + 26, contact->name, PH_VIEW_TEXT_NAME, 1, row->width - 70 - time_width - 20, ink);
	(void)kl_text_draw_fit(style->text, style->canvas, left, row->y + 47, preview, PH_VIEW_TEXT_BODY - 1U, 0, preview_width, soft);
}

/*
 * Draws a contact's picture: a circle of its color with its initials.
 */
static void
view_avatar(
	const struct kl_style *style,
	const struct ph_contact *contact,
	int cx,
	int cy,
	int radius)
{
	unsigned pixels;
	int width;
	int baseline;

	/* The circle. */
	kl_canvas_circle(style->canvas, (float)cx, (float)cy, (float)radius, contact->color);

	/* The initials in the middle. */
	pixels = (unsigned)(radius * 7 / 10);
	width = kl_text_width(style->text, contact->initials, strlen(contact->initials), pixels, 1);
	baseline = kl_text_center(pixels, cy - radius, 2 * radius);
	(void)kl_text_draw(style->text, style->canvas, cx - width / 2, baseline, contact->initials, strlen(contact->initials), pixels, 1, PH_COLOR_WHITE);
}

/*
 * Draws the timeline's side: the header, the items and the field, or a
 * word when no contact is shown.
 */
static void
view_conversation(
	struct ph_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	const struct ph_contact *contacts;
	const struct ph_contact *contact;
	struct kl_rect header;
	struct kl_rect timeline;
	struct kl_rect composer;
	size_t count;

	int composer_height;
	size_t draft_count;

	/* A new contact being added. */
	if (view->adding) {
		view_new_contact(view, ui, style, area, now_us);
		return;
	}

	/* No contact shown. */
	contacts = ph_contacts(&count);
	if (count == 0U) {
		view_empty(style, area, "No contacts yet", "Add one with + at the top of the list.");
		return;
	}

	/* None chosen. */
	if (view->selected < 0 || (size_t)view->selected >= count) {
		view_empty(style, area, "No conversation", "Choose a contact at the left.");
		return;
	}

	/* The three bands: the header, the items, the field. */
	contact = &contacts[view->selected];
	header = *area;
	header.height = PH_VIEW_HEADER;
	composer = *area;
	composer_height = PH_VIEW_COMPOSER;
	draft_count = ph_draft_count(view, contact->id);
	if (draft_count != 0U)
		composer_height += 54;
	composer.y = area->y + area->height - composer_height;
	composer.height = composer_height;
	timeline = *area;
	timeline.y = area->y + PH_VIEW_HEADER;
	timeline.height = area->height - PH_VIEW_HEADER - composer_height;
	view_timeline(view, ui, style, contact, &timeline, now_us);
	view_header(view, ui, style, contact, &header, now_us);
	view_composer(view, ui, style, contact, &composer, now_us);
}

/*
 * Draws the timeline's header: back (in a narrow window), the picture, the
 * name, the number and the way the latest message went, and the call
 * button.
 */
static void
view_header(
	struct ph_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct ph_contact *contact,
	const struct kl_rect *area,
	uint64_t now_us)
{
	const struct ph_item *message;
	struct kl_rect edge;
	struct kl_rect button;
	char line[128];
	kl_color ground;
	unsigned hit;
	int left;

	/* The band (white on an opaque window), its edge against the items (inset on glass). */
	edge = *area;
	edge.y = area->y + area->height - 1;
	edge.height = 1;
	if (view->glass) {
		edge.x += PH_VIEW_SIDE;
		edge.width -= 2 * PH_VIEW_SIDE;
		kl_canvas_fill(style->canvas, &edge, style->theme->row_separator);
	} else {
		kl_canvas_fill(style->canvas, area, PH_COLOR_SURFACE);
		kl_canvas_fill(style->canvas, &edge, style->theme->separator);
	}

	/* Back to the list, in a narrow window. */
	left = area->x + 16;
	if (view->narrow) {
		button.x = area->x + 6;
		button.y = area->y + 12;
		button.width = 40;
		button.height = 40;
		hit = kl_ui_hit(ui, PH_ID_BACK, 0U, &button);
		if ((hit & KL_HIT_CLICKED) != 0U)
			view->opened = 0;
		kl_icon_draw(style->canvas, KL_ICON_BACK, (float)button.x + 8.0f, (float)button.y + 8.0f, 24.0f, style->theme->accent);
		left = area->x + 50;
	}

	/* The picture, the name, and the number with the way messages go. */
	view_avatar(style, contact, left + 20, area->y + area->height / 2, 20);
	(void)kl_text_draw_fit(style->text, style->canvas, left + 52, area->y + 28, contact->name, PH_VIEW_TEXT_HEADER, 1, area->width - (left - area->x) - 120, style->theme->text);
	message = view_last_message(contact);
	if (message == NULL)
		(void)snprintf(line, sizeof(line), "%s", contact->number);
	else
		(void)snprintf(line, sizeof(line), "%s \xc2\xb7 %s", contact->number, view_channel_name(message->channel));
	(void)kl_text_draw_fit(style->text, style->canvas, left + 52, area->y + 47, line, PH_VIEW_TEXT_SMALL, 0, area->width - (left - area->x) - 120, style->theme->text_secondary);

	/* The call button: a handset in a soft circle of the accent. */
	button.x = area->x + area->width - 58;
	button.y = area->y + 12;
	button.width = 40;
	button.height = 40;
	hit = kl_ui_hit(ui, PH_ID_CALL, 0U, &button);
	if ((hit & KL_HIT_CLICKED) != 0U)
		ph_view_action(view, PH_ACTION_CALL, now_us);
	ground = KL_RGBA(style->theme->accent, 28);
	if ((hit & (KL_HIT_HOT | KL_HIT_ACTIVE)) != 0U)
		ground = KL_RGBA(style->theme->accent, 56);
	kl_canvas_circle(style->canvas, (float)button.x + 20.0f, (float)button.y + 20.0f, 20.0f, ground);
	view_handset(style->canvas, (float)button.x + 11.0f, (float)button.y + 11.0f, 18.0f, style->theme->accent);
}

/*
 * Draws the items of a timeline in its band, scrolled; at its end after a
 * contact is chosen.
 */
static void
view_timeline(
	struct ph_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct ph_contact *contact,
	const struct kl_rect *area,
	uint64_t now_us)
{
	const struct ph_item *previous;
	double limit;
	size_t i;
	int content;
	int height;
	int y;

	/* A timeline with nothing in it. */
	if (contact->item_count == 0U) {
		view_empty(style, area, "No messages yet", "Write a message or call to start.");
		return;
	}

	/* The items' height, measured without drawing. */
	content = 0;
	previous = NULL;
	for (i = 0; i < contact->item_count; i++) {
		/* One item, below the one before. */
		content += view_item(style, &contact->items[i], previous, area->x, 0, area->width, 0);
		previous = &contact->items[i];
	}

	/* The scroll's sizes, and its end after a contact was chosen. */
	content += PH_VIEW_SIDE;
	kl_scroll_set_size(&view->timeline_scroll, (double)area->width, (double)content, (double)area->width, (double)area->height);
	if (view->to_end) {
		view->to_end = 0;
		limit = kl_scroll_limit_y(&view->timeline_scroll);
		kl_scroll_move_to(&view->timeline_scroll, 0.0, limit, 0, now_us);
	}

	/* The viewport takes the wheel and a finger's drag. */
	kl_ui_scroll_region(ui, PH_ID_TIMELINE, area, &view->timeline_scroll);
	kl_canvas_clip_push(style->canvas, area);

	/* Each item, drawn where it is. */
	y = area->y - (int)view->timeline_scroll.y;
	previous = NULL;
	for (i = 0; i < contact->item_count; i++) {
		/* One item; those out of the band are only measured. */
		height = view_item(style, &contact->items[i], previous, area->x, y, area->width, 0);
		if (y + height >= area->y && y <= area->y + area->height)
			(void)view_item(style, &contact->items[i], previous, area->x, y, area->width, 1);
		y += height;
		previous = &contact->items[i];
	}

	/* The clip goes, and the bar shows while the items move. */
	kl_canvas_clip_pop(style->canvas);
	(void)kl_scroll_draw_bars(&view->timeline_scroll, style->canvas, area, style->theme, now_us);
}

/*
 * Lays out (and draws, when asked) one item of a timeline at a top edge:
 * the space after the one before, the date when the day changed, and the
 * item.  Returns the height it takes.
 */
static int
view_item(
	const struct kl_style *style,
	const struct ph_item *item,
	const struct ph_item *previous,
	int x,
	int y,
	int width,
	int draw)
{
	char line[96];
	int height;
	int same;

	/* Whether the day changed since the item before. */
	same = 1;
	if (previous != NULL)
		same = strcmp(previous->day, item->day);

	/* The first item, or a new day: the date, the time and the way it went, in the middle. */
	if (previous == NULL || same != 0) {
		/* The words of the date. */
		if (draw) {
			(void)snprintf(line, sizeof(line), "%s \xc2\xb7 %s %s", view_channel_name(item->channel), item->day, item->time);
			view_centred(style, x + width / 2, y + 30, line, PH_VIEW_TEXT_SMALL, 0, style->theme->text_secondary);
		}

		/* The date's band. */
		height = 44;
	} else if (previous->outgoing == item->outgoing && previous->kind == PH_TEXT && item->kind == PH_TEXT) {
		/* Messages one after another from the same side stand close. */
		height = 3;
	} else {
		/* Anything else stands apart. */
		height = 10;
	}

	/* The item itself. */
	switch (item->kind) {
	case PH_TEXT:
		height += view_text(style, item, x, y + height, width, draw);
		break;
	case PH_CALL:
		height += view_call(style, item, x, y + height, width, draw);
		break;
	case PH_PHOTO:
		height += view_photo(style, item, x, y + height, width, draw);
		break;
	case PH_FILE:
		height += view_file(style, item, x, y + height, width, draw);
		break;
	default:
		break;
	}

	/* The height of the space and the item. */
	return height;
}

/*
 * Lays out (and draws) a message: its words broken into lines in a bubble
 * at its side, and under one's own its state.  Returns its height.
 */
static int
view_text(
	const struct kl_style *style,
	const struct ph_item *item,
	int x,
	int y,
	int width,
	int draw)
{
	struct kl_text_line metrics;
	size_t starts[PH_VIEW_LINES_MAX];
	size_t lengths[PH_VIEW_LINES_MAX];
	const char *text;
	kl_color ground;
	kl_color ink;
	float tail;
	size_t count;
	size_t at;
	size_t length;
	size_t shown;
	size_t line_length;
	size_t i;
	int widest;
	int line_width;
	int bubble_width;
	int bubble_height;
	int left;
	int detail_width;
	int height;

	/* The widest a bubble may be: a share of the timeline, within limits. */
	widest = (int)((double)width * PH_VIEW_BUBBLE_SHARE);
	if (widest > PH_VIEW_BUBBLE_MAX)
		widest = PH_VIEW_BUBBLE_MAX;
	else if (widest < PH_VIEW_BUBBLE_MIN)
		widest = PH_VIEW_BUBBLE_MIN;

	/* Its words', within its margins. */
	widest -= 2 * PH_VIEW_PAD_X;

	/* The lines, and the widest of them (a line's last space not counted). */
	text = item->text;
	if (text == NULL)
		text = "";
	length = strlen(text);
	count = 0;
	at = 0;
	bubble_width = 0;
	while (at < length && count < PH_VIEW_LINES_MAX) {
		/* Measures one logical line before asking the generic word wrapper. */
		line_length = strcspn(text + at, "\r\n");
		line_width = kl_text_width(style->text, text + at, line_length, PH_VIEW_TEXT_BODY, 0);
		shown = line_length;
		if (line_width > widest)
			shown = kl_text_break(style->text, text + at, PH_VIEW_TEXT_BODY, 0, widest);

		/* A nonempty oversized word still advances at least one character. */
		if (shown == 0U && line_length != 0U)
			shown = 1;

		/* Where it starts, and its length without the space it breaks at. */
		starts[count] = at;
		lengths[count] = shown;
		if (shown > 0U && text[at + shown - 1U] == ' ')
			lengths[count] = shown - 1U;

		/* Its width, the widest so far making the bubble's. */
		line_width = kl_text_width(style->text, text + at, lengths[count], PH_VIEW_TEXT_BODY, 0);
		if (line_width > bubble_width)
			bubble_width = line_width;

		/* The next line. */
		count++;
		at += shown;

		/* Consumes the line separator without drawing either CR or LF. */
		if (shown == line_length) {
			if (at < length && text[at] == '\r')
				at++;
			if (at < length && text[at] == '\n')
				at++;
		}
	}

	/* The bubble's size: its lines in its margins, one line's height at least. */
	kl_text_metrics(style->text, PH_VIEW_TEXT_BODY, &metrics);
	bubble_width += 2 * PH_VIEW_PAD_X;
	bubble_height = (int)count * (metrics.height + 2) + 2 * PH_VIEW_PAD_Y;
	if (bubble_height < 36)
		bubble_height = 36;

	/* Its side: one's own at the right. */
	left = x + PH_VIEW_SIDE;
	if (item->outgoing)
		left = x + width - PH_VIEW_SIDE - bubble_width;

	/* Its height, with the state under one's own. */
	height = bubble_height;
	if (item->outgoing && item->detail != NULL)
		height += 18;

	/* Measured only. */
	if (!draw)
		return height;

	/* The colors: the person's light (see-through on glass), one's own the accent over RCS and teal over the carrier. */
	ground = PH_COLOR_INCOMING;
	if (style->glass)
		ground = PH_COLOR_INCOMING_GLASS;
	ink = style->theme->text;
	if (item->outgoing) {
		ground = PH_COLOR_SMS;
		if (item->channel == PH_RCS)
			ground = PH_COLOR_RCS;
		ink = PH_COLOR_WHITE;
	}

	/* The bubble, its lower corner on the sender's side nearly square, and its lines. */
	kl_canvas_round(style->canvas, (float)left, (float)y, (float)bubble_width, (float)bubble_height, PH_VIEW_RADIUS, ground);
	tail = (float)left;
	if (item->outgoing)
		tail = (float)(left + bubble_width) - PH_VIEW_RADIUS;
	kl_canvas_round(style->canvas, tail, (float)(y + bubble_height) - PH_VIEW_RADIUS, PH_VIEW_RADIUS, PH_VIEW_RADIUS, PH_VIEW_TAIL, ground);
	for (i = 0; i < count; i++) {
		/* One line. */
		(void)kl_text_draw(style->text, style->canvas, left + PH_VIEW_PAD_X,
				   y + (bubble_height - (int)count * (metrics.height + 2)) / 2 + (int)i * (metrics.height + 2) + metrics.ascent + 1,
				   text + starts[i],
				   lengths[i],
				   PH_VIEW_TEXT_BODY,
				   0,
				   ink);
	}

	/* One's own message's state, under it at the right. */
	if (item->outgoing && item->detail != NULL) {
		detail_width = kl_text_width(style->text, item->detail, strlen(item->detail), PH_VIEW_TEXT_SMALL - 1U, 0);
		(void)kl_text_draw(style->text, style->canvas, x + width - PH_VIEW_SIDE - 4 - detail_width, y + bubble_height + 14, item->detail, strlen(item->detail), PH_VIEW_TEXT_SMALL - 1U, 0, style->theme->text_faint);
	}

	/* The bubble's height, with the state. */
	return height;
}

/*
 * Lays out (and draws) a call: a card at its side with a handset, what
 * kind of call it was, the way it went and how long it lasted.  Returns
 * its height.
 */
static int
view_call(
	const struct kl_style *style,
	const struct ph_item *item,
	int x,
	int y,
	int width,
	int draw)
{
	const char *title;
	char line[64];
	kl_color ground;
	kl_color tone;
	int card_width;
	int left;

	/* The card's size and side. */
	card_width = 236;
	left = x + PH_VIEW_SIDE;
	if (item->outgoing)
		left = x + width - PH_VIEW_SIDE - card_width;

	/* Measured only. */
	if (!draw)
		return 56;

	/* What kind of call: a call not answered is a missed one, in red. */
	tone = style->theme->good;
	if (item->outgoing && item->detail == NULL) {
		title = "Not answered";
		tone = style->theme->text_secondary;
	} else if (item->outgoing) {
		title = "Outgoing call";
	} else if (item->detail == NULL) {
		title = "Missed call";
		tone = style->theme->danger;
	} else {
		title = "Incoming call";
	}

	/* The card, the handset in its circle, and the words. */
	ground = PH_COLOR_CARD;
	if (style->glass)
		ground = PH_COLOR_CARD_GLASS;
	kl_canvas_round(style->canvas, (float)left, (float)y, (float)card_width, 56.0f, 16.0f, ground);
	kl_canvas_round_border(style->canvas, (float)left, (float)y, (float)card_width, 56.0f, 16.0f, 1.0f, style->theme->panel_edge);
	kl_canvas_circle(style->canvas, (float)left + 30.0f, (float)y + 28.0f, 18.0f, tone);
	view_handset(style->canvas, (float)left + 22.0f, (float)y + 20.0f, 16.0f, PH_COLOR_WHITE);
	(void)kl_text_draw(style->text, style->canvas, left + 58, y + 24, title, strlen(title), PH_VIEW_TEXT_BODY, 1, style->theme->text);
	if (item->detail != NULL)
		(void)snprintf(line, sizeof(line), "%s \xc2\xb7 %s \xc2\xb7 %s", view_channel_name(item->channel), item->time, item->detail);
	else
		(void)snprintf(line, sizeof(line), "%s \xc2\xb7 %s", view_channel_name(item->channel), item->time);
	(void)kl_text_draw_fit(style->text, style->canvas, left + 58, y + 43, line, PH_VIEW_TEXT_SMALL, 0, card_width - 70, style->theme->text_secondary);

	/* The card's height. */
	return 56;
}

/*
 * Lays out (and draws) a picture: the mock has none, so a drawn landscape
 * stands for it, with its caption under it.  Returns its height.
 */
static int
view_photo(
	const struct kl_style *style,
	const struct ph_item *item,
	int x,
	int y,
	int width,
	int draw)
{
	float mountains[14];
	struct kl_rect band;
	kl_color sky_top;
	kl_color sky_bottom;
	kl_color hill;
	kl_color ground;
	float left;
	float top;
	int picture_width;
	int picture_height;
	int height;
	int caption_width;

	/* The picture's size, with the caption's line. */
	picture_width = 240;
	picture_height = 160;
	height = picture_height;
	if (item->text != NULL)
		height += 20;

	/* Its side: one's own at the right. */
	left = (float)(x + PH_VIEW_SIDE);
	top = (float)y;
	if (item->outgoing)
		left = (float)(x + width - PH_VIEW_SIDE - picture_width);

	/* Measured only. */
	if (!draw)
		return height;

	/* The colours: a river's morning, or a warm evening for a picture with a caption. */
	sky_top = KL_RGB(0x8ec5f2);
	sky_bottom = KL_RGB(0xf6dcb0);
	hill = KL_RGB(0x4f7f6a);
	ground = KL_RGB(0x5b9bd5);
	if (item->text != NULL) {
		sky_top = KL_RGB(0xf2a65a);
		sky_bottom = KL_RGB(0xfbe3b8);
		hill = KL_RGB(0x9c4f2e);
		ground = KL_RGB(0xd9a066);
	}

	/* The sky, the sun, the hills and the water or the field. */
	kl_canvas_round_gradient(style->canvas, left, top, (float)picture_width, (float)picture_height, PH_VIEW_RADIUS, sky_top, sky_bottom);
	kl_canvas_circle(style->canvas, left + 178.0f, top + 46.0f, 18.0f, KL_RGBA(0xffffff, 200));
	mountains[0] = left;
	mountains[1] = top + 118.0f;
	mountains[2] = left + 62.0f;
	mountains[3] = top + 66.0f;
	mountains[4] = left + 104.0f;
	mountains[5] = top + 98.0f;
	mountains[6] = left + 150.0f;
	mountains[7] = top + 72.0f;
	mountains[8] = left + (float)picture_width;
	mountains[9] = top + 116.0f;
	mountains[10] = left + (float)picture_width;
	mountains[11] = top + 124.0f;
	mountains[12] = left;
	mountains[13] = top + 124.0f;
	kl_canvas_polygon(style->canvas, mountains, 7, hill);
	kl_canvas_round(style->canvas, left, top + 118.0f, (float)picture_width, (float)picture_height - 118.0f, PH_VIEW_RADIUS, ground);
	band.x = (int)left;
	band.y = (int)top + 118;
	band.width = picture_width;
	band.height = 12;
	kl_canvas_fill(style->canvas, &band, ground);

	/* The caption under it. */
	if (item->text != NULL) {
		caption_width = kl_text_width(style->text, item->text, strlen(item->text), PH_VIEW_TEXT_SMALL, 0);
		if (item->outgoing)
			left = (float)(x + width - PH_VIEW_SIDE - caption_width);
		(void)kl_text_draw(style->text, style->canvas, (int)left + 4, y + picture_height + 15, item->text, strlen(item->text), PH_VIEW_TEXT_SMALL, 0, style->theme->text_secondary);
	}

	/* The picture's height, with the caption. */
	return height;
}

/*
 * Lays out (and draws) a file: a card at its side with the file's
 * picture, its name, its type and size.  Returns its height.
 */
static int
view_file(
	const struct kl_style *style,
	const struct ph_item *item,
	int x,
	int y,
	int width,
	int draw)
{
	char label[8];
	const char *dot;
	kl_color ground;
	size_t i;
	int card_width;
	int left;

	/* The card's size and side. */
	card_width = 260;
	left = x + PH_VIEW_SIDE;
	if (item->outgoing)
		left = x + width - PH_VIEW_SIDE - card_width;

	/* Measured only. */
	if (!draw)
		return 60;

	/* The label on the file's picture: its extension in capitals. */
	label[0] = '\0';
	dot = strrchr(item->text, '.');
	if (dot != NULL) {
		(void)snprintf(label, sizeof(label), "%s", dot + 1);
		for (i = 0; label[i] != '\0'; i++) {
			/* One letter in capitals. */
			if (label[i] >= 'a' && label[i] <= 'z')
				label[i] = (char)(label[i] - 'a' + 'A');
		}
	}

	/* The card, the file's picture, the name and the detail. */
	ground = PH_COLOR_INCOMING;
	if (style->glass)
		ground = PH_COLOR_INCOMING_GLASS;
	kl_canvas_round(style->canvas, (float)left, (float)y, (float)card_width, 60.0f, 16.0f, ground);
	kl_icon_file(style->canvas, style->text, (float)left + 10.0f, (float)y + 8.0f, 44.0f, style->theme->danger, label);
	(void)kl_text_draw_fit(style->text, style->canvas, left + 60, y + 26, item->text, PH_VIEW_TEXT_BODY, 1, card_width - 72, style->theme->text);
	if (item->detail != NULL)
		(void)kl_text_draw_fit(style->text, style->canvas, left + 60, y + 45, item->detail, PH_VIEW_TEXT_SMALL, 0, card_width - 72, style->theme->text_secondary);

	/* The card's height. */
	return 60;
}

/*
 * Draws the band at the bottom: attach, the field to write a message in
 * (Enter sends) and send.
 */
static void
view_composer(
	struct ph_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct ph_contact *contact,
	const struct kl_rect *area,
	uint64_t now_us)
{
	const struct ph_item *message;
	const char *placeholder;
	struct kl_rect edge;
	struct kl_rect button;
	struct kl_rect field;
	kl_color ground;
	unsigned changes;
	unsigned hit;

	struct kl_rect input;
	const char *name;
	size_t index;
	size_t shown;
	int same;
	int removed;
	size_t skipped;
	size_t total;

	/* The band (white on an opaque window), its edge against the items (inset on glass). */
	edge = *area;
	edge.height = 1;
	if (view->glass) {
		edge.x += PH_VIEW_SIDE;
		edge.width -= 2 * PH_VIEW_SIDE;
		kl_canvas_fill(style->canvas, &edge, style->theme->row_separator);
	} else {
		kl_canvas_fill(style->canvas, area, PH_COLOR_SURFACE);
		kl_canvas_fill(style->canvas, &edge, style->theme->separator);
	}

	/* Draft cards show file names and photo/video icons, with an explicit remove action. */
	input = *area;
	input.y += area->height - PH_VIEW_COMPOSER;
	input.height = PH_VIEW_COMPOSER;
	shown = 0U;
	skipped = 0U;
	total = ph_draft_count(view, contact->id);
	if (view->attachment_page >= total)
		view->attachment_page = 0U;
	for (index = 0U; index < view->attachment_count; index++) {
		same = strcmp(view->attachments[index].contact, contact->id);
		if (same != 0)
			continue;
		if (skipped++ < view->attachment_page)
			continue;
		button.x = input.x + 12 + (int)shown * 156;
		button.y = area->y + 8;
		button.width = 148;
		button.height = 40;
		if (button.x + button.width > area->x + area->width - 58)
			break;
		kl_canvas_round(style->canvas, (float)button.x, (float)button.y, (float)button.width, (float)button.height, 8.0f, style->theme->track);
		if (view->attachments[index].video)
			kl_icon_draw(style->canvas, KL_ICON_MOVIES, (float)button.x + 6.0f, (float)button.y + 10.0f, 20.0f, style->theme->icon);
		else
			kl_icon_draw(style->canvas, KL_ICON_PICTURES, (float)button.x + 6.0f, (float)button.y + 10.0f, 20.0f, style->theme->icon);
		name = strrchr(view->attachments[index].path, '/');
		if (name != NULL)
			name++;
		else
			name = view->attachments[index].path;
		(void)kl_text_draw_fit(style->text, style->canvas, button.x + 30, button.y + 25, name, PH_VIEW_TEXT_SMALL, 0, 84, style->theme->text);
		button.x += 118;
		button.width = 28;
		hit = kl_ui_hit(ui, 900U + (uint32_t)index, 0U, &button);
		kl_icon_draw(style->canvas, KL_ICON_CLOSE, (float)button.x + 6.0f, (float)button.y + 12.0f, 16.0f, style->theme->icon);
		removed = 0;
		if ((hit & KL_HIT_CLICKED) != 0U) {
			ph_draft_remove(view, index);
			removed = 1;
		}

		/* Removal shifts later slots; stop this frame before drawing stale indexes. */
		if (removed)
			break;
		shown++;
	}

	/* Paging makes every retained attachment reachable even in a narrow composer. */
	if (total != 0U) {
		button.x = area->x + area->width - 54;
		button.y = area->y + 8;
		button.width = 24;
		button.height = 40;
		hit = kl_ui_hit(ui, 950U, 0U, &button);
		kl_icon_draw(style->canvas, KL_ICON_BACK, (float)button.x + 2.0f, (float)button.y + 10.0f, 20.0f, style->theme->icon);
		if ((hit & KL_HIT_CLICKED) != 0U && view->attachment_page != 0U)
			view->attachment_page--;
		button.x += 26;
		hit = kl_ui_hit(ui, 951U, 0U, &button);
		kl_icon_draw(style->canvas, KL_ICON_FORWARD, (float)button.x + 2.0f, (float)button.y + 10.0f, 20.0f, style->theme->icon);
		if ((hit & KL_HIT_CLICKED) != 0U && view->attachment_page + shown < total)
			view->attachment_page++;
	}

	/* Attach: a plus in a grey circle. */
	button.x = input.x + 12;
	button.y = input.y + 14;
	button.width = 32;
	button.height = 32;
	hit = kl_ui_hit(ui, PH_ID_ATTACH, 0U, &button);
	if ((hit & KL_HIT_CLICKED) != 0U)
		ph_view_action(view, PH_ACTION_ATTACH, now_us);
	ground = kl_theme_choose(KL_RGB(0xeef1f5), KL_RGB(0x2f3540));
	if ((hit & (KL_HIT_HOT | KL_HIT_ACTIVE)) != 0U)
		ground = kl_theme_choose(KL_RGB(0xe1e6ee), KL_RGB(0x3a414d));
	kl_canvas_circle(style->canvas, (float)button.x + 16.0f, (float)button.y + 16.0f, 16.0f, ground);
	kl_icon_draw(style->canvas, KL_ICON_PLUS, (float)button.x + 6.0f, (float)button.y + 6.0f, 20.0f, style->theme->icon);

	/* The field, named after the way the latest message went; Enter sends. */
	placeholder = "Text Message";
	message = view_last_message(contact);
	if (message != NULL && message->channel == PH_RCS)
		placeholder = "RCS Message";
	field.x = input.x + 54;
	field.y = input.y + 12;
	field.width = area->width - 54 - 56;
	field.height = 36;
	changes = kl_field(ui, style, PH_ID_MESSAGE, &field, &view->message, placeholder);
	if ((changes & KL_FIELD_SUBMITTED) != 0U)
		ph_view_action(view, PH_ACTION_SEND, now_us);

	/* Send: an arrow up in a circle of the accent, grey while nothing is written or the paired phone takes no text. */
	button.x = input.x + input.width - 46;
	button.y = input.y + 12;
	button.width = 36;
	button.height = 36;
	hit = kl_ui_hit(ui, PH_ID_SEND, 0U, &button);
	if ((hit & KL_HIT_CLICKED) != 0U)
		ph_view_action(view, PH_ACTION_SEND, now_us);
	ground = style->theme->track;
	if (view->message.length != 0U && !view->cannot_send)
		ground = style->theme->accent;
	kl_canvas_circle(style->canvas, (float)button.x + 18.0f, (float)button.y + 18.0f, 16.0f, ground);
	kl_icon_draw(style->canvas, KL_ICON_UP, (float)button.x + 8.0f, (float)button.y + 8.0f, 20.0f, PH_COLOR_WHITE);
}

/*
 * Draws a title and a line in the middle of an area that has nothing
 * else to show.
 */
static void
view_empty(
	const struct kl_style *style,
	const struct kl_rect *area,
	const char *title,
	const char *line)
{
	int cx;
	int cy;

	/* The two lines, around the middle. */
	cx = area->x + area->width / 2;
	cy = area->y + area->height / 2;
	view_centred(style, cx, cy - 4, title, PH_VIEW_TEXT_HEADER, 1, style->theme->text_secondary);
	view_centred(style, cx, cy + 20, line, PH_VIEW_TEXT_BODY, 0, style->theme->text_faint);
}

/*
 * Draws a handset in a square box of a size at (x, y).
 */
static void
view_handset(
	struct kl_canvas *canvas,
	float x,
	float y,
	float size,
	kl_color color)
{
	float points[sizeof(ph_view_handset) / sizeof(ph_view_handset[0])];
	float scale;
	size_t i;

	/* The outline, scaled from its 18-pixel box. */
	scale = size / 18.0f;
	for (i = 0; i < sizeof(points) / sizeof(points[0]); i += 2U) {
		points[i] = x + ph_view_handset[i] * scale;
		points[i + 1U] = y + ph_view_handset[i + 1U] * scale;
	}

	/* Filled. */
	kl_canvas_polygon(canvas, points, (int)(sizeof(points) / sizeof(points[0]) / 2U), color);
}

/*
 * Draws a line of text centred on a point across.
 */
static void
view_centred(
	const struct kl_style *style,
	int cx,
	int baseline,
	const char *text,
	unsigned pixels,
	int bold,
	kl_color color)
{
	int width;

	/* Half its width to the left. */
	width = kl_text_width(style->text, text, strlen(text), pixels, bold);
	(void)kl_text_draw(style->text, style->canvas, cx - width / 2, baseline, text, strlen(text), pixels, bold, color);
}

/*
 * Reports the name of a channel as the timeline shows it.
 */
static const char *
view_channel_name(
	enum ph_channel channel)
{
	/* Each channel. */
	switch (channel) {
	case PH_SMS:
		return "SMS";
	case PH_MMS:
		return "MMS";
	case PH_RCS:
		return "RCS";
	case PH_LINE:
		return "Phone";
	case PH_VOIP:
		return "VoIP";
	default:
		break;
	}

	/* A channel not known. */
	return "";
}

/*
 * Reports the line of an item the list of contacts shows.
 */
static const char *
view_preview(
	const struct ph_item *item)
{
	/* A message's own words, or what the item was. */
	switch (item->kind) {
	case PH_TEXT:
		return item->text;
	case PH_CALL:
		/* A call: one's own not answered or made, the person's missed or answered. */
		if (item->outgoing && item->detail == NULL) {
			return "Call not answered";
		} else if (item->outgoing) {
			return "Outgoing call";
		} else if (item->detail == NULL) {
			return "Missed call";
		} else {
			return "Incoming call";
		}
	case PH_PHOTO:
		return "Photo";
	case PH_FILE:
		return item->text;
	default:
		break;
	}

	/* A kind not known. */
	return "";
}

/*
 * Reports the latest message of a contact's timeline (not a call), NULL
 * when there is none.
 */
static const struct ph_item *
view_last_message(
	const struct ph_contact *contact)
{
	size_t i;

	/* From the end, the first that is not a call. */
	for (i = contact->item_count; i > 0U; i--) {
		/* A message, a picture or a file. */
		if (contact->items[i - 1U].kind != PH_CALL)
			return &contact->items[i - 1U];
	}

	/* Calls only, or nothing. */
	return NULL;
}
