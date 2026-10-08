/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A page's media (ws121-p004): each <video> and <audio> with a source (its
 * src, or the first <source> child's) is played by an engine of libmedia
 * (userland/desktop/libmedia/media.h), which reads and decodes on a thread
 * of its own.  A local file is read by its path; an http or https source is
 * fetched whole with the page's loader (without one, at once) and a data:
 * URL decoded, and the engine reads those bytes from memory.  The page
 * polls each engine's wake with the loader's descriptors: when an engine
 * opened, the video's size lays the page out again; when a picture came,
 * it is scaled into the element's bitmap (the video's own size, which the
 * layout and the painting take as an <img>'s) and the page is painted
 * again.  The engines go with the page.
 *
 * Normal playing only (plan/ws121/phase004): autoplay plays a muted
 * element at once (one with sound waits for a script's play(), ws121-p005);
 * the media are not given up when an element leaves the document, and an
 * element's later change of source is not followed.
 */

#include "page/page.h"
#include "net/net.h"

#include "userland/desktop/libmedia/media.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The deepest element nesting the walk descends (the parser caps nesting too). */
#define MEDIA_DEPTH		512

/* The size of a <video> without a picture yet (HTML's default object size). */
#define MEDIA_DEFAULT_WIDTH	300
#define MEDIA_DEFAULT_HEIGHT	150

/* The controls' bar (ws121-p006): its height's share of the video and its limits, in the video's pixels. */
#define MEDIA_BAR_SHARE		8
#define MEDIA_BAR_MIN		20
#define MEDIA_BAR_MAX		48

/* The bar's colours: its ground, its marks, the time played. */
#define MEDIA_BAR_GROUND	0x000000U
#define MEDIA_BAR_MARK		0xffffffU
#define MEDIA_BAR_PLAYED	0x3d8bfdU
#define MEDIA_BAR_TRACK		0x8a8f98U

/* The largest media fetched whole (plan/ws121/phase001 U7). */
#define MEDIA_BODY_MAX		((size_t)64 * 1024U * 1024U)

/*
 * One media element of the page: the element (a root of the page's heap
 * while the page lives), its location, the request while it is fetched,
 * the bytes it was fetched into (the engine's source reads them), the
 * engine, the state last read from it, the element's picture, whether it
 * failed, whether it is muted and asked to play, and the page.
 */
struct page_media {
	struct dom_element *element;
	char *location;
	struct net_request *request;
	unsigned char *bytes;
	size_t length;
	struct media_engine *engine;
	struct media_status status;
	struct img_bitmap bitmap;
	int failed;
	int muted;
	int play_wanted;
	int sized;
	double told_time;
	int controls;
	unsigned drawn_state;
	int drawn_position;
	struct page *page;
};

static int media_walk(struct page *page, struct dom_node *node, int depth);
static int media_add(struct page *page, struct dom_element *element);
static int media_source(struct page *page, const struct dom_element *element, struct wb_buffer *location, int *found);
static int media_attribute(struct page *page, const struct dom_element *element, const char *name, struct wb_buffer *value);
static void media_start(struct page_media *media, const unsigned char *bytes, size_t length);
static void media_arrived(void *context, struct net_request *request);
static int media_read_at(void *context, uint64_t offset, void *data, size_t size);
static struct page_media *media_find(const struct page *page, const struct dom_element *element);
static int media_follow(struct page_media *media);
static void media_events(struct page_media *media, unsigned before);
static void media_fire(struct page_media *media, const char *type);
static void media_log_line(void *context, const char *line);
static void media_controls(struct page_media *media);
static int media_bar_height(int height);
static void media_fill(struct img_bitmap *bitmap, int x, int y, int width, int height, uint32_t color, unsigned alpha);

/*
 * Starts a page's table of media empty, and sends libmedia's log lines to
 * standard error as "BROWSER MEDIA" lines (for the tests).
 */
void
page_media_init(
	struct page *page)
{
	/* No media yet. */
	wb_vector_init(&page->media, sizeof(struct page_media *));
	media_set_log(media_log_line, NULL);
}

/*
 * Ends every engine of the page and frees the table.
 */
void
page_media_release(
	struct page *page)
{
	struct page_media *media;
	size_t index;

	/* Each media: its engine first (its thread reads the bytes), then the rest. */
	for (index = 0; index < page->media.count; index++) {
		media = *(struct page_media **)wb_vector_at(&page->media, index);
		media_engine_close(media->engine);
		net_request_cancel(media->request);
		vm_heap_remove_root(page->heap, (struct vm_cell **)&media->element);
		free(media->bytes);
		free(media->location);
		img_bitmap_release(&media->bitmap);
		free(media);
	}

	/* The table itself. */
	wb_vector_release(&page->media);
}

/*
 * Finds the <video> and <audio> elements of the document that are not in
 * the table yet and starts their media.
 */
int
page_load_media(
	struct page *page)
{
	int error;

	/* Every element of the document. */
	error = media_walk(page, &page->document->node, 0);
	return error;
}

/*
 * Finds the picture a <video> shows for the layout and the painting: its
 * bitmap once a picture came, NULL before (the layout then gives it its
 * default size).
 */
const struct img_bitmap *
page_media_bitmap(
	const struct page *page,
	const struct dom_element *element)
{
	struct page_media *media;

	/* The element's media with a picture. */
	media = media_find(page, element);
	if (media == NULL || media->bitmap.pixels == NULL)
		return NULL;
	return &media->bitmap;
}

/*
 * Lists the engines' wakes for the caller's poll, after up to capacity
 * descriptors; returns how many were written.
 */
size_t
page_media_poll_fds(
	const struct page *page,
	struct pollfd *fds,
	size_t capacity)
{
	struct page_media *media;
	size_t count;
	size_t index;

	/* Each engine's wake. */
	count = 0;
	for (index = 0; index < page->media.count && count < capacity; index++) {
		media = *(struct page_media **)wb_vector_at(&page->media, index);
		if (media->engine == NULL)
			continue;
		fds[count].fd = media_engine_wake_fd(media->engine);
		fds[count].events = POLLIN;
		fds[count].revents = 0;
		count++;
	}

	/* The count. */
	return count;
}

/*
 * Reports how long the caller may wait before the next picture of a
 * playing video is due (ms), or -1 for none.
 */
int
page_media_timeout(
	const struct page *page)
{
	struct page_media *media;
	size_t index;
	int timeout;

	/* A playing video's next picture: a frame's time; other playing media, their time's updates. */
	timeout = -1;
	for (index = 0; index < page->media.count; index++) {
		media = *(struct page_media **)wb_vector_at(&page->media, index);
		if (media->engine == NULL || media->status.state != MEDIA_PLAYING)
			continue;
		if (media->status.has_video)
			timeout = 10;
		else if (timeout < 0)
			timeout = 250;
	}

	/* The wait. */
	return timeout;
}

/*
 * Follows each engine: its state, the size its video came with (the page
 * is laid out again), and the picture whose time has come (the page is
 * painted again).
 */
void
page_media_process(
	struct page *page)
{
	struct page_media *media;
	size_t index;
	int drawn;

	/* Each media with an engine. */
	for (index = 0; index < page->media.count; index++) {
		media = *(struct page_media **)wb_vector_at(&page->media, index);
		if (media->engine == NULL)
			continue;
		drawn = media_follow(media);
		if (drawn)
			page->media_generation++;
	}
}

/*
 * Asks a media element to play or to pause (the scripts' play() and
 * pause(), and autoplay); one without media does nothing.  Returns 0, or
 * ENOENT when the element has none.
 */
int
page_media_play(
	struct page *page,
	const struct dom_element *element,
	int play)
{
	struct page_media *media;

	/* The element's media. */
	media = media_find(page, element);
	if (media == NULL)
		return ENOENT;

	/* Remembered until the engine is open, or asked of it now (the engine plays an ended file from its start). */
	media->play_wanted = play;
	if (media->engine == NULL || media->status.state == MEDIA_OPENING)
		return 0;
	if (play)
		media_engine_play(media->engine);
	else
		media_engine_pause(media->engine);
	return 0;
}

/*
 * Reports a media element's state as the engine last told (zero for an
 * element without media); returns 1 when it has media.
 */
int
page_media_status(
	const struct page *page,
	const struct dom_element *element,
	struct media_status *status)
{
	struct page_media *media;

	/* The element's media. */
	memset(status, 0, sizeof(*status));
	media = media_find(page, element);
	if (media == NULL)
		return 0;
	*status = media->status;
	if (media->failed)
		status->state = MEDIA_FAILED;
	return 1;
}

/*
 * Moves a media element to a time (seconds); one without an engine does
 * nothing.
 */
void
page_media_seek(
	struct page *page,
	const struct dom_element *element,
	double seconds)
{
	struct page_media *media;

	/* The element's engine. */
	media = media_find(page, element);
	if (media == NULL || media->engine == NULL)
		return;
	media_engine_seek(media->engine, seconds);
}

/*
 * The default action of a click on a <video> with controls (ws121-p006):
 * on the bar's button it plays or pauses, on its track it goes to the time
 * at that place, elsewhere on the video it plays or pauses.  x and y are
 * the click's place in the document (pixels).  Returns 1 when the click
 * was the media's.
 */
int
page_media_click(
	struct page *page,
	struct dom_element *element,
	int x,
	int y)
{
	struct layout_rect rect;
	struct page_media *media;
	double left;
	double top;
	double width;
	double height;
	double bar;
	double fraction;
	int found;

	/* A video with controls, and its box. */
	media = media_find(page, element);
	if (media == NULL || !media->controls || media->engine == NULL || media->bitmap.pixels == NULL)
		return 0;
	found = layout_node_bounds(&page->layout, &element->node, &rect);
	if (!found)
		return 0;
	left = (double)layout_to_px(rect.x);
	top = (double)layout_to_px(rect.y);
	width = (double)layout_to_px(rect.width);
	height = (double)layout_to_px(rect.height);
	if (width <= 0.0 || height <= 0.0)
		return 0;

	/* The bar's height in the box, as drawn in the picture. */
	bar = height * (double)media_bar_height(media->bitmap.height) / (double)media->bitmap.height;

	/* Above the bar, or on its button: play or pause. */
	if ((double)y < top + height - bar || (double)x < left + bar) {
		(void)page_media_play(page, element, !media->play_wanted);
		return 1;
	}

	/* On the track: the time at that place. */
	fraction = ((double)x - (left + bar)) / (width - bar * 1.5);
	if (fraction < 0.0)
		fraction = 0.0;
	if (fraction > 1.0)
		fraction = 1.0;
	media_engine_seek(media->engine, fraction * media->status.duration);
	return 1;
}

/*
 * The scripts' questions about a media element (bind_host.media,
 * ws121-p005): its state, and a play, a pause or a seek.  An element a
 * script asks to play before the page found it is started then.
 */
int
page_media_host(
	void *context,
	struct dom_element *element,
	int request,
	double value,
	struct bind_media *state)
{
	struct page_media *media;
	struct page *page;
	int error;

	/* The element's media; a play starts one not found yet. */
	page = context;
	media = media_find(page, element);
	if (media == NULL && request == BIND_MEDIA_PLAY) {
		error = media_add(page, element);
		if (error != 0)
			return error;
		media = media_find(page, element);
	}

	/* What is asked. */
	if (media != NULL && request == BIND_MEDIA_PLAY)
		(void)page_media_play(page, element, 1);
	else if (media != NULL && request == BIND_MEDIA_PAUSE)
		(void)page_media_play(page, element, 0);
	else if (media != NULL && request == BIND_MEDIA_SEEK)
		page_media_seek(page, element, value);

	/* The state as the engine last told: nothing without media. */
	memset(state, 0, sizeof(*state));
	state->paused = 1;
	if (media == NULL)
		return 0;
	state->has = 1;
	state->paused = !media->play_wanted;
	state->ended = media->status.state == MEDIA_ENDED;
	state->current_time = media->status.position;
	state->duration = media->status.duration;
	state->width = media->status.width;
	state->height = media->status.height;
	state->error = media->failed;
	if (media->status.state != MEDIA_OPENING && !media->failed)
		state->ready_state = 4;
	return 0;
}

/* Starts the media of a node's <video> and <audio> descendants (and its own). */
static int
media_walk(
	struct page *page,
	struct dom_node *node,
	int depth)
{
	struct page_media *known;
	struct dom_element *element;
	struct dom_node *child;
	int error;

	/* Stops at the depth the parser stops at. */
	if (depth > MEDIA_DEPTH)
		return 0;

	/* A <video> or an <audio> not in the table yet. */
	if (node->type == DOM_ELEMENT) {
		element = (struct dom_element *)node;
		known = NULL;
		if (element->ns == DOM_NS_HTML && (element->tag == DOM_TAG_VIDEO || element->tag == DOM_TAG_AUDIO))
			known = media_find(page, element);
		if (element->ns == DOM_NS_HTML && (element->tag == DOM_TAG_VIDEO || element->tag == DOM_TAG_AUDIO) && known == NULL) {
			error = media_add(page, element);
			if (error != 0)
				return error;
		}
	}

	/* The children, in document order. */
	for (child = node->first_child; child != NULL; child = child->next) {
		error = media_walk(page, child, depth + 1);
		if (error != 0)
			return error;
	}

	/* Succeeded: the subtree's media are started. */
	return 0;
}

/*
 * Adds a media element to the table and starts its media (a source that
 * cannot be had marks it failed): a path is played at once, a remote
 * source fetched first.
 */
static int
media_add(
	struct page *page,
	struct dom_element *element)
{
	struct page_media *media;
	struct wb_buffer location;
	struct wb_buffer value;
	int autoplay;
	int remote;
	int found;
	int error;

	/* The entry, allocated alone so that its bitmap stays where it is while the table grows. */
	media = calloc(1, sizeof(*media));
	if (media == NULL)
		return ENOMEM;
	media->element = element;
	media->page = page;

	/* Muted, and playing at once when muted with autoplay. */
	wb_buffer_init(&value);
	media->muted = media_attribute(page, element, "muted", &value);
	media->controls = media_attribute(page, element, "controls", &value);
	autoplay = media_attribute(page, element, "autoplay", &value);
	if (autoplay && media->muted)
		media->play_wanted = 1;
	wb_buffer_release(&value);

	/* The element is kept while the page lives (the engine's thread reports to it). */
	error = vm_heap_add_root(page->heap, (struct vm_cell **)&media->element);
	if (error != 0) {
		free(media);
		return error;
	}

	/* In the table. */
	error = wb_vector_push(&page->media, &media);
	if (error != 0) {
		vm_heap_remove_root(page->heap, (struct vm_cell **)&media->element);
		free(media);
		return error;
	}

	/* Its source; an element without one waits for nothing. */
	wb_buffer_init(&location);
	error = media_source(page, element, &location, &found);
	if (error != 0 || !found) {
		wb_buffer_release(&location);
		media->failed = !found;
		return error;
	}

	/* The location kept. */
	media->location = strdup(wb_buffer_string(&location));
	wb_buffer_release(&location);
	if (media->location == NULL)
		return ENOMEM;

	/* A local file by its path. */
	if (media->location[0] == '/') {
		media_start(media, NULL, 0U);
		return 0;
	}

	/* A remote source with the loader is fetched without blocking. */
	remote = net_loader_takes(media->location);
	if (page->loader != NULL && remote) {
		error = net_loader_fetch(page->loader, media->location, media_arrived, media, &media->request);
		if (error != 0)
			media->failed = 1;
		return 0;
	}

	/* Otherwise its bytes at once (a data: URL, or no loader). */
	wb_buffer_init(&value);
	error = page_fetch(page->base, media->location, &value, NULL);
	if (error == 0)
		media_start(media, value.data, value.length);
	else
		media->failed = 1;
	wb_buffer_release(&value);
	return 0;
}

/*
 * Writes a media element's source resolved against the page's location:
 * its src, or the src of its first <source> child; *found is 0 for none.
 */
static int
media_source(
	struct page *page,
	const struct dom_element *element,
	struct wb_buffer *location,
	int *found)
{
	const struct dom_element *child_element;
	const struct dom_node *child;
	struct wb_buffer href;
	int has;
	int error;

	/* The element's own src, or the first <source> child's. */
	*found = 0;
	wb_buffer_init(&href);
	has = media_attribute(page, element, "src", &href);
	for (child = element->node.first_child; child != NULL && !has; child = child->next) {
		if (child->type != DOM_ELEMENT)
			continue;
		child_element = (const struct dom_element *)child;
		if (child_element->ns != DOM_NS_HTML || child_element->tag != DOM_TAG_SOURCE)
			continue;
		has = media_attribute(page, child_element, "src", &href);
	}

	/* None, or no location to resolve it against. */
	if (!has || page->base == NULL) {
		wb_buffer_release(&href);
		return 0;
	}

	/* The location it names; a source that is not a URL has none. */
	error = page_resolve_location(page->base, wb_buffer_string(&href), location);
	wb_buffer_release(&href);
	if (error == EINVAL)
		return 0;
	if (error != 0)
		return error;

	/* Succeeded: the location is written. */
	*found = 1;
	return 0;
}

/* Reads an attribute of an element into a buffer (emptied first); 1 when the element has it. */
static int
media_attribute(
	struct page *page,
	const struct dom_element *element,
	const char *name,
	struct wb_buffer *value)
{
	const struct dom_attribute *attribute;
	struct vm_string *atom;
	int error;

	/* The attribute by its name. */
	wb_buffer_clear(value);
	atom = vm_atom_from_ascii(page->heap, name);
	if (atom == NULL)
		return 0;
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, atom);
	if (attribute == NULL)
		return 0;

	/* Its value. */
	error = vm_string_to_utf8(attribute->value, value);
	return error == 0;
}

/*
 * Starts a media's engine: from its path (bytes NULL), or from a copy of
 * the bytes fetched, which the engine reads from memory.  A failure marks
 * the media failed.
 */
static void
media_start(
	struct page_media *media,
	const unsigned char *bytes,
	size_t length)
{
	struct media_source source;
	unsigned flags;
	int error;

	/* The sound, unless muted. */
	flags = MEDIA_SOUND;
	if (media->muted)
		flags = 0U;

	/* A local file by its path. */
	if (bytes == NULL) {
		error = media_engine_open(media->location, NULL, flags, &media->engine);
		if (error != 0)
			media->failed = 1;
		return;
	}

	/* The bytes kept, read through a source. */
	if (length > MEDIA_BODY_MAX || length == 0U) {
		media->failed = 1;
		return;
	}

	/* The copy. */
	media->bytes = malloc(length);
	if (media->bytes == NULL) {
		media->failed = 1;
		return;
	}

	/* Read through a source. */
	memcpy(media->bytes, bytes, length);
	media->length = length;
	source.read_at = media_read_at;
	source.size = length;
	source.context = media;
	error = media_engine_open(NULL, &source, flags, &media->engine);
	if (error != 0)
		media->failed = 1;
}

/* The loader's callback for a media: its body played when it came (a failure, or a status other than 2xx, marks it failed). */
static void
media_arrived(
	void *context,
	struct net_request *request)
{
	const struct net_response *response;
	struct page_media *media;
	int error;

	/* The media, which waits no longer. */
	media = context;
	media->request = NULL;

	/* A response with the media's bytes. */
	error = net_request_error(request);
	response = net_request_response(request);
	if (error == 0 && (response->status < 200 || response->status > 299))
		error = EINVAL;
	if (error != 0) {
		media->failed = 1;
		return;
	}

	/* Played from a copy of the body. */
	media_start(media, (const unsigned char *)response->body.data, response->body.length);
}

/* The engine's source of fetched bytes: a copy of the bytes asked (they stay while the engine lives). */
static int
media_read_at(
	void *context,
	uint64_t offset,
	void *data,
	size_t size)
{
	struct page_media *media;

	/* Within the bytes. */
	media = context;
	if (offset > media->length || size > media->length - offset)
		return EINVAL;
	memcpy(data, media->bytes + offset, size);
	return 0;
}

/* Finds an element's media in the table. */
static struct page_media *
media_find(
	const struct page *page,
	const struct dom_element *element)
{
	struct page_media *media;
	size_t index;

	/* Each media of the table. */
	for (index = 0; index < page->media.count; index++) {
		media = *(struct page_media **)wb_vector_at(&page->media, index);
		if (media->element == element)
			return media;
	}

	/* Not in the table. */
	return NULL;
}

/*
 * Reads an engine's state: when it opened, the video's size (its bitmap
 * made, the page laid out again) and a play asked meanwhile; then the
 * picture whose time has come.  Returns 1 when the bitmap was drawn.
 */
static int
media_follow(
	struct page_media *media)
{
	unsigned before;
	double next;
	int drawn;
	int error;

	/* The state. */
	before = media->status.state;
	media_engine_status(media->engine, &media->status);

	/* Opened: the video's picture of its size, a play asked meanwhile. */
	if (before == MEDIA_OPENING && media->status.state == MEDIA_PAUSED) {
		if (media->status.width > 0 && media->status.height > 0 && media->bitmap.pixels == NULL) {
			error = img_bitmap_create(&media->bitmap, media->status.width, media->status.height);
			if (error == 0)
				media->sized = 1;
		}

		/* A play asked before it opened. */
		if (media->play_wanted)
			media_engine_play(media->engine);
	}

	/* A failure is kept. */
	if (media->status.state == MEDIA_FAILED)
		media->failed = 1;

	/* The events of what changed (the scripts they run may play or pause it). */
	media_events(media, before);

	/* The picture whose time has come, into the bitmap (a new serial: the GPU takes it anew). */
	if (media->bitmap.pixels == NULL)
		return 0;
	drawn = media_engine_picture(media->engine, media->bitmap.pixels, (size_t)media->bitmap.width, media->bitmap.width,
	    media->bitmap.height, &next);

	/* The controls changed without a new picture: the picture again under them. */
	if (!drawn && media->controls &&
	    (media->drawn_state != media->status.state || media->drawn_position != (int)(media->status.position * 4.0)))
		drawn = media_engine_redraw(media->engine, media->bitmap.pixels, (size_t)media->bitmap.width, media->bitmap.width,
		    media->bitmap.height);
	if (!drawn)
		return 0;

	/* The controls over it. */
	if (media->controls)
		media_controls(media);
	img_bitmap_renew(&media->bitmap);

	/* The first picture: the page is laid out again at the video's size. */
	if (media->sized) {
		media->sized = 0;
		media->page->images_generation++;
	}

	/* Drawn. */
	return 1;
}

/*
 * Fires the events of an engine's change of state, and timeupdate four
 * times a second while it plays.
 */
static void
media_events(
	struct page_media *media,
	unsigned before)
{
	unsigned now;

	/* Opened: what is known of it. */
	now = media->status.state;
	if (before == MEDIA_OPENING && now != MEDIA_OPENING && now != MEDIA_FAILED) {
		media_fire(media, "durationchange");
		media_fire(media, "loadedmetadata");
		media_fire(media, "loadeddata");
		media_fire(media, "canplay");
	}

	/* A failure. */
	if (now == MEDIA_FAILED && before != MEDIA_FAILED)
		media_fire(media, "error");
	if (now == MEDIA_PLAYING && before != MEDIA_PLAYING) {
		media_fire(media, "play");
		media_fire(media, "playing");
	}

	/* A pause. */
	if (now == MEDIA_PAUSED && before == MEDIA_PLAYING)
		media_fire(media, "pause");

	/* The end: paused there. */
	if (now == MEDIA_ENDED && before != MEDIA_ENDED) {
		media->play_wanted = 0;
		media->told_time = media->status.position;
		media_fire(media, "timeupdate");
		media_fire(media, "pause");
		media_fire(media, "ended");
		return;
	}

	/* The time moving on. */
	if (now == MEDIA_PLAYING && (media->status.position - media->told_time >= 0.25 || media->status.position < media->told_time)) {
		media->told_time = media->status.position;
		media_fire(media, "timeupdate");
	}
}

/* Fires a media event at the element (it does not bubble). */
static void
media_fire(
	struct page_media *media,
	const char *type)
{
	int canceled;

	/* The window's, when the page runs scripts. */
	if (media->page->window == NULL)
		return;
	(void)bind_fire_event(media->page->window, &media->element->node, type, 0U, &canceled);
}

/* The height of the controls' bar for a video of a height (pixels of the video). */
static int
media_bar_height(
	int height)
{
	int bar;

	/* A share of the height, within limits, never more than the video. */
	bar = height / MEDIA_BAR_SHARE;
	if (bar < MEDIA_BAR_MIN)
		bar = MEDIA_BAR_MIN;
	if (bar > MEDIA_BAR_MAX)
		bar = MEDIA_BAR_MAX;
	if (bar > height)
		bar = height;
	return bar;
}

/*
 * Draws the controls over the bottom of the picture: a dark band, at its
 * left the play sign (paused) or the pause sign (playing), and the track
 * of the time with the part played.
 */
static void
media_controls(
	struct page_media *media)
{
	struct img_bitmap *bitmap;
	int bar;
	int top;
	int mark;
	int row;
	int half;
	int track_left;
	int track_width;
	int played;
	int line;

	/* The band. */
	bitmap = &media->bitmap;
	bar = media_bar_height(bitmap->height);
	top = bitmap->height - bar;
	media_fill(bitmap, 0, top, bitmap->width, bar, MEDIA_BAR_GROUND, 150U);
	media->drawn_state = media->status.state;
	media->drawn_position = (int)(media->status.position * 4.0);

	/* The sign in a square at the left: two bars while playing, a triangle otherwise. */
	mark = bar / 2;
	if (media->play_wanted) {
		media_fill(bitmap, bar / 2 - mark / 2, top + bar / 4, mark / 3, mark, MEDIA_BAR_MARK, 255U);
		media_fill(bitmap, bar / 2 + mark / 6, top + bar / 4, mark / 3, mark, MEDIA_BAR_MARK, 255U);
	} else {
		half = mark / 2;
		for (row = 0; row < mark; row++) {
			/* The triangle's width at the row: widest in its middle. */
			if (row < half)
				line = row;
			else
				line = mark - 1 - row;
			media_fill(bitmap, bar / 2 - half / 2, top + bar / 4 + row, line + 1, 1, MEDIA_BAR_MARK, 255U);
		}
	}

	/* The track and the part played. */
	track_left = bar;
	track_width = bitmap->width - bar - bar / 2;
	if (track_width <= 0)
		return;
	media_fill(bitmap, track_left, top + bar / 2 - 2, track_width, 4, MEDIA_BAR_TRACK, 255U);
	played = 0;
	if (media->status.duration > 0.0)
		played = (int)((double)track_width * media->status.position / media->status.duration);
	if (played > track_width)
		played = track_width;
	media_fill(bitmap, track_left, top + bar / 2 - 2, played, 4, MEDIA_BAR_PLAYED, 255U);
}

/* Blends a colour (0xRRGGBB) at an opacity over a rectangle of a bitmap, within it. */
static void
media_fill(
	struct img_bitmap *bitmap,
	int x,
	int y,
	int width,
	int height,
	uint32_t color,
	unsigned alpha)
{
	uint32_t *pixel;
	unsigned channel;
	unsigned below;
	unsigned over;
	unsigned shift;
	uint32_t mixed;
	int column;
	int row;

	/* Each pixel inside the bitmap. */
	for (row = y; row < y + height; row++) {
		if (row < 0 || row >= bitmap->height)
			continue;
		for (column = x; column < x + width; column++) {
			if (column < 0 || column >= bitmap->width)
				continue;

			/* Each colour channel mixed; the pixel stays opaque. */
			pixel = &bitmap->pixels[(size_t)row * (size_t)bitmap->width + (size_t)column];
			mixed = 0xff000000U;
			for (shift = 0; shift < 24U; shift += 8U) {
				below = (*pixel >> shift) & 0xffU;
				over = (color >> shift) & 0xffU;
				channel = (over * alpha + below * (255U - alpha)) / 255U;
				mixed |= (uint32_t)channel << shift;
			}

			/* The mixed pixel. */
			*pixel = mixed;
		}
	}
}

/* Writes one of libmedia's log lines on standard error. */
static void
media_log_line(
	void *context,
	const char *line)
{
	/* The line, marked as the browser's. */
	(void)context;
	fprintf(stderr, "BROWSER MEDIA %s\n", line);
}
