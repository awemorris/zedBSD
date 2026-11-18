/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * HTMLMediaElement, HTMLVideoElement and HTMLAudioElement (ws121-p005):
 * what a script sees of a <video> or an <audio>.  The playing is the
 * host's (the page's media, page/media.c, through bind_host.media): play()
 * and pause() ask it, and the state (paused, ended, currentTime, duration,
 * readyState, the video's size) is read from it.  play() returns a
 * Promise resolved at once.  The events (play, pause, ended, timeupdate
 * and the loading ones) are fired by the host as its engine moves on.
 *
 * Normal use only: muted reflects the attribute (a change does not reach
 * the sound of a playing element), and load() does nothing.
 */

#include "bind/internal.h"

#include <errno.h>
#include <string.h>

/* The ready states (HTMLMediaElement). */
#define MEDIA_HAVE_NOTHING	0
#define MEDIA_HAVE_ENOUGH_DATA	4

static int media_this(struct vm_realm *realm, vm_value this_value, struct dom_element **element);
static int media_ask(struct vm_realm *realm, vm_value this_value, int request, double value, struct bind_media *state);
static int media_paused(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int media_ended(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int media_current_time_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int media_current_time_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int media_duration(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int media_ready_state(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int media_muted_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int media_muted_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int media_play(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int media_pause(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int media_load(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int media_can_play_type(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int media_video_width(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int media_video_height(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* The attributes of HTMLMediaElement. */
static const struct bind_attribute media_attributes[] = {
	{ "paused", media_paused, NULL },
	{ "ended", media_ended, NULL },
	{ "currentTime", media_current_time_get, media_current_time_set },
	{ "duration", media_duration, NULL },
	{ "readyState", media_ready_state, NULL },
	{ "muted", media_muted_get, media_muted_set },
	{ NULL, NULL, NULL }
};

/* The operations of HTMLMediaElement. */
static const struct bind_operation media_operations[] = {
	{ "play", 0, media_play },
	{ "pause", 0, media_pause },
	{ "load", 0, media_load },
	{ "canPlayType", 1, media_can_play_type },
	{ NULL, 0, NULL }
};

/* The constants of HTMLMediaElement (the ready states, on the interface and its prototype). */
static const struct bind_constant media_constants[] = {
	{ "HAVE_NOTHING", 0 },
	{ "HAVE_METADATA", 1 },
	{ "HAVE_CURRENT_DATA", 2 },
	{ "HAVE_FUTURE_DATA", 3 },
	{ "HAVE_ENOUGH_DATA", 4 },
	{ NULL, 0 }
};

/* The attributes of HTMLVideoElement. */
static const struct bind_attribute video_attributes[] = {
	{ "videoWidth", media_video_width, NULL },
	{ "videoHeight", media_video_height, NULL },
	{ NULL, NULL, NULL }
};

/* The HTMLMediaElement interface. */
const struct bind_interface bind_html_media_element_interface = {
	"HTMLMediaElement", BIND_HTML_ELEMENT, 0, NULL, media_attributes, media_operations, media_constants
};

/* The HTMLVideoElement interface (video elements'). */
const struct bind_interface bind_html_video_element_interface = {
	"HTMLVideoElement", BIND_HTML_MEDIA_ELEMENT, 0, NULL, video_attributes, NULL, NULL
};

/* The HTMLAudioElement interface (audio elements'). */
const struct bind_interface bind_html_audio_element_interface = {
	"HTMLAudioElement", BIND_HTML_MEDIA_ELEMENT, 0, NULL, NULL, NULL, NULL
};

/* Finds the element of this (a TypeError for anything else). */
static int
media_this(
	struct vm_realm *realm,
	vm_value this_value,
	struct dom_element **element)
{
	struct dom_node *node;
	int status;

	/* The node, which must be an element. */
	node = bind_node_of(this_value);
	if (node == NULL || node->type != DOM_ELEMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the element. */
	*element = (struct dom_element *)node;
	return 0;
}

/* Asks the host about this element (its state, or a play, a pause or a seek); a host without media reports none. */
static int
media_ask(
	struct vm_realm *realm,
	vm_value this_value,
	int request,
	double value,
	struct bind_media *state)
{
	struct dom_element *element;
	struct bind_window *window;
	int status;

	/* The element. */
	memset(state, 0, sizeof(*state));
	state->paused = 1;
	status = media_this(realm, this_value, &element);
	if (status != 0)
		return status;

	/* The host's answer (an element without media is paused and has nothing). */
	window = bind_window_of(realm);
	if (window->host.media == NULL)
		return 0;
	(void)window->host.media(window->host.context, element, request, value, state);
	return 0;
}

/* Reports whether the element is paused (paused). */
static int
media_paused(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_media state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The host's state. */
	status = media_ask(realm, this_value, BIND_MEDIA_STATE, 0.0, &state);
	if (status != 0)
		return status;
	*result = vm_value_boolean(state.paused);
	return 0;
}

/* Reports whether the element played to its end (ended). */
static int
media_ended(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_media state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The host's state. */
	status = media_ask(realm, this_value, BIND_MEDIA_STATE, 0.0, &state);
	if (status != 0)
		return status;
	*result = vm_value_boolean(state.ended);
	return 0;
}

/* Reports where the element is, in seconds (currentTime). */
static int
media_current_time_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_media state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The host's state. */
	status = media_ask(realm, this_value, BIND_MEDIA_STATE, 0.0, &state);
	if (status != 0)
		return status;
	*result = vm_value_number(state.current_time);
	return 0;
}

/* Moves the element to a time in seconds (currentTime =). */
static int
media_current_time_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_media state;
	double seconds;
	int status;

	/* The time asked. */
	seconds = 0.0;
	if (count > 0U) {
		status = vm_to_number(realm, args[0], &seconds);
		if (status != 0)
			return status;
	}

	/* Asked of the host. */
	status = media_ask(realm, this_value, BIND_MEDIA_SEEK, seconds, &state);
	*result = VM_VALUE_UNDEFINED;
	return status;
}

/* Reports the element's length in seconds, NaN before it is known (duration). */
static int
media_duration(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_media state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The host's state; nothing known is NaN. */
	status = media_ask(realm, this_value, BIND_MEDIA_STATE, 0.0, &state);
	if (status != 0)
		return status;
	*result = vm_value_number(state.duration);
	if (state.ready_state == MEDIA_HAVE_NOTHING)
		*result = vm_value_number(0.0 / 0.0);
	return 0;
}

/* Reports how ready the element is (readyState). */
static int
media_ready_state(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_media state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The host's state. */
	status = media_ask(realm, this_value, BIND_MEDIA_STATE, 0.0, &state);
	if (status != 0)
		return status;
	*result = vm_value_int32(state.ready_state);
	return 0;
}

/* Reports whether the element has the muted attribute (muted). */
static int
media_muted_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	const struct dom_attribute *attribute;
	struct dom_element *element;
	struct vm_string *name;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The element and the attribute. */
	status = media_this(realm, this_value, &element);
	if (status != 0)
		return status;
	name = vm_atom_from_ascii(realm->heap, "muted");
	if (name == NULL)
		return ENOMEM;
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, name);
	*result = vm_value_boolean(attribute != NULL);
	return 0;
}

/* Sets or takes away the muted attribute (muted =). */
static int
media_muted_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *name;
	struct vm_string *empty;
	int muted;
	int status;

	/* The element and the attribute's name. */
	*result = VM_VALUE_UNDEFINED;
	status = media_this(realm, this_value, &element);
	if (status != 0)
		return status;
	name = vm_atom_from_ascii(realm->heap, "muted");
	empty = vm_atom_from_ascii(realm->heap, "");
	if (name == NULL || empty == NULL)
		return ENOMEM;

	/* Set or taken away. */
	muted = 0;
	if (count > 0U)
		muted = vm_to_boolean(args[0]);
	if (muted)
		status = dom_element_set_attribute(element, name, empty);
	else
		status = dom_element_remove_attribute(element, name);
	return status;
}

/* Asks the element to play; returns a Promise resolved at once (play()). */
static int
media_play(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_media state;
	vm_value promise;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Asked of the host. */
	status = media_ask(realm, this_value, BIND_MEDIA_PLAY, 0.0, &state);
	if (status != 0)
		return status;

	/* The Promise, resolved. */
	status = vm_promise_create(realm, NULL, &promise);
	if (status != 0)
		return status;
	status = vm_promise_resolve(realm, promise, VM_VALUE_UNDEFINED);
	if (status != 0)
		return status;
	*result = promise;
	return 0;
}

/* Asks the element to pause (pause()). */
static int
media_pause(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_media state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Asked of the host. */
	status = media_ask(realm, this_value, BIND_MEDIA_PAUSE, 0.0, &state);
	*result = VM_VALUE_UNDEFINED;
	return status;
}

/* Starts the element's loading again: nothing in this pass (load()). */
static int
media_load(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* this must be an element. */
	status = media_this(realm, this_value, &element);
	*result = VM_VALUE_UNDEFINED;
	return status;
}

/*
 * Tells whether a type can be played (canPlayType): "maybe" for the
 * containers the reader knows (MP4 and WebM, video or audio), "" for
 * others.
 */
static int
media_can_play_type(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	static const char *const known[] = { "video/mp4", "audio/mp4", "audio/x-m4a", "video/webm", "audio/webm", "video/x-matroska", NULL };
	struct dom_element *element;
	struct vm_string *type;
	struct vm_string *answer;
	struct wb_buffer bytes;
	const char *text;
	size_t length;
	size_t index;
	int same;
	int status;

	/* this must be an element, and the type a string. */
	status = media_this(realm, this_value, &element);
	if (status != 0)
		return status;
	type = NULL;
	if (count > 0U) {
		status = vm_to_string(realm, args[0], &type);
		if (status != 0)
			return status;
	}

	/* The type's bytes. */
	wb_buffer_init(&bytes);
	if (type != NULL) {
		status = vm_string_to_utf8(type, &bytes);
		if (status != 0) {
			wb_buffer_release(&bytes);
			return status;
		}
	}

	/* A known container's type, alone or before its parameters. */
	answer = vm_atom_from_ascii(realm->heap, "");
	text = wb_buffer_string(&bytes);
	for (index = 0; type != NULL && known[index] != NULL; index++) {
		length = strlen(known[index]);
		same = strncmp(text, known[index], length);
		if (same == 0 && (text[length] == '\0' || text[length] == ';' || text[length] == ' '))
			answer = vm_atom_from_ascii(realm->heap, "maybe");
	}

	/* The bytes are not needed after. */
	wb_buffer_release(&bytes);

	/* The answer. */
	if (answer == NULL)
		return ENOMEM;
	*result = vm_value_cell(answer);
	return 0;
}

/* Reports the video's width in pixels, 0 before it is known (videoWidth). */
static int
media_video_width(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_media state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The host's state. */
	status = media_ask(realm, this_value, BIND_MEDIA_STATE, 0.0, &state);
	if (status != 0)
		return status;
	*result = vm_value_int32(state.width);
	return 0;
}

/* Reports the video's height in pixels, 0 before it is known (videoHeight). */
static int
media_video_height(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_media state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The host's state. */
	status = media_ask(realm, this_value, BIND_MEDIA_STATE, 0.0, &state);
	if (status != 0)
		return status;
	*result = vm_value_int32(state.height);
	return 0;
}
