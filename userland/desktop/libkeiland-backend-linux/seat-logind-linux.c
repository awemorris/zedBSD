/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * logind's seat on Linux (libkeiland-backend since ws131-p006): session
 * control, one TakeDevice lease a device, and the PauseDevice and
 * ResumeDevice signals.  The compositor hears them through the backend's
 * callbacks and knows its inputs by their device path; it is told to stop
 * before a cooperative pause is acknowledged.
 *
 * Nothing waits for logind after the seat is open: an input's TakeDevice is
 * sent and its answer taken from the bus with the signals (the device is
 * offered to the compositor then), and ReleaseDevice and
 * PauseDeviceComplete expect no answer.  A descriptor the compositor may
 * still read when a callback returns (its input thread lets go later)
 * closes, and a pause it concerns is acknowledged, only at the
 * compositor's kl_backend_seat_device_let_go, and the primary node's pause
 * at kl_backend_seat_paused_done.
 */
#include "userland/desktop/libkeiland-backend-linux/seat-linux.h"
#include "userland/desktop/libkeiland-backend-linux/dbus-linux.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define LOGIND_DEVICES LINUX_SEAT_DEVICES

/* The most descriptors waiting for the compositor's let-go at once (a pause and a resume of every input). */
#define LOGIND_ANSWERS (2U * LINUX_SEAT_DEVICES)

/* What the compositor's let-go of a descriptor does: close it, or acknowledge the pause of its device. */
#define LOGIND_ANSWER_CLOSE 1U
#define LOGIND_ANSWER_COMPLETE 2U

/*
 * One device holds exactly one TakeDevice lease until release or seat
 * cleanup; taking says its TakeDevice is sent and unanswered (serial names
 * the answer), and the slot is reserved for path meanwhile.
 */
struct logind_device {
	unsigned owned;
	unsigned taking;
	uint32_t serial;
	unsigned paused;
	unsigned display;
	uint32_t major;
	uint32_t minor;
	int fd;
	char path[4096];
};

/* A descriptor waiting for the compositor's let-go, and what it does then (of the device major:minor). */
struct logind_answer {
	unsigned action;
	int descriptor;
	uint32_t major;
	uint32_t minor;
};

/* The single compositor process owns this bus until OS cleanup. */
static struct linux_dbus seat_bus = {-1, 0, NULL, 0, 0, {NULL}, 0};

/* Device leases stay live while paused; slot zero is reserved for the primary node. */
static struct logind_device seat_devices[LOGIND_DEVICES];

/* The actual escaped path returned by GetSession determines every later call and match. */
static char seat_session[4096];

/* Only signals from the resolved unique logind owner may change device authority. */
static char seat_owner[256];

/* Control is returned only when TakeControl succeeded, including partial startup. */
static unsigned seat_control;

/* A paused primary node suppresses common composition and input discovery. */
static unsigned seat_paused;

/* The descriptors waiting for the compositor's let-go, oldest first (seat_answer_count of them). */
static struct logind_answer seat_answers[LOGIND_ANSWERS];
static unsigned seat_answer_count;

/* The primary node's cooperative pause, acknowledged once the compositor is done (kl_backend_seat_paused_done). */
static unsigned seat_display_answer;

static int seat_call(const char *member, const char *signature, const struct dbus_arg *args, size_t count, struct dbus_reply **reply);
static int seat_device_send(const char *member, uint32_t major, uint32_t minor);
static int seat_identify(const char *path, uint32_t *major_number, uint32_t *minor_number);
static int seat_take(struct logind_device *device, const char *path, unsigned display);
static int seat_take_ask(struct logind_device *device, const char *path);
static int seat_take_reply(struct logind_device *device, struct dbus_reply *reply);
static void seat_take_answered(struct kl_backend *backend, struct dbus_reply *message);
static void seat_release(struct logind_device *device);
static void seat_answer_wait(unsigned action, int descriptor, uint32_t major, uint32_t minor);
static int seat_message(struct dbus_reply *message, void *data);
static int seat_signal(struct dbus_reply *message, void *data);
static int seat_pause(struct kl_backend *backend, struct logind_device *device, const char *kind);
static int seat_resume(struct kl_backend *backend, struct logind_device *device, struct dbus_reply *message, uint32_t index);

/*
 * Takes session control and the primary device before Vulkan inquiry begins.
 */
int
linux_logind_seat_open(
	struct kl_backend *backend)
{
	struct dbus_arg argument;
	struct dbus_reply *reply;
	const char *session;
	const char *value;
	const char *path;
	char match[4608];
	size_t length;
	int error;
	int same;
	int written;

	/* Session metadata must identify the caller's existing login session. */
	session = getenv("XDG_SESSION_ID");
	if (session == NULL || session[0] == '\0')
		return EINVAL;
	error = dbus_open_system(&seat_bus);
	if (error != 0)
		return error;

	/* Resolve service authority once; arbitrary unicast signals cannot impersonate it. */
	memset(&argument, 0, sizeof(argument));
	argument.string = "org.freedesktop.login1";
	error = dbus_call(&seat_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner", "s", &argument, 1, &reply);
	if (error != 0)
		return error;
	same = strcmp(reply->signature, "s");
	error = EPROTO;
	if (same == 0)
		error = dbus_read_string(reply, &value);
	if (error != 0) {
		dbus_reply_free(reply);
		return error;
	}

	/* Store the unique owner before releasing borrowed message storage. */
	length = strlen(value);
	if (length >= sizeof(seat_owner)) {
		dbus_reply_free(reply);
		return EOVERFLOW;
	}

	/* Retains the validated device or session value before its next use. */
	memcpy(seat_owner, value, length + 1);
	dbus_reply_free(reply);

	/* GetSession returns the exact escaped path; self and auto are not signal paths. */
	argument.string = session;
	error = dbus_call(&seat_bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "GetSession", "s", &argument, 1, &reply);
	if (error != 0)
		return error;
	same = strcmp(reply->signature, "o");
	error = EPROTO;
	if (same == 0)
		error = dbus_read_string(reply, &value);
	if (error != 0) {
		dbus_reply_free(reply);
		return error;
	}

	/* A truncated session path must never redirect later device calls. */
	length = strlen(value);
	if (length >= sizeof(seat_session)) {
		dbus_reply_free(reply);
		return EOVERFLOW;
	}

	/* Retains the validated device or session value before its next use. */
	memcpy(seat_session, value, length + 1);
	dbus_reply_free(reply);

	/* Subscribe before control so no pause can be missed during startup. */
	written = snprintf(match, sizeof(match), "type='signal',sender='org.freedesktop.login1',interface='org.freedesktop.login1.Session',path='%s'", seat_session);
	if (written < 0 || (size_t)written >= sizeof(match))
		return EOVERFLOW;
	error = dbus_add_match(&seat_bus, match);
	if (error != 0)
		return error;
	argument.number = 0;
	error = seat_call("TakeControl", "b", &argument, 1, &reply);
	if (error != 0)
		return error;
	dbus_reply_free(reply);
	seat_control = 1;

	/* The selected primary path remains authoritative for the Vulkan backend as well. */
	path = getenv("KEILAND_DRM_DEVICE");
	if (path == NULL)
		path = "/dev/dri/card0";
	error = seat_take(&seat_devices[0], path, 1);
	if (error != 0)
		return error;
	seat_paused = seat_devices[0].paused;
	(void)backend;
	printf("KWL SEAT logind session=%s\n", seat_session);

	/* Succeeded: logind, rather than direct open, owns device authority. */
	return 0;
}

/*
 * Returns all device leases and session control, including paused descriptors.
 */
void
linux_logind_seat_close(
	void)
{
	struct dbus_reply *reply;
	unsigned index;
	int error;

	/* Common input cleanup may have skipped negative paused descriptors; an unanswered take's lease goes with the control below. */
	for (index = 0; index < LOGIND_DEVICES; index++) {
		seat_release(&seat_devices[index]);
		seat_devices[index].taking = 0;
	}

	/* The descriptors still waiting for a let-go: the compositor's threads have ended. */
	for (index = 0; index < seat_answer_count; index++) {
		if (seat_answers[index].action == LOGIND_ANSWER_CLOSE)
			(void)close(seat_answers[index].descriptor);
	}

	/* Forgets the answers, all of which have been carried out. */
	seat_answer_count = 0;
	seat_display_answer = 0;

	/* Control is relinquished only if this process successfully took it. */
	if (seat_control != 0) {
		error = seat_call("ReleaseControl", "", NULL, 0, &reply);
		if (error == 0)
			dbus_reply_free(reply);
		seat_control = 0;
	}

	/* Losing the bus also returns server-side leases after any transport failure. */
	dbus_close(&seat_bus);
	seat_paused = 0;

	/* Succeeded: no session authority survives this compositor. */
	return;
}

/*
 * Asks logind for one input's lease: -1 with EINPROGRESS once TakeDevice
 * is sent (or while an earlier one for path is unanswered); the answer
 * offers the device to the compositor from linux_logind_dispatch.
 */
int
linux_logind_device_open(
	const char *path)
{
	unsigned index;
	unsigned free_slot;
	int same;
	int error;

	/* Input discovery is postponed until the primary seat is active again. */
	if (seat_paused != 0) {
		errno = EAGAIN;
		return -1;
	}

	/* A take of path already under way answers for this one too; else the first free slot. */
	free_slot = LOGIND_DEVICES;
	for (index = 1; index < LOGIND_DEVICES; index++) {
		if (seat_devices[index].taking != 0) {
			same = strcmp(seat_devices[index].path, path);
			if (same == 0) {
				errno = EINPROGRESS;
				return -1;
			}

			/* A slot with a take under way for another path is not free. */
			continue;
		}

		/* Remembers the first slot nothing owns. */
		if (seat_devices[index].owned == 0 && free_slot == LOGIND_DEVICES)
			free_slot = index;
	}

	/* The ownership table is bounded by the common input capacity. */
	if (free_slot == LOGIND_DEVICES) {
		errno = EMFILE;
		return -1;
	}

	/* The request goes out; its answer comes on the bus. */
	error = seat_take_ask(&seat_devices[free_slot], path);
	if (error != 0) {
		errno = error;
		return -1;
	}

	/* Succeeded: the answer offers the device. */
	errno = EINPROGRESS;
	return -1;
}

/*
 * Releases the lease corresponding to one input descriptor.
 */
void
linux_logind_device_close(
	int descriptor)
{
	unsigned index;

	/* A paused common record has no descriptor; final seat cleanup owns its lease. */
	if (descriptor < 0)
		return;

	/* Only the exact owned descriptor can release a service lease. */
	for (index = 1; index < LOGIND_DEVICES; index++) {
		if (seat_devices[index].owned != 0 && seat_devices[index].fd == descriptor) {
			seat_release(&seat_devices[index]);
			return;
		}
	}

	/* A descriptor outside the ownership table must not be closed speculatively. */
	return;
}

/*
 * Supplies the current logind primary descriptor for Vulkan acquisition.
 */
int
linux_logind_drm_fd(
	void)
{
	/* An absent lease cannot authorize a display acquisition. */
	if (seat_devices[0].owned == 0)
		return -1;

	/* Succeeded: Vulkan may duplicate this active service-owned file. */
	return seat_devices[0].fd;
}

/*
 * Supplies the selected primary-node path for the compatibility library.
 */
const char *
linux_logind_drm_path(
	void)
{
	/* Succeeded: the pathname remains stable across fd replacement on resume. */
	return seat_devices[0].path;
}

/*
 * Reports whether the primary seat is paused.
 */
int
linux_logind_seat_paused(
	void)
{
	/* Succeeded: common input discovery observes the same pause as composition. */
	return (int)seat_paused;
}

/*
 * Supplies the authenticated system-bus socket to the OS poll range.
 */
int
linux_logind_poll_fd(
	void)
{
	int descriptor;

	/* The bus remains polled while the primary device itself is paused. */
	descriptor = dbus_fd(&seat_bus);

	/* Succeeded: resume signals can wake the common event loop. */
	return descriptor;
}

/*
 * Applies queued and newly arrived device signals before common input reads.
 */
int
linux_logind_dispatch(
	struct kl_backend *backend)
{
	int error;

	/* The signals, and the answers to the inputs' TakeDevice, in their order. */
	error = dbus_dispatch_messages(&seat_bus, seat_message, backend);
	if (error != 0)
		return error;

	/* Succeeded: the seat's descriptor generations agree with current authority. */
	return 0;
}

/*
 * Excludes a revoked input file while preserving its lease for the pending signal.
 */
int
linux_logind_device_revoked(
	int descriptor)
{
	struct logind_device *device;
	unsigned index;

	/* Only this seat's exact current input descriptor can retain service ownership; the compositor stops reading it. */
	for (index = 1; index < LOGIND_DEVICES; index++) {
		device = &seat_devices[index];
		if (device->owned == 0 || device->fd != descriptor)
			continue;
		device->paused = 1;
		printf("KWL SEAT input_revoked device=%u:%u lease=retained\n", device->major, device->minor);
		return 1;
	}

	/* An unknown descriptor cannot acquire logind ownership by reporting an error. */
	return 0;
}

/*
 * Does what the oldest wait for descriptor asked, now that the compositor
 * no longer reads it: closes it, or acknowledges its device's pause while
 * the device is still paused on it.
 */
void
linux_logind_device_let_go(
	int descriptor)
{
	struct logind_answer answer;
	struct logind_device *device;
	unsigned index;
	unsigned slot;
	int error;

	/* The oldest wait for it; a descriptor nothing waits for is left alone. */
	for (index = 0; index < seat_answer_count; index++) {
		if (seat_answers[index].descriptor == descriptor)
			break;
	}

	/* Ignores a descriptor nothing waits for. */
	if (index == seat_answer_count)
		return;
	answer = seat_answers[index];

	/* The rest keep their order. */
	seat_answer_count--;
	for (; index < seat_answer_count; index++)
		seat_answers[index] = seat_answers[index + 1U];

	/* A descriptor the seat has given up on closes now. */
	if (answer.action == LOGIND_ANSWER_CLOSE) {
		(void)close(answer.descriptor);
		return;
	}

	/* A pause is acknowledged only while its device is still paused on that descriptor (a resume came first otherwise). */
	device = NULL;
	for (slot = 1; slot < LOGIND_DEVICES; slot++) {
		if (seat_devices[slot].owned != 0 && seat_devices[slot].fd == descriptor && seat_devices[slot].major == answer.major && seat_devices[slot].minor == answer.minor) {
			device = &seat_devices[slot];
			break;
		}
	}

	/* Ignores a device that is gone or no longer paused. */
	if (device == NULL || device->paused == 0)
		return;

	/* Tells logind that the pause is acknowledged. */
	error = seat_device_send("PauseDeviceComplete", answer.major, answer.minor);
	if (error != 0)
		fprintf(stderr, "wayland: logind pause complete %u:%u errno=%d\n", answer.major, answer.minor, error);
}

/*
 * Acknowledges the primary node's cooperative pause once the compositor
 * has closed its output and reads no input, unless it has resumed since.
 */
void
linux_logind_paused_done(
	void)
{
	int error;

	/* Only a pause still waiting, of a node still paused. */
	if (seat_display_answer == 0)
		return;
	seat_display_answer = 0;
	if (seat_devices[0].owned == 0 || seat_devices[0].paused == 0)
		return;

	/* logind may go on with the switch. */
	error = seat_device_send("PauseDeviceComplete", seat_devices[0].major, seat_devices[0].minor);
	if (error != 0)
		fprintf(stderr, "wayland: logind pause complete %u:%u errno=%d\n", seat_devices[0].major, seat_devices[0].minor, error);
	printf("KWL SEAT pause_complete device=%u:%u display=1\n", seat_devices[0].major, seat_devices[0].minor);
}

/* Calls the exact session object without rederiving its escaped path. */
static int
seat_call(
	const char *member,
	const char *signature,
	const struct dbus_arg *args,
	size_t count,
	struct dbus_reply **reply)
{
	int error;

	/* Every device operation is scoped to the session whose control we own. */
	error = dbus_call(&seat_bus, "org.freedesktop.login1", seat_session, "org.freedesktop.login1.Session", member, signature, args, count, reply);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the method response. */
	return 0;
}

/*
 * Sends a lease transition naming the device's immutable major and minor,
 * expecting no answer (an answer awaited would leave the bus's other
 * messages to a wait of their own).
 */
static int
seat_device_send(
	const char *member,
	uint32_t major,
	uint32_t minor)
{
	struct dbus_arg args[2];
	uint32_t serial;
	int error;

	/* The service identity is independent of the current, replaceable descriptor. */
	memset(args, 0, sizeof(args));
	args[0].number = major;
	args[1].number = minor;
	error = dbus_send_call(&seat_bus, LINUX_DBUS_NO_REPLY_EXPECTED, "org.freedesktop.login1", seat_session, "org.freedesktop.login1.Session", member, "uu", args, 2, &serial);
	if (error != 0)
		return error;

	/* Succeeded: the transition is on its way to logind. */
	return 0;
}

/* Reads the major and minor of the character device at path, which must be absolute and fit a slot. */
static int
seat_identify(
	const char *path,
	uint32_t *major_number,
	uint32_t *minor_number)
{
	struct stat status;
	size_t length;
	int character;
	int error;

	/* The path must fit intact and designate a character device. */
	length = strlen(path);
	if (length >= sizeof(seat_devices[0].path) || path[0] != '/')
		return EINVAL;
	error = stat(path, &status);
	if (error != 0)
		return errno;
	character = S_ISCHR(status.st_mode);
	if (character == 0)
		return ENODEV;

	/* Succeeded: logind names the device by these. */
	*major_number = (uint32_t)major(status.st_rdev);
	*minor_number = (uint32_t)minor(status.st_rdev);
	return 0;
}

/* Takes a device once, waiting for logind (the primary node, at open), and preserves its service identity for later release. */
static int
seat_take(
	struct logind_device *device,
	const char *path,
	unsigned display)
{
	struct dbus_arg args[2];
	struct dbus_reply *reply;
	uint32_t major_number;
	uint32_t minor_number;
	size_t length;
	int error;

	/* The device's identity. */
	error = seat_identify(path, &major_number, &minor_number);
	if (error != 0)
		return error;

	/* No device is opened directly; logind supplies its owned file. */
	memset(args, 0, sizeof(args));
	args[0].number = major_number;
	args[1].number = minor_number;
	error = seat_call("TakeDevice", "uu", args, 2, &reply);
	if (error != 0)
		return error;

	/* A successful method owns a lease even if its response is malformed. */
	length = strlen(path);
	memset(device, 0, sizeof(*device));
	device->fd = -1;
	device->owned = 1;
	device->display = display;
	device->major = major_number;
	device->minor = minor_number;
	memcpy(device->path, path, length + 1);
	error = seat_take_reply(device, reply);
	dbus_reply_free(reply);
	if (error != 0)
		return error;

	/* Succeeded: this slot owns exactly one service lease and descriptor. */
	return 0;
}

/* Sends an input's TakeDevice and reserves the slot for its answer. */
static int
seat_take_ask(
	struct logind_device *device,
	const char *path)
{
	struct dbus_arg args[2];
	uint32_t major_number;
	uint32_t minor_number;
	uint32_t serial;
	size_t length;
	int error;

	/* The device's identity. */
	error = seat_identify(path, &major_number, &minor_number);
	if (error != 0)
		return error;

	/* The request, whose answer names serial. */
	memset(args, 0, sizeof(args));
	args[0].number = major_number;
	args[1].number = minor_number;
	error = dbus_send_call(&seat_bus, 0, "org.freedesktop.login1", seat_session, "org.freedesktop.login1.Session", "TakeDevice", "uu", args, 2, &serial);
	if (error != 0)
		return error;

	/* The slot waits for it. */
	length = strlen(path);
	memset(device, 0, sizeof(*device));
	device->fd = -1;
	device->taking = 1;
	device->serial = serial;
	device->major = major_number;
	device->minor = minor_number;
	memcpy(device->path, path, length + 1);

	/* Succeeded: the answer comes on the bus. */
	return 0;
}

/* Takes TakeDevice's descriptor and active flag into an owned slot (a failure releases the lease). */
static int
seat_take_reply(
	struct logind_device *device,
	struct dbus_reply *reply)
{
	uint32_t index;
	uint32_t inactive;
	int descriptor;
	int flags;
	int error;
	int same;

	/* The answer's exact form. */
	same = strcmp(reply->signature, "hb");
	error = EPROTO;
	if (same == 0)
		error = dbus_read_number(reply, &index);
	if (error != 0) {
		seat_release(device);
		return error;
	}

	/* The active flag is a D-Bus Boolean, never an arbitrary integer. */
	error = dbus_read_number(reply, &inactive);
	if (error != 0 ||
	    inactive > 1 ||
	    reply->cursor != reply->size) {
		seat_release(device);
		return EPROTO;
	}

	/* Take the indexed fd before the message's remaining rights close with it. */
	descriptor = dbus_take_fd(reply, index);
	if (descriptor < 0) {
		seat_release(device);
		return EPROTO;
	}

	/* Retains the validated device or session value before its next use. */
	device->fd = descriptor;
	device->paused = inactive;

	/* Every input read remains nonblocking after transfer from the service. */
	flags = fcntl(descriptor, F_GETFL);
	if (flags < 0) {
		error = errno;
		seat_release(device);
		return error;
	}

	/* Retains the validated device or session value before its next use. */
	error = fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
	if (error != 0) {
		error = errno;
		seat_release(device);
		return error;
	}

	/* Succeeded: this slot owns exactly one service lease and descriptor. */
	return 0;
}

/*
 * Takes the answer to an input's TakeDevice: its descriptor is offered to
 * the compositor, unless the seat or the device is paused meanwhile (the
 * lease goes back, a later scan asks again) or logind refused.
 */
static void
seat_take_answered(
	struct kl_backend *backend,
	struct dbus_reply *message)
{
	struct logind_device *device;
	unsigned slot;
	int same;
	int error;

	/* Only logind answers, to a take under way. */
	if (message->sender == NULL)
		return;
	same = strcmp(message->sender, seat_owner);
	if (same != 0)
		return;
	device = NULL;
	for (slot = 1; slot < LOGIND_DEVICES; slot++) {
		if (seat_devices[slot].taking != 0 && seat_devices[slot].serial == message->reply_serial) {
			device = &seat_devices[slot];
			break;
		}
	}

	/* Ignores an answer that matches no take under way. */
	if (device == NULL)
		return;
	device->taking = 0;

	/* A refusal leaves the slot free (the path is tried again at a later scan). */
	if (message->type == LINUX_DBUS_ERROR) {
		fprintf(stderr, "wayland: logind take %u:%u: %s\n", device->major, device->minor, message->error != NULL ? message->error : "error");
		memset(device, 0, sizeof(*device));
		device->fd = -1;
		return;
	}

	/* The lease, owned from here. */
	device->owned = 1;
	error = seat_take_reply(device, message);
	if (error != 0) {
		fprintf(stderr, "wayland: logind take %u:%u errno=%d\n", device->major, device->minor, error);
		return;
	}

	/* An inactive node, or a seat paused while it was asked, cannot be probed; a later scan asks again. */
	if (device->paused != 0 || seat_paused != 0) {
		seat_release(device);
		return;
	}

	/* The compositor classifies it (a node it does not keep comes back through linux_logind_device_close). */
	kl_backend_input_offer(backend, device->fd, device->path);
}

/* Returns a taken lease exactly once even when the transport has failed. */
static void
seat_release(
	struct logind_device *device)
{
	int error;

	/* Unallocated and already released slots have no service ownership. */
	if (device->owned == 0)
		return;

	/* Closing the local file never substitutes for returning a live service lease. */
	if (seat_bus.fd >= 0) {
		error = seat_device_send("ReleaseDevice", device->major, device->minor);
		if (error != 0)
			fprintf(stderr, "wayland: logind release %u:%u errno=%d\n", device->major, device->minor, error);
	}

	/* A disconnected bus has already returned service control; local rights still close here. */
	if (device->fd >= 0)
		(void)close(device->fd);
	memset(device, 0, sizeof(*device));
	device->fd = -1;

	/* Succeeded: repeated cleanup cannot return or close this lease again. */
	return;
}

/*
 * Keeps descriptor for the compositor's let-go, after the waits before it;
 * without room the descriptor stays open (closing it while it may be read
 * could make another file read) and a pause is acknowledged at once.
 */
static void
seat_answer_wait(
	unsigned action,
	int descriptor,
	uint32_t major_number,
	uint32_t minor_number)
{
	struct logind_answer *answer;
	int error;

	/* Room for it. */
	if (seat_answer_count < LOGIND_ANSWERS) {
		answer = &seat_answers[seat_answer_count];
		answer->action = action;
		answer->descriptor = descriptor;
		answer->major = major_number;
		answer->minor = minor_number;
		seat_answer_count++;
		return;
	}

	/* No room: the safe half of the action. */
	fprintf(stderr, "wayland: logind answers full device=%u:%u action=%u\n", major_number, minor_number, action);
	if (action != LOGIND_ANSWER_COMPLETE)
		return;
	error = seat_device_send("PauseDeviceComplete", major_number, minor_number);
	if (error != 0)
		fprintf(stderr, "wayland: logind pause complete %u:%u errno=%d\n", major_number, minor_number, error);
}

/* Routes one message of the bus: a signal to seat_signal, an answer to a take under way. */
static int
seat_message(
	struct dbus_reply *message,
	void *data)
{
	int error;

	/* The signals, as before. */
	if (message->type == LINUX_DBUS_SIGNAL) {
		error = seat_signal(message, data);
		if (error != 0)
			return error;
		return 0;
	}

	/* A return or an error may answer a TakeDevice; nothing else is asked of the bus. */
	if (message->type == LINUX_DBUS_METHOD_RETURN || message->type == LINUX_DBUS_ERROR)
		seat_take_answered(data, message);

	/* Succeeded: the message is consumed. */
	return 0;
}

/* Validates signal authority and routes transitions only to known leases. */
static int
seat_signal(
	struct dbus_reply *message,
	void *data)
{
	struct kl_backend *backend;
	struct logind_device *device;
	const char *kind;
	uint32_t device_major;
	uint32_t device_minor;
	uint32_t index;
	unsigned slot;
	int same;
	int pause;
	int error;

	/* Other daemon signals are harmless and never affect device ownership. */
	backend = data;
	if (message->sender == NULL)
		return 0;
	same = strcmp(message->sender, seat_owner);
	if (same != 0)
		return 0;
	same = strcmp(message->path, seat_session);
	if (same != 0)
		return 0;
	same = strcmp(message->interface, "org.freedesktop.login1.Session");
	if (same != 0)
		return 0;
	same = strcmp(message->member, "PauseDevice");
	pause = 0;
	if (same == 0) {
		pause = 1;
		same = strcmp(message->signature, "uus");
	} else {
		same = strcmp(message->member, "ResumeDevice");
		if (same != 0)
			return 0;
		same = strcmp(message->signature, "uuh");
	}

	/* A recognized device signal must carry its exact argument representation. */
	if (same != 0)
		return EPROTO;
	error = dbus_read_number(message, &device_major);
	if (error != 0)
		return error;
	error = dbus_read_number(message, &device_minor);
	if (error != 0)
		return error;

	/* A released or unsupported device cannot acquire a new lease through a signal. */
	device = NULL;
	for (slot = 0; slot < LOGIND_DEVICES; slot++) {
		if (seat_devices[slot].owned != 0 &&
		    seat_devices[slot].major == device_major &&
		    seat_devices[slot].minor == device_minor) {
			device = &seat_devices[slot];
			break;
		}
	}

	/* The message destructor closes unused resume descriptors for absent leases. */
	if (device == NULL)
		return 0;
	if (pause != 0) {
		error = dbus_read_string(message, &kind);
		if (error != 0)
			return error;
		if (message->cursor != message->size)
			return EPROTO;
		error = seat_pause(backend, device, kind);
		if (error != 0)
			return error;
	} else {
		error = dbus_read_number(message, &index);
		if (error != 0)
			return error;
		if (message->cursor != message->size)
			return EPROTO;
		error = seat_resume(backend, device, message, index);
		if (error != 0)
			return error;
	}

	/* Succeeded: the device transition and common state agree. */
	return 0;
}

/* Stops device use before acknowledging a cooperative pause. */
static int
seat_pause(
	struct kl_backend *backend,
	struct logind_device *device,
	const char *kind)
{
	int cooperative;
	int gone;
	int later;
	int same;
	int error;

	/* Only the three logind pause kinds have defined ownership semantics. */
	cooperative = 0;
	gone = 0;
	same = strcmp(kind, "pause");
	if (same == 0) {
		cooperative = 1;
	} else {
		same = strcmp(kind, "gone");
		if (same == 0) {
			gone = 1;
		} else {
			same = strcmp(kind, "force");
			if (same != 0)
				return EPROTO;
		}
	}

	/* Pause excludes descriptors but retains the TakeDevice lease for resume; the compositor stops first. */
	device->paused = 1;
	later = 0;
	if (device->display != 0) {
		seat_paused = 1;
		if (backend->host.session_paused != NULL)
			backend->host.session_paused(backend->host.data);
	} else if (backend->host.input_paused != NULL) {
		later = backend->host.input_paused(backend->host.data, device->path);
	}

	/*
	 * The log records actual notification kind rather than assuming VT
	 * behavior.  A cooperative pause is acknowledged once the compositor
	 * has let go: the primary node's at kl_backend_seat_paused_done, an
	 * input's at its let-go (at once when nothing reads it).
	 */
	printf("KWL SEAT pause device=%u:%u type=%s display=%u later=%d\n", device->major, device->minor, kind, device->display, later);
	if (cooperative != 0) {
		if (device->display != 0 && backend->host.session_paused != NULL) {
			seat_display_answer = 1;
		} else if (later != 0) {
			seat_answer_wait(LOGIND_ANSWER_COMPLETE, device->fd, device->major, device->minor);
		} else {
			error = seat_device_send("PauseDeviceComplete", device->major, device->minor);
			if (error != 0)
				return error;
		}
	}

	/* Gone devices cannot receive a resume: the compositor forgets the input, then the lease returns once. */
	if (gone != 0) {
		if (device->display != 0) {
			seat_release(device);
			return ENODEV;
		}

		/* Asks the compositor to let go of the device it may be reading. */
		later = 0;
		if (backend->host.input_gone != NULL)
			later = backend->host.input_gone(backend->host.data, device->path);

		/* A descriptor the compositor may still read closes at its let-go; the lease returns now. */
		if (later != 0 && device->fd >= 0) {
			seat_answer_wait(LOGIND_ANSWER_CLOSE, device->fd, device->major, device->minor);
			device->fd = -1;
		}
		seat_release(device);
	}

	/* Succeeded: a cooperative acknowledgement follows complete withdrawal of use. */
	return 0;
}

/* Replaces a revoked file and restores the common input or display generation. */
static int
seat_resume(
	struct kl_backend *backend,
	struct logind_device *device,
	struct dbus_reply *message,
	uint32_t index)
{
	int descriptor;
	int old_descriptor;
	int flags;
	int error;
	int clock_id;
	int later;

	/* The message owns this new file until all descriptor setup has succeeded. */
	if (index >= message->fd_count)
		return EPROTO;
	descriptor = message->fds[index];
	if (descriptor < 0)
		return EPROTO;
	flags = fcntl(descriptor, F_GETFL);
	if (flags < 0)
		return errno;
	error = fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
	if (error != 0)
		return errno;

	/* Resumed evdev reports must retain the common monotonic timestamp domain. */
	if (device->display == 0) {
		clock_id = CLOCK_MONOTONIC;
		error = ioctl(descriptor, EVIOCSCLOCKID, &clock_id);
		if (error != 0)
			return errno;
	}

	/* Only one descriptor generation is owned after successful replacement. */
	descriptor = dbus_take_fd(message, index);
	if (descriptor < 0)
		return EPROTO;
	old_descriptor = device->fd;
	device->fd = descriptor;
	device->paused = 0;

	/* The compositor opens its output again from its scheduler (a pause not acknowledged yet stays so), or reads the input from the new file. */
	later = 0;
	if (device->display != 0) {
		seat_paused = 0;
		seat_display_answer = 0;
		if (backend->host.session_resumed != NULL)
			backend->host.session_resumed(backend->host.data);
	} else if (backend->host.input_resumed != NULL) {
		later = backend->host.input_resumed(backend->host.data, device->path, descriptor);
	}

	/*
	 * The old file closes only after the compositor has let go of it: its
	 * input thread may still read the old descriptor after the callback
	 * returns (it answers with kl_backend_seat_device_let_go), and a number
	 * closed earlier could already be another file's (ws213-p004, the
	 * design's 6.1 N-1).
	 */
	if (old_descriptor >= 0) {
		if (later != 0)
			seat_answer_wait(LOGIND_ANSWER_CLOSE, old_descriptor, device->major, device->minor);
		else
			(void)close(old_descriptor);
	}

	/* Succeeded: input and display use only the newly supplied file. */
	printf("KWL SEAT resume device=%u:%u display=%u\n", device->major, device->minor, device->display);
	return 0;
}
