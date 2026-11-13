/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The printers on the wire (ws145-p003, plan/ws145/design.md section 3):
 * kl_system_printers_v1, made by the system manager's get_printers (since
 * its version 17) for a client of the compositor's own user.  The printers
 * and the jobs are libkeiland-backend's (print/print.c), opened when the
 * first printers object is made: the settings file is the user's
 * ~/.config/keiland/printers.conf, the daemon is keiland-printd.
 *
 *   add(request, protocol, host, port, path)  remove(request, printer)
 *   set_default(request, printer)            cancel(request, job)
 *   print(request, printer, title, document)  the document's descriptor beside it
 *   edit(request, printer, name, path)        since 24: a printer's name, IPP path or LPD queue
 *                                            ("" keeps each; ws177-p025)
 *
 * Every printers object is sent the printers, the jobs and done when it is
 * made and whenever they change; the object that asked gets queued
 * (print's job) and the result.  The backend numbers its requests; a table
 * pairs them with the client's.  Titles and hosts are not logged.
 *
 * The settings file is written, and checked for another session's change,
 * on the compositor's file I/O thread (io.c), or in the event loop when
 * that thread does not run.  Nothing is followed on the clock: the file is
 * checked once when a printers object is made (a client is about to show
 * the printers, as they are then; another session's later change is seen
 * by the next object or with this compositor's next change); the daemon's
 * lines come on its socket, which the loop waits on; and a daemon shut for
 * writing is given up by a timer at the end of its time.
 */

#include "kwl.h"

#include "userland/desktop/libkeiland/system/kl-system-protocol.h"
#include "userland/desktop/paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Marks a parameter a callback's signature has and this file does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* The longest host, path, title and name a request carries, with their NULs. */
#define PRINTERS_HOST_MAX	64U
#define PRINTERS_PATH_MAX	64U
#define PRINTERS_TITLE_MAX	128U
#define PRINTERS_NAME_MAX	128U

/* The longest event: a printer's (four words, three strings). */
#define PRINTERS_EVENT_MAX	(4U * 4U + 4U + PRINTERS_HOST_MAX + 4U + PRINTERS_PATH_MAX + 4U + KL_BACKEND_PRINTER_NAME_MAX + 4U)

/* The most requests waiting for the backend's answer. */
#define PRINTERS_WAITING_MAX	16U

/*
 * A request waiting: the backend's number, the client (by its number) and
 * object, the client's number, and a print's job.
 */
struct printers_waiting {
	uint32_t backend;
	uint64_t client;
	uint32_t object;
	uint32_t request;
	uint32_t job;
	int used;
};

/*
 * A write or a check of the settings file on the file I/O thread: the
 * lane's job, the backend (open for the process's life), and a check's
 * record (NULL for a write).
 */
struct printers_file_job {
	struct kwl_lane_job lane;
	struct kl_backend_print *backend;
	struct kl_backend_print_check *check;
};

/*
 * The printers of the compositor: the backend (opened once), whether its
 * file's writes and checks are the compositor's (hosted), a write or a
 * check given and not taken yet, a check asked for and not given yet, and
 * the requests waiting.  Only the event loop's thread touches it.
 */
static struct {
	struct kl_backend_print *backend;
	int opened;
	int hosted;
	int writing;
	int checking;
	int check_wanted;
	struct printers_waiting waiting[PRINTERS_WAITING_MAX];
	uint32_t serial;
} printers_state;

/*
 * The timer of a daemon shut for writing (it broke the protocol): armed
 * for the end of the time it is given to end (kl_backend_print_due), when
 * the pass's update closes its socket; disarmed once the daemon has gone.
 * Readied by the first tick that has the backend; only the event loop's
 * thread touches it.
 */
static struct kwl_timer printers_shut_timer;

static struct kl_backend_print *printers_backend(void);
static void printers_host(void);
static void printers_file(struct kwl_server *server);
static void printers_check(struct kwl_server *server);
static void printers_shut_fired(void *context, struct kwl_timer *timer);
static int printers_file_give(struct kwl_server *server, struct kl_backend_print_check *check);
static void printers_file_run(struct kwl_lane_job *job);
static void printers_file_done(void *context, struct kwl_lane_job *job);
static void printers_file_drop(struct kwl_lane_job *job);
static int printers_add(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int printers_number_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
static int printers_print(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int printers_edit(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int printers_path_ok(const char *path);
static void printers_wait(struct kwl_object *object, uint32_t backend, uint32_t request, uint32_t job);
static void printers_state_to(struct kwl_client *client, uint32_t id);
static void printers_tell(struct kwl_server *server);
static void printers_answers(struct kwl_server *server);
static void printers_result(struct kwl_client *client, uint32_t id, uint32_t request, uint32_t applied, uint32_t saved);
static uint32_t printers_applied(int error);
static int printers_title_ok(const char *title);
static int printers_string(const unsigned char *bytes, size_t size, size_t offset, size_t bound, const char **text, size_t *next);
static size_t printers_put_string(unsigned char *payload, size_t offset, const char *text);
static uint32_t printers_word(const unsigned char *bytes, size_t offset);

/* The operations of a write or a check of the settings file on the file I/O thread. */
static const struct kwl_lane_ops printers_file_ops = {
	printers_file_run,
	printers_file_done,
	printers_file_drop
};

/*
 * Opens the printers at the start, before the clients come (the event
 * loop reads no file for them later): the settings file read, the
 * daemon's program looked at, and the file's work the compositor's from
 * now on (its file I/O thread's when it runs).
 */
void
kwl_printers_prepare(
	struct kwl_server *server)
{
	struct kl_backend_print *backend;
	int opened;
	int can;

	/* The backend open and hosted. */
	printers_host();

	/* The log says what the clients will be told. */
	backend = printers_backend();
	opened = 0;
	if (backend != NULL)
		opened = 1;
	can = kl_backend_print_can(backend);
	printf("KWL PRINTERS prepared open=%d hosted=%d can=%d\n", opened, printers_state.hosted, can);
}

/*
 * Tells whether printing is offered: the daemon's program is there.
 */
int
kwl_printers_available(void)
{
	struct kl_backend_print *backend;

	/* The backend, and its daemon. */
	backend = printers_backend();
	if (backend == NULL)
		return 0;
	return kl_backend_print_can(backend);
}

/*
 * Makes a printers object for a manager's get_printers (new id), and
 * sends it the printers and the jobs.  Returns 0, or EPROTO.
 */
int
kwl_printers_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;
	uint32_t id;

	/* The new object's ID. */
	if (size != 4U)
		return EPROTO;
	id = printers_word(bytes, 0U);

	/* The object, under the ID the client chose. */
	created = kwl_create(manager->client, id, KWL_SYSTEM_PRINTERS, manager->version);
	if (created == NULL)
		return EPROTO;

	/* The backend's file is the compositor's before any change is asked. */
	printers_host();

	/*
	 * A client is about to show the printers: the file is checked for
	 * another session's change by the next tick, and a change found is
	 * told to this object too.
	 */
	printers_state.check_wanted = 1;

	/* Its first state. */
	printers_state_to(manager->client, id);
	printf("KWL PRINTERS object client=%llu id=%u\n", (unsigned long long)manager->client->number, id);
	return 0;
}

/*
 * Carries out a request of a printers object.  Returns 0, EAGAIN for a
 * print whose document has not come yet, or EPROTO.
 */
int
kwl_printers_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* The object goes. */
	if (opcode == KL_SYSTEM_PRINTERS_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	}

	/* Each request. */
	if (opcode == KL_SYSTEM_PRINTERS_ADD)
		error = printers_add(object, bytes, size);
	else if (opcode == KL_SYSTEM_PRINTERS_PRINT)
		error = printers_print(object, bytes, size);
	else if (opcode == KL_SYSTEM_PRINTERS_REMOVE || opcode == KL_SYSTEM_PRINTERS_SET_DEFAULT || opcode == KL_SYSTEM_PRINTERS_CANCEL)
		error = printers_number_request(object, opcode, bytes, size);
	else if (opcode == KL_SYSTEM_PRINTERS_EDIT && object->version >= KL_SYSTEM_SINCE_PRINTER_EDIT)
		error = printers_edit(object, bytes, size);
	else
		error = EPROTO;
	return error;
}

/*
 * Looks after the printers once a pass: the settings file's writes and
 * the checks asked for, the daemon's lines, a new state to every printers
 * object, the answers to the clients that asked, and the wait for the
 * daemon's socket and for a shut daemon's time.
 */
void
kwl_printers_tick(
	struct kwl_server *server)
{
	unsigned changed;
	uint64_t due;
	short events;
	int descriptor;

	/* Nothing before the first printers object. */
	if (printers_state.backend == NULL)
		return;

	/* The settings file's writes, and a check asked for, given to the file I/O thread. */
	printers_file(server);
	printers_check(server);

	/* What came. */
	changed = 0;
	(void)kl_backend_print_update(printers_state.backend, &changed);

	/*
	 * The printers and jobs are told before the answers, so that a client
	 * that has its answer already holds the list the change made (T1-459:
	 * printtest edit printed the old list after its result).
	 */
	if ((changed & KL_BACKEND_PRINT_CHANGED_LIST) != 0U)
		printers_tell(server);
	if ((changed & KL_BACKEND_PRINT_CHANGED_RESULT) != 0U)
		printers_answers(server);

	/* The daemon's connection wakes the loop with what the daemon says. */
	descriptor = kl_backend_print_descriptor(printers_state.backend, &events);
	kwl_watch(server, descriptor, events);

	/* A daemon shut for writing is given up at the end of its time: the timer wakes the pass whose update does it. */
	if (printers_shut_timer.fire == NULL)
		kwl_timer_init(&printers_shut_timer, "printers-shut", printers_shut_fired);
	due = kl_backend_print_due(printers_state.backend);
	kwl_timer_set(server, &printers_shut_timer, due);
}

/* Opens the backend once: the user's settings file and the daemon's program. */
static struct kl_backend_print *
printers_backend(void)
{
	char home[512];
	char config[640];
	int error;

	/* Once. */
	if (printers_state.opened)
		return printers_state.backend;
	printers_state.opened = 1;

	/* The user's file. */
	error = kwl_settings_home(home, sizeof(home));
	if (error != 0)
		return NULL;
	(void)snprintf(config, sizeof(config), "%s/.config/keiland/printers.conf", home);
	printers_state.backend = kl_backend_print_open(config, NULL, KEILAND_LIBEXECDIR "/keiland-printd");
	return printers_state.backend;
}

/* Carries out add(request, protocol, host, port, path). */
static int
printers_add(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	struct kl_backend_print *backend;
	const char *host;
	const char *path;
	uint32_t request;
	uint32_t protocol;
	uint32_t port;
	uint32_t backend_request;
	size_t offset;
	int error;

	/* request, protocol, host, port, path. */
	if (size < 8U)
		return EPROTO;
	request = printers_word(bytes, 0U);
	protocol = printers_word(bytes, 4U);
	error = printers_string(bytes, size, 8U, PRINTERS_HOST_MAX, &host, &offset);
	if (error != 0 || offset + 4U > size)
		return EPROTO;
	port = printers_word(bytes, offset);
	error = printers_string(bytes, size, offset + 4U, PRINTERS_PATH_MAX, &path, &offset);
	if (error != 0 || offset != size)
		return EPROTO;

	/* The backend's. */
	backend = printers_backend();
	if (backend == NULL) {
		printers_result(object->client, object->id, request, KL_SYSTEM_RESULT_UNAVAILABLE, 0U);
		return 0;
	}

	/* Asked of the backend. */
	error = kl_backend_print_add(backend, protocol, host, port, path, &backend_request);
	printf("KWL PRINTERS add client=%llu protocol=%u error=%d\n", (unsigned long long)object->client->number, protocol, error);
	if (error != 0) {
		printers_result(object->client, object->id, request, printers_applied(error), 0U);
		return 0;
	}

	/* Answered at the next tick. */
	printers_wait(object, backend_request, request, 0U);
	return 0;
}

/* Carries out remove, set_default or cancel (request, number). */
static int
printers_number_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kl_backend_print *backend;
	uint32_t backend_request;
	uint32_t request;
	uint32_t number;
	int error;

	/* request, number. */
	if (size != 8U)
		return EPROTO;
	request = printers_word(bytes, 0U);
	number = printers_word(bytes, 4U);

	/* The backend's. */
	backend = printers_backend();
	if (backend == NULL) {
		printers_result(object->client, object->id, request, KL_SYSTEM_RESULT_UNAVAILABLE, 0U);
		return 0;
	}

	/* Asked of the backend. */
	if (opcode == KL_SYSTEM_PRINTERS_REMOVE)
		error = kl_backend_print_remove(backend, number, &backend_request);
	else if (opcode == KL_SYSTEM_PRINTERS_SET_DEFAULT)
		error = kl_backend_print_set_default(backend, number, &backend_request);
	else
		error = kl_backend_print_cancel(backend, number, &backend_request);
	if (error != 0) {
		printers_result(object->client, object->id, request, printers_applied(error), 0U);
		return 0;
	}

	/* Answered at the next tick. */
	printers_wait(object, backend_request, request, 0U);
	return 0;
}

/*
 * Carries out print(request, printer, title, document): the document's
 * descriptor is taken first (none yet: EAGAIN, the request comes again);
 * from there it is closed or the backend's.
 */
static int
printers_print(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	struct kl_backend_print *backend;
	const char *title;
	uint32_t backend_request;
	uint32_t request;
	uint32_t printer;
	uint32_t job;
	uint32_t refused;
	size_t offset;
	int title_ok;
	int error;
	int fd;

	/* request, printer, title; the descriptor beside them. */
	if (size < 8U)
		return EPROTO;
	request = printers_word(bytes, 0U);
	printer = printers_word(bytes, 4U);
	error = printers_string(bytes, size, 8U, PRINTERS_TITLE_MAX, &title, &offset);
	if (error != 0 || offset != size)
		return EPROTO;
	fd = kwl_take_fd(object->client);
	if (fd < 0)
		return EAGAIN;

	/* A title of the rules, and a backend. */
	title_ok = printers_title_ok(title);
	backend = printers_backend();
	if (!title_ok || backend == NULL) {
		(void)close(fd);
		refused = KL_SYSTEM_RESULT_INVALID;
		if (title_ok)
			refused = KL_SYSTEM_RESULT_UNAVAILABLE;
		printers_result(object->client, object->id, request, refused, 0U);
		return 0;
	}

	/* The backend's from here: the job known at once. */
	error = kl_backend_print_submit(backend, printer, title, fd, &backend_request, &job);
	printf("KWL PRINTERS print client=%llu printer=%u job=%u error=%d\n", (unsigned long long)object->client->number, printer, job, error);
	if (error != 0) {
		printers_result(object->client, object->id, request, printers_applied(error), 0U);
		return 0;
	}

	/* Answered at the next tick, queued first. */
	printers_wait(object, backend_request, request, job);
	return 0;
}

/*
 * Carries out edit(request, printer, name, path) (since 24, ws177-p025): a
 * name of the title's rules and a path or queue without spaces or control
 * characters, each "" to keep it; others are answered invalid.
 */
static int
printers_edit(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	struct kl_backend_print *backend;
	const char *name;
	const char *path;
	uint32_t backend_request;
	uint32_t request;
	uint32_t printer;
	size_t offset;
	int name_ok;
	int path_ok;
	int error;

	/* request, printer, name, path. */
	if (size < 8U)
		return EPROTO;
	request = printers_word(bytes, 0U);
	printer = printers_word(bytes, 4U);
	error = printers_string(bytes, size, 8U, PRINTERS_NAME_MAX, &name, &offset);
	if (error != 0)
		return EPROTO;
	error = printers_string(bytes, size, offset, PRINTERS_PATH_MAX, &path, &offset);
	if (error != 0 || offset != size)
		return EPROTO;

	/* A name and a path of the rules. */
	name_ok = printers_title_ok(name);
	path_ok = printers_path_ok(path);
	if (!name_ok || !path_ok) {
		printers_result(object->client, object->id, request, KL_SYSTEM_RESULT_INVALID, 0U);
		return 0;
	}

	/* The backend's. */
	backend = printers_backend();
	if (backend == NULL) {
		printers_result(object->client, object->id, request, KL_SYSTEM_RESULT_UNAVAILABLE, 0U);
		return 0;
	}

	/* Asked of the backend. */
	error = kl_backend_print_edit(backend, printer, name, path, &backend_request);
	printf("KWL PRINTERS edit client=%llu printer=%u error=%d\n", (unsigned long long)object->client->number, printer, error);
	if (error != 0) {
		printers_result(object->client, object->id, request, printers_applied(error), 0U);
		return 0;
	}

	/* Answered at the next tick. */
	printers_wait(object, backend_request, request, 0U);
	return 0;
}

/* Tells whether an IPP path or LPD queue keeps the rules: no space and no control character (C0, DEL). */
static int
printers_path_ok(
	const char *path)
{
	const unsigned char *byte;

	/* Each byte. */
	for (byte = (const unsigned char *)path; *byte != '\0'; byte++) {
		if (*byte <= 0x20U || *byte == 0x7fU)
			return 0;
	}

	/* Succeeded: the path keeps the rules. */
	return 1;
}

/* Keeps a request waiting for its answer (a full table answers it busy). */
static void
printers_wait(
	struct kwl_object *object,
	uint32_t backend,
	uint32_t request,
	uint32_t job)
{
	size_t index;

	/* A free slot. */
	for (index = 0; index < PRINTERS_WAITING_MAX; index++) {
		if (printers_state.waiting[index].used)
			continue;
		printers_state.waiting[index].used = 1;
		printers_state.waiting[index].backend = backend;
		printers_state.waiting[index].client = object->client->number;
		printers_state.waiting[index].object = object->id;
		printers_state.waiting[index].request = request;
		printers_state.waiting[index].job = job;
		return;
	}

	/* None. */
	printers_result(object->client, object->id, request, KL_SYSTEM_RESULT_BUSY, 0U);
}

/* Sends a client's printers object the printers, the jobs and done. */
static void
printers_state_to(
	struct kwl_client *client,
	uint32_t id)
{
	static unsigned char payload[PRINTERS_EVENT_MAX + KL_BACKEND_PRINT_TITLE_MAX];
	struct kl_backend_printer printers[KL_BACKEND_PRINTERS_MAX];
	struct kl_backend_print_job jobs[KL_BACKEND_PRINT_JOBS_MAX];
	struct kl_backend_print *backend;
	uint32_t words[4];
	size_t printer_count;
	size_t job_count;
	size_t length;
	size_t index;

	/* What the backend has (nothing without one). */
	printer_count = 0;
	job_count = 0;
	backend = printers_backend();
	if (backend != NULL) {
		printer_count = kl_backend_print_printers(printers_state.backend, printers, KL_BACKEND_PRINTERS_MAX);
		job_count = kl_backend_print_jobs(printers_state.backend, jobs, KL_BACKEND_PRINT_JOBS_MAX);
	}

	/* Each printer: id, protocol, host, port, path, name, flags. */
	for (index = 0; index < printer_count; index++) {
		words[0] = printers[index].id;
		words[1] = printers[index].protocol;
		memcpy(payload, words, 8U);
		length = printers_put_string(payload, 8U, printers[index].host);
		words[0] = printers[index].port;
		memcpy(payload + length, words, 4U);
		length = printers_put_string(payload, length + 4U, printers[index].path);
		length = printers_put_string(payload, length, printers[index].name);
		words[0] = 0U;
		if (printers[index].is_default)
			words[0] = KL_SYSTEM_PRINTER_DEFAULT;
		memcpy(payload + length, words, 4U);
		length += 4U;
		(void)kwl_emit(client, id, KL_SYSTEM_PRINTERS_EVENT_PRINTER, payload, length);
	}

	/* Each job: job, printer, state, title, detail. */
	for (index = 0; index < job_count; index++) {
		words[0] = jobs[index].job;
		words[1] = jobs[index].printer;
		words[2] = jobs[index].state;
		memcpy(payload, words, 12U);
		length = printers_put_string(payload, 12U, jobs[index].title);
		length = printers_put_string(payload, length, jobs[index].detail);
		(void)kwl_emit(client, id, KL_SYSTEM_PRINTERS_EVENT_JOB, payload, length);
	}

	/* The whole state. */
	printers_state.serial++;
	words[0] = printers_state.serial;
	(void)kwl_emit(client, id, KL_SYSTEM_PRINTERS_EVENT_DONE, words, 4U);
}

/* Sends every printers object of every client the state. */
static void
printers_tell(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *object;
	struct kl_backend_print_job jobs[KL_BACKEND_PRINT_JOBS_MAX];
	size_t count;
	size_t index;

	/* Each live printers object. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (object = client->objects; object != NULL; object = object->next) {
			if (object->dead || object->kind != KWL_SYSTEM_PRINTERS)
				continue;
			printers_state_to(client, object->id);
		}
	}

	/* The log the tests read: the jobs' states. */
	count = kl_backend_print_jobs(printers_state.backend, jobs, KL_BACKEND_PRINT_JOBS_MAX);
	for (index = 0; index < count; index++)
		printf("KWL PRINTERS job=%u printer=%u state=%u detail=%s\n", jobs[index].job, jobs[index].printer, jobs[index].state, jobs[index].detail);
}

/* Sends the backend's answers to the clients that asked (queued before a print's result). */
static void
printers_answers(
	struct kwl_server *server)
{
	struct printers_waiting *waiting;
	struct kwl_client *client;
	uint32_t backend_request;
	uint32_t words[2];
	uint32_t applied;
	unsigned saved;
	size_t index;
	int error;
	int taken;

	/* Each answer. */
	for (;;) {
		taken = kl_backend_print_take_result(printers_state.backend, &backend_request, &error, &saved);
		if (!taken)
			break;

		/* The request it answers. */
		waiting = NULL;
		for (index = 0; index < PRINTERS_WAITING_MAX; index++) {
			if (printers_state.waiting[index].used && printers_state.waiting[index].backend == backend_request)
				waiting = &printers_state.waiting[index];
		}

		/* None waits for it (answered already). */
		if (waiting == NULL)
			continue;
		waiting->used = 0;

		/* Its client, still there. */
		for (client = server->clients; client != NULL; client = client->next) {
			if (client->number == waiting->client && !client->fatal)
				break;
		}

		/* The client went. */
		if (client == NULL)
			continue;

		/* A print's job first, then the result. */
		if (error == 0 && waiting->job != 0U) {
			words[0] = waiting->request;
			words[1] = waiting->job;
			(void)kwl_emit(client, waiting->object, KL_SYSTEM_PRINTERS_EVENT_QUEUED, words, sizeof(words));
		}

		/* The result, not saved when the file could not be written. */
		applied = printers_applied(error);
		if (error == 0 && !saved)
			applied = KL_SYSTEM_RESULT_NOT_SAVED;
		printers_result(client, waiting->object, waiting->request, applied, saved);
	}
}

/* Answers a request: result(request, applied, saved). */
static void
printers_result(
	struct kwl_client *client,
	uint32_t id,
	uint32_t request,
	uint32_t applied,
	uint32_t saved)
{
	uint32_t words[3];

	/* request, applied, saved. */
	words[0] = request;
	words[1] = applied;
	words[2] = saved;
	(void)kwl_emit(client, id, KL_SYSTEM_PRINTERS_EVENT_RESULT, words, sizeof(words));
}

/* The result's number for a backend's errno value. */
static uint32_t
printers_applied(
	int error)
{
	/* Each value. */
	if (error == 0)
		return KL_SYSTEM_RESULT_OK;
	if (error == EINVAL)
		return KL_SYSTEM_RESULT_INVALID;
	if (error == EBUSY)
		return KL_SYSTEM_RESULT_BUSY;
	if (error == ENOTSUP)
		return KL_SYSTEM_RESULT_UNSUPPORTED;
	return KL_SYSTEM_RESULT_FAILED;
}

/*
 * Tells whether a title keeps the rules (design §2): valid UTF-8 (no
 * overlong form, no surrogate, nothing past U+10FFFF, no sequence cut
 * short), no C0, DEL or C1 control character, at most 127 bytes
 * (ws177-p024: the check was loose before).
 */
static int
printers_title_ok(
	const char *title)
{
	const unsigned char *byte;
	unsigned long code;
	unsigned follow;
	unsigned index;
	size_t length;

	/* Its length. */
	length = strlen(title);
	if (length > 127U)
		return 0;

	/* Each character. */
	for (byte = (const unsigned char *)title; *byte != '\0'; byte += follow + 1U) {
		/* The first byte: the character's length and its first bits. */
		if (*byte < 0x80U) {
			follow = 0;
			code = *byte;
		} else if (*byte >= 0xc2U && *byte <= 0xdfU) {
			follow = 1;
			code = *byte & 0x1fU;
		} else if (*byte >= 0xe0U && *byte <= 0xefU) {
			follow = 2;
			code = *byte & 0x0fU;
		} else if (*byte >= 0xf0U && *byte <= 0xf4U) {
			follow = 3;
			code = *byte & 0x07U;
		} else {
			/* A continuation byte first, an overlong C0 or C1, or past F4. */
			return 0;
		}

		/* The bytes that follow it. */
		for (index = 1; index <= follow; index++) {
			if ((byte[index] & 0xc0U) != 0x80U)
				return 0;
			code = code << 6 | (byte[index] & 0x3fU);
		}

		/* An overlong three or four bytes, a surrogate, or past U+10FFFF. */
		if (follow == 2U && code < 0x800UL)
			return 0;
		if (follow == 3U && code < 0x10000UL)
			return 0;
		if (code >= 0xd800UL && code <= 0xdfffUL)
			return 0;
		if (code > 0x10ffffUL)
			return 0;

		/* A control character: C0, DEL or C1. */
		if (code < 0x20UL || (code >= 0x7fUL && code <= 0x9fUL))
			return 0;
	}

	/* Succeeded: the title keeps the rules. */
	return 1;
}

/*
 * Reads a string argument in place, at most bound bytes with its NUL;
 * *next is the offset after it.  Returns 0, or EPROTO.
 */
static int
printers_string(
	const unsigned char *bytes,
	size_t size,
	size_t offset,
	size_t bound,
	const char **text,
	size_t *next)
{
	const void *inner;
	uint32_t length;
	size_t padded;

	/* The length, with the NUL, within the request and the bound. */
	if (offset + 4U > size)
		return EPROTO;
	length = printers_word(bytes, offset);
	if (length == 0U || length > bound)
		return EPROTO;

	/* The bytes, padded to a word, within the request. */
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	if (offset + 4U + padded > size)
		return EPROTO;

	/* The text ends with its NUL and has no other. */
	if (bytes[offset + 4U + length - 1U] != '\0')
		return EPROTO;
	inner = memchr(bytes + offset + 4U, '\0', length - 1U);
	if (inner != NULL)
		return EPROTO;

	/* Succeeded: the text where it is. */
	*text = (const char *)(bytes + offset + 4U);
	*next = offset + 4U + padded;
	return 0;
}

/* Writes a string argument and reports the offset after it. */
static size_t
printers_put_string(
	unsigned char *payload,
	size_t offset,
	const char *text)
{
	uint32_t length;
	size_t padded;

	/* The length with the NUL, the bytes, and zeros to a four-byte boundary. */
	length = (uint32_t)strlen(text) + 1U;
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	memcpy(payload + offset, &length, sizeof(length));
	memset(payload + offset + 4U, 0, padded);
	memcpy(payload + offset + 4U, text, length - 1U);
	return offset + 4U + padded;
}

/* Reads a 32-bit word of a request in the wire's native byte order. */
static uint32_t
printers_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The word. */
	memcpy(&word, bytes + offset, sizeof(word));
	return word;
}

/*
 * Makes the backend's settings file the compositor's, once: its writes and
 * checks run on the file I/O thread, or in the event loop when the thread
 * does not run (the backend's own writer thread would not wake the loop
 * with what it wrote).
 */
static void
printers_host(void)
{
	struct kl_backend_print *backend;

	/* Once, with the backend open. */
	backend = printers_backend();
	if (backend == NULL || printers_state.hosted)
		return;

	/* Succeeded: the compositor writes and checks the file from now on. */
	kl_backend_print_host(backend);
	printers_state.hosted = 1;
}

/*
 * Gives the file I/O thread the settings file's writes of this pass: the
 * changes waiting written (one write given at a time).
 */
static void
printers_file(
	struct kwl_server *server)
{
	int wanted;
	int error;

	/* Only a hosted backend. */
	if (!printers_state.hosted)
		return;

	/* The changes waiting, when no write is under way. */
	wanted = kl_backend_print_write_wanted(printers_state.backend);
	if (!wanted || printers_state.writing)
		return;

	/*
	 * Under way until its done clears it, which a write run in the loop
	 * itself reaches before the give returns.
	 */
	printers_state.writing = 1;
	error = printers_file_give(server, NULL);
	if (error != 0)
		printers_state.writing = 0;
}

/*
 * Gives the file I/O thread the check of the settings file for another
 * session's change that was asked for, when no check is under way (one
 * asked meanwhile is given by the tick after it).
 */
static void
printers_check(
	struct kwl_server *server)
{
	struct kl_backend_print_check *check;
	int error;

	/* Only a hosted backend, a check asked for, one check at a time. */
	if (!printers_state.hosted || !printers_state.check_wanted || printers_state.checking)
		return;

	/* What the file was when last read; the ask is taken by this check. */
	printers_state.check_wanted = 0;
	check = kl_backend_print_check_begin(printers_state.backend);
	if (check == NULL)
		return;

	/*
	 * Under way until its done clears it, which a check run in the loop
	 * itself reaches before the give returns.  A check not given is freed
	 * without being applied (it read nothing).
	 */
	printers_state.checking = 1;
	error = printers_file_give(server, check);
	if (error != 0) {
		printers_state.checking = 0;
		kl_backend_print_check_end(NULL, check);
	}
}

/*
 * Gives the file I/O thread a write (check NULL) or a check of the
 * settings file; without the thread it runs in the loop here and is done
 * at once.  Returns 0, or ENOMEM.
 */
static int
printers_file_give(
	struct kwl_server *server,
	struct kl_backend_print_check *check)
{
	struct printers_file_job *job;
	int error;

	/* The record. */
	job = calloc(1U, sizeof(*job));
	if (job == NULL)
		return ENOMEM;
	job->backend = printers_state.backend;
	job->check = check;

	/* The thread's from here; without the thread it runs here and is taken as done (which frees the record). */
	error = kwl_io_file_submit(server, &job->lane, &printers_file_ops, 0U);
	if (error != 0) {
		printers_file_run(&job->lane);
		printers_file_done(server, &job->lane);
	}

	/* Succeeded: given to the thread, or written or checked in the loop. */
	return 0;
}

/* Writes the changes waiting, or checks the file, on the file I/O thread. */
static void
printers_file_run(
	struct kwl_lane_job *lane_job)
{
	struct printers_file_job *job;

	/* A check reads the file when it changed; a write makes the changes. */
	job = (struct printers_file_job *)lane_job;
	if (job->check != NULL)
		kl_backend_print_check_run(job->backend, job->check);
	else
		kl_backend_print_write(job->backend);
}

/*
 * Takes a write or a check that ran, on the event loop's thread: the
 * update takes the changes made and tells them, a table read is the
 * printers' (told at the next update).
 */
static void
printers_file_done(
	void *context,
	struct kwl_lane_job *lane_job)
{
	struct printers_file_job *job;

	UNUSED_PARAMETER(context);

	/* The next write or check may be given; a check's table applied. */
	job = (struct printers_file_job *)lane_job;
	if (job->check != NULL) {
		kl_backend_print_check_end(job->backend, job->check);
		printers_state.checking = 0;
	} else {
		printers_state.writing = 0;
	}

	/* The record. */
	free(job);
}

/* Frees a write or a check that did not run (the thread stopped at the end). */
static void
printers_file_drop(
	struct kwl_lane_job *lane_job)
{
	struct printers_file_job *job;

	/* A check's record, then the job's. */
	job = (struct printers_file_job *)lane_job;
	if (job->check != NULL)
		kl_backend_print_check_end(NULL, job->check);
	free(job);
}

/*
 * Ends the wait for a daemon shut for writing: the pass the timer woke
 * gives it up (kwl_printers_tick's update looks at its time).
 */
static void
printers_shut_fired(
	void *context,
	struct kwl_timer *timer)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(timer);
}
