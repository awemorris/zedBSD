/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The displays of a GPU node as the kernel reports them: the list
 * (GPU_DISPLAY_QUERY), a display's modes (GPU_DISPLAY_MODE) and its EDID
 * (GPU_DISPLAY_EDID).  Nothing is claimed and nothing is shown: these are
 * reads.  A change of a display's mode is asked of the desktop session's
 * compositor (session.c), never sent to the kernel from here.
 */

#include "gpu-command.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* The room of a list of node names. */
#define GPU_NODE_NAMES_MAX		256U

/* The room of a size or a refresh as text. */
#define GPU_SIZE_TEXT_MAX		32U

/* The room of a mode with its refresh as text. */
#define GPU_MODE_TEXT_MAX		80U

/*
 * Where an EDID base block keeps what gpu decodes.
 *
 * Values referenced: VESA Enhanced EDID Standard, Release A, Revision 2
 * (E-EDID 1.4), section 3 (the 128-byte base block): the header at 0, the
 * manufacturer at 8, the product at 10, the serial number at 12, the week
 * and year of manufacture at 16 and 17, the version and revision at 18 and
 * 19, the screen size in cm at 21 and 22, the four 18-byte descriptors
 * from 54, the extension count at 126; a display descriptor starts with
 * three 0 bytes and names its kind at its byte 3 (0xfc the product name,
 * 0xff the serial number), with up to 13 bytes of text from its byte 5,
 * ended by 0x0a and padded with spaces.
 */
#define GPU_EDID_BLOCK			128U
#define GPU_EDID_MANUFACTURER		8U
#define GPU_EDID_PRODUCT		10U
#define GPU_EDID_SERIAL			12U
#define GPU_EDID_WEEK			16U
#define GPU_EDID_YEAR			17U
#define GPU_EDID_VERSION		18U
#define GPU_EDID_REVISION		19U
#define GPU_EDID_WIDTH_CM		21U
#define GPU_EDID_HEIGHT_CM		22U
#define GPU_EDID_DESCRIPTORS		54U
#define GPU_EDID_DESCRIPTOR		18U
#define GPU_EDID_DESCRIPTOR_COUNT	4U
#define GPU_EDID_EXTENSIONS		126U
#define GPU_EDID_TAG_NAME		0xfcU
#define GPU_EDID_TAG_SERIAL		0xffU
#define GPU_EDID_TEXT			5U
#define GPU_EDID_TEXT_MAX		13U
#define GPU_EDID_YEAR_BASE		1990U
#define GPU_EDID_WEEK_MODEL_YEAR	0xffU

/*
 * The fixed header that starts every EDID base block.
 *
 * Value referenced: VESA E-EDID 1.4, section 3.3 (header).
 */
static const uint8_t gpu_edid_header[8] = { 0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x00U };

static int gpu_display_node_check(const struct gpu_invocation *command, const struct gpu_node_facts *facts);
static int gpu_display_list(const struct gpu_invocation *command, const struct gpu_display_set *set);
static int gpu_display_modes(const struct gpu_invocation *command, const struct gpu_display_info *display, uint32_t index);
static int gpu_display_edid(const struct gpu_invocation *command, const struct gpu_display_info *display, uint32_t index);
static void gpu_display_edid_lines(const struct gpu_invocation *command, const struct gpu_display_info *display, uint32_t index, const struct gpu_display_edid *edid);
static void gpu_display_edid_raw(const struct gpu_invocation *command, const struct gpu_display_info *display, uint32_t index, const struct gpu_display_edid *edid);
static void gpu_edid_text(const uint8_t *descriptor, char *text);
static int gpu_display_find(const struct gpu_invocation *command, const struct gpu_display_set *set, uint32_t *index);
static void gpu_display_record_start(const char *kind, const char *node, uint32_t index, const struct gpu_display_info *display);
static int gpu_display_digits(const char *word, uint32_t *value);

/*
 * Reads every output of a node into set, in the kernel's index order.
 *
 * Returns 0, or the errno of the first request that failed; the outputs
 * read before it stay in set.
 */
int
gpu_display_read(
	int fd,
	struct gpu_display_set *set)
{
	struct gpu_display_info *display;
	struct gpu_display_info query;
	uint32_t index;
	int status;

	/* Starts empty. */
	memset(set, 0, sizeof(*set));

	/* How many outputs the node has. */
	memset(&query, 0, sizeof(query));
	query.version = GPU_ABI_VERSION;
	query.size = sizeof(query);
	query.index = GPU_DISPLAY_COUNT_ONLY;
	status = ioctl(fd, GPU_DISPLAY_QUERY, &query);
	if (status != 0)
		return errno;
	set->total = query.count;

	/* Each output, as many as the set holds. */
	for (index = 0U; index < set->total && index < GPU_DISPLAYS_MAX; index++) {
		display = &set->displays[index];
		display->version = GPU_ABI_VERSION;
		display->size = sizeof(*display);
		display->index = index;
		status = ioctl(fd, GPU_DISPLAY_QUERY, display);
		if (status != 0)
			return errno;

		/* The name is a terminated string from here on. */
		display->name[sizeof(display->name) - 1U] = '\0';
		set->count++;
	}

	/* Succeeded: every output that fits is read. */
	return 0;
}

/*
 * Finds the display a word names: its number in the list (from 1), id=N
 * for its kernel display id, its whole name, or the start of its name.
 *
 * Returns 0 with its index, -1 when no display is named, or -2 when the
 * word names more than one.
 */
int
gpu_display_select(
	const struct gpu_display_set *set,
	const char *word,
	uint32_t *index)
{
	uint32_t number;
	uint32_t at;
	uint32_t matches;
	size_t length;
	int status;
	int same;

	/* A number counts from 1 in the list. */
	status = gpu_display_digits(word, &number);
	if (status == 0) {
		if (number == 0U || number > set->count)
			return -1;
		*index = number - 1U;
		return 0;
	}

	/* id=N is the kernel's display id. */
	same = strncmp(word, "id=", 3U);
	if (same == 0) {
		status = gpu_display_digits(word + 3, &number);
		if (status != 0)
			return -1;

		/* Looks the id up. */
		for (at = 0U; at < set->count; at++) {
			if (set->displays[at].display_id == number) {
				*index = at;
				return 0;
			}
		}
		return -1;
	}

	/* The whole name, which must name one display. */
	matches = 0U;
	for (at = 0U; at < set->count; at++) {
		same = strcmp(set->displays[at].name, word);
		if (same == 0) {
			*index = at;
			matches++;
		}
	}
	if (matches == 1U)
		return 0;
	if (matches > 1U)
		return -2;

	/* The start of a name, which must start one name only. */
	length = strlen(word);
	if (length == 0U)
		return -1;
	for (at = 0U; at < set->count; at++) {
		same = strncmp(set->displays[at].name, word, length);
		if (same == 0) {
			*index = at;
			matches++;
		}
	}
	if (matches > 1U)
		return -2;
	if (matches == 0U)
		return -1;

	/* Succeeded: the one display the word names. */
	return 0;
}

/*
 * Decodes the parts of an EDID base block a person reads into summary.
 *
 * Fewer than 128 bytes, or a block without the EDID header, leave summary
 * with valid 0.  The texts are kept as the sink sent them, cut at their
 * end mark and stripped of the trailing spaces.
 */
void
gpu_edid_decode(
	const uint8_t *bytes,
	size_t length,
	struct gpu_edid_summary *summary)
{
	const uint8_t *descriptor;
	uint32_t letters;
	unsigned sum;
	size_t index;
	int same;

	/* Starts from nothing decoded. */
	memset(summary, 0, sizeof(*summary));

	/* A block too short, or without the header, is no EDID. */
	if (length < GPU_EDID_BLOCK)
		return;
	same = memcmp(bytes, gpu_edid_header, sizeof(gpu_edid_header));
	if (same != 0)
		return;
	summary->valid = 1U;

	/* The block's bytes add up to 0 modulo 256 when it is whole. */
	sum = 0U;
	for (index = 0U; index < GPU_EDID_BLOCK; index++)
		sum += bytes[index];
	if ((sum & 0xffU) == 0U)
		summary->checksum_ok = 1U;

	/* The manufacturer: three letters of five bits, A as 1, big-endian. */
	letters = ((uint32_t)bytes[GPU_EDID_MANUFACTURER] << 8) | bytes[GPU_EDID_MANUFACTURER + 1U];
	summary->manufacturer[0] = (char)('A' - 1 + (int)((letters >> 10) & 0x1fU));
	summary->manufacturer[1] = (char)('A' - 1 + (int)((letters >> 5) & 0x1fU));
	summary->manufacturer[2] = (char)('A' - 1 + (int)(letters & 0x1fU));
	summary->manufacturer[3] = '\0';

	/* The product code, little-endian. */
	summary->product = (uint32_t)bytes[GPU_EDID_PRODUCT];
	summary->product |= (uint32_t)bytes[GPU_EDID_PRODUCT + 1U] << 8;

	/* The serial number, little-endian. */
	summary->serial = (uint32_t)bytes[GPU_EDID_SERIAL];
	summary->serial |= (uint32_t)bytes[GPU_EDID_SERIAL + 1U] << 8;
	summary->serial |= (uint32_t)bytes[GPU_EDID_SERIAL + 2U] << 16;
	summary->serial |= (uint32_t)bytes[GPU_EDID_SERIAL + 3U] << 24;

	/* The week and year of manufacture; week 0xff makes the year the model's. */
	summary->week = bytes[GPU_EDID_WEEK];
	summary->year = GPU_EDID_YEAR_BASE + bytes[GPU_EDID_YEAR];
	if (summary->week == GPU_EDID_WEEK_MODEL_YEAR) {
		summary->model_year = 1U;
		summary->week = 0U;
	}

	/* The version, the screen's size and the extension blocks that follow. */
	summary->version = bytes[GPU_EDID_VERSION];
	summary->revision = bytes[GPU_EDID_REVISION];
	summary->width_cm = bytes[GPU_EDID_WIDTH_CM];
	summary->height_cm = bytes[GPU_EDID_HEIGHT_CM];
	summary->extensions = bytes[GPU_EDID_EXTENSIONS];

	/* The display descriptors that carry the product's name and serial number. */
	for (index = 0U; index < GPU_EDID_DESCRIPTOR_COUNT; index++) {
		descriptor = bytes + GPU_EDID_DESCRIPTORS + index * GPU_EDID_DESCRIPTOR;

		/* A timing descriptor does not start with three 0 bytes. */
		if (descriptor[0] != 0U || descriptor[1] != 0U || descriptor[2] != 0U)
			continue;

		/* Keeps the name and the serial number's text. */
		if (descriptor[3] == GPU_EDID_TAG_NAME) {
			gpu_edid_text(descriptor, summary->name);
		} else if (descriptor[3] == GPU_EDID_TAG_SERIAL) {
			gpu_edid_text(descriptor, summary->serial_text);
		}
	}
}

/*
 * Prints one display's line: a porcelain record, or a line for people.
 *
 * index is the display's place in the kernel's list, from 0; people and
 * scripts see it from 1, as DISPLAY names it.
 */
void
gpu_display_line(
	const char *node,
	uint32_t index,
	const struct gpu_display_info *display,
	unsigned porcelain)
{
	char refresh[GPU_SIZE_TEXT_MAX];
	char shown[GPU_MODE_TEXT_MAX];
	char state[GPU_SIZE_TEXT_MAX];

	/* The porcelain record: every value the kernel gave. */
	if (porcelain) {
		gpu_display_record_start("display", node, index, display);
		gpu_field_unsigned("connected", (display->flags & GPU_DISPLAY_CONNECTED) != 0U);
		gpu_field_unsigned("active", (display->flags & GPU_DISPLAY_ACTIVE) != 0U);
		gpu_field_unsigned("width", display->current_width);
		gpu_field_unsigned("height", display->current_height);
		gpu_field_unsigned("refresh_mhz", display->refresh_millihz);
		gpu_field_unsigned("preferred_width", display->preferred_width);
		gpu_field_unsigned("preferred_height", display->preferred_height);
		gpu_field_unsigned("max_width", display->max_width);
		gpu_field_unsigned("max_height", display->max_height);
		gpu_field_unsigned("width_mm", display->physical_width_mm);
		gpu_field_unsigned("height_mm", display->physical_height_mm);
		gpu_field_hex("flags", display->flags);
		gpu_field_unsigned("generation", display->generation);
		gpu_field_text("session", "none", 8U);
		gpu_record_end();
		return;
	}

	/* The number and the name. */
	printf("  %-2u ", index + 1U);
	gpu_print_column(display->name, sizeof(display->name), 18U);

	/* A display without a monitor says only that. */
	if ((display->flags & GPU_DISPLAY_CONNECTED) == 0U) {
		printf("disconnected  id=%u\n", display->display_id);
		return;
	}

	/* Whether it is lit, or why not. */
	(void)snprintf(state, sizeof(state), "connected, dark");
	if ((display->flags & GPU_DISPLAY_ACTIVE) != 0U)
		(void)snprintf(state, sizeof(state), "connected, lit");
	if ((display->flags & GPU_DISPLAY_POWERED_OFF) != 0U)
		(void)snprintf(state, sizeof(state), "connected, off");
	if ((display->flags & GPU_DISPLAY_LIMITED) != 0U)
		(void)snprintf(state, sizeof(state), "connected, limited");
	gpu_print_column(state, sizeof(state), 20U);

	/* The mode shown now, or none while nothing is shown. */
	gpu_refresh_text(display->refresh_millihz, refresh, sizeof(refresh));
	(void)snprintf(shown, sizeof(shown), "no mode");
	if (display->current_width != 0U && display->current_height != 0U)
		(void)snprintf(shown, sizeof(shown), "%ux%u @ %s Hz", display->current_width, display->current_height, refresh);
	gpu_print_column(shown, sizeof(shown), 24U);

	/* The preferred size, the physical size and the id. */
	printf("preferred %ux%u  %ux%u mm  id=%u\n",
		display->preferred_width,
		display->preferred_height,
		display->physical_width_mm,
		display->physical_height_mm,
		display->display_id);
}

/*
 * Runs gpu GPU display: the list, a display's modes and its EDID from the
 * kernel; a change goes to the desktop session.
 */
int
gpu_display_run(
	const struct gpu_invocation *command)
{
	struct gpu_display_set set;
	struct gpu_node_facts facts;
	uint32_t index;
	int status;

	/* What the node tells, its displays among it. */
	gpu_node_facts_read(command->node, command->unit, &facts, &set);
	status = gpu_display_node_check(command, &facts);
	if (status != GPU_EXIT_OK)
		return status;

	/* A change is the desktop session's to make. */
	if (command->display_verb >= GPU_DISPLAY_VERB_SET) {
		status = gpu_session_display_change(command);
		return status;
	}

	/* The list. */
	if (command->display_verb == GPU_DISPLAY_VERB_LIST) {
		status = gpu_display_list(command, &set);
		return status;
	}

	/* The display the word names. */
	status = gpu_display_find(command, &set, &index);
	if (status != GPU_EXIT_OK)
		return status;

	/* Its modes, or its EDID. */
	if (command->display_verb == GPU_DISPLAY_VERB_MODES) {
		status = gpu_display_modes(command, &set.displays[index], index);
	} else {
		status = gpu_display_edid(command, &set.displays[index], index);
	}

	/* The exit status of the read. */
	return status;
}

/*
 * Refuses a node that did not open, did not answer, or has no display, and
 * names the nodes that have one; GPU_EXIT_OK for a node with displays.
 */
static int
gpu_display_node_check(
	const struct gpu_invocation *command,
	const struct gpu_node_facts *facts)
{
	char nodes[GPU_NODE_NAMES_MAX];
	char roles[GPU_NODE_NAMES_MAX];
	unsigned display;

	/* A node that did not open. */
	if (facts->open_error == ETIMEDOUT) {
		gpu_fail(command, "display", ETIMEDOUT, "%s: busy (reset?): the open waited %u s", command->node, GPU_OPEN_WAIT_S);
		return GPU_EXIT_FAILED;
	}
	if (facts->open_error != 0) {
		gpu_fail(command, "display", facts->open_error, "%s: %s", command->node, strerror(facts->open_error));
		return GPU_EXIT_FAILED;
	}

	/* A node that did not answer for itself. */
	if (facts->info_error != 0) {
		gpu_fail(command, "display", facts->info_error, "%s: GPU_GET_INFO: %s", command->node, strerror(facts->info_error));
		return GPU_EXIT_FAILED;
	}

	/* A node has displays when it says so in its capabilities and, if it answers, its roles. */
	display = 0U;
	if ((facts->info.capabilities & GPU_CAP_DISPLAY) != 0U)
		display = 1U;
	if (facts->device_error == 0 && (facts->device.roles & GPU_DEVICE_DISPLAY) == 0U)
		display = 0U;

	/* A node without displays names those that have them. */
	if (!display) {
		(void)snprintf(roles, sizeof(roles), "not known");
		if (facts->device_error == 0)
			(void)gpu_role_names(facts->device.roles, ",", roles, sizeof(roles));
		(void)gpu_node_display_nodes(command->unit, nodes, sizeof(nodes));
		gpu_fail(command, "display", ENODEV, "gpu%u has no display (roles %s); display nodes: %s", command->unit, roles, nodes);
		return GPU_EXIT_FAILED;
	}

	/* A node whose displays could not be read. */
	if (facts->display_error != 0) {
		gpu_fail(command, "display", facts->display_error, "%s: GPU_DISPLAY_QUERY: %s", command->node, strerror(facts->display_error));
		return GPU_EXIT_FAILED;
	}

	/* Succeeded: the node has displays, and they are read. */
	return GPU_EXIT_OK;
}

/* Prints the list of a node's displays as the kernel reports them. */
static int
gpu_display_list(
	const struct gpu_invocation *command,
	const struct gpu_display_set *set)
{
	uint32_t index;

	/* The heading for people. */
	if (!command->porcelain)
		printf("gpu%u (%s): %u display%s as the kernel reports them; desktop session: none\n", command->unit, command->node, set->total, set->total == 1U ? "" : "s");

	/* One line for each display. */
	for (index = 0U; index < set->count; index++)
		gpu_display_line(command->node, index, &set->displays[index], command->porcelain);

	/* Says when the node has more than gpu reads. */
	if (set->count < set->total && !command->porcelain)
		printf("  (%u more not shown)\n", set->total - set->count);

	/* Succeeded: the list is printed. */
	return GPU_EXIT_OK;
}

/* Prints the modes the driver offers for one display. */
static int
gpu_display_modes(
	const struct gpu_invocation *command,
	const struct gpu_display_info *display,
	uint32_t index)
{
	struct gpu_display_mode mode;
	char refresh[GPU_SIZE_TEXT_MAX];
	char size[GPU_SIZE_TEXT_MAX];
	uint32_t ordinal;
	uint32_t count;
	unsigned native;
	unsigned current;
	int status;
	int error;
	int fd;

	/* Opens the node for the requests. */
	error = gpu_node_open(command->node, &fd);
	if (error != 0) {
		gpu_fail(command, "modes", error, "%s: %s", command->node, strerror(error));
		return GPU_EXIT_FAILED;
	}

	/* How many modes the driver offers. */
	memset(&mode, 0, sizeof(mode));
	mode.version = GPU_ABI_VERSION;
	mode.size = sizeof(mode);
	mode.display_id = display->display_id;
	mode.generation = display->generation;
	mode.operation = GPU_DISPLAY_MODE_ENUMERATE;
	mode.index = GPU_DISPLAY_COUNT_ONLY;
	status = ioctl(fd, GPU_DISPLAY_MODE, &mode);
	if (status != 0) {
		error = errno;
		(void)close(fd);
		gpu_fail(command, "modes", error, "%s: display %u: GPU_DISPLAY_MODE: %s", command->node, index + 1U, strerror(error));
		return GPU_EXIT_FAILED;
	}
	count = mode.count;

	/* The heading for people. */
	if (!command->porcelain) {
		printf("gpu%u display %u (", command->unit, index + 1U);
		gpu_print_text(display->name, sizeof(display->name));
		printf("): the driver's %u mode%s; no desktop session\n", count, count == 1U ? "" : "s");
	}

	/* Each mode, in the driver's order, the default first. */
	for (ordinal = 0U; ordinal < count; ordinal++) {
		memset(&mode, 0, sizeof(mode));
		mode.version = GPU_ABI_VERSION;
		mode.size = sizeof(mode);
		mode.display_id = display->display_id;
		mode.generation = display->generation;
		mode.operation = GPU_DISPLAY_MODE_ENUMERATE;
		mode.index = ordinal;
		status = ioctl(fd, GPU_DISPLAY_MODE, &mode);
		if (status != 0) {
			error = errno;
			(void)close(fd);
			gpu_fail(command, "modes", error, "%s: display %u: mode %u: %s", command->node, index + 1U, ordinal, strerror(error));
			return GPU_EXIT_FAILED;
		}

		/* The mode the monitor prefers. */
		native = 0U;
		if (mode.width == display->preferred_width && mode.height == display->preferred_height)
			native = 1U;

		/* The mode shown now, by its size and refresh. */
		current = 0U;
		if (mode.width == display->current_width &&
		    mode.height == display->current_height &&
		    mode.refresh_millihz == display->refresh_millihz)
			current = 1U;

		/* The porcelain record. */
		if (command->porcelain) {
			gpu_display_record_start("mode", command->node, index, display);
			gpu_field_unsigned("ordinal", ordinal);
			gpu_field_unsigned("width", mode.width);
			gpu_field_unsigned("height", mode.height);
			gpu_field_unsigned("refresh_mhz", mode.refresh_millihz);
			gpu_field_unsigned("native", native);
			gpu_field_unsigned("current", current);
			gpu_field_text("source", "driver", 8U);
			gpu_record_end();
			continue;
		}

		/* The line for people: * for the current mode, + for the preferred. */
		gpu_refresh_text(mode.refresh_millihz, refresh, sizeof(refresh));
		(void)snprintf(size, sizeof(size), "%ux%u", mode.width, mode.height);
		printf("%c%c %-11s @ %s Hz\n", current ? '*' : ' ', native ? '+' : ' ', size, refresh);
	}

	/* The node is not kept open. */
	(void)close(fd);

	/* Succeeded: the modes are printed. */
	return GPU_EXIT_OK;
}

/* Prints one display's EDID: its decoded parts, or with --raw its bytes. */
static int
gpu_display_edid(
	const struct gpu_invocation *command,
	const struct gpu_display_info *display,
	uint32_t index)
{
	struct gpu_display_edid edid;
	int status;
	int error;
	int fd;

	/* Opens the node for the request. */
	error = gpu_node_open(command->node, &fd);
	if (error != 0) {
		gpu_fail(command, "edid", error, "%s: %s", command->node, strerror(error));
		return GPU_EXIT_FAILED;
	}

	/* Reads the EDID the driver holds. */
	memset(&edid, 0, sizeof(edid));
	edid.version = GPU_ABI_VERSION;
	edid.size = sizeof(edid);
	edid.display_id = display->display_id;
	edid.generation = display->generation;
	status = ioctl(fd, GPU_DISPLAY_EDID, &edid);
	error = errno;
	(void)close(fd);
	if (status != 0) {
		gpu_fail(command, "edid", error, "%s: display %u: GPU_DISPLAY_EDID: %s", command->node, index + 1U, strerror(error));
		return GPU_EXIT_FAILED;
	}

	/* A display whose sink sent no EDID. */
	if (edid.length == 0U || edid.length > GPU_DISPLAY_EDID_MAX) {
		gpu_fail(command, "edid", ENOENT, "%s: display %u has no EDID", command->node, index + 1U);
		return GPU_EXIT_FAILED;
	}

	/* The bytes, or what they say. */
	if (command->raw) {
		gpu_display_edid_raw(command, display, index, &edid);
	} else {
		gpu_display_edid_lines(command, display, index, &edid);
	}

	/* Succeeded: the EDID is printed. */
	return GPU_EXIT_OK;
}

/* Prints what an EDID's base block says. */
static void
gpu_display_edid_lines(
	const struct gpu_invocation *command,
	const struct gpu_display_info *display,
	uint32_t index,
	const struct gpu_display_edid *edid)
{
	struct gpu_edid_summary summary;

	/* Decodes the base block. */
	gpu_edid_decode(edid->data, edid->length, &summary);

	/* The porcelain record. */
	if (command->porcelain) {
		gpu_display_record_start("edid", command->node, index, display);
		gpu_field_unsigned("bytes", edid->length);
		gpu_field_unsigned("total", edid->total);
		gpu_field_unsigned("valid", summary.valid);
		if (summary.valid) {
			gpu_field_unsigned("checksum", summary.checksum_ok);
			gpu_field_text("manufacturer", summary.manufacturer, sizeof(summary.manufacturer));
			gpu_field_hex("product", summary.product);
			gpu_field_hex("serial", summary.serial);
			gpu_field_unsigned("week", summary.week);
			gpu_field_unsigned("year", summary.year);
			gpu_field_unsigned("model_year", summary.model_year);
			gpu_field_unsigned("version", summary.version);
			gpu_field_unsigned("revision", summary.revision);
			gpu_field_unsigned("width_cm", summary.width_cm);
			gpu_field_unsigned("height_cm", summary.height_cm);
			gpu_field_unsigned("extensions", summary.extensions);
			gpu_field_text("name", summary.name, sizeof(summary.name));
			gpu_field_text("serial_text", summary.serial_text, sizeof(summary.serial_text));
		}
		gpu_record_end();
		return;
	}

	/* The heading for people. */
	printf("gpu%u display %u (", command->unit, index + 1U);
	gpu_print_text(display->name, sizeof(display->name));
	printf("): EDID, %u bytes\n", edid->length);

	/* Bytes that are no EDID say so. */
	if (!summary.valid) {
		printf("  not an EDID: the base block's header is missing\n");
		return;
	}

	/* The maker, the product and the serial number. */
	printf("  manufacturer   ");
	gpu_print_text(summary.manufacturer, sizeof(summary.manufacturer));
	printf("\n  product        0x%04x\n", summary.product);
	printf("  serial         0x%08x\n", summary.serial);

	/* When it was made, or the model year. */
	if (summary.model_year) {
		printf("  model year     %u\n", summary.year);
	} else {
		printf("  made           week %u, %u\n", summary.week, summary.year);
	}

	/* The version, the size and the blocks that follow. */
	printf("  version        %u.%u\n", summary.version, summary.revision);
	printf("  size           %ux%u cm\n", summary.width_cm, summary.height_cm);
	printf("  extensions     %u\n", summary.extensions);

	/* The texts, when the sink sent them. */
	if (summary.name[0] != '\0') {
		printf("  name           ");
		gpu_print_text(summary.name, sizeof(summary.name));
		printf("\n");
	}
	if (summary.serial_text[0] != '\0') {
		printf("  serial text    ");
		gpu_print_text(summary.serial_text, sizeof(summary.serial_text));
		printf("\n");
	}

	/* Whether the base block adds up. */
	if (summary.checksum_ok) {
		printf("  checksum       ok\n");
	} else {
		printf("  checksum       bad\n");
	}
}

/* Prints an EDID's bytes in hexadecimal. */
static void
gpu_display_edid_raw(
	const struct gpu_invocation *command,
	const struct gpu_display_info *display,
	uint32_t index,
	const struct gpu_display_edid *edid)
{
	char hex[GPU_DISPLAY_EDID_MAX * 2U + 1U];
	uint32_t at;

	/* The bytes as one hexadecimal text. */
	for (at = 0U; at < edid->length; at++)
		(void)snprintf(hex + at * 2U, 3U, "%02x", edid->data[at]);
	hex[edid->length * 2U] = '\0';

	/* The porcelain record. */
	if (command->porcelain) {
		gpu_display_record_start("edid", command->node, index, display);
		gpu_field_unsigned("bytes", edid->length);
		gpu_field_unsigned("total", edid->total);
		gpu_field_text("data", hex, sizeof(hex));
		gpu_record_end();
		return;
	}

	/* The bytes for people, 16 a line after their offset. */
	printf("gpu%u display %u (", command->unit, index + 1U);
	gpu_print_text(display->name, sizeof(display->name));
	printf("): EDID, %u bytes\n", edid->length);
	for (at = 0U; at < edid->length; at += 16U)
		printf("  %04x  %.32s\n", at, hex + at * 2U);
}

/* Copies a display descriptor's text, cut at its end mark and without the padding. */
static void
gpu_edid_text(
	const uint8_t *descriptor,
	char *text)
{
	size_t length;

	/* Copies up to the end mark or the descriptor's end. */
	length = 0U;
	while (length < GPU_EDID_TEXT_MAX && descriptor[GPU_EDID_TEXT + length] != 0x0aU) {
		text[length] = (char)descriptor[GPU_EDID_TEXT + length];
		length++;
	}

	/* Drops the spaces that pad the end. */
	while (length > 0U && text[length - 1U] == ' ')
		length--;
	text[length] = '\0';
}

/* Finds the display the command names, or reports why none is; GPU_EXIT_OK with its index. */
static int
gpu_display_find(
	const struct gpu_invocation *command,
	const struct gpu_display_set *set,
	uint32_t *index)
{
	int status;

	/* Looks the word up in the list. */
	status = gpu_display_select(set, command->display, index);

	/* A word that names more than one display. */
	if (status == -2) {
		gpu_fail(command, "display", EINVAL, "%s names more than one display of gpu%u; give its number or id=N", command->display, command->unit);
		return GPU_EXIT_FAILED;
	}

	/* A word that names none. */
	if (status != 0) {
		gpu_fail(command, "display", ENOENT, "gpu%u has no display %s", command->unit, command->display);
		return GPU_EXIT_FAILED;
	}

	/* Succeeded: the display's index. */
	return GPU_EXIT_OK;
}

/* Starts a porcelain record about one display with the keys that name it. */
static void
gpu_display_record_start(
	const char *kind,
	const char *node,
	uint32_t index,
	const struct gpu_display_info *display)
{
	/* The kind, the node, the number from 1, the kernel's id and the name. */
	gpu_record_begin(kind);
	gpu_field_text("node", node, GPU_NODE_PATH_MAX);
	gpu_field_unsigned("index", index + 1U);
	gpu_field_unsigned("display_id", display->display_id);
	gpu_field_text("key", display->name, sizeof(display->name));
}

/* Reads a word made of decimal digits alone; 0 with its value, or -1. */
static int
gpu_display_digits(
	const char *word,
	uint32_t *value)
{
	int status;

	/* A word of digits alone. */
	if (word[0] < '0' || word[0] > '9')
		return -1;
	status = gpu_parse_number(word, UINT32_MAX, value);
	if (status != 0)
		return -1;

	/* Succeeded: the number. */
	return 0;
}
