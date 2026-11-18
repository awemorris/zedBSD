/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libz-compat: zedBSD's own implementation of the parts of zlib's
 * interface its base programs use (plan/ws035/ws.md, D1 to D6).  It holds
 * no zlib code; the names and values are zlib's so that a program written
 * for zlib reads the same.  Programs of the packages use the real zlib
 * (/usr/include/zlib.h); base programs include <compat/zlib/zlib.h>.
 *
 * It decompresses (inflate, uncompress), compresses (deflate, compress,
 * compress2; ws175-p006) and checksums (adler32, crc32).  inflate reads
 * zlib streams (RFC 1950) and raw deflate (RFC 1951, windowBits
 * negative), and deflate writes them.  Each keeps the input it is given
 * until the stream is whole, then hands out the output as there is room
 * for it.
 */

#ifndef KERN_COMPAT_ZLIB_H
#define KERN_COMPAT_ZLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version zlib's interface is named by (D1). */
#define ZLIB_VERSION		"1.3.2"
#define ZLIB_VERNUM		0x1320

/* The kinds zlib names its types by. */
typedef unsigned char Bytef;
typedef unsigned int uInt;
typedef unsigned long uLong;
typedef unsigned long uLongf;
typedef void *voidpf;
typedef const void *voidpc;
typedef voidpf (*alloc_func)(voidpf opaque, uInt items, uInt size);
typedef void (*free_func)(voidpf opaque, voidpf address);

/*
 * A stream: where the input is and how much is left, where the output
 * goes and how much room is left, the totals, the last message, the
 * implementation's state, and the caller's allocator (unused here: the
 * state is allocated with malloc).
 */
typedef struct z_stream_s {
	const Bytef *next_in;
	uInt avail_in;
	uLong total_in;
	Bytef *next_out;
	uInt avail_out;
	uLong total_out;
	const char *msg;
	struct internal_state *state;
	alloc_func zalloc;
	free_func zfree;
	voidpf opaque;
	int data_type;
	uLong adler;
	uLong reserved;
} z_stream;

/* A pointer to a stream, as zlib's functions take it. */
typedef z_stream *z_streamp;

/* The flush values. */
#define Z_NO_FLUSH		0
#define Z_PARTIAL_FLUSH		1
#define Z_SYNC_FLUSH		2
#define Z_FULL_FLUSH		3
#define Z_FINISH		4
#define Z_BLOCK			5
#define Z_TREES			6

/* The return values. */
#define Z_OK			0
#define Z_STREAM_END		1
#define Z_NEED_DICT		2
#define Z_ERRNO			(-1)
#define Z_STREAM_ERROR		(-2)
#define Z_DATA_ERROR		(-3)
#define Z_MEM_ERROR		(-4)
#define Z_BUF_ERROR		(-5)
#define Z_VERSION_ERROR		(-6)

/* The compression levels, strategy and method (for ws035-p040's deflate), and a null pointer. */
#define Z_NO_COMPRESSION	0
#define Z_BEST_SPEED		1
#define Z_BEST_COMPRESSION	9
#define Z_DEFAULT_COMPRESSION	(-1)
#define Z_DEFAULT_STRATEGY	0
#define Z_DEFLATED		8
#define Z_NULL			0
#define MAX_WBITS		15

/* The version of the library that was loaded. */
const char *zlibVersion(void);

/* Decompression. */
int inflateInit_(z_streamp strm, const char *version, int stream_size);
int inflateInit2_(z_streamp strm, int windowBits, const char *version, int stream_size);
int inflate(z_streamp strm, int flush);
int inflateEnd(z_streamp strm);
int inflateReset(z_streamp strm);
int uncompress(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen);

/* The compression (ws175-p006): a zlib wrapper or raw deflate; the input is taken whole and compressed at Z_FINISH. */
int deflateInit_(z_streamp strm, int level, const char *version, int stream_size);
int deflateInit2_(z_streamp strm, int level, int method, int windowBits, int memLevel, int strategy, const char *version, int stream_size);
int deflate(z_streamp strm, int flush);
int deflateEnd(z_streamp strm);
uLong compressBound(uLong sourceLen);
int compress(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen);
int compress2(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen, int level);

/* The checksums; a null buffer gives the starting value. */
uLong adler32(uLong adler, const Bytef *buf, uInt len);
uLong crc32(uLong crc, const Bytef *buf, uInt len);

/* The initializers as zlib's header spells them. */
#define inflateInit(strm)		inflateInit_((strm), ZLIB_VERSION, (int)sizeof(z_stream))
#define inflateInit2(strm, windowBits)	inflateInit2_((strm), (windowBits), ZLIB_VERSION, (int)sizeof(z_stream))
#define deflateInit(strm, level)	deflateInit_((strm), (level), ZLIB_VERSION, (int)sizeof(z_stream))
#define deflateInit2(strm, level, method, windowBits, memLevel, strategy) \
	deflateInit2_((strm), (level), (method), (windowBits), (memLevel), (strategy), ZLIB_VERSION, (int)sizeof(z_stream))

#ifdef __cplusplus
}
#endif

#endif
