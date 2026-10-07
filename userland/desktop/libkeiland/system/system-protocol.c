/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The interfaces of Keiland's system extension (libkeiland/system/kl-system-protocol.h;
 * WS135 for the manager and the settings, WS131 p010 for the network, the
 * sound, the power and the devices), described as wayland-scanner would
 * make them, over libwayland's marshalling.  They are not static because
 * the settings and the system both use them; exports.map keeps them
 * inside the library.
 */

#include "system-protocol.h"

#include "userland/desktop/libkeiland/system/kl-system-protocol.h"

#include <stddef.h>

/* The manager's get_* argument types: the new object each makes. */
static const struct wl_interface *system_get_settings_types[] = {
	&kl_system_settings_v1_interface,
};
static const struct wl_interface *system_get_network_types[] = {
	&kl_system_network_v1_interface,
};
static const struct wl_interface *system_get_audio_types[] = {
	&kl_system_audio_v1_interface,
};
static const struct wl_interface *system_get_power_types[] = {
	&kl_system_power_v1_interface,
};
static const struct wl_interface *system_get_devices_types[] = {
	&kl_system_devices_v1_interface,
};
static const struct wl_interface *system_get_monitor_types[] = {
	&kl_system_monitor_v1_interface,
	NULL,
};
static const struct wl_interface *system_get_account_types[] = {
	&kl_system_account_v1_interface,
};
static const struct wl_interface *system_get_sharing_types[] = {
	&kl_system_sharing_v1_interface,
};
static const struct wl_interface *system_get_notify_types[] = {
	&kl_system_notify_v1_interface,
};
static const struct wl_interface *system_get_mail_types[] = {
	&kl_system_mail_v1_interface,
};
static const struct wl_interface *system_get_phone_types[] = {
	&kl_system_phone_v1_interface,
};
static const struct wl_interface *system_get_printers_types[] = {
	&kl_system_printers_v1_interface,
};
static const struct wl_interface *system_get_displays_types[] = {
	&kl_system_displays_v1_interface,
};

/* The arguments of messages that name no interface (at most sixteen, a monitor's disk's). */
static const struct wl_interface *system_plain_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

/* The requests of kl_system_manager_v1. */
static const struct wl_message system_manager_requests[] = {
	{ "destroy", "", NULL },
	{ "get_settings", "n", system_get_settings_types },
	{ "get_network", "n", system_get_network_types },
	{ "get_audio", "n", system_get_audio_types },
	{ "get_power", "n", system_get_power_types },
	{ "get_devices", "n", system_get_devices_types },
	{ "get_monitor", "2nu", system_get_monitor_types },
	{ "get_account", "4n", system_get_account_types },
	{ "get_sharing", "7n", system_get_sharing_types },
	{ "get_notify", "13n", system_get_notify_types },
	{ "get_mail", "15n", system_get_mail_types },
	{ "get_phone", "16n", system_get_phone_types },
	{ "get_printers", "17n", system_get_printers_types },
	{ "get_displays", "18n", system_get_displays_types },
};

/* The events of kl_system_manager_v1. */
static const struct wl_message system_manager_events[] = {
	{ "capabilities", "u", system_plain_types },
};

/* kl_system_manager_v1, at KL_SYSTEM_MANAGER_VERSION: fourteen requests (get_monitor since 2, get_account since 4, get_sharing since 7, get_notify since 13, get_mail since 15, get_phone since 16, get_printers since 17, get_displays since 18; the displays' set_shown since 19; the mail's allowed since 20) and one event.  It lives for the program. */
const struct wl_interface kl_system_manager_v1_interface = {
	KL_SYSTEM_MANAGER_NAME,
	KL_SYSTEM_MANAGER_VERSION,
	14,
	system_manager_requests,
	1,
	system_manager_events
};

/* The requests of kl_system_settings_v1. */
static const struct wl_message system_settings_requests[] = {
	{ "destroy", "", NULL },
	{ "set", "uss", system_plain_types },
	{ "reset", "us", system_plain_types },
};

/* The events of kl_system_settings_v1. */
static const struct wl_message system_settings_events[] = {
	{ "value", "ssu", system_plain_types },
	{ "done", "u", system_plain_types },
	{ "result", "uuu", system_plain_types },
};

/* kl_system_settings_v1: three requests and three events.  It lives for the program. */
const struct wl_interface kl_system_settings_v1_interface = {
	KL_SYSTEM_SETTINGS_NAME,
	1,
	3,
	system_settings_requests,
	3,
	system_settings_events
};

/* The requests of kl_system_network_v1. */
static const struct wl_message system_network_requests[] = {
	{ "destroy", "", NULL },
	{ "request", "uus", system_plain_types },
	{ "save_key", "uss", system_plain_types },
	{ "query_details", "u", system_plain_types },
	{ "set_scanning", "3u", system_plain_types },
	{ "configure_wired", "6ususssss", system_plain_types },
};

/* The events of kl_system_network_v1. */
static const struct wl_message system_network_events[] = {
	{ "state", "uuussuss", system_plain_types },
	{ "access_point", "siu", system_plain_types },
	{ "scan_done", "", NULL },
	{ "link", "susssuuuuu", system_plain_types },
	{ "dns", "s", system_plain_types },
	{ "saved_network", "s", system_plain_types },
	{ "details_done", "", NULL },
	{ "done", "u", system_plain_types },
	{ "result", "uuu", system_plain_types },
	{ "wired", "6sus", system_plain_types },
	{ "link_speed", "12su", system_plain_types },
};

/* kl_system_network_v1, version 12: six requests (set_scanning since 3, configure_wired since 6) and eleven events (wired since 6, link_speed since 12).  It lives for the program. */
const struct wl_interface kl_system_network_v1_interface = {
	KL_SYSTEM_NETWORK_NAME,
	KL_SYSTEM_NETWORK_SINCE_LINK_SPEED,
	6,
	system_network_requests,
	11,
	system_network_events
};

/* The requests of kl_system_audio_v1. */
static const struct wl_message system_audio_requests[] = {
	{ "destroy", "", NULL },
	{ "set_volume", "uuuu", system_plain_types },
	{ "feedback", "u", system_plain_types },
};

/* The events of kl_system_audio_v1. */
static const struct wl_message system_audio_events[] = {
	{ "state", "uuuuuuu", system_plain_types },
	{ "done", "u", system_plain_types },
	{ "result", "uuu", system_plain_types },
};

/* kl_system_audio_v1: three requests and three events.  It lives for the program. */
const struct wl_interface kl_system_audio_v1_interface = {
	KL_SYSTEM_AUDIO_NAME,
	1,
	3,
	system_audio_requests,
	3,
	system_audio_events
};

/* The requests of kl_system_power_v1. */
static const struct wl_message system_power_requests[] = {
	{ "destroy", "", NULL },
	{ "action", "uu", system_plain_types },
};

/* The events of kl_system_power_v1. */
static const struct wl_message system_power_events[] = {
	{ "state", "uiuu", system_plain_types },
	{ "done", "u", system_plain_types },
	{ "result", "uuu", system_plain_types },
};

/* kl_system_power_v1: two requests and three events.  It lives for the program. */
const struct wl_interface kl_system_power_v1_interface = {
	KL_SYSTEM_POWER_NAME,
	1,
	2,
	system_power_requests,
	3,
	system_power_events
};

/* The requests of kl_system_devices_v1. */
static const struct wl_message system_devices_requests[] = {
	{ "destroy", "", NULL },
	{ "eject", "us", system_plain_types },
	{ "mount", "5us", system_plain_types },
};

/* The events of kl_system_devices_v1. */
static const struct wl_message system_devices_events[] = {
	{ "device", "suuss", system_plain_types },
	{ "done", "u", system_plain_types },
	{ "result", "uuu", system_plain_types },
	{ "busy", "5us", system_plain_types },
	{ "volume", "9ssuu", system_plain_types },
};

/* kl_system_devices_v1: three requests and five events (ws132-p004: mount and busy since version 5; ws132-p009: volume since version 9).  It lives for the program. */
const struct wl_interface kl_system_devices_v1_interface = {
	KL_SYSTEM_DEVICES_NAME,
	KL_SYSTEM_DEVICES_SINCE_VOLUME,
	3,
	system_devices_requests,
	5,
	system_devices_events
};

/* The requests of kl_system_account_v1 (ws160-p002). */
static const struct wl_message system_account_requests[] = {
	{ "destroy", "", NULL },
	{ "set_password", "uss", system_plain_types },
	{ "administer", "8uss", system_plain_types },
	{ "set_pin", "10uss", system_plain_types },
	{ "add_key", "14usss", system_plain_types },
	{ "remove_key", "14uss", system_plain_types },
};

/* The events of kl_system_account_v1 (refused since version 8, ws089-p026; enrolled since 11, ws172-p002). */
static const struct wl_message system_account_events[] = {
	{ "result", "uuu", system_plain_types },
	{ "refused", "8us", system_plain_types },
	{ "enrolled", "11uu", system_plain_types },
	{ "key", "14ss", system_plain_types },
	{ "touch", "14u", system_plain_types },
};

/* kl_system_account_v1, made at the manager's version (14, ws172-p003): six requests and five events.  It lives for the program. */
const struct wl_interface kl_system_account_v1_interface = {
	KL_SYSTEM_ACCOUNT_NAME,
	KL_SYSTEM_SINCE_KEYS,
	6,
	system_account_requests,
	5,
	system_account_events
};

/* The requests of kl_system_monitor_v1 (WS134 p012). */
static const struct wl_message system_monitor_requests[] = {
	{ "destroy", "", NULL },
	{ "ack", "u", system_plain_types },
	{ "set_period", "u", system_plain_types },
};

/* The events of kl_system_monitor_v1. */
static const struct wl_message system_monitor_events[] = {
	{ "info", "usuu", system_plain_types },
	{ "device", "uuuuuuss", system_plain_types },
	{ "info_done", "u", system_plain_types },
	{ "cpu", "uuuuuuuuu", system_plain_types },
	{ "memory", "uuuuuuuuuuuu", system_plain_types },
	{ "link", "uuuuuuu", system_plain_types },
	{ "disk", "uuuuuuuuuuuuuuuu", system_plain_types },
	{ "gpu", "uuuuuuuuuuuuiu", system_plain_types },
	{ "sample_done", "uuuuuui", system_plain_types },
};

/* kl_system_monitor_v1: three requests and nine events.  It lives for the program. */
const struct wl_interface kl_system_monitor_v1_interface = {
	KL_SYSTEM_MONITOR_NAME,
	1,
	3,
	system_monitor_requests,
	9,
	system_monitor_events
};

/* The requests of kl_system_sharing_v1 (ws089-p025). */
static const struct wl_message system_sharing_requests[] = {
	{ "destroy", "", NULL },
	{ "set_ssh", "uu", system_plain_types },
	{ "query", "u", system_plain_types },
};

/* The events of kl_system_sharing_v1. */
static const struct wl_message system_sharing_events[] = {
	{ "state", "uuuuus", system_plain_types },
	{ "done", "u", system_plain_types },
	{ "result", "uuu", system_plain_types },
};

/* kl_system_sharing_v1, made at the manager's version (7): three requests and three events.  It lives for the program. */
const struct wl_interface kl_system_sharing_v1_interface = {
	KL_SYSTEM_SHARING_NAME,
	KL_SYSTEM_SINCE_SHARING,
	3,
	system_sharing_requests,
	3,
	system_sharing_events
};

/* The requests of kl_system_notify_v1 (ws156-p002). */
static const struct wl_message system_notify_requests[] = {
	{ "destroy", "", NULL },
	{ "post", "uusssu", system_plain_types },
	{ "withdraw", "uu", system_plain_types },
};

/* The events of kl_system_notify_v1. */
static const struct wl_message system_notify_events[] = {
	{ "posted", "uu", system_plain_types },
	{ "activated", "u", system_plain_types },
	{ "closed", "uu", system_plain_types },
	{ "result", "uuu", system_plain_types },
};

/* kl_system_notify_v1, made at the manager's version (13): three requests and four events.  It lives for the program. */
const struct wl_interface kl_system_notify_v1_interface = {
	KL_SYSTEM_NOTIFY_NAME,
	KL_SYSTEM_SINCE_NOTIFY,
	3,
	system_notify_requests,
	4,
	system_notify_events
};

/* The requests of kl_system_mail_v1 (ws169-p002). */
static const struct wl_message system_mail_requests[] = {
	{ "destroy", "", NULL },
	{ "arrived", "ussss", system_plain_types },
	{ "listen", "us", system_plain_types },
};

/* The events of kl_system_mail_v1. */
static const struct wl_message system_mail_events[] = {
	{ "mail", "sss", system_plain_types },
	{ "result", "uuu", system_plain_types },
	{ "allowed", "20u", system_plain_types },
};

/* kl_system_mail_v1, made at the manager's version (15; allowed since 20, ws177-p005): three requests and three events.  It lives for the program. */
const struct wl_interface kl_system_mail_v1_interface = {
	KL_SYSTEM_MAIL_NAME,
	KL_SYSTEM_SINCE_MAIL_ALLOWED,
	3,
	system_mail_requests,
	3,
	system_mail_events
};

/* The requests of kl_system_phone_v1 (ws170-p004). */
static const struct wl_message system_phone_requests[] = {
	{ "destroy", "", NULL },
	{ "send", "uuss", system_plain_types },
	{ "call", "uus", system_plain_types },
};

/* The events of kl_system_phone_v1. */
static const struct wl_message system_phone_events[] = {
	{ "received", "ussuu", system_plain_types },
	{ "status", "uu", system_plain_types },
	{ "result", "uuu", system_plain_types },
};

/* kl_system_phone_v1, made at the manager's version (16): three requests and three events.  It lives for the program. */
const struct wl_interface kl_system_phone_v1_interface = {
	KL_SYSTEM_PHONE_NAME,
	KL_SYSTEM_SINCE_PHONE,
	3,
	system_phone_requests,
	3,
	system_phone_events
};

/* The requests of kl_system_printers_v1 (ws145-p003). */
static const struct wl_message system_printers_requests[] = {
	{ "destroy", "", NULL },
	{ "add", "uusus", system_plain_types },
	{ "remove", "uu", system_plain_types },
	{ "set_default", "uu", system_plain_types },
	{ "print", "uush", system_plain_types },
	{ "cancel", "uu", system_plain_types },
};

/* The events of kl_system_printers_v1. */
static const struct wl_message system_printers_events[] = {
	{ "printer", "uusussu", system_plain_types },
	{ "job", "uuuss", system_plain_types },
	{ "done", "u", system_plain_types },
	{ "queued", "uu", system_plain_types },
	{ "result", "uuu", system_plain_types },
};

/* kl_system_printers_v1, made at the manager's version (17): six requests and five events.  It lives for the program. */
const struct wl_interface kl_system_printers_v1_interface = {
	KL_SYSTEM_PRINTERS_NAME,
	KL_SYSTEM_SINCE_PRINTERS,
	6,
	system_printers_requests,
	5,
	system_printers_events
};

/* The requests of kl_system_displays_v1 (ws113-p005). */
static const struct wl_message system_displays_requests[] = {
	{ "destroy", "", NULL },
	{ "apply", "uuus", system_plain_types },
	{ "set_brightness", "usu", system_plain_types },
	{ "set_shown", "usu", system_plain_types },
};

/* The events of kl_system_displays_v1. */
static const struct wl_message system_displays_events[] = {
	{ "output", "ssiiuuuuu", system_plain_types },
	{ "done", "uu", system_plain_types },
	{ "result", "uuu", system_plain_types },
};

/* kl_system_displays_v1, made at the manager's version (18; set_shown since 19, ws113-p014): four requests and three events.  It lives for the program. */
const struct wl_interface kl_system_displays_v1_interface = {
	KL_SYSTEM_DISPLAYS_NAME,
	KL_SYSTEM_SINCE_SHOWN,
	4,
	system_displays_requests,
	3,
	system_displays_events
};
