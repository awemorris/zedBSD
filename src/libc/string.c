/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "src/libc/heap.h"

#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>

/* The longest "Unknown error N" description, with its terminator. */
#define ERROR_UNKNOWN_SIZE 32

/* The size in bytes of the word the block operations move. */
#define STRING_WORD_SIZE sizeof(unsigned long)

/* The low address bits that say how far an address is past a word boundary. */
#define STRING_WORD_MASK (STRING_WORD_SIZE - 1U)

/* The number of words one pass of an unrolled word loop moves. */
#define STRING_WORD_RUN 4U

/*
 * The shortest block that is moved by words.
 *
 * A shorter block is moved byte by byte: aligning it would cost more than
 * the words would save.
 */
#define STRING_WORD_MINIMUM (STRING_WORD_SIZE * 2U)

/*
 * One error number and the description strerror gives it.
 *
 * The descriptions follow the wording that other systems print, so that a
 * diagnostic reads the same everywhere.
 */
struct error_description {
	int number;
	const char *text;
};

/*
 * The word memcpy, memmove and memset move once the destination is aligned.
 *
 * A whole word is stored at a time so that a copy into an uncached or
 * write-combined mapping costs one bus write per word rather than one per
 * byte.  The type may alias an object of any type, because the bytes the
 * block operations move belong to objects of every type, and this libc is
 * also built with strict aliasing.
 */
typedef unsigned long string_word __attribute__((__may_alias__));

/*
 * A word read from an address that need not be word aligned.
 *
 * A source whose alignment differs from the destination's is read through
 * this type.  A target that loads unaligned words (amd64, arm64) reads it
 * with one load each; on a target that cannot (m68k, sparcv9) the compiler
 * reads its bytes one by one.  The store is a whole aligned word on every
 * target.
 */
typedef unsigned long string_unaligned_word __attribute__((__may_alias__, __aligned__(1)));

/*
 * The description of every error number of <uapi/errno.h>, in its order.
 *
 * The table is constant for the life of the program.  An error number
 * added to <uapi/errno.h> needs a row here;
 * plan/ws001/tests/strerror-host-test.sh checks that every one has one.
 */
static const struct error_description error_descriptions[] = {
	{ EDOM, "Numerical argument out of domain" },
	{ ERANGE, "Numerical result out of range" },
	{ EINVAL, "Invalid argument" },
	{ ENOMEM, "Cannot allocate memory" },
	{ EIO, "Input/output error" },
	{ ENOENT, "No such file or directory" },
	{ EINTR, "Interrupted system call" },
	{ ENOSPC, "No space left on device" },
	{ EROFS, "Read-only file system" },
	{ EOVERFLOW, "Value too large for defined data type" },
	{ ENAMETOOLONG, "File name too long" },
	{ ENXIO, "No such device or address" },
	{ ENODEV, "No such device" },
	{ ENOTDIR, "Not a directory" },
	{ EISDIR, "Is a directory" },
	{ EEXIST, "File exists" },
	{ EBUSY, "Device or resource busy" },
	{ ENOTEMPTY, "Directory not empty" },
	{ EBADF, "Bad file descriptor" },
	{ ENOSYS, "Function not implemented" },
	{ EOPNOTSUPP, "Operation not supported" },
	{ ENOEXEC, "Exec format error" },
	{ EFAULT, "Bad address" },
	{ EAGAIN, "Resource temporarily unavailable" },
	{ EACCES, "Permission denied" },
	{ ESRCH, "No such process" },
	{ ECHILD, "No child processes" },
	{ E2BIG, "Argument list too long" },
	{ ENFILE, "Too many open files in system" },
	{ EMSGSIZE, "Message too long" },
	{ ENOBUFS, "No buffer space available" },
	{ ENETDOWN, "Network is down" },
	{ ENETUNREACH, "Network is unreachable" },
	{ EPROTONOSUPPORT, "Protocol not supported" },
	{ EAFNOSUPPORT, "Address family not supported by protocol" },
	{ EADDRINUSE, "Address already in use" },
	{ EADDRNOTAVAIL, "Cannot assign requested address" },
	{ EISCONN, "Transport endpoint is already connected" },
	{ ENOTCONN, "Transport endpoint is not connected" },
	{ ECONNREFUSED, "Connection refused" },
	{ ECONNRESET, "Connection reset by peer" },
	{ ETIMEDOUT, "Connection timed out" },
	{ EHOSTUNREACH, "No route to host" },
	{ EPIPE, "Broken pipe" },
	{ EDESTADDRREQ, "Destination address required" },
	{ EMFILE, "Too many open files" },
	{ EPERM, "Operation not permitted" },
	{ EXDEV, "Invalid cross-device link" },
	{ ESPIPE, "Illegal seek" },
	{ ELOOP, "Too many levels of symbolic links" },
	{ EFBIG, "File too large" },
	{ ENOTTY, "Inappropriate ioctl for device" },
	{ EINPROGRESS, "Operation now in progress" },
	{ EALREADY, "Operation already in progress" },
	{ ECONNABORTED, "Software caused connection abort" },
	{ ENOPROTOOPT, "Protocol not available" },
	{ EMLINK, "Too many links" },
	{ EDEADLK, "Resource deadlock avoided" },
	{ ECANCELED, "Operation canceled" },
	{ ENOTSOCK, "Socket operation on non-socket" },
	{ EILSEQ, "Invalid or incomplete multibyte or wide character" },
	{ ENODATA, "No data available" },
	{ EDQUOT, "Disk quota exceeded" },
	{ ENOMSG, "No message of desired type" },
	{ EIDRM, "Identifier removed" },
	{ ESTALE, "Stale file handle" },
	{ EPROTO, "Protocol error" },
	{ EPFNOSUPPORT, "Protocol family not supported" },
	{ ETXTBSY, "Text file busy" },
	{ EBADMSG, "Bad message" },
	{ EMULTIHOP, "Multihop attempted" },
	{ ENETRESET, "Network dropped connection on reset" },
	{ ENOLCK, "No locks available" },
	{ ENOLINK, "Link has been severed" },
	{ ENOSR, "Out of streams resources" },
	{ ENOSTR, "Device not a stream" },
	{ ENOTRECOVERABLE, "State not recoverable" },
	{ EOWNERDEAD, "Owner died" },
	{ EPROTOTYPE, "Protocol wrong type for socket" },
	{ ESOCKTNOSUPPORT, "Socket type not supported" },
	{ ETIME, "Timer expired" },
};

/*
 * The description strerror returns for a number that is not an error
 * number.  POSIX lets strerror reuse its result, so it is rewritten by the
 * next such call.
 */
static char error_unknown[ERROR_UNKNOWN_SIZE];

static const char *error_text(int error);
static void error_format_unknown(int error, char *buffer, size_t size);
static void string_copy_forward(unsigned char *destination, const unsigned char *source, size_t count);
static void string_copy_backward(unsigned char *destination_end, const unsigned char *source_end, size_t count);
static void string_copy_words(string_word *destination, const string_word *source, size_t words);
static void string_copy_unaligned_words(string_word *destination, const string_unaligned_word *source, size_t words);
static void string_copy_words_backward(string_word *destination_end, const string_word *source_end, size_t words);
static void string_copy_unaligned_words_backward(string_word *destination_end, const string_unaligned_word *source_end, size_t words);
static void string_fill(unsigned char *destination, unsigned char value, size_t count);
static void string_fill_words(string_word *destination, string_word pattern, size_t words);

#if defined(__x86_64__)
/*
 * Unaligned loads and stores of 2, 4, 8 and 16 bytes for memcpy on amd64,
 * which loads and stores at any alignment; one instruction each.
 */
typedef uint16_t string_u16 __attribute__((__may_alias__, __aligned__(1)));
typedef uint32_t string_u32 __attribute__((__may_alias__, __aligned__(1)));
typedef uint64_t string_u64 __attribute__((__may_alias__, __aligned__(1)));
typedef unsigned char string_v16 __attribute__((__vector_size__(16), __may_alias__, __aligned__(1)));

/*
 * The length from which memcpy copies with "rep movsb" on a processor with
 * Enhanced REP MOVSB (CPUID leaf 7, EBX bit 9): the length from which the
 * microcoded string copy beats a loop of 16-byte moves (the threshold
 * glibc uses on such processors, Intel 64 and IA-32 Architectures
 * Optimization Reference Manual, "Enhanced REP MOVSB and STOSB operation").
 */
#define STRING_REP_MOVSB_MINIMUM 2048U

/* Whether the processor has Enhanced REP MOVSB: 0 not asked yet, 1 yes, 2 no. */
static int string_erms;

/* Reports whether "rep movsb" is the fast copy on this processor, asking it once. */
static int
string_erms_present(
	void)
{
	unsigned eax;
	unsigned ebx;
	unsigned ecx;
	unsigned edx;
	int state;

	/* Answers from the first ask (a race only asks twice, with the same answer). */
	state = __atomic_load_n(&string_erms, __ATOMIC_RELAXED);
	if (state != 0)
		return state == 1;

	/* Leaf 7 exists when the highest basic leaf reaches it. */
	state = 2;
	__asm__ volatile("cpuid"
			 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
			 : "a"(0U), "c"(0U));
	if (eax >= 7U) {
		__asm__ volatile("cpuid"
				 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
				 : "a"(7U), "c"(0U));
		if ((ebx & (1U << 9)) != 0U)
			state = 1;
	}
	__atomic_store_n(&string_erms, state, __ATOMIC_RELAXED);
	return state == 1;
}

/*
 * Copies a block of memory that does not overlap its destination.
 *
 * A block up to 64 bytes is copied by loads of its head and its tail that
 * may overlap in the middle, all loaded before anything is stored, without
 * a loop.  A longer one goes 32 bytes a time and ends with the 32 bytes of
 * its tail; from STRING_REP_MOVSB_MINIMUM bytes a processor with Enhanced
 * REP MOVSB copies it with "rep movsb".
 */
void *
memcpy(
	void *destination,
	const void *source,
	size_t count)
{
	unsigned char *to;
	const unsigned char *from;
	string_v16 head0;
	string_v16 head1;
	string_v16 tail0;
	string_v16 tail1;
	uint64_t word0;
	uint64_t word1;
	uint32_t half0;
	uint32_t half1;
	uint16_t pair0;
	uint16_t pair1;
	size_t done;

	to = destination;
	from = source;

	/* A short block: its head and its tail, which cover it between them. */
	if (count <= 16U) {
		if (count >= 8U) {
			word0 = *(const string_u64 *)(const void *)from;
			word1 = *(const string_u64 *)(const void *)(from + count - 8U);
			*(string_u64 *)(void *)to = word0;
			*(string_u64 *)(void *)(to + count - 8U) = word1;
		} else if (count >= 4U) {
			half0 = *(const string_u32 *)(const void *)from;
			half1 = *(const string_u32 *)(const void *)(from + count - 4U);
			*(string_u32 *)(void *)to = half0;
			*(string_u32 *)(void *)(to + count - 4U) = half1;
		} else if (count >= 2U) {
			pair0 = *(const string_u16 *)(const void *)from;
			pair1 = *(const string_u16 *)(const void *)(from + count - 2U);
			*(string_u16 *)(void *)to = pair0;
			*(string_u16 *)(void *)(to + count - 2U) = pair1;
		} else if (count == 1U) {
			*to = *from;
		}
		return destination;
	}
	if (count <= 32U) {
		head0 = *(const string_v16 *)(const void *)from;
		tail0 = *(const string_v16 *)(const void *)(from + count - 16U);
		*(string_v16 *)(void *)to = head0;
		*(string_v16 *)(void *)(to + count - 16U) = tail0;
		return destination;
	}
	if (count <= 64U) {
		head0 = *(const string_v16 *)(const void *)from;
		head1 = *(const string_v16 *)(const void *)(from + 16U);
		tail0 = *(const string_v16 *)(const void *)(from + count - 32U);
		tail1 = *(const string_v16 *)(const void *)(from + count - 16U);
		*(string_v16 *)(void *)to = head0;
		*(string_v16 *)(void *)(to + 16U) = head1;
		*(string_v16 *)(void *)(to + count - 32U) = tail0;
		*(string_v16 *)(void *)(to + count - 16U) = tail1;
		return destination;
	}

	/* A long block on a processor whose string copy is fast for it. */
	if (count >= STRING_REP_MOVSB_MINIMUM && string_erms_present()) {
		__asm__ volatile("rep movsb"
				 : "+D"(to), "+S"(from), "+c"(count)
				 :
				 : "memory");
		return destination;
	}

	/* The rest: 32 bytes a time, and the last 32 bytes, which may overlap the loop's last. */
	tail0 = *(const string_v16 *)(const void *)(from + count - 32U);
	tail1 = *(const string_v16 *)(const void *)(from + count - 16U);
	for (done = 0; done + 32U < count; done += 32U) {
		head0 = *(const string_v16 *)(const void *)(from + done);
		head1 = *(const string_v16 *)(const void *)(from + done + 16U);
		*(string_v16 *)(void *)(to + done) = head0;
		*(string_v16 *)(void *)(to + done + 16U) = head1;
	}
	*(string_v16 *)(void *)(to + count - 32U) = tail0;
	*(string_v16 *)(void *)(to + count - 16U) = tail1;

	/* Succeeded: the destination, as the standard asks. */
	return destination;
}
#else
/*
 * Copies a block of memory that does not overlap its destination.
 *
 * The destination is written a whole aligned word at a time between its
 * unaligned ends.
 */
void *
memcpy(
	void *destination,
	const void *source,
	size_t count)
{
	/* Copies the block front to back. */
	string_copy_forward(destination, source, count);

	/* Succeeded: the destination, as the standard asks. */
	return destination;
}
#endif

/*
 * Copies a block of memory that may overlap its destination.
 *
 * A destination below the source, or one that does not overlap it, is
 * copied front to back; a destination that overlaps the source from above
 * is copied back to front, so that every source byte is read before it is
 * overwritten.
 */
void *
memmove(
	void *destination,
	const void *source,
	size_t count)
{
	uintptr_t destination_address;
	uintptr_t source_address;
	unsigned char *destination_bytes;
	const unsigned char *source_bytes;

	/* Takes both addresses as numbers, so that their order can be compared. */
	destination_address = (uintptr_t)destination;
	source_address = (uintptr_t)source;
	destination_bytes = destination;
	source_bytes = source;

	/* A block copied onto itself is already in place. */
	if (destination_address == source_address)
		return destination;

	/* Chooses the direction that reads every source byte before overwriting it. */
	if (destination_address < source_address) {
		/* A destination below the source never overtakes it front to back. */
		string_copy_forward(destination_bytes, source_bytes, count);
	} else if (destination_address - source_address >= count) {
		/* A destination past the end of the source does not overlap it. */
		string_copy_forward(destination_bytes, source_bytes, count);
	} else {
		/* A destination overlapping the source from above is copied from the end. */
		string_copy_backward(destination_bytes + count, source_bytes + count, count);
	}

	/* Succeeded: the destination, as the standard asks. */
	return destination;
}

/*
 * Fills a block of memory with one byte value.
 *
 * The block is written a whole aligned word at a time between its
 * unaligned ends.
 */
void *
memset(
	void *destination,
	int value,
	size_t count)
{
	/* Stores the byte value over the whole block. */
	string_fill(destination, (unsigned char)value, count);

	/* Succeeded: the destination, as the standard asks. */
	return destination;
}

void *
memchr(const void *memory, int character, size_t count)
{
	const unsigned char *bytes = memory;
	unsigned char wanted = (unsigned char)character;

	while (count-- != 0) {
		if (*bytes == wanted)
			return (void *)bytes;
		bytes++;
	}
	return NULL;
}

int
memcmp(const void *left, const void *right, size_t count)
{
	const unsigned char *a = left;
	const unsigned char *b = right;

	while (count-- != 0) {
		if (*a != *b)
			return *a < *b ? -1 : 1;
		a++;
		b++;
	}
	return 0;
}

size_t
strlen(const char *string)
{
	const char *end = string;
	while (*end != '\0')
		end++;
	return (size_t)(end - string);
}

size_t
strnlen(const char *string, size_t maximum)
{
	size_t length = 0;
	while (length < maximum && string[length] != '\0')
		length++;
	return length;
}

int
strcmp(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}
	return *(const unsigned char *)left - *(const unsigned char *)right;
}

int
strncmp(const char *left, const char *right, size_t count)
{
	while (count != 0 && *left != '\0' && *left == *right) {
		left++;
		right++;
		count--;
	}
	if (count == 0)
		return 0;
	return *(const unsigned char *)left - *(const unsigned char *)right;
}

char *
strcpy(char *destination, const char *source)
{
	char *result = destination;
	while ((*destination++ = *source++) != '\0')
		;
	return result;
}

char *
strncpy(char *destination, const char *source, size_t count)
{
	char *result = destination;
	while (count != 0 && *source != '\0') {
		*destination++ = *source++;
		count--;
	}
	while (count-- != 0)
		*destination++ = '\0';
	return result;
}

char *
strcat(char *destination, const char *source)
{
	strcpy(destination + strlen(destination), source);
	return destination;
}

char *
strncat(char *destination, const char *source, size_t count)
{
	char *tail = destination + strlen(destination);
	while (count-- != 0 && *source != '\0')
		*tail++ = *source++;
	*tail = '\0';
	return destination;
}

char *
strchr(const char *string, int character)
{
	char wanted = (char)character;
	for (;;) {
		if (*string == wanted)
			return (char *)string;
		if (*string++ == '\0')
			return NULL;
	}
}

char *
strrchr(const char *string, int character)
{
	const char *last = NULL;
	char wanted = (char)character;
	for (;;) {
		if (*string == wanted)
			last = string;
		if (*string++ == '\0')
			return (char *)last;
	}
}

char *
strstr(const char *haystack, const char *needle)
{
	size_t length = strlen(needle);
	if (length == 0)
		return (char *)haystack;
	while (*haystack != '\0') {
		if (strncmp(haystack, needle, length) == 0)
			return (char *)haystack;
		haystack++;
	}
	return NULL;
}

char *
strdup(const char *string)
{
	return heap_strdup_active(string);
}

/*
 * Describes an error number.
 *
 * A number that is not an error number gets "Unknown error" and the number.
 */
char *
strerror(
	int error)
{
	const char *text;

	/* Looks the number up. */
	text = error_text(error);
	if (text == NULL) {
		error_format_unknown(error, error_unknown, sizeof(error_unknown));
		return error_unknown;
	}

	/* Succeeded: the description, which the caller must not change. */
	return (char *)text;
}

/*
 * Describes an error number into the caller's buffer.
 *
 * POSIX's reentrant form: the result is an errno value rather than a
 * pointer.  EINVAL says the number is not an error number, though the
 * buffer still receives "Unknown error" and the number; ERANGE says the
 * buffer was too small, in which case what fits is still left terminated.
 */
int
strerror_r(
	int error,
	char *buffer,
	size_t size)
{
	char unknown[ERROR_UNKNOWN_SIZE];
	const char *text;
	size_t length;
	int result;

	/* Rejects a buffer that cannot even hold a terminator. */
	if (buffer == NULL || size == 0)
		return ERANGE;

	/* Looks the number up; an unknown one is described and refused. */
	result = 0;
	text = error_text(error);
	if (text == NULL) {
		error_format_unknown(error, unknown, sizeof(unknown));
		text = unknown;
		result = EINVAL;
	}

	/* Reports a buffer too small, having filled what fits. */
	length = strlen(text);
	if (length >= size) {
		memcpy(buffer, text, size - 1U);
		buffer[size - 1U] = '\0';
		return ERANGE;
	}

	/* Copies the whole description. */
	memcpy(buffer, text, length + 1U);

	/* Reports an unknown number after describing it. */
	if (result != 0)
		return result;

	/* Succeeded: the buffer holds the description. */
	return 0;
}

/* Finds the description of an error number, or NULL for another number. */
static const char *
error_text(
	int error)
{
	size_t index;
	size_t count;

	/* Searches the table; it is short and read rarely. */
	count = sizeof(error_descriptions) / sizeof(error_descriptions[0]);
	for (index = 0; index < count; index++) {
		if (error_descriptions[index].number == error)
			return error_descriptions[index].text;
	}

	/* Not an error number. */
	return NULL;
}

/* Writes "Unknown error" and a number in decimal into a buffer. */
static void
error_format_unknown(
	int error,
	char *buffer,
	size_t size)
{
	static const char prefix[] = "Unknown error ";
	char digits[16];
	unsigned int magnitude;
	size_t count;
	size_t length;

	/* The digits of the magnitude, least significant first. */
	magnitude = (unsigned int)error;
	if (error < 0)
		magnitude = 0U - (unsigned int)error;
	count = 0;
	do {
		digits[count] = (char)('0' + magnitude % 10U);
		count++;
		magnitude /= 10U;
	} while (magnitude != 0U);

	/* The prefix, the sign, then the digits most significant first. */
	length = 0;
	while (prefix[length] != '\0' && length + 1U < size) {
		buffer[length] = prefix[length];
		length++;
	}

	/* A negative number keeps its sign. */
	if (error < 0 && length + 1U < size) {
		buffer[length] = '-';
		length++;
	}

	/* The digits, as many as fit. */
	while (count > 0 && length + 1U < size) {
		count--;
		buffer[length] = digits[count];
		length++;
	}

	/* Terminates the description. */
	buffer[length] = '\0';
}

/* Copies a block front to back, storing whole aligned words where it can. */
static void
string_copy_forward(
	unsigned char *destination,
	const unsigned char *source,
	size_t count)
{
	size_t words;
	size_t word_bytes;

	/* A block long enough to be worth aligning is copied mostly by words. */
	if (count >= STRING_WORD_MINIMUM) {
		/* Copies single bytes up to the destination's first word boundary. */
		while (((uintptr_t)destination & STRING_WORD_MASK) != 0) {
			*destination = *source;
			destination++;
			source++;
			count--;
		}

		/* Copies the whole words, reading the source as its alignment allows. */
		words = count / STRING_WORD_SIZE;
		if (((uintptr_t)source & STRING_WORD_MASK) == 0) {
			string_copy_words(
				(string_word *)(void *)destination,
				(const string_word *)(const void *)source,
				words);
		} else {
			string_copy_unaligned_words(
				(string_word *)(void *)destination,
				(const string_unaligned_word *)(const void *)source,
				words);
		}

		/* Steps past the words just copied. */
		word_bytes = words * STRING_WORD_SIZE;
		destination += word_bytes;
		source += word_bytes;
		count -= word_bytes;
	}

	/* Copies the bytes after the last whole word, or a short block. */
	while (count != 0) {
		*destination = *source;
		destination++;
		source++;
		count--;
	}
}

/* Copies a block back to front from the ends given, storing whole aligned words where it can. */
static void
string_copy_backward(
	unsigned char *destination_end,
	const unsigned char *source_end,
	size_t count)
{
	size_t words;
	size_t word_bytes;

	/* A block long enough to be worth aligning is copied mostly by words. */
	if (count >= STRING_WORD_MINIMUM) {
		/* Copies single bytes down to the destination's last word boundary. */
		while (((uintptr_t)destination_end & STRING_WORD_MASK) != 0) {
			destination_end--;
			source_end--;
			*destination_end = *source_end;
			count--;
		}

		/* Copies the whole words, reading the source as its alignment allows. */
		words = count / STRING_WORD_SIZE;
		if (((uintptr_t)source_end & STRING_WORD_MASK) == 0) {
			string_copy_words_backward(
				(string_word *)(void *)destination_end,
				(const string_word *)(const void *)source_end,
				words);
		} else {
			string_copy_unaligned_words_backward(
				(string_word *)(void *)destination_end,
				(const string_unaligned_word *)(const void *)source_end,
				words);
		}

		/* Steps back past the words just copied. */
		word_bytes = words * STRING_WORD_SIZE;
		destination_end -= word_bytes;
		source_end -= word_bytes;
		count -= word_bytes;
	}

	/* Copies the bytes before the first whole word, or a short block. */
	while (count != 0) {
		destination_end--;
		source_end--;
		*destination_end = *source_end;
		count--;
	}
}

/* Copies aligned words front to back from an aligned source. */
static void
string_copy_words(
	string_word *destination,
	const string_word *source,
	size_t words)
{
	string_word first;
	string_word second;
	string_word third;
	string_word fourth;

	/*
	 * Copies a run of words a pass.  The run is loaded before it is stored,
	 * which memmove relies on when the destination lies below the source.
	 */
	while (words >= STRING_WORD_RUN) {
		first = source[0];
		second = source[1];
		third = source[2];
		fourth = source[3];
		destination[0] = first;
		destination[1] = second;
		destination[2] = third;
		destination[3] = fourth;
		destination += STRING_WORD_RUN;
		source += STRING_WORD_RUN;
		words -= STRING_WORD_RUN;
	}

	/* Copies the words that do not fill a run. */
	while (words != 0) {
		*destination = *source;
		destination++;
		source++;
		words--;
	}
}

/* Copies aligned words front to back from a source that is not word aligned. */
static void
string_copy_unaligned_words(
	string_word *destination,
	const string_unaligned_word *source,
	size_t words)
{
	string_word first;
	string_word second;
	string_word third;
	string_word fourth;

	/*
	 * Copies a run of words a pass.  The run is loaded before it is stored,
	 * which memmove relies on when the destination lies below the source.
	 */
	while (words >= STRING_WORD_RUN) {
		first = source[0];
		second = source[1];
		third = source[2];
		fourth = source[3];
		destination[0] = first;
		destination[1] = second;
		destination[2] = third;
		destination[3] = fourth;
		destination += STRING_WORD_RUN;
		source += STRING_WORD_RUN;
		words -= STRING_WORD_RUN;
	}

	/* Copies the words that do not fill a run. */
	while (words != 0) {
		*destination = *source;
		destination++;
		source++;
		words--;
	}
}

/* Copies aligned words back to front from the ends of an aligned source. */
static void
string_copy_words_backward(
	string_word *destination_end,
	const string_word *source_end,
	size_t words)
{
	string_word first;
	string_word second;
	string_word third;
	string_word fourth;

	/*
	 * Copies a run of words a pass, last word first.  The run is loaded
	 * before it is stored, so a destination overlapping the source from
	 * above overwrites only source words already read.
	 */
	while (words >= STRING_WORD_RUN) {
		destination_end -= STRING_WORD_RUN;
		source_end -= STRING_WORD_RUN;
		first = source_end[3];
		second = source_end[2];
		third = source_end[1];
		fourth = source_end[0];
		destination_end[3] = first;
		destination_end[2] = second;
		destination_end[1] = third;
		destination_end[0] = fourth;
		words -= STRING_WORD_RUN;
	}

	/* Copies the words that do not fill a run. */
	while (words != 0) {
		destination_end--;
		source_end--;
		*destination_end = *source_end;
		words--;
	}
}

/* Copies aligned words back to front from the ends of a source that is not word aligned. */
static void
string_copy_unaligned_words_backward(
	string_word *destination_end,
	const string_unaligned_word *source_end,
	size_t words)
{
	string_word first;
	string_word second;
	string_word third;
	string_word fourth;

	/*
	 * Copies a run of words a pass, last word first.  The run is loaded
	 * before it is stored, so a destination overlapping the source from
	 * above overwrites only source words already read.
	 */
	while (words >= STRING_WORD_RUN) {
		destination_end -= STRING_WORD_RUN;
		source_end -= STRING_WORD_RUN;
		first = source_end[3];
		second = source_end[2];
		third = source_end[1];
		fourth = source_end[0];
		destination_end[3] = first;
		destination_end[2] = second;
		destination_end[1] = third;
		destination_end[0] = fourth;
		words -= STRING_WORD_RUN;
	}

	/* Copies the words that do not fill a run. */
	while (words != 0) {
		destination_end--;
		source_end--;
		*destination_end = *source_end;
		words--;
	}
}

/* Stores one byte value over a block, storing whole aligned words where it can. */
static void
string_fill(
	unsigned char *destination,
	unsigned char value,
	size_t count)
{
	string_word pattern;
	size_t words;
	size_t word_bytes;

	/* A block long enough to be worth aligning is filled mostly by words. */
	if (count >= STRING_WORD_MINIMUM) {
		/* Stores single bytes up to the destination's first word boundary. */
		while (((uintptr_t)destination & STRING_WORD_MASK) != 0) {
			*destination = value;
			destination++;
			count--;
		}

		/*
		 * Repeats the byte in every byte of a word: the all-ones word divided
		 * by 0xff has 0x01 in every byte.
		 */
		pattern = (~(string_word)0 / 0xffU) * value;

		/* Stores the whole words. */
		words = count / STRING_WORD_SIZE;
		string_fill_words((string_word *)(void *)destination, pattern, words);

		/* Steps past the words just stored. */
		word_bytes = words * STRING_WORD_SIZE;
		destination += word_bytes;
		count -= word_bytes;
	}

	/* Stores the bytes after the last whole word, or a short block. */
	while (count != 0) {
		*destination = value;
		destination++;
		count--;
	}
}

/* Stores one pattern over aligned words. */
static void
string_fill_words(
	string_word *destination,
	string_word pattern,
	size_t words)
{
	/* Stores a run of words a pass. */
	while (words >= STRING_WORD_RUN) {
		destination[0] = pattern;
		destination[1] = pattern;
		destination[2] = pattern;
		destination[3] = pattern;
		destination += STRING_WORD_RUN;
		words -= STRING_WORD_RUN;
	}

	/* Stores the words that do not fill a run. */
	while (words != 0) {
		*destination = pattern;
		destination++;
		words--;
	}
}
