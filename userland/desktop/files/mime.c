/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The types of files: a MIME name, what the window calls the kind, and the
 * category that decides its icon and how it opens.
 *
 * The type comes from the name's extension; a file with no known
 * extension is an executable when its mode says so and a plain document
 * otherwise.  Looking into a file's first bytes (fm_mime_sniff) is done
 * only where a file is opened or previewed, so listing a large folder
 * reads no file's contents.
 */

#include "files.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The longest extension looked up. */
#define MIME_EXTENSION_MAX	16

/*
 * One extension and the type it names.
 */
struct mime_extension {
	const char *extension;
	struct fm_mime mime;
};

/* The type of a folder. */
static const struct fm_mime mime_folder = { "inode/directory", "Folder", FM_CATEGORY_FOLDER };

/* The type of a file with no known extension. */
static const struct fm_mime mime_unknown = { "application/octet-stream", "Document", FM_CATEGORY_FILE };

/* The type of a file whose mode lets it run. */
static const struct fm_mime mime_executable = { "application/x-executable", "Program", FM_CATEGORY_EXECUTABLE };

/* The types of the contents recognized by their first bytes. */
static const struct fm_mime mime_text = { "text/plain", "Plain Text", FM_CATEGORY_TEXT };
static const struct fm_mime mime_elf = { "application/x-executable", "Program", FM_CATEGORY_EXECUTABLE };
static const struct fm_mime mime_script = { "application/x-shellscript", "Script", FM_CATEGORY_CODE };
static const struct fm_mime mime_png = { "image/png", "PNG Image", FM_CATEGORY_IMAGE };
static const struct fm_mime mime_jpeg = { "image/jpeg", "JPEG Image", FM_CATEGORY_IMAGE };
static const struct fm_mime mime_gif = { "image/gif", "GIF Image", FM_CATEGORY_IMAGE };
static const struct fm_mime mime_pdf = { "application/pdf", "PDF Document", FM_CATEGORY_PDF };
static const struct fm_mime mime_zip = { "application/zip", "ZIP Archive", FM_CATEGORY_ARCHIVE };
static const struct fm_mime mime_gzip = { "application/gzip", "Gzip Archive", FM_CATEGORY_ARCHIVE };
static const struct fm_mime mime_xz = { "application/x-xz", "XZ Archive", FM_CATEGORY_ARCHIVE };
static const struct fm_mime mime_ppm = { "image/x-portable-pixmap", "PPM Image", FM_CATEGORY_IMAGE };

/*
 * The extensions the file manager knows, lower case.
 */
static const struct mime_extension mime_extensions[] = {
	{ "txt", { "text/plain", "Plain Text", FM_CATEGORY_TEXT } },
	{ "text", { "text/plain", "Plain Text", FM_CATEGORY_TEXT } },
	{ "md", { "text/markdown", "Markdown", FM_CATEGORY_TEXT } },
	{ "log", { "text/plain", "Log", FM_CATEGORY_TEXT } },
	{ "csv", { "text/csv", "CSV Document", FM_CATEGORY_TEXT } },
	{ "json", { "application/json", "JSON", FM_CATEGORY_CODE } },
	{ "xml", { "application/xml", "XML", FM_CATEGORY_CODE } },
	{ "html", { "text/html", "HTML Document", FM_CATEGORY_CODE } },
	{ "htm", { "text/html", "HTML Document", FM_CATEGORY_CODE } },
	{ "css", { "text/css", "CSS", FM_CATEGORY_CODE } },
	{ "c", { "text/x-c", "C Source", FM_CATEGORY_CODE } },
	{ "h", { "text/x-c", "C Header", FM_CATEGORY_CODE } },
	{ "cc", { "text/x-c++", "C++ Source", FM_CATEGORY_CODE } },
	{ "cpp", { "text/x-c++", "C++ Source", FM_CATEGORY_CODE } },
	{ "hpp", { "text/x-c++", "C++ Header", FM_CATEGORY_CODE } },
	{ "s", { "text/x-asm", "Assembly Source", FM_CATEGORY_CODE } },
	{ "py", { "text/x-python", "Python Script", FM_CATEGORY_CODE } },
	{ "sh", { "application/x-shellscript", "Shell Script", FM_CATEGORY_CODE } },
	{ "mk", { "text/x-makefile", "Makefile", FM_CATEGORY_CODE } },
	{ "js", { "text/javascript", "JavaScript", FM_CATEGORY_CODE } },
	{ "ts", { "text/typescript", "TypeScript", FM_CATEGORY_CODE } },
	{ "rs", { "text/x-rust", "Rust Source", FM_CATEGORY_CODE } },
	{ "go", { "text/x-go", "Go Source", FM_CATEGORY_CODE } },
	{ "java", { "text/x-java", "Java Source", FM_CATEGORY_CODE } },
	{ "el", { "text/x-emacs-lisp", "Emacs Lisp", FM_CATEGORY_CODE } },
	{ "noct", { "text/x-noct", "NoctLang Script", FM_CATEGORY_CODE } },
	{ "glsl", { "text/x-glsl", "GLSL Shader", FM_CATEGORY_CODE } },
	{ "vert", { "text/x-glsl", "Vertex Shader", FM_CATEGORY_CODE } },
	{ "frag", { "text/x-glsl", "Fragment Shader", FM_CATEGORY_CODE } },
	{ "png", { "image/png", "PNG Image", FM_CATEGORY_IMAGE } },
	{ "jpg", { "image/jpeg", "JPEG Image", FM_CATEGORY_IMAGE } },
	{ "jpeg", { "image/jpeg", "JPEG Image", FM_CATEGORY_IMAGE } },
	{ "gif", { "image/gif", "GIF Image", FM_CATEGORY_IMAGE } },
	{ "bmp", { "image/bmp", "BMP Image", FM_CATEGORY_IMAGE } },
	{ "webp", { "image/webp", "WebP Image", FM_CATEGORY_IMAGE } },
	{ "svg", { "image/svg+xml", "SVG Image", FM_CATEGORY_IMAGE } },
	{ "ppm", { "image/x-portable-pixmap", "PPM Image", FM_CATEGORY_IMAGE } },
	{ "pgm", { "image/x-portable-graymap", "PGM Image", FM_CATEGORY_IMAGE } },
	{ "fig", { "application/x-figma", "Design", FM_CATEGORY_IMAGE } },
	{ "mp3", { "audio/mpeg", "MP3 Audio", FM_CATEGORY_AUDIO } },
	{ "wav", { "audio/wav", "WAVE Audio", FM_CATEGORY_AUDIO } },
	{ "flac", { "audio/flac", "FLAC Audio", FM_CATEGORY_AUDIO } },
	{ "ogg", { "audio/ogg", "Ogg Audio", FM_CATEGORY_AUDIO } },
	{ "m4a", { "audio/mp4", "AAC Audio", FM_CATEGORY_AUDIO } },
	{ "mp4", { "video/mp4", "MPEG-4 Movie", FM_CATEGORY_VIDEO } },
	{ "mkv", { "video/x-matroska", "Matroska Movie", FM_CATEGORY_VIDEO } },
	{ "mov", { "video/quicktime", "QuickTime Movie", FM_CATEGORY_VIDEO } },
	{ "webm", { "video/webm", "WebM Movie", FM_CATEGORY_VIDEO } },
	{ "avi", { "video/x-msvideo", "AVI Movie", FM_CATEGORY_VIDEO } },
	{ "m2ts", { "video/mp2t", "MPEG-TS Movie", FM_CATEGORY_VIDEO } },
	{ "mts", { "video/mp2t", "MPEG-TS Movie", FM_CATEGORY_VIDEO } },
	{ "ogv", { "video/ogg", "Ogg Movie", FM_CATEGORY_VIDEO } },
	{ "zip", { "application/zip", "ZIP Archive", FM_CATEGORY_ARCHIVE } },
	{ "tar", { "application/x-tar", "Tar Archive", FM_CATEGORY_ARCHIVE } },
	{ "gz", { "application/gzip", "Gzip Archive", FM_CATEGORY_ARCHIVE } },
	{ "tgz", { "application/gzip", "Gzip Archive", FM_CATEGORY_ARCHIVE } },
	{ "xz", { "application/x-xz", "XZ Archive", FM_CATEGORY_ARCHIVE } },
	{ "bz2", { "application/x-bzip2", "Bzip2 Archive", FM_CATEGORY_ARCHIVE } },
	{ "zst", { "application/zstd", "Zstandard Archive", FM_CATEGORY_ARCHIVE } },
	{ "7z", { "application/x-7z-compressed", "7-Zip Archive", FM_CATEGORY_ARCHIVE } },
	{ "iso", { "application/x-iso9660-image", "Disk Image", FM_CATEGORY_ARCHIVE } },
	{ "img", { "application/x-raw-disk-image", "Disk Image", FM_CATEGORY_ARCHIVE } },
	{ "pdf", { "application/pdf", "PDF Document", FM_CATEGORY_PDF } },
	{ "doc", { "application/msword", "Word Document", FM_CATEGORY_DOCUMENT } },
	{ "docx", { "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "Word Document", FM_CATEGORY_DOCUMENT } },
	{ "odt", { "application/vnd.oasis.opendocument.text", "Text Document", FM_CATEGORY_DOCUMENT } },
	{ "xls", { "application/vnd.ms-excel", "Spreadsheet", FM_CATEGORY_DOCUMENT } },
	{ "xlsx", { "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "Spreadsheet", FM_CATEGORY_DOCUMENT } },
	{ "ods", { "application/vnd.oasis.opendocument.spreadsheet", "Spreadsheet", FM_CATEGORY_DOCUMENT } },
	{ "ppt", { "application/vnd.ms-powerpoint", "Presentation", FM_CATEGORY_DOCUMENT } },
	{ "pptx", { "application/vnd.openxmlformats-officedocument.presentationml.presentation", "Presentation", FM_CATEGORY_DOCUMENT } },
	{ "key", { "application/x-iwork-keynote", "Presentation", FM_CATEGORY_DOCUMENT } },
	{ "odp", { "application/vnd.oasis.opendocument.presentation", "Presentation", FM_CATEGORY_DOCUMENT } },
	{ "ttf", { "font/ttf", "TrueType Font", FM_CATEGORY_FONT } },
	{ "otf", { "font/otf", "OpenType Font", FM_CATEGORY_FONT } },
	{ "obj", { "model/obj", "3D Model", FM_CATEGORY_MODEL } },
	{ "stl", { "model/stl", "3D Model", FM_CATEGORY_MODEL } },
	{ "gltf", { "model/gltf+json", "3D Model", FM_CATEGORY_MODEL } },
	{ "glb", { "model/gltf-binary", "3D Model", FM_CATEGORY_MODEL } }
};

/*
 * A signature the first bytes of a file may start with, and its type.
 */
struct mime_signature {
	const char *bytes;
	size_t length;
	const struct fm_mime *mime;
};

/* The signatures looked for, in order. */
static const struct mime_signature mime_signatures[] = {
	{ "\x7f" "ELF", 4, &mime_elf },
	{ "#!", 2, &mime_script },
	{ "\x89" "PNG\r\n", 6, &mime_png },
	{ "\xff\xd8\xff", 3, &mime_jpeg },
	{ "GIF8", 4, &mime_gif },
	{ "%PDF-", 5, &mime_pdf },
	{ "PK\x03\x04", 4, &mime_zip },
	{ "\x1f\x8b", 2, &mime_gzip },
	{ "\xfd" "7zXZ", 5, &mime_xz },
	{ "P6\n", 3, &mime_ppm },
	{ "P6 ", 3, &mime_ppm },
	{ "P5\n", 3, &mime_ppm },
	{ "P5 ", 3, &mime_ppm }
};

static int mime_extension_of(const char *name, char *extension, size_t size);
static int mime_text_like(const unsigned char *bytes, size_t length);

/*
 * Guesses a file's type from its name and mode (a folder is a folder
 * whatever its name).
 */
const struct fm_mime *
fm_mime_guess(
	const char *name,
	mode_t mode)
{
	char extension[MIME_EXTENSION_MAX];
	size_t index;
	int regular;
	int folder;
	int found;
	int match;

	/* A folder. */
	folder = S_ISDIR(mode);
	if (folder != 0)
		return &mime_folder;

	/* The name's extension, when it has one. */
	found = mime_extension_of(name, extension, sizeof(extension));
	if (found != 0) {
		for (index = 0; index < sizeof(mime_extensions) / sizeof(mime_extensions[0]); index++) {
			match = strcmp(extension, mime_extensions[index].extension);
			if (match == 0)
				return &mime_extensions[index].mime;
		}
	}

	/* A file without a known extension that may run is a program. */
	regular = S_ISREG(mode);
	if (regular != 0 && (mode & 0111) != 0)
		return &mime_executable;

	/* Anything else is a document of no known kind. */
	return &mime_unknown;
}

/*
 * Looks at a file's first bytes when its name did not tell its type (the
 * guess is the plain document or program type), and returns the better
 * type; the guess itself when the bytes say nothing more.
 */
const struct fm_mime *
fm_mime_sniff(
	const char *path,
	const struct fm_mime *guess)
{
	unsigned char bytes[4096];
	ssize_t length;
	size_t index;
	int descriptor;
	int match;
	int text;

	/* Only a type the name did not decide is looked into. */
	if (guess != &mime_unknown && guess != &mime_executable)
		return guess;

	/* The file's first bytes. */
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return guess;
	length = read(descriptor, bytes, sizeof(bytes));
	close(descriptor);

	/* An empty or unreadable file says nothing. */
	if (length <= 0)
		return guess;

	/* The signatures of the formats the window shows or opens differently. */
	for (index = 0; index < sizeof(mime_signatures) / sizeof(mime_signatures[0]); index++) {
		if ((size_t)length < mime_signatures[index].length)
			continue;

		/* The file starts with this signature. */
		match = memcmp(bytes, mime_signatures[index].bytes, mime_signatures[index].length);
		if (match == 0)
			return mime_signatures[index].mime;
	}

	/* Bytes that read as text are plain text. */
	text = mime_text_like(bytes, (size_t)length);
	if (text != 0)
		return &mime_text;

	/* Nothing more is known. */
	return guess;
}

/*
 * Reports the color of a category's band on its file icon.
 */
kl_color
fm_mime_color(
	unsigned category)
{
	/* Each category's color; a plain document is grey. */
	switch (category) {
	case FM_CATEGORY_TEXT:
		return KL_RGB(0x8a94a6);
	case FM_CATEGORY_CODE:
		return KL_RGB(0x14a3a0);
	case FM_CATEGORY_IMAGE:
		return KL_RGB(0x3fb27f);
	case FM_CATEGORY_AUDIO:
		return KL_RGB(0xe85d9a);
	case FM_CATEGORY_VIDEO:
		return KL_RGB(0x8b5cf6);
	case FM_CATEGORY_ARCHIVE:
		return KL_RGB(0xa97142);
	case FM_CATEGORY_PDF:
		return KL_RGB(0xe0574f);
	case FM_CATEGORY_EXECUTABLE:
		return KL_RGB(0x3d4556);
	case FM_CATEGORY_DOCUMENT:
		return KL_RGB(0x3b82f6);
	case FM_CATEGORY_FONT:
		return KL_RGB(0xd08a1e);
	case FM_CATEGORY_MODEL:
		return KL_RGB(0xe07a5a);
	default:
		break;
	}

	/* A document of no known kind. */
	return KL_RGB(0x9aa3b2);
}

/*
 * Writes a name's extension in capitals (at most four letters) for a file
 * icon's band; empty when there is none.
 */
void
fm_mime_label(
	const char *name,
	char *label,
	size_t size)
{
	char extension[MIME_EXTENSION_MAX];
	size_t index;
	int found;

	/* No extension, no label. */
	label[0] = '\0';
	found = mime_extension_of(name, extension, sizeof(extension));
	if (found == 0 || size < 2U)
		return;

	/* Up to four letters, in capitals, as far as the label holds them. */
	index = 0;
	while (extension[index] != '\0') {
		if (index == 4U || index + 1U == size)
			break;
		label[index] = extension[index];
		if (label[index] >= 'a' && label[index] <= 'z')
			label[index] = (char)(label[index] - 'a' + 'A');
		index++;
	}

	/* The label ends after them. */
	label[index] = '\0';
}

/*
 * Tells whether bytes read from the start of a file are text: no NUL, and
 * well-formed UTF-8 but for a character cut at the end of the read.
 */
int
fm_mime_text(
	const unsigned char *bytes,
	size_t length)
{
	int text;

	/* The same test the type's sniffing makes. */
	text = mime_text_like(bytes, length);
	if (text == 0)
		return 0;

	/* Succeeded: the bytes are text. */
	return 1;
}

/* Copies a name's extension in lower case; zero when it has none (or one too long). */
static int
mime_extension_of(
	const char *name,
	char *extension,
	size_t size)
{
	const char *dot;
	size_t length;
	size_t index;

	/* The last dot, which must not start the name (a hidden file is not an extension). */
	dot = strrchr(name, '.');
	if (dot == NULL ||
	    dot == name ||
	    dot[1] == '\0')
		return 0;

	/* The extension must fit. */
	length = strlen(dot + 1);
	if (length + 1U > size)
		return 0;

	/* The extension, in lower case. */
	for (index = 0; index <= length; index++) {
		extension[index] = dot[1 + index];
		if (extension[index] >= 'A' && extension[index] <= 'Z')
			extension[index] = (char)(extension[index] - 'A' + 'a');
	}

	/* Succeeded: the name has an extension. */
	return 1;
}

/* Tells whether bytes read as text: no NUL, and well-formed UTF-8 but for a character cut at the end. */
static int
mime_text_like(
	const unsigned char *bytes,
	size_t length)
{
	size_t index;
	size_t before;
	uint32_t codepoint;

	/* Each character in turn. */
	index = 0;
	while (index < length) {
		/* A NUL does not appear in text. */
		if (bytes[index] == 0U)
			return 0;

		/* A malformed character is not text, unless it is the last one, cut by the read. */
		before = index;
		codepoint = kl_utf8_next((const char *)bytes, length, &index);
		if (codepoint == 0xfffdU && length - before > 4U)
			return 0;
	}

	/* Every character was text. */
	return 1;
}
