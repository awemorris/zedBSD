/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Monitor's history: for each summary graph, a ring of the last
 * hour of one-second samples (design.md section 3.10).  The values kept are
 * what the graphs draw: shares for the CPU, the GPU and the memory, rates
 * in bytes a second for the network and the disks.
 */

#include "monitor.h"

#include <string.h>

static void ring_add(struct sm_ring *ring, float value);

/*
 * Empties every series.
 */
void
sm_history_init(
	struct sm_history *history)
{
	/* Nothing kept. */
	memset(history, 0, sizeof(*history));
}

/*
 * Adds a frame's values to the series.
 */
void
sm_history_add(
	struct sm_history *history,
	const struct sm_info *info,
	const struct sm_frame *frame)
{
	double memory_share;

	/* The memory's share of the total that is in use. */
	memory_share = 0.0;
	if (info->memory_total != 0U)
		memory_share = (double)frame->memory_used / (double)info->memory_total;

	/* One value for each series. */
	ring_add(&history->series[SM_SERIES_CPU], (float)frame->cpu);
	ring_add(&history->series[SM_SERIES_GPU], (float)frame->gpu_busy[0]);
	ring_add(&history->series[SM_SERIES_MEMORY], (float)memory_share);
	ring_add(&history->series[SM_SERIES_RX], (float)frame->rx_rate);
	ring_add(&history->series[SM_SERIES_TX], (float)frame->tx_rate);
	ring_add(&history->series[SM_SERIES_READ], (float)frame->read_rate);
	ring_add(&history->series[SM_SERIES_WRITE], (float)frame->write_rate);
}

/*
 * Copies the newest count values of a series, oldest first, and reports
 * how many there were (fewer while the history fills).
 */
unsigned
sm_history_read(
	const struct sm_history *history,
	enum sm_series series,
	unsigned count,
	float *values)
{
	const struct sm_ring *ring;
	unsigned index;
	unsigned start;

	/* As many as were kept. */
	ring = &history->series[series];
	if (count > ring->count)
		count = ring->count;

	/* From the oldest of them to the newest. */
	start = (ring->head + SM_HISTORY_MAX - count) % SM_HISTORY_MAX;
	for (index = 0; index < count; index++)
		values[index] = ring->values[(start + index) % SM_HISTORY_MAX];

	/* Succeeded: the number copied. */
	return count;
}

/*
 * Reports the largest of the newest count values of a series (0 when none).
 */
float
sm_history_peak(
	const struct sm_history *history,
	enum sm_series series,
	unsigned count)
{
	const struct sm_ring *ring;
	unsigned index;
	unsigned start;
	float peak;
	float value;

	/* As many as were kept. */
	ring = &history->series[series];
	if (count > ring->count)
		count = ring->count;

	/* The largest from the oldest of them to the newest. */
	peak = 0.0f;
	start = (ring->head + SM_HISTORY_MAX - count) % SM_HISTORY_MAX;
	for (index = 0; index < count; index++) {
		value = ring->values[(start + index) % SM_HISTORY_MAX];
		if (value > peak)
			peak = value;
	}

	/* Succeeded: the peak. */
	return peak;
}

/* Adds a value at the head of a ring, dropping the oldest when it is full. */
static void
ring_add(
	struct sm_ring *ring,
	float value)
{
	/* The value at the head. */
	ring->values[ring->head] = value;
	ring->head = (ring->head + 1U) % SM_HISTORY_MAX;

	/* One more, up to the ring's size. */
	if (ring->count < SM_HISTORY_MAX)
		ring->count++;
}
