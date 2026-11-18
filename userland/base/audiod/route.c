/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sound's endpoints and routes (ws209-p002; plan/ws209/phase001/phase.md
 * design 1.1 and 1.2): protocol version 2's control.
 *
 * An endpoint is a way in or out of sound: the kernel's /dev/dsp0 (its
 * playback device:out and its recording device:in, audiod's own) or one a
 * daemon registers (bluetoothd's headset or phone links, a modem's).  A
 * route says where sound goes: media (the applications' streams) has an
 * output and an input; call joins a call's far side, which the daemon that
 * holds it sets, with the user's output and input.  Each route takes only
 * the classes that belong to it (R6'), so that the seat's processes, which
 * may change routes, can neither record a call's far side nor speak into
 * it.
 *
 * A daemon's endpoint may be DORMANT (it opens when wanted) or SUSPENDED:
 * when a route uses it, audiod sends ENDPOINT_WANTED with a ticket and
 * writes to it only once the daemon answers ACTIVE with that ticket (N2).
 * One that does not open in time (2 s for a headset's media, 3 s for the
 * rest) leaves its route as it is, told TIMEOUT (N1); the compositor
 * chooses another.  When the media output goes away, the media streams
 * playing then are held (G5): not read until their client starts them
 * again or the compositor releases them.  The mixing into daemon endpoints
 * is p003's; here the media streams play on device:out only.
 */

#include "userland/base/audiod/audiod.h"
#include "userland/base/audiod/asrc.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pwd.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* The account bluetoothd runs as, and the node whose owner is the seat's user. */
#define ROUTE_BLUETOOTH_ACCOUNT	"_bluetooth"
#define ROUTE_SEAT_NODE		"/dev/gpu0"

/* How long an endpoint wanted may take to open: a headset's media, and the rest (ms). */
#define ROUTE_OPEN_MEDIA_MS	2000
#define ROUTE_OPEN_OTHER_MS	3000

/* How long the media output stays wanted after the last media stream stops (ms). */
#define ROUTE_MEDIA_GRACE_MS	5000

/* The volume an endpoint starts at until its daemon tells one (0..100, N5). */
#define ROUTE_VOLUME_START	50U

/*
 * The flow (p003): the periods a fill is averaged over, the error past
 * which the middle stage moves whole blocks (ms, M9), the fade of a
 * block moved (ms), the gain over a call (-12 dB, Q13), a chunk of a ring
 * converted at a time (frames), the mix's full scale, and the device's
 * fragments ahead of it while a call runs (M8, G4).
 */
#define ROUTE_FILL_PERIODS	8U
#define ROUTE_MIDDLE_MS		10.0
#define ROUTE_FADE_MS		2.0
#define ROUTE_CALL_GAIN		0.25
#define ROUTE_CHUNK_FRAMES	256U
#define ROUTE_MIX_SCALE		2147483648.0
#define ROUTE_CALL_FRAGMENTS	1U
#define ROUTE_FILL_FRAGMENTS	2U

/* The most frames of a master period the flow holds at once. */
#define ROUTE_PERIOD_MAX	4096U

/* One endpoint. */
struct route_endpoint {
	struct route_endpoint *next;
	struct audiod_client *owner;
	uint32_t id;
	uint32_t kind;
	uint32_t endpoint_class;
	uint32_t group;
	uint32_t format;
	uint32_t channels;
	uint32_t rate;
	uint32_t capacity_frames;
	uint32_t target_frames;
	uint32_t state;
	uint32_t flags;
	uint32_t latency_us;
	char key[AUDIOD_KEY_MAX];
	char name[AUDIOD_NAME_MAX];
	struct audiod_shm_header *shm;
	size_t shm_bytes;

	/* WANTED: the last ticket, whether it asked to open, when, whether the daemon answered ACTIVE, and whether audiod gave up. */
	uint32_t ticket;
	unsigned wanted;
	int64_t wanted_ns;
	unsigned open;
	unsigned gave_up;

	/* The endpoint's own volume (0..100). */
	uint32_t volume_left;
	uint32_t volume_right;
	uint32_t muted;

	/*
	 * The flow (p003): its converter (to the endpoint's rate for a sink,
	 * from it for a source), whether it is made, whether the ring was
	 * filled to its target (primed), the fill of the last periods (frames)
	 * and where the next goes, and the blocks the middle stage moved.
	 */
	struct asrc converter;
	unsigned converter_ready;
	unsigned primed;
	double fills[ROUTE_FILL_PERIODS];
	unsigned fill_next;
	unsigned fill_count;
	uint64_t moved;
};

/* The media route: output and input (0: none), why it last changed, and whether its output was lost (G5). */
struct route_media {
	uint32_t output;
	uint32_t input;
	uint32_t reason;
	unsigned lost;
};

/* The call route: the far group and its daemon, the user's output and input, whether set, and the daemon's state. */
struct route_call {
	unsigned set;
	uint32_t far_group;
	struct audiod_client *far_owner;
	uint32_t output;
	uint32_t input;
	uint32_t reason;
	uint32_t active;
	uint32_t audio_here;
};

static struct route_endpoint *route_endpoints;
static uint32_t route_next_id = AUDIOD_ENDPOINT_FIRST;
static struct route_media route_media;
static struct route_call route_call;

/* Whether media is held because a call is on the speaker (Q13), as last told. */
static unsigned route_call_held;
static int64_t route_media_last_ns;
static uid_t route_bluetooth_uid;
static int route_bluetooth_known;

static struct route_endpoint *route_find(uint32_t id);
static struct route_endpoint *route_internal(uint32_t id, uint32_t kind, uint32_t endpoint_class, const char *key, const char *name);
static int route_may_register(const struct audiod_client *client);
static int route_may_choose(const struct audiod_client *client);
static int route_fits(uint32_t route, uint32_t kind, const struct route_endpoint *endpoint);
static uint32_t route_default(uint32_t route, uint32_t kind);
static int route_add(struct audiod_client *client, const uint8_t *bytes, uint32_t length);
static int route_remove_request(struct audiod_client *client, const uint8_t *bytes, uint32_t length);
static int route_state(struct audiod_client *client, const uint8_t *bytes, uint32_t length);
static int route_failed(struct audiod_client *client, const uint8_t *bytes, uint32_t length);
static int route_volume_report(struct audiod_client *client, const uint8_t *bytes, uint32_t length);
static int route_set(struct audiod_client *client, const uint8_t *bytes, uint32_t length);
static int route_call_set(struct audiod_client *client, const uint8_t *bytes, uint32_t length);
static int route_call_clear(struct audiod_client *client);
static int route_call_state(struct audiod_client *client, const uint8_t *bytes, uint32_t length);
static int route_release(struct audiod_client *client);
static void route_list_endpoints(struct audiod_client *client, const struct audiod_header *request);
static void route_list_routes(struct audiod_client *client, const struct audiod_header *request);
static void route_remove(struct route_endpoint *endpoint, uint32_t reason, int tell_owner);
static void route_lost(uint32_t id, uint32_t reason);
static void route_give_up(struct route_endpoint *endpoint, uint32_t reason);
static void route_want(struct route_endpoint *endpoint, unsigned wanted, int64_t now_ns);
static unsigned route_desired(const struct route_endpoint *endpoint, int64_t now_ns);
static int route_media_in_use(int64_t now_ns);
static int route_capture_running(void);
static void route_hold_media(void);
static void route_held_changed(unsigned held, uint32_t reason);
static uint32_t route_volume_target(void);
static void route_info(const struct route_endpoint *endpoint, struct audiod_endpoint_info *info, int gone);
static void route_broadcast_endpoint(const struct route_endpoint *endpoint, int gone);
static void route_broadcast_route(uint32_t route);
static void route_fill_route(uint32_t route, struct audiod_route_info *info);
static void route_broadcast_volume(void);
static void route_copy_text(char *target, const char *source, size_t size);
static int route_flowing(void);
static struct route_endpoint *route_far(uint32_t kind);
static int route_flow_ready(struct route_endpoint *endpoint, uint32_t rate_in, uint32_t rate_out);
static double route_fill_average(struct route_endpoint *endpoint, double fill);
static void route_fill_reset(struct route_endpoint *endpoint);
static int route_write_sink(struct route_endpoint *endpoint, const float *stereo, uint32_t frames);
static int route_read_source(struct route_endpoint *endpoint, float *stereo, uint32_t frames);
static void route_ring_put(struct route_endpoint *endpoint, uint64_t position, const float *samples, size_t frames);
static void route_ring_get(struct route_endpoint *endpoint, uint64_t position, float *samples, size_t frames);
static void route_broken(struct route_endpoint *endpoint);
static void route_volume_gain(const struct route_endpoint *endpoint, float gain[2]);
static void route_ring_fade(struct route_endpoint *endpoint, uint64_t position, size_t frames, int rising);
static void route_call_hold(void);

/*
 * Starts the endpoints and routes: device:out and device:in when the kernel
 * has a sound device (and records), media and call on them, and the
 * Bluetooth daemon's account.
 */
void
audiod_route_init(
	void)
{
	struct passwd *account;
	const char *test_uid;

	/* The kernel's device, audiod's own endpoints. */
	if (audiod_device.dsp >= 0) {
		(void)route_internal(AUDIOD_ENDPOINT_DEVICE_OUT, AUDIOD_KIND_SINK, AUDIOD_CLASS_SPEAKER, "dev:dsp0:out", "Speaker");
		if (audiod_device.capture)
			(void)route_internal(AUDIOD_ENDPOINT_DEVICE_IN, AUDIOD_KIND_SOURCE, AUDIOD_CLASS_MIC, "dev:dsp0:in", "Microphone");
	}

	/* The routes on their defaults. */
	memset(&route_media, 0, sizeof(route_media));
	memset(&route_call, 0, sizeof(route_call));
	route_media.output = route_default(AUDIOD_ROUTE_MEDIA, AUDIOD_KIND_SINK);
	route_media.input = route_default(AUDIOD_ROUTE_MEDIA, AUDIOD_KIND_SOURCE);
	route_call.output = route_default(AUDIOD_ROUTE_CALL, AUDIOD_KIND_SINK);
	route_call.input = route_default(AUDIOD_ROUTE_CALL, AUDIOD_KIND_SOURCE);

	/* The Bluetooth daemon's account (a host test may name another uid, compiled in only there). */
	account = getpwnam(ROUTE_BLUETOOTH_ACCOUNT);
	if (account != NULL) {
		route_bluetooth_uid = account->pw_uid;
		route_bluetooth_known = 1;
	}
#ifdef AUDIOD_HOST_TEST
	test_uid = getenv("AUDIOD_TEST_BLUETOOTH_UID");
	if (test_uid != NULL) {
		route_bluetooth_uid = (uid_t)strtoul(test_uid, NULL, 10);
		route_bluetooth_known = 1;
	}
#else
	test_uid = NULL;
	(void)test_uid;
#endif
}

/*
 * Carries out a version 2 request of the endpoints and routes; answers it
 * itself.  Returns 1 when the request was one of these, 0 for any other.
 */
int
audiod_route_request(
	struct audiod_client *client,
	const uint8_t *bytes,
	uint32_t length)
{
	struct audiod_header header;
	int error;

	/* Which request. */
	memcpy(&header, bytes, sizeof(header));
	if (header.type < AUDIOD_ENDPOINT_ADD || header.type > AUDIOD_MEDIA_RELEASE)
		return 0;

	/* Version 2's requests only. */
	if (client->version < AUDIOD_PROTOCOL_VERSION_2) {
		audiod_send_result(client, &header, EINVAL);
		return 1;
	}

	/* Each request. */
	error = 0;
	switch (header.type) {
	case AUDIOD_ENDPOINT_ADD:
		error = route_add(client, bytes, length);
		if (error == 0)
			return 1;
		break;
	case AUDIOD_ENDPOINT_REMOVE:
		error = route_remove_request(client, bytes, length);
		break;
	case AUDIOD_ENDPOINT_STATE:
		error = route_state(client, bytes, length);
		break;
	case AUDIOD_ENDPOINT_FAILED:
		error = route_failed(client, bytes, length);
		break;
	case AUDIOD_ENDPOINT_VOLUME_REPORT:
		error = route_volume_report(client, bytes, length);
		break;
	case AUDIOD_ENDPOINT_LIST:
		route_list_endpoints(client, &header);
		return 1;
	case AUDIOD_ROUTE_SET:
		error = route_set(client, bytes, length);
		break;
	case AUDIOD_ROUTE_LIST:
		route_list_routes(client, &header);
		return 1;
	case AUDIOD_CALL_SET:
		error = route_call_set(client, bytes, length);
		break;
	case AUDIOD_CALL_CLEAR:
		error = route_call_clear(client);
		break;
	case AUDIOD_CALL_STATE:
		error = route_call_state(client, bytes, length);
		break;
	case AUDIOD_MEDIA_RELEASE:
		error = route_release(client);
		break;
	default:
		error = EINVAL;
		break;
	}

	/* Answered. */
	audiod_send_result(client, &header, error);
	return 1;
}

/* Takes away what a client that went away held: its endpoints, and its call. */
void
audiod_route_client_gone(
	struct audiod_client *client)
{
	struct route_endpoint *endpoint;
	struct route_endpoint *next;

	/* Its call. */
	if (route_call.set && route_call.far_owner == client) {
		route_call.set = 0U;
		route_call.far_owner = NULL;
		route_call.reason = AUDIOD_REASON_REMOVED;
		route_broadcast_route(AUDIOD_ROUTE_CALL);
	}

	/* Its endpoints. */
	for (endpoint = route_endpoints; endpoint != NULL; endpoint = next) {
		next = endpoint->next;
		if (endpoint->owner == client)
			route_remove(endpoint, AUDIOD_REASON_REMOVED, 0);
	}
}

/*
 * Looks at the routes' endpoints each round: asks daemons to open the ones
 * used and close the ones no longer used, and gives up the ones that did
 * not open in time.
 */
void
audiod_route_tick(
	int64_t now_ns)
{
	struct route_endpoint *endpoint;
	int64_t limit_ns;
	unsigned desired;

	/* Each daemon's endpoint. */
	for (endpoint = route_endpoints; endpoint != NULL; endpoint = endpoint->next) {
		if (endpoint->owner == NULL)
			continue;

		/* Wanted or not. */
		desired = route_desired(endpoint, now_ns);
		if (desired && !endpoint->open && !endpoint->wanted)
			route_want(endpoint, 1U, now_ns);
		if (!desired && endpoint->wanted)
			route_want(endpoint, 0U, now_ns);

		/* Not open in time: the route is left as it is (N1). */
		limit_ns = (int64_t)ROUTE_OPEN_OTHER_MS * 1000000;
		if (endpoint->endpoint_class == AUDIOD_CLASS_HEADSET_MEDIA)
			limit_ns = (int64_t)ROUTE_OPEN_MEDIA_MS * 1000000;
		if (endpoint->wanted && !endpoint->open && now_ns - endpoint->wanted_ns >= limit_ns)
			route_give_up(endpoint, AUDIOD_REASON_TIMEOUT);
	}
}

/* Tells how long the poll may wait before an endpoint's time to open runs out (ms; -1 none). */
int
audiod_route_timeout_ms(
	int64_t now_ns)
{
	struct route_endpoint *endpoint;
	int64_t limit_ns;
	int64_t left_ns;
	int64_t nearest_ns;

	/* The nearest deadline of an endpoint wanted and not open. */
	nearest_ns = -1;
	for (endpoint = route_endpoints; endpoint != NULL; endpoint = endpoint->next) {
		if (!endpoint->wanted || endpoint->open)
			continue;
		limit_ns = (int64_t)ROUTE_OPEN_OTHER_MS * 1000000;
		if (endpoint->endpoint_class == AUDIOD_CLASS_HEADSET_MEDIA)
			limit_ns = (int64_t)ROUTE_OPEN_MEDIA_MS * 1000000;
		left_ns = endpoint->wanted_ns + limit_ns - now_ns;
		if (left_ns < 0)
			left_ns = 0;
		if (nearest_ns < 0 || left_ns < nearest_ns)
			nearest_ns = left_ns;
	}

	/* Waiting on the media grace too, a second at a time. */
	if (route_media_last_ns != 0 && (nearest_ns < 0 || nearest_ns > 1000000000))
		nearest_ns = 1000000000;

	/* None. */
	if (nearest_ns < 0)
		return -1;

	/* Succeeded: whole milliseconds, at least one. */
	return (int)(nearest_ns / 1000000) + 1;
}

/* Tells whether the media streams play on device:out now (its route's output is the device's). */
int
audiod_route_media_to_device(
	void)
{
	/* device:out exists and is the media output. */
	if (route_media.output != AUDIOD_ENDPOINT_DEVICE_OUT)
		return 0;

	/* Succeeded: to the device. */
	return route_find(AUDIOD_ENDPOINT_DEVICE_OUT) != NULL;
}

/*
 * Tells whether a client may record now (N13): while a call is set, the
 * microphone carries the user's side of it, so only the seat's user and
 * root (and the daemon) record.
 */
int
audiod_route_capture_allowed(
	const struct audiod_client *client)
{
	int allowed;

	/* No call: anyone, as before. */
	if (!route_call.set)
		return 1;

	/* Succeeded or not: the seat's user, root or the daemon. */
	allowed = route_may_choose(client);
	return allowed;
}

/* Tells whether there is an output at all (WELCOME's device, every version, N19). */
int
audiod_route_output_exists(
	void)
{
	struct route_endpoint *endpoint;

	/* Any sink that streams or opens when wanted. */
	for (endpoint = route_endpoints; endpoint != NULL; endpoint = endpoint->next) {
		if (endpoint->kind == AUDIOD_KIND_SINK)
			return 1;
	}

	/* None. */
	return 0;
}

/*
 * A stream started: one held since its output was lost is released by its
 * client's own start after a stop (G5), and the media output's grace runs
 * from now.
 */
void
audiod_route_stream_started(
	struct audiod_stream *stream)
{
	struct audiod_client *client;
	struct audiod_stream *other;
	unsigned held;

	/* Released. */
	stream->held = 0U;

	/* Were others still held? */
	held = 0U;
	for (client = audiod_clients; client != NULL; client = client->next) {
		for (other = client->streams; other != NULL; other = other->next) {
			if (other->held)
				held = 1U;
		}
	}

	/* None left: the hold is over. */
	if (!held && route_media.lost) {
		route_media.lost = 0U;
		route_held_changed(0U, AUDIOD_REASON_NONE);
	}
}

/* Gives the volume of the output the volume keys act on now (N5); 1 when it is an endpoint's, 0 for the device's. */
int
audiod_route_volume_get(
	uint32_t *left,
	uint32_t *right,
	uint32_t *muted)
{
	struct route_endpoint *endpoint;

	/* A daemon's endpoint has its own. */
	endpoint = route_find(route_volume_target());
	if (endpoint == NULL || endpoint->owner == NULL)
		return 0;

	/* Succeeded: the endpoint's. */
	*left = endpoint->volume_left;
	*right = endpoint->volume_right;
	*muted = endpoint->muted;
	return 1;
}

/*
 * Sets the volume of the output the volume keys act on now (the call's
 * while a call is set, else the media's).  Returns 1 when it was an
 * endpoint's (its daemon told when it takes it), 0 when it is the device's
 * (the caller sets that).
 */
int
audiod_route_volume_set(
	uint32_t left,
	uint32_t right,
	uint32_t muted)
{
	struct audiod_endpoint_volume volume;
	struct route_endpoint *endpoint;

	/* A daemon's endpoint. */
	endpoint = route_find(route_volume_target());
	if (endpoint == NULL || endpoint->owner == NULL)
		return 0;

	/* Its own volume (audiod applies it in p003 unless the daemon takes it). */
	endpoint->volume_left = left;
	endpoint->volume_right = right;
	endpoint->muted = muted;

	/* The daemon that takes the volume is told. */
	if ((endpoint->flags & AUDIOD_ENDPOINT_FLAG_VOLUME) != 0U) {
		memset(&volume, 0, sizeof(volume));
		volume.header.type = AUDIOD_ENDPOINT_VOLUME;
		volume.header.length = sizeof(volume);
		volume.id = endpoint->id;
		volume.left = left;
		volume.right = right;
		volume.muted = muted;
		audiod_send(endpoint->owner, &volume, sizeof(volume), -1);
	}

	/* Succeeded: the endpoint's. */
	return 1;
}

/* Tells a client the volume of the output the keys act on (VOLUME_CHANGED). */
void
audiod_route_send_volume(
	struct audiod_client *client)
{
	struct audiod_volume volume;
	int endpoint;

	/* The endpoint's, or the device's. */
	memset(&volume, 0, sizeof(volume));
	volume.header.type = AUDIOD_VOLUME_CHANGED;
	volume.header.length = sizeof(volume);
	endpoint = audiod_route_volume_get(&volume.left, &volume.right, &volume.muted);
	if (!endpoint)
		audiod_device_get_volume(&audiod_device, &volume.left, &volume.right, &volume.muted);

	/* Sent. */
	audiod_send(client, &volume, sizeof(volume), -1);
}

/* Finds an endpoint by its id. */
static struct route_endpoint *
route_find(
	uint32_t id)
{
	struct route_endpoint *endpoint;

	/* Each one. */
	for (endpoint = route_endpoints; endpoint != NULL; endpoint = endpoint->next) {
		if (endpoint->id == id)
			return endpoint;
	}

	/* None. */
	return NULL;
}

/* Adds one of audiod's own endpoints (the device's), always open. */
static struct route_endpoint *
route_internal(
	uint32_t id,
	uint32_t kind,
	uint32_t endpoint_class,
	const char *key,
	const char *name)
{
	struct route_endpoint *endpoint;

	/* The record. */
	endpoint = calloc(1U, sizeof(*endpoint));
	if (endpoint == NULL)
		return NULL;
	endpoint->id = id;
	endpoint->kind = kind;
	endpoint->endpoint_class = endpoint_class;
	endpoint->format = audiod_device.format;
	endpoint->channels = audiod_device.channels;
	endpoint->rate = audiod_device.rate;
	endpoint->state = AUDIOD_ENDPOINT_ACTIVE;
	endpoint->open = 1U;
	route_copy_text(endpoint->key, key, sizeof(endpoint->key));
	route_copy_text(endpoint->name, name, sizeof(endpoint->name));

	/* Listed. */
	endpoint->next = route_endpoints;
	route_endpoints = endpoint;
	return endpoint;
}

/* Tells whether a client may register endpoints and set calls: root or the Bluetooth daemon (E3). */
static int
route_may_register(
	const struct audiod_client *client)
{
	/* A peer of known uid. */
	if (!client->uid_known)
		return 0;
	if (client->uid == 0)
		return 1;

	/* Succeeded or not: the Bluetooth daemon's account. */
	return route_bluetooth_known && client->uid == route_bluetooth_uid;
}

/*
 * Tells whether a client may choose routes and release held media: root,
 * the Bluetooth daemon, or the seat's user (the owner sessiond gave the
 * display node, as volumed asks).  The seat's applications share that uid:
 * the routes' classes (route_fits) are what keeps them from a call.
 */
static int
route_may_choose(
	const struct audiod_client *client)
{
	struct stat status;
	const char *seat;
	int allowed;
	int error;

	/* Root and the daemon. */
	allowed = route_may_register(client);
	if (allowed)
		return 1;
	if (!client->uid_known)
		return 0;

	/* The seat's user (a host test may name it, compiled in only there). */
#ifdef AUDIOD_HOST_TEST
	seat = getenv("AUDIOD_TEST_SEAT_UID");
	if (seat != NULL)
		return client->uid == (uid_t)strtoul(seat, NULL, 10);
#else
	seat = NULL;
	(void)seat;
#endif
	error = stat(ROUTE_SEAT_NODE, &status);
	if (error != 0)
		return 0;

	/* Succeeded or not. */
	return status.st_uid == client->uid;
}

/*
 * Tells whether an endpoint fits a route's place (R6'): media's output a
 * speaker or a headset, its input a microphone or a headset's voice; the
 * call's user side a speaker, a microphone or a headset's voice.  A call's
 * far side (phone, modem) fits no route: only CALL_SET takes it.
 */
static int
route_fits(
	uint32_t route,
	uint32_t kind,
	const struct route_endpoint *endpoint)
{
	uint32_t endpoint_class;

	/* The right kind. */
	if (endpoint->kind != kind)
		return 0;
	endpoint_class = endpoint->endpoint_class;

	/* A headset's voice fits every place. */
	if (endpoint_class == AUDIOD_CLASS_HEADSET_VOICE && (route == AUDIOD_ROUTE_MEDIA || route == AUDIOD_ROUTE_CALL))
		return 1;

	/* An input: the microphone. */
	if (kind == AUDIOD_KIND_SOURCE && (route == AUDIOD_ROUTE_MEDIA || route == AUDIOD_ROUTE_CALL))
		return endpoint_class == AUDIOD_CLASS_MIC;

	/* Media's output: the speaker or a headset's media. */
	if (route == AUDIOD_ROUTE_MEDIA && endpoint_class == AUDIOD_CLASS_HEADSET_MEDIA)
		return 1;

	/* The speaker, for media and the call. */
	if (route == AUDIOD_ROUTE_MEDIA || route == AUDIOD_ROUTE_CALL)
		return endpoint_class == AUDIOD_CLASS_SPEAKER;

	/* No other route. */
	return 0;
}

/*
 * Gives a route's default endpoint (0 for none): the device's, and for the
 * call a headset's voice first (G1, while no compositor chooses).
 */
static uint32_t
route_default(
	uint32_t route,
	uint32_t kind)
{
	struct route_endpoint *endpoint;
	uint32_t device;

	/* A headset's voice for a call. */
	if (route == AUDIOD_ROUTE_CALL) {
		for (endpoint = route_endpoints; endpoint != NULL; endpoint = endpoint->next) {
			if (endpoint->kind == kind && endpoint->endpoint_class == AUDIOD_CLASS_HEADSET_VOICE)
				return endpoint->id;
		}
	}

	/* The device's. */
	device = AUDIOD_ENDPOINT_DEVICE_OUT;
	if (kind == AUDIOD_KIND_SOURCE)
		device = AUDIOD_ENDPOINT_DEVICE_IN;
	endpoint = route_find(device);
	if (endpoint != NULL)
		return device;

	/* None. */
	return 0U;
}

/*
 * Registers a daemon's endpoint (ENDPOINT_ADD): checked, its ring made and
 * sent with ENDPOINT_ADDED.  An endpoint of the same key and kind (the
 * daemon restarted) replaces the old one.  Returns 0 (answered) or an
 * error for the caller to answer.
 */
static int
route_add(
	struct audiod_client *client,
	const uint8_t *bytes,
	uint32_t length)
{
	struct audiod_endpoint_add request;
	struct audiod_endpoint_added added;
	struct route_endpoint *endpoint;
	struct route_endpoint *other;
	struct route_endpoint *next;
	uint32_t capacity;
	int allowed;
	int same;
	int error;
	int fd;

	/* A whole request from the daemon's account. */
	if (length != sizeof(request))
		return EINVAL;
	allowed = route_may_register(client);
	if (!allowed)
		return EACCES;
	memcpy(&request, bytes, sizeof(request));
	request.key[AUDIOD_KEY_MAX - 1U] = '\0';
	request.name[AUDIOD_NAME_MAX - 1U] = '\0';

	/* What audiod can stream. */
	if (request.kind > AUDIOD_KIND_SOURCE || request.reserved != 0U)
		return EINVAL;
	if (request.endpoint_class < AUDIOD_CLASS_SPEAKER || request.endpoint_class > AUDIOD_CLASS_MODEM_CALL)
		return EINVAL;
	if (request.format != AUDIOD_FORMAT_S16_LE || request.channels < 1U || request.channels > 2U)
		return EINVAL;
	if (request.rate < 8000U || request.rate > 48000U)
		return EINVAL;
	if (request.state < AUDIOD_ENDPOINT_ACTIVE || request.state > AUDIOD_ENDPOINT_DORMANT)
		return EINVAL;

	/* A ring audiod can hold, and a target under half of it. */
	capacity = request.buffer_frames;
	if (capacity == 0U || capacity > AUDIOD_BUFFER_MAX)
		return EINVAL;
	if (request.target_frames >= capacity / 2U)
		return EINVAL;

	/* The daemon restarted: its old endpoint of the key goes (N15). */
	for (other = route_endpoints; other != NULL; other = next) {
		next = other->next;
		same = strcmp(other->key, request.key);
		if (other->owner != NULL && same == 0 && other->kind == request.kind)
			route_remove(other, AUDIOD_REASON_REMOVED, 1);
	}

	/* The record and its ring. */
	endpoint = calloc(1U, sizeof(*endpoint));
	if (endpoint == NULL)
		return ENOMEM;
	error = audiod_shm_make(request.format, request.channels, request.rate, capacity, 0U, &fd, &endpoint->shm,
	    &endpoint->shm_bytes);
	if (error != 0) {
		free(endpoint);
		return error;
	}

	/* What it is. */
	endpoint->owner = client;
	endpoint->id = route_next_id;
	route_next_id++;
	endpoint->kind = request.kind;
	endpoint->endpoint_class = request.endpoint_class;
	endpoint->group = request.group;
	endpoint->format = request.format;
	endpoint->channels = request.channels;
	endpoint->rate = request.rate;
	endpoint->capacity_frames = capacity;
	endpoint->target_frames = request.target_frames;
	endpoint->state = request.state;
	endpoint->flags = request.flags;
	endpoint->latency_us = request.latency_us;
	endpoint->open = request.state == AUDIOD_ENDPOINT_ACTIVE;
	endpoint->volume_left = ROUTE_VOLUME_START;
	endpoint->volume_right = ROUTE_VOLUME_START;
	route_copy_text(endpoint->key, request.key, sizeof(endpoint->key));
	route_copy_text(endpoint->name, request.name, sizeof(endpoint->name));
	endpoint->next = route_endpoints;
	route_endpoints = endpoint;

	/* The answer with the ring. */
	memset(&added, 0, sizeof(added));
	added.header.type = AUDIOD_ENDPOINT_ADDED;
	added.header.length = sizeof(added);
	added.header.serial = request.header.serial;
	added.header.stream = endpoint->id;
	added.id = endpoint->id;
	added.shm_bytes = (uint32_t)endpoint->shm_bytes;
	added.capacity_frames = capacity;
	audiod_send(client, &added, sizeof(added), fd);
	(void)close(fd);

	/* A call without a headset's voice takes it now as its default (G1). */
	if (endpoint->endpoint_class == AUDIOD_CLASS_HEADSET_VOICE && !route_call.set) {
		route_call.output = route_default(AUDIOD_ROUTE_CALL, AUDIOD_KIND_SINK);
		route_call.input = route_default(AUDIOD_ROUTE_CALL, AUDIOD_KIND_SOURCE);
		route_broadcast_route(AUDIOD_ROUTE_CALL);
	}

	/* Succeeded: told to the subscribers. */
	route_broadcast_endpoint(endpoint, 0);
	return 0;
}

/* Takes a daemon's endpoint away at its word (ENDPOINT_REMOVE). */
static int
route_remove_request(
	struct audiod_client *client,
	const uint8_t *bytes,
	uint32_t length)
{
	struct audiod_endpoint_state request;
	struct route_endpoint *endpoint;

	/* A whole request about its own endpoint. */
	if (length != sizeof(request))
		return EINVAL;
	memcpy(&request, bytes, sizeof(request));
	endpoint = route_find(request.id);
	if (endpoint == NULL || endpoint->owner != client)
		return ENOENT;

	/* Succeeded: gone. */
	route_remove(endpoint, AUDIOD_REASON_REMOVED, 0);
	return 0;
}

/*
 * Takes a daemon's word on its endpoint's state (ENDPOINT_STATE): ACTIVE
 * opens it only with the ticket of the last WANTED (N2), SUSPENDED and
 * DORMANT close it; its latency now.
 */
static int
route_state(
	struct audiod_client *client,
	const uint8_t *bytes,
	uint32_t length)
{
	struct audiod_endpoint_state request;
	struct route_endpoint *endpoint;

	/* A whole request about its own endpoint, a state that exists. */
	if (length != sizeof(request))
		return EINVAL;
	memcpy(&request, bytes, sizeof(request));
	endpoint = route_find(request.id);
	if (endpoint == NULL || endpoint->owner != client)
		return ENOENT;
	if (request.state < AUDIOD_ENDPOINT_ACTIVE || request.state > AUDIOD_ENDPOINT_DORMANT)
		return EINVAL;

	/* The state and the latency; opened, the ring starts from its target of silence (N3, C3 (a)). */
	endpoint->state = request.state;
	endpoint->latency_us = request.latency_us;
	endpoint->open = 0U;
	endpoint->primed = 0U;
	route_fill_reset(endpoint);
	if (endpoint->converter_ready)
		asrc_reset(&endpoint->converter);
	if (request.state == AUDIOD_ENDPOINT_ACTIVE && request.ticket == endpoint->ticket)
		endpoint->open = 1U;

	/* A daemon's own change lets audiod try it again. */
	endpoint->gave_up = 0U;

	/* Succeeded: told. */
	route_broadcast_endpoint(endpoint, 0);
	return 0;
}

/* Takes a daemon's word that its endpoint could not open for a ticket (ENDPOINT_FAILED): its route is told FAILED (N1). */
static int
route_failed(
	struct audiod_client *client,
	const uint8_t *bytes,
	uint32_t length)
{
	struct audiod_endpoint_state request;
	struct route_endpoint *endpoint;

	/* A whole request about its own endpoint, for the ticket in force. */
	if (length != sizeof(request))
		return EINVAL;
	memcpy(&request, bytes, sizeof(request));
	endpoint = route_find(request.id);
	if (endpoint == NULL || endpoint->owner != client)
		return ENOENT;
	if (request.ticket != endpoint->ticket)
		return 0;

	/* Succeeded: given up. */
	route_give_up(endpoint, AUDIOD_REASON_FAILED);
	return 0;
}

/* Takes a daemon's endpoint's volume (a headset's buttons, N5). */
static int
route_volume_report(
	struct audiod_client *client,
	const uint8_t *bytes,
	uint32_t length)
{
	struct audiod_endpoint_volume request;
	struct route_endpoint *endpoint;
	uint32_t target;

	/* A whole request about its own endpoint, 0..100. */
	if (length != sizeof(request))
		return EINVAL;
	memcpy(&request, bytes, sizeof(request));
	endpoint = route_find(request.id);
	if (endpoint == NULL || endpoint->owner != client)
		return ENOENT;
	if (request.left > 100U || request.right > 100U)
		return EINVAL;

	/* Its volume. */
	endpoint->volume_left = request.left;
	endpoint->volume_right = request.right;
	endpoint->muted = request.muted != 0U;

	/* Succeeded: the keys' output told again when it is this one. */
	target = route_volume_target();
	if (target == endpoint->id)
		route_broadcast_volume();
	return 0;
}

/* Chooses a route's output and input (ROUTE_SET), each of the route's classes (0: the default). */
static int
route_set(
	struct audiod_client *client,
	const uint8_t *bytes,
	uint32_t length)
{
	struct audiod_route_set request;
	struct route_endpoint *output;
	struct route_endpoint *input;
	uint32_t output_id;
	uint32_t input_id;
	int allowed;
	int fits;

	/* A whole request from the seat, root or the daemon. */
	if (length != sizeof(request))
		return EINVAL;
	allowed = route_may_choose(client);
	if (!allowed)
		return EACCES;
	memcpy(&request, bytes, sizeof(request));
	if (request.route != AUDIOD_ROUTE_MEDIA && request.route != AUDIOD_ROUTE_CALL)
		return EINVAL;

	/* The endpoints, or the defaults. */
	output_id = request.output;
	if (output_id == AUDIOD_ENDPOINT_DEFAULT)
		output_id = route_default(request.route, AUDIOD_KIND_SINK);
	input_id = request.input;
	if (input_id == AUDIOD_ENDPOINT_DEFAULT)
		input_id = route_default(request.route, AUDIOD_KIND_SOURCE);

	/* Each of the route's classes (R6'). */
	output = route_find(output_id);
	if (output_id != 0U) {
		if (output == NULL)
			return ENOENT;
		fits = route_fits(request.route, AUDIOD_KIND_SINK, output);
		if (!fits)
			return EINVAL;
		output->gave_up = 0U;
	}

	/* The input likewise. */
	input = route_find(input_id);
	if (input_id != 0U) {
		if (input == NULL)
			return ENOENT;
		fits = route_fits(request.route, AUDIOD_KIND_SOURCE, input);
		if (!fits)
			return EINVAL;
		input->gave_up = 0U;
	}

	/* The route; held media stays held (G5). */
	if (request.route == AUDIOD_ROUTE_MEDIA) {
		route_media.output = output_id;
		route_media.input = input_id;
		route_media.reason = AUDIOD_REASON_NONE;
	} else {
		route_call.output = output_id;
		route_call.input = input_id;
		route_call.reason = AUDIOD_REASON_NONE;
	}

	/* Succeeded: told, and the keys' volume may be another output's now. */
	route_broadcast_route(request.route);
	route_broadcast_volume();
	return 0;
}

/*
 * Sets a call (CALL_SET): its far group, of the caller's own phone or modem
 * endpoints, joined with the user's side.  A second call is EBUSY.
 */
static int
route_call_set(
	struct audiod_client *client,
	const uint8_t *bytes,
	uint32_t length)
{
	struct audiod_call request;
	struct route_endpoint *endpoint;
	struct route_endpoint *chosen;
	unsigned sinks;
	unsigned sources;
	int allowed;

	/* A whole request from a daemon, no call yet. */
	if (length != sizeof(request))
		return EINVAL;
	allowed = route_may_register(client);
	if (!allowed)
		return EACCES;
	if (route_call.set)
		return EBUSY;
	memcpy(&request, bytes, sizeof(request));

	/* The group: the caller's phone or modem endpoints, a sink and a source (N14). */
	sinks = 0U;
	sources = 0U;
	for (endpoint = route_endpoints; endpoint != NULL; endpoint = endpoint->next) {
		if (endpoint->owner != client || endpoint->group != request.far_group)
			continue;
		if (endpoint->endpoint_class != AUDIOD_CLASS_PHONE_CALL && endpoint->endpoint_class != AUDIOD_CLASS_MODEM_CALL)
			return EINVAL;
		if (endpoint->kind == AUDIOD_KIND_SINK)
			sinks++;
		else
			sources++;
	}

	/* Both ways. */
	if (sinks == 0U || sources == 0U)
		return EINVAL;

	/* The call, on the user's side chosen or the defaults. */
	route_call.set = 1U;
	route_call.far_group = request.far_group;
	route_call.far_owner = client;
	route_call.active = 1U;
	route_call.audio_here = 1U;
	route_call.reason = AUDIOD_REASON_NONE;
	chosen = route_find(route_call.output);
	if (chosen == NULL)
		route_call.output = route_default(AUDIOD_ROUTE_CALL, AUDIOD_KIND_SINK);
	chosen = route_find(route_call.input);
	if (chosen == NULL)
		route_call.input = route_default(AUDIOD_ROUTE_CALL, AUDIOD_KIND_SOURCE);

	/* Succeeded: told; the keys' volume is the call's now (N5). */
	route_broadcast_route(AUDIOD_ROUTE_CALL);
	route_broadcast_volume();
	return 0;
}

/* Clears the call (CALL_CLEAR), by the daemon that set it. */
static int
route_call_clear(
	struct audiod_client *client)
{
	/* The call's daemon. */
	if (!route_call.set)
		return ENOENT;
	if (route_call.far_owner != client)
		return EACCES;

	/* Cleared. */
	route_call.set = 0U;
	route_call.far_owner = NULL;
	route_call.reason = AUDIOD_REASON_NONE;

	/* Succeeded: told; the keys' volume is the media's again. */
	route_broadcast_route(AUDIOD_ROUTE_CALL);
	route_broadcast_volume();
	return 0;
}

/* Records the call's state from its daemon (CALL_STATE: recorded and told, G3, N12). */
static int
route_call_state(
	struct audiod_client *client,
	const uint8_t *bytes,
	uint32_t length)
{
	struct audiod_call request;

	/* A whole request from the call's daemon. */
	if (length != sizeof(request))
		return EINVAL;
	if (!route_call.set)
		return ENOENT;
	if (route_call.far_owner != client)
		return EACCES;
	memcpy(&request, bytes, sizeof(request));

	/* Recorded. */
	route_call.active = request.active != 0U;
	route_call.audio_here = request.audio_here != 0U;

	/* Succeeded: told. */
	route_broadcast_route(AUDIOD_ROUTE_CALL);
	return 0;
}

/* Releases the media held since its output was lost (MEDIA_RELEASE: the user pressed play, G5). */
static int
route_release(
	struct audiod_client *client)
{
	struct audiod_client *each;
	struct audiod_stream *stream;
	int allowed;

	/* The seat, root or the daemon. */
	allowed = route_may_choose(client);
	if (!allowed)
		return EACCES;

	/* Every stream. */
	for (each = audiod_clients; each != NULL; each = each->next) {
		for (stream = each->streams; stream != NULL; stream = stream->next)
			stream->held = 0U;
	}

	/* The hold is over. */
	if (route_media.lost) {
		route_media.lost = 0U;
		route_held_changed(0U, AUDIOD_REASON_NONE);
	}

	/* Succeeded. */
	return 0;
}

/* Answers ENDPOINT_LIST: an ENDPOINT_INFO for each endpoint, then DONE. */
static void
route_list_endpoints(
	struct audiod_client *client,
	const struct audiod_header *request)
{
	struct audiod_endpoint_info info;
	struct route_endpoint *endpoint;

	/* Each endpoint. */
	for (endpoint = route_endpoints; endpoint != NULL; endpoint = endpoint->next) {
		route_info(endpoint, &info, 0);
		info.header.serial = request->serial;
		audiod_send(client, &info, sizeof(info), -1);
	}

	/* The end of the list. */
	audiod_send_result(client, request, 0);
}

/* Answers ROUTE_LIST: media's and the call's ROUTE_INFO, then DONE. */
static void
route_list_routes(
	struct audiod_client *client,
	const struct audiod_header *request)
{
	struct audiod_route_info info;

	/* Media. */
	route_fill_route(AUDIOD_ROUTE_MEDIA, &info);
	info.header.serial = request->serial;
	audiod_send(client, &info, sizeof(info), -1);

	/* The call. */
	route_fill_route(AUDIOD_ROUTE_CALL, &info);
	info.header.serial = request->serial;
	audiod_send(client, &info, sizeof(info), -1);

	/* The end of the list. */
	audiod_send_result(client, request, 0);
}

/*
 * Takes an endpoint away: its routes move as design 1.2 says (the media
 * output lost holds the media, G5; the call's user side goes to the
 * defaults; the call's far side ends the call), its ring is unmapped, and
 * the subscribers (and, when audiod took it, its owner) are told.
 */
static void
route_remove(
	struct route_endpoint *endpoint,
	uint32_t reason,
	int tell_owner)
{
	struct audiod_endpoint_removed removed;
	struct route_endpoint **link;

	/* Off the list. */
	for (link = &route_endpoints; *link != NULL; link = &(*link)->next) {
		if (*link == endpoint) {
			*link = endpoint->next;
			break;
		}
	}

	/* The call whose far side it was ends. */
	if (route_call.set && endpoint->owner == route_call.far_owner && endpoint->group == route_call.far_group &&
	    (endpoint->endpoint_class == AUDIOD_CLASS_PHONE_CALL || endpoint->endpoint_class == AUDIOD_CLASS_MODEM_CALL)) {
		route_call.set = 0U;
		route_call.far_owner = NULL;
		route_call.reason = reason;
		route_broadcast_route(AUDIOD_ROUTE_CALL);
	}

	/* The routes that used it. */
	route_lost(endpoint->id, reason);

	/* The owner, when audiod took it away. */
	if (tell_owner && endpoint->owner != NULL) {
		memset(&removed, 0, sizeof(removed));
		removed.header.type = AUDIOD_ENDPOINT_REMOVED;
		removed.header.length = sizeof(removed);
		removed.id = endpoint->id;
		removed.reason = reason;
		audiod_send(endpoint->owner, &removed, sizeof(removed), -1);
	}

	/* The subscribers, then the record. */
	route_broadcast_endpoint(endpoint, 1);
	if (endpoint->converter_ready)
		asrc_free(&endpoint->converter);
	if (endpoint->shm != NULL)
		(void)munmap(endpoint->shm, endpoint->shm_bytes);
	free(endpoint);
}

/* Moves the routes off an endpoint that went away. */
static void
route_lost(
	uint32_t id,
	uint32_t reason)
{
	/* The media output: none, and the media playing held (G5). */
	if (route_media.output == id) {
		route_media.output = 0U;
		route_media.reason = AUDIOD_REASON_OUTPUT_LOST;
		route_hold_media();
		route_broadcast_route(AUDIOD_ROUTE_MEDIA);
	}

	/* The media input: the default. */
	if (route_media.input == id) {
		route_media.input = route_default(AUDIOD_ROUTE_MEDIA, AUDIOD_KIND_SOURCE);
		route_media.reason = reason;
		route_broadcast_route(AUDIOD_ROUTE_MEDIA);
	}

	/* The call's user side: the defaults, the call keeps its sound (G5). */
	if (route_call.output == id || route_call.input == id) {
		if (route_call.output == id)
			route_call.output = route_default(AUDIOD_ROUTE_CALL, AUDIOD_KIND_SINK);
		if (route_call.input == id)
			route_call.input = route_default(AUDIOD_ROUTE_CALL, AUDIOD_KIND_SOURCE);
		route_call.reason = reason;
		route_broadcast_route(AUDIOD_ROUTE_CALL);
	}

	/* The keys' volume may be another output's now. */
	route_broadcast_volume();
}

/* Gives an endpoint up after it did not open (TIMEOUT or FAILED): closed, its routes told, left as they are (N1). */
static void
route_give_up(
	struct route_endpoint *endpoint,
	uint32_t reason)
{
	int64_t now_ns;

	/* Closed, not tried again until a route or the daemon changes. */
	now_ns = audiod_now_ns();
	route_want(endpoint, 0U, now_ns);
	endpoint->gave_up = 1U;

	/* The routes that use it, told why. */
	if (route_media.output == endpoint->id || route_media.input == endpoint->id) {
		route_media.reason = reason;
		route_broadcast_route(AUDIOD_ROUTE_MEDIA);
	}

	/* And the call's. */
	if (route_call.output == endpoint->id || route_call.input == endpoint->id) {
		route_call.reason = reason;
		route_broadcast_route(AUDIOD_ROUTE_CALL);
	}
}

/* Sends ENDPOINT_WANTED with a new ticket; closing makes the endpoint not open at once (N2). */
static void
route_want(
	struct route_endpoint *endpoint,
	unsigned wanted,
	int64_t now_ns)
{
	struct audiod_endpoint_wanted message;

	/* A new ticket. */
	endpoint->ticket++;
	endpoint->wanted = wanted;
	endpoint->wanted_ns = now_ns;
	if (!wanted)
		endpoint->open = 0U;

	/* To its daemon. */
	memset(&message, 0, sizeof(message));
	message.header.type = AUDIOD_ENDPOINT_WANTED;
	message.header.length = sizeof(message);
	message.id = endpoint->id;
	message.wanted = wanted;
	message.ticket = endpoint->ticket;
	audiod_send(endpoint->owner, &message, sizeof(message), -1);
}

/*
 * Tells whether a route uses an endpoint now (E5', N8): the media output
 * while a media stream plays and for its grace after, the media input
 * while a recording runs, the call's user side and far side while a call
 * is set.  One audiod gave up is not wanted again until something changes.
 */
static unsigned
route_desired(
	const struct route_endpoint *endpoint,
	int64_t now_ns)
{
	int used;

	/* Given up. */
	if (endpoint->gave_up)
		return 0U;

	/* The media output while media plays. */
	if (route_media.output == endpoint->id) {
		used = route_media_in_use(now_ns);
		if (used)
			return 1U;
	}

	/* The media input while recording. */
	if (route_media.input == endpoint->id) {
		used = route_capture_running();
		if (used)
			return 1U;
	}

	/* The call's sides while a call is set. */
	if (route_call.set) {
		if (route_call.output == endpoint->id || route_call.input == endpoint->id)
			return 1U;
		if (endpoint->owner == route_call.far_owner && endpoint->group == route_call.far_group)
			return 1U;
	}

	/* Not used. */
	return 0U;
}

/* Tells whether media plays, or played within the grace (a media stream running and not held). */
static int
route_media_in_use(
	int64_t now_ns)
{
	struct audiod_client *client;
	struct audiod_stream *stream;

	/* A running media playback stream, not held. */
	for (client = audiod_clients; client != NULL; client = client->next) {
		for (stream = client->streams; stream != NULL; stream = stream->next) {
			if (stream->direction != AUDIOD_PLAYBACK || !stream->running || stream->held)
				continue;
			if (stream->role != AUDIOD_ROLE_MEDIA)
				continue;
			route_media_last_ns = now_ns;
			return 1;
		}
	}

	/* The grace after the last one. */
	if (route_media_last_ns != 0 && now_ns - route_media_last_ns < (int64_t)ROUTE_MEDIA_GRACE_MS * 1000000)
		return 1;

	/* Not in use. */
	route_media_last_ns = 0;
	return 0;
}

/* Tells whether a recording runs. */
static int
route_capture_running(
	void)
{
	struct audiod_client *client;
	struct audiod_stream *stream;

	/* A running capture stream. */
	for (client = audiod_clients; client != NULL; client = client->next) {
		for (stream = client->streams; stream != NULL; stream = stream->next) {
			if (stream->direction == AUDIOD_CAPTURE && stream->running)
				return 1;
		}
	}

	/* None. */
	return 0;
}

/* Holds the media streams playing now: their output went away (G5). */
static void
route_hold_media(
	void)
{
	struct audiod_client *client;
	struct audiod_stream *stream;
	unsigned held;

	/* Each running media playback stream. */
	held = 0U;
	for (client = audiod_clients; client != NULL; client = client->next) {
		for (stream = client->streams; stream != NULL; stream = stream->next) {
			if (stream->direction != AUDIOD_PLAYBACK || !stream->running || stream->role != AUDIOD_ROLE_MEDIA)
				continue;
			stream->held = 1U;
			held = 1U;
		}
	}

	/* Told when something is held. */
	if (held) {
		route_media.lost = 1U;
		route_held_changed(1U, AUDIOD_REASON_OUTPUT_LOST);
	}
}

/* Tells the route subscribers whether media is held (MEDIA_HELD). */
static void
route_held_changed(
	unsigned held,
	uint32_t reason)
{
	struct audiod_media_held message;
	struct audiod_client *client;

	/* A call on the speaker holds the media whatever else is released (Q13). */
	if (!held && route_call_held) {
		held = 1U;
		reason = AUDIOD_REASON_CALL;
	}

	/* The message. */
	memset(&message, 0, sizeof(message));
	message.header.type = AUDIOD_MEDIA_HELD;
	message.header.length = sizeof(message);
	message.held = held;
	message.reason = reason;

	/* Each version 2 subscriber of the routes. */
	for (client = audiod_clients; client != NULL; client = client->next) {
		if (client->version >= AUDIOD_PROTOCOL_VERSION_2 && (client->subscribed & AUDIOD_SUBSCRIBE_ROUTES) != 0U)
			audiod_send(client, &message, sizeof(message), -1);
	}
}

/* Gives the endpoint the volume keys act on: the call's output while a call is set, else the media's (N5). */
static uint32_t
route_volume_target(
	void)
{
	/* The call's. */
	if (route_call.set)
		return route_call.output;

	/* The media's. */
	return route_media.output;
}

/* Fills an ENDPOINT_INFO (gone: state 0). */
static void
route_info(
	const struct route_endpoint *endpoint,
	struct audiod_endpoint_info *info,
	int gone)
{
	/* What the endpoint is. */
	memset(info, 0, sizeof(*info));
	info->header.type = AUDIOD_ENDPOINT_INFO;
	info->header.length = sizeof(*info);
	info->id = endpoint->id;
	info->kind = endpoint->kind;
	info->endpoint_class = endpoint->endpoint_class;
	info->group = endpoint->group;
	info->state = endpoint->state;
	if (gone)
		info->state = 0U;
	info->rate = endpoint->rate;
	info->channels = endpoint->channels;
	info->flags = endpoint->flags;
	info->latency_us = endpoint->latency_us;
	info->volume_left = endpoint->volume_left;
	info->volume_right = endpoint->volume_right;
	info->muted = endpoint->muted;
	route_copy_text(info->key, endpoint->key, sizeof(info->key));
	route_copy_text(info->name, endpoint->name, sizeof(info->name));
}

/* Tells the endpoint subscribers of an endpoint (added, changed or gone). */
static void
route_broadcast_endpoint(
	const struct route_endpoint *endpoint,
	int gone)
{
	struct audiod_endpoint_info info;
	struct audiod_client *client;

	/* The message. */
	route_info(endpoint, &info, gone);

	/* Each version 2 subscriber of the endpoints. */
	for (client = audiod_clients; client != NULL; client = client->next) {
		if (client->version >= AUDIOD_PROTOCOL_VERSION_2 && (client->subscribed & AUDIOD_SUBSCRIBE_ENDPOINTS) != 0U)
			audiod_send(client, &info, sizeof(info), -1);
	}
}

/* Tells the route subscribers of a route. */
static void
route_broadcast_route(
	uint32_t route)
{
	struct audiod_route_info info;
	struct audiod_client *client;

	/* The message. */
	route_fill_route(route, &info);

	/* Each version 2 subscriber of the routes. */
	for (client = audiod_clients; client != NULL; client = client->next) {
		if (client->version >= AUDIOD_PROTOCOL_VERSION_2 && (client->subscribed & AUDIOD_SUBSCRIBE_ROUTES) != 0U)
			audiod_send(client, &info, sizeof(info), -1);
	}
}

/* Fills a ROUTE_INFO. */
static void
route_fill_route(
	uint32_t route,
	struct audiod_route_info *info)
{
	/* The header. */
	memset(info, 0, sizeof(*info));
	info->header.type = AUDIOD_ROUTE_INFO;
	info->header.length = sizeof(*info);
	info->route = route;

	/* Media. */
	if (route == AUDIOD_ROUTE_MEDIA) {
		info->output = route_media.output;
		info->input = route_media.input;
		info->active = route_media.output != 0U;
		info->reason = route_media.reason;
		info->held = route_media.lost;
		return;
	}

	/* The call. */
	info->output = route_call.output;
	info->input = route_call.input;
	info->active = route_call.set;
	info->reason = route_call.reason;
	info->far_group = route_call.far_group;
	info->audio_here = route_call.audio_here;
}

/* Tells the volume subscribers the keys' output's volume. */
static void
route_broadcast_volume(
	void)
{
	struct audiod_client *client;

	/* Each subscriber of the volume (every version). */
	for (client = audiod_clients; client != NULL; client = client->next) {
		if ((client->subscribed & AUDIOD_SUBSCRIBE_VOLUME) != 0U)
			audiod_route_send_volume(client);
	}
}

/* Copies a text into a field, cut at a character's boundary and ended by a NUL (E10). */
static void
route_copy_text(
	char *target,
	const char *source,
	size_t size)
{
	size_t source_length;
	size_t length;

	/* As much as fits. */
	source_length = strlen(source);
	length = source_length;
	if (length > size - 1U)
		length = size - 1U;

	/* Not in the middle of a UTF-8 character: back over continuation bytes. */
	while (length > 0U && length < source_length && ((unsigned char)source[length] & 0xc0U) == 0x80U)
		length--;

	/* Succeeded: copied and ended. */
	memcpy(target, source, length);
	target[length] = '\0';
}

/*
 * Tells whether a stream is read this period (p003): its output open (the
 * device's, or a daemon's endpoint that answered ACTIVE, N2 and N20).
 * While a call flows its output is the call's: on the speaker media
 * stops and alerts go on (Q13), on a headset both go on, quieter.
 */
int
audiod_route_stream_plays(
	const struct audiod_stream *stream)
{
	struct route_endpoint *output;
	int flowing;

	/* A call's output, the media stopped on the speaker. */
	flowing = route_flowing();
	if (flowing) {
		output = route_find(route_call.output);
		if (output == NULL)
			return 0;
		if (stream->role == AUDIOD_ROLE_MEDIA && output->owner == NULL)
			return 0;
		return output->open;
	}

	/* The media output, open. */
	output = route_find(route_media.output);
	if (output == NULL)
		return 0;

	/* Succeeded or not. */
	return output->open;
}

/* Gives a stream's gain (65536 is unity): -12 dB over a call (Q13), else unity. */
uint32_t
audiod_route_stream_gain(
	const struct audiod_stream *stream)
{
	int flowing;

	/* Over a call. */
	(void)stream;
	flowing = route_flowing();
	if (flowing)
		return (uint32_t)(65536.0 * ROUTE_CALL_GAIN);

	/* Unity. */
	return 65536U;
}

/* Tells the device's fragments ahead of it: one while a call runs (M8, G4), else two. */
unsigned
audiod_route_fill_fragments(
	void)
{
	int flowing;

	/* Fewer over a call. */
	flowing = route_flowing();
	if (flowing)
		return ROUTE_CALL_FRAGMENTS;

	/* Succeeded: as before. */
	return ROUTE_FILL_FRAGMENTS;
}

/*
 * Tells how far behind what was read what is heard now is, in seconds
 * (N4): the device's fragments ahead and the kernel's lead for device:out;
 * the ring's fill and the daemon's latency for a daemon's sink (an
 * endpoint's converter adds its half window, under a millisecond, left out).
 */
double
audiod_route_output_delay(
	void)
{
	struct route_endpoint *output;
	struct audiod_shm_header *shm;
	double seconds;
	uint64_t fill;
	int faulted;
	int flowing;

	/* The output the streams play to: the call's while one flows, else the media's. */
	output = route_find(route_media.output);
	flowing = route_flowing();
	if (flowing)
		output = route_find(route_call.output);
	if (output == NULL || audiod_device.rate == 0U || audiod_device.frame_bytes == 0U)
		return 0.0;

	/* The device's: its fragments ahead and the kernel's lead. */
	if (output->owner == NULL) {
		seconds = (double)audiod_route_fill_fragments() * (double)audiod_device.period_frames / (double)audiod_device.rate;
		seconds += (double)(audiod_device.lead_bytes / audiod_device.frame_bytes) / (double)audiod_device.rate;
		return seconds;
	}

	/* A daemon's: its ring's fill (audiod's own ring, guarded as any) and its latency. */
	shm = output->shm;
	/*
	 * A fault comes back past sigsetjmp with the guard still armed (the
	 * handler jumps only when it is): the call stands alone as the C
	 * standard allows, and the guard tells which return this is.
	 */
	audiod_bus_armed = 0;
	(void)sigsetjmp(audiod_bus_jump, 1);
	faulted = audiod_bus_armed;
	if (faulted) {
		audiod_bus_armed = 0;
		return 0.0;
	}

	/* From here a fault jumps back above. */
	audiod_bus_armed = 1;
	fill = shm->write_position - audiod_position_load(&shm->read_position, &shm->read_sequence);
	audiod_bus_armed = 0;
	if (fill > output->capacity_frames)
		fill = 0U;
	seconds = (double)fill / (double)output->rate + (double)output->latency_us / 1000000.0;

	/* Succeeded. */
	return seconds;
}

/*
 * Takes the period the streams were mixed into (the mix's scale, stereo)
 * where the routes say: to the media output when it is a daemon's sink
 * (the device then plays silence), and over a call the far side's voice to
 * the user's output with the mix under it (Q13), and the user's voice from
 * a daemon's source to the far side.
 */
void
audiod_flow_playback(
	struct audiod_device *device,
	int64_t *mix,
	uint32_t frames)
{
	static float stereo[ROUTE_PERIOD_MAX * 2U];
	static float voice[ROUTE_PERIOD_MAX * 2U];
	struct route_endpoint *output;
	struct route_endpoint *user_output;
	struct route_endpoint *user_input;
	struct route_endpoint *far_input;
	struct route_endpoint *far_output;
	uint32_t index;
	int flowing;

	/* Media held while a call is on the speaker (Q13), told when that changes. */
	route_call_hold();

	/* A period the flow holds. */
	if (frames > ROUTE_PERIOD_MAX)
		return;

	/* The mix as floats. */
	for (index = 0U; index < frames * 2U; index++)
		stereo[index] = (float)((double)mix[index] / ROUTE_MIX_SCALE);

	/* A call: the far voice to the user's output, the user's voice from a daemon's source to the far side. */
	flowing = route_flowing();
	if (flowing) {
		far_input = route_far(AUDIOD_KIND_SOURCE);
		far_output = route_far(AUDIOD_KIND_SINK);
		user_output = route_find(route_call.output);
		user_input = route_find(route_call.input);
		(void)route_read_source(far_input, voice, frames);

		/* On the speaker: the voice into the device's mix. */
		if (user_output == NULL || user_output->owner == NULL) {
			for (index = 0U; index < frames * 2U; index++)
				mix[index] += (int64_t)((double)voice[index] * ROUTE_MIX_SCALE);
		} else {
			/* On a headset: the voice with the mix under it, the device silent. */
			for (index = 0U; index < frames * 2U; index++) {
				voice[index] += stereo[index];
				mix[index] = 0;
			}

			/* To the headset. */
			(void)route_write_sink(user_output, voice, frames);
		}

		/* The user's voice from a daemon's source (the device's comes through audiod_flow_capture). */
		if (user_input != NULL && user_input->owner != NULL) {
			(void)route_read_source(user_input, voice, frames);
			(void)route_write_sink(far_output, voice, frames);
		}

		/* The call takes the period. */
		return;
	}

	/* No call: the media to a daemon's sink, the device silent. */
	output = route_find(route_media.output);
	if (output == NULL || output->owner == NULL)
		return;
	for (index = 0U; index < frames * 2U; index++)
		mix[index] = 0;
	(void)route_write_sink(output, stereo, frames);
	(void)device;
}

/* Takes a recorded period of the device (stereo, 32-bit) to a call's far side when the user's input is the device's. */
void
audiod_flow_capture(
	struct audiod_device *device,
	const int32_t *frames_stereo,
	uint32_t frames)
{
	static float stereo[ROUTE_PERIOD_MAX * 2U];
	struct route_endpoint *user_input;
	uint32_t index;
	int flowing;

	/* Only over a call whose input is the device's. */
	(void)device;
	flowing = route_flowing();
	if (!flowing || frames > ROUTE_PERIOD_MAX)
		return;
	user_input = route_find(route_call.input);
	if (user_input == NULL || user_input->owner != NULL)
		return;

	/* The period as floats, to the far side. */
	for (index = 0U; index < frames * 2U; index++)
		stereo[index] = (float)((double)frames_stereo[index] / ROUTE_MIX_SCALE);
	(void)route_write_sink(route_far(AUDIOD_KIND_SINK), stereo, frames);
}

/*
 * Gives the recording streams a period from the media input when it is a
 * daemon's source (stereo, 32-bit, in place of the device's period), at
 * the device's capture as the clock.  A source the call reads at the same
 * time (its user's input) gives silence here (one reader a ring).
 */
void
audiod_flow_input(
	struct audiod_device *device,
	int32_t *frames_stereo,
	uint32_t frames)
{
	static float stereo[ROUTE_PERIOD_MAX * 2U];
	struct route_endpoint *input;
	uint32_t index;
	double value;
	int flowing;
	int result;

	/* A daemon's source as the media input. */
	(void)device;
	input = route_find(route_media.input);
	if (input == NULL || input->owner == NULL || frames > ROUTE_PERIOD_MAX)
		return;

	/* Silence while it does not stream or the call reads it. */
	memset(frames_stereo, 0, (size_t)frames * 2U * sizeof(*frames_stereo));
	flowing = route_flowing();
	if (flowing && route_call.input == input->id)
		return;
	result = route_read_source(input, stereo, frames);
	if (result != 0)
		return;

	/* As the mix's scale, saturated. */
	for (index = 0U; index < frames * 2U; index++) {
		value = (double)stereo[index] * ROUTE_MIX_SCALE;
		if (value > 2147483647.0)
			value = 2147483647.0;
		if (value < -2147483648.0)
			value = -2147483648.0;
		frames_stereo[index] = (int32_t)value;
	}
}

/* Tells whether a call's voice flows through the PC (set, and its audio not on the phone, G3). */
static int
route_flowing(
	void)
{
	/* Set and here. */
	if (!route_call.set)
		return 0;

	/* Succeeded or not. */
	return route_call.audio_here != 0U;
}

/* Finds the call's far endpoint of a kind (the far sink takes the user's voice, the far source gives the far one). */
static struct route_endpoint *
route_far(
	uint32_t kind)
{
	struct route_endpoint *endpoint;

	/* The call's daemon's endpoint of the far group and the kind. */
	for (endpoint = route_endpoints; endpoint != NULL; endpoint = endpoint->next) {
		if (endpoint->owner == route_call.far_owner && endpoint->group == route_call.far_group && endpoint->kind == kind &&
		    (endpoint->endpoint_class == AUDIOD_CLASS_PHONE_CALL || endpoint->endpoint_class == AUDIOD_CLASS_MODEM_CALL))
			return endpoint;
	}

	/* None. */
	return NULL;
}

/* Makes an endpoint's converter on first use; 0 or an errno. */
static int
route_flow_ready(
	struct route_endpoint *endpoint,
	uint32_t rate_in,
	uint32_t rate_out)
{
	int error;

	/* Made once. */
	if (endpoint->converter_ready)
		return 0;
	error = asrc_init(&endpoint->converter, rate_in, rate_out, endpoint->channels);
	if (error != 0)
		return error;

	/* Succeeded: ready. */
	endpoint->converter_ready = 1U;
	return 0;
}

/* Keeps a period's fill and gives the average of the last ROUTE_FILL_PERIODS (M9's measuring point). */
static double
route_fill_average(
	struct route_endpoint *endpoint,
	double fill)
{
	double sum;
	unsigned index;

	/* Kept. */
	endpoint->fills[endpoint->fill_next] = fill;
	endpoint->fill_next = (endpoint->fill_next + 1U) % ROUTE_FILL_PERIODS;
	if (endpoint->fill_count < ROUTE_FILL_PERIODS)
		endpoint->fill_count++;

	/* The average. */
	sum = 0.0;
	for (index = 0U; index < endpoint->fill_count; index++)
		sum += endpoint->fills[index];

	/* Succeeded. */
	return sum / (double)endpoint->fill_count;
}

/* Starts an endpoint's fill average afresh (opened, or after the middle stage moved a block). */
static void
route_fill_reset(
	struct route_endpoint *endpoint)
{
	/* Empty. */
	endpoint->fill_count = 0U;
	endpoint->fill_next = 0U;
}

/*
 * Writes a master period (stereo floats) into a daemon's sink: converted to
 * its rate and channels at its volume, the first time after its target of
 * silence, kept at its target by the controller (C3) or, past 10 ms, by
 * writing a block less or more (M9).  Returns 0, or -1 when the sink is not
 * open or broke.
 */
static int
route_write_sink(
	struct route_endpoint *endpoint,
	const float *stereo,
	uint32_t frames)
{
	static float input[ROUTE_PERIOD_MAX * 2U];
	static float converted[ROUTE_PERIOD_MAX * 2U * 8U];
	struct audiod_shm_header *shm;
	uint64_t write_position;
	uint64_t read_position;
	double average;
	double error_ms;
	double target;
	size_t made;
	size_t used;
	size_t room;
	size_t skip;
	size_t fade;
	uint32_t index;
	int jumped;
	int whole;
	unsigned channel;
	float gain[2];
	int error;
	int faulted;

	/* An open sink only (N2, N20). */
	if (endpoint == NULL || !endpoint->open || endpoint->kind != AUDIOD_KIND_SINK)
		return -1;
	error = route_flow_ready(endpoint, audiod_device.rate, endpoint->rate);
	if (error != 0)
		return -1;

	/* Its channels and volume (a daemon that takes the volume gets unity, N5). */
	route_volume_gain(endpoint, gain);
	for (index = 0U; index < frames; index++) {
		if (endpoint->channels == 1U) {
			input[index] = (gain[0] * stereo[index * 2U] + gain[1] * stereo[index * 2U + 1U]) / 2.0f;
			continue;
		}

		/* Stereo as it is. */
		for (channel = 0U; channel < 2U; channel++)
			input[index * 2U + channel] = gain[channel] * stereo[index * 2U + channel];
	}

	/* Converted (a 48 kHz period makes at most its frames at 48 kHz). */
	made = asrc_process(&endpoint->converter, input, frames, converted, ROUTE_PERIOD_MAX * 8U, &used);

	/* The ring's positions, checked (E6); a fault in its memory breaks it. */
	shm = endpoint->shm;
	/*
	 * A fault comes back past sigsetjmp with the guard still armed (the
	 * handler jumps only when it is): the call stands alone as the C
	 * standard allows, and the guard tells which return this is.
	 */
	audiod_bus_armed = 0;
	(void)sigsetjmp(audiod_bus_jump, 1);
	faulted = audiod_bus_armed;
	if (faulted) {
		audiod_bus_armed = 0;
		route_broken(endpoint);
		return -1;
	}

	/* From here a fault jumps back above. */
	audiod_bus_armed = 1;
	write_position = shm->write_position;
	read_position = audiod_position_load(&shm->read_position, &shm->read_sequence);
	if (read_position > write_position || write_position - read_position > endpoint->capacity_frames) {
		audiod_bus_armed = 0;
		route_broken(endpoint);
		return -1;
	}

	/* The first time: its target of silence first (C3 (a)). */
	target = (double)endpoint->target_frames;
	if (!endpoint->primed) {
		route_ring_put(endpoint, write_position, NULL, endpoint->target_frames);
		write_position += endpoint->target_frames;
		endpoint->primed = 1U;
	}

	/* The fill, averaged; past 10 ms a block less or more (M9), within it the controller. */
	average = route_fill_average(endpoint, (double)(write_position - read_position));
	error_ms = (average - target) * 1000.0 / (double)endpoint->rate;
	skip = 0U;
	fade = (size_t)(ROUTE_FADE_MS * (double)endpoint->rate / 1000.0);
	jumped = 0;
	whole = endpoint->fill_count == ROUTE_FILL_PERIODS;
	if (!whole)
		error_ms = 0.0;
	if (error_ms > ROUTE_MIDDLE_MS || error_ms < -ROUTE_MIDDLE_MS) {
		/* The written end faded out where the reader is not yet, the next written faded in. */
		if (write_position - read_position >= 2U * fade)
			route_ring_fade(endpoint, write_position - fade, fade, 0);
		jumped = 1;
		endpoint->moved++;
		route_fill_reset(endpoint);
		endpoint->converter.integral = 0.0;
	}

	/* Too full: a block less; too empty: silence first; else the controller. */
	if (error_ms > ROUTE_MIDDLE_MS) {
		skip = (size_t)(average - target);
		if (skip > made)
			skip = made;
	} else if (error_ms < -ROUTE_MIDDLE_MS) {
		route_ring_put(endpoint, write_position, NULL, (size_t)(target - average));
		write_position += (uint64_t)(target - average);
	} else {
		asrc_steer(&endpoint->converter, (average - target) / target);
	}

	/* The frames, as many as the ring takes; the rest is lost, counted (N16: a sink's overruns are audiod's). */
	room = endpoint->capacity_frames - (size_t)(write_position - read_position);
	if (made - skip > room) {
		skip = made - room;
		shm->overruns++;
	}

	/* Written, faded in after a jump. */
	route_ring_put(endpoint, write_position, converted + skip * endpoint->channels, made - skip);
	if (jumped && made - skip < fade)
		fade = made - skip;
	if (jumped)
		route_ring_fade(endpoint, write_position, fade, 1);
	write_position += made - skip;
	audiod_position_store(&shm->write_position, &shm->write_sequence, write_position);
	audiod_bus_armed = 0;

	/* Succeeded: written. */
	return 0;
}

/*
 * Reads a master period (stereo floats) from a daemon's source: converted
 * from its rate, once its fill reached its target (and again after it ran
 * dry), kept at its target by the controller or, past 10 ms over, by
 * dropping a block (M9).  Silence when it is not open or has nothing yet.
 * Returns 0, or -1 for silence.
 */
static int
route_read_source(
	struct route_endpoint *endpoint,
	float *stereo,
	uint32_t frames)
{
	static float chunk[ROUTE_CHUNK_FRAMES * 2U];
	static float converted[ROUTE_PERIOD_MAX * 2U];
	struct audiod_shm_header *shm;
	uint64_t write_position;
	uint64_t read_position;
	uint64_t available;
	double average;
	double error_ms;
	double target;
	size_t made;
	size_t used;
	size_t take;
	size_t step;
	size_t fade;
	uint64_t drop;
	double need;
	uint32_t index;
	unsigned channel;
	int dropped;
	int error;
	int faulted;

	/* Silence unless it streams. */
	memset(stereo, 0, (size_t)frames * 2U * sizeof(float));
	if (endpoint == NULL || !endpoint->open || endpoint->kind != AUDIOD_KIND_SOURCE)
		return -1;
	error = route_flow_ready(endpoint, endpoint->rate, audiod_device.rate);
	if (error != 0)
		return -1;

	/* The ring's positions, checked (E6). */
	shm = endpoint->shm;
	/*
	 * A fault comes back past sigsetjmp with the guard still armed (the
	 * handler jumps only when it is): the call stands alone as the C
	 * standard allows, and the guard tells which return this is.
	 */
	audiod_bus_armed = 0;
	(void)sigsetjmp(audiod_bus_jump, 1);
	faulted = audiod_bus_armed;
	if (faulted) {
		audiod_bus_armed = 0;
		route_broken(endpoint);
		return -1;
	}

	/* From here a fault jumps back above. */
	audiod_bus_armed = 1;
	write_position = audiod_position_load(&shm->write_position, &shm->write_sequence);
	read_position = shm->read_position;
	if (read_position > write_position || write_position - read_position > endpoint->capacity_frames) {
		audiod_bus_armed = 0;
		route_broken(endpoint);
		return -1;
	}

	/* What is there. */
	available = write_position - read_position;

	/*
	 * What a period takes from it, and its fill as what stays after that
	 * (the target is what stays).  Filled to its target and a period
	 * first (C3 (a)), again after running dry.
	 */
	need = (double)frames * (double)endpoint->rate / (double)audiod_device.rate;
	target = (double)endpoint->target_frames;
	if (!endpoint->primed && (double)available < target + need) {
		audiod_bus_armed = 0;
		return -1;
	}

	/* Primed. */
	endpoint->primed = 1U;

	/* Too full past 10 ms: the oldest block dropped (M9); within it the controller. */
	average = route_fill_average(endpoint, (double)available - need);
	error_ms = (average - target) * 1000.0 / (double)endpoint->rate;
	dropped = 0;
	if (endpoint->fill_count == ROUTE_FILL_PERIODS && error_ms > ROUTE_MIDDLE_MS) {
		drop = (uint64_t)(average - target);
		if (drop > available)
			drop = available;
		read_position += drop;
		available = write_position - read_position;
		dropped = 1;
		endpoint->moved++;
		route_fill_reset(endpoint);
		endpoint->converter.integral = 0.0;
	} else {
		asrc_steer(&endpoint->converter, (average - target) / target);
	}

	/* Converted a chunk at a time until the period is made or the ring is dry. */
	made = 0U;
	while (made < frames && available != 0U) {
		take = ROUTE_CHUNK_FRAMES;
		if (take > available)
			take = (size_t)available;
		route_ring_get(endpoint, read_position, chunk, take);
		used = 0U;
		step = asrc_process(&endpoint->converter, chunk, take, converted + made * endpoint->channels, frames - made, &used);
		made += step;
		read_position += used;
		available -= used;
		if (used == 0U && step == 0U)
			break;
	}

	/* Given back. */
	audiod_position_store(&shm->read_position, &shm->read_sequence, read_position);

	/* Ran dry: silence for the rest, counted (N16: a source's underruns are audiod's), refilled to the target before reading again. */
	if (made < frames) {
		shm->underruns++;
		endpoint->primed = 0U;
		route_fill_reset(endpoint);
		asrc_reset(&endpoint->converter);
	}

	/* The ring is no longer touched. */
	audiod_bus_armed = 0;

	/* After a dropped block, faded in over ROUTE_FADE_MS. */
	if (dropped) {
		fade = (size_t)(ROUTE_FADE_MS * (double)audiod_device.rate / 1000.0);
		for (index = 0U; index < made && index < fade; index++) {
			for (channel = 0U; channel < endpoint->channels; channel++)
				converted[index * endpoint->channels + channel] *= (float)index / (float)fade;
		}
	}

	/* As stereo. */
	for (index = 0U; index < made; index++) {
		if (endpoint->channels == 1U) {
			stereo[index * 2U] = converted[index];
			stereo[index * 2U + 1U] = converted[index];
			continue;
		}

		/* Stereo as it is. */
		stereo[index * 2U] = converted[index * 2U];
		stereo[index * 2U + 1U] = converted[index * 2U + 1U];
	}

	/* Succeeded: read. */
	return 0;
}

/* Puts frames (floats, the endpoint's channels; NULL for silence) into a sink's ring as S16 from a position. */
static void
route_ring_put(
	struct route_endpoint *endpoint,
	uint64_t position,
	const float *samples,
	size_t frames)
{
	int16_t *ring;
	size_t index;
	size_t slot;
	unsigned channel;
	float value;

	/* Each frame at its place in the ring. */
	ring = (int16_t *)(void *)((uint8_t *)endpoint->shm + AUDIOD_SHM_HEADER);
	for (index = 0U; index < frames; index++) {
		slot = (size_t)((position + index) % endpoint->capacity_frames);
		for (channel = 0U; channel < endpoint->channels; channel++) {
			value = 0.0f;
			if (samples != NULL)
				value = samples[index * endpoint->channels + channel];

			/* Saturated. */
			if (value > 32767.0f / 32768.0f)
				value = 32767.0f / 32768.0f;
			if (value < -1.0f)
				value = -1.0f;
			ring[slot * endpoint->channels + channel] = (int16_t)lrintf(value * 32768.0f);
		}
	}
}

/* Gets frames from a source's ring (S16) as floats of its channels from a position. */
static void
route_ring_get(
	struct route_endpoint *endpoint,
	uint64_t position,
	float *samples,
	size_t frames)
{
	const int16_t *ring;
	size_t index;
	size_t slot;
	unsigned channel;

	/* Each frame from its place in the ring. */
	ring = (const int16_t *)(const void *)((const uint8_t *)endpoint->shm + AUDIOD_SHM_HEADER);
	for (index = 0U; index < frames; index++) {
		slot = (size_t)((position + index) % endpoint->capacity_frames);
		for (channel = 0U; channel < endpoint->channels; channel++)
			samples[index * endpoint->channels + channel] = (float)ring[slot * endpoint->channels + channel] / 32768.0f;
	}
}

/* Takes away an endpoint whose ring broke (positions out of range, or its memory faulted), told to its owner (E6). */
static void
route_broken(
	struct route_endpoint *endpoint)
{
	/* Gone, the routes moved as for any removal. */
	route_remove(endpoint, AUDIOD_REASON_BROKEN, 1);
}

/* Gives an endpoint's soft gain a channel: unity when its daemon takes the volume (N5) or it is a call's far side, else the device's curve, 0 muted. */
static void
route_volume_gain(
	const struct route_endpoint *endpoint,
	float gain[2])
{
	/* The daemon's, or the far side's (what the user hears is not what the far side hears). */
	gain[0] = 1.0f;
	gain[1] = 1.0f;
	if ((endpoint->flags & AUDIOD_ENDPOINT_FLAG_VOLUME) != 0U)
		return;
	if (endpoint->endpoint_class == AUDIOD_CLASS_PHONE_CALL || endpoint->endpoint_class == AUDIOD_CLASS_MODEM_CALL)
		return;

	/* Muted, or the curve audiod uses for the device. */
	gain[0] = 0.0f;
	gain[1] = 0.0f;
	if (endpoint->muted)
		return;
	gain[0] = (float)audiod_soft_factor(endpoint->volume_left) / 65536.0f;
	gain[1] = (float)audiod_soft_factor(endpoint->volume_right) / 65536.0f;
}

/* Fades a sink's written frames from a position: out (to silence) or in (from it). */
static void
route_ring_fade(
	struct route_endpoint *endpoint,
	uint64_t position,
	size_t frames,
	int rising)
{
	int16_t *ring;
	size_t index;
	size_t slot;
	unsigned channel;
	float factor;

	/* Each frame, scaled by its place in the fade. */
	ring = (int16_t *)(void *)((uint8_t *)endpoint->shm + AUDIOD_SHM_HEADER);
	for (index = 0U; index < frames; index++) {
		factor = (float)(frames - index) / (float)frames;
		if (rising)
			factor = (float)index / (float)frames;
		slot = (size_t)((position + index) % endpoint->capacity_frames);
		for (channel = 0U; channel < endpoint->channels; channel++)
			ring[slot * endpoint->channels + channel] = (int16_t)lrintf((float)ring[slot * endpoint->channels + channel] * factor);
	}
}

/*
 * Each period: records from the device's microphone while it is a flowing
 * call's input, and holds the media while a call is on the speaker (Q13,
 * MEDIA_HELD reason CALL), released when it ends or moves.
 */
static void
route_call_hold(
	void)
{
	struct route_endpoint *output;
	unsigned speaker;
	uint32_t reason;
	int flowing;

	/* The device's microphone recording while it is the call's input. */
	speaker = 0U;
	flowing = route_flowing();
	if (flowing && route_call.input == AUDIOD_ENDPOINT_DEVICE_IN)
		audiod_device_start_capture(&audiod_device);

	/* On the speaker: flowing, its output the device's. */
	if (flowing) {
		output = route_find(route_call.output);
		speaker = output == NULL || output->owner == NULL;
	}

	/* Told only when it changes. */
	if (speaker == route_call_held)
		return;
	route_call_held = speaker;

	/* Told, a hold for a lost output kept. */
	reason = AUDIOD_REASON_CALL;
	if (!speaker && route_media.lost)
		reason = AUDIOD_REASON_OUTPUT_LOST;
	route_held_changed(speaker || route_media.lost, reason);
}
