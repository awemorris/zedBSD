/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The backend object's insides, shared by backend.c and the operating
 * systems' areas that keep state in it (WS131 p005).  The compositor never
 * includes this header; it sees struct kl_backend only through
 * keiland-backend.h.
 */

#ifndef KL_BACKEND_PRIVATE_H
#define KL_BACKEND_PRIVATE_H

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

/* The bytes kept of the session manager's lines not read whole yet. */
#define KL_BACKEND_SESSION_LINE	512U

/*
 * The evdev nodes (/dev/input/eventN) remembered as refusing to open: N
 * below 64 times KL_BACKEND_INPUT_REFUSED_WORDS.
 */
#define KL_BACKEND_INPUT_REFUSED_WORDS	4U

/*
 * The compositor's backend.
 *
 * Allocated by kl_backend_open and freed by kl_backend_close.  host and
 * options are copies of what the compositor passed, kept unchanged for the
 * areas that call back.  power_asked is the action the power area has
 * asked of the session manager (0 when none), so that only one is asked.
 *
 * The session's state (ws131-p006): session_request is the request whose
 * answer is awaited (KL_BACKEND_SESSION_NONE when none); session_line holds
 * session_used bytes of a line not read whole; session_gone is set once
 * the manager closed a session's descriptor (it no longer listens);
 * logout_asked and logout_ms say a Log Out was asked and when the first
 * tick after it saw it (0 until then).  session_styles, session_pin,
 * session_keys and session_reason are what the manager last answered to
 * STYLES, ENROLLED and a refusal (ws172-p002); session_key_list holds the
 * keys ENROLLED listed (session_key_count of them, ws172-p003);
 * session_key_info what KEYINFO last answered, session_key_removed the
 * registrations a reset removed and session_key_owner what KEYOWNER last
 * answered (ws199-p001).
 *
 * events_descriptor is where the system's events are read (ws132-p003),
 * or -1 when the system has none.  power_outcome is what the last sleep
 * came to as the manager answered it (ws052-p011).
 */
struct kl_backend {
	struct kl_backend_host host;
	struct kl_backend_options options;
	unsigned power_asked;
	unsigned session_request;
	char session_line[KL_BACKEND_SESSION_LINE];
	size_t session_used;
	unsigned session_gone;
	unsigned logout_asked;
	uint64_t logout_ms;
	unsigned session_styles;
	unsigned session_pin;
	unsigned session_keys;
	struct kl_backend_key session_key_list[KL_BACKEND_KEYS_MAX];
	size_t session_key_count;
	char session_reason[KL_BACKEND_SESSION_REASON];
	struct kl_backend_key_info session_key_info;
	unsigned session_key_pin;
	unsigned session_key_touch;
	unsigned session_methods;
	unsigned session_key_removed;
	struct kl_backend_key_owner session_key_owner;
	int events_descriptor;
	/*
	 * A power button's press whose release is awaited (zedBSD's
	 * events-zedbsd.c, WS182): the firmware tells the release as another
	 * press, so the next record soon after a press is dropped.
	 * power_release_due is 1 from a press passed on until the next record,
	 * and power_press_ms is that press's time (the record's clock).
	 */
	unsigned power_release_due;
	uint64_t power_press_ms;
	struct kl_backend_power_outcome power_outcome;
	/* Remote Login's state as sessiond last answered it (ws089-p025). */
	struct kl_backend_sharing sharing;
	/*
	 * The bus's notifications (ws156-p006): the operating system's own
	 * state of serving them, NULL while they are not served.
	 */
	struct kl_backend_notify *notify;
	/*
	 * The evdev nodes whose open failed with the device's own error
	 * (bit N of the words for /dev/input/eventN) while the system's
	 * events are heard: the periodic scan passes them by until an input
	 * device comes or goes, because each open of such a node can block
	 * the event loop for tens of milliseconds (a PS/2 mouse port without
	 * a mouse answers EIO only after its timeout).  Only the code that
	 * opens the nodes reads and writes them: the event loop's scan, or the
	 * thread that runs kl_backend_input_find (never both in one run).
	 */
	uint64_t input_refused[KL_BACKEND_INPUT_REFUSED_WORDS];
	/*
	 * The refusals' forgetting: the system's events count
	 * input_refused_generation up (on the event loop's thread) when an
	 * input device came or went; the opening code empties input_refused
	 * when the count it was given differs from input_refused_seen, the
	 * count it last emptied them at (its own).
	 */
	uint64_t input_refused_generation;
	uint64_t input_refused_seen;
};

/*
 * Gives the compositor's threads lent to the backend's own (backend.c):
 * the open backend's, or none when no backend is open.
 */
void kl_backend_threads(struct kl_backend_threads *threads);

/*
 * The session's work in kl_backend_tick: each operating system's session
 * reads what its manager sent and keeps its deadlines.
 */
void kl_backend_session_tick(struct kl_backend *backend, uint64_t now_ms);
int kl_backend_session_wait(const struct kl_backend *backend, uint64_t *due_ms);

/*
 * Sends one request line to the session's manager and remembers which
 * request awaits its answer (zedBSD's session-zedbsd.c), and takes the
 * answer to a SERVICE request (sharing-zedbsd.c, ws089-p025).
 */
int kl_backend_session_send(struct kl_backend *backend, unsigned request, const char *line);
int kl_backend_sharing_take(struct kl_backend *backend, const char *line);

/*
 * The seat's part of the event loop's poll (ws131-p006): each operating
 * system's seat counts, fills and handles its descriptors (the service it
 * hears the pauses and the resumes from).
 */
size_t kl_backend_seat_poll_count(const struct kl_backend *backend);
void kl_backend_seat_poll_fill(struct kl_backend *backend, struct pollfd *descriptors);
void kl_backend_seat_poll_done(struct kl_backend *backend, const struct pollfd *descriptors);

/*
 * The system's events (ws132-p003): each operating system opens where it
 * hears them (kl_backend_events_open sets events_descriptor, or leaves it
 * -1), closes it, and counts, fills and handles its part of the poll; the
 * events go to the host's input_changed, power_changed, power_button and
 * lid_changed.
 */
void kl_backend_events_open(struct kl_backend *backend);
void kl_backend_events_close(struct kl_backend *backend);
size_t kl_backend_events_poll_count(const struct kl_backend *backend);
void kl_backend_events_poll_fill(struct kl_backend *backend, struct pollfd *descriptors);
void kl_backend_events_poll_done(struct kl_backend *backend, const struct pollfd *descriptors);

/*
 * The bus's notifications' part of the poll (ws156-p006): each operating
 * system counts, fills and handles the descriptor of its bus, none while
 * the notifications are not served.
 */
size_t kl_backend_notify_poll_count(const struct kl_backend *backend);
void kl_backend_notify_poll_fill(struct kl_backend *backend, struct pollfd *descriptors);
void kl_backend_notify_poll_done(struct kl_backend *backend, const struct pollfd *descriptors);

/*
 * Offers an input device the seat opened after its scan (logind answers
 * TakeDevice later, evdev/input-evdev.c): the compositor's clock and the
 * capability bitmaps, then input_found; a node the compositor does not
 * keep is given back to the seat.
 */
void kl_backend_input_offer(struct kl_backend *backend, int descriptor, const char *path);

/*
 * The shared POSIX parts of the computer's reading (ws188-p002,
 * machine/machine.c and machine/users.c): the PRETTY_NAME of an
 * os-release file (0 with the name, -1 when the file has none), and the
 * accounts read with the administrators' groups an operating system names
 * (a list of group names ended by NULL).
 */
int kl_backend_machine_pretty_name(const char *path, char *name, size_t size);
void kl_backend_machine_copy(char *to, size_t size, const char *from, size_t length);

/*
 * The shared part of the mount tables (ws188-p004, machine/mounts.c):
 * whether a file system's type has files (not a pseudo one), whether a
 * mount is in one of the system's trees (listed last), and the copy of
 * one mount into the list (0 when its path or type does not fit; the list
 * is not touched then).
 */
int kl_backend_mounts_keep(const char *type);
int kl_backend_mounts_system(const char *path);
int kl_backend_mounts_add(struct kl_backend_mount *mount, const char *path, const char *type);
size_t kl_backend_users_posix(struct kl_backend_user *list, size_t capacity, unsigned *skipped, const char *const *admin_groups);

#endif
