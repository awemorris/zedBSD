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
	int (*input_paused)(void *data, const char *path);
	int (*input_resumed)(void *data, const char *path, int descriptor);
	int (*input_gone)(void *data, const char *path);
	int (*input_known)(void *data, const char *path);
	int (*input_found)(void *data, int descriptor, const char *path, const struct kl_backend_input_caps *caps);
	void (*input_changed)(void *data);
	void (*power_changed)(void *data);
	void (*power_button)(void *data, unsigned button);
	void (*lid_changed)(void *data, unsigned open);
	void (*keys_changed)(void *data);
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
 * The compositor's threads, lent to the backend's own.
 *
 * wake_descriptor is the write end of the pipe the compositor's event loop
 * polls: a thread of the backend writes one byte to it when the event
 * loop's next update has something to take (-1 when there is none; the
 * updates then find it on their own).  start makes a thread with every
 * signal blocked, under a name the compositor reports with its own, and
 * gives its handle (0, or an errno value and no thread); the backend joins
 * the thread itself and then calls ended with the same name.  Without
 * start the backend makes its threads with pthread_create.  The
 * descriptor and the functions stay valid until kl_backend_close.
 *
 * The backend's threads never call a callback of struct kl_backend_host;
 * the call that takes what one of them found (the area's update) is the
 * event loop's, as before.
 */
struct kl_backend_threads {
	int wake_descriptor;
	int (*start)(const char *name, void *(*run)(void *), void *argument, pthread_t *thread);
	void (*ended)(const char *name);
};

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
 * threads are the compositor's threads lent to the backend's own.
 */
struct kl_backend_options {
	unsigned flags;
	int greeter_descriptor;
	int session_descriptor;
	struct kl_backend_threads threads;
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
 * its hardware address and MTU, the bytes it has received and sent, its
 * link's speed in Mb/s (0 while not known, BUG-222), and whether it is a
 * radio (Wi-Fi), as the system tells it (BUG-284: not by its name).
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
	unsigned wireless;
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
 * none (an OSS mixer is read by the periodic updates; zedBSD's audiod
 * connection is read by a thread of the backend's, which wakes the event
 * loop through the lent wake pipe).
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
 * EINVAL (out of range) or the error of sending (on zedBSD the thread
 * sends it later: a failed send drops the connection, which an update
 * reports as KL_BACKEND_AUDIO_CHANGED_REACHABLE).
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
 * The sound's playback streams (WS191, plan/ws191/design.md).
 *
 * A stream is a ring of frames in shared memory that the backend makes and
 * reads, and that the compositor hands to a client to write into: audiod's
 * shared memory on zedBSD, and on Linux and FreeBSD a sealed memfd a pump
 * thread of the backend's carries to ALSA (alsa-lib, opened with dlopen)
 * or OSS.  Nothing here waits: an open, a control and a close only start
 * their work, and kl_backend_audio_stream_next hands over what came of it.
 */
struct kl_backend_audio_stream;

/*
 * The first page of a stream's memory: what the frames are, and the
 * positions, each writer's on its own cache line.  A position is a count of
 * frames that only grows, read with an 8-byte acquire load and written with
 * an 8-byte release store by its one writer; the write and read sequence
 * words are not used (audiod's, for systems without an 8-byte atomic).  The
 * played position and its time are one pair under played_sequence, a
 * sequence lock: the writer makes it odd, writes the two, and makes it even
 * again; a reader reads again until it finds the same even value before and
 * after (as audiod writes them).  libkeiland's
 * kl-audio-protocol.h gives the same layout as offsets; the compositor
 * checks that the two agree.
 */
struct kl_backend_audio_ring {
	uint32_t tag;
	uint32_t version;
	uint32_t format;
	uint32_t channels;
	uint32_t rate;
	uint32_t frame_bytes;
	uint32_t capacity_frames;
	uint32_t period_frames;
	_Alignas(64) uint64_t write_position;
	uint32_t write_sequence;
	_Alignas(64) uint64_t read_position;
	uint32_t read_sequence;
	_Alignas(64) uint64_t played_position;
	int64_t played_time_ns;
	uint32_t played_sequence;
	uint32_t underruns;
	uint32_t overruns;
	uint32_t state;
};

/* The ring's page, its version, and its state's values. */
#define KL_BACKEND_AUDIO_RING_HEADER		4096U
#define KL_BACKEND_AUDIO_RING_VERSION		1U
#define KL_BACKEND_AUDIO_STATE_STOPPED		0U
#define KL_BACKEND_AUDIO_STATE_RUNNING		1U
#define KL_BACKEND_AUDIO_STATE_DRAINING		2U

/* The sample formats. */
#define KL_BACKEND_AUDIO_FORMAT_S16_LE		1U
#define KL_BACKEND_AUDIO_FORMAT_S32_LE		2U
#define KL_BACKEND_AUDIO_FORMAT_F32_LE		3U

/* What a stream is asked to be (checked by the compositor before it opens one). */
struct kl_backend_audio_stream_format {
	unsigned format;	/* KL_BACKEND_AUDIO_FORMAT_* */
	unsigned channels;	/* 1 or 2 */
	unsigned rate;		/* 8000 to 192000 */
	unsigned buffer_frames;	/* the ring's frames, 0 for the backend's choice */
	unsigned period_frames;	/* 0 for the backend's choice */
};

/* The controls. */
#define KL_BACKEND_AUDIO_START			1U
#define KL_BACKEND_AUDIO_STOP			2U
#define KL_BACKEND_AUDIO_FLUSH			3U
#define KL_BACKEND_AUDIO_DRAIN			4U

/* What came of a stream: kl_backend_audio_stream_report's what. */
#define KL_BACKEND_AUDIO_READY			1U	/* fd, bytes, capacity_frames, period_frames */
#define KL_BACKEND_AUDIO_FAILED			2U	/* error; nothing follows */
#define KL_BACKEND_AUDIO_RESULT			3U	/* request, error */
#define KL_BACKEND_AUDIO_DRAINED		4U	/* request */
#define KL_BACKEND_AUDIO_UNDERRUN		5U	/* count, the total so far */
#define KL_BACKEND_AUDIO_LOST			6U	/* error; nothing follows */

/* The errors (the values of kl-audio-protocol.h's KL_AUDIO_ERROR_*, which the compositor checks). */
#define KL_BACKEND_AUDIO_ERROR_NONE		0U
#define KL_BACKEND_AUDIO_ERROR_INVALID		1U
#define KL_BACKEND_AUDIO_ERROR_NO_DEVICE	2U
#define KL_BACKEND_AUDIO_ERROR_UNAVAILABLE	3U
#define KL_BACKEND_AUDIO_ERROR_UNSUPPORTED	4U
#define KL_BACKEND_AUDIO_ERROR_NO_MEMORY	5U
#define KL_BACKEND_AUDIO_ERROR_TOO_MANY		6U
#define KL_BACKEND_AUDIO_ERROR_STATE		7U
#define KL_BACKEND_AUDIO_ERROR_GONE		8U
#define KL_BACKEND_AUDIO_ERROR_BROKEN		9U
#define KL_BACKEND_AUDIO_ERROR_FAILED		10U

/* One thing that came of a stream. */
struct kl_backend_audio_stream_report {
	unsigned what;			/* KL_BACKEND_AUDIO_READY ... _LOST */
	unsigned error;			/* KL_BACKEND_AUDIO_ERROR_* */
	uint32_t request;		/* RESULT and DRAINED: the control's request */
	uint32_t count;			/* UNDERRUN */
	int fd;				/* READY: the ring's memory, now the caller's to close */
	uint32_t bytes;			/* READY: the memory's size */
	uint32_t capacity_frames;	/* READY */
	uint32_t period_frames;		/* READY: the device's period, 0 when not known (the client does not use it) */
};

/*
 * Tells whether this backend can make streams: 1 when it can, 0 when it
 * cannot (no such code, or Linux without alsa-lib).
 */
int kl_backend_audio_stream_supported(void);

/*
 * Starts making a stream; READY or FAILED comes through
 * kl_backend_audio_stream_next.  Returns NULL with errno (ENOMEM) when it
 * could not even start.
 */
struct kl_backend_audio_stream *kl_backend_audio_stream_open(const struct kl_backend_audio_stream_format *format);

/*
 * Sends a control (KL_BACKEND_AUDIO_START ... _DRAIN) with the client's
 * request number; its RESULT comes through kl_backend_audio_stream_next.
 * Returns 0, ENOTCONN (not ready, or ended) or EAGAIN (the service's queue
 * is full).
 */
int kl_backend_audio_stream_control(struct kl_backend_audio_stream *stream, unsigned what, uint32_t request);

/*
 * Takes the next thing that came of the stream without waiting.  Returns
 * 1 with *report filled, or 0 when nothing more has come.
 */
int kl_backend_audio_stream_next(struct kl_backend_audio_stream *stream, struct kl_backend_audio_stream_report *report);

/*
 * Ends a stream without waiting (a pump thread is let go and joined later);
 * a ring handed over in a READY report not taken yet is closed with it.
 */
void kl_backend_audio_stream_close(struct kl_backend_audio_stream *stream);

/*
 * Lets every ended pump thread go (the compositor calls it each pass).
 */
void kl_backend_audio_stream_reap(void);

/*
 * Waits for every pump thread to end and lets it go (at the compositor's
 * end, after every stream was closed).
 */
void kl_backend_audio_stream_reap_all(void);

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
 * zedBSD's is bluetoothd's socket, Linux's BlueZ's org.bluez on the
 * system bus (ws143-p007); on FreeBSD the state is unreachable (D7).
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
#define KL_BACKEND_BT_PAIR_PHONE	7U	/* pair a phone as this user's phone (bluetoothd's PAIR ... phone=1, ws197-p004c) */

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
 * Takes the oldest answer: its id, its errno value (0, EACCES for a caller
 * not permitted, EBUSY, ETIMEDOUT for a device that did not answer,
 * ECONNREFUSED for a pairing or a connection the device or the user
 * refused, ENOTSUP for what this computer cannot do with the device,
 * ENETDOWN while off, EIO) and the service's reason in words.  Returns 1
 * with one, 0 when none waits.
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
 * The phone (ws197-p004a, plan/ws197/phase004/phase.md section 5.3): the
 * user's paired phone's messages as bluetoothd relays them (MAP), the
 * phone link's state, and the requests of the desktop: a page of the
 * synchronisation, a message marked read on the phone, a text sent, the
 * phone's switch.  Nothing here keeps a message: an item lives until the
 * next take.  zedBSD's is bluetoothd's socket (three connections: the
 * events, the synchronisation and the other requests); elsewhere every
 * request is refused with ENOTSUP.
 *
 * Nothing here waits: kl_backend_phone_update reads what has arrived,
 * writes what waits and starts what is due.  The answers come later as
 * results, numbered by the id each request gave.
 */
struct kl_backend_phone;

/* What kl_backend_phone_update found changed (bits). */
#define KL_BACKEND_PHONE_CHANGED_STATE		1U	/* the state changed */
#define KL_BACKEND_PHONE_CHANGED_ITEM		2U	/* an item came */
#define KL_BACKEND_PHONE_CHANGED_RESULT		4U	/* a request's result came */
#define KL_BACKEND_PHONE_CHANGED_SENT		8U	/* a sent message's state came on the events */
#define KL_BACKEND_PHONE_CHANGED_DROPPED	16U	/* bluetoothd or this dropped an event (synchronise again) */
#define KL_BACKEND_PHONE_CHANGED_CALL		32U	/* a call's state came (ws197-p006) */
#define KL_BACKEND_PHONE_CHANGED_HF		64U	/* the hands-free unit's state came (ws197-p006) */

/*
 * The calls (ws197-p006, HFP): a call's state (keiland.h's KL_PHONE_CALL_*
 * have the same values), the hands-free unit's state, and what the owner
 * asks of a call (keiland.h's KL_PHONE_ASK_*).
 */
#define KL_BACKEND_PHONE_CALL_INCOMING	1U
#define KL_BACKEND_PHONE_CALL_DIALING	2U
#define KL_BACKEND_PHONE_CALL_ALERTING	3U
#define KL_BACKEND_PHONE_CALL_ACTIVE	4U
#define KL_BACKEND_PHONE_CALL_HELD	5U
#define KL_BACKEND_PHONE_CALL_WAITING	6U
#define KL_BACKEND_PHONE_CALL_ENDED	7U
#define KL_BACKEND_PHONE_HF_OFF		0U
#define KL_BACKEND_PHONE_HF_CONNECTING	1U
#define KL_BACKEND_PHONE_HF_READY	2U
#define KL_BACKEND_PHONE_HF_FAILED	3U
#define KL_BACKEND_PHONE_ASK_DIAL	1U
#define KL_BACKEND_PHONE_ASK_ANSWER	2U
#define KL_BACKEND_PHONE_ASK_HANGUP	3U
#define KL_BACKEND_PHONE_ASK_HOLD	4U
#define KL_BACKEND_PHONE_ASK_DTMF	5U
#define KL_BACKEND_PHONE_ASK_VOLUME	6U
#define KL_BACKEND_PHONE_ASK_VOICE	7U

/* A sent message's state (keiland.h's KL_PHONE_SENT, _DELIVERED and _FAILED have the same values). */
#define KL_BACKEND_PHONE_SENT		1U
#define KL_BACKEND_PHONE_DELIVERED	2U
#define KL_BACKEND_PHONE_FAILED		3U

/* The state of the messages on the phone link. */
#define KL_BACKEND_PHONE_MESSAGES_OFF		0U
#define KL_BACKEND_PHONE_MESSAGES_CONNECTING	1U
#define KL_BACKEND_PHONE_MESSAGES_READY		2U
#define KL_BACKEND_PHONE_MESSAGES_FAILED	3U

/* The profiles of the phone's switch (bits). */
#define KL_BACKEND_PHONE_PROFILE_MESSAGES	1U
#define KL_BACKEND_PHONE_PROFILE_CONTACTS	2U
#define KL_BACKEND_PHONE_PROFILE_CALLS		4U

/* What a page asks for: the messages (MAP), the phone's contacts and its calls' history (PBAP, ws197-p005). */
#define KL_BACKEND_PHONE_WHAT_MESSAGES		0U
#define KL_BACKEND_PHONE_WHAT_CONTACTS		1U
#define KL_BACKEND_PHONE_WHAT_CALLS		2U

/* A call's kind, in a call's item's folder. */
#define KL_BACKEND_PHONE_CALL_RECEIVED		0U
#define KL_BACKEND_PHONE_CALL_DIALED		1U
#define KL_BACKEND_PHONE_CALL_MISSED		2U

/* An item's folder, direction and where its time came from. */
#define KL_BACKEND_PHONE_FOLDER_INBOX		0U
#define KL_BACKEND_PHONE_FOLDER_SENT		1U

/* A messages-only folder flag: the payload is decoded MMS plain text. */
#define KL_BACKEND_PHONE_FOLDER_MMS		0x100U
#define KL_BACKEND_PHONE_DIRECTION_IN		0U
#define KL_BACKEND_PHONE_DIRECTION_OUT		1U
#define KL_BACKEND_PHONE_ZONE_PHONE		0U
#define KL_BACKEND_PHONE_ZONE_MSE		1U
#define KL_BACKEND_PHONE_ZONE_LOCAL		2U
#define KL_BACKEND_PHONE_ZONE_RECEIVED		3U

/* The lengths of an item's texts with their NULs, the longest text and the longest text sent. */
#define KL_BACKEND_PHONE_HANDLE_MAX	32U
#define KL_BACKEND_PHONE_KEY_MAX	20U
#define KL_BACKEND_PHONE_DATETIME_MAX	24U
#define KL_BACKEND_PHONE_PEER_MAX	132U
#define KL_BACKEND_PHONE_CURSOR_MAX	64U
#define KL_BACKEND_PHONE_NUMBER_MAX	33U
#define KL_BACKEND_PHONE_TEXT_MAX	16384U
#define KL_BACKEND_PHONE_MIME_MAX (16U * 1024U * 1024U)
#define KL_BACKEND_PHONE_SEND_MAX	8192U

/* The most items a page has, and the most a synchronisation takes from a folder (0: no limit). */
#define KL_BACKEND_PHONE_PAGE_MAX	32U
#define KL_BACKEND_PHONE_LIMIT_MAX	500U

/*
 * The state as last read: whether bluetoothd answers, whether this
 * follows the phone's events (the owner does), whether there is a phone
 * record, whether the phone link is ready, the messages' state
 * (KL_BACKEND_PHONE_MESSAGES_*), whether a text can be sent, whether new
 * messages are told, whether the owner is at the seat, the phone's switch
 * and its profiles (KL_BACKEND_PHONE_PROFILE_*), the phone's address, and
 * why the link or the messages stopped (a word of section 3.5, or empty).
 *
 * ws197-p005: the contacts' state (PBAP, the values of the messages'),
 * why they stopped (a word, or empty), and whether have_record is known:
 * record_known is 0 while bluetoothd does not answer and from the moment
 * it answers again until SHOW or the events have said whether there is a
 * record (have_record 0 then means no more than "not read yet").
 */
struct kl_backend_phone_state {
	unsigned reachable;
	unsigned subscribed;
	unsigned have_record;
	unsigned linked;
	unsigned messages;
	unsigned can_send;
	unsigned notify;
	unsigned present;
	unsigned enabled;
	unsigned profiles;
	char address[KL_BACKEND_BT_ADDRESS_MAX];
	char why[KL_BACKEND_BT_REASON_MAX];
	unsigned contacts;
	unsigned record_known;
	char contacts_why[KL_BACKEND_BT_REASON_MAX];
};

/*
 * One item of what a page asks for (KL_BACKEND_PHONE_WHAT_*), and the
 * page's id (0 for a message that came by itself).
 *
 * A message: its handle for kl_backend_phone_read (good for the phone's
 * session only), its key (16 hexadecimal digits, the same across
 * sessions, or "-" when it has none), folder, direction, time (UNIX
 * seconds) and where that came from, the phone's datetime as written, the
 * other side's number and name, whether it is read, has no key, or was
 * cut, and its text (UTF-8 ended by a NUL, in the backend's buffer until
 * the next take).
 *
 * A contact (ws197-p005): its key, how many numbers it has (folder), its
 * first number (peer), its name, and its reduced vCard 3.0 as the text.
 * A call: its key, its kind (folder, KL_BACKEND_PHONE_CALL_*), out for a
 * call dialed (direction), its time and where that came from (phone,
 * local, or received when the phone gave none: partial), the phone's
 * datetime, the number and the name, and no text.
 */
struct kl_backend_phone_item {
	unsigned what;
	uint32_t id;
	char handle[KL_BACKEND_PHONE_HANDLE_MAX];
	char key[KL_BACKEND_PHONE_KEY_MAX];
	unsigned folder;
	unsigned direction;
	int64_t time;
	unsigned zone;
	char datetime[KL_BACKEND_PHONE_DATETIME_MAX];
	char peer[KL_BACKEND_PHONE_PEER_MAX];
	char name[KL_BACKEND_PHONE_PEER_MAX];
	unsigned read;
	unsigned partial;
	unsigned truncated;
	const char *text;
	size_t length;
	/* The backend owns this MIME descriptor until the next take or close. */
	unsigned has_mime;
	int mime_descriptor;
};

/*
 * A request's result: its id, its errno value (section 3.5), and for a
 * page where the next starts, whether more follow, how many items came,
 * how many were skipped and capped's bits (1 a limit stopped it; for the
 * contacts, ws197-p005: 2 the phone's count changed while it was read, 4
 * a card did not end, so the count is not to be trusted); for a text
 * sent, bluetoothd's number for its later states.
 */
struct kl_backend_phone_result {
	uint32_t id;
	int error;
	char cursor[KL_BACKEND_PHONE_CURSOR_MAX];
	unsigned more;
	unsigned count;
	unsigned skipped;
	unsigned capped;
	uint32_t sent_request;
};

/* Starts following the phone.  Returns NULL only without memory (elsewhere a phone that refuses everything). */
struct kl_backend_phone *kl_backend_phone_open(void);

/* Stops following; the requests going on are dropped (the caller answers them). */
void kl_backend_phone_close(struct kl_backend_phone *phone);

/* Reads and writes without waiting; *changed has the KL_BACKEND_PHONE_CHANGED_* bits.  Returns 0 or EINVAL. */
int kl_backend_phone_update(struct kl_backend_phone *phone, unsigned *changed);

/* Copies the state as last read. */
void kl_backend_phone_get_state(const struct kl_backend_phone *phone, struct kl_backend_phone_state *state);

/*
 * Asks a page of the synchronisation: what (KL_BACKEND_PHONE_WHAT_*), the
 * messages or calls since a time (not for the contacts), at most limit a
 * folder (the messages only, 0: all), from a cursor (empty: the start),
 * count items (1 to KL_BACKEND_PHONE_PAGE_MAX).  The items come numbered
 * *id, then the result.  Returns 0, ENOTCONN without bluetoothd, ENOTSUP,
 * EINVAL, or EBUSY when too many wait.
 */
int kl_backend_phone_page(struct kl_backend_phone *phone, unsigned what, int64_t since, unsigned limit, const char *cursor, unsigned count, uint32_t *id);

/* Marks a message read on the phone by its handle.  Returns as kl_backend_phone_page. */
int kl_backend_phone_read(struct kl_backend_phone *phone, const char *handle, uint32_t *id);

/* Sends a text (1 to KL_BACKEND_PHONE_SEND_MAX bytes, no NUL) to a number ([0-9+*#], 1 to 32).  Returns as kl_backend_phone_page. */
int kl_backend_phone_send(struct kl_backend_phone *phone, const char *to, const uint8_t *text, size_t length, uint32_t *id);

/*
 * Sends an MMS (ws197-p010): its MIME body, the whole of a regular file the
 * caller keeps open (1 to KL_BACKEND_PHONE_MIME_MAX bytes, read from its
 * start), to a number, answered and followed as kl_backend_phone_send.
 * Returns as kl_backend_phone_page, and EMSGSIZE for a body too long.
 */
int kl_backend_phone_send_mms(struct kl_backend_phone *phone, const char *to, int descriptor, uint32_t *id);

/* Turns the phone's switch on with its profiles (KL_BACKEND_PHONE_PROFILE_*), or off keeping them.  Returns as kl_backend_phone_page. */
int kl_backend_phone_link_set(struct kl_backend_phone *phone, const char *address, unsigned on, unsigned profiles, uint32_t *id);

/* Takes the oldest item.  Returns 1 with one, 0 when none waits. */
int kl_backend_phone_take_item(struct kl_backend_phone *phone, struct kl_backend_phone_item *item);

/* Takes the oldest result.  Returns 1 with one, 0 when none waits. */
int kl_backend_phone_take_result(struct kl_backend_phone *phone, struct kl_backend_phone_result *result);

/* Takes the oldest state of a sent text (bluetoothd's number, KL_BACKEND_PHONE_SENT and the others).  Returns 1 or 0. */
int kl_backend_phone_take_sent(struct kl_backend_phone *phone, uint32_t *sent_request, unsigned *state);

/* Reads the record and follows the events again at once (after a pairing of the phone or a forget). */
void kl_backend_phone_refresh(struct kl_backend_phone *phone);

/*
 * One change of a call (ws197-p006): the phone's index of it, its state
 * (KL_BACKEND_PHONE_CALL_*, ENDED once when it ends), whether it was
 * dialed here or there, the number and the name the phone gave (UTF-8,
 * possibly empty).
 */
struct kl_backend_phone_call {
	unsigned index;
	unsigned state;
	unsigned outgoing;
	char number[KL_BACKEND_PHONE_PEER_MAX];
	char name[KL_BACKEND_PHONE_PEER_MAX];
};

/*
 * The hands-free unit's state (ws197-p006): KL_BACKEND_PHONE_HF_*, the
 * phone's network service (0 or 1), signal (0 to 5), battery (0 to 5),
 * roaming (0 or 1), the speaker's volume (0 to 15, -1 not told), whether
 * voice recognition is on, and whether calls can be held.
 */
struct kl_backend_phone_hf {
	unsigned state;
	int service;
	int signal;
	int battery;
	int roam;
	int speaker;
	unsigned voice;
	unsigned hold;
};

/*
 * Asks something of the calls (KL_BACKEND_PHONE_ASK_*): DIAL with the
 * number, HOLD with CHLD's action "0" to "2", DTMF with a key, VOLUME
 * with "0" to "15", VOICE with "0" or "1", ANSWER and HANGUP with NULL.
 * Returns as kl_backend_phone_page.
 */
int kl_backend_phone_call_ask(struct kl_backend_phone *phone, unsigned ask, const char *argument, uint32_t *id);

/* Takes the oldest change of a call.  Returns 1 with one, 0 when none waits. */
int kl_backend_phone_take_call(struct kl_backend_phone *phone, struct kl_backend_phone_call *call);

/* Copies the hands-free unit's state as last told (all zero before; speaker -1). */
void kl_backend_phone_get_hf(const struct kl_backend_phone *phone, struct kl_backend_phone_hf *hf);

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
 * Tells whether printing can be offered: the daemon's program is there,
 * as it is now, or after kl_backend_print_host as the host's last check
 * found (looked at by the open and by each kl_backend_print_check_run).
 */
int kl_backend_print_can(struct kl_backend_print *print);

/*
 * Reads what the daemon said and the settings file changed by another
 * session, without waiting.  *changed has the KL_BACKEND_PRINT_CHANGED_*
 * bits.  Returns 0.
 */
int kl_backend_print_update(struct kl_backend_print *print, unsigned *changed);

/*
 * The settings file's writes and checks on a thread of the host's (the
 * compositor's file I/O thread), so that its own thread neither starts a
 * writer nor looks at the file: after kl_backend_print_host, the host
 * calls kl_backend_print_write on its thread when
 * kl_backend_print_write_wanted says changes wait (the update answers them
 * once made), and checks the file for another session's change from time
 * to time: kl_backend_print_check_begin and _end on its own thread,
 * kl_backend_print_check_run between them on the other.  The close makes
 * the changes still waiting.
 */
struct kl_backend_print_check;

void kl_backend_print_host(struct kl_backend_print *print);
int kl_backend_print_write_wanted(struct kl_backend_print *print);
void kl_backend_print_write(struct kl_backend_print *print);
struct kl_backend_print_check *kl_backend_print_check_begin(struct kl_backend_print *print);
void kl_backend_print_check_run(struct kl_backend_print *print, struct kl_backend_print_check *check);
void kl_backend_print_check_end(struct kl_backend_print *print, struct kl_backend_print_check *check);

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
 * Ask for a printer's name and its IPP path or LPD queue to be changed
 * ("" keeps each, ws177-p025); *request numbers the answer.  Returns 0
 * when asked, or EINVAL (a path with a space or too long, a name too long).
 */
int kl_backend_print_edit(struct kl_backend_print *print, uint32_t printer, const char *name, const char *path, uint32_t *request);

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
 * The process at the other end of a client's connection (WS191): the one
 * the kernel recorded when the connection was made, so that the compositor
 * counts a process's sound streams.  Returns 0, or an errno value.
 */
int kl_backend_peer_pid(int descriptor, pid_t *pid);

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
#define KL_BACKEND_SESSION_KEYINFO	9U	/* kl_backend_session_key_info (ws199-p001) */
#define KL_BACKEND_SESSION_KEYOP	10U	/* kl_backend_session_key_pin, kl_backend_session_key_reset (ws199-p001) */
#define KL_BACKEND_SESSION_REPLUG	11U	/* not an answer: the key is to be plugged in again (ws199-p001) */
#define KL_BACKEND_SESSION_KEYOWNER	12U	/* kl_backend_session_key_owner (ws199-p001) */

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
 * Tells the manager that the session's lock screen shows (locked 1) or has
 * gone (0), so that a session started again after a crash starts locked
 * when it was.  Returns 0, ENOTSUP without a session manager, or the error
 * of telling it.
 */
int kl_backend_session_lock_state(struct kl_backend *backend, unsigned locked);

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
 * The session user's key options as the last ENROLLED told them
 * (ws199-p001): whether the key's PIN is asked to sign in, and whether its
 * touch is asked to unlock (1 and 1 until told).  kl_backend_session_
 * set_options sets them, checked by the user's password (SETOPTIONS, a
 * session); the answer is session_answer(KL_BACKEND_SESSION_ENROLL, error)
 * as kl_backend_session_set_pin's.  Returns as
 * kl_backend_session_authenticate.
 */
void kl_backend_session_options_get(const struct kl_backend *backend, unsigned *key_pin, unsigned *key_touch);
int kl_backend_session_set_options(struct kl_backend *backend, const char *password, unsigned key_pin, unsigned key_touch);

/*
 * The methods the login and locked screens take for the session user, as
 * the last ENROLLED told them (WS200): KL_BACKEND_METHOD_* bits, every
 * method until told.  kl_backend_session_set_methods sets them, checked by
 * the user's password (SETMETHODS, a session), answered as
 * kl_backend_session_set_options.  Returns as
 * kl_backend_session_authenticate.
 */
#define KL_BACKEND_METHOD_PASSWORD	0x1U
#define KL_BACKEND_METHOD_PIN		0x2U
#define KL_BACKEND_METHOD_KEY		0x4U
#define KL_BACKEND_METHODS_ALL		0x7U
unsigned kl_backend_session_methods_get(const struct kl_backend *backend);
int kl_backend_session_set_methods(struct kl_backend *backend, const char *password, unsigned methods);

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
 * What the security keys there are (ws199-p001 section 4.1): how many,
 * and for one key its name, whether it has a PIN, its PIN's retries and
 * its PIN's fewest characters.
 */
#define KL_BACKEND_KEY_NAME		64U
struct kl_backend_key_info {
	unsigned count;
	char name[KL_BACKEND_KEY_NAME];
	unsigned pin;
	unsigned retries;
	unsigned min;
};

/*
 * Asks the manager what the security keys there are (KEYINFO, a session);
 * the answer is session_answer(KL_BACKEND_SESSION_KEYINFO, error), and
 * kl_backend_session_key_info_get then gives it.  Returns as
 * kl_backend_session_authenticate.
 */
int kl_backend_session_key_info(struct kl_backend *backend);
void kl_backend_session_key_info_get(const struct kl_backend *backend, struct kl_backend_key_info *info);

/*
 * Sets the one key's first PIN (current NULL) or changes its PIN (KEYPIN,
 * a session; the key checks the PINs, no password).  The answer is
 * session_answer(KL_BACKEND_SESSION_KEYOP, error): 0, EACCES with
 * kl_backend_session_reason (bad-key-pin, key-locked, key-replug, no-pin,
 * pin-set, pin-policy, no-key, many-keys, device), EIO.  Returns as
 * kl_backend_session_authenticate.
 */
int kl_backend_session_key_pin(struct kl_backend *backend, const char *current, const char *pin);

/*
 * Resets the key the user plugs in again (KEYRESET, a session), checked by
 * the user's password: session_answer(KL_BACKEND_SESSION_REPLUG, 0) while
 * the key is to be plugged in again, KL_BACKEND_SESSION_TOUCH while it is
 * to be touched, then session_answer(KL_BACKEND_SESSION_KEYOP, error) as
 * kl_backend_session_key_pin's (bad-secret, not-allowed, timeout, canceled
 * ...); kl_backend_session_key_removed gives how many of this machine's
 * registrations went with it.  Returns as kl_backend_session_authenticate.
 */
int kl_backend_session_key_reset(struct kl_backend *backend, const char *password);
unsigned kl_backend_session_key_removed(const struct kl_backend *backend);

/*
 * Whose the security key there is (KEYOWNER, ws199-p001 section 4.2):
 * found 1 with the owner's account name, whether the key's PIN is asked
 * to sign in, whether its touch is asked to unlock, and whether the key
 * is a card held to a reader; found 0 with the manager's reason (none: no
 * account's, many-owners, no-key, many-keys, ...).
 */
struct kl_backend_key_owner {
	unsigned found;
	char user[KL_BACKEND_KEY_LABEL];
	unsigned key_pin;
	unsigned key_touch;
	unsigned card;
	char reason[KL_BACKEND_SESSION_REASON];
};

/*
 * Asks the manager whose the security key there is (KEYOWNER): of every
 * account on the login screen, of the session's own user in a session.
 * The answer is session_answer(KL_BACKEND_SESSION_KEYOWNER, error) (EBUSY
 * when the manager was asked another within a second), and
 * kl_backend_session_key_owner_get then gives it.  Returns as
 * kl_backend_session_authenticate.
 */
int kl_backend_session_key_owner(struct kl_backend *backend);
void kl_backend_session_key_owner_get(const struct kl_backend *backend, struct kl_backend_key_owner *owner);

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
 *   session_paused    stop drawing, close the output and stop reading
 *                     every input; the compositor calls
 *                     kl_backend_seat_paused_done once it has, from its
 *                     event loop after the callback, and only then is the
 *                     seat's service told it has let go
 *   session_resumed   the display may be opened again (the next frame)
 *   input_paused      stop reading the input of path; its descriptor stays
 *                     the seat's and is not closed
 *   input_resumed     read the input of path from descriptor from now on
 *                     (the old one is the seat's to close)
 *   input_gone        the input of path is gone; forget it without closing
 *                     its descriptor, which is the seat's to close
 * The three input callbacks return 1 when the compositor may still read
 * the input's descriptor it had before the callback (another thread of
 * its own reads it): the compositor then calls
 * kl_backend_seat_device_let_go with that descriptor once nothing reads
 * it, from its event loop, once for each such callback and in their order,
 * and the seat closes the descriptor (or tells its service the input has
 * stopped) only then.  They return 0 when nothing reads it any longer,
 * and the seat goes on at once.  A descriptor closed while another thread
 * might still read it could be another file's by then.
 * A failed authority calls session_stop(KL_BACKEND_SESSION_LOST).
 *
 * The input devices' scan (keiland-backend-evdev.h, ws131-p007) asks
 * input_known(path) (1 when the compositor already reads the device) and
 * offers each new device with input_found(descriptor, path, caps), which
 * returns 1 when the compositor keeps it (it then owns the descriptor
 * until kl_backend_input_close) and 0 when the backend is to close it.
 * The offer comes from the scan itself, or from kl_backend_poll_done when
 * the seat's service answers the open later.
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
 * descriptor, or -1 with errno (EAGAIN while paused).  A seat whose
 * service answers later (logind) returns -1 with EINPROGRESS, also while
 * an earlier request for path is unanswered, and offers the device with
 * input_found from kl_backend_poll_done once the answer has come.
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
 * Tells the seat that nothing of the compositor reads descriptor any
 * longer, the answer to an input callback that returned 1 (above): the
 * seat may close it or tell its service now.  A descriptor the seat waits
 * for no answer of is left alone.
 */
void kl_backend_seat_device_let_go(struct kl_backend *backend, int descriptor);

/*
 * Tells the seat that the compositor has done what session_paused asked
 * (its output is closed and no input is read): the seat lets its service
 * go on.  Called once for each session_paused, after it.
 */
void kl_backend_seat_paused_done(struct kl_backend *backend);

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

/* The media database CLI operations offered by every desktop OS backend. */
#define KL_BACKEND_MEDIA_LIST 1U
#define KL_BACKEND_MEDIA_ADD 2U
#define KL_BACKEND_MEDIA_APPLY 4U
#define KL_BACKEND_MEDIA_REMOVE 8U

/* An asynchronous spawned CLI queue with pipe input/output; notification uses libkeiland. */
struct kl_backend_media;
struct kl_backend_media *kl_backend_media_open(void);
int kl_backend_media_child(struct kl_backend_media *media, int64_t child, int status);
void kl_backend_media_close(struct kl_backend_media *media);
int kl_backend_media_request(struct kl_backend_media *media, unsigned command, const char *argument, int descriptor, uint32_t *id);
void kl_backend_media_update(struct kl_backend_media *media);
int kl_backend_media_take(struct kl_backend_media *media, uint32_t *id, int *error, int *descriptor);

/*
 * The applications' notifications of the desktop's bus (ws156-p006).
 *
 * On Linux the backend serves org.freedesktop.Notifications (the Desktop
 * Notifications Specification 1.2) on the user's session bus, so the
 * notifications of GTK, Qt and libnotify programs reach the compositor's
 * own notifications.  Keiland's programs keep posting through the
 * compositor's protocol and never pass here.  zedBSD has no D-Bus and
 * FreeBSD's backend does not serve it yet: kl_backend_notify_open answers
 * ENOTSUP there.
 *
 * Once opened, the bus is part of the backend's poll, and the callbacks are
 * called from kl_backend_poll_done.  posted is given a notification: sender
 * is the backend's number for the bus connection that sent it (each
 * connection has its own, never 0, so the compositor can keep each sender
 * to its own limit), app, title and body are plain UTF-8 text cut to
 * KL_BACKEND_NOTIFY_*_MAX bytes, replaces is the number of an earlier one
 * it replaces (0 for none; one the compositor does not know as the
 * sender's makes a new one), and flags are KL_BACKEND_NOTIFY_URGENT and
 * KL_BACKEND_NOTIFY_ACTION.  It returns the notification's number (never
 * 0), which the sender is told, or 0 with *error EBUSY (too many or too
 * fast), EACCES (the user turned the program's notifications off) or
 * another errno value.  withdrawn takes back a sender's notification by its
 * number (returns 0, or ENOENT when the sender has none of it); the
 * compositor then calls kl_backend_notify_closed with
 * KL_BACKEND_NOTIFY_CLOSED as for any other close.
 *
 * The compositor tells the backend what became of a notification it was
 * given: kl_backend_notify_closed when one went (KL_BACKEND_NOTIFY_EXPIRED:
 * out of the log; KL_BACKEND_NOTIFY_DISMISSED: by the user, its close sign
 * or the log's clearing; KL_BACKEND_NOTIFY_CLOSED: taken back by its
 * sender), and kl_backend_notify_activated when its body was clicked (one
 * posted with KL_BACKEND_NOTIFY_ACTION).  It calls them only for the
 * notifications the backend gave it, and may call kl_backend_notify_closed
 * from within posted and withdrawn too (a sender's oldest expired to make
 * room, the withdrawn one closed): unlike the host's callbacks, these two
 * may call back into the backend.  kl_backend_notify_close gives the bus's
 * name up; the backend's close does too.
 */
#define KL_BACKEND_NOTIFY_URGENT	0x1U
#define KL_BACKEND_NOTIFY_ACTION	0x2U

#define KL_BACKEND_NOTIFY_EXPIRED	1U
#define KL_BACKEND_NOTIFY_DISMISSED	2U
#define KL_BACKEND_NOTIFY_CLOSED	3U

#define KL_BACKEND_NOTIFY_APP_MAX	64U
#define KL_BACKEND_NOTIFY_TITLE_MAX	128U
#define KL_BACKEND_NOTIFY_BODY_MAX	512U

/* What the backend tells the compositor of the bus's notifications; data is passed to both unchanged. */
struct kl_backend_notify_callbacks {
	uint32_t (*posted)(void *data, uint32_t sender, const char *app, const char *title, const char *body, uint32_t replaces, unsigned flags, int *error);
	int (*withdrawn)(void *data, uint32_t sender, uint32_t id);
};

/*
 * Serves the bus's notifications.  Returns 0, ENOTSUP where the system has
 * no such bus, EEXIST when another program serves them already, ENOENT
 * when there is no session bus, or the error of connecting.
 */
int kl_backend_notify_open(struct kl_backend *backend, const struct kl_backend_notify_callbacks *callbacks, void *data);

/* Tells the notification's sender that it went, and why (KL_BACKEND_NOTIFY_EXPIRED, _DISMISSED, _CLOSED). */
void kl_backend_notify_closed(struct kl_backend *backend, uint32_t id, unsigned reason);

/* Tells the notification's sender that its body was clicked. */
void kl_backend_notify_activated(struct kl_backend *backend, uint32_t id);

/* Stops serving the bus's notifications; nothing happens when they are not served. */
void kl_backend_notify_close(struct kl_backend *backend);

#endif
