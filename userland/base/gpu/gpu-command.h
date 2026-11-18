/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of /sbin/gpu that its files share: the command a line asks
 * for, what is known of one GPU node and one display, and the functions of
 * each file.
 */

#ifndef USERLAND_BASE_GPU_COMMAND_H
#define USERLAND_BASE_GPU_COMMAND_H

#include <uapi/gpu.h>
#include <uapi/gpu-display.h>
#include <uapi/gpu-scanout.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Marks a parameter a function has to take but does not use. */
#define UNUSED_PARAMETER(name)		((void)(name))

/* The exit statuses: done, refused or failed, and a wrong use. */
#define GPU_EXIT_OK			0
#define GPU_EXIT_FAILED			1
#define GPU_EXIT_USAGE			2

/* The longest node path, "/dev/gpu" and up to ten digits. */
#define GPU_NODE_PATH_MAX		32U

/* The most nodes one run looks at. */
#define GPU_NODES_MAX			64U

/* The most displays of one node one run reads. */
#define GPU_DISPLAYS_MAX		16U

/* How long the open of a node may wait (s), for a device in a reset. */
#define GPU_OPEN_WAIT_S			10U

/* The room an escaped text needs for each byte: "\xHH". */
#define GPU_ESCAPE_EXPANSION		4U

/* The room of the parse error's text. */
#define GPU_ERROR_TEXT_MAX		128U

/*
 * What a command line asks for: the list, one node's summary, or one area
 * of a node.
 */
enum gpu_area {
	GPU_AREA_LIST,
	GPU_AREA_HELP,
	GPU_AREA_SHOW,
	GPU_AREA_CAPS,
	GPU_AREA_DISPLAY,
	GPU_AREA_VULKAN,
	GPU_AREA_GL,
	GPU_AREA_VIDEO,
	GPU_AREA_RESET,
	GPU_AREA_DIAGNOSE
};

/* What the display area is asked to do. */
enum gpu_display_verb {
	GPU_DISPLAY_VERB_LIST,
	GPU_DISPLAY_VERB_MODES,
	GPU_DISPLAY_VERB_EDID,
	GPU_DISPLAY_VERB_SET,
	GPU_DISPLAY_VERB_KEEP,
	GPU_DISPLAY_VERB_REVERT,
	GPU_DISPLAY_VERB_RESET
};

/* Which part of a Vulkan device is shown. */
enum gpu_vulkan_section {
	GPU_VULKAN_PROPERTIES,
	GPU_VULKAN_FEATURES,
	GPU_VULKAN_LIMITS,
	GPU_VULKAN_FORMATS,
	GPU_VULKAN_EXTENSIONS,
	GPU_VULKAN_QUEUES
};

/* Which GL API is asked about. */
enum gpu_gl_api {
	GPU_GL_BOTH,
	GPU_GL_ES,
	GPU_GL_DESKTOP
};

/*
 * One command line, taken apart.
 *
 * gpu_command_parse fills it from the words alone, without looking at the
 * machine, so the grammar is the same for every caller.  The words it
 * points to are the caller's argv and live as long as the run.  A wrong use
 * leaves usage set and error holding the reason.
 */
struct gpu_invocation {
	enum gpu_area area;
	unsigned porcelain;
	unsigned usage;
	char error[GPU_ERROR_TEXT_MAX];

	/* The node named: "/dev/gpuN" and N. */
	char node[GPU_NODE_PATH_MAX];
	unsigned unit;

	/* caps: --capset=ID. */
	unsigned capset_given;
	uint32_t capset_id;

	/* display: the verb, the display named, the mode asked for and the options of a change. */
	enum gpu_display_verb display_verb;
	const char *display;
	uint32_t width;
	uint32_t height;
	uint32_t refresh_mhz;
	unsigned raw;
	unsigned keep;
	unsigned timeout_given;
	uint32_t timeout_s;
	const char *session;

	/* vulkan and gl: what is shown. */
	enum gpu_vulkan_section vulkan_section;
	enum gpu_gl_api gl_api;

	/* reset: --wait=MS and --yes. */
	unsigned wait_given;
	uint32_t wait_ms;
	unsigned yes;

	/* diagnose: list, or one kind with its argument and the file the report goes to. */
	unsigned diagnose_list;
	const char *diagnose_kind;
	unsigned argument_given;
	uint32_t argument;
	const char *output;
};

/*
 * What one node told about itself.
 *
 * open_error is the errno of the open (ETIMEDOUT when it waited past
 * GPU_OPEN_WAIT_S); the other errors are those of each request and are
 * meaningful only when the open worked.  displays and connected count the
 * node's outputs and are 0 for a node without GPU_CAP_DISPLAY.
 */
struct gpu_node_facts {
	char path[GPU_NODE_PATH_MAX];
	unsigned unit;
	int open_error;
	int info_error;
	struct gpu_info info;
	int device_error;
	struct gpu_device_info device;
	int display_error;
	uint32_t displays;
	uint32_t connected;
};

/*
 * The outputs of one node as the kernel listed them, in its index order.
 *
 * Each entry is a GPU_DISPLAY_QUERY answer whose name is made a terminated
 * string.  count is how many were read, total how many the node said.
 */
struct gpu_display_set {
	uint32_t total;
	uint32_t count;
	struct gpu_display_info displays[GPU_DISPLAYS_MAX];
};

/*
 * The parts of an EDID base block a person reads.
 *
 * Texts are terminated, cut at the descriptor's 13 bytes and stripped of the
 * padding; they come from the sink and are escaped when printed.  valid says
 * that the 8-byte header is there; checksum_ok that the block sums to 0.
 */
struct gpu_edid_summary {
	unsigned valid;
	unsigned checksum_ok;
	char manufacturer[4];
	uint32_t product;
	uint32_t serial;
	uint32_t week;
	uint32_t year;
	unsigned model_year;
	uint32_t version;
	uint32_t revision;
	uint32_t width_cm;
	uint32_t height_cm;
	uint32_t extensions;
	char name[14];
	char serial_text[14];
};

/* main.c */
int gpu_command_parse(int argc, char **argv, struct gpu_invocation *command);
int gpu_parse_number(const char *word, uint32_t max, uint32_t *value);
int gpu_parse_mode(const char *word, uint32_t *width, uint32_t *height, uint32_t *refresh_mhz);
void gpu_help(FILE *stream);

/* output.c */
size_t gpu_escape(const char *text, size_t length, unsigned porcelain, char *out, size_t size);
size_t gpu_text_length(const char *text, size_t max);
void gpu_porcelain_begin(void);
void gpu_record_begin(const char *kind);
void gpu_record_end(void);
void gpu_field_text(const char *key, const char *text, size_t max);
void gpu_field_unsigned(const char *key, unsigned long long value);
void gpu_field_hex(const char *key, unsigned long long value);
void gpu_print_text(const char *text, size_t max);
void gpu_print_column(const char *text, size_t max, size_t width);
const char *gpu_capability_name(uint32_t bit);
size_t gpu_capability_names(uint32_t capabilities, const char *separator, char *out, size_t size);
size_t gpu_role_names(uint32_t roles, const char *separator, char *out, size_t size);
const char *gpu_error_name(int error);
void gpu_fail(const struct gpu_invocation *command, const char *verb, int error, const char *format, ...) __attribute__((format(printf, 4, 5)));
void gpu_refresh_text(uint32_t refresh_mhz, char *text, size_t size);

/* node.c */
int gpu_node_parse_name(const char *word, char *path, size_t size, unsigned *unit);
int gpu_node_unit_of(const char *name, unsigned *unit);
int gpu_node_open(const char *path, int *fd);
void gpu_node_facts_read(const char *path, unsigned unit, struct gpu_node_facts *facts, struct gpu_display_set *set);
int gpu_node_list(const struct gpu_invocation *command);
int gpu_node_show(const struct gpu_invocation *command);
int gpu_node_caps(const struct gpu_invocation *command);
int gpu_node_display_nodes(unsigned self, char *out, size_t size);

/* display.c */
int gpu_display_read(int fd, struct gpu_display_set *set);
int gpu_display_select(const struct gpu_display_set *set, const char *word, uint32_t *index);
void gpu_edid_decode(const uint8_t *bytes, size_t length, struct gpu_edid_summary *summary);
void gpu_display_line(const char *node, uint32_t index, const struct gpu_display_info *display, unsigned porcelain);
int gpu_display_run(const struct gpu_invocation *command);

/* session.c */
int gpu_session_display_change(const struct gpu_invocation *command);

/* vulkan.c, gl.c, video.c, service.c */
int gpu_vulkan_run(const struct gpu_invocation *command);
int gpu_gl_run(const struct gpu_invocation *command);
int gpu_video_run(const struct gpu_invocation *command);
int gpu_service_reset(const struct gpu_invocation *command);
int gpu_service_diagnose(const struct gpu_invocation *command);

#endif
