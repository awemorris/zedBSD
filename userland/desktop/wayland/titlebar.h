/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Titlebar Presentation (WS070 p008, plan/ws070/titlebar-design.md):
 * the model a client gives a window's titlebar through kl_titlebar_v1 (its
 * mode, its controls and its tabs), kept by titlebar.c, and what the
 * presentation (titlebar-shell.c) reads of it and sends back.
 */

#ifndef KWL_TITLEBAR_H
#define KWL_TITLEBAR_H

#include "menu.h"

/* The presentation modes. */
#define KWL_TITLEBAR_MENU		0U
#define KWL_TITLEBAR_CONTROLS		1U
#define KWL_TITLEBAR_TABS		2U
#define KWL_TITLEBAR_SHEET		3U

/* The controls' roles. */
#define KWL_CONTROL_BACK		1U
#define KWL_CONTROL_FORWARD		2U
#define KWL_CONTROL_HOME		3U
#define KWL_CONTROL_UP			4U
#define KWL_CONTROL_BREADCRUMB		5U
#define KWL_CONTROL_SEARCH		6U
#define KWL_CONTROL_VIEW_GRID		7U
#define KWL_CONTROL_VIEW_LIST		8U
#define KWL_CONTROL_VIEW_COLUMNS	9U
#define KWL_CONTROL_SORT		10U
#define KWL_CONTROL_FILTER		11U
#define KWL_CONTROL_SIDEBAR		12U
#define KWL_CONTROL_PREVIEW		13U
#define KWL_CONTROL_PROGRESS		14U
#define KWL_CONTROL_PRIMARY_ACTION	15U
#define KWL_CONTROL_GENERIC		16U

/* A page's reload (since version 6): a square with the reload icon, beside the navigation. */
#define KWL_CONTROL_RELOAD		17U

/* The controls' priorities: the order they give way in when the room runs short. */
#define KWL_PRIORITY_PRIMARY		0U
#define KWL_PRIORITY_NORMAL		1U
#define KWL_PRIORITY_SECONDARY		2U

/*
 * A breadcrumb's value (set_control_value, since version 5): shown as a
 * box, like an address field, even while its path is not edited
 * (ws074-p229, the browser's location).
 */
#define KWL_BREADCRUMB_BOXED		1U

/*
 * A box's load (since version 7): (thousandths + 1) in these bits of a
 * KWL_BREADCRUMB_BOXED breadcrumb's value, 0 for none; its ground is
 * filled from the left that far (the browser's page loading).
 */
#define KWL_BREADCRUMB_PROGRESS_SHIFT	8U
#define KWL_BREADCRUMB_PROGRESS_MASK	(0x7ffU << KWL_BREADCRUMB_PROGRESS_SHIFT)

/* The tabs' flags, and the tab strip's options. */
#define KWL_TAB_ACTIVE			1U
#define KWL_TAB_ATTENTION		2U
#define KWL_TAB_CLOSABLE		4U
#define KWL_TABS_NEW_BUTTON		1U

/* How a text control's editing ended (text_done). */
#define KWL_TEXT_SUBMITTED		0U
#define KWL_TEXT_CANCELLED		1U
#define KWL_TEXT_LEFT			2U

/* The bounds of one titlebar: its controls, its tabs, a breadcrumb's parts, and a string in bytes. */
#define KWL_TITLEBAR_CONTROLS_MAX	64U
#define KWL_TITLEBAR_TABS_MAX		128U
#define KWL_TITLEBAR_SEGMENTS_MAX	32U
#define KWL_TITLEBAR_TEXT_MAX		1023U

/* The most suggestions a text field shows under it (ws127-p010). */
#define KWL_TITLEBAR_SUGGESTIONS_MAX	12U

/*
 * One control of a titlebar: what it is (its role), when it gives way (its
 * priority), the segmented pill it joins (its group, 0 for none), its
 * state, and its strings.  A breadcrumb's parts are its segments.  The
 * strings are allocated with the state the control is in.
 */
struct kwl_titlebar_control {
	uint32_t id;
	uint32_t role;
	uint32_t priority;
	uint32_t group;
	uint32_t enabled;
	uint32_t checked;
	uint32_t value;
	char *label;
	char *text;
	char *placeholder;
	char *segments[KWL_TITLEBAR_SEGMENTS_MAX];
	unsigned segment_count;
};

/*
 * One tab of a titlebar: its ID, its flags (active, attention, closable)
 * and its title, allocated with the state it is in.
 */
struct kwl_titlebar_tab {
	uint32_t id;
	uint32_t flags;
	char *title;
};

/*
 * One whole state of a titlebar: the mode, the controls and the tabs in
 * their order, and the tab strip's options.  A model keeps the state shown
 * and, during a transaction, the state being built.
 */
struct kwl_titlebar_state {
	uint32_t mode;
	struct kwl_titlebar_control controls[KWL_TITLEBAR_CONTROLS_MAX];
	unsigned control_count;
	struct kwl_titlebar_tab tabs[KWL_TITLEBAR_TABS_MAX];
	unsigned tab_count;
	uint32_t options;
};

/*
 * A window's titlebar model, owned by its kl_titlebar_v1 object: the state
 * shown, the state being built between begin_update and commit, the
 * transaction's serial, and how many commits there were.  A control the
 * client asked the keyboard for (focus_control) waits in focus_id (0 when
 * none) until the presentation takes it.  The presentation keeps the first
 * tab its scrolled strip shows (tab_first) and the commit it last showed
 * (tab_seen), to bring the active tab into sight once after a change.
 */
struct kwl_titlebar_model {
	struct kwl_titlebar_state shown;
	struct kwl_titlebar_state pending;
	unsigned updating;
	uint32_t update_serial;
	uint64_t generation;
	uint32_t focus_id;
	uint32_t focus_mode;
	unsigned tab_first;
	uint64_t tab_seen;
};

/* The protocol and the model (titlebar.c). */
int kwl_titlebar_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_titlebar_object_gone(struct kwl_object *object);
struct kwl_titlebar_model *kwl_titlebar_of_surface(struct kwl_object *surface, struct kwl_object **titlebar);
const struct kwl_titlebar_control *kwl_titlebar_control(const struct kwl_titlebar_state *state, uint32_t id);
void kwl_titlebar_send_activated(struct kwl_object *titlebar, uint32_t id, uint32_t detail, const char *via);
void kwl_titlebar_send_text(struct kwl_object *titlebar, uint32_t id, const char *text, int done, uint32_t how);
void kwl_titlebar_send_tab(struct kwl_object *titlebar, uint32_t id, unsigned event);
void kwl_titlebar_send_overflow(struct kwl_object *titlebar);
void kwl_titlebar_send_drop_target(struct kwl_object *titlebar, uint32_t id, uint32_t detail);
int kwl_titlebar_drop_at(struct kwl_server *server, int32_t x, int32_t y, struct kwl_object **surface, struct kwl_object **titlebar, uint32_t *id, uint32_t *detail);

/* The presentation (titlebar-shell.c). */
void kwl_titlebar_frame(struct kwl_server *server);
int32_t kwl_titlebar_title_limit(struct kwl_server *server, struct kwl_object *surface, int32_t available);
void kwl_titlebar_draw(struct kwl_server *server, struct kwl_list *command, struct kwl_object *surface, unsigned docked, const struct kwl_menu_area *area, const float *ink, float fade);
int kwl_titlebar_button(struct kwl_server *server, uint32_t button, uint32_t state);
int kwl_titlebar_motion(struct kwl_server *server);
int kwl_titlebar_key(struct kwl_server *server, uint32_t key, uint32_t state);
void kwl_titlebar_key_released(uint32_t key);
int kwl_titlebar_tab_key(struct kwl_server *server, uint32_t key, uint32_t state);
int kwl_titlebar_axis(struct kwl_server *server, int32_t vertical, int32_t horizontal);
void kwl_titlebar_overflow_chosen(struct kwl_server *server, struct kwl_object *surface, uint32_t id);
void kwl_titlebar_overflow_opened(struct kwl_object *surface);
void kwl_titlebar_forget(struct kwl_server *server, struct kwl_object *object);
void kwl_titlebar_suggestions(struct kwl_server *server, struct kwl_object *titlebar, uint32_t id, const char *const *strings, size_t count);
void kwl_titlebar_draw_suggestions(struct kwl_server *server, struct kwl_list *command);
struct kwl_object *kwl_titlebar_field_surface(struct kwl_server *server);
int kwl_titlebar_field_state(struct kwl_server *server, char *text, size_t size, int32_t *cursor, int32_t *anchor, int32_t *rectangle);
void kwl_titlebar_field_input(struct kwl_server *server, const char *preedit, const char *commit, uint32_t before, uint32_t after);

/* The tab events kwl_titlebar_send_tab sends. */
#define KWL_TAB_EVENT_ACTIVATED		0U
#define KWL_TAB_EVENT_CLOSE		1U
#define KWL_TAB_EVENT_NEW		2U

#endif
