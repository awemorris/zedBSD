/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The installer's helper, install-admin, as the installer sees it: a probe
 * that answers and ends (while no session runs), and a session that lives
 * from the first write until the installer ends it, its requests one at a
 * time.  Each runs as a process of the application (kl_process): its lines
 * come as the process's output; each line goes to the flow (flow.c) with
 * the request it answers, and the line that ends a request ("end" of a
 * probe, "DONE" or "FAIL" of the others) frees the helper for the next.
 * The helper's standard error is the installer's, the session's log.
 */

#include "installer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int helper_spawn(struct inst_app *app, const char *verb, int input, struct kl_process **process);
static void helper_read(struct inst_app *app, struct kl_process *process);
static void helper_line(struct inst_app *app, char *line);
static void helper_ended(struct inst_app *app, struct kl_process *process);
static int helper_terminal(enum inst_job job, const char *line);
static int helper_write(struct kl_process *process, const char *text, size_t length);

/*
 * Asks the helper what the disks are: the session's probe while a session
 * runs, else a probe of its own.  Returns 0 when asked, EBUSY while
 * another request is answered, or the error of starting the helper.
 */
int
inst_helper_probe(
	struct inst_app *app)
{
	struct inst_helper *helper;
	int error;

	/* One request at a time. */
	helper = &app->helper;
	if (helper->job != INST_JOB_NONE)
		return EBUSY;

	/* A session answers the probe itself (a second helper would find the lock taken). */
	if (helper->session != NULL) {
		error = inst_helper_request(app, helper->session_disk, INST_JOB_PROBE, "probe\n");
		if (error != 0)
			return error;

		/* Succeeded: the session probes. */
		return 0;
	}

	/* A probe of its own. */
	error = helper_spawn(app, "probe", 0, &helper->probe);
	if (error != 0)
		return error;
	helper->job = INST_JOB_PROBE;
	helper->line_long = 0;
	inst_log("HELPER probe");

	/* Succeeded: the probe runs. */
	return 0;
}

/*
 * Sends a request to the session, starting the session (bound to a disk)
 * when none runs.  A cancellation sent while an installation runs is only
 * written: the installation answers it.  Returns 0 when sent, EBUSY while
 * another request is answered or a session is ending, or the error of
 * starting or writing to the helper.
 */
int
inst_helper_request(
	struct inst_app *app,
	uint32_t disk,
	enum inst_job job,
	const char *request)
{
	struct inst_helper *helper;
	size_t length;
	int error;

	/* A cancellation of the installation that runs goes on the same input. */
	helper = &app->helper;
	length = strlen(request);
	if (job == INST_JOB_CANCEL && helper->job == INST_JOB_INSTALL && helper->session != NULL && helper->session_open) {
		error = helper_write(helper->session, request, length);
		if (error != 0)
			return error;

		/* Succeeded: the installation stops at its next point. */
		return 0;
	}

	/* One request at a time; a session that ends is waited out. */
	if (helper->job != INST_JOB_NONE)
		return EBUSY;
	if (helper->session != NULL && !helper->session_open)
		return EBUSY;

	/* A session bound to another disk ends first. */
	if (helper->session != NULL && helper->session_disk != disk) {
		inst_helper_end_session(app);
		return EBUSY;
	}

	/* The session, when none runs. */
	if (helper->session == NULL) {
		error = helper_spawn(app, "session", 1, &helper->session);
		if (error != 0)
			return error;
		helper->session_open = 1;
		helper->session_disk = disk;
		helper->line_long = 0;
		inst_log("HELPER session disk=%u", (unsigned)disk);
	}

	/* The request, whole. */
	helper->job = job;
	error = helper_write(helper->session, request, length);
	if (error != 0) {
		helper->job = INST_JOB_NONE;
		return error;
	}

	/* Succeeded: the session answers it. */
	return 0;
}

/*
 * Ends the session: its input closes, which the helper takes as the end
 * (as a cancellation between requests), and its output is read until it
 * ends too.
 */
void
inst_helper_end_session(
	struct inst_app *app)
{
	struct inst_helper *helper;

	/* Nothing runs. */
	helper = &app->helper;
	if (helper->session == NULL || !helper->session_open)
		return;

	/* The end of its input. */
	(void)kl_process_close_input(helper->session);
	helper->session_open = 0;
	inst_log("HELPER session-end");
}

/*
 * Takes what a helper's process told: its lines, then its end.
 */
void
inst_helper_ready(
	struct inst_app *app,
	struct kl_process *process,
	unsigned ready)
{
	/* The lines that came (read to the end of the output when it ended). */
	if ((ready & (KL_PROCESS_READY_OUTPUT | KL_PROCESS_READY_EXIT)) != 0U)
		helper_read(app, process);

	/* The process ended. */
	if ((ready & KL_PROCESS_READY_EXIT) != 0U)
		helper_ended(app, process);
}

/* Reports whether the helper is answering a request. */
int
inst_helper_busy(
	const struct inst_app *app)
{
	/* A request is answered. */
	if (app->helper.job != INST_JOB_NONE)
		return 1;

	/* A session that ends is still busy. */
	if (app->helper.session != NULL && !app->helper.session_open)
		return 1;

	/* Free. */
	return 0;
}

/*
 * Starts the helper with a verb as a process of the application, its
 * output read by the line and, when asked, its input written.  Returns 0
 * or an errno value.
 */
static int
helper_spawn(
	struct inst_app *app,
	const char *verb,
	int input,
	struct kl_process **process)
{
	struct kl_process_options options;
	const char *argument[3];
	int error;

	/* The helper and its verb; the output read, the input when asked, the error the installer's. */
	argument[0] = INST_HELPER;
	argument[1] = verb;
	argument[2] = NULL;
	memset(&options, 0, sizeof(options));
	options.argv = argument;
	options.streams = KL_PROCESS_OUTPUT;
	if (input)
		options.streams |= KL_PROCESS_INPUT;
	error = kl_process_start(app->app, &options, process);
	if (error != 0)
		return error;

	/* Succeeded: the helper runs. */
	return 0;
}

/* Reads the lines a process has and hands each whole one on (a line too long is dropped whole). */
static void
helper_read(
	struct inst_app *app,
	struct kl_process *process)
{
	struct inst_helper *helper;
	int kind;

	/* Until no whole line waits. */
	helper = &app->helper;
	for (;;) {
		kind = kl_process_read_line(process, KL_PROCESS_OUTPUT, helper->line, sizeof(helper->line));
		if (kind == 0)
			return;

		/* The start of a line too long: it and its rest are dropped. */
		if (kind == KL_PROCESS_LINE_CUT) {
			helper->line_long = 1;
			continue;
		}

		/* The rest of a dropped line ends it. */
		if (helper->line_long) {
			helper->line_long = 0;
			continue;
		}

		/* A line to the flow. */
		helper_line(app, helper->line);
	}
}

/* Gives a line to the flow, and frees the helper when it ends a request. */
static void
helper_line(
	struct inst_app *app,
	char *line)
{
	enum inst_job job;
	int ended;

	/* The line, for the request it answers. */
	job = app->helper.job;
	ended = helper_terminal(job, line);
	inst_flow_line(app, job, line);

	/* The end of a session's request: the next may be sent. */
	if (ended && app->helper.session != NULL && app->helper.job == job) {
		app->helper.job = INST_JOB_NONE;
		inst_flow_ended(app, job, 0);
	}
}

/*
 * Takes the end of a helper's process: the probe or the session has
 * ended.  A request it had not answered ends with it.  The status given on
 * is the wait status the system reports (an exit code in its second byte,
 * or the signal).
 */
static void
helper_ended(
	struct inst_app *app,
	struct kl_process *process)
{
	struct inst_helper *helper;
	enum inst_job job;
	unsigned how;
	int status;
	int code;

	/* How it ended, as a wait status. */
	helper = &app->helper;
	how = kl_process_exit(process, &code);
	status = code;
	if (how == KL_PROCESS_EXITED)
		status = code << 8;
	job = helper->job;
	kl_process_close(process);

	/* The probe of its own. */
	if (process == helper->probe) {
		inst_log("HELPER probe-end status=%d", status);
		helper->probe = NULL;
		helper->job = INST_JOB_NONE;
		inst_flow_ended(app, job, status);
		return;
	}

	/* Another process is not the helper's. */
	if (process != helper->session)
		return;

	/* The session: it has ended, with whatever it was answering. */
	inst_log("HELPER session-gone status=%d job=%d", status, (int)job);
	helper->session = NULL;
	helper->session_open = 0;
	helper->session_disk = 0;
	helper->job = INST_JOB_NONE;
	if (job != INST_JOB_NONE) {
		/* A session that ended with a failure status reports -1. */
		if (status == 0)
			inst_flow_ended(app, job, 0);
		else
			inst_flow_ended(app, job, -1);
	}
}

/* Reports whether a line of the session ends the request it answers. */
static int
helper_terminal(
	enum inst_job job,
	const char *line)
{
	int same;

	/* A failure ends any request. */
	same = strncmp(line, "FAIL ", 5U);
	if (same == 0)
		return 1;

	/* A probe ends with its "end". */
	if (job == INST_JOB_PROBE) {
		same = strcmp(line, "end");
		if (same == 0)
			return 1;
		return 0;
	}

	/* The others end with DONE. */
	same = strcmp(line, "DONE");
	if (same == 0)
		return 1;

	/* A line within the answer. */
	return 0;
}

/* Writes a text whole to a process's input; returns 0 or an errno value. */
static int
helper_write(
	struct kl_process *process,
	const char *text,
	size_t length)
{
	size_t written;
	int error;

	/* The text (a request is far smaller than the input's room). */
	error = kl_process_write(process, text, length, &written);
	if (error != 0)
		return error;
	if (written != length)
		return EAGAIN;

	/* Succeeded: the text was taken whole. */
	return 0;
}
