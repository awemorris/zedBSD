/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A connection through ssh_config's ProxyCommand (proxy.h).
 *
 * The command runs without a shell and without a console window: its
 * program (proxy-command.c takes it from the line; program.c finds it in
 * PATH when it is named without a folder) is started with the rest of
 * the line as its command line, and with three pipes as its standard
 * input, output and errors.  It runs in a job object that ends it, and
 * every program it starts, when the connection ends or mikuterm does.
 *
 * The caller gets one end of a pair of connected loopback sockets.  Three
 * threads carry the bytes: what the program writes goes to the other end
 * of the pair, what comes on that end goes to the program's input, and
 * the last of what it writes on its errors is kept to tell why a
 * connection failed.  The pair is checked to be the two ends this side
 * made (the connecting socket's own address is the one accepted), so no
 * other program on the computer can take the listening end's place.
 */

#include "mikuterm.h"
#include "proxy.h"
#include "proxy-command.h"
#include "program.h"

#include <keiland/keiland.h>

#include <windows.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest command line, in UTF-16 units with its NUL. */
#define PROXY_COMMAND_LINE_MAX	8192U

/* The most words a program's path without quotes may span (a path with spaces). */
#define PROXY_WORDS_MAX		16U

/* The bytes one relay moves at once. */
#define PROXY_CHUNK		16384U

/*
 * One command's run: the job and the program, mikuterm's ends of its
 * three pipes, the end of the socket pair the relays use, the three relay
 * threads, and the last of what the program wrote on its errors (tail,
 * tail_size bytes of it, under lock: the errors' relay writes it while
 * the connection's thread may read it).  input is closed by the input's
 * relay when the connection's end closes (the program then reads its
 * end), and is NULL after.
 */
struct proxy {
	HANDLE job;
	HANDLE process;
	HANDLE input;
	HANDLE output;
	HANDLE errors;
	struct kl_socket *relay;
	struct kl_thread *output_thread;
	struct kl_thread *input_thread;
	struct kl_thread *errors_thread;
	struct kl_mutex lock;
	char tail[PROXY_ERRORS_MAX];
	size_t tail_size;
};

static int proxy_find(const char *command, wchar_t *program, size_t program_size, const char **rest, char *error, size_t error_size);
static int proxy_line(const wchar_t *program, const char *rest, wchar_t *line, size_t line_size);
static int proxy_pipes(struct proxy *proxy, HANDLE *child_input, HANDLE *child_output, HANDLE *child_errors);
static int proxy_job(struct proxy *proxy);
static int proxy_spawn(struct proxy *proxy, const wchar_t *program, wchar_t *line, HANDLE child_input, HANDLE child_output, HANDLE child_errors);
static int proxy_pair(struct kl_socket **ours, struct kl_socket **theirs);
static int proxy_threads(struct proxy *proxy);
static void proxy_from_program(void *argument);
static void proxy_to_program(void *argument);
static void proxy_from_errors(void *argument);
static void proxy_free(struct proxy *proxy);

/*
 * Runs a command (UTF-8, its %-tokens already put in) and gives the
 * connection to it: *socket is the caller's end (it does not block; the
 * caller closes it before proxy_stop), and *proxy the run to stop.
 * Returns 0, or ENOENT (no such program), EINVAL, ENOMEM or EIO with a
 * line about it in error (error_size bytes).
 */
int
proxy_start(
	const char *command,
	struct proxy **proxy,
	struct kl_socket **socket,
	char *error,
	size_t error_size)
{
	struct proxy *run;
	wchar_t program[MIKUTERM_PATH_MAX];
	wchar_t *line;
	const char *rest;
	HANDLE child_input;
	HANDLE child_output;
	HANDLE child_errors;
	int status;

	/* Nothing started yet. */
	*proxy = NULL;
	*socket = NULL;
	error[0] = '\0';

	/* The program the command names, and the rest of its line. */
	status = proxy_find(command, program, sizeof(program) / sizeof(program[0]), &rest, error, error_size);
	if (status != 0)
		return status;

	/* The command line: the program's path in quotes, then the rest as written. */
	line = malloc(PROXY_COMMAND_LINE_MAX * sizeof(*line));
	if (line == NULL)
		return ENOMEM;
	status = proxy_line(program, rest, line, PROXY_COMMAND_LINE_MAX);
	if (status != 0) {
		free(line);
		(void)snprintf(error, error_size, "ProxyCommand: the command line is too long or not UTF-8");
		return status;
	}

	/* The run's state. */
	run = calloc(1U, sizeof(*run));
	if (run == NULL) {
		free(line);
		return ENOMEM;
	}
	status = kl_mutex_init(&run->lock);
	if (status != 0) {
		free(run);
		free(line);
		return status;
	}

	/* The three pipes. */
	status = proxy_pipes(run, &child_input, &child_output, &child_errors);
	if (status != 0) {
		free(line);
		proxy_free(run);
		(void)snprintf(error, error_size, "ProxyCommand: the program could not be started");
		return status;
	}

	/* The job, and the program in it; its ends of the pipes are its own after. */
	status = proxy_job(run);
	if (status == 0)
		status = proxy_spawn(run, program, line, child_input, child_output, child_errors);
	CloseHandle(child_input);
	CloseHandle(child_output);
	CloseHandle(child_errors);
	free(line);
	if (status != 0) {
		proxy_free(run);
		(void)snprintf(error, error_size, "ProxyCommand: the program could not be started");
		return status;
	}

	/* The socket pair: the caller's end and the relays'. */
	status = proxy_pair(socket, &run->relay);
	if (status != 0) {
		proxy_free(run);
		(void)snprintf(error, error_size, "ProxyCommand: no connection to the program could be made");
		return status;
	}

	/* The relays. */
	status = proxy_threads(run);
	if (status != 0) {
		kl_socket_close(*socket);
		*socket = NULL;
		proxy_free(run);
		(void)snprintf(error, error_size, "ProxyCommand: the program's relays could not start");
		return status;
	}

	/* Succeeded: the program runs and the socket is its connection. */
	*proxy = run;
	return 0;
}

/*
 * Writes the last of what the command wrote on its errors into text (size
 * bytes with its NUL), as it wrote it (its line breaks kept, a NUL left
 * out); empty when it wrote nothing.
 */
void
proxy_errors(
	struct proxy *proxy,
	char *text,
	size_t size)
{
	size_t length;
	size_t index;
	size_t used;

	/* Nothing yet. */
	if (size == 0U)
		return;
	text[0] = '\0';

	/* The tail, under the lock its relay writes it under. */
	kl_mutex_lock(&proxy->lock);

	length = proxy->tail_size;
	if (length >= size)
		length = size - 1U;
	used = 0U;
	for (index = proxy->tail_size - length; index < proxy->tail_size; index++) {
		/* A NUL would end the text early: it is left out. */
		if (proxy->tail[index] != '\0') {
			text[used] = proxy->tail[index];
			used++;
		}
	}
	text[used] = '\0';

	kl_mutex_unlock(&proxy->lock);
}

/*
 * Stops a command's run: the program and every program it started end,
 * the relays end, and the run's state goes.  The caller has closed its
 * end of the socket pair, which ends the input's relay.
 */
void
proxy_stop(
	struct proxy *proxy)
{
	/* No run. */
	if (proxy == NULL)
		return;

	/* The run's state goes, the program ended first. */
	proxy_free(proxy);
}

/*
 * Finds the program the command names: a path in quotes, a Windows path
 * (one with spaces and without quotes is taken as Windows takes it: the
 * shortest run of its words that names a program), or a name looked for
 * in PATH.  Returns 0 with the program's path and *rest the rest of the
 * line, or an error with a line about it.
 */
static int
proxy_find(
	const char *command,
	wchar_t *program,
	size_t program_size,
	const char **rest,
	char *error,
	size_t error_size)
{
	char name[MIKUTERM_PATH_MAX];
	char first[MIKUTERM_PATH_MAX];
	unsigned words;
	int quoted;
	int status;
	int absolute;

	/* The first word, the name the program goes by when nothing is found. */
	status = proxy_command_program(command, 1U, first, sizeof(first), rest, &quoted);
	if (status != 0) {
		(void)snprintf(error, error_size, "ProxyCommand: no program is named");
		return EINVAL;
	}

	/* A program in quotes, or a name without a folder: the first word alone. */
	absolute = mikuterm_path_absolute(first);
	if (quoted || !absolute) {
		status = mikuterm_program_find(first, program, program_size);
		if (status == ENOENT)
			(void)snprintf(error, error_size, "ProxyCommand: %s: no such program", first);
		else if (status != 0)
			(void)snprintf(error, error_size, "ProxyCommand: %s: not a program name or path (a path from a drive, or a name in PATH)", first);
		return status;
	}

	/* A Windows path without quotes: one word, then two, until a program is there. */
	status = ENOENT;
	for (words = 1U; words <= PROXY_WORDS_MAX; words++) {
		/* So many words, or no more of them. */
		status = proxy_command_program(command, words, name, sizeof(name), rest, &quoted);
		if (status != 0)
			break;

		/* The path those words make. */
		status = mikuterm_program_find(name, program, program_size);
		if (status != ENOENT)
			break;
	}

	/* A path that names no program. */
	if (status != 0) {
		(void)snprintf(error, error_size, "ProxyCommand: %s: no such program", first);
		return ENOENT;
	}

	/* Succeeded: the program and the rest of the line. */
	return 0;
}

/* Makes the command line: the program in quotes, then the rest of the line (UTF-8) as written.  Returns 0 or EINVAL. */
static int
proxy_line(
	const wchar_t *program,
	const char *rest,
	wchar_t *line,
	size_t line_size)
{
	int written;
	int units;

	/* The program in quotes (a path has no quote in it). */
	written = swprintf(line, line_size, L"\"%ls\"", program);
	if (written < 0 || (size_t)written >= line_size)
		return EINVAL;

	/* The rest after it, from UTF-8. */
	units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, rest, -1, line + written, (int)(line_size - (size_t)written));
	if (units <= 0)
		return EINVAL;

	/* Succeeded: the line. */
	return 0;
}

/*
 * Makes the three pipes: the program's ends (*child_*, which it inherits)
 * and mikuterm's (the run's input, output and errors, which it does not).
 * Returns 0 or EIO.
 */
static int
proxy_pipes(
	struct proxy *proxy,
	HANDLE *child_input,
	HANDLE *child_output,
	HANDLE *child_errors)
{
	SECURITY_ATTRIBUTES inherit;
	BOOL made;

	/* The program's ends are handed down. */
	memset(&inherit, 0, sizeof(inherit));
	inherit.nLength = sizeof(inherit);
	inherit.bInheritHandle = TRUE;

	/* Its input: it reads, mikuterm writes. */
	made = CreatePipe(child_input, &proxy->input, &inherit, 0U);
	if (!made)
		return EIO;
	(void)SetHandleInformation(proxy->input, HANDLE_FLAG_INHERIT, 0);

	/* Its output: it writes, mikuterm reads. */
	made = CreatePipe(&proxy->output, child_output, &inherit, 0U);
	if (!made) {
		CloseHandle(*child_input);
		return EIO;
	}
	(void)SetHandleInformation(proxy->output, HANDLE_FLAG_INHERIT, 0);

	/* Its errors, the same way. */
	made = CreatePipe(&proxy->errors, child_errors, &inherit, 0U);
	if (!made) {
		CloseHandle(*child_input);
		CloseHandle(*child_output);
		return EIO;
	}
	(void)SetHandleInformation(proxy->errors, HANDLE_FLAG_INHERIT, 0);

	/* Succeeded: the three pipes. */
	return 0;
}

/* Makes the job the program runs in: closing it ends every program in it.  Returns 0 or EIO. */
static int
proxy_job(
	struct proxy *proxy)
{
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
	BOOL set;

	/* The job. */
	proxy->job = CreateJobObjectW(NULL, NULL);
	if (proxy->job == NULL)
		return EIO;

	/* Its programs end when it is closed (mikuterm's end closes it too). */
	memset(&limits, 0, sizeof(limits));
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	set = SetInformationJobObject(proxy->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
	if (!set)
		return EIO;

	/* Succeeded: the job. */
	return 0;
}

/*
 * Starts the program in the job, suspended until it is in it, without a
 * console window, handing it only its three pipes' ends.  Returns 0,
 * ENOMEM or EIO.
 */
static int
proxy_spawn(
	struct proxy *proxy,
	const wchar_t *program,
	wchar_t *line,
	HANDLE child_input,
	HANDLE child_output,
	HANDLE child_errors)
{
	STARTUPINFOEXW startup;
	PROCESS_INFORMATION process;
	LPPROC_THREAD_ATTRIBUTE_LIST attributes;
	HANDLE handed[3];
	SIZE_T attributes_size;
	DWORD resumed;
	BOOL made;
	BOOL assigned;

	/* The room for the list of handles handed down. */
	attributes_size = 0U;
	(void)InitializeProcThreadAttributeList(NULL, 1U, 0U, &attributes_size);
	attributes = malloc(attributes_size);
	if (attributes == NULL)
		return ENOMEM;

	/* The list: the pipes' ends, nothing else. */
	made = InitializeProcThreadAttributeList(attributes, 1U, 0U, &attributes_size);
	if (!made) {
		free(attributes);
		return EIO;
	}
	handed[0] = child_input;
	handed[1] = child_output;
	handed[2] = child_errors;
	made = UpdateProcThreadAttribute(attributes, 0U, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handed, sizeof(handed), NULL, NULL);
	if (!made) {
		DeleteProcThreadAttributeList(attributes);
		free(attributes);
		return EIO;
	}

	/* Its standard handles. */
	memset(&startup, 0, sizeof(startup));
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = child_input;
	startup.StartupInfo.hStdOutput = child_output;
	startup.StartupInfo.hStdError = child_errors;
	startup.lpAttributeList = attributes;

	/* The program, suspended, without a console window. */
	memset(&process, 0, sizeof(process));
	made = CreateProcessW(program, line, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &startup.StartupInfo, &process);
	DeleteProcThreadAttributeList(attributes);
	free(attributes);
	if (!made)
		return EIO;

	/* In the job before it runs, so what it starts is in it too. */
	proxy->process = process.hProcess;
	assigned = AssignProcessToJobObject(proxy->job, process.hProcess);
	if (!assigned) {
		(void)TerminateProcess(process.hProcess, 1U);
		CloseHandle(process.hThread);
		return EIO;
	}

	/* It runs. */
	resumed = ResumeThread(process.hThread);
	CloseHandle(process.hThread);
	if (resumed == (DWORD)-1)
		return EIO;

	/* Succeeded: the program runs in the job. */
	return 0;
}

/*
 * Makes a pair of connected loopback sockets: *ours, which does not
 * block, and *theirs, which does.  The connection accepted must come from
 * ours (its address and port).  Returns 0 or the error.
 */
static int
proxy_pair(
	struct kl_socket **ours,
	struct kl_socket **theirs)
{
	struct kl_socket *listening;
	struct kl_socket *connecting;
	struct kl_socket *accepted;
	struct kl_address address;
	struct kl_address peer;
	struct kl_address mine;
	int differs;
	int error;

	/* A socket listening on a port of the loopback the system chooses. */
	*ours = NULL;
	*theirs = NULL;
	error = kl_socket_open(KL_ADDRESS_IPV4, KL_SOCKET_STREAM, 0U, &listening);
	if (error != 0)
		return error;
	kl_address_loopback(KL_ADDRESS_IPV4, 0U, &address);
	error = kl_socket_bind(listening, &address);
	if (error == 0)
		error = kl_socket_listen(listening, 1U);
	if (error == 0)
		error = kl_socket_local_address(listening, &address);
	if (error != 0) {
		kl_socket_close(listening);
		return error;
	}

	/* The connecting end. */
	error = kl_socket_open(KL_ADDRESS_IPV4, KL_SOCKET_STREAM, 0U, &connecting);
	if (error != 0) {
		kl_socket_close(listening);
		return error;
	}
	error = kl_socket_connect(connecting, &address);
	if (error == 0)
		error = kl_socket_local_address(connecting, &mine);
	if (error != 0) {
		kl_socket_close(connecting);
		kl_socket_close(listening);
		return error;
	}

	/* The accepted end; the listening one goes. */
	error = kl_socket_accept(listening, 0U, &accepted, &peer);
	kl_socket_close(listening);
	if (error != 0) {
		kl_socket_close(connecting);
		return error;
	}

	/* The accepted connection must be this side's own: its family, its port and its address. */
	differs = memcmp(peer.bytes, mine.bytes, sizeof(peer.bytes));
	if (peer.family != mine.family ||
	    peer.port != mine.port ||
	    differs != 0) {
		kl_socket_close(accepted);
		kl_socket_close(connecting);
		return EIO;
	}

	/* The caller's end does not block; small packets go at once both ways. */
	error = kl_socket_set_nonblocking(connecting, 1);
	if (error != 0) {
		kl_socket_close(accepted);
		kl_socket_close(connecting);
		return error;
	}
	(void)kl_socket_set_option(connecting, KL_SOCKET_NO_DELAY, 1);
	(void)kl_socket_set_option(accepted, KL_SOCKET_NO_DELAY, 1);

	/* Succeeded: the pair. */
	*ours = connecting;
	*theirs = accepted;
	return 0;
}

/* Starts the three relays.  Returns 0 or the error of the one that did not start (the others run on). */
static int
proxy_threads(
	struct proxy *proxy)
{
	struct kl_thread_options options;
	int error;

	/* What the program writes, to the pair. */
	memset(&options, 0, sizeof(options));
	options.name = "proxy-output";
	options.run = proxy_from_program;
	options.argument = proxy;
	options.priority = KL_THREAD_PRIORITY_NORMAL;
	error = kl_thread_start(&options, &proxy->output_thread);
	if (error != 0)
		return error;

	/* What comes on the pair, to the program. */
	options.name = "proxy-input";
	options.run = proxy_to_program;
	error = kl_thread_start(&options, &proxy->input_thread);
	if (error != 0)
		return error;

	/* What the program writes on its errors, kept. */
	options.name = "proxy-errors";
	options.run = proxy_from_errors;
	error = kl_thread_start(&options, &proxy->errors_thread);
	if (error != 0)
		return error;

	/* Succeeded: the relays run. */
	return 0;
}

/*
 * The output's relay: what the program writes goes to the pair, until it
 * ends its output; then the pair's sending is ended, which the
 * connection reads as the server closing.
 */
static void
proxy_from_program(
	void *argument)
{
	struct proxy *proxy;
	unsigned char *bytes;
	size_t offset;
	size_t sent;
	DWORD got;
	BOOL read;
	int error;

	/* The run, and the room the bytes pass through. */
	proxy = argument;
	bytes = malloc(PROXY_CHUNK);

	/* Until the program ends its output or the pair is gone. */
	while (bytes != NULL) {
		/* What the program wrote. */
		got = 0U;
		read = ReadFile(proxy->output, bytes, PROXY_CHUNK, &got, NULL);
		if (!read || got == 0U)
			break;

		/* All of it to the pair. */
		error = 0;
		for (offset = 0U; offset < got; offset += sent) {
			/* The pair takes some of it. */
			sent = 0U;
			error = kl_socket_send(proxy->relay, bytes + offset, got - offset, &sent);
			if (error != 0)
				break;
		}
		if (error != 0)
			break;
	}

	/* The pair's sending ends: the connection sees the end. */
	free(bytes);
	(void)kl_socket_shutdown(proxy->relay, KL_SOCKET_SHUT_SEND);
}

/*
 * The input's relay: what comes on the pair goes to the program, until
 * the connection's end closes; then the program's input is closed, which
 * the program reads as the end.
 */
static void
proxy_to_program(
	void *argument)
{
	struct proxy *proxy;
	unsigned char *bytes;
	size_t received;
	DWORD offset;
	DWORD written;
	BOOL wrote;
	int error;

	/* The run, and the room the bytes pass through. */
	proxy = argument;
	bytes = malloc(PROXY_CHUNK);

	/* Until the pair ends or the program does. */
	while (bytes != NULL) {
		/* What came on the pair; none is its end. */
		received = 0U;
		error = kl_socket_receive(proxy->relay, bytes, PROXY_CHUNK, &received);
		if (error != 0 || received == 0U)
			break;

		/* All of it to the program. */
		wrote = TRUE;
		for (offset = 0U; offset < (DWORD)received; offset += written) {
			/* The program's input takes some of it. */
			written = 0U;
			wrote = WriteFile(proxy->input, bytes + offset, (DWORD)received - offset, &written, NULL);
			if (!wrote)
				break;
		}
		if (!wrote)
			break;
	}

	/* The program's input ends. */
	free(bytes);
	CloseHandle(proxy->input);
	proxy->input = NULL;
}

/* The errors' relay: what the program writes on its errors is kept, the last PROXY_ERRORS_MAX bytes of it. */
static void
proxy_from_errors(
	void *argument)
{
	struct proxy *proxy;
	char bytes[512];
	size_t keep;
	size_t drop;
	DWORD got;
	BOOL read;

	/* The run. */
	proxy = argument;

	/* Until the program ends its errors. */
	for (;;) {
		/* What it wrote. */
		got = 0U;
		read = ReadFile(proxy->errors, bytes, sizeof(bytes), &got, NULL);
		if (!read || got == 0U)
			break;

		/* After the tail, the oldest bytes dropped to make room. */
		kl_mutex_lock(&proxy->lock);

		keep = got;
		if (keep > PROXY_ERRORS_MAX)
			keep = PROXY_ERRORS_MAX;
		drop = 0U;
		if (proxy->tail_size + keep > PROXY_ERRORS_MAX)
			drop = proxy->tail_size + keep - PROXY_ERRORS_MAX;
		memmove(proxy->tail, proxy->tail + drop, proxy->tail_size - drop);
		proxy->tail_size -= drop;
		memcpy(proxy->tail + proxy->tail_size, bytes + (got - keep), keep);
		proxy->tail_size += keep;

		kl_mutex_unlock(&proxy->lock);
	}
}

/*
 * Lets a run go: the program and what it started end (the job's end),
 * which ends the relays reading its pipes; the input's relay has ended
 * with the caller's end of the pair, or ends now that the program's
 * input is broken.  Each relay is waited for, then the handles go.
 */
static void
proxy_free(
	struct proxy *proxy)
{
	/* The program and every program it started end. */
	if (proxy->job != NULL)
		(void)TerminateJobObject(proxy->job, 1U);
	if (proxy->process != NULL)
		(void)WaitForSingleObject(proxy->process, INFINITE);

	/* The relays' pair is ended both ways, so a relay waiting on it returns. */
	if (proxy->relay != NULL)
		(void)kl_socket_shutdown(proxy->relay, KL_SOCKET_SHUT_BOTH);

	/* Each relay that started is waited for. */
	if (proxy->output_thread != NULL)
		(void)kl_thread_join(proxy->output_thread);
	if (proxy->input_thread != NULL)
		(void)kl_thread_join(proxy->input_thread);
	if (proxy->errors_thread != NULL)
		(void)kl_thread_join(proxy->errors_thread);

	/* The handles, the pair's end and the state go. */
	if (proxy->relay != NULL)
		kl_socket_close(proxy->relay);
	if (proxy->input != NULL)
		CloseHandle(proxy->input);
	if (proxy->output != NULL)
		CloseHandle(proxy->output);
	if (proxy->errors != NULL)
		CloseHandle(proxy->errors);
	if (proxy->process != NULL)
		CloseHandle(proxy->process);
	if (proxy->job != NULL)
		CloseHandle(proxy->job);
	kl_mutex_destroy(&proxy->lock);
	free(proxy);
}
