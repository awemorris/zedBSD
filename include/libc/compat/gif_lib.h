/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libgif-compat: zedBSD's own implementation of giflib's high-level
 * decoding interface for its base programs (plan/ws074/phase051).  It
 * holds no giflib code; the names and values are giflib 5.2's.  The
 * structures are written here with giflib's field names and are not
 * promised to be laid out like the real ones: only programs built against
 * this header use the library (its SONAME is libgif-compat.so).  Programs
 * of the packages use the real giflib (/usr/include/gif_lib.h); base
 * programs include <compat/gif_lib.h>.
 *
 * The subset is what a program needs to read a whole file: DGifOpen (a
 * reading callback), DGifOpenFileName and DGifOpenFileHandle, DGifSlurp
 * (every image, its raster de-interlaced into rows top to bottom, and the
 * extension blocks), DGifSavedExtensionToGCB, DGifCloseFile and
 * GifErrorString.  The record-by-record calls (DGifGetRecordType,
 * DGifGetLine...) and encoding (EGif*) are not provided.
 *
 * One difference from giflib: when an image's data is cut short,
 * DGifSlurp reports the same error but keeps the image (counted in
 * ImageCount) with the pixels decoded so far and the rest at index 0, so
 * that a program can show a file that is still arriving or damaged.
 */

#ifndef KERN_COMPAT_GIF_LIB_H
#define KERN_COMPAT_GIF_LIB_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The interface's version: giflib 5.2's. */
#define GIFLIB_MAJOR			5
#define GIFLIB_MINOR			2
#define GIFLIB_RELEASE			2

/* What the calls report. */
#define GIF_ERROR			0
#define GIF_OK				1

/* The byte and index types. */
typedef unsigned char GifPixelType;
typedef unsigned char *GifRowType;
typedef unsigned char GifByteType;
typedef unsigned int GifPrefixType;
typedef int GifWord;

/* One colour of a colour map. */
typedef struct GifColorType {
	GifByteType Red;
	GifByteType Green;
	GifByteType Blue;
} GifColorType;

/*
 * A colour map (the global one of the screen, or an image's local one):
 * ColorCount colours, 2 to the power BitsPerPixel.
 */
typedef struct ColorMapObject {
	int ColorCount;
	int BitsPerPixel;
	bool SortFlag;
	GifColorType *Colors;
} ColorMapObject;

/*
 * Where an image sits on the screen, whether its rows came interlaced,
 * and its local colour map (NULL when it uses the global one).
 */
typedef struct GifImageDesc {
	GifWord Left;
	GifWord Top;
	GifWord Width;
	GifWord Height;
	bool Interlace;
	ColorMapObject *ColorMap;
} GifImageDesc;

/*
 * One sub-block of an extension: the first carries the extension's
 * Function code, the ones after it CONTINUE_EXT_FUNC_CODE.
 */
typedef struct ExtensionBlock {
	int ByteCount;
	GifByteType *Bytes;
	int Function;
} ExtensionBlock;

/* The extension codes. */
#define CONTINUE_EXT_FUNC_CODE		0x00
#define COMMENT_EXT_FUNC_CODE		0xfe
#define GRAPHICS_EXT_FUNC_CODE		0xf9
#define PLAINTEXT_EXT_FUNC_CODE		0x01
#define APPLICATION_EXT_FUNC_CODE	0xff

/*
 * One image read by DGifSlurp: its description, its colour indexes
 * (Width by Height, rows top to bottom), and the extensions before it.
 */
typedef struct SavedImage {
	GifImageDesc ImageDesc;
	GifByteType *RasterBits;
	int ExtensionBlockCount;
	ExtensionBlock *ExtensionBlocks;
} SavedImage;

/*
 * An open GIF file: the screen (its size, colour resolution, background
 * and global colour map), the images read so far, the extensions after
 * the last image, the last error, and the program's own pointer.
 */
typedef struct GifFileType {
	GifWord SWidth;
	GifWord SHeight;
	GifWord SColorResolution;
	GifWord SBackGroundColor;
	GifByteType AspectByte;
	ColorMapObject *SColorMap;
	int ImageCount;
	GifImageDesc Image;
	SavedImage *SavedImages;
	int ExtensionBlockCount;
	ExtensionBlock *ExtensionBlocks;
	int Error;
	void *UserData;
	void *Private;
} GifFileType;

/* A program's reading callback: up to the count of bytes into the buffer, returning how many it gave. */
typedef int (*InputFunc)(GifFileType *, GifByteType *, int);

/*
 * What a graphic control extension says of the image after it: how it
 * is disposed of, whether it waits for the user, how long it shows (in
 * 1/100 s), and its transparent colour index (NO_TRANSPARENT_COLOR for
 * none).
 */
typedef struct GraphicsControlBlock {
	int DisposalMode;
	bool UserInputFlag;
	int DelayTime;
	int TransparentColor;
} GraphicsControlBlock;

/* The disposal modes, and the transparent colour of an image that has none. */
#define DISPOSAL_UNSPECIFIED		0
#define DISPOSE_DO_NOT			1
#define DISPOSE_BACKGROUND		2
#define DISPOSE_PREVIOUS		3
#define NO_TRANSPARENT_COLOR		-1

/* The errors (giflib's codes). */
#define D_GIF_SUCCEEDED			0
#define D_GIF_ERR_OPEN_FAILED		101
#define D_GIF_ERR_READ_FAILED		102
#define D_GIF_ERR_NOT_GIF_FILE		103
#define D_GIF_ERR_NO_SCRN_DSCR		104
#define D_GIF_ERR_NO_IMAG_DSCR		105
#define D_GIF_ERR_NO_COLOR_MAP		106
#define D_GIF_ERR_WRONG_RECORD		107
#define D_GIF_ERR_DATA_TOO_BIG		108
#define D_GIF_ERR_NOT_ENOUGH_MEM	109
#define D_GIF_ERR_CLOSE_FAILED		110
#define D_GIF_ERR_NOT_READABLE		111
#define D_GIF_ERR_IMAGE_DEFECT		112
#define D_GIF_ERR_EOF_TOO_SOON		113

/* Opens a GIF file (its screen descriptor and global colour map are read); NULL with *Error on failure. */
GifFileType *DGifOpenFileName(const char *GifFileName, int *Error);
GifFileType *DGifOpenFileHandle(int GifFileHandle, int *Error);
GifFileType *DGifOpen(void *userPtr, InputFunc readFunc, int *Error);

/* Reads every image and extension to the end of the file. */
int DGifSlurp(GifFileType *GifFile);

/* Tells what the graphic control extension before an image says. */
int DGifSavedExtensionToGCB(GifFileType *GifFile, int ImageIndex, GraphicsControlBlock *GCB);

/* Closes the file and frees everything it holds. */
int DGifCloseFile(GifFileType *GifFile, int *ErrorCode);

/* Describes an error code. */
const char *GifErrorString(int ErrorCode);

#ifdef __cplusplus
}
#endif

#endif
