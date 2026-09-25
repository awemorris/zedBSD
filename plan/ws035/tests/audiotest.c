/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Guest test for /dev/dsp0 and /dev/mixer0 (ws035-p007).
 *
 *   audiotest info
 *   audiotest play FRAMES        counting pattern: left = n, right = ~n
 *   audiotest tone FRAMES AMP    square wave of +-AMP, 100 frames per half
 *   audiotest capture FRAMES     reads FRAMES and reports the time taken
 *   audiotest volume L R MUTED   sets the mixer and reads it back
 *   audiotest mmapplay FRAMES    the counting pattern through the mmap interface
 *   audiotest mmapcapture FRAMES captures FRAMES through the mmap interface
 *
 * Every mode prints one "AUDIOTEST ..." result line for the host script.
 */

#include <uapi/audio.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int
fail(const char *what)
{
	printf("AUDIOTEST FAIL %s errno=%d\n", what, errno);
	return 1;
}

static long
elapsed_ms(const struct timespec *start)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - start->tv_sec) * 1000L +
	    (now.tv_nsec - start->tv_nsec) / 1000000L;
}

static int
info(void)
{
	struct audio_format format;
	struct audio_buffer_info buffer;
	struct audio_volume volume;
	int dsp;
	int mixer;

	dsp = open("/dev/dsp0", O_WRONLY);
	if (dsp < 0)
		return fail("open-dsp");
	if (ioctl(dsp, KERN_AUDIO_GET_FORMAT, &format) != 0)
		return fail("get-format");
	if (ioctl(dsp, KERN_AUDIO_GET_BUFFER, &buffer) != 0)
		return fail("get-buffer");
	close(dsp);

	mixer = open("/dev/mixer0", O_RDONLY);
	if (mixer < 0)
		return fail("open-mixer");
	if (ioctl(mixer, KERN_AUDIO_GET_VOLUME, &volume) != 0)
		return fail("get-volume");
	close(mixer);

	printf("AUDIOTEST INFO format=%u channels=%u rate=%u fragment=%u count=%u frame=%u volume=%u/%u/%u\n",
	    format.format, format.channels, format.rate,
	    buffer.fragment_bytes, buffer.fragment_count, buffer.bytes_per_frame,
	    volume.left, volume.right, volume.muted);
	return 0;
}

/* Writes frames made by make() in chunks, then drains. */
static int
play(unsigned long frames, int tone, int amplitude)
{
	static int16_t chunk[2048 * 2];
	struct audio_space space;
	struct timespec start;
	unsigned long done;
	unsigned long count;
	unsigned long index;
	unsigned long n;
	ssize_t written;
	int dsp;

	dsp = open("/dev/dsp0", O_WRONLY);
	if (dsp < 0)
		return fail("open-dsp");

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (done = 0; done < frames; done += count) {
		count = frames - done;
		if (count > 2048)
			count = 2048;
		for (index = 0; index < count; index++) {
			n = done + index;
			if (tone) {
				chunk[index * 2] = (int16_t)((n / 100) % 2 ? -amplitude : amplitude);
				chunk[index * 2 + 1] = chunk[index * 2];
			} else {
				chunk[index * 2] = (int16_t)(uint16_t)n;
				chunk[index * 2 + 1] = (int16_t)(uint16_t)~n;
			}
		}

		written = write(dsp, chunk, count * 4);
		if (written != (ssize_t)(count * 4))
			return fail("write");
	}

	if (ioctl(dsp, KERN_AUDIO_DRAIN, NULL) != 0)
		return fail("drain");
	if (ioctl(dsp, KERN_AUDIO_GET_OSPACE, &space) != 0)
		return fail("ospace");

	printf("AUDIOTEST PLAY frames=%lu transferred=%llu underruns=%u ms=%ld\n",
	    frames, (unsigned long long)space.transferred, space.underruns,
	    elapsed_ms(&start));
	close(dsp);
	return 0;
}

static int
capture(unsigned long frames)
{
	static uint8_t chunk[16384];
	struct audio_space space;
	struct timespec start;
	unsigned long long total;
	unsigned long long wanted;
	ssize_t got;
	size_t size;
	int dsp;

	dsp = open("/dev/dsp0", O_RDONLY);
	if (dsp < 0)
		return fail("open-dsp");

	clock_gettime(CLOCK_MONOTONIC, &start);
	wanted = (unsigned long long)frames * 4;
	for (total = 0; total < wanted; total += (unsigned long long)got) {
		size = sizeof(chunk);
		if (wanted - total < size)
			size = (size_t)(wanted - total);
		got = read(dsp, chunk, size);
		if (got <= 0)
			return fail("read");
	}

	if (ioctl(dsp, KERN_AUDIO_GET_ISPACE, &space) != 0)
		return fail("ispace");

	printf("AUDIOTEST CAPTURE bytes=%llu ms=%ld overruns=%u\n",
	    total, elapsed_ms(&start), space.overruns);
	close(dsp);
	return 0;
}

static int
volume(unsigned left, unsigned right, unsigned muted)
{
	struct audio_volume set;
	struct audio_volume got;
	int mixer;
	int error;

	mixer = open("/dev/mixer0", O_RDWR);
	if (mixer < 0)
		return fail("open-mixer");

	set.left = left;
	set.right = right;
	set.muted = muted;
	set.reserved = 0;
	error = ioctl(mixer, KERN_AUDIO_SET_VOLUME, &set);
	if (error != 0) {
		printf("AUDIOTEST VOLUME set-errno=%d\n", errno);
		close(mixer);
		return 0;
	}

	if (ioctl(mixer, KERN_AUDIO_GET_VOLUME, &got) != 0)
		return fail("get-volume");

	printf("AUDIOTEST VOLUME %u/%u/%u\n", got.left, got.right, got.muted);
	close(mixer);
	return 0;
}

/* Writes the counting pattern for byte positions [from, to) of the stream, silence past the end. */
static void
mmap_fill(uint8_t *ring, uint32_t ring_bytes, uint64_t from, uint64_t to,
    uint64_t end)
{
	uint64_t position;
	uint32_t n;
	int16_t sample[2];

	for (position = from; position < to; position += 4) {
		if (position < end) {
			n = (uint32_t)(position / 4);
			sample[0] = (int16_t)(uint16_t)n;
			sample[1] = (int16_t)(uint16_t)~n;
		} else {
			sample[0] = 0;
			sample[1] = 0;
		}
		memcpy(ring + position % ring_bytes, sample, sizeof(sample));
	}
}

/*
 * Plays the counting pattern through the mapping: fills half a ring ahead
 * of the frontier, waits a fragment with poll, and stops once the frontier
 * is past the end by the lead and a two-fragment tail.
 */
static int
mmap_play(unsigned long frames)
{
	struct audio_caps caps;
	struct audio_buffer_info buffer;
	struct audio_mmap_position position;
	struct audio_space space;
	struct timespec start;
	struct pollfd descriptor;
	uint64_t filled;
	uint64_t end;
	uint32_t bits;
	uint8_t *ring;
	unsigned polls;
	int dsp;

	/* A write-only device open cannot be mapped; PROT_WRITE picks playback. */
	dsp = open("/dev/dsp0", O_RDWR);
	if (dsp < 0)
		return fail("open-dsp");
	if (ioctl(dsp, KERN_AUDIO_GET_CAPS, &caps) != 0)
		return fail("get-caps");
	if ((caps.caps & KERN_AUDIO_CAP_MMAP) == 0)
		return fail("no-mmap");
	if (ioctl(dsp, KERN_AUDIO_GET_BUFFER, &buffer) != 0)
		return fail("get-buffer");
	ring = mmap(NULL, caps.mmap_bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
	    dsp, 0);
	if (ring == MAP_FAILED)
		return fail("mmap");

	end = (uint64_t)frames * 4;
	filled = caps.mmap_bytes / 2;
	mmap_fill(ring, caps.mmap_bytes, 0, filled, end);

	bits = KERN_AUDIO_TRIGGER_OUTPUT;
	if (ioctl(dsp, KERN_AUDIO_SET_TRIGGER, &bits) != 0)
		return fail("trigger");
	clock_gettime(CLOCK_MONOTONIC, &start);

	polls = 0;
	for (;;) {
		descriptor.fd = dsp;
		descriptor.events = POLLOUT;
		descriptor.revents = 0;
		if (poll(&descriptor, 1, 1000) <= 0)
			return fail("poll");
		polls++;
		if (ioctl(dsp, KERN_AUDIO_GET_OPTR, &position) != 0)
			return fail("get-optr");
		if (position.bytes >= end + caps.mmap_lead_bytes +
		    2U * buffer.fragment_bytes)
			break;
		if (position.bytes + caps.mmap_bytes / 2 > filled) {
			mmap_fill(ring, caps.mmap_bytes, filled,
			    position.bytes + caps.mmap_bytes / 2, end);
			filled = position.bytes + caps.mmap_bytes / 2;
		}
	}

	bits = 0;
	if (ioctl(dsp, KERN_AUDIO_SET_TRIGGER, &bits) != 0)
		return fail("untrigger");
	if (write(dsp, ring, 4) != -1 || errno != EBUSY)
		return fail("write-not-refused");
	if (ioctl(dsp, KERN_AUDIO_GET_OSPACE, &space) != 0)
		return fail("ospace");

	printf("AUDIOTEST MMAPPLAY frames=%lu frontier=%llu polls=%u ms=%ld lead=%u copy=%u\n",
	    frames, (unsigned long long)position.bytes, polls, elapsed_ms(&start),
	    caps.mmap_lead_bytes, (caps.caps & KERN_AUDIO_CAP_MMAP_COPY) != 0);
	munmap(ring, caps.mmap_bytes);
	close(dsp);
	return 0;
}

/* Captures through the mapping until FRAMES have arrived, and reports the time. */
static int
mmap_capture(unsigned long frames)
{
	struct audio_caps caps;
	struct audio_mmap_position position;
	struct timespec start;
	struct pollfd descriptor;
	uint32_t bits;
	uint8_t *ring;
	unsigned fragments;
	int dsp;

	dsp = open("/dev/dsp0", O_RDONLY);
	if (dsp < 0)
		return fail("open-dsp");
	if (ioctl(dsp, KERN_AUDIO_GET_CAPS, &caps) != 0)
		return fail("get-caps");
	ring = mmap(NULL, caps.mmap_bytes, PROT_READ, MAP_SHARED, dsp, 0);
	if (ring == MAP_FAILED)
		return fail("mmap");

	bits = KERN_AUDIO_TRIGGER_INPUT;
	if (ioctl(dsp, KERN_AUDIO_SET_TRIGGER, &bits) != 0)
		return fail("trigger");
	clock_gettime(CLOCK_MONOTONIC, &start);

	fragments = 0;
	do {
		descriptor.fd = dsp;
		descriptor.events = POLLIN;
		descriptor.revents = 0;
		if (poll(&descriptor, 1, 1000) <= 0)
			return fail("poll");
		if (ioctl(dsp, KERN_AUDIO_GET_IPTR, &position) != 0)
			return fail("get-iptr");
		fragments += position.fragments;
	} while (position.bytes < (uint64_t)frames * 4);

	printf("AUDIOTEST MMAPCAPTURE bytes=%llu fragments=%u ms=%ld\n",
	    (unsigned long long)position.bytes, fragments, elapsed_ms(&start));
	munmap(ring, caps.mmap_bytes);
	close(dsp);
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "info") == 0)
		return info();
	if (argc >= 3 && strcmp(argv[1], "play") == 0)
		return play(strtoul(argv[2], NULL, 10), 0, 0);
	if (argc >= 4 && strcmp(argv[1], "tone") == 0)
		return play(strtoul(argv[2], NULL, 10), 1, atoi(argv[3]));
	if (argc >= 3 && strcmp(argv[1], "capture") == 0)
		return capture(strtoul(argv[2], NULL, 10));
	if (argc >= 3 && strcmp(argv[1], "mmapplay") == 0)
		return mmap_play(strtoul(argv[2], NULL, 10));
	if (argc >= 3 && strcmp(argv[1], "mmapcapture") == 0)
		return mmap_capture(strtoul(argv[2], NULL, 10));
	if (argc >= 5 && strcmp(argv[1], "volume") == 0)
		return volume((unsigned)atoi(argv[2]), (unsigned)atoi(argv[3]), (unsigned)atoi(argv[4]));

	fprintf(stderr, "usage: audiotest info|play N|tone N AMP|capture N|volume L R M|mmapplay N|mmapcapture N\n");
	return 2;
}
