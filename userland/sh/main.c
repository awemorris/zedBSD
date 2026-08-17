/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <zedbsd/process.h>
#include <zedbsd/console.h>
#include <zedbsd/system.h>
#include "userland/sh/applet.h"
#include "userland/sh/builtins.h"
#include "userland/sh/expand.h"
#include "userland/sh/lexer.h"

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
#define ARG_MAX 64
#define SOURCE_MAX 8192
#define PIPELINE_MAX 16

static int command_background;
static int command_subshell;
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
		if (zedbsd_wait_result(pid, &status, result, capacity) < 0) {
			if (terminal) (void)tcsetpgrp(0, shell_pgrp);
			fprintf(stderr, "wait: %d\n", errno);
			return 0;
		}
		if (terminal) (void)tcsetpgrp(0, shell_pgrp);
	} else if (command_subshell) {
		if (waitpid(pid, &status, 0) < 0) {
			fprintf(stderr, "wait: %d\n", errno);
			return 0;
		}
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
	const char *path = getenv("PATH");
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
	char *script[ARG_MAX + 1];
	int i;

	if (search_path(argv[0], "", 1, candidate, sizeof(candidate))) {
		child[0] = candidate;
		for (i = 1; i < argc; i++)
			child[i] = argv[i];
		child[argc] = NULL;
		return run_external(child);
	}
	if (search_path(argv[0], ".nct", 0, candidate, sizeof(candidate))) {
		script[0] = candidate;
		for (i = 1; i < argc; i++)
			script[i] = argv[i];
		script[argc] = NULL;
		return run_noct(argc, script);
	}
	/* Compiled Noct applications remain executable by command name. */
	if (search_path(argv[0], ".nap", 0, candidate, sizeof(candidate))) {
		script[0] = candidate;
		for (i = 1; i < argc; i++)
			script[i] = argv[i];
		script[argc] = NULL;
		return run_noct(argc, script);
	}
	return 0;
}

static int
command_argv(int argc, char **argv)
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
execute_pipeline(struct pipeline_command *items, int count, int background)
{
	pid_t children[PIPELINE_MAX];
	pid_t group = 0;
	pid_t shell_group = getpgrp();
	int terminal = isatty(STDIN_FILENO);
	int input = -1;
	int index, created = 0;
	int last_status = 0;

	if (count == 1 && !background && items[0].input == NULL &&
	    items[0].output == NULL) {
		command_background = background;
		return command_argv(items[0].argc, items[0].argv);
	}
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
			if (item->argc == ARG_MAX) {
				fprintf(stderr, "sh: too many arguments\n");
				pipeline_free(items, count);
				return 0;
			}
			if (!sh_expand_word(&list->tokens[*position], context,
			    &item->argv[item->argc], &error_text)) {
				fprintf(stderr, "sh: expansion: %s\n", error_text);
				pipeline_free(items, count);
				return 0;
			}
			item->argc++;
			(*position)++;
			continue;
		}
		if (type == SH_TOKEN_INPUT || type == SH_TOKEN_OUTPUT ||
		    type == SH_TOKEN_APPEND) {
			char *path;
			(*position)++;
			if (list->tokens[*position].type != SH_TOKEN_WORD) {
				fprintf(stderr, "sh: redirection requires a path\n");
				pipeline_free(items, count);
				return 0;
			}
			if (!sh_expand_word(&list->tokens[*position], context, &path,
			    &error_text)) {
				fprintf(stderr, "sh: expansion: %s\n", error_text);
				pipeline_free(items, count);
				return 0;
			}
			(*position)++;
			if (type == SH_TOKEN_INPUT) {
				free(item->input);
				item->input = path;
			} else {
				free(item->output);
				item->output = path;
				item->append = type == SH_TOKEN_APPEND;
			}
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
	while (list.tokens[index].type != SH_TOKEN_END) {
		struct pipeline_command items[PIPELINE_MAX];
		struct sh_expand_context context;
		enum sh_token_type next;
		int item_count;
		int execute;

		context.status = result ? 0 : 1;
		context.shell_pid = (long)getpid();
		context.last_job = (long)last_job;
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
main(void)
{
	char line[LINE_MAX];
	if (getenv("PATH") == NULL)
		(void)setenv("PATH", "/bin:/apps", 0);
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
