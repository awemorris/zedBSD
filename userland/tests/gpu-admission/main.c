/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises concurrent kernel GPU admission through one shared open session.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <uapi/gpu.h>
#include <uapi/gpu-display.h>

#define TEST_THREADS 4U
#define TEST_ROUNDS 32U
#define TEST_BYTES 4096U
#define TEST_EVENT_ATTEMPTS 8U
#define TEST_DISPLAY_LIMIT 64U

/* One start gate and fd remain alive until every worker has joined. */
struct test_session {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	int fd;
	int started;
};

/* One worker alone updates its result until main joins that worker. */
struct test_worker {
	struct test_session *session;
	pthread_t thread;
	unsigned identity;
	unsigned completed;
	int error;
	const char *stage;
};

static void *test_run(void *argument);
static int test_round(struct test_worker *worker, unsigned round);
static int test_display_events(int fd);
static int test_display_ready(int fd, int *ready);
static int test_display_inventory(int fd, unsigned *count);

/*
 * Runs a finite concurrent storage lifecycle against the first GPU device.
 */
int
main(void)
{
	struct test_session session;
	struct test_worker workers[TEST_THREADS];
	unsigned created;
	unsigned index;
	unsigned completed;
	int status;
	int failed;

	/* Announces execution before any device or thread setup can fail. */
	printf("GPUADMISSION START threads=%u rounds=%u bytes=%u\n",
	    TEST_THREADS,
	    TEST_ROUNDS,
	    TEST_BYTES);
	fflush(stdout);

	/* Keeps the single session open through all independent resource tests. */
	memset(&session, 0, sizeof(session));
	session.fd = open("/dev/gpu0", O_RDWR | O_CLOEXEC);
	if (session.fd < 0) {
		printf("GPUADMISSION FAILED stage=open errno=%d\n", errno);
		return 1;
	}

	/* Observes the real initial display inventory before concurrent storage traffic begins. */
	status = test_display_events(session.fd);
	if (status != 0) {
		printf("GPUADMISSION FAILED stage=display-events error=%d\n", status);
		close(session.fd);
		return 1;
	}

	/* Initializes each start-gate primitive before any worker can observe it. */
	status = pthread_mutex_init(&session.mutex, NULL);
	if (status != 0) {
		printf("GPUADMISSION FAILED stage=mutex error=%d\n", status);
		close(session.fd);
		return 1;
	}

	status = pthread_cond_init(&session.condition, NULL);
	if (status != 0) {
		printf("GPUADMISSION FAILED stage=condition error=%d\n", status);
		pthread_mutex_destroy(&session.mutex);
		close(session.fd);
		return 1;
	}

	/* Starts as many workers as possible without trapping a partial barrier. */
	memset(workers, 0, sizeof(workers));
	created = 0;
	failed = 0;
	for (index = 0; index < TEST_THREADS; index++) {
		workers[index].session = &session;
		workers[index].identity = index;
		status = pthread_create(
		    &workers[index].thread,
		    NULL,
		    test_run,
		    &workers[index]);
		if (status != 0) {
			printf("GPUADMISSION FAILED stage=create error=%d\n", status);
			failed = 1;
			break;
		}

		created++;
	}

	/* Releases all created workers; the mutex never covers a device ioctl. */
	pthread_mutex_lock(&session.mutex);

	session.started = 1;
	pthread_cond_broadcast(&session.condition);

	pthread_mutex_unlock(&session.mutex);

	/* Joins before inspecting worker-owned counters or closing their fd. */
	completed = 0;
	for (index = 0; index < created; index++) {
		status = pthread_join(workers[index].thread, NULL);
		if (status != 0) {
			printf("GPUADMISSION FAILED stage=join error=%d\n", status);
			_exit(1);
		}

		completed += workers[index].completed;
		if (workers[index].error != 0) {
			printf("GPUADMISSION FAILED thread=%u round=%u stage=%s errno=%d\n",
			    index,
			    workers[index].completed,
			    workers[index].stage,
			    workers[index].error);
			failed = 1;
		}
	}

	/* Retires the shared gate only after no worker can reach it. */
	pthread_cond_destroy(&session.condition);
	pthread_mutex_destroy(&session.mutex);
	status = close(session.fd);
	if (status != 0) {
		printf("GPUADMISSION FAILED stage=close errno=%d\n", errno);
		failed = 1;
	}

	/* Refuses partial completion even if no worker reported a device error. */
	if (completed != TEST_THREADS * TEST_ROUNDS)
		failed = 1;
	if (failed != 0)
		return 1;

	/* Succeeded: every admitted storage lifecycle preserved its own bytes. */
	printf("GPUADMISSION PASS threads=%u completed=%u bytes=%u\n",
	    TEST_THREADS,
	    completed,
	    TEST_BYTES);
	return 0;
}

static void *
test_run(
	void *argument)
{
	struct test_worker *worker;
	struct test_session *session;
	unsigned round;
	int status;

	/* Resolves worker-owned state before joining the common start gate. */
	worker = argument;
	session = worker->session;

	/* Starts concurrent ioctl traffic only after main releases the gate. */
	pthread_mutex_lock(&session->mutex);

	/* Sleeps without holding back main's release of the start gate. */
	while (session->started == 0)
		pthread_cond_wait(&session->condition, &session->mutex);

	pthread_mutex_unlock(&session->mutex);

	/* Uses independent resource handles while contending on the same GPU fd. */
	for (round = 0; round < TEST_ROUNDS; round++) {
		status = test_round(worker, round);
		if (status != 0) {
			worker->error = status;
			return NULL;
		}

		/* Main treats this count as proof only after joining this thread. */
		worker->completed++;
	}

	/* Succeeded: main can collect this worker's complete finite result. */
	return NULL;
}

static int
test_round(
	struct test_worker *worker,
	unsigned round)
{
	struct gpu_resource_create create;
	struct gpu_resource_destroy destroy;
	struct gpu_transfer transfer;
	struct gpu_info info;
	unsigned char source[TEST_BYTES];
	unsigned char observed[TEST_BYTES];
	unsigned index;
	int status;
	int error;
	int fd;

	/* Makes every offset identify its owning thread and lifecycle iteration. */
	fd = worker->session->fd;
	for (index = 0; index < TEST_BYTES; index++) {
		source[index] = (unsigned char)(index * 13U + round * 37U +
		    worker->identity * 71U);
	}

	/* Allocates coherent storage without constructing a Vulkan queue. */
	memset(&create, 0, sizeof(create));
	create.version = GPU_ABI_VERSION;
	create.size = sizeof(create);
	create.bytes = TEST_BYTES;
	create.usage = GPU_RESOURCE_USAGE_STORAGE;
	worker->stage = "resource-create";
	status = ioctl(fd, GPU_RESOURCE_CREATE, &create);
	if (status != 0)
		return errno;

	/* Retains the exact handle for both transfers and every cleanup path. */
	memset(&destroy, 0, sizeof(destroy));
	destroy.version = GPU_ABI_VERSION;
	destroy.size = sizeof(destroy);
	destroy.handle = create.handle;
	error = 0;

	/* Writes one complete worker-owned pattern through admitted copy I/O. */
	memset(&transfer, 0, sizeof(transfer));
	transfer.version = GPU_ABI_VERSION;
	transfer.size = sizeof(transfer);
	transfer.handle = create.handle;
	transfer.address = (uintptr_t)source;
	transfer.bytes = TEST_BYTES;
	worker->stage = "resource-write";
	status = ioctl(fd, GPU_RESOURCE_WRITE, &transfer);
	if (status != 0) {
		error = errno;
		goto cleanup;
	}

	/* Offers other threads admission between this resource's two transfers. */
	sched_yield();
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	worker->stage = "get-info";
	status = ioctl(fd, GPU_GET_INFO, &info);
	if (status != 0) {
		error = errno;
		goto cleanup;
	}

	/* Reads back every byte while peers create and retire unrelated resources. */
	memset(observed, 0, sizeof(observed));
	transfer.address = (uintptr_t)observed;
	worker->stage = "resource-read";
	status = ioctl(fd, GPU_RESOURCE_READ, &transfer);
	if (status != 0) {
		error = errno;
		goto cleanup;
	}

	/* Detects handle confusion, lost writes and partial copy completion. */
	worker->stage = "byte-compare";
	status = memcmp(source, observed, TEST_BYTES);
	if (status != 0)
		error = EIO;

cleanup:
	/* Retires every successfully allocated handle even after a failed copy. */
	status = ioctl(fd, GPU_RESOURCE_DESTROY, &destroy);
	if (status != 0 && error == 0) {
		worker->stage = "resource-destroy";
		error = errno;
	}

	/* Reports the first failed operation, including unexpected EBUSY admission. */
	if (error != 0)
		return error;

	/* Succeeded: no resource from this iteration remains in the session. */
	return 0;
}

/* Checks the initial inventory notification without requiring a physical hotplug. */
static int
test_display_events(
	int fd)
{
	struct gpu_info info;
	struct gpu_display_events events;
	uint64_t sequence;
	unsigned attempt;
	unsigned count;
	int ready;
	int status;

	/* Only an advertised event contract can require initial priority readiness. */
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	status = ioctl(fd, GPU_GET_INFO, &info);
	if (status != 0)
		return errno;

	/* A driver without optional topology events still runs the storage admission test. */
	if ((info.capabilities & GPU_CAP_DISPLAY_EVENTS) == 0U) {
		printf("GPUADMISSION DISPLAY SKIP reason=capability-absent\n");
		return 0;
	}

	/* A fresh open must advertise its inventory until this observer acknowledges it. */
	status = test_display_ready(fd, &ready);
	if (status != 0)
		return status;

	/* Initial readiness is required even if the host has not changed any connectors. */
	if (ready == 0)
		return EIO;

	/* Retry only when real events arrive while the previous snapshot is being enumerated. */
	for (attempt = 0U; attempt < TEST_EVENT_ATTEMPTS; attempt++) {
		/* QUERY returns a generation but does not acknowledge this open's observation. */
		memset(&events, 0, sizeof(events));
		events.version = GPU_ABI_VERSION;
		events.size = sizeof(events);
		status = ioctl(fd, GPU_DISPLAY_EVENTS, &events);
		if (status != 0)
			return errno;

		/* Every pending inventory snapshot must identify an unacknowledged generation. */
		if (events.sequence == 0U || events.events != GPU_DISPLAY_EVENT_CHANGE)
			return EIO;

		/* Preserve the first query's exact generation across a second non-destructive observation. */
		sequence = events.sequence;
		memset(&events, 0, sizeof(events));
		events.version = GPU_ABI_VERSION;
		events.size = sizeof(events);
		status = ioctl(fd, GPU_DISPLAY_EVENTS, &events);
		if (status != 0)
			return errno;

		/* A second query cannot consume readiness or move the device sequence backwards. */
		if (events.sequence < sequence || events.events != GPU_DISPLAY_EVENT_CHANGE)
			return EIO;

		/* POLLPRI is level-triggered and remains ready after successful QUERY copyout. */
		status = test_display_ready(fd, &ready);
		if (status != 0)
			return status;

		/* No acknowledgement has yet permitted this open to stop reporting its inventory. */
		if (ready == 0)
			return EIO;

		/* Enumerate real output snapshots before acknowledging the generation that prompted them. */
		status = test_display_inventory(fd, &count);
		if (status != 0)
			return status;

		/* Acknowledge only the generation observed before inventory enumeration began. */
		memset(&events, 0, sizeof(events));
		events.version = GPU_ABI_VERSION;
		events.size = sizeof(events);
		events.flags = GPU_DISPLAY_EVENT_ACK;
		events.ack_sequence = sequence;
		status = ioctl(fd, GPU_DISPLAY_EVENTS, &events);
		if (status != 0)
			return errno;

		/* A concurrent newer generation remains pending after an older exact acknowledgement. */
		if (events.sequence < sequence)
			return EIO;

		/* Acknowledging the current snapshot must clear the returned change flag. */
		if (events.sequence == sequence) {
			if (events.events != 0U)
				return EIO;
		} else if (events.events != GPU_DISPLAY_EVENT_CHANGE) {
			return EIO;
		}

		/* Observe priority readiness again without waiting for a hypothetical physical event. */
		status = test_display_ready(fd, &ready);
		if (status != 0)
			return status;

		/* A stable acknowledgement finishes; any actual newer notification takes another bounded pass. */
		if (ready == 0) {
			/* A pending generation cannot disappear without another successful acknowledgement. */
			if (events.events != 0U)
				return EIO;

			/* A stable inventory has completed the bounded observation loop. */
			break;
		}
	}

	/* Repeated physical changes cannot make this finite acceptance probe run forever. */
	if (attempt == TEST_EVENT_ATTEMPTS)
		return ETIMEDOUT;

	/* Reports the exact inventory observation that cleared this open's initial readiness. */
	printf("GPUADMISSION DISPLAY PASS sequence=%llu outputs=%u attempts=%u initial=1 query_preserved=1 acknowledged=1\n",
	    (unsigned long long)sequence,
	    count,
	    attempt + 1U);

	/* Succeeded: the initial inventory was queried and exactly acknowledged through its real API. */
	return 0;
}

/* Samples priority readiness and rejects terminal GPU or descriptor errors. */
static int
test_display_ready(
	int fd,
	int *ready)
{
	struct pollfd descriptor;
	int status;

	/* Poll only the inventory channel; renderer completion readiness is independent. */
	memset(&descriptor, 0, sizeof(descriptor));
	descriptor.fd = fd;
	descriptor.events = POLLPRI;
	*ready = 0;
	status = poll(&descriptor, 1U, 0);
	if (status < 0)
		return errno;

	/* Invalid handles and lost devices must fail instead of looking acknowledged. */
	if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
		return EIO;

	/* Only one fd was submitted, and only its requested readiness may count as success. */
	if (status > 1 || (descriptor.revents & ~POLLPRI) != 0)
		return EIO;

	/* A readable initial inventory has one matching poll result. */
	if ((descriptor.revents & POLLPRI) != 0) {
		if (status != 1)
			return EIO;

		/* Preserve the explicit priority result for the caller's generation protocol. */
		*ready = 1;
	} else if (status != 0) {
		return EIO;
	}

	/* Succeeded: the caller receives the current priority readiness without blocking. */
	return 0;
}

/* Reads every current output before acknowledging its prompting event generation. */
static int
test_display_inventory(
	int fd,
	unsigned *count)
{
	struct gpu_display_info display;
	unsigned index;
	int status;

	/* Discover a finite output count without assuming any fixed display identity. */
	memset(&display, 0, sizeof(display));
	display.version = GPU_ABI_VERSION;
	display.size = sizeof(display);
	display.index = GPU_DISPLAY_COUNT_ONLY;
	status = ioctl(fd, GPU_DISPLAY_QUERY, &display);
	if (status != 0)
		return errno;

	/* This acceptance probe bounds its work even if a device advertises an unreasonable count. */
	if (display.count > TEST_DISPLAY_LIMIT)
		return EOVERFLOW;

	/* Each ordinal query exercises the actual display discovery path before ACK. */
	*count = display.count;
	for (index = 0U; index < *count; index++) {
		memset(&display, 0, sizeof(display));
		display.version = GPU_ABI_VERSION;
		display.size = sizeof(display);
		display.index = index;
		status = ioctl(fd, GPU_DISPLAY_QUERY, &display);
		if (status != 0)
			return errno;

		/* Output identity and mode generation must be usable for a later display operation. */
		if (display.display_id == 0U || display.generation == 0U)
			return EIO;
	}

	/* Succeeded: the caller queried the actual inventory associated with its pending notification. */
	return 0;
}
