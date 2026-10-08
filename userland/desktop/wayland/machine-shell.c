/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The computer on the wire (ws188-p002, plan/ws188/phase001/phase.md
 * sections D1 and D3; libkeiland/system/kl-system-protocol.h's
 * kl_system_machine_v1): what Settings reads of the computer -- the
 * system's names, the file systems' sizes, the people's accounts and the
 * login screen's language, and the mounted file systems of the file
 * manager (ws188-p004) -- read by libkeiland-backend's machine area and
 * the language file when a client asks.
 *
 * A file system's size (a network mount) and the accounts (a directory
 * service) may wait, so a reading runs on a thread of its own, one at a
 * time; the queries that come during one wait for the next
 * (machine-wait.c), and a reading reads what its queries ask together.
 * Each reading's job is on the heap and its thread is detached: the event
 * loop looks at the job each pass and answers its queries when it is done.
 * A reading that has not ended in ten seconds is given up: its queries are
 * answered failed, and the thread frees the job itself when it ends at
 * last.  While a given-up thread is still there, a query is answered
 * failed at once, so that the threads cannot pile up behind a mount that
 * hangs.  The compositor's end gives up a reading the same way rather than
 * wait for it.
 */

#include "kwl.h"

#include "language.h"
#include "machine-wait.h"

#include "userland/desktop/libkeiland/system/kl-system-protocol.h"
#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A parameter a function does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* How long a reading may take before it is given up, in milliseconds. */
#define MACHINE_STALL_MS	10000U

/* The largest event this file sends (an about, or a user with its home), and an event's header. */
#define MACHINE_EVENT_MAX	512U
#define MACHINE_HEADER		8U

/* The longest word of the language file looked at. */
#define MACHINE_WORD_MAX	32U

/*
 * One reading: the parts it reads, what it read, and its two flags.  done
 * is set by the thread when it has read everything; given_up by the event
 * loop when it stopped waiting.  Both are under machine_lock: whichever
 * comes second decides who frees the job (the event loop after done, the
 * thread after given_up).  The results are the event loop's to read only
 * after it saw done.
 */
struct machine_job {
	uint32_t what;
	unsigned done;
	unsigned given_up;
	struct kl_backend_machine machine;
	struct kl_backend_filesystem filesystems[KL_BACKEND_FILESYSTEMS_MAX];
	size_t filesystem_count;
	struct kl_backend_user users[KL_BACKEND_USERS_MAX];
	size_t user_count;
	unsigned skipped;
	char language[KL_SYSTEM_MACHINE_CODE_MAX];
	struct kl_backend_mount mounts[KL_BACKEND_MOUNTS_MAX];
	size_t mount_count;
	unsigned mounts_skipped;
	int mounts_error;
};

/*
 * The computer's readings of the compositor: the queries waiting, the
 * reading under way (NULL when none) and when it began.  The event loop's
 * alone.
 */
static struct {
	struct kwl_machine_wait wait;
	struct machine_job *job;
	uint64_t started_ms;
} machine_state;

/*
 * Guards each job's done and given_up and machine_given_up, the number of
 * given-up threads not ended yet.  It is static and never destroyed, since
 * a given-up thread may take it after the compositor's end began.
 */
static pthread_mutex_t machine_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned machine_given_up;

static void *machine_run(void *argument);
static void machine_begin(struct kwl_server *server);
static void machine_answer_all(struct kwl_server *server, const struct machine_job *job, uint32_t applied);
static void machine_answer(struct kwl_server *server, const struct kwl_machine_query *query, const struct machine_job *job, uint32_t applied);
static size_t machine_answer_bytes(const struct machine_job *job, uint32_t what);
static int machine_send(struct kwl_client *client, uint32_t id, const struct machine_job *job, uint32_t what, uint32_t request);
static void machine_result(struct kwl_client *client, uint32_t id, uint32_t request, uint32_t applied);
static void machine_language_code(const char *word, char *code, size_t size);
static size_t machine_put_word(unsigned char *payload, size_t offset, uint32_t word);
static size_t machine_put_string(unsigned char *payload, size_t offset, const char *text);
static size_t machine_string_bytes(const char *text);
static uint32_t machine_word(const unsigned char *bytes, size_t offset);

/*
 * Makes a machine object for a manager's get_machine (new id).  It has no
 * first state.  Returns 0, or EPROTO for a malformed request.
 */
int
kwl_machine_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;
	uint32_t id;

	/* The new object's ID. */
	if (size != 4U)
		return EPROTO;
	id = machine_word(bytes, 0U);

	/* The object, under the ID the client chose. */
	created = kwl_create(manager->client, id, KWL_SYSTEM_MACHINE, manager->version);
	if (created == NULL)
		return EPROTO;
	printf("KWL SYSTEM machine object client=%llu id=%u\n", (unsigned long long)manager->client->number, id);

	/* Succeeded: the object is the client's. */
	return 0;
}

/*
 * Carries out a request of a machine object: it goes, or it asks for a
 * reading.  Returns 0, or EPROTO for a malformed request.
 */
int
kwl_machine_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	uint32_t request;
	uint32_t what;
	uint32_t known;
	unsigned given_up;
	int error;

	/* The object goes (its queries with it, kwl_machine_gone). */
	if (opcode == KL_SYSTEM_MACHINE_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	}

	/* Anything but a query is not of this interface. */
	if (opcode != KL_SYSTEM_MACHINE_QUERY || size != 8U)
		return EPROTO;

	/* The request's number and the parts it asks. */
	request = machine_word(bytes, 0U);
	what = machine_word(bytes, 4U);
	printf("KWL SYSTEM machine query client=%llu request=%u what=%u\n", (unsigned long long)object->client->number, request, what);

	/* No part, or a part this object's version does not have (the mounts since 22, ws188-p004). */
	known = KL_SYSTEM_MACHINE_PARTS_21;
	if (object->version >= KL_SYSTEM_SINCE_MOUNTS)
		known = KL_SYSTEM_MACHINE_PARTS;
	if (what == 0U || (what & ~known) != 0U) {
		machine_result(object->client, object->id, request, KL_SYSTEM_RESULT_INVALID);
		return 0;
	}

	/* A given-up reading still hangs: no other is started behind it. */
	(void)pthread_mutex_lock(&machine_lock);
	given_up = machine_given_up;
	(void)pthread_mutex_unlock(&machine_lock);
	if (given_up != 0U) {
		machine_result(object->client, object->id, request, KL_SYSTEM_RESULT_FAILED);
		return 0;
	}

	/* The query waits for a reading; too many waiting is busy. */
	error = kwl_machine_wait_add(&machine_state.wait, object->client->number, object->id, request, what);
	if (error != 0) {
		machine_result(object->client, object->id, request, KL_SYSTEM_RESULT_BUSY);
		return 0;
	}

	/* A reading starts now when none is under way. */
	machine_begin(object->client->server);

	/* Succeeded: the answer comes when the reading ends. */
	return 0;
}

/*
 * Answers the queries of a reading that ended, gives up one that took too
 * long, and starts the next.
 */
void
kwl_machine_tick(
	struct kwl_server *server)
{
	struct machine_job *job;
	uint64_t now;
	unsigned done;

	/* No reading: one starts when a query waits. */
	job = machine_state.job;
	if (job == NULL) {
		machine_begin(server);
		return;
	}

	/* Whether the thread has read everything. */
	(void)pthread_mutex_lock(&machine_lock);
	done = job->done;
	(void)pthread_mutex_unlock(&machine_lock);

	/* Done: its queries are answered, the job goes, and the next reading starts. */
	if (done) {
		machine_answer_all(server, job, KL_SYSTEM_RESULT_OK);
		machine_state.job = NULL;
		free(job);
		machine_begin(server);
		return;
	}

	/* Still within its time. */
	now = kwl_milliseconds();
	if (now - machine_state.started_ms < MACHINE_STALL_MS)
		return;

	/*
	 * Given up: the thread frees the job when it ends (the job is not
	 * touched after the lock is let go); until then no reading starts.
	 */
	printf("KWL SYSTEM machine stalled what=%u\n", job->what);
	(void)pthread_mutex_lock(&machine_lock);
	job->given_up = 1U;
	machine_given_up++;
	(void)pthread_mutex_unlock(&machine_lock);
	machine_state.job = NULL;

	/* Its queries and those waiting behind it are answered failed. */
	machine_answer_all(server, NULL, KL_SYSTEM_RESULT_FAILED);
	(void)kwl_machine_wait_start(&machine_state.wait);
	machine_answer_all(server, NULL, KL_SYSTEM_RESULT_FAILED);
}

/*
 * Forgets the queries of a machine object that went.
 */
void
kwl_machine_gone(
	struct kwl_object *object)
{
	/* Its queries, taken by a reading or not. */
	kwl_machine_wait_drop(&machine_state.wait, object->client->number, object->id);
}

/*
 * Lets go of a reading under way at the compositor's end, without waiting
 * for it (a mount that hangs must not keep the session from ending).
 */
void
kwl_machine_close(
	struct kwl_server *server)
{
	struct machine_job *job;

	UNUSED_PARAMETER(server);

	/* No reading. */
	job = machine_state.job;
	if (job == NULL)
		return;

	/* A finished one goes; one still reading frees itself when it ends. */
	(void)pthread_mutex_lock(&machine_lock);
	if (job->done) {
		(void)pthread_mutex_unlock(&machine_lock);
		free(job);
	} else {
		job->given_up = 1U;
		machine_given_up++;
		(void)pthread_mutex_unlock(&machine_lock);
	}

	/* Nothing is under way any more. */
	machine_state.job = NULL;
}

/* Starts a reading of the waiting queries when none is under way. */
static void
machine_begin(
	struct kwl_server *server)
{
	struct machine_job *job;
	pthread_attr_t attributes;
	pthread_t thread;
	uint32_t what;
	int error;

	/* One reading at a time. */
	if (machine_state.job != NULL)
		return;

	/* The waiting queries, taken by the new reading; none needs no reading. */
	what = kwl_machine_wait_start(&machine_state.wait);
	if (what == 0U)
		return;

	/* The job. */
	job = calloc(1, sizeof(*job));
	if (job == NULL) {
		machine_answer_all(server, NULL, KL_SYSTEM_RESULT_FAILED);
		return;
	}
	job->what = what;

	/* The thread, detached: nobody joins it, the job tells its end. */
	error = pthread_attr_init(&attributes);
	if (error == 0) {
		(void)pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
		error = pthread_create(&thread, &attributes, machine_run, job);
		(void)pthread_attr_destroy(&attributes);
	}

	/* No thread: the queries are answered failed. */
	if (error != 0) {
		printf("KWL SYSTEM machine thread error=%d\n", error);
		free(job);
		machine_answer_all(server, NULL, KL_SYSTEM_RESULT_FAILED);
		return;
	}

	/* The reading is under way from now. */
	machine_state.job = job;
	machine_state.started_ms = kwl_milliseconds();
}

/* The reading's thread: reads the parts asked, then says so (or frees the job when it was given up). */
static void *
machine_run(
	void *argument)
{
	struct machine_job *job;
	char word[MACHINE_WORD_MAX];
	int error;

	/* The job the event loop made. */
	job = argument;

	/* The system's names. */
	if ((job->what & KL_SYSTEM_MACHINE_ABOUT) != 0U)
		(void)kl_backend_machine_read(&job->machine);

	/* The file systems' sizes. */
	if ((job->what & KL_SYSTEM_MACHINE_FILESYSTEMS) != 0U)
		job->filesystem_count = kl_backend_filesystems_read(job->filesystems, KL_BACKEND_FILESYSTEMS_MAX);

	/* The people's accounts and the own user's. */
	if ((job->what & KL_SYSTEM_MACHINE_USERS) != 0U)
		job->user_count = kl_backend_users_read(job->users, KL_BACKEND_USERS_MAX, &job->skipped);

	/* The login screen's language, as Settings shows it. */
	if ((job->what & KL_SYSTEM_MACHINE_LOGIN_LANGUAGE) != 0U) {
		error = kwl_language_system_word(word, sizeof(word));
		if (error != 0)
			word[0] = '\0';
		machine_language_code(word, job->language, sizeof(job->language));
	}

	/* The mounted file systems of files (ws188-p004). */
	if ((job->what & KL_SYSTEM_MACHINE_MOUNTS) != 0U)
		job->mounts_error = kl_backend_mounts_read(job->mounts, KL_BACKEND_MOUNTS_MAX, &job->mount_count, &job->mounts_skipped);

	/* Done; a job given up is this thread's to free. */
	(void)pthread_mutex_lock(&machine_lock);
	if (job->given_up) {
		machine_given_up--;
		(void)pthread_mutex_unlock(&machine_lock);
		free(job);
		return NULL;
	}
	job->done = 1U;
	(void)pthread_mutex_unlock(&machine_lock);

	/* Succeeded: the event loop answers from the job. */
	return NULL;
}

/* Answers every query the reading took: with the job's parts (ok) or a failure (no job). */
static void
machine_answer_all(
	struct kwl_server *server,
	const struct machine_job *job,
	uint32_t applied)
{
	struct kwl_machine_query query;
	int taken;

	/* Each query, the oldest first. */
	for (;;) {
		taken = kwl_machine_wait_take(&machine_state.wait, &query);
		if (!taken)
			break;
		machine_answer(server, &query, job, applied);
	}
}

/* Answers one query: its parts and an ok, or a failure alone. */
static void
machine_answer(
	struct kwl_server *server,
	const struct kwl_machine_query *query,
	const struct machine_job *job,
	uint32_t applied)
{
	struct kwl_client *client;
	struct kwl_object *object;
	size_t needed;
	int error;

	/* Its client, still there. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->number == query->client && !client->fatal)
			break;
	}

	/* The client went. */
	if (client == NULL)
		return;

	/* Its object, still a machine object (one that went dropped its queries). */
	object = kwl_find(client, query->object);
	if (object == NULL || object->dead || object->kind != KWL_SYSTEM_MACHINE)
		return;

	/* A failure is the result alone. */
	if (applied != KL_SYSTEM_RESULT_OK || job == NULL) {
		machine_result(client, query->object, query->request, applied);
		printf("KWL SYSTEM machine answer client=%llu request=%u what=%u result=%u\n", (unsigned long long)client->number, query->request, query->what, applied);
		return;
	}

	/* A mount table that could not be read fails the query that asked it (the client keeps the mounts it had). */
	if ((query->what & KL_SYSTEM_MACHINE_MOUNTS) != 0U && job->mounts_error != 0) {
		machine_result(client, query->object, query->request, KL_SYSTEM_RESULT_FAILED);
		printf("KWL SYSTEM machine answer client=%llu request=%u what=%u result=%u mounts_error=%d\n", (unsigned long long)client->number, query->request, query->what, (unsigned)KL_SYSTEM_RESULT_FAILED, job->mounts_error);
		return;
	}

	/* The whole answer, or none: a client without room for it hears unavailable. */
	needed = machine_answer_bytes(job, query->what);
	if (client->output_bytes + needed > KWL_OUTPUT_MAX) {
		machine_result(client, query->object, query->request, KL_SYSTEM_RESULT_UNAVAILABLE);
		printf("KWL SYSTEM machine answer client=%llu request=%u what=%u result=%u\n", (unsigned long long)client->number, query->request, query->what, (unsigned)KL_SYSTEM_RESULT_UNAVAILABLE);
		return;
	}

	/* The parts and the ok. */
	error = machine_send(client, query->object, job, query->what, query->request);
	printf("KWL SYSTEM machine answer client=%llu request=%u what=%u result=%d users=%u filesystems=%u skipped=%u mounts=%u mounts_skipped=%u\n",
	       (unsigned long long)client->number, query->request, query->what, error, (unsigned)job->user_count,
	       (unsigned)job->filesystem_count, job->skipped, (unsigned)job->mount_count, job->mounts_skipped);
}

/* Counts the bytes an answer of some parts takes on the wire, the headers included. */
static size_t
machine_answer_bytes(
	const struct machine_job *job,
	uint32_t what)
{
	size_t bytes;
	size_t index;

	/* The parts event and the result. */
	bytes = MACHINE_HEADER + 8U + MACHINE_HEADER + 12U;

	/* The system's names. */
	if ((what & KL_SYSTEM_MACHINE_ABOUT) != 0U) {
		bytes += MACHINE_HEADER + 4U;
		bytes += machine_string_bytes(job->machine.system);
		bytes += machine_string_bytes(job->machine.kernel);
		bytes += machine_string_bytes(job->machine.architecture);
		bytes += machine_string_bytes(job->machine.processor);
		bytes += machine_string_bytes(job->machine.host);
	}

	/* Each file system: its path and six words. */
	if ((what & KL_SYSTEM_MACHINE_FILESYSTEMS) != 0U) {
		for (index = 0; index < job->filesystem_count; index++)
			bytes += MACHINE_HEADER + machine_string_bytes(job->filesystems[index].path) + 24U;
	}

	/* Each account: three strings and its flags. */
	if ((what & KL_SYSTEM_MACHINE_USERS) != 0U) {
		for (index = 0; index < job->user_count; index++) {
			bytes += MACHINE_HEADER + 4U;
			bytes += machine_string_bytes(job->users[index].name);
			bytes += machine_string_bytes(job->users[index].full_name);
			bytes += machine_string_bytes(job->users[index].home);
		}
	}

	/* The language's code. */
	if ((what & KL_SYSTEM_MACHINE_LOGIN_LANGUAGE) != 0U)
		bytes += MACHINE_HEADER + machine_string_bytes(job->language);

	/* Each mount: its path and its type. */
	if ((what & KL_SYSTEM_MACHINE_MOUNTS) != 0U) {
		for (index = 0; index < job->mount_count; index++) {
			bytes += MACHINE_HEADER;
			bytes += machine_string_bytes(job->mounts[index].path);
			bytes += machine_string_bytes(job->mounts[index].type);
		}
	}

	/* Succeeded: the answer's size. */
	return bytes;
}

/*
 * Sends an answer: parts, the events of the parts asked, and the ok.
 * Returns 0, or the error of an event that could not be queued (the client
 * is then failing as a whole).
 */
static int
machine_send(
	struct kwl_client *client,
	uint32_t id,
	const struct machine_job *job,
	uint32_t what,
	uint32_t request)
{
	unsigned char payload[MACHINE_EVENT_MAX];
	const struct kl_backend_filesystem *filesystem;
	const struct kl_backend_user *user;
	size_t offset;
	size_t index;
	int error;

	/* The answer's start: its request and parts. */
	offset = machine_put_word(payload, 0U, request);
	offset = machine_put_word(payload, offset, what);
	error = kwl_emit(client, id, KL_SYSTEM_MACHINE_EVENT_PARTS, payload, offset);
	if (error != 0)
		return error;

	/* The system's names. */
	if ((what & KL_SYSTEM_MACHINE_ABOUT) != 0U) {
		offset = machine_put_string(payload, 0U, job->machine.system);
		offset = machine_put_string(payload, offset, job->machine.kernel);
		offset = machine_put_string(payload, offset, job->machine.architecture);
		offset = machine_put_string(payload, offset, job->machine.processor);
		offset = machine_put_string(payload, offset, job->machine.host);
		offset = machine_put_word(payload, offset, job->machine.cpus);
		error = kwl_emit(client, id, KL_SYSTEM_MACHINE_EVENT_ABOUT, payload, offset);
		if (error != 0)
			return error;
	}

	/* Each file system, its sizes as their halves. */
	for (index = 0; (what & KL_SYSTEM_MACHINE_FILESYSTEMS) != 0U && index < job->filesystem_count; index++) {
		filesystem = &job->filesystems[index];
		offset = machine_put_string(payload, 0U, filesystem->path);
		offset = machine_put_word(payload, offset, (uint32_t)(filesystem->total >> 32));
		offset = machine_put_word(payload, offset, (uint32_t)filesystem->total);
		offset = machine_put_word(payload, offset, (uint32_t)(filesystem->available >> 32));
		offset = machine_put_word(payload, offset, (uint32_t)filesystem->available);
		offset = machine_put_word(payload, offset, (uint32_t)(filesystem->used >> 32));
		offset = machine_put_word(payload, offset, (uint32_t)filesystem->used);
		error = kwl_emit(client, id, KL_SYSTEM_MACHINE_EVENT_FILESYSTEM, payload, offset);
		if (error != 0)
			return error;
	}

	/* Each account. */
	for (index = 0; (what & KL_SYSTEM_MACHINE_USERS) != 0U && index < job->user_count; index++) {
		user = &job->users[index];
		offset = machine_put_string(payload, 0U, user->name);
		offset = machine_put_string(payload, offset, user->full_name);
		offset = machine_put_string(payload, offset, user->home);
		offset = machine_put_word(payload, offset, user->flags);
		error = kwl_emit(client, id, KL_SYSTEM_MACHINE_EVENT_USER, payload, offset);
		if (error != 0)
			return error;
	}

	/* The login screen's language. */
	if ((what & KL_SYSTEM_MACHINE_LOGIN_LANGUAGE) != 0U) {
		offset = machine_put_string(payload, 0U, job->language);
		error = kwl_emit(client, id, KL_SYSTEM_MACHINE_EVENT_LOGIN_LANGUAGE, payload, offset);
		if (error != 0)
			return error;
	}

	/* Each mount (ws188-p004). */
	for (index = 0; (what & KL_SYSTEM_MACHINE_MOUNTS) != 0U && index < job->mount_count; index++) {
		offset = machine_put_string(payload, 0U, job->mounts[index].path);
		offset = machine_put_string(payload, offset, job->mounts[index].type);
		error = kwl_emit(client, id, KL_SYSTEM_MACHINE_EVENT_MOUNT, payload, offset);
		if (error != 0)
			return error;
	}

	/* The answer's end. */
	machine_result(client, id, request, KL_SYSTEM_RESULT_OK);

	/* Succeeded: the whole answer is queued. */
	return 0;
}

/* Answers a query: result(request, applied, saved 0). */
static void
machine_result(
	struct kwl_client *client,
	uint32_t id,
	uint32_t request,
	uint32_t applied)
{
	uint32_t words[3];

	/* request, applied, nothing saved. */
	words[0] = request;
	words[1] = applied;
	words[2] = 0U;
	(void)kwl_emit(client, id, KL_SYSTEM_MACHINE_EVENT_RESULT, words, sizeof(words));
}

/*
 * Writes the login screen's language as Settings shows it: the file's
 * first word when it is "en" or "ja", else "" (not set; the login screen
 * itself takes the line as it is, language.c).
 */
static void
machine_language_code(
	const char *word,
	char *code,
	size_t size)
{
	size_t length;
	int english;
	int japanese;

	/* Not set unless the first word is a language Settings knows. */
	code[0] = '\0';
	length = strcspn(word, " \t");

	/* English. */
	english = strncmp(word, "en", length);
	if (length == 2U && english == 0) {
		(void)snprintf(code, size, "%s", "en");
		return;
	}

	/* Japanese. */
	japanese = strncmp(word, "ja", length);
	if (length == 2U && japanese == 0)
		(void)snprintf(code, size, "%s", "ja");
}

/* Puts a word into an event's payload; returns the offset after it. */
static size_t
machine_put_word(
	unsigned char *payload,
	size_t offset,
	uint32_t word)
{
	/* The word, native-endian as the wire is. */
	memcpy(payload + offset, &word, sizeof(word));

	/* The offset after it. */
	return offset + sizeof(word);
}

/*
 * Puts a string into an event's payload: its length with the NUL, the
 * bytes, and zeros to a four-byte boundary.  The machine's texts are cut
 * to their fields already, so every event fits MACHINE_EVENT_MAX.
 */
static size_t
machine_put_string(
	unsigned char *payload,
	size_t offset,
	const char *text)
{
	uint32_t length;
	size_t padded;
	size_t bytes;

	/* The length with the NUL, the bytes and the padding. */
	bytes = strlen(text);
	length = (uint32_t)bytes + 1U;
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	memcpy(payload + offset, &length, sizeof(length));
	memset(payload + offset + 4U, 0, padded);
	memcpy(payload + offset + 4U, text, bytes);

	/* The offset after the string. */
	return offset + 4U + padded;
}

/* Counts the bytes a string takes in a payload. */
static size_t
machine_string_bytes(
	const char *text)
{
	size_t length;

	/* Its length word, its bytes with the NUL, padded. */
	length = strlen(text) + 1U;
	return 4U + ((length + 3U) & ~(size_t)3U);
}

/* Reads a word of a request. */
static uint32_t
machine_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The word. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: the word. */
	return word;
}
