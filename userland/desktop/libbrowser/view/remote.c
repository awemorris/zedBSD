/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The frames of other agents of a view of its own threads (view/remote.h).
 *
 * A frame (struct view_remote) is made by its iframe's first response of
 * another origin, on the thread of its parent's document holding the
 * view's agent, and ended by that agent's holder (the iframe left its
 * document, shows a document of the view's agent again, or its page is put
 * away).  It is reached by its number, which no other frame of the view
 * ever has.
 *
 * Locks: the frames' lock (struct view_remotes's) guards the list, the
 * queue to the view's agent (up), each frame's attachment, the size its
 * parent gives it, its scene and what its proxies read (its frames'
 * count; its parent's and top's); a frame's lock guards its inbox (the
 * responses it is to show, the messages and the inputs for it) and its
 * wake.  The frames' lock is taken before a frame's, never the other way;
 * a frame's thread (frames.c's) is woken under the frames' lock.  The
 * element, the page and the runner of its parent are the view's agent's:
 * read by its holder only.
 *
 * A frame's thread waits for its inbox or its next timer, and runs one
 * task of its page's agent a round: the responses it is to show (its page
 * made anew for each), the messages and the inputs, the callbacks of its
 * requests, its timers and images, the navigation its script asked for
 * (to its parent's agent, whose frame-load does it: a document of the
 * parent's origin goes back into the parent's agent); then it lays out,
 * paints and commits its page when it changed, and wakes the drawing.
 */

#include "view/remote.h"
#include "view/frames.h"
#include "view/front.h"
#include "page/page.h"
#include "bind/internal.h"
#include "base/base.h"
#include "base/os-time.h"
#include "base/thread.h"
#include "net/net.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The stack of a frame's thread, as the page's own script thread's. */
#define REMOTE_STACK_64		(8U * 1024U * 1024U)
#define REMOTE_STACK_32		(2U * 1024U * 1024U)

/* How long one round dispatches the network's news, in nanoseconds (the rest at the next round). */
#define REMOTE_DISPATCH_NS	50000000ULL

/* The longest a frame's thread waits without a timer, in milliseconds (a wake comes sooner). */
#define REMOTE_WAIT_MAX		1000U

/* The most entries a frame's inbox holds (a message more is dropped). */
#define REMOTE_INBOX_MAX	4096U

/* What waits in a frame's inbox. */
enum remote_entry_kind {
	REMOTE_ENTRY_LOAD,
	REMOTE_ENTRY_MESSAGE,
	REMOTE_ENTRY_POINTER,
	REMOTE_ENTRY_KEY
};

/*
 * One entry of a frame's inbox: a response to show (url, mime, body), a
 * message (its tape, the sender's origin, the origin it may go to and what
 * the sender is to the frame), a pointer's input (type, pointer) or a
 * key's (type, key, code, repeat, modifiers).
 */
struct remote_entry {
	int kind;
	char *url;
	char *mime;
	size_t mime_length;
	unsigned char *body;
	size_t length;
	struct bind_tape tape;
	char *origin;
	char *target;
	int source;
	char type[16];
	struct page_pointer pointer;
	char *key;
	char *code;
	int repeat;
	unsigned modifiers;
};

/* What waits for the view's agent. */
enum remote_up_kind {
	REMOTE_UP_MESSAGE,
	REMOTE_UP_NAVIGATE,
	REMOTE_UP_LOADED
};

/*
 * One entry of the queue to the view's agent, of a frame: a message to its
 * parent or its top (target), a navigation of the frame itself (target
 * BIND_REMOTE_CHILD), of its parent or of its top, or the end of its load;
 * the runner is the thread that delivers it.
 */
struct remote_up {
	struct view_remote *remote;
	void *runner;
	int kind;
	int target;
	struct bind_tape tape;
	char *origin;
	char *target_origin;
	char *url;
	int replace;
};

/*
 * One frame of another agent.
 *
 * The holder of the view's agent's: element, page (the parent's),
 * runner (the thread of its parent's document), loading (its parent waits
 * for its load).  Under the frames' lock: attached, width, height, sized
 * (the size changed since the frame laid out), the scene and its scroll,
 * frames_count, parent_count, top_count, parent_is_top.  Under its own
 * lock: inbox, rung, ending.  The frame's thread's: own (its page), told
 * (its load was told), epoch_ns, laid (the size it was laid out at).
 * agent is its page's agent, published for the question of a page that
 * does not answer (through __atomic).  refs counts the frame's holders
 * (the list, the queue's entries) and is changed atomically.
 */
struct view_remote {
	struct view_remotes *remotes;
	uint64_t number;
	unsigned refs;
	struct wb_thread thread;
	struct wb_mutex lock;
	struct wb_cond changed;
	struct wb_vector inbox;
	int rung;
	int ending;
	struct net_loader_client *client;
	struct dom_element *element;
	struct page *page;
	void *runner;
	int loading;
	int attached;
	int width;
	int height;
	int sized;
	struct paint_scene *scene;
	layout_unit scroll_x;
	layout_unit scroll_y;
	int frames_count;
	int parent_count;
	int top_count;
	int parent_is_top;
	int dark;
	struct page *own;
	int told;
	uint64_t epoch_ns;
	int laid_width;
	int laid_height;
	struct vm_agent *agent;
};

/* The frames of other agents of a view. */
struct view_remotes {
	struct view_frames *frames;
	struct browser_view *view;
	struct wb_mutex lock;
	struct wb_vector live;
	struct wb_vector up;
	uint64_t next_number;
};

static int remote_post(void *context, struct bind_window *from, int kind, uint64_t frame, struct bind_tape *tape, const char *target_origin, const char *origin);
static int remote_navigate(void *context, struct bind_window *from, int kind, uint64_t frame, const char *url, int replace);
static int remote_info(void *context, int kind, uint64_t frame, int what);
static uint64_t remote_frame_number(void *context, const struct dom_element *element);
static int child_post(void *context, struct bind_window *from, int kind, uint64_t frame, struct bind_tape *tape, const char *target_origin, const char *origin);
static int child_navigate(void *context, struct bind_window *from, int kind, uint64_t frame, const char *url, int replace);
static int child_info(void *context, int kind, uint64_t frame, int what);
static uint64_t child_frame_number(void *context, const struct dom_element *element);
static struct view_remote *remote_find(struct view_remotes *remotes, uint64_t number);
static int remote_queue(struct view_remote *remote, struct remote_entry *entry, int bounded);
static int remote_up(struct view_remote *remote, struct remote_up *up);
static void remote_up_free(struct remote_up *up);
static void remote_entry_free(struct remote_entry *entry);
static void remote_notify(void *context);
static void remote_run(void *argument);
static void remote_round(struct view_remote *remote, struct wb_vector *taken);
static void remote_show(struct view_remote *remote, struct remote_entry *entry);
static void remote_input(struct view_remote *remote, struct remote_entry *entry);
static void remote_loaded_check(struct view_remote *remote);
static void remote_script_navigation(struct view_remote *remote);
static void remote_commit(struct view_remote *remote);
static uint64_t remote_deadline(struct view_remote *remote);
static double remote_time(const struct view_remote *remote);
static int remote_count_frames(struct page *page);
static int remote_count_document(struct dom_document *document);
static void remote_release(struct view_remote *remote);
static void remote_end(struct view_remote *remote);
static void remote_terminate(struct view_remote *remote, uint32_t task);
static void remote_set_agent(struct view_remote *remote, struct vm_agent *agent);
static int remote_navigate_self(struct view_remote *remote, const char *url, int replace);
static char *remote_strdup(const char *text, size_t length);

/* How the windows of the view's agent reach the frames (an iframe's proxy). */
const struct bind_remote_calls view_remote_parent_calls = {
	remote_post,
	remote_navigate,
	remote_info,
	remote_frame_number
};

/* How the window of a frame's page reaches its parent and its top. */
static const struct bind_remote_calls remote_child_calls = {
	child_post,
	child_navigate,
	child_info,
	child_frame_number
};

/* Makes the frames of other agents of a view's frames (none yet). */
int
view_remotes_create(
	struct view_frames *frames,
	struct view_remotes **remotes)
{
	struct view_remotes *made;
	int error;

	/* The record and its lock. */
	*remotes = NULL;
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->frames = frames;
	made->next_number = 1;
	wb_vector_init(&made->live, sizeof(struct view_remote *));
	wb_vector_init(&made->up, sizeof(struct remote_up));
	error = wb_mutex_init(&made->lock);
	if (error != 0) {
		free(made);
		return error;
	}

	/* Succeeded: no frame yet. */
	*remotes = made;
	return 0;
}

/* Tells the frames their view. */
void
view_remotes_set_view(
	struct view_remotes *remotes,
	struct browser_view *view)
{
	/* The view whose loader, fonts and drawing the frames use. */
	remotes->view = view;
}

/* Ends whatever frame is left and lets the record go (the view's agent held). */
void
view_remotes_destroy(
	struct view_remotes *remotes)
{
	struct remote_up *up;
	size_t index;

	/* Nothing to let go. */
	if (remotes == NULL)
		return;

	/* A frame still live (none should be) ends. */
	view_remotes_close_all(remotes);

	/* The queue, the list and the lock. */
	for (index = 0; index < remotes->up.count; index++) {
		up = wb_vector_at(&remotes->up, index);
		remote_up_free(up);
	}
	wb_vector_release(&remotes->up);
	wb_vector_release(&remotes->live);
	wb_mutex_destroy(&remotes->lock);
	free(remotes);
}

/* Ends every frame still live, its parent's page gone (none should be left: the pages' iframes ended theirs). */
void
view_remotes_close_all(
	struct view_remotes *remotes)
{
	struct view_remote *remote;

	/* Each, as a closed one, without its parent's element and page (gone). */
	if (remotes == NULL)
		return;
	while (view_remotes_count(remotes) != 0) {
		wb_mutex_lock(&remotes->lock);
		remote = *(struct view_remote **)wb_vector_at(&remotes->live, 0);
		wb_mutex_unlock(&remotes->lock);
		remote->page = NULL;
		remote->element = NULL;
		remote->loading = 0;
		remote_end(remote);
	}
}

/* Reports how many frames of other agents the view has. */
size_t
view_remotes_count(
	struct view_remotes *remotes)
{
	size_t count;

	/* The list's length, under the lock. */
	if (remotes == NULL)
		return 0;
	wb_mutex_lock(&remotes->lock);

	count = remotes->live.count;

	wb_mutex_unlock(&remotes->lock);
	return count;
}

/* Reports the calls the windows of the view's agent reach the frames through. */
const struct bind_remote_calls *
view_remotes_parent_calls(void)
{
	/* The one table. */
	return &view_remote_parent_calls;
}

/*
 * Shows a response of another origin in an iframe (the view's agent held,
 * on the thread of the iframe's document): in the frame it has, or a new
 * one with its thread, which waits in the view's bound on frames.
 * Reports 0, EAGAIN (no room, or the thread could not start), ENOMEM.
 */
int
view_remotes_load(
	void *context,
	struct page *page,
	struct dom_element *element,
	const struct page_remote_response *response)
{
	struct view_remotes *remotes;
	struct view_remote *remote;
	struct remote_entry entry;
	size_t stack;
	int error;

	/* The response, copied for the frame's thread. */
	remotes = view_frames_remotes(context);
	memset(&entry, 0, sizeof(entry));
	entry.kind = REMOTE_ENTRY_LOAD;
	entry.url = remote_strdup(response->url, strlen(response->url));
	entry.mime = remote_strdup(response->mime != NULL ? response->mime : "", response->mime_length);
	entry.mime_length = response->mime_length;
	entry.body = malloc(response->length == 0 ? 1U : response->length);
	entry.length = response->length;
	bind_tape_init(&entry.tape);
	if (entry.url == NULL || entry.mime == NULL || entry.body == NULL) {
		remote_entry_free(&entry);
		return ENOMEM;
	}
	if (response->length != 0)
		memcpy(entry.body, response->body, response->length);

	/* The frame the iframe has shows it next (its relatives' frames counted again). */
	remote = element->frame_remote;
	if (remote != NULL) {
		wb_mutex_lock(&remotes->lock);
		remote->parent_count = remote_count_document(element->node.document);
		remote->top_count = remote_count_document(page->document);
		wb_mutex_unlock(&remotes->lock);
		error = remote_queue(remote, &entry, 0);
		if (error != 0)
			return error;
		if (!remote->loading) {
			remote->loading = 1;
			page_remote_loading(remote->page);
		}
		return 0;
	}

	/* A new frame, in the view's bound (its iframes' threads and its frames). */
	if (!view_frames_room(context) || page->loader == NULL) {
		remote_entry_free(&entry);
		return EAGAIN;
	}
	remote = calloc(1, sizeof(*remote));
	if (remote == NULL) {
		remote_entry_free(&entry);
		return ENOMEM;
	}
	remote->remotes = remotes;
	remote->refs = 1U;
	remote->element = element;
	remote->page = page;
	remote->runner = bind_window_runner(element->node.document->view);
	remote->attached = 1;
	remote->parent_is_top = element->node.document->view == page->window;
	remote->dark = page->dark;
	remote->parent_count = remote_count_document(element->node.document);
	remote->top_count = remote_count_document(page->document);
	wb_vector_init(&remote->inbox, sizeof(struct remote_entry));
	error = wb_mutex_init(&remote->lock);
	if (error == 0)
		error = wb_cond_init(&remote->changed);
	if (error == 0)
		error = net_loader_client_open(page->loader, remote_notify, remote, &remote->client);
	if (error == 0)
		error = wb_vector_push(&remote->inbox, &entry);
	if (error != 0) {
		remote_entry_free(&entry);
		remote_release(remote);
		return error;
	}

	/* Its number and its place in the list. */
	wb_mutex_lock(&remotes->lock);

	remote->number = remotes->next_number++;
	error = wb_vector_push(&remotes->live, &remote);

	wb_mutex_unlock(&remotes->lock);
	if (error != 0) {
		remote_release(remote);
		return error;
	}

	/* Its thread, which shows the response. */
	stack = REMOTE_STACK_64;
	if (sizeof(void *) < 8U)
		stack = REMOTE_STACK_32;
	error = wb_thread_start("browser-script", stack, remote_run, remote, &remote->thread);
	if (error != 0) {
		wb_mutex_lock(&remotes->lock);
		remotes->live.count--;
		wb_mutex_unlock(&remotes->lock);
		remote_release(remote);
		if (error != ENOMEM)
			error = EAGAIN;
		return error;
	}

	/* Succeeded: the iframe shows the frame; its parent waits for its load. */
	element->frame_remote = remote;
	element->frame_proxy = NULL;
	remote->loading = 1;
	page_remote_loading(page);
	return 0;
}

/*
 * Ends the frame an iframe shows (the view's agent held): it is taken out
 * of the list, its queue's entries dropped, its task asked to end, its
 * requests woken, its thread joined (its page goes there), its client
 * closed; a load its parent waited for is waited for no more.
 */
void
view_remotes_close(
	void *context,
	struct dom_element *element)
{
	struct view_remote *remote;

	UNUSED_PARAMETER(context);

	/* The frame, no more the iframe's, then its end. */
	remote = element->frame_remote;
	if (remote == NULL)
		return;
	element->frame_remote = NULL;
	element->frame_proxy = NULL;
	remote_end(remote);
}

/* Ends a frame (the view's agent held; its element's state is the caller's). */
static void
remote_end(
	struct view_remote *remote)
{
	struct view_remotes *remotes;
	struct view_remote **slot;
	struct remote_up *up;
	size_t index;
	size_t kept;

	/* Out of the list, its entries out of the queue. */
	remotes = remote->remotes;
	wb_mutex_lock(&remotes->lock);

	remote->attached = 0;
	for (index = 0; index < remotes->live.count; index++) {
		slot = wb_vector_at(&remotes->live, index);
		if (*slot != remote)
			continue;
		memmove(slot, slot + 1, (remotes->live.count - index - 1U) * sizeof(*slot));
		remotes->live.count--;
		break;
	}
	kept = 0;
	for (index = 0; index < remotes->up.count; index++) {
		up = wb_vector_at(&remotes->up, index);
		if (up->remote == remote) {
			remote_up_free(up);
			continue;
		}
		*(struct remote_up *)wb_vector_at(&remotes->up, kept) = *up;
		kept++;
	}
	remotes->up.count = kept;

	wb_mutex_unlock(&remotes->lock);

	/* Its end: the mark, its task, its waited requests. */
	wb_mutex_lock(&remote->lock);

	__atomic_store_n(&remote->ending, 1, __ATOMIC_RELEASE);
	wb_cond_signal(&remote->changed);

	wb_mutex_unlock(&remote->lock);
	remote_terminate(remote, 0);
	net_loader_client_abort_waited(remote->client);

	/* Joined (its page goes on its thread), then its load waited for no more, then let go. */
	wb_thread_join(&remote->thread);
	if (remote->loading && remote->page != NULL)
		(void)page_remote_loaded(remote->page, remote->element, 0);
	remote->loading = 0;
	remote->page = NULL;
	remote->element = NULL;
	remote_release(remote);
}

/*
 * Gives the number of the frame an iframe shows (0 for none) and the size
 * its parent lays it out at (any of the parent's threads, the style
 * thread's too: the iframe's frame_remote is read only).
 */
uint64_t
view_remotes_frame(
	void *context,
	const struct dom_element *element,
	int width,
	int height)
{
	struct view_remotes *remotes;
	struct view_remote *remote;
	uint64_t number;
	int changed;

	/* The frame, its size under the lock. */
	remote = element->frame_remote;
	if (remote == NULL)
		return 0;
	remotes = view_frames_remotes(context);
	if (width < 1)
		width = 1;
	if (height < 1)
		height = 1;
	changed = 0;
	wb_mutex_lock(&remotes->lock);

	number = remote->number;
	if (remote->width != width || remote->height != height) {
		remote->width = width;
		remote->height = height;
		remote->sized = 1;
		changed = 1;
	}

	/* A new size wakes its thread. */
	if (changed) {
		wb_mutex_lock(&remote->lock);
		remote->rung = 1;
		wb_cond_signal(&remote->changed);
		wb_mutex_unlock(&remote->lock);
	}

	wb_mutex_unlock(&remotes->lock);
	return number;
}

/*
 * Delivers the queue's entries a thread is to deliver (runner: the page's
 * own thread is NULL), each a task of the view's agent: a message to the
 * frame's parent or top, a navigation, the end of a load.
 */
void
view_remotes_deliver(
	struct view_remotes *remotes,
	void *runner)
{
	struct wb_vector taken;
	struct remote_up *up;
	struct view_remote *remote;
	struct bind_window *window;
	struct dom_element *element;
	struct vm_agent *agent;
	size_t index;
	size_t kept;
	int error;

	/* The runner's entries, taken out of the queue. */
	if (remotes == NULL)
		return;
	wb_vector_init(&taken, sizeof(struct remote_up));
	wb_mutex_lock(&remotes->lock);

	kept = 0;
	for (index = 0; index < remotes->up.count; index++) {
		up = wb_vector_at(&remotes->up, index);
		if (up->runner == runner && wb_vector_push(&taken, up) == 0)
			continue;
		*(struct remote_up *)wb_vector_at(&remotes->up, kept) = *up;
		kept++;
	}
	remotes->up.count = kept;

	wb_mutex_unlock(&remotes->lock);

	/* Each, unless its frame ended meanwhile. */
	for (index = 0; index < taken.count; index++) {
		up = wb_vector_at(&taken, index);
		remote = up->remote;
		element = remote->element;
		if (element == NULL || element->frame_remote != remote || remote->page == NULL) {
			remote_up_free(up);
			continue;
		}
		window = element->node.document->view;
		if (up->target == BIND_REMOTE_TOP)
			window = remote->page->window;
		agent = vm_heap_agent(remote->page->heap);
		vm_agent_task_begin(agent);
		error = 0;
		switch (up->kind) {
		case REMOTE_UP_MESSAGE:
			if (window != NULL)
				error = bind_remote_deliver(window, &up->tape, up->origin, up->target_origin, BIND_REMOTE_CHILD, element);
			break;
		case REMOTE_UP_NAVIGATE:
			if (up->target == BIND_REMOTE_CHILD)
				error = page_remote_navigate(remote->page, element, up->url);
			else if (window != NULL)
				error = bind_window_request_navigation(window, up->url, up->replace);
			break;
		case REMOTE_UP_LOADED:
			if (remote->loading) {
				remote->loading = 0;
				error = page_remote_loaded(remote->page, element, 1);
			}
			break;
		default:
			break;
		}
		vm_agent_task_end(agent);
		(void)error;
		remote_up_free(up);
	}
	wb_vector_release(&taken);
}

/* Wakes the frames out of their waited requests and asks their tasks to end (the view navigates away or ends). */
void
view_remotes_abort(
	struct view_remotes *remotes)
{
	struct view_remote *remote;
	size_t index;

	/* Each live frame. */
	if (remotes == NULL)
		return;
	wb_mutex_lock(&remotes->lock);

	for (index = 0; index < remotes->live.count; index++) {
		remote = *(struct view_remote **)wb_vector_at(&remotes->live, index);
		remote_terminate(remote, 0);
		net_loader_client_abort_waited(remote->client);
	}

	wb_mutex_unlock(&remotes->lock);
}

/* The latest scene of a frame, held, and its scroll (the drawing's lookup). */
struct paint_scene *
view_remotes_scene(
	void *context,
	uint64_t frame,
	layout_unit *scroll_x,
	layout_unit *scroll_y)
{
	struct view_remotes *remotes;
	struct view_remote *remote;
	struct paint_scene *scene;

	/* The frame's, under the lock. */
	remotes = context;
	scene = NULL;
	if (remotes == NULL)
		return NULL;
	wb_mutex_lock(&remotes->lock);

	remote = remote_find(remotes, frame);
	if (remote != NULL && remote->scene != NULL) {
		scene = remote->scene;
		paint_scene_retain(scene);
		*scroll_x = remote->scroll_x;
		*scroll_y = remote->scroll_y;
	}

	wb_mutex_unlock(&remotes->lock);
	return scene;
}

/* Hands a pointer's input to a frame (the page's own thread). */
int
view_remotes_pointer(
	struct view_remotes *remotes,
	uint64_t frame,
	const char *type,
	const struct page_pointer *pointer)
{
	struct view_remote *remote;
	struct remote_entry entry;
	int error;

	/* The entry. */
	memset(&entry, 0, sizeof(entry));
	entry.kind = REMOTE_ENTRY_POINTER;
	snprintf(entry.type, sizeof(entry.type), "%s", type);
	entry.pointer = *pointer;
	bind_tape_init(&entry.tape);

	/* To the frame, if it is there. */
	error = ENOENT;
	wb_mutex_lock(&remotes->lock);

	remote = remote_find(remotes, frame);
	if (remote != NULL)
		error = remote_queue(remote, &entry, 1);

	wb_mutex_unlock(&remotes->lock);
	return error;
}

/* Hands a key's input to a frame (the page's own thread). */
int
view_remotes_key(
	struct view_remotes *remotes,
	uint64_t frame,
	const char *type,
	const char *key,
	const char *code,
	int repeat,
	unsigned modifiers)
{
	struct view_remote *remote;
	struct remote_entry entry;
	int error;

	/* The entry. */
	memset(&entry, 0, sizeof(entry));
	entry.kind = REMOTE_ENTRY_KEY;
	snprintf(entry.type, sizeof(entry.type), "%s", type);
	entry.key = remote_strdup(key != NULL ? key : "", key != NULL ? strlen(key) : 0);
	entry.code = remote_strdup(code != NULL ? code : "", code != NULL ? strlen(code) : 0);
	entry.repeat = repeat;
	entry.modifiers = modifiers;
	bind_tape_init(&entry.tape);
	if (entry.key == NULL || entry.code == NULL) {
		remote_entry_free(&entry);
		return ENOMEM;
	}

	/* To the frame, if it is there. */
	error = ENOENT;
	wb_mutex_lock(&remotes->lock);

	remote = remote_find(remotes, frame);
	if (remote != NULL)
		error = remote_queue(remote, &entry, 1);

	wb_mutex_unlock(&remotes->lock);
	if (error != 0)
		remote_entry_free(&entry);
	return error;
}

/*
 * Finds a frame whose task runs now (the first of the list), for the
 * question of a page that does not answer: its number and its task's
 * (0 when none runs one).
 */
uint64_t
view_remotes_busy(
	struct view_remotes *remotes,
	uint32_t *task)
{
	struct view_remote *remote;
	uint64_t found;
	uint32_t current;
	size_t index;

	/* Each frame's agent, under its lock. */
	*task = 0;
	found = 0;
	if (remotes == NULL)
		return 0;
	wb_mutex_lock(&remotes->lock);

	for (index = 0; index < remotes->live.count && found == 0; index++) {
		remote = *(struct view_remote **)wb_vector_at(&remotes->live, index);
		wb_mutex_lock(&remote->lock);
		current = remote->agent != NULL ? vm_agent_task_current(remote->agent) : 0U;
		wb_mutex_unlock(&remote->lock);
		if (current != 0) {
			found = remote->number;
			*task = current;
		}
	}

	wb_mutex_unlock(&remotes->lock);
	return found;
}

/* Asks the task of a number of a frame to end (any thread; nothing when the frame or the task is gone). */
void
view_remotes_stop(
	struct view_remotes *remotes,
	uint64_t frame,
	uint32_t task)
{
	struct view_remote *remote;

	/* The frame, under the lock. */
	if (remotes == NULL || task == 0)
		return;
	wb_mutex_lock(&remotes->lock);

	remote = remote_find(remotes, frame);
	if (remote != NULL)
		remote_terminate(remote, task);

	wb_mutex_unlock(&remotes->lock);
}

/* postMessage of a window of the view's agent to the frame an iframe shows (its proxy's). */
static int
remote_post(
	void *context,
	struct bind_window *from,
	int kind,
	uint64_t frame,
	struct bind_tape *tape,
	const char *target_origin,
	const char *origin)
{
	struct view_remotes *remotes;
	struct view_remote *remote;
	struct remote_entry entry;
	int error;

	UNUSED_PARAMETER(from);

	/* The entry: the tape is the frame's from now on. */
	remotes = view_frames_remotes(context);
	memset(&entry, 0, sizeof(entry));
	entry.kind = REMOTE_ENTRY_MESSAGE;
	entry.tape = *tape;
	bind_tape_init(tape);
	entry.origin = remote_strdup(origin, strlen(origin));
	entry.target = remote_strdup(target_origin, strlen(target_origin));
	entry.source = BIND_REMOTE_PARENT;
	if (kind != BIND_REMOTE_CHILD || entry.origin == NULL || entry.target == NULL) {
		remote_entry_free(&entry);
		return kind != BIND_REMOTE_CHILD ? ENOENT : ENOMEM;
	}

	/* The sender is the frame's parent, or its top seen from a deeper frame. */
	error = ENOENT;
	wb_mutex_lock(&remotes->lock);

	remote = remote_find(remotes, frame);
	if (remote != NULL) {
		if (!remote->parent_is_top && from->frame == NULL)
			entry.source = BIND_REMOTE_TOP;
		error = remote_queue(remote, &entry, 1);
	}

	wb_mutex_unlock(&remotes->lock);
	if (error != 0)
		remote_entry_free(&entry);
	return error;
}

/* A window of the view's agent navigates the frame an iframe shows (its proxy's location): its parent loads the URL into the iframe. */
static int
remote_navigate(
	void *context,
	struct bind_window *from,
	int kind,
	uint64_t frame,
	const char *url,
	int replace)
{
	struct view_remotes *remotes;
	struct view_remote *remote;
	struct dom_element *element;
	struct page *page;

	UNUSED_PARAMETER(from);
	UNUSED_PARAMETER(replace);

	/* The frame (the view's agent is held: its element and page are the caller's to read). */
	remotes = view_frames_remotes(context);
	if (kind != BIND_REMOTE_CHILD)
		return ENOENT;
	wb_mutex_lock(&remotes->lock);

	remote = remote_find(remotes, frame);
	element = remote != NULL ? remote->element : NULL;
	page = remote != NULL ? remote->page : NULL;

	wb_mutex_unlock(&remotes->lock);
	if (element == NULL || page == NULL)
		return ENOENT;
	return page_remote_navigate(page, element, url);
}

/* What a proxy of the view's agent asks of a frame: whether it ended, how many frames its document has. */
static int
remote_info(
	void *context,
	int kind,
	uint64_t frame,
	int what)
{
	struct view_remotes *remotes;
	struct view_remote *remote;
	int value;

	/* Under the lock: a frame gone is closed and has none. */
	remotes = view_frames_remotes(context);
	value = what == BIND_REMOTE_CLOSED ? 1 : 0;
	if (kind != BIND_REMOTE_CHILD)
		return value;
	wb_mutex_lock(&remotes->lock);

	remote = remote_find(remotes, frame);
	if (remote != NULL)
		value = what == BIND_REMOTE_CLOSED ? 0 : remote->frames_count;

	wb_mutex_unlock(&remotes->lock);
	return value;
}

/* The number of the frame an iframe of the view's agent shows (0: none). */
static uint64_t
remote_frame_number(
	void *context,
	const struct dom_element *element)
{
	struct view_remote *remote;

	UNUSED_PARAMETER(context);

	/* The frame's number never changes. */
	remote = element->frame_remote;
	if (remote == NULL)
		return 0;
	return remote->number;
}

/* postMessage of a frame's window to its parent or top: queued for the thread that runs that window. */
static int
child_post(
	void *context,
	struct bind_window *from,
	int kind,
	uint64_t frame,
	struct bind_tape *tape,
	const char *target_origin,
	const char *origin)
{
	struct view_remote *remote;
	struct remote_up up;
	int error;

	UNUSED_PARAMETER(from);
	UNUSED_PARAMETER(frame);

	/* The entry: the tape is the queue's from now on. */
	remote = context;
	if (kind != BIND_REMOTE_PARENT && kind != BIND_REMOTE_TOP)
		return ENOENT;
	memset(&up, 0, sizeof(up));
	up.kind = REMOTE_UP_MESSAGE;
	up.target = kind;
	up.tape = *tape;
	bind_tape_init(tape);
	up.origin = remote_strdup(origin, strlen(origin));
	up.target_origin = remote_strdup(target_origin, strlen(target_origin));
	if (up.origin == NULL || up.target_origin == NULL) {
		bind_tape_release(&up.tape);
		free(up.origin);
		free(up.target_origin);
		return ENOMEM;
	}
	error = remote_up(remote, &up);
	return error;
}

/* A frame's window navigates its parent or its top (their proxies' location). */
static int
child_navigate(
	void *context,
	struct bind_window *from,
	int kind,
	uint64_t frame,
	const char *url,
	int replace)
{
	struct view_remote *remote;
	struct remote_up up;

	UNUSED_PARAMETER(from);
	UNUSED_PARAMETER(frame);

	/* The entry. */
	remote = context;
	if (kind != BIND_REMOTE_PARENT && kind != BIND_REMOTE_TOP)
		return ENOENT;
	memset(&up, 0, sizeof(up));
	up.kind = REMOTE_UP_NAVIGATE;
	up.target = kind;
	up.replace = replace;
	bind_tape_init(&up.tape);
	up.url = remote_strdup(url, strlen(url));
	if (up.url == NULL)
		return ENOMEM;
	return remote_up(remote, &up);
}

/* What a frame's window asks of its parent or top: never closed while it is shown, and their frames' counts. */
static int
child_info(
	void *context,
	int kind,
	uint64_t frame,
	int what)
{
	struct view_remote *remote;
	int value;

	UNUSED_PARAMETER(frame);

	/* Under the lock. */
	remote = context;
	if (what == BIND_REMOTE_CLOSED)
		return 0;
	wb_mutex_lock(&remote->remotes->lock);

	value = kind == BIND_REMOTE_TOP ? remote->top_count : remote->parent_count;

	wb_mutex_unlock(&remote->remotes->lock);
	return value;
}

/* A frame's page has no frames of other agents (its iframes keep its agent). */
static uint64_t
child_frame_number(
	void *context,
	const struct dom_element *element)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(element);
	return 0;
}

/* Finds a live frame by its number (the frames' lock held). */
static struct view_remote *
remote_find(
	struct view_remotes *remotes,
	uint64_t number)
{
	struct view_remote *remote;
	size_t index;

	/* The list is short (the view's bound). */
	for (index = 0; index < remotes->live.count; index++) {
		remote = *(struct view_remote **)wb_vector_at(&remotes->live, index);
		if (remote->number == number)
			return remote;
	}
	return NULL;
}

/* Puts an entry in a frame's inbox and wakes it; a bounded one is refused past the inbox's bound (ENOBUFS). */
static int
remote_queue(
	struct view_remote *remote,
	struct remote_entry *entry,
	int bounded)
{
	int error;

	/* The inbox, under the frame's lock. */
	wb_mutex_lock(&remote->lock);

	if (bounded && remote->inbox.count >= REMOTE_INBOX_MAX)
		error = ENOBUFS;
	else
		error = wb_vector_push(&remote->inbox, entry);
	if (error == 0)
		wb_cond_signal(&remote->changed);

	wb_mutex_unlock(&remote->lock);
	return error;
}

/*
 * Queues an entry of a frame for the view's agent (on the frame's thread),
 * to be delivered by the thread that runs its parent's document (its
 * top's for one to the top), which is woken; dropped once the frame ended.
 */
static int
remote_up(
	struct view_remote *remote,
	struct remote_up *up)
{
	struct view_remotes *remotes;
	int error;

	/* The entry holds the frame. */
	remotes = remote->remotes;
	up->remote = remote;
	wb_mutex_lock(&remotes->lock);

	error = ENOENT;
	if (remote->attached) {
		up->runner = up->target == BIND_REMOTE_TOP ? NULL : remote->runner;
		__atomic_fetch_add(&remote->refs, 1U, __ATOMIC_RELAXED);
		error = wb_vector_push(&remotes->up, up);
		if (error != 0)
			__atomic_fetch_sub(&remote->refs, 1U, __ATOMIC_RELAXED);
		else
			view_frames_ring(remotes->frames, up->runner);
	}

	wb_mutex_unlock(&remotes->lock);
	if (error != 0) {
		up->remote = NULL;
		remote_up_free(up);
	}
	return error;
}

/* Lets go of an entry of the queue and its hold on its frame. */
static void
remote_up_free(
	struct remote_up *up)
{
	/* Its strings and its tape, then the frame. */
	bind_tape_release(&up->tape);
	free(up->origin);
	free(up->target_origin);
	free(up->url);
	if (up->remote != NULL)
		remote_release(up->remote);
	up->remote = NULL;
}

/* Lets go of an inbox's entry. */
static void
remote_entry_free(
	struct remote_entry *entry)
{
	/* Its strings, its bytes and its tape. */
	free(entry->url);
	free(entry->mime);
	free(entry->body);
	bind_tape_release(&entry->tape);
	free(entry->origin);
	free(entry->target);
	free(entry->key);
	free(entry->code);
	memset(entry, 0, sizeof(*entry));
}

/* A frame's client has news (on the network's thread): its thread wakes. */
static void
remote_notify(
	void *context)
{
	struct view_remote *remote;

	/* The mark, under its lock. */
	remote = context;
	wb_mutex_lock(&remote->lock);

	remote->rung = 1;
	wb_cond_signal(&remote->changed);

	wb_mutex_unlock(&remote->lock);
}

/* A frame's thread: waits for its inbox, news or its next timer, and runs its round, until it is told to end; its page goes with it. */
static void
remote_run(
	void *argument)
{
	struct view_remote *remote;
	struct wb_vector taken;
	struct remote_entry *entry;
	uint64_t deadline;
	uint64_t now;
	uint64_t wait;
	size_t index;
	int ending;

	/* This thread is the frame's: its requests' client, no runner (its page is its own). */
	remote = argument;
	bind_set_thread_runner(NULL);
	page_set_thread_client(remote->client);
	deadline = UINT64_MAX;

	/* Round after round. */
	for (;;) {
		/* The wait: for the inbox, news or the end, at most until the next timer. */
		wb_mutex_lock(&remote->lock);

		while (!remote->rung && remote->inbox.count == 0 && !__atomic_load_n(&remote->ending, __ATOMIC_ACQUIRE)) {
			now = wb_os_monotonic_ns();
			if (deadline != UINT64_MAX && now >= deadline)
				break;
			wait = REMOTE_WAIT_MAX;
			if (deadline != UINT64_MAX && (deadline - now + 999999ULL) / 1000000ULL < wait)
				wait = (deadline - now + 999999ULL) / 1000000ULL;
			(void)wb_cond_wait_for(&remote->changed, &remote->lock, (uint32_t)wait);
		}
		remote->rung = 0;
		taken = remote->inbox;
		wb_vector_init(&remote->inbox, sizeof(struct remote_entry));
		ending = __atomic_load_n(&remote->ending, __ATOMIC_ACQUIRE);

		wb_mutex_unlock(&remote->lock);

		/* The end. */
		if (ending) {
			for (index = 0; index < taken.count; index++) {
				entry = wb_vector_at(&taken, index);
				remote_entry_free(entry);
			}
			wb_vector_release(&taken);
			break;
		}

		/* The round, then the commit and the next timer. */
		remote_round(remote, &taken);
		wb_vector_release(&taken);
		remote_commit(remote);
		deadline = remote_deadline(remote);
	}

	/* The page goes on its thread. */
	if (remote->own != NULL) {
		remote_set_agent(remote, NULL);
		page_destroy(remote->own);
		remote->own = NULL;
	}
}

/*
 * A frame's round, one task of its page's agent: the responses to show,
 * the messages and inputs, its requests' callbacks, its timers and images,
 * the navigation its script asked for, the end of its load.
 */
static void
remote_round(
	struct view_remote *remote,
	struct wb_vector *taken)
{
	struct remote_entry *entry;
	struct vm_agent *agent;
	size_t index;
	int error;

	/* The responses first (each makes the page anew; the others go to the page shown then). */
	for (index = 0; index < taken->count; index++) {
		entry = wb_vector_at(taken, index);
		if (entry->kind == REMOTE_ENTRY_LOAD && !__atomic_load_n(&remote->ending, __ATOMIC_ACQUIRE))
			remote_show(remote, entry);
		if (remote->own == NULL || entry->kind == REMOTE_ENTRY_LOAD) {
			remote_entry_free(entry);
			continue;
		}
		agent = vm_heap_agent(remote->own->heap);
		vm_agent_task_begin(agent);
		if (entry->kind == REMOTE_ENTRY_MESSAGE)
			(void)bind_remote_deliver(remote->own->window, &entry->tape, entry->origin, entry->target, entry->source, NULL);
		else
			remote_input(remote, entry);
		vm_agent_task_end(agent);
		remote_entry_free(entry);
	}
	if (remote->own == NULL)
		return;

	/* The network's news, the timers and the images, as one task. */
	agent = vm_heap_agent(remote->own->heap);
	vm_agent_task_begin(agent);
	if (net_loader_client_dispatch_until(remote->client, wb_os_monotonic_ns() + REMOTE_DISPATCH_NS)) {
		wb_mutex_lock(&remote->lock);
		remote->rung = 1;
		wb_mutex_unlock(&remote->lock);
	}
	error = page_set_time(remote->own, remote_time(remote));
	if (error == 0)
		error = page_images_checkpoint(remote->own);
	(void)error;
	vm_agent_task_end(agent);

	/* What the round left for the parent's agent. */
	remote_script_navigation(remote);
	remote_loaded_check(remote);
}

/*
 * Shows a response in a frame (on its thread): the page shown is left
 * (pagehide and unload) and goes, and a new one, whose window reaches its
 * parent and top through the frame, loads the response.
 */
static void
remote_show(
	struct view_remote *remote,
	struct remote_entry *entry)
{
	struct view_remotes *remotes;
	struct page *made;
	struct vm_agent *agent;
	int width;
	int height;
	int parent_is_top;
	int error;

	/* The page shown is left for good. */
	remotes = remote->remotes;
	if (remote->own != NULL) {
		agent = vm_heap_agent(remote->own->heap);
		vm_agent_task_begin(agent);
		(void)page_unload(remote->own);
		vm_agent_task_end(agent);
		remote_set_agent(remote, NULL);
		page_destroy(remote->own);
		remote->own = NULL;
	}

	/* The size and the relatives its parent gives. */
	wb_mutex_lock(&remotes->lock);

	width = remote->width > 0 ? remote->width : 300;
	height = remote->height > 0 ? remote->height : 150;
	parent_is_top = remote->parent_is_top;
	remote->sized = 0;

	wb_mutex_unlock(&remotes->lock);

	/* The new page, its heap's stack this thread's. */
	error = view_make_remote_page(remotes->view, __builtin_frame_address(0), width, height, remote->dark, &made);
	if (error != 0)
		return;
	bind_window_set_remote(made->window, &remote_child_calls, remote, 1, parent_is_top);
	remote->own = made;
	remote->told = 0;
	remote->epoch_ns = wb_os_monotonic_ns();
	remote->laid_width = 0;
	remote->laid_height = 0;
	remote_set_agent(remote, vm_heap_agent(made->heap));

	/* The document, its scripts run (one asked to end is no error). */
	agent = vm_heap_agent(made->heap);
	vm_agent_task_begin(agent);
	error = page_load_bytes(made, entry->body, entry->length, entry->url, entry->mime, entry->mime_length);
	vm_agent_task_end(agent);
	(void)error;
}

/* A pointer's or a key's input for a frame's page (its task begun). */
static void
remote_input(
	struct view_remote *remote,
	struct remote_entry *entry)
{
	struct page_pointer pointer;
	struct bind_key key;
	struct wb_buffer href;
	struct page *page;
	int canceled;
	int found;

	/* The page's document's place of the pointer. */
	page = remote->own;
	pointer = entry->pointer;
	pointer.x = pointer.client_x + (int)page->scroll_x;
	pointer.y = pointer.client_y + (int)page->scroll_y;

	/* A key goes to the focused element. */
	if (entry->kind == REMOTE_ENTRY_KEY) {
		memset(&key, 0, sizeof(key));
		key.key = entry->key;
		key.code = entry->code;
		key.repeat = entry->repeat;
		key.modifiers = entry->modifiers;
		(void)page_key_event(page, entry->type, &key, &canceled);
		return;
	}

	/* A wheel, a move, a press (the focus moves there), a release and its click (a link followed is the frame's navigation). */
	if (strcmp(entry->type, "wheel") == 0) {
		(void)page_wheel_event(page, &pointer, &canceled);
		return;
	}
	(void)page_mouse_event(page, entry->type, &pointer, &canceled);
	if (strcmp(entry->type, "mousedown") == 0) {
		(void)page_focus_at(page, pointer.x, pointer.y);
		return;
	}
	if (strcmp(entry->type, "mouseup") != 0)
		return;
	(void)page_mouse_event(page, "click", &pointer, &canceled);
	if (canceled)
		return;
	wb_buffer_init(&href);
	found = 0;
	if (page_click_control(page, pointer.x, pointer.y, &href, &found) == 0 && found && href.length != 0)
		(void)remote_navigate_self(remote, wb_buffer_string(&href), 0);
	wb_buffer_release(&href);
}

/* Tells the parent's agent once that the frame's document loaded (its window's load ran). */
static void
remote_loaded_check(
	struct view_remote *remote)
{
	struct remote_up up;
	int count;

	/* The frames' count its proxies tell. */
	count = remote_count_frames(remote->own);
	wb_mutex_lock(&remote->remotes->lock);

	remote->frames_count = count;

	wb_mutex_unlock(&remote->remotes->lock);

	/* Once, after the load ran. */
	if (remote->told || remote->own->load_pending || page_load_waiting(remote->own))
		return;
	remote->told = 1;
	memset(&up, 0, sizeof(up));
	up.kind = REMOTE_UP_LOADED;
	up.target = BIND_REMOTE_PARENT;
	bind_tape_init(&up.tape);
	(void)remote_up(remote, &up);
}

/* A navigation the frame's script asked for (its location): its parent loads it into the iframe. */
static void
remote_script_navigation(
	struct view_remote *remote)
{
	char *location;
	int replace;

	/* The request. */
	location = bind_take_navigation(remote->own->window, &replace);
	if (location == NULL)
		return;
	(void)remote_navigate_self(remote, location, replace);
	free(location);
}

/* Asks the frame's parent to load a URL (resolved against the frame's page) into the iframe. */
static int
remote_navigate_self(
	struct view_remote *remote,
	const char *url,
	int replace)
{
	struct remote_up up;
	struct wb_buffer path;
	int error;

	/* Resolved against the page's location. */
	wb_buffer_init(&path);
	error = page_resolve_location(remote->own->base, url, &path);
	if (error != 0) {
		wb_buffer_release(&path);
		return error;
	}

	/* To the parent's agent. */
	memset(&up, 0, sizeof(up));
	up.kind = REMOTE_UP_NAVIGATE;
	up.target = BIND_REMOTE_CHILD;
	up.replace = replace;
	bind_tape_init(&up.tape);
	up.url = remote_strdup(wb_buffer_string(&path), path.length);
	wb_buffer_release(&path);
	if (up.url == NULL)
		return ENOMEM;
	return remote_up(remote, &up);
}

/*
 * Lays out, paints and commits a frame's page when it changed or its
 * parent gave it another size (its parent's runner sees the change at its
 * next round: the drawing is woken).
 */
static void
remote_commit(
	struct view_remote *remote)
{
	struct view_remotes *remotes;
	struct paint_scene *made;
	struct paint_scene *old;
	struct page *page;
	int width;
	int height;
	int changed;
	int error;

	/* The size its parent gives. */
	page = remote->own;
	if (page == NULL)
		return;
	remotes = remote->remotes;
	wb_mutex_lock(&remotes->lock);

	width = remote->width > 0 ? remote->width : 300;
	height = remote->height > 0 ? remote->height : 150;

	wb_mutex_unlock(&remotes->lock);

	/* Laid out again when the page or its size changed, painted again when it shows otherwise. */
	changed = width != remote->laid_width || height != remote->laid_height || page_needs_layout(page);
	if (changed) {
		page_set_viewport(page, width, height);
		error = page_open_fonts(page, page->font_paths);
		if (error == 0)
			error = page_layout(page, width, height);
		if (error == 0)
			error = page_paint(page);
		if (error != 0)
			return;
		remote->laid_width = width;
		remote->laid_height = height;
	} else if (page_needs_paint(page)) {
		error = page_paint(page);
		if (error != 0)
			return;
		changed = 1;
	}
	if (!changed && remote->scene != NULL)
		return;
	if (!page->painted)
		return;
	if (page->paint_taken) {
		error = page_paint(page);
		if (error != 0)
			return;
	}

	/* The scene, in place of the last one. */
	error = paint_scene_make(&page->paint, &page->text, &made);
	if (error != 0)
		return;
	page->paint_taken = 1;
	wb_mutex_lock(&remotes->lock);

	old = remote->scene;
	remote->scene = made;
	remote->scroll_x = layout_from_px((float)page->scroll_x);
	remote->scroll_y = layout_from_px((float)page->scroll_y);

	wb_mutex_unlock(&remotes->lock);
	paint_scene_release(old);

	/* The drawing shows it. */
	view_front_wake_drawing(view_front_of(remotes->view));
}

/* When a frame's next timer, image or picture is due on the monotonic clock (UINT64_MAX: none). */
static uint64_t
remote_deadline(
	struct view_remote *remote)
{
	double due;
	double wait;
	uint64_t now;

	/* Without a page, none; with work due now, now. */
	if (remote->own == NULL)
		return UINT64_MAX;
	now = wb_os_monotonic_ns();
	if (page_images_ready(remote->own) || bind_navigation_pending(remote->own->window))
		return now;
	if (!page_next_timer(remote->own, &due))
		return UINT64_MAX;
	wait = due - remote_time(remote);
	if (wait <= 0.0)
		return now;
	if (wait > 3600000.0)
		wait = 3600000.0;
	return now + (uint64_t)(wait * 1000000.0);
}

/* The page's clock of a frame, in milliseconds since its page was made. */
static double
remote_time(
	const struct view_remote *remote)
{
	/* The monotonic clock less the page's start. */
	return (double)(wb_os_monotonic_ns() - remote->epoch_ns) / 1000000.0;
}

/* Counts the iframes of a page's document (what window.length tells, near enough). */
static int
remote_count_frames(
	struct page *page)
{
	/* The document's. */
	if (page == NULL)
		return 0;
	return remote_count_document(page->document);
}

/* Counts the iframes of a document. */
static int
remote_count_document(
	struct dom_document *document)
{
	struct dom_node *walk;
	int count;

	/* Each iframe element. */
	count = 0;
	if (document == NULL)
		return 0;
	walk = document->node.first_child;
	while (walk != NULL) {
		if (walk->type == DOM_ELEMENT && dom_element_is(walk, DOM_NS_HTML, DOM_TAG_IFRAME))
			count++;
		walk = bind_following(walk, &document->node);
	}
	return count;
}

/* Lets go of a hold on a frame; the last frees it (its inbox, its client, its scene, its lock). */
static void
remote_release(
	struct view_remote *remote)
{
	struct remote_entry *entry;
	unsigned refs;
	size_t index;

	/* Held by others still. */
	refs = __atomic_sub_fetch(&remote->refs, 1U, __ATOMIC_ACQ_REL);
	if (refs != 0U)
		return;

	/* Everything it holds. */
	for (index = 0; index < remote->inbox.count; index++) {
		entry = wb_vector_at(&remote->inbox, index);
		remote_entry_free(entry);
	}
	wb_vector_release(&remote->inbox);
	if (remote->client != NULL)
		net_loader_client_close(remote->client);
	paint_scene_release(remote->scene);
	wb_cond_destroy(&remote->changed);
	wb_mutex_destroy(&remote->lock);
	free(remote);
}

/* Asks a frame's task to end: the one of a number, or the one under way (0); the frames' lock may be held. */
static void
remote_terminate(
	struct view_remote *remote,
	uint32_t task)
{
	/* The agent stays while its lock is held (its thread lets it go under it). */
	wb_mutex_lock(&remote->lock);

	if (remote->agent != NULL) {
		if (task == 0)
			task = vm_agent_task_current(remote->agent);
		if (task != 0)
			vm_agent_request_termination(remote->agent, task);
	}

	wb_mutex_unlock(&remote->lock);
}

/* Publishes the agent of a frame's page (NULL as the page goes), on the frame's thread. */
static void
remote_set_agent(
	struct view_remote *remote,
	struct vm_agent *agent)
{
	/* Under its lock. */
	wb_mutex_lock(&remote->lock);

	remote->agent = agent;

	wb_mutex_unlock(&remote->lock);
}

/* Copies a string of a length (NUL-terminated). */
static char *
remote_strdup(
	const char *text,
	size_t length)
{
	char *copy;

	/* The bytes and the end. */
	copy = malloc(length + 1U);
	if (copy == NULL)
		return NULL;
	memcpy(copy, text, length);
	copy[length] = '\0';
	return copy;
}
