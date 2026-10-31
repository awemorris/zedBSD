/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The storage thread of an instance and its queue.
 *
 * Requests wait in one first-in first-out list under the queue's lock.
 * The storage thread takes them from the front and runs each outside the
 * lock, so a slow backend never holds the lock.  A write, remove or clear
 * lives in one heap block the queue owns until the thread has run it; a
 * read, list or flush lives on its caller's stack and the caller waits
 * until the thread marks it done, so no memory is handed between threads
 * except through the lock.
 */

#include "storage/queue.h"
#include "storage/records.h"
#include "base/base.h"
#include "base/os-system.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/*
 * The most waiting writes, removes and clears at once.  The byte limit
 * does not bound removes and clears (they carry no bytes), so this bounds
 * the memory a stalled backend can make the queue hold.
 */
#define QUEUE_ITEM_LIMIT	65536U

/* The most waiting requests looked at when a write searches for an earlier write to replace. */
#define QUEUE_MERGE_SCAN	256U

/* The synchronization objects of a queue are made in this order; the count says how many exist. */
#define QUEUE_SYNC_NONE		0
#define QUEUE_SYNC_LOCK		1
#define QUEUE_SYNC_WORK		2
#define QUEUE_SYNC_ALL		3

/* What a request asks the storage thread to do. */
enum request_kind {
	REQUEST_WRITE,
	REQUEST_REMOVE,
	REQUEST_CLEAR,
	REQUEST_READ,
	REQUEST_LIST,
	REQUEST_FLUSH
};

/*
 * One request in the queue.
 *
 * A request with waiting 0 is a heap block (the request, then the key's
 * text, then the bytes) owned by the queue until the storage thread has
 * run it.  A request with waiting 1 is on the stack of a caller that
 * stays in the queue's wait until done is 1, so it outlives its time in
 * the list.  Every field but the results is fixed when the request is
 * linked; next, prev, done and the results are under the queue's lock or
 * belong to the storage thread while it runs the request.
 */
struct wb_storage_request {
	struct wb_storage_request *next;
	struct wb_storage_request *prev;
	enum request_kind kind;
	unsigned space;
	const char *key;
	const void *bytes;
	size_t length;
	int waiting;
	int done;
	int error;
	int found;
	struct wb_buffer *buffer;
	size_t count;
};

/*
 * One instance's queue and thread.
 *
 * The lock protects the list, the counters, the first error and the
 * closing flag.  The work condition wakes the thread when a request
 * arrives or the queue closes; the done condition wakes the callers
 * waiting for a request.  A queue without a backend has no thread and no
 * list.
 */
struct wb_storage_queue {
	const struct browser_storage_calls *calls;
	void *context;
	pthread_t thread;
	int started;
	int sync_ready;
	pthread_mutex_t lock;
	pthread_cond_t work;
	pthread_cond_t done;
	struct wb_storage_request *head;
	struct wb_storage_request *tail;
	size_t pending_bytes;
	size_t pending_items;
	int first_error;
	int closing;
};

/*
 * What a list collects while the backend calls back with its keys.
 */
struct queue_list_state {
	struct wb_buffer *keys;
	size_t count;
	int error;
};

static void queue_release_sync(struct wb_storage_queue *queue);
static int queue_table_complete(const struct browser_storage_calls *calls);
static int queue_check_key(const char *key);
static int queue_new_item(enum request_kind kind, unsigned space, const char *key, const void *bytes, size_t length, struct wb_storage_request **request);
static int queue_add(struct wb_storage_queue *queue, struct wb_storage_request *request);
static void queue_wait(struct wb_storage_queue *queue, struct wb_storage_request *request);
static void queue_link_tail(struct wb_storage_queue *queue, struct wb_storage_request *request);
static struct wb_storage_request *queue_find_merge(const struct wb_storage_queue *queue, const struct wb_storage_request *request);
static void *queue_thread_main(void *argument);
static void queue_execute(struct wb_storage_queue *queue, struct wb_storage_request *request);
static void queue_execute_read(struct wb_storage_queue *queue, struct wb_storage_request *request);
static void queue_execute_list(struct wb_storage_queue *queue, struct wb_storage_request *request);
static int queue_list_each(void *each_context, const char *key);

/*
 * Creates a queue over a backend and starts its thread.
 */
int
wb_storage_queue_open(
	const struct browser_storage_calls *calls,
	void *context,
	struct wb_storage_queue **queue)
{
	struct wb_storage_queue *created;
	int error;
	int complete;

	/* Refuses a place to put the queue that is not there. */
	if (queue == NULL)
		return EINVAL;
	*queue = NULL;

	/* Refuses a table of another version or one with a member missing. */
	if (calls != NULL) {
		complete = queue_table_complete(calls);
		if (complete == 0)
			return ENOTSUP;
	}

	/* Allocates the queue. */
	created = malloc(sizeof(*created));
	if (created == NULL)
		return ENOMEM;

	/* Initializes the queue with nothing waiting and no thread. */
	memset(created, 0, sizeof(*created));
	created->calls = calls;
	created->context = context;
	created->sync_ready = QUEUE_SYNC_NONE;

	/* Makes the lock; every queue has one, a queue without a backend simply never contends on it. */
	error = pthread_mutex_init(&created->lock, NULL);
	if (error != 0) {
		queue_release_sync(created);
		free(created);
		return ENOMEM;
	}
	created->sync_ready = QUEUE_SYNC_LOCK;

	/* Makes the condition the thread sleeps on. */
	error = pthread_cond_init(&created->work, NULL);
	if (error != 0) {
		queue_release_sync(created);
		free(created);
		return ENOMEM;
	}
	created->sync_ready = QUEUE_SYNC_WORK;

	/* Makes the condition the waiting callers sleep on. */
	error = pthread_cond_init(&created->done, NULL);
	if (error != 0) {
		queue_release_sync(created);
		free(created);
		return ENOMEM;
	}
	created->sync_ready = QUEUE_SYNC_ALL;

	/* Starts the storage thread, unless there is no backend for it to call. */
	if (calls != NULL) {
		error = pthread_create(&created->thread, NULL, queue_thread_main, created);
		if (error != 0) {
			queue_release_sync(created);
			free(created);
			return EAGAIN;
		}
		created->started = 1;
	}

	/* Succeeded: the caller owns the queue until it closes it. */
	*queue = created;
	return 0;
}

/*
 * Queues a write of a copy of bytes under a space and key.
 */
int
wb_storage_queue_write(
	struct wb_storage_queue *queue,
	unsigned space,
	const char *key,
	const void *bytes,
	size_t length)
{
	struct wb_storage_request *request;
	int error;

	/* Refuses arguments that cannot be a record. */
	if (queue == NULL)
		return EINVAL;
	error = queue_check_key(key);
	if (error != 0)
		return error;
	if (bytes == NULL && length > 0)
		return EINVAL;

	/* Refuses a record that could never fit in the queue. */
	if (length > WB_STORAGE_QUEUE_LIMIT)
		return EFBIG;

	/* A queue without a backend drops the write. */
	if (queue->calls == NULL)
		return 0;

	/* Makes the copy the storage thread will write. */
	error = queue_new_item(REQUEST_WRITE, space, key, bytes, length, &request);
	if (error != 0)
		return error;

	/* Queues the copy, or refuses it when the queue is full. */
	error = queue_add(queue, request);
	if (error != 0) {
		free(request);
		return error;
	}

	/* Succeeded: the storage thread will write it. */
	return 0;
}

/*
 * Queues a remove of a record.
 */
int
wb_storage_queue_remove(
	struct wb_storage_queue *queue,
	unsigned space,
	const char *key)
{
	struct wb_storage_request *request;
	int error;

	/* Refuses arguments that cannot name a record. */
	if (queue == NULL)
		return EINVAL;
	error = queue_check_key(key);
	if (error != 0)
		return error;

	/* A queue without a backend has nothing to remove. */
	if (queue->calls == NULL)
		return 0;

	/* Makes the request the storage thread will run. */
	error = queue_new_item(REQUEST_REMOVE, space, key, NULL, 0, &request);
	if (error != 0)
		return error;

	/* Queues the request, or refuses it when the queue is full. */
	error = queue_add(queue, request);
	if (error != 0) {
		free(request);
		return error;
	}

	/* Succeeded: the storage thread will remove it. */
	return 0;
}

/*
 * Queues a clear of a whole space.
 */
int
wb_storage_queue_clear(
	struct wb_storage_queue *queue,
	unsigned space)
{
	struct wb_storage_request *request;
	int error;

	/* Refuses a queue that is not there. */
	if (queue == NULL)
		return EINVAL;

	/* A queue without a backend has nothing to clear. */
	if (queue->calls == NULL)
		return 0;

	/* Makes the request the storage thread will run; a clear names no key. */
	error = queue_new_item(REQUEST_CLEAR, space, "", NULL, 0, &request);
	if (error != 0)
		return error;

	/* Queues the request, or refuses it when the queue is full. */
	error = queue_add(queue, request);
	if (error != 0) {
		free(request);
		return error;
	}

	/* Succeeded: the storage thread will clear the space. */
	return 0;
}

/*
 * Reads a record, waiting for the queue to reach it.
 */
int
wb_storage_queue_read(
	struct wb_storage_queue *queue,
	unsigned space,
	const char *key,
	struct wb_buffer *out,
	int *found)
{
	struct wb_storage_request request;
	int error;

	/* Refuses arguments that cannot name a record or receive one. */
	if (queue == NULL)
		return EINVAL;
	if (out == NULL)
		return EINVAL;
	if (found == NULL)
		return EINVAL;
	error = queue_check_key(key);
	if (error != 0)
		return error;

	/* Nothing is found until the backend says so. */
	*found = 0;

	/* A queue without a backend finds nothing. */
	if (queue->calls == NULL)
		return 0;

	/* Describes the read on this thread's stack; the key stays valid while this thread waits. */
	memset(&request, 0, sizeof(request));
	request.kind = REQUEST_READ;
	request.space = space;
	request.key = key;
	request.buffer = out;
	request.waiting = 1;

	/* Waits for the storage thread to run the read. */
	queue_wait(queue, &request);

	/* Reports the backend's error; the buffer is as before the call. */
	if (request.error != 0)
		return request.error;

	/* Takes the answer the storage thread wrote. */
	*found = request.found;

	/* Succeeded: found says whether the record exists. */
	return 0;
}

/*
 * Lists a space's keys, waiting for the queue to reach the list.
 */
int
wb_storage_queue_list(
	struct wb_storage_queue *queue,
	unsigned space,
	struct wb_buffer *keys,
	size_t *count)
{
	struct wb_storage_request request;

	/* Refuses arguments that cannot receive the keys. */
	if (queue == NULL)
		return EINVAL;
	if (keys == NULL)
		return EINVAL;
	if (count == NULL)
		return EINVAL;

	/* A queue without a backend has no keys. */
	if (queue->calls == NULL) {
		*count = 0;
		return 0;
	}

	/* Describes the list on this thread's stack. */
	memset(&request, 0, sizeof(request));
	request.kind = REQUEST_LIST;
	request.space = space;
	request.key = "";
	request.buffer = keys;
	request.waiting = 1;

	/* Waits for the storage thread to run the list. */
	queue_wait(queue, &request);

	/* Reports the backend's error; keys and count are as before the call. */
	if (request.error != 0)
		return request.error;

	/* Takes the answer the storage thread wrote. */
	*count = request.count;

	/* Succeeded: the keys were appended and counted. */
	return 0;
}

/*
 * Waits until everything queued before the call has been done.
 */
void
wb_storage_queue_flush(
	struct wb_storage_queue *queue)
{
	struct wb_storage_request request;

	/* A queue that is not there, or has no backend, has nothing to wait for. */
	if (queue == NULL)
		return;
	if (queue->calls == NULL)
		return;

	/* Describes a marker that does nothing when the storage thread reaches it. */
	memset(&request, 0, sizeof(request));
	request.kind = REQUEST_FLUSH;
	request.key = "";
	request.waiting = 1;

	/* Waits for the marker, which comes after everything queued before it. */
	queue_wait(queue, &request);
}

/*
 * Reports the first error a queued write, remove or clear met, and forgets it.
 */
int
wb_storage_queue_take_error(
	struct wb_storage_queue *queue)
{
	int error;

	/* A queue that is not there has met no error. */
	if (queue == NULL)
		return 0;

	/* Takes the kept error so the next call starts afresh. */
	pthread_mutex_lock(&queue->lock);

	error = queue->first_error;
	queue->first_error = 0;

	pthread_mutex_unlock(&queue->lock);

	/* Reports the error that was kept, or 0 for none. */
	return error;
}

/*
 * Destroys a queue after doing everything queued.
 */
void
wb_storage_queue_close(
	struct wb_storage_queue *queue)
{
	/* Closing nothing does nothing. */
	if (queue == NULL)
		return;

	/* Tells the thread to finish what is queued and stop, then waits for it. */
	if (queue->started != 0) {
		pthread_mutex_lock(&queue->lock);

		queue->closing = 1;
		pthread_cond_broadcast(&queue->work);

		pthread_mutex_unlock(&queue->lock);

		pthread_join(queue->thread, NULL);
		queue->started = 0;
	}

	/* The thread is gone, so the backend is called no more; closes it exactly once. */
	if (queue->calls != NULL) {
		if (queue->calls->close != NULL)
			queue->calls->close(queue->context);
	}

	/* Frees the queue. */
	queue_release_sync(queue);
	free(queue);
}

/*
 * Destroys the synchronization objects that were made.
 */
static void
queue_release_sync(
	struct wb_storage_queue *queue)
{
	/* Destroys them in the opposite order of their making. */
	if (queue->sync_ready >= QUEUE_SYNC_ALL)
		pthread_cond_destroy(&queue->done);
	if (queue->sync_ready >= QUEUE_SYNC_WORK)
		pthread_cond_destroy(&queue->work);
	if (queue->sync_ready >= QUEUE_SYNC_LOCK)
		pthread_mutex_destroy(&queue->lock);
	queue->sync_ready = QUEUE_SYNC_NONE;
}

/*
 * Reports whether a backend's table has the right version and every member the queue calls.
 */
static int
queue_table_complete(
	const struct browser_storage_calls *calls)
{
	/* Refuses another version of the table. */
	if (calls->version != BROWSER_STORAGE_VERSION)
		return 0;

	/* Refuses a table with a call missing. */
	if (calls->read == NULL)
		return 0;
	if (calls->write == NULL)
		return 0;
	if (calls->remove == NULL)
		return 0;
	if (calls->list == NULL)
		return 0;
	if (calls->clear == NULL)
		return 0;

	/* Succeeded: the close member is optional. */
	return 1;
}

/*
 * Checks a key argument: EINVAL for NULL or an invalid key.
 */
static int
queue_check_key(
	const char *key)
{
	int valid;

	/* Refuses a key that is not there. */
	if (key == NULL)
		return EINVAL;

	/* Only keys the engine makes may reach a backend. */
	valid = wb_storage_key_valid(key);
	if (valid == 0)
		return EINVAL;

	/* Succeeded: the key may be queued. */
	return 0;
}

/*
 * Makes a heap request holding copies of the key and the bytes.
 */
static int
queue_new_item(
	enum request_kind kind,
	unsigned space,
	const char *key,
	const void *bytes,
	size_t length,
	struct wb_storage_request **request)
{
	struct wb_storage_request *item;
	size_t key_size;
	char *key_copy;
	unsigned char *bytes_copy;

	/* Allocates the request, the key and the bytes in one block (length is at most the queue's limit). */
	key_size = strlen(key) + 1U;
	item = malloc(sizeof(*item) + key_size + length);
	if (item == NULL)
		return ENOMEM;

	/* Initializes the request. */
	memset(item, 0, sizeof(*item));
	item->kind = kind;
	item->space = space;
	item->length = length;
	item->waiting = 0;

	/* Copies the key after the request. */
	key_copy = (char *)(item + 1);
	memcpy(key_copy, key, key_size);
	item->key = key_copy;

	/* Copies the bytes after the key; a record of no bytes points at the end of the block. */
	bytes_copy = (unsigned char *)key_copy + key_size;
	if (length > 0)
		memcpy(bytes_copy, bytes, length);
	item->bytes = bytes_copy;

	/* Succeeded: the caller owns the request. */
	*request = item;
	return 0;
}

/*
 * Links a heap request into the list, or lets it replace an earlier write.
 *
 * On a failure the request is not linked and still belongs to the caller.
 */
static int
queue_add(
	struct wb_storage_queue *queue,
	struct wb_storage_request *request)
{
	struct wb_storage_request *old;
	size_t bytes_after;

	/* Links the request under the lock, refusing it when the queue is full. */
	pthread_mutex_lock(&queue->lock);

	old = NULL;
	if (request->kind == REQUEST_WRITE)
		old = queue_find_merge(queue, request);

	bytes_after = queue->pending_bytes;
	if (old != NULL)
		bytes_after -= old->length;

	if (old == NULL && queue->pending_items >= QUEUE_ITEM_LIMIT) {
		pthread_mutex_unlock(&queue->lock);
		return ENOSPC;
	}
	if (request->length > WB_STORAGE_QUEUE_LIMIT - bytes_after) {
		pthread_mutex_unlock(&queue->lock);
		return ENOSPC;
	}

	if (old != NULL) {
		/* The new write takes the place of the earlier one, which nothing observes in between. */
		request->prev = old->prev;
		request->next = old->next;
		if (old->prev != NULL)
			old->prev->next = request;
		else
			queue->head = request;
		if (old->next != NULL)
			old->next->prev = request;
		else
			queue->tail = request;
		queue->pending_bytes = bytes_after + request->length;
	} else {
		/* The request waits behind everything queued so far. */
		queue_link_tail(queue, request);
		queue->pending_bytes += request->length;
		queue->pending_items++;
	}

	pthread_cond_signal(&queue->work);

	pthread_mutex_unlock(&queue->lock);

	/* The replaced write was never run; its block is no longer in the list. */
	free(old);

	/* Succeeded: the request is waiting. */
	return 0;
}

/*
 * Links a caller's request into the list and waits until the thread has run it.
 */
static void
queue_wait(
	struct wb_storage_queue *queue,
	struct wb_storage_request *request)
{
	/* Links the request behind everything queued so far, then sleeps until the thread marks it done. */
	pthread_mutex_lock(&queue->lock);

	queue_link_tail(queue, request);
	pthread_cond_signal(&queue->work);

	/* Waits again after every wakeup that was not for this request. */
	while (request->done == 0)
		pthread_cond_wait(&queue->done, &queue->lock);

	pthread_mutex_unlock(&queue->lock);
}

/*
 * Appends a request to the end of the list; the lock is held.
 */
static void
queue_link_tail(
	struct wb_storage_queue *queue,
	struct wb_storage_request *request)
{
	/* Makes the request the new last one. */
	request->next = NULL;
	request->prev = queue->tail;
	if (queue->tail != NULL)
		queue->tail->next = request;
	else
		queue->head = request;
	queue->tail = request;
}

/*
 * Finds a waiting write that a new write may replace; the lock is held.
 *
 * Looks back from the end of the list.  A request that could observe the
 * earlier bytes stops the search: a flush, a clear or list of the space,
 * and a remove or read of the key.  Requests that concern other keys are
 * passed.
 */
static struct wb_storage_request *
queue_find_merge(
	const struct wb_storage_queue *queue,
	const struct wb_storage_request *request)
{
	struct wb_storage_request *candidate;
	unsigned scanned;
	int same_key;

	/* Looks back through the most recent waiting requests. */
	scanned = 0;
	for (candidate = queue->tail;
	     candidate != NULL && scanned < QUEUE_MERGE_SCAN;
	     candidate = candidate->prev) {
		scanned++;

		/* A flush promises what came before it was done as queued. */
		if (candidate->kind == REQUEST_FLUSH)
			return NULL;

		/* A request in another space cannot see this key. */
		if (candidate->space != request->space)
			continue;

		/* Says whether the request concerns the same key. */
		same_key = 0;
		if (strcmp(candidate->key, request->key) == 0)
			same_key = 1;

		/* Decides what the request means for the key. */
		switch (candidate->kind) {
		case REQUEST_WRITE:
			if (same_key != 0)
				return candidate;
			break;
		case REQUEST_REMOVE:
		case REQUEST_READ:
			if (same_key != 0)
				return NULL;
			break;
		case REQUEST_CLEAR:
		case REQUEST_LIST:
		case REQUEST_FLUSH:
			return NULL;
		}
	}

	/* No earlier write may be replaced. */
	return NULL;
}

/*
 * The body of the storage thread: runs requests in order until the queue is closing and empty.
 */
static void *
queue_thread_main(
	void *argument)
{
	struct wb_storage_queue *queue;
	struct wb_storage_request *request;

	queue = argument;

	/* Names this thread for debuggers. */
	wb_os_thread_name(WB_STORAGE_QUEUE_THREAD);

	/* Takes requests from the front until the queue is closing and empty. */
	pthread_mutex_lock(&queue->lock);

	for (;;) {
		/* Sleeps until a request arrives or the queue closes; every wakeup rechecks both. */
		while (queue->head == NULL && queue->closing == 0)
			pthread_cond_wait(&queue->work, &queue->lock);

		/* Stops when everything queued was run and the queue is closing. */
		request = queue->head;
		if (request == NULL)
			break;

		/* Takes the request off the list; from now on only this thread touches it. */
		queue->head = request->next;
		if (queue->head != NULL)
			queue->head->prev = NULL;
		else
			queue->tail = NULL;
		request->next = NULL;
		request->prev = NULL;

		pthread_mutex_unlock(&queue->lock);

		/* Runs the request without the lock, because the backend may take long. */
		queue_execute(queue, request);

		/* Publishes the outcome under the lock. */
		pthread_mutex_lock(&queue->lock);

		if (request->waiting != 0) {
			/* The caller waits with the request on its stack: marks it done and wakes the callers; it is not touched again. */
			request->done = 1;
			pthread_cond_broadcast(&queue->done);
			continue;
		}

		/* The block is the queue's: its bytes no longer count, and the first error is kept for the engine. */
		queue->pending_bytes -= request->length;
		queue->pending_items--;
		if (request->error != 0 && queue->first_error == 0)
			queue->first_error = request->error;

		pthread_mutex_unlock(&queue->lock);

		/* Frees the finished block outside the lock. */
		free(request);

		pthread_mutex_lock(&queue->lock);
	}

	pthread_mutex_unlock(&queue->lock);

	/* The thread has nothing more to run. */
	return NULL;
}

/*
 * Runs one request against the backend and records its outcome in the request.
 */
static void
queue_execute(
	struct wb_storage_queue *queue,
	struct wb_storage_request *request)
{
	const struct browser_storage_calls *calls;

	calls = queue->calls;

	/* Calls the backend member the request asks for. */
	switch (request->kind) {
	case REQUEST_WRITE:
		request->error = calls->write(queue->context, request->space, request->key, request->bytes, request->length);
		break;
	case REQUEST_REMOVE:
		request->error = calls->remove(queue->context, request->space, request->key);
		break;
	case REQUEST_CLEAR:
		request->error = calls->clear(queue->context, request->space);
		break;
	case REQUEST_READ:
		queue_execute_read(queue, request);
		break;
	case REQUEST_LIST:
		queue_execute_list(queue, request);
		break;
	case REQUEST_FLUSH:
		request->error = 0;
		break;
	}
}

/*
 * Runs a read: the record's bytes go to the caller's buffer, which is put back if the read fails.
 */
static void
queue_execute_read(
	struct wb_storage_queue *queue,
	struct wb_storage_request *request)
{
	struct browser_storage_sink sink;
	struct wb_buffer *buffer;
	size_t start;
	int found;
	int error;
	int sink_error;

	buffer = request->buffer;
	start = buffer->length;
	found = 0;

	/* Binds a sink to the caller's buffer, which the caller leaves alone while it waits. */
	wb_storage_sink_init(&sink, buffer);

	/* Asks the backend for the record. */
	error = queue->calls->read(queue->context, request->space, request->key, &sink, &found);

	/* A failed append fails the read even when the backend ignored it. */
	sink_error = wb_storage_sink_error(&sink);
	if (error == 0)
		error = sink_error;

	/* Takes back whatever was appended when nothing was found or the read failed. */
	if (error != 0 || found == 0) {
		buffer->length = start;
		if (buffer->data != NULL)
			buffer->data[start] = 0;
		found = 0;
	}

	/* Publishes the answer for the waiting caller. */
	request->error = error;
	request->found = 0;
	if (found != 0)
		request->found = 1;
}

/*
 * Runs a list: the valid keys go to the caller's buffer, which is put back if the list fails.
 */
static void
queue_execute_list(
	struct wb_storage_queue *queue,
	struct wb_storage_request *request)
{
	struct queue_list_state state;
	struct wb_buffer *keys;
	size_t start;
	int error;

	keys = request->buffer;
	start = keys->length;

	/* Prepares the state the backend's callback fills. */
	state.keys = keys;
	state.count = 0;
	state.error = 0;

	/* Asks the backend for every key of the space. */
	error = queue->calls->list(queue->context, request->space, queue_list_each, &state);

	/* A key that could not be kept fails the list even when the backend ignored the stop. */
	if (error == 0)
		error = state.error;

	/* Takes back the keys already appended when the list failed. */
	if (error != 0) {
		keys->length = start;
		if (keys->data != NULL)
			keys->data[start] = 0;
		state.count = 0;
	}

	/* Publishes the answer for the waiting caller. */
	request->error = error;
	request->count = state.count;
}

/*
 * Receives one key from the backend's list: keeps it when valid, skips it otherwise.
 */
static int
queue_list_each(
	void *each_context,
	const char *key)
{
	struct queue_list_state *state;
	int valid;
	int error;

	state = each_context;

	/* Stops as soon as one append failed. */
	if (state->error != 0)
		return state->error;

	/* Skips a key a backend should not have produced. */
	valid = wb_storage_key_valid(key);
	if (valid == 0)
		return 0;

	/* Appends the key with its NUL. */
	error = wb_buffer_append(state->keys, key, strlen(key) + 1U);
	if (error != 0) {
		state->error = error;
		return error;
	}
	state->count++;

	/* Succeeded: asks the backend for the next key. */
	return 0;
}
