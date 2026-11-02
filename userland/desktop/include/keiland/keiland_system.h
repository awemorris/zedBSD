/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop's system for applications, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_SYSTEM_H
#define KEILAND_SYSTEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;

/*
 * The desktop's system for applications: the network, the sound, the power
 * and the removable devices, as the compositor holds them through Keiland's
 * system extension.  An application asks the compositor and never reaches a
 * daemon or the operating system itself.
 *
 * The state is the compositor's, told as one state at a time: an
 * application sees the state before a change or after it, never half of
 * it.  A request is answered once, by a result kl_system_take_result
 * gives; a change it makes comes as a new state.  One network request is
 * outstanding at a time, the system bar's included; another is answered
 * EBUSY.  One thread uses one kl_system.
 */

/* The longest SSID, interface name and dotted IPv4 address, with their NULs. */
#define KL_NETWORK_SSID_MAX	33U
#define KL_NETWORK_NAME_MAX	16U
#define KL_NETWORK_ADDRESS_MAX	16U

/* The longest IPv6 address with its prefix length ("ADDRESS/LENGTH") and its NUL, and the most a link carries. */
#define KL_NETWORK_INET6_MAX	50U
#define KL_NETWORK_INET6_COUNT	4U

/* The most networks a scan, interfaces, DNS servers and saved networks a kl_system keeps. */
#define KL_NETWORK_SCAN_MAX	24U
#define KL_NETWORK_LINKS_MAX	16U
#define KL_NETWORK_DNS_MAX	4U
#define KL_NETWORK_SAVED_MAX	24U

/* A WPA key's length. */
#define KL_NETWORK_KEY_MIN	8U
#define KL_NETWORK_KEY_MAX	63U

/* What carries the connection. */
#define KL_NETWORK_NONE		0U
#define KL_NETWORK_WIRED	1U
#define KL_NETWORK_WIFI		2U

/* The Wi-Fi's state. */
#define KL_WIFI_ABSENT		0U	/* no radio */
#define KL_WIFI_OFF		1U
#define KL_WIFI_SEARCHING	2U
#define KL_WIFI_CONNECTING	3U
#define KL_WIFI_CONNECTED	4U
#define KL_WIFI_DISCONNECTED	5U	/* on, and left unconnected by the user */

/* The network's requests (kl_system_network_request). */
#define KL_NETWORK_SCAN		1U
#define KL_NETWORK_JOIN		2U	/* names the network; its key must be saved */
#define KL_NETWORK_DISCONNECT	3U
#define KL_NETWORK_WIFI_ON	4U
#define KL_NETWORK_WIFI_OFF	5U

/* The power's actions, their bits in the state's actions, and where the power comes from. */
#define KL_POWER_POWEROFF	1U
#define KL_POWER_REBOOT		2U
#define KL_POWER_SUSPEND	3U
#define KL_POWER_ACTION_BIT(action)	(1U << (action))
#define KL_POWER_SOURCE_UNKNOWN	0U
#define KL_POWER_SOURCE_AC	1U
#define KL_POWER_SOURCE_BATTERY	2U

/* The longest device ID, name and location, with their NULs, and the most devices kept. */
#define KL_DEVICE_TEXT_MAX	64U
#define KL_DEVICES_MAX		16U

/* What the compositor offers (kl_system_capabilities). */
#define KL_SYSTEM_HAS_NETWORK	0x2U
#define KL_SYSTEM_HAS_AUDIO	0x4U
#define KL_SYSTEM_HAS_POWER	0x8U
#define KL_SYSTEM_HAS_DEVICES	0x10U
#define KL_SYSTEM_HAS_MONITOR	0x20U	/* kl_system_monitor_open */
#define KL_SYSTEM_HAS_ACCOUNT	0x40U	/* kl_system_account_set_password (KL_VERSION 27) */
#define KL_SYSTEM_HAS_SHARING	0x80U	/* kl_system_sharing_* (KL_VERSION 30) */
#define KL_SYSTEM_HAS_ADMINISTER	0x100U	/* kl_system_account_administer (KL_VERSION 31) */
#define KL_SYSTEM_HAS_PIN	0x200U	/* kl_system_account_set_pin (KL_VERSION 34) */
#define KL_SYSTEM_HAS_NOTIFY	0x400U	/* kl_system_notify (KL_VERSION 49) */
#define KL_SYSTEM_HAS_KEYS	0x800U	/* kl_system_account_add_key and the keys (KL_VERSION 52) */
#define KL_SYSTEM_HAS_MAIL	0x1000U	/* kl_system_mail_arrived and kl_system_mail_listen (KL_VERSION 54) */
#define KL_SYSTEM_HAS_PHONE	0x2000U	/* kl_system_phone_send and kl_system_phone_call (KL_VERSION 55) */
#define KL_SYSTEM_HAS_PRINTERS	0x4000U	/* kl_system_printers_* and kl_system_print_* (KL_VERSION 56) */
#define KL_SYSTEM_HAS_DISPLAYS	0x8000U	/* kl_system_displays_* (KL_VERSION 58) */
#define KL_SYSTEM_HAS_MACHINE	0x10000U	/* kl_system_machine_* (KL_VERSION 69) */
#define KL_SYSTEM_HAS_BLUETOOTH	0x20000U	/* kl_system_bluetooth_* (KL_VERSION 72) */
#define KL_SYSTEM_HAS_KEY_OPS	0x40000U	/* kl_system_account_key_info, _key_pin, _key_reset (KL_VERSION 77) */
#define KL_SYSTEM_HAS_METHODS	0x80000U	/* kl_system_account_methods and _set_methods (KL_VERSION 78) */
#define KL_SYSTEM_HAS_PHONE_SYNC	0x100000U	/* kl_system_phone_listen, _link, _watch_link, _link_set, _sync, _page_end, _send_text, _mark_read, kl_system_take_phone_item (KL_VERSION 79) */
#define KL_SYSTEM_HAS_PHONE_CONTACTS	0x200000U	/* kl_system_phone_sync of KL_PHONE_CONTACTS and KL_PHONE_CALLS, kl_phone_link's contacts and record (KL_VERSION 80) */
#define KL_SYSTEM_HAS_PHONE_HF		0x800000U	/* kl_system_phone_calls, _hf and _control, the paired phone's calls (KL_VERSION 87) */

/* What a kl_system_dispatch found changed. */
#define KL_SYSTEM_CHANGED_NETWORK	0x1U	/* the network's state */
#define KL_SYSTEM_CHANGED_SCAN		0x2U	/* the networks of the scan */
#define KL_SYSTEM_CHANGED_DETAILS	0x4U	/* the interfaces, DNS servers and saved networks asked for */
#define KL_SYSTEM_CHANGED_AUDIO		0x8U
#define KL_SYSTEM_CHANGED_POWER		0x10U
#define KL_SYSTEM_CHANGED_DEVICES	0x20U
#define KL_SYSTEM_CHANGED_RESULT	0x40U	/* a request was answered */
#define KL_SYSTEM_CHANGED_SHARING	0x80U	/* Remote Login's state (KL_VERSION 30) */
#define KL_SYSTEM_CHANGED_ENROLLED	0x100U	/* the user's PIN and security keys (KL_VERSION 36) */
#define KL_SYSTEM_CHANGED_NOTIFY	0x200U	/* a notification's event (KL_VERSION 49) */
#define KL_SYSTEM_CHANGED_TOUCH	0x400U	/* a security key waits to be touched for an addition (KL_VERSION 52) */
#define KL_SYSTEM_CHANGED_MAIL	0x800U	/* a message arrived for a listener (KL_VERSION 54) */
#define KL_SYSTEM_CHANGED_PHONE	0x1000U	/* a phone's message came, or a message's or call's state (KL_VERSION 55) */
#define KL_SYSTEM_CHANGED_PRINTERS	0x2000U	/* the printers or the print jobs (KL_VERSION 56) */
#define KL_SYSTEM_CHANGED_DISPLAYS	0x4000U	/* the displays, their mode or a light (KL_VERSION 58) */
#define KL_SYSTEM_CHANGED_MACHINE	0x8000U	/* an answer of kl_system_machine_query (KL_VERSION 69) */
#define KL_SYSTEM_CHANGED_BLUETOOTH	0x10000U	/* Bluetooth's state or its devices (KL_VERSION 72) */
#define KL_SYSTEM_CHANGED_KEYS		0x20000U	/* a security key came or went, or the screen was unlocked (KL_VERSION 77) */
#define KL_SYSTEM_CHANGED_REPLUG	0x40000U	/* a key's reset waits for the key to be plugged in again (KL_VERSION 77) */

/*
 * The network: whether the daemon is reached, whether the machine is
 * connected and through what and which interface, the wired interface up
 * with an address (empty when none), and the Wi-Fi's state, interface and
 * the network it is on or joining.
 */
struct kl_network_state {
	unsigned reachable;
	unsigned connected;
	unsigned kind;
	char interface[KL_NETWORK_NAME_MAX];
	char wired[KL_NETWORK_NAME_MAX];
	unsigned wifi;
	char wifi_interface[KL_NETWORK_NAME_MAX];
	char ssid[KL_NETWORK_SSID_MAX];
};

/* One network a scan found: its SSID, its signal in dBm, and whether it asks for a key. */
struct kl_network_ap {
	char ssid[KL_NETWORK_SSID_MAX];
	int rssi;
	unsigned secured;
};

/*
 * One interface: its name, whether it is up, has its link and is the
 * loopback, its IPv4 address and netmask (empty when none), its hardware
 * address as text, its MTU, and the bytes it has received and sent; for a
 * wired one (KL_VERSION 29) how it is configured (KL_WIRED_*, unknown for
 * any other or an older compositor) and the router it was given;
 * (KL_VERSION 39) its link's speed in Mb/s, 0 while it is not known (or
 * from an older compositor); (KL_VERSION 76) whether it is a radio
 * (Wi-Fi), as the system tells it (0 from an older compositor); and
 * (KL_VERSION 108) its IPv6 addresses, each "ADDRESS/LENGTH", the global
 * ones before the link-local ones, and the router of its IPv6 default
 * route (none and empty from an older compositor).
 */
struct kl_network_link {
	char name[KL_NETWORK_NAME_MAX];
	unsigned up;
	unsigned running;
	unsigned loopback;
	char address[KL_NETWORK_ADDRESS_MAX];
	char netmask[KL_NETWORK_ADDRESS_MAX];
	char hardware[18];
	unsigned mtu;
	uint64_t received_bytes;
	uint64_t sent_bytes;
	unsigned wired_mode;
	char router[KL_NETWORK_ADDRESS_MAX];
	unsigned link_mbps;
	unsigned wireless;
	unsigned inet6_count;
	char inet6[KL_NETWORK_INET6_COUNT][KL_NETWORK_INET6_MAX];
	char router6[KL_NETWORK_INET6_MAX];
};

/* How a wired interface is configured (KL_VERSION 29). */
#define KL_WIRED_UNKNOWN	0U
#define KL_WIRED_DHCP		1U
#define KL_WIRED_STATIC		2U

/*
 * A wired interface's configuration asked for (KL_VERSION 29): its name,
 * DHCP or a static IPv4 address (KL_WIRED_*), the address, netmask and
 * router of a static one (empty otherwise; the router may be empty), and
 * up to two DNS servers (empty: the servers DHCP gives).
 */
struct kl_network_wired_config {
	char interface[KL_NETWORK_NAME_MAX];
	unsigned mode;
	char address[KL_NETWORK_ADDRESS_MAX];
	char netmask[KL_NETWORK_ADDRESS_MAX];
	char router[KL_NETWORK_ADDRESS_MAX];
	char dns[2][KL_NETWORK_ADDRESS_MAX];
};

/*
 * The sound output: whether the sound service is reached and has a
 * device, the device's rate and channels, the volume of each channel
 * (0 to 100) and whether it is muted.
 */
struct kl_audio_state {
	unsigned reachable;
	unsigned device;
	unsigned rate;
	unsigned channels;
	unsigned left;
	unsigned right;
	unsigned muted;
};

/*
 * The power: where it comes from, the battery's charge in percent (-1 when
 * unknown), whether it charges, and the KL_POWER_ACTION_BIT of each action
 * the user may take now.
 */
struct kl_power_state {
	unsigned source;
	int percent;
	unsigned charging;
	unsigned actions;
};

/*
 * One removable device (KL_VERSION 28, a volume volumed lists): its ID (the
 * disk's name), kind (KL_DEVICE_STORAGE), state (the KL_DEVICE_* bits:
 * mounted; new, inserted and never mounted since), name (the label, or the
 * disk's name) and where it is mounted ("" when not).
 */
#define KL_DEVICE_STORAGE	1U
#define KL_DEVICE_MOUNTED	0x1U
#define KL_DEVICE_NEW		0x2U

struct kl_device {
	char id[KL_DEVICE_TEXT_MAX];
	unsigned kind;
	unsigned state;
	char name[KL_DEVICE_TEXT_MAX];
	char location[KL_DEVICE_TEXT_MAX];
};

/*
 * A removable device's file system ("fat", "ufs"; "" when the compositor did
 * not tell) and size in bytes (0 when not told), KL_VERSION 33: what a
 * mount's confirmation shows.
 */
#define KL_DEVICE_FS_MAX	8U

struct kl_device_info {
	char fs[KL_DEVICE_FS_MAX];
	uint64_t bytes;
};

/*
 * One application's view of the system: the state the compositor told,
 * and the requests not answered yet.  It lives from kl_system_open to
 * kl_system_close.
 */
struct kl_system;

/*
 * Opens the system on a display and waits once for its first state.
 * Returns NULL with errno ENOTSUP without Keiland's system extension (not
 * Keiland, or another user's compositor), EPIPE when the compositor went,
 * or ENOMEM.
 */
struct kl_system *kl_system_open(struct wl_display *display);

/*
 * Closes the system.
 */
void kl_system_close(struct kl_system *system);

/*
 * Takes the compositor's events the display has read; *changed (may be
 * NULL) has the KL_SYSTEM_CHANGED_* bits of what changed since the last
 * dispatch.  It never waits.  Returns 0, or EPIPE once the compositor went.
 */
int kl_system_dispatch(struct kl_system *system, unsigned *changed);

/*
 * Reports the KL_SYSTEM_HAS_* bits of what the compositor offers.
 */
unsigned kl_system_capabilities(const struct kl_system *system);

/*
 * Takes one answered request: 1 with its number and its error (0, EPERM,
 * ENOTSUP, EBUSY, EINVAL, ENODEV, EIO; a join's and a save_key's own
 * ENOENT: no key saved, EACCES: the network refused the key, ENETUNREACH:
 * the network is out of reach), 0 when none is answered.
 */
int kl_system_take_result(struct kl_system *system, uint32_t *request, int *error);

/*
 * Copies the network's state.
 */
void kl_system_network_get_state(const struct kl_system *system, struct kl_network_state *state);

/*
 * Copies up to capacity networks of the last scan, the strongest first,
 * and returns how many were copied.
 */
size_t kl_system_network_get_scan(const struct kl_system *system, struct kl_network_ap *aps, size_t capacity);

/*
 * Asks for a network request (KL_NETWORK_*; a join names the SSID, the
 * others take NULL); request (may be NULL) names its answer.  Returns 0
 * when asked, ENOTSUP, or EINVAL.
 */
int kl_system_network_request(struct kl_system *system, unsigned what, const char *ssid, uint32_t *request);

/*
 * Asks for a Wi-Fi network's key to be saved in the user's store and the
 * network joined, answered once it is joined or it failed.  Returns 0 when
 * asked, ENOTSUP, or EINVAL (an SSID or key outside its bounds).  The key
 * is not kept.
 */
int kl_system_network_save_key(struct kl_system *system, const char *ssid, const char *key, uint32_t *request);

/*
 * Asks for the network's details: the interfaces, the DNS servers and the
 * saved networks come (KL_SYSTEM_CHANGED_DETAILS) before the answer.  It
 * is no request of the network daemon's and is asked alongside one.
 * Returns 0 when asked, or ENOTSUP.
 */
int kl_system_network_query_details(struct kl_system *system, uint32_t *request);

/*
 * Asks the compositor to keep the radios scanning (on 1) while the
 * application shows the networks around, and no longer (on 0) when it stops
 * showing them.  It is no request and has no answer: a new scan comes as
 * KL_SYSTEM_CHANGED_SCAN.  The compositor counts every application that asked
 * (and its own Wi-Fi menu), and an application that closes its system or ends
 * asks no longer.  One asking holds a minute: an application that shows the
 * networks longer asks again (every 30 seconds, say), so that one that hangs
 * does not keep the radios scanning.  Returns 0, or ENOTSUP when the
 * compositor does not know it (one older than KL_VERSION 24).
 */
int kl_system_network_set_scanning(struct kl_system *system, unsigned on);

/*
 * Asks for a wired interface to be configured (KL_VERSION 29): DHCP or a
 * static IPv4 address, with its router and DNS servers; the network daemon
 * keeps it for the next start too.  request (may be NULL) names its answer,
 * which comes once it is applied or refused.  Returns 0 when asked, ENOTSUP
 * (an older compositor), or EINVAL (a field that is too long, or a mode that
 * is neither).
 */
int kl_system_network_configure_wired(struct kl_system *system, const struct kl_network_wired_config *config, uint32_t *request);

/*
 * Remote Login (KL_VERSION 30): whether the system has it, whether it starts
 * with the system and runs now, its port, whether this user may change it
 * (root or a member of wheel), whether the state was ever read, and the host
 * key's fingerprint ("SHA256:...", or empty).
 */
#define KL_SHARING_FINGERPRINT_MAX	64U
struct kl_sharing_state {
	unsigned available;
	unsigned enabled;
	unsigned running;
	unsigned port;
	unsigned allowed;
	char fingerprint[KL_SHARING_FINGERPRINT_MAX];
};

/* Copies Remote Login's state (all zero without KL_SYSTEM_HAS_SHARING). */
void kl_system_sharing_get_state(const struct kl_system *system, struct kl_sharing_state *state);

/*
 * Turns Remote Login on (on 1) or off (0), now and at every start; the
 * answer names request and comes after the new state.  Returns 0 when
 * asked, or ENOTSUP without KL_SYSTEM_HAS_SHARING.  The answer is EPERM
 * for a user who may not.
 */
int kl_system_sharing_set_ssh(struct kl_system *system, unsigned on, uint32_t *request);

/* Reads Remote Login's state again (KL_SYSTEM_CHANGED_SHARING follows).  Returns 0 or ENOTSUP. */
int kl_system_sharing_query(struct kl_system *system, uint32_t *request);

/*
 * KL_VERSION 49: a notification shown at the bottom of the screen and kept in
 * the desktop's log.  app is the name shown (NULL: the window's application
 * ID), title and body its words (at most 64, 128 and 512 bytes of UTF-8),
 * replaces an earlier notification's number to give it new words (0 for a new
 * one), flags KL_NOTIFY_URGENT (shown over a fullscreen window) and
 * KL_NOTIFY_ACTION (a click of its body is told).  kl_system_notify asks; the
 * answer comes as a notification event: KL_NOTIFY_POSTED with its number for
 * the request, or the request's result (KL_SYSTEM_CHANGED_RESULT: EINVAL for
 * words too long, EBUSY when the application has 32 already).  Later events:
 * KL_NOTIFY_ACTIVATED (its body clicked) and KL_NOTIFY_CLOSED with why
 * (KL_NOTIFY_DISMISSED, _EXPIRED out of the log, _CLEARED, _WITHDRAWN).
 * kl_system_notify_withdraw takes one back.  Returns 0 when asked, ENOTSUP
 * without KL_SYSTEM_HAS_NOTIFY, or EINVAL.  KL_VERSION 65: when the events
 * come faster than they are taken and the library's ring of 32 drops the
 * oldest, the next take gives a KL_NOTIFY_LOST first, its id the number
 * dropped; a post faster than the desktop's rate (10 a second) is refused as
 * EBUSY, and words that are not UTF-8 or hold control characters are shown
 * mended (U+FFFD, a space).
 */
#define KL_NOTIFY_URGENT	0x1U
#define KL_NOTIFY_ACTION	0x2U
#define KL_NOTIFY_POSTED	1U
#define KL_NOTIFY_ACTIVATED	2U
#define KL_NOTIFY_CLOSED	3U
#define KL_NOTIFY_LOST		4U
#define KL_NOTIFY_DISMISSED	1U
#define KL_NOTIFY_EXPIRED	2U
#define KL_NOTIFY_CLEARED	3U
#define KL_NOTIFY_WITHDRAWN	4U
struct kl_notification {
	const char *app;
	const char *title;
	const char *body;
	uint32_t replaces;
	unsigned flags;
};
struct kl_notify_event {
	unsigned kind;
	uint32_t request;
	uint32_t id;
	unsigned reason;
};
int kl_system_notify(struct kl_system *system, const struct kl_notification *notification, uint32_t *request);
int kl_system_notify_withdraw(struct kl_system *system, uint32_t id, uint32_t *request);

/*
 * KL_VERSION 79: kl_system_notify with what the lock screen shows of the
 * notification (at most 128 bytes; never its body, which the lock screen does
 * not show); a compositor older than that takes it as kl_system_notify, and
 * the lock screen shows nothing of it.
 */
int kl_system_notify_lock(struct kl_system *system, const struct kl_notification *notification, const char *lock_text, uint32_t *request);
int kl_system_take_notify_event(struct kl_system *system, struct kl_notify_event *event);

/*
 * KL_VERSION 54: the arrivals of mail.  The mail program tells the compositor
 * of a new message with kl_system_mail_arrived: the account's name, the
 * sender, the subject and a sign-in code found in it (empty or NULL for none;
 * the body is never sent).  A reader (the browser) asks with
 * kl_system_mail_listen under its name; the compositor takes only a name its
 * settings know (mail.codes.<app>, EINVAL as the request's result otherwise)
 * and tells a message to the reader only while the user has that setting on.
 * Each message told is a KL_SYSTEM_CHANGED_MAIL and one
 * kl_system_take_mail_event.  The strings are cut to KL_MAIL_TEXT_MAX and
 * KL_MAIL_CODE_MAX bytes with their NUL.  Returns 0 when asked, ENOTSUP
 * without KL_SYSTEM_HAS_MAIL, or EINVAL.
 */
#define KL_MAIL_TEXT_MAX	256U
#define KL_MAIL_CODE_MAX	16U
struct kl_mail_arrival {
	const char *account;
	const char *from;
	const char *subject;
	const char *code;
};
struct kl_mail_event {
	char from[KL_MAIL_TEXT_MAX];
	char subject[KL_MAIL_TEXT_MAX];
	char code[KL_MAIL_CODE_MAX];
};
int kl_system_mail_arrived(struct kl_system *system, const struct kl_mail_arrival *arrival, uint32_t *request);
int kl_system_mail_listen(struct kl_system *system, const char *app, uint32_t *request);
int kl_system_take_mail_event(struct kl_system *system, struct kl_mail_event *event);

/*
 * KL_VERSION 65: whether the user lets this reader hear the arrivals now,
 * told after its listen is taken and whenever the setting changes (each a
 * KL_SYSTEM_CHANGED_MAIL): 1 allowed, 0 not, -1 not told yet.  Since then
 * only the mail program (its windows' app_id "mailer") is heard by
 * kl_system_mail_arrived; another program's is refused (EINVAL as the
 * request's result).
 */
int kl_system_mail_allowed(const struct kl_system *system);

/*
 * KL_VERSION 55: the phone.  kl_system_phone_send sends a message on a
 * channel (KL_PHONE_SMS, _MMS, _RCS) to a number, kl_system_phone_call calls
 * one (KL_PHONE_LINE, _VOIP); the compositor's backend, which the desktop's
 * setting phone.backend chooses, carries them out.  The request's result is
 * ENODEV without a backend.  What follows comes as phone events, each a
 * KL_SYSTEM_CHANGED_PHONE: KL_PHONE_STATUS with the request and its state
 * (KL_PHONE_SENT, _DELIVERED, _FAILED, _ANSWERED, _NO_ANSWER), and
 * KL_PHONE_RECEIVED with a message that came (to every application of the
 * user that has the phone).  Numbers are cut to KL_PHONE_NUMBER_MAX and words
 * to KL_PHONE_TEXT_MAX bytes with their NUL.  Returns 0 when asked, ENOTSUP
 * without KL_SYSTEM_HAS_PHONE, or EINVAL.
 */
#define KL_PHONE_SMS		0U
#define KL_PHONE_MMS		1U
#define KL_PHONE_RCS		2U
#define KL_PHONE_LINE		3U
#define KL_PHONE_VOIP		4U
#define KL_PHONE_RECEIVED	1U
#define KL_PHONE_STATUS		2U
#define KL_PHONE_SENT		1U
#define KL_PHONE_DELIVERED	2U
#define KL_PHONE_FAILED		3U
#define KL_PHONE_ANSWERED	4U
#define KL_PHONE_NO_ANSWER	5U
#define KL_PHONE_NUMBER_MAX	64U
#define KL_PHONE_TEXT_MAX	1024U
struct kl_phone_event {
	unsigned kind;
	uint32_t request;
	unsigned state;
	unsigned channel;
	char from[KL_PHONE_NUMBER_MAX];
	char text[KL_PHONE_TEXT_MAX];
	uint64_t time;
};
int kl_system_phone_send(struct kl_system *system, unsigned channel, const char *to, const char *text, uint32_t *request);
int kl_system_phone_call(struct kl_system *system, unsigned channel, const char *to, uint32_t *request);
int kl_system_take_phone_event(struct kl_system *system, struct kl_phone_event *event);

/*
 * KL_VERSION 79: the paired phone's messages (Bluetooth's MAP), for the phone
 * program (its windows' app_id "phone") only; the phone program keeps them,
 * nothing else does.
 *
 * kl_system_phone_listen(1) makes this program hear what comes: items (a
 * message that came by itself, request 0), the link's changes and the
 * drops; the link's state is told once at once (why "not-phone" for
 * another program).  kl_system_phone_sync asks a page of the
 * synchronisation (the messages since a time, at most limit a folder (0:
 * all), from a cursor ("" at the start), count items (1 to 32); one at a
 * time a program): its items come numbered by the request, then its end
 * (kl_system_phone_page_end: where the next starts, whether more follow,
 * how many items came, were skipped, whether a folder's limit stopped it),
 * then its result.  kl_system_phone_send_text sends a text (1 to
 * KL_PHONE_SEND_MAX bytes, no NUL) to a number (the separators "-", " ",
 * "(", ")" and "." are taken out; then 1 to 32 of the digits, '+', '*' and
 * '#'): its result 0 says the phone's outbox has it, and KL_PHONE_STATUS
 * events say it was sent, delivered or failed.  kl_system_phone_mark_read
 * marks a message read on the phone by the handle of its item.
 * kl_system_phone_watch_link and kl_system_phone_link_set are Settings':
 * the link's changes alone, and the phone's switch and its profiles
 * (KL_PHONE_PROFILE_*; off keeps the phone's profiles as they are).
 *
 * KL_VERSION 80 (KL_SYSTEM_HAS_PHONE_CONTACTS): kl_system_phone_sync also
 * reads the phone's contacts (KL_PHONE_CONTACTS: since and limit 0) and its
 * calls' history (KL_PHONE_CALLS: the calls since a time, limit 0),
 * Bluetooth's PBAP.  The page's capped is bits: 1 a limit stopped it (5000
 * contacts, 500 calls of a kind), 2 the contacts' count changed while they
 * were read, 4 a page's count is not to be trusted (a card did not end); a
 * program that forgets what did not come keeps it while any is set.
 * kl_phone_link tells the contacts' state and whether the phone's record is
 * known.
 *
 * Every request is answered once by its result (kl_system_take_result):
 * 0, ENODEV without a backend, ENOTSUP, ENOTCONN (the phone's messages
 * are not connected), EACCES (not the phone's owner, or not the phone
 * program), EINVAL, EBUSY, ESTALE (a cursor or a handle of an earlier
 * session), ECONNRESET (lost on the way: a text may or may not have gone),
 * ETIMEDOUT, EMSGSIZE, ENOENT (not on the phone), ENOBUFS (this program
 * read too little and items were lost) or EIO.  A sync not answered in 180
 * seconds lets the next one start.
 *
 * Each item, link change and drop is a KL_SYSTEM_CHANGED_PHONE; the phone
 * events tell KL_PHONE_ITEMS (items wait: take them all with
 * kl_system_take_phone_item until it gives 0, also after any
 * KL_SYSTEM_CHANGED_PHONE), KL_PHONE_LINK_CHANGED (kl_system_phone_link
 * copies the state) and KL_PHONE_DROPPED (items or events were lost:
 * synchronise again).  An item's text lives in the library until the next
 * kl_system_take_phone_item.  The structures are passed with their size,
 * so that a later version adds fields at their end.
 */
#define KL_PHONE_MESSAGES		0U

/* A message folder flag, combined with 0 inbox or 1 sent, for MMS text. */
#define KL_PHONE_FOLDER_MMS		0x100U
#define KL_PHONE_CONTACTS		1U
#define KL_PHONE_CALLS			2U
#define KL_PHONE_ITEMS			10U
#define KL_PHONE_LINK_CHANGED		11U
#define KL_PHONE_DROPPED		12U
#define KL_PHONE_SEND_MAX		8192U
#define KL_PHONE_ITEM_TEXT_MAX		16384U
#define KL_PHONE_HANDLE_MAX		32U
#define KL_PHONE_KEY_MAX		20U
#define KL_PHONE_CURSOR_MAX		64U
#define KL_PHONE_PEER_MAX		132U
#define KL_PHONE_DATETIME_MAX		24U
#define KL_PHONE_ADDRESS_MAX		18U
#define KL_PHONE_WHY_MAX		32U
#define KL_PHONE_PROFILE_MESSAGES	1U
#define KL_PHONE_PROFILE_CONTACTS	2U
#define KL_PHONE_PROFILE_CALLS		4U

/*
 * One message: the sync's request (0 for one that came by itself), what
 * (KL_PHONE_MESSAGES), its handle for kl_system_phone_mark_read (good for
 * the phone's session only: not to be kept), its key (16 hexadecimal
 * digits, the same across sessions, or "-"), its folder (0 inbox, 1 sent, with KL_PHONE_FOLDER_MMS for MMS text)
 * and direction (0 in, 1 out), its time in UNIX seconds and where that
 * came from (0 the phone, 1 the phone's zone, 2 this computer's zone, 3
 * when it came), the phone's datetime as written, the other side's number
 * and name (empty when none), whether it is read, has no key, or was cut
 * to KL_PHONE_ITEM_TEXT_MAX bytes, and its text (UTF-8, ended by a NUL)
 * and length.
 *
 * KL_VERSION 80: a contact (what KL_PHONE_CONTACTS) has its key, how many
 * numbers it has (folder), its first number (peer), its name and its
 * reduced vCard 3.0 (text), the rest 0.  A call (KL_PHONE_CALLS) has its
 * key, its kind (folder: 0 received, 1 dialed, 2 missed), direction 1 for
 * one dialed, its time and where that came from (0 the phone's datetime
 * with its zone, 2 the phone's datetime read in this computer's zone, 3
 * the phone gave none or one not read: the time it came, partial), the
 * phone's datetime, the number and the name, and no text.
 */
struct kl_phone_item {
	uint32_t request;
	unsigned what;
	char handle[KL_PHONE_HANDLE_MAX];
	char key[KL_PHONE_KEY_MAX];
	unsigned folder;
	unsigned direction;
	int64_t time;
	unsigned zone;
	char datetime[KL_PHONE_DATETIME_MAX];
	char peer[KL_PHONE_PEER_MAX];
	char name[KL_PHONE_PEER_MAX];
	unsigned read;
	unsigned partial;
	unsigned truncated;
	const char *text;
	size_t length;
	/* KL_VERSION 82: when has_mime is set, the caller owns and closes mime_descriptor. */
	unsigned has_mime;
	int mime_descriptor;
};

/*
 * The phone link's state: the backend (0 none, 1 loopback, 2 bluetooth),
 * whether the phone is linked, its messages (0 off, 1 connecting, 2 ready,
 * 3 failed), whether a text can be sent and new messages are told, whether
 * this user owns the phone, the phone's switch and its profiles
 * (KL_PHONE_PROFILE_*), whether the owner is at the seat, the phone's
 * address, and why it does not work (a word, or empty).
 *
 * KL_VERSION 80 (at the end, 0 from an older compositor): the contacts (0
 * off, 1 connecting, 2 ready, 3 failed), why they stopped (a word, or
 * empty), and whether the phone's record is known (0 not known: the
 * loopback, or the phone's service not answering, or not read yet since
 * it answered; 1 there is none; 2 there is one), told whatever the
 * backend chosen.  A program of KL_VERSION 79 passes the smaller size and
 * gets the fields it knows.
 */
struct kl_phone_link {
	unsigned backend;
	unsigned linked;
	unsigned messages;
	unsigned can_send;
	unsigned notify;
	unsigned owner;
	unsigned enabled;
	unsigned profiles;
	unsigned present;
	char address[KL_PHONE_ADDRESS_MAX];
	char why[KL_PHONE_WHY_MAX];
	unsigned contacts;
	char contacts_why[KL_PHONE_WHY_MAX];
	unsigned record;
};

/* The phone's messages' calls, as told above (KL_SYSTEM_HAS_PHONE_SYNC). */
int kl_system_phone_listen(struct kl_system *system, unsigned on);
int kl_system_phone_link(const struct kl_system *system, struct kl_phone_link *link, size_t size);
int kl_system_phone_sync(struct kl_system *system, unsigned what, int64_t since, unsigned limit, const char *cursor, unsigned count, uint32_t *request);
int kl_system_take_phone_item(struct kl_system *system, struct kl_phone_item *item, size_t size);
int kl_system_phone_page_end(const struct kl_system *system, uint32_t request, char *cursor, size_t cursor_size, unsigned *more, unsigned *count, unsigned *skipped, unsigned *capped);
int kl_system_phone_send_text(struct kl_system *system, unsigned channel, const char *to, const char *text, size_t length, uint32_t *request);
/*
 * KL_VERSION 86: sends an MMS through the paired phone: its whole MIME body
 * (a caption and photos and videos, as libmms writes it) in a regular file
 * the caller keeps (read from its start, at most 16 MiB), to a number as for
 * kl_system_phone_send_text, whose result and KL_PHONE_STATUS events it has.
 * ENOTSUP with a compositor older than this, or without the phone's messages.
 */
int kl_system_phone_send_mms(struct kl_system *system, const char *to, int descriptor, uint32_t *request);

/*
 * KL_VERSION 87: the paired phone's calls (Bluetooth's HFP), for the phone
 * program only, which hears them while it listens (kl_system_phone_listen).
 * kl_system_phone_calls copies the calls there are (at most
 * KL_PHONE_CALLS_MAX: the phone's index of each, its state KL_PHONE_CALL_*,
 * whether it was dialed, the number and the name the phone gave),
 * kl_system_phone_hf the hands-free unit's state (KL_PHONE_HF_*, the phone's
 * network service 0 or 1, signal and battery 0 to 5, roaming, the speaker's
 * volume 0 to 15 or -1, voice recognition on, calls that can be held).  Each
 * change is a phone event KL_PHONE_CALLS_CHANGED or KL_PHONE_HF_CHANGED; a
 * call that ended is gone from the list.  kl_system_phone_control asks a
 * call's request (KL_PHONE_ASK_*): DIAL with the number, HOLD with "0" (end
 * the held or waiting call), "1" (end the active, take the other) or "2"
 * (swap), DTMF with one key of "0123456789*#ABCD", VOLUME with "0" to "15",
 * VOICE with "0" or "1", ANSWER and HANGUP with NULL; its result follows (as
 * send_text's).  Returns ENOTSUP with a compositor older than this or without
 * the phone's messages, EINVAL for a request it does not know.
 */
#define KL_PHONE_CALLS_CHANGED		13U
#define KL_PHONE_HF_CHANGED		14U
#define KL_PHONE_CALLS_MAX		2U
#define KL_PHONE_CALL_INCOMING		1U
#define KL_PHONE_CALL_DIALING		2U
#define KL_PHONE_CALL_ALERTING		3U
#define KL_PHONE_CALL_ACTIVE		4U
#define KL_PHONE_CALL_HELD		5U
#define KL_PHONE_CALL_WAITING		6U
#define KL_PHONE_HF_OFF			0U
#define KL_PHONE_HF_CONNECTING		1U
#define KL_PHONE_HF_READY		2U
#define KL_PHONE_HF_FAILED		3U
#define KL_PHONE_ASK_DIAL		1U
#define KL_PHONE_ASK_ANSWER		2U
#define KL_PHONE_ASK_HANGUP		3U
#define KL_PHONE_ASK_HOLD		4U
#define KL_PHONE_ASK_DTMF		5U
#define KL_PHONE_ASK_VOLUME		6U
#define KL_PHONE_ASK_VOICE		7U
struct kl_phone_call {
	unsigned index;
	unsigned state;
	unsigned outgoing;
	char number[KL_PHONE_PEER_MAX];
	char name[KL_PHONE_PEER_MAX];
};
struct kl_phone_hf {
	unsigned state;
	int service;
	int signal;
	int battery;
	int roam;
	int speaker;
	unsigned voice;
	unsigned hold;
};
int kl_system_phone_calls(const struct kl_system *system, struct kl_phone_call *calls, size_t capacity, size_t *count);
int kl_system_phone_hf(const struct kl_system *system, struct kl_phone_hf *hf);
int kl_system_phone_control(struct kl_system *system, unsigned ask, const char *argument, uint32_t *request);
int kl_system_phone_mark_read(struct kl_system *system, const char *handle, uint32_t *request);
int kl_system_phone_watch_link(struct kl_system *system, unsigned on);
int kl_system_phone_link_set(struct kl_system *system, const char *address, unsigned on, unsigned profiles, uint32_t *request);

/*
 * KL_VERSION 56: the printers.  The user's printers (an address, a port and a
 * protocol, IPP or LPD, with the IPP path or the LPD queue, "" for the usual
 * one) are kept by the compositor; kl_system_printers_get and
 * kl_system_print_jobs_get copy the printers and the jobs (the jobs not ended
 * and the last ones ended), and a change of either is
 * KL_SYSTEM_CHANGED_PRINTERS.  kl_system_printers_print prints a PDF file
 * (its path; the library opens it and sends its descriptor) on a printer (0:
 * the default) under a title; the job's number is kl_system_print_job_of's
 * for the request once its result came.  Each request is answered as a
 * result: 0, EINVAL (a printer or a job not known, the same printer twice, a
 * file that is not a PDF), EBUSY, ENOTSUP or EIO.  The calls return 0 when
 * asked, ENOTSUP without KL_SYSTEM_HAS_PRINTERS, or EINVAL; print also the
 * file's errno, EFBIG for an empty file or one over 256 MiB.
 */
#define KL_PRINTER_IPP		1U
#define KL_PRINTER_LPD		2U
#define KL_PRINTER_DEFAULT	0x1U
#define KL_PRINTERS_MAX		16U
#define KL_PRINT_JOBS_MAX	32U
#define KL_PRINTER_HOST_MAX	64U
#define KL_PRINTER_PATH_MAX	64U
#define KL_PRINTER_NAME_MAX	128U
#define KL_PRINT_TITLE_MAX	128U
#define KL_PRINT_DETAIL_MAX	32U
#define KL_PRINT_QUEUED		1U
#define KL_PRINT_SENDING	2U
#define KL_PRINT_WAITING	3U
#define KL_PRINT_DONE		4U
#define KL_PRINT_FAILED		5U
#define KL_PRINT_CANCELLED	6U
struct kl_printer {
	uint32_t id;
	unsigned protocol;
	char host[KL_PRINTER_HOST_MAX];
	unsigned port;
	char path[KL_PRINTER_PATH_MAX];
	char name[KL_PRINTER_NAME_MAX];
	unsigned flags;
};
struct kl_print_job {
	uint32_t job;
	uint32_t printer;
	unsigned state;
	char title[KL_PRINT_TITLE_MAX];
	char detail[KL_PRINT_DETAIL_MAX];
};
size_t kl_system_printers_get(const struct kl_system *system, struct kl_printer *printers, size_t capacity);
size_t kl_system_print_jobs_get(const struct kl_system *system, struct kl_print_job *jobs, size_t capacity);
int kl_system_printers_add(struct kl_system *system, unsigned protocol, const char *host, unsigned port, const char *path, uint32_t *request);
int kl_system_printers_remove(struct kl_system *system, uint32_t printer, uint32_t *request);
int kl_system_printers_set_default(struct kl_system *system, uint32_t printer, uint32_t *request);
int kl_system_printers_print(struct kl_system *system, uint32_t printer, const char *path, const char *title, uint32_t *request);
int kl_system_print_job_of(const struct kl_system *system, uint32_t request, uint32_t *job);
int kl_system_print_cancel(struct kl_system *system, uint32_t job, uint32_t *request);

/*
 * KL_VERSION 75: changes a printer's name and its IPP path or LPD queue (""
 * or NULL keeps each).  The name is made one line as a print's title is
 * (control characters become spaces, cut to 127 bytes); the path has no
 * space.  The answer is a result; ENOTSUP from a desktop older than the
 * printers' edit (kl_system_manager_v1 version 24).
 */
int kl_system_printers_edit(struct kl_system *system, uint32_t printer, const char *name, const char *path, uint32_t *request);

/*
 * KL_VERSION 58: the displays.  The compositor shows every display connected
 * in one of two modes, all extended (each its own part of one plane) or all
 * mirrored.  kl_system_displays_get copies the displays (each one's key, a
 * label for people, its place in the plane, its mode, KL_DISPLAY_* flags and
 * the light of a built-in panel in percent), kl_system_displays_mode tells
 * the mode; a change of either (a display plugged or unplugged, a choice
 * applied by anyone, a light changed, by the light keys too) is
 * KL_SYSTEM_CHANGED_DISPLAYS.  kl_system_displays_apply chooses the mode and,
 * for the extended mode, the places of the displays named (by their keys; the
 * others keep theirs); kl_system_displays_set_brightness sets the light of a
 * built-in panel (0 to 100).  Both are answered as a result
 * (kl_system_take_result): 0, ESTALE when the displays changed after the copy
 * the choice was made from (copy them again), EINVAL for places that overlap,
 * are not joined by an edge or are out of range, EPERM outside an active
 * session (the login screen, a lock), ENOTSUP for a display without a light
 * the compositor sets, or EIO.  The calls return 0 when asked, ENOTSUP
 * without KL_SYSTEM_HAS_DISPLAYS, or EINVAL.
 *
 * KL_VERSION 59: kl_system_displays_set_shown turns a display off (shown 0),
 * or on again, in the extended mode; the compositor keeps the choice, and an
 * anchor turned off gives the desktop to a display on.  A display turned off
 * carries KL_DISPLAY_OFF (in the mirror too, which shows it still).  It is
 * answered 0, EINVAL for the last display on or in the mirror, ENODEV for a
 * key not connected, EPERM outside an active session, or EIO; the call
 * returns ENOTSUP from a compositor before it.
 */
#define KL_DISPLAYS_MAX			8U
#define KL_DISPLAY_KEY_MAX		64U
#define KL_DISPLAY_LABEL_MAX		64U
#define KL_DISPLAYS_EXTENDED		0U
#define KL_DISPLAYS_MIRROR		1U
#define KL_DISPLAY_INTERNAL		0x1U	/* the machine's own panel */
#define KL_DISPLAY_ANCHOR		0x2U	/* the desktop's display: the bar and the windows are on it */
#define KL_DISPLAY_SHOWN		0x4U	/* shown now */
#define KL_DISPLAY_BACKLIGHT		0x8U	/* its light can be set */
#define KL_DISPLAY_LIMITED		0x10U	/* held back: the machine shows no more displays at once */
#define KL_DISPLAY_OFF			0x20U	/* turned off by the user in the extended mode (KL_VERSION 59) */
struct kl_display {
	char key[KL_DISPLAY_KEY_MAX];
	char label[KL_DISPLAY_LABEL_MAX];
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t refresh_mhz;
	unsigned flags;
	unsigned brightness;
};
struct kl_display_place {
	const char *key;
	int32_t x;
	int32_t y;
};
size_t kl_system_displays_get(const struct kl_system *system, struct kl_display *displays, size_t capacity);
unsigned kl_system_displays_mode(const struct kl_system *system);
int kl_system_displays_apply(struct kl_system *system, unsigned mode, const struct kl_display_place *places, size_t count, uint32_t *request);
int kl_system_displays_set_brightness(struct kl_system *system, const char *key, unsigned percent, uint32_t *request);
int kl_system_displays_set_shown(struct kl_system *system, const char *key, unsigned shown, uint32_t *request);

/*
 * A display's angle (KL_VERSION 93): the picture turned on the panel
 * clockwise by KL_DISPLAY_ROTATE_0, _90, _180 or _270 degrees (at 90 and 270
 * the display's width and height are its panel's height and width).
 * kl_system_displays_rotation gives the angle the user chose for a display of
 * the last snapshot (by its key): the one kept for it, or 0 when the display
 * may not take it.  kl_system_displays_rotations gives the angles it may be
 * turned to now, as a mask of KL_DISPLAY_ROTATION_*, and why it may not take
 * the others or does not show the one chosen (KL_DISPLAY_ROTATE_WHY_*): its
 * hardware cannot turn the picture, the quarter turns are not offered on this
 * device, a game pauses the turn, the turn's images could not be made, or
 * another display is shown beside it.  Both return 0, ENODEV for a key not in
 * the snapshot, or ENOTSUP from a compositor before them.
 * kl_system_displays_set_rotation turns a display at once and keeps the
 * choice; it is answered as a result (kl_system_take_result): 0, ENOTSUP for
 * an angle the display may not take now (nothing changes), ENODEV for a key
 * not connected, EPERM outside an active session, EINVAL, or EIO when the
 * turn could not be shown (the angle as it was).  The call returns 0 when
 * asked, EINVAL for a number that is not an angle, or ENOTSUP from a
 * compositor before it.  The snapshot that follows
 * (KL_SYSTEM_CHANGED_DISPLAYS) carries the new size.
 */
#define KL_DISPLAY_ROTATE_0		0U
#define KL_DISPLAY_ROTATE_90		90U
#define KL_DISPLAY_ROTATE_180		180U
#define KL_DISPLAY_ROTATE_270		270U
#define KL_DISPLAY_ROTATION_0		0x1U
#define KL_DISPLAY_ROTATION_90		0x2U
#define KL_DISPLAY_ROTATION_180		0x4U
#define KL_DISPLAY_ROTATION_270		0x8U
#define KL_DISPLAY_ROTATE_WHY_SCANOUT	0x1U	/* the display's hardware cannot turn the picture */
#define KL_DISPLAY_ROTATE_WHY_TURN	0x2U	/* 90 and 270 are not offered on this device yet */
#define KL_DISPLAY_ROTATE_WHY_GAME	0x4U	/* a game is shown: the turn is paused */
#define KL_DISPLAY_ROTATE_WHY_RESOURCE	0x8U	/* the turn's images could not be made */
#define KL_DISPLAY_ROTATE_WHY_ALONE	0x10U	/* another display is shown beside it */
int kl_system_displays_rotation(const struct kl_system *system, const char *key, unsigned *rotation);
int kl_system_displays_rotations(const struct kl_system *system, const char *key, unsigned *mask, unsigned *why);
int kl_system_displays_set_rotation(struct kl_system *system, const char *key, unsigned rotation, uint32_t *request);

/*
 * What Settings reads of the computer (KL_VERSION 69): the system's names
 * (About), the file systems' sizes (Storage), the people's accounts and the
 * program's own (Users), and the login screen's language (Languages).  The
 * compositor reads them when asked, on a thread of its own, and answers the
 * parts asked.
 *
 * kl_system_machine_query asks for the KL_MACHINE_* parts; it returns 0
 * when asked (a request number, as every request), ENOTSUP without
 * KL_SYSTEM_HAS_MACHINE, or EINVAL.  The answer is a result
 * (kl_system_take_result): 0 when the parts came, EBUSY when too many
 * queries wait, ENODEV when the program's queue had no room for the whole
 * answer, EINVAL or EIO; a part that came is a KL_SYSTEM_CHANGED_MACHINE
 * before the result, and its serial (kl_system_machine_serial) grows by
 * one, so that a program copies only the parts that changed.  A part never
 * answered is not known (kl_system_machine_known): kl_system_machine_about
 * and kl_system_machine_login_language then answer ENOENT, and the lists
 * copy none.  The lists are whole: each answer replaces the last.  A user
 * is a person's account (KL_MACHINE_USER_PERSON: a user ID from 1000 and a
 * shell to log in with), the program's own (KL_MACHINE_USER_SELF, the only
 * one with its home), or both, with whether it is an administrator and may
 * control Wi-Fi.  The login language is "en", "ja", or "" when it is not
 * set.
 */
#define KL_MACHINE_ABOUT		0x1U
#define KL_MACHINE_FILESYSTEMS		0x2U
#define KL_MACHINE_USERS		0x4U
#define KL_MACHINE_LOGIN_LANGUAGE	0x8U
#define KL_MACHINE_MOUNTS		0x10U	/* KL_VERSION 71: ENOTSUP from a compositor without it */
#define KL_MACHINE_USER_PERSON		0x1U
#define KL_MACHINE_USER_SELF		0x2U
#define KL_MACHINE_USER_ADMIN		0x4U
#define KL_MACHINE_USER_NETWORK		0x8U
#define KL_MACHINE_FILESYSTEMS_MAX	8U
#define KL_MACHINE_USERS_MAX		64U
#define KL_MACHINE_MOUNTS_MAX		64U

/* The system's names (an empty one could not be read) and how many processors are online (0 not known). */
struct kl_machine_about {
	char system[128];
	char kernel[160];
	char architecture[32];
	char processor[64];
	char host[64];
	unsigned cpus;
};

/* One file system: where it is mounted and its sizes in bytes. */
struct kl_machine_filesystem {
	char path[64];
	uint64_t total;
	uint64_t available;
	uint64_t used;
};

/* One account: its name, its full name, its home (the program's own only) and its KL_MACHINE_USER_* flags. */
struct kl_machine_user {
	char name[64];
	char full_name[128];
	char home[256];
	unsigned flags;
};

/* One mounted file system a user may keep files on (KL_VERSION 71): where it is mounted and its type. */
struct kl_machine_mount {
	char path[256];
	char type[32];
};

int kl_system_machine_query(struct kl_system *system, unsigned what, uint32_t *request);
unsigned kl_system_machine_known(const struct kl_system *system);
uint32_t kl_system_machine_serial(const struct kl_system *system, unsigned part);
int kl_system_machine_about(const struct kl_system *system, struct kl_machine_about *about);
size_t kl_system_machine_filesystems(const struct kl_system *system, struct kl_machine_filesystem *list, size_t capacity);
size_t kl_system_machine_users(const struct kl_system *system, struct kl_machine_user *list, size_t capacity);
int kl_system_machine_login_language(const struct kl_system *system, char *code, size_t size);
size_t kl_system_machine_mounts(const struct kl_system *system, struct kl_machine_mount *list, size_t capacity);

/*
 * KL_VERSION 72: Bluetooth, which the compositor keeps (a pairing's questions
 * are the compositor's own window's, never an application's).
 * kl_system_bluetooth_state copies the state: whether the service answers,
 * the controller's state (KL_BLUETOOTH_ABSENT ... KL_BLUETOOTH_ERROR), the
 * KL_BLUETOOTH_SCANNING, _PAIRING, _POWERED (the user's switch) and _ANSWERS
 * (this desktop answers the pairings' questions; 0 while another program of
 * the user does) flags, what the service can do (KL_BLUETOOTH_CAN_*), and the
 * controller's address and name.  kl_system_bluetooth_devices copies the
 * devices (the paired first, then those a scan saw): address, type, name,
 * kind (for an icon), KL_BLUETOOTH_PAIRED, _LEGACY and _CONNECTED, battery in
 * percent (-1 unknown) and signal in dBm (0 unknown).  Either changing is
 * KL_SYSTEM_CHANGED_BLUETOOTH.  kl_system_bluetooth_watch(1) has the state
 * read often while it is shown (0 when it is not);
 * kl_system_bluetooth_scan(1) looks for the devices around, and holds for a
 * minute: ask again while they are shown, 0 when they are not.
 * kl_system_bluetooth_power turns the controller on or off (the service
 * remembers it); kl_system_bluetooth_device pairs, forgets, connects or
 * disconnects a device (KL_BLUETOOTH_PAIR ... _DISCONNECT), or pairs a phone
 * as this user's (KL_BLUETOOTH_PAIR_PHONE, KL_VERSION 79; an older compositor
 * answers EINVAL).  They are answered as a result (kl_system_take_result): 0,
 * EPERM (not allowed), EBUSY (another request or a pairing runs), EINVAL,
 * ENOTSUP (the service cannot), ENODEV (no service, or the controller is
 * off), ENETUNREACH (the device did not answer), EACCES (the device or the
 * person refused) or EIO.  The calls return 0 when asked, ENOTSUP without
 * KL_SYSTEM_HAS_BLUETOOTH, or EINVAL.
 */
#define KL_BLUETOOTH_ABSENT		0U	/* no service to ask */
#define KL_BLUETOOTH_NONE		1U	/* no controller */
#define KL_BLUETOOTH_STARTING		2U
#define KL_BLUETOOTH_OFF		3U	/* turned off by the user */
#define KL_BLUETOOTH_ON			4U
#define KL_BLUETOOTH_FIRMWARE		5U	/* its firmware is missing or did not load */
#define KL_BLUETOOTH_UNSUPPORTED	6U
#define KL_BLUETOOTH_ERROR		7U
#define KL_BLUETOOTH_SCANNING		0x1U
#define KL_BLUETOOTH_PAIRING		0x2U
#define KL_BLUETOOTH_POWERED		0x4U
#define KL_BLUETOOTH_ANSWERS		0x8U
#define KL_BLUETOOTH_CAN_POWER		0x1U
#define KL_BLUETOOTH_CAN_CONNECT	0x2U
#define KL_BLUETOOTH_BREDR		0U
#define KL_BLUETOOTH_LE_PUBLIC		1U
#define KL_BLUETOOTH_LE_RANDOM		2U
#define KL_BLUETOOTH_KIND_OTHER		0U
#define KL_BLUETOOTH_KIND_KEYBOARD	1U
#define KL_BLUETOOTH_KIND_MOUSE		2U
#define KL_BLUETOOTH_KIND_AUDIO		3U
#define KL_BLUETOOTH_KIND_PHONE		4U
#define KL_BLUETOOTH_KIND_COMPUTER	5U
#define KL_BLUETOOTH_PAIRED		0x1U
#define KL_BLUETOOTH_LEGACY		0x2U
#define KL_BLUETOOTH_CONNECTED		0x4U
#define KL_BLUETOOTH_PAIR		1U
#define KL_BLUETOOTH_FORGET		2U
#define KL_BLUETOOTH_CONNECT		3U
#define KL_BLUETOOTH_DISCONNECT		4U
#define KL_BLUETOOTH_PAIR_PHONE		5U	/* pair a phone as this user's phone, over BR/EDR (KL_VERSION 79) */
#define KL_BLUETOOTH_ADDRESS_MAX	18U
#define KL_BLUETOOTH_NAME_MAX		64U
#define KL_BLUETOOTH_DEVICES_MAX	32U
struct kl_bluetooth_state {
	unsigned reachable;
	unsigned state;
	unsigned flags;
	unsigned features;
	char address[KL_BLUETOOTH_ADDRESS_MAX];
	char name[KL_BLUETOOTH_NAME_MAX];
};
struct kl_bluetooth_device {
	char address[KL_BLUETOOTH_ADDRESS_MAX];
	unsigned type;
	char name[KL_BLUETOOTH_NAME_MAX];
	unsigned kind;
	unsigned flags;
	int battery;
	int rssi;
};
int kl_system_bluetooth_state(const struct kl_system *system, struct kl_bluetooth_state *state);
size_t kl_system_bluetooth_devices(const struct kl_system *system, struct kl_bluetooth_device *devices, size_t capacity);
int kl_system_bluetooth_watch(struct kl_system *system, unsigned on);
int kl_system_bluetooth_scan(struct kl_system *system, unsigned on);
int kl_system_bluetooth_power(struct kl_system *system, unsigned on, uint32_t *request);
int kl_system_bluetooth_device(struct kl_system *system, unsigned action, const char *address, unsigned type, uint32_t *request);

/*
 * Copy up to capacity of the details last asked for and return how many
 * were copied.
 */
size_t kl_system_network_get_links(const struct kl_system *system, struct kl_network_link *links, size_t capacity);
size_t kl_system_network_get_dns(const struct kl_system *system, char (*servers)[KL_NETWORK_ADDRESS_MAX], size_t capacity);
size_t kl_system_network_get_saved(const struct kl_system *system, char (*ssids)[KL_NETWORK_SSID_MAX], size_t capacity);

/*
 * Copies the sound output's state.
 */
void kl_system_audio_get_state(const struct kl_system *system, struct kl_audio_state *state);

/*
 * Asks for each channel's volume (0 to 100) and the mute.  Returns 0 when
 * asked, ENOTSUP, or EINVAL.
 */
int kl_system_audio_set_volume(struct kl_system *system, unsigned left, unsigned right, unsigned muted, uint32_t *request);

/*
 * Asks for the short feedback sound at the device volume.  Returns 0 when
 * asked, or ENOTSUP.
 */
int kl_system_audio_feedback(struct kl_system *system, uint32_t *request);

/*
 * Copies the power's state.
 */
void kl_system_power_get_state(const struct kl_system *system, struct kl_power_state *state);

/*
 * Asks for a power action (KL_POWER_*); one not in the state's actions is
 * answered ENOTSUP.  Returns 0 when asked, ENOTSUP, or EINVAL.
 */
int kl_system_power_action(struct kl_system *system, unsigned action, uint32_t *request);

/*
 * Copies up to capacity removable devices and returns how many were
 * copied.
 */
size_t kl_system_devices_get(const struct kl_system *system, struct kl_device *devices, size_t capacity);

/*
 * Copies a listed device's file system and size: 1 when the device is in
 * the list (its info is empty from a compositor older than version 9), 0
 * when it is not.
 */
int kl_system_devices_info(const struct kl_system *system, const char *id, struct kl_device_info *info);

/*
 * Asks for a removable device to be ejected (unmounted; a device that is not
 * mounted may simply be taken out), answered by a result: 0, EBUSY while a
 * program uses it (kl_system_devices_busy_program names it), EACCES for a
 * user who is not the session's.  Returns 0 when asked, ENOTSUP, or EINVAL.
 */
int kl_system_devices_eject(struct kl_system *system, const char *id, uint32_t *request);

/*
 * Asks for a removable device to be mounted under /media (KL_VERSION 28),
 * answered by a result: 0 (its location then comes with the devices' change),
 * EACCES, or another errno value.  Returns 0 when asked, ENOTSUP (a
 * compositor without it), or EINVAL.
 */
int kl_system_devices_mount(struct kl_system *system, const char *id, uint32_t *request);

/*
 * Copies the program that keeps a device from being ejected, for a request
 * answered EBUSY (KL_VERSION 28).  Returns 1 with it, 0 without one.
 */
int kl_system_devices_busy_program(const struct kl_system *system, uint32_t request, char *program, size_t size);

/*
 * Asks for the password of the user the desktop runs as to be changed from
 * current to fresh (KL_VERSION 27), answered by a result
 * (kl_system_take_result): 0, EPERM when current is wrong, EINVAL when fresh
 * breaks the system's rules (on zedBSD: at least 8 characters, not the old
 * one), ENOTSUP where the desktop cannot change it, EBUSY while another
 * change is under way, EIO when it failed.  Returns 0 when asked, ENOTSUP
 * without KL_SYSTEM_HAS_ACCOUNT, or EINVAL (an empty password, or one longer
 * than 256 bytes).  The caller wipes its copies; the library keeps none.
 */
int kl_system_account_set_password(struct kl_system *system, const char *current, const char *fresh, uint32_t *request);

/* The longest operation kl_system_account_administer takes (without its NUL), and the room of a refusal's word (with it). */
#define KL_ACCOUNT_OPERATION_MAX	1024U
#define KL_ACCOUNT_REASON_SIZE		32U

/*
 * Asks for an administrator's change of the people's accounts (KL_VERSION 31;
 * docs/architecture/security.md): password is the caller's own, operation the
 * request's lines after it, each ended by a line end
 * ("add\nNAME\nDISPLAY\nPASSWORD\nadmin|user\n", "remove\nNAME\n
 * keep-home|remove-home\n", "reset-password\nNAME\nPASSWORD\n",
 * "group-add\nNAME\nwheel|network\n", "group-remove\n..."), answered by a
 * result (kl_system_take_result): 0, EPERM when the caller is not an
 * administrator or the password is wrong, EINVAL for another refusal,
 * ENOTSUP, EBUSY while another change is under way, EIO when it failed.  A
 * refusal's word comes first (kl_system_account_refusal).  Returns 0 when
 * asked, ENOTSUP without KL_SYSTEM_HAS_ADMINISTER, or EINVAL.  The caller
 * wipes its copies; the library keeps none.
 */
int kl_system_account_administer(struct kl_system *system, const char *password, const char *operation, uint32_t *request);

/*
 * Asks for the six-digit PIN of the user the desktop runs as to be set to
 * pin, or removed when pin is empty (KL_VERSION 34; the session manager keeps
 * it, in /etc/passkey on zedBSD), answered by a result
 * (kl_system_take_result): 0, EPERM when current (the user's password) is
 * wrong (the refusal's word, as kl_system_account_refusal gives it, says why:
 * bad-secret, locked, ...), EINVAL when pin is not six digits, ENOTSUP
 * without a session manager, EBUSY while another change is under way, EIO
 * when it failed.  Returns 0 when asked, ENOTSUP without KL_SYSTEM_HAS_PIN,
 * or EINVAL.  The caller wipes its copies; the library keeps none.
 */
int kl_system_account_set_pin(struct kl_system *system, const char *current, const char *pin, uint32_t *request);

/*
 * Gives whether the user has a PIN (pin 1) and how many security keys are
 * registered (KL_VERSION 36), as the compositor last told
 * (KL_SYSTEM_CHANGED_ENROLLED says it changed).  Returns 1 when known, 0
 * before the compositor told it (or one that cannot: both 0).
 */
int kl_system_account_enrolled(const struct kl_system *system, unsigned *pin, unsigned *keys);

/*
 * The user's security keys (KL_VERSION 52; with KL_SYSTEM_HAS_KEYS).  A key
 * is its reference (what remove_key takes) and its label.
 * kl_system_account_keys copies at most capacity of the keys the compositor
 * last told (with each enrolled, KL_SYSTEM_CHANGED_ENROLLED) and returns how
 * many there are.
 *
 * kl_system_account_add_key registers the key plugged in: its label (1 to
 * KL_SYSTEM_KEY_LABEL_MAX bytes, no colon) and the key's own PIN, checked
 * by the user's password; while the key waits to be touched
 * KL_SYSTEM_CHANGED_TOUCH comes and kl_system_account_touched gives the
 * request (once).  kl_system_account_remove_key removes one by its
 * reference.  Both are answered as kl_system_account_set_pin (a refusal's
 * word through kl_system_account_refusal: bad-secret, no-key, many-keys,
 * key-locked, timeout, ...).  Neither secret is kept.  They return 0,
 * ENOTSUP without KL_SYSTEM_HAS_KEYS, or EINVAL.
 */
#define KL_SYSTEM_KEY_REF_MAX	16U
#define KL_SYSTEM_KEY_LABEL_MAX	32U
#define KL_SYSTEM_KEYS_MAX	5U
struct kl_system_key {
	char ref[KL_SYSTEM_KEY_REF_MAX + 1U];
	char label[KL_SYSTEM_KEY_LABEL_MAX + 1U];
};
size_t kl_system_account_keys(const struct kl_system *system, struct kl_system_key *keys, size_t capacity);
int kl_system_account_add_key(struct kl_system *system, const char *password, const char *label, const char *pin, uint32_t *request);
int kl_system_account_remove_key(struct kl_system *system, const char *password, const char *ref, uint32_t *request);
int kl_system_account_touched(struct kl_system *system, uint32_t *request);

/*
 * The keys' own operations (KL_VERSION 77; with KL_SYSTEM_HAS_KEY_OPS).
 * kl_system_account_key_info asks what the keys there are; its result's
 * answer comes with kl_system_account_key_info_get (count, and for one key
 * its name, whether it has a PIN, its PIN's retries and its PIN's fewest
 * characters).  kl_system_account_key_pin sets the one key's first PIN
 * (current NULL) or changes it; the key checks the PINs.
 * kl_system_account_key_reset resets the key the user plugs in again, checked
 * by the user's password: KL_SYSTEM_CHANGED_REPLUG comes while the key is to
 * be plugged in again (kl_system_account_replugged gives the request, once),
 * KL_SYSTEM_CHANGED_TOUCH while it is to be touched, and after the result
 * kl_system_account_key_removed gives how many of this machine's
 * registrations went.  The results are as kl_system_account_set_pin's (the
 * refusal's word: bad-secret, bad-key-pin, key-locked, key-replug, no-pin,
 * pin-set, pin-policy, not-allowed, no-key, many-keys, timeout, canceled,
 * device).  kl_system_account_key_cancel stops the operation under way.
 * KL_SYSTEM_CHANGED_KEYS says a key came or went (or the screen was
 * unlocked): what is there may be asked again.  The requests return 0,
 * ENOTSUP without KL_SYSTEM_HAS_KEY_OPS, or EINVAL.
 */
#define KL_SYSTEM_KEY_NAME_MAX	63U
struct kl_system_key_info {
	unsigned count;
	char name[KL_SYSTEM_KEY_NAME_MAX + 1U];
	unsigned pin;
	unsigned retries;
	unsigned min;
};
int kl_system_account_key_info(struct kl_system *system, uint32_t *request);
int kl_system_account_key_info_get(const struct kl_system *system, struct kl_system_key_info *info);
int kl_system_account_key_pin(struct kl_system *system, const char *current, const char *pin, uint32_t *request);
int kl_system_account_key_reset(struct kl_system *system, const char *password, uint32_t *request);
int kl_system_account_key_cancel(struct kl_system *system);
int kl_system_account_replugged(struct kl_system *system, uint32_t *request);
unsigned kl_system_account_key_removed(const struct kl_system *system);

/*
 * Whether the user's security key asks its PIN to sign in (key_pin 1) and its
 * touch to unlock (key_touch 1), as the compositor last told with each
 * enrolled (KL_VERSION 77): 1 when known.  Set with
 * kl_system_account_set_key_options, checked by the user's password (no touch
 * only without the PIN), answered as kl_system_account_set_pin (the refusal's
 * word: bad-secret, not-enrolled, ...).  Returns 0, ENOTSUP without
 * KL_SYSTEM_HAS_KEY_OPS, or EINVAL.
 */
int kl_system_account_key_options(const struct kl_system *system, unsigned *key_pin, unsigned *key_touch);
int kl_system_account_set_key_options(struct kl_system *system, const char *password, unsigned key_pin, unsigned key_touch, uint32_t *request);

/*
 * The methods the login and locked screens take for the user (KL_VERSION 78;
 * with KL_SYSTEM_HAS_METHODS): KL_SYSTEM_METHOD_* bits, as the compositor
 * last told with each enrolled (every method until told; 1 when known).  A
 * method the user has not set up (no PIN, no key) is not taken whatever its
 * bit; the password is taken too when nothing else is a first sign-in.  The
 * console, su, sudo and SSH always take the password.
 * kl_system_account_set_methods sets them, checked by the user's password,
 * the password or a key among them, answered as kl_system_account_set_pin
 * (the refusal's word: bad-secret, not-enrolled, bad-request, ...).  Returns
 * 0, ENOTSUP without KL_SYSTEM_HAS_METHODS, or EINVAL.
 */
#define KL_SYSTEM_METHOD_PASSWORD	0x1U
#define KL_SYSTEM_METHOD_PIN		0x2U
#define KL_SYSTEM_METHOD_KEY		0x4U
#define KL_SYSTEM_METHODS_ALL		0x7U
int kl_system_account_methods(const struct kl_system *system, unsigned *methods);
int kl_system_account_set_methods(struct kl_system *system, const char *password, unsigned methods, uint32_t *request);

/*
 * Copies the word of a refused kl_system_account_administer request
 * (not-administrator, bad-password, no-such-user, name-taken, bad-name,
 * weak-password, last-administrator, self, root, busy, home-exists,
 * bad-request, failed).  Returns 1 with it, 0 without one.
 */
int kl_system_account_refusal(const struct kl_system *system, uint32_t request, char *reason, size_t size);

/*
 * The machine's monitor: what the System Monitor shows, sampled by the
 * compositor and made into rates here.  A monitor is opened on a kl_system
 * and its samples come with kl_system_dispatch; kl_system_monitor_take gives
 * the newest frame: the rates over the time since the sample before it (the
 * CPUs' shares, bytes and operations a second, the disks' mean latency, the
 * GPUs' shares) and the present values.  The first frame comes with the
 * second sample.  A device that came again, or a counter that went back, has
 * no rate for that frame.  Close every monitor before its kl_system.
 */

/* The most CPUs, GPUs, disks and links a monitor follows. */
#define KL_MONITOR_CPU_MAX	256U
#define KL_MONITOR_GPU_MAX	4U
#define KL_MONITOR_DISK_MAX	8U
#define KL_MONITOR_LINK_MAX	16U

/* What a frame has (struct kl_monitor_frame's valid). */
#define KL_MONITOR_FRAME_CPU		0x001U
#define KL_MONITOR_FRAME_MEMORY		0x002U
#define KL_MONITOR_FRAME_SWAP		0x004U
#define KL_MONITOR_FRAME_LINKS		0x008U
#define KL_MONITOR_FRAME_DISKS		0x010U
#define KL_MONITOR_FRAME_GPU_BUSY	0x020U
#define KL_MONITOR_FRAME_GPU_MEMORY	0x040U
#define KL_MONITOR_FRAME_GPU_FREQ	0x080U
#define KL_MONITOR_FRAME_TEMPERATURE	0x100U
#define KL_MONITOR_FRAME_POWER		0x200U

/* A disk's kind (struct kl_monitor_info's disk kind). */
#define KL_MONITOR_KIND_OTHER	1U
#define KL_MONITOR_KIND_NVME	2U
#define KL_MONITOR_KIND_USB	3U
#define KL_MONITOR_KIND_UAS	4U
#define KL_MONITOR_KIND_IDE	5U
#define KL_MONITOR_KIND_SDMMC	6U
#define KL_MONITOR_KIND_SCSI	7U

/* One GPU of the info: its id, name and driver. */
struct kl_monitor_gpu_info {
	uint64_t id;
	char name[48];
	char driver[16];
};

/* One disk of the info: its id, name and kind. */
struct kl_monitor_disk_info {
	uint64_t id;
	char name[32];
	unsigned kind;
};

/* One link of the info: its id and name. */
struct kl_monitor_link_info {
	uint64_t id;
	char name[16];
};

/* What does not change from frame to frame: the CPUs, the machine's name, the devices. */
struct kl_monitor_info {
	unsigned cpu_count;
	char host[64];
	unsigned gpu_count;
	unsigned disk_count;
	unsigned link_count;
	struct kl_monitor_gpu_info gpu[KL_MONITOR_GPU_MAX];
	struct kl_monitor_disk_info disk[KL_MONITOR_DISK_MAX];
	struct kl_monitor_link_info link[KL_MONITOR_LINK_MAX];
};

/* One link's bytes a second, received and sent, and whether it is up. */
struct kl_monitor_link_rate {
	uint64_t id;
	double rx_rate;
	double tx_rate;
	unsigned up;
};

/* One disk's bytes and operations a second, its mean latency in milliseconds, and its busy share (0 to 1). */
struct kl_monitor_disk_rate {
	uint64_t id;
	double read_rate;
	double write_rate;
	double read_ops;
	double write_ops;
	double latency_ms;
	double busy;
};

/* One GPU's busy share (0 to 1), its memory, frequencies, temperature and power. */
struct kl_monitor_gpu_rate {
	uint64_t id;
	double busy;
	uint64_t memory_used;
	uint64_t memory_total;
	unsigned cur_mhz;
	unsigned max_mhz;
	int milli_celsius;
	unsigned milli_watts;
};

/*
 * One frame: when (CLOCK_MONOTONIC, ns) and over how many seconds, what it
 * has, the CPUs' busy shares (0 to 1, all and each), the memory in bytes,
 * the links' and the disks' rates (each, and their sums; the disks'
 * latency is the mean over every operation), the GPUs, and the CPU's
 * temperature.
 */
struct kl_monitor_frame {
	uint64_t time_ns;
	double seconds;
	unsigned valid;
	double cpu;
	unsigned cpu_count;
	double cpu_core[KL_MONITOR_CPU_MAX];
	uint64_t memory_total;
	uint64_t memory_free;
	uint64_t memory_cache;
	uint64_t memory_reclaimable;
	uint64_t swap_total;
	uint64_t swap_used;
	unsigned link_count;
	struct kl_monitor_link_rate link[KL_MONITOR_LINK_MAX];
	double rx_rate;
	double tx_rate;
	unsigned disk_count;
	struct kl_monitor_disk_rate disk[KL_MONITOR_DISK_MAX];
	double read_rate;
	double write_rate;
	double disk_latency_ms;
	unsigned gpu_count;
	struct kl_monitor_gpu_rate gpu[KL_MONITOR_GPU_MAX];
	int cpu_milli_celsius;
};

/* A monitor on a kl_system. */
struct kl_system_monitor;

/*
 * Opens a monitor sampled every period_ms (250 to 10000; 0 is 1000).
 * Returns NULL with errno ENOTSUP when the compositor offers none, or
 * ENOMEM.
 */
struct kl_system_monitor *kl_system_monitor_open(struct kl_system *system, unsigned period_ms);

/*
 * Copies the newest frame into *frame: 1 when one came since the last
 * take, 0 when none did (*frame is left as it was).
 */
int kl_system_monitor_take(struct kl_system_monitor *monitor, struct kl_monitor_frame *frame);

/*
 * The info as last told (all zero before the first), and how many times it
 * changed (a new number means the devices changed).
 */
const struct kl_monitor_info *kl_system_monitor_info(const struct kl_system_monitor *monitor, unsigned *changes);

/*
 * Closes a monitor.
 */
void kl_system_monitor_close(struct kl_system_monitor *monitor);

/* The photo/video metadata library (KL_VERSION 81); media bytes stay in files. */
#define KL_SYSTEM_HAS_MEDIA 0x400000U
#define KL_SYSTEM_CHANGED_MEDIA 0x80000U

/* An answer of a media request started without waiting came (KL_VERSION 88): kl_system_media_take gives it. */
#define KL_SYSTEM_CHANGED_MEDIA_DONE 0x100000U

/* The most media requests under way on one system object, and the most paths or identities in one request. */
#define KL_SYSTEM_MEDIA_PENDING_MAX 8U
#define KL_SYSTEM_MEDIA_IDS_MAX 256U

/*
 * Executes a bounded query through the compositor's CLI backend and waits
 * for it: the returned metadata descriptor is caller-owned and contains
 * absolute file paths.  The wait is up to 125 seconds (an import up to an
 * hour): never call them on a window's thread; use the _begin forms there.
 * An older compositor returns ENOTSUP. apply borrows a rewound metadata file.
 */
int kl_system_media_list(struct kl_system *system, int *descriptor);
int kl_system_media_add_paths(struct kl_system *system, const char *const *paths, size_t count, int *descriptor);
int kl_system_media_apply(struct kl_system *system, int changes, int *descriptor);

/*
 * Removes managed media by stable ID after the application confirms deletion.
 * The import source is untouched. Call from a worker, as with other media mutations.
 * On success, descriptor receives a caller-owned metadata snapshot FD.
 * Returns ENOTSUP with a compositor older than manager version 31.
 */
int kl_system_media_remove_ids(struct kl_system *system, const char *const *ids, size_t count, int *descriptor);

/*
 * The same requests started without waiting (KL_VERSION 88): each returns 0
 * with its request's number, ENOTSUP without the media library, EPIPE once
 * the compositor went, or EBUSY with KL_SYSTEM_MEDIA_PENDING_MAX under way.
 * Its answer is told by KL_SYSTEM_CHANGED_MEDIA_DONE from kl_system_dispatch;
 * kl_system_media_take then gives each answered request (1, or 0 when none
 * is): its number, its errno value and, on success, the snapshot's descriptor
 * (the caller's).  kl_system_media_cancel gives a request up (its answer is
 * dropped).
 */
int kl_system_media_list_begin(struct kl_system *system, uint32_t *request);
int kl_system_media_add_paths_begin(struct kl_system *system, const char *const *paths, size_t count, uint32_t *request);
int kl_system_media_remove_ids_begin(struct kl_system *system, const char *const *ids, size_t count, uint32_t *request);
int kl_system_media_apply_begin(struct kl_system *system, int changes, uint32_t *request);
int kl_system_media_take(struct kl_system *system, uint32_t *request, int *error, int *descriptor);
void kl_system_media_cancel(struct kl_system *system, uint32_t request);

/* Subscribes to KL_SYSTEM_CHANGED_MEDIA; a query obtains the latest metadata. */
int kl_system_media_watch(struct kl_system *system, unsigned on);
/* Publishes a committed CLI mutation; the caller flushes or completes a round trip before exit. */
int kl_system_media_notify(struct kl_system *system);

#ifdef __cplusplus
}
#endif

#endif
