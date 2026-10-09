/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The model of Settings (WS089): its pages, the window's layout, the
 * clickable regions of a frame, the history of pages and what the titlebar
 * and the menus show.
 *
 * Nothing here knows about Wayland or Vulkan.  The window's parts
 * (window.h) hand inputs to the interface and show the frames it draws;
 * the host tests drive the same interface and draw its frames into
 * pictures.  The drawing surface is libkeiland's canvas, text and icons
 * (kl_canvas, kl_text, kl_icon; ws090-p009 replaced the file manager's
 * copies that were compiled in before).
 */

#ifndef SETTINGS_SETTINGS_H
#define SETTINGS_SETTINGS_H

#include "storage-scan.h"
#include "storage-trash.h"
#include "arrange.h"

#include <keiland/keiland.h>

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* The window's size when the compositor leaves it to the program. */
#define SE_WIDTH		1180
#define SE_HEIGHT		800

/* How many clickable regions one frame records. */
#define SE_HITS			512

/* How many pages the history of pages keeps. */
#define SE_HISTORY		64

/* The bytes of a titlebar's text with its NUL, and of one part of the breadcrumb. */
#define SE_TITLEBAR_TEXT	1024
#define SE_TITLEBAR_PART	65

/* How many parts the breadcrumb has at most: Settings and the page. */
#define SE_CRUMBS		2

/* How many things done with the titlebar wait for the main loop at most. */
#define SE_TITLEBAR_EVENTS	16U

/* How many glass panels a frame has at most: the list of pages and the page. */
#define SE_PANELS		2

/*
 * The colours of the interface, the file manager's (ws071) so that the two
 * windows look alike: quiet slate text, the accent for what is chosen.
 * There are two sets, the light appearance's and the dark one's
 * (ws089-p017, palette.c); se_palette is the set of the appearance the
 * compositor told last, and every colour is read through it.
 */
struct se_palette {
	/* The window's ground, at the top and at the bottom (a gradient). */
	kl_color background_top;
	kl_color background_bottom;

	/* A panel and its edge. */
	kl_color panel;
	kl_color panel_edge;

	/* The veils over the compositor's glass: the list's and the page's. */
	kl_color glass_sidebar;
	kl_color glass_page;

	/* A card, its edge, a tile and a tile under the pointer. */
	kl_color card;
	kl_color card_edge;
	kl_color tile;
	kl_color tile_hover;

	/* The text: the main ink, the secondary, the faint, a heading's and an icon's. */
	kl_color text;
	kl_color text_secondary;
	kl_color text_faint;
	kl_color title;
	kl_color icon;

	/* The accent, a selection with and without the keyboard, the pointer's hover and a separator. */
	kl_color accent;
	kl_color selection;
	kl_color selection_inactive;
	kl_color hover;
	kl_color separator;

	/* Good and bad news. */
	kl_color good;
	kl_color bad;

	/* A control's ground and edge, a field's ground, a switch's track when off, a slider's rail, what a control is faded towards when it does nothing, and the ink a pressed control is darkened (or lightened) with. */
	kl_color control;
	kl_color control_edge;
	kl_color field;
	kl_color track;
	kl_color rail;
	kl_color faded;
	kl_color pressed;

	/* The accent as the colour of text on the page (ws179-p001). */
	kl_color accent_text;
};

/* The set in use, and the choice of the appearance's (KL_APPEARANCE_*; palette.c). */
extern const struct se_palette *se_palette;
void se_palette_set(unsigned appearance);
const struct se_palette *se_palette_of(unsigned appearance);

#define SE_COLOR_BACKGROUND_TOP	(se_palette->background_top)
#define SE_COLOR_BACKGROUND_BOTTOM	(se_palette->background_bottom)
#define SE_COLOR_PANEL		(se_palette->panel)
#define SE_COLOR_PANEL_EDGE	(se_palette->panel_edge)
#define SE_COLOR_GLASS_SIDEBAR	(se_palette->glass_sidebar)
#define SE_COLOR_GLASS_PAGE	(se_palette->glass_page)
#define SE_COLOR_CARD		(se_palette->card)
#define SE_COLOR_CARD_EDGE	(se_palette->card_edge)
#define SE_COLOR_TILE		(se_palette->tile)
#define SE_COLOR_TILE_HOVER	(se_palette->tile_hover)
#define SE_COLOR_TEXT		(se_palette->text)
#define SE_COLOR_TEXT_SECONDARY	(se_palette->text_secondary)
#define SE_COLOR_TEXT_FAINT	(se_palette->text_faint)
#define SE_COLOR_TITLE		(se_palette->title)
#define SE_COLOR_ICON		(se_palette->icon)
#define SE_COLOR_ACCENT		(se_palette->accent)
#define SE_COLOR_ACCENT_TEXT	(se_palette->accent_text)
#define SE_COLOR_SELECTION	(se_palette->selection)
#define SE_COLOR_SELECTION_INACTIVE	(se_palette->selection_inactive)
#define SE_COLOR_HOVER		(se_palette->hover)
#define SE_COLOR_SEPARATOR	(se_palette->separator)
#define SE_COLOR_GOOD		(se_palette->good)
#define SE_COLOR_BAD		(se_palette->bad)
#define SE_COLOR_CONTROL		(se_palette->control)
#define SE_COLOR_CONTROL_EDGE	(se_palette->control_edge)
#define SE_COLOR_FIELD		(se_palette->field)
#define SE_COLOR_TRACK		(se_palette->track)
#define SE_COLOR_RAIL		(se_palette->rail)
#define SE_COLOR_FADED		(se_palette->faded)
#define SE_COLOR_PRESSED		(se_palette->pressed)

/* The modifier keys held with an input, the program's own bits. */
#define SE_MOD_SHIFT		0x01U
#define SE_MOD_CTRL		0x02U
#define SE_MOD_ALT		0x04U
#define SE_MOD_SUPER		0x08U

/* The pointer buttons, as evdev codes (BTN_LEFT, BTN_RIGHT). */
#define SE_BUTTON_LEFT		0x110U
#define SE_BUTTON_RIGHT		0x111U

/* The evdev codes of the keys the interface acts on. */
#define SE_KEY_ESC		1U
#define SE_KEY_BACKSPACE	14U
#define SE_KEY_TAB		15U
#define SE_KEY_ENTER		28U
#define SE_KEY_LEFTSHIFT	42U
#define SE_KEY_RIGHTSHIFT	54U
#define SE_KEY_SPACE		57U
#define SE_KEY_Q		16U
#define SE_KEY_W		17U
#define SE_KEY_F		33U
#define SE_KEY_HOME		102U
#define SE_KEY_UP		103U
#define SE_KEY_PAGE_UP		104U
#define SE_KEY_LEFT		105U
#define SE_KEY_RIGHT		106U
#define SE_KEY_END		107U
#define SE_KEY_DOWN		108U
#define SE_KEY_PAGE_DOWN	109U

/*
 * The kinds of input the window gives the interface.
 */
enum se_event_type {
	SE_EVENT_MOTION,
	SE_EVENT_BUTTON,
	SE_EVENT_AXIS,
	SE_EVENT_LEAVE,
	SE_EVENT_KEY,
	SE_EVENT_FOCUS,
	SE_EVENT_ACTION,
	SE_EVENT_AXIS_STOP,
	SE_EVENT_TEXT,
	SE_EVENT_TEXT_DELETE,
	SE_EVENT_PREEDIT
};

/* The longest text an input method sends at once, with its NUL (KL_WINDOW_TEXT_MAX). */
#define SE_TEXT_INPUT_MAX	256U

/* What a scroll came from (se_event's source, BUG-211): a wheel, or a touch pad's fingers. */
#define SE_SOURCE_WHEEL		0U
#define SE_SOURCE_FINGER	1U

/*
 * One input: where the pointer is, which button or key, the modifiers
 * held, and a menu's action; a scroll's source and the compositor's time
 * of it (axis_ms, milliseconds: the touch pad's moves keep their own
 * spacing however late they are read, BUG-211).  serial is the compositor's serial of a
 * button press.  touch is 1 for the pointer's moves and presses a finger
 * on the touch screen made (a drag of the finger scrolls a pane, ws089-p012).
 */
struct se_event {
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
	int touch;
	unsigned source;
	uint32_t axis_ms;
	char text[SE_TEXT_INPUT_MAX];
	uint32_t before;
};

/*
 * The pages, in the order the list of pages shows them.  Home is not in
 * the list; the titlebar's Home control opens it.
 */
enum se_page_id {
	SE_PAGE_HOME,
	SE_PAGE_WIFI,
	SE_PAGE_ETHERNET,
	SE_PAGE_BLUETOOTH,
	SE_PAGE_VPN,
	SE_PAGE_NETWORK,
	SE_PAGE_APPEARANCE,
	SE_PAGE_WALLPAPER,
	SE_PAGE_NOTIFICATIONS,
	SE_PAGE_SOUND,
	SE_PAGE_DISPLAY,
	SE_PAGE_LANGUAGES,
	SE_PAGE_STORAGE,
	SE_PAGE_POWER,
	SE_PAGE_KEYBOARD,
	SE_PAGE_MOUSE,
	SE_PAGE_TOUCHPAD,
	SE_PAGE_PRINTERS,
	SE_PAGE_SHARING,
	SE_PAGE_USERS,
	SE_PAGE_SECURITY_KEYS,
	SE_PAGE_UPDATES,
	SE_PAGE_ABOUT,
	SE_PAGES
};

/*
 * The groups the list of pages is divided into, a thin line between two.
 */
enum se_group {
	SE_GROUP_NONE,
	SE_GROUP_CONNECTIVITY,
	SE_GROUP_PERSONALIZATION,
	SE_GROUP_DEVICES,
	SE_GROUP_SYSTEM
};

/*
 * The line pictures of the pages (glyphs.c), drawn in a square box.
 */
enum se_glyph {
	SE_GLYPH_GRID,
	SE_GLYPH_WIFI,
	SE_GLYPH_ETHERNET,
	SE_GLYPH_BLUETOOTH,
	SE_GLYPH_SHIELD,
	SE_GLYPH_GLOBE,
	SE_GLYPH_PALETTE,
	SE_GLYPH_PICTURE,
	SE_GLYPH_BELL,
	SE_GLYPH_SPEAKER,
	SE_GLYPH_MONITOR,
	SE_GLYPH_DISK,
	SE_GLYPH_BATTERY,
	SE_GLYPH_KEYBOARD,
	SE_GLYPH_MOUSE,
	SE_GLYPH_TOUCHPAD,
	SE_GLYPH_PRINTER,
	SE_GLYPH_SHARE,
	SE_GLYPH_PEOPLE,
	SE_GLYPH_EYE,
	SE_GLYPH_LOCK,
	SE_GLYPH_PERSON,
	SE_GLYPH_REFRESH,
	SE_GLYPH_INFO,
	SE_GLYPH_CHEVRON
};

struct se_app;

/*
 * One page of Settings: its identity, where the list shows it, its
 * words, and how it is drawn.
 *
 * The table of pages lives in pages.c for the whole run.  word names the
 * page on the command line (settings network).  ready is 0 for a page
 * that shows only its frame and "coming in a later version".  draw lays
 * the page's cards out from a top edge within a column and returns the
 * bottom edge of what it drew; press carries out a click on one of the
 * page's own controls (its hit index); key takes a key press first and
 * returns 1 when it used it (a text field has the keyboard); drag
 * follows a press held on one of the page's controls (a slider) through
 * its moves to its release; any may be NULL.
 */
struct se_page {
	unsigned id;
	unsigned group;
	unsigned glyph;
	const char *name;
	const char *summary;
	const char *word;
	const char *keywords;
	int ready;
	int (*draw)(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
	void (*press)(struct se_app *app, int index);
	int (*key)(struct se_app *app, const struct se_event *event);
	void (*drag)(struct se_app *app, int index, int x, unsigned phase);
};

/*
 * The phases of a drag on a page's control (a slider): the press, each
 * move while the button is held, and the release (wherever it lands).
 */
enum se_drag_phase {
	SE_DRAG_START,
	SE_DRAG_MOVE,
	SE_DRAG_END
};

/*
 * The kinds of clickable region a frame records.
 */
enum se_hit_kind {
	SE_HIT_NONE,
	SE_HIT_PAGE_ROW,
	SE_HIT_SIDEBAR,
	SE_HIT_PAGE,
	SE_HIT_TILE,
	SE_HIT_CONTROL,
	SE_HIT_RESULT
};

/*
 * One clickable region of the last frame: where it is, what it is and
 * which one (a page's ID, or a page's own control).
 */
struct se_hit {
	struct kl_rect rect;
	unsigned kind;
	int index;
};

/*
 * The panes of the last frame: the list of pages and the page, each a
 * card standing on the compositor's frosted glass.  A hidden list has no size.
 */
struct se_layout {
	struct kl_rect sidebar;
	struct kl_rect page;
};

/* The kind of glass panel the window has: a card floating in the window. */
#define SE_PANEL_CARD		0U

/*
 * One part of the window that stands on the compositor's frosted glass: its
 * rectangle in the window, its corners' radius and its kind.
 */
struct se_panel {
	struct kl_rect rect;
	int radius;
	unsigned kind;
};

/* How many networks, interfaces, DNS servers and saved networks Settings keeps. */
#define SE_NETWORK_SCAN		KL_NETWORK_SCAN_MAX
#define SE_NETWORK_LINKS	KL_NETWORK_LINKS_MAX
#define SE_NETWORK_DNS		KL_NETWORK_DNS_MAX
#define SE_NETWORK_SAVED	KL_NETWORK_SAVED_MAX

/*
 * The network's requests Settings makes: none, the daemon's (KL_NETWORK_JOIN
 * to KL_NETWORK_WIFI_OFF; no scan, ws089-p021), and a key saved and its
 * network joined, which the compositor carries out as one request.
 */
#define SE_NETWORK_NONE		0U
#define SE_NETWORK_SAVE_KEY	6U

/* How many seconds of the network's activity the graph keeps (one sample a second). */
#define SE_USAGE_SAMPLES	120U

/* The bytes of a message the network pages show, and of a key typed (63 characters and its NUL). */
#define SE_MESSAGE		160U
#define SE_KEY_TEXT		64U

/*
 * What a join is: none, a join of a network whose key is saved, or a join
 * with a key just typed (the compositor saves the key, tells the daemon and
 * joins, WS131 p011).
 */
enum se_join_step {
	SE_JOIN_NONE,
	SE_JOIN_KEY,
	SE_JOIN_CONNECT
};


/*
 * The network as the network pages show it (network.c keeps it up to date
 * through libkeiland's kl_system_*; the host tests fill it by hand).
 *
 * live is 1 when the desktop offers the network (Keiland's system
 * extension); without it the pages say so.  state and scan are the
 * compositor's last reports, and links, dns and saved its last details,
 * asked for now and then (details_id while asked, details_known once one
 * came).  request is the request outstanding (SE_NETWORK_NONE when none)
 * and request_id the number its answer carries; join_step and join_ssid
 * carry a join through it.  One request goes at a time, the system bar's
 * included: a switch, a disconnect or a join asked for while another
 * request is out waits in one slot -- pending_request (NONE when empty),
 * the join's step and its network -- and is sent when that one is
 * answered (ws089-p012 C1, as the system bar does since ws005-p019); one
 * the compositor answered busy (the system bar's request was out) waits
 * there until retry_at.  scan_received tells that a scan's report arrived,
 * so an empty list means no network is in reach rather than none looked
 * for yet.  scanning is 1 while the compositor was asked to keep the
 * radios scanning, which it is while a page that lists the networks
 * around is shown (ws089-p021; there is no Scan button), and scanning_at
 * is when it was last asked (asked again every 30 seconds: the compositor
 * lets an asking go after a minute).
 * key_ssid names the network whose key is being typed (empty when the key
 * form is closed); key_reveal asks the next frame to scroll the page so
 * that the form, under its network's row, is in sight (BUG-160).  The
 * usage ring holds the bytes a second received and sent, newest at
 * usage_next - 1.
 */
struct se_network {
	int live;
	struct kl_network_state state;
	struct kl_network_ap scan[SE_NETWORK_SCAN];
	size_t scan_count;
	int scan_received;
	int scanning;
	uint64_t scanning_at;
	struct kl_network_link links[SE_NETWORK_LINKS];
	size_t link_count;
	char dns[SE_NETWORK_DNS][KL_NETWORK_ADDRESS_MAX];
	size_t dns_count;
	char saved[SE_NETWORK_SAVED][KL_NETWORK_SSID_MAX];
	size_t saved_count;
	uint32_t details_id;
	int details_known;
	uint64_t details_at;
	unsigned request;
	uint32_t request_id;
	unsigned join_step;
	char join_ssid[KL_NETWORK_SSID_MAX];
	unsigned pending_request;
	unsigned pending_step;
	char pending_ssid[KL_NETWORK_SSID_MAX];
	uint64_t retry_at;
	char key_ssid[KL_NETWORK_SSID_MAX];
	struct kl_field key;
	struct kl_field join_key;
	int wifi_wanted;
	uint64_t wifi_until;
	char wifi_words[96];
	int key_shown;
	int key_reveal;
	char message[SE_MESSAGE];
	int message_bad;
	uint64_t sampled_at;
	uint64_t received_total;
	uint64_t sent_total;
	uint32_t received[SE_USAGE_SAMPLES];
	uint32_t sent[SE_USAGE_SAMPLES];
	unsigned usage_count;
	unsigned usage_next;
	uint32_t wired_request;
};

/*
 * The Ethernet page's editor of a wired interface's IPv4 configuration
 * (wired.c, page-wired.c, ws089-p022): the interface being edited (empty
 * while none is), DHCP or a static address (KL_WIRED_*), the fields
 * (address, netmask, router, DNS 1, DNS 2; only the DNS servers count with
 * DHCP), the field with the keyboard, whether an Apply is waiting for its
 * answer (request), and the last message (red when it tells of a failure).
 */
#define SE_WIRED_FIELDS		5
#define SE_WIRED_ADDRESS	0
#define SE_WIRED_NETMASK	1
#define SE_WIRED_ROUTER		2
#define SE_WIRED_DNS1		3
#define SE_WIRED_DNS2		4
struct se_wired {
	char interface[KL_NETWORK_NAME_MAX];
	unsigned mode;
	struct kl_field fields[SE_WIRED_FIELDS];
	int focus;
	int asked;
	uint32_t request;
	char message[SE_MESSAGE];
	int message_bad;
};

/*
 * The Storage page's analysis and Trash (storage.c, page-storage.c,
 * ws089-p023): the folder analysed (the home unless the user went into a
 * folder) and its scan with the view last copied, the home; the trash's
 * folder, the scan of its files (its size) and its emptying, whether the
 * Empty button waits for its confirmation, the last message about it, and
 * when the page was last drawn again for a new count; whether the
 * desktop's recent list is kept (read once, q824) and the last message
 * about it (red for a failure).
 */
struct se_storage {
	char home[SE_SCAN_PATH];
	char root[SE_SCAN_PATH];
	struct se_scan scan;
	struct se_scan_view view;
	char trash_path[SE_SCAN_PATH];
	struct se_scan trash_scan;
	struct se_scan_view trash_view;
	int trash_asked;
	struct se_trash trash;
	int confirming;
	char message[SE_MESSAGE];
	int message_bad;
	uint64_t drawn_generation;
	uint64_t drawn_at;
	int recent_known;
	int recent_keep;
	char recent_message[SE_MESSAGE];
	int recent_bad;
};

/*
 * The Sharing page's Remote Login (page-sharing.c, ws089-p025): its state
 * as the desktop last told it, whether it was asked for since the page was
 * shown, the request awaited (0 for none), and the last message (red when
 * it tells of a failure).
 */
struct se_sharing {
	struct kl_sharing_state state;
	int asked;
	uint32_t request;
	char message[SE_MESSAGE];
	int message_bad;
};

/*
 * The Display page (ws113-p006, page-display.c): the displays and the mode
 * the desktop last told, the draft the page edits (the places and the mode
 * the user arranged, apart from the desktop's until Apply), whether the
 * draft differs, a card being dragged (its index, the pointer's offset in
 * it in the plane's units), the plane's mapping onto the arrangement's box
 * in the last frame (arrange.h), the light's slider (its rectangle, being
 * dragged, the light shown and when it was last sent), the requests
 * awaited (0 for none; a display turned off or on too, ws113-p014), the
 * last answer (red for a failure), and whether the first snapshot was
 * taken.
 */
struct se_display {
	struct kl_display displays[KL_DISPLAYS_MAX];
	size_t count;
	unsigned mode;
	struct kl_display draft[KL_DISPLAYS_MAX];
	unsigned draft_mode;
	unsigned edited;
	int dragging;
	int drag_index;
	int32_t grab_x;
	int32_t grab_y;
	struct se_arrange_view view;
	struct kl_rect slider;
	int light_dragging;
	unsigned light;
	uint64_t light_sent_ms;
	uint32_t request;
	uint32_t light_request;
	uint32_t shown_request;
	char message[SE_MESSAGE];
	int message_bad;
	int taken;
};

/* The Printers page's fields (ws145-p004): the address, the port, the path or queue; an edit's name and path or queue (ws177-p025). */
#define SE_PRINTER_FIELDS	3
#define SE_PRINTER_EDIT_FIELDS	2

/*
 * The Printers page (ws145-p004): the protocol chosen for an addition
 * (KL_PRINTER_IPP or _LPD, 0 for IPP), the fields, the printer being
 * edited (0 for none) and its fields (ws177-p025), the field with the
 * keyboard (0 to 2 the addition's, 3 and 4 the edit's) and whether one has
 * it, the request asked (0 for none) and its kind, and the last answer
 * (red for a failure).
 */
struct se_printers {
	unsigned protocol;
	struct kl_field fields[SE_PRINTER_FIELDS];
	uint32_t editing;
	struct kl_field edit_fields[SE_PRINTER_EDIT_FIELDS];
	int focus;
	int typing;
	uint32_t request;
	char doing[16];
	char message[SE_MESSAGE];
	int message_bad;
};

/*
 * The Bluetooth page (ws143-p006): whether its state was logged once
 * (T1-438: the log says it even when no change comes), whether the desktop was told to watch,
 * when the scan was last asked (0: not scanning), the devices as last
 * drawn (the clicks name them by their place), the request asked (0 for
 * none) and its kind, and the last answer (red for a failure).
 */
struct se_bluetooth {
	int state_logged;
	int watching;
	uint64_t scan_at;
	struct kl_bluetooth_device drawn[KL_BLUETOOTH_DEVICES_MAX];
	size_t drawn_count;
	uint32_t request;
	char doing[16];
	char message[SE_MESSAGE];
	int message_bad;
};

/*
 * The Languages page's system language (ws158-p004): the language of the
 * login screen as /etc/keiland/language holds it (whether it was read, and
 * -1 not set, else 0 English, 1 Japanese), the one an administrator chose
 * to set (the system's when it is read), their password, whether the
 * field has the keyboard, the change asked
 * (its request, 0 for none), and the last message (red for a failure).
 */
struct se_languages {
	int system_read;
	int system;
	int chosen;
	struct kl_field password;
	int focused;
	int asked;
	uint32_t request;
	/*
	 * The reading of the computer asked after a change was made (ws188-p002;
	 * 0 for none) and the change's request it reports, so that the change's
	 * log line carries the language read after it.
	 */
	uint32_t reload_request;
	uint32_t reload_for;
	char message[SE_MESSAGE];
	int message_bad;
};

/* The Users page's password fields: the current password, the new one, the new one again. */
#define SE_USERS_FIELDS		3

/* Whose fields have the Users page's keyboard: the password card's, or the administration's. */
#define SE_USERS_KEYBOARD_PASSWORD	0
#define SE_USERS_KEYBOARD_ADMIN		1

/* The most users the Users page lists (ws089-p026). */
#define SE_USERS_LIST_MAX	64

/*
 * One user of the list (ws089-p026): the name, the full name, whether the
 * user is an administrator (a member of wheel), may control Wi-Fi (a
 * member of network), and whether it is the user Settings runs as.
 */
struct se_user_row {
	char name[64];
	char full_name[128];
	int admin;
	int network;
	int self;
};

/*
 * The administrator's changes of the Users page (ws089-p026,
 * page-users-admin.c): none chosen, adding a user, resetting the chosen
 * user's password, removing the chosen user, and turning the chosen user's
 * administrator or Wi-Fi membership on or off.
 */
enum se_admin_mode {
	SE_ADMIN_NONE,
	SE_ADMIN_ADD,
	SE_ADMIN_RESET,
	SE_ADMIN_REMOVE,
	SE_ADMIN_WHEEL,
	SE_ADMIN_NETWORK
};

/* The administration's fields: the new user's name and full name, a new password, and the administrator's own. */
#define SE_ADMIN_FIELDS		4
#define SE_ADMIN_NAME		0
#define SE_ADMIN_FULL_NAME	1
#define SE_ADMIN_PASSWORD	2
#define SE_ADMIN_YOURS		3

/*
 * The Users page (page-users.c, ws160-p002): the account as the passwd
 * database has it (read once: read), the three password fields (wiped when
 * the change is asked, when Esc empties them and when the window closes),
 * the field with the keyboard, whether the passwords are shown, the change
 * asked and its request's number, and the last answer (bad when it
 * failed).
 *
 * The administration (ws089-p026): the user chosen in the list (its row
 * plus one, 0 for none), whose fields have the keyboard
 * (SE_USERS_KEYBOARD_*), the change being made, its fields (wiped when it
 * is asked or cancelled) and the one with the keyboard, its switch (the
 * new user an administrator; the home removed with the user), the change
 * asked and its request's number, and the last answer (bad when it
 * failed).
 *
 * The PIN and the security keys have their own page since ws199-p001
 * (struct se_keys).
 */
struct se_users {
	int read;
	char name[64];
	char full_name[128];
	char home[256];
	int self_admin;
	char selected_name[64];
	struct se_user_row rows[SE_USERS_LIST_MAX];
	int row_count;
	struct kl_field fields[SE_USERS_FIELDS];
	int focus;
	int shown;
	int asked;
	uint32_t request;
	char message[SE_MESSAGE];
	int message_bad;

	int selected;
	int keyboard;
	enum se_admin_mode admin_mode;
	struct kl_field admin_fields[SE_ADMIN_FIELDS];
	int admin_focus;
	int admin_flag;
	int admin_asked;
	uint32_t admin_request;
	char admin_message[SE_MESSAGE];
	int admin_bad;
};

/*
 * The popup (dialog.c, ws199-p001 section 3.2): whether it is open, its
 * step's title, place among the steps, text and button, whether Back is
 * offered, its fields (each with a label, a placeholder, a kind and, for
 * a PIN of digits, the most digits; 0: any text) and the one with the
 * keyboard, the small link, the line of what went wrong, whether it is
 * busy (and with what, and whether Cancel works then), when it last had
 * input, and its owner's two functions.
 */
#define SE_DIALOG_FIELDS	3
enum se_dialog_action {
	SE_DIALOG_PRIMARY,
	SE_DIALOG_BACK,
	SE_DIALOG_CANCEL,
	SE_DIALOG_LINK,
	SE_DIALOG_IDLE
};
struct se_dialog {
	int open;
	char title[64];
	unsigned step;
	unsigned steps;
	char body[320];
	char primary[32];
	int can_back;
	unsigned field_count;
	char labels[SE_DIALOG_FIELDS][48];
	char placeholders[SE_DIALOG_FIELDS][64];
	unsigned kinds[SE_DIALOG_FIELDS];
	size_t digits[SE_DIALOG_FIELDS];
	struct kl_field fields[SE_DIALOG_FIELDS];
	int focus;
	char link[64];
	char error[SE_MESSAGE];
	int busy;
	int cancellable;
	int final;
	char busy_text[SE_MESSAGE];
	uint64_t input_ms;
	void (*act)(struct se_app *app, unsigned action);
	int (*ready)(const struct se_app *app);
};

/*
 * The Security Keys page's wizards (ws199-p001, page-users-keys.c and
 * page-users-pin.c): the one under way and its step, the account's
 * password kept between the steps that ask it and the one that sends it
 * (wiped then, when the popup closes, and when it is left alone), the new
 * key's name and the key a removal names, the change asked and its
 * request's number, and whether the key waits to be touched; with the
 * keys' own operations (i03): a new PIN kept from its step to the
 * registration that uses it, what the keys there are (asked, its request,
 * known), the part of an addition under way (its first PIN, or the
 * registration), and how many registrations a reset removed.
 */
enum se_keys_flow {
	SE_KEYS_FLOW_NONE,
	SE_KEYS_FLOW_ADD,
	SE_KEYS_FLOW_REMOVE,
	SE_KEYS_FLOW_PIN_SET,
	SE_KEYS_FLOW_PIN_REMOVE,
	SE_KEYS_FLOW_KEY_PIN,
	SE_KEYS_FLOW_RESET,
	SE_KEYS_FLOW_OPTIONS
};
struct se_keys {
	unsigned flow;
	unsigned step;
	struct kl_field password;
	char name[KL_SYSTEM_KEY_LABEL_MAX + 1U];
	char ref[KL_SYSTEM_KEY_REF_MAX + 1U];
	char label[KL_SYSTEM_KEY_LABEL_MAX + 1U];
	int asked;
	uint32_t request;
	int touch;
	struct kl_field pin;
	int info_asked;
	uint32_t info_request;
	int info_known;
	struct kl_system_key_info info;
	int setting_pin;
	unsigned removed;
	unsigned option_pin;
	unsigned option_touch;
	int weaker;
};

/*
 * The readings of the computer Settings asks the desktop for (machine.c,
 * ws188-p002, plan/ws188/phase001/phase.md section D6), one slot a part
 * (About's names, the file systems, the users, the login screen's
 * language): the request asked and when (0: none asked), when the last
 * one failed (0: it did not), when the part was last copied and its serial
 * then.  wanted has the KL_MACHINE_* parts the pages want known, and
 * filesystems_ms when Home or Storage last wanted the file systems.
 * recent keeps the last queries' numbers, so that an answer to one a
 * newer query replaced is still recognised as the computer's.
 */
#define SE_MACHINE_PARTS	4
#define SE_MACHINE_RECENT	8
struct se_machine {
	uint32_t asked[SE_MACHINE_PARTS];
	uint64_t asked_ms[SE_MACHINE_PARTS];
	uint64_t failed_ms[SE_MACHINE_PARTS];
	uint64_t copied_ms[SE_MACHINE_PARTS];
	uint32_t serials[SE_MACHINE_PARTS];
	unsigned wanted;
	uint64_t filesystems_ms;
	uint32_t recent[SE_MACHINE_RECENT];
	unsigned recent_next;
};

/*
 * What About shows of the machine: the system's names as the desktop read
 * them (machine.c, ws188-p002), the graphics device and the screen as the
 * window learned them, and the memory as the monitor tells it.  An empty text is a value that could not be read, and the
 * row is not shown; system is the version's name of /etc/os-release
 * (PRETTY_NAME, ws089-p027), shown as the operating system, which is
 * "Kei" when it is empty.
 */
struct se_about {
	char system[128];
	char kernel[160];
	char machine[80];
	char processor[64];
	char host[64];
	char graphics[128];
	char display[64];
	unsigned cores;
	int memory_known;
	uint64_t memory_total;
	uint64_t memory_free;
};

/*
 * The actions of the menus and the keys, carried out by se_ui_action.
 * Going to a page is SE_ACTION_PAGE_FIRST plus the page's ID.
 */
enum se_action {
	SE_ACTION_NONE,
	SE_ACTION_CLOSE_WINDOW,
	SE_ACTION_QUIT,
	SE_ACTION_BACK,
	SE_ACTION_FORWARD,
	SE_ACTION_HOME,
	SE_ACTION_SHOW_SIDEBAR,
	SE_ACTION_MINIMIZE,
	SE_ACTION_ZOOM,
	SE_ACTION_ABOUT,
	SE_ACTION_FIND,
	SE_ACTION_PAGE_FIRST = 100
};

/*
 * What the interface asks of the window, which the main loop carries out
 * once.
 */
enum se_request {
	SE_REQUEST_NONE,
	SE_REQUEST_CLOSE,
	SE_REQUEST_MINIMIZE,
	SE_REQUEST_ZOOM
};

/*
 * The controls of the window's titlebar (WS070's CONTROLS presentation,
 * drawn by the compositor): their IDs in the model titlebar.c gives the compositor.
 */
enum se_control {
	SE_CONTROL_NONE,
	SE_CONTROL_BACK,
	SE_CONTROL_FORWARD,
	SE_CONTROL_HOME,
	SE_CONTROL_PATH,
	SE_CONTROL_SEARCH,
	SE_CONTROL_SIDEBAR
};

/*
 * What the titlebar shows of the window's state: whether the history can
 * go back and forward, the parts of the breadcrumb, the search's query,
 * and whether the list of pages is shown.  focus_serial moves each time
 * the window asks for the search field to have the keyboard (Ctrl+F);
 * titlebar.c gives it the keyboard when the serial differs from the one
 * it last sent.  titlebar.c sends the state to the compositor when it differs
 * from what the titlebar shows.
 */
struct se_titlebar_state {
	int can_back;
	int can_forward;
	int part_count;
	char parts[SE_CRUMBS][SE_TITLEBAR_PART];
	char query[SE_TITLEBAR_TEXT];
	unsigned focus_serial;
	int sidebar;
};

/*
 * What the titlebar tells the window.
 */
enum se_titlebar_kind {
	SE_TITLEBAR_ACTIVATED,
	SE_TITLEBAR_CHANGED,
	SE_TITLEBAR_DONE
};

/*
 * One thing done with the titlebar: a control chosen (with the
 * breadcrumb's part for the breadcrumb), a text control's text as typed,
 * or its editing ended, with its text.
 */
struct se_titlebar_event {
	unsigned kind;
	uint32_t id;
	uint32_t detail;
	char text[SE_TITLEBAR_TEXT];
};

/*
 * What the menus show of the window's state: the history's steps, the
 * list of pages shown, and the page shown (checked in the Go menu).
 */
struct se_menu_state {
	int can_back;
	int can_forward;
	int sidebar;
	unsigned page;
};

/* The bytes of a search's query with its NUL, and how many results it lists at most. */
#define SE_SEARCH_QUERY		128U
#define SE_SEARCH_RESULTS	48U

/*
 * One thing a search found: a page, or one setting on a page (setting is
 * then its name, NULL for the page itself).
 */
struct se_search_result {
	unsigned page;
	const char *setting;
};

/*
 * The search of the titlebar (search.c, ws089-p008): the query as typed,
 * and what it found.
 *
 * While the query has a word in it (active), the page pane shows the
 * results in place of the page, which stays the history's step; ending the
 * search (Esc, a result or a page chosen) shows the page again.  The
 * results point into the tables of pages and of settings, which live for
 * the whole run.  focus_serial moves when the field is to have the
 * keyboard (see se_titlebar_state).
 */
struct se_search {
	char query[SE_SEARCH_QUERY];
	int active;
	struct se_search_result results[SE_SEARCH_RESULTS];
	unsigned count;
	unsigned focus_serial;

	/*
	 * The result the keys have chosen (ws089-p012): the first of a new list,
	 * moved by Up and Down, and opened by Enter.  reveal asks the next frame
	 * to scroll it into sight after the keys moved it.
	 */
	unsigned chosen;
	int reveal;
};

/* How many pictures the Wallpaper page offers besides the default, and the bytes of a path. */
#define SE_WALLPAPERS		8U
#define SE_PATH			256U

/* How many file systems the Storage page shows. */
#define SE_VOLUMES		6U

/*
 * One picture the Wallpaper page offers: its file, the name shown (the
 * file's name without its ending), and a small copy for its tile (empty until
 * the page is first shown, or when the file cannot be read).
 *
 * pending is 1 from the moment the page lists the picture until the
 * loader's small copy is taken (BUG-152): the tile shows a quiet
 * stand-in meanwhile.  read is 1 once a small copy is in thumbnail.
 */
struct se_wallpaper {
	char path[SE_PATH];
	char name[64];
	struct kl_image thumbnail;
	int read;
	int pending;
};

/*
 * The thread that reads the pictures' small copies away from the window's
 * thread (BUG-152: seven pictures of 6 MB each held the first frame of the
 * Wallpaper page for about ten seconds on a USB disk).
 *
 * The window's thread fills count and paths before the thread starts and
 * does not change them while it runs; the thread reads only those.  Under
 * lock, the thread fills images[i], errors[i] and milliseconds[i] and then
 * sets done[i]; it looks at stopping between pictures and ends early when
 * it is set.  taken[] and taken_count are the window's thread's alone: a
 * picture whose small copy has moved to the page.  running is 1 while the
 * thread has to be joined; the lock exists while running is 1.
 */
struct se_look_loader {
	pthread_t thread;
	pthread_mutex_t lock;
	int running;
	int stopping;
	unsigned count;
	char paths[SE_WALLPAPERS][SE_PATH];
	struct kl_image images[SE_WALLPAPERS];
	int errors[SE_WALLPAPERS];
	long milliseconds[SE_WALLPAPERS];
	int done[SE_WALLPAPERS];
	int taken[SE_WALLPAPERS];
	unsigned taken_count;
};

/*
 * One file system the Storage page shows: where it is mounted and its
 * sizes in bytes.
 */
struct se_volume {
	char path[64];
	uint64_t total;
	uint64_t available;
	uint64_t used;
};

/*
 * The desktop's look and settings as Settings shows them
 * (look.c, ws089-p004).
 *
 * settings are the desktop's settings (libkeiland's kl_settings, WS135;
 * NULL, with open_error, without memory), and writable says the
 * compositor's can be changed here (Keiland's extension); a change made
 * elsewhere comes to a watch.  opacity
 * is the windows' opacity shown (a drag moves it before it is written),
 * and so are a mouse's and the touch pads' speed (percent), acceleration
 * (0 none to 3 strong) and natural scrolling (ws089-p024) and the
 * keyboards' repeat (keys a second, milliseconds before it starts);
 * accent is the accent colour chosen (KL_ACCENT_*, ws179-p001);
 * wallpaper is the settings' picture (empty for the default).  The
 * pictures are found and read when the Wallpaper page is first shown; the
 * default's (the session's --wallpaper) is wallpapers[0] when it exists.
 * The sliders' rectangles are the last frame's, for a drag (slider for
 * the opacity, sliders[] for the input pages' by their order).  The volumes are
 * read when the Storage page is shown.  loader reads the pictures' small
 * copies while the page is already shown.
 */
struct se_look {
	struct kl_settings *settings;
	int open_error;
	int writable;
	int opacity;
	int mouse_speed;
	int mouse_acceleration;
	int mouse_natural;
	int touchpad_speed;
	int touchpad_acceleration;
	int touchpad_natural;
	int repeat_rate;
	int repeat_delay;
	int ime_method;
	int ui_language;
	int dark;
	int accent;
	/*
	 * The accent swatch that has the keyboard, plus one (ws177-p004): 0
	 * while none has it, as the page starts; Tab gives it the chosen one's.
	 */
	int accent_focus;
	int frosted;
	/* The minutes without input before a sleep, on the adapter and on battery (0 never; ws052-p013). */
	int sleep_ac;
	int sleep_battery;
	int dragging;
	struct kl_rect slider;
	struct kl_rect sliders[8];
	char wallpaper[SE_PATH];
	struct se_wallpaper wallpapers[SE_WALLPAPERS];
	unsigned wallpaper_count;
	int has_default;
	int scanned;
	struct se_look_loader loader;
	struct se_volume volumes[SE_VOLUMES];
	unsigned volume_count;
	char message[SE_MESSAGE];
	int message_bad;
};

/*
 * The sound's volume as the Sound page shows and sets it (ws100-p005,
 * sound.c): live when the desktop offers the sound (Keiland's system
 * extension), what the compositor last reported of the sound service, the
 * volume and mute shown (the service's, or the one being set), a drag in
 * progress, a volume held back (at most one every 50 milliseconds while
 * dragging, as the system bar does; the feedback sound plays only when a
 * change is final, BUG-170), and the slider's place in the last frame.
 */
struct se_sound {
	int live;
	struct kl_audio_state state;
	int value;
	int muted;
	int dragging;
	int send_waiting;
	uint64_t sent_at;
	struct kl_rect slider;
};

/*
 * A touch pad's two fingers scrolling a pane, and its flight after they
 * lift (BUG-211, ws090-p019): whether they hold a pane and which
 * (SE_KINETIC_*), and whether it flies.  libkeiland's scroller does the
 * rest, as for every program: their velocity and the flight's slowing.
 */
#define SE_KINETIC_NONE		0
#define SE_KINETIC_PAGE		1
#define SE_KINETIC_SIDEBAR	2

struct se_kinetic {
	int holding;
	int pane;
	int flying;
	struct kl_scroller *scroller;
};

/*
 * A finger on the touch screen held on a pane, which a drag of it scrolls
 * (ws089-p012): where it touched, the pane and its scroll then, and whether
 * it has moved far enough to be a scroll rather than a tap.  held is 0 when
 * no finger is down (or the finger is on a slider, which it drags instead).
 */
struct se_touch_scroll {
	int held;
	int scrolling;
	unsigned pane;
	int start_x;
	int start_y;
	int start_scroll;
};

/*
 * Settings in one window: the page shown and its history, the list's and
 * the page's scroll, what the last frame drew, and what the pointer is
 * doing.
 *
 * One lives for the whole run.
 */
struct se_app {
	/* The fonts, the window's size, the time of the input being handled, and whether a new frame is due. */
	struct kl_text *text;
	int width;
	int height;
	uint64_t now;
	int dirty;

	/* Whether the window is glass (its ground clear, the panes on the compositor's glass), and has the focus. */
	int glass;
	int focused;

	/* Whether the list of pages is shown. */
	int show_sidebar;

	/* The page shown, and the history of pages: its steps and the one shown. */
	unsigned page;
	unsigned history[SE_HISTORY];
	int history_count;
	int history_index;

	/* How far the page and the list are scrolled, and how tall their content was in the last frame (pixels). */
	int page_scroll;
	int page_extent;
	int sidebar_scroll;
	int sidebar_extent;

	/* Whether the list is to scroll the page shown into sight at the next frame (after the page changed). */
	int reveal;

	/* A finger held on a pane, which a drag of it scrolls (ws089-p012). */
	struct se_touch_scroll touch;

	/* A touch pad's scrolling, which flies on after the fingers lift (BUG-211). */
	struct se_kinetic kinetic;

	/*
	 * A change of the lit region alone (BUG-226): the frame needs drawing
	 * only within hover_damage (the region lit before and the one lit now),
	 * which se_ui_draw draws clipped to it rather than the whole window.
	 * Any other change (dirty) draws everything.
	 */
	int hover_pending;
	struct kl_rect hover_damage;

	/* The panes of the last frame and its clickable regions. */
	struct se_layout layout;
	struct se_hit hits[SE_HITS];
	int hit_count;

	/*
	 * The page's controls as the log last listed them (a new frame whose
	 * controls moved, came or went lists them again, so that a test can
	 * find them).
	 */
	struct se_hit logged[SE_HITS];
	int logged_count;

	/* The region under the pointer (lit), and the one a press started on (a click lands on the same one). */
	unsigned hover_kind;
	int hover_index;
	unsigned press_kind;
	int press_index;

	/* Where the pointer is while a page's control is dragged, down the window (page->drag is given only x). */
	int drag_y;

	/* The minute of the clock last drawn (About shows how long the machine has run). */
	uint64_t minute;

	/* What the window is asked to do (SE_REQUEST_*), taken by the main loop. */
	unsigned request;

	/* What About shows of the machine. */
	struct se_about about;

	/* The titlebar's search and what it found. */
	struct se_search search;

	/*
	 * The desktop's system (libkeiland's kl_system, WS131 p011; NULL on a
	 * desktop without Keiland's system extension) and what its last
	 * dispatch found changed (KL_SYSTEM_CHANGED_*), for the network and
	 * the sound to follow.
	 */
	struct kl_system *system;
	unsigned system_changed;

	/* The readings of the computer asked of the desktop (machine.c, ws188-p002). */
	struct se_machine machine;

	/* The machine's monitor while About is shown (ws089-p013: its memory; NULL before About or without one). */
	struct kl_system_monitor *monitor;

	/* What the network pages show and have asked of the daemon. */
	struct se_network network;

	/* The desktop's look and settings. */
	struct se_look look;

	/* The sound's volume (ws100-p005). */
	struct se_sound sound;

	/* The Users page's account and password fields (ws160-p002). */
	struct se_users users;
	struct se_dialog dialog;
	struct se_keys keys;

	/* The Ethernet page's editor of a wired interface (ws089-p022). */
	struct se_wired wired;

	/* The Storage page's analysis and Trash (ws089-p023). */
	struct se_storage storage;

	/* The Sharing page's Remote Login (ws089-p025). */
	struct se_sharing sharing;
	struct se_display display;

	/* The Printers page (ws145-p004). */
	struct se_printers printers;

	/* The Bluetooth page (ws143-p006). */
	struct se_bluetooth bluetooth;

	/* The Languages page's system language (ws158-p004). */
	struct se_languages languages;

	/*
	 * The Welcome (welcome.c, ws164-p002): whether the window shows it, the
	 * step shown, and whether Files is to be opened as the window closes
	 * (its last step), which the main loop carries out.  ws177-p007: what
	 * went wrong at its end, said over its bar (empty: nothing), whether
	 * welcome.done could not be set (the next Start or Skip closes all the
	 * same), and whether Files could not be opened (Start then only
	 * closes).
	 */
	int welcome;
	int welcome_step;
	int request_files;
	char welcome_message[SE_MESSAGE];
	int welcome_unsaved;
	int welcome_files_failed;
};

/*
 * The Welcome's controls (ws164-p002), numbered apart from the pages' own
 * so that a step showing a page's controls keeps them: Back, Next, Skip,
 * and About's "Show Welcome again".
 */
#define SE_WELCOME_BACK		9000
#define SE_WELCOME_NEXT		9001
#define SE_WELCOME_SKIP		9002
#define SE_ABOUT_WELCOME	9003

/* The table of pages (pages.c). */
extern const struct se_page se_pages[SE_PAGES];
const struct se_page *se_page_find(const char *word);

/* The interface (ui.c). */
void se_ui_init(struct se_app *app, struct kl_text *text, unsigned page);
void se_ui_close(struct se_app *app);
void se_ui_event(struct se_app *app, const struct se_event *event);
void se_ui_action(struct se_app *app, uint32_t action);
void se_ui_tick(struct se_app *app, uint64_t now);
void se_ui_draw(struct se_app *app, struct kl_canvas *canvas);
size_t se_ui_panels(struct se_app *app, struct se_panel *panels, size_t capacity);
void se_ui_hit(struct se_app *app, const struct kl_rect *rect, unsigned kind, int index);
void se_ui_go(struct se_app *app, unsigned page);
void se_ui_titlebar_state(struct se_app *app, struct se_titlebar_state *state);
void se_ui_titlebar(struct se_app *app, const struct se_titlebar_event *event);
void se_ui_menu_state(struct se_app *app, struct se_menu_state *state);
int se_ui_lit(const struct se_app *app, unsigned kind, int index);
void se_log(const char *format, ...);

/* The parts pages are built of (widgets.c). */
int se_page_header(struct se_app *app, struct kl_canvas *canvas, const struct se_page *page, int x, int top, int width);
int se_card_begin(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width, int height, const char *title, const char *subtitle);
int se_card_height(int rows, int titled);
int se_row_value(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width, const char *label, const char *value, int last);
void se_mark_draw(struct kl_canvas *canvas, int x, int y, unsigned pixels, float opacity);
void se_toggle_draw(struct se_app *app, struct kl_canvas *canvas, int x, int y, int on, int enabled, int index);
int se_button_draw(struct se_app *app, struct kl_canvas *canvas, int x, int y, const char *label, int primary, int enabled, int index);
void se_icon_button_draw(struct se_app *app, struct kl_canvas *canvas, int x, int y, enum kl_icon icon, int index);
int se_button_width(struct se_app *app, const char *label);
void se_dot_draw(struct kl_canvas *canvas, float cx, float cy, kl_color color);
void se_signal_draw(struct kl_canvas *canvas, float x, float y, int rssi, kl_color color);
void se_bytes_text(uint64_t bytes, char *text, size_t size);

/* The line pictures (glyphs.c). */
void se_glyph_draw(struct kl_canvas *canvas, unsigned glyph, float x, float y, float size, kl_color color);

/* The network pages (page-network.c). */
int se_network_page_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
int se_wifi_page_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
int se_ethernet_page_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
void se_network_press(struct se_app *app, int index);
int se_network_key(struct se_app *app, const struct se_event *event);

/* The desktop's system the network and the sound follow (system.c). */
void se_system_open(struct se_app *app, struct wl_display *display);
void se_system_poll(struct se_app *app);
void se_system_close(struct se_app *app);

/* The network's backend (network.c; the host tests have their own). */
void se_network_open(struct se_app *app);
void se_network_poll(struct se_app *app, uint64_t now);
int se_network_result(struct se_app *app, uint32_t request, int error);
int se_network_wait(struct se_app *app);
void se_network_close(struct se_app *app);
void se_network_wifi(struct se_app *app, int on);
int se_network_wifi_on(const struct se_network *network);
void se_network_join(struct se_app *app, const char *ssid);
void se_network_join_key(struct se_app *app, const char *ssid, const char *key);
void se_network_disconnect(struct se_app *app);
int se_network_configure_wired(struct se_app *app, const struct kl_network_wired_config *config);
void se_wired_edit(struct se_app *app, const struct kl_network_link *link);
void se_storage_analyze(struct se_app *app, const char *root);
int se_sharing_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
void se_sharing_press(struct se_app *app, int index);
int se_languages_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
void se_languages_press(struct se_app *app, int index);
int se_languages_key(struct se_app *app, const struct se_event *event);
int se_languages_result(struct se_app *app, uint32_t request, int error);
void se_sharing_poll(struct se_app *app);
int se_sharing_result(struct se_app *app, uint32_t request, int error);
int se_printers_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
void se_printers_press(struct se_app *app, int index);
int se_printers_key(struct se_app *app, const struct se_event *event);
void se_printers_poll(struct se_app *app);

/* The Display page (page-display.c, ws113-p006). */
int se_display_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
void se_display_press(struct se_app *app, int index);
void se_display_drag(struct se_app *app, int index, int x, unsigned phase);
void se_display_poll(struct se_app *app);
int se_display_result(struct se_app *app, uint32_t request, int error);
int se_printers_result(struct se_app *app, uint32_t request, int error);

/* The Bluetooth page (page-bluetooth.c, ws143-p006). */
int se_bluetooth_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
void se_bluetooth_press(struct se_app *app, int index);
void se_bluetooth_poll(struct se_app *app);
int se_bluetooth_wait(const struct se_app *app);
void se_bluetooth_close(struct se_app *app);
int se_bluetooth_result(struct se_app *app, uint32_t request, int error);
void se_storage_stop(struct se_app *app);
void se_storage_empty_trash(struct se_app *app);
void se_storage_poll(struct se_app *app, uint64_t now);
int se_storage_wait(const struct se_app *app);
void se_storage_close(struct se_app *app);
void se_storage_press(struct se_app *app, int index);
int se_storage_cards(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
void se_wired_cancel(struct se_app *app);
int se_wired_check(const struct se_wired *wired, char *message, size_t size);
void se_wired_apply(struct se_app *app);
void se_wired_outcome(struct se_app *app, int error);
int se_wired_type(struct se_wired *wired, const struct se_event *event);
void se_wired_press(struct se_app *app, int index);
int se_wired_key(struct se_app *app, const struct se_event *event);
int se_wired_card(struct se_app *app, struct kl_canvas *canvas, const struct kl_network_link *link, int x, int top, int width);

/* The text fields (widgets.c). */
/*
 * The text fields (widgets.c, ws090-p007): libkeiland's kl_field under one
 * kl_ui for every page.  A field's text is wiped when it is closed
 * (se_field_clear).
 */
int se_fields_open(struct kl_text *text);
void se_fields_close(void);
void se_fields_begin(uint64_t now_us);
int se_fields_end(uint64_t now_us);
void se_fields_input(const struct se_event *event);
struct kl_ui *se_fields_ui(void);
int se_field_key(struct kl_field *field, const struct se_event *event);
unsigned se_field_draw(struct se_app *app, struct kl_canvas *canvas, struct kl_field *field, const struct kl_rect *rect, const char *placeholder, unsigned kind, int focused);

/* What a field holds (se_field_draw): text an input method may write, a secret (dots, no input method), or plain characters (shown, no input method). */
#define SE_FIELD_TEXT		0U
#define SE_FIELD_SECRET		1U
#define SE_FIELD_PLAIN		2U
void se_field_clear(struct kl_field *field);

/* The look and the settings (look.c). */
void se_look_open(struct se_app *app, struct wl_display *display);
void se_look_poll(struct se_app *app, uint64_t now);
int se_look_wait(const struct se_app *app);
void se_look_close(struct se_app *app);
void se_look_set_opacity(struct se_app *app, int percent);
void se_look_set_number(struct se_app *app, const char *key, int value, int fallback);
void se_look_set_wallpaper(struct se_app *app, int index);
int se_look_set(struct se_app *app, const char *key, const char *value);
void se_look_scan(struct se_app *app);
void se_look_volumes(struct se_app *app);
const char *se_look_wallpaper_name(const struct se_app *app);

/* The look's pages (page-look.c). */
int se_appearance_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
int se_wallpaper_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
int se_storage_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
void se_look_press(struct se_app *app, int index);
int se_look_key(struct se_app *app, const struct se_event *event);
void se_look_drag(struct se_app *app, int index, int x, unsigned phase);

/* The input and sound pages (page-input.c). */
int se_mouse_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
int se_touchpad_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
int se_keyboard_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
int se_sound_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
void se_input_press(struct se_app *app, int index);
void se_input_drag(struct se_app *app, int index, int x, unsigned phase);
int se_power_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
int se_notifications_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
void se_notifications_press(struct se_app *app, int index);
void se_power_drag(struct se_app *app, int index, int x, unsigned phase);

/* The sound's volume (sound.c, ws100-p005), and its page's controls (hit indices). */
#define SE_SOUND_VOLUME		6
#define SE_SOUND_MUTE		7
void se_sound_open(struct se_app *app);
void se_sound_poll(struct se_app *app, uint64_t now);
int se_sound_wait(const struct se_app *app);
void se_sound_close(struct se_app *app);
void se_sound_press(struct se_app *app, int index);
void se_sound_drag(struct se_app *app, int index, int x, unsigned phase);
int se_sound_available(const struct se_app *app);
int se_sound_running(const struct se_app *app);

/* A slider (widgets.c). */
void se_slider_draw(struct se_app *app, struct kl_canvas *canvas, int x, int y, int width, float fraction, int enabled, int index, struct kl_rect *rect);
float se_slider_fraction(const struct kl_rect *rect, int x);

/* The search (search.c). */
void se_search_set(struct se_app *app, const char *query);
void se_search_end(struct se_app *app);
void se_search_focus(struct se_app *app);
int se_search_open_chosen(struct se_app *app);
void se_search_step(struct se_app *app, int direction);
int se_search_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
void se_search_press(struct se_app *app, int index);

/* The pages' drawing (page-home.c, page-about.c, page-soon.c). */
int se_home_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
int se_about_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
void se_about_press(struct se_app *app, int index);
void se_about_follow(struct se_app *app);

/* The Welcome (welcome.c, ws164-p002). */
void se_welcome_start(struct se_app *app);
int se_welcome_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
void se_welcome_bar(struct se_app *app, struct kl_canvas *canvas, const struct kl_rect *panel);
int se_welcome_bar_height(void);
int se_welcome_press(struct se_app *app, int index);
int se_welcome_key(struct se_app *app, const struct se_event *event);
void se_welcome_files_failed(struct se_app *app, int error);
int se_soon_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);

/* The Users page (page-users.c, ws160-p002). */
int se_users_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
void se_users_press(struct se_app *app, int index);
int se_users_key(struct se_app *app, const struct se_event *event);
void se_users_load(struct se_app *app);
int se_users_result(struct se_app *app, uint32_t request, int error);
void se_users_close(struct se_app *app);
int se_users_admin_available(const struct se_app *app);
int se_users_admin_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
int se_users_admin_press(struct se_app *app, int index);
int se_users_admin_key(struct se_app *app, const struct se_event *event);
int se_users_admin_result(struct se_app *app, uint32_t request, int error);
void se_users_admin_wipe(struct se_users *users);

/* The Security Keys page (page-users-keys.c and page-users-pin.c, ws199-p001). */
int se_keys_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
void se_keys_press(struct se_app *app, int index);
int se_keys_result(struct se_app *app, uint32_t request, int error);
void se_keys_touched(struct se_app *app);
void se_keys_replugged(struct se_app *app);
void se_keys_changed(struct se_app *app);
void se_keys_close(struct se_app *app);
void se_keys_end(struct se_app *app);
int se_keys_soft_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
int se_keys_soft_press(struct se_app *app, int index);
void se_keys_soft_act(struct se_app *app, unsigned action);
int se_keys_soft_ready(const struct se_app *app);
void se_keys_soft_result(struct se_app *app, int error);
const char *se_keys_refusal(struct se_app *app, uint32_t request);
void se_keys_keep_password(struct se_app *app);

/* The popup (dialog.c, ws199-p001). */
void se_dialog_open(struct se_app *app, void (*act)(struct se_app *app, unsigned action), int (*ready)(const struct se_app *app));
void se_dialog_close(struct se_app *app);
void se_dialog_dismiss(struct se_app *app);
void se_dialog_step(struct se_app *app, const char *title, unsigned step, unsigned steps, const char *body, const char *primary, int can_back);
void se_dialog_field(struct se_app *app, const char *label, const char *placeholder, unsigned kind, size_t limit);
void se_dialog_digits(struct se_app *app, unsigned index, size_t digits);
void se_dialog_set_text(struct se_app *app, unsigned index, const char *text);
const char *se_dialog_text(const struct se_app *app, unsigned index);
size_t se_dialog_length(const struct se_app *app, unsigned index);
void se_dialog_focus(struct se_app *app, unsigned index);
void se_dialog_link(struct se_app *app, const char *text);
void se_dialog_error(struct se_app *app, const char *text);
void se_dialog_busy(struct se_app *app, const char *text, int cancellable);
void se_dialog_final(struct se_app *app);
void se_dialog_draw(struct se_app *app, struct kl_canvas *canvas);
int se_dialog_press(struct se_app *app, int index);
int se_dialog_key(struct se_app *app, const struct se_event *event);
void se_dialog_tick(struct se_app *app, uint64_t now);
int se_dialog_wait(const struct se_app *app);
void se_users_reload(struct se_app *app);
void se_users_copy(struct se_app *app);
void se_languages_copy(struct se_app *app);
void se_languages_reloaded(struct se_app *app, int error);

/* The readings of the computer asked of the desktop (machine.c, ws188-p002). */
void se_machine_open(struct se_app *app);
void se_machine_want(struct se_app *app, unsigned parts);
uint32_t se_machine_ask_now(struct se_app *app, unsigned parts);
int se_machine_reading(const struct se_app *app, unsigned part);
int se_machine_known(const struct se_app *app, unsigned part);
int se_machine_offered(const struct se_app *app);
void se_machine_follow(struct se_app *app);
int se_machine_result(struct se_app *app, uint32_t request, int error);
void se_machine_poll(struct se_app *app, uint64_t now);
int se_machine_wait(const struct se_app *app);

#endif
