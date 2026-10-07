/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals the compositor's Titlebar Presentation protocol (WS070
 * p008).
 *
 * kl_titlebar_manager_v1 gives a window (xdg_toplevel) its titlebar's
 * presentation (kl_titlebar_v1): a mode and the model of controls or tabs
 * that compositor draws in the window's floating titlebar or, docked, in the
 * system bar.  The protocol is the compositor's own; its header is private and
 * applications use it through libkeiland.  plan/ws070/titlebar-design.md
 * defines every request and event.
 */

#include "internal.h"

/*
 * The argument types of every message whose arguments are all numbers,
 * strings or arrays.  Such a message names no interface, so one array of
 * empty entries, as long as the longest of them (add_control's five),
 * serves them all.
 */
static const struct wl_interface *titlebar_plain_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

/* The arguments of kl_titlebar_manager_v1.get_titlebar: the new titlebar, and the window. */
static const struct wl_interface *titlebar_manager_get_types[] = {
	&kl_titlebar_v1_interface,
	&xdg_toplevel_interface,
};

/* The requests of kl_titlebar_manager_v1, in wire order. */
static const struct wl_message titlebar_manager_requests[] = {
	{ "destroy", "", NULL },
	{ "get_titlebar", "no", titlebar_manager_get_types },
};

/* Describes the global that gives windows their titlebar's presentation. */
const struct wl_interface kl_titlebar_manager_v1_interface = {
	"kl_titlebar_manager_v1", 4, 2, titlebar_manager_requests,
	0, NULL
};

/* The requests of kl_titlebar_v1, in wire order. */
static const struct wl_message titlebar_requests[] = {
	{ "destroy", "", NULL },
	{ "begin_update", "u", titlebar_plain_types },
	{ "commit", "u", titlebar_plain_types },
	{ "set_mode", "u", titlebar_plain_types },
	{ "add_control", "uuuus", titlebar_plain_types },
	{ "remove_control", "u", titlebar_plain_types },
	{ "set_control_label", "us", titlebar_plain_types },
	{ "set_control_state", "uuu", titlebar_plain_types },
	{ "set_control_value", "uu", titlebar_plain_types },
	{ "set_control_text", "uss", titlebar_plain_types },
	{ "set_breadcrumb", "ua", titlebar_plain_types },
	{ "add_tab", "us", titlebar_plain_types },
	{ "remove_tab", "u", titlebar_plain_types },
	{ "set_tab", "usu", titlebar_plain_types },
	{ "set_tabs_options", "u", titlebar_plain_types },
	{ "focus_control", "uu", titlebar_plain_types },
	{ "set_suggestions", "4ua", titlebar_plain_types },
};

/* The arguments of kl_titlebar_v1.control_activated: control, detail, the seat (or none), serial. */
static const struct wl_interface *titlebar_activated_types[] = {
	NULL,
	NULL,
	&wl_seat_interface,
	NULL,
};

/* The events of kl_titlebar_v1, in wire order. */
static const struct wl_message titlebar_events[] = {
	{ "control_activated", "uu?ou", titlebar_activated_types },
	{ "text_changed", "us", titlebar_plain_types },
	{ "text_done", "usu", titlebar_plain_types },
	{ "tab_activated", "uu", titlebar_plain_types },
	{ "tab_close_requested", "u", titlebar_plain_types },
	{ "new_tab_requested", "u", titlebar_plain_types },
	{ "overflow_menu_opened", "", NULL },
	{ "drop_target", "2uu", titlebar_plain_types },
};

/* Describes one window's titlebar presentation. */
const struct wl_interface kl_titlebar_v1_interface = {
	"kl_titlebar_v1", 4, 17, titlebar_requests,
	8, titlebar_events
};

/*
 * Sends kl_titlebar_manager_v1.destroy; the titlebars it gave stay.
 */
void
kl_titlebar_manager_v1_destroy(
	struct kl_titlebar_manager_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_MANAGER_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends kl_titlebar_manager_v1.get_titlebar and returns the window's new titlebar.
 */
struct kl_titlebar_v1 *
kl_titlebar_manager_v1_get_titlebar(
	struct kl_titlebar_manager_v1 *object,
	struct xdg_toplevel *toplevel)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;
	uint32_t version;

	/* The new titlebar's identity, then the window it belongs to. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)toplevel;

	/* The titlebar has the manager's version (2 hears drop_target), as the compositor makes it. */
	version = wl_proxy_get_version((struct wl_proxy *)object);

	/* Queues the request together with the new proxy. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_MANAGER_V1_GET_TITLEBAR, &kl_titlebar_v1_interface, version, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new titlebar. */
	return (struct kl_titlebar_v1 *)created;
}

/*
 * Associates client state with the kl_titlebar_manager_v1 proxy.
 */
void
kl_titlebar_manager_v1_set_user_data(
	struct kl_titlebar_manager_v1 *object,
	void *data)
{
	/* The common proxy keeps the pointer. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);
}

/*
 * Obtains client state from the kl_titlebar_manager_v1 proxy.
 */
void *
kl_titlebar_manager_v1_get_user_data(
	struct kl_titlebar_manager_v1 *object)
{
	void *data;

	/* The common proxy keeps the pointer. */
	data = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the pointer. */
	return data;
}

/*
 * Obtains the negotiated version of the kl_titlebar_manager_v1 proxy.
 */
uint32_t
kl_titlebar_manager_v1_get_version(
	struct kl_titlebar_manager_v1 *object)
{
	uint32_t version;

	/* The version the binding was made at. */
	version = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the version. */
	return version;
}

/*
 * Installs the typed listener of a titlebar's events.
 */
int
kl_titlebar_v1_add_listener(
	struct kl_titlebar_v1 *object,
	const struct kl_titlebar_v1_listener *listener,
	void *data)
{
	int error;

	/* The typed callbacks receive the proxy's events from now on. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: the listener is installed. */
	return 0;
}

/*
 * Sends kl_titlebar_v1.destroy; the window's titlebar shows its menu again.
 */
void
kl_titlebar_v1_destroy(
	struct kl_titlebar_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends kl_titlebar_v1.begin_update: the changes that follow wait for the commit of the same serial.
 */
void
kl_titlebar_v1_begin_update(
	struct kl_titlebar_v1 *object,
	uint32_t serial)
{
	union wl_argument arguments[1];

	/* The serial the commit will name. */
	arguments[0].u = serial;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_BEGIN_UPDATE, NULL, 0, 0, arguments);
}

/*
 * Sends kl_titlebar_v1.commit: the changes since begin_update are shown together.
 */
void
kl_titlebar_v1_commit(
	struct kl_titlebar_v1 *object,
	uint32_t serial)
{
	union wl_argument arguments[1];

	/* The serial begin_update named. */
	arguments[0].u = serial;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_COMMIT, NULL, 0, 0, arguments);
}

/*
 * Sends kl_titlebar_v1.set_mode: the model the titlebar shows (menu, controls or tabs).
 */
void
kl_titlebar_v1_set_mode(
	struct kl_titlebar_v1 *object,
	uint32_t mode)
{
	union wl_argument arguments[1];

	/* The mode. */
	arguments[0].u = mode;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_SET_MODE, NULL, 0, 0, arguments);
}

/*
 * Sends kl_titlebar_v1.add_control: a new last control of a role.
 */
void
kl_titlebar_v1_add_control(
	struct kl_titlebar_v1 *object,
	uint32_t id,
	uint32_t role,
	uint32_t priority,
	uint32_t group,
	const char *label)
{
	union wl_argument arguments[5];

	/* The control's ID, role, priority, group and label. */
	arguments[0].u = id;
	arguments[1].u = role;
	arguments[2].u = priority;
	arguments[3].u = group;
	arguments[4].s = label;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_ADD_CONTROL, NULL, 0, 0, arguments);
}

/*
 * Sends kl_titlebar_v1.remove_control.
 */
void
kl_titlebar_v1_remove_control(
	struct kl_titlebar_v1 *object,
	uint32_t id)
{
	union wl_argument arguments[1];

	/* The control. */
	arguments[0].u = id;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_REMOVE_CONTROL, NULL, 0, 0, arguments);
}

/*
 * Sends kl_titlebar_v1.set_control_label.
 */
void
kl_titlebar_v1_set_control_label(
	struct kl_titlebar_v1 *object,
	uint32_t id,
	const char *label)
{
	union wl_argument arguments[2];

	/* The control and its new label. */
	arguments[0].u = id;
	arguments[1].s = label;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_SET_CONTROL_LABEL, NULL, 0, 0, arguments);
}

/*
 * Sends kl_titlebar_v1.set_control_state: whether a control works now and whether it is checked.
 */
void
kl_titlebar_v1_set_control_state(
	struct kl_titlebar_v1 *object,
	uint32_t id,
	uint32_t enabled,
	uint32_t checked)
{
	union wl_argument arguments[3];

	/* The control, enabled and checked. */
	arguments[0].u = id;
	arguments[1].u = enabled;
	arguments[2].u = checked;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_SET_CONTROL_STATE, NULL, 0, 0, arguments);
}

/*
 * Sends kl_titlebar_v1.set_control_value: a progress control's share done, in thousandths.
 */
void
kl_titlebar_v1_set_control_value(
	struct kl_titlebar_v1 *object,
	uint32_t id,
	uint32_t value)
{
	union wl_argument arguments[2];

	/* The control and its value. */
	arguments[0].u = id;
	arguments[1].u = value;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_SET_CONTROL_VALUE, NULL, 0, 0, arguments);
}

/*
 * Sends kl_titlebar_v1.set_control_text: a text control's text and what it shows when empty.
 */
void
kl_titlebar_v1_set_control_text(
	struct kl_titlebar_v1 *object,
	uint32_t id,
	const char *text,
	const char *placeholder)
{
	union wl_argument arguments[3];

	/* The control, its text and its placeholder. */
	arguments[0].u = id;
	arguments[1].s = text;
	arguments[2].s = placeholder;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_SET_CONTROL_TEXT, NULL, 0, 0, arguments);
}

/*
 * Sends kl_titlebar_v1.set_breadcrumb: a breadcrumb's parts, NUL-separated.
 */
void
kl_titlebar_v1_set_breadcrumb(
	struct kl_titlebar_v1 *object,
	uint32_t id,
	struct wl_array *segments)
{
	union wl_argument arguments[2];

	/* The control and its parts. */
	arguments[0].u = id;
	arguments[1].a = segments;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_SET_BREADCRUMB, NULL, 0, 0, arguments);
}

/*
 * Sends kl_titlebar_v1.add_tab: a new last tab.
 */
void
kl_titlebar_v1_add_tab(
	struct kl_titlebar_v1 *object,
	uint32_t id,
	const char *title)
{
	union wl_argument arguments[2];

	/* The tab's ID and title. */
	arguments[0].u = id;
	arguments[1].s = title;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_ADD_TAB, NULL, 0, 0, arguments);
}

/*
 * Sends kl_titlebar_v1.remove_tab.
 */
void
kl_titlebar_v1_remove_tab(
	struct kl_titlebar_v1 *object,
	uint32_t id)
{
	union wl_argument arguments[1];

	/* The tab. */
	arguments[0].u = id;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_REMOVE_TAB, NULL, 0, 0, arguments);
}

/*
 * Sends kl_titlebar_v1.set_tab: a tab's title and flags (active, attention, closable).
 */
void
kl_titlebar_v1_set_tab(
	struct kl_titlebar_v1 *object,
	uint32_t id,
	const char *title,
	uint32_t flags)
{
	union wl_argument arguments[3];

	/* The tab, its title and its flags. */
	arguments[0].u = id;
	arguments[1].s = title;
	arguments[2].u = flags;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_SET_TAB, NULL, 0, 0, arguments);
}

/*
 * Sends kl_titlebar_v1.set_tabs_options: whether the tab strip has a new-tab button.
 */
void
kl_titlebar_v1_set_tabs_options(
	struct kl_titlebar_v1 *object,
	uint32_t options)
{
	union wl_argument arguments[1];

	/* The options. */
	arguments[0].u = options;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_SET_TABS_OPTIONS, NULL, 0, 0, arguments);
}

/*
 * Sends kl_titlebar_v1.focus_control: the keyboard goes to a text control.
 */
void
kl_titlebar_v1_focus_control(
	struct kl_titlebar_v1 *object,
	uint32_t id,
	uint32_t mode)
{
	union wl_argument arguments[2];

	/* The control and how it takes the keyboard. */
	arguments[0].u = id;
	arguments[1].u = mode;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_FOCUS_CONTROL, NULL, 0, 0, arguments);
}

/*
 * Sends kl_titlebar_v1.set_suggestions (version 4): a text field's
 * suggestions, NUL-separated, each a label and the text it puts in the
 * field.
 */
void
kl_titlebar_v1_set_suggestions(
	struct kl_titlebar_v1 *object,
	uint32_t id,
	struct wl_array *suggestions)
{
	union wl_argument arguments[2];

	/* The control and its suggestions. */
	arguments[0].u = id;
	arguments[1].a = suggestions;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_TITLEBAR_V1_SET_SUGGESTIONS, NULL, 0, 0, arguments);
}

/*
 * Associates client state with the kl_titlebar_v1 proxy.
 */
void
kl_titlebar_v1_set_user_data(
	struct kl_titlebar_v1 *object,
	void *data)
{
	/* The common proxy keeps the pointer. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);
}

/*
 * Obtains client state from the kl_titlebar_v1 proxy.
 */
void *
kl_titlebar_v1_get_user_data(
	struct kl_titlebar_v1 *object)
{
	void *data;

	/* The common proxy keeps the pointer. */
	data = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the pointer. */
	return data;
}

/*
 * Obtains the negotiated version of the kl_titlebar_v1 proxy.
 */
uint32_t
kl_titlebar_v1_get_version(
	struct kl_titlebar_v1 *object)
{
	uint32_t version;

	/* The version of the manager that made it. */
	version = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the version. */
	return version;
}

/*
 * Calls the typed listener of a kl_titlebar_v1 event.
 *
 * Returns 0 when the event was delivered or deliberately ignored, EPROTO for
 * an opcode the interface does not have.
 */
int
wlc_titlebar_dispatch(
	struct wlc_event *event,
	const void *listener,
	void *data)
{
	const struct kl_titlebar_v1_listener *callbacks;
	struct kl_titlebar_v1 *object;
	union wl_argument *arguments;

	/* The listener, the proxy and the decoded arguments. */
	callbacks = listener;
	object = (struct kl_titlebar_v1 *)event->proxy;
	arguments = event->arguments;

	/* Selects the callback by the event's opcode. */
	switch (event->opcode) {
	case 0:
		/* A control was chosen: control, detail, seat and serial. */
		if (callbacks->control_activated == NULL)
			return 0;
		event->delivered = 1;
		callbacks->control_activated(data, object, arguments[0].u, arguments[1].u, (struct wl_seat *)arguments[2].o, arguments[3].u);
		return 0;
	case 1:
		/* A text control's text changed. */
		if (callbacks->text_changed == NULL)
			return 0;
		event->delivered = 1;
		callbacks->text_changed(data, object, arguments[0].u, arguments[1].s);
		return 0;
	case 2:
		/* A text control's editing ended, and how. */
		if (callbacks->text_done == NULL)
			return 0;
		event->delivered = 1;
		callbacks->text_done(data, object, arguments[0].u, arguments[1].s, arguments[2].u);
		return 0;
	case 3:
		/* A tab was chosen. */
		if (callbacks->tab_activated == NULL)
			return 0;
		event->delivered = 1;
		callbacks->tab_activated(data, object, arguments[0].u, arguments[1].u);
		return 0;
	case 4:
		/* A tab's close button was pressed. */
		if (callbacks->tab_close_requested == NULL)
			return 0;
		event->delivered = 1;
		callbacks->tab_close_requested(data, object, arguments[0].u);
		return 0;
	case 5:
		/* The new-tab button was pressed. */
		if (callbacks->new_tab_requested == NULL)
			return 0;
		event->delivered = 1;
		callbacks->new_tab_requested(data, object, arguments[0].u);
		return 0;
	case 6:
		/* The overflow popup opened. */
		if (callbacks->overflow_menu_opened == NULL)
			return 0;
		event->delivered = 1;
		callbacks->overflow_menu_opened(data, object);
		return 0;
	case 7:
		/* A drag and drop is over a control's part, or left it (version 2); a listener without the callback ignores it. */
		if (callbacks->drop_target == NULL)
			return 0;

		/* The callback takes the event. */
		event->delivered = 1;
		callbacks->drop_target(data, object, arguments[0].u, arguments[1].u);
		return 0;
	default:
		break;
	}

	/* The interface has no other event. */
	return EPROTO;
}
