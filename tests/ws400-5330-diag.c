/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Non-blocking diagnostic probe for /dev/dsp1 and /dev/mixer1 on 5330.
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
	void)
{
	int mfd;
	int dfd;
	struct audio_volume vol;
	struct audio_format fmt;
	struct audio_buffer_info info;
	struct audio_space sp;
	uint8_t buf[4096];
	ssize_t n;
	int i;

	alarm(10);

	mfd = open("/dev/mixer1", O_RDWR);
	if (mfd < 0) {
		perror("open /dev/mixer1");
	} else {
		memset(&vol, 0, sizeof(vol));
		if (ioctl(mfd, KERN_AUDIO_GET_VOLUME, &vol) < 0)
			perror("GET_VOLUME");
		else
			printf("mixer1 get: left=%u right=%u muted=%u\n", vol.left, vol.right, vol.muted);

		vol.left = 80U;
		vol.right = 80U;
		vol.muted = 0U;
		if (ioctl(mfd, KERN_AUDIO_SET_VOLUME, &vol) < 0)
			perror("SET_VOLUME");
		close(mfd);
	}
	fflush(stdout);

	dfd = open("/dev/dsp1", O_RDWR | O_NONBLOCK);
	if (dfd < 0) {
		perror("open /dev/dsp1");
		return 1;
	}

	memset(&fmt, 0, sizeof(fmt));
	if (ioctl(dfd, KERN_AUDIO_GET_FORMAT, &fmt) < 0)
		perror("GET_FORMAT");
	else
		printf("dsp1 default fmt: format=%u ch=%u rate=%u\n", fmt.format, fmt.channels, fmt.rate);

	memset(&fmt, 0, sizeof(fmt));
	fmt.format = KERN_AUDIO_FORMAT_S16_LE;
	fmt.channels = 2U;
	fmt.rate = 48000U;
	if (ioctl(dfd, KERN_AUDIO_SET_FORMAT, &fmt) < 0)
		printf("SET_FORMAT S16_LE 48k failed: %s\n", strerror(errno));

	memset(&fmt, 0, sizeof(fmt));
	(void)ioctl(dfd, KERN_AUDIO_GET_FORMAT, &fmt);
	printf("dsp1 active fmt: format=%u ch=%u rate=%u\n", fmt.format, fmt.channels, fmt.rate);

	memset(&info, 0, sizeof(info));
	if (ioctl(dfd, KERN_AUDIO_GET_BUFFER, &info) < 0)
		perror("GET_BUFFER");
	else
		printf("dsp1 buffer: frag_bytes=%u frag_count=%u bpf=%u\n",
		    info.fragment_bytes, info.fragment_count, info.bytes_per_frame);

	memset(&sp, 0, sizeof(sp));
	if (ioctl(dfd, KERN_AUDIO_GET_OSPACE, &sp) < 0)
		perror("GET_OSPACE before");
	else
		printf("ospace before: bytes=%u transferred=%llu underruns=%u\n",
		    sp.bytes, (unsigned long long)sp.transferred, sp.underruns);
	fflush(stdout);

	memset(buf, 0, sizeof(buf));
	for (i = 0; i < 8; i++) {
		n = write(dfd, buf, sizeof(buf));
		printf("write %d: n=%zd errno=%d\n", i, n, (n < 0) ? errno : 0);
		fflush(stdout);
		if (n < 0)
			break;
	}

	usleep(200000);
	memset(&sp, 0, sizeof(sp));
	if (ioctl(dfd, KERN_AUDIO_GET_OSPACE, &sp) < 0)
		perror("GET_OSPACE after");
	else
		printf("ospace after 200ms: bytes=%u transferred=%llu underruns=%u\n",
		    sp.bytes, (unsigned long long)sp.transferred, sp.underruns);
	fflush(stdout);

	close(dfd);
	return 0;
}
