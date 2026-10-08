/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel ps reads, on the host (ws001-p044): /dev/system answers
 * KERN_SYSTEM_GET_PROCESS and KERN_SYSTEM_GET_PROCESS_ARGUMENTS from a fixed
 * table, and the caller is effective user 4242 in session 100.  ps's
 * main.c is built with open, close, ioctl, geteuid and getsid renamed to
 * these (plan/ws001/tests/ps-host-test.sh).
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <uapi/system.h>

/* The descriptor the fake device answers on. */
#define FAKE_DESCRIPTOR	42

/*
 * One process of the table: what GET_PROCESS gives and its command line
 * (NULL: the kernel gives none).
 */
struct fake_process {
	int32_t pid;
	int32_t ppid;
	int32_t group;
	int32_t session;
	uint32_t uid;
	uint32_t gid;
	uint32_t tty;
	uint32_t state;
	uint64_t ticks;
	uint64_t bytes;
	const char *command;
	const char *arguments;
};

/*
 * The table, in the order of the IDs.
 *
 * init; the caller's shell (leader of session 100 on a terminal) and a
 * sleep it started; a root daemon that has run more than a day; a kernel
 * process without a command.
 */
static const struct fake_process fake_processes[] = {
	{ 1, 0, 1, 1, 0, 0, 0, 1, 12, 4096 * 1024, "/sbin/init", "/sbin/init" },
	{ 100, 1, 100, 100, 4242, 4242, 1, 1, 250, 8192 * 1024, "-sh", "-sh" },
	{ 101, 100, 101, 100, 4242, 4242, 1, 1, 9000, 2048 * 1024, "sleep", "sleep 60 abc, def" },
	{ 102, 1, 102, 102, 0, 0, 0, 2, 9000000, 65536 * 1024, "/sbin/daemond", NULL },
	{ 103, 0, 0, 0, 0, 0, 0, 1, 0, 0, "", NULL },
};

int ps_fake_open(const char *path, int flags, ...);
int ps_fake_close(int descriptor);
int ps_fake_ioctl(int descriptor, unsigned long request, ...);
uid_t ps_fake_geteuid(void);
pid_t ps_fake_getsid(pid_t pid);

/* Opens the fake /dev/system. */
int
ps_fake_open(
	const char *path,
	int flags,
	...)
{
	(void)flags;

	/* Only the system's device. */
	if (strcmp(path, "/dev/system") != 0) {
		errno = ENOENT;
		return -1;
	}

	/* Succeeded: the device's descriptor. */
	return FAKE_DESCRIPTOR;
}

/* Closes the fake device. */
int
ps_fake_close(
	int descriptor)
{
	(void)descriptor;

	/* Succeeded. */
	return 0;
}

/* Answers the two process requests from the table. */
int
ps_fake_ioctl(
	int descriptor,
	unsigned long request,
	...)
{
	struct process_info *info;
	struct system_process_arguments *line;
	const struct fake_process *process;
	va_list arguments;
	void *argument;
	size_t index;
	size_t count;

	/* The argument. */
	va_start(arguments, request);
	argument = va_arg(arguments, void *);
	va_end(arguments);
	count = sizeof(fake_processes) / sizeof(fake_processes[0]);

	/* Only the fake device. */
	if (descriptor != FAKE_DESCRIPTOR) {
		errno = EBADF;
		return -1;
	}

	/* The first process after the cursor. */
	if (request == (unsigned long)KERN_SYSTEM_GET_PROCESS) {
		info = argument;
		for (index = 0; index < count; index++) {
			process = &fake_processes[index];
			if (process->pid <= info->pid)
				continue;
			memset(info, 0, sizeof(*info));
			info->pid = process->pid;
			info->ppid = process->ppid;
			info->process_group = process->group;
			info->session = process->session;
			info->uid = process->uid;
			info->gid = process->gid;
			info->has_controlling_terminal = process->tty;
			info->state = process->state;
			info->cpu_ticks = process->ticks;
			info->virtual_bytes = process->bytes;
			strncpy(info->command, process->command, sizeof(info->command) - 1);
			info->version = KERN_SYSTEM_PROCESS_INFO_VERSION;
			info->struct_size = sizeof(*info);
			return 0;
		}
		errno = ENOENT;
		return -1;
	}

	/* The command line of the pid asked for. */
	if (request == (unsigned long)KERN_SYSTEM_GET_PROCESS_ARGUMENTS) {
		line = argument;
		for (index = 0; index < count; index++) {
			process = &fake_processes[index];
			if (process->pid != line->pid || process->arguments == NULL)
				continue;
			strncpy(line->arguments, process->arguments, sizeof(line->arguments) - 1);
			line->version = KERN_SYSTEM_PROCESS_ARGUMENTS_VERSION;
			line->struct_size = sizeof(*line);
			return 0;
		}
		errno = ENOENT;
		return -1;
	}

	/* Any other request. */
	errno = ENOTTY;
	return -1;
}

/* The caller's effective user. */
uid_t
ps_fake_geteuid(
	void)
{
	/* Succeeded: user 4242 (a number no host names). */
	return 4242;
}

/* The caller's session. */
pid_t
ps_fake_getsid(
	pid_t pid)
{
	(void)pid;

	/* Succeeded: session 100. */
	return 100;
}
