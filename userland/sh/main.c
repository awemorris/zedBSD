/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <zedbsd/process.h>
#include <zedbsd/console.h>
#include <zedbsd/system.h>
#include "userland/sh/alias.h"
#include "userland/sh/builtins.h"
#include "userland/sh/expand.h"
#include "userland/sh/glob.h"
#include "userland/sh/lexer.h"
#include "userland/sh/vars.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <stdio.h>
#include <stdint.h>
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
#define ARG_MAX 64
#define SOURCE_MAX 8192
#define PIPELINE_MAX 16
#define SHELL_SIGNAL_MAX 32

static int command_background;
static int command_subshell;
static pid_t last_job;
static const char *shell_name = "/bin/sh";
static int shell_positional_count;
static char **shell_positional;
static char *trap_action[SHELL_SIGNAL_MAX];
static volatile int trap_pending[SHELL_SIGNAL_MAX];
static int getopts_offset = 1;
static long getopts_last_index = 1;

static const char *
signal_message(int number)
{
	switch (number) {
	case SIGHUP: return "Hangup";
	case SIGINT: return "Interrupt";
	case SIGQUIT: return "Quit";
	case SIGILL: return "Illegal instruction";
	case SIGTRAP: return "Trace/BPT trap";
	case SIGABRT: return "Abort trap";
	case SIGFPE: return "Floating point exception";
	case SIGKILL: return "Killed";
	case SIGBUS: return "Bus error";
	case SIGSEGV: return "Segmentation fault";
	case SIGPIPE: return "Broken pipe";
	case SIGALRM: return "Alarm clock";
	case SIGTERM: return "Terminated";
	default: return "Terminated by signal";
	}
}

static const char *
shell_lookup(void *context, const char *name)
{
	(void)context;
	return sh_var_get(name);
}

static int
shell_assign(void *context, const char *name, const char *value)
{
	(void)context;
	return sh_var_set(name, value, -1);
}

static int
wait_foreground(pid_t pid, int *status)
{
	pid_t shell_pgrp = getpgrp();
	int terminal = isatty(0);
	pid_t result;
	if (terminal) (void)tcsetpgrp(0, pid);
	do result = waitpid(pid, status, WUNTRACED);
	while (result < 0 && errno == EINTR);
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
		fprintf(stderr, "sh: %s: %s\n", argv[0], strerror(errno));
		return 0;
	}
	if (!command_subshell)
		(void)setpgid(pid, pid);
	if (command_background) {
		last_job = pid;
		printf("[%d]\n", (int)pid);
		return 1;
	}
	if ((flags & ZEDBSD_SPAWN_RESULT) != 0) {
		pid_t shell_pgrp = getpgrp();
		int terminal = !command_subshell && isatty(0);
		if (terminal) (void)tcsetpgrp(0, pid);
		int waited;
		do waited = zedbsd_wait_result(pid, &status, result, capacity);
		while (waited < 0 && errno == EINTR);
		if (waited < 0) {
			if (terminal) (void)tcsetpgrp(0, shell_pgrp);
			fprintf(stderr, "wait: %d\n", errno);
			return 0;
		}
		if (terminal) (void)tcsetpgrp(0, shell_pgrp);
	} else if (command_subshell) {
		pid_t waited;
		do waited = waitpid(pid, &status, 0);
		while (waited < 0 && errno == EINTR);
		if (waited < 0) {
			fprintf(stderr, "wait: %d\n", errno);
			return 0;
		}
	} else if (!wait_foreground(pid, &status)) {
		fprintf(stderr, "wait: %d\n", errno);
		return 0;
	}
	if (WIFSIGNALED(status)) {
		fprintf(stderr, "%s\n", signal_message(WTERMSIG(status)));
		return 0;
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
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

static int command(char *);

static void
shell_signal_handler(int signal_number)
{
	if (signal_number > 0 && signal_number < SHELL_SIGNAL_MAX)
		trap_pending[signal_number] = 1;
}

static int
signal_number(const char *name)
{
	static const struct { const char *name; int number; } names[] = {
		{ "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT },
		{ "ILL", SIGILL }, { "TRAP", SIGTRAP }, { "ABRT", SIGABRT },
		{ "FPE", SIGFPE }, { "KILL", SIGKILL }, { "BUS", SIGBUS },
		{ "SEGV", SIGSEGV }, { "PIPE", SIGPIPE }, { "ALRM", SIGALRM },
		{ "TERM", SIGTERM }, { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 },
		{ "CHLD", SIGCHLD }, { "CONT", SIGCONT }, { "STOP", SIGSTOP },
		{ "TSTP", SIGTSTP }, { "TTIN", SIGTTIN }, { "TTOU", SIGTTOU }
	};
	char *end;
	long value;
	size_t index;
	if (!strncmp(name, "SIG", 3)) name += 3;
	value = strtol(name, &end, 10);
	if (*name != '\0' && *end == '\0' && value > 0 &&
	    value < SHELL_SIGNAL_MAX) return (int)value;
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++)
		if (!strcmp(name, names[index].name)) return names[index].number;
	return -1;
}

static int
set_trap(const char *action, int number)
{
	struct sigaction disposition;
	char *copy = NULL;
	if (number <= 0 || number >= SHELL_SIGNAL_MAX ||
	    number == SIGKILL || number == SIGSTOP) {
		errno = EINVAL;
		return 0;
	}
	if (action != NULL && action[0] != '\0') {
		copy = malloc(strlen(action) + 1U);
		if (copy == NULL) return 0;
		strcpy(copy, action);
	}
	memset(&disposition, 0, sizeof(disposition));
	if (action == NULL) disposition.sa_handler = SIG_DFL;
	else if (action[0] == '\0') disposition.sa_handler = SIG_IGN;
	else disposition.sa_handler = (uint64_t)(uintptr_t)shell_signal_handler;
	disposition.sa_flags = SA_RESTART;
	sigemptyset(&disposition.sa_mask);
	if (sigaction(number, &disposition, NULL) != 0) {
		free(copy);
		return 0;
	}
	free(trap_action[number]);
	trap_action[number] = copy;
	trap_pending[number] = 0;
	return 1;
}

static int
run_pending_traps(void)
{
	int number;
	int result = 1;
	for (number = 1; number < SHELL_SIGNAL_MAX; number++) {
		char *action;
		if (!trap_pending[number] || trap_action[number] == NULL) continue;
		trap_pending[number] = 0;
		action = malloc(strlen(trap_action[number]) + 1U);
		if (action == NULL) return 0;
		strcpy(action, trap_action[number]);
		if (!command(action)) result = 0;
		free(action);
	}
	return result;
}

static int
shell_command_substitute(void *context, const char *source, char **result)
{
	int descriptors[2];
	pid_t child;
	char *output = NULL;
	size_t length = 0, capacity = 0;
	int status;
	(void)context;
	*result = NULL;
	if (pipe(descriptors) != 0)
		return 0;
	child = fork();
	if (child < 0) {
		(void)close(descriptors[0]);
		(void)close(descriptors[1]);
		return 0;
	}
	if (child == 0) {
		int success;
		(void)close(descriptors[0]);
		if (dup2(descriptors[1], STDOUT_FILENO) < 0)
			_exit(1);
		(void)close(descriptors[1]);
		command_subshell = 1;
		success = command((char *)source);
		(void)fflush(NULL);
		_exit(success ? 0 : 1);
	}
	(void)close(descriptors[1]);
	for (;;) {
		char chunk[256];
		ssize_t count = read(descriptors[0], chunk, sizeof(chunk));
		char *larger;
		if (count < 0) {
			if (errno == EINTR) continue;
			free(output);
			(void)close(descriptors[0]);
			(void)waitpid(child, NULL, 0);
			return 0;
		}
		if (count == 0) break;
		if (length + (size_t)count + 1U < length) {
			free(output);
			(void)close(descriptors[0]);
			(void)waitpid(child, NULL, 0);
			return 0;
		}
		if (length + (size_t)count + 1U > capacity) {
			capacity = capacity == 0 ? 512U : capacity;
			while (capacity < length + (size_t)count + 1U)
				capacity *= 2U;
			larger = realloc(output, capacity);
			if (larger == NULL) {
				free(output);
				(void)close(descriptors[0]);
				(void)waitpid(child, NULL, 0);
				return 0;
			}
			output = larger;
		}
		memcpy(output + length, chunk, (size_t)count);
		length += (size_t)count;
	}
	(void)close(descriptors[0]);
	if (waitpid(child, &status, 0) < 0) {
		free(output);
		return 0;
	}
	while (length != 0 && output[length - 1U] == '\n') length--;
	if (output == NULL) {
		output = malloc(1U);
		if (output == NULL) return 0;
	}
	output[length] = '\0';
	*result = output;
	return 1;
}

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
path_candidate(const char *path, size_t *position, const char *name,
    const char *suffix, char *candidate, size_t capacity, int *last)
{
	size_t start = *position;
	size_t length;
	size_t name_length = strlen(name);
	size_t suffix_length = strlen(suffix);
	while (path[*position] != '\0' && path[*position] != ':')
		(*position)++;
	length = *position - start;
	*last = path[*position] == '\0';
	if (!*last)
		(*position)++;
	if (length == 0) {
		if (2U + name_length + suffix_length > capacity)
			return -1;
		candidate[0] = '.';
		candidate[1] = '/';
		memcpy(candidate + 2, name, name_length);
		memcpy(candidate + 2 + name_length, suffix, suffix_length + 1U);
		return 1;
	}
	if (length + 1U + name_length + suffix_length + 1U > capacity)
		return -1;
	memcpy(candidate, path + start, length);
	candidate[length] = '/';
	memcpy(candidate + length + 1U, name, name_length);
	memcpy(candidate + length + 1U + name_length, suffix,
	    suffix_length + 1U);
	return 1;
}

static int
search_path(const char *name, const char *suffix, int elf,
    char *candidate, size_t capacity)
{
	const char *path = sh_var_get("PATH");
	size_t position = 0;
	if (path == NULL)
		path = "/bin:/apps";
	for (;;) {
		int last;
		int result = path_candidate(path, &position, name, suffix,
		    candidate, capacity, &last);
		if (result > 0 && (elf ? is_elf(candidate) :
		    access(candidate, F_OK) == 0))
			return 1;
		if (last)
			break;
	}
	return 0;
}

static int
run_search_path(int argc, char **argv)
{
	char candidate[256];
	char *child[ARG_MAX + 1];
	int i;

	if (search_path(argv[0], "", 1, candidate, sizeof(candidate))) {
		child[0] = candidate;
		for (i = 1; i < argc; i++)
			child[i] = argv[i];
		child[argc] = NULL;
		return run_external(child);
	}
	/* Compiled Noct applications remain executable by command name. */
	if (search_path(argv[0], ".nap", 0, candidate, sizeof(candidate))) {
		char *script[ARG_MAX + 1];
		script[0] = candidate;
		for (i = 1; i < argc; i++)
			script[i] = argv[i];
		script[argc] = NULL;
		return run_noct(argc, script);
	}
	fprintf(stderr, "sh: %s: not found\n", argv[0]);
	return 0;
}

static int
assignment_length(const char *text)
{
	const char *cursor = text;
	if (!( (*cursor >= 'A' && *cursor <= 'Z') ||
	    (*cursor >= 'a' && *cursor <= 'z') || *cursor == '_'))
		return -1;
	cursor++;
	while ((*cursor >= 'A' && *cursor <= 'Z') ||
	    (*cursor >= 'a' && *cursor <= 'z') || *cursor == '_' ||
	    (*cursor >= '0' && *cursor <= '9'))
		cursor++;
	return *cursor == '=' ? (int)(cursor - text) : -1;
}

static int
apply_assignment(char *text)
{
	int length = assignment_length(text);
	char saved;
	int result;
	if (length < 0)
		return 0;
	saved = text[length];
	text[length] = '\0';
	result = sh_var_set(text, text + length + 1, -1) == 0;
	text[length] = saved;
	return result ? 1 : -1;
}

static int
shell_builtin_name(const char *name)
{
	static const char *const names[] = {
		":", ".", "[", "alias", "bg", "cd", "command", "device", "echo", "env",
		"eval", "exec", "exit", "export", "false", "fg", "getopts", "halt",
		"help", "jobs", "part", "pause", "printf", "pwd", "read", "readonly",
		"reboot", "run", "set", "shift", "source", "true", "type",
		"test", "umask", "unalias", "unset", "vmstat", "wait", NULL
	};
	int index;
	for (index = 0; names[index] != NULL; index++)
		if (strcmp(name, names[index]) == 0)
			return 1;
	return 0;
}

static int
join_arguments(int argc, char **argv, int first, char **result)
{
	size_t length = 0;
	int index;
	char *text, *cursor;
	for (index = first; index < argc; index++) {
		size_t item = strlen(argv[index]);
		if (length > (size_t)-1 - item - 2U)
			return 0;
		length += item + (index != first);
	}
	text = malloc(length + 1U);
	if (text == NULL)
		return 0;
	cursor = text;
	for (index = first; index < argc; index++) {
		size_t item = strlen(argv[index]);
		if (index != first) *cursor++ = ' ';
		memcpy(cursor, argv[index], item);
		cursor += item;
	}
	*cursor = '\0';
	*result = text;
	return 1;
}

static int
shell_wait_builtin(int argc, char **argv)
{
	pid_t target;
	int status = 0;
	if (argc > 2) {
		fprintf(stderr, "usage: wait [PID]\n");
		return 0;
	}
	if (argc == 2) {
		char *end;
		long value = strtol(argv[1], &end, 10);
		if (*argv[1] == '\0' || *end != '\0' || value <= 0) {
			fprintf(stderr, "wait: invalid pid: %s\n", argv[1]);
			return 0;
		}
		target = (pid_t)value;
		if (waitpid(target, &status, 0) != target)
			return 0;
	} else if (last_job > 0) {
		target = last_job;
		if (waitpid(target, &status, 0) != target)
			return 0;
		last_job = 0;
	} else {
		while (waitpid(-1, &status, 0) > 0)
			;
		return errno == ECHILD;
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int
set_decimal_variable(const char *name, long value)
{
	char buffer[32];
	int length = snprintf(buffer, sizeof(buffer), "%ld", value);
	return length > 0 && (size_t)length < sizeof(buffer) &&
	    sh_var_set(name, buffer, -1) == 0;
}

static int
shell_getopts_builtin(int argc, char **argv)
{
	const char *options;
	const char *index_text;
	char **arguments;
	int argument_count;
	char *end;
	long option_index;
	char option_name[2] = { 0, 0 };
	const char *definition;
	int silent;
	if (argc < 3 || !sh_var_name(argv[2]))
		return 0;
	options = argv[1];
	silent = options[0] == ':';
	if (silent) options++;
	arguments = argc > 3 ? argv + 3 : shell_positional;
	argument_count = argc > 3 ? argc - 3 : shell_positional_count;
	index_text = sh_var_get("OPTIND");
	option_index = index_text == NULL ? 1 : strtol(index_text, &end, 10);
	if (index_text != NULL && (*index_text == '\0' || *end != '\0'))
		option_index = 1;
	if (option_index < 1) option_index = 1;
	if (option_index != getopts_last_index) getopts_offset = 1;
	if (option_index > argument_count) return 0;
	if (getopts_offset == 1) {
		const char *argument = arguments[option_index - 1];
		if (argument[0] != '-' || argument[1] == '\0') return 0;
		if (!strcmp(argument, "--")) {
			option_index++;
			getopts_last_index = option_index;
			(void)set_decimal_variable("OPTIND", option_index);
			return 0;
		}
	}
	option_name[0] = arguments[option_index - 1][getopts_offset++];
	if (arguments[option_index - 1][getopts_offset] == '\0') {
		option_index++;
		getopts_offset = 1;
	}
	definition = strchr(options, option_name[0]);
	if (definition == NULL) {
		char bad[2] = { option_name[0], '\0' };
		(void)sh_var_set(argv[2], "?", -1);
		if (silent) (void)sh_var_set("OPTARG", bad, -1);
		else fprintf(stderr, "getopts: illegal option -- %c\n", option_name[0]);
		getopts_last_index = option_index;
		(void)set_decimal_variable("OPTIND", option_index);
		return 1;
	}
	if (definition[1] == ':') {
		const char *value;
		if (getopts_offset != 1) {
			value = arguments[option_index - 1] + getopts_offset;
			option_index++;
			getopts_offset = 1;
		} else if (option_index <= argument_count) {
			value = arguments[option_index - 1];
			option_index++;
		} else {
			char missing[2] = { option_name[0], '\0' };
			(void)sh_var_set(argv[2], silent ? ":" : "?", -1);
			if (silent) (void)sh_var_set("OPTARG", missing, -1);
			else fprintf(stderr, "getopts: option requires an argument -- %c\n",
			    option_name[0]);
			getopts_last_index = option_index;
			(void)set_decimal_variable("OPTIND", option_index);
			return 1;
		}
		(void)sh_var_set("OPTARG", value, -1);
	} else {
		(void)sh_var_unset("OPTARG");
	}
	(void)sh_var_set(argv[2], option_name, -1);
	getopts_last_index = option_index;
	return set_decimal_variable("OPTIND", option_index);
}

static int command_argv(int, char **);

static int
command_dispatch(int argc, char **argv)
{
	int handled;
	if (argc == 0)
		return 1;
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
		     "env set export readonly unset pause wait device probe-ide probe-scsi "
		     "part source "
		     "run noct autoexec emacs vmstat reboot halt exit");
		return 1;
	}
	if (!strcmp(argv[0], "alias")) {
		int index;
		if (argc == 1) { sh_alias_print(); return 1; }
		for (index = 1; index < argc; index++) {
			char *equals = strchr(argv[index], '=');
			if (equals == NULL) {
				const char *value = sh_alias_get(argv[index]);
				if (value == NULL) return 0;
				printf("alias %s='%s'\n", argv[index], value);
				continue;
			}
			*equals = '\0';
			if (sh_alias_set(argv[index], equals + 1) != 0) {
				*equals = '=';
				return 0;
			}
			*equals = '=';
		}
		return 1;
	}
	if (!strcmp(argv[0], "unalias")) {
		int index, result = 1;
		if (argc == 2 && !strcmp(argv[1], "-a")) {
			sh_alias_clear();
			return 1;
		}
		if (argc < 2) return 0;
		for (index = 1; index < argc; index++)
			if (sh_alias_unset(argv[index]) != 0) result = 0;
		return result;
	}
	if (!strcmp(argv[0], "getopts"))
		return shell_getopts_builtin(argc, argv);
	if (!strcmp(argv[0], "trap")) {
		int index;
		const char *action;
		if (argc == 1) {
			for (index = 1; index < SHELL_SIGNAL_MAX; index++)
				if (trap_action[index] != NULL)
					printf("trap -- '%s' %d\n", trap_action[index], index);
			return 1;
		}
		if (argc < 3) return 0;
		action = !strcmp(argv[1], "-") ? NULL : argv[1];
		for (index = 2; index < argc; index++) {
			int number = signal_number(argv[index]);
			if (number < 0 || !set_trap(action, number)) return 0;
		}
		return 1;
	}
	if (!strcmp(argv[0], ".") || !strcmp(argv[0], "source"))
		return argc == 2 && source_file(argv[1]);
	if (!strcmp(argv[0], "eval")) {
		char *text;
		int result;
		if (!join_arguments(argc, argv, 1, &text)) return 0;
		result = command(text);
		free(text);
		return result;
	}
	if (!strcmp(argv[0], "shift")) {
		long count = 1;
		char *end;
		if (argc > 2) return 0;
		if (argc == 2) {
			count = strtol(argv[1], &end, 10);
			if (*argv[1] == '\0' || *end != '\0' || count < 0)
				return 0;
		}
		if (count > shell_positional_count) return 0;
		shell_positional += count;
		shell_positional_count -= (int)count;
		return 1;
	}
	if (!strcmp(argv[0], "umask")) {
		mode_t old;
		if (argc == 1) {
			old = umask(0);
			(void)umask(old);
			printf("%04o\n", (unsigned)old);
			return 1;
		}
		if (argc == 2) {
			char *end;
			unsigned long value = strtoul(argv[1], &end, 8);
			if (*argv[1] == '\0' || *end != '\0' || value > 0777UL)
				return 0;
			(void)umask((mode_t)value);
			return 1;
		}
		return 0;
	}
	if (!strcmp(argv[0], "read")) {
		char input[LINE_MAX];
		const char *name = argc == 2 ? argv[1] : "REPLY";
		if (argc > 2 || assignment_length(name) >= 0 ||
		    !(name[0] == '_' || (name[0] >= 'A' && name[0] <= 'Z') ||
		    (name[0] >= 'a' && name[0] <= 'z')))
			return 0;
		if (read_line(input, sizeof(input)) < 0)
			return 0;
		return sh_var_set(name, input, -1) == 0;
	}
	if (!strcmp(argv[0], "wait"))
		return shell_wait_builtin(argc, argv);
	if (!strcmp(argv[0], "type") ||
	    (!strcmp(argv[0], "command") && argc > 1 && !strcmp(argv[1], "-v"))) {
		int first = !strcmp(argv[0], "type") ? 1 : 2;
		int index, success = first < argc;
		for (index = first; index < argc; index++) {
			char candidate[256];
			if (shell_builtin_name(argv[index]))
				printf("%s%s\n", !strcmp(argv[0], "type") ?
				    "shell builtin: " : "", argv[index]);
			else if (strchr(argv[index], '/') != NULL &&
			    access(argv[index], F_OK) == 0)
				puts(argv[index]);
			else if (search_path(argv[index], "", 1, candidate,
			    sizeof(candidate)))
				puts(candidate);
			else success = 0;
		}
		return success;
	}
	if (!strcmp(argv[0], "command"))
		return argc > 1 && command_argv(argc - 1, argv + 1);
	if (!strcmp(argv[0], "exec")) {
		char candidate[256];
		char **child = argv + 1;
		if (argc < 2) return 1;
		if (strchr(child[0], '/') == NULL) {
			if (!search_path(child[0], "", 1, candidate,
			    sizeof(candidate))) return 0;
			child[0] = candidate;
		}
		execve(child[0], child, environ);
		fprintf(stderr, "exec: %s: %s\n", child[0], strerror(errno));
		return 0;
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
		return argc == 3 && sh_var_set(argv[1], argv[2], -1) == 0;
	if (!strcmp(argv[0], "unset"))
		return argc == 2 && sh_var_unset(argv[1]) == 0;
	if (!strcmp(argv[0], "export")) {
		int index;
		if (argc < 2) return 0;
		for (index = 1; index < argc; index++) {
			int length = assignment_length(argv[index]);
			if (length >= 0) {
				char saved = argv[index][length];
				argv[index][length] = '\0';
				if (sh_var_set(argv[index], argv[index] + length + 1, 1) != 0) {
					argv[index][length] = saved;
					return 0;
				}
				argv[index][length] = saved;
			} else if (sh_var_export(argv[index]) != 0) return 0;
		}
		return 1;
	}
	if (!strcmp(argv[0], "readonly")) {
		int index;
		if (argc < 2) return 0;
		for (index = 1; index < argc; index++) {
			int length = assignment_length(argv[index]);
			if (length >= 0) {
				char saved = argv[index][length];
				argv[index][length] = '\0';
				if (sh_var_set(argv[index], argv[index] + length + 1, -1) != 0 ||
				    sh_var_readonly(argv[index]) != 0) {
					argv[index][length] = saved;
					return 0;
				}
				argv[index][length] = saved;
			} else if (sh_var_readonly(argv[index]) != 0) return 0;
		}
		return 1;
	}
	if (!strcmp(argv[0], ":")) return 1;
	if (!strcmp(argv[0], "pause")) {
		unsigned char byte;
		int i;
		for (i = 1; i < argc; i++)
			printf("%s ", argv[i]);
		return read(0, &byte, 1) == 1;
	}
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

static int
special_builtin_name(const char *name)
{
	static const char *const names[] = {
		":", ".", "break", "continue", "eval", "exec", "exit",
		"export", "readonly", "return", "set", "shift", "trap",
		"unset", NULL
	};
	int index;
	for (index = 0; names[index] != NULL; index++)
		if (strcmp(name, names[index]) == 0)
			return 1;
	return 0;
}

static int
temporary_assignment(char *text, struct sh_var_snapshot *snapshot)
{
	int length = assignment_length(text);
	char saved;
	int result;
	if (length < 0)
		return -1;
	saved = text[length];
	text[length] = '\0';
	if (sh_var_snapshot(text, snapshot) != 0) {
		text[length] = saved;
		return -1;
	}
	result = sh_var_set(text, text + length + 1, 1);
	text[length] = saved;
	if (result != 0) {
		(void)sh_var_restore(snapshot);
		return -1;
	}
	return 0;
}

static int
command_argv(int argc, char **argv)
{
	struct sh_var_snapshot snapshots[ARG_MAX];
	int assignments = 0;
	int temporary = 0;
	int index;
	int result;
	if (argc == 0)
		return 1;
	while (assignments < argc && assignment_length(argv[assignments]) >= 0)
		assignments++;
	if (assignments == argc ||
	    (assignments != 0 && special_builtin_name(argv[assignments]))) {
		for (index = 0; index < assignments; index++) {
			if (apply_assignment(argv[index]) < 0) {
				fprintf(stderr, "sh: %s: %s\n", argv[index],
				    strerror(errno));
				return 0;
			}
		}
	} else {
		for (index = 0; index < assignments; index++) {
			if (temporary_assignment(argv[index], &snapshots[index]) != 0) {
				while (index-- > 0)
					(void)sh_var_restore(&snapshots[index]);
				fprintf(stderr, "sh: %s: %s\n", argv[index + 1],
				    strerror(errno));
				return 0;
			}
			temporary++;
		}
	}
	if (assignments == argc)
		return 1;
	result = command_dispatch(argc - assignments, argv + assignments);
	while (temporary-- > 0)
		if (sh_var_restore(&snapshots[temporary]) != 0)
			result = 0;
	return result;
}

struct pipeline_command {
	char *argv[ARG_MAX + 1];
	int argc;
	char *input;
	char *output;
	int append;
};

static void
pipeline_free(struct pipeline_command *items, int count)
{
	int command_index, argument;
	for (command_index = 0; command_index < count; command_index++) {
		for (argument = 0; argument < items[command_index].argc; argument++)
			free(items[command_index].argv[argument]);
		free(items[command_index].input);
		free(items[command_index].output);
	}
}

static int
pipeline_child(struct pipeline_command *item)
{
	int descriptor;
	if (item->input != NULL) {
		descriptor = open(item->input, O_RDONLY);
		if (descriptor < 0 || dup2(descriptor, STDIN_FILENO) < 0) {
			fprintf(stderr, "%s: %s\n", item->input, strerror(errno));
			if (descriptor >= 0) (void)close(descriptor);
			return 0;
		}
		if (descriptor != STDIN_FILENO) (void)close(descriptor);
	}
	if (item->output != NULL) {
		descriptor = open(item->output, O_WRONLY | O_CREAT |
		    (item->append ? O_APPEND : O_TRUNC), 0666);
		if (descriptor < 0 || dup2(descriptor, STDOUT_FILENO) < 0) {
			fprintf(stderr, "%s: %s\n", item->output, strerror(errno));
			if (descriptor >= 0) (void)close(descriptor);
			return 0;
		}
		if (descriptor != STDOUT_FILENO) (void)close(descriptor);
	}
	command_subshell = 1;
	command_background = 0;
	return command_argv(item->argc, item->argv);
}

static int
execute_parent_command(struct pipeline_command *item)
{
	int saved_input = -1, saved_output = -1;
	int descriptor = -1;
	int result = 0;
	if (item->input != NULL) {
		saved_input = dup(STDIN_FILENO);
		descriptor = open(item->input, O_RDONLY);
		if (saved_input < 0 || descriptor < 0 ||
		    dup2(descriptor, STDIN_FILENO) < 0) {
			fprintf(stderr, "%s: %s\n", item->input, strerror(errno));
			goto done;
		}
		(void)close(descriptor);
		descriptor = -1;
	}
	if (item->output != NULL) {
		(void)fflush(stdout);
		saved_output = dup(STDOUT_FILENO);
		descriptor = open(item->output, O_WRONLY | O_CREAT |
		    (item->append ? O_APPEND : O_TRUNC), 0666);
		if (saved_output < 0 || descriptor < 0 ||
		    dup2(descriptor, STDOUT_FILENO) < 0) {
			fprintf(stderr, "%s: %s\n", item->output, strerror(errno));
			goto done;
		}
		(void)close(descriptor);
		descriptor = -1;
	}
	command_background = 0;
	result = command_argv(item->argc, item->argv);
	done:
	(void)fflush(NULL);
	if (descriptor >= 0) (void)close(descriptor);
	if (saved_output >= 0) {
		if (dup2(saved_output, STDOUT_FILENO) < 0) result = 0;
		(void)close(saved_output);
	}
	if (saved_input >= 0) {
		if (dup2(saved_input, STDIN_FILENO) < 0) result = 0;
		(void)close(saved_input);
	}
	clearerr(stdin);
	clearerr(stdout);
	return result;
}

static int
execute_pipeline(struct pipeline_command *items, int count, int background)
{
	pid_t children[PIPELINE_MAX];
	pid_t group = 0;
	pid_t shell_group = getpgrp();
	int terminal = !command_subshell && isatty(STDIN_FILENO);
	int input = -1;
	int index, created = 0;
	int last_status = 0;

	if (count == 1 && !background)
		return execute_parent_command(&items[0]);
	(void)fflush(NULL);
	for (index = 0; index < count; index++) {
		int descriptors[2] = { -1, -1 };
		pid_t child;
		if (index + 1 < count && pipe(descriptors) != 0)
			goto failed;
		child = fork();
		if (child < 0) {
			if (descriptors[0] >= 0) (void)close(descriptors[0]);
			if (descriptors[1] >= 0) (void)close(descriptors[1]);
			goto failed;
		}
		if (child == 0) {
			(void)setpgid(0, group == 0 ? 0 : group);
			if (input >= 0 && dup2(input, STDIN_FILENO) < 0)
				_exit(126);
			if (descriptors[1] >= 0 &&
			    dup2(descriptors[1], STDOUT_FILENO) < 0)
				_exit(126);
			if (input >= 0) (void)close(input);
			if (descriptors[0] >= 0) (void)close(descriptors[0]);
			if (descriptors[1] >= 0) (void)close(descriptors[1]);
			if (!pipeline_child(&items[index])) {
				(void)fflush(NULL);
				_exit(1);
			}
			(void)fflush(NULL);
			_exit(0);
		}
		if (group == 0) group = child;
		(void)setpgid(child, group);
		children[created++] = child;
		if (input >= 0) (void)close(input);
		if (descriptors[1] >= 0) (void)close(descriptors[1]);
		input = descriptors[0];
	}
	if (input >= 0) (void)close(input);
	if (background) {
		last_job = group;
		printf("[%d]\n", (int)group);
		return 1;
	}
	if (terminal) (void)tcsetpgrp(STDIN_FILENO, group);
	for (index = 0; index < created; index++) {
		int status = 0;
		pid_t waited = waitpid(children[index], &status, WUNTRACED);
		if (waited < 0) {
			last_status = 1;
			continue;
		}
		if (children[index] == children[created - 1])
			last_status = status;
		if (WIFSTOPPED(status)) {
			last_job = group;
			printf("[%d] stopped\n", (int)group);
		}
	}
	if (terminal) (void)tcsetpgrp(STDIN_FILENO, shell_group);
	return WIFEXITED(last_status) && WEXITSTATUS(last_status) == 0;
failed:
	if (input >= 0) (void)close(input);
	while (created-- > 0)
		(void)waitpid(children[created], NULL, 0);
	fprintf(stderr, "sh: pipeline: %s\n", strerror(errno));
	return 0;
}

static int
parse_pipeline(const struct sh_token_list *list, size_t *position,
    struct pipeline_command *items, int *item_count,
    enum sh_token_type *following, const struct sh_expand_context *context)
{
	int count = 1;
	struct pipeline_command *item = &items[0];
	const char *error_text;
	memset(items, 0, PIPELINE_MAX * sizeof(*items));
	for (;;) {
		enum sh_token_type type = list->tokens[*position].type;
		if (type == SH_TOKEN_WORD) {
			struct sh_field_list fields;
			size_t field;
			int assignment = assignment_length(
			    list->tokens[*position].text) >= 0;
			if (assignment) {
				char *word;
				if (!sh_expand_word(&list->tokens[*position], context,
				    &word, &error_text)) {
					fprintf(stderr, "sh: expansion: %s\n", error_text);
					pipeline_free(items, count);
					return 0;
				}
				memset(&fields, 0, sizeof(fields));
				fields.fields = malloc(sizeof(*fields.fields));
				fields.quoted = calloc(1, sizeof(*fields.quoted));
				if (fields.fields == NULL || fields.quoted == NULL) {
					free(word);
					free(fields.fields);
					free(fields.quoted);
					pipeline_free(items, count);
					return 0;
				}
				fields.fields[0] = word;
				fields.quoted[0] = NULL;
				fields.count = 1;
			} else if (!sh_expand_fields(&list->tokens[*position], context,
			    &fields, &error_text)) {
				fprintf(stderr, "sh: expansion: %s\n", error_text);
				pipeline_free(items, count);
				return 0;
			}
			if (!assignment && !sh_glob_fields(&fields, &error_text)) {
				fprintf(stderr, "sh: pathname expansion: %s\n", error_text);
				sh_fields_free(&fields);
				pipeline_free(items, count);
				return 0;
			}
			if (fields.count > (size_t)(ARG_MAX - item->argc)) {
				fprintf(stderr, "sh: too many arguments\n");
				sh_fields_free(&fields);
				pipeline_free(items, count);
				return 0;
			}
			for (field = 0; field < fields.count; field++) {
				item->argv[item->argc++] = fields.fields[field];
				free(fields.quoted[field]);
			}
			free(fields.fields);
			free(fields.quoted);
			(*position)++;
			continue;
		}
		if (type == SH_TOKEN_INPUT || type == SH_TOKEN_OUTPUT ||
		    type == SH_TOKEN_APPEND) {
			struct sh_field_list fields;
			(*position)++;
			if (list->tokens[*position].type != SH_TOKEN_WORD) {
				fprintf(stderr, "sh: redirection requires a path\n");
				pipeline_free(items, count);
				return 0;
			}
			if (!sh_expand_fields(&list->tokens[*position], context,
			    &fields, &error_text)) {
				fprintf(stderr, "sh: expansion: %s\n", error_text);
				pipeline_free(items, count);
				return 0;
			}
			if (!sh_glob_fields(&fields, &error_text)) {
				fprintf(stderr, "sh: pathname expansion: %s\n", error_text);
				sh_fields_free(&fields);
				pipeline_free(items, count);
				return 0;
			}
			(*position)++;
			if (fields.count != 1) {
				fprintf(stderr, "sh: ambiguous redirection\n");
				sh_fields_free(&fields);
				pipeline_free(items, count);
				return 0;
			}
			if (type == SH_TOKEN_INPUT) {
				free(item->input);
				item->input = fields.fields[0];
			} else {
				free(item->output);
				item->output = fields.fields[0];
				item->append = type == SH_TOKEN_APPEND;
			}
			free(fields.quoted[0]);
			free(fields.fields);
			free(fields.quoted);
			continue;
		}
		if (item->argc == 0) {
			fprintf(stderr, "sh: empty pipeline command\n");
			pipeline_free(items, count);
			return 0;
		}
		item->argv[item->argc] = NULL;
		if (type != SH_TOKEN_PIPE) {
			*following = type;
			*item_count = count;
			return 1;
		}
		if (count == PIPELINE_MAX) {
			fprintf(stderr, "sh: pipeline is too long\n");
			pipeline_free(items, count);
			return 0;
		}
		(*position)++;
		item = &items[count++];
	}
}

static int
command(char *text)
{
	struct sh_token_list list;
	const char *error_text;
	enum sh_token_type connector = SH_TOKEN_SEMI;
	size_t index = 0;
	int result = 1;
	int any = 0;

	if (!sh_lex(text, &list, &error_text)) {
		fprintf(stderr, "sh: syntax error: %s\n", error_text);
		return 0;
	}
	if (!sh_alias_expand(&list, &error_text)) {
		fprintf(stderr, "sh: alias: %s\n", error_text);
		sh_tokens_free(&list);
		return 0;
	}
	while (list.tokens[index].type != SH_TOKEN_END) {
		struct pipeline_command items[PIPELINE_MAX];
		struct sh_expand_context context;
		enum sh_token_type next;
		int item_count;
		int execute;

		if (!run_pending_traps()) result = 0;
		context.status = result ? 0 : 1;
		context.shell_pid = (long)getpid();
		context.last_job = (long)last_job;
		context.lookup = shell_lookup;
		context.assign = shell_assign;
		context.command_substitute = shell_command_substitute;
		context.lookup_context = NULL;
		context.shell_name = shell_name;
		context.positional_count = shell_positional_count;
		context.positional = shell_positional;
		if (!parse_pipeline(&list, &index, items, &item_count, &next,
		    &context)) {
			result = 0;
			goto done;
		}
		if (next != SH_TOKEN_END && next != SH_TOKEN_SEMI &&
		    next != SH_TOKEN_AMP && next != SH_TOKEN_AND_IF &&
		    next != SH_TOKEN_OR_IF) {
			fprintf(stderr, "sh: invalid operator\n");
			pipeline_free(items, item_count);
			result = 0;
			goto done;
		}
		execute = connector == SH_TOKEN_SEMI || connector == SH_TOKEN_AMP ||
		    (connector == SH_TOKEN_AND_IF && result) ||
		    (connector == SH_TOKEN_OR_IF && !result);
		if (execute) {
			result = execute_pipeline(items, item_count,
			    next == SH_TOKEN_AMP);
			any = 1;
		}
		pipeline_free(items, item_count);
		connector = next;
		if (next == SH_TOKEN_END)
			break;
		index++;
		if (list.tokens[index].type == SH_TOKEN_END &&
		    (next == SH_TOKEN_AND_IF || next == SH_TOKEN_OR_IF)) {
			fprintf(stderr, "sh: syntax error after operator\n");
			result = 0;
			goto done;
		}
	}
	if (!any)
		result = 1;
	if (!run_pending_traps()) result = 0;
done:
	command_background = 0;
	sh_tokens_free(&list);
	return result;
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
main(int argc, char **argv)
{
	if (sh_var_get("PATH") == NULL)
		(void)sh_var_set("PATH", "/bin:/apps", 1);
	if (argc > 1) {
		shell_name = argv[1];
		shell_positional_count = argc - 2;
		shell_positional = argv + 2;
		return source_file_mode(argv[1], 0) ? 0 : 1;
	}
	if (argc > 0 && argv[0] != NULL)
		shell_name = argv[0];
	run_startup();
	using_history();
	for (;;) {
		char cwd[256];
		char prompt[sizeof(cwd) + 4U];
		char *line;
		if (getcwd(cwd, sizeof(cwd)) == NULL)
			strcpy(cwd, "/");
		(void)snprintf(prompt, sizeof(prompt), "%s $ ", cwd);
		line = readline(prompt);
		if (line == NULL)
			continue;
		if (line[0] != '\0')
			add_history(line);
		(void)command(line);
		free(line);
	}
}
