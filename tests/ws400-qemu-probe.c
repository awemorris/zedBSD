/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Guest-side probe for USB Audio Class (/dev/dspN and /dev/mixerN).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <uapi/audio.h>

int
main(
	int argc,
	char **argv)
{
	const char *dsp_path;
	const char *mixer_path;
	int hold_sec;
	int mfd;
	int dfd;
	struct audio_volume vol;
	struct audio_format fmt;
	struct audio_buffer_info bufinfo;
	uint8_t buf[4096];
	ssize_t n;
	int i;

	dsp_path = (argc > 1) ? argv[1] : "/dev/dsp0";
	mixer_path = (argc > 2) ? argv[2] : "/dev/mixer0";
	hold_sec = (argc > 3) ? atoi(argv[3]) : 0;

	mfd = open(mixer_path, O_RDWR);
	if (mfd < 0) {
		perror("open mixer");
		return 1;
	}

	memset(&vol, 0, sizeof(vol));
	if (ioctl(mfd, KERN_AUDIO_GET_VOLUME, &vol) < 0) {
		perror("GET_VOLUME");
		close(mfd);
		return 1;
	}
	printf("mixer initial: left=%u right=%u muted=%u\n",
	    vol.left, vol.right, vol.muted);

	vol.left = 75U;
	vol.right = 75U;
	vol.muted = 0U;
	if (ioctl(mfd, KERN_AUDIO_SET_VOLUME, &vol) < 0) {
		perror("SET_VOLUME");
		close(mfd);
		return 1;
	}

	memset(&vol, 0, sizeof(vol));
	if (ioctl(mfd, KERN_AUDIO_GET_VOLUME, &vol) < 0) {
		perror("GET_VOLUME after set");
		close(mfd);
		return 1;
	}
	printf("mixer after set: left=%u right=%u muted=%u\n",
	    vol.left, vol.right, vol.muted);
	close(mfd);

	dfd = open(dsp_path, O_WRONLY);
	if (dfd < 0) {
		perror("open dsp");
		return 1;
	}

	memset(&fmt, 0, sizeof(fmt));
	if (ioctl(dfd, KERN_AUDIO_GET_FORMAT, &fmt) < 0) {
		perror("GET_FORMAT");
		close(dfd);
		return 1;
	}
	printf("dsp format: fmt=%u ch=%u rate=%u\n",
	    fmt.format, fmt.channels, fmt.rate);

	memset(&bufinfo, 0, sizeof(bufinfo));
	if (ioctl(dfd, KERN_AUDIO_GET_BUFFER, &bufinfo) < 0) {
		perror("GET_BUFFER");
		close(dfd);
		return 1;
	}
	printf("dsp buffer: frag_bytes=%u frag_count=%u bpf=%u\n",
	    bufinfo.fragment_bytes, bufinfo.fragment_count, bufinfo.bytes_per_frame);

	memset(buf, 0, sizeof(buf));
	for (i = 0; i < 4; i++) {
		n = write(dfd, buf, sizeof(buf));
		if (n < 0) {
			perror("write dsp");
			close(dfd);
			return 1;
		}
	}
	printf("dsp wrote 4 fragments\n");

	if (hold_sec > 0) {
		printf("holding dsp open for %d s\n", hold_sec);
		fflush(stdout);
		sleep((unsigned)hold_sec);
	}

	close(dfd);
	printf("ws400-qemu-probe: PASS\n");
	return 0;
}
