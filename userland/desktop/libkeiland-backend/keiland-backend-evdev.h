/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The input devices of libkeiland-backend (ws131-p007): the evdev types and
 * codes, and the calls that find, read and close the devices.
 *
 * zedBSD, Linux and FreeBSD share the evdev interface; only the header
 * that defines it differs, and this is the one place that chooses it (the
 * one block of the desktop selected by operating system, decision D3).
 * The compositor includes this header for the event types and codes it
 * interprets; nothing else of the operating system reaches it.
 */

#ifndef KL_BACKEND_EVDEV_H
#define KL_BACKEND_EVDEV_H

#if defined(__linux__)
#include <linux/input.h>
#elif defined(__FreeBSD__)
#include <dev/evdev/input.h>
#else
#include <uapi/input.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct kl_backend;

/* The longest device path offered, with its NUL. */
#define KL_BACKEND_INPUT_PATH_MAX	64U

/*
 * The capability bitmaps of one evdev node, as EVIOCGBIT reports them, and
 * its INPUT_PROP_* properties as EVIOCGPROP reports them (ws159-p004: a
 * touch pad is a pointer, a touch screen is direct; WS217: a pen on the
 * display is direct).  Every backend reads them (Linux and FreeBSD since
 * WS217's decision D11); a kernel without EVIOCGPROP leaves them empty.
 *
 * One instance lives on the stack while a node is classified.
 */
struct kl_backend_input_caps {
	unsigned long event[EV_MAX / (8U * sizeof(unsigned long)) + 1U];
	unsigned long key[KEY_MAX / (8U * sizeof(unsigned long)) + 1U];
	unsigned long relative[REL_MAX / (8U * sizeof(unsigned long)) + 1U];
	unsigned long absolute[ABS_MAX / (8U * sizeof(unsigned long)) + 1U];
	unsigned long properties[INPUT_PROP_MAX / (8U * sizeof(unsigned long)) + 1U];
};

/*
 * Opens every evdev node (/dev/input/eventN) the compositor does not read
 * yet (host input_known), through the seat where there is one (nothing
 * while it is paused), with the compositor's monotonic clock for the
 * events' times, and offers each to the compositor (host input_found).
 * A node the compositor does not keep is closed here.  A missing directory
 * is no error; the compositor scans again from time to time for devices
 * that come later.  The callbacks are called from this call, or, for a
 * node the seat's service opens later (logind), from kl_backend_poll_done
 * once its answer has come.
 */
void kl_backend_input_scan(struct kl_backend *backend);

/* The nodes the compositor names as read already, and the nodes one finding opens, at most. */
#define KL_BACKEND_INPUT_KNOWN_MAX	64U
#define KL_BACKEND_INPUT_FOUND_MAX	32U

/*
 * One node a finding opened: its descriptor (the finder's until the
 * compositor keeps it or has it closed), its path and its capabilities.
 */
struct kl_backend_input_node {
	int descriptor;
	char path[KL_BACKEND_INPUT_PATH_MAX];
	struct kl_backend_input_caps caps;
};

/*
 * One finding of the input devices away from the event loop.
 *
 * The event loop's thread fills the paths the compositor reads already
 * (known) and calls kl_backend_input_find_begin, which copies what the
 * finding needs of the backend's state; kl_backend_input_find then runs on
 * another thread and fills the nodes it opened.  The record is a copy:
 * the finding reads nothing the event loop changes meanwhile.
 */
struct kl_backend_input_find {
	unsigned known_count;
	char known[KL_BACKEND_INPUT_KNOWN_MAX][KL_BACKEND_INPUT_PATH_MAX];
	unsigned events_heard;
	uint64_t refused_generation;
	unsigned count;
	struct kl_backend_input_node nodes[KL_BACKEND_INPUT_FOUND_MAX];
};

/*
 * Tells whether the backend opens the input devices itself, so that the
 * opening may run on a thread of the compositor's (kl_backend_input_find):
 * 1 on zedBSD; 0 where a seat service opens them for the event loop
 * (Linux and FreeBSD), whose compositor calls kl_backend_input_scan.
 */
int kl_backend_input_find_supported(void);

/*
 * Prepares a finding on the event loop's thread (after the compositor has
 * filled known), and runs it on any one thread at a time: every evdev node
 * not known is opened and its capabilities read, nothing is offered and
 * no callback is called.  Returns 0, or ENOTSUP where the backend does not
 * open the devices itself.  A node whose open failed with the device's own
 * error is passed by until the system's events tell of an input device
 * that came or went.
 */
void kl_backend_input_find_begin(struct kl_backend *backend, struct kl_backend_input_find *find);
int kl_backend_input_find(struct kl_backend *backend, struct kl_backend_input_find *find);

/*
 * Reads an axis's range, the device's name, its identity: 0 or an errno
 * value.
 */
int kl_backend_input_absinfo(int descriptor, uint32_t axis, struct input_absinfo *info);
int kl_backend_input_name(int descriptor, char *name, size_t size);
int kl_backend_input_id(int descriptor, struct input_id *id);

/*
 * Reads whole events without waiting: the count, 0 at the device's end, or
 * -1 with errno (EAGAIN when nothing is ready, EIO for a torn event, ENODEV
 * when the seat revoked the device: see kl_backend_seat_device_revoked).
 */
ssize_t kl_backend_input_read(int descriptor, struct input_event *events, size_t capacity);

/*
 * Closes a device the compositor kept, through the seat that opened it.
 * Where kl_backend_input_find_supported says 1, it may be called on a
 * thread of the compositor's (a device's close can wait for its driver).
 */
void kl_backend_input_close(struct kl_backend *backend, int descriptor);

/* The input devices one wait of the devices' thread opens at most (more are left to a finding). */
#define KL_BACKEND_DEVICES_NODES_MAX	8U

/*
 * What one wait of the devices' thread brought: an input device came or
 * went (input_changed), a security key or a smart card did (keys_changed),
 * records were lost or more devices came than the batch holds, so every
 * device is to be looked at again (overflow), and the input devices that
 * came, opened with their capabilities (count of them, each descriptor
 * the compositor's to keep or close).  One instance is filled on the
 * devices' thread and taken on the event loop's.
 */
struct kl_backend_device_batch {
	unsigned input_changed;
	unsigned keys_changed;
	unsigned overflow;
	unsigned count;
	struct kl_backend_input_node nodes[KL_BACKEND_DEVICES_NODES_MAX];
};

/*
 * The devices' thread (zedBSD): the system's events of the devices that
 * come and go are heard on a thread of the compositor's, which opens each
 * input device that comes (through the session manager's seat socket when
 * there is one) and hands it to the event loop.
 *
 * kl_backend_devices_start, on the event loop's thread before the first
 * finding, subscribes the thread's descriptor and takes those classes off
 * the event loop's: 0, or ENOTSUP where the system does not tell them
 * (Linux, FreeBSD, a kernel without the events), or an errno value.
 * kl_backend_devices_wait, on the devices' thread, waits without a time
 * limit for records, opens the input devices that came and fills batch:
 * 0, ECANCELED once kl_backend_input_cancel was called, or an errno value
 * (the thread ends).  kl_backend_devices_take, on the event loop's thread,
 * acts on a batch's notes (the refusals forgotten, keys_changed, and
 * input_changed for an overflow); the compositor offers its nodes itself.
 * kl_backend_devices_stop, on the event loop's thread after the thread
 * ended, gives the classes back to the event loop's descriptor.
 */
int kl_backend_devices_start(struct kl_backend *backend);
int kl_backend_devices_wait(struct kl_backend *backend, struct kl_backend_device_batch *batch);
void kl_backend_devices_take(struct kl_backend *backend, const struct kl_backend_device_batch *batch);
void kl_backend_devices_stop(struct kl_backend *backend);

/*
 * Ends, from any thread and for good, the devices' thread's wait and every
 * wait for the session manager's answer to an open (they return
 * ECANCELED); called as the compositor stops, before its threads are
 * joined.
 */
void kl_backend_input_cancel(struct kl_backend *backend);

#endif
