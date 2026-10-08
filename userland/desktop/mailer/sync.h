/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Mail's thread that talks to the servers (WS169 p004; sync.c): the jobs
 * the window queues and the results it takes.
 */

#ifndef MAILER_SYNC_H
#define MAILER_SYNC_H

#include "mail.h"

#include <stddef.h>
#include <stdint.h>

/* What a job asks. */
enum ml_job_kind {
	ML_JOB_SIGN_IN,		/* a new account: its login and folders tried (config) */
	ML_JOB_REFRESH,		/* an account's folders' new messages got */
	ML_JOB_SEND,		/* a written message sent (receivers, raw) and kept in Sent */
	ML_JOB_SEEN,		/* a message marked read (folder, uid) */
	ML_JOB_MOVE,		/* a message moved (folder, uid, to_folder) */
	ML_JOB_DELETE,		/* a message of the trash deleted for good (folder, uid; ws177-p015) */
	ML_JOB_CHECK		/* an account's new settings tried (config), not taken (ws177-p015) */
};

/*
 * One job: its kind, the account (an index of the window's and the
 * thread's), a new account's settings, the folder and UID of the message,
 * the folder it goes to, the receivers of a message to send (as the user
 * wrote them, To and Cc joined), and its bytes (allocated, freed with the
 * job).  next links the queue.
 */
struct ml_job {
	enum ml_job_kind kind;
	int account;
	struct ml_account_config config;
	enum ml_folder folder;
	enum ml_folder to_folder;
	uint32_t uid;
	char receivers[ML_LIST_MAX * 2U];
	char *raw;
	size_t length;
	struct ml_job *next;
};

/* What a result tells. */
enum ml_result_kind {
	ML_RESULT_SIGNED_IN,	/* the new account works (its index) */
	ML_RESULT_MESSAGE,	/* a message got (folder, uid, flags, size, parsed; arrived: new in IDLE) */
	ML_RESULT_REFRESHED,	/* an account's Get Mail is done */
	ML_RESULT_SENT,		/* a message was sent */
	ML_RESULT_FAILED,	/* something failed (error, text; account -1 for the form's account, new or edited) */
	ML_RESULT_CHECKED	/* an account's new settings work (ws177-p015) */
};

/*
 * One result: its kind and account, a message's folder, UID, flags and
 * size, whether it arrived while idling, an errno value and words of a
 * failure, and the message read (its body allocated; ml_sync_release
 * frees it).  A failure of ML_ERROR_UNTRUSTED also has the server's host,
 * whether it is the SMTP one, and its certificate's fingerprint, for the
 * user to trust (ws177-p015).  next links the queue.
 */
struct ml_result {
	enum ml_result_kind kind;
	int account;
	enum ml_folder folder;
	uint32_t uid;
	unsigned flags;
	size_t size;
	int arrived;
	int error;
	char text[ML_TEXT_MAX];
	char host[ML_TEXT_MAX];
	int smtp;
	char fingerprint[ML_PIN_MAX];
	struct ml_parsed parsed;
	struct ml_result *next;
};

struct ml_sync;

int ml_sync_start(const struct ml_account_config *accounts, size_t count, struct ml_sync **made);
void ml_sync_stop(struct ml_sync *sync);
int ml_sync_fd(const struct ml_sync *sync);
int ml_sync_queue(struct ml_sync *sync, const struct ml_job *job);
int ml_sync_take(struct ml_sync *sync, struct ml_result *result);
void ml_sync_release(struct ml_result *result);

#endif
