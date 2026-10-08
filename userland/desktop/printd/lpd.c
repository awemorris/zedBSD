/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * LPD (RFC 1179; plan/ws145/design.md §5.5): "receive a printer job" for
 * the queue, the data file first (as BSD's lpr sends it, so that no printer
 * starts before the data is there), then the control file, each answered
 * by a zero byte.  The source port is an ordinary one (the user cannot
 * bind 721 to 731).  A job asked to stop before its control file is sent
 * is dropped: between two subcommands with "abort job" (\001) and the
 * connection closed, in the middle of the data file by closing it.  One
 * asked to stop after its control file went cannot be taken back: it is
 * told failed, unconfirmed (ws177-p022, design §5.5).
 */

#include "printd.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* How long the last answer is waited for (seconds), and the most bytes of a title in the control file. */
#define LPD_LAST_WAIT		300
#define LPD_TITLE_MOST		99U

static int lpd_ack(int fd);
static void lpd_cut(char *text, size_t most);

/*
 * Sends a job to its LPD queue; its STATE lines go to the backend.
 * number is the job's three-digit number in the files' names.
 */
void
pd_lpd_job(
	struct pd_job *job,
	unsigned number)
{
	struct timeval wait;
	const char *detail;
	char control[512];
	char line[256];
	char title[PD_TITLE_MAX];
	char user[32];
	char host[32];
	int length;
	int status;
	int stop;
	int fd;

	/* The connection. */
	status = pd_connect(job->host, job->port, &fd, &detail);
	if (status != 0) {
		pd_send("STATE %lu failed %s", (unsigned long)job->job, detail);
		return;
	}

	/* Connected: sending. */
	pd_send("STATE %lu sending", (unsigned long)job->job);

	/* The names in the files, cut to their lengths. */
	(void)snprintf(host, sizeof(host), "%s", pd_host_name());
	(void)snprintf(user, sizeof(user), "%s", pd_user());
	(void)snprintf(title, sizeof(title), "%s", job->title);
	if (title[0] == '\0')
		(void)snprintf(title, sizeof(title), "Document");
	lpd_cut(title, LPD_TITLE_MOST);

	/* Receive a printer job, for the queue. */
	(void)snprintf(line, sizeof(line), "\002%s\n", job->path);
	status = pd_write_all(fd, line, strlen(line));
	if (status == 0)
		status = lpd_ack(fd);

	/* The data file first. */
	if (status == 0) {
		(void)snprintf(line, sizeof(line), "\003%llu dfA%03u%s\n", (unsigned long long)job->size, number, host);
		status = pd_write_all(fd, line, strlen(line));
	}

	/* Its bytes and the zero after them. */
	if (status == 0)
		status = lpd_ack(fd);
	if (status == 0)
		status = pd_send_file(fd, job);
	if (status == 0)
		status = pd_write_all(fd, "", 1U);
	if (status == 0)
		status = lpd_ack(fd);

	/* Stopped in the middle of the data file: the connection closed, no job at the printer. */
	if (status == ECANCELED) {
		(void)close(fd);
		pd_send("STATE %lu cancelled", (unsigned long)job->job);
		return;
	}

	/* Stopped after the data file's answer: "abort job", then closed. */
	stop = pd_cancelled(job);
	if (status == 0 && stop) {
		(void)pd_write_all(fd, "\001\n", 2U);
		(void)close(fd);
		pd_send("STATE %lu cancelled", (unsigned long)job->job);
		return;
	}

	/* The control file. */
	length = snprintf(control, sizeof(control), "H%s\nP%s\nJ%s\nN%s\nldfA%03u%s\nUdfA%03u%s\n", host, user, title, title, number, host,
	    number, host);
	if (status == 0) {
		(void)snprintf(line, sizeof(line), "\002%d cfA%03u%s\n", length, number, host);
		status = pd_write_all(fd, line, strlen(line));
	}

	/* Its bytes and the zero after them. */
	if (status == 0)
		status = lpd_ack(fd);
	if (status == 0)
		status = pd_write_all(fd, control, (size_t)length);
	if (status == 0)
		status = pd_write_all(fd, "", 1U);

	/* The last answer, waited for longer: without it the job is done, not confirmed. */
	if (status == 0) {
		wait.tv_sec = LPD_LAST_WAIT;
		wait.tv_usec = 0;
		(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
		status = lpd_ack(fd);
		if (status == ETIMEDOUT || status == EAGAIN) {
			(void)close(fd);
			pd_send("STATE %lu done unconfirmed", (unsigned long)job->job);
			return;
		}
	}

	/* The connection ends. */
	(void)close(fd);

	/* The outcome; a job asked to stop after its control file went may have printed. */
	stop = pd_cancelled(job);
	if (status == EPROTO)
		pd_send("STATE %lu failed refused", (unsigned long)job->job);
	else if (status != 0)
		pd_send("STATE %lu failed io", (unsigned long)job->job);
	else if (stop)
		pd_send("STATE %lu failed unconfirmed", (unsigned long)job->job);
	else
		pd_send("STATE %lu done", (unsigned long)job->job);
	pd_log("job %lu lpd status %d", (unsigned long)job->job, status);
}

/* Reads the printer's answer: 0 for a zero byte, EPROTO for another, or the read's errno. */
static int
lpd_ack(
	int fd)
{
	unsigned char byte;
	ssize_t got;

	/* One byte. */
	got = pd_read_some(fd, &byte, 1U);
	if (got < 0)
		return errno;
	if (got == 0)
		return EIO;
	if (byte != 0U)
		return EPROTO;
	return 0;
}

/* Cuts a text to a number of bytes at a character's boundary. */
static void
lpd_cut(
	char *text,
	size_t most)
{
	size_t length;

	/* Short enough. */
	length = strlen(text);
	if (length <= most)
		return;

	/* Back to where a character starts. */
	length = most;
	while (length > 0U && ((unsigned char)text[length] & 0xc0U) == 0x80U)
		length--;
	text[length] = '\0';
}
