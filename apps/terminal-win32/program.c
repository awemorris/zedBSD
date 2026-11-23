/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Other programs mikuterm runs and reads (program.h).
 *
 * A program named without a folder is looked for in the folders of PATH
 * only, never in the current folder or beside mikuterm, so a file left in
 * a folder the user happens to be in is not run in its place; a name
 * without an extension is the .exe of that name, as Windows takes it.
 *
 * A program run is started without a console window, with nothing to
 * read and only the pipe its output (and its errors) goes to handed
 * down: mikuterm's other handles (the sockets) stay its own.  The wait
 * has a limit and gives up when the tab closes; a program given up is
 * ended.
 */

#include "program.h"
#include "mikuterm.h"

#include <keiland/keiland.h>

#include <windows.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The pipe's room, and how long one round of the wait is, in milliseconds. */
#define PROGRAM_PIPE_SIZE	65536U
#define PROGRAM_ROUND_MS	50U

/* The longest command line, and the longest PATH read, in UTF-16 units with their NULs. */
#define PROGRAM_COMMAND_MAX	4096U
#define PROGRAM_SEARCH_MAX	32768U

/* The longest name looked for, in UTF-16 units with its NUL. */
#define PROGRAM_NAME_MAX	260U

/* The extension Windows gives a program named without one. */
#define PROGRAM_EXTENSION	L".exe"

/*
 * What the program wrote: the caller's room and how much is in it.  What
 * comes after the room is full is read and dropped, so the program never
 * waits on a full pipe.
 */
struct program_output {
	unsigned char *bytes;
	size_t capacity;
	size_t size;
};

static int program_is_file(const wchar_t *path);
static int program_try(const wchar_t *folder, size_t folder_length, const wchar_t *name, int extension, wchar_t *path, size_t size);
static int program_command(const wchar_t *program, const char *arguments, wchar_t *command, size_t command_size);
static int program_start(const wchar_t *program, wchar_t *command, HANDLE write_end, HANDLE nothing, PROCESS_INFORMATION *process);
static void program_drain(HANDLE read_end, struct program_output *output);

/*
 * Finds a program (UTF-8 name): a Windows path from a drive or a server
 * is taken when the file is there (with .exe added when it has no
 * extension and is not there as it is), and a name without a folder is
 * looked for in each folder of PATH in turn.  Writes its path into path
 * (size UTF-16 units).  Returns 0, ENOENT when it is nowhere, EINVAL for
 * a name that is not UTF-8 or names a folder relative to the current
 * one, or ENAMETOOLONG.
 */
int
mikuterm_program_find(
	const char *name,
	wchar_t *path,
	size_t size)
{
	wchar_t wide[PROGRAM_NAME_MAX];
	wchar_t *search;
	const wchar_t *folder;
	const wchar_t *end;
	const wchar_t *dot;
	const wchar_t *separator;
	size_t length;
	size_t folder_length;
	DWORD search_length;
	int units;
	int extension;
	int absolute;
	int found;

	/* The name in UTF-16. */
	units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, (int)(sizeof(wide) / sizeof(wide[0])));
	if (units <= 1)
		return EINVAL;

	/* Whether it has an extension of its own (a dot after its last separator). */
	extension = 0;
	separator = wcspbrk(wide, L"\\/:");
	dot = wcsrchr(wide, L'.');
	if (dot != NULL) {
		extension = 1;
		if (wcspbrk(dot, L"\\/") != NULL)
			extension = 0;
	}

	/* A name with a folder in it must be a path from a drive (C:\...) or a server (\\server\...). */
	if (separator != NULL) {
		absolute = mikuterm_path_absolute(name);
		if (!absolute)
			return EINVAL;

		/* The path as it is, then with .exe when it has no extension. */
		found = program_try(NULL, 0U, wide, 1, path, size);
		if (found == 0)
			return 0;
		if (!extension) {
			found = program_try(NULL, 0U, wide, 0, path, size);
			if (found == 0)
				return 0;
		}

		/* Not there. */
		return found;
	}

	/* PATH, read whole. */
	search = malloc(PROGRAM_SEARCH_MAX * sizeof(*search));
	if (search == NULL)
		return ENOMEM;
	search_length = GetEnvironmentVariableW(L"PATH", search, PROGRAM_SEARCH_MAX);
	if (search_length == 0U || search_length >= PROGRAM_SEARCH_MAX) {
		free(search);
		return ENOENT;
	}

	/* Each folder of PATH in turn, separated by ';' and perhaps in quotes. */
	found = ENOENT;
	folder = search;
	while (*folder != L'\0') {
		/* The folder's end. */
		end = wcschr(folder, L';');
		if (end == NULL)
			end = folder + wcslen(folder);
		length = (size_t)(end - folder);

		/* Without the quotes round it. */
		folder_length = length;
		if (folder_length >= 2U && folder[0] == L'"' && folder[folder_length - 1U] == L'"') {
			found = program_try(folder + 1, folder_length - 2U, wide, extension, path, size);
		} else if (folder_length != 0U) {
			found = program_try(folder, folder_length, wide, extension, path, size);
		}
		if (found == 0)
			break;

		/* The next folder. */
		folder = end;
		if (*folder == L';')
			folder++;
	}
	free(search);

	/* Reports a program in no folder. */
	if (found != 0)
		return found;

	/* Succeeded: the program's path. */
	return 0;
}

/*
 * Runs a program (its path) with the arguments (UTF-8) and collects what
 * it writes to its output and its errors into output (capacity bytes;
 * *size says how many), with its exit code in *code.  The wait gives up
 * after so many milliseconds, or when stop (with data) says the tab
 * closed.  Returns 0, ENOENT when the program is not there, ETIMEDOUT,
 * ECANCELED, EINVAL for arguments that are not UTF-8 or too long,
 * ENOMEM, or EIO.
 */
int
mikuterm_program_run(
	const wchar_t *program,
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
	struct program_output collected;
	wchar_t *command;
	HANDLE read_end;
	HANDLE write_end;
	HANDLE nothing;
	DWORD waited;
	DWORD exit_code;
	BOOL code_read;
	BOOL made;
	uint64_t deadline;
	uint64_t now;
	int stopped;
	int there;
	int error;

	/* Nothing collected yet. */
	*size = 0U;
	*code = 0UL;
	collected.bytes = output;
	collected.capacity = capacity;
	collected.size = 0U;

	/* The program must be there. */
	there = program_is_file(program);
	if (!there)
		return ENOENT;

	/* The command line. */
	command = malloc(PROGRAM_COMMAND_MAX * sizeof(*command));
	if (command == NULL)
		return ENOMEM;
	error = program_command(program, arguments, command, PROGRAM_COMMAND_MAX);
	if (error != 0) {
		free(command);
		return error;
	}

	/* The pipe the program writes to; only its writing end is handed down. */
	memset(&inherit, 0, sizeof(inherit));
	inherit.nLength = sizeof(inherit);
	inherit.bInheritHandle = TRUE;
	made = CreatePipe(&read_end, &write_end, &inherit, PROGRAM_PIPE_SIZE);
	if (!made) {
		free(command);
		return EIO;
	}
	(void)SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

	/* Its input: nothing to read. */
	nothing = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, NULL);
	if (nothing == INVALID_HANDLE_VALUE) {
		CloseHandle(read_end);
		CloseHandle(write_end);
		free(command);
		return EIO;
	}

	/* The program; the ends it was handed are its own from now on. */
	error = program_start(program, command, write_end, nothing, &process);
	CloseHandle(write_end);
	CloseHandle(nothing);
	free(command);
	if (error != 0) {
		CloseHandle(read_end);
		return error;
	}

	/* What it writes, until it ends, the time is up or the tab closes. */
	deadline = kl_clock_us() / 1000U + milliseconds;
	error = 0;
	for (;;) {
		/* What came so far. */
		program_drain(read_end, &collected);

		/* The end of the program. */
		waited = WaitForSingleObject(process.hProcess, PROGRAM_ROUND_MS);
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

		/* A wait given up ends the program. */
		if (error != 0) {
			(void)TerminateProcess(process.hProcess, 1U);
			(void)WaitForSingleObject(process.hProcess, PROGRAM_ROUND_MS);
			break;
		}
	}

	/* The last of what it wrote, and its exit code. */
	program_drain(read_end, &collected);
	exit_code = 0U;
	code_read = GetExitCodeProcess(process.hProcess, &exit_code);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	CloseHandle(read_end);

	/* Reports a wait that ended early. */
	if (error != 0)
		return error;

	/* Reports an exit code that could not be read. */
	if (!code_read)
		return EIO;

	/* Succeeded: what it wrote and how it ended. */
	*size = collected.size;
	*code = (unsigned long)exit_code;
	return 0;
}

/* Tells whether a path names a file that is not a folder (1) or not (0). */
static int
program_is_file(
	const wchar_t *path)
{
	DWORD attributes;

	/* What Windows says of the path. */
	attributes = GetFileAttributesW(path);
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return 0;

	/* A folder is no program. */
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)
		return 0;

	/* A file. */
	return 1;
}

/*
 * Tries a program's path: the folder (folder_length units, NULL for a
 * name that is a path), the name, and .exe after it unless extension
 * says the name has its own.  Writes it into path (size units) when the
 * file is there.  Returns 0, ENOENT or ENAMETOOLONG.
 */
static int
program_try(
	const wchar_t *folder,
	size_t folder_length,
	const wchar_t *name,
	int extension,
	wchar_t *path,
	size_t size)
{
	size_t used;
	size_t name_length;
	size_t added_length;
	int there;

	/* The folder and a separator, then the name and .exe. */
	name_length = wcslen(name);
	added_length = 0U;
	if (!extension)
		added_length = wcslen(PROGRAM_EXTENSION);
	if (folder_length + 1U + name_length + added_length + 1U > size)
		return ENAMETOOLONG;

	/* The folder, with a separator after it unless it ends with one. */
	used = 0U;
	if (folder != NULL) {
		memcpy(path, folder, folder_length * sizeof(*path));
		used = folder_length;
		if (used != 0U && path[used - 1U] != L'\\' && path[used - 1U] != L'/') {
			path[used] = L'\\';
			used++;
		}
	}

	/* The name, and .exe after it. */
	memcpy(path + used, name, (name_length + 1U) * sizeof(*path));
	if (!extension)
		wcscat(path, PROGRAM_EXTENSION);

	/* A file must be there. */
	there = program_is_file(path);
	if (!there)
		return ENOENT;

	/* Succeeded: the path. */
	return 0;
}

/*
 * Makes the command line: the program's path in quotes, a space, then
 * the arguments.  Returns 0 or EINVAL.
 */
static int
program_command(
	const wchar_t *program,
	const char *arguments,
	wchar_t *command,
	size_t command_size)
{
	int written;
	int units;

	/* The path in quotes and a space. */
	written = swprintf(command, command_size, L"\"%ls\" ", program);
	if (written < 0 || (size_t)written >= command_size)
		return EINVAL;

	/* The arguments after them, from UTF-8. */
	units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, arguments, -1, command + written, (int)(command_size - (size_t)written));
	if (units <= 0)
		return EINVAL;

	/* Succeeded: the line. */
	return 0;
}

/*
 * Starts the program without a console window, handing it only its
 * output (also its errors) and its input.  Returns 0, ENOMEM or EIO.
 */
static int
program_start(
	const wchar_t *program,
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

	/* The two handles in the list. */
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
program_drain(
	HANDLE read_end,
	struct program_output *output)
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

		/* No more than waits, so the read never waits. */
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
