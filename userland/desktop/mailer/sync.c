/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Mail's thread that talks to the servers (sync.h): the
 * window queues jobs (sign in, get mail, send, mark read, move), the
 * thread does them one by one on its own IMAP session for each account
 * and queues the results, and between jobs it waits in IDLE on every
 * account's inbox for new mail.
 *
 * Every account's folders are got first (the accounts the thread starts
 * with, and a new one right after its sign-in), so that what IDLE tells
 * after is new mail.
 *
 * The window wakes the thread through a waker (rings merge until the
 * thread's wait finds them; the thread then looks at its jobs); the
 * thread tells the window of a batch of results through the window's ready
 * function (the application posts itself an event).  The queues are lists
 * under one lock; a job's and a result's memory goes with it from one
 * thread to the other.
 *
 * A message of the trash is deleted for good, an account's
 * new settings are tried without being taken (the window starts the
 * thread again with them), a server that keeps the sent messages by
 * itself (Gmail) gets no copy appended to Sent, and a failure says in
 * words what failed on which server (and gives an untrusted
 * certificate's fingerprint for the user to trust).
 *
 * A message larger than ML_FETCH_BYTES is not read from the
 * start the fetch gave; after the fetch it is fetched again by its
 * structure (its header and its words alone, ml_imap_fetch_large).
 */

#include "sync.h"

#include <keiland/keiland_socket.h>
#include <keiland/keiland_thread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How many of the latest messages of a folder are got the first time. */
#define SYNC_LATEST		50U

/* How long one IDLE lasts before it is started again (RFC 2177 asks for less than 29 minutes), in ms. */
#define SYNC_IDLE_MS		(25 * 60 * 1000)

/* The most messages larger than ML_FETCH_BYTES a fetch keeps to fetch by their structure (the others are read from their start). */
#define SYNC_LARGE_MAX		16U

/* How long the thread waits before it tries an account that failed again, in ms. */
#define SYNC_RETRY_MS		(5 * 60 * 1000)

/*
 * One account as the thread keeps it: its settings, its session (open or
 * not), its folders' names on the server, which folder is selected (-1
 * none), whether it idles, the highest UID got of each folder, and when
 * an account that failed is tried again (0: at once).
 */
struct sync_account {
	struct ml_account_config config;
	struct ml_imap imap;
	int open;
	char folders[ML_FOLDERS][ML_MAILBOX_MAX];
	int selected;
	int idling;
	uint32_t last_uid[ML_FOLDERS];
	uint64_t retry_ms;
};

/*
 * The thread and its queues: the thread, its waker (to wake it among the
 * servers' sockets it waits for), the window's function told when results
 * wait (with its context) and whether it was told since the window last
 * found none, the lock over the queues, the flag and the stop flag, the
 * jobs and the results (first and last of each list), and the accounts
 * (the thread's own, in the window's order).
 */
struct ml_sync {
	struct kl_thread *thread;
	int started;
	struct kl_socket_waker *waker;
	int (*ready)(void *context);
	void *ready_context;
	int told;
	struct kl_mutex lock;
	int stopping;
	struct ml_job *jobs;
	struct ml_job *jobs_last;
	struct ml_result *results;
	struct ml_result *results_last;
	struct sync_account accounts[ML_ACCOUNTS_MAX];
	size_t account_count;
};

/* A message larger than ML_FETCH_BYTES, to fetch by its structure after the fetch: its UID, flags and size. */
struct sync_large {
	uint32_t uid;
	unsigned flags;
	size_t size;
};

/*
 * What a fetch's callback carries: the thread, the account and the
 * folder, whether the messages are new arrivals, how many were handed
 * on, and the large messages kept for after the fetch.
 */
struct sync_fetch {
	struct ml_sync *sync;
	int account;
	enum ml_folder folder;
	int arrived;
	unsigned count;
	struct sync_large large[SYNC_LARGE_MAX];
	size_t large_count;
};

static void sync_run(void *argument);
static int sync_wait(struct ml_sync *sync);
static void sync_do(struct ml_sync *sync, struct ml_job *job);
static int sync_open(struct ml_sync *sync, int account);
static void sync_close(struct ml_sync *sync, int account);
static int sync_select(struct ml_sync *sync, int account, enum ml_folder folder);
static void sync_refresh(struct ml_sync *sync, int account, enum ml_folder folder, int arrived);
static void sync_refresh_account(struct ml_sync *sync, int account);
static void sync_fetched(void *data, uint32_t uid, unsigned flags, size_t size, const char *raw, size_t length);
static void sync_fetch_large(struct sync_fetch *fetch);
static void sync_idle_all(struct ml_sync *sync);
static void sync_idle_stop_all(struct ml_sync *sync);
static void sync_check(struct ml_sync *sync, struct ml_job *job);
static void sync_send(struct ml_sync *sync, struct ml_job *job);
static void sync_failed(struct ml_sync *sync, int account, const char *what, int error, const char *words, const struct ml_server *server);
static void sync_reason(int error, const char *words, char *text, size_t size);
static struct ml_result *sync_result(enum ml_result_kind kind, int account);
static void sync_post(struct ml_sync *sync, struct ml_result *result);
static uint64_t sync_now_ms(void);

/*
 * Starts the thread with the accounts the window has; ready is called on
 * the thread, with context, when a result waits and the window was not told
 * since it last found none (the window then takes them with ml_sync_take
 * until none is left).  ready returns 0 when the window was told; otherwise
 * the next result tries again.  Returns 0 with the thread, or an errno value.
 */
int
ml_sync_start(
	const struct ml_account_config *accounts,
	size_t count,
	int (*ready)(void *context),
	void *context,
	struct ml_sync **made)
{
	struct kl_thread_options options;
	struct ml_sync *sync;
	size_t index;
	int status;

	/* The state. */
	*made = NULL;
	sync = calloc(1U, sizeof(*sync));
	if (sync == NULL)
		return ENOMEM;
	sync->ready = ready;
	sync->ready_context = context;

	/* The lock over the queues. */
	status = kl_mutex_init(&sync->lock);
	if (status != 0) {
		free(sync);
		return status;
	}

	/* The accounts. */
	for (index = 0; index < count && index < ML_ACCOUNTS_MAX; index++) {
		sync->accounts[index].config = accounts[index];
		sync->accounts[index].selected = -1;
	}

	/* How many were taken. */
	sync->account_count = index;

	/* The waker that wakes the thread. */
	status = kl_socket_waker_open(&sync->waker);
	if (status != 0) {
		ml_sync_stop(sync);
		return status;
	}

	/* The thread, by its name. */
	memset(&options, 0, sizeof(options));
	options.name = "mail-sync";
	options.run = sync_run;
	options.argument = sync;
	options.priority = KL_THREAD_PRIORITY_NORMAL;
	status = kl_thread_start(&options, &sync->thread);
	if (status != 0) {
		ml_sync_stop(sync);
		return status;
	}

	/* Publishes the running thread. */
	sync->started = 1;
	*made = sync;

	/* Succeeded: the thread runs. */
	return 0;
}

/*
 * Stops the thread (it ends after its job; an IDLE is ended) and frees
 * everything left.
 */
void
ml_sync_stop(
	struct ml_sync *sync)
{
	struct ml_result result;
	struct ml_job *job;
	int taken;

	/* Nothing started. */
	if (sync == NULL)
		return;

	/* Told to stop and woken, then waited for. */
	if (sync->started) {
		kl_mutex_lock(&sync->lock);

		sync->stopping = 1;

		kl_mutex_unlock(&sync->lock);

		/* Wakes the thread out of its wait and waits for it to end. */
		kl_socket_waker_ring(sync->waker);
		(void)kl_thread_join(sync->thread);
		sync->thread = NULL;
	}

	/* The jobs not done. */
	while (sync->jobs != NULL) {
		job = sync->jobs;
		sync->jobs = job->next;
		free(job->raw);
		free(job);
	}

	/* The results not taken. */
	for (;;) {
		taken = ml_sync_take(sync, &result);
		if (!taken)
			break;
		ml_sync_release(&result);
	}

	/* The waker and the state. */
	if (sync->waker != NULL)
		kl_socket_waker_close(sync->waker);
	kl_mutex_destroy(&sync->lock);
	free(sync);
}

/*
 * Queues a job (copied; its raw message is taken over) and wakes the
 * thread.  Returns 0 or ENOMEM.
 */
int
ml_sync_queue(
	struct ml_sync *sync,
	const struct ml_job *job)
{
	struct ml_job *queued;

	/* The copy. */
	queued = malloc(sizeof(*queued));
	if (queued == NULL)
		return ENOMEM;
	*queued = *job;
	queued->next = NULL;

	/* At the end of the list. */
	kl_mutex_lock(&sync->lock);

	if (sync->jobs_last != NULL)
		sync->jobs_last->next = queued;
	else
		sync->jobs = queued;
	sync->jobs_last = queued;

	kl_mutex_unlock(&sync->lock);

	/* The thread woken (rings merge until its wait finds them). */
	kl_socket_waker_ring(sync->waker);

	/* Succeeded: the job is queued. */
	return 0;
}

/*
 * Takes the oldest result: 1 with it (the caller releases it), 0 when none
 * waits.
 */
int
ml_sync_take(
	struct ml_sync *sync,
	struct ml_result *result)
{
	struct ml_result *first;

	/* The first result; none left means the window is to be told of the next one. */
	kl_mutex_lock(&sync->lock);

	first = sync->results;
	if (first != NULL) {
		sync->results = first->next;
		if (sync->results == NULL)
			sync->results_last = NULL;
	} else {
		sync->told = 0;
	}

	kl_mutex_unlock(&sync->lock);

	/* None. */
	if (first == NULL)
		return 0;

	/* Given to the caller, the list's node freed. */
	*result = *first;
	result->next = NULL;
	free(first);

	/* Succeeded: one result is handed over. */
	return 1;
}

/*
 * Frees what a result holds.
 */
void
ml_sync_release(
	struct ml_result *result)
{
	/* The message read. */
	ml_mime_release(&result->parsed);
}

/* The thread: each job, then the IDLE of every account until the next job. */
static void
sync_run(
	void *argument)
{
	struct ml_sync *sync;
	struct ml_job *job;
	size_t index;
	int stopping;

	/* The accounts' folders first. */
	sync = argument;
	for (index = 0; index < sync->account_count; index++)
		sync_refresh_account(sync, (int)index);

	/* Each round. */
	for (;;) {
		/* The next job, or the stop. */
		kl_mutex_lock(&sync->lock);

		stopping = sync->stopping;
		job = sync->jobs;
		if (job != NULL) {
			sync->jobs = job->next;
			if (sync->jobs == NULL)
				sync->jobs_last = NULL;
		}

		kl_mutex_unlock(&sync->lock);

		/* Stopped: the sessions end. */
		if (stopping)
			break;

		/* A job: done, the sessions out of IDLE first. */
		if (job != NULL) {
			sync_idle_stop_all(sync);
			sync_do(sync, job);
			free(job->raw);
			free(job);
			continue;
		}

		/* No job: every inbox idles until a job or new mail. */
		sync_idle_all(sync);
		(void)sync_wait(sync);
	}

	/* The sessions closed. */
	sync_idle_stop_all(sync);
	while (sync->account_count > 0U) {
		sync->account_count--;
		sync_close(sync, (int)sync->account_count);
	}
}

/* Waits for a job's byte or for a server to speak in IDLE; new mail is got at once. */
static int
sync_wait(
	struct ml_sync *sync)
{
	struct kl_socket_wait watched[1 + ML_ACCOUNTS_MAX];
	int accounts[1 + ML_ACCOUNTS_MAX];
	size_t count;
	size_t found;
	size_t index;
	int ready;
	int arrived;
	int error;
	int timeout;

	/* The waker, and each idling session. */
	memset(watched, 0, sizeof(watched));
	watched[0].waker = sync->waker;
	watched[0].events = KL_SOCKET_READABLE;
	count = 1;
	timeout = SYNC_IDLE_MS;
	for (index = 0; index < sync->account_count; index++) {
		/* An account not idling (or without its connection) is tried again later. */
		if (!sync->accounts[index].idling || sync->accounts[index].imap.conn.socket == NULL) {
			timeout = SYNC_RETRY_MS;
			continue;
		}

		/* Bytes already read need no wait. */
		ready = ml_conn_ready(&sync->accounts[index].imap.conn);
		if (ready)
			timeout = 0;

		/* Its socket. */
		watched[count].socket = sync->accounts[index].imap.conn.socket;
		watched[count].events = KL_SOCKET_READABLE;
		accounts[count] = (int)index;
		count++;
	}

	/* The wait; a ring is emptied by it (the jobs are in the list). */
	error = kl_socket_poll(watched, count, timeout, &found);
	if (error != 0)
		return error;

	/* A long IDLE is ended so that the next round starts it again. */
	if (found == 0U) {
		sync_idle_stop_all(sync);
		return 0;
	}

	/* Each session that spoke: new mail is got. */
	for (index = 1; index < count; index++) {
		/* Nothing from this one. */
		if ((watched[index].ready & (KL_SOCKET_READABLE | KL_SOCKET_FAILED)) == 0U)
			continue;

		/* What it said. */
		error = ml_imap_idle_take(&sync->accounts[accounts[index]].imap, &arrived);
		if (error != 0) {
			sync_failed(sync, accounts[index], "Lost the connection to", error, sync->accounts[accounts[index]].imap.error, &sync->accounts[accounts[index]].config.imap);
			sync_close(sync, accounts[index]);
			continue;
		}

		/* No new mail: the session keeps idling. */
		if (!arrived)
			continue;

		/* New mail: the session's IDLE is ended so that the inbox can be read. */
		error = ml_imap_idle_stop(&sync->accounts[accounts[index]].imap);
		sync->accounts[accounts[index]].idling = 0;
		if (error != 0) {
			sync_close(sync, accounts[index]);
			continue;
		}

		/* New mail: the inbox's new messages got. */
		sync_refresh(sync, accounts[index], ML_INBOX, 1);
	}

	/* Succeeded: the wait is over. */
	return 0;
}

/* Does one job. */
static void
sync_do(
	struct ml_sync *sync,
	struct ml_job *job)
{
	struct ml_result *result;
	struct sync_account *account;
	int error;

	/* A new account: its login and folders tried; taken when they work. */
	if (job->kind == ML_JOB_SIGN_IN) {
		if (sync->account_count == ML_ACCOUNTS_MAX) {
			sync_failed(sync, -1, "Cannot add", ENOSPC, "Mail has as many accounts as it can keep", &job->config.imap);
			return;
		}

		/* The account taken for the try. */
		account = &sync->accounts[sync->account_count];
		memset(account, 0, sizeof(*account));
		account->config = job->config;
		account->selected = -1;
		sync->account_count++;
		error = sync_open(sync, (int)sync->account_count - 1);
		if (error != 0) {
			sync_failed(sync, -1, "Cannot sign in to", error, account->imap.error, &account->config.imap);
			sync_close(sync, (int)sync->account_count - 1);
			sync->account_count--;
			return;
		}

		/* It works: told to the window. */
		result = sync_result(ML_RESULT_SIGNED_IN, (int)sync->account_count - 1);
		if (result != NULL)
			sync_post(sync, result);

		/* Its folders, before it idles. */
		sync_refresh_account(sync, (int)sync->account_count - 1);
		return;
	}

	/* An account's new settings: tried, not taken. */
	if (job->kind == ML_JOB_CHECK) {
		sync_check(sync, job);
		return;
	}

	/* The other jobs are an account's. */
	if (job->account < 0 || (size_t)job->account >= sync->account_count)
		return;
	account = &sync->accounts[job->account];

	/* Get mail: each folder's new messages. */
	if (job->kind == ML_JOB_REFRESH) {
		sync_refresh_account(sync, job->account);
		return;
	}

	/* Send: by SMTP, then a copy in Sent. */
	if (job->kind == ML_JOB_SEND) {
		sync_send(sync, job);
		return;
	}

	/* Read: \Seen on the server. */
	if (job->kind == ML_JOB_SEEN) {
		error = sync_select(sync, job->account, job->folder);
		if (error == 0)
			error = ml_imap_flag(&account->imap, job->uid, "\\Seen", 1);
		if (error != 0)
			sync_failed(sync, job->account, "Cannot mark the message read on", error, account->imap.error, &account->config.imap);
		return;
	}

	/* Move: to the folder, which is then got. */
	if (job->kind == ML_JOB_MOVE) {
		error = sync_select(sync, job->account, job->folder);
		if (error == 0 && account->folders[job->to_folder][0] == '\0')
			error = ENOENT;
		if (error == 0)
			error = ml_imap_move(&account->imap, job->uid, account->folders[job->to_folder]);
		if (error != 0) {
			sync_failed(sync, job->account, "Cannot move the message on", error, account->imap.error, &account->config.imap);
			return;
		}

		/* The folder it went to, got. */
		sync_refresh(sync, job->account, job->to_folder, 0);
		return;
	}

	/* Delete for good: a message of the trash (the window has hidden it already). */
	if (job->kind == ML_JOB_DELETE) {
		error = sync_select(sync, job->account, job->folder);
		if (error == 0)
			error = ml_imap_delete(&account->imap, job->uid);
		if (error != 0)
			sync_failed(sync, job->account, "Cannot delete the message on", error, account->imap.error, &account->config.imap);
	}
}

/* Tries an account's new settings on a session of their own, closed after; tells the window whether they work. */
static void
sync_check(
	struct ml_sync *sync,
	struct ml_job *job)
{
	static struct ml_imap trial;
	static char folders[ML_FOLDERS][ML_MAILBOX_MAX];
	struct ml_result *result;
	int error;

	/* The login. */
	error = ml_imap_open(&trial, &job->config);
	if (error != 0) {
		sync_failed(sync, -1, "Cannot sign in to", error, trial.error, &job->config.imap);
		return;
	}

	/* The folders. */
	error = ml_imap_folders(&trial, folders);
	ml_imap_close(&trial);
	if (error != 0) {
		sync_failed(sync, -1, "Cannot read the folders on", error, trial.error, &job->config.imap);
		return;
	}

	/* They work: told to the window, which takes them. */
	result = sync_result(ML_RESULT_CHECKED, job->account);
	if (result != NULL)
		sync_post(sync, result);
}

/* Sends a message by SMTP, then keeps a copy in Sent (a server that keeps one itself, Gmail, gets none) and gets Sent. */
static void
sync_send(
	struct ml_sync *sync,
	struct ml_job *job)
{
	const char *receivers[32];
	char addresses[32][ML_TEXT_MAX];
	char words[ML_TEXT_MAX];
	struct ml_result *result;
	struct sync_account *account;
	size_t count;
	size_t index;
	int error;

	/* The receivers. */
	account = &sync->accounts[job->account];
	error = ml_mime_address_list(job->receivers, addresses, 32U, &count);
	if (error == 0 && count == 0U)
		error = EINVAL;
	if (error != 0) {
		sync_failed(sync, job->account, "Cannot send through", error, "the receivers are not written right", &account->config.smtp);
		return;
	}

	/* Their addresses for SMTP. */
	for (index = 0; index < count; index++)
		receivers[index] = addresses[index];

	/* Sent. */
	words[0] = '\0';
	error = ml_smtp_send(&account->config, receivers, count, job->raw, job->length, words, sizeof(words));
	if (error != 0) {
		sync_failed(sync, job->account, "Cannot send through", error, words, &account->config.smtp);
		return;
	}

	/* Sent: told to the window. */
	result = sync_result(ML_RESULT_SENT, job->account);
	if (result != NULL)
		sync_post(sync, result);

	/* The session for the copy. */
	error = sync_open(sync, job->account);
	if (error != 0)
		return;

	/* A server without Sent keeps no copy. */
	if (account->folders[ML_SENT][0] == '\0')
		return;

	/* The copy appended, unless the server keeps one itself (it would be there twice). */
	if ((account->imap.capabilities & ML_IMAP_GMAIL) == 0U) {
		error = ml_imap_append(&account->imap, account->folders[ML_SENT], job->raw, job->length);
		if (error != 0)
			return;
	}

	/* Sent got, with the copy. */
	sync_refresh(sync, job->account, ML_SENT, 0);
}

/* Opens an account's session when it is not open: the login and the folders' names. */
static int
sync_open(
	struct ml_sync *sync,
	int account)
{
	struct sync_account *kept;
	int error;

	/* Already open. */
	kept = &sync->accounts[account];
	if (kept->open)
		return 0;

	/* The login. */
	error = ml_imap_open(&kept->imap, &kept->config);
	if (error != 0) {
		kept->retry_ms = sync_now_ms() + SYNC_RETRY_MS;
		return error;
	}

	/* Open, nothing selected yet. */
	kept->open = 1;
	kept->selected = -1;

	/* The folders. */
	error = ml_imap_folders(&kept->imap, kept->folders);
	if (error != 0) {
		sync_close(sync, account);
		return error;
	}

	/* Succeeded: the session is open. */
	kept->retry_ms = 0;
	return 0;
}

/* Closes an account's session. */
static void
sync_close(
	struct ml_sync *sync,
	int account)
{
	struct sync_account *kept;

	/* An open one, logged out. */
	kept = &sync->accounts[account];
	if (kept->open)
		ml_imap_close(&kept->imap);
	kept->open = 0;
	kept->idling = 0;
	kept->selected = -1;
}

/* Selects an account's folder when it is not the selected one. */
static int
sync_select(
	struct ml_sync *sync,
	int account,
	enum ml_folder folder)
{
	struct sync_account *kept;
	uint32_t exists;
	int error;

	/* The session. */
	error = sync_open(sync, account);
	if (error != 0)
		return error;
	kept = &sync->accounts[account];

	/* Already selected. */
	if (kept->selected == (int)folder)
		return 0;

	/* A folder the server does not have. */
	if (kept->folders[folder][0] == '\0')
		return ENOENT;

	/* Selected. */
	error = ml_imap_select(&kept->imap, kept->folders[folder], &exists);
	if (error != 0) {
		kept->selected = -1;
		return error;
	}

	/* Succeeded: the folder is selected. */
	kept->selected = (int)folder;
	return 0;
}

/* Gets a folder's messages: the latest the first time, then those after the last UID got. */
static void
sync_refresh(
	struct ml_sync *sync,
	int account,
	enum ml_folder folder,
	int arrived)
{
	struct sync_fetch fetch;
	struct sync_account *kept;
	uint32_t first;
	int error;

	/* The folder selected (a folder the server does not have is skipped). */
	kept = &sync->accounts[account];
	error = sync_select(sync, account, folder);
	if (error == ENOENT && kept->open)
		return;
	if (error != 0) {
		sync_failed(sync, account, "Cannot get mail from", error, kept->imap.error, &kept->config.imap);
		sync_close(sync, account);
		return;
	}

	/* The messages to the window (new arrivals only in a folder got before). */
	fetch.sync = sync;
	fetch.account = account;
	fetch.folder = folder;
	fetch.arrived = 0;
	if (arrived && kept->last_uid[folder] != 0U)
		fetch.arrived = 1;
	fetch.count = 0;
	fetch.large_count = 0;
	first = 0;
	if (kept->last_uid[folder] != 0U)
		first = kept->last_uid[folder] + 1U;
	error = ml_imap_fetch(&kept->imap, first, SYNC_LATEST, sync_fetched, &fetch);
	if (error != 0) {
		sync_failed(sync, account, "Cannot get mail from", error, kept->imap.error, &kept->config.imap);
		sync_close(sync, account);
		return;
	}

	/* The large ones, by their structure. */
	sync_fetch_large(&fetch);
}

/* Gets every folder of an account, then tells the window it is done. */
static void
sync_refresh_account(
	struct ml_sync *sync,
	int account)
{
	struct ml_result *result;
	int folder;

	/* Each folder. */
	for (folder = 0; folder < ML_FOLDERS; folder++)
		sync_refresh(sync, account, (enum ml_folder)folder, 0);

	/* Done. */
	result = sync_result(ML_RESULT_REFRESHED, account);
	if (result != NULL)
		sync_post(sync, result);
}

/* Hands a fetched message, read, to the window; notes its UID. */
static void
sync_fetched(
	void *data,
	uint32_t uid,
	unsigned flags,
	size_t size,
	const char *raw,
	size_t length)
{
	struct sync_fetch *fetch;
	struct ml_result *result;
	int error;

	/* The highest UID got of the folder. */
	fetch = data;
	if (uid > fetch->sync->accounts[fetch->account].last_uid[fetch->folder])
		fetch->sync->accounts[fetch->account].last_uid[fetch->folder] = uid;

	/* A large one waits for the fetch's end (only its start came). */
	if (size > ML_FETCH_BYTES && fetch->large_count < SYNC_LARGE_MAX) {
		fetch->large[fetch->large_count].uid = uid;
		fetch->large[fetch->large_count].flags = flags;
		fetch->large[fetch->large_count].size = size;
		fetch->large_count++;
		return;
	}

	/* The message, read. */
	result = sync_result(ML_RESULT_MESSAGE, fetch->account);
	if (result == NULL)
		return;
	result->folder = fetch->folder;
	result->uid = uid;
	result->flags = flags;
	result->size = size;
	result->arrived = fetch->arrived;
	error = ml_mime_parse(raw, length, &result->parsed);
	if (error != 0) {
		free(result);
		return;
	}

	/* To the window. */
	sync_post(fetch->sync, result);
	fetch->count++;
}

/* Fetches the large messages a fetch kept, each by its structure, and hands them to the window. */
static void
sync_fetch_large(
	struct sync_fetch *fetch)
{
	struct sync_account *kept;
	struct ml_result *result;
	size_t index;
	int error;

	/* Each large message. */
	kept = &fetch->sync->accounts[fetch->account];
	for (index = 0; index < fetch->large_count; index++) {
		/* Its result. */
		result = sync_result(ML_RESULT_MESSAGE, fetch->account);
		if (result == NULL)
			return;
		result->folder = fetch->folder;
		result->uid = fetch->large[index].uid;
		result->flags = fetch->large[index].flags;
		result->size = fetch->large[index].size;
		result->arrived = fetch->arrived;

		/* Its header and words. */
		error = ml_imap_fetch_large(&kept->imap, fetch->large[index].uid, &result->parsed);
		if (error != 0) {
			ml_mime_release(&result->parsed);
			free(result);
			sync_failed(fetch->sync, fetch->account, "Cannot get a large message from", error, kept->imap.error, &kept->config.imap);
			return;
		}

		/* To the window. */
		sync_post(fetch->sync, result);
		fetch->count++;
	}
}

/* Starts IDLE on every account's inbox that is not idling (an account that failed only after its retry time). */
static void
sync_idle_all(
	struct ml_sync *sync)
{
	struct sync_account *kept;
	uint64_t now;
	size_t index;
	int error;

	/* Each account. */
	now = sync_now_ms();
	for (index = 0; index < sync->account_count; index++) {
		kept = &sync->accounts[index];
		if (kept->idling)
			continue;

		/* An account that failed waits for its time. */
		if (!kept->open && kept->retry_ms != 0U && now < kept->retry_ms)
			continue;

		/* The inbox selected (and new mail got since), then IDLE. */
		error = sync_select(sync, (int)index, ML_INBOX);
		if (error != 0) {
			kept->retry_ms = now + SYNC_RETRY_MS;
			continue;
		}

		/* The waiting started. */
		error = ml_imap_idle_start(&kept->imap);
		if (error != 0) {
			sync_close(sync, (int)index);
			kept->retry_ms = now + SYNC_RETRY_MS;
			continue;
		}

		/* Idling until a job or new mail. */
		kept->idling = 1;
	}
}

/* Ends every IDLE. */
static void
sync_idle_stop_all(
	struct ml_sync *sync)
{
	size_t index;
	int error;

	/* Each idling account. */
	for (index = 0; index < sync->account_count; index++) {
		if (!sync->accounts[index].idling)
			continue;
		sync->accounts[index].idling = 0;
		error = ml_imap_idle_stop(&sync->accounts[index].imap);
		if (error != 0)
			sync_close(sync, (int)index);
	}
}

/*
 * Tells the window that something failed, in words: what could not be
 * done on which server, and why (the server's or OpenSSL's words, or the
 * failure's meaning).  An untrusted certificate's fingerprint goes with
 * it, and whether the server is the account's SMTP one.
 */
static void
sync_failed(
	struct ml_sync *sync,
	int account,
	const char *what,
	int error,
	const char *words,
	const struct ml_server *server)
{
	struct ml_result *result;
	char reason[ML_TEXT_MAX];

	/* The result. */
	result = sync_result(ML_RESULT_FAILED, account);
	if (result == NULL)
		return;
	result->error = error;

	/* Why, in words. */
	sync_reason(error, words, reason, sizeof(reason));
	(void)snprintf(result->text, sizeof(result->text), "%s %.80s: %.150s.", what, server->host, reason);

	/* The server: the account's SMTP one, or its IMAP one (a new account's is always IMAP). */
	(void)snprintf(result->host, sizeof(result->host), "%s", server->host);
	if (account >= 0 && server == &sync->accounts[account].config.smtp)
		result->smtp = 1;

	/* The certificate to trust. */
	if (error == ML_ERROR_UNTRUSTED)
		(void)snprintf(result->fingerprint, sizeof(result->fingerprint), "%s", ml_tls_fingerprint());

	/* To the window. */
	sync_post(sync, result);
}

/* Says why something failed: the server's or OpenSSL's words when there are any, else the failure's meaning. */
static void
sync_reason(
	int error,
	const char *words,
	char *text,
	size_t size)
{
	const char *meaning;

	/* An untrusted certificate, with OpenSSL's words. */
	if (error == ML_ERROR_UNTRUSTED) {
		(void)snprintf(text, size, "its certificate is not trusted (%.120s)", words);
		return;
	}

	/* The words of the server or of OpenSSL. */
	if (words != NULL && words[0] != '\0') {
		(void)snprintf(text, size, "%s", words);
		return;
	}

	/* The meaning of the failure. */
	switch (error) {
	case ML_ERROR_NO_TLS:
		meaning = "the server does not offer TLS, so the password is not sent";
		break;
	case ENOENT:
		meaning = "the server's name was not found";
		break;
	case ETIMEDOUT:
		meaning = "the server did not answer in time";
		break;
	case ECONNREFUSED:
		meaning = "the server refused the connection";
		break;
	case ENETUNREACH:
	case EHOSTUNREACH:
		meaning = "the network cannot reach the server";
		break;
	case EPIPE:
	case ECONNRESET:
		meaning = "the server closed the connection";
		break;
	case EPROTONOSUPPORT:
		meaning = "TLS is not available (the OpenSSL package is not installed)";
		break;
	case EACCES:
		meaning = "the server refused the user name or the password";
		break;
	default:
		meaning = NULL;
		break;
	}

	/* A failure without a meaning of its own, by its number. */
	if (meaning == NULL) {
		(void)snprintf(text, size, "it failed (%d)", error);
		return;
	}

	/* The meaning. */
	(void)snprintf(text, size, "%s", meaning);
}

/* Makes a result of a kind for an account (NULL without memory). */
static struct ml_result *
sync_result(
	enum ml_result_kind kind,
	int account)
{
	struct ml_result *result;

	/* Nothing in it yet. */
	result = calloc(1U, sizeof(*result));
	if (result == NULL)
		return NULL;
	result->kind = kind;
	result->account = account;
	return result;
}

/* Queues a result, and tells the window unless it was told already. */
static void
sync_post(
	struct ml_sync *sync,
	struct ml_result *result)
{
	int tell;
	int error;

	/* At the end of the list; the window is to be told when it was not. */
	kl_mutex_lock(&sync->lock);

	result->next = NULL;
	if (sync->results_last != NULL)
		sync->results_last->next = result;
	else
		sync->results = result;
	sync->results_last = result;
	tell = 0;
	if (!sync->told && sync->ready != NULL) {
		sync->told = 1;
		tell = 1;
	}

	kl_mutex_unlock(&sync->lock);

	/* Not to be told. */
	if (!tell)
		return;

	/* The window told; when it cannot be, the next result tries again. */
	error = sync->ready(sync->ready_context);
	if (error != 0) {
		kl_mutex_lock(&sync->lock);

		sync->told = 0;

		kl_mutex_unlock(&sync->lock);
	}
}

/* Reports a monotonic time in ms. */
static uint64_t
sync_now_ms(void)
{
	struct timespec now;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
