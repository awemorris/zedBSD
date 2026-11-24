/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Windows terminal and pipe backend for vi.
 *
 * Supports both standalone execution in a Windows Console (using Virtual
 * Terminal input and output modes) and child-process execution under a
 * parent terminal over anonymous pipes (using VT100 sequences and in-band
 * CSI 8 ; rows ; cols t resize notifications).
 */

#include "userland/base/vi/vi.h"

#include <windows.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004U
#endif
#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008U
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200U
#endif

/*
 * Fallback VT100 capability strings used on Windows.
 */
struct win32_capability {
	const char *value;
	size_t field;
};

static const struct win32_capability win32_capabilities[] = {
	{ "\033[%i%p1%d;%p2%dH", offsetof(struct vi_terminal, move_string) },
	{ "\033[K", offsetof(struct vi_terminal, clear_line_string) },
	{ "\033[H\033[2J", offsetof(struct vi_terminal, clear_string) },
	{ "\033[?1049h", offsetof(struct vi_terminal, enter_string) },
	{ "\033[?1049l", offsetof(struct vi_terminal, leave_string) },
	{ "\033[?25l", offsetof(struct vi_terminal, hide_cursor_string) },
	{ "\033[?25h", offsetof(struct vi_terminal, show_cursor_string) },
	{ "\033[m", offsetof(struct vi_terminal, normal_string) },
	{ "\007", offsetof(struct vi_terminal, bell_string) }
};

/*
 * Pointer to the active terminal for console control events.
 */
static struct vi_terminal *win32_active_terminal;

static BOOL WINAPI win32_ctrl_handler(DWORD ctrl_type);
static int win32_read_console_byte(struct vi_terminal *terminal, int wait, unsigned char *byte);
static int win32_read_pipe_byte(struct vi_terminal *terminal, int wait, unsigned char *byte);
static int win32_env_int(const char *name, int fallback);

/*
 * Opens the terminal on Windows (console or pipe).
 */
int
vi_terminal_open(
	struct vi_terminal *terminal)
{
	HANDLE in_handle;
	HANDLE out_handle;
	DWORD mode;
	int mikuterm_mode;
	size_t index;
	char *field;

	mikuterm_mode = terminal->mikuterm_mode;
	memset(terminal, 0, sizeof(*terminal));
	terminal->mikuterm_mode = mikuterm_mode;

	in_handle = GetStdHandle(STD_INPUT_HANDLE);
	out_handle = GetStdHandle(STD_OUTPUT_HANDLE);
	if (in_handle == INVALID_HANDLE_VALUE || in_handle == NULL ||
	    out_handle == INVALID_HANDLE_VALUE || out_handle == NULL)
		return ENOTTY;

	terminal->input_handle = in_handle;
	terminal->output_handle = out_handle;

	/* Pipe mode when --mikuterm was passed or standard handles are not consoles. */
	if (mikuterm_mode ||
	    !GetConsoleMode(in_handle, &mode) ||
	    !GetConsoleMode(out_handle, &mode)) {
		terminal->pipe_mode = 1;
	}

	/* Loads VT100 control strings. */
	for (index = 0U; index < sizeof(win32_capabilities) / sizeof(win32_capabilities[0]); index++) {
		field = (char *)terminal + win32_capabilities[index].field;
		strcpy(field, win32_capabilities[index].value);
	}

	/* Learns the initial window size. */
	terminal->pipe_rows = win32_env_int("LINES", 24);
	terminal->pipe_columns = win32_env_int("COLUMNS", 80);
	vi_terminal_measure(terminal);

	return 0;
}

/*
 * Puts the Windows console or pipe into raw VT editing mode.
 */
int
vi_terminal_enter(
	struct vi_terminal *terminal)
{
	HANDLE in_handle;
	HANDLE out_handle;
	DWORD in_mode;
	DWORD out_mode;

	in_handle = (HANDLE)terminal->input_handle;
	out_handle = (HANDLE)terminal->output_handle;

	if (!terminal->pipe_mode) {
		if (!terminal->modes_saved) {
			if (!GetConsoleMode(in_handle, &terminal->saved_input_mode) ||
			    !GetConsoleMode(out_handle, &terminal->saved_output_mode))
				return EIO;
			terminal->saved_input_cp = (unsigned)GetConsoleCP();
			terminal->saved_output_cp = (unsigned)GetConsoleOutputCP();
			terminal->modes_saved = 1;
		}

		in_mode = ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;
		out_mode = terminal->saved_output_mode |
		    ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;

		(void)SetConsoleCP(CP_UTF8);
		(void)SetConsoleOutputCP(CP_UTF8);
		if (!SetConsoleMode(in_handle, in_mode))
			return EIO;
		if (!SetConsoleMode(out_handle, out_mode))
			return EIO;
	}

	win32_active_terminal = terminal;
	(void)SetConsoleCtrlHandler(win32_ctrl_handler, TRUE);

	terminal->raw = 1;
	vi_terminal_command(terminal, terminal->enter_string);
	vi_terminal_flush(terminal);
	terminal->screen_entered = 1;

	return 0;
}

/*
 * Restores the Windows console mode and leaves the alternate screen.
 */
void
vi_terminal_leave(
	struct vi_terminal *terminal)
{
	if (terminal->screen_entered) {
		vi_terminal_command(terminal, terminal->normal_string);
		vi_terminal_move(terminal, terminal->rows - 1, 0);
		vi_terminal_command(terminal, terminal->clear_line_string);
		vi_terminal_command(terminal, terminal->show_cursor_string);
		vi_terminal_command(terminal, terminal->leave_string);
		vi_terminal_flush(terminal);
		terminal->screen_entered = 0;
	}

	if (terminal->raw) {
		if (!terminal->pipe_mode && terminal->modes_saved) {
			(void)SetConsoleMode((HANDLE)terminal->input_handle, terminal->saved_input_mode);
			(void)SetConsoleMode((HANDLE)terminal->output_handle, terminal->saved_output_mode);
			(void)SetConsoleCP((UINT)terminal->saved_input_cp);
			(void)SetConsoleOutputCP((UINT)terminal->saved_output_cp);
		}
		(void)SetConsoleCtrlHandler(win32_ctrl_handler, FALSE);
		if (win32_active_terminal == terminal)
			win32_active_terminal = NULL;
		terminal->raw = 0;
	}
}

/*
 * Updates rows and columns from the console buffer or the last pipe resize.
 */
void
vi_terminal_measure(
	struct vi_terminal *terminal)
{
	CONSOLE_SCREEN_BUFFER_INFO info;

	if (terminal->pipe_mode) {
		terminal->rows = terminal->pipe_rows;
		terminal->columns = terminal->pipe_columns;
	} else if (GetConsoleScreenBufferInfo((HANDLE)terminal->output_handle, &info)) {
		terminal->rows = (int)(info.srWindow.Bottom - info.srWindow.Top + 1);
		terminal->columns = (int)(info.srWindow.Right - info.srWindow.Left + 1);
	} else {
		terminal->rows = 24;
		terminal->columns = 80;
	}

	if (terminal->rows < VI_MINIMUM_ROWS)
		terminal->rows = VI_MINIMUM_ROWS;
	if (terminal->columns < VI_MINIMUM_COLUMNS)
		terminal->columns = VI_MINIMUM_COLUMNS;
}

/*
 * Writes buffered output bytes to the console or stdout pipe.
 */
void
vi_terminal_flush(
	struct vi_terminal *terminal)
{
	size_t written;
	DWORD chunk;
	DWORD count;

	written = 0U;
	while (written < terminal->output_length) {
		chunk = (DWORD)(terminal->output_length - written);
		count = 0U;
		if (!WriteFile((HANDLE)terminal->output_handle,
		    terminal->output_buffer + written, chunk, &count, NULL) ||
		    count == 0U)
			break;
		written += (size_t)count;
	}

	terminal->output_length = 0U;
}

/*
 * Reports whether input is currently waiting without blocking.
 */
int
vi_terminal_input_pending(
	struct vi_terminal *terminal)
{
	DWORD waiting;

	if (terminal->has_pushed_byte)
		return 1;

	if (terminal->pipe_mode) {
		waiting = 0U;
		if (PeekNamedPipe((HANDLE)terminal->input_handle, NULL, 0U, NULL, &waiting, NULL) &&
		    waiting > 0U)
			return 1;
		return 0;
	}

	if (WaitForSingleObject((HANDLE)terminal->input_handle, 0U) == WAIT_OBJECT_0)
		return 1;

	return 0;
}

/*
 * Reads one byte from the Windows console or stdin pipe.
 */
int
vi_win32_read_byte(
	struct vi_terminal *terminal,
	int wait,
	unsigned char *byte)
{
	if (terminal->has_pushed_byte) {
		*byte = terminal->pushed_byte;
		terminal->has_pushed_byte = 0;
		return 1;
	}

	if (terminal->pipe_mode)
		return win32_read_pipe_byte(terminal, wait, byte);

	return win32_read_console_byte(terminal, wait, byte);
}

/*
 * Reads one byte from a Windows console handle, translating resize events.
 */
static int
win32_read_console_byte(
	struct vi_terminal *terminal,
	int wait,
	unsigned char *byte)
{
	HANDLE in_handle;
	DWORD timeout;
	DWORD waited;
	INPUT_RECORD record;
	DWORD count;
	DWORD read_count;
	char ch;

	in_handle = (HANDLE)terminal->input_handle;
	timeout = (wait < 0) ? INFINITE : (DWORD)wait;

	for (;;) {
		if (terminal->stop_pending || terminal->resize_pending)
			return -1;

		waited = WaitForSingleObject(in_handle, timeout);
		if (waited == WAIT_TIMEOUT)
			return 0;
		if (waited != WAIT_OBJECT_0)
			return 0;

		count = 0U;
		if (!PeekConsoleInputW(in_handle, &record, 1U, &count) || count == 0U)
			return 0;

		if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
			(void)ReadConsoleInputW(in_handle, &record, 1U, &count);
			terminal->resize_pending = 1;
			return -1;
		}

		/* Reads one UTF-8 byte from the console VT stream. */
		read_count = 0U;
		if (!ReadFile(in_handle, &ch, 1U, &read_count, NULL))
			return 0;
		if (read_count == 0U) {
			/* Non-character event (e.g. focus or mouse): discard and continue. */
			if (wait >= 0)
				return 0;
			continue;
		}

		*byte = (unsigned char)ch;
		return 1;
	}
}

/*
 * Reads one byte from an anonymous pipe, polling when a bounded wait is given.
 */
static int
win32_read_pipe_byte(
	struct vi_terminal *terminal,
	int wait,
	unsigned char *byte)
{
	HANDLE in_handle;
	DWORD waiting;
	DWORD count;
	ULONGLONG deadline;
	ULONGLONG now;
	char ch;

	in_handle = (HANDLE)terminal->input_handle;

	if (wait >= 0) {
		deadline = GetTickCount64() + (ULONGLONG)wait;
		for (;;) {
			waiting = 0U;
			if (!PeekNamedPipe(in_handle, NULL, 0U, NULL, &waiting, NULL)) {
				DWORD last = GetLastError();
				if (last == ERROR_BROKEN_PIPE || last == ERROR_NOT_SUPPORTED)
					break;
				return 0;
			}
			if (waiting > 0U)
				break;
			now = GetTickCount64();
			if (now >= deadline)
				return 0;
			Sleep(1U);
		}
	}

	count = 0U;
	if (!ReadFile(in_handle, &ch, 1U, &count, NULL) || count == 0U)
		return 0;

	*byte = (unsigned char)ch;
	return 1;
}

/*
 * Handles console control events so Ctrl+C stays an editor key and close
 * triggers a graceful stop.
 */
static BOOL WINAPI
win32_ctrl_handler(
	DWORD ctrl_type)
{
	if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT)
		return TRUE;

	if (ctrl_type == CTRL_CLOSE_EVENT ||
	    ctrl_type == CTRL_LOGOFF_EVENT ||
	    ctrl_type == CTRL_SHUTDOWN_EVENT) {
		if (win32_active_terminal != NULL)
			win32_active_terminal->stop_pending = 1;
		return TRUE;
	}

	return FALSE;
}

/*
 * Parses a positive integer environment variable or returns fallback.
 */
static int
win32_env_int(
	const char *name,
	int fallback)
{
	const char *value;
	long parsed;

	value = getenv(name);
	if (value == NULL || value[0] == '\0')
		return fallback;

	parsed = strtol(value, NULL, 10);
	if (parsed <= 0L || parsed > 10000L)
		return fallback;

	return (int)parsed;
}
