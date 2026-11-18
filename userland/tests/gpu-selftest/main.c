/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * gpu-selftest: the PowerVR stage test.  It runs the stages in order, each in
 * a child of its own with a time limit, so a hung GPU or a crash still lets
 * the report go on, and prints one line a stage:
 *
 *   gpu-selftest: <stage>: ok
 *   gpu-selftest: <stage>: fail(<reason>)
 *   gpu-selftest: <stage>: skip(<stage it needs>)
 *   gpu-selftest: <stage>: skip(not built)
 *
 * and last "gpu-selftest: N ok, M fail, K skip".  The kernel stage reads
 * hw.gpu.powervr.stages and copies it on a line "gpu-selftest: stages: ...".
 * instance, device and compute come from one run of vkcompute, image
 * from a run of vkimage, and clear, copy, draw, sample, depth and
 * present-clear each from a run of vkstage with the stage's name
 * (present-clear runs "vkstage present", one cleared frame on the display held
 * three seconds through the WSI readback route,
 * under a limit of 30 seconds).  present first has vkdemo render its
 * frame at 0 ms offscreen to a PPM file and checks it: the corners the
 * clear colour 101828 exactly, the centre within 24 a channel of e06895
 * (the frame llvmpipe renders); then runs vkdemo
 * on the display for four seconds, all under the same limit.  It needs
 * depth, as vkdemo renders with a depth attachment, and is skipped with
 * depth's name until depth passes.
 *
 * Options (the host test uses them in place of the board's):
 *   -c          also write each line to /dev/console
 *   -s FILE     read the stages text from FILE, not the sysctl
 *   -n NODE     a render node to open (more than one -n tries each)
 *   -v PATH     the vkcompute program
 *   -i PATH     the vkimage program
 *   -g PATH     the vkstage program
 *   -d PATH     the vkdemo program
 *   -f PATH     where vkdemo writes its offscreen frame
 *   -t SECONDS  the time limit of each stage (default 10; present-clear 3 times it)
 *
 * The exit status is 0 when no stage failed.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef GPU_SELFTEST_HOST
#include <sys/sysctl.h>
#endif

/* The most render nodes -n names. */
#define SELFTEST_NODES_MAX	4

/*
 * How long the kernel stage waits for the GPU to attach: the firmware is
 * loaded after the root file system is live (the driver's late start),
 * and the boot's selftest may come before it.
 */
#define SELFTEST_ATTACH_SECONDS	120

/* The most bytes of a reason or of the stages text. */
#define SELFTEST_TEXT_MAX	256

/* What a stage came to. */
enum outcome {
	OUTCOME_OK,
	OUTCOME_FAIL,
	OUTCOME_SKIP
};

/* The run's settings and what it has seen. */
struct selftest {
	int console;
	const char *stages_file;
	const char *nodes[SELFTEST_NODES_MAX];
	int node_count;
	const char *vkcompute;
	const char *vkimage;
	const char *vkstage;
	const char *vkdemo;
	const char *frame;
	const char *argument;
	int limit;
	int counts[3];
};

/*
 * One stage: its name, the stage it needs (or NULL), how it runs in the
 * child, vkstage's argument for it (or NULL), and how many times the time
 * limit it gets.
 */
struct stage {
	const char *name;
	const char *needs;
	int (*run)(struct selftest *test, char *reason, size_t capacity);
	const char *argument;
	int limit_times;
};

static void report(struct selftest *test, const char *format, ...);
static int run_child(struct selftest *test, const struct stage *stage, char *reason, size_t capacity);
static int read_stages(struct selftest *test, char *text, size_t capacity);
static int gpu_attaching(void);
static int wait_attach(struct selftest *test, char *text, size_t capacity);
static int stdout_is_console(void);
static int stage_kernel(struct selftest *test, char *reason, size_t capacity);
static int stage_node(struct selftest *test, char *reason, size_t capacity);
static int stage_vkcompute(struct selftest *test, char *reason, size_t capacity);
static int stage_image(struct selftest *test, char *reason, size_t capacity);
static int stage_vkstage(struct selftest *test, char *reason, size_t capacity);
static int stage_vkdemo(struct selftest *test, char *reason, size_t capacity);
static int run_program(const char *path, const char *argument, char *line, size_t capacity);
static int run_program_list(const char *path, char *const *arguments, char *line, size_t capacity);
static int frame_check(const char *path, char *reason, size_t capacity);
static void frame_describe(FILE *file, char *reason, size_t capacity);
static int ppm_header(FILE *file, unsigned *width, unsigned *height, unsigned *maximum);
static int ppm_number(FILE *file, unsigned *value);
static int is_vkcompute_stage(const char *name);
static void vkcompute_outcomes(int status, const char *line, enum outcome *results, char reasons[3][SELFTEST_TEXT_MAX]);

/* The stages, in order; a NULL run is "not built". */
static const struct stage stages[] = {
	/* 13 times the default 10 s: the kernel stage waits up to SELFTEST_ATTACH_SECONDS for the GPU to attach. */
	{ "kernel", NULL, stage_kernel, NULL, 13 },
	{ "node", "kernel", stage_node, NULL, 1 },
	{ "instance", "node", NULL, NULL, 1 },
	{ "device", "instance", NULL, NULL, 1 },
	{ "compute", "device", NULL, NULL, 1 },
	{ "image", "device", stage_image, NULL, 1 },
	{ "clear", "image", stage_vkstage, "clear", 1 },
	{ "copy", "image", stage_vkstage, "copy", 1 },
	{ "draw", "image", stage_vkstage, "draw", 1 },
	{ "sample", "draw", stage_vkstage, "sample", 1 },
	{ "depth", "draw", stage_vkstage, "depth", 1 },
	{ "present-clear", "device", stage_vkstage, "present", 3 },
	{ "present", "depth", stage_vkdemo, "--duration=4", 3 },
};

/* The stages that come from the one vkcompute run, in the order of its results. */
static const char *const vkcompute_stages[3] = { "instance", "device", "compute" };

/* Prints one line to standard output and, with -c, to the console. */
static void
report(
	struct selftest *test,
	const char *format,
	...)
{
	char line[SELFTEST_TEXT_MAX * 2];
	va_list list;
	int fd;

	/* Formats the line once. */
	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);

	/* Standard output, flushed so a later child's output follows it. */
	printf("%s\n", line);
	fflush(stdout);

	/* The console, opened for each line so a missing console costs nothing. */
	if (!test->console)
		return;
	fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd < 0)
		return;
	dprintf(fd, "%s\n", line);
	close(fd);
}

/*
 * Runs stage->run in a child under the time limit.  Returns 0 when the
 * child said ok, 1 with the reason otherwise: its own, "timeout" when the
 * limit ran out (the child is killed), "crash" when it died.
 */
static int
run_child(
	struct selftest *test,
	const struct stage *stage,
	char *reason,
	size_t capacity)
{
	struct timespec pause;
	ssize_t got;
	pid_t child;
	pid_t done;
	int pipes[2];
	int status;
	int waited;
	int result;

	/* The child sends its reason back through a pipe. */
	snprintf(reason, capacity, "error");
	if (pipe(pipes) != 0)
		return 1;
	child = fork();
	if (child < 0) {
		close(pipes[0]);
		close(pipes[1]);
		return 1;
	}

	/* The child: runs the stage with its vkstage argument, writes its reason, exits 0 on ok. */
	if (child == 0) {
		setpgid(0, 0);
		close(pipes[0]);
		test->argument = stage->argument;
		result = stage->run(test, reason, capacity);
		if (result != 0) {
			got = write(pipes[1], reason, strlen(reason));
			(void)got;
		}
		_exit(result == 0 ? 0 : 1);
	}

	/* The parent: its own group for the child, so a kill reaches what it started; then polls every 50 ms until the child ends or the limit runs out. */
	setpgid(child, child);
	close(pipes[1]);
	pause.tv_sec = 0;
	pause.tv_nsec = 50000000L;
	waited = 0;
	for (;;) {
		done = waitpid(child, &status, WNOHANG);
		if (done == child)
			break;
		if (waited >= test->limit * stage->limit_times * 20) {
			kill(-child, SIGKILL);
			kill(child, SIGKILL);
			waitpid(child, &status, 0);
			close(pipes[0]);
			snprintf(reason, capacity, "timeout");
			return 1;
		}
		nanosleep(&pause, NULL);
		waited++;
	}

	/* A child that died is a crash; one that exited 0 is ok. */
	if (!WIFEXITED(status)) {
		close(pipes[0]);
		snprintf(reason, capacity, "crash");
		return 1;
	}
	if (WEXITSTATUS(status) == 0) {
		close(pipes[0]);
		return 0;
	}

	/* Otherwise the child's reason, when it sent one. */
	got = read(pipes[0], reason, capacity - 1U);
	close(pipes[0]);
	if (got > 0)
		reason[got] = '\0';
	else
		snprintf(reason, capacity, "error");
	return 1;
}

/* Reads the stages text from -s's file or from hw.gpu.powervr.stages. */
static int
read_stages(
	struct selftest *test,
	char *text,
	size_t capacity)
{
	FILE *file;
	size_t length;

	/* The file the host test gives. */
	if (test->stages_file != NULL) {
		file = fopen(test->stages_file, "r");
		if (file == NULL)
			return -1;
		length = fread(text, 1, capacity - 1U, file);
		fclose(file);
		text[length] = '\0';
		text[strcspn(text, "\n")] = '\0';
		return 0;
	}

#ifdef GPU_SELFTEST_HOST
	/* The host has no such sysctl. */
	errno = ENOENT;
	return -1;
#else
	/* The kernel's text with its NUL. */
	length = capacity;
	if (sysctlbyname("hw.gpu.powervr.stages", text, &length, NULL, 0) != 0)
		return -1;
	text[capacity - 1U] = '\0';
	return 0;
#endif
}

/*
 * Reports whether the kernel says a GPU is still attaching
 * (hw.gpu.attaching above 0); a kernel without the count, and the host,
 * say not.
 */
static int
gpu_attaching(void)
{
#ifdef GPU_SELFTEST_HOST
	/* The host has no such sysctl. */
	return 0;
#else
	uint32_t attaching;
	size_t size;
	int error;

	/* The count, when the kernel has it. */
	attaching = 0;
	size = sizeof(attaching);
	error = sysctlbyname("hw.gpu.attaching", &attaching, &size, NULL, 0);
	if (error != 0 || size != sizeof(attaching))
		return 0;

	/* Succeeded: whether some GPU is attaching. */
	return attaching != 0 ? 1 : 0;
#endif
}

/*
 * Reads the stages text, waiting a second at a time while the firmware
 * stage is still "-" (not reached): until it is something else, until
 * hw.gpu.attaching, once seen above 0, falls back to 0, or for at most
 * SELFTEST_ATTACH_SECONDS.  Says how long it waited when it did.
 * Returns -1 when there is no text, 1 when the wait ran out with the
 * firmware still "-", 0 otherwise.
 */
static int
wait_attach(
	struct selftest *test,
	char *text,
	size_t capacity)
{
	int waited;
	int seen;
	int attaching;
	int error;

	/* The text, again each second while the firmware has not been reached. */
	waited = 0;
	seen = 0;
	for (;;) {
		error = read_stages(test, text, capacity);
		if (error != 0)
			return -1;
		if (strstr(text, "firmware=-") == NULL)
			break;

		/* An attach that ended leaves the firmware as it is. */
		attaching = gpu_attaching();
		if (attaching)
			seen = 1;
		else if (seen)
			break;

		/* The limit. */
		if (waited >= SELFTEST_ATTACH_SECONDS)
			break;
		sleep(1);
		waited++;
	}

	/* How long it waited, when it did. */
	if (waited != 0)
		report(test, "gpu-selftest: waited %d s for the GPU to attach", waited);

	/* The firmware still not reached when the wait ran out. */
	if (waited >= SELFTEST_ATTACH_SECONDS && strstr(text, "firmware=-") != NULL)
		return 1;

	/* Succeeded: the text as the kernel has it now. */
	return 0;
}

/*
 * Says whether standard output is the console itself (the boot's service
 * runs the selftest with its output there), so that -c does not write
 * each line twice.
 */
static int
stdout_is_console(void)
{
	struct stat output;
	struct stat console;
	int error;

	/* Both, as files. */
	error = fstat(STDOUT_FILENO, &output);
	if (error != 0)
		return 0;
	error = stat("/dev/console", &console);
	if (error != 0)
		return 0;

	/* Succeeded: the same device or the same file. */
	if (S_ISCHR(output.st_mode) && S_ISCHR(console.st_mode) && output.st_rdev == console.st_rdev)
		return 1;
	return output.st_dev == console.st_dev && output.st_ino == console.st_ino ? 1 : 0;
}

/*
 * The kernel stage: the stages text, copied on a line, is ok when the
 * firmware and the render node are ok; otherwise the first of them that
 * is not.
 */
static int
stage_kernel(
	struct selftest *test,
	char *reason,
	size_t capacity)
{
	char text[SELFTEST_TEXT_MAX];
	const char *word;
	size_t length;
	int error;

	/* The text once the GPU attached (or the wait ran out), copied for the reader. */
	error = wait_attach(test, text, sizeof(text));
	if (error < 0) {
		snprintf(reason, capacity, "no stages");
		return 1;
	}
	report(test, "gpu-selftest: stages: %s", text);
	if (error > 0) {
		snprintf(reason, capacity, "attach timeout, firmware=-");
		return 1;
	}

	/* The two stages before any node. */
	word = strstr(text, "firmware=ok");
	if (word == NULL) {
		word = strstr(text, "firmware=");
		length = word == NULL ? 0 : strcspn(word, " ");
		snprintf(reason, capacity, "%.*s", (int)length, word == NULL ? "firmware" : word);
		return 1;
	}
	word = strstr(text, "render-node=ok");
	if (word == NULL) {
		word = strstr(text, "render-node=");
		length = word == NULL ? 0 : strcspn(word, " ");
		snprintf(reason, capacity, "%.*s", (int)length, word == NULL ? "render-node" : word);
		return 1;
	}

	/* Succeeded: the kernel came as far as the node. */
	return 0;
}

/* The node stage: a render node opens for reading and writing. */
static int
stage_node(
	struct selftest *test,
	char *reason,
	size_t capacity)
{
	int fd;
	int i;
	int last;

	/* Tries each node; the first that opens is enough. */
	last = ENOENT;
	for (i = 0; i < test->node_count; i++) {
		fd = open(test->nodes[i], O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			close(fd);
			return 0;
		}
		last = errno;
	}

	/* None opened: the last error's name. */
	snprintf(reason, capacity, "%s", last == ENOENT ? "enoent" : last == EBUSY ? "ebusy" : last == EACCES ? "eacces" : "open");
	return 1;
}

/*
 * The child of the vkcompute stages: runs vkcompute with its output in
 * the reason buffer, as "<exit status> <first line>".
 */
static int
stage_vkcompute(
	struct selftest *test,
	char *reason,
	size_t capacity)
{
	char line[SELFTEST_TEXT_MAX];
	int status;

	/* Always "fails" to the parent so the line comes back; the parent reads the status in it. */
	status = run_program(test->vkcompute, NULL, line, sizeof(line));
	snprintf(reason, capacity, "%d %s", status, line);
	return 1;
}

/* The child of the image stage: vkimage's exit 0 is ok, else its line is the reason. */
static int
stage_image(
	struct selftest *test,
	char *reason,
	size_t capacity)
{
	char line[SELFTEST_TEXT_MAX];
	int status;

	/* One run of vkimage. */
	status = run_program(test->vkimage, NULL, line, sizeof(line));
	if (status == 0)
		return 0;

	/* Failed: its line, or its status when it printed none. */
	if (line[0] != '\0')
		snprintf(reason, capacity, "%s", line);
	else
		snprintf(reason, capacity, "exit %d", status);
	return 1;
}

/* The child of a vkstage stage: vkstage's exit 0 is ok, else its line is the reason. */
static int
stage_vkstage(
	struct selftest *test,
	char *reason,
	size_t capacity)
{
	char line[SELFTEST_TEXT_MAX];
	int status;

	/* One run of vkstage with the stage's argument. */
	status = run_program(test->vkstage, test->argument, line, sizeof(line));
	if (status == 0)
		return 0;

	/* Failed: its line, or its status when it printed none. */
	if (line[0] != '\0')
		snprintf(reason, capacity, "%s", line);
	else
		snprintf(reason, capacity, "exit %d", status);
	return 1;
}

/*
 * Runs vkdemo offscreen at 0 ms, checks the frame it wrote, then runs it
 * on the display with the stage's argument; fails with the first line
 * that went wrong.
 */
static int
stage_vkdemo(
	struct selftest *test,
	char *reason,
	size_t capacity)
{
	char output[SELFTEST_TEXT_MAX];
	char line[SELFTEST_TEXT_MAX];
	char *arguments[6];
	int status;

	/* The offscreen frame at 0 ms, written to the frame file; an older frame is removed first so it cannot pass. */
	(void)unlink(test->frame);
	snprintf(output, sizeof(output), "--output=%s", test->frame);
	arguments[0] = (char *)test->vkdemo;
	arguments[1] = "--offscreen";
	arguments[2] = "--time-ms=0";
	arguments[3] = "--hold=0";
	arguments[4] = output;
	arguments[5] = NULL;
	status = run_program_list(test->vkdemo, arguments, line, sizeof(line));
	if (status != 0) {
		snprintf(reason, capacity, "offscreen: %s", line[0] != '\0' ? line : "no output");
		return 1;
	}

	/* The frame's pixels. */
	if (frame_check(test->frame, reason, capacity) != 0)
		return 1;

	/* Then the display. */
	status = run_program(test->vkdemo, test->argument, line, sizeof(line));
	if (status == 0)
		return 0;

	/* Failed: its line, or its status when it printed none. */
	if (line[0] != '\0')
		snprintf(reason, capacity, "%s", line);
	else
		snprintf(reason, capacity, "exit %d", status);
	return 1;
}

/*
 * Reads a binary PPM's header byte by byte: "P6", then the width, height
 * and maximum as decimal numbers each after white space, then the one
 * white space byte before the pixels.  (The library's fscanf reads one
 * line only, so a header over several lines is read here instead.)
 */
static int
ppm_header(
	FILE *file,
	unsigned *width,
	unsigned *height,
	unsigned *maximum)
{
	int c;
	int error;

	/* The magic's first byte. */
	c = fgetc(file);
	if (c != 'P')
		return -1;

	/* The magic's second byte: binary pixels. */
	c = fgetc(file);
	if (c != '6')
		return -1;

	/* The width. */
	error = ppm_number(file, width);
	if (error != 0)
		return -1;

	/* The height. */
	error = ppm_number(file, height);
	if (error != 0)
		return -1;

	/* The largest channel value. */
	error = ppm_number(file, maximum);
	if (error != 0)
		return -1;

	/* Succeeded: the single white space byte ending the header was read with the last number. */
	return 0;
}

/* Reads white space (and comments), then a decimal number and the one byte after it, which must be white space. */
static int
ppm_number(
	FILE *file,
	unsigned *value)
{
	unsigned number;
	int digits;
	int c;

	/* White space and comment lines. */
	c = fgetc(file);
	while (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '#') {
		if (c == '#') {
			while (c != '\n' && c != EOF)
				c = fgetc(file);
		}

		c = fgetc(file);
	}

	/* The digits. */
	number = 0;
	digits = 0;
	while (c >= '0' && c <= '9' && digits < 9) {
		number = number * 10U + (unsigned)(c - '0');
		digits++;
		c = fgetc(file);
	}

	/* Refuses a number without digits. */
	if (digits == 0)
		return -1;

	/* Refuses a number not followed by white space. */
	if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
		return -1;

	/* Succeeded: the number, and the white space byte after it consumed. */
	*value = number;
	return 0;
}

/*
 * Checks vkdemo's 320x240 PPM frame at 0 ms: two corners exactly the
 * clear colour 101828, the centre within 24 a channel of e06895.
 */
static int
frame_check(
	const char *path,
	char *reason,
	size_t capacity)
{
	static const int points[3][2] = { { 5, 5 }, { 314, 234 }, { 160, 120 } };
	static const unsigned char want[3][3] = { { 0x10, 0x18, 0x28 }, { 0x10, 0x18, 0x28 }, { 0xe0, 0x68, 0x95 } };
	static const int slack[3] = { 0, 0, 24 };
	unsigned char pixel[3];
	unsigned width;
	unsigned height;
	unsigned maximum;
	long start;
	long place;
	size_t got;
	FILE *file;
	int index;
	int channel;
	int difference;
	int error;

	/* Opens the frame. */
	file = fopen(path, "rb");
	if (file == NULL) {
		snprintf(reason, capacity, "no frame %s", path);
		return 1;
	}

	/* Reads the header; says what the file held when it is not a 320x240 PPM. */
	error = ppm_header(file, &width, &height, &maximum);
	if (error != 0 || width != 320U || height != 240U || maximum != 255U) {
		frame_describe(file, reason, capacity);
		fclose(file);
		return 1;
	}

	/* The pixels start after the header. */
	start = ftell(file);

	/* Compares each point with its expected colour. */
	for (index = 0; index < 3; index++) {
		/* Reads the point's pixel. */
		place = start + ((long)points[index][1] * 320L + points[index][0]) * 3L;
		error = fseek(file, place, SEEK_SET);
		if (error != 0) {
			fclose(file);
			snprintf(reason, capacity, "frame short");
			return 1;
		}

		got = fread(pixel, 1, 3, file);
		if (got != 3) {
			fclose(file);
			snprintf(reason, capacity, "frame short");
			return 1;
		}

		/* Compares its channels within the point's slack. */
		for (channel = 0; channel < 3; channel++) {
			difference = (int)pixel[channel] - (int)want[index][channel];
			if (difference > slack[index] || difference < -slack[index]) {
				fclose(file);
				snprintf(reason, capacity, "pixel (%d,%d) %02x%02x%02x, want %02x%02x%02x", points[index][0], points[index][1], pixel[0], pixel[1], pixel[2], want[index][0], want[index][1], want[index][2]);
				return 1;
			}
		}
	}

	/* Succeeded: the frame is as expected. */
	fclose(file);
	return 0;
}

/* Writes into reason what a file that is not the expected PPM held: its size and its first bytes. */
static void
frame_describe(
	FILE *file,
	char *reason,
	size_t capacity)
{
	unsigned char head[12];
	size_t got;
	long size;

	/* The file's size. */
	(void)fseek(file, 0, SEEK_END);
	size = ftell(file);

	/* Its first bytes, zero past what could be read. */
	memset(head, 0, sizeof(head));
	rewind(file);
	got = fread(head, 1, sizeof(head), file);

	/* The reason line. */
	snprintf(reason, capacity, "frame not a 320x240 PPM (%ld bytes, starts %02x %02x %02x %02x %02x %02x %02x %02x, %zu read)", size,
		 head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7], got);
}

/* Runs a program with one argument when argument is not NULL; see run_program_list(). */
static int
run_program(
	const char *path,
	const char *argument,
	char *line,
	size_t capacity)
{
	char *arguments[3];
	int status;

	/* The program's name, its argument, the end. */
	arguments[0] = (char *)path;
	arguments[1] = (char *)argument;
	arguments[2] = NULL;
	status = run_program_list(path, arguments, line, capacity);

	/* Succeeded: its exit status. */
	return status;
}

/*
 * Runs a program with a list of arguments (its name first, NULL last),
 * with its first output line in line; reports its exit status (128 when
 * it did not exit, 127 when it could not run).
 */
static int
run_program_list(
	const char *path,
	char *const *arguments,
	char *line,
	size_t capacity)
{
	char drain[256];
	size_t length;
	pid_t child;
	int pipes[2];
	int status;
	ssize_t got;

	/* Its standard output comes back through a pipe. */
	line[0] = '\0';
	if (pipe(pipes) != 0) {
		snprintf(line, capacity, "pipe");
		return 1;
	}
	child = fork();
	if (child < 0) {
		snprintf(line, capacity, "fork");
		return 1;
	}
	if (child == 0) {
		close(pipes[0]);
		dup2(pipes[1], STDOUT_FILENO);
		execv(path, arguments);
		printf("%s: not run (exec %d)\n", path, errno);
		fflush(stdout);
		_exit(127);
	}

	/* Reads what it printed, keeping the start and draining the rest so it never writes to a closed pipe, then its status. */
	close(pipes[1]);
	length = 0;
	for (;;) {
		if (length < capacity - 1U) {
			got = read(pipes[0], line + length, capacity - 1U - length);
			if (got > 0)
				length += (size_t)got;
		} else {
			got = read(pipes[0], drain, sizeof(drain));
		}
		if (got <= 0)
			break;
	}
	line[length] = '\0';
	close(pipes[0]);
	waitpid(child, &status, 0);
	line[strcspn(line, "\n")] = '\0';

	/* Succeeded: its exit status. */
	if (!WIFEXITED(status))
		return 128;
	return WEXITSTATUS(status);
}

/* Says whether name is one of the stages the vkcompute run decides. */
static int
is_vkcompute_stage(
	const char *name)
{
	int i;

	/* Compares with each of the three. */
	for (i = 0; i < 3; i++) {
		if (strcmp(name, vkcompute_stages[i]) == 0)
			return 1;
	}

	/* Not one of them. */
	return 0;
}

/*
 * Splits one vkcompute run into the instance, device and compute stages:
 * exit 0 is all ok; "no Vulkan device (vkCreateInstance" fails the
 * instance; "no Vulkan device" otherwise or "FAIL, vkCreateDevice" fails
 * the device; any other failure fails compute.
 */
static void
vkcompute_outcomes(
	int status,
	const char *line,
	enum outcome *results,
	char reasons[3][SELFTEST_TEXT_MAX])
{
	int failed;
	int i;

	/* Which of the three failed first; 3 when none did. */
	failed = 3;
	if (status != 0) {
		failed = 2;
		if (strstr(line, "no Vulkan device (vkCreateInstance") != NULL || strstr(line, "not run") != NULL)
			failed = 0;
		else if (strstr(line, "no Vulkan device") != NULL || strstr(line, "FAIL, vkCreateDevice") != NULL)
			failed = 1;
	}

	/* Those before it are ok, it fails with the line, those after are skipped. */
	for (i = 0; i < 3; i++) {
		if (i < failed) {
			results[i] = OUTCOME_OK;
		} else if (i == failed) {
			results[i] = OUTCOME_FAIL;
			snprintf(reasons[i], SELFTEST_TEXT_MAX, "%s", line[0] != '\0' ? line : "no output");
		} else {
			results[i] = OUTCOME_SKIP;
			snprintf(reasons[i], SELFTEST_TEXT_MAX, "%s", vkcompute_stages[failed]);
		}
	}
}

int
main(
	int argc,
	char **argv)
{
	static const struct stage vk_stage = { "vkcompute", NULL, stage_vkcompute, NULL, 1 };
	char reasons[3][SELFTEST_TEXT_MAX];
	char reason[SELFTEST_TEXT_MAX];
	enum outcome outcomes[sizeof(stages) / sizeof(stages[0])];
	enum outcome vk_results[3];
	struct selftest test;
	const char *blocker;
	size_t count;
	size_t i;
	size_t j;
	int vk_done;
	int option;
	int status;

	/* The board's defaults, then the options. */
	memset(&test, 0, sizeof(test));
	test.vkcompute = "/bin/vkcompute";
	test.vkimage = "/bin/vkimage";
	test.vkstage = "/bin/vkstage";
	test.vkdemo = "/bin/vkdemo";
	test.frame = "/tmp/gpu-selftest-vkdemo.ppm";
	test.limit = 10;
	while ((option = getopt(argc, argv, "cs:n:v:i:g:d:f:t:")) != -1) {
		if (option == 'c') {
			test.console = 1;
			if (stdout_is_console())
				test.console = 0;
		} else if (option == 's') {
			test.stages_file = optarg;
		} else if (option == 'n' && test.node_count < SELFTEST_NODES_MAX) {
			test.nodes[test.node_count++] = optarg;
		} else if (option == 'v') {
			test.vkcompute = optarg;
		} else if (option == 'i') {
			test.vkimage = optarg;
		} else if (option == 'g') {
			test.vkstage = optarg;
		} else if (option == 'd') {
			test.vkdemo = optarg;
		} else if (option == 'f') {
			test.frame = optarg;
		} else if (option == 't') {
			test.limit = atoi(optarg);
		} else {
			fprintf(stderr, "usage: gpu-selftest [-c] [-s file] [-n node]... [-v vkcompute] [-i vkimage] [-g vkstage] [-d vkdemo] [-f frame] [-t seconds]\n");
			return 2;
		}
	}
	if (test.node_count == 0) {
		test.nodes[0] = "/dev/gpu0";
		test.nodes[1] = "/dev/gpu1";
		test.node_count = 2;
	}
	if (test.limit <= 0)
		test.limit = 10;

	/* Each stage in order. */
	count = sizeof(stages) / sizeof(stages[0]);
	vk_done = 0;
	for (i = 0; i < count; i++) {
		/* A stage not built yet says so; one whose need did not pass is skipped with that need's name. */
		blocker = NULL;
		if (stages[i].run == NULL && !is_vkcompute_stage(stages[i].name))
			blocker = "not built";
		for (j = 0; j < i && stages[i].needs != NULL && blocker == NULL; j++) {
			if (strcmp(stages[j].name, stages[i].needs) == 0 && outcomes[j] != OUTCOME_OK)
				blocker = stages[j].name;
		}

		/* The three vkcompute stages share one run, made when the first of them comes. */
		for (j = 0; j < 3 && blocker == NULL; j++) {
			if (strcmp(stages[i].name, vkcompute_stages[j]) != 0)
				continue;
			if (!vk_done) {
				run_child(&test, &vk_stage, reason, sizeof(reason));
				status = 128;
				if (strcmp(reason, "timeout") == 0 || strcmp(reason, "crash") == 0) {
					vk_results[0] = OUTCOME_FAIL;
					snprintf(reasons[0], SELFTEST_TEXT_MAX, "%s", reason);
					vk_results[1] = vk_results[2] = OUTCOME_SKIP;
					snprintf(reasons[1], SELFTEST_TEXT_MAX, "instance");
					snprintf(reasons[2], SELFTEST_TEXT_MAX, "instance");
				} else {
					status = atoi(reason);
					vkcompute_outcomes(status, strchr(reason, ' ') != NULL ? strchr(reason, ' ') + 1 : "", vk_results, reasons);
				}
				vk_done = 1;
			}
			outcomes[i] = vk_results[j];
			if (outcomes[i] == OUTCOME_OK)
				report(&test, "gpu-selftest: %s: ok", stages[i].name);
			else if (outcomes[i] == OUTCOME_FAIL)
				report(&test, "gpu-selftest: %s: fail(%s)", stages[i].name, reasons[j]);
			else
				report(&test, "gpu-selftest: %s: skip(%s)", stages[i].name, reasons[j]);
			test.counts[outcomes[i]]++;
			break;
		}
		if (j < 3 && blocker == NULL)
			continue;

		/* Skipped, not built, or run in its child. */
		if (blocker != NULL) {
			outcomes[i] = OUTCOME_SKIP;
			report(&test, "gpu-selftest: %s: skip(%s)", stages[i].name, blocker);
		} else if (run_child(&test, &stages[i], reason, sizeof(reason)) == 0) {
			outcomes[i] = OUTCOME_OK;
			report(&test, "gpu-selftest: %s: ok", stages[i].name);
		} else {
			outcomes[i] = OUTCOME_FAIL;
			report(&test, "gpu-selftest: %s: fail(%s)", stages[i].name, reason);
		}
		test.counts[outcomes[i]]++;
	}

	/* The total, and the status. */
	report(&test, "gpu-selftest: %d ok, %d fail, %d skip", test.counts[OUTCOME_OK], test.counts[OUTCOME_FAIL], test.counts[OUTCOME_SKIP]);
	return test.counts[OUTCOME_FAIL] == 0 ? 0 : 1;
}
