/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Unit test of libmedia's operating-system layer: loading the Vulkan
 * loader, and opening and reading files at any offset.  Every expected
 * value comes from the contract in media-os.h and from hand arithmetic.
 */

#include "check.h"

#include "media-os.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* The size of the largest test file: 5 MiB. */
#define TEST_BIG_SIZE ((size_t)5 * 1024 * 1024)

/* The name "動画.mp4" in UTF-8, spelled in bytes so that the encoding of this source does not matter. */
#define TEST_UNICODE_NAME "\xe5\x8b\x95\xe7\x94\xbb.mp4"

/* Which system the harness runs the test on ("posix" or "win32"). */
static const char *test_system;

static unsigned char test_pattern(uint64_t index);
static FILE *test_fopen(const char *name, const char *mode);
static int test_make_file(const char *name, size_t size);
static int test_make_directory(const char *name);
static void test_vulkan(void);
static void test_sizes(void);
static void test_reads(void);
static void test_far(void);
static void test_contract(void);
static void test_names(void);
static void test_sharing(void);
static void test_descriptor(void);
static void test_device(void);
static void test_access(void);

/*
 * Runs every group of checks on the system the harness named.
 */
int
main(
	int argc,
	char **argv)
{
	/* The harness names the system. */
	CHECK(argc == 2);
	if (argc != 2)
		return check_finish("media-os");
	test_system = argv[1];

	/* Runs the groups. */
	test_vulkan();
	test_sizes();
	test_reads();
	test_far();
	test_contract();
	test_names();
	test_sharing();
	test_descriptor();
	test_device();
	test_access();

	/* Reports the result. */
	return check_finish("media-os");
}

/* The byte a test file holds at an offset: a known function of the offset, with a period that is not a power of two. */
static unsigned char
test_pattern(
	uint64_t index)
{
	/* Mixes the low bits and the position in blocks so that a shifted read shows. */
	return (unsigned char)((index * 7U + (index >> 8) * 13U + 5U) % 251U);
}

/* Opens a file by its UTF-8 name, converting the name on Windows by a path of the test's own. */
static FILE *
test_fopen(
	const char *name,
	const char *mode)
{
#ifdef _WIN32
	wchar_t wide_name[256];
	wchar_t wide_mode[8];
	int count;
	int index;

	/* Converts the name. */
	count = MultiByteToWideChar(CP_UTF8, 0, name, -1, wide_name, 256);
	if (count == 0)
		return NULL;

	/* Converts the mode, which is plain ASCII. */
	index = 0;
	while (mode[index] != '\0' && index < 7) {
		wide_mode[index] = (wchar_t)(unsigned char)mode[index];
		index++;
	}
	wide_mode[index] = 0;

	return _wfopen(wide_name, wide_mode);
#else
	return fopen(name, mode);
#endif
}

/* Writes a file of size bytes of the test pattern; returns 0 when it was written. */
static int
test_make_file(
	const char *name,
	size_t size)
{
	FILE *stream;
	unsigned char *bytes;
	size_t index;
	size_t written;

	/* Fills a buffer with the pattern. */
	bytes = malloc(size == 0 ? 1 : size);
	if (bytes == NULL)
		return 1;
	for (index = 0;
	     index < size;
	     index++)
		bytes[index] = test_pattern(index);

	/* Writes it out. */
	stream = test_fopen(name, "wb");
	if (stream == NULL) {
		free(bytes);
		return 1;
	}
	written = fwrite(bytes, 1, size, stream);
	fclose(stream);
	free(bytes);
	if (written != size)
		return 1;

	return 0;
}

/* Makes a directory; returns 0 when it was made. */
static int
test_make_directory(
	const char *name)
{
	int status;

#ifdef _WIN32
	status = _mkdir(name);
#else
	status = mkdir(name, 0777);
#endif

	return status;
}

/* Checks loading the Vulkan loader and finding functions in it. */
static void
test_vulkan(void)
{
	struct media_os_library *library;
	struct media_os_library *again;
	media_os_function function;
	media_os_function other;
	int error;

	/* A missing result slot is the caller's fault. */
	error = media_os_vulkan_open(NULL);
	CHECK_EQ(error, EINVAL);

	/* Loads the loader, which the host and the Wine system directory both have. */
	library = (struct media_os_library *)1;
	error = media_os_vulkan_open(&library);
	CHECK_EQ(error, 0);
	CHECK(library != NULL);
	if (error != 0 || library == NULL)
		return;

	/* The loader's first entry point is there. */
	function = media_os_library_symbol(library, "vkGetInstanceProcAddr");
	CHECK(function != NULL);

	/* Another entry point is a different function. */
	other = media_os_library_symbol(library, "vkCreateInstance");
	CHECK(other != NULL);
	CHECK(other != function);

	/* A name the loader does not export, and the empty name, find nothing. */
	function = media_os_library_symbol(library, "vkNoSuchFunctionAnywhere");
	CHECK(function == NULL);
	function = media_os_library_symbol(library, "");
	CHECK(function == NULL);

	/* Missing arguments find nothing. */
	function = media_os_library_symbol(NULL, "vkGetInstanceProcAddr");
	CHECK(function == NULL);
	function = media_os_library_symbol(library, NULL);
	CHECK(function == NULL);
	function = media_os_library_symbol(NULL, NULL);
	CHECK(function == NULL);

	/* Loading a second time gives the same loader. */
	again = NULL;
	error = media_os_vulkan_open(&again);
	CHECK_EQ(error, 0);
	CHECK(again == library);
}

/* Checks the sizes that open reports, for an empty, a one-byte and a 5 MiB file. */
static void
test_sizes(void)
{
	static const size_t sizes[3] = { 0, 1, TEST_BIG_SIZE };
	static const char *const names[3] = { "empty.bin", "one.bin", "big.bin" };
	struct media_os_file *file;
	uint64_t size;
	unsigned char byte;
	int error;
	int index;

	/* Makes each file and opens it. */
	for (index = 0;
	     index < 3;
	     index++) {
		error = test_make_file(names[index], sizes[index]);
		CHECK_EQ(error, 0);

		file = (struct media_os_file *)1;
		size = 12345;
		error = media_os_file_open(names[index], &file, &size);
		CHECK_EQ(error, 0);
		CHECK(file != NULL);
		CHECK_EQ(size, sizes[index]);

		/* The file reads its first byte when it has one. */
		if (file != NULL && sizes[index] > 0) {
			byte = 0;
			error = media_os_file_read_at(file, 0, &byte, 1);
			CHECK_EQ(error, 0);
			CHECK_EQ(byte, test_pattern(0));
		}

		media_os_file_close(file);
	}
}

/* Checks reads at the start, the middle and the end of a 5 MiB file, and reads that fail. */
static void
test_reads(void)
{
	struct media_os_file *file;
	uint64_t size;
	unsigned char *buffer;
	unsigned char byte;
	uint64_t index;
	uint64_t offset;
	int wrong;
	int error;

	/* Opens the big file made by the size group. */
	buffer = malloc(TEST_BIG_SIZE);
	CHECK(buffer != NULL);
	if (buffer == NULL)
		return;
	error = test_make_file("big.bin", TEST_BIG_SIZE);
	CHECK_EQ(error, 0);
	file = NULL;
	error = media_os_file_open("big.bin", &file, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, TEST_BIG_SIZE);
	if (error != 0 || file == NULL) {
		free(buffer);
		return;
	}

	/* Reads the first 4096 bytes. */
	memset(buffer, 0, TEST_BIG_SIZE);
	error = media_os_file_read_at(file, 0, buffer, 4096);
	CHECK_EQ(error, 0);
	wrong = 0;
	for (index = 0;
	     index < 4096;
	     index++) {
		if (buffer[index] != test_pattern(index))
			wrong++;
	}
	CHECK_EQ(wrong, 0);

	/* Reads 100000 bytes from the middle, from an offset that is not aligned. */
	offset = 2621441;
	error = media_os_file_read_at(file, offset, buffer, 100000);
	CHECK_EQ(error, 0);
	wrong = 0;
	for (index = 0;
	     index < 100000;
	     index++) {
		if (buffer[index] != test_pattern(offset + index))
			wrong++;
	}
	CHECK_EQ(wrong, 0);

	/* Reads the last 100 bytes, the last byte alone, and the whole file at once. */
	offset = TEST_BIG_SIZE - 100;
	error = media_os_file_read_at(file, offset, buffer, 100);
	CHECK_EQ(error, 0);
	wrong = 0;
	for (index = 0;
	     index < 100;
	     index++) {
		if (buffer[index] != test_pattern(offset + index))
			wrong++;
	}
	CHECK_EQ(wrong, 0);
	byte = 0;
	error = media_os_file_read_at(file, TEST_BIG_SIZE - 1, &byte, 1);
	CHECK_EQ(error, 0);
	CHECK_EQ(byte, test_pattern(TEST_BIG_SIZE - 1));
	memset(buffer, 0, TEST_BIG_SIZE);
	error = media_os_file_read_at(file, 0, buffer, TEST_BIG_SIZE);
	CHECK_EQ(error, 0);
	wrong = 0;
	for (index = 0;
	     index < TEST_BIG_SIZE;
	     index++) {
		if (buffer[index] != test_pattern(index))
			wrong++;
	}
	CHECK_EQ(wrong, 0);

	/* Reading nothing succeeds at the start, at the end, past the end and at the largest offset. */
	error = media_os_file_read_at(file, 0, NULL, 0);
	CHECK_EQ(error, 0);
	error = media_os_file_read_at(file, TEST_BIG_SIZE, NULL, 0);
	CHECK_EQ(error, 0);
	error = media_os_file_read_at(file, TEST_BIG_SIZE + 77, &byte, 0);
	CHECK_EQ(error, 0);
	error = media_os_file_read_at(file, UINT64_MAX, &byte, 0);
	CHECK_EQ(error, 0);

	/* A read that starts at the end, or ends one byte after it, finds the file ended. */
	error = media_os_file_read_at(file, TEST_BIG_SIZE, buffer, 1);
	CHECK_EQ(error, EIO);
	error = media_os_file_read_at(file, TEST_BIG_SIZE - 1, buffer, 2);
	CHECK_EQ(error, EIO);
	error = media_os_file_read_at(file, TEST_BIG_SIZE - 10, buffer, 11);
	CHECK_EQ(error, EIO);
	error = media_os_file_read_at(file, 0, buffer, TEST_BIG_SIZE + 1);
	CHECK_EQ(error, EIO);
	error = media_os_file_read_at(file, TEST_BIG_SIZE + 4096, buffer, 16);
	CHECK_EQ(error, EIO);

	/* A range that ends beyond what any file offset can name finds the file ended too. */
	error = media_os_file_read_at(file, (uint64_t)1 << 63, buffer, 1);
	CHECK_EQ(error, EIO);
	error = media_os_file_read_at(file, UINT64_MAX - 1, buffer, 1);
	CHECK_EQ(error, EIO);
	error = media_os_file_read_at(file, UINT64_MAX - 15, buffer, 15);
	CHECK_EQ(error, EIO);

	/* The reads that failed left the file usable. */
	byte = 0;
	error = media_os_file_read_at(file, 12, &byte, 1);
	CHECK_EQ(error, 0);
	CHECK_EQ(byte, test_pattern(12));

	media_os_file_close(file);
	free(buffer);
}

/* Checks reads beyond 4 GiB, where the high half of the offset matters, in a sparse file. */
static void
test_far(void)
{
	static const uint64_t far_offset = ((uint64_t)1 << 32) + 5;
	struct media_os_file *file;
	FILE *stream;
	uint64_t size;
	unsigned char bytes[3];
	int status;
	int error;

	/* Makes a file whose bytes at 4 GiB + 5 are 0xa1, 0xb2, 0xc3 and that holds no data before them. */
	stream = test_fopen("far.bin", "wb");
	CHECK(stream != NULL);
	if (stream == NULL)
		return;
#ifdef _WIN32
	status = _fseeki64(stream, (long long)far_offset, SEEK_SET);
#else
	status = fseeko(stream, (off_t)far_offset, SEEK_SET);
#endif
	CHECK_EQ(status, 0);
	bytes[0] = 0xa1;
	bytes[1] = 0xb2;
	bytes[2] = 0xc3;
	fwrite(bytes, 1, 3, stream);
	status = fclose(stream);
	CHECK_EQ(status, 0);

	/* The size has the high half. */
	file = NULL;
	error = media_os_file_open("far.bin", &file, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, far_offset + 3);
	if (file == NULL)
		return;

	/* The three bytes are found at the far offset, and the bytes before them are zero. */
	memset(bytes, 0, sizeof(bytes));
	error = media_os_file_read_at(file, far_offset, bytes, 3);
	CHECK_EQ(error, 0);
	CHECK_EQ(bytes[0], 0xa1);
	CHECK_EQ(bytes[1], 0xb2);
	CHECK_EQ(bytes[2], 0xc3);
	memset(bytes, 0xff, sizeof(bytes));
	error = media_os_file_read_at(file, 5, bytes, 3);
	CHECK_EQ(error, 0);
	CHECK_EQ(bytes[0], 0);
	CHECK_EQ(bytes[2], 0);

	/* One byte more than the file holds fails. */
	error = media_os_file_read_at(file, far_offset + 1, bytes, 3);
	CHECK_EQ(error, EIO);

	media_os_file_close(file);
}

/* Checks the arguments that break the contract. */
static void
test_contract(void)
{
	struct media_os_file *file;
	struct media_os_file *kept;
	uint64_t size;
	unsigned char buffer[16];
	int error;

	/* Makes a small file. */
	error = test_make_file("small.bin", 64);
	CHECK_EQ(error, 0);
	file = NULL;
	error = media_os_file_open("small.bin", &file, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, 64);
	if (file == NULL)
		return;
	kept = file;

	/* A missing file is refused, even for a read of nothing. */
	error = media_os_file_read_at(NULL, 0, buffer, 1);
	CHECK_EQ(error, EINVAL);
	error = media_os_file_read_at(NULL, 0, NULL, 0);
	CHECK_EQ(error, EINVAL);

	/* A missing buffer is refused for a read of something. */
	error = media_os_file_read_at(file, 0, NULL, 1);
	CHECK_EQ(error, EINVAL);

	/* Ranges whose end passes 2^64 are refused: the sum 2^64 itself, and larger. */
	error = media_os_file_read_at(file, UINT64_MAX, buffer, 1);
	CHECK_EQ(error, EINVAL);
	error = media_os_file_read_at(file, UINT64_MAX - 1, buffer, 2);
	CHECK_EQ(error, EINVAL);
	error = media_os_file_read_at(file, UINT64_MAX - 15, buffer, 17);
	CHECK_EQ(error, EINVAL);
	error = media_os_file_read_at(file, 2, buffer, SIZE_MAX);
	CHECK_EQ(error, EINVAL);
	error = media_os_file_read_at(file, UINT64_MAX, buffer, SIZE_MAX);
	CHECK_EQ(error, EINVAL);
	error = media_os_file_read_at(file, UINT64_MAX, NULL, 1);
	CHECK_EQ(error, EINVAL);

	/* Open refuses its missing arguments and leaves the results as "no file". */
	size = 99;
	error = media_os_file_open(NULL, &file, &size);
	CHECK_EQ(error, EINVAL);
	CHECK(file == NULL);
	CHECK_EQ(size, 0);
	file = (struct media_os_file *)1;
	size = 99;
	error = media_os_file_open("", &file, &size);
	CHECK_EQ(error, EINVAL);
	CHECK(file == NULL);
	CHECK_EQ(size, 0);
	size = 99;
	error = media_os_file_open("small.bin", NULL, &size);
	CHECK_EQ(error, EINVAL);
	file = (struct media_os_file *)1;
	error = media_os_file_open("small.bin", &file, NULL);
	CHECK_EQ(error, EINVAL);
	CHECK(file == NULL);

	/* Closing nothing does nothing. */
	media_os_file_close(NULL);
	media_os_file_close(NULL);

	/* The file opened first is still open and closes. */
	media_os_file_close(kept);
}

/* Checks the errors for names: missing, a directory, a file used as a directory, too long, not UTF-8, UTF-8. */
static void
test_names(void)
{
	struct media_os_file *file;
	uint64_t size;
	unsigned char buffer[8];
	char *long_name;
	int error;
	int index;
	const char *invalid[4] = {
		"bad\xff.mp4",
		"bad\xe5\x8b.mp4",
		"bad\xc0\xaf.mp4",
		"\x80"
	};

	/* A file that is not there. */
	file = (struct media_os_file *)1;
	size = 5;
	error = media_os_file_open("no-such-file.mp4", &file, &size);
	CHECK_EQ(error, ENOENT);
	CHECK(file == NULL);
	CHECK_EQ(size, 0);

	/* A directory that is not there on the way to the file. */
	error = media_os_file_open("no-such-directory/file.mp4", &file, &size);
	CHECK_EQ(error, ENOENT);
	CHECK(file == NULL);

	/* A directory, named plainly, with a trailing separator as a file would not have, and as the current one. */
	error = test_make_directory("folder");
	CHECK_EQ(error, 0);
	size = 5;
	error = media_os_file_open("folder", &file, &size);
	CHECK_EQ(error, EISDIR);
	CHECK(file == NULL);
	CHECK_EQ(size, 0);
	error = media_os_file_open(".", &file, &size);
	CHECK_EQ(error, EISDIR);
	CHECK(file == NULL);

	/* A file used as a directory on the way to another name. */
	error = test_make_file("regular.bin", 10);
	CHECK_EQ(error, 0);
	error = media_os_file_open("regular.bin/inner.mp4", &file, &size);
	CHECK_EQ(error, ENOENT);
	CHECK(file == NULL);

	/* A path far longer than any system takes. */
	long_name = malloc(200001);
	CHECK(long_name != NULL);
	if (long_name != NULL) {
		memset(long_name, 'a', 200000);
		long_name[200000] = '\0';
		error = media_os_file_open(long_name, &file, &size);
		CHECK_EQ(error, ENAMETOOLONG);
		CHECK(file == NULL);
		CHECK_EQ(size, 0);
		long_name[40000] = '\0';
		error = media_os_file_open(long_name, &file, &size);
		CHECK_EQ(error, ENAMETOOLONG);
		CHECK(file == NULL);
		free(long_name);
	}

	/*
	 * A name with characters outside ASCII opens and reads.  Wine run
	 * in a locale its host does not have cannot make such a file, and
	 * then only the conversion is checked: valid UTF-8 of every length
	 * must get as far as "not found" and not be refused as bad UTF-8.
	 */
	error = test_make_file(TEST_UNICODE_NAME, 3000);
	if (error == 0) {
		file = NULL;
		size = 0;
		error = media_os_file_open(TEST_UNICODE_NAME, &file, &size);
		CHECK_EQ(error, 0);
		CHECK_EQ(size, 3000);
		if (file != NULL) {
			error = media_os_file_read_at(file, 2990, buffer, 8);
			CHECK_EQ(error, 0);
			CHECK_EQ(buffer[0], test_pattern(2990));
			CHECK_EQ(buffer[7], test_pattern(2997));
			media_os_file_close(file);
		}
	} else {
		printf("note: no file with a non-ASCII name can be made here\n");
		CHECK(strcmp(test_system, "win32") == 0);
	}
	error = media_os_file_open("absent-\xc3\xa9-\xe5\x8b\x95-\xf0\x9f\x8e\xac.mp4", &file, &size);
	CHECK_EQ(error, ENOENT);
	CHECK(file == NULL);

	/* Windows refuses bytes that are not UTF-8; a POSIX path is bytes and is not judged. */
	for (index = 0;
	     index < 4;
	     index++) {
		file = (struct media_os_file *)1;
		size = 5;
		error = media_os_file_open(invalid[index], &file, &size);
		if (strcmp(test_system, "win32") == 0)
			CHECK_EQ(error, EILSEQ);
		else
			CHECK_EQ(error, ENOENT);
		CHECK(file == NULL);
		CHECK_EQ(size, 0);
	}
}

/* Checks that a file can be opened while another handle writes it, and read after it was removed. */
static void
test_sharing(void)
{
	struct media_os_file *file;
	struct media_os_file *second;
	FILE *writer;
	uint64_t size;
	unsigned char buffer[4];
	int error;

	/* Another handle holds the file open for writing and appends to it. */
	error = test_make_file("growing.bin", 1000);
	CHECK_EQ(error, 0);
	writer = test_fopen("growing.bin", "ab");
	CHECK(writer != NULL);
	if (writer == NULL)
		return;

	/* The file still opens, with the size it has now. */
	file = NULL;
	size = 0;
	error = media_os_file_open("growing.bin", &file, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, 1000);
	CHECK(file != NULL);

	/* The writer appends 4 bytes; they can be read through the open file. */
	buffer[0] = test_pattern(1000);
	buffer[1] = test_pattern(1001);
	buffer[2] = test_pattern(1002);
	buffer[3] = test_pattern(1003);
	fwrite(buffer, 1, 4, writer);
	fflush(writer);
	memset(buffer, 0, sizeof(buffer));
	if (file != NULL) {
		error = media_os_file_read_at(file, 1000, buffer, 4);
		CHECK_EQ(error, 0);
		CHECK_EQ(buffer[0], test_pattern(1000));
		CHECK_EQ(buffer[3], test_pattern(1003));
	}

	/* A second open sees the grown size, and a writer can also start while the file is open. */
	second = NULL;
	error = media_os_file_open("growing.bin", &second, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, 1004);
	media_os_file_close(second);
	fclose(writer);
	writer = test_fopen("growing.bin", "ab");
	CHECK(writer != NULL);
	if (writer != NULL)
		fclose(writer);

	/* The file can be removed while it is open; what was open is still read. */
	error = remove("growing.bin");
	CHECK_EQ(error, 0);
	if (file != NULL) {
		memset(buffer, 0, sizeof(buffer));
		error = media_os_file_read_at(file, 10, buffer, 4);
		CHECK_EQ(error, 0);
		CHECK_EQ(buffer[0], test_pattern(10));
		CHECK_EQ(buffer[3], test_pattern(13));
	}
	media_os_file_close(file);
}

/* Checks closing a descriptor that came with a picture. */
static void
test_descriptor(void)
{
#ifdef _WIN32
	int descriptor;
	int status;

	/* Windows does nothing: a C descriptor given to it is still open and closes once. */
	descriptor = _open("small.bin", _O_RDONLY | _O_BINARY);
	CHECK(descriptor >= 0);
	media_os_close_descriptor(descriptor);
	media_os_close_descriptor(-1);
	media_os_close_descriptor(-100);
	status = _close(descriptor);
	CHECK_EQ(status, 0);
#else
	int ends[2];
	int status;
	int flags;

	/* Closes one end of a pipe; the descriptor is then not open. */
	status = pipe(ends);
	CHECK_EQ(status, 0);
	if (status != 0)
		return;
	media_os_close_descriptor(ends[0]);
	errno = 0;
	flags = fcntl(ends[0], F_GETFD);
	CHECK_EQ(flags, -1);
	CHECK_EQ(errno, EBADF);

	/* The other end is untouched. */
	flags = fcntl(ends[1], F_GETFD);
	CHECK(flags != -1);

	/* Negative values do nothing: the other end is still open afterwards. */
	media_os_close_descriptor(-1);
	media_os_close_descriptor(-1000);
	flags = fcntl(ends[1], F_GETFD);
	CHECK(flags != -1);
	close(ends[1]);
#endif
}

/* Checks that a device is refused, because it has no size to read at offsets. */
static void
test_device(void)
{
	struct media_os_file *file;
	uint64_t size;
	int error;

	/* The null device opens as a handle but is no file. */
	file = (struct media_os_file *)1;
	size = 5;
#ifdef _WIN32
	error = media_os_file_open("NUL", &file, &size);
#else
	error = media_os_file_open("/dev/null", &file, &size);
#endif
	CHECK_EQ(error, EIO);
	CHECK(file == NULL);
	CHECK_EQ(size, 0);
}

/* Checks the refusal of a file the user may not read. */
static void
test_access(void)
{
	struct media_os_file *file;
	uint64_t size;
	int error;
#ifdef _WIN32
	HANDLE holder;
#endif

	error = test_make_file("locked.bin", 10);
	CHECK_EQ(error, 0);
	file = (struct media_os_file *)1;
	size = 5;

#ifdef _WIN32
	/* Another handle that shares the file with nobody makes the open fail on a sharing violation. */
	holder = CreateFileW(L"locked.bin", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(holder != INVALID_HANDLE_VALUE);
	if (holder == INVALID_HANDLE_VALUE)
		return;
	error = media_os_file_open("locked.bin", &file, &size);
	CHECK_EQ(error, EACCES);
	CHECK(file == NULL);
	CHECK_EQ(size, 0);
	CloseHandle(holder);

	/* Once the other handle is closed, the file opens. */
	error = media_os_file_open("locked.bin", &file, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, 10);
	media_os_file_close(file);
#else
	/* A file with no permission bits refuses everyone but the superuser. */
	if (geteuid() == 0)
		return;
	error = chmod("locked.bin", 0);
	CHECK_EQ(error, 0);
	error = media_os_file_open("locked.bin", &file, &size);
	CHECK_EQ(error, EACCES);
	CHECK(file == NULL);
	CHECK_EQ(size, 0);
	chmod("locked.bin", 0600);
#endif
}
