/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU nodes: their names, the open of one with a limit on its wait,
 * what each tells of itself (GPU_GET_INFO, GPU_DEVICE_QUERY and its
 * displays), and the list, the summary and the capabilities.
 *
 * Every node is opened only for the requests of one answer and closed
 * again.  An open waits while the device is in a reset; gpu stops waiting
 * after GPU_OPEN_WAIT_S and calls the node busy.
 */

#include "gpu-command.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* The room of a list of capability or role names. */
#define GPU_NAMES_MAX			512U

/* The most digits of a node's unit, which keeps it below 10^9. */
#define GPU_UNIT_DIGITS_MAX		9U

/*
 * Whether the handler that ends a waiting open is installed.
 *
 * It is installed once, before the first open, and stays for the run.
 */
static unsigned gpu_node_alarm_ready;

static int gpu_node_scan(unsigned *units, size_t max, size_t *count);
static void gpu_node_alarm(int signal_number);
static void gpu_node_record(const struct gpu_node_facts *facts);
static void gpu_node_line(const struct gpu_node_facts *facts);
static void gpu_node_sysctl(const struct gpu_invocation *command);
static void gpu_node_refuse_open(const struct gpu_invocation *command, const char *verb, const struct gpu_node_facts *facts);
static int gpu_node_capset(const struct gpu_invocation *command, const struct gpu_node_facts *facts);
static void gpu_node_device_lines(const struct gpu_node_facts *facts);

/*
 * Reads a node's name as the user gives it, gpuN or /dev/gpuN.
 *
 * Returns 0 with the node's path and unit, or -1 for a word that names no
 * node.
 */
int
gpu_node_parse_name(
	const char *word,
	char *path,
	size_t size,
	unsigned *unit)
{
	const char *name;
	int status;
	int same;

	/* Takes /dev/ off a path. */
	name = word;
	same = strncmp(word, "/dev/", 5U);
	if (same == 0)
		name = word + 5;

	/* The name must be gpu and a unit. */
	status = gpu_node_unit_of(name, unit);
	if (status != 0)
		return -1;

	/* Succeeded: the node's path from its unit. */
	(void)snprintf(path, size, "/dev/gpu%u", *unit);
	return 0;
}

/*
 * Reads the unit of a node's name in /dev: gpu followed by a decimal
 * number without a leading 0, as the kernel names its nodes.
 *
 * Returns 0 with the unit, or -1 for any other name.
 */
int
gpu_node_unit_of(
	const char *name,
	unsigned *unit)
{
	unsigned number;
	size_t digits;
	char letter;

	/* The name starts with gpu and has more. */
	if (name[0] != 'g' ||
	    name[1] != 'p' ||
	    name[2] != 'u' ||
	    name[3] == '\0')
		return -1;

	/* A leading 0 is a name of its own only as gpu0. */
	if (name[3] == '0' && name[4] != '\0')
		return -1;

	/* Adds up the digits, refusing anything else and a number too long. */
	number = 0U;
	for (digits = 0U; name[3 + digits] != '\0'; digits++) {
		letter = name[3 + digits];
		if (letter < '0' || letter > '9')
			return -1;
		if (digits == GPU_UNIT_DIGITS_MAX)
			return -1;
		number = number * 10U + (unsigned)(letter - '0');
	}

	/* Succeeded: the node's unit. */
	*unit = number;
	return 0;
}

/*
 * Opens a node for its requests, waiting at most GPU_OPEN_WAIT_S for a
 * device in a reset.
 *
 * Returns 0 with the descriptor, ETIMEDOUT when the open waited too long,
 * or the errno of the open.
 */
int
gpu_node_open(
	const char *path,
	int *fd)
{
	struct sigaction action;
	int opened;
	int error;
	int status;

	/*
	 * Installs, once, a handler for SIGALRM without SA_RESTART, so that the
	 * alarm ends an open that waits instead of starting it again.
	 */
	if (!gpu_node_alarm_ready) {
		memset(&action, 0, sizeof(action));
		action.sa_handler = gpu_node_alarm;
		sigemptyset(&action.sa_mask);
		action.sa_flags = 0;
		status = sigaction(SIGALRM, &action, NULL);
		if (status != 0)
			return errno;
		gpu_node_alarm_ready = 1U;
	}

	/* Opens the node under the alarm. */
	(void)alarm(GPU_OPEN_WAIT_S);
	opened = open(path, O_RDWR | O_CLOEXEC);
	error = errno;
	(void)alarm(0U);

	/* An open the alarm ended waited too long. */
	if (opened < 0 && error == EINTR)
		return ETIMEDOUT;

	/* Reports why the node did not open. */
	if (opened < 0)
		return error;

	/* Succeeded: the open node. */
	*fd = opened;
	return 0;
}

/*
 * Reads what one node tells of itself into facts and, when set is not
 * NULL, its displays.
 *
 * The node is opened for these requests alone and closed again.  Each
 * request's error is kept in facts; nothing is printed.
 */
void
gpu_node_facts_read(
	const char *path,
	unsigned unit,
	struct gpu_node_facts *facts,
	struct gpu_display_set *set)
{
	struct gpu_display_set own;
	struct gpu_display_set *displays;
	uint32_t index;
	int status;
	int fd;

	/* Starts from nothing known. */
	memset(facts, 0, sizeof(*facts));
	(void)snprintf(facts->path, sizeof(facts->path), "%s", path);
	facts->unit = unit;
	displays = set;
	if (displays == NULL)
		displays = &own;
	memset(displays, 0, sizeof(*displays));

	/* Opens the node. */
	facts->open_error = gpu_node_open(path, &fd);
	if (facts->open_error != 0)
		return;

	/* The capabilities, the limits and the driver's name. */
	facts->info.version = GPU_ABI_VERSION;
	facts->info.size = sizeof(facts->info);
	status = ioctl(fd, GPU_GET_INFO, &facts->info);
	if (status != 0)
		facts->info_error = errno;

	/* The kernel's identity of the device and its roles. */
	facts->device.version = GPU_ABI_VERSION;
	facts->device.size = sizeof(facts->device);
	status = ioctl(fd, GPU_DEVICE_QUERY, &facts->device);
	if (status != 0)
		facts->device_error = errno;

	/* The displays, of a node that has them. */
	if (facts->info_error == 0 && (facts->info.capabilities & GPU_CAP_DISPLAY) != 0U) {
		facts->display_error = gpu_display_read(fd, displays);
		facts->displays = displays->total;

		/* Counts those with a monitor. */
		for (index = 0U; index < displays->count; index++) {
			if ((displays->displays[index].flags & GPU_DISPLAY_CONNECTED) != 0U)
				facts->connected++;
		}
	}

	/* The node is not kept open. */
	(void)close(fd);
}

/*
 * Lists every GPU node, one line each, then the GPUs still attaching and
 * those held back for a start.
 *
 * The list does not open libvulkan.  Returns GPU_EXIT_OK, or
 * GPU_EXIT_FAILED when /dev cannot be read.
 */
int
gpu_node_list(
	const struct gpu_invocation *command)
{
	unsigned units[GPU_NODES_MAX];
	struct gpu_node_facts facts;
	char path[GPU_NODE_PATH_MAX];
	size_t count;
	size_t index;
	int error;

	/* The nodes in /dev, by their units. */
	error = gpu_node_scan(units, GPU_NODES_MAX, &count);
	if (error != 0) {
		gpu_fail(command, "list", error, "cannot read /dev: %s", strerror(error));
		return GPU_EXIT_FAILED;
	}

	/* One line for each node. */
	for (index = 0U; index < count; index++) {
		(void)snprintf(path, sizeof(path), "/dev/gpu%u", units[index]);
		gpu_node_facts_read(path, units[index], &facts, NULL);

		/* The record of a script, or the line of a person. */
		if (command->porcelain) {
			gpu_node_record(&facts);
		} else {
			gpu_node_line(&facts);
		}
	}

	/* Says so when there is none. */
	if (count == 0U && !command->porcelain)
		printf("no GPU node\n");

	/* The GPUs not yet published, and those held back. */
	gpu_node_sysctl(command);

	/* Succeeded: the list is printed. */
	return GPU_EXIT_OK;
}

/*
 * Shows one node's summary: what the kernel tells of it and of its
 * displays.
 */
int
gpu_node_show(
	const struct gpu_invocation *command)
{
	struct gpu_display_set set;
	struct gpu_node_facts facts;
	uint32_t index;

	/* What the node tells. */
	gpu_node_facts_read(command->node, command->unit, &facts, &set);
	if (facts.open_error != 0) {
		gpu_node_refuse_open(command, "show", &facts);
		return GPU_EXIT_FAILED;
	}
	if (facts.info_error != 0) {
		gpu_fail(command, "show", facts.info_error, "%s: GPU_GET_INFO: %s", command->node, strerror(facts.info_error));
		return GPU_EXIT_FAILED;
	}

	/* The porcelain form: the node's record, then one record for each display. */
	if (command->porcelain) {
		gpu_node_record(&facts);
		for (index = 0U; index < set.count; index++)
			gpu_display_line(command->node, index, &set.displays[index], 1U);
		return GPU_EXIT_OK;
	}

	/* The node's lines for people. */
	printf("gpu%u (%s)\n", facts.unit, facts.path);
	gpu_node_device_lines(&facts);

	/* One line for each display. */
	for (index = 0U; index < set.count; index++)
		gpu_display_line(command->node, index, &set.displays[index], 0U);

	/* Says when the displays could not all be read. */
	if (facts.display_error != 0)
		printf("  displays       not read: %s\n", strerror(facts.display_error));

	/* Succeeded: the summary is printed. */
	return GPU_EXIT_OK;
}

/*
 * Shows one node's capability bits, limits, roles and identity, and with
 * --capset one capability set's bytes.
 */
int
gpu_node_caps(
	const struct gpu_invocation *command)
{
	char names[GPU_NAMES_MAX];
	char roles[GPU_NAMES_MAX];
	struct gpu_node_facts facts;
	const char *name;
	uint32_t bit;
	unsigned shift;
	int status;

	/* What the node tells. */
	gpu_node_facts_read(command->node, command->unit, &facts, NULL);
	if (facts.open_error != 0) {
		gpu_node_refuse_open(command, "caps", &facts);
		return GPU_EXIT_FAILED;
	}
	if (facts.info_error != 0) {
		gpu_fail(command, "caps", facts.info_error, "%s: GPU_GET_INFO: %s", command->node, strerror(facts.info_error));
		return GPU_EXIT_FAILED;
	}

	/* One capability set's bytes, when asked for. */
	if (command->capset_given) {
		status = gpu_node_capset(command, &facts);
		return status;
	}

	/* The porcelain form: one record. */
	if (command->porcelain) {
		(void)gpu_capability_names(facts.info.capabilities, ",", names, sizeof(names));
		gpu_record_begin("caps");
		gpu_field_text("node", facts.path, sizeof(facts.path));
		gpu_field_text("driver", facts.info.driver_name, sizeof(facts.info.driver_name));
		gpu_field_hex("capabilities", facts.info.capabilities);
		gpu_field_text("names", names, sizeof(names));
		gpu_field_unsigned("max_resources", facts.info.max_resources);
		gpu_field_unsigned("max_resource_bytes", facts.info.max_resource_bytes);

		/* The identity, when the node answered for it. */
		if (facts.device_error == 0) {
			(void)gpu_role_names(facts.device.roles, ",", roles, sizeof(roles));
			gpu_field_hex("roles", facts.device.roles);
			gpu_field_text("role", roles, sizeof(roles));
			gpu_field_unsigned("device_id", facts.device.device_id);
			gpu_field_unsigned("companion_id", facts.device.companion_id);
		}
		gpu_record_end();
		return GPU_EXIT_OK;
	}

	/* The node and every bit that is set, by name. */
	printf("gpu%u (%s)\n", facts.unit, facts.path);
	printf("  capabilities   0x%08x\n", facts.info.capabilities);
	for (shift = 0U; shift < 32U; shift++) {
		bit = 1U << shift;
		if ((facts.info.capabilities & bit) == 0U)
			continue;

		/* Its name, or only its value. */
		name = gpu_capability_name(bit);
		if (name == NULL)
			name = "(unknown)";
		printf("    %-18s 0x%x\n", name, bit);
	}

	/* The limits, the driver and the identity. */
	gpu_node_device_lines(&facts);

	/* Succeeded: the capabilities are printed. */
	return GPU_EXIT_OK;
}

/*
 * Writes the names of the nodes other than self that have displays into
 * out, separated by commas; "none" when there is none.  Returns the number
 * of such nodes.
 */
int
gpu_node_display_nodes(
	unsigned self,
	char *out,
	size_t size)
{
	unsigned units[GPU_NODES_MAX];
	struct gpu_node_facts facts;
	char path[GPU_NODE_PATH_MAX];
	char name[GPU_NODE_PATH_MAX];
	size_t count;
	size_t index;
	size_t used;
	size_t length;
	int found;
	int error;

	/* Starts with none. */
	found = 0;
	used = 0U;
	(void)snprintf(out, size, "none");

	/* The nodes in /dev. */
	error = gpu_node_scan(units, GPU_NODES_MAX, &count);
	if (error != 0)
		return 0;

	/* Adds each other node whose roles include display. */
	for (index = 0U; index < count; index++) {
		if (units[index] == self)
			continue;
		(void)snprintf(path, sizeof(path), "/dev/gpu%u", units[index]);
		gpu_node_facts_read(path, units[index], &facts, NULL);
		if (facts.open_error != 0 || facts.device_error != 0)
			continue;
		if ((facts.device.roles & GPU_DEVICE_DISPLAY) == 0U)
			continue;

		/* Its name, after a comma when it is not the first. */
		(void)snprintf(name, sizeof(name), "%sgpu%u", found != 0 ? "," : "", units[index]);
		length = strlen(name);
		if (used + length >= size)
			break;
		memcpy(out + used, name, length + 1U);
		used += length;
		found++;
	}

	/* The number of display nodes found. */
	return found;
}

/*
 * Lists the units of the GPU nodes in /dev, in rising order, at most max;
 * 0, or the errno of reading /dev.
 */
static int
gpu_node_scan(
	unsigned *units,
	size_t max,
	size_t *count)
{
	struct dirent *entry;
	DIR *directory;
	unsigned unit;
	size_t found;
	size_t at;
	int status;
	int error;

	/* Opens /dev. */
	*count = 0U;
	directory = opendir("/dev");
	if (directory == NULL)
		return errno;

	/* Takes each entry whose name is a node's, kept in order. */
	found = 0U;
	error = 0;
	while (1) {
		/* Tells the end of the directory from a failed read. */
		errno = 0;
		entry = readdir(directory);
		if (entry == NULL) {
			error = errno;
			break;
		}

		/* Skips every other name. */
		status = gpu_node_unit_of(entry->d_name, &unit);
		if (status != 0)
			continue;

		/* Keeps at most max nodes. */
		if (found == max)
			continue;

		/* Inserts the unit after the smaller ones. */
		at = found;
		while (at > 0U && units[at - 1U] > unit) {
			units[at] = units[at - 1U];
			at--;
		}
		units[at] = unit;
		found++;
	}

	/* The directory is not kept. */
	(void)closedir(directory);

	/* Reports a directory that could not be read to its end. */
	if (error != 0)
		return error;

	/* Succeeded: the units found. */
	*count = found;
	return 0;
}

/* Ends a waiting open; the handler itself has nothing to do. */
static void
gpu_node_alarm(
	int signal_number)
{
	UNUSED_PARAMETER(signal_number);
}

/* Prints a node's porcelain record. */
static void
gpu_node_record(
	const struct gpu_node_facts *facts)
{
	char names[GPU_NAMES_MAX];
	const char *name;
	int error;

	/* The node and its unit. */
	gpu_record_begin("gpu");
	gpu_field_text("node", facts->path, sizeof(facts->path));
	gpu_field_unsigned("unit", facts->unit);

	/* A node that did not open: busy after the wait, failed otherwise. */
	if (facts->open_error != 0 || facts->info_error != 0) {
		if (facts->open_error == ETIMEDOUT) {
			gpu_field_text("state", "busy", 8U);
		} else {
			gpu_field_text("state", "failed", 8U);
		}

		/* The error of the open, or of the request, by name or by number. */
		error = facts->open_error;
		if (error == 0)
			error = facts->info_error;
		name = gpu_error_name(error);
		if (name != NULL) {
			gpu_field_text("error", name, GPU_NAMES_MAX);
		} else {
			gpu_field_unsigned("error", (unsigned long long)error);
		}
		gpu_record_end();
		return;
	}

	/* The node that answered. */
	gpu_field_text("state", "ok", 8U);
	gpu_field_text("driver", facts->info.driver_name, sizeof(facts->info.driver_name));
	gpu_field_hex("capabilities", facts->info.capabilities);

	/* The roles and the identity, when the node answered for them. */
	if (facts->device_error == 0) {
		(void)gpu_role_names(facts->device.roles, ",", names, sizeof(names));
		gpu_field_hex("roles", facts->device.roles);
		gpu_field_text("role", names, sizeof(names));
		gpu_field_unsigned("device_id", facts->device.device_id);
		gpu_field_unsigned("companion_id", facts->device.companion_id);
	}

	/* The displays. */
	gpu_field_unsigned("displays", facts->displays);
	gpu_field_unsigned("connected", facts->connected);
	gpu_record_end();
}

/* Prints a node's line of the list for people. */
static void
gpu_node_line(
	const struct gpu_node_facts *facts)
{
	char name[GPU_NODE_PATH_MAX];
	char roles[GPU_NAMES_MAX];

	/* The node's name. */
	(void)snprintf(name, sizeof(name), "gpu%u", facts->unit);
	gpu_print_column(name, sizeof(name), 6U);

	/* A node whose open waited past the limit. */
	if (facts->open_error == ETIMEDOUT) {
		printf("busy (reset?): the open waited %u s\n", GPU_OPEN_WAIT_S);
		return;
	}

	/* A node that did not open or answer. */
	if (facts->open_error != 0) {
		printf("failed: %s\n", strerror(facts->open_error));
		return;
	}
	if (facts->info_error != 0) {
		printf("failed: GPU_GET_INFO: %s\n", strerror(facts->info_error));
		return;
	}

	/* The driver and the roles. */
	gpu_print_column(facts->info.driver_name, sizeof(facts->info.driver_name), 18U);
	(void)snprintf(roles, sizeof(roles), "?");
	if (facts->device_error == 0)
		(void)gpu_role_names(facts->device.roles, ",", roles, sizeof(roles));

	/* A node without displays ends with its roles. */
	if ((facts->info.capabilities & GPU_CAP_DISPLAY) == 0U) {
		gpu_print_text(roles, sizeof(roles));
		printf("\n");
		return;
	}

	/* The displays of a node that has them, in a column after the roles. */
	gpu_print_column(roles, sizeof(roles), 16U);
	printf("displays=%u connected=%u\n", facts->displays, facts->connected);
}

/* Prints the GPUs still attaching and those held back for a start (hw.gpu.attaching, hw.gpu.start). */
static void
gpu_node_sysctl(
	const struct gpu_invocation *command)
{
	uint32_t attaching;
	uint64_t held;
	size_t length;
	int attaching_status;
	int held_status;

	/* The GPUs a driver has attached but not yet published. */
	attaching = 0U;
	length = sizeof(attaching);
	attaching_status = sysctlbyname("hw.gpu.attaching", &attaching, &length, NULL, 0U);
	if (attaching_status == 0 && length != sizeof(attaching))
		attaching_status = -1;

	/* The GPUs a driver holds back until root starts them. */
	held = 0U;
	length = sizeof(held);
	held_status = sysctlbyname("hw.gpu.start", &held, &length, NULL, 0U);
	if (held_status == 0 && length != sizeof(held))
		held_status = -1;

	/* The porcelain form leaves out what could not be read. */
	if (command->porcelain) {
		gpu_record_begin("sysctl");
		if (attaching_status == 0)
			gpu_field_unsigned("attaching", attaching);
		if (held_status == 0)
			gpu_field_unsigned("held", held);
		gpu_record_end();
		return;
	}

	/* The line for people, with ? for what could not be read. */
	if (attaching_status == 0) {
		printf("attaching=%u", attaching);
	} else {
		printf("attaching=?");
	}
	if (held_status == 0) {
		printf(" held=%llu\n", (unsigned long long)held);
	} else {
		printf(" held=?\n");
	}
}

/* Reports a node that did not open. */
static void
gpu_node_refuse_open(
	const struct gpu_invocation *command,
	const char *verb,
	const struct gpu_node_facts *facts)
{
	/* A node whose open waited past the limit is busy, likely in a reset. */
	if (facts->open_error == ETIMEDOUT) {
		gpu_fail(command, verb, ETIMEDOUT, "%s: busy (reset?): the open waited %u s", facts->path, GPU_OPEN_WAIT_S);
		return;
	}

	/* Any other failed open. */
	gpu_fail(command, verb, facts->open_error, "%s: %s", facts->path, strerror(facts->open_error));
}

/* Prints one capability set's bytes in hexadecimal (GPU_GET_CAPSET, version 0). */
static int
gpu_node_capset(
	const struct gpu_invocation *command,
	const struct gpu_node_facts *facts)
{
	struct gpu_capset capset;
	char hex[GPU_CAPSET_MAX * 2U + 1U];
	uint32_t index;
	uint32_t line;
	int status;
	int error;
	int fd;

	/* Opens the node again for this request. */
	error = gpu_node_open(facts->path, &fd);
	if (error != 0) {
		gpu_fail(command, "caps", error, "%s: %s", facts->path, strerror(error));
		return GPU_EXIT_FAILED;
	}

	/* Asks for the set, as many bytes as one can hold. */
	memset(&capset, 0, sizeof(capset));
	capset.version = GPU_ABI_VERSION;
	capset.size = sizeof(capset);
	capset.capset_id = command->capset_id;
	capset.capset_version = 0U;
	capset.capacity = GPU_CAPSET_MAX;
	status = ioctl(fd, GPU_GET_CAPSET, &capset);
	error = errno;
	(void)close(fd);
	if (status != 0) {
		gpu_fail(command, "caps", error, "%s: capset %u: %s", facts->path, command->capset_id, strerror(error));
		return GPU_EXIT_FAILED;
	}

	/* Refuses a length past the array, which the kernel never gives. */
	if (capset.bytes > GPU_CAPSET_MAX) {
		gpu_fail(command, "caps", EMSGSIZE, "%s: capset %u: %u bytes", facts->path, command->capset_id, capset.bytes);
		return GPU_EXIT_FAILED;
	}

	/* The bytes as one hexadecimal text. */
	for (index = 0U; index < capset.bytes; index++)
		(void)snprintf(hex + index * 2U, 3U, "%02x", capset.data[index]);
	hex[capset.bytes * 2U] = '\0';

	/* The porcelain form: one record. */
	if (command->porcelain) {
		gpu_record_begin("caps");
		gpu_field_text("node", facts->path, sizeof(facts->path));
		gpu_field_unsigned("capset", capset.capset_id);
		gpu_field_unsigned("capset_version", capset.capset_version);
		gpu_field_unsigned("bytes", capset.bytes);
		gpu_field_text("data", hex, sizeof(hex));
		gpu_record_end();
		return GPU_EXIT_OK;
	}

	/* The bytes for people, 16 a line after their offset. */
	printf("gpu%u (%s) capset %u version %u, %u bytes\n", facts->unit, facts->path, capset.capset_id, capset.capset_version, capset.bytes);
	for (index = 0U; index < capset.bytes; index += 16U) {
		/* The bytes of this line: 16, or what is left. */
		line = capset.bytes - index;
		if (line > 16U)
			line = 16U;
		printf("  %04x  %.*s\n", index, (int)(line * 2U), hex + index * 2U);
	}

	/* Succeeded: the set is printed. */
	return GPU_EXIT_OK;
}

/* Prints a node's driver, roles, identity, capabilities and limits for people. */
static void
gpu_node_device_lines(
	const struct gpu_node_facts *facts)
{
	char names[GPU_NAMES_MAX];

	/* The driver. */
	printf("  driver         ");
	gpu_print_text(facts->info.driver_name, sizeof(facts->info.driver_name));
	printf("\n");

	/* The roles and the identity, or why they are not known. */
	if (facts->device_error != 0) {
		printf("  roles          not known: %s\n", strerror(facts->device_error));
	} else {
		(void)gpu_role_names(facts->device.roles, ", ", names, sizeof(names));
		printf("  roles          %s\n", names);
		printf("  device id      %llu\n", (unsigned long long)facts->device.device_id);

		/* The companion the device prefers for rendering, if any. */
		if (facts->device.companion_id == 0U) {
			printf("  companion      none\n");
		} else {
			printf("  companion      device %llu\n", (unsigned long long)facts->device.companion_id);
		}
	}

	/* The capabilities by name. */
	(void)gpu_capability_names(facts->info.capabilities, " ", names, sizeof(names));
	printf("  capabilities   %s\n", names);

	/* The limits of one open. */
	printf("  resources      at most %u per open, %llu bytes each\n", facts->info.max_resources, (unsigned long long)facts->info.max_resource_bytes);
}
