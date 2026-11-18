/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The machine's sleep through sessiond (ws052-p011,
 * plan/ws052/phase007/phase.md section 1): a compositor's
 * "POWER suspend", from a session or the login screen.
 *
 * sessiond does not sleep in its own process: a helper (a child of
 * sessiond) asks networkd to turn the radios off (SLEEP_PREPARE), asks the
 * kernel to sleep to idle (KERN_SYSTEM_SLEEP, S0IDLE), asks networkd to
 * take them up again (SLEEP_END), writes one answer line on a pipe and
 * ends.  sessiond's loops (the session's and the login screen's) keep
 * serving the logins, the unlocks and the passkey attempts meanwhile; they
 * poll the pipe and pass the line to the compositor that asked
 * (sleep-rules.c says the lines).  "POWER cancel" (a lid opened while the
 * helper still waits for networkd) writes a byte on a second pipe, which
 * the helper reads just before it asks the kernel; it is never answered.
 *
 * One sleep at a time.  A machine that cannot sleep (the kernel's
 * EOPNOTSUPP) is remembered until sessiond ends, and later requests are
 * answered at once.  The state is this file's; sessiond runs one thread.
 */

#include "sessiond.h"
#include "sleep-rules.h"
#include "../../base/net/protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#include <uapi/system.h>

/* The kernel's system device. */
#define SLEEP_SYSTEM_NODE	"/dev/system"

/* How long the helper waits for networkd's SLEEP_PREPARE and for its SLEEP_END (seconds), and how often it asks SLEEP_END. */
#define SLEEP_PREPARE_WAIT_SECONDS	(NETWORKD_SLEEP_PREPARE_SECONDS + 5U)
#define SLEEP_END_WAIT_SECONDS		2U
#define SLEEP_END_TRIES			2U

/* The highest descriptor the helper closes before its work (it keeps only its two pipes). */
#define SLEEP_DESCRIPTOR_LIMIT		1024

/* The longest stage text of networkd's answer the helper keeps. */
#define SLEEP_STAGE_MAX			64U

/*
 * The helper: its process (0 when none runs), the pipe its answer comes
 * on and the pipe a cancel goes on (-1 when none), the control socket of
 * the compositor that asked (-1 once it went), the answer read so far, and
 * whether the machine was found unable to sleep.
 */
struct sleep_helper {
	pid_t pid;
	int answer;
	int cancel;
	int requester;
	char line[SESSIOND_SLEEP_LINE_MAX + 2U];
	size_t used;
	unsigned unsupported;
};

/*
 * What networkd answered one request: whether it was done, whether
 * networkd was not there, whether no answer came in time, its error, and
 * the stage it named.
 */
struct sleep_network_answer {
	int ok;
	int unavailable;
	int timed_out;
	int error;
	char stage[SLEEP_STAGE_MAX];
};

/* The helper, none at the start; sessiond's one thread uses it. */
static struct sleep_helper sleep_helper = { 0, -1, -1, -1, { 0 }, 0U, 0U };

static void sleep_reply(int control, const char *line);
static void sleep_child(int answer, int cancel) __attribute__((noreturn));
static void sleep_close_others(int keep, int also);
static void sleep_networkd(uint32_t opcode, unsigned seconds, struct sleep_network_answer *answer);
static void sleep_network_fields(const unsigned char *payload, size_t length, struct sleep_network_answer *answer);
static int sleep_cancelled(int cancel);
static void sleep_kernel(struct sessiond_sleep_seen *seen);
static const char *sleep_wake_name(uint32_t wake);
static void sleep_finish(void);

/*
 * Answers a compositor's "POWER suspend" on its control socket: busy while
 * another sleep runs or the machine is ending, "NOSLEEP unsupported" at
 * once for a machine found unable to sleep, otherwise a helper starts and
 * the answer comes when it is done (sessiond_sleep_collect).  from names
 * who asked, for the log.
 */
void
sessiond_sleep_request(
	int control,
	const char *from)
{
	int answer_pipe[2];
	int cancel_pipe[2];
	pid_t child;
	int result;

	/* The machine is ending, or another sleep is under way. */
	if (sessiond_power_started || sleep_helper.pid > 0) {
		sessiond_log("SESSIOND SLEEP refused from=%s (busy)", from);
		sleep_reply(control, "ERROR busy");
		return;
	}

	/* A machine that cannot sleep is told at once. */
	if (sleep_helper.unsupported) {
		sleep_reply(control, "NOSLEEP unsupported");
		return;
	}

	/* The two pipes, kept from every program sessiond starts. */
	result = pipe2(answer_pipe, O_CLOEXEC);
	if (result != 0) {
		sleep_reply(control, "ERROR");
		return;
	}
	result = pipe2(cancel_pipe, O_CLOEXEC);
	if (result != 0) {
		(void)close(answer_pipe[0]);
		(void)close(answer_pipe[1]);
		sleep_reply(control, "ERROR");
		return;
	}

	/* The helper, which does the sleep and ends. */
	child = fork();
	if (child < 0) {
		(void)close(answer_pipe[0]);
		(void)close(answer_pipe[1]);
		(void)close(cancel_pipe[0]);
		(void)close(cancel_pipe[1]);
		sleep_reply(control, "ERROR");
		return;
	}
	if (child == 0)
		sleep_child(answer_pipe[1], cancel_pipe[0]);

	/* sessiond keeps the answer's reading end and the cancel's writing end; the answer is read as it comes. */
	(void)close(answer_pipe[1]);
	(void)close(cancel_pipe[0]);
	(void)fcntl(answer_pipe[0], F_SETFL, O_NONBLOCK);
	sleep_helper.pid = child;
	sleep_helper.answer = answer_pipe[0];
	sleep_helper.cancel = cancel_pipe[1];
	sleep_helper.requester = control;
	sleep_helper.used = 0U;
	sessiond_log("SESSIOND SLEEP start from=%s helper=%ld", from, (long)child);
	syslog(LOG_NOTICE, "sleep asked from %s", from);
}

/*
 * Passes a compositor's "POWER cancel" to the helper, which stops before it
 * asks the kernel if the byte comes in time.  Never answered.
 */
void
sessiond_sleep_cancel(void)
{
	unsigned char byte;

	/* Only a helper under way. */
	if (sleep_helper.pid <= 0 || sleep_helper.cancel < 0)
		return;

	/* One byte on the cancel's pipe. */
	byte = 1U;
	(void)write(sleep_helper.cancel, &byte, 1U);
	sessiond_log("SESSIOND SLEEP cancel helper=%ld", (long)sleep_helper.pid);
}

/* Gives the descriptor the helper's answer comes on, for a loop's poll, or -1 when none runs. */
int
sessiond_sleep_fd(void)
{
	/* Succeeded: the answer's pipe, or none. */
	return sleep_helper.answer;
}

/*
 * Reads what the helper wrote, once its descriptor is readable (or closed):
 * the whole line (or the helper's end without one) finishes the sleep, its
 * answer goes to the compositor that asked, and the helper is reaped.
 */
void
sessiond_sleep_collect(void)
{
	ssize_t count;
	char *end;

	/* Only a helper under way. */
	if (sleep_helper.answer < 0)
		return;

	/* What has come. */
	count = read(sleep_helper.answer, sleep_helper.line + sleep_helper.used, sizeof(sleep_helper.line) - 1U - sleep_helper.used);
	if (count < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (count > 0) {
		sleep_helper.used += (size_t)count;
		sleep_helper.line[sleep_helper.used] = '\0';
		end = strchr(sleep_helper.line, '\n');
		if (end == NULL && sleep_helper.used < sizeof(sleep_helper.line) - 1U)
			return;
		if (end != NULL)
			*end = '\0';
	}

	/* The helper ended without a line: nothing is known of the sleep. */
	if (count <= 0)
		snprintf(sleep_helper.line, sizeof(sleep_helper.line), "ERROR");

	/* Succeeded: the answer is whole. */
	sleep_finish();
}

/* Forgets a compositor's control socket that is closing: an answer still to come is not written to it. */
void
sessiond_sleep_forget(
	int control)
{
	/* Only the one that asked. */
	if (sleep_helper.requester == control)
		sleep_helper.requester = -1;
}

/* Writes one answer line and its end on a compositor's control socket. */
static void
sleep_reply(
	int control,
	const char *line)
{
	char reply[SESSIOND_SLEEP_LINE_MAX + 2U];
	size_t length;
	ssize_t written;

	/* The line and its end, in one write. */
	snprintf(reply, sizeof(reply), "%s\n", line);
	length = strlen(reply);
	written = write(control, reply, length);
	if (written != (ssize_t)length)
		sessiond_log("SESSIOND SLEEP answer lost");
}

/*
 * The helper: networkd's SLEEP_PREPARE, the cancel's check, the kernel's
 * sleep, networkd's SLEEP_END, then the answer on the pipe.  It never
 * returns.
 */
static void
sleep_child(
	int answer,
	int cancel)
{
	struct sessiond_sleep_seen seen;
	struct sleep_network_answer network;
	char line[SESSIOND_SLEEP_LINE_MAX + 2U];
	unsigned tries;
	size_t length;
	int prepared;

	/* Only its two pipes are kept; a write to a gone reader is an error, not a signal. */
	sleep_close_others(answer, cancel);
	(void)signal(SIGPIPE, SIG_IGN);
	memset(&seen, 0, sizeof(seen));
	seen.wake = "none";

	/* networkd turns the radios off; without networkd there is nothing to turn off. */
	sleep_networkd(NETWORKD_OP_SLEEP_PREPARE, SLEEP_PREPARE_WAIT_SECONDS, &network);
	seen.network = sessiond_sleep_network_of(network.ok, network.unavailable, network.timed_out, network.error, network.stage);
	seen.network_error = network.error;
	if (network.timed_out)
		seen.network_error = ETIMEDOUT;
	prepared = 0;
	if (seen.network == SESSIOND_SLEEP_NETWORK_OFF)
		prepared = 1;

	/* A cancel that came in time, or networkd that could not, keeps the kernel from being asked. */
	seen.cancelled = 0U;
	if (seen.network == SESSIOND_SLEEP_NETWORK_OFF || seen.network == SESSIOND_SLEEP_NETWORK_ABSENT)
		seen.cancelled = (unsigned)sleep_cancelled(cancel);
	if (!seen.cancelled &&
	    (seen.network == SESSIOND_SLEEP_NETWORK_OFF || seen.network == SESSIOND_SLEEP_NETWORK_ABSENT))
		sleep_kernel(&seen);

	/* networkd takes the radios up again (also after its own time ran out), asked twice if it does not answer. */
	if (prepared || seen.network == SESSIOND_SLEEP_NETWORK_TIMEOUT) {
		for (tries = 0U; tries < SLEEP_END_TRIES; tries++) {
			sleep_networkd(NETWORKD_OP_SLEEP_END, SLEEP_END_WAIT_SECONDS, &network);
			if (network.ok || network.unavailable)
				break;
		}
	}

	/* The answer, after networkd's end, so that Wi-Fi is the user's again when it is read. */
	(void)sessiond_sleep_answer(&seen, line, sizeof(line) - 1U);
	length = strlen(line);
	line[length] = '\n';
	(void)write(answer, line, length + 1U);
	_exit(0);
}

/* Closes every descriptor of the helper's above the standard ones but the two it keeps. */
static void
sleep_close_others(
	int keep,
	int also)
{
	int descriptor;

	/* Each one up to the limit. */
	for (descriptor = 3; descriptor < SLEEP_DESCRIPTOR_LIMIT; descriptor++) {
		if (descriptor == keep || descriptor == also)
			continue;
		(void)close(descriptor);
	}
}

/*
 * Sends networkd one request without fields (SLEEP_PREPARE or SLEEP_END)
 * and waits up to seconds for its answer.
 */
static void
sleep_networkd(
	uint32_t opcode,
	unsigned seconds,
	struct sleep_network_answer *answer)
{
	struct networkd_protocol_header request;
	struct networkd_protocol_header response;
	struct sockaddr_un address;
	struct timeval receive_timeout;
	struct timeval send_timeout;
	unsigned char payload[NETWORKD_RESPONSE_MAX];
	int descriptor;
	int result;

	/* Nothing known yet. */
	memset(answer, 0, sizeof(*answer));
	answer->error = EIO;

	/* The socket, bounded in both directions. */
	descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0) {
		answer->error = errno;
		return;
	}
	receive_timeout.tv_sec = (time_t)seconds;
	receive_timeout.tv_usec = 0;
	send_timeout.tv_sec = 5;
	send_timeout.tv_usec = 0;
	(void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
	(void)setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

	/* networkd's endpoint; no socket, or nobody listening, is networkd not running. */
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", NETWORKD_SOCKET);
	result = connect(descriptor, (struct sockaddr *)&address, sizeof(address));
	if (result != 0) {
		answer->error = errno;
		if (errno == ENOENT || errno == ECONNREFUSED)
			answer->unavailable = 1;
		(void)close(descriptor);
		return;
	}

	/* One frame, then the request's direction closed (networkd reads to its end). */
	request.request_id = 1U;
	request.opcode = opcode;
	request.payload_length = 0U;
	result = networkd_protocol_write_frame(descriptor, &request, NULL);
	if (result == 0)
		result = shutdown(descriptor, SHUT_WR);
	if (result != 0) {
		answer->error = errno;
		(void)close(descriptor);
		return;
	}

	/* The answer, in time. */
	result = networkd_protocol_read_frame_timed(descriptor, &response, payload, sizeof(payload), NETWORKD_RESPONSE_MAX, seconds);
	if (result != 0) {
		answer->error = errno;
		if (errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK)
			answer->timed_out = 1;
		(void)close(descriptor);
		return;
	}
	(void)close(descriptor);

	/* Succeeded: what the answer says. */
	sleep_network_fields(payload, response.payload_length, answer);
}

/* Reads networkd's answer's status, error and stage. */
static void
sleep_network_fields(
	const unsigned char *payload,
	size_t length,
	struct sleep_network_answer *answer)
{
	struct networkd_field_reader reader;
	struct networkd_field field;
	uint32_t status;
	uint32_t error;
	size_t stage_length;
	int result;

	/* An error unless the answer says otherwise. */
	status = NETWORKD_RESULT_ERROR;
	error = EIO;
	networkd_field_reader_init(&reader, payload, length);

	/* Each field: the status, the error and the stage; others are passed over. */
	for (;;) {
		result = networkd_field_read(&reader, &field);
		if (result != 0)
			break;
		if (field.type == NETWORKD_FIELD_STATUS) {
			(void)networkd_field_read_u32(&field, &status);
		} else if (field.type == NETWORKD_FIELD_ERROR) {
			(void)networkd_field_read_u32(&field, &error);
		} else if (field.type == NETWORKD_FIELD_STAGE) {
			stage_length = field.length;
			if (stage_length >= sizeof(answer->stage))
				stage_length = sizeof(answer->stage) - 1U;
			memcpy(answer->stage, field.value, stage_length);
			answer->stage[stage_length] = '\0';
		}
	}

	/* Succeeded: done, or refused with its error. */
	answer->error = (int)error;
	if (status == NETWORKD_RESULT_OK && error == 0U)
		answer->ok = 1;
}

/* Tells whether a cancel's byte has come on its pipe (without waiting). */
static int
sleep_cancelled(
	int cancel)
{
	struct pollfd entry;
	unsigned char byte;
	ssize_t count;
	int ready;

	/* Only a byte already there. */
	entry.fd = cancel;
	entry.events = POLLIN;
	entry.revents = 0;
	ready = poll(&entry, 1U, 0);
	if (ready <= 0 || (entry.revents & POLLIN) == 0)
		return 0;

	/* A byte is a cancel; the other end closed is not. */
	count = read(cancel, &byte, 1U);
	if (count != 1)
		return 0;

	/* Succeeded: cancelled. */
	return 1;
}

/* Asks the kernel to sleep to idle and records what it said. */
static void
sleep_kernel(
	struct sessiond_sleep_seen *seen)
{
	struct system_sleep_request request;
	int descriptor;
	int result;

	/* The system device. */
	descriptor = open(SLEEP_SYSTEM_NODE, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		seen->ioctl_error = errno;
		return;
	}

	/* The request: the mode, every other field zero. */
	memset(&request, 0, sizeof(request));
	request.mode = KERN_SYSTEM_SLEEP_S0IDLE;
	result = ioctl(descriptor, KERN_SYSTEM_SLEEP, &request);
	if (result != 0) {
		seen->ioctl_error = errno;
		(void)close(descriptor);
		return;
	}
	(void)close(descriptor);

	/* Succeeded: the outcome, the device's name bounded. */
	seen->ioctl_error = 0;
	seen->result = request.result;
	seen->resume_result = request.resume_result;
	seen->wake = sleep_wake_name(request.wake);
	memcpy(seen->device, request.device, sizeof(seen->device));
	seen->device[sizeof(seen->device) - 1U] = '\0';
}

/* Gives a wake's name as the kernel's sleep.end event says it ("none" for KERN_SYSTEM_WAKE_NONE). */
static const char *
sleep_wake_name(
	uint32_t wake)
{
	/* Each reason. */
	switch (wake) {
	case KERN_SYSTEM_WAKE_NONE:
		return "none";
	case KERN_SYSTEM_WAKE_POWER_BUTTON:
		return "power-button";
	case KERN_SYSTEM_WAKE_LID:
		return "lid";
	case KERN_SYSTEM_WAKE_KEYBOARD:
		return "keyboard";
	case KERN_SYSTEM_WAKE_USB:
		return "usb";
	case KERN_SYSTEM_WAKE_AC:
		return "ac";
	case KERN_SYSTEM_WAKE_TIMER:
		return "timer";
	case KERN_SYSTEM_WAKE_SPURIOUS:
		return "spurious";
	default:
		break;
	}

	/* Succeeded: any other. */
	return "other";
}

/* Ends a sleep whose answer is whole: the answer passed on, the helper reaped, the pipes closed. */
static void
sleep_finish(void)
{
	int unsupported;
	int status;

	/* A machine that cannot sleep is remembered. */
	unsupported = strcmp(sleep_helper.line, "NOSLEEP unsupported");
	if (unsupported == 0)
		sleep_helper.unsupported = 1U;

	/* The answer goes to the compositor that asked, if it is still there. */
	sessiond_log("SESSIOND SLEEP answer %s", sleep_helper.line);
	syslog(LOG_NOTICE, "sleep: %s", sleep_helper.line);
	if (sleep_helper.requester >= 0)
		sleep_reply(sleep_helper.requester, sleep_helper.line);

	/* The helper, which ends right after its line. */
	(void)waitpid(sleep_helper.pid, &status, 0);
	(void)close(sleep_helper.answer);
	(void)close(sleep_helper.cancel);

	/* Succeeded: no sleep is under way. */
	sleep_helper.pid = 0;
	sleep_helper.answer = -1;
	sleep_helper.cancel = -1;
	sleep_helper.requester = -1;
	sleep_helper.used = 0U;
}
