/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Private QEMU workload; measurements describe guest operations, not hardware. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <zedbsd/io-stats.h>
#include <time.h>
#include <unistd.h>

#define IMOD_BYTES 65536U
#define IMOD_SAMPLES 64U

/* One request and independent readback storage, retained for the whole run. */
static unsigned char written[IMOD_BYTES];
static unsigned char observed[IMOD_BYTES];

static int monotonic_ns(uint64_t *result);
static uint64_t cpu_us(const struct rusage *usage);

/*
 * Measures confirmed writes and exact readback on the private USB overlay.
 */
int
imod_storage_measure(void)
{
	const char *path = "/root/imod-measure.bin";
	struct timespec resolution;
	struct io_stats irq_before, irq_after;
	size_t stats_size;
	uint64_t irq_start, irq_end;
	struct rusage before;
	struct rusage after;
	uint64_t start;
	uint64_t end;
	uint64_t a;
	uint64_t b;
	uint64_t c;
	uint64_t d;
	unsigned sample;
	unsigned index;
	int descriptor;
	int error;
	ssize_t count;

	/* Creates only a named test file in the disposable installation. */
	descriptor = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (descriptor < 0)
		goto failed_open;
	error = clock_getres(CLOCK_MONOTONIC, &resolution);
	if (error != 0)
		goto failed;
	printf("IMOD-STORAGE READY samples=%u bytes=%u path=%s clock_resolution_ns=%llu\n",
	    IMOD_SAMPLES, IMOD_BYTES, path,
	    (unsigned long long)((uint64_t)resolution.tv_sec *
	    UINT64_C(1000000000) + resolution.tv_nsec));
	fflush(stdout);
	error = getrusage(RUSAGE_SELF, &before);
	if (error != 0)
		goto failed;
	error = monotonic_ns(&start);
	if (error != 0)
		goto failed;

	stats_size = sizeof(irq_before);
	if (monotonic_ns(&irq_start) != 0 ||
	    sysctlbyname("vfs.io.stats", &irq_before, &stats_size, NULL, 0) != 0 ||
	    stats_size != sizeof(irq_before) || irq_before.version != IO_STATS_VERSION ||
	    irq_before.count != IO_STAT_COUNT)
		goto failed;

	/* Changes every byte each round and validates the entire confirmed prefix. */
	for (sample = 0; sample < IMOD_SAMPLES; sample++) {
		for (index = 0; index < IMOD_BYTES; index++)
			written[index] = (unsigned char)(index * 17U + sample * 31U);
		memset(observed, 0, sizeof(observed));
		error = monotonic_ns(&a);
		if (error != 0)
			goto failed;
		count = pwrite(descriptor, written, sizeof(written), 0);
		if (count != (ssize_t)sizeof(written))
			goto failed;
		error = monotonic_ns(&b);
		if (error != 0)
			goto failed;
		error = fsync(descriptor);
		if (error != 0)
			goto failed;
		error = monotonic_ns(&c);
		if (error != 0)
			goto failed;
		count = pread(descriptor, observed, sizeof(observed), 0);
		if (count != (ssize_t)sizeof(observed))
			goto failed;
		error = monotonic_ns(&d);
		if (error != 0)
			goto failed;
		if (a > b || b > c || c > d)
			goto failed;
		error = memcmp(written, observed, sizeof(written));
		if (error != 0)
			goto failed;
		printf("IMOD-STORAGE SAMPLE %u write_ns=%llu fsync_ns=%llu read_ns=%llu\n",
		    sample, (unsigned long long)(b - a),
		    (unsigned long long)(c - b), (unsigned long long)(d - c));
		fflush(stdout);

		/* Pacing permits concurrent HID input; it is outside operation samples. */
		if (sample + 1U < IMOD_SAMPLES)
			usleep(250000);
	}

	stats_size = sizeof(irq_after);
	if (sysctlbyname("vfs.io.stats", &irq_after, &stats_size, NULL, 0) != 0 ||
	    monotonic_ns(&irq_end) != 0 || stats_size != sizeof(irq_after) ||
	    irq_after.version != IO_STATS_VERSION || irq_after.count != IO_STAT_COUNT ||
	    irq_end <= irq_start)
		goto failed;
	for (index = IO_XHCI_IRQ_ENTRY; index <= IO_XHCI_IRQ_EVENT; index++) {
		if (irq_after.events[index].calls < irq_before.events[index].calls)
			goto failed;
	}
	printf("IMOD-STORAGE IRQ entry=%llu owned=%llu events=%llu duration_ns=%llu\n",
	    (unsigned long long)(irq_after.events[IO_XHCI_IRQ_ENTRY].calls - irq_before.events[IO_XHCI_IRQ_ENTRY].calls),
	    (unsigned long long)(irq_after.events[IO_XHCI_IRQ_OWNED].calls - irq_before.events[IO_XHCI_IRQ_OWNED].calls),
	    (unsigned long long)(irq_after.events[IO_XHCI_IRQ_EVENT].calls - irq_before.events[IO_XHCI_IRQ_EVENT].calls),
	    (unsigned long long)(irq_end - irq_start));

	/* Reports scheduler-accounted process CPU separately from paced wall time. */
	error = monotonic_ns(&end);
	if (error != 0 || end < start)
		goto failed;
	error = getrusage(RUSAGE_SELF, &after);
	if (error != 0)
		goto failed;
	if (cpu_us(&after) < cpu_us(&before))
		goto failed;
	error = close(descriptor);
	if (error != 0)
		goto failed_open;
	error = unlink(path);
	if (error != 0)
		goto failed_open;
	printf("IMOD-STORAGE PASS samples=%u bytes=%u wall_ns=%llu process_cpu_us=%llu\n",
	    IMOD_SAMPLES, IMOD_BYTES,
	    (unsigned long long)(end - start),
	    (unsigned long long)(cpu_us(&after) - cpu_us(&before)));
	fflush(stdout);
	return 0;

failed:
	/* Retains the file for diagnosis, but never leaves its descriptor owned. */
	close(descriptor);
failed_open:
	fprintf(stderr, "IMOD-STORAGE FAIL errno=%d\n", errno);
	return 1;
}

/* Reads the guest's monotonic clock at an operation boundary. */
static int
monotonic_ns(
	uint64_t *result)
{
	struct timespec now;
	int error;

	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return error;
	*result = (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
	return 0;
}

/* Adds the tick-accounted user and system CPU totals for this process. */
static uint64_t
cpu_us(
	const struct rusage *usage)
{
	return ((uint64_t)usage->ru_utime.tv_sec + usage->ru_stime.tv_sec) *
	    UINT64_C(1000000) + usage->ru_utime.tv_usec + usage->ru_stime.tv_usec;
}
