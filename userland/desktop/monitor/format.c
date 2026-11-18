/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Monitor's formatting of values for the screen and the log:
 * shares as percents, sizes in binary units, rates in decimal ones (bytes
 * or bits a second), a used/total pair and an uptime.
 */

#include "monitor.h"

#include <stdio.h>

/* The units of sizes (binary) and of rates (decimal), from the smallest. */
static const char *const size_units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
static const char *const byte_rate_units[] = { "B/s", "KB/s", "MB/s", "GB/s", "TB/s" };
static const char *const bit_rate_units[] = { "b/s", "Kb/s", "Mb/s", "Gb/s", "Tb/s" };

static size_t format_scaled(char *out, size_t size, double value, double step, const char *const *units, unsigned unit_count);

/*
 * Writes a share (0 to 1) as a whole percent, "37%".
 */
size_t
sm_format_percent(
	char *out,
	size_t size,
	double share)
{
	int percent;
	int written;

	/* The share, rounded and kept within 0 and 100. */
	percent = (int)(share * 100.0 + 0.5);
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;

	/* The text. */
	written = snprintf(out, size, "%d%%", percent);
	if (written < 0)
		return 0;

	/* Succeeded: the length written. */
	return (size_t)written;
}

/*
 * Writes a size in binary units with one decimal below ten, "2.1 GiB".
 */
size_t
sm_format_bytes(
	char *out,
	size_t size,
	uint64_t bytes)
{
	/* Steps of 1024. */
	return format_scaled(out, size, (double)bytes, 1024.0, size_units, 5U);
}

/*
 * Writes a rate in decimal units, of bytes or (bits) of bits a second.
 */
size_t
sm_format_rate(
	char *out,
	size_t size,
	double bytes_per_second,
	int bits)
{
	/* Bits are eight to a byte. */
	if (bits)
		return format_scaled(out, size, bytes_per_second * 8.0, 1000.0, bit_rate_units, 5U);

	/* Bytes. */
	return format_scaled(out, size, bytes_per_second, 1000.0, byte_rate_units, 5U);
}

/*
 * Writes a used part of a total in the total's unit, "2.1 / 8 GiB".
 */
size_t
sm_format_pair(
	char *out,
	size_t size,
	uint64_t used,
	uint64_t total)
{
	double scale;
	double used_value;
	double total_value;
	unsigned unit;
	int decimals;
	int written;

	/* The total's unit. */
	scale = 1.0;
	unit = 0;
	while ((double)total >= scale * 1024.0 && unit + 1U < 5U) {
		scale *= 1024.0;
		unit++;
	}

	/* Both values in that unit. */
	used_value = (double)used / scale;
	total_value = (double)total / scale;

	/* One decimal for the used part below ten. */
	decimals = 0;
	if (used_value < 10.0)
		decimals = 1;

	/* None for the total when it is whole. */
	if (total_value == (double)(uint64_t)total_value) {
		written = snprintf(out, size, "%.*f / %.0f %s", decimals, used_value, total_value, size_units[unit]);
	} else {
		written = snprintf(out, size, "%.*f / %.1f %s", decimals, used_value, total_value, size_units[unit]);
	}

	/* A failed write is nothing. */
	if (written < 0)
		return 0;

	/* Succeeded: the length written. */
	return (size_t)written;
}

/*
 * Writes an uptime as days, hours and minutes, "2:14" or "3d 2:14".
 */
size_t
sm_format_uptime(
	char *out,
	size_t size,
	uint64_t seconds)
{
	uint64_t days;
	uint64_t hours;
	uint64_t minutes;
	int written;

	/* The parts. */
	days = seconds / 86400U;
	hours = (seconds / 3600U) % 24U;
	minutes = (seconds / 60U) % 60U;

	/* Days only when there are any. */
	if (days != 0U) {
		written = snprintf(out, size, "%llud %llu:%02llu", (unsigned long long)days, (unsigned long long)hours,
				   (unsigned long long)minutes);
	} else {
		written = snprintf(out, size, "%llu:%02llu", (unsigned long long)hours, (unsigned long long)minutes);
	}

	/* A failed write is nothing. */
	if (written < 0)
		return 0;

	/* Succeeded: the length written. */
	return (size_t)written;
}

/* Writes a value in the largest unit that keeps it at one or more, one decimal below ten. */
static size_t
format_scaled(
	char *out,
	size_t size,
	double value,
	double step,
	const char *const *units,
	unsigned unit_count)
{
	unsigned unit;
	int written;

	/* A negative value is none. */
	if (value < 0.0)
		value = 0.0;

	/* Up the units while the value is a whole step or more. */
	unit = 0;
	while (value >= step && unit + 1U < unit_count) {
		value /= step;
		unit++;
	}

	/* One decimal below ten in a unit above the smallest. */
	if (unit != 0U && value < 10.0) {
		written = snprintf(out, size, "%.1f %s", value, units[unit]);
	} else {
		written = snprintf(out, size, "%.0f %s", value, units[unit]);
	}

	/* A failed write is nothing. */
	if (written < 0)
		return 0;

	/* Succeeded: the length written. */
	return (size_t)written;
}
