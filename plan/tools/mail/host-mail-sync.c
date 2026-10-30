/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws169-p004: the host test of Mail's thread (userland/desktop/mailer/
 * sync.c) against fake-mail-server.py: a new account that works and one
 * whose password is wrong, Get Mail's messages of every folder, a new
 * message told while idling (with its sign-in code), a message sent and
 * kept in Sent, and a message moved to the archive.
 *
 *     host-mail-sync CA-FILE IMAPS SMTPS
 *
 * Prints "PASS name" or "FAIL name ..." for each check; exits with 1 when
 * one failed.
 */

#include "userland/desktop/mailer/sync.h"

#include <errno.h>
#include <keiland/keiland_thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* How long a step waits for its result, in ms. */
#define TEST_WAIT_MS		10000

/* The checks that failed. */
static int test_failures;

int main(int argc, char **argv);
static void test_check(const char *name, int passed, const char *detail);
static int test_wait_for(struct ml_sync *sync, enum ml_result_kind kind, int folder, int arrived, struct ml_result *found, unsigned *messages);

/*
 * Drives the thread and checks its results.
 */
int
main(
	int argc,
	char **argv)
{
	struct ml_account_config account;
	struct ml_result result;
	struct ml_sync *sync;
	struct ml_job job;
	char server[64];
	unsigned messages;
	int got;
	int error;

	/* The arguments. */
	if (argc != 4) {
		fprintf(stderr, "usage: host-mail-sync CA-FILE IMAPS SMTPS\n");
		return 2;
	}
	(void)ml_tls_add_ca_file(argv[1]);

	/* The account, its servers with TLS from the start. */
	memset(&account, 0, sizeof(account));
	(void)snprintf(account.name, sizeof(account.name), "Kei");
	(void)snprintf(account.address, sizeof(account.address), "kei@example.net");
	(void)snprintf(account.user, sizeof(account.user), "kei@example.net");
	(void)snprintf(account.password, sizeof(account.password), "wrong");
	(void)snprintf(server, sizeof(server), "localhost:%s", argv[2]);
	(void)ml_server_parse(server, 993U, &account.imap);
	account.imap.secure = 1;
	(void)snprintf(server, sizeof(server), "localhost:%s", argv[3]);
	(void)ml_server_parse(server, 465U, &account.smtp);
	account.smtp.secure = 1;

	/* The thread without accounts. */
	error = ml_sync_start(NULL, 0U, NULL, NULL, &sync);
	test_check("start", error == 0, "");
	if (error != 0)
		return 1;

	/* A wrong password: the sign-in fails for no account. */
	memset(&job, 0, sizeof(job));
	job.kind = ML_JOB_SIGN_IN;
	job.account = -1;
	job.config = account;
	(void)ml_sync_queue(sync, &job);
	got = test_wait_for(sync, ML_RESULT_FAILED, -1, -1, &result, NULL);
	test_check("sign-in-wrong", got && result.account == -1 && result.error == EACCES, result.text);

	/* The right one: account 0. */
	(void)snprintf(job.config.password, sizeof(job.config.password), "secret 1");
	(void)ml_sync_queue(sync, &job);
	got = test_wait_for(sync, ML_RESULT_SIGNED_IN, -1, -1, &result, NULL);
	test_check("sign-in", got && result.account == 0, "");

	/* Get Mail: the inbox's messages (three, or four when an IDLE came first), then done. */
	memset(&job, 0, sizeof(job));
	job.kind = ML_JOB_REFRESH;
	job.account = 0;
	(void)ml_sync_queue(sync, &job);
	messages = 0;
	got = test_wait_for(sync, ML_RESULT_REFRESHED, -1, -1, &result, &messages);
	test_check("refresh", got && messages >= 3U, "");

	/* Idling: a new message is told, with its code. */
	got = test_wait_for(sync, ML_RESULT_MESSAGE, ML_INBOX, 1, &result, NULL);
	test_check("arrived", got && strcmp(result.parsed.code, "7351") == 0 && (result.flags & ML_UNREAD) != 0U, result.parsed.code);
	if (got)
		ml_sync_release(&result);

	/* Sent, then its copy got from Sent. */
	memset(&job, 0, sizeof(job));
	job.kind = ML_JOB_SEND;
	job.account = 0;
	(void)snprintf(job.receivers, sizeof(job.receivers), "Ben <ben@example.com>, ");
	error = ml_compose(&account, "Ben <ben@example.com>", "", "Hi", "Hello", "", time(NULL), &job.raw, &job.length);
	test_check("compose", error == 0, "");
	(void)ml_sync_queue(sync, &job);
	got = test_wait_for(sync, ML_RESULT_SENT, -1, -1, &result, NULL);
	test_check("sent", got, "");
	got = test_wait_for(sync, ML_RESULT_MESSAGE, ML_SENT, 0, &result, NULL);
	test_check("sent-copy", got && strcmp(result.parsed.subject, "Hi") == 0, result.parsed.subject);
	if (got)
		ml_sync_release(&result);

	/* The bank's message (UID 10) moved to the archive: it comes back from there. */
	memset(&job, 0, sizeof(job));
	job.kind = ML_JOB_MOVE;
	job.account = 0;
	job.folder = ML_INBOX;
	job.uid = 10;
	job.to_folder = ML_ARCHIVE;
	(void)ml_sync_queue(sync, &job);
	got = test_wait_for(sync, ML_RESULT_MESSAGE, ML_ARCHIVE, 0, &result, NULL);
	test_check("moved", got && strcmp(result.parsed.code, "482913") == 0, result.parsed.subject);
	if (got)
		ml_sync_release(&result);

	/* Stopped (in the middle of an IDLE). */
	ml_sync_stop(sync);
	test_check("stop", 1, "");

	/* The outcome. */
	if (test_failures != 0) {
		printf("host-mail-sync: %d FAILED\n", test_failures);
		return 1;
	}
	printf("host-mail-sync: PASS\n");
	return 0;
}

/* Prints a check's outcome. */
static void
test_check(
	const char *name,
	int passed,
	const char *detail)
{
	/* Passed. */
	if (passed) {
		printf("PASS %s\n", name);
		return;
	}

	/* Failed, with what was seen. */
	printf("FAIL %s [%s]\n", name, detail);
	test_failures++;
}

/*
 * Waits for a result of a kind (a message also of a folder and, unless -1,
 * arrived or not), releasing the others (counting the messages); 1 with it
 * in found, 0 when it did not come in time.
 */
static int
test_wait_for(
	struct ml_sync *sync,
	enum ml_result_kind kind,
	int folder,
	int arrived,
	struct ml_result *found,
	unsigned *messages)
{
	struct ml_result result;
	int waited;
	int taken;

	/* Until it comes or the time is up. */
	memset(found, 0, sizeof(*found));
	for (waited = 0; waited < TEST_WAIT_MS; waited += 100) {
		/* Each result there. */
		for (;;) {
			taken = ml_sync_take(sync, &result);
			if (!taken)
				break;

			/* A message counted. */
			if (result.kind == ML_RESULT_MESSAGE && messages != NULL)
				(*messages)++;

			/* The one waited for. */
			if (result.kind == kind &&
			    (folder < 0 || (int)result.folder == folder) &&
			    (arrived < 0 || result.arrived == arrived)) {
				*found = result;
				return 1;
			}
			ml_sync_release(&result);
		}

		/* The next wake. */
		kl_thread_sleep(100U);
	}

	/* Not in time. */
	return 0;
}
