/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system extension's monitor (WS134 p012, plan/ws134/design.md section
 * 1.3; libkeiland/system/kl-system-protocol.h's kl_system_monitor_v1): the machine's
 * counters, sampled by libkeiland-backend's monitor area, for the System
 * Monitor.
 *
 * The samples are taken on the device I/O thread (io.c) while a monitor
 * object exists (reading the memory walks the kernel's pages, and a disk's
 * statistics may wait on a lock: the event loop never does either): the
 * event loop gives it a sample's job at the shortest period asked, one at
 * a time, the info read every few samples, and tells every monitor object
 * the sample and a new info when the job is taken (kwl_sysmon_tick).  The
 * backend's monitor is the device I/O thread's: opened by the first job,
 * closed by a job after the last object went (or at the compositor's end,
 * after the thread).
 *
 * The flow is held by the clients (design.md review 2): an object hears
 * no sample before it acked the last, and none when its client's queue
 * has no room for a whole one.  A sample is sent whole or not at all; one
 * an object missed is not owed, since the counters only grow and the next
 * covers the time.
 */

#include "kwl.h"

#include "userland/desktop/libkeiland/system/kl-system-protocol.h"
#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A parameter a function does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* How often the event loop looks for the monitor objects (their number and their periods), in milliseconds. */
#define SYSMON_SCAN_MS		250U

/* How many samples the thread takes between two reads of the info (a device that came shows within them). */
#define SYSMON_INFO_EVERY	5U

/* The largest event this file sends (an info with its host, a device with its names). */
#define SYSMON_EVENT_MAX	256U

/*
 * The sampling, the event loop's: whether it runs (a monitor object
 * exists), the period it samples at, when the next sample's job is due,
 * whether a sample's job is under way and how many were given (the info is
 * read with every SYSMON_INFO_EVERY-th), whether the backend's monitor may
 * be open on the device I/O thread, the latest sample and info with their
 * serials (0: none yet), the serials told last, when it last looked for
 * the objects, and how many there were.  One per process.
 */
struct sysmon_state {
	unsigned running;
	uint32_t period_ms;
	uint64_t next_ms;
	unsigned sampling;
	unsigned given;
	unsigned opened;
	struct kl_backend_monitor_sample current;
	uint32_t current_serial;
	struct kl_backend_monitor_info current_info;
	uint32_t info_serial;
	uint32_t taken_sample;
	uint32_t taken_info;
	uint64_t scanned_ms;
	unsigned subscribers;
};

/*
 * A job of the device I/O thread: a sample (with the info when info is
 * set, published when it is the first or the devices changed), or the
 * monitor's close (close set).  error and info_error say how the readings
 * went; info_new whether the info is new.
 */
struct sysmon_job {
	struct kwl_lane_job lane;
	unsigned close;
	unsigned info;
	int error;
	int info_error;
	unsigned info_new;
	struct kl_backend_monitor_sample sample;
	struct kl_backend_monitor_info read;
};

/*
 * The backend's monitor and the generation of the info last published:
 * the device I/O thread's alone (or the event loop's where the thread does
 * not run, and at the end after it stopped).
 */
static struct kl_backend_monitor *sysmon_monitor;
static uint64_t sysmon_generation;
static unsigned sysmon_have_info;

/* The one sampling of the process, zero until the first monitor object (see struct sysmon_state). */
static struct sysmon_state sysmon_state;

static void sysmon_give(struct kwl_server *server, unsigned close);
static void sysmon_run(struct kwl_lane_job *job);
static void sysmon_done(void *context, struct kwl_lane_job *job);
static void sysmon_drop(struct kwl_lane_job *job);
static void sysmon_monitor_close(void);
static void sysmon_scan(struct kwl_server *server);
static void sysmon_tell(struct kwl_server *server);
static void sysmon_send_info(struct kwl_object *object);
static void sysmon_send_sample(struct kwl_object *object);
static size_t sysmon_sample_bytes(const struct kl_backend_monitor_sample *sample);
static uint32_t sysmon_period(uint32_t period_ms);
static size_t sysmon_put_word(unsigned char *payload, size_t offset, uint32_t word);
static size_t sysmon_put_wide(unsigned char *payload, size_t offset, uint64_t wide);
static size_t sysmon_put_string(unsigned char *payload, size_t offset, const char *text);
static uint32_t sysmon_word(const unsigned char *bytes, size_t offset);

/* The operations of a sample's job and a close's on the device I/O thread. */
static const struct kwl_lane_ops sysmon_ops = {
	sysmon_run,
	sysmon_done,
	sysmon_drop
};

/*
 * Makes a monitor object for a manager's get_monitor (new id, period):
 * it hears the info at once when there is one, and the samples from then
 * on.  Returns 0, or EPROTO for a malformed request.
 */
int
kwl_sysmon_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;
	uint32_t id;
	uint32_t period;

	/* The new object's ID and its period. */
	if (size != 8U)
		return EPROTO;
	id = sysmon_word(bytes, 0U);
	period = sysmon_word(bytes, 4U);

	/* The object, under the ID the client chose. */
	created = kwl_create(manager->client, id, KWL_SYSTEM_MONITOR, 1U);
	if (created == NULL)
		return EPROTO;
	created->monitor_period = sysmon_period(period);
	created->monitor_waiting = 0;
	created->monitor_info = 0;
	printf("KWL SYSTEM monitor client=%llu id=%u period=%u\n", (unsigned long long)manager->client->number, id, created->monitor_period);

	/* The info, when one was read. */
	if (sysmon_state.taken_info != 0U)
		sysmon_send_info(created);

	/* The sampling starts at the next pass, at the shortest period asked. */
	sysmon_state.scanned_ms = 0;

	/* Succeeded: the object is the client's. */
	return 0;
}

/*
 * Carries out a request of a monitor object: it goes, it acks a sample,
 * or it asks for another period.  Returns 0, or EPROTO for a malformed
 * request.
 */
int
kwl_sysmon_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	uint32_t serial;

	/* The object goes; the sampling stops at a later pass when it was the last. */
	if (opcode == KL_SYSTEM_MONITOR_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		sysmon_state.scanned_ms = 0;
		return 0;
	}

	/* The sample of a serial is taken: the next may come. */
	if (opcode == KL_SYSTEM_MONITOR_ACK) {
		if (size != 4U)
			return EPROTO;
		serial = sysmon_word(bytes, 0U);
		if (serial == object->monitor_waiting)
			object->monitor_waiting = 0;
		return 0;
	}

	/* Another period, within its bounds. */
	if (opcode != KL_SYSTEM_MONITOR_SET_PERIOD || size != 4U)
		return EPROTO;
	object->monitor_period = sysmon_period(sysmon_word(bytes, 0U));
	sysmon_state.scanned_ms = 0;

	/* Succeeded: the request is carried out. */
	return 0;
}

/*
 * Looks after the monitor once a pass: the objects counted now and then
 * (the sampling started for the first, stopped after the last, at the
 * shortest period asked), and the next sample's job given when it is due.
 */
void
kwl_sysmon_tick(
	struct kwl_server *server)
{
	uint64_t now;

	/* The objects, every SYSMON_SCAN_MS or after one came or went. */
	now = kwl_milliseconds();
	if (sysmon_state.scanned_ms == 0U || now >= sysmon_state.scanned_ms + SYSMON_SCAN_MS) {
		sysmon_state.scanned_ms = now;
		sysmon_scan(server);
	}

	/*
	 * While objects are there, they are counted again now and then (a
	 * client that goes takes its objects without a request); without any,
	 * a new one asks for the count itself.
	 */
	if (sysmon_state.subscribers != 0U)
		kwl_due(server, sysmon_state.scanned_ms + SYSMON_SCAN_MS);

	/* The next sample, at the period, once the last is taken. */
	if (sysmon_state.running &&
	    !sysmon_state.sampling &&
	    now >= sysmon_state.next_ms) {
		sysmon_state.next_ms = now + sysmon_state.period_ms;
		sysmon_give(server, 0U);
	}
	if (sysmon_state.running && !sysmon_state.sampling)
		kwl_due(server, sysmon_state.next_ms);
}

/*
 * Stops the sampling at the compositor's end, after the device I/O thread
 * (its jobs were dropped): the backend's monitor is closed here.
 */
void
kwl_sysmon_close(
	struct kwl_server *server)
{
	UNUSED_PARAMETER(server);

	/* Nothing samples any more. */
	sysmon_state.running = 0;
	sysmon_state.sampling = 0;
	sysmon_monitor_close();
	sysmon_state.opened = 0;
}

/*
 * Gives the device I/O thread a sample's job (close 0) or the monitor's
 * close (close 1); without the thread it runs here.  Without memory for
 * the job, the sample is skipped (the next comes at the period) and the
 * close waits for the end.
 */
static void
sysmon_give(
	struct kwl_server *server,
	unsigned close)
{
	struct sysmon_job *job;
	int error;

	/* The job's record. */
	job = calloc(1U, sizeof(*job));
	if (job == NULL)
		return;
	job->close = close;

	/* The info with the first sample and every few. */
	if (!close) {
		if (sysmon_state.given % SYSMON_INFO_EVERY == 0U)
			job->info = 1U;
		sysmon_state.given++;
		sysmon_state.sampling = 1U;
		sysmon_state.opened = 1U;
	}

	/* The thread runs it; without it, it runs here. */
	error = kwl_io_device_submit(server, &job->lane, &sysmon_ops, 0U);
	if (error != 0) {
		sysmon_run(&job->lane);
		sysmon_done(server, &job->lane);
	}
}

/* Takes a sample (and the info when asked), or closes the monitor, on the device I/O thread. */
static void
sysmon_run(
	struct kwl_lane_job *lane_job)
{
	struct sysmon_job *job;
	int error;

	/* The close: the monitor goes, and the next sampling starts afresh. */
	job = (struct sysmon_job *)lane_job;
	if (job->close) {
		sysmon_monitor_close();
		return;
	}

	/* The backend's monitor, opened by the first sample. */
	if (sysmon_monitor == NULL)
		sysmon_monitor = kl_backend_monitor_open();
	if (sysmon_monitor == NULL) {
		job->error = ENODEV;
		job->info_error = ENODEV;
		return;
	}

	/* The info when due: new when it is the first or the devices changed. */
	job->info_error = ENOENT;
	if (job->info) {
		error = kl_backend_monitor_info(sysmon_monitor, &job->read);
		job->info_error = error;
		if (error != 0) {
			job->info_new = 0U;
		} else if (!sysmon_have_info) {
			job->info_new = 1U;
		} else if (job->read.generation != sysmon_generation) {
			job->info_new = 1U;
		}

		/* A new info is the one published from now. */
		if (job->info_new) {
			sysmon_generation = job->read.generation;
			sysmon_have_info = 1U;
		}
	}

	/* The sample. */
	error = kl_backend_monitor_sample(sysmon_monitor, &job->sample);
	job->error = error;
}

/*
 * Takes a sample's job, on the event loop's thread: the sample and a new
 * info become the latest and are told to every monitor object.
 */
static void
sysmon_done(
	void *context,
	struct kwl_lane_job *lane_job)
{
	struct sysmon_job *job;

	/* A close needs nothing more. */
	job = (struct sysmon_job *)lane_job;
	if (job->close) {
		free(job);
		return;
	}

	/* The latest, with serials that tell the objects they are new. */
	sysmon_state.sampling = 0U;
	if (job->info_new) {
		sysmon_state.current_info = job->read;
		sysmon_state.info_serial++;
	}

	/* The sample, when it was read. */
	if (job->error == 0) {
		sysmon_state.current = job->sample;
		sysmon_state.current_serial++;
	}

	/* Frees the job, whose results are copied. */
	free(job);

	/* Told to every object that may hear it, while the sampling runs. */
	if (sysmon_state.running)
		sysmon_tell(context);
}

/* Frees a job that did not run (the thread stopped at the end; the close then comes from kwl_sysmon_close). */
static void
sysmon_drop(
	struct kwl_lane_job *lane_job)
{
	/* Nothing waits for it. */
	sysmon_state.sampling = 0U;
	free(lane_job);
}

/* Closes the backend's monitor, on the thread that owns it; the next info is the first again. */
static void
sysmon_monitor_close(void)
{
	/* The monitor, if open. */
	kl_backend_monitor_close(sysmon_monitor);
	sysmon_monitor = NULL;
	sysmon_generation = 0U;
	sysmon_have_info = 0U;
}

/*
 * Counts the monitor objects and their shortest period: the sampling
 * starts for the first and follows the period, and stops after the last
 * (the backend's monitor closed on the device I/O thread).
 */
static void
sysmon_scan(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *object;
	uint32_t period;
	unsigned count;

	/* Each live monitor object, and the shortest period. */
	count = 0;
	period = KL_SYSTEM_MONITOR_PERIOD_MAX;
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (object = client->objects; object != NULL; object = object->next) {
			if (object->kind != KWL_SYSTEM_MONITOR || object->dead)
				continue;
			count++;
			if (object->monitor_period < period)
				period = object->monitor_period;
		}
	}

	/* The log says when the count changes. */
	if (count != sysmon_state.subscribers)
		printf("KWL SYSTEM monitor subscribers=%u period=%u\n", count, period);
	sysmon_state.subscribers = count;

	/* No object: the sampling stops, and the monitor is closed after the sample under way. */
	if (count == 0U) {
		sysmon_state.running = 0U;
		if (sysmon_state.opened) {
			sysmon_state.opened = 0U;
			sysmon_give(server, 1U);
		}
		return;
	}

	/* The shortest period asked; the first object's sampling starts at once. */
	sysmon_state.period_ms = period;
	if (!sysmon_state.running) {
		sysmon_state.running = 1U;
		sysmon_state.given = 0U;
		sysmon_state.next_ms = 0U;
	}
}

/* Tells every monitor object that may hear it the latest sample (and a new info). */
static void
sysmon_tell(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *object;
	uint32_t sample_serial;
	uint32_t info_serial;
	size_t needed;

	/* The serials of the latest. */
	sample_serial = sysmon_state.current_serial;
	info_serial = sysmon_state.info_serial;

	/* Nothing new. */
	if (sample_serial == sysmon_state.taken_sample && info_serial == sysmon_state.taken_info)
		return;
	sysmon_state.taken_info = info_serial;

	/* A sample's size on the wire, to see whether a client has room for it. */
	needed = sysmon_sample_bytes(&sysmon_state.current);

	/* Each live monitor object: a new info, then the new sample when it acked the last and has room. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (object = client->objects; object != NULL; object = object->next) {
			if (object->kind != KWL_SYSTEM_MONITOR || object->dead)
				continue;

			/* The info it has not heard. */
			if (object->monitor_info != info_serial && info_serial != 0U)
				sysmon_send_info(object);

			/* The sample, whole or not at all. */
			if (sample_serial == sysmon_state.taken_sample || sample_serial == 0U)
				continue;
			if (object->monitor_waiting != 0U)
				continue;
			if (client->output_bytes + needed > KWL_OUTPUT_MAX)
				continue;
			sysmon_send_sample(object);
		}
	}

	/* The sample is taken. */
	sysmon_state.taken_sample = sample_serial;
}

/* Sends an object the info: the machine, a device a GPU, disk or link, and the done. */
static void
sysmon_send_info(
	struct kwl_object *object)
{
	const struct kl_backend_monitor_info *info;
	unsigned char payload[SYSMON_EVENT_MAX];
	size_t offset;
	unsigned index;

	/* The machine. */
	info = &sysmon_state.current_info;
	offset = sysmon_put_word(payload, 0U, info->cpu_count);
	offset = sysmon_put_string(payload, offset, info->host);
	offset = sysmon_put_wide(payload, offset, info->generation);
	(void)kwl_emit(object->client, object->id, KL_SYSTEM_MONITOR_EVENT_INFO, payload, offset);

	/* Each GPU. */
	for (index = 0; index < info->gpu_count; index++) {
		offset = sysmon_put_word(payload, 0U, KL_SYSTEM_MONITOR_DEVICE_GPU);
		offset = sysmon_put_wide(payload, offset, info->gpu[index].id);
		offset = sysmon_put_wide(payload, offset, info->gpu[index].generation);
		offset = sysmon_put_word(payload, offset, 0U);
		offset = sysmon_put_string(payload, offset, info->gpu[index].name);
		offset = sysmon_put_string(payload, offset, info->gpu[index].driver);
		(void)kwl_emit(object->client, object->id, KL_SYSTEM_MONITOR_EVENT_DEVICE, payload, offset);
	}

	/* Each disk, with its kind. */
	for (index = 0; index < info->disk_count; index++) {
		offset = sysmon_put_word(payload, 0U, KL_SYSTEM_MONITOR_DEVICE_DISK);
		offset = sysmon_put_wide(payload, offset, info->disk[index].id);
		offset = sysmon_put_wide(payload, offset, info->disk[index].generation);
		offset = sysmon_put_word(payload, offset, info->disk[index].kind);
		offset = sysmon_put_string(payload, offset, info->disk[index].name);
		offset = sysmon_put_string(payload, offset, "");
		(void)kwl_emit(object->client, object->id, KL_SYSTEM_MONITOR_EVENT_DEVICE, payload, offset);
	}

	/* Each link. */
	for (index = 0; index < info->link_count; index++) {
		offset = sysmon_put_word(payload, 0U, KL_SYSTEM_MONITOR_DEVICE_LINK);
		offset = sysmon_put_wide(payload, offset, info->link[index].id);
		offset = sysmon_put_wide(payload, offset, info->link[index].generation);
		offset = sysmon_put_word(payload, offset, 0U);
		offset = sysmon_put_string(payload, offset, info->link[index].name);
		offset = sysmon_put_string(payload, offset, "");
		(void)kwl_emit(object->client, object->id, KL_SYSTEM_MONITOR_EVENT_DEVICE, payload, offset);
	}

	/* The done: the info is whole. */
	offset = sysmon_put_word(payload, 0U, sysmon_state.taken_info);
	(void)kwl_emit(object->client, object->id, KL_SYSTEM_MONITOR_EVENT_INFO_DONE, payload, offset);
	object->monitor_info = sysmon_state.taken_info;
}

/* Sends an object the current sample: each CPU, the memory, each link, disk and GPU, and the done it acks. */
static void
sysmon_send_sample(
	struct kwl_object *object)
{
	const struct kl_backend_monitor_sample *sample;
	unsigned char payload[SYSMON_EVENT_MAX];
	uint32_t serial;
	size_t offset;
	unsigned index;

	/* Each CPU's ticks. */
	sample = &sysmon_state.current;
	for (index = 0; index < sample->cpu_count; index++) {
		offset = sysmon_put_word(payload, 0U, index);
		offset = sysmon_put_wide(payload, offset, sample->cpu[index].user);
		offset = sysmon_put_wide(payload, offset, sample->cpu[index].system);
		offset = sysmon_put_wide(payload, offset, sample->cpu[index].idle);
		offset = sysmon_put_wide(payload, offset, sample->cpu[index].other);
		(void)kwl_emit(object->client, object->id, KL_SYSTEM_MONITOR_EVENT_CPU, payload, offset);
	}

	/* The memory. */
	offset = sysmon_put_wide(payload, 0U, sample->memory_total);
	offset = sysmon_put_wide(payload, offset, sample->memory_free);
	offset = sysmon_put_wide(payload, offset, sample->memory_cache);
	offset = sysmon_put_wide(payload, offset, sample->memory_reclaimable);
	offset = sysmon_put_wide(payload, offset, sample->swap_total);
	offset = sysmon_put_wide(payload, offset, sample->swap_used);
	(void)kwl_emit(object->client, object->id, KL_SYSTEM_MONITOR_EVENT_MEMORY, payload, offset);

	/* Each link. */
	for (index = 0; index < sample->link_count; index++) {
		offset = sysmon_put_wide(payload, 0U, sample->link[index].id);
		offset = sysmon_put_wide(payload, offset, sample->link[index].rx_bytes);
		offset = sysmon_put_wide(payload, offset, sample->link[index].tx_bytes);
		offset = sysmon_put_word(payload, offset, sample->link[index].up);
		(void)kwl_emit(object->client, object->id, KL_SYSTEM_MONITOR_EVENT_LINK, payload, offset);
	}

	/* Each disk. */
	for (index = 0; index < sample->disk_count; index++) {
		offset = sysmon_put_wide(payload, 0U, sample->disk[index].id);
		offset = sysmon_put_wide(payload, offset, sample->disk[index].read_ops);
		offset = sysmon_put_wide(payload, offset, sample->disk[index].write_ops);
		offset = sysmon_put_wide(payload, offset, sample->disk[index].read_bytes);
		offset = sysmon_put_wide(payload, offset, sample->disk[index].write_bytes);
		offset = sysmon_put_wide(payload, offset, sample->disk[index].read_ns);
		offset = sysmon_put_wide(payload, offset, sample->disk[index].write_ns);
		offset = sysmon_put_wide(payload, offset, sample->disk[index].busy_ns);
		(void)kwl_emit(object->client, object->id, KL_SYSTEM_MONITOR_EVENT_DISK, payload, offset);
	}

	/* Each GPU. */
	for (index = 0; index < sample->gpu_count; index++) {
		offset = sysmon_put_wide(payload, 0U, sample->gpu[index].id);
		offset = sysmon_put_wide(payload, offset, sample->gpu[index].time_ns);
		offset = sysmon_put_wide(payload, offset, sample->gpu[index].busy_ns);
		offset = sysmon_put_wide(payload, offset, sample->gpu[index].memory_used);
		offset = sysmon_put_wide(payload, offset, sample->gpu[index].memory_total);
		offset = sysmon_put_word(payload, offset, sample->gpu[index].cur_mhz);
		offset = sysmon_put_word(payload, offset, sample->gpu[index].max_mhz);
		offset = sysmon_put_word(payload, offset, (uint32_t)sample->gpu[index].milli_celsius);
		offset = sysmon_put_word(payload, offset, sample->gpu[index].milli_watts);
		(void)kwl_emit(object->client, object->id, KL_SYSTEM_MONITOR_EVENT_GPU, payload, offset);
	}

	/* The done, which the client acks: the sample is whole. */
	serial = sysmon_state.current_serial;
	offset = sysmon_put_word(payload, 0U, serial);
	offset = sysmon_put_wide(payload, offset, sample->time_ns);
	offset = sysmon_put_wide(payload, offset, sample->valid);
	offset = sysmon_put_word(payload, offset, (uint32_t)sample->cpu_hz);
	offset = sysmon_put_word(payload, offset, (uint32_t)sample->cpu_milli_celsius);
	(void)kwl_emit(object->client, object->id, KL_SYSTEM_MONITOR_EVENT_SAMPLE_DONE, payload, offset);
	object->monitor_waiting = serial;
}

/* A sample's size on the wire: each event's header and words. */
static size_t
sysmon_sample_bytes(
	const struct kl_backend_monitor_sample *sample)
{
	size_t bytes;

	/* The CPUs (9 words), the memory (12), the links (7), the disks (16), the GPUs (14), the done (7), each with its 8-byte header. */
	bytes = (size_t)sample->cpu_count * (8U + 9U * 4U);
	bytes += 8U + 12U * 4U;
	bytes += (size_t)sample->link_count * (8U + 7U * 4U);
	bytes += (size_t)sample->disk_count * (8U + 16U * 4U);
	bytes += (size_t)sample->gpu_count * (8U + 14U * 4U);
	bytes += 8U + 7U * 4U;

	/* Succeeded: the size. */
	return bytes;
}

/* A period within the protocol's bounds; 0 is the default. */
static uint32_t
sysmon_period(
	uint32_t period_ms)
{
	/* None asked: the default. */
	if (period_ms == 0U)
		return KL_SYSTEM_MONITOR_PERIOD_DEFAULT;

	/* Within the bounds. */
	if (period_ms < KL_SYSTEM_MONITOR_PERIOD_MIN)
		return KL_SYSTEM_MONITOR_PERIOD_MIN;
	if (period_ms > KL_SYSTEM_MONITOR_PERIOD_MAX)
		return KL_SYSTEM_MONITOR_PERIOD_MAX;

	/* Succeeded: the period as asked. */
	return period_ms;
}

/* Writes a word and gives the offset after it. */
static size_t
sysmon_put_word(
	unsigned char *payload,
	size_t offset,
	uint32_t word)
{
	/* The word in the wire's native byte order. */
	memcpy(payload + offset, &word, sizeof(word));

	/* Succeeded: the offset after it. */
	return offset + sizeof(word);
}

/* Writes a u64 as its high and low words and gives the offset after them. */
static size_t
sysmon_put_wide(
	unsigned char *payload,
	size_t offset,
	uint64_t wide)
{
	size_t next;

	/* The high half, then the low. */
	next = sysmon_put_word(payload, offset, (uint32_t)(wide >> 32));
	next = sysmon_put_word(payload, next, (uint32_t)(wide & 0xffffffffU));

	/* Succeeded: the offset after both. */
	return next;
}

/* Writes a string argument (cut to fit an event) and gives the offset after it. */
static size_t
sysmon_put_string(
	unsigned char *payload,
	size_t offset,
	const char *text)
{
	uint32_t length;
	size_t padded;
	size_t bytes;

	/* The length with the NUL (at most 64 bytes of text), the bytes, and zeros to a four-byte boundary. */
	bytes = strlen(text);
	if (bytes > 64U)
		bytes = 64U;
	length = (uint32_t)bytes + 1U;
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	memcpy(payload + offset, &length, sizeof(length));
	memset(payload + offset + 4U, 0, padded);
	memcpy(payload + offset + 4U, text, bytes);

	/* The offset after the string. */
	return offset + 4U + padded;
}

/* Reads a word of a request. */
static uint32_t
sysmon_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The word. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: the word. */
	return word;
}
