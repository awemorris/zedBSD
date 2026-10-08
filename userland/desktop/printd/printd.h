/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * keiland-printd (ws145-p002, plan/ws145/design.md §5): the user's printer
 * daemon.  libkeiland-backend starts it with a socket on descriptor 3 and
 * sends it one command a line (a job comes with the document's descriptor);
 * it copies each document into its spool under $XDG_RUNTIME_DIR, sends it
 * to the printer by IPP (RFC 8010, 8011, plain HTTP/1.1) or LPD (RFC
 * 1179), and tells the backend how each job goes, a line each.
 *
 * The main thread reads the commands; each job is sent by a thread of its
 * own with blocking sockets and time outs, and its lines go out under a
 * lock.  The main thread starts the jobs in the order they came, one at a
 * time for each printer and four at a time in all (the watching of an IPP
 * job that was taken does not count); a host's name is looked up by a
 * helper thread, ten seconds at most (ws177-p022, design §5.3).
 */

#ifndef PRINTD_PRINTD_H
#define PRINTD_PRINTD_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* The longest line of the protocol, with its line feed. */
#define PD_LINE_MAX		1024U

/* The most jobs held (waiting and being sent), and the largest document and spool. */
#define PD_JOBS_MAX		16U
#define PD_FILE_MAX		(256ULL * 1024U * 1024U)
#define PD_SPOOL_MAX		(512ULL * 1024U * 1024U)

/* The lengths of a job's fields, with their NUL. */
#define PD_HOST_MAX		64U
#define PD_PATH_MAX		64U
#define PD_TITLE_MAX		128U
#define PD_NAME_MAX		128U

/* The protocols. */
#define PD_IPP			1
#define PD_LPD			2

/* How long a connection and a name's look up may take, and a send or a receive may stand still (seconds). */
#define PD_CONNECT_SECONDS	10
#define PD_LOOKUP_SECONDS	10
#define PD_IDLE_SECONDS		60

/* The jobs sent at once in all, and the names looked up at once. */
#define PD_SENDING_MAX		4U
#define PD_LOOKUPS_MAX		4U

/*
 * A job: the backend's number, the printer (protocol, host, port, IPP path
 * or LPD queue), the title, the spool file and its size, the order it was
 * accepted in, the document format sent to an IPP printer (empty for
 * application/pdf), its thread and whether it was started, and under the
 * daemon's lock whether it is asked to stop, whether it holds one of the
 * sending places (from its start until the printer took it or it ended)
 * and whether it ended.
 */
struct pd_job {
	uint32_t job;
	int protocol;
	char host[PD_HOST_MAX];
	unsigned port;
	char path[PD_PATH_MAX];
	char title[PD_TITLE_MAX];
	char file[512];
	uint64_t size;
	uint64_t order;
	char format[32];
	pthread_t thread;
	int started;
	int cancel;
	int sending;
	int ended;
};

/*
 * A question for a printer's name (NAME): the backend's number and the
 * printer's address.
 */
struct pd_name {
	uint32_t seq;
	char host[PD_HOST_MAX];
	unsigned port;
};

/* The daemon (main.c). */
void pd_send(const char *format, ...) __attribute__((format(printf, 1, 2)));
int pd_cancelled(struct pd_job *job);
void pd_sent(struct pd_job *job);
int pd_wait(struct pd_job *job, unsigned seconds);
const char *pd_user(void);
const char *pd_host_name(void);
void pd_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* The spool (spool.c). */
int pd_spool_open(char *dir, size_t size);
int pd_spool_copy(const char *dir, struct pd_job *job, int fd, uint64_t used, const char **detail);
void pd_spool_close(const char *dir);

/* The network (net.c). */
int pd_connect(const char *host, unsigned port, int *fd, const char **detail);
int pd_write_all(int fd, const void *data, size_t size);
int pd_send_file(int fd, struct pd_job *job);
ssize_t pd_read_some(int fd, void *data, size_t size);

/* IPP (ipp.c). */
void pd_ipp_job(struct pd_job *job);
void pd_ipp_name(const struct pd_name *name);

/* LPD (lpd.c). */
void pd_lpd_job(struct pd_job *job, unsigned number);

#endif
