/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Windows' part of mikuterm's X server process (x11-spawn.h): the child
 * started suspended at a low integrity level where Windows allows it, in
 * a Job object that ends it with mikuterm, given its end of the link
 * through a pipe; its end, and the right to bring its windows forward.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include "x11-host.h"
#include "x11-spawn.h"

#include <keiland/keiland_socket.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The command line's switch of the child (X11_SPAWN_SWITCH), after the program's quoted path. */
#define SPAWN_SWITCH		L" --x11-server"

static HANDLE spawn_low_token(void);
static void spawn_kill(struct x11_host *host);

/*
 * Keeps the pair's listener from being shared: its address is
 * exclusively its own while the pair is made.
 */
void
x11_spawn_guard(
	struct kl_socket *listener)
{
	BOOL exclusive;

	/* SO_EXCLUSIVEADDRUSE; without it the accepted end is still checked. */
	exclusive = TRUE;
	(void)setsockopt((SOCKET)kl_socket_native(listener), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive));
}

/*
 * Starts mikuterm.exe --x11-server: suspended, at a low integrity level
 * where Windows allows it, inheriting only the reading end of a pipe
 * (its standard input), put in a Job object that ends it with mikuterm,
 * then resumed; the socket's end is duplicated into it and written to the
 * pipe.  mikuterm keeps its own descriptor of the end (the child's copy
 * is made only when the child opens it).  Returns 0, or an errno value.
 */
int
x11_spawn_start(
	struct x11_host *host,
	struct kl_socket *child_end)
{
	WCHAR program[MAX_PATH];
	WCHAR command[MAX_PATH + 64];
	SECURITY_ATTRIBUTES inherit;
	HANDLE pipe_read;
	HANDLE pipe_write;
	SIZE_T list_size;
	LPPROC_THREAD_ATTRIBUTE_LIST list;
	STARTUPINFOEXW start;
	PROCESS_INFORMATION process;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
	WSAPROTOCOL_INFOW information;
	HANDLE token;
	DWORD length;
	DWORD written;
	BOOL done;
	int low;

	/* The program and its command line. */
	length = GetModuleFileNameW(NULL, program, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
		return ENOENT;
	(void)_snwprintf(command, sizeof(command) / sizeof(command[0]), L"\"%ls\"%ls", program, SPAWN_SWITCH);
	command[sizeof(command) / sizeof(command[0]) - 1] = L'\0';

	/* The pipe of the socket's description: only its reading end is inherited. */
	inherit.nLength = sizeof(inherit);
	inherit.lpSecurityDescriptor = NULL;
	inherit.bInheritHandle = TRUE;
	done = CreatePipe(&pipe_read, &pipe_write, &inherit, 0);
	if (!done)
		return EIO;
	(void)SetHandleInformation(pipe_write, HANDLE_FLAG_INHERIT, 0);

	/* The attribute that limits the inherited handles to the pipe's end. */
	list_size = 0;
	(void)InitializeProcThreadAttributeList(NULL, 1, 0, &list_size);
	list = malloc(list_size);
	if (list == NULL) {
		CloseHandle(pipe_read);
		CloseHandle(pipe_write);
		return ENOMEM;
	}
	done = InitializeProcThreadAttributeList(list, 1, 0, &list_size);
	if (done)
		done = UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &pipe_read, sizeof(pipe_read), NULL, NULL);
	if (!done) {
		free(list);
		CloseHandle(pipe_read);
		CloseHandle(pipe_write);
		return EIO;
	}

	/* The start: the pipe's end as standard input, nothing else. */
	memset(&start, 0, sizeof(start));
	start.StartupInfo.cb = sizeof(start);
	start.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	start.StartupInfo.hStdInput = pipe_read;
	start.StartupInfo.hStdOutput = NULL;
	start.StartupInfo.hStdError = NULL;
	start.lpAttributeList = list;

	/* Suspended, at a low integrity level where the token can be made, else as mikuterm runs. */
	low = 0;
	token = spawn_low_token();
	done = FALSE;
	if (token != NULL) {
		done = CreateProcessAsUserW(token, program, command, NULL, NULL, TRUE,
		    CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &start.StartupInfo, &process);
		CloseHandle(token);
		if (done)
			low = 1;
	}
	if (!done)
		done = CreateProcessW(program, command, NULL, NULL, TRUE, CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
		    NULL, NULL, &start.StartupInfo, &process);
	DeleteProcThreadAttributeList(list);
	free(list);
	CloseHandle(pipe_read);
	if (!done) {
		CloseHandle(pipe_write);
		return EIO;
	}

	/* The Job that ends the child with mikuterm, made once per connection. */
	if (host->job == NULL) {
		host->job = CreateJobObjectW(NULL, NULL);
		if (host->job != NULL) {
			memset(&limits, 0, sizeof(limits));
			limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			(void)SetInformationJobObject(host->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
		}
	}
	if (host->job == NULL || !AssignProcessToJobObject(host->job, process.hProcess)) {
		TerminateProcess(process.hProcess, 1);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		CloseHandle(pipe_write);
		return EIO;
	}
	ResumeThread(process.hThread);
	CloseHandle(process.hThread);
	host->process = process.hProcess;
	host->pid = process.dwProcessId;

	/* The socket's end, duplicated into the child, written to its pipe. */
	if (WSADuplicateSocketW((SOCKET)kl_socket_native(child_end), process.dwProcessId, &information) != 0) {
		CloseHandle(pipe_write);
		spawn_kill(host);
		return EIO;
	}
	done = WriteFile(pipe_write, &information, sizeof(information), &written, NULL);
	CloseHandle(pipe_write);
	if (!done || written != sizeof(information)) {
		spawn_kill(host);
		return EIO;
	}

	/* Succeeded: the child runs. */
	(void)low;
	return 0;
}

/*
 * Makes a primary token of mikuterm's at the low integrity level (NULL
 * when Windows does not allow it), so the child, which reads the remote
 * host's bytes, cannot write to the user's files or reach mikuterm's
 * memory.
 */
static HANDLE
spawn_low_token(void)
{
	HANDLE own;
	HANDLE token;
	SID_IDENTIFIER_AUTHORITY authority = { SECURITY_MANDATORY_LABEL_AUTHORITY };
	PSID low;
	TOKEN_MANDATORY_LABEL label;
	BOOL done;

	/* mikuterm's token, duplicated as a primary token. */
	done = OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY, &own);
	if (!done)
		return NULL;
	done = DuplicateTokenEx(own, 0, NULL, SecurityImpersonation, TokenPrimary, &token);
	CloseHandle(own);
	if (!done)
		return NULL;

	/* Its integrity level lowered. */
	done = AllocateAndInitializeSid(&authority, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0, &low);
	if (!done) {
		CloseHandle(token);
		return NULL;
	}
	label.Label.Attributes = SE_GROUP_INTEGRITY;
	label.Label.Sid = low;
	done = SetTokenInformation(token, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(low));
	FreeSid(low);
	if (!done) {
		CloseHandle(token);
		return NULL;
	}

	/* Succeeded: the token. */
	return token;
}

/*
 * Ends the child after its link closed: it has X11_SPAWN_END_MS to end
 * by itself, then it is ended.
 */
void
x11_spawn_stop(
	struct x11_host *host)
{
	DWORD waited;

	/* Nothing runs. */
	if (host->process == NULL)
		return;

	/* The moment, then the end. */
	waited = WaitForSingleObject(host->process, X11_SPAWN_END_MS);
	if (waited != WAIT_OBJECT_0)
		TerminateProcess(host->process, 1);
	CloseHandle(host->process);
	host->process = NULL;
	host->pid = 0;
}

/*
 * Lets the child bring its windows to the front (the user just used the
 * tab of the connection, which is mikuterm's foreground right).
 */
void
x11_host_foreground(
	const struct x11_host *host)
{
	/* Only a running child. */
	if (host->process == NULL)
		return;

	(void)AllowSetForegroundWindow((DWORD)host->pid);
}

/* Ends a child whose start failed half way, at once. */
static void
spawn_kill(
	struct x11_host *host)
{
	/* Nothing runs. */
	if (host->process == NULL)
		return;

	/* Ended and forgotten. */
	TerminateProcess(host->process, 1);
	CloseHandle(host->process);
	host->process = NULL;
	host->pid = 0;
}
