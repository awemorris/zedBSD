/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The guest side of the display-event fence test (GPU_DISPLAY_FENCE): one
 * step per run, each printing one result line that qemu-display-fence.sh reads.
 *
 *   display-fence-test idle SECONDS                 a registration stays unsignaled
 *   display-fence-test wait SECONDS MARKER [GATE]   a registration wakes poll(); prints the milliseconds after MARKER
 *   display-fence-test cancel SECONDS MARKER [GATE] a canceled registration never wakes poll()
 *   display-fence-test limit                        16 per open, ENOSPC past them, a closed open's fences end
 *   display-fence-test race COUNT GAP_MS [GATE]     registrations made while the head is plugged and unplugged
 *
 * With GATE, the program opens the node, makes GATE.ready and waits for the
 * host to make GATE.go before it registers.  Opening a GPU node puts a kernel
 * record on the console the host drives the guest through, and a record that
 * lands in the middle of the host's command costs that command time; the gate
 * keeps the record before the registration, so the host's time from the
 * marker to the change stays short.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <uapi/gpu.h>
#include <uapi/gpu-display.h>

/* The GPU nodes looked at for one that reports display events. */
#define TEST_NODE_MAX	8

/* The registrations the race step makes at most. */
#define TEST_RACE_MAX	4096

/* How long a gated step waits for the host's go, in milliseconds, and how often it looks. */
#define TEST_GATE_MS		120000
#define TEST_GATE_POLL_US	5000

/*
 * One registration of the race step: the topology sequence read before and
 * after it, and its fence's descriptor.
 */
struct race_entry {
	uint64_t before;
	uint64_t after;
	int fd;
};

static int open_display_node(char *path, size_t bytes);
static int register_hotplug(int node, int *fence);
static int cancel_hotplug(int node, int fence);
static int read_sequence(int node, uint64_t *sequence);
static uint64_t now_ms(void);
static int wait_fence(int fence, int timeout_ms, short *revents);
static int gate_pass(const char *gate);
static int step_idle(int seconds);
static int step_wait(int seconds, const char *marker, const char *gate, int expect_wake);
static int step_limit(void);
static int race_check(const struct race_entry *entry, uint64_t now);
static int step_race(int count, int gap_ms, const char *gate);

/* The race step's registrations, too many for the stack. */
static struct race_entry race_entries[TEST_RACE_MAX];

/* Runs the step the arguments name. */
int
main(
	int argc,
	char **argv)
{
	int status;

	/* Each result line reaches the result file at once, whatever ends the run. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* The step and its arguments. */
	if (argc < 2) {
		fprintf(stderr, "usage: display-fence-test idle|wait|cancel|limit|race ...\n");
		return 2;
	}

	/* One step per run. */
	status = 2;
	if (strcmp(argv[1], "idle") == 0 && argc == 3) {
		status = step_idle(atoi(argv[2]));
	} else if (strcmp(argv[1], "wait") == 0 && (argc == 4 || argc == 5)) {
		status = step_wait(atoi(argv[2]), argv[3], argc == 5 ? argv[4] : NULL, 1);
	} else if (strcmp(argv[1], "cancel") == 0 && (argc == 4 || argc == 5)) {
		status = step_wait(atoi(argv[2]), argv[3], argc == 5 ? argv[4] : NULL, 0);
	} else if (strcmp(argv[1], "limit") == 0 && argc == 2) {
		status = step_limit();
	} else if (strcmp(argv[1], "race") == 0 && (argc == 4 || argc == 5)) {
		status = step_race(atoi(argv[2]), atoi(argv[3]), argc == 5 ? argv[4] : NULL);
	} else {
		fprintf(stderr, "display-fence-test: unknown step or arguments\n");
	}

	/* The step's result decides the exit status, after its line is written out. */
	fflush(stdout);
	return status;
}

/* Opens the first GPU node that reports display events, read-only; -1 when none does. */
static int
open_display_node(
	char *path,
	size_t bytes)
{
	struct gpu_info info;
	int index;
	int node;
	int error;

	/* Looks at /dev/gpu0 and on. */
	for (index = 0; index < TEST_NODE_MAX; index++) {
		snprintf(path, bytes, "/dev/gpu%d", index);
		node = open(path, O_RDONLY | O_CLOEXEC);
		if (node < 0)
			continue;

		/* The node's capabilities. */
		memset(&info, 0, sizeof(info));
		info.version = GPU_ABI_VERSION;
		info.size = sizeof(info);
		error = ioctl(node, GPU_GET_INFO, &info);
		if (error == 0 && (info.capabilities & GPU_CAP_DISPLAY_EVENTS) != 0U)
			return node;

		close(node);
	}

	/* No node reports display events. */
	return -1;
}

/* Registers one hotplug fence on the node: 0, or the errno. */
static int
register_hotplug(
	int node,
	int *fence)
{
	struct gpu_display_fence request;
	int error;

	/* Every input but version, size, flags and event is zero, and fd is -1. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.flags = GPU_HANDLE_CLOEXEC;
	request.event = GPU_DISPLAY_FENCE_HOTPLUG;
	request.fd = -1;
	error = ioctl(node, GPU_DISPLAY_FENCE, &request);
	if (error != 0)
		return errno;

	/* Succeeded: the fence's descriptor. */
	*fence = request.fd;
	return 0;
}

/* Withdraws one registration: 0, or the errno. */
static int
cancel_hotplug(
	int node,
	int fence)
{
	struct gpu_display_fence_cancel request;
	int error;

	/* The request names the fence. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.fd = fence;
	error = ioctl(node, GPU_DISPLAY_FENCE_CANCEL, &request);
	if (error != 0)
		return errno;

	/* Succeeded: the kernel forgot the registration. */
	return 0;
}

/* Reads the node's topology sequence (a GPU_DISPLAY_EVENTS query): 0, or the errno. */
static int
read_sequence(
	int node,
	uint64_t *sequence)
{
	struct gpu_display_events request;
	int error;

	/* A query has only version and size. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	error = ioctl(node, GPU_DISPLAY_EVENTS, &request);
	if (error != 0)
		return errno;

	/* Succeeded: the sequence. */
	*sequence = request.sequence;
	return 0;
}

/* The monotonic clock in milliseconds. */
static uint64_t
now_ms(void)
{
	struct timespec now;

	/* The clock. */
	clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: the milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Waits for the fence's descriptor with one poll(): 1 when it woke, 0 on timeout, -1 on failure. */
static int
wait_fence(
	int fence,
	int timeout_ms,
	short *revents)
{
	struct pollfd entry;
	int ready;

	/* One descriptor, readable when the fence is signaled. */
	entry.fd = fence;
	entry.events = POLLIN;
	entry.revents = 0;
	ready = poll(&entry, 1, timeout_ms);
	*revents = entry.revents;
	if (ready < 0)
		return -1;

	/* Succeeded: whether the descriptor was ready. */
	return ready > 0;
}

/*
 * Makes GATE.ready and waits for GATE.go: 0 when the host said go (or there
 * is no gate), or the errno.
 */
static int
gate_pass(
	const char *gate)
{
	char path[256];
	uint64_t started;
	int file;
	int found;

	/* An ungated step goes on at once. */
	if (gate == NULL)
		return 0;

	/* Tells the host the node is open and its kernel record written. */
	snprintf(path, sizeof(path), "%s.ready", gate);
	file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (file < 0)
		return errno;
	close(file);

	/* Looks for the host's go until it comes or the wait runs out. */
	snprintf(path, sizeof(path), "%s.go", gate);
	started = now_ms();
	for (;;) {
		found = access(path, F_OK);
		if (found == 0)
			break;
		if (now_ms() - started >= TEST_GATE_MS)
			return ETIMEDOUT;
		usleep(TEST_GATE_POLL_US);
	}

	/* Succeeded: the host said go. */
	return 0;
}

/* A registration with no topology change stays unsignaled for SECONDS. */
static int
step_idle(
	int seconds)
{
	char path[32];
	uint64_t before;
	uint64_t after;
	short revents;
	int node;
	int fence;
	int woke;
	int error;

	/* The node and one registration. */
	node = open_display_node(path, sizeof(path));
	if (node < 0) {
		printf("idle: FAIL no display node\n");
		return 1;
	}
	error = read_sequence(node, &before);
	if (error == 0)
		error = register_hotplug(node, &fence);
	if (error != 0) {
		printf("idle: FAIL register %d\n", error);
		return 1;
	}

	/* One poll for the whole time. */
	woke = wait_fence(fence, seconds * 1000, &revents);
	error = read_sequence(node, &after);
	if (woke != 0) {
		printf("idle: FAIL woke=%d revents=0x%x sequence %llu -> %llu\n", woke, revents,
		    (unsigned long long)before, (unsigned long long)after);
		return 1;
	}

	/* Succeeded: no wake while nothing changed. */
	printf("idle: PASS %s %d s sequence %llu -> %llu\n", path, seconds,
	    (unsigned long long)before, (unsigned long long)after);
	return 0;
}

/*
 * A registration wakes poll() at the next change (expect_wake), or never
 * once canceled.  MARKER is made after the registration, for the host to
 * start the change.
 */
static int
step_wait(
	int seconds,
	const char *marker,
	const char *gate,
	int expect_wake)
{
	const char *name;
	char path[32];
	uint64_t armed;
	uint64_t before;
	uint64_t after;
	short revents;
	int node;
	int fence;
	int woke;
	int file;
	int error;

	/* The node and one registration, canceled for the cancel step. */
	name = expect_wake ? "wait" : "cancel";
	node = open_display_node(path, sizeof(path));
	if (node < 0) {
		printf("%s: FAIL no display node\n", name);
		return 1;
	}

	/* The host's go, after the open's kernel record. */
	error = gate_pass(gate);
	if (error != 0) {
		printf("%s: FAIL gate %d\n", name, error);
		return 1;
	}
	error = read_sequence(node, &before);
	if (error == 0)
		error = register_hotplug(node, &fence);
	if (error == 0 && !expect_wake)
		error = cancel_hotplug(node, fence);
	if (error != 0) {
		printf("%s: FAIL register or cancel %d\n", name, error);
		return 1;
	}

	/* Tells the host the registration is in place. */
	file = open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (file < 0) {
		printf("%s: FAIL marker %d\n", name, errno);
		return 1;
	}
	close(file);
	armed = now_ms();

	/* One poll for the whole time. */
	woke = wait_fence(fence, seconds * 1000, &revents);
	error = read_sequence(node, &after);
	if (woke < 0 || woke != expect_wake) {
		printf("%s: FAIL woke=%d revents=0x%x after %llu ms sequence %llu -> %llu\n", name, woke, revents,
		    (unsigned long long)(now_ms() - armed), (unsigned long long)before, (unsigned long long)after);
		return 1;
	}

	/* An error status is a failure of the change's signal. */
	if (expect_wake && (revents & POLLERR) != 0) {
		printf("%s: FAIL error signal revents=0x%x\n", name, revents);
		return 1;
	}

	/* Succeeded: woke as expected; the time from the marker. */
	printf("%s: PASS woke=%d ms=%llu sequence %llu -> %llu\n", name, woke,
	    (unsigned long long)(now_ms() - armed), (unsigned long long)before, (unsigned long long)after);
	return 0;
}

/* 16 registrations per open, ENOSPC past them; a closed open's fences end and its slots are free again. */
static int
step_limit(void)
{
	int fences[GPU_DISPLAY_FENCE_MAX];
	char path[32];
	short revents;
	unsigned index;
	int extra;
	int node;
	int woke;
	int error;

	/* The node. */
	node = open_display_node(path, sizeof(path));
	if (node < 0) {
		printf("limit: FAIL no display node\n");
		return 1;
	}

	/* The most an open may hold. */
	for (index = 0U; index < GPU_DISPLAY_FENCE_MAX; index++) {
		error = register_hotplug(node, &fences[index]);
		if (error != 0) {
			printf("limit: FAIL registration %u: %d\n", index, error);
			return 1;
		}
	}

	/* One more is refused. */
	error = register_hotplug(node, &extra);
	if (error != ENOSPC) {
		printf("limit: FAIL registration 17 gave %d, not ENOSPC\n", error);
		return 1;
	}

	/* A canceled one makes room. */
	error = cancel_hotplug(node, fences[0]);
	if (error == 0)
		error = register_hotplug(node, &extra);
	if (error != 0) {
		printf("limit: FAIL cancel then register %d\n", error);
		return 1;
	}
	close(fences[0]);
	fences[0] = extra;

	/* The final close of the open ends every registration with an error. */
	close(node);
	for (index = 0U; index < GPU_DISPLAY_FENCE_MAX; index++) {
		woke = wait_fence(fences[index], 0, &revents);
		if (woke != 1 || (revents & POLLERR) == 0) {
			printf("limit: FAIL fence %u after close woke=%d revents=0x%x\n", index, woke, revents);
			return 1;
		}
		close(fences[index]);
	}

	/* A new open holds 16 again. */
	node = open_display_node(path, sizeof(path));
	if (node < 0) {
		printf("limit: FAIL reopen\n");
		return 1;
	}
	for (index = 0U; index < GPU_DISPLAY_FENCE_MAX; index++) {
		error = register_hotplug(node, &fences[index]);
		if (error != 0) {
			printf("limit: FAIL registration %u after reopen: %d\n", index, error);
			return 1;
		}
	}

	/* Succeeded: the limit, the close and the reopen. */
	printf("limit: PASS 16 per open, ENOSPC past them, ENODEV at close, 16 after reopen\n");
	return 0;
}

/* Checks one registration against the sequence now: 0 when right, 1 when missed, 2 when signaled without a change. */
static int
race_check(
	const struct race_entry *entry,
	uint64_t now)
{
	short revents;
	int woke;

	/* A change past what was read after the registration signals it (the push may still be in flight). */
	if (now > entry->after) {
		woke = wait_fence(entry->fd, 1000, &revents);
		if (woke != 1)
			return 1;
		return 0;
	}

	/* No change since before the registration leaves it unsignaled. */
	woke = wait_fence(entry->fd, 0, &revents);
	if (now == entry->before && woke != 0)
		return 2;

	/* Succeeded: either signaled or not, as the sequence allows. */
	return 0;
}

/*
 * Registrations made back to back while the host plugs and unplugs the
 * head: once the sequence has moved past what was read after a
 * registration, its fence must be signaled; a fence must stay unsignaled
 * while the sequence has not moved since before it.  An open holds 16, so
 * each registration is checked and canceled 16 registrations later.
 */
static int
step_race(
	int count,
	int gap_ms,
	const char *gate)
{
	struct race_entry *entry;
	struct race_entry *old;
	char path[32];
	uint64_t now;
	unsigned missed;
	unsigned spurious;
	unsigned passed;
	int verdict;
	int index;
	int node;
	int error;

	/* The node. */
	if (count < 1 || count > TEST_RACE_MAX)
		count = TEST_RACE_MAX;
	node = open_display_node(path, sizeof(path));
	if (node < 0) {
		printf("race: FAIL no display node\n");
		return 1;
	}

	/* The host's go, after the open's kernel record. */
	error = gate_pass(gate);
	if (error != 0) {
		printf("race: FAIL gate %d\n", error);
		return 1;
	}

	/* Registers; the one 16 back is checked and canceled first. */
	missed = 0U;
	spurious = 0U;
	passed = 0U;
	for (index = 0; index < count + (int)GPU_DISPLAY_FENCE_MAX; index++) {
		if (index >= (int)GPU_DISPLAY_FENCE_MAX) {
			old = &race_entries[index - (int)GPU_DISPLAY_FENCE_MAX];
			if (index >= count)
				sleep(index == count ? 2 : 0);
			error = read_sequence(node, &now);
			if (error != 0) {
				printf("race: FAIL sequence %d\n", error);
				return 1;
			}
			verdict = race_check(old, now);
			if (verdict == 1)
				missed++;
			if (verdict == 2)
				spurious++;
			if (now > old->after)
				passed++;
			(void)cancel_hotplug(node, old->fd);
			close(old->fd);
		}
		if (index >= count)
			continue;

		/* One registration between two reads of the sequence. */
		entry = &race_entries[index];
		error = read_sequence(node, &entry->before);
		if (error == 0)
			error = register_hotplug(node, &entry->fd);
		if (error == 0)
			error = read_sequence(node, &entry->after);
		if (error != 0) {
			printf("race: FAIL registration %d: %d\n", index, error);
			return 1;
		}
		if (gap_ms > 0)
			usleep((useconds_t)gap_ms * 1000U);
	}

	/* Succeeded only with no fence missed and none signaled without a change. */
	if (missed != 0U || spurious != 0U) {
		printf("race: FAIL missed=%u spurious=%u passed=%u\n", missed, spurious, passed);
		return 1;
	}
	printf("race: PASS registrations=%d passed=%u sequence %llu -> %llu\n", count, passed,
	    (unsigned long long)race_entries[0].before, (unsigned long long)race_entries[count - 1].after);
	return 0;
}
