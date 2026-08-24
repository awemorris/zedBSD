/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <zedbsd/system.h>

#define PS_MAX_PROCESSES 256
#define PS_MAX_FIELDS 24
#define PS_MAX_SELECTIONS 64

enum field_kind {
	FIELD_PID,
	FIELD_PPID,
	FIELD_PGID,
	FIELD_SID,
	FIELD_UID,
	FIELD_USER,
	FIELD_GID,
	FIELD_GROUP,
	FIELD_STATE,
	FIELD_NICE,
	FIELD_PRIORITY,
	FIELD_TIME,
	FIELD_TTY,
	FIELD_SIZE,
	FIELD_COMMAND,
};

struct output_field {
	enum field_kind kind;
	const char *header;
};

struct selection {
	long values[PS_MAX_SELECTIONS];
	size_t count;
};

static char
state_name(unsigned state)
{
	static const char names[] = "?RTXZX";
	return state < sizeof(names) - 1 ? names[state] : '?';
}

static const char *
uid_name(uid_t uid, char buffer[32])
{
	struct passwd *entry = getpwuid(uid);
	if (entry != NULL)
		return entry->pw_name;
	(void)snprintf(buffer, 32, "%u", (unsigned)uid);
	return buffer;
}

static const char *
gid_name(gid_t gid, char buffer[32])
{
	struct group *entry = getgrgid(gid);
	if (entry != NULL)
		return entry->gr_name;
	(void)snprintf(buffer, 32, "%u", (unsigned)gid);
	return buffer;
}

static int
parse_selection(const char *text, struct selection *selection)
{
	char *copy = strdup(text);
	char *cursor = copy;
	char *item;
	if (copy == NULL)
		return -1;
	while ((item = strsep(&cursor, ",")) != NULL) {
		char *end;
		long value;
		if (*item == '\0' || selection->count == PS_MAX_SELECTIONS) {
			free(copy);
			return -1;
		}
		errno = 0;
		value = strtol(item, &end, 10);
		if (errno != 0 || *end != '\0') {
			free(copy);
			return -1;
		}
		selection->values[selection->count++] = value;
	}
	free(copy);
	return 0;
}

static int
selected(long value, const struct selection *selection)
{
	size_t index;
	if (selection->count == 0)
		return 1;
	for (index = 0; index < selection->count; index++)
		if (selection->values[index] == value)
			return 1;
	return 0;
}

static int
field_definition(const char *name, struct output_field *field)
{
	static const struct {
		const char *name;
		enum field_kind kind;
		const char *header;
	} definitions[] = {
	    {"pid", FIELD_PID, "PID"},
	    {"ppid", FIELD_PPID, "PPID"},
	    {"pgid", FIELD_PGID, "PGID"},
	    {"sid", FIELD_SID, "SID"},
	    {"uid", FIELD_UID, "UID"},
	    {"user", FIELD_USER, "USER"},
	    {"gid", FIELD_GID, "GID"},
	    {"group", FIELD_GROUP, "GROUP"},
	    {"stat", FIELD_STATE, "S"},
	    {"state", FIELD_STATE, "S"},
	    {"ni", FIELD_NICE, "NI"},
	    {"nice", FIELD_NICE, "NI"},
	    {"pri", FIELD_PRIORITY, "PRI"},
	    {"time", FIELD_TIME, "TIME"},
	    {"tty", FIELD_TTY, "TTY"},
	    {"vsz", FIELD_SIZE, "VSZ"},
	    {"comm", FIELD_COMMAND, "COMMAND"},
	    {"args", FIELD_COMMAND, "COMMAND"},
	    {"command", FIELD_COMMAND, "COMMAND"},
	};
	size_t index;
	for (index = 0; index < sizeof(definitions) / sizeof(definitions[0]);
	     index++)
		if (!strcmp(name, definitions[index].name)) {
			field->kind = definitions[index].kind;
			field->header = definitions[index].header;
			return 0;
		}
	return -1;
}

static int
parse_fields(char *argument, struct output_field *fields, size_t *count)
{
	char *cursor = argument;
	char *item;
	while ((item = strsep(&cursor, ", ")) != NULL) {
		char *equals;
		if (*item == '\0')
			continue;
		if (*count == PS_MAX_FIELDS)
			return -1;
		equals = strchr(item, '=');
		if (equals != NULL)
			*equals++ = '\0';
		if (field_definition(item, &fields[*count]) != 0)
			return -1;
		if (equals != NULL)
			fields[*count].header = equals;
		(*count)++;
	}
	return 0;
}

static int
snapshot(int descriptor, struct process_info *processes, size_t *count)
{
	int32_t cursor = -1;
	*count = 0;
	while (*count < PS_MAX_PROCESSES) {
		struct process_info *process = &processes[*count];
		memset(process, 0, sizeof(*process));
		process->pid = cursor;
		if (ioctl(descriptor, ZEDBSD_SYSTEM_GET_PROCESS, process) !=
		    0) {
			if (errno == ENOENT)
				return 0;
			return -1;
		}
		if (process->version != ZEDBSD_SYSTEM_PROCESS_INFO_VERSION ||
		    process->struct_size != sizeof(*process)) {
			errno = EINVAL;
			return -1;
		}
		process->command[sizeof(process->command) - 1] = '\0';
		cursor = process->pid;
		(*count)++;
	}
	return 0;
}

static void
print_value(const struct output_field *field,
	    const struct process_info *process)
{
	char buffer[32];
	unsigned long long seconds;
	switch (field->kind) {
	case FIELD_PID:
		printf("%d", process->pid);
		break;
	case FIELD_PPID:
		printf("%d", process->ppid);
		break;
	case FIELD_PGID:
		printf("%d", process->process_group);
		break;
	case FIELD_SID:
		printf("%d", process->session);
		break;
	case FIELD_UID:
		printf("%u", process->uid);
		break;
	case FIELD_USER:
		printf("%s", uid_name(process->uid, buffer));
		break;
	case FIELD_GID:
		printf("%u", process->gid);
		break;
	case FIELD_GROUP:
		printf("%s", gid_name(process->gid, buffer));
		break;
	case FIELD_STATE:
		printf("%c", state_name(process->state));
		break;
	case FIELD_NICE:
		printf("%d", process->nice_value);
		break;
	case FIELD_PRIORITY:
		printf("%d", 20 + process->nice_value);
		break;
	case FIELD_TIME:
		seconds = process->cpu_ticks / 100;
		printf("%02llu:%02llu:%02llu", seconds / 3600,
		       seconds / 60 % 60, seconds % 60);
		break;
	case FIELD_TTY:
		printf("%s", process->has_controlling_terminal ? "tty" : "?");
		break;
	case FIELD_SIZE:
		printf("%llu",
		       (unsigned long long)(process->virtual_bytes / 1024));
		break;
	case FIELD_COMMAND:
		printf("%s", process->command[0] ? process->command : "kernel");
		break;
	}
}

static void
usage(void)
{
	fprintf(stderr, "usage: ps [-Aadefl] [-o format] [-p pidlist] "
			"[-g pgrouplist] [-u uidlist]\n");
}

int
main(int argc, char **argv)
{
	struct process_info processes[PS_MAX_PROCESSES];
	struct output_field fields[PS_MAX_FIELDS];
	struct selection pids = {{0}, 0}, groups = {{0}, 0}, users = {{0}, 0};
	size_t process_count, field_count = 0, index, field_index;
	int descriptor, option, all = 0, tty_only = 0, full = 0, long_form = 0;

	while ((option = getopt(argc, argv, "Aadeflo:p:g:u:")) != -1) {
		switch (option) {
		case 'A':
		case 'e':
			all = 1;
			break;
		case 'a':
			all = 1;
			tty_only = 1;
			break;
		case 'd':
			all = 1;
			break;
		case 'f':
			full = 1;
			break;
		case 'l':
			long_form = 1;
			break;
		case 'o':
			if (parse_fields(optarg, fields, &field_count) != 0) {
				usage();
				return 2;
			}
			break;
		case 'p':
			if (parse_selection(optarg, &pids) != 0)
				return 2;
			break;
		case 'g':
			if (parse_selection(optarg, &groups) != 0)
				return 2;
			break;
		case 'u':
			if (parse_selection(optarg, &users) != 0)
				return 2;
			break;
		default:
			usage();
			return 2;
		}
	}
	if (optind != argc) {
		usage();
		return 2;
	}
	if (field_count == 0) {
		const char *format =
		    long_form ? "state,uid,pid,ppid,pri,ni,vsz,tty,time,comm"
		    : full    ? "user,pid,ppid,tty,time,comm"
			      : "pid,tty,time,comm";
		char copy[128];
		strcpy(copy, format);
		(void)parse_fields(copy, fields, &field_count);
	}
	descriptor = open("/dev/system", O_RDONLY);
	if (descriptor < 0 ||
	    snapshot(descriptor, processes, &process_count) != 0) {
		fprintf(stderr, "ps: process snapshot: %s\n", strerror(errno));
		if (descriptor >= 0)
			close(descriptor);
		return 1;
	}
	close(descriptor);
	for (field_index = 0; field_index < field_count; field_index++)
		printf("%s%s", field_index == 0 ? "" : " ",
		       fields[field_index].header);
	putchar('\n');
	for (index = 0; index < process_count; index++) {
		struct process_info *process = &processes[index];
		if (!selected(process->pid, &pids) ||
		    !selected(process->process_group, &groups) ||
		    !selected(process->uid, &users))
			continue;
		if (!all && pids.count == 0 && groups.count == 0 &&
		    users.count == 0 &&
		    (process->uid != geteuid() ||
		     !process->has_controlling_terminal))
			continue;
		if (tty_only && !process->has_controlling_terminal)
			continue;
		for (field_index = 0; field_index < field_count;
		     field_index++) {
			if (field_index != 0)
				putchar(' ');
			print_value(&fields[field_index], process);
		}
		putchar('\n');
	}
	return ferror(stdout) ? 1 : 0;
}
