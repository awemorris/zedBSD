/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The interface between the Keiland compositor and its operating system
 * (WS131, plan/ws131/design.md section 3).
 *
 * libkeiland-backend holds every piece of the desktop that differs between
 * zedBSD, Linux and FreeBSD.  This header is the one interface they share:
 * each of userland/desktop/libkeiland-backend-zedbsd, -linux and -freebsd
 * implements it, and only the compositor calls it.  Applications never see
 * it; they reach the system through the compositor's extensions and
 * libkeiland.  Nothing here includes a header of the compositor or of
 * libkeiland, and an implementation reports to the compositor only through
 * the callbacks of struct kl_backend_host.
 *
 * The areas move here one at a time (plan/ws131/design.md section 7): the
 * network first, then the sound, the power, the seat and the session, the
 * input devices, the display and the GPU buffers.  An area an operating
 * system does not offer answers ENOTSUP.
 */

#ifndef KL_BACKEND_H
#define KL_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct pollfd;
struct kl_backend_input_caps;

/*
 * The revision of this interface.  The backend is a static library built
 * with the compositor from the same tree, so the number is only compared at
 * compile time; it is not an ABI version.
 */
#define KL_BACKEND_INTERFACE	1U

/*
 * The compositor's backend.
 *
 * One exists per compositor process, from kl_backend_open to
 * kl_backend_close.  It keeps what the operating system's areas need
 * between calls of the event loop.
 */
struct kl_backend;

/*
 * What the backend tells the compositor.
 *
 * The compositor fills it and hands it to kl_backend_open; the backend
 * keeps a copy.  Each area adds the callbacks it needs when it moves here,
 * and a callback is only called from kl_backend_poll_done or
 * kl_backend_tick, on the event loop's thread.  A callback never calls back
 * into the backend.  data is passed to every callback unchanged.
 */
struct kl_backend_host {
	void *data;
	void (*session_stop)(void *data, unsigned reason);
	void (*session_answer)(void *data, unsigned request, int error);
	void (*session_paused)(void *data);
	void (*session_resumed)(void *data);
	void (*input_paused)(void *data, const char *path);
	void (*input_resumed)(void *data, const char *path, int descriptor);
	void (*input_gone)(void *data, const char *path);
	int (*input_known)(void *data, const char *path);
	int (*input_found)(void *data, int descriptor, const char *path, const struct kl_backend_input_caps *caps);
	void (*input_changed)(void *data);
	void (*power_changed)(void *data);
	void (*power_button)(void *data, unsigned button);
	void (*lid_changed)(void *data, unsigned open);
};

/*
 * The system's events (ws132-p003), told through the host's last four
 * callbacks from kl_backend_poll_done: input_changed when an input device
 * came or went (the compositor scans the devices again), power_changed
 * when the AC adapter or a battery changed (kl_backend_power_get_state
 * reads the new state), power_button when a power or sleep button was
 * pressed (KL_BACKEND_BUTTON_*), and lid_changed with 1 when the lid
 * opened and 0 when it closed.  When events were lost, input_changed and
 * power_changed are both called.  A system without the events never calls
 * them, and the compositor finds devices by scanning on its own.  A host
 * may leave any of them NULL.
 */
#define KL_BACKEND_BUTTON_POWER		1U
#define KL_BACKEND_BUTTON_SLEEP		2U

/*
 * How the compositor is started.
 *
 * flags is 0 so far; the areas that need to know (a windowed run, the
 * descriptors sessiond hands over) add their fields when they move here.
 * greeter_descriptor is the login screen's descriptor to the session
 * manager (zedBSD's sessiond, --auth-fd), or -1 when the compositor is not
 * the login screen; session_descriptor is a session's descriptor to it
 * (--control-fd), or -1 when no session manager started the session.  The
 * session (ws131-p006) and the power (ws131-p005) speak on them; the
 * compositor makes them nonblocking and keeps them from its children.
 */
struct kl_backend_options {
	unsigned flags;
	int greeter_descriptor;
	int session_descriptor;
};

/*
 * Opens the backend.  Returns 0 and the backend through *backend, or an
 * errno value (ENOMEM).
 */
int kl_backend_open(const struct kl_backend_options *options, const struct kl_backend_host *host, struct kl_backend **backend);

/*
 * Closes the backend; NULL is allowed.
 */
void kl_backend_close(struct kl_backend *backend);

/*
 * Counts the descriptors the backend needs in the event loop's next poll.
 */
size_t kl_backend_poll_count(const struct kl_backend *backend);

/*
 * Fills the backend's kl_backend_poll_count descriptors, starting at
 * descriptors.
 */
void kl_backend_poll_fill(struct kl_backend *backend, struct pollfd *descriptors);

/*
 * Handles what poll reported for the descriptors kl_backend_poll_fill
 * filled.
 */
void kl_backend_poll_done(struct kl_backend *backend, const struct pollfd *descriptors);

/*
 * Lets the backend do the work that waits for time rather than for a
 * descriptor (now_ms is the compositor's monotonic clock).
 */
void kl_backend_tick(struct kl_backend *backend, uint64_t now_ms);

/*
 * The network (ws035-p013).
 *
 * The desktop's view of the network and its Wi-Fi switch, for the system
 * bar: whether the machine is connected and through what (a wired
 * interface, or a Wi-Fi network by its SSID), the networks the radio sees,
 * and the requests a user makes from a menu (join a network, disconnect,
 * turn Wi-Fi on or off).  The system's network daemon is behind it; the
 * desktop never speaks the daemon's protocol itself.
 *
 * Nothing here waits.  The state arrives when the daemon reports a change;
 * kl_backend_network_update reads what has arrived and says what changed.  A
 * request is sent at once and its answer arrives through the same update,
 * so a scan or a join that takes seconds does not stop the caller.  One
 * request is outstanding at a time (EBUSY otherwise).
 *
 * Every call that can fail returns 0 or an errno value: ENOENT (the daemon
 * is not running), EACCES or EPERM (the user may not look or act),
 * EBUSY, EINVAL, ENOMEM.
 */
struct kl_backend_network;

/* The longest SSID shown, as printable text with the terminating NUL. */
#define KL_BACKEND_NETWORK_SSID_MAX	33U

/* The longest interface name, with the terminating NUL. */
#define KL_BACKEND_NETWORK_NAME_MAX	16U

/* The most networks a scan keeps. */
#define KL_BACKEND_NETWORK_SCAN_MAX	24U

/* What carries the connection. */
#define KL_BACKEND_NETWORK_NONE		0U
#define KL_BACKEND_NETWORK_WIRED		1U
#define KL_BACKEND_NETWORK_WIFI		2U

/* The Wi-Fi's state. */
#define KL_BACKEND_WIFI_ABSENT		0U	/* no radio */
#define KL_BACKEND_WIFI_OFF		1U
#define KL_BACKEND_WIFI_SEARCHING		2U
#define KL_BACKEND_WIFI_CONNECTING	3U
#define KL_BACKEND_WIFI_CONNECTED		4U
#define KL_BACKEND_WIFI_DISCONNECTED	5U	/* on, and left unconnected by the user */

/* What kl_backend_network_update found (bits). */
#define KL_BACKEND_NETWORK_CHANGED_STATE	1U
#define KL_BACKEND_NETWORK_CHANGED_SCAN	2U
#define KL_BACKEND_NETWORK_CHANGED_DONE	4U

/* The requests. */
#define KL_BACKEND_NETWORK_REQUEST_NONE		0U
#define KL_BACKEND_NETWORK_REQUEST_SCAN		1U
#define KL_BACKEND_NETWORK_REQUEST_JOIN		2U
#define KL_BACKEND_NETWORK_REQUEST_DISCONNECT	3U
#define KL_BACKEND_NETWORK_REQUEST_WIFI_ON	4U
#define KL_BACKEND_NETWORK_REQUEST_WIFI_OFF	5U
#define KL_BACKEND_NETWORK_REQUEST_PROFILES	6U	/* the user's saved networks changed */
#define KL_BACKEND_NETWORK_REQUEST_WIRED	7U	/* a wired interface configured (ws089-p022) */

/*
 * The network as last reported: connected (an interface is up with an
 * address), through what and which interface, the wired interface that is
 * up with an address (empty when none, even while the Wi-Fi carries the
 * connection), and the Wi-Fi's state with the SSID of the network it is on
 * or joining (empty otherwise).  reachable is 0 while the daemon cannot be
 * reached.
 */
struct kl_backend_network_state {
	unsigned reachable;
	unsigned connected;
	unsigned kind;
	char interface[KL_BACKEND_NETWORK_NAME_MAX];
	char wired[KL_BACKEND_NETWORK_NAME_MAX];
	unsigned wifi;
	char wifi_interface[KL_BACKEND_NETWORK_NAME_MAX];
	char ssid[KL_BACKEND_NETWORK_SSID_MAX];
};

/*
 * One network a scan found: its SSID, its signal in dBm, and whether it
 * asks for a key.  The strongest of the access points of one SSID stands
 * for it.
 */
struct kl_backend_network_ap {
	char ssid[KL_BACKEND_NETWORK_SSID_MAX];
	int rssi;
	unsigned secured;
};

/*
 * Starts watching the network.  Returns NULL with errno set on ENOMEM; a
 * daemon that is not running yet is tried again by the updates.
 */
struct kl_backend_network *kl_backend_network_open(void);

/*
 * Stops watching and drops an outstanding request.
 */
void kl_backend_network_close(struct kl_backend_network *network);

/*
 * Reads what has arrived without waiting, and reconnects to a daemon that
 * went away (at most once a second).  *changed gets the
 * KL_BACKEND_NETWORK_CHANGED_* bits of what changed.
 */
int kl_backend_network_update(struct kl_backend_network *network, unsigned *changed);

/*
 * Copies the network's state as last reported.
 */
void kl_backend_network_get_state(const struct kl_backend_network *network, struct kl_backend_network_state *state);

/*
 * Copies up to capacity networks of the last scan, the strongest first,
 * and returns how many there are.
 */
size_t kl_backend_network_get_scan(const struct kl_backend_network *network, struct kl_backend_network_ap *aps, size_t capacity);

/*
 * Sends a request (KL_BACKEND_NETWORK_REQUEST_*; a join names the SSID, the
 * others take NULL).  A join uses the network's saved profile.
 */
int kl_backend_network_request(struct kl_backend_network *network, unsigned request, const char *ssid);

/*
 * Tells the request outstanding (KL_BACKEND_NETWORK_REQUEST_NONE when none),
 * or, after KL_BACKEND_NETWORK_CHANGED_DONE, the one that finished and its
 * errno value (0 when it succeeded) through *error.
 */
unsigned kl_backend_network_get_request(const struct kl_backend_network *network, int *error);

/*
 * Asks the daemon for fresh scans while on is 1, and no longer when it is 0
 * (ws089-p021: the compositor counts the windows that show the networks
 * around).  It is no request: it goes beside the one outstanding and never
 * makes one busy.  While it is on, the scan is read again every few
 * seconds and a new one comes as KL_BACKEND_NETWORK_CHANGED_SCAN; a radio
 * that is connected keeps the last scan (it cannot scan while it is on a
 * network).  Returns 0, or EINVAL without a watch.
 */
int kl_backend_network_set_scanning(struct kl_backend_network *network, unsigned on);

/*
 * The network's details for Settings (ws089-p003): each
 * interface as the kernel reports it, the DNS servers, and the keys of the
 * Wi-Fi networks the user has saved.  These read the kernel and the files
 * directly and do not wait for the daemon.
 *
 * A new network is joined with its key in three steps: the key is saved in
 * the user's credential store (kl_backend_network_save_key: /etc/wifi.conf for
 * root, the .wifi.conf of the passwd home otherwise), the daemon is told
 * the saved networks changed (KL_BACKEND_NETWORK_REQUEST_PROFILES), and the
 * network is joined (KL_BACKEND_NETWORK_REQUEST_JOIN).  A key is a WPA
 * passphrase of 8 to 63 characters; the daemon joins with the keys of the
 * user who turned the Wi-Fi on.
 */

/* The most interfaces and DNS servers reported, and an IPv4 address's text with its NUL. */
#define KL_BACKEND_NETWORK_LINKS_MAX	16U
#define KL_BACKEND_NETWORK_DNS_MAX		4U
#define KL_BACKEND_NETWORK_ADDRESS_MAX	16U

/* The shortest and the longest key. */
#define KL_BACKEND_NETWORK_KEY_MIN		8U
#define KL_BACKEND_NETWORK_KEY_MAX		63U

/*
 * One interface: its name, whether it is up and has its link, whether it
 * is the loopback, its IPv4 address and netmask (empty when it has none),
 * its hardware address and MTU, the bytes it has received and sent, and
 * its link's speed in Mb/s (0 while not known, BUG-222).
 */
struct kl_backend_network_link {
	char name[KL_BACKEND_NETWORK_NAME_MAX];
	unsigned up;
	unsigned running;
	unsigned loopback;
	char address[KL_BACKEND_NETWORK_ADDRESS_MAX];
	char netmask[KL_BACKEND_NETWORK_ADDRESS_MAX];
	unsigned char hardware[6];
	unsigned mtu;
	uint64_t received_bytes;
	uint64_t sent_bytes;
	unsigned wired_mode;
	char router[KL_BACKEND_NETWORK_ADDRESS_MAX];
	unsigned link_mbps;
};

/*
 * Copies up to capacity interfaces and returns how many there are (0 when
 * they cannot be read).
 */
size_t kl_backend_network_get_links(struct kl_backend_network_link *links, size_t capacity);

/* How a wired interface is configured (ws089-p022): not known, DHCP, a static IPv4 address. */
#define KL_BACKEND_WIRED_UNKNOWN	0U
#define KL_BACKEND_WIRED_DHCP		1U
#define KL_BACKEND_WIRED_STATIC		2U

/*
 * A wired interface's configuration asked for (ws089-p022): its name, the
 * mode, the address, netmask and router of a static one (the router may be
 * empty), and up to two DNS servers (empty: the servers DHCP gives).
 */
struct kl_backend_wired_config {
	char interface[KL_BACKEND_NETWORK_NAME_MAX];
	unsigned mode;
	char address[KL_BACKEND_NETWORK_ADDRESS_MAX];
	char netmask[KL_BACKEND_NETWORK_ADDRESS_MAX];
	char router[KL_BACKEND_NETWORK_ADDRESS_MAX];
	char dns[2][KL_BACKEND_NETWORK_ADDRESS_MAX];
};

/*
 * Sends a wired interface's configuration as a request of its own
 * (KL_BACKEND_NETWORK_REQUEST_WIRED, one at a time like the others): the
 * daemon keeps it for the next start and applies it now, and the answer
 * comes as KL_BACKEND_NETWORK_CHANGED_DONE.  Returns 0, EBUSY while
 * another request is outstanding, EINVAL for a field that does not fit,
 * ENOTSUP where the OS's backend cannot configure an interface, or the
 * errno value of reaching the daemon.
 */
int kl_backend_network_configure_wired(struct kl_backend_network *network, const struct kl_backend_wired_config *config);

/*
 * Copies up to capacity DNS servers of /etc/resolv.conf (dotted IPv4) and
 * returns how many were copied.
 */
size_t kl_backend_network_get_dns(char (*servers)[KL_BACKEND_NETWORK_ADDRESS_MAX], size_t capacity);

/*
 * Saves the key of a Wi-Fi network in the user's credential store (joined
 * by itself from then on).  Returns 0 or an errno value (EINVAL for an SSID
 * or a key outside the bounds).
 */
int kl_backend_network_save_key(const char *ssid, const char *key);

/*
 * Copies up to capacity SSIDs the user has saved keys for and returns how
 * many there are (0 when none, or the store cannot be read).
 */
size_t kl_backend_network_get_saved(char (*ssids)[KL_BACKEND_NETWORK_SSID_MAX], size_t capacity);


/*
 * The sound output's volume (ws100-p003, ws131-p004).
 *
 * The device volume the system's sound service applies to everything it
 * plays, 0 to 100 per channel, and whether it is muted, for the system
 * bar: audiod on zedBSD, the ALSA mixer on Linux and the OSS mixer on
 * FreeBSD.  Nothing here waits: kl_backend_audio_update reads what has
 * arrived, and a set or the feedback sound is sent at once.  A service that
 * is not running is not a failure; the updates connect again, at most once
 * a second.
 */
struct kl_backend_audio;

/* What the sound service last reported. */
struct kl_backend_audio_state {
	unsigned reachable;	/* 0 while the service cannot be reached */
	unsigned device;	/* 0 when the service has no sound device */
	unsigned rate;		/* the device's rate, 0 unknown */
	unsigned channels;
	unsigned left;		/* 0..100 */
	unsigned right;		/* 0..100 */
	unsigned muted;		/* 0 or 1 */
};

/* What kl_backend_audio_update found changed. */
#define KL_BACKEND_AUDIO_CHANGED_REACHABLE	1U	/* the service came or went */
#define KL_BACKEND_AUDIO_CHANGED_VOLUME		2U	/* the volume or mute changed */

/*
 * Starts following the volume.  Returns NULL only without memory.
 */
struct kl_backend_audio *kl_backend_audio_open(void);

/*
 * Stops following the volume.
 */
void kl_backend_audio_close(struct kl_backend_audio *audio);

/*
 * The descriptor to poll for the service's reports, or -1 when there is
 * none (an OSS mixer is read by the periodic updates).
 */
int kl_backend_audio_fd(const struct kl_backend_audio *audio);

/*
 * Reads what has arrived without waiting, and connects again when the
 * connection went (at most once a second).  *changed has the
 * KL_BACKEND_AUDIO_CHANGED_* bits of what changed.  Returns 0, or EINVAL.
 */
int kl_backend_audio_update(struct kl_backend_audio *audio, unsigned *changed);

/*
 * Copies what the service last reported.
 */
void kl_backend_audio_get_state(const struct kl_backend_audio *audio, struct kl_backend_audio_state *state);

/*
 * Asks for a volume (0..100 each) and mute.  The new volume comes back
 * through kl_backend_audio_update.  Returns 0, ENOTCONN (not connected),
 * EINVAL (out of range) or the error of sending.
 */
int kl_backend_audio_set_volume(struct kl_backend_audio *audio, unsigned left, unsigned right, unsigned muted);

/*
 * Asks for the short feedback sound at the device volume (a service
 * without it stays silent).  Returns 0, ENOTCONN, or the error of sending.
 */
int kl_backend_audio_feedback(struct kl_backend_audio *audio);

/*
 * Tells whether the sound service runs: 1 when it does, 0 when it does
 * not.  It does not connect and does not wait; a service that runs may
 * still have no sound device (struct kl_backend_audio_state's device).
 */
int kl_backend_audio_available(void);

/*
 * The removable media (ws132-p004): the volumes zedBSD's volumed lists (a
 * USB stick's FAT or UFS filesystem), mounted only when the user asks.
 * Like the sound, nothing here waits: kl_backend_volumes_update reads what
 * has arrived and connects again when the service went (at most every two
 * seconds); a mount or an eject is sent at once and answered later as a
 * result.  Linux and FreeBSD offer no volumes yet (every call ENOTSUP or
 * an empty list).
 */
struct kl_backend_volumes;

/* The most volumes kept, and the lengths of a volume's texts with their NULs. */
#define KL_BACKEND_VOLUMES_MAX		16U
#define KL_BACKEND_VOLUME_ID_MAX	32U
#define KL_BACKEND_VOLUME_LABEL_MAX	64U
#define KL_BACKEND_VOLUME_PATH_MAX	128U

/*
 * One volume: its ID (the disk's name), filesystem, label ("" without one),
 * size, where it is mounted ("" when it is not), and whether it was never
 * mounted since it was inserted (fresh: the desktop shows it).
 */
struct kl_backend_volume {
	char id[KL_BACKEND_VOLUME_ID_MAX];
	char fs[8];
	char label[KL_BACKEND_VOLUME_LABEL_MAX];
	char path[KL_BACKEND_VOLUME_PATH_MAX];
	uint64_t bytes;
	unsigned fresh;
};

/* What kl_backend_volumes_update found changed. */
#define KL_BACKEND_VOLUMES_CHANGED_LIST		1U	/* the volumes (or the service's reach) changed */
#define KL_BACKEND_VOLUMES_CHANGED_RESULT	2U	/* a mount or an eject was answered */

/*
 * Starts following the volumes.  Returns NULL only without memory.
 */
struct kl_backend_volumes *kl_backend_volumes_open(void);

/*
 * Stops following the volumes.
 */
void kl_backend_volumes_close(struct kl_backend_volumes *volumes);

/*
 * Reads what has arrived without waiting, and connects again when the
 * connection went.  *changed has the KL_BACKEND_VOLUMES_CHANGED_* bits.
 * Returns 0, or EINVAL.
 */
int kl_backend_volumes_update(struct kl_backend_volumes *volumes, unsigned *changed);

/*
 * Copies up to capacity volumes and returns how many were copied.
 */
size_t kl_backend_volumes_get(const struct kl_backend_volumes *volumes, struct kl_backend_volume *list, size_t capacity);

/*
 * Asks for a volume to be mounted (under /media, nosuid and noexec, owned
 * by the user) or ejected; *request numbers the answer.  Returns 0 when
 * asked, ENOTCONN (no service), ENOTSUP, EINVAL, or the error of sending.
 */
int kl_backend_volumes_mount(struct kl_backend_volumes *volumes, const char *id, uint32_t *request);
int kl_backend_volumes_eject(struct kl_backend_volumes *volumes, const char *id, uint32_t *request);

/*
 * Takes the oldest answer: its request, its errno value (0, EACCES, EBUSY
 * with the program using the volume in user, ...).  Returns 1 with one,
 * 0 when none is waiting.
 */
int kl_backend_volumes_take_result(struct kl_backend_volumes *volumes, uint32_t *request, int *error, char *user, size_t size);

/*
 * Bluetooth (ws143-p006, plan/ws143/phase006/phase.md section 2): the
 * controller's state and the user's switch, the devices (paired, seen by
 * a scan, connected), and the requests of the desktop (power, pair,
 * forget, connect, disconnect), whose answers come later as results.  A
 * pairing's questions (compare a number, agree, type a number shown on the
 * device) come as questions the compositor asks the user and answers.
 * zedBSD's is bluetoothd's socket; elsewhere the state is unreachable
 * until a backend is written (Linux's BlueZ: ws143-p007).
 *
 * Nothing here waits: kl_backend_bluetooth_update reads what has arrived,
 * and reads the state again every two seconds while someone watches
 * (kl_backend_bluetooth_set_watching) and after each answer, otherwise
 * every thirty seconds.  One request goes at a time (EBUSY otherwise); the
 * answer to a question goes beside it.
 */
struct kl_backend_bluetooth;

/* The most devices kept, and the lengths of a device's texts with their NULs. */
#define KL_BACKEND_BT_DEVICES_MAX	32U
#define KL_BACKEND_BT_ADDRESS_MAX	18U
#define KL_BACKEND_BT_NAME_MAX		64U
#define KL_BACKEND_BT_REASON_MAX	32U
#define KL_BACKEND_BT_USER_MAX		32U

/* The controller's state. */
#define KL_BACKEND_BT_ABSENT		0U	/* no service to ask */
#define KL_BACKEND_BT_NONE		1U	/* no controller */
#define KL_BACKEND_BT_STARTING		2U
#define KL_BACKEND_BT_OFF		3U	/* turned off by the user */
#define KL_BACKEND_BT_ON		4U
#define KL_BACKEND_BT_FIRMWARE		5U	/* its firmware is missing or did not load */
#define KL_BACKEND_BT_UNSUPPORTED	6U
#define KL_BACKEND_BT_ERROR		7U

/* What the service can do beyond the state and pairing (bits). */
#define KL_BACKEND_BT_CAN_POWER		1U
#define KL_BACKEND_BT_CAN_CONNECT	2U

/* A device's address type. */
#define KL_BACKEND_BT_BREDR		0U
#define KL_BACKEND_BT_LE_PUBLIC		1U
#define KL_BACKEND_BT_LE_RANDOM		2U

/* What a device is, for its icon. */
#define KL_BACKEND_BT_KIND_OTHER	0U
#define KL_BACKEND_BT_KIND_KEYBOARD	1U
#define KL_BACKEND_BT_KIND_MOUSE	2U
#define KL_BACKEND_BT_KIND_AUDIO	3U
#define KL_BACKEND_BT_KIND_PHONE	4U
#define KL_BACKEND_BT_KIND_COMPUTER	5U

/* The requests. */
#define KL_BACKEND_BT_POWER_ON		1U
#define KL_BACKEND_BT_POWER_OFF		2U
#define KL_BACKEND_BT_PAIR		3U
#define KL_BACKEND_BT_FORGET		4U
#define KL_BACKEND_BT_CONNECT		5U
#define KL_BACKEND_BT_DISCONNECT	6U

/* A pairing's questions. */
#define KL_BACKEND_BT_ASK_CONFIRM	1U	/* the same number on both sides? (yes or no) */
#define KL_BACKEND_BT_ASK_CONSENT	2U	/* pair with it at all? (yes or no) */
#define KL_BACKEND_BT_ASK_PASSKEY	3U	/* type this number on the device (no answer) */
#define KL_BACKEND_BT_ASK_END		4U	/* the question of the same id is over */

/* What kl_backend_bluetooth_update found changed (bits). */
#define KL_BACKEND_BT_CHANGED_STATE	1U
#define KL_BACKEND_BT_CHANGED_DEVICES	2U
#define KL_BACKEND_BT_CHANGED_RESULT	4U
#define KL_BACKEND_BT_CHANGED_QUESTION	8U

/*
 * The state as last read: whether the service answers, the controller's
 * state, whether it scans and whether a pairing runs, what the service can
 * do (KL_BACKEND_BT_CAN_*), whether the user's switch is on (it holds when
 * there is no controller), whether this answers the pairings' questions
 * (0: another program of the user does), and the controller's address and
 * name.
 */
struct kl_backend_bluetooth_state {
	unsigned reachable;
	unsigned state;
	unsigned scanning;
	unsigned pairing;
	unsigned features;
	unsigned power;
	unsigned agent;
	char address[KL_BACKEND_BT_ADDRESS_MAX];
	char name[KL_BACKEND_BT_NAME_MAX];
};

/*
 * One device: its address and type, name (the address when it has none),
 * kind (KL_BACKEND_BT_KIND_*), whether it is paired (and by the legacy way),
 * whether it is connected, its battery in percent (-1 unknown) and its
 * signal in dBm (0 unknown).
 */
struct kl_backend_bluetooth_device {
	char address[KL_BACKEND_BT_ADDRESS_MAX];
	unsigned type;
	char name[KL_BACKEND_BT_NAME_MAX];
	unsigned kind;
	unsigned paired;
	unsigned legacy;
	unsigned connected;
	int battery;
	int rssi;
};

/* Starts following Bluetooth.  Returns NULL only without memory. */
struct kl_backend_bluetooth *kl_backend_bluetooth_open(void);

/* Stops following, and ends a request or a question outstanding. */
void kl_backend_bluetooth_close(struct kl_backend_bluetooth *bluetooth);

/* Reads what has arrived without waiting; *changed has the KL_BACKEND_BT_CHANGED_* bits.  Returns 0 or EINVAL. */
int kl_backend_bluetooth_update(struct kl_backend_bluetooth *bluetooth, unsigned *changed);

/* Copies the state as last read. */
void kl_backend_bluetooth_get_state(const struct kl_backend_bluetooth *bluetooth, struct kl_backend_bluetooth_state *state);

/* Copies up to capacity devices (the paired first) and returns how many there are. */
size_t kl_backend_bluetooth_get_devices(const struct kl_backend_bluetooth *bluetooth, struct kl_backend_bluetooth_device *devices, size_t capacity);

/* Reads the state often while on is 1 (someone shows it), and scans for devices while scanning is 1. */
void kl_backend_bluetooth_set_watching(struct kl_backend_bluetooth *bluetooth, unsigned on);
void kl_backend_bluetooth_set_scanning(struct kl_backend_bluetooth *bluetooth, unsigned on);

/*
 * Sends a request (KL_BACKEND_BT_*; the power's take a NULL address) and
 * numbers its answer in *id.  Returns 0, EBUSY while one is outstanding,
 * ENOTCONN without the service, ENOTSUP for what it cannot do, or EINVAL.
 */
int kl_backend_bluetooth_request(struct kl_backend_bluetooth *bluetooth, unsigned request, const char *address, unsigned type, uint32_t *id);

/*
 * Takes the oldest answer: its id, its errno value (0, EACCES, EBUSY,
 * ETIMEDOUT, ECONNREFUSED, ENETDOWN while off, EIO) and the service's
 * reason in words.  Returns 1 with one, 0 when none waits.
 */
int kl_backend_bluetooth_take_result(struct kl_backend_bluetooth *bluetooth, uint32_t *id, int *error, char *reason, size_t size);

/*
 * One question of a pairing: its id (an ASK_END names the id it ends),
 * kind (KL_BACKEND_BT_ASK_*), number (CONFIRM and PASSKEY), the device
 * being paired (address, type, name as known), and who started the
 * pairing (the user's name, and whether it is this program's own).
 */
struct kl_backend_bluetooth_question {
	uint32_t id;
	unsigned kind;
	uint32_t number;
	char address[KL_BACKEND_BT_ADDRESS_MAX];
	unsigned type;
	char name[KL_BACKEND_BT_NAME_MAX];
	char user[KL_BACKEND_BT_USER_MAX];
	unsigned own;
};

/* Takes the oldest question.  Returns 1 with one, 0 when none waits. */
int kl_backend_bluetooth_take_question(struct kl_backend_bluetooth *bluetooth, struct kl_backend_bluetooth_question *question);

/*
 * Answers the question of an id (CONFIRM and CONSENT): 1 yes, 0 no.
 * Returns 0, or ENOENT when that question is not the one asked now (it
 * ended, or a newer one came).
 */
int kl_backend_bluetooth_answer(struct kl_backend_bluetooth *bluetooth, uint32_t id, unsigned yes);

/* Gives up this program's own pairing going on (its connection closes; the service stops it).  Returns 0, or ENOENT. */
int kl_backend_bluetooth_cancel(struct kl_backend_bluetooth *bluetooth);

/*
 * The printers (ws145-p003, plan/ws145/design.md section 4): the user's
 * printers, kept in a file of the user's (~/.config/keiland/printers.conf),
 * and the jobs sent to them.  The jobs go to the user's printer daemon,
 * keiland-printd, which this starts when there is something to do (with a
 * socket on its descriptor 3) and which sends them by IPP or LPD.  The code
 * is the same on every system.  Nothing here waits: kl_backend_print_update
 * reads what the daemon said; additions, removals, prints and cancels are
 * answered later as results.
 */
struct kl_backend_print;

/* The most printers and jobs kept, and the lengths of their texts with their NULs. */
#define KL_BACKEND_PRINTERS_MAX		16U
#define KL_BACKEND_PRINT_JOBS_MAX	32U
#define KL_BACKEND_PRINTER_HOST_MAX	64U
#define KL_BACKEND_PRINTER_PATH_MAX	64U
#define KL_BACKEND_PRINTER_NAME_MAX	128U
#define KL_BACKEND_PRINT_TITLE_MAX	128U
#define KL_BACKEND_PRINT_DETAIL_MAX	32U

/* The protocols. */
#define KL_BACKEND_PRINTER_IPP		1U
#define KL_BACKEND_PRINTER_LPD		2U

/* A job's states. */
#define KL_BACKEND_PRINT_QUEUED		1U
#define KL_BACKEND_PRINT_SENDING	2U
#define KL_BACKEND_PRINT_WAITING	3U
#define KL_BACKEND_PRINT_DONE		4U
#define KL_BACKEND_PRINT_FAILED		5U
#define KL_BACKEND_PRINT_CANCELLED	6U

/*
 * A printer: its number (kept in the file, never used again), protocol,
 * host, port, IPP path or LPD queue, name, and whether it is the default.
 */
struct kl_backend_printer {
	uint32_t id;
	unsigned protocol;
	char host[KL_BACKEND_PRINTER_HOST_MAX];
	unsigned port;
	char path[KL_BACKEND_PRINTER_PATH_MAX];
	char name[KL_BACKEND_PRINTER_NAME_MAX];
	unsigned is_default;
};

/* A job: its number, its printer, its state, its title and the word of its state ("" or a failure's). */
struct kl_backend_print_job {
	uint32_t job;
	uint32_t printer;
	unsigned state;
	char title[KL_BACKEND_PRINT_TITLE_MAX];
	char detail[KL_BACKEND_PRINT_DETAIL_MAX];
};

/* What kl_backend_print_update found changed. */
#define KL_BACKEND_PRINT_CHANGED_LIST	1U	/* the printers or the jobs */
#define KL_BACKEND_PRINT_CHANGED_RESULT	2U	/* a request was answered */

/*
 * Starts the printers: the settings file, the runtime directory the daemon
 * spools in (NULL: $XDG_RUNTIME_DIR), and the daemon's program.  Returns
 * NULL only without memory.
 */
struct kl_backend_print *kl_backend_print_open(const char *config_path, const char *runtime_dir, const char *program);

/*
 * Stops the printers: the daemon is told to end (its socket shut).
 */
void kl_backend_print_close(struct kl_backend_print *print);

/*
 * Tells whether printing can be offered: the daemon's program is there.
 */
int kl_backend_print_can(const struct kl_backend_print *print);

/*
 * Reads what the daemon said and the settings file changed by another
 * session, without waiting.  *changed has the KL_BACKEND_PRINT_CHANGED_*
 * bits.  Returns 0.
 */
int kl_backend_print_update(struct kl_backend_print *print, unsigned *changed);

/*
 * Copy up to capacity printers or jobs and return how many were copied.
 */
size_t kl_backend_print_printers(const struct kl_backend_print *print, struct kl_backend_printer *list, size_t capacity);
size_t kl_backend_print_jobs(const struct kl_backend_print *print, struct kl_backend_print_job *list, size_t capacity);

/*
 * Ask for a printer to be added, removed or made the default, a document
 * (a descriptor of a PDF, which the call takes whatever it returns) to be
 * printed (printer 0: the default), or a job to be cancelled; *request
 * numbers the answer, and a print's job is known at once.  Each returns 0
 * when asked, or EINVAL, EBUSY.
 */
int kl_backend_print_add(struct kl_backend_print *print, unsigned protocol, const char *host, unsigned port, const char *path, uint32_t *request);
int kl_backend_print_remove(struct kl_backend_print *print, uint32_t printer, uint32_t *request);
int kl_backend_print_set_default(struct kl_backend_print *print, uint32_t printer, uint32_t *request);
int kl_backend_print_submit(struct kl_backend_print *print, uint32_t printer, const char *title, int fd, uint32_t *request, uint32_t *job);
int kl_backend_print_cancel(struct kl_backend_print *print, uint32_t job, uint32_t *request);

/*
 * Takes the oldest answer: its request, its errno value (0, EINVAL, EBUSY,
 * EIO) and whether the settings file was written.  Returns 1 with one, 0
 * when none is waiting.
 */
int kl_backend_print_take_result(struct kl_backend_print *print, uint32_t *request, int *error, unsigned *saved);

/*
 * The machine's monitor (WS134 p008, plan/ws134/design.md section 1.3): what
 * the System Monitor shows, as counters that only grow (the CPUs' ticks,
 * the links' and the disks' bytes, the GPUs' busy time) and present values
 * (the memory), for the compositor to sample on a thread of its own and
 * send to the clients that ask.
 *
 * Unlike the other areas, nothing here touches struct kl_backend or calls
 * the host: the functions may be called from any thread, one monitor at a
 * time from one thread.  Each device has an id that does not change while
 * it is there and a generation that changes when it comes again; a sample's
 * entries carry the id of the info's device they belong to (not its place).
 * A field the system does not give leaves its KL_MONITOR_HAVE_* bit clear.
 */
struct kl_backend_monitor;

/* The most CPUs, GPUs, disks and links a monitor follows. */
#define KL_MONITOR_CPU_MAX		256U
#define KL_MONITOR_GPU_MAX		4U
#define KL_MONITOR_DISK_MAX		8U
#define KL_MONITOR_LINK_MAX		16U

/* What a sample gives (struct kl_backend_monitor_sample's valid). */
#define KL_MONITOR_HAVE_CPU_TIMES	0x00000001U
#define KL_MONITOR_HAVE_MEMORY		0x00000002U
#define KL_MONITOR_HAVE_SWAP		0x00000004U
#define KL_MONITOR_HAVE_LINKS		0x00000008U
#define KL_MONITOR_HAVE_DISKS		0x00000010U
#define KL_MONITOR_HAVE_GPU_BUSY	0x00000020U
#define KL_MONITOR_HAVE_GPU_MEMORY	0x00000040U
#define KL_MONITOR_HAVE_GPU_FREQ	0x00000080U
#define KL_MONITOR_HAVE_TEMPERATURE	0x00000100U
#define KL_MONITOR_HAVE_POWER		0x00000200U

/* A disk's kind (struct kl_backend_monitor_info's disk kind). */
#define KL_MONITOR_DISK_OTHER		1U
#define KL_MONITOR_DISK_NVME		2U
#define KL_MONITOR_DISK_USB		3U
#define KL_MONITOR_DISK_UAS		4U
#define KL_MONITOR_DISK_IDE		5U
#define KL_MONITOR_DISK_SDMMC		6U
#define KL_MONITOR_DISK_SCSI		7U

/* One GPU of the info: its id and generation, its name and its driver. */
struct kl_backend_monitor_gpu_info {
	uint64_t id;
	uint64_t generation;
	char name[48];
	char driver[16];
};

/* One disk of the info: its id and generation, name, kind and size in bytes (0 unknown). */
struct kl_backend_monitor_disk_info {
	uint64_t id;
	uint64_t generation;
	char name[32];
	unsigned kind;
	uint64_t size_bytes;
};

/* One link of the info: its id and generation, and its name. */
struct kl_backend_monitor_link_info {
	uint64_t id;
	uint64_t generation;
	char name[16];
};

/*
 * What does not change from sample to sample: the CPUs, the machine's name,
 * and the GPUs, the disks and the links (no loopback) with their ids and
 * generations.  generation is the set's: it changes whenever a device comes
 * or goes, so a client reads the info again when it differs.
 */
struct kl_backend_monitor_info {
	unsigned cpu_count;
	char host[64];
	uint64_t generation;
	unsigned gpu_count;
	unsigned disk_count;
	unsigned link_count;
	struct kl_backend_monitor_gpu_info gpu[KL_MONITOR_GPU_MAX];
	struct kl_backend_monitor_disk_info disk[KL_MONITOR_DISK_MAX];
	struct kl_backend_monitor_link_info link[KL_MONITOR_LINK_MAX];
};

/* One CPU's ticks since boot (cpu_hz a second). */
struct kl_backend_monitor_cpu {
	uint64_t user;
	uint64_t system;
	uint64_t idle;
	uint64_t other;
};

/* One link's bytes since it came, and whether it is up. */
struct kl_backend_monitor_link {
	uint64_t id;
	uint64_t rx_bytes;
	uint64_t tx_bytes;
	unsigned up;
};

/* One disk's work since it came (the times in nanoseconds). */
struct kl_backend_monitor_disk {
	uint64_t id;
	uint64_t read_ops;
	uint64_t write_ops;
	uint64_t read_bytes;
	uint64_t write_bytes;
	uint64_t read_ns;
	uint64_t write_ns;
	uint64_t busy_ns;
};

/* One GPU's busy time at its driver's clock, its memory and frequencies, temperature and power. */
struct kl_backend_monitor_gpu {
	uint64_t id;
	uint64_t time_ns;
	uint64_t busy_ns;
	uint64_t memory_used;
	uint64_t memory_total;
	unsigned cur_mhz;
	unsigned max_mhz;
	int milli_celsius;
	unsigned milli_watts;
};

/*
 * One sample: when (CLOCK_MONOTONIC, ns), what it gives, the counters and
 * the present values.  The memory is in bytes: total, free, the caches,
 * and the part of the caches that can be dropped at once (clean file
 * data); it is read at most every 5 seconds (reading it walks the kernel's
 * pages), so samples in between repeat the last.
 */
struct kl_backend_monitor_sample {
	uint64_t time_ns;
	uint64_t valid;
	uint64_t cpu_hz;
	unsigned cpu_count;
	struct kl_backend_monitor_cpu cpu[KL_MONITOR_CPU_MAX];
	uint64_t memory_total;
	uint64_t memory_free;
	uint64_t memory_cache;
	uint64_t memory_reclaimable;
	uint64_t swap_total;
	uint64_t swap_used;
	unsigned link_count;
	struct kl_backend_monitor_link link[KL_MONITOR_LINK_MAX];
	unsigned disk_count;
	struct kl_backend_monitor_disk disk[KL_MONITOR_DISK_MAX];
	unsigned gpu_count;
	struct kl_backend_monitor_gpu gpu[KL_MONITOR_GPU_MAX];
	int cpu_milli_celsius;
};

/*
 * Opens a monitor.  Returns NULL with errno set (ENOMEM, or ENOTSUP where
 * the system gives nothing).
 */
struct kl_backend_monitor *kl_backend_monitor_open(void);

/*
 * Reads the info.  Returns 0 or an errno value.
 */
int kl_backend_monitor_info(struct kl_backend_monitor *monitor, struct kl_backend_monitor_info *info);

/*
 * Takes a sample.  Returns 0 (with what could be read; valid says what),
 * or an errno value when nothing could.
 */
int kl_backend_monitor_sample(struct kl_backend_monitor *monitor, struct kl_backend_monitor_sample *sample);

/*
 * Closes a monitor.
 */
void kl_backend_monitor_close(struct kl_backend_monitor *monitor);

/*
 * The computer as Settings shows it (ws188-p002, plan/ws188/phase001/
 * phase.md section D4): the system's names, the file systems' sizes and
 * the people's accounts, read when a client asks (kl_system_machine_v1).
 *
 * Like the monitor, nothing here touches struct kl_backend or calls the
 * host: the compositor calls these on a thread of its own, since a file
 * system's size (a network mount) and the accounts (a directory service)
 * may wait.  Only that thread enumerates the accounts.
 */

/* The lengths of the texts, with their NULs. */
#define KL_BACKEND_MACHINE_SYSTEM	128U
#define KL_BACKEND_MACHINE_KERNEL	160U
#define KL_BACKEND_MACHINE_ARCH		32U
#define KL_BACKEND_MACHINE_PROCESSOR	64U
#define KL_BACKEND_MACHINE_HOST		64U
#define KL_BACKEND_FILESYSTEM_PATH	64U
#define KL_BACKEND_USER_NAME		64U
#define KL_BACKEND_USER_FULL_NAME	128U
#define KL_BACKEND_USER_HOME		256U
#define KL_BACKEND_MOUNT_PATH		256U
#define KL_BACKEND_MOUNT_TYPE		32U

/* The most file systems and accounts one reading gives. */
#define KL_BACKEND_FILESYSTEMS_MAX	8U
#define KL_BACKEND_USERS_MAX		64U
#define KL_BACKEND_MOUNTS_MAX		64U

/*
 * The system's names: its version's name (PRETTY_NAME of os-release), the
 * kernel's name and release, the architecture, the processor's name (empty
 * where the processor does not tell it), the computer's name, and how many
 * processors are online (0 when not known).
 */
struct kl_backend_machine {
	char system[KL_BACKEND_MACHINE_SYSTEM];
	char kernel[KL_BACKEND_MACHINE_KERNEL];
	char architecture[KL_BACKEND_MACHINE_ARCH];
	char processor[KL_BACKEND_MACHINE_PROCESSOR];
	char host[KL_BACKEND_MACHINE_HOST];
	unsigned cpus;
};

/* One file system: where it is mounted, and its sizes in bytes. */
struct kl_backend_filesystem {
	char path[KL_BACKEND_FILESYSTEM_PATH];
	uint64_t total;
	uint64_t available;
	uint64_t used;
};

/* An account's flags: a person's, the compositor's own user's, an administrator's, allowed to control Wi-Fi. */
#define KL_BACKEND_USER_PERSON		0x1U
#define KL_BACKEND_USER_SELF		0x2U
#define KL_BACKEND_USER_ADMIN		0x4U
#define KL_BACKEND_USER_NETWORK		0x8U

/*
 * One account: its name (never cut: an account whose name does not fit is
 * left out), its full name (the comment's first field, cut at a character's
 * boundary), its home (the compositor's own user's only; empty for the
 * others) and its KL_BACKEND_USER_* flags.
 */
struct kl_backend_user {
	char name[KL_BACKEND_USER_NAME];
	char full_name[KL_BACKEND_USER_FULL_NAME];
	char home[KL_BACKEND_USER_HOME];
	unsigned flags;
};

/*
 * One mounted file system a user may keep files on (ws188-p004): where it
 * is mounted and its file system's type.
 */
struct kl_backend_mount {
	char path[KL_BACKEND_MOUNT_PATH];
	char type[KL_BACKEND_MOUNT_TYPE];
};

/*
 * Reads the system's names; a value that cannot be read is left empty.
 * Returns 0.
 */
int kl_backend_machine_read(struct kl_backend_machine *machine);

/*
 * Reads the sizes of the file systems of the usual places ("/", "/home",
 * "/usr", "/var", "/tmp", "/boot"), each file system once.  Returns how
 * many were copied (at most capacity).
 */
size_t kl_backend_filesystems_read(struct kl_backend_filesystem *list, size_t capacity);

/*
 * Reads the people's accounts (a user ID from 1000 that is not nobody's,
 * with a shell that lets it log in), then the compositor's own user's when
 * it is not one of them.  Each operating system names its administrators'
 * groups.  Returns how many were copied (at most capacity; the own user's
 * always has room when capacity is not 0), and in skipped how many people
 * were left out (no room, or a name that does not fit).
 */
size_t kl_backend_users_read(struct kl_backend_user *list, size_t capacity, unsigned *skipped);

/*
 * Reads the mounted file systems (ws188-p004): every mount with files in
 * it, tmpfs and overlays included (zedBSD's root is an overlay; a user's
 * own tmpfs keeps a Trash); only the pseudo file systems without files of
 * their own (the kernel's and the devices' views, control files) are left
 * out.  The mounts outside the system's trees (/dev, /proc, /sys, /run,
 * /snap, /var/lib) come first, so a full list loses those last.  Returns 0
 * with count copied (at most capacity) and skipped left out for want of
 * room or a path too long, or an errno value when the table could not be
 * read.  Each operating system reads its own mount table (zedBSD and Linux
 * the mntent table, FreeBSD getfsstat); only the machine thread reads it
 * (getmntent's storage is static).
 */
int kl_backend_mounts_read(struct kl_backend_mount *list, size_t capacity, size_t *count, unsigned *skipped);

/*
 * The peer of a client's connection (WS135, plan/ws135/design.md section
 * 4.2): which user runs the process at the other end of a connected local
 * socket, so that the compositor shows its system extension only to its
 * own user.  Returns 0, or an errno value (the caller then treats the
 * peer as another user).
 */
int kl_backend_peer_uid(int descriptor, uid_t *uid);

/*
 * The power (ws131-p005).
 *
 * The machine's power source and the actions a user may take on the
 * machine: power it off, restart it, suspend it.  An action is asked of
 * the system's session manager (zedBSD's sessiond from the login screen or
 * a session, ws131-p027; logind on Linux) and the machine then ends or
 * sleeps.  On zedBSD sessiond's answer comes as
 * session_answer(KL_BACKEND_SESSION_POWER) (EACCES when it refused: the
 * session's user is neither root nor in wheel); on
 * Linux logind answers the call itself and the action's return value is
 * the answer.  One action is asked at a time.  The power source is read on
 * zedBSD (ws132-p003, the kernel's KERN_SYSTEM_GET_POWER); elsewhere the
 * state says unknown.  The state may be read from any thread.
 */

/* The actions (kl_backend_power_action), and their bits in the state's actions. */
#define KL_BACKEND_POWER_POWEROFF	1U
#define KL_BACKEND_POWER_REBOOT		2U
#define KL_BACKEND_POWER_SUSPEND	3U
#define KL_BACKEND_POWER_ACTION_BIT(action)	(1U << (action))

/* Where the power comes from. */
#define KL_BACKEND_POWER_SOURCE_UNKNOWN	0U
#define KL_BACKEND_POWER_SOURCE_AC	1U
#define KL_BACKEND_POWER_SOURCE_BATTERY	2U

/*
 * The power as the backend knows it: the source, the battery's charge in
 * percent (-1 when unknown), whether it charges, and the
 * KL_BACKEND_POWER_ACTION_BIT of each action a user may take now (0 when
 * none: without a session manager, or a zedBSD session of a user neither
 * root nor in wheel, ws131-p027); the lid (1 open, 0 closed, -1 unknown)
 * and whether the machine can sleep to idle (ws052-p011, zedBSD's
 * KERN_SYSTEM_POWER_FLAG_CAN_SLEEP; 0 elsewhere).
 */
struct kl_backend_power_state {
	unsigned source;
	int percent;
	unsigned charging;
	unsigned actions;
	int lid;
	unsigned can_sleep;
};

/*
 * What a sleep (kl_backend_power_action(KL_BACKEND_POWER_SUSPEND)) came to,
 * as zedBSD's sessiond answered it (ws052-p011,
 * plan/ws052/phase007/phase.md section 1.1): the kind, the error the
 * session_answer carried (0 slept; EOPNOTSUPP unsupported; the device's
 * error; EBUSY networkd or busy; ECANCELED; EIO), the wake's name as the
 * kernel's events say it ("lid", "power-button", "keyboard", "usb", "ac",
 * "timer", "spurious", "other", "none"), the device that refused, why
 * networkd could not turn the radios off, and the error of a device that
 * did not come back after the sleep (0 when every one did).
 */
#define KL_BACKEND_POWER_DEVICE_MAX	48U
#define KL_BACKEND_POWER_WAKE_MAX	16U

enum kl_backend_sleep_kind {
	KL_BACKEND_SLEEP_NONE,
	KL_BACKEND_SLEEP_SLEPT,
	KL_BACKEND_SLEEP_UNSUPPORTED,
	KL_BACKEND_SLEEP_DEVICE,
	KL_BACKEND_SLEEP_NETWORK,
	KL_BACKEND_SLEEP_CANCELLED,
	KL_BACKEND_SLEEP_BUSY,
	KL_BACKEND_SLEEP_ERROR
};

enum kl_backend_sleep_network {
	KL_BACKEND_SLEEP_NETWORK_NONE,
	KL_BACKEND_SLEEP_NETWORK_RADIO,
	KL_BACKEND_SLEEP_NETWORK_TIMEOUT,
	KL_BACKEND_SLEEP_NETWORK_CONFIRMED,
	KL_BACKEND_SLEEP_NETWORK_BUSY
};

struct kl_backend_power_outcome {
	enum kl_backend_sleep_kind kind;
	int error;
	char wake[KL_BACKEND_POWER_WAKE_MAX];
	char device[KL_BACKEND_POWER_DEVICE_MAX];
	enum kl_backend_sleep_network network;
	int resume_error;
};

/*
 * Copies the power's state.  Returns 0, or EINVAL without a backend.
 */
int kl_backend_power_get_state(const struct kl_backend *backend, struct kl_backend_power_state *state);

/*
 * Asks for an action (KL_BACKEND_POWER_*).  Returns 0 when it was asked,
 * ENOTSUP for an action not in the state's actions (a sleep: a machine
 * that cannot sleep, or no sessiond), EBUSY when one was asked already
 * (EALREADY for a sleep while a sleep is asked; EBUSY while another
 * request of sessiond's awaits its answer, to be asked again), EINVAL, or
 * the error of sending.
 */
int kl_backend_power_action(struct kl_backend *backend, unsigned action);

/*
 * Copies what the last sleep came to (kind KL_BACKEND_SLEEP_NONE before
 * one was answered).  Returns 0, EINVAL, or ENOTSUP where sleeps are not
 * answered so (Linux, FreeBSD).
 */
int kl_backend_power_outcome(const struct kl_backend *backend, struct kl_backend_power_outcome *outcome);

/*
 * Asks sessiond to stop the sleep asked for before the kernel is asked
 * (ws052-p011: a lid opened while the sleep waits for networkd); it is
 * never answered, the sleep's own answer still comes.  Returns 0, EALREADY
 * when no sleep is asked for, ENOTSUP where there is no sessiond, or the
 * error of sending.
 */
int kl_backend_power_cancel_sleep(struct kl_backend *backend);


/*
 * The backlight (ws113-p013): the brightness of the machine's built-in
 * panel, in percent (0 is the panel's lowest light, not off).  zedBSD
 * reads and sets /dev/backlight/backlight0 (the GPU driver's panel, the
 * device FreeBSD's backlight(9) also has); Linux and FreeBSD answer ENOTSUP
 * until their sysfs and backlight(9) parts are written.  Nothing is kept
 * between calls but the open device; the compositor calls these on its own
 * thread, and a call waits for the driver (briefly).
 */
struct kl_backend_backlight;

/*
 * Opens the built-in panel's backlight.  Returns 0 with *backlight, ENOENT
 * when the machine has none (no panel, or the driver does not light it),
 * EACCES when the compositor may not set it, ENOTSUP, ENOMEM, or another
 * errno value.
 */
int kl_backend_backlight_open(struct kl_backend_backlight **backlight);

/*
 * Reads the brightness into *percent.  Returns 0, EBUSY while the panel is
 * not lit by the driver (before the compositor's first frame), EINVAL, or
 * another errno value.
 */
int kl_backend_backlight_get(struct kl_backend_backlight *backlight, unsigned *percent);

/*
 * Sets the brightness (0..100).  Returns 0, EINVAL above 100, EBUSY as
 * kl_backend_backlight_get, or another errno value.
 */
int kl_backend_backlight_set(struct kl_backend_backlight *backlight, unsigned percent);

/*
 * Closes the backlight.
 */
void kl_backend_backlight_close(struct kl_backend_backlight *backlight);

/*
 * The account (ws160-p002): the password of the user the compositor runs
 * as.  zedBSD changes it with passwd (its batch mode, the passwords on a
 * pipe), which checks the current password and the system's rules and
 * writes the shadow file; Linux and FreeBSD answer ENOTSUP until the
 * desktop asks their own services.  It waits for passwd (seconds when the
 * current password is wrong), so the compositor calls it on a thread.
 * Returns 0, EACCES when current is wrong, EINVAL when fresh breaks the
 * rules (or the two passwords do not fit), ENOTSUP, or EIO.  The passwords
 * are not kept or logged.
 */
int kl_backend_account_set_password(const char *current, const char *fresh);

/*
 * The administration of the people's accounts (ws089-p026;
 * docs/architecture/security.md): zedBSD runs account-admin with the
 * caller's password and the operation's lines (kl-system-protocol.h,
 * administer) on its standard input and reads its answer; Linux and
 * FreeBSD answer ENOTSUP.  kl_backend_account_can_administer tells
 * whether the system has the tool.  kl_backend_account_administer waits
 * for it (seconds for a wrong password), so the compositor calls it on a
 * thread; it returns 0, EACCES (not an administrator, or a wrong
 * password), EINVAL (another refusal), ENOTSUP, or EIO, with the tool's
 * word of a refusal in reason (empty otherwise).  The password is not kept
 * or logged.
 */
int kl_backend_account_can_administer(void);
int kl_backend_account_administer(const char *password, const char *operation, char *reason, size_t size);


/*
 * The session (ws131-p006): the session manager that started the
 * compositor, and the hand-over of the display between the login screen
 * and a session.
 *
 * On zedBSD that is sessiond (plan/ws035/login-manager-design.md): the
 * login screen asks it to log a user in on its descriptor, a session asks
 * it to log out or to unlock its lock screen on its own descriptor, and
 * both say when they first take the display and when they have given it
 * back.  Elsewhere no session manager speaks to the compositor yet: every
 * call answers ENOTSUP and kl_backend_session_managed is 0.
 *
 * The answers and the end come through the host's callbacks, from
 * kl_backend_tick: session_answer(request, error) for the request asked
 * last (KL_BACKEND_SESSION_NONE for a line no request asked for), error 0
 * when it was granted, EACCES when refused (a wrong password), EIO when
 * the manager could not do it, EPROTO for a line not understood; and
 * session_stop(reason) when the compositor is to end.  For
 * KL_BACKEND_SESSION_QUIT and KL_BACKEND_SESSION_ENDED the compositor gives
 * the display back (its swapchain and lease) inside the callback, and the
 * backend tells the manager so when the callback returns.
 */

/* The requests a session_answer answers. */
#define KL_BACKEND_SESSION_NONE		0U
#define KL_BACKEND_SESSION_AUTH		1U	/* the login screen's log in */
#define KL_BACKEND_SESSION_UNLOCK	2U	/* a session's lock screen */
#define KL_BACKEND_SESSION_POWER	3U	/* kl_backend_power_action */
#define KL_BACKEND_SESSION_SERVICE	4U	/* kl_backend_sharing_request (ws089-p025) */
#define KL_BACKEND_SESSION_STYLES	5U	/* kl_backend_session_styles (ws172-p002) */
#define KL_BACKEND_SESSION_ENROLL	6U	/* kl_backend_session_set_pin (ws172-p002) */
#define KL_BACKEND_SESSION_ENROLLED	7U	/* kl_backend_session_enrolled (ws172-p002) */
#define KL_BACKEND_SESSION_TOUCH	8U	/* not an answer: a security key waits to be touched (ws172-p003) */

/*
 * The ways to log in or unlock (ws172-p002, docs/architecture/security.md
 * "Login authentication"): the password, the PIN, a security key.
 */
#define KL_BACKEND_STYLE_PASSWORD	0x1U
#define KL_BACKEND_STYLE_PIN		0x2U
#define KL_BACKEND_STYLE_KEY		0x4U

/* The longest reason word of a refusal, with its end. */
#define KL_BACKEND_SESSION_REASON	24U

/*
 * A registered security key as ENROLLED lists it (ws172-p003): its
 * reference (16 hexadecimal digits, what kl_backend_session_remove_key
 * takes) and its label, and the most an account has.
 */
#define KL_BACKEND_KEYS_MAX		5U
#define KL_BACKEND_KEY_REF		17U
#define KL_BACKEND_KEY_LABEL		33U
struct kl_backend_key {
	char ref[KL_BACKEND_KEY_REF];
	char label[KL_BACKEND_KEY_LABEL];
};

/* Why session_stop is called. */
#define KL_BACKEND_SESSION_QUIT		1U	/* the session's Log Out was answered: end */
#define KL_BACKEND_SESSION_ENDED	2U	/* the login screen's manager is done with it (a session is ready, or the manager went) */
#define KL_BACKEND_SESSION_UNANSWERED	3U	/* a Log Out had no answer in time: end anyway */
#define KL_BACKEND_SESSION_LOST		4U	/* the seat's authority failed or went away: end through the ordinary cleanup */

/*
 * Says the compositor is about to take the display for the first time,
 * and waits (at most 20 seconds) for the manager to let it: 0 when it did,
 * ETIMEDOUT when the wait ended without it (the display is taken anyway),
 * ENOTSUP without a session manager, or the error of saying so.  An answer
 * that comes before the manager's word is kept, and the next
 * kl_backend_tick gives it.
 */
int kl_backend_session_ready(struct kl_backend *backend);

/*
 * Asks the manager to log the session out.  Returns 0 when asked (the
 * answer is session_stop(KL_BACKEND_SESSION_QUIT), or
 * KL_BACKEND_SESSION_UNANSWERED after 30 seconds), ENOTSUP when no manager
 * started the session (the compositor simply ends), or the error of
 * asking.
 */
int kl_backend_session_logout(struct kl_backend *backend);

/*
 * Asks the manager to log user in with secret in style (a
 * KL_BACKEND_STYLE_*: the password, or the PIN) on the login screen.
 * Returns 0 when asked, EBUSY while another request waits for its answer,
 * ENOTSUP, EINVAL, or the error of asking.  Nothing of the secret is kept.
 * A refusal answers EACCES, and kl_backend_session_reason gives its word
 * (bad-secret, locked, pin-off, timeout, ...).
 */
int kl_backend_session_authenticate(struct kl_backend *backend, const char *user, unsigned style, const char *secret);

/*
 * Asks the manager to unlock the session's lock screen with secret in
 * style.  Returns as kl_backend_session_authenticate.
 */
int kl_backend_session_unlock(struct kl_backend *backend, unsigned style, const char *secret);

/*
 * Asks the manager which styles user (the login screen), or the session's
 * own user (user NULL, a session), may use now; the answer is
 * session_answer(KL_BACKEND_SESSION_STYLES, 0), and
 * kl_backend_session_styles_get then gives the KL_BACKEND_STYLE_* bits
 * (the password alone until an answer came).  Returns as
 * kl_backend_session_authenticate.
 */
int kl_backend_session_styles(struct kl_backend *backend, const char *user);
unsigned kl_backend_session_styles_get(const struct kl_backend *backend);

/*
 * Sets the session user's PIN (six digits), or removes it when pin is
 * empty, checked by the user's password (a session).  The answer is
 * session_answer(KL_BACKEND_SESSION_ENROLL, error): 0, EACCES (a wrong
 * password, or another refusal: kl_backend_session_reason), EIO.  Returns
 * as kl_backend_session_authenticate.
 */
int kl_backend_session_set_pin(struct kl_backend *backend, const char *password, const char *pin);

/*
 * Asks the manager what the session user has enrolled; the answer is
 * session_answer(KL_BACKEND_SESSION_ENROLLED, 0), and
 * kl_backend_session_enrolled_get then gives whether a PIN is set and how
 * many security keys are registered (0 and 0 until an answer came).
 */
int kl_backend_session_enrolled(struct kl_backend *backend);
void kl_backend_session_enrolled_get(const struct kl_backend *backend, unsigned *pin, unsigned *keys);

/*
 * Gives the security keys of the last ENROLLED answer (ws172-p003): at
 * most capacity of them in keys; returns how many there are.
 */
size_t kl_backend_session_keys_get(const struct kl_backend *backend, struct kl_backend_key *keys, size_t capacity);

/*
 * Registers the security key that is plugged in for the session user
 * (ENROLL fido2), with its label (1 to 32 bytes, no control character and
 * no colon) and the key's own PIN, checked by the user's password.  While
 * the key waits to be touched, session_answer(KL_BACKEND_SESSION_TOUCH, 0)
 * comes; the answer is session_answer(KL_BACKEND_SESSION_ENROLL, error) as
 * kl_backend_session_set_pin's.  Returns as kl_backend_session_authenticate.
 */
int kl_backend_session_add_key(struct kl_backend *backend, const char *password, const char *label, const char *pin);

/*
 * Removes one of the session user's keys by its reference (REMOVE fido2),
 * checked by the user's password.  Answered as kl_backend_session_set_pin.
 */
int kl_backend_session_remove_key(struct kl_backend *backend, const char *password, const char *ref);

/*
 * Stops a security key's attempt under way (CANCEL): its answer is a
 * refusal (timeout).  Returns 0 when said, ENOTSUP, or the error of saying.
 */
int kl_backend_session_cancel(struct kl_backend *backend);

/*
 * The word of the last refusal (FAIL reason), empty when it had none.
 */
const char *kl_backend_session_reason(const struct kl_backend *backend);

/*
 * Tells whether a session manager started this session and still listens
 * (1), so that it can be locked and logged out through it, or not (0).
 */
int kl_backend_session_managed(const struct kl_backend *backend);


/*
 * The seat (ws131-p006): who may use the display and the input devices.
 *
 * Linux takes them through logind (the session's TakeDevice) or directly as
 * root, FreeBSD through seatd (libseat); zedBSD's compositor uses its own
 * kernel interfaces and no seat (every call answers ENOTSUP, and the
 * compositor does not open it).  On Linux the seat also puts a virtual
 * terminal on standard input in graphics mode while it is held.  kl_backend_seat_open takes the seat and
 * the primary display node before Vulkan opens; the input devices are
 * opened through the seat, one descriptor each, and the seat may take them
 * all away while another session has the display (a virtual terminal
 * switch) and give them back later.
 *
 * The callbacks come from kl_backend_poll_done:
 *   session_paused    stop drawing and close the output now; the seat is
 *                     told the compositor has let go when it returns
 *   session_resumed   the display may be opened again (the next frame)
 *   input_paused      stop reading the input of path; its descriptor stays
 *                     the seat's and is not closed
 *   input_resumed     read the input of path from descriptor from now on
 *                     (the old one is the seat's to close)
 *   input_gone        the input of path is gone; forget it without closing
 *                     its descriptor, which the seat has closed
 * A failed authority calls session_stop(KL_BACKEND_SESSION_LOST).
 *
 * The input devices' scan (keiland-backend-evdev.h, ws131-p007) asks
 * input_known(path) (1 when the compositor already reads the device) and
 * offers each new device with input_found(descriptor, path, caps), which
 * returns 1 when the compositor keeps it (it then owns the descriptor
 * until kl_backend_input_close) and 0 when the backend is to close it.
 */

/*
 * Takes the seat and the primary display node (KEILAND_DRM_DEVICE, or
 * /dev/dri/card0).  Returns 0, ENOTSUP where there is no seat, or an errno
 * value; kl_backend_seat_close is called after a failure too.
 */
int kl_backend_seat_open(struct kl_backend *backend);

/*
 * Returns every device and the seat (partial opens too).
 */
void kl_backend_seat_close(struct kl_backend *backend);

/*
 * The primary display node's descriptor (-1 while it is paused or not
 * taken) and path, for the display's acquisition.
 */
int kl_backend_seat_primary_fd(const struct kl_backend *backend);
const char *kl_backend_seat_primary_path(const struct kl_backend *backend);

/*
 * Tells whether the seat is paused (1: no drawing, no new input device).
 */
int kl_backend_seat_paused(const struct kl_backend *backend);

/*
 * Opens the input device at path through the seat: a nonblocking
 * descriptor, or -1 with errno (EAGAIN while paused).
 */
int kl_backend_seat_device_open(struct kl_backend *backend, const char *path);

/*
 * Returns an input device's descriptor to the seat.
 */
void kl_backend_seat_device_close(struct kl_backend *backend, int descriptor);

/*
 * Tells the seat that reading descriptor failed as revoked (ENODEV).
 * Returns 1 when the seat keeps the device for a later resume (the
 * compositor stops reading it and waits for input_resumed or input_gone),
 * 0 when the compositor closes it as usual.
 */
int kl_backend_seat_device_revoked(struct kl_backend *backend, int descriptor);

/*
 * Sharing (ws089-p025): the Remote Login (sshd) the Sharing page turns on
 * and off.  On zedBSD the session's manager does it (sessiond's SERVICE,
 * root or a member of wheel), on the session's descriptor like the lock
 * screen's requests; its answer comes as
 * session_answer(KL_BACKEND_SESSION_SERVICE, error): 0 with the state
 * (kl_backend_sharing_get), EPERM for a user who may not, EIO when it
 * failed.  Elsewhere it is not supported (ENOTSUP).
 */
#define KL_BACKEND_SHARING_STATUS	0U
#define KL_BACKEND_SHARING_ON		1U
#define KL_BACKEND_SHARING_OFF		2U

/* The longest host key fingerprint ("SHA256:" and 43 characters of base64, and its end). */
#define KL_BACKEND_SHARING_FINGERPRINT	64U

/*
 * Remote Login's state: whether the system has it at all, whether it
 * starts with the system, whether it runs, the port, whether this user may
 * change it (root or wheel), whether the state was ever read, and the
 * host key's fingerprint (empty when there is none).
 */
struct kl_backend_sharing {
	unsigned available;
	unsigned enabled;
	unsigned running;
	unsigned port;
	unsigned allowed;
	unsigned known;
	char fingerprint[KL_BACKEND_SHARING_FINGERPRINT];
};

/* Asks for Remote Login's state, or to turn it on or off (KL_BACKEND_SHARING_*). Returns 0, EBUSY, ENOTSUP, EINVAL. */
int kl_backend_sharing_request(struct kl_backend *backend, unsigned action);

/* Copies Remote Login's state as last answered. */
void kl_backend_sharing_get(const struct kl_backend *backend, struct kl_backend_sharing *state);

#endif
