/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Child-process vi.exe connection for mikuterm over Win32 anonymous pipes.
 *
 * Spawns vi.exe --mikuterm without a console window, relays its stdout pipe
 * into a bounded ring buffer on a dedicated reader thread, writes keystrokes
 * and in-band CSI 8 ; rows ; cols t resize notifications to its stdin pipe,
 * and signals TERMINAL_READY_EXIT when vi.exe terminates.
 */

#include "apps/terminal-win32/vi-connection-win32.h"
#include "mikuterm.h"

#include <keiland/keiland.h>

#include <windows.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VI_CONN_POST		0x76695031U
#define VI_CONN_OUTPUT_MAX	(256U * 1024U)
#define VI_CONN_WIDE_MAX	32768U
#define VI_CONN_CLOSE_WAIT_MS	500U

/*
 * One embedded vi tab's connection, shared between the UI thread and the
 * stdout reader thread under lock.
 */
struct vi_connection {
	struct terminal_connection connection;
	struct kl_app *app;
	uint32_t number;
	HANDLE process_handle;
	HANDLE stdin_write;
	HANDLE stdout_read;
	HANDLE reader_thread;
	struct kl_mutex lock;
	unsigned references;
	unsigned char *output;
	size_t output_capacity;
	size_t output_start;
	size_t output_length;
	int ended;
	int posted;
};

static uint32_t vi_next_number = 1U;

static size_t vi_conn_read(struct terminal_connection *connection, unsigned char *buffer, size_t size);
static int vi_conn_write(struct terminal_connection *connection, const unsigned char *bytes, size_t size, size_t *written);
static int vi_conn_resize(struct terminal_connection *connection, unsigned columns, unsigned rows, unsigned width, unsigned height);
static int vi_conn_mode(const struct terminal_connection *connection, unsigned *mode);
static unsigned vi_conn_event(struct terminal_connection *connection, const struct kl_app_event *event);
static unsigned vi_conn_pending(const struct terminal_connection *connection);
static void vi_conn_close(struct terminal_connection *connection);
static DWORD WINAPI vi_reader_main(LPVOID param);
static void vi_conn_release(struct vi_connection *conn);
static int vi_utf8_to_wide(const char *utf8, wchar_t **wide);
static int vi_spawn_child(const char *file_path, unsigned columns, unsigned rows, int ambiguous_wide, HANDLE *process_out, HANDLE *stdin_out, HANDLE *stdout_out);
static void vi_make_title(const char *path, char *title, size_t size);

static const struct terminal_connection_ops vi_conn_ops = {
	vi_conn_read,
	vi_conn_write,
	vi_conn_resize,
	vi_conn_mode,
	vi_conn_event,
	vi_conn_pending,
	vi_conn_close,
	NULL,
	NULL,
	NULL
};

/*
 * Opens a file in vi.exe --mikuterm and returns a new terminal_connection.
 */
int
mikuterm_vi_open(
	struct kl_app *app,
	struct terminal_window *window,
	unsigned columns,
	unsigned rows,
	int ambiguous_wide,
	const char *path,
	char *title,
	size_t title_size,
	struct terminal_connection **connection)
{
	char chosen[MIKUTERM_PATH_MAX];
	struct vi_connection *conn;
	struct kl_window *owner;
	HANDLE process_handle;
	HANDLE stdin_write;
	HANDLE stdout_read;
	int error;

	/* Validates required output parameters. */
	if (app == NULL || connection == NULL)
		return EINVAL;
	*connection = NULL;

	/* Prompts the user for a file when no path was given. */
	if (path == NULL) {
		owner = NULL;
		if (window != NULL)
			owner = window->kui;
		error = kl_file_open_dialog(owner, chosen, sizeof(chosen));
		if (error != 0)
			return error;
		path = chosen;
	}

	/* Allocates the connection descriptor and output ring buffer. */
	conn = calloc(1U, sizeof(*conn));
	if (conn == NULL)
		return ENOMEM;

	/* Allocates the output ring buffer. */
	conn->output = malloc(VI_CONN_OUTPUT_MAX);
	if (conn->output == NULL) {
		free(conn);
		return ENOMEM;
	}

	/* Initializes the connection lock. */
	error = kl_mutex_init(&conn->lock);
	if (error != 0) {
		free(conn->output);
		free(conn);
		return error;
	}

	/* Spawns vi.exe --mikuterm with anonymous stdin/stdout pipes. */
	error = vi_spawn_child(path, columns, rows, ambiguous_wide, &process_handle, &stdin_write, &stdout_read);
	if (error != 0) {
		kl_mutex_destroy(&conn->lock);
		free(conn->output);
		free(conn);
		return error;
	}

	/* Initializes connection fields with two references (tab and reader thread). */
	conn->connection.ops = &vi_conn_ops;
	conn->app = app;
	conn->number = vi_next_number++;
	conn->process_handle = process_handle;
	conn->stdin_write = stdin_write;
	conn->stdout_read = stdout_read;
	conn->output_capacity = VI_CONN_OUTPUT_MAX;
	conn->references = 2U;

	/* Starts the stdout relay thread. */
	conn->reader_thread = CreateThread(NULL, 0U, vi_reader_main, conn, 0U, NULL);
	if (conn->reader_thread == NULL) {
		CloseHandle(stdin_write);
		CloseHandle(stdout_read);
		(void)TerminateProcess(process_handle, 1U);
		CloseHandle(process_handle);
		kl_mutex_destroy(&conn->lock);
		free(conn->output);
		free(conn);
		return EIO;
	}

	/* Formats the tab title and returns the connection. */
	vi_make_title(path, title, title_size);
	*connection = &conn->connection;
	return 0;
}

/*
 * Reads up to size bytes from the vi output ring.
 */
static size_t
vi_conn_read(
	struct terminal_connection *connection,
	unsigned char *buffer,
	size_t size)
{
	struct vi_connection *conn;
	size_t taken;
	size_t first;

	/* Copies available bytes from the ring under lock. */
	conn = (struct vi_connection *)connection;
	kl_mutex_lock(&conn->lock);

	taken = conn->output_length;
	if (taken > size)
		taken = size;
	if (taken > 0U) {
		first = conn->output_capacity - conn->output_start;
		if (first > taken)
			first = taken;
		memcpy(buffer, conn->output + conn->output_start, first);
		if (taken > first)
			memcpy(buffer + first, conn->output, taken - first);
		conn->output_start = (conn->output_start + taken) % conn->output_capacity;
		conn->output_length -= taken;
	}

	kl_mutex_unlock(&conn->lock);
	return taken;
}

/*
 * Writes keystrokes to vi.exe's stdin pipe.
 */
static int
vi_conn_write(
	struct terminal_connection *connection,
	const unsigned char *bytes,
	size_t size,
	size_t *written)
{
	struct vi_connection *conn;
	HANDLE pipe;
	DWORD chunk;
	DWORD sent;
	BOOL ok;

	/* Captures the stdin pipe handle under lock. */
	conn = (struct vi_connection *)connection;
	*written = 0U;

	/* Checks whether the connection is still open. */
	kl_mutex_lock(&conn->lock);
	pipe = conn->stdin_write;
	if (conn->ended || pipe == NULL) {
		kl_mutex_unlock(&conn->lock);
		return EPIPE;
	}

	kl_mutex_unlock(&conn->lock);

	/* Writes all bytes to vi.exe's stdin pipe. */
	while (*written < size) {
		chunk = (DWORD)(size - *written);
		sent = 0U;
		ok = WriteFile(pipe, bytes + *written, chunk, &sent, NULL);
		if (!ok || sent == 0U)
			return EPIPE;
		*written += (size_t)sent;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Sends an in-band CSI 8 ; rows ; cols t resize report to vi.exe.
 */
static int
vi_conn_resize(
	struct terminal_connection *connection,
	unsigned columns,
	unsigned rows,
	unsigned width,
	unsigned height)
{
	char sequence[48];
	int length;
	size_t written;

	/* Pixel dimensions are unused by vi. */
	(void)width;
	(void)height;

	/* Ignores zero-sized initial notifications. */
	if (columns == 0U || rows == 0U)
		return 0;

	/* Formats the CSI 8 ; rows ; cols t sequence. */
	length = snprintf(sequence, sizeof(sequence), "\x1b[8;%u;%ut", rows, columns);
	if (length <= 0 || (size_t)length >= sizeof(sequence))
		return EINVAL;

	/* Sends the resize report over stdin. */
	return vi_conn_write(connection, (const unsigned char *)sequence, (size_t)length, &written);
}

/*
 * Reports raw terminal mode (no local echo, no line buffering).
 */
static int
vi_conn_mode(
	const struct terminal_connection *connection,
	unsigned *mode)
{
	(void)connection;
	*mode = 0U;
	return 0;
}

/*
 * Checks whether an application post belongs to this vi connection.
 */
static unsigned
vi_conn_event(
	struct terminal_connection *connection,
	const struct kl_app_event *event)
{
	struct vi_connection *conn;
	unsigned ready;

	/* Filters posts belonging to this connection instance. */
	conn = (struct vi_connection *)connection;
	if (event->kind != KL_APP_POST || event->code != VI_CONN_POST)
		return 0U;
	if ((uintptr_t)event->data != (uintptr_t)conn->number)
		return 0U;

	/* Clears the post flag and reports ready state bits. */
	kl_mutex_lock(&conn->lock);
	conn->posted = 0;
	ready = TERMINAL_READY_INPUT;
	if (conn->output_length != 0U)
		ready |= TERMINAL_READY_OUTPUT;
	if (conn->ended)
		ready |= TERMINAL_READY_EXIT;
	kl_mutex_unlock(&conn->lock);

	/* Returns the ready mask. */
	return ready;
}

/*
 * Reports currently buffered output or process exit status.
 */
static unsigned
vi_conn_pending(
	const struct terminal_connection *connection)
{
	const struct vi_connection *conn;
	unsigned ready;

	/* Queries buffered output and exit status under lock. */
	conn = (const struct vi_connection *)connection;
	kl_mutex_lock((struct kl_mutex *)&conn->lock);
	ready = 0U;
	if (conn->output_length != 0U)
		ready |= TERMINAL_READY_OUTPUT;
	if (conn->ended)
		ready |= TERMINAL_READY_EXIT;
	kl_mutex_unlock((struct kl_mutex *)&conn->lock);

	/* Returns the pending ready bits. */
	return ready;
}

/*
 * Closes the tab's side of the connection, sending EOF to vi.exe.
 */
static void
vi_conn_close(
	struct terminal_connection *connection)
{
	struct vi_connection *conn;
	HANDLE stdin_pipe;
	HANDLE proc;
	DWORD waited;

	/* Marks the connection ended and detaches owned handles. */
	conn = (struct vi_connection *)connection;
	kl_mutex_lock(&conn->lock);
	conn->ended = 1;
	stdin_pipe = conn->stdin_write;
	conn->stdin_write = NULL;
	proc = conn->process_handle;
	conn->process_handle = NULL;
	kl_mutex_unlock(&conn->lock);

	/* Closes stdin to trigger EOF, then waits briefly before terminating. */
	if (stdin_pipe != NULL)
		CloseHandle(stdin_pipe);
	if (proc != NULL) {
		waited = WaitForSingleObject(proc, VI_CONN_CLOSE_WAIT_MS);
		if (waited == WAIT_TIMEOUT)
			(void)TerminateProcess(proc, 1U);
		CloseHandle(proc);
	}

	/* Releases the tab's reference. */
	vi_conn_release(conn);
}

/*
 * Reader thread: drains vi.exe stdout pipe into the output ring and wakes UI.
 */
static DWORD WINAPI
vi_reader_main(
	LPVOID param)
{
	struct vi_connection *conn;
	unsigned char buffer[4096];
	DWORD got;
	size_t offset;
	size_t space;
	size_t chunk;
	size_t tail;
	size_t first;
	int should_post;
	int running;
	BOOL ok;

	/* Reads stdout chunks until EOF or connection close. */
	conn = (struct vi_connection *)param;
	running = 1;
	while (running) {
		got = 0U;
		ok = ReadFile(conn->stdout_read, buffer, (DWORD)sizeof(buffer), &got, NULL);
		if (!ok || got == 0U)
			break;

		/* Copies the chunk into the ring buffer, waiting if full. */
		offset = 0U;
		while (offset < (size_t)got) {
			kl_mutex_lock(&conn->lock);
			if (conn->ended) {
				kl_mutex_unlock(&conn->lock);
				running = 0;
				break;
			}

			/* Waits for ring space before copying the next chunk. */
			space = conn->output_capacity - conn->output_length;
			if (space == 0U) {
				kl_mutex_unlock(&conn->lock);
				Sleep(1U);
				continue;
			}

			/* Copies up to the available ring capacity. */
			chunk = (size_t)got - offset;
			if (chunk > space)
				chunk = space;
			tail = (conn->output_start + conn->output_length) % conn->output_capacity;
			first = conn->output_capacity - tail;
			if (first > chunk)
				first = chunk;
			memcpy(conn->output + tail, buffer + offset, first);
			if (chunk > first)
				memcpy(conn->output, buffer + offset + first, chunk - first);
			conn->output_length += chunk;
			offset += chunk;
			should_post = !conn->posted;
			conn->posted = 1;
			kl_mutex_unlock(&conn->lock);

			/* Wakes the UI thread if no post is already in flight. */
			if (should_post)
				(void)kl_app_post(conn->app, VI_CONN_POST, (void *)(uintptr_t)conn->number);
		}
	}

	/* Marks the stream ended and notifies the UI thread. */
	kl_mutex_lock(&conn->lock);
	conn->ended = 1;
	should_post = !conn->posted;
	conn->posted = 1;
	kl_mutex_unlock(&conn->lock);

	/* Delivers the final exit notification and releases the thread reference. */
	if (should_post)
		(void)kl_app_post(conn->app, VI_CONN_POST, (void *)(uintptr_t)conn->number);
	vi_conn_release(conn);
	return 0U;
}

/*
 * Decrements reference count and frees conn when both tab and thread release.
 */
static void
vi_conn_release(
	struct vi_connection *conn)
{
	unsigned refs;
	HANDLE thread_handle;
	HANDLE stdout_pipe;

	/* Drops one reference under lock. */
	kl_mutex_lock(&conn->lock);
	conn->references--;
	refs = conn->references;
	kl_mutex_unlock(&conn->lock);
	if (refs != 0U)
		return;

	/* Frees all resources once the last holder releases. */
	thread_handle = conn->reader_thread;
	stdout_pipe = conn->stdout_read;
	if (stdout_pipe != NULL)
		CloseHandle(stdout_pipe);
	if (thread_handle != NULL)
		CloseHandle(thread_handle);
	kl_mutex_destroy(&conn->lock);
	free(conn->output);
	free(conn);
}

/*
 * Converts UTF-8 to a newly allocated UTF-16 string.
 */
static int
vi_utf8_to_wide(
	const char *utf8,
	wchar_t **wide)
{
	int units;
	wchar_t *out;
	int converted;

	/* Rejects null strings. */
	if (utf8 == NULL)
		return EINVAL;

	/* Measures the UTF-16 length. */
	units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
	if (units <= 0 || (unsigned)units > VI_CONN_WIDE_MAX)
		return EINVAL;

	/* Allocates and converts into the UTF-16 buffer. */
	out = malloc((size_t)units * sizeof(wchar_t));
	if (out == NULL)
		return ENOMEM;
	converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, units);
	if (converted <= 0) {
		free(out);
		return EINVAL;
	}

	/* Succeeded. */
	*wide = out;
	return 0;
}

/*
 * Spawns vi.exe --mikuterm -- <file_path> with redirected stdin/stdout pipes.
 */
static int
vi_spawn_child(
	const char *file_path,
	unsigned columns,
	unsigned rows,
	int ambiguous_wide,
	HANDLE *process_out,
	HANDLE *stdin_out,
	HANDLE *stdout_out)
{
	char exe_utf8[MIKUTERM_PATH_MAX];
	wchar_t *exe_wide;
	wchar_t *file_wide;
	wchar_t *cmd;
	size_t cmd_len;
	SECURITY_ATTRIBUTES sa;
	HANDLE in_r;
	HANDLE in_w;
	HANDLE out_r;
	HANDLE out_w;
	STARTUPINFOEXW startup;
	LPPROC_THREAD_ATTRIBUTE_LIST attrs;
	SIZE_T attr_size;
	HANDLE handed[2];
	PROCESS_INFORMATION proc;
	wchar_t env_cols[32];
	wchar_t env_rows[32];
	const wchar_t *ambiguous_val;
	unsigned eff_cols;
	unsigned eff_rows;
	DWORD exe_attrs;
	BOOL ok;
	int error;

	/* Resolves vi.exe beside mikuterm.exe. */
	error = mikuterm_path(exe_utf8, sizeof(exe_utf8), "vi.exe");
	if (error != 0)
		return error;

	/* Converts executable and target paths to UTF-16. */
	exe_wide = NULL;
	file_wide = NULL;
	error = vi_utf8_to_wide(exe_utf8, &exe_wide);
	if (error != 0)
		return error;
	error = vi_utf8_to_wide(file_path, &file_wide);
	if (error != 0) {
		free(exe_wide);
		return error;
	}

	/* Verifies vi.exe is present in the folder. */
	exe_attrs = GetFileAttributesW(exe_wide);
	if (exe_attrs == INVALID_FILE_ATTRIBUTES) {
		free(file_wide);
		free(exe_wide);
		return ENOENT;
	}

	/* Formats the command line with -- so leading + or - filenames work. */
	cmd_len = wcslen(exe_wide) + wcslen(file_wide) + 32U;
	cmd = malloc(cmd_len * sizeof(wchar_t));
	if (cmd == NULL) {
		free(file_wide);
		free(exe_wide);
		return ENOMEM;
	}

	/* Formats the child command line with --mikuterm and target file. */
	(void)swprintf(cmd, cmd_len, L"\"%ls\" --mikuterm -- \"%ls\"", exe_wide, file_wide);
	free(file_wide);

	/* Creates inheritable anonymous pipes for stdin and stdout. */
	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	in_r = NULL;
	in_w = NULL;
	out_r = NULL;
	out_w = NULL;
	ok = CreatePipe(&in_r, &in_w, &sa, 0U);
	if (ok)
		ok = CreatePipe(&out_r, &out_w, &sa, 0U);
	if (!ok) {
		if (in_r != NULL) CloseHandle(in_r);
		if (in_w != NULL) CloseHandle(in_w);
		if (out_r != NULL) CloseHandle(out_r);
		if (out_w != NULL) CloseHandle(out_w);
		free(cmd);
		free(exe_wide);
		return EIO;
	}

	/* Prevents parent pipe ends from being inherited. */
	(void)SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0U);
	(void)SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0U);

	/* Allocates the attribute list to inherit only the child pipe handles. */
	attr_size = 0U;
	(void)InitializeProcThreadAttributeList(NULL, 1U, 0U, &attr_size);
	attrs = malloc(attr_size);
	if (attrs == NULL) {
		CloseHandle(in_r);
		CloseHandle(in_w);
		CloseHandle(out_r);
		CloseHandle(out_w);
		free(cmd);
		free(exe_wide);
		return ENOMEM;
	}

	/* Initializes the attribute list with the two child pipe handles. */
	ok = InitializeProcThreadAttributeList(attrs, 1U, 0U, &attr_size);
	if (!ok) {
		free(attrs);
		CloseHandle(in_r);
		CloseHandle(in_w);
		CloseHandle(out_r);
		CloseHandle(out_w);
		free(cmd);
		free(exe_wide);
		return EIO;
	}

	/* Binds the inherited handle list to stdin and stdout pipes. */
	handed[0] = in_r;
	handed[1] = out_w;
	ok = UpdateProcThreadAttribute(attrs, 0U, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handed, sizeof(handed), NULL, NULL);
	if (!ok) {
		DeleteProcThreadAttributeList(attrs);
		free(attrs);
		CloseHandle(in_r);
		CloseHandle(in_w);
		CloseHandle(out_r);
		CloseHandle(out_w);
		free(cmd);
		free(exe_wide);
		return EIO;
	}

	/* Sets initial grid dimensions and ambiguous-width flag in child environment. */
	eff_cols = 80U;
	if (columns != 0U)
		eff_cols = columns;
	eff_rows = 24U;
	if (rows != 0U)
		eff_rows = rows;
	ambiguous_val = L"0";
	if (ambiguous_wide)
		ambiguous_val = L"1";
	(void)swprintf(env_cols, sizeof(env_cols) / sizeof(env_cols[0]), L"%u", eff_cols);
	(void)swprintf(env_rows, sizeof(env_rows) / sizeof(env_rows[0]), L"%u", eff_rows);
	(void)SetEnvironmentVariableW(L"COLUMNS", env_cols);
	(void)SetEnvironmentVariableW(L"LINES", env_rows);
	(void)SetEnvironmentVariableW(L"ZEDBSD_AMBIGUOUS_WIDE", ambiguous_val);

	/* Prepares STARTUPINFOEXW with redirected standard handles. */
	memset(&startup, 0, sizeof(startup));
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = in_r;
	startup.StartupInfo.hStdOutput = out_w;
	startup.StartupInfo.hStdError = out_w;
	startup.lpAttributeList = attrs;

	/* Spawns vi.exe without a console window. */
	memset(&proc, 0, sizeof(proc));
	ok = CreateProcessW(exe_wide, cmd, NULL, NULL, TRUE,
	    CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
	    NULL, NULL, &startup.StartupInfo, &proc);

	/* Releases temporary launch resources and child-side pipe ends. */
	DeleteProcThreadAttributeList(attrs);
	free(attrs);
	CloseHandle(in_r);
	CloseHandle(out_w);
	free(cmd);
	free(exe_wide);
	if (!ok) {
		CloseHandle(in_w);
		CloseHandle(out_r);
		return EIO;
	}

	/* Returns the process and parent-side pipe handles. */
	CloseHandle(proc.hThread);
	*process_out = proc.hProcess;
	*stdin_out = in_w;
	*stdout_out = out_r;
	return 0;
}

/*
 * Formats "vi: <basename>" into title.
 */
static void
vi_make_title(
	const char *path,
	char *title,
	size_t size)
{
	const char *slash;
	const char *bslash;
	const char *name;

	/* Ignores empty title buffers. */
	if (title == NULL || size == 0U)
		return;

	/* Extracts the trailing path component. */
	name = path;
	slash = strrchr(path, '/');
	bslash = strrchr(path, '\\');
	if (slash != NULL && (bslash == NULL || slash > bslash))
		name = slash + 1;
	else if (bslash != NULL)
		name = bslash + 1;
	if (name == NULL || name[0] == '\0')
		name = "vi";

	/* Formats the tab title. */
	(void)snprintf(title, size, "vi: %s", name);
}
