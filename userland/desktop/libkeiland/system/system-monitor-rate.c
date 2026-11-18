/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The machine's monitor's rates (WS134 p012): a frame made from two raw
 * samples, the shares, the bytes and operations a second and the mean
 * latency over the time between them.  It knows nothing of Wayland, so
 * that the host tests build it alone (plan/ws134/tests/host/).
 *
 * A device is matched by its id, never its place.  A device the sample
 * before did not have, or a counter that went back (the device came again,
 * or the system started counting over), has no rate in this frame: its
 * rates are zero and it adds nothing to the sums.
 */

#include "system-private.h"

#include <string.h>

static double rate_of(uint64_t before, uint64_t after, double seconds, int *reset);
static const struct system_monitor_link *find_link(const struct system_monitor_raw *raw, uint64_t id);
static const struct system_monitor_disk *find_disk(const struct system_monitor_raw *raw, uint64_t id);
static const struct system_monitor_gpu *find_gpu(const struct system_monitor_raw *raw, uint64_t id);
static void cpu_rates(const struct system_monitor_raw *previous, const struct system_monitor_raw *current, struct kl_monitor_frame *frame);
static void link_rates(const struct system_monitor_raw *previous, const struct system_monitor_raw *current, struct kl_monitor_frame *frame);
static void disk_rates(const struct system_monitor_raw *previous, const struct system_monitor_raw *current, struct kl_monitor_frame *frame);
static void gpu_rates(const struct system_monitor_raw *previous, const struct system_monitor_raw *current, struct kl_monitor_frame *frame);

/*
 * Makes a frame from the sample before and the current one: the time
 * between them, the CPUs, the memory, the links, the disks and the GPUs.
 */
void
system_monitor_rates(
	const struct system_monitor_raw *previous,
	const struct system_monitor_raw *current,
	struct kl_monitor_frame *frame)
{
	/* Nothing yet; the time and what the current sample has. */
	memset(frame, 0, sizeof(*frame));
	frame->time_ns = current->time_ns;
	frame->valid = current->valid;
	if (current->time_ns > previous->time_ns)
		frame->seconds = (double)(current->time_ns - previous->time_ns) / 1e9;

	/* The present values. */
	frame->memory_total = current->memory_total;
	frame->memory_free = current->memory_free;
	frame->memory_cache = current->memory_cache;
	frame->memory_reclaimable = current->memory_reclaimable;
	frame->swap_total = current->swap_total;
	frame->swap_used = current->swap_used;
	frame->cpu_milli_celsius = current->cpu_milli_celsius;

	/* The rates. */
	cpu_rates(previous, current, frame);
	link_rates(previous, current, frame);
	disk_rates(previous, current, frame);
	gpu_rates(previous, current, frame);
}

/* A counter's change a second; a counter that went back (or no time) sets *reset and gives 0. */
static double
rate_of(
	uint64_t before,
	uint64_t after,
	double seconds,
	int *reset)
{
	/* Gone back: counted over. */
	if (after < before) {
		*reset = 1;
		return 0.0;
	}

	/* No time between the samples. */
	if (seconds <= 0.0)
		return 0.0;

	/* Succeeded: the change a second. */
	return (double)(after - before) / seconds;
}

/* The sample's link of an id, or NULL. */
static const struct system_monitor_link *
find_link(
	const struct system_monitor_raw *raw,
	uint64_t id)
{
	unsigned index;

	/* Each link. */
	for (index = 0; index < raw->link_count; index++) {
		if (raw->link[index].id == id)
			return &raw->link[index];
	}

	/* Not in the sample. */
	return NULL;
}

/* The sample's disk of an id, or NULL. */
static const struct system_monitor_disk *
find_disk(
	const struct system_monitor_raw *raw,
	uint64_t id)
{
	unsigned index;

	/* Each disk. */
	for (index = 0; index < raw->disk_count; index++) {
		if (raw->disk[index].id == id)
			return &raw->disk[index];
	}

	/* Not in the sample. */
	return NULL;
}

/* The sample's GPU of an id, or NULL. */
static const struct system_monitor_gpu *
find_gpu(
	const struct system_monitor_raw *raw,
	uint64_t id)
{
	unsigned index;

	/* Each GPU. */
	for (index = 0; index < raw->gpu_count; index++) {
		if (raw->gpu[index].id == id)
			return &raw->gpu[index];
	}

	/* Not in the sample. */
	return NULL;
}

/* Each CPU's busy share (all but idle, over all its ticks) and the whole's (over every CPU's ticks). */
static void
cpu_rates(
	const struct system_monitor_raw *previous,
	const struct system_monitor_raw *current,
	struct kl_monitor_frame *frame)
{
	const struct system_monitor_cpu *before;
	const struct system_monitor_cpu *after;
	uint64_t total;
	uint64_t idle;
	uint64_t all_total;
	uint64_t all_idle;
	unsigned cpu;

	/* As many CPUs as both samples have. */
	frame->cpu_count = current->cpu_count;
	if (previous->cpu_count < frame->cpu_count)
		frame->cpu_count = previous->cpu_count;

	/* Each CPU's ticks since the sample before. */
	all_total = 0;
	all_idle = 0;
	for (cpu = 0; cpu < frame->cpu_count; cpu++) {
		before = &previous->cpu[cpu];
		after = &current->cpu[cpu];

		/* A CPU whose counters went back has no share this frame. */
		if (after->user < before->user || after->system < before->system)
			continue;
		if (after->idle < before->idle || after->other < before->other)
			continue;

		/* Its ticks, and its idle ones. */
		total = (after->user - before->user) + (after->system - before->system) +
		    (after->idle - before->idle) + (after->other - before->other);
		idle = after->idle - before->idle;
		if (total == 0U)
			continue;

		/* Its share busy. */
		frame->cpu_core[cpu] = (double)(total - idle) / (double)total;
		all_total += total;
		all_idle += idle;
	}

	/* The whole's share busy. */
	if (all_total != 0U)
		frame->cpu = (double)(all_total - all_idle) / (double)all_total;
}

/* Each link's bytes a second, matched by id, and their sums. */
static void
link_rates(
	const struct system_monitor_raw *previous,
	const struct system_monitor_raw *current,
	struct kl_monitor_frame *frame)
{
	const struct system_monitor_link *before;
	const struct system_monitor_link *after;
	struct kl_monitor_link_rate *rate;
	unsigned index;
	int reset;

	/* Each current link. */
	frame->link_count = current->link_count;
	for (index = 0; index < current->link_count; index++) {
		after = &current->link[index];
		rate = &frame->link[index];
		rate->id = after->id;
		rate->up = after->up;

		/* A link the sample before did not have has no rate yet. */
		before = find_link(previous, after->id);
		if (before == NULL)
			continue;

		/* Its rates; a counter that went back gives none. */
		reset = 0;
		rate->rx_rate = rate_of(before->rx, after->rx, frame->seconds, &reset);
		rate->tx_rate = rate_of(before->tx, after->tx, frame->seconds, &reset);
		if (reset) {
			rate->rx_rate = 0.0;
			rate->tx_rate = 0.0;
			continue;
		}

		/* Into the sums. */
		frame->rx_rate += rate->rx_rate;
		frame->tx_rate += rate->tx_rate;
	}
}

/*
 * Each disk's bytes and operations a second, mean latency and busy share,
 * matched by id, and the sums (the latency over every operation of every
 * disk).
 */
static void
disk_rates(
	const struct system_monitor_raw *previous,
	const struct system_monitor_raw *current,
	struct kl_monitor_frame *frame)
{
	const struct system_monitor_disk *before;
	const struct system_monitor_disk *after;
	struct kl_monitor_disk_rate *rate;
	uint64_t operations;
	uint64_t nanoseconds;
	uint64_t all_operations;
	uint64_t all_nanoseconds;
	unsigned index;
	int reset;

	/* Each current disk. */
	all_operations = 0;
	all_nanoseconds = 0;
	frame->disk_count = current->disk_count;
	for (index = 0; index < current->disk_count; index++) {
		after = &current->disk[index];
		rate = &frame->disk[index];
		rate->id = after->id;

		/* A disk the sample before did not have has no rate yet. */
		before = find_disk(previous, after->id);
		if (before == NULL)
			continue;

		/* Its rates; a counter that went back gives none. */
		reset = 0;
		rate->read_rate = rate_of(before->read_bytes, after->read_bytes, frame->seconds, &reset);
		rate->write_rate = rate_of(before->write_bytes, after->write_bytes, frame->seconds, &reset);
		rate->read_ops = rate_of(before->read_ops, after->read_ops, frame->seconds, &reset);
		rate->write_ops = rate_of(before->write_ops, after->write_ops, frame->seconds, &reset);
		rate->busy = rate_of(before->busy_ns, after->busy_ns, frame->seconds, &reset) / 1e9;
		(void)rate_of(before->read_ns, after->read_ns, frame->seconds, &reset);
		(void)rate_of(before->write_ns, after->write_ns, frame->seconds, &reset);
		if (reset) {
			memset(rate, 0, sizeof(*rate));
			rate->id = after->id;
			continue;
		}

		/* Its mean latency: the time over the operations that completed. */
		operations = (after->read_ops - before->read_ops) + (after->write_ops - before->write_ops);
		nanoseconds = (after->read_ns - before->read_ns) + (after->write_ns - before->write_ns);
		if (operations != 0U)
			rate->latency_ms = (double)nanoseconds / (double)operations / 1e6;
		if (rate->busy > 1.0)
			rate->busy = 1.0;

		/* Into the sums. */
		frame->read_rate += rate->read_rate;
		frame->write_rate += rate->write_rate;
		all_operations += operations;
		all_nanoseconds += nanoseconds;
	}

	/* The mean latency of every operation. */
	if (all_operations != 0U)
		frame->disk_latency_ms = (double)all_nanoseconds / (double)all_operations / 1e6;
}

/* Each GPU's busy share over its driver's clock, matched by id, and its present values. */
static void
gpu_rates(
	const struct system_monitor_raw *previous,
	const struct system_monitor_raw *current,
	struct kl_monitor_frame *frame)
{
	const struct system_monitor_gpu *before;
	const struct system_monitor_gpu *after;
	struct kl_monitor_gpu_rate *rate;
	unsigned index;

	/* Each current GPU. */
	frame->gpu_count = current->gpu_count;
	for (index = 0; index < current->gpu_count; index++) {
		after = &current->gpu[index];
		rate = &frame->gpu[index];
		rate->id = after->id;
		rate->memory_used = after->memory_used;
		rate->memory_total = after->memory_total;
		rate->cur_mhz = after->cur_mhz;
		rate->max_mhz = after->max_mhz;
		rate->milli_celsius = after->milli_celsius;
		rate->milli_watts = after->milli_watts;

		/* A GPU the sample before did not have, or whose clock or busy time went back, has no share yet. */
		before = find_gpu(previous, after->id);
		if (before == NULL)
			continue;
		if (after->time_ns <= before->time_ns || after->busy_ns < before->busy_ns)
			continue;

		/* Its share busy, at most all of the time. */
		rate->busy = (double)(after->busy_ns - before->busy_ns) / (double)(after->time_ns - before->time_ns);
		if (rate->busy > 1.0)
			rate->busy = 1.0;
	}
}
