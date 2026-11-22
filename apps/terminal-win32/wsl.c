/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Runs Windows' wsl.exe (the one in the system folder, never one beside
 * mikuterm) without a console window and collects what it writes, for
 * the list of WSL's distributions ("-l -q") and a distribution's user
 * ("-d name -e whoami", which also starts the distribution and with it
 * an sshd that systemd starts).  The text is read by launch.c.
 *
 * Only the pipe's end and NUL are handed to wsl.exe: mikuterm's other
 * handles (the sockets) stay its own.  The wait has a limit and gives up
 * when the tab closes; a wsl.exe given up is ended.
 */

#include "mikuterm.h"

#include <keiland/keiland.h>

#include <windows.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The program's name in the system folder. */
#define WSL_PROGRAM		L"\\wsl.exe"

/* The pipe's room, and how long one round of the wait is, in milliseconds. */
#define WSL_PIPE_SIZE		65536U
#define WSL_ROUND_MS		50U

/* The longest command line, in UTF-16 units with its NUL. */
#define WSL_COMMAND_MAX		1024U

/*
 * What wsl.exe wrote: the caller's room, how much is in it, and whether
 * more came than it holds (the rest is read and dropped, so wsl.exe never
 * waits on a full pipe).
 */
struct wsl_output {
	unsigned char *bytes;
	size_t capacity;
	size_t size;
};

static int wsl_command(const char *arguments, wchar_t *program, size_t program_size, wchar_t *command, size_t command_size);
static int wsl_start(wchar_t *program, wchar_t *command, HANDLE write_end, HANDLE nothing, PROCESS_INFORMATION *process);
static void wsl_drain(HANDLE read_end, struct wsl_output *output);

/*
 * Runs wsl.exe with the arguments (UTF-8) and collects what it writes to
 * its output and its errors into output (capacity bytes; *size says how
 * many), with its exit code in *code.  The wait gives up after so many
 * milliseconds, or when stop (with data) says the tab closed.  Returns 0,
 * ENOENT when Windows has no wsl.exe, ETIMEDOUT, ECANCELED, EINVAL for
 * arguments that are not UTF-8 or too long, ENOMEM, or EIO.
 */
int
mikuterm_wsl_run(
	const char *arguments,
	unsigned milliseconds,
	int (*stop)(void *data),
	void *data,
	unsigned char *output,
	size_t capacity,
	size_t *size,
	unsigned long *code)
{
	SECURITY_ATTRIBUTES inherit;
	PROCESS_INFORMATION process;
	struct wsl_output collected;
	wchar_t program[MAX_PATH + 16U];
	wchar_t command[WSL_COMMAND_MAX];
	HANDLE read_end;
	HANDLE write_end;
	HANDLE nothing;
	DWORD found;
	DWORD waited;
	DWORD exit_code;
	BOOL made;
	uint64_t deadline;
	uint64_t now;
	int stopped;
	int error;

	/* Nothing collected yet. */
	*size = 0U;
	*code = 0UL;
	collected.bytes = output;
	collected.capacity = capacity;
	collected.size = 0U;

	/* The program in the system folder, and the command line. */
	error = wsl_command(arguments, program, sizeof(program) / sizeof(program[0]), command, sizeof(command) / sizeof(command[0]));
	if (error != 0)
		return error;

	/* A Windows without WSL has no wsl.exe there. */
	found = GetFileAttributesW(program);
	if (found == INVALID_FILE_ATTRIBUTES)
		return ENOENT;

	/* The pipe wsl.exe writes to; only its writing end is handed down. */
	memset(&inherit, 0, sizeof(inherit));
	inherit.nLength = sizeof(inherit);
	inherit.bInheritHandle = TRUE;
	made = CreatePipe(&read_end, &write_end, &inherit, WSL_PIPE_SIZE);
	if (!made)
		return EIO;
	(void)SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

	/* Its input: nothing to read. */
	nothing = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, NULL);
	if (nothing == INVALID_HANDLE_VALUE) {
		CloseHandle(read_end);
		CloseHandle(write_end);
		return EIO;
	}

	/* The program; the ends it was handed are its own from now on. */
	error = wsl_start(program, command, write_end, nothing, &process);
	CloseHandle(write_end);
	CloseHandle(nothing);
	if (error != 0) {
		CloseHandle(read_end);
		return error;
	}

	/* What it writes, until it ends, the time is up or the tab closes. */
	deadline = kl_clock_us() / 1000U + milliseconds;
	error = 0;
	for (;;) {
		/* What came so far. */
		wsl_drain(read_end, &collected);

		/* The end of the program. */
		waited = WaitForSingleObject(process.hProcess, WSL_ROUND_MS);
		if (waited == WAIT_OBJECT_0)
			break;

		/* The tab that closed, or the time that is up, ends the wait and the program. */
		now = kl_clock_us() / 1000U;
		stopped = 0;
		if (stop != NULL)
			stopped = stop(data);
		if (stopped) {
			error = ECANCELED;
		} else if (now >= deadline) {
			error = ETIMEDOUT;
		}
		if (error != 0) {
			(void)TerminateProcess(process.hProcess, 1U);
			(void)WaitForSingleObject(process.hProcess, WSL_ROUND_MS);
			break;
		}
	}

	/* The last of what it wrote, and its exit code. */
	wsl_drain(read_end, &collected);
	exit_code = 0U;
	made = GetExitCodeProcess(process.hProcess, &exit_code);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	CloseHandle(read_end);

	/* Reports a wait that ended early. */
	if (error != 0)
		return error;
	if (!made)
		return EIO;

	/* Succeeded: what it wrote and how it ended. */
	*size = collected.size;
	*code = (unsigned long)exit_code;
	return 0;
}

/*
 * Makes the program's path (wsl.exe in the system folder) and the command
 * line: the path in quotes, then the arguments.  Returns 0 or EINVAL.
 */
static int
wsl_command(
	const char *arguments,
	wchar_t *program,
	size_t program_size,
	wchar_t *command,
	size_t command_size)
{
	UINT length;
	int written;
	int units;

	/* The system folder, and the program in it. */
	length = GetSystemDirectoryW(program, (UINT)(program_size - 16U));
	if (length == 0U || length >= program_size - 16U)
		return EINVAL;
	wcscpy(program + length, WSL_PROGRAM);

	/* The path in quotes and a space. */
	written = swprintf(command, command_size, L"\"%ls\" ", program);
	if (written < 0 || (size_t)written >= command_size)
		return EINVAL;

	/* The arguments after them, from UTF-8. */
	units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, arguments, -1, command + written, (int)(command_size - (size_t)written));
	if (units <= 0)
		return EINVAL;

	/* Succeeded: the path and the line. */
	return 0;
}

/*
 * Starts the program without a console window, handing it only its
 * output (also its errors) and its input.  Returns 0, ENOMEM or EIO.
 */
static int
wsl_start(
	wchar_t *program,
	wchar_t *command,
	HANDLE write_end,
	HANDLE nothing,
	PROCESS_INFORMATION *process)
{
	STARTUPINFOEXW startup;
	LPPROC_THREAD_ATTRIBUTE_LIST attributes;
	HANDLE handed[2];
	SIZE_T attributes_size;
	BOOL made;

	/* The room for the list of handles handed down. */
	attributes_size = 0U;
	(void)InitializeProcThreadAttributeList(NULL, 1U, 0U, &attributes_size);
	attributes = malloc(attributes_size);
	if (attributes == NULL)
		return ENOMEM;

	/* The list: the pipe's end and NUL, nothing else. */
	made = InitializeProcThreadAttributeList(attributes, 1U, 0U, &attributes_size);
	if (!made) {
		free(attributes);
		return EIO;
	}
	handed[0] = write_end;
	handed[1] = nothing;
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
	startup.StartupInfo.hStdInput = nothing;
	startup.StartupInfo.hStdOutput = write_end;
	startup.StartupInfo.hStdError = write_end;
	startup.lpAttributeList = attributes;

	/* The program, without a console window. */
	memset(process, 0, sizeof(*process));
	made = CreateProcessW(program, command, NULL, NULL, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &startup.StartupInfo, process);
	DeleteProcThreadAttributeList(attributes);
	free(attributes);
	if (!made)
		return EIO;

	/* Succeeded: it runs. */
	return 0;
}

/* Reads what waits in the pipe without waiting for more; what does not fit is dropped. */
static void
wsl_drain(
	HANDLE read_end,
	struct wsl_output *output)
{
	unsigned char spill[4096];
	unsigned char *into;
	DWORD waiting;
	DWORD wanted;
	DWORD got;
	BOOL peeked;
	BOOL read;

	/* Until the pipe is empty or broken. */
	for (;;) {
		/* How much waits. */
		waiting = 0U;
		peeked = PeekNamedPipe(read_end, NULL, 0U, NULL, &waiting, NULL);
		if (!peeked || waiting == 0U)
			return;

		/* Into the room left, or into the spill when there is none. */
		into = spill;
		wanted = sizeof(spill);
		if (output->size < output->capacity) {
			into = output->bytes + output->size;
			wanted = (DWORD)(output->capacity - output->size);
		}
		if (wanted > waiting)
			wanted = waiting;
		got = 0U;
		read = ReadFile(read_end, into, wanted, &got, NULL);
		if (!read || got == 0U)
			return;

		/* Kept when it went into the room. */
		if (into != spill)
			output->size += got;
	}
}
