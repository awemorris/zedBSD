/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The per-thread error queue.
 */

#include "cryc-err.h"
#include "cryc-os.h"

#include <stdlib.h>

/*
 * The layout of a code: the library in the bits from 23 up, the reason in
 * the 23 bits below.
 * Values referenced: OpenSSL 3.5.8 include/openssl/err.h.in
 * (ERR_LIB_OFFSET, ERR_LIB_MASK, ERR_REASON_MASK).
 */
#define ERR_LIBRARY_SHIFT	23U
#define ERR_LIBRARY_FIELD	0xffUL
#define ERR_REASON_FIELD	0x7fffffUL

/* The most characters of "reason(<number>)" and "lib(<number>)" digits: enough for a 64-bit unsigned long. */
#define ERR_DECIMAL_DIGITS	24U

/* The hex digits of a code, at least this many (the "%08lX" of the text). */
#define ERR_HEX_MIN		8U

/* The hex digits of the widest unsigned long. */
#define ERR_HEX_MAX		(sizeof(unsigned long) * 2U)

/*
 * One thread's queue: the codes in a ring, oldest first from index first,
 * count of them.  Allocated by the first cryc_err_put of the thread and
 * freed by err_queue_destroy when the thread exits.
 */
struct err_queue {
	unsigned long codes[CRYC_ERR_QUEUE];
	unsigned first;
	unsigned count;
};

/*
 * A text being written into the caller's buffer: length bytes at buffer,
 * used of them filled (the last byte is kept for the NUL).
 */
struct err_output {
	char *buffer;
	size_t length;
	size_t used;
};

/* The name of a library number. */
struct err_library_name {
	unsigned library;
	const char *name;
};

/* The text of a reason; library 0 means the reason is common to every library. */
struct err_reason_text {
	unsigned library;
	unsigned reason;
	const char *text;
};

/*
 * The library names.
 * Values referenced: OpenSSL 3.5.8 crypto/err/err.c (ERR_str_libraries, the
 * entries for the library numbers of cryc-err.h and ERR_LIB_NONE and
 * ERR_LIB_PEM).
 */
static const struct err_library_name err_library_names[] = {
	{ 1U, "unknown library" },
	{ CRYC_LIB_SYS, "system library" },
	{ CRYC_LIB_EVP, "digital envelope routines" },
	{ 9U, "PEM routines" },
	{ CRYC_LIB_X509, "x509 certificate routines" },
	{ CRYC_LIB_ASN1, "asn1 encoding routines" },
	{ CRYC_LIB_CRYPTO, "common libcrypto routines" },
	{ CRYC_LIB_SSL, "SSL routines" },
	{ CRYC_LIB_RAND, "random number generator" }
};

/* The digits of a hex number, indexed by the value of a nibble. */
static const char err_hex_chars[] = "0123456789ABCDEF";

/*
 * The reason texts of libcrypto-compat's own reasons (the wording is its
 * own; the numbers are those of cryc-err.h).
 */
static const struct err_reason_text err_reason_texts[] = {
	{ 0U, CRYC_R_MALLOC_FAILURE, "out of memory" },
	{ 0U, CRYC_R_PASSED_NULL_PARAMETER, "a required argument is NULL" },
	{ 0U, CRYC_R_INTERNAL_ERROR, "internal error" },
	{ 0U, CRYC_R_PASSED_INVALID_ARGUMENT, "an argument is not valid" },
	{ 0U, CRYC_R_UNSUPPORTED, "not supported" },
	{ CRYC_LIB_ASN1, CRYC_R_ASN1_WRONG_TAG, "unexpected ASN.1 tag" },
	{ CRYC_LIB_ASN1, CRYC_R_ASN1_HEADER_TOO_LONG, "ASN.1 header too long" },
	{ CRYC_LIB_X509, CRYC_R_X509_INVALID_ATTRIBUTES, "invalid attributes" },
	{ CRYC_LIB_EVP, CRYC_R_EVP_DIFFERENT_KEY_TYPES, "the key types differ" },
	{ CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY_LENGTH, "invalid key length" },
	{ CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED, "operation not initialized" },
	{ CRYC_LIB_EVP, CRYC_R_EVP_BUFFER_TOO_SMALL, "buffer too small" },
	{ CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM, "unsupported algorithm" },
	{ CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY, "invalid key" }
};

/*
 * libssl-compat's reason numbers and texts, set once by
 * cryc_err_register_ssl before err_ssl_ready becomes nonzero and constant
 * after that (the tables are the caller's, constant for the life of the
 * process).
 */
static const unsigned *err_ssl_reasons;
static const char *const *err_ssl_texts;
static size_t err_ssl_count;

/*
 * Counts the calls of cryc_err_register_ssl that passed a table: the
 * caller that makes it 1 owns the registration, every later one is a no-op.
 * Changed only with cryc_os_atomic_add.
 */
static volatile long err_ssl_claimed;

/*
 * Becomes 1 (with cryc_os_atomic_add, a full barrier) when the three
 * variables above are final, so that a reader that sees 1 sees the tables.
 * Zero means no table is registered.
 */
static volatile long err_ssl_ready;

/* Forward declarations. */
static void err_queue_destroy(void *value);
static struct err_queue *err_queue_make(void);
static void err_emit_char(struct err_output *output, char character);
static void err_emit_text(struct err_output *output, const char *text);
static void err_emit_hex(struct err_output *output, unsigned long value);
static void err_emit_decimal(struct err_output *output, unsigned long value);
static const char *err_library_name(unsigned library);
static const char *err_reason_text(unsigned library, unsigned reason);

/*
 * Packs a library and a reason into a code.
 */
unsigned long
cryc_err_pack(
	unsigned library,
	unsigned reason)
{
	unsigned long code;

	/* The library goes above the reason; both are cut to their fields. */
	code = ((unsigned long)library & ERR_LIBRARY_FIELD) << ERR_LIBRARY_SHIFT;
	code |= (unsigned long)reason & ERR_REASON_FIELD;

	/* The packed code. */
	return code;
}

/*
 * Adds a code to the calling thread's queue (dropping the oldest when
 * full; dropped silently without memory).
 */
void
cryc_err_put(
	unsigned library,
	unsigned reason)
{
	struct err_queue *queue;
	unsigned long code;
	unsigned index;

	/* The calling thread's queue, if it has one yet. */
	queue = (struct err_queue *)cryc_os_thread_value();

	/* The first error of a thread makes its queue; without memory the code is dropped. */
	if (queue == NULL) {
		queue = err_queue_make();
		if (queue == NULL)
			return;
	}

	/* Packs the code. */
	code = cryc_err_pack(library, reason);

	/* A full queue drops its oldest code to make room. */
	if (queue->count == CRYC_ERR_QUEUE) {
		queue->first = (queue->first + 1U) % CRYC_ERR_QUEUE;
		queue->count--;
	}

	/* Appends the code after the newest one. */
	index = (queue->first + queue->count) % CRYC_ERR_QUEUE;
	queue->codes[index] = code;
	queue->count++;
}

/*
 * Removes and returns the oldest code, or 0 when empty (ERR_get_error).
 */
unsigned long
cryc_err_get(void)
{
	struct err_queue *queue;
	unsigned long code;

	/* A thread that never queued an error has no queue. */
	queue = (struct err_queue *)cryc_os_thread_value();
	if (queue == NULL)
		return 0;

	/* An empty queue holds no code. */
	if (queue->count == 0)
		return 0;

	/* Takes the oldest code out of the ring. */
	code = queue->codes[queue->first];
	queue->first = (queue->first + 1U) % CRYC_ERR_QUEUE;
	queue->count--;

	/* The code that was oldest. */
	return code;
}

/*
 * Returns the oldest code without removing it, or 0 (ERR_peek_error).
 */
unsigned long
cryc_err_peek(void)
{
	struct err_queue *queue;
	unsigned long code;

	/* A thread that never queued an error has no queue. */
	queue = (struct err_queue *)cryc_os_thread_value();
	if (queue == NULL)
		return 0;

	/* An empty queue holds no code. */
	if (queue->count == 0)
		return 0;

	/* Reads the oldest code and leaves it in the ring. */
	code = queue->codes[queue->first];

	/* The code that is oldest. */
	return code;
}

/*
 * Empties the calling thread's queue (ERR_clear_error).
 */
void
cryc_err_clear(void)
{
	struct err_queue *queue;

	/* A thread without a queue has nothing to clear. */
	queue = (struct err_queue *)cryc_os_thread_value();
	if (queue == NULL)
		return;

	/* Forgets every code; the memory stays for the next error. */
	queue->first = 0;
	queue->count = 0;
}

/*
 * Writes "error:%08lX:<library>::<reason>" for code into buffer (length
 * bytes, NUL-terminated, cut to fit; nothing when length is 0).
 */
void
cryc_err_string(
	unsigned long code,
	char *buffer,
	size_t length)
{
	struct err_output output;
	const char *library_name;
	const char *reason_text;
	unsigned library;
	unsigned reason;

	/* A buffer without room takes nothing, not even the NUL. */
	if (buffer == NULL || length == 0)
		return;

	/* Splits the code into its two fields. */
	library = (unsigned)((code >> ERR_LIBRARY_SHIFT) & ERR_LIBRARY_FIELD);
	reason = (unsigned)(code & ERR_REASON_FIELD);

	/* Looks up the two names; a number without a name is written as a number. */
	library_name = err_library_name(library);
	reason_text = err_reason_text(library, reason);

	/* Starts the output at the first byte of the buffer. */
	output.buffer = buffer;
	output.length = length;
	output.used = 0;

	/* Writes the fixed part and the code in hex. */
	err_emit_text(&output, "error:");
	err_emit_hex(&output, code);
	err_emit_char(&output, ':');

	/* Writes the library: its name, or "lib(<number>)". */
	if (library_name != NULL) {
		err_emit_text(&output, library_name);
	} else {
		err_emit_text(&output, "lib(");
		err_emit_decimal(&output, (unsigned long)library);
		err_emit_char(&output, ')');
	}

	/* Writes the empty function field. */
	err_emit_text(&output, "::");

	/* Writes the reason: its text, or "reason(<number>)". */
	if (reason_text != NULL) {
		err_emit_text(&output, reason_text);
	} else {
		err_emit_text(&output, "reason(");
		err_emit_decimal(&output, (unsigned long)reason);
		err_emit_char(&output, ')');
	}

	/* Ends the text; used is below length because every write leaves a byte for this. */
	output.buffer[output.used] = '\0';
}

/*
 * Registers libssl-compat's reason texts: table is count pairs of a reason
 * number and its text, constant for the life of the process.
 */
void
cryc_err_register_ssl(
	const unsigned *reasons,
	const char *const *texts,
	size_t count)
{
	long claimed;

	/* A call without a table registers nothing and does not use up the registration. */
	if (reasons == NULL || texts == NULL || count == 0)
		return;

	/*
	 * Claims the registration: the first call to get 1 fills the tables,
	 * any later call (the same table again, or another) changes nothing.
	 */
	claimed = cryc_os_atomic_add(&err_ssl_claimed, 1);
	if (claimed != 1)
		return;

	/* Records the caller's tables. */
	err_ssl_reasons = reasons;
	err_ssl_texts = texts;
	err_ssl_count = count;

	/* Publishes the tables to the readers (the add is a full barrier). */
	cryc_os_atomic_add(&err_ssl_ready, 1);
}

/*
 * Frees a thread's queue when the thread exits.
 */
static void
err_queue_destroy(
	void *value)
{
	/* Frees the queue (it holds no secret). */
	free(value);
}

/*
 * Allocates the calling thread's queue and makes it the thread's value;
 * NULL when memory or the per-thread slot is not available.
 */
static struct err_queue *
err_queue_make(void)
{
	struct err_queue *queue;
	int error;

	/* Allocates the queue. */
	queue = (struct err_queue *)malloc(sizeof(*queue));
	if (queue == NULL)
		return NULL;

	/* Starts the queue empty. */
	queue->first = 0;
	queue->count = 0;

	/* Stores the queue as the thread's value, with the function that frees it at the thread's exit. */
	error = cryc_os_thread_set_value(queue, err_queue_destroy);
	if (error != 0) {
		free(queue);
		return NULL;
	}

	/* Succeeded: the thread now owns the queue. */
	return queue;
}

/*
 * Writes one character when there is still room for it and the NUL.
 */
static void
err_emit_char(
	struct err_output *output,
	char character)
{
	/* The last byte of the buffer is kept for the NUL. */
	if (output->used + 1U >= output->length)
		return;

	/* Appends the character. */
	output->buffer[output->used] = character;
	output->used++;
}

/*
 * Writes a string, cut where the buffer ends.
 */
static void
err_emit_text(
	struct err_output *output,
	const char *text)
{
	size_t index;

	/* Writes every character of the text; err_emit_char stops at the end of the buffer. */
	for (index = 0; text[index] != '\0'; index++)
		err_emit_char(output, text[index]);
}

/*
 * Writes value as upper-case hex with at least ERR_HEX_MIN digits.
 */
static void
err_emit_hex(
	struct err_output *output,
	unsigned long value)
{
	char digits[ERR_HEX_MAX];
	unsigned long rest;
	size_t count;
	size_t index;

	/* Takes the digits from the lowest, so they are in reverse order; at least one is made. */
	count = 0;
	rest = value;
	digits[count] = err_hex_chars[rest & 0xfUL];
	count++;
	rest >>= 4;
	while (rest != 0) {
		digits[count] = err_hex_chars[rest & 0xfUL];
		count++;
		rest >>= 4;
	}

	/* Pads with zeros up to the minimum width. */
	for (index = count; index < ERR_HEX_MIN; index++)
		err_emit_char(output, '0');

	/* Writes the digits from the highest. */
	for (index = count; index > 0; index--)
		err_emit_char(output, digits[index - 1U]);
}

/*
 * Writes value in decimal.
 */
static void
err_emit_decimal(
	struct err_output *output,
	unsigned long value)
{
	char digits[ERR_DECIMAL_DIGITS];
	unsigned long rest;
	size_t count;
	size_t index;

	/* Takes the digits from the lowest, so they are in reverse order; at least one is made. */
	count = 0;
	rest = value;
	digits[count] = (char)('0' + (int)(rest % 10UL));
	count++;
	rest /= 10UL;
	while (rest != 0) {
		digits[count] = (char)('0' + (int)(rest % 10UL));
		count++;
		rest /= 10UL;
	}

	/* Writes the digits from the highest. */
	for (index = count; index > 0; index--)
		err_emit_char(output, digits[index - 1U]);
}

/*
 * Finds the name of a library number; NULL when it has none.
 */
static const char *
err_library_name(
	unsigned library)
{
	size_t index;

	/* Looks for the number in the table. */
	for (index = 0; index < sizeof(err_library_names) / sizeof(err_library_names[0]); index++) {
		/* A table entry for this number gives the name. */
		if (err_library_names[index].library == library)
			return err_library_names[index].name;
	}

	/* The library has no name. */
	return NULL;
}

/*
 * Finds the text of a reason of a library: libcrypto-compat's own table
 * first, then libssl-compat's registered one for the SSL library; NULL when
 * there is none.
 */
static const char *
err_reason_text(
	unsigned library,
	unsigned reason)
{
	long ready;
	size_t index;

	/* Looks for the reason among libcrypto-compat's, common reasons matching in every library. */
	for (index = 0; index < sizeof(err_reason_texts) / sizeof(err_reason_texts[0]); index++) {
		/* An entry for another reason is skipped. */
		if (err_reason_texts[index].reason != reason)
			continue;

		/* An entry of another library is skipped; a common one (library 0) fits every library. */
		if (err_reason_texts[index].library != 0U && err_reason_texts[index].library != library)
			continue;

		/* This entry fits. */
		return err_reason_texts[index].text;
	}

	/* Only the SSL library has registered reasons. */
	if (library != CRYC_LIB_SSL)
		return NULL;

	/* Reads whether the tables are published (an add of 0 is an atomic read with a barrier). */
	ready = cryc_os_atomic_add(&err_ssl_ready, 0);
	if (ready == 0)
		return NULL;

	/* Looks for the reason among the registered ones. */
	for (index = 0; index < err_ssl_count; index++) {
		/* The entry with this number gives the text (NULL when the table has none for it). */
		if (err_ssl_reasons[index] == reason)
			return err_ssl_texts[index];
	}

	/* No registered reason matches. */
	return NULL;
}
