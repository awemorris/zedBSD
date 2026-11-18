/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Synchronization objects and notifications: AML mutexes and events
 * (ACPI 6.5 sections 19.6.1, 19.6.87 and 19.6.144), and the delivery of
 * Notify to the handlers drivers install.
 *
 * One interpreter lock serializes all AML, so an AML mutex that another
 * evaluation owns can only become free while that evaluation sleeps; the
 * waits here poll with drv_acpi_sleep(), which lets the lock go.
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "aml-internal.h"
#include "aml-os.h"

/*
 * The timeout that means waiting for ever.
 */
#define TIMEOUT_FOREVER 0xffffU

/*
 * How long one poll of a busy mutex or an unsignaled event sleeps, in
 * milliseconds.
 */
#define POLL_MILLISECONDS 1U

/*
 * The bits of the FACS Global Lock (ACPI 6.5 section 5.2.10.1): the lock
 * is owned, and its other side waits for it.
 */
#define GLOBAL_LOCK_PENDING	0x1U
#define GLOBAL_LOCK_OWNED	0x2U

/*
 * The Global Lock's dword in the FACS, which firmware takes too.
 *
 * It is NULL until drv_acpi_global_lock_attach(), and while it is NULL
 * \_GL_ is only an AML mutex among the operating system's threads.  It is
 * set once at attachment and never changes afterwards; the dword itself is
 * changed only by atomic exchanges, as firmware changes it concurrently.
 */
static volatile uint32_t *hardware_lock;

static int sync_object(struct drv_acpi_eval *eval, enum drv_acpi_type type, struct drv_acpi_object **result);
static int mutex_acquire(struct drv_acpi_eval *eval, struct drv_acpi_object *mutex, uint64_t timeout, bool *timed_out);
static int mutex_release(struct drv_acpi_eval *eval, struct drv_acpi_object *mutex);
static struct drv_acpi_object *global_lock_object(void);
static bool is_hardware_lock(struct drv_acpi_object *mutex);
static bool hardware_acquire(void);
static bool hardware_release(void);
static void held_mutex_drop(struct drv_acpi_thread *thread, struct drv_acpi_object *mutex);
static int event_wait(struct drv_acpi_object *event, uint64_t timeout, bool *timed_out);

/*
 * Installs a handler for the notifications a node receives.
 *
 * The list changes inside the interpreter, which Notify delivers from, so
 * that no delivery walks the list while it changes.  The handler runs in
 * the thread that runs the AML, with the interpreter held: it must not
 * evaluate AML, and of the handlers it may only remove its own.
 */
int
drv_acpi_notify_install(
	struct drv_acpi_node *node,
	drv_acpi_notify_handler_t handler,
	void *argument)
{
	struct drv_acpi_thread storage;
	struct drv_acpi_thread *thread;
	struct drv_acpi_notify *notify;

	/* Refuses a missing node or handler. */
	if (node == NULL || handler == NULL)
		return EINVAL;

	/* Allocates the entry. */
	notify = drv_acpi_os_alloc(sizeof(*notify));
	if (notify == NULL)
		return ENOMEM;

	/* Names the handler and its argument. */
	notify->handler = handler;
	notify->argument = argument;

	/* Puts the entry at the head of the node's list, inside the interpreter. */
	thread = drv_acpi_enter(&storage);
	notify->next = node->notify;
	node->notify = notify;
	drv_acpi_leave(thread);

	/* Succeeded: the node's notifications reach the handler. */
	return 0;
}

/*
 * Removes a handler drv_acpi_notify_install() installed.
 *
 * The handler and its argument name the entry.  The list changes inside
 * the interpreter, so once this returns no Notify calls the handler.
 */
int
drv_acpi_notify_remove(
	struct drv_acpi_node *node,
	drv_acpi_notify_handler_t handler,
	void *argument)
{
	struct drv_acpi_thread storage;
	struct drv_acpi_thread *thread;
	struct drv_acpi_notify **link;
	struct drv_acpi_notify *found;

	/* Refuses a missing node or handler. */
	if (node == NULL || handler == NULL)
		return EINVAL;

	/* Finds the entry and unlinks it, inside the interpreter. */
	thread = drv_acpi_enter(&storage);
	found = NULL;
	for (link = &node->notify;
	     *link != NULL;
	     link = &(*link)->next) {
		/* Stops at the entry of the handler and its argument. */
		if ((*link)->handler == handler && (*link)->argument == argument) {
			found = *link;
			*link = found->next;
			break;
		}
	}

	/* Leaves the interpreter; the entry, when found, is no longer on the list. */
	drv_acpi_leave(thread);

	/* Reports a handler that was not installed. */
	if (found == NULL)
		return ENOENT;

	/* Frees the entry. */
	drv_acpi_os_free(found);

	/* Succeeded: the handler no longer receives the node's notifications. */
	return 0;
}

/*
 * Runs work inside the interpreter with an AML mutex held.
 *
 * mutex_path names the mutex, such as "\\ECMU".  The work runs as one
 * interpreter entry that holds the mutex as AML would, so AML that
 * acquires the same mutex waits until the work is done.  The work may
 * evaluate AML itself: those evaluations join the entry and may acquire
 * the mutex again.
 */
int
drv_acpi_run_locked(
	const char *mutex_path,
	drv_acpi_locked_work_t work,
	void *argument)
{
	struct drv_acpi_thread storage;
	struct drv_acpi_thread *thread;
	struct drv_acpi_object *mutex;
	struct drv_acpi_node *node;
	bool timed_out;
	int release_error;
	int error;

	/* Refuses a missing mutex path or work. */
	if (mutex_path == NULL || work == NULL)
		return EINVAL;

	/* Enters the interpreter for the whole work. */
	thread = drv_acpi_enter(&storage);

	/* Finds the mutex. */
	error = drv_acpi_lookup(NULL, mutex_path, &node);
	if (error != 0) {
		drv_acpi_leave(thread);
		return error;
	}

	/* Refuses a name that is not a mutex. */
	mutex = node->object;
	if (mutex == NULL || mutex->type != DRV_ACPI_TYPE_MUTEX) {
		drv_acpi_leave(thread);
		return EINVAL;
	}

	/* Acquires it as the entry's thread, waiting as long as AML holds it. */
	error = drv_acpi_mutex_acquire(thread, mutex, TIMEOUT_FOREVER, &timed_out);
	if (error != 0) {
		drv_acpi_leave(thread);
		return error;
	}

	/* Runs the work. */
	error = work(argument);

	/* Releases the mutex; a release that fails is logged, and leaving the interpreter releases it anyway. */
	release_error = drv_acpi_mutex_release(thread, mutex);
	if (release_error != 0)
		drv_acpi_os_log("ACPI: %s could not be released after the work (error %d)\n", mutex_path, release_error);

	/* Leaves the interpreter. */
	drv_acpi_leave(thread);

	/* Reports a failed work. */
	if (error != 0)
		return error;

	/* Reports a mutex that could not be released. */
	if (release_error != 0)
		return release_error;

	/* Succeeded: the work ran with the mutex held. */
	return 0;
}

/*
 * Hands a Notify to every handler installed on the node.
 *
 * A notification nobody handles is logged and dropped, as firmware
 * expects of an operating system without the driver.
 */
int
drv_acpi_notify(
	struct drv_acpi_node *node,
	uint32_t value)
{
	struct drv_acpi_notify *notify;
	struct drv_acpi_notify *next;
	char path[128];
	int error;

	/* Logs a notification that has no handler. */
	if (node->notify == NULL) {
		/* Finds the node's path, or a mark when it does not fit. */
		error = drv_acpi_node_path(node, path, sizeof(path));
		if (error != 0)
			kern_strcpy(path, "(long path)");

		/* Writes the log line; the notification is dropped. */
		drv_acpi_os_log("ACPI: Notify(%s, 0x%x) has no handler\n", path, (unsigned)value);
		return 0;
	}

	/* Calls each handler; the next entry is read first, so that a handler may remove itself. */
	for (notify = node->notify;
	     notify != NULL;
	     notify = next) {
		next = notify->next;
		notify->handler(node, value, notify->argument);
	}

	/* Succeeded: every handler saw the notification. */
	return 0;
}

/*
 * Acquires an AML mutex for a thread, waiting up to the timeout in
 * milliseconds (0xffff waits for ever).
 *
 * A thread may acquire only mutexes of its current sync level or higher,
 * and a mutex it already owns is acquired again (ACPI 6.5 section 19.6.2).
 */
int
drv_acpi_mutex_acquire(
	struct drv_acpi_thread *thread,
	struct drv_acpi_object *mutex,
	uint64_t timeout,
	bool *timed_out)
{
	struct drv_acpi_mutex *state;
	uint64_t waited;
	bool hardware;
	bool taken;

	/* Starts with no timeout seen. */
	state = &mutex->value.mutex;
	*timed_out = false;

	/* Refuses a mutex below the level the thread already holds. */
	if (state->sync_level < thread->sync_level) {
		drv_acpi_os_log(
			"ACPI: Acquire of a sync level %u mutex while holding level %u\n",
			(unsigned)state->sync_level,
			(unsigned)thread->sync_level);
		return EDEADLK;
	}

	/* A mutex the thread already owns is acquired again: depth counts the Releases it waits for. */
	if (state->owner == thread) {
		state->depth++;
		return 0;
	}

	/*
	 * Waits for the owner to release it, sleeping so that it can.  The
	 * Global Lock must also be free of firmware; a try that finds
	 * firmware owning it asks firmware to signal its release.
	 */
	waited = 0;
	hardware = is_hardware_lock(mutex);
	for (;;) {
		/* Takes the mutex when no thread and no firmware owns it. */
		if (state->owner == NULL) {
			/* The Global Lock is also taken from firmware. */
			taken = true;
			if (hardware)
				taken = hardware_acquire();

			/* Stops waiting once it is taken. */
			if (taken)
				break;
		}

		/* Gives up when the timeout has passed. */
		if (timeout != TIMEOUT_FOREVER && waited >= timeout) {
			*timed_out = true;
			return 0;
		}

		/* Sleeps, which lets the owner run. */
		drv_acpi_sleep(POLL_MILLISECONDS);
		waited += POLL_MILLISECONDS;
	}

	/*
	 * The thread owns the mutex now and holds it above its older ones;
	 * the list keeps a reference so that the mutex outlives its node.
	 * The thread's level becomes the mutex's until the last Release.
	 */
	state->owner = thread;
	state->depth = 1;
	state->original_sync_level = thread->sync_level;
	state->next_held = thread->held;
	drv_acpi_object_ref(mutex);
	thread->held = mutex;
	thread->sync_level = state->sync_level;

	/* Succeeded: the thread owns the mutex. */
	return 0;
}

/*
 * Releases an AML mutex a thread owns.
 *
 * Mutexes are released in the reverse order of their sync levels: the one
 * released must be at the thread's current level.
 */
int
drv_acpi_mutex_release(
	struct drv_acpi_thread *thread,
	struct drv_acpi_object *mutex)
{
	struct drv_acpi_mutex *state;
	struct drv_acpi_object **link;
	bool hardware;
	bool waiting;

	/* Refuses a release by a thread that does not own the mutex. */
	state = &mutex->value.mutex;
	if (state->owner != thread) {
		drv_acpi_os_log("ACPI: Release of a mutex the thread does not own\n");
		return EPERM;
	}

	/* Refuses a release out of the order of the sync levels. */
	if (state->sync_level != thread->sync_level) {
		drv_acpi_os_log("ACPI: Release of a sync level %u mutex at level %u\n",
				(unsigned)state->sync_level,
				(unsigned)thread->sync_level);
		return EDEADLK;
	}

	/* Only the last matching Release frees it: depth counts the Acquires not yet released. */
	state->depth--;
	if (state->depth != 0)
		return 0;

	/* Finds the link to it in the thread's list of held mutexes. */
	for (link = &thread->held;
	     *link != NULL;
	     link = &(*link)->value.mutex.next_held) {
		/* Stops at the link that points at this mutex. */
		if (*link == mutex)
			break;
	}

	/* Takes it out of the list when it is there. */
	if (*link == mutex)
		*link = state->next_held;

	/*
	 * The mutex is free and the thread is back at the level it had
	 * before acquiring it.
	 */
	thread->sync_level = state->original_sync_level;
	state->owner = NULL;
	state->next_held = NULL;

	/* Gives the Global Lock back to firmware, telling it when it waits. */
	hardware = is_hardware_lock(mutex);
	if (hardware) {
		waiting = hardware_release();
		if (waiting)
			drv_acpi_events_global_release();
	}

	/* The list's reference goes. */
	drv_acpi_object_release(mutex);

	/* Succeeded: no thread owns the mutex. */
	return 0;
}

/*
 * Releases every mutex a thread still holds when it leaves the
 * interpreter, newest first.
 */
void
drv_acpi_thread_end(
	struct drv_acpi_thread *thread)
{
	struct drv_acpi_object *mutex;
	int error;

	/* Releases the held mutexes one at a time, whatever their depth. */
	while (thread->held != NULL) {
		/* Logs the newest mutex still held. */
		mutex = thread->held;
		drv_acpi_os_log("ACPI: a mutex was still held when the evaluation ended\n");

		/* Makes the next Release the last one, at the mutex's level. */
		mutex->value.mutex.depth = 1;
		thread->sync_level = mutex->value.mutex.sync_level;

		/* Releases it; one that will not release is dropped, so that the list still shrinks. */
		error = drv_acpi_mutex_release(thread, mutex);
		if (error != 0)
			held_mutex_drop(thread, mutex);
	}
}

/*
 * Acquires or releases the ACPI Global Lock (\_GL_) for a field whose
 * lock rule asks for it.
 */
int
drv_acpi_global_lock(
	struct drv_acpi_eval *eval,
	bool acquire)
{
	struct drv_acpi_thread *thread;
	struct drv_acpi_object *lock;
	bool timed_out;
	int error;

	/* Finds the global lock's mutex; a namespace without one needs no locking. */
	lock = global_lock_object();
	if (lock == NULL)
		return 0;

	/* Finds the thread that takes or lets go of it. */
	thread = drv_acpi_eval_thread(eval);

	/* Releases it, or acquires it, waiting as long as it takes. */
	if (!acquire) {
		error = drv_acpi_mutex_release(thread, lock);
	} else {
		error = drv_acpi_mutex_acquire(thread, lock, TIMEOUT_FOREVER, &timed_out);
	}

	/* Reports a lock that could not be taken or let go. */
	if (error != 0)
		return error;

	/* Succeeded: the thread holds the lock, or no longer does. */
	return 0;
}

/*
 * Starts taking the FACS Global Lock with \_GL_, which firmware shares.
 *
 * The word is the lock's dword in the FACS, mapped for the life of the
 * system.  It is attached after drv_acpi_events_init(), which enables the
 * event firmware raises when it lets the lock go and knows where to
 * signal firmware that the operating system let it go.
 */
int
drv_acpi_global_lock_attach(
	volatile uint32_t *word)
{
	/* Refuses a missing lock. */
	if (word == NULL)
		return EINVAL;

	/* Uses it from the next Acquire of \_GL_ on. */
	hardware_lock = word;

	/* Succeeded: \_GL_ is shared with firmware from now on. */
	return 0;
}

/*
 * Runs Acquire, Release, Signal, Wait or Reset.
 */
int
drv_acpi_sync_operator(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *object;
	struct drv_acpi_object *answer;
	enum drv_acpi_type type;
	uint64_t timeout;
	bool timed_out;
	int error;

	/* Acquire and Release name a mutex; the others name an event. */
	type = DRV_ACPI_TYPE_EVENT;
	if (opcode == DRV_ACPI_OP_ACQUIRE || opcode == DRV_ACPI_OP_RELEASE)
		type = DRV_ACPI_TYPE_MUTEX;

	/* Finds the mutex or event the operator names. */
	error = sync_object(eval, type, &object);
	if (error != 0)
		return error;

	/* Runs the operator. */
	timed_out = false;
	error = 0;
	switch (opcode) {
	case DRV_ACPI_OP_ACQUIRE:
		/* The timeout is a word in the AML, not a TermArg. */
		error = drv_acpi_stream_integer(eval, 2, &timeout);
		if (error != 0)
			break;

		/* Acquires the mutex within the timeout. */
		error = mutex_acquire(eval, object, timeout, &timed_out);
		break;
	case DRV_ACPI_OP_RELEASE:
		/* Releases the mutex. */
		error = mutex_release(eval, object);
		break;
	case DRV_ACPI_OP_SIGNAL:
		/* A signal is counted until a Wait takes it: pending counts the signals not yet taken. */
		object->value.event.pending++;
		break;
	case DRV_ACPI_OP_WAIT:
		/* Evaluates the timeout. */
		error = drv_acpi_eval_integer(eval, &timeout);
		if (error != 0)
			break;

		/* Takes one signal within the timeout. */
		error = event_wait(object, timeout, &timed_out);
		break;
	default:
		/* Reset forgets every signal not yet taken. */
		object->value.event.pending = 0;
		break;
	}

	/* Lets go of the object. */
	drv_acpi_object_release(object);

	/* Reports a failed operator. */
	if (error != 0)
		return error;

	/* Makes the operator's value. */
	answer = drv_acpi_object_integer_new(0);
	if (answer == NULL)
		return ENOMEM;

	/* Acquire and Wait report true when they timed out; the rest report zero. */
	if (timed_out)
		answer->value.integer = drv_acpi_integer_mask();

	/* Hands over the value. */
	*result = answer;

	/* Succeeded: result is the operator's value. */
	return 0;
}

/* Parses the SuperName of a mutex or event and checks its type. */
static int
sync_object(
	struct drv_acpi_eval *eval,
	enum drv_acpi_type type,
	struct drv_acpi_object **result)
{
	struct drv_acpi_target target;
	struct drv_acpi_object *object;
	struct drv_acpi_node *node;
	int error;

	/* Parses the operand. */
	error = drv_acpi_parse_target(eval, &target);
	if (error != 0)
		return error;

	/* A name is the object; a reference is followed to its node. */
	object = NULL;
	if (target.kind == DRV_ACPI_TARGET_NODE) {
		node = drv_acpi_ns_resolve_alias(target.node);
		object = node->object;
	} else if (target.kind == DRV_ACPI_TARGET_REFERENCE) {
		node = drv_acpi_object_reference_node(target.reference);
		if (node != NULL) {
			node = drv_acpi_ns_resolve_alias(node);
			object = node->object;
		}
	} else if (target.kind == DRV_ACPI_TARGET_LOCAL) {
		object = eval->frame->locals[target.index];
	} else if (target.kind == DRV_ACPI_TARGET_ARGUMENT) {
		object = eval->frame->arguments[target.index];
	}

	/* The object stays alive in its node or slot; the target is done with. */
	drv_acpi_target_release(&target);

	/* A local or an argument may hold a reference to the object. */
	if (object != NULL && object->type == DRV_ACPI_TYPE_REFERENCE) {
		/* Follows the reference to the node it names, through any alias. */
		node = drv_acpi_object_reference_node(object);
		object = NULL;
		if (node != NULL) {
			node = drv_acpi_ns_resolve_alias(node);
			object = node->object;
		}
	}

	/* Refuses anything but the expected type. */
	if (object == NULL || object->type != type) {
		drv_acpi_os_log("ACPI: synchronization operand of the wrong type\n");
		return EINVAL;
	}

	/* Hands over the object with a reference of the caller's own. */
	drv_acpi_object_ref(object);
	*result = object;

	/* Succeeded: the caller shares the object. */
	return 0;
}

/* Acquires an AML mutex for the operator, waiting up to the timeout. */
static int
mutex_acquire(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *mutex,
	uint64_t timeout,
	bool *timed_out)
{
	struct drv_acpi_thread *thread;
	int error;

	/* The evaluating thread becomes the owner. */
	thread = drv_acpi_eval_thread(eval);
	error = drv_acpi_mutex_acquire(thread, mutex, timeout, timed_out);
	if (error != 0)
		return error;

	/* Succeeded: the thread owns the mutex, unless timed_out says the wait ran out. */
	return 0;
}

/* Releases an AML mutex for the operator. */
static int
mutex_release(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *mutex)
{
	struct drv_acpi_thread *thread;
	int error;

	/* Only the owning thread can release it. */
	thread = drv_acpi_eval_thread(eval);
	error = drv_acpi_mutex_release(thread, mutex);
	if (error != 0)
		return error;

	/* Succeeded: the thread let the mutex go. */
	return 0;
}

/* Finds the mutex of the global lock, \_GL_. */
static struct drv_acpi_object *
global_lock_object(void)
{
	struct drv_acpi_node *node;
	int error;

	/* Looks the predefined name up. */
	error = drv_acpi_lookup(NULL, "\\_GL_", &node);
	if (error != 0)
		return NULL;

	/* Only a mutex can be the lock. */
	if (node->object == NULL || node->object->type != DRV_ACPI_TYPE_MUTEX)
		return NULL;

	/* Reports the mutex \_GL_ holds. */
	return node->object;
}

/* Reports whether a mutex is \_GL_ backed by the FACS Global Lock. */
static bool
is_hardware_lock(
	struct drv_acpi_object *mutex)
{
	struct drv_acpi_object *lock;

	/* Only an attached lock is shared with firmware. */
	if (hardware_lock == NULL)
		return false;

	/* The mutex must be the one \_GL_ names. */
	lock = global_lock_object();
	if (mutex == lock)
		return true;

	/* Reports an ordinary AML mutex. */
	return false;
}

/*
 * Tries to take the FACS Global Lock (ACPI 6.5 section 5.2.10.1).  When
 * firmware owns it, the try marks it pending instead, and firmware raises
 * the Global Lock event when it lets it go; the caller tries again.
 */
static bool
hardware_acquire(void)
{
	uint32_t old;
	uint32_t new;
	bool exchanged;

	/* Sets owned, and pending when it was owned already, in one exchange. */
	old = *hardware_lock;
	for (;;) {
		/* Computes the new value from the one read. */
		new = (old & ~GLOBAL_LOCK_PENDING) | GLOBAL_LOCK_OWNED;
		if ((old & GLOBAL_LOCK_OWNED) != 0)
			new |= GLOBAL_LOCK_PENDING;

		/* Exchanges it; a failed exchange rereads the value and tries again. */
		exchanged = __atomic_compare_exchange_n(hardware_lock, &old, new, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
		if (exchanged)
			break;
	}

	/* A pending new value means firmware owns the lock and will signal its release. */
	if ((new & GLOBAL_LOCK_PENDING) != 0)
		return false;

	/* Reports the lock taken. */
	return true;
}

/*
 * Lets the FACS Global Lock go and reports whether firmware was waiting
 * for it, in which case the caller signals firmware with GBL_RLS.
 */
static bool
hardware_release(void)
{
	uint32_t old;
	uint32_t new;
	bool exchanged;

	/* Clears owned and pending in one exchange. */
	old = *hardware_lock;
	for (;;) {
		/* Exchanges the value without the two bits; a failed exchange rereads it. */
		new = old & ~(GLOBAL_LOCK_PENDING | GLOBAL_LOCK_OWNED);
		exchanged = __atomic_compare_exchange_n(hardware_lock, &old, new, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
		if (exchanged)
			break;
	}

	/* A pending old value means firmware waits for the lock. */
	if ((old & GLOBAL_LOCK_PENDING) != 0)
		return true;

	/* Reports that firmware does not wait. */
	return false;
}

/*
 * Drops a held mutex whose Release failed when its thread ended, so that
 * the thread's list still shrinks.
 *
 * The Release fails only when another thread owns the mutex, so the
 * owner, its depth and the Global Lock are left to that thread: the mutex
 * is only unlinked and the list's reference released.
 */
static void
held_mutex_drop(
	struct drv_acpi_thread *thread,
	struct drv_acpi_object *mutex)
{
	struct drv_acpi_mutex *state;

	/* Logs the mutex that could not be released. */
	state = &mutex->value.mutex;
	drv_acpi_os_log("ACPI: a held mutex could not be released; it is dropped\n");

	/* Unlinks it from the head of the list, where the thread's end found it. */
	thread->held = state->next_held;
	state->next_held = NULL;

	/* The thread is back at the level it had before acquiring it. */
	thread->sync_level = state->original_sync_level;

	/* The list's reference goes. */
	drv_acpi_object_release(mutex);
}

/* Takes one signal of an event, waiting up to the timeout in milliseconds. */
static int
event_wait(
	struct drv_acpi_object *event,
	uint64_t timeout,
	bool *timed_out)
{
	uint64_t waited;

	/* Waits for a signal, sleeping so that another evaluation can give one. */
	*timed_out = false;
	waited = 0;
	while (event->value.event.pending == 0) {
		/* Gives up when the timeout has passed. */
		if (timeout != TIMEOUT_FOREVER && waited >= timeout) {
			*timed_out = true;
			return 0;
		}

		/* Sleeps, which lets a signaler run. */
		drv_acpi_sleep(POLL_MILLISECONDS);
		waited += POLL_MILLISECONDS;
	}

	/* Takes the signal: one fewer is left for the next Wait. */
	event->value.event.pending--;

	/* Succeeded: the wait took a signal. */
	return 0;
}
