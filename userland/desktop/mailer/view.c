/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Mail's view (WS169 p000, p004; mailer.h): three panes side by side -- the
 * accounts and their folders with New Message and Get Mail, the folder's
 * messages under a search field (the sender, the subject, a line of the
 * words, unread ones marked), and the message chosen with Reply, Reply
 * All, Forward, Archive and Delete over it (its sender, its date, its
 * words, the file it carries, and a sign-in code it holds standing out);
 * or, in the third pane, a new message being written (To, Cc, Subject and
 * the words).  A window narrower than ML_VIEW_NARROW leaves out the
 * folders and shows the list or the message, with a back button.  On
 * the compositor's glass the panes are cards with the desktop between.
 *
 * Sending, getting mail, archiving, deleting and opening an unread
 * message are queued as requests for the window (ml_view_take_request),
 * which carries them out with the servers.  Without an account, or after
 * Add Account, the list and the message give way to the form of a new
 * account: the user's name, the address, the password and the two
 * servers, Sign In, and whether the browser may fill in sign-in codes.
 */

#include "mailer.h"

#include <stdio.h>
#include <string.h>

/* The width under which the folders are left out and the list or the message is shown alone. */
#define ML_VIEW_NARROW		900

/*
 * The panes: the folders' and the list's widths, the cards' gap and corner
 * on glass.  The cards reach the window's edges, so that they line up with
 * the floating titlebar (ws090-p021).
 */
#define ML_VIEW_SIDEBAR		220
#define ML_VIEW_LIST		340
#define ML_VIEW_GAP		10
#define ML_VIEW_CARD_RADIUS	16.0f

/* The list: its header's height and a row's; the message's bar and the margin of its words. */
#define ML_VIEW_LIST_HEADER	104
#define ML_VIEW_ROW		78
#define ML_VIEW_BAR		56
#define ML_VIEW_PAD		24

/* The text sizes. */
#define ML_VIEW_TEXT_TITLE	20U
#define ML_VIEW_TEXT_NAME	14U
#define ML_VIEW_TEXT_BODY	14U
#define ML_VIEW_TEXT_SMALL	12U

/* The most lines a message's words are drawn in, and how long a notice shows. */
#define ML_VIEW_LINES_MAX	400
#define ML_VIEW_NOTICE_US	4000000U

/* The widgets' ids. */
#define ML_ID_NEW		1U
#define ML_ID_FOLDER		2U
#define ML_ID_GET		3U
#define ML_ID_SEARCH		4U
#define ML_ID_LIST		5U
#define ML_ID_ROW		6U
#define ML_ID_TOOL		7U
#define ML_ID_READER		8U
#define ML_ID_BACK		9U
#define ML_ID_TO		10U
#define ML_ID_CC		11U
#define ML_ID_SUBJECT		12U
#define ML_ID_BODY		13U
#define ML_ID_SEND		14U
#define ML_ID_CANCEL		15U
#define ML_ID_ADD		16U
#define ML_ID_SETUP		17U
#define ML_ID_SIGN_IN		22U
#define ML_ID_CODES		23U
#define ML_ID_SETUP_CANCEL	24U

/* The colors: the folders' ground on an opaque window, white, and a code's tint. */
#define ML_COLOR_SIDEBAR	kl_theme_choose(KL_RGB(0xf4f6f9), KL_RGB(0x1f232a))
#define ML_COLOR_WHITE		KL_RGB(0xffffff)
#define ML_COLOR_SURFACE	kl_theme_choose(KL_RGB(0xffffff), KL_RGB(0x23272f))
#define ML_COLOR_CODE		KL_RGBA(kl_theme_default()->accent, 26)

/* Where the panes of a frame are (a pane left out is zero wide). */
struct view_layout {
	struct kl_rect sidebar;
	struct kl_rect list;
	struct kl_rect reader;
	/* The form of a new account, which takes the list's and the message's place (width 0 when it does not show). */
	struct kl_rect form;
};

/* The folders' icons. */
static const enum kl_icon view_folder_icons[ML_FOLDERS] = { KL_ICON_DOWNLOADS, KL_ICON_SHARE, KL_ICON_DOCUMENTS, KL_ICON_FOLDER_LINE, KL_ICON_TRASH };

/* The message's tools: their words and actions. */
static const char *const view_tools[] = { "Reply", "Reply All", "Forward" };
static const unsigned view_tool_actions[] = { ML_ACTION_REPLY, ML_ACTION_REPLY_ALL, ML_ACTION_FORWARD };

static void view_layout(const struct ml_view *view, int width, int height, struct view_layout *layout);
static void view_layout_panes(const struct ml_view *view, int width, int height, struct view_layout *layout);
static void view_sidebar(struct ml_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_list(struct ml_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_row(struct ml_view *view, const struct kl_style *style, size_t index, const struct kl_rect *row, int chosen);
static void view_reader(struct ml_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_compose(struct ml_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static int view_words(const struct kl_style *style, const char *text, int x, int y, int width, unsigned pixels, kl_color color, int draw);
static void view_avatar(const struct kl_style *style, const char *name, kl_color color, int cx, int cy, int radius);
static void view_clip(struct kl_canvas *canvas, float x, float y, float size, kl_color color);
static void view_edge(const struct ml_view *view, const struct kl_style *style, int x, int y, int width);
static size_t view_shown(const struct ml_view *view, size_t *indices, size_t size);
static size_t view_unread(const struct ml_view *view, int account, enum ml_folder folder);
static int view_is_unread(const struct ml_view *view, size_t index);
static int view_contains(const char *text, const char *part);
static int view_lower(int c);
static void view_open(struct ml_view *view, long index);
static void view_reply(struct ml_view *view, unsigned action);
static void view_setup(struct ml_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_request(struct ml_view *view, unsigned action, long message);
static const char *view_action_name(unsigned action);
static int view_newer(const struct ml_message *messages, size_t a, size_t b);

/*
 * Makes the view's state: the first account's inbox, its first message
 * shown.
 */
int
ml_view_init(
	struct ml_view *view)
{
	int error;

	/* Nothing yet; the password's characters show as dots. */
	memset(view, 0, sizeof(view[0]));
	view->selected = -1;
	view->setup_password.secret = 1;
	view->setup_address.plain = 1;
	view->setup_imap.plain = 1;
	view->setup_smtp.plain = 1;

	/* The list's scroll, down only. */
	error = kl_scroll_init(&view->list_scroll, KL_SCROLL_Y);
	if (error != 0)
		return error;

	/* The message's scroll, down only. */
	error = kl_scroll_init(&view->reader_scroll, KL_SCROLL_Y);
	if (error != 0) {
		kl_scroll_release(&view->list_scroll);
		return error;
	}

	/* Succeeded: the inbox's first message shown (a narrow window shows the list first). */
	view_open(view, 0);
	return 0;
}

/*
 * Frees what the view's state holds.
 */
void
ml_view_release(
	struct ml_view *view)
{
	/* The scrolls. */
	kl_scroll_release(&view->reader_scroll);
	kl_scroll_release(&view->list_scroll);
}

/*
 * Carries out an action of the menu, a key or a button.
 */
void
ml_view_action(
	struct ml_view *view,
	unsigned action,
	uint64_t now_us)
{
	UNUSED_PARAMETER(now_us);

	/* Each action. */
	switch (action) {
	case ML_ACTION_NEW:
		/* An empty message to write. */
		view->composing = 1;
		view->opened = 1;
		view->adding = 0;
		view->reply_id[0] = '\0';
		kl_field_set(&view->to, "");
		kl_field_set(&view->cc, "");
		kl_field_set(&view->subject, "");
		kl_text_area_set(&view->body, "");
		ml_log("COMPOSE kind=new");
		break;
	case ML_ACTION_REPLY:
	case ML_ACTION_REPLY_ALL:
	case ML_ACTION_FORWARD:
		view_reply(view, action);
		break;
	case ML_ACTION_SEND:
		/* The message written, for the window to send (it closes the writing when it is sent). */
		if (!view->composing)
			break;
		view_request(view, ML_ACTION_SEND, -1);
		ml_log("REQUEST action=send to=%zu subject=%zu body=%zu", view->to.length, view->subject.length, view->body.length);
		break;
	case ML_ACTION_GET:
		view_request(view, ML_ACTION_GET, -1);
		ml_log("REQUEST action=get");
		break;
	case ML_ACTION_ARCHIVE:
	case ML_ACTION_DELETE:
		/* The message shown, for the window to move; nothing is shown after it. */
		if (view->selected < 0 || view->composing)
			break;
		view_request(view, action, view->selected);
		ml_log("REQUEST action=%s message=%ld", view_action_name(action), view->selected);
		view->selected = -1;
		view->opened = 0;
		break;
	case ML_ACTION_ADD_ACCOUNT:
		/* The form of a new account. */
		view->adding = 1;
		view->composing = 0;
		view->opened = 1;
		ml_log("SETUP open");
		break;
	case ML_ACTION_SIGN_IN:
		/* The form's account, for the window to try. */
		view_request(view, ML_ACTION_SIGN_IN, -1);
		ml_log("REQUEST action=sign-in address=%zu imap=%zu smtp=%zu", view->setup_address.length, view->setup_imap.length, view->setup_smtp.length);
		break;
	case ML_ACTION_CANCEL:
		/* The message written goes (no drafts are kept), or the form of a new account closes. */
		view->composing = 0;
		view->adding = 0;
		ml_log("COMPOSE kind=cancel");
		break;
	case ML_ACTION_QUIT:
		view->quit = 1;
		break;
	default:
		break;
	}
}

/*
 * Takes a key no widget took: while the words of a new message have the
 * keyboard, it writes them; else Up and Down move through the list, Enter
 * opens the message and Esc goes back to the list of a narrow window.
 */
void
ml_view_key(
	struct ml_view *view,
	uint32_t key,
	unsigned modifiers,
	uint64_t now_us)
{
	size_t indices[ML_MESSAGES_MAX];
	size_t count;
	size_t at;
	size_t i;

	/* The keys with Ctrl, Alt or Super are the menu's. */
	(void)now_us;
	if ((modifiers & (KL_MOD_CTRL | KL_MOD_ALT | KL_MOD_SUPER)) != 0U)
		return;

	/* Esc goes back to the list of a narrow window. */
	if (key == KL_KEY_ESC) {
		view->opened = 0;
		return;
	}

	/* Enter opens the message of a narrow window. */
	if (key == KL_KEY_ENTER || key == KL_KEY_KPENTER) {
		view->opened = 1;
		return;
	}

	/* Up and Down alone move, through the messages the list shows. */
	count = view_shown(view, indices, ML_MESSAGES_MAX);
	if ((key != KL_KEY_UP && key != KL_KEY_DOWN) || count == 0U)
		return;

	/* Where the message shown is among them (the first when it is not). */
	at = 0;
	for (i = 0; i < count; i++) {
		/* The message shown. */
		if ((long)indices[i] == view->selected)
			at = i;
	}

	/* One up or down, within the list. */
	if (key == KL_KEY_UP && at > 0U)
		at--;
	else if (key == KL_KEY_DOWN && at + 1U < count)
		at++;
	view_open(view, (long)indices[at]);
}

/*
 * Draws a frame of the view in a window of a size, between the caller's
 * kl_ui_begin and kl_ui_end, and takes what the input did to its widgets.
 */
void
ml_view_draw(
	struct ml_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	int width,
	int height,
	uint64_t now_us)
{
	struct view_layout layout;
	struct kl_rect whole;
	struct kl_rect bottom;

	/* The ground: clear on glass (the desktop shows between the cards), else white. */
	whole.x = 0;
	whole.y = 0;
	whole.width = width;
	whole.height = height;
	if (view->glass) {
		kl_canvas_clear(style->canvas);
	} else {
		kl_canvas_fill(style->canvas, &whole, ML_COLOR_SURFACE);
	}

	/* Where the panes go. */
	view->narrow = 0;
	if (width < ML_VIEW_NARROW)
		view->narrow = 1;
	view_layout(view, width, height, &layout);

	/* The folders, on their card on glass, else on their own ground with an edge. */
	if (layout.sidebar.width > 0) {
		if (view->glass) {
			kl_canvas_round(style->canvas, (float)layout.sidebar.x, (float)layout.sidebar.y, (float)layout.sidebar.width, (float)layout.sidebar.height, ML_VIEW_CARD_RADIUS, style->theme->glass_sidebar);
		} else {
			kl_canvas_fill(style->canvas, &layout.sidebar, ML_COLOR_SIDEBAR);
		}

		/* What is on it. */
		view_sidebar(view, ui, style, &layout.sidebar, now_us);
	}

	/*
	 * Without an account, or after Add Account: the form of a new account
	 * on one card where the list and the message go (ws169, the UAT of
	 * 2026-10-07: on two cards the gap between them showed through it).
	 */
	if (layout.form.width > 0) {
		if (view->glass)
			kl_canvas_round(style->canvas, (float)layout.form.x, (float)layout.form.y, (float)layout.form.width, (float)layout.form.height, ML_VIEW_CARD_RADIUS, style->theme->glass_content);
		view_setup(view, ui, style, &layout.form, now_us);
	}

	/* The list, on its card on glass. */
	if (layout.list.width > 0) {
		if (view->glass)
			kl_canvas_round(style->canvas, (float)layout.list.x, (float)layout.list.y, (float)layout.list.width, (float)layout.list.height, ML_VIEW_CARD_RADIUS, style->theme->glass_content);
		view_list(view, ui, style, &layout.list, now_us);
	}

	/* The message or the one being written, on its card on glass. */
	if (layout.reader.width > 0) {
		if (view->glass)
			kl_canvas_round(style->canvas, (float)layout.reader.x, (float)layout.reader.y, (float)layout.reader.width, (float)layout.reader.height, ML_VIEW_CARD_RADIUS, style->theme->glass_content);

		/* The one being written, or the one chosen. */
		if (view->composing)
			view_compose(view, ui, style, &layout.reader, now_us);
		else
			view_reader(view, ui, style, &layout.reader, now_us);
	}

	/* On an opaque window, the lines between the panes. */
	if (!view->glass) {
		bottom.y = 0;
		bottom.width = 1;
		bottom.height = height;
		bottom.x = layout.sidebar.x + layout.sidebar.width - 1;
		if (layout.sidebar.width > 0)
			kl_canvas_fill(style->canvas, &bottom, style->theme->separator);

		/* Between the list and the message, when both show. */
		bottom.x = layout.list.x + layout.list.width - 1;
		if (layout.list.width > 0 && layout.reader.width > 0)
			kl_canvas_fill(style->canvas, &bottom, style->theme->separator);
	}

	/* The notice over the bottom of the window while it shows. */
	if (view->notice[0] != '\0' && now_us < view->notice_until)
		kl_chip(style, width / 2, height - 24, view->notice);
}

/*
 * Lists the parts of the view that stand on the compositor's glass (its panes'
 * cards) for a window of a size, into up to capacity panels; returns how
 * many there are.
 */
size_t
ml_view_panels(
	struct ml_view *view,
	int width,
	int height,
	struct kl_glass_panel *panels,
	size_t capacity)
{
	struct view_layout layout;
	const struct kl_rect *cards[4];
	size_t count;
	size_t i;

	/* The panes, as the frame draws them (the form, when it shows, in the list's and the message's place). */
	view_layout(view, width, height, &layout);
	cards[0] = &layout.sidebar;
	cards[1] = &layout.list;
	cards[2] = &layout.reader;
	cards[3] = &layout.form;
	count = 0;
	for (i = 0; i < 4U && count < capacity; i++) {
		/* A pane left out has no panel. */
		if (cards[i]->width <= 0 || cards[i]->height <= 0)
			continue;

		/* The pane's panel. */
		memset(&panels[count], 0, sizeof(panels[count]));
		panels[count].x = cards[i]->x;
		panels[count].y = cards[i]->y;
		panels[count].width = cards[i]->width;
		panels[count].height = cards[i]->height;
		panels[count].radius = (int32_t)ML_VIEW_CARD_RADIUS;
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
ml_view_wait(
	const struct ml_view *view,
	uint64_t now_us)
{
	uint64_t left;

	/* No notice: only input changes the view. */
	if (view->notice[0] == '\0' || now_us >= view->notice_until)
		return -1;

	/* The notice's time left, rounded up. */
	left = view->notice_until - now_us;
	return (int)(left / 1000U) + 1;
}

/*
 * Lays out the panes in a window of a size; without an account or while
 * one is added, the form of a new account takes the list's and the
 * message's place (the whole window when it is narrow).
 */
static void
view_layout(
	const struct ml_view *view,
	int width,
	int height,
	struct view_layout *layout)
{
	size_t account_count;

	/* The panes. */
	view_layout_panes(view, width, height, layout);

	/* The form, when there is no account or one is being added. */
	(void)ml_accounts(&account_count);
	if (!view->adding && account_count != 0U)
		return;

	/* One card from the list's left to the message's right, or the whole narrow window. */
	if (view->narrow) {
		layout->form.x = 0;
		layout->form.y = 0;
		layout->form.width = width;
		layout->form.height = height;
	} else {
		layout->form.x = layout->list.x;
		layout->form.y = layout->list.y;
		layout->form.width = layout->reader.x + layout->reader.width - layout->list.x;
		layout->form.height = layout->list.height;
	}

	/* The list and the message do not show under it. */
	layout->list.width = 0;
	layout->reader.width = 0;
}

/*
 * Lays out the panes in a window of a size: the folders, the list and the
 * message side by side, or in a narrow window the list or the message
 * alone; on glass, as cards with a gap between.
 */
static void
view_layout_panes(
	const struct ml_view *view,
	int width,
	int height,
	struct view_layout *layout)
{
	int margin;
	int gap;
	int left;

	/* No margin (see ML_VIEW_SIDEBAR), and the gap: only cards on glass have one. */
	memset(layout, 0, sizeof(layout[0]));
	margin = 0;
	gap = 0;
	if (view->glass)
		gap = ML_VIEW_GAP;

	/* A narrow window: the list or the message alone. */
	if (view->narrow) {
		if (view->opened) {
			layout->reader.x = margin;
			layout->reader.y = margin;
			layout->reader.width = width - 2 * margin;
			layout->reader.height = height - 2 * margin;
		} else {
			layout->list.x = margin;
			layout->list.y = margin;
			layout->list.width = width - 2 * margin;
			layout->list.height = height - 2 * margin;
		}

		/* The one pane laid out. */
		return;
	}

	/* The folders, the list and the message, left to right. */
	layout->sidebar.x = margin;
	layout->sidebar.y = margin;
	layout->sidebar.width = ML_VIEW_SIDEBAR - margin;
	layout->sidebar.height = height - 2 * margin;
	left = ML_VIEW_SIDEBAR + gap;
	layout->list.x = left;
	layout->list.y = margin;
	layout->list.width = ML_VIEW_LIST;
	layout->list.height = height - 2 * margin;
	left += ML_VIEW_LIST + gap;
	layout->reader.x = left;
	layout->reader.y = margin;
	layout->reader.width = width - left - margin;
	layout->reader.height = height - 2 * margin;
}

/*
 * Draws the folders' pane: the title, New Message, each account's folders
 * with their unread counts, and Get Mail at the bottom.
 */
static void
view_sidebar(
	struct ml_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	const struct ml_account_config *accounts;
	struct kl_rect button;
	struct kl_rect row;
	char count_text[24];
	size_t account_count;
	size_t unread;
	size_t a;
	int clicked;
	int current;
	int width;
	int folder;
	int y;

	/* The title, and New Message under it. */
	(void)kl_text_draw(style->text, style->canvas, area->x + 20, area->y + 38, "Mail", strlen("Mail"), 22U, 1, style->theme->text);
	button.x = area->x + 14;
	button.y = area->y + 54;
	button.width = area->width - 28;
	button.height = 34;
	clicked = kl_button(ui, style, ML_ID_NEW, &button, "New Message", KL_BUTTON_PRIMARY);
	if (clicked)
		ml_view_action(view, ML_ACTION_NEW, now_us);

	/* Each account: its name and address, and its folders. */
	accounts = ml_accounts(&account_count);
	y = area->y + 104;
	for (a = 0; a < account_count; a++) {
		/* The account's name, and its address under it. */
		y = kl_sidebar_section(style, area->x + 8, y, area->width - 16, accounts[a].name);
		(void)kl_text_draw_fit(style->text, style->canvas, area->x + 16, y + 4, accounts[a].address, 11U, 0, area->width - 32, style->theme->text_faint);
		y += 14;
		for (folder = 0; folder < ML_FOLDERS; folder++) {
			/* The folder; a click shows it. */
			row.x = area->x + 8;
			row.y = y;
			row.width = area->width - 16;
			row.height = 30;
			current = 0;
			if ((int)a == view->account && folder == (int)view->folder)
				current = 1;
			clicked = kl_sidebar_item(ui, style, ML_ID_FOLDER, (uint32_t)(a * ML_FOLDERS + (size_t)folder), &row, view_folder_icons[folder], ml_folder_name((enum ml_folder)folder), current);
			y += 32;

			/* The count of unread messages at the right. */
			unread = view_unread(view, (int)a, (enum ml_folder)folder);
			if (unread != 0U) {
				(void)snprintf(count_text, sizeof(count_text), "%zu", unread);
				width = kl_text_width(style->text, count_text, strlen(count_text), ML_VIEW_TEXT_SMALL, 1);
				(void)kl_text_draw(style->text, style->canvas, row.x + row.width - 10 - width, row.y + 20, count_text, strlen(count_text), ML_VIEW_TEXT_SMALL, 1, style->theme->accent);
			}

			/* Clicked: the folder shown, from its top, without a message chosen. */
			if (clicked) {
				view->account = (int)a;
				view->folder = (enum ml_folder)folder;
				view->selected = -1;
				view->composing = 0;
				kl_scroll_move_to(&view->list_scroll, 0.0, 0.0, 0, now_us);
				ml_log("FOLDER account=%zu folder=%s", a, ml_folder_name((enum ml_folder)folder));
			}
		}

		/* Space before the next account. */
		y += 10;
	}

	/* Add Account above Get Mail. */
	button.x = area->x + 14;
	button.y = area->y + area->height - 104;
	button.width = area->width - 28;
	button.height = 32;
	clicked = kl_button(ui, style, ML_ID_ADD, &button, "Add Account", 0U);
	if (clicked)
		ml_view_action(view, ML_ACTION_ADD_ACCOUNT, now_us);

	/* Get Mail at the bottom, and when it last did (or what failed). */
	button.y = area->y + area->height - 64;
	clicked = kl_button(ui, style, ML_ID_GET, &button, "Get Mail", 0U);
	if (clicked)
		ml_view_action(view, ML_ACTION_GET, now_us);
	(void)kl_text_draw_fit(style->text, style->canvas, area->x + 16, area->y + area->height - 16, view->status, 11U, 0, area->width - 32, style->theme->text_faint);
}

/*
 * Draws the list: the folder's name and counts, the search field, and a
 * row for each message the search lets through.
 */
static void
view_list(
	struct ml_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	const struct ml_account_config *accounts;
	size_t indices[ML_MESSAGES_MAX];
	struct kl_rect field;
	struct kl_rect list;
	struct kl_rect row;
	struct kl_rect button;
	char title[96];
	char counts[160];
	unsigned changes;
	unsigned hit;
	size_t account_count;
	size_t shown;
	size_t unread;
	size_t i;
	int left;
	int chosen;

	/* Back to the folders is not needed: a narrow window keeps the folder; its name and account head the list. */
	accounts = ml_accounts(&account_count);
	left = area->x + 18;
	(void)snprintf(title, sizeof(title), "%s", ml_folder_name(view->folder));
	(void)kl_text_draw(style->text, style->canvas, left, area->y + 36, title, strlen(title), ML_VIEW_TEXT_TITLE, 1, style->theme->text);

	/* The counts beside the account. */
	shown = view_shown(view, indices, ML_MESSAGES_MAX);
	unread = view_unread(view, view->account, view->folder);
	counts[0] = '\0';
	if ((size_t)view->account < account_count)
		(void)snprintf(counts, sizeof(counts), "%s \xc2\xb7 %zu messages, %zu unread", accounts[view->account].name, shown, unread);
	(void)kl_text_draw_fit(style->text, style->canvas, left, area->y + 56, counts, ML_VIEW_TEXT_SMALL, 0, area->width - 36, style->theme->text_secondary);

	/* In a narrow window, New Message at the right of the title. */
	if (view->narrow) {
		button.width = 120;
		button.height = 30;
		button.x = area->x + area->width - 16 - button.width;
		button.y = area->y + 16;
		chosen = kl_button(ui, style, ML_ID_NEW, &button, "New Message", KL_BUTTON_PRIMARY);
		if (chosen)
			ml_view_action(view, ML_ACTION_NEW, now_us);
	}

	/* The search; a change goes back to the top. */
	field.x = area->x + 14;
	field.y = area->y + 66;
	field.width = area->width - 28;
	field.height = 30;
	changes = kl_field(ui, style, ML_ID_SEARCH, &field, &view->search, "Search mail");
	if ((changes & KL_FIELD_CHANGED) != 0U)
		kl_scroll_move_to(&view->list_scroll, 0.0, 0.0, 0, now_us);

	/* The rows' viewport, which scrolls. */
	list.x = area->x;
	list.y = area->y + ML_VIEW_LIST_HEADER;
	list.width = area->width;
	list.height = area->height - ML_VIEW_LIST_HEADER;
	kl_scroll_set_size(&view->list_scroll, (double)list.width, (double)shown * ML_VIEW_ROW + 8.0, (double)list.width, (double)list.height);
	kl_ui_scroll_region(ui, ML_ID_LIST, &list, &view->list_scroll);
	kl_canvas_clip_push(style->canvas, &list);

	/* Each row: a click opens the message. */
	for (i = 0; i < shown; i++) {
		/* The row, inset. */
		row.x = list.x + 8;
		row.y = list.y + (int)i * ML_VIEW_ROW - (int)view->list_scroll.y;
		row.width = list.width - 16;
		row.height = ML_VIEW_ROW - 4;

		/* A row out of the viewport is not drawn. */
		if (row.y + row.height < list.y || row.y > list.y + list.height)
			continue;

		/* Its input. */
		hit = kl_ui_hit(ui, ML_ID_ROW, (uint32_t)indices[i], &row);
		if ((hit & KL_HIT_CLICKED) != 0U) {
			view_open(view, (long)indices[i]);
			view->opened = 1;
		}

		/* The ground under the pointer. */
		if ((hit & KL_HIT_HOT) != 0U)
			kl_canvas_round(style->canvas, (float)row.x, (float)row.y, (float)row.width, (float)row.height, 10.0f, style->theme->hover);

		/* The row's content; the chosen one is not marked in a narrow window, where the list stands alone. */
		chosen = 0;
		if ((long)indices[i] == view->selected && !view->narrow && !view->composing)
			chosen = 1;
		view_row(view, style, indices[i], &row, chosen);
	}

	/* The clip goes, the bar shows while the list moves, and a word for an empty list. */
	kl_canvas_clip_pop(style->canvas);
	(void)kl_scroll_draw_bars(&view->list_scroll, style->canvas, &list, style->theme, now_us);
	if (shown == 0U) {
		left = kl_text_width(style->text, "No messages", strlen("No messages"), ML_VIEW_TEXT_BODY, 0);
		(void)kl_text_draw(style->text, style->canvas, area->x + (area->width - left) / 2, list.y + 48, "No messages", strlen("No messages"), ML_VIEW_TEXT_BODY, 0, style->theme->text_faint);
	}
}

/*
 * Draws one message's row: the dot of an unread one, the sender, the
 * date, the subject, a line of the words, and a file's mark.
 */
static void
view_row(
	struct ml_view *view,
	const struct kl_style *style,
	size_t index,
	const struct kl_rect *row,
	int chosen)
{
	const struct ml_message *messages;
	const struct ml_message *message;
	char line[160];
	size_t count;
	size_t i;
	int unread;
	int date_width;
	int left;

	/* The message, and whether it is unread. */
	messages = ml_messages(&count);
	message = &messages[index];
	unread = view_is_unread(view, index);

	/* The selection under the chosen row, as Files shows it. */
	if (chosen)
		kl_canvas_round(style->canvas, (float)row->x, (float)row->y, (float)row->width, (float)row->height, 10.0f, style->theme->selection);

	/* The dot of an unread one. */
	if (unread)
		kl_canvas_circle(style->canvas, (float)row->x + 10.0f, (float)row->y + 20.0f, 4.0f, style->theme->accent);

	/* The date at the right, the sender (bold while unread) before it. */
	left = row->x + 22;
	date_width = kl_text_width(style->text, message->date_short, strlen(message->date_short), ML_VIEW_TEXT_SMALL, 0);
	(void)kl_text_draw(style->text, style->canvas, row->x + row->width - 10 - date_width, row->y + 24, message->date_short, strlen(message->date_short), ML_VIEW_TEXT_SMALL, 0, style->theme->text_secondary);
	(void)kl_text_draw_fit(style->text, style->canvas, left, row->y + 24, message->from_name, ML_VIEW_TEXT_NAME, unread, row->width - 40 - date_width, style->theme->text);

	/* The subject. */
	(void)kl_text_draw_fit(style->text, style->canvas, left, row->y + 44, message->subject, ML_VIEW_TEXT_SMALL + 1U, unread, row->width - 56, style->theme->text);

	/* A paper clip at the right of it when the message has a file (q826-i02: a file's icon at this size looked like a missing glyph). */
	if ((message->flags & ML_ATTACHMENT) != 0U)
		view_clip(style->canvas, (float)(row->x + row->width - 24), (float)row->y + 33.0f, 14.0f, style->theme->text_secondary);

	/* A line of the words, the line breaks as spaces. */
	(void)snprintf(line, sizeof(line), "%s", message->body);
	for (i = 0; line[i] != '\0'; i++) {
		/* A line break. */
		if (line[i] == '\n')
			line[i] = ' ';
	}

	/* The line, cut to the row. */
	(void)kl_text_draw_fit(style->text, style->canvas, left, row->y + 63, line, ML_VIEW_TEXT_SMALL, 0, row->width - 34, style->theme->text_secondary);
}

/*
 * Draws the message chosen: the bar of tools (back in a narrow window),
 * the subject, the sender with the date, a sign-in code standing out, the
 * words, and the file it carries; scrolled as a whole.
 */
static void
view_reader(
	struct ml_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	const struct ml_message *messages;
	const struct ml_message *message;
	struct kl_rect button;
	struct kl_rect body;
	char line[160];
	enum kl_icon icon;
	const char *hint;
	unsigned action;
	unsigned hit;
	size_t count;
	int clicked;
	int content;
	int x;
	int y;
	int i;

	/* Nothing chosen. */
	messages = ml_messages(&count);
	if (view->selected < 0 || (size_t)view->selected >= count) {
		x = kl_text_width(style->text, "No message selected", strlen("No message selected"), 16U, 1);
		(void)kl_text_draw(style->text, style->canvas, area->x + (area->width - x) / 2, area->y + area->height / 2, "No message selected", strlen("No message selected"), 16U, 1, style->theme->text_secondary);
		return;
	}

	/* The bar: back in a narrow window, Reply, Reply All and Forward, Archive and Delete at the right. */
	message = &messages[view->selected];
	x = area->x + 16;
	if (view->narrow) {
		button.x = area->x + 8;
		button.y = area->y + 10;
		button.width = 36;
		button.height = 36;
		hit = kl_ui_hit(ui, ML_ID_BACK, 0U, &button);
		if ((hit & KL_HIT_CLICKED) != 0U)
			view->opened = 0;
		kl_icon_draw(style->canvas, KL_ICON_BACK, (float)button.x + 7.0f, (float)button.y + 7.0f, 22.0f, style->theme->accent);
		x = area->x + 50;
	}

	/* Reply, Reply All and Forward, left to right. */
	for (i = 0; i < 3; i++) {
		/* One tool. */
		button.x = x;
		button.y = area->y + 12;
		button.width = kl_button_width(style, view_tools[i]);
		button.height = 32;
		clicked = kl_button(ui, style, ML_ID_TOOL + (uint32_t)i, &button, view_tools[i], 0U);
		if (clicked)
			ml_view_action(view, view_tool_actions[i], now_us);
		x += button.width + 8;
	}

	/* Archive and Delete, as icons. */
	for (i = 0; i < 2; i++) {
		/* One of them. */
		button.width = 36;
		button.height = 32;
		button.x = area->x + area->width - 16 - (2 - i) * 42;
		button.y = area->y + 12;
		hit = kl_ui_hit(ui, ML_ID_TOOL + 10U + (uint32_t)i, 0U, &button);

		/* Archive (the first) or Delete, clicked; its icon, on a soft ground under the pointer. */
		icon = KL_ICON_TRASH;
		action = ML_ACTION_DELETE;
		if (i == 0) {
			icon = KL_ICON_FOLDER_LINE;
			action = ML_ACTION_ARCHIVE;
		}

		/* Carried out when clicked. */
		if ((hit & KL_HIT_CLICKED) != 0U)
			ml_view_action(view, action, now_us);

		/* Drawn. */
		if ((hit & KL_HIT_HOT) != 0U)
			kl_canvas_round(style->canvas, (float)button.x, (float)button.y, (float)button.width, (float)button.height, 8.0f, style->theme->hover);
		kl_icon_draw(style->canvas, icon, (float)button.x + 8.0f, (float)button.y + 6.0f, 20.0f, style->theme->icon);
	}

	/* The bar's edge. */
	view_edge(view, style, area->x, area->y + ML_VIEW_BAR, area->width);

	/* The rest scrolls: its height measured without drawing. */
	body.x = area->x;
	body.y = area->y + ML_VIEW_BAR;
	body.width = area->width;
	body.height = area->height - ML_VIEW_BAR;
	content = 128 + view_words(style, message->body, 0, 0, area->width - 2 * ML_VIEW_PAD, ML_VIEW_TEXT_BODY, style->theme->text, 0) + 120;
	if (message->code != NULL)
		content += 76;
	kl_scroll_set_size(&view->reader_scroll, (double)body.width, (double)content, (double)body.width, (double)body.height);
	kl_ui_scroll_region(ui, ML_ID_READER, &body, &view->reader_scroll);
	kl_canvas_clip_push(style->canvas, &body);

	/* The subject. */
	x = area->x + ML_VIEW_PAD;
	y = body.y + 40 - (int)view->reader_scroll.y;
	(void)kl_text_draw_fit(style->text, style->canvas, x, y, message->subject, ML_VIEW_TEXT_TITLE, 1, area->width - 2 * ML_VIEW_PAD, style->theme->text);

	/* The sender: the picture, the name and address, to whom, and the date at the right. */
	view_avatar(style, message->from_name, message->color, x + 20, y + 44, 20);
	(void)snprintf(line, sizeof(line), "%s  <%s>", message->from_name, message->from_address);
	(void)kl_text_draw_fit(style->text, style->canvas, x + 52, y + 40, line, ML_VIEW_TEXT_NAME, 1, area->width - 2 * ML_VIEW_PAD - 60, style->theme->text);
	(void)snprintf(line, sizeof(line), "To: %s \xc2\xb7 %s", message->to, message->date_long);
	(void)kl_text_draw_fit(style->text, style->canvas, x + 52, y + 60, line, ML_VIEW_TEXT_SMALL, 0, area->width - 2 * ML_VIEW_PAD - 60, style->theme->text_secondary);
	view_edge(view, style, area->x, y + 84, area->width);
	y += 116;

	/* A sign-in code, standing out with what it is for. */
	if (message->code != NULL) {
		kl_canvas_round(style->canvas, (float)x, (float)y - 22.0f, (float)(area->width - 2 * ML_VIEW_PAD), 60.0f, 12.0f, ML_COLOR_CODE);
		(void)kl_text_draw(style->text, style->canvas, x + 16, y + 17, message->code, strlen(message->code), 24U, 1, style->theme->accent);
		(void)kl_text_draw_fit(style->text, style->canvas, x + 140, y + 4, "Sign-in code found in this message", ML_VIEW_TEXT_SMALL, 1, area->width - 2 * ML_VIEW_PAD - 156, style->theme->text);
		hint = "Allow Browser to fill it in under Add Account.";
		if (view->codes_allowed)
			hint = "Browser can fill it in for you.";
		(void)kl_text_draw_fit(style->text, style->canvas, x + 140, y + 22, hint, 11U, 0, area->width - 2 * ML_VIEW_PAD - 156, style->theme->text_secondary);
		y += 76;
	}

	/* The words. */
	y += view_words(style, message->body, x, y, area->width - 2 * ML_VIEW_PAD, ML_VIEW_TEXT_BODY, style->theme->text, 1);

	/* The file it carries, on a card. */
	if (message->file_name != NULL) {
		y += 16;
		kl_canvas_round(style->canvas, (float)x, (float)y, 280.0f, 60.0f, 14.0f, KL_RGBA(0x8a96aa, 26));
		kl_icon_file(style->canvas, style->text, (float)x + 10.0f, (float)y + 8.0f, 44.0f, style->theme->accent, NULL);
		(void)kl_text_draw_fit(style->text, style->canvas, x + 62, y + 26, message->file_name, ML_VIEW_TEXT_BODY - 1U, 1, 206, style->theme->text);
		(void)kl_text_draw_fit(style->text, style->canvas, x + 62, y + 45, message->file_detail, ML_VIEW_TEXT_SMALL, 0, 206, style->theme->text_secondary);
	}

	/* The clip goes, and the bar shows while the message moves. */
	kl_canvas_clip_pop(style->canvas);
	(void)kl_scroll_draw_bars(&view->reader_scroll, style->canvas, &body, style->theme, now_us);
}

/*
 * Draws the message being written: its title with Cancel and Send, the
 * fields To, Cc and Subject, and its words, which take the keyboard when
 * clicked.
 */
static void
view_compose(
	struct ml_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	static const char *const labels[] = { "To", "Cc", "Subject" };
	struct kl_field *fields[3];
	struct kl_rect button;
	struct kl_rect field;
	struct kl_rect body;
	int clicked;
	int x;
	int y;
	int i;

	/* The title, Cancel and Send. */
	x = area->x + ML_VIEW_PAD;
	(void)kl_text_draw(style->text, style->canvas, x, area->y + 36, "New Message", strlen("New Message"), ML_VIEW_TEXT_TITLE, 1, style->theme->text);
	button.width = 80;
	button.height = 32;
	button.x = area->x + area->width - 16 - button.width;
	button.y = area->y + 12;
	clicked = kl_button(ui, style, ML_ID_SEND, &button, "Send", KL_BUTTON_PRIMARY);
	if (clicked)
		ml_view_action(view, ML_ACTION_SEND, now_us);

	/* Cancel before it. */
	button.x -= button.width + 8;
	clicked = kl_button(ui, style, ML_ID_CANCEL, &button, "Cancel", 0U);
	if (clicked)
		ml_view_action(view, ML_ACTION_CANCEL, now_us);

	/* The bar's edge. */
	view_edge(view, style, area->x, area->y + ML_VIEW_BAR, area->width);

	/* The fields, a label each. */
	fields[0] = &view->to;
	fields[1] = &view->cc;
	fields[2] = &view->subject;
	y = area->y + ML_VIEW_BAR + 10;
	for (i = 0; i < 3; i++) {
		(void)kl_text_draw(style->text, style->canvas, x, y + 21, labels[i], strlen(labels[i]), ML_VIEW_TEXT_BODY - 1U, 0, style->theme->text_secondary);
		field.x = x + 70;
		field.y = y;
		field.width = area->width - 2 * ML_VIEW_PAD - 70;
		field.height = 32;
		(void)kl_field(ui, style, ML_ID_TO + (uint32_t)i, &field, fields[i], NULL);
		y += 40;
	}

	/* The words, in libkeiland's text area (an input method's text too, ws090-p022). */
	view_edge(view, style, area->x, y + 4, area->width);
	body.x = area->x + 8;
	body.y = y + 12;
	body.width = area->width - 16;
	body.height = area->y + area->height - body.y - 36;
	(void)kl_text_area(ui, style, ML_ID_BODY, &body, &view->body, "Write your message here.");

	/* What the mock leaves out. */
	(void)kl_text_draw_fit(style->text, style->canvas, x, area->y + area->height - 14, "Attachments and drafts are not kept yet.", 11U, 0, area->width - 2 * ML_VIEW_PAD, style->theme->text_faint);
}

/*
 * Lays out (and draws, when asked) words in a width: each paragraph broken
 * into lines, an empty line between paragraphs.  Returns the height they
 * take, or with draw -1 the width of their last line (where a caret goes).
 */
static int
view_words(
	const struct kl_style *style,
	const char *text,
	int x,
	int y,
	int width,
	unsigned pixels,
	kl_color color,
	int draw)
{
	struct kl_text_line metrics;
	size_t length;
	size_t end;
	size_t shown;
	size_t at;
	int line_height;
	int height;
	int last;
	int lines;

	/* The line's height. */
	kl_text_metrics(style->text, pixels, &metrics);
	line_height = metrics.height + 6;
	height = 0;
	last = 0;
	lines = 0;
	at = 0;
	length = strlen(text);

	/* Each line of the text (a line break ends one; an empty one is a gap). */
	while (at <= length && lines < ML_VIEW_LINES_MAX) {
		/* Where the text's line ends. */
		end = at;
		while (end < length && text[end] != '\n')
			end++;

		/* Its pieces that fit the width (an empty line is one empty piece). */
		do {
			/* One piece: what fits the width, of what is left of the line. */
			shown = end - at;
			if (shown > 0U)
				shown = kl_text_break(style->text, text + at, pixels, 0, width);

			/* No further than the line's end, and a byte at least while some is left. */
			if (shown > end - at)
				shown = end - at;
			else if (shown == 0U && end > at)
				shown = 1;

			/* Drawn on its line. */
			if (draw == 1)
				(void)kl_text_draw(style->text, style->canvas, x, y + height + metrics.ascent, text + at, shown, pixels, 0, color);
			last = kl_text_width(style->text, text + at, shown, pixels, 0);
			height += line_height;
			lines++;
			at += shown;
		} while (at < end && lines < ML_VIEW_LINES_MAX);

		/* Past the line break. */
		at = end + 1U;
	}

	/* The width of the last line, for a caret. */
	if (draw == -1)
		return last;

	/* The height. */
	return height;
}

/*
 * Draws a sender's picture: a circle of its color with the first letter of
 * its name.
 */
static void
view_avatar(
	const struct kl_style *style,
	const char *name,
	kl_color color,
	int cx,
	int cy,
	int radius)
{
	size_t index;
	unsigned pixels;
	int width;

	/* The circle. */
	kl_canvas_circle(style->canvas, (float)cx, (float)cy, (float)radius, color);

	/* The name's first character in the middle. */
	index = 0;
	(void)kl_utf8_next(name, strlen(name), &index);
	pixels = (unsigned)(radius * 8 / 10);
	width = kl_text_width(style->text, name, index, pixels, 1);
	(void)kl_text_draw(style->text, style->canvas, cx - width / 2, kl_text_center(pixels, cy - radius, 2 * radius), name, index, pixels, 1, ML_COLOR_WHITE);
}

/*
 * Draws a line across a pane at a height: inset on glass.
 */
static void
view_edge(
	const struct ml_view *view,
	const struct kl_style *style,
	int x,
	int y,
	int width)
{
	struct kl_rect edge;

	/* Across the pane, inset on glass. */
	edge.x = x;
	edge.y = y;
	edge.width = width;
	edge.height = 1;
	if (view->glass) {
		edge.x += 16;
		edge.width -= 32;
		kl_canvas_fill(style->canvas, &edge, style->theme->row_separator);
	} else {
		kl_canvas_fill(style->canvas, &edge, style->theme->separator);
	}
}

/*
 * Finds the messages of the folder shown that the search lets through
 * (its words in the sender, the subject or the words), and reports how
 * many.
 */
static size_t
view_shown(
	const struct ml_view *view,
	size_t *indices,
	size_t size)
{
	const struct ml_message *messages;
	size_t count;
	size_t found;
	size_t moved;
	size_t at;
	size_t i;
	int held;
	int newer;

	/* Each message of the folder. */
	messages = ml_messages(&count);
	found = 0;
	for (i = 0; i < count && found < size; i++) {
		/* Another account's or folder's. */
		if (messages[i].account != view->account || messages[i].folder != view->folder)
			continue;

		/* The search's words in the sender. */
		held = view_contains(messages[i].from_name, view->search.text);

		/* Else in the subject. */
		if (!held)
			held = view_contains(messages[i].subject, view->search.text);

		/* Else in the words. */
		if (!held)
			held = view_contains(messages[i].body, view->search.text);

		/* Let through. */
		if (held) {
			indices[found] = i;
			found++;
		}
	}

	/* The newest first: each one moved up past the older ones before it. */
	for (i = 1; i < found; i++) {
		moved = indices[i];
		at = i;
		while (at > 0U) {
			newer = view_newer(messages, moved, indices[at - 1U]);
			if (!newer)
				break;
			indices[at] = indices[at - 1U];
			at--;
		}

		/* Its place. */
		indices[at] = moved;
	}

	/* The number found. */
	return found;
}

/*
 * Reports how many messages of an account's folder are unread.
 */
static size_t
view_unread(
	const struct ml_view *view,
	int account,
	enum ml_folder folder)
{
	const struct ml_message *messages;
	size_t count;
	size_t found;
	size_t i;
	int unread;

	/* Each message of the folder, unread. */
	messages = ml_messages(&count);
	found = 0;
	for (i = 0; i < count; i++) {
		/* One of the folder's. */
		if (messages[i].account != account || messages[i].folder != folder)
			continue;

		/* Counted when unread. */
		unread = view_is_unread(view, i);
		if (unread)
			found++;
	}

	/* The count. */
	return found;
}

/*
 * Reports whether a message is unread: marked so, and not opened yet.
 */
static int
view_is_unread(
	const struct ml_view *view,
	size_t index)
{
	const struct ml_message *messages;
	size_t count;

	UNUSED_PARAMETER(view);

	/* A message not there. */
	messages = ml_messages(&count);
	if (index >= count)
		return 0;

	/* Not marked unread (opening one takes the mark off). */
	if ((messages[index].flags & ML_UNREAD) == 0U)
		return 0;

	/* Unread. */
	return 1;
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
		/* The bytes from there, matched one by one in lower case. */
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
 * Opens a message: shown from its top, read, and the writing of a new one
 * left.
 */
static void
view_open(
	struct ml_view *view,
	long index)
{
	struct ml_message *message;

	/* A message that is not there. */
	message = ml_store_at(index);
	if (message == NULL)
		return;

	/* Shown from its top. */
	view->selected = index;
	view->composing = 0;
	view->adding = 0;
	kl_scroll_move_to(&view->reader_scroll, 0.0, 0.0, 0, 0U);
	ml_log("OPEN message=%ld", index);

	/* Read: the mark taken off, and the server told. */
	if ((message->flags & ML_UNREAD) != 0U) {
		message->flags &= ~ML_UNREAD;
		view_request(view, ML_ACTION_SEEN, index);
	}
}

/*
 * Starts writing a reply to the message shown, to all of it, or its
 * forward: the fields filled and the message quoted under.
 */
static void
view_reply(
	struct ml_view *view,
	unsigned action)
{
	const struct ml_message *messages;
	const struct ml_message *message;
	char text[ML_BODY_MAX];
	size_t count;
	const char *kind;
	const char *prefix;

	/* A message to answer. */
	messages = ml_messages(&count);
	if (view->selected < 0 || (size_t)view->selected >= count)
		return;
	message = &messages[view->selected];

	/* The ID it answers (a forward answers none). */
	view->reply_id[0] = '\0';
	if (action != ML_ACTION_FORWARD && message->message_id != NULL)
		(void)snprintf(view->reply_id, sizeof(view->reply_id), "%s", message->message_id);

	/* A reply: to the sender, nobody in copy. */
	kind = "reply";
	prefix = "Re: ";
	kl_field_set(&view->to, message->from_address);
	kl_field_set(&view->cc, "");

	/* To all: the other receivers in copy; a forward: to nobody yet. */
	if (action == ML_ACTION_REPLY_ALL) {
		kind = "reply-all";
		if (message->cc != NULL && message->cc[0] != '\0')
			(void)snprintf(text, sizeof(text), "%s, %s", message->to, message->cc);
		else
			(void)snprintf(text, sizeof(text), "%s", message->to);
		kl_field_set(&view->cc, text);
	} else if (action == ML_ACTION_FORWARD) {
		kind = "forward";
		prefix = "Fwd: ";
		kl_field_set(&view->to, "");
	}

	/* The subject, with its prefix. */
	(void)snprintf(text, sizeof(text), "%s%s", prefix, message->subject);
	kl_field_set(&view->subject, text);

	/* The words: an empty line, then the message quoted under who wrote it when. */
	(void)snprintf(text, sizeof(text), "\n\nOn %s, %s wrote:\n\n%s", message->date_long, message->from_name, message->body);
	kl_text_area_set(&view->body, text);

	/* The caret on the empty line at the top, where the reply is written. */
	view->body.caret = 0;
	view->body.anchor = 0;

	/* Written in the third pane. */
	view->composing = 1;
	view->opened = 1;
	ml_log("COMPOSE kind=%s", kind);
}

/* Queues a request for the window (a full queue drops it: the user asks again). */
static void
view_request(
	struct ml_view *view,
	unsigned action,
	long message)
{
	/* No room. */
	if (view->request_count == ML_REQUESTS_MAX)
		return;

	/* At the end. */
	view->requests[view->request_count].action = action;
	view->requests[view->request_count].message = message;
	view->request_count++;
}

/* Names a move's action for the log. */
static const char *
view_action_name(
	unsigned action)
{
	/* The archive. */
	if (action == ML_ACTION_ARCHIVE)
		return "archive";

	/* The trash. */
	return "delete";
}

/* Tells whether message a is newer than b (by date, then by the order they came). */
static int
view_newer(
	const struct ml_message *messages,
	size_t a,
	size_t b)
{
	/* A later date. */
	if (messages[a].date != messages[b].date) {
		if (messages[a].date > messages[b].date)
			return 1;
		return 0;
	}

	/* The same date: the later one kept. */
	if (a > b)
		return 1;
	return 0;
}

/*
 * Draws the form of a new account: its fields, Sign In (and Cancel when
 * an account is there already), the status, and whether the browser may
 * fill in sign-in codes from Mail.
 */
static void
view_setup(
	struct ml_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	static const char *const labels[] = { "Your name", "Email", "Password", "IMAP server", "SMTP server" };
	static const char *const hints[] = { "Kei Example", "kei@example.net", "", "imap.example.net (993)", "smtp.example.net (465 or :587)" };
	struct kl_field *fields[5];
	struct kl_rect field;
	struct kl_rect button;
	size_t account_count;
	int clicked;
	int width;
	int on;
	int x;
	int y;
	int i;

	/* The title and what the form is for. */
	width = area->width - 2 * ML_VIEW_PAD;
	if (width > 560)
		width = 560;
	x = area->x + (area->width - width) / 2;
	y = area->y + 60;
	(void)kl_text_draw(style->text, style->canvas, x, y, "Add an Account", strlen("Add an Account"), 22U, 1, style->theme->text);
	(void)kl_text_draw_fit(style->text, style->canvas, x, y + 26, "Mail reads with IMAP and sends with SMTP. With Gmail, use an app password.", ML_VIEW_TEXT_SMALL, 0, width, style->theme->text_secondary);
	y += 52;

	/* The fields, a label each. */
	fields[0] = &view->setup_name;
	fields[1] = &view->setup_address;
	fields[2] = &view->setup_password;
	fields[3] = &view->setup_imap;
	fields[4] = &view->setup_smtp;
	for (i = 0; i < 5; i++) {
		(void)kl_text_draw(style->text, style->canvas, x, y + 21, labels[i], strlen(labels[i]), ML_VIEW_TEXT_BODY - 1U, 0, style->theme->text_secondary);
		field.x = x + 120;
		field.y = y;
		field.width = width - 120;
		field.height = 32;
		(void)kl_field(ui, style, ML_ID_SETUP + (uint32_t)i, &field, fields[i], hints[i]);
		y += 42;
	}

	/* Sign In, and Cancel when an account is there to go back to. */
	y += 8;
	button.width = 110;
	button.height = 34;
	button.x = x + width - button.width;
	button.y = y;
	clicked = kl_button(ui, style, ML_ID_SIGN_IN, &button, "Sign In", KL_BUTTON_PRIMARY);
	if (clicked)
		ml_view_action(view, ML_ACTION_SIGN_IN, now_us);
	(void)ml_accounts(&account_count);
	if (account_count != 0U) {
		button.x -= button.width + 8;
		clicked = kl_button(ui, style, ML_ID_SETUP_CANCEL, &button, "Cancel", 0U);
		if (clicked)
			ml_view_action(view, ML_ACTION_CANCEL, now_us);
	}

	/* What the last try said. */
	(void)kl_text_draw_fit(style->text, style->canvas, x, y + 22, view->status, ML_VIEW_TEXT_SMALL, 0, width - 240, style->theme->text_secondary);
	y += 70;

	/* The browser and the sign-in codes (the desktop's setting). */
	(void)kl_text_draw(style->text, style->canvas, x, y + 4, "Sign-in codes", strlen("Sign-in codes"), ML_VIEW_TEXT_NAME, 1, style->theme->text);
	(void)kl_text_draw_fit(style->text, style->canvas, x, y + 24, "Let Browser fill in sign-in codes from the mail that comes in.", ML_VIEW_TEXT_SMALL, 0, width - 70, style->theme->text_secondary);
	on = view->codes_allowed;
	clicked = kl_switch(ui, style, ML_ID_CODES, x + width - 52, y - 4, &on, 0U);
	if (clicked) {
		view->codes_allowed = on;
		view_request(view, ML_ACTION_CODES, -1);
		ml_log("CODES allowed=%d", on);
	}
}

/*
 * Shows a notice for a while.
 */
void
ml_view_notice(
	struct ml_view *view,
	const char *message,
	uint64_t now_us)
{
	/* The words and until when. */
	(void)snprintf(view->notice, sizeof(view->notice), "%s", message);
	view->notice_until = now_us + ML_VIEW_NOTICE_US;
}

/*
 * Takes the oldest request the view queued: 1 with it, 0 when none waits.
 */
int
ml_view_take_request(
	struct ml_view *view,
	struct ml_request *request)
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

/* Draws a paper clip in a square of size from (x, y): a long loop and the inner wire up its middle. */
static void
view_clip(
	struct kl_canvas *canvas,
	float x,
	float y,
	float size,
	kl_color color)
{
	float width;
	float left;

	/* The loop, as tall as the square and half as wide, round at both ends. */
	width = 0.5f * size;
	left = x + 0.25f * size;
	kl_canvas_round_border(canvas, left, y, width, size, 0.5f * width, 1.5f, color);

	/* The inner wire, from near the bottom up to two thirds. */
	kl_canvas_line(canvas, left + 0.5f * width, y + 0.30f * size, left + 0.5f * width, y + 0.78f * size, 1.5f, color);
}
