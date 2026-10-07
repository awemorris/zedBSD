/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * keiland-preview (WS168 p003, plan/ws168/phase001/phase.md section 4):
 * makes the preview (a small picture) of a picture or a PDF document, in
 * a process that can do nothing but compute.  The caller opens the input
 * file as fd 0 and the output file as fd 1 and starts the program in a
 * sandbox (zedBSD's sandbox_spawn: no other descriptor, no path, no
 * network, no new process); on Linux the program confines itself with
 * seccomp before it reads a byte (confine.c of each system's directory).
 *
 *   keiland-preview --width=N --height=N [--fit=contain|cover] [--stamp=TEXT]
 *
 * The input is read whole (64 MiB at most), its kind told by its first
 * bytes (PNG, JPEG, GIF, PPM and PGM, PDF's first page), decoded and
 * scaled: within the size, its shape kept and never made larger
 * (contain), or filling the size and cut to it (cover).  The output is a
 * binary PPM (P6), its comment "# TEXT" when a stamp is given, written to
 * fd 1 with write(2) (the C library's stdio is not used: its first use
 * asks whether the descriptor is a terminal).
 */

#ifndef KEILAND_PREVIEW_H
#define KEILAND_PREVIEW_H

#include <stddef.h>
#include <stdint.h>

/* The exit statuses (section 4.1). */
#define PREVIEW_OK		0
#define PREVIEW_UNKNOWN		1	/* not a kind the program reads */
#define PREVIEW_DAMAGED		2	/* the kind's, but it cannot be decoded */
#define PREVIEW_TOO_LARGE	3	/* the input or the picture is over the limits */
#define PREVIEW_NO_MEMORY	4
#define PREVIEW_NO_OUTPUT	5	/* the output cannot be written */
#define PREVIEW_USAGE		64	/* the arguments are wrong */
#define PREVIEW_NO_SANDBOX	70	/* the system's confinement could not be entered (Linux, FreeBSD) */

/* The largest input read, the largest side asked for, and the largest picture decoded. */
#define PREVIEW_INPUT_MAX	(64UL * 1024UL * 1024UL)
#define PREVIEW_SIDE_MAX	4096
#define PREVIEW_DECODE_SIDE	8192U
#define PREVIEW_DECODE_PIXELS	(16UL * 1024UL * 1024UL)

/* The longest stamp, with its NUL. */
#define PREVIEW_STAMP_MAX	256U

/* A picture: premultiplied 0xAARRGGBB pixels, width by height, no padding (allocated). */
struct preview_image {
	uint32_t *pixels;
	int width;
	int height;
};

/*
 * A test build (PREVIEW_TEST_ESCAPE 1, never installed) takes
 * --test-escape=open|socket|fork: after the confinement and the reading
 * of the input it tries the call, as code planted in a file would, so the
 * tests see the confinement end the process (section 6).
 */
#ifndef PREVIEW_TEST_ESCAPE
#define PREVIEW_TEST_ESCAPE	0
#endif

/* The calls a test build tries. */
#define PREVIEW_ESCAPE_NONE	0
#define PREVIEW_ESCAPE_OPEN	1
#define PREVIEW_ESCAPE_SOCKET	2
#define PREVIEW_ESCAPE_FORK	3

/* What is asked: the size, cover (else contain), the stamp (empty for none) and a test build's escape. */
struct preview_request {
	int width;
	int height;
	int cover;
	char stamp[PREVIEW_STAMP_MAX];
	int escape;
};

/* The decoding (decode.c). */
int preview_decode(const unsigned char *data, size_t size, const struct preview_request *request, struct preview_image *image);

/* The scaling (scale.c). */
void preview_fit(int width, int height, const struct preview_request *request, int *fitted_width, int *fitted_height);
int preview_scale(const struct preview_image *source, const struct preview_request *request, struct preview_image *scaled);
void preview_image_release(struct preview_image *image);

/* The making and the output (make.c). */
int preview_make(int input, int output, const struct preview_request *request);
int preview_write_ppm(int fd, const struct preview_image *image, const char *stamp);

/* The substitute fonts carried in the program (fonts.c, ws177-p010). */
int preview_fonts_register(void);

/* The system's confinement (each system's confine.c). */
int preview_confine(void);

#endif
