/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <zedbsd/process.h>
#include <zedbsd/console.h>
#include <zedbsd/system.h>
#include "userland/sh/applet.h"
#include "userland/sh/builtins.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>

#define LINE_MAX 256
#define ARG_MAX 20
#define SOURCE_MAX 8192

static int command_background;
static pid_t last_job;

static int
wait_foreground(pid_t pid, int *status)
{
	pid_t shell_pgrp = getpgrp();
	int terminal = isatty(0);
	pid_t result;
	if (terminal) (void)tcsetpgrp(0, pid);
	result = waitpid(pid, status, WUNTRACED);
	if (terminal) (void)tcsetpgrp(0, shell_pgrp);
	if (result < 0) return 0;
	if (WIFSTOPPED(*status)) {
		last_job = pid;
		printf("[%d] stopped\n", (int)pid);
	}
	return 1;
}

static int
spawn_wait(char *const argv[], unsigned flags, char *result, size_t capacity)
{
	pid_t pid;
	int status = 0;
	pid = zedbsd_spawn(argv[0], argv, environ, flags);
	if (pid < 0) {
		fprintf(stderr, "%s: %d\n", argv[0], errno);
		return 0;
	}
	(void)setpgid(pid, pid);
	if (command_background) {
		last_job = pid;
		printf("[%d]\n", (int)pid);
		return 1;
	}
	if ((flags & ZEDBSD_SPAWN_RESULT) != 0) {
		pid_t shell_pgrp = getpgrp();
		int terminal = isatty(0);
		if (terminal) (void)tcsetpgrp(0, pid);
		if (zedbsd_wait_result(pid, &status, result, capacity) < 0) {
			if (terminal) (void)tcsetpgrp(0, shell_pgrp);
			fprintf(stderr, "wait: %d\n", errno);
			return 0;
		}
		if (terminal) (void)tcsetpgrp(0, shell_pgrp);
	} else if (!wait_foreground(pid, &status)) {
		fprintf(stderr, "wait: %d\n", errno);
		return 0;
	}
	if (status != 0) {
		fprintf(stderr, "%s: status %d\n", argv[0], status);
		return 0;
	}
	return 1;
}

static int
read_line(char *buffer, size_t capacity)
{
	ssize_t length;
	if (capacity < 2) return -1;
	length = read(0, buffer, capacity - 1U);
	if (length <= 0) return -1;
	while (length > 0 && (buffer[length - 1] == '\r' ||
	    buffer[length - 1] == '\n')) length--;
	buffer[length] = '\0';
	return (int)length;
}

static int
split(char *text, char **argv, int maximum)
{
	int argc = 0;
	while (*text != '\0') {
		while (*text == ' ' || *text == '\t')
			text++;
		if (*text == '\0' || *text == '#' || *text == ';')
			break;
		if (argc == maximum)
			break;
		argv[argc++] = text;
		if (*text == '"') {
			argv[argc - 1] = ++text;
			while (*text != '\0' && *text != '"')
				text++;
		} else {
			while (*text != '\0' && *text != ' ' && *text != '\t')
				text++;
		}
		if (*text != '\0')
			*text++ = '\0';
	}
	return argc;
}

static int command(char *);

static int
source_file_mode(const char *path, int continue_on_error)
{
	FILE *file;
	char *buffer, *line;
	struct stat status;
	unsigned line_number = 0;
	if (stat(path, &status) != 0 || status.st_size < 0 ||
	    status.st_size >= SOURCE_MAX)
		return 0;
	file = fopen(path, "rb");
	if (file == NULL)
		return 0;
	buffer = malloc((size_t)status.st_size + 1U);
	if (buffer == NULL) {
		fclose(file);
		return 0;
	}
	if (fread(buffer, 1, (size_t)status.st_size, file) !=
	    (size_t)status.st_size) {
		fclose(file);
		free(buffer);
		return 0;
	}
	fclose(file);
	buffer[status.st_size] = '\0';
	line = buffer;
	while (*line != '\0') {
		char *end = line;
		line_number++;
		while (*end != '\0' && *end != '\r' && *end != '\n')
			end++;
		if (*end != '\0') {
			*end++ = '\0';
			while (*end == '\r' || *end == '\n')
				end++;
		}
		if (!command(line)) {
			if (!continue_on_error) {
				free(buffer);
				return 0;
			}
			fprintf(stderr, "%s:%u: command failed\n", path,
			    line_number);
		}
		line = end;
	}
	free(buffer);
	return 1;
}

static int
source_file(const char *path)
{
	return source_file_mode(path, 0);
}

static int
list_partitions(void)
{
	DIR *directory = opendir("/");
	struct dirent *entry;
	if (directory == NULL)
		return 0;
	while ((entry = readdir(directory)) != NULL)
		printf("%s\n", entry->d_name);
	closedir(directory);
	return 1;
}

static int
run_noct(int argc, char **argv)
{
	char *child[ARG_MAX + 2];
	char resolved[256];
	int i;
	child[0] = "/bin/noct";
	if (argc == 0) {
		child[1] = NULL;
		return spawn_wait(child, 0, NULL, 0);
	}
	if (argv[0][0] == '/' || strchr(argv[0], '/') != NULL) {
		strncpy(resolved, argv[0], sizeof(resolved) - 1U);
		resolved[sizeof(resolved) - 1U] = '\0';
	} else {
		snprintf(resolved, sizeof(resolved), "/apps/%s", argv[0]);
		if (access(resolved, F_OK) != 0) {
			snprintf(resolved, sizeof(resolved), "/bin/%s", argv[0]);
			if (access(resolved, F_OK) != 0) {
				snprintf(resolved, sizeof(resolved), "/%s", argv[0]);
				if (access(resolved, F_OK) != 0)
					return 0;
			}
		}
	}
	child[1] = resolved;
	for (i = 1; i < argc && i + 1 < ARG_MAX + 1; i++)
		child[i + 1] = argv[i];
	child[i + 1] = NULL;
	return spawn_wait(child, 0, NULL, 0);
}

static int
show_devices(void)
{
	int fd = open("/dev/system", O_RDONLY);
	struct zedbsd_system_info info;
	uint32_t index;
	if (fd < 0 || ioctl(fd, ZEDBSD_SYSTEM_GET_INFO, &info) != 0) {
		if (fd >= 0) close(fd);
		return 0;
	}
	for (index = 0; index < info.device_count; index++) {
		struct zedbsd_system_device device;
		memset(&device, 0, sizeof(device));
		device.index = index;
		if (ioctl(fd, ZEDBSD_SYSTEM_GET_DEVICE, &device) != 0)
			continue;
		printf("%s%u BIOS %02x H/S %u/%u%s\n",
		    device.device_class == 2 ? "ide" :
		    device.device_class == 3 ? "scsi" : "fd",
		    device.display_index, device.bios_id, device.heads,
		    device.sectors,
		    device.bios_id == info.boot_bios_id ? " boot" : "");
	}
	close(fd);
	return 1;
}

static int
show_vmstat(void)
{
	int fd = open("/dev/system", O_RDONLY);
	struct zedbsd_system_vmstat s;
	if (fd < 0 || ioctl(fd, ZEDBSD_SYSTEM_GET_VMSTAT, &s) != 0) {
		if (fd >= 0) close(fd);
		return 0;
	}
	close(fd);
	printf("physical.total=%llu\nphysical.free=%llu\n"
	       "heap.current=%llu\nheap.peak=%llu\n"
	       "hal.tasks=%llu\nvm.resident=%llu\nvm.swapped=%llu\n"
	       "swap.total=%llu\nswap.free=%llu\n",
	    (unsigned long long)s.physical_total,
	    (unsigned long long)s.physical_free,
	    (unsigned long long)s.heap_current,
	    (unsigned long long)s.heap_peak,
	    (unsigned long long)s.hal_tasks,
	    (unsigned long long)s.vm_resident,
	    (unsigned long long)s.vm_swapped,
	    (unsigned long long)s.swap_total,
	    (unsigned long long)s.swap_free);
	return 1;
}

static int
system_power(unsigned long request)
{
	int fd = open("/dev/system", O_RDONLY);
	if (fd < 0)
		return 0;
	if (ioctl(fd, request) != 0) {
		close(fd);
		return 0;
	}
	return 1;
}

static int
run_autoexec(const char *path)
{
	char result[256] = {0};
	char action[sizeof(result)];
	char *argv[] = { "/bin/noct", (char *)path, NULL };
	if (!spawn_wait(argv, ZEDBSD_SPAWN_RESULT, result, sizeof(result)))
		return 0;
	if (result[0] == '\0')
		return 1;
	strncpy(action, result, sizeof(action) - 1U);
	action[sizeof(action) - 1U] = '\0';
	if (!command(result)) {
		fprintf(stderr, "BOOT_ACTION failed: %s\n", action);
		return 0;
	}
	return 1;
}

static int
run_external(char *const argv[])
{
	char result[256] = {0};
	char action[sizeof(result)];

	if (!spawn_wait(argv, ZEDBSD_SPAWN_RESULT, result, sizeof(result)))
		return 0;
	if (result[0] == '\0')
		return 1;
	strncpy(action, result, sizeof(action) - 1U);
	action[sizeof(action) - 1U] = '\0';
	if (!command(result)) {
		fprintf(stderr, "command result failed: %s\n", action);
		return 0;
	}
	return 1;
}

static int
is_elf(const char *path)
{
	unsigned char magic[4];
	int fd = open(path, O_RDONLY);
	ssize_t count;
	if (fd < 0)
		return 0;
	count = read(fd, magic, sizeof(magic));
	close(fd);
	return count == (ssize_t)sizeof(magic) && magic[0] == 0x7f &&
	    magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

static int
run_search_path(int argc, char **argv)
{
	static const char *const directories[] = { "/bin", "/apps" };
	char candidate[256];
	char *child[ARG_MAX + 1];
	char *script[ARG_MAX + 1];
	unsigned directory;
	int i;

	for (directory = 0; directory < sizeof(directories) /
	    sizeof(directories[0]); directory++) {
		snprintf(candidate, sizeof(candidate), "%s/%s",
		    directories[directory], argv[0]);
		if (!is_elf(candidate))
			continue;
		child[0] = candidate;
		for (i = 1; i < argc; i++)
			child[i] = argv[i];
		child[argc] = NULL;
		return run_external(child);
	}
	for (directory = 0; directory < sizeof(directories) /
	    sizeof(directories[0]); directory++) {
		snprintf(candidate, sizeof(candidate), "%s/%s.nct",
		    directories[directory], argv[0]);
		if (access(candidate, F_OK) != 0)
			continue;
		script[0] = candidate;
		for (i = 1; i < argc; i++)
			script[i] = argv[i];
		script[argc] = NULL;
		return run_noct(argc, script);
	}
	/* Compiled Noct applications remain executable by command name. */
	for (directory = 0; directory < sizeof(directories) /
	    sizeof(directories[0]); directory++) {
		snprintf(candidate, sizeof(candidate), "%s/%s.nap",
		    directories[directory], argv[0]);
		if (access(candidate, F_OK) != 0)
			continue;
		script[0] = candidate;
		for (i = 1; i < argc; i++)
			script[i] = argv[i];
		script[argc] = NULL;
		return run_noct(argc, script);
	}
	return 0;
}

static int
command(char *text)
{
	char *argv[ARG_MAX];
	int argc = split(text, argv, ARG_MAX);
	int handled;
	if (argc == 0)
		return 1;
	command_background = argc > 1 && !strcmp(argv[argc - 1], "&");
	if (command_background) argv[--argc] = NULL;
	if (!strcmp(argv[0], "jobs")) {
		if (last_job > 0) printf("[%d] active or stopped\n", (int)last_job);
		return 1;
	}
	if (!strcmp(argv[0], "bg")) {
		return last_job > 0 && kill(-last_job, SIGCONT) == 0;
	}
	if (!strcmp(argv[0], "fg")) {
		int status = 0;
		pid_t job = last_job;
		if (job <= 0) return 0;
		(void)kill(-job, SIGCONT);
		last_job = 0;
		return wait_foreground(job, &status);
	}
	if (!strcmp(argv[0], "help")) {
		puts("help echo pwd cd ls cp cat stat touch clear true false jobs fg bg "
		     "env set unset pause wait device probe-ide probe-scsi "
		     "part source "
		     "run noct autoexec emacs vmstat reboot halt exit");
		return 1;
	}
	{
		int result = sh_builtin_dispatch(argc, argv, &handled);
		if (handled)
			return result;
	}
	if (!strcmp(argv[0], "env")) {
		int i;
		for (i = 0; environ != NULL && environ[i] != NULL; i++)
			puts(environ[i]);
		return argc == 1;
	}
	if (!strcmp(argv[0], "set"))
		return argc == 3 && setenv(argv[1], argv[2], 1) == 0;
	if (!strcmp(argv[0], "unset"))
		return argc == 2 && unsetenv(argv[1]) == 0;
	if (!strcmp(argv[0], "pause")) {
		unsigned char byte;
		int i;
		for (i = 1; i < argc; i++)
			printf("%s ", argv[i]);
		return read(0, &byte, 1) == 1;
	}
	if (!strcmp(argv[0], "wait")) {
		struct timespec delay = { argc == 2 ? atoi(argv[1]) : 1, 0 };
		return nanosleep(&delay, NULL) == 0;
	}
	if (!strcmp(argv[0], "source"))
		return argc == 2 && source_file(argv[1]);
	if (!strcmp(argv[0], "device") || !strcmp(argv[0], "probe-ide") ||
	    !strcmp(argv[0], "probe-scsi"))
		return show_devices();
	if (!strcmp(argv[0], "part"))
		return list_partitions();
	if (!strcmp(argv[0], "vmstat"))
		return argc == 1 && show_vmstat();
	if (!strcmp(argv[0], "halt"))
		return argc == 1 && system_power(ZEDBSD_SYSTEM_HALT);
	if (!strcmp(argv[0], "reboot"))
		return argc == 1 && system_power(ZEDBSD_SYSTEM_REBOOT);
	if (!strcmp(argv[0], "autoexec"))
		return argc <= 2 && run_autoexec(argc == 2 ? argv[1] :
		    "/autoexec.nct");
	if (!strcmp(argv[0], "emacs")) {
		char *args[ARG_MAX];
		int i;
		if (getenv("REMACS_SKK_DICT") == NULL)
			(void)setenv("REMACS_SKK_DICT", "/home/skkjisyo.dic", 1);
		args[0] = "/apps/remacs.nap";
		for (i = 1; i < argc && i < ARG_MAX; i++)
			args[i] = argv[i];
		return run_noct(argc, args);
	}
	if (!strcmp(argv[0], "run")) {
		return argc >= 2 && sh_run_applet(argv[1], argc - 1, argv + 1);
	}
	if (!strcmp(argv[0], "exit"))
		exit(argc == 2 ? atoi(argv[1]) : 0);
	if (strchr(argv[0], '/') != NULL && access(argv[0], F_OK) == 0) {
		char *child[ARG_MAX + 1];
		int i;

		for (i = 0; i < argc; i++)
			child[i] = argv[i];
		child[argc] = NULL;
		return run_external(child);
	}
	return run_search_path(argc, argv);
}

static void
run_startup(void)
{
	struct zedbsd_console_event event;
	struct timespec start, now, delay = { 0, 10000000L };
	int cancelled = 0;

	if (access("/etc/zinit.rc", F_OK) != 0)
		return;
	(void)ioctl(0, ZEDBSD_CONSOLE_DRAIN_INPUT);
	puts("Loading /etc/zinit.rc ... (Press any key to cancel)");
	if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
		return;
	for (;;) {
		if (ioctl(0, ZEDBSD_CONSOLE_POLL_EVENT, &event) == 0) {
			cancelled = 1;
			break;
		}
		if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
			break;
		if (now.tv_sec > start.tv_sec + 1 ||
		    (now.tv_sec == start.tv_sec + 1 &&
		    now.tv_nsec >= start.tv_nsec))
			break;
		(void)nanosleep(&delay, NULL);
	}
	if (!cancelled)
		(void)source_file_mode("/etc/zinit.rc", 1);
	else
		(void)tcflush(0, TCIFLUSH);
}

int
main(void)
{
	char line[LINE_MAX];
	run_startup();
	for (;;) {
		char cwd[256];
		if (getcwd(cwd, sizeof(cwd)) == NULL)
			strcpy(cwd, "/");
		printf("%s $ ", cwd);
		fflush(stdout);
		if (read_line(line, sizeof(line)) < 0)
			continue;
		if (!command(line))
			puts("error");
	}
}
