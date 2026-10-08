/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * IPP (RFC 8010, 8011; plan/ws145/design.md §5.4) over plain HTTP/1.1,
 * one request a connection: Get-Printer-Attributes (the printer's name,
 * whether it takes PDF, and the path that answers), Print-Job with the
 * document after the message, Get-Job-Attributes every five seconds until
 * the job ends, and Cancel-Job.  Version 2.0 first, 1.1 when the printer
 * says it does not take 2.0.
 *
 * A response is read within bounds (ws177-p022, design §5.3): a line of
 * 1024 bytes, 16 KiB of header, and a body decoded as it comes, each value
 * kept to 1024 bytes (a longer one passed over) and 256 attributes' names
 * looked at (the rest passed over), so that a printer that answers with
 * every attribute it has is still understood.
 */

#include "printd.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The operations. */
#define IPP_PRINT_JOB		0x0002U
#define IPP_CANCEL_JOB		0x0008U
#define IPP_GET_JOB		0x0009U
#define IPP_GET_PRINTER		0x000bU

/* The delimiter tags, and the value tags written and read. */
#define IPP_OPERATION_GROUP	0x01U
#define IPP_JOB_GROUP		0x02U
#define IPP_END			0x03U
#define IPP_INTEGER		0x21U
#define IPP_ENUM		0x23U
#define IPP_TEXT_LANGUAGE	0x35U
#define IPP_NAME_LANGUAGE	0x36U
#define IPP_TEXT		0x41U
#define IPP_NAME		0x42U
#define IPP_KEYWORD		0x44U
#define IPP_URI			0x45U
#define IPP_CHARSET		0x47U
#define IPP_LANGUAGE		0x48U
#define IPP_MIME		0x49U

/* The status codes read. */
#define IPP_NOT_AUTHENTICATED	0x0402U
#define IPP_NOT_AUTHORIZED	0x0403U
#define IPP_NOT_FOUND		0x0406U
#define IPP_FORMAT		0x040aU
#define IPP_NOT_ACCEPTING	0x0506U
#define IPP_BUSY		0x0507U
#define IPP_VERSION		0x0503U

/* The job states. */
#define IPP_CANCELED		7
#define IPP_ABORTED		8
#define IPP_COMPLETED		9

/*
 * The bounds of a response: a line, the header, a value kept, the
 * attributes' names looked at, and the body read at most.
 */
#define IPP_LINE_MAX		1024U
#define IPP_HEADER_MAX		(16U * 1024U)
#define IPP_VALUE_MAX		1024U
#define IPP_NAMES_MAX		256U
#define IPP_BODY_MOST		(16U * 1024U * 1024U)

/* The stages of a response's IPP message as it is decoded. */
#define IPP_STAGE_HEADER	0
#define IPP_STAGE_TAG		1
#define IPP_STAGE_NAME_LENGTH	2
#define IPP_STAGE_NAME		3
#define IPP_STAGE_VALUE_LENGTH	4
#define IPP_STAGE_VALUE		5
#define IPP_STAGE_END		6

/* How long and how often a job is watched (seconds). */
#define IPP_WATCH_SECONDS	(30 * 60)
#define IPP_WATCH_EVERY		5
#define IPP_BUSY_TRIES		3
#define IPP_BUSY_WAIT		30

/* A message being written. */
struct ipp_message {
	unsigned char *data;
	size_t length;
	size_t room;
	int failed;
};

/*
 * What a response said: the HTTP status, the IPP status and request-id,
 * the job's id and state (-1 for none), whether the printer lists PDF and
 * lists formats at all, and its info and model.
 */
struct ipp_answer {
	int http;
	unsigned status;
	uint32_t request;
	int job_id;
	int job_state;
	int has_pdf;
	int has_formats;
	char info[PD_NAME_MAX];
	char model[PD_NAME_MAX];
};

/*
 * A response's IPP message decoded as its bytes come: the stage and the
 * bytes it still needs, the small fields gathered, the attribute's tag,
 * name (its first bytes) and value (its first IPP_VALUE_MAX bytes) with
 * their lengths, the name the next values belong to, the names counted,
 * and the answer filled.
 */
struct ipp_stream {
	struct ipp_answer *answer;
	int stage;
	size_t need;
	size_t have;
	unsigned char small[8];
	unsigned tag;
	char name[64];
	size_t name_length;
	unsigned char value[IPP_VALUE_MAX];
	size_t value_length;
	char last[64];
	unsigned names;
};

/*
 * A connection's bytes read ahead: the socket, the bytes not taken yet
 * (from start to end), and whether the printer closed it.
 */
struct ipp_reader {
	int fd;
	unsigned char data[4096];
	size_t start;
	size_t end;
	int closed;
};

/* The request-ids, one after another for the daemon's life. */
static uint32_t ipp_next_request = 1;
static pthread_mutex_t ipp_lock = PTHREAD_MUTEX_INITIALIZER;

static int ipp_find_path(const char *host, unsigned port, const char *first, char *path, size_t size, struct ipp_answer *answer, int *minor, const char **detail);
static int ipp_ask(const char *host, unsigned port, const char *path, unsigned operation, int minor, int job_id, const char *job_name, struct pd_job *document, struct ipp_answer *answer, const char **detail);
static void ipp_message_put(struct ipp_message *message, const void *data, size_t size);
static void ipp_message_u16(struct ipp_message *message, unsigned value);
static void ipp_attribute(struct ipp_message *message, unsigned tag, const char *name, const void *value, size_t size);
static void ipp_text_attribute(struct ipp_message *message, unsigned tag, const char *name, const char *value);
static int ipp_exchange(const char *host, unsigned port, const char *path, const struct ipp_message *message, struct pd_job *document, struct ipp_stream *stream, int *http, const char **detail);
static int ipp_read_header(struct ipp_reader *reader, int *http, long long *length, int *chunked);
static int ipp_read_body(struct ipp_reader *reader, long long length, int chunked, struct ipp_stream *stream);
static int ipp_fill(struct ipp_reader *reader);
static int ipp_line(struct ipp_reader *reader, char *line, size_t size, size_t *taken);
static int ipp_feed_from(struct ipp_reader *reader, unsigned long long count, struct ipp_stream *stream, unsigned long long *total);
static void ipp_stream_start(struct ipp_stream *stream, struct ipp_answer *answer);
static int ipp_stream_feed(struct ipp_stream *stream, const unsigned char *bytes, size_t length);
static int ipp_stream_field(struct ipp_stream *stream);
static void ipp_stream_attribute(struct ipp_stream *stream);
static int ipp_header_is(const char *line, const char *name, const char **value);
static int ipp_named(const char *name);
static void ipp_take_text(const unsigned char *value, size_t size, unsigned tag, char *text, size_t room);
static const char *ipp_detail(const struct ipp_answer *answer);
static uint32_t ipp_request_id(void);
static uint32_t ipp_be32(const unsigned char *bytes);

/*
 * Sends a job: the printer asked first (its path, whether it takes PDF),
 * then the document, then the job watched until it ends; its STATE lines
 * go to the backend.
 */
void
pd_ipp_job(
	struct pd_job *job)
{
	struct ipp_answer answer;
	const char *detail;
	char path[PD_PATH_MAX];
	time_t started;
	time_t now;
	int minor;
	int status;
	int tries;
	int job_id;
	int stop;
	int same;

	/* The printer, at its path. */
	status = ipp_find_path(job->host, job->port, job->path, path, sizeof(path), &answer, &minor, &detail);
	if (status != 0) {
		pd_send("STATE %lu failed %s", (unsigned long)job->job, detail);
		return;
	}

	/* A path of its own, kept by the backend. */
	same = strcmp(path, job->path);
	if (same != 0) {
		pd_send("PATH %lu %s", (unsigned long)job->job, path);
		(void)snprintf(job->path, sizeof(job->path), "%s", path);
	}

	/* A printer that lists its formats without PDF. */
	if (answer.has_formats && !answer.has_pdf) {
		pd_send("STATE %lu failed format", (unsigned long)job->job);
		return;
	}

	/* The document, again a few times while the printer is busy. */
	pd_send("STATE %lu sending", (unsigned long)job->job);
	for (tries = 0;; tries++) {
		status = ipp_ask(job->host, job->port, job->path, IPP_PRINT_JOB, minor, 0, job->title, job, &answer, &detail);
		if (status == ECANCELED) {
			pd_send("STATE %lu cancelled", (unsigned long)job->job);
			return;
		}

		/* Not sent. */
		if (status != 0) {
			pd_send("STATE %lu failed %s", (unsigned long)job->job, detail);
			return;
		}

		/* A busy printer is tried again later; a job asked to stop meanwhile was not taken. */
		if (answer.status != IPP_BUSY || tries + 1 >= IPP_BUSY_TRIES)
			break;
		stop = pd_wait(job, IPP_BUSY_WAIT);
		if (stop) {
			pd_send("STATE %lu cancelled", (unsigned long)job->job);
			return;
		}
	}

	/* Refused. */
	if (answer.http != 200 || answer.status > 0x00ffU) {
		pd_send("STATE %lu failed %s", (unsigned long)job->job, ipp_detail(&answer));
		return;
	}

	/* Taken by the printer: the next job for it may be sent while this one is watched. */
	pd_log("job %lu sent", (unsigned long)job->job);
	pd_sent(job);

	/* Without a job-id it cannot be watched: done, not confirmed. */
	job_id = answer.job_id;
	if (job_id <= 0) {
		pd_send("STATE %lu done unconfirmed", (unsigned long)job->job);
		return;
	}

	/* Watched from here. */
	pd_send("STATE %lu waiting", (unsigned long)job->job);

	/* Watched until it ends, asked to stop, or half an hour went. */
	started = time(NULL);
	for (;;) {
		/* Asked to stop: Cancel-Job. */
		stop = pd_cancelled(job);
		if (stop) {
			status = ipp_ask(job->host, job->port, job->path, IPP_CANCEL_JOB, minor, job_id, NULL, NULL, &answer, &detail);
			if (status == 0 && answer.http == 200 && answer.status <= 0x00ffU)
				pd_send("STATE %lu cancelled", (unsigned long)job->job);
			else
				pd_send("STATE %lu failed unconfirmed", (unsigned long)job->job);
			return;
		}

		/* Its state; a printer that does not tell it is done, not confirmed. */
		status = ipp_ask(job->host, job->port, job->path, IPP_GET_JOB, minor, job_id, NULL, NULL, &answer, &detail);
		if (status == 0 && answer.job_state < 0) {
			pd_send("STATE %lu done unconfirmed", (unsigned long)job->job);
			return;
		}

		/* Completed. */
		if (status == 0 && answer.job_state == IPP_COMPLETED) {
			pd_send("STATE %lu done", (unsigned long)job->job);
			return;
		}

		/* Cancelled at the printer. */
		if (status == 0 && answer.job_state == IPP_CANCELED) {
			pd_send("STATE %lu cancelled", (unsigned long)job->job);
			return;
		}

		/* Aborted by the printer. */
		if (status == 0 && answer.job_state == IPP_ABORTED) {
			pd_send("STATE %lu failed printer", (unsigned long)job->job);
			return;
		}

		/* Too long: done, not confirmed. */
		now = time(NULL);
		if (now - started >= IPP_WATCH_SECONDS) {
			pd_send("STATE %lu done unconfirmed", (unsigned long)job->job);
			return;
		}

		/* The next look, sooner when the job is asked to stop. */
		(void)pd_wait(job, IPP_WATCH_EVERY);
	}
}

/*
 * Answers NAME: the printer asked at the usual paths for its info or
 * model; NAMED with the path and the name, or NAMED alone.
 */
void
pd_ipp_name(
	const struct pd_name *name)
{
	struct ipp_answer answer;
	const char *detail;
	const char *text;
	char path[PD_PATH_MAX];
	int minor;
	int status;

	/* The printer at a path that answers. */
	status = ipp_find_path(name->host, name->port, "/ipp/print", path, sizeof(path), &answer, &minor, &detail);
	if (status != 0) {
		pd_send("NAMED %lu", (unsigned long)name->seq);
		return;
	}

	/* Its info, else its model, else its address. */
	text = answer.info;
	if (text[0] == '\0')
		text = answer.model;
	if (text[0] == '\0') {
		pd_send("NAMED %lu %s %s (IPP)", (unsigned long)name->seq, path, name->host);
		return;
	}

	/* Its own name. */
	pd_send("NAMED %lu %s %s", (unsigned long)name->seq, path, text);
}

/*
 * Finds the path a printer answers at: the one given, then /ipp/print,
 * /ipp and /.  Version 2.0 first, 1.1 when 2.0 is refused (*minor says
 * which).  Returns 0 with the path and the printer's answer, or an errno
 * value with the word of the failure.
 */
static int
ipp_find_path(
	const char *host,
	unsigned port,
	const char *first,
	char *path,
	size_t size,
	struct ipp_answer *answer,
	int *minor,
	const char **detail)
{
	static const char *const usual[] = { "/ipp/print", "/ipp", "/" };
	const char *tried[4];
	size_t count;
	size_t index;
	size_t seen;
	int duplicate;
	int status;

	/* The paths to try, the given one first, each once. */
	count = 0;
	tried[count] = first;
	count++;
	for (index = 0; index < 3U; index++) {
		duplicate = 0;
		for (seen = 0; seen < count; seen++)
			duplicate |= strcmp(tried[seen], usual[index]) == 0;
		if (!duplicate) {
			tried[count] = usual[index];
			count++;
		}
	}

	/* Each, until one answers. */
	*detail = "refused";
	for (index = 0; index < count; index++) {
		*minor = 0;
		status = ipp_ask(host, port, tried[index], IPP_GET_PRINTER, 0, 0, NULL, NULL, answer, detail);
		if (status == 0 && answer->status == IPP_VERSION) {
			*minor = 1;
			status = ipp_ask(host, port, tried[index], IPP_GET_PRINTER, 1, 0, NULL, NULL, answer, detail);
		}

		/* Not reached, not there, refused, or found. */
		if (status != 0)
			return status;
		if (answer->http == 404 || answer->status == IPP_NOT_FOUND)
			continue;
		if (answer->http != 200 || answer->status > 0x00ffU) {
			*detail = ipp_detail(answer);
			return EIO;
		}

		/* This path answers. */
		(void)snprintf(path, size, "%s", tried[index]);
		return 0;
	}

	/* None answered. */
	*detail = "refused";
	return ENOENT;
}

/*
 * Makes a request (its operation's attributes), sends it (with the job's
 * document after it for Print-Job) and reads the answer.  minor is 0 for
 * version 2.0, 1 for 1.1.  Returns 0 with the answer, ECANCELED, or an
 * errno value with the word of the failure.
 */
static int
ipp_ask(
	const char *host,
	unsigned port,
	const char *path,
	unsigned operation,
	int minor,
	int job_id,
	const char *job_name,
	struct pd_job *document,
	struct ipp_answer *answer,
	const char **detail)
{
	static const char *const printer_wanted[] = { "printer-state", "document-format-supported", "printer-info", "printer-make-and-model" };
	static const char *const job_wanted[] = { "job-state", "job-state-reasons" };
	struct ipp_message message;
	struct ipp_stream stream;
	unsigned char number[4];
	char uri[256];
	size_t index;
	uint32_t request;
	int status;

	/* The header: version, operation, request-id. */
	memset(&message, 0, sizeof(message));
	memset(answer, 0, sizeof(*answer));
	answer->job_state = -1;
	request = ipp_request_id();
	if (minor)
		ipp_message_u16(&message, 0x0101U);
	else
		ipp_message_u16(&message, 0x0200U);
	ipp_message_u16(&message, operation);
	number[0] = (unsigned char)(request >> 24);
	number[1] = (unsigned char)(request >> 16);
	number[2] = (unsigned char)(request >> 8);
	number[3] = (unsigned char)request;
	ipp_message_put(&message, number, 4U);

	/* The operation's attributes, in their order. */
	(void)snprintf(uri, sizeof(uri), "ipp://%s:%u%s", host, port, path);
	ipp_message_put(&message, "\x01", 1U);
	ipp_text_attribute(&message, IPP_CHARSET, "attributes-charset", "utf-8");
	ipp_text_attribute(&message, IPP_LANGUAGE, "attributes-natural-language", "en");
	ipp_text_attribute(&message, IPP_URI, "printer-uri", uri);
	if (job_id > 0) {
		number[0] = (unsigned char)((uint32_t)job_id >> 24);
		number[1] = (unsigned char)((uint32_t)job_id >> 16);
		number[2] = (unsigned char)((uint32_t)job_id >> 8);
		number[3] = (unsigned char)job_id;
		ipp_attribute(&message, IPP_INTEGER, "job-id", number, 4U);
	}

	/* Who asks, and the operation's own. */
	ipp_text_attribute(&message, IPP_NAME, "requesting-user-name", pd_user());
	if (operation == IPP_PRINT_JOB) {
		if (job_name == NULL || job_name[0] == '\0')
			job_name = "Document";
		ipp_text_attribute(&message, IPP_NAME, "job-name", job_name);
		ipp_text_attribute(&message, IPP_MIME, "document-format", "application/pdf");
	}

	/* The printer's attributes wanted: the name once, then the other values. */
	if (operation == IPP_GET_PRINTER) {
		ipp_text_attribute(&message, IPP_KEYWORD, "requested-attributes", printer_wanted[0]);
		for (index = 1; index < 4U; index++)
			ipp_text_attribute(&message, IPP_KEYWORD, "", printer_wanted[index]);
	}

	/* The job's attributes wanted. */
	if (operation == IPP_GET_JOB) {
		ipp_text_attribute(&message, IPP_KEYWORD, "requested-attributes", job_wanted[0]);
		ipp_text_attribute(&message, IPP_KEYWORD, "", job_wanted[1]);
	}

	/* The end of the attributes. */
	ipp_message_put(&message, "\x03", 1U);
	if (message.failed) {
		free(message.data);
		*detail = "io";
		return ENOMEM;
	}

	/* Sent and answered, the answer decoded as it came. */
	ipp_stream_start(&stream, answer);
	status = ipp_exchange(host, port, path, &message, document, &stream, &answer->http, detail);
	free(message.data);
	if (status != 0)
		return status;

	/* A whole IPP message for this request; a refusal by HTTP is only its status. */
	if (answer->http == 200) {
		if (stream.stage != IPP_STAGE_END || answer->request != request) {
			*detail = "protocol";
			return EPROTO;
		}
	}

	/* Succeeded: the answer. */
	return 0;
}

/* Appends bytes to a message (a failure marks it failed). */
static void
ipp_message_put(
	struct ipp_message *message,
	const void *data,
	size_t size)
{
	unsigned char *grown;
	size_t room;

	/* Room. */
	if (message->failed)
		return;
	if (message->length + size > message->room) {
		room = message->room * 2U + size + 256U;
		grown = realloc(message->data, room);
		if (grown == NULL) {
			message->failed = 1;
			return;
		}

		/* Grown. */
		message->data = grown;
		message->room = room;
	}

	/* The bytes. */
	memcpy(message->data + message->length, data, size);
	message->length += size;
}

/* Appends a big-endian 16-bit number. */
static void
ipp_message_u16(
	struct ipp_message *message,
	unsigned value)
{
	unsigned char bytes[2];

	/* High byte first. */
	bytes[0] = (unsigned char)(value >> 8);
	bytes[1] = (unsigned char)value;
	ipp_message_put(message, bytes, 2U);
}

/* Appends an attribute: its tag, its name (empty for another value of the one before) and its value. */
static void
ipp_attribute(
	struct ipp_message *message,
	unsigned tag,
	const char *name,
	const void *value,
	size_t size)
{
	unsigned char byte;

	/* The tag, the name, the value. */
	byte = (unsigned char)tag;
	ipp_message_put(message, &byte, 1U);
	ipp_message_u16(message, (unsigned)strlen(name));
	ipp_message_put(message, name, strlen(name));
	ipp_message_u16(message, (unsigned)size);
	ipp_message_put(message, value, size);
}

/* Appends an attribute whose value is a text. */
static void
ipp_text_attribute(
	struct ipp_message *message,
	unsigned tag,
	const char *name,
	const char *value)
{
	/* The text's bytes. */
	ipp_attribute(message, tag, name, value, strlen(value));
}

/*
 * Sends a message by HTTP POST (with a job's document after it) and reads
 * the response within its bounds: its final status, and for 200 its body
 * fed to the stream.  Returns 0, ECANCELED, or an errno value with the
 * word of the failure.
 */
static int
ipp_exchange(
	const char *host,
	unsigned port,
	const char *path,
	const struct ipp_message *message,
	struct pd_job *document,
	struct ipp_stream *stream,
	int *http,
	const char **detail)
{
	struct ipp_reader *reader;
	char header[512];
	long long declared;
	uint64_t length;
	int chunked;
	int status;
	int fd;

	/* The connection. */
	*http = 0;
	status = pd_connect(host, port, &fd, detail);
	if (status != 0)
		return status;

	/* The request's header and the message, then the document. */
	length = message->length;
	if (document != NULL)
		length += document->size;
	(void)snprintf(header, sizeof(header),
	    "POST %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: application/ipp\r\nContent-Length: %llu\r\nConnection: close\r\n\r\n",
	    path, host, port, (unsigned long long)length);
	status = pd_write_all(fd, header, strlen(header));
	if (status == 0)
		status = pd_write_all(fd, message->data, message->length);
	if (status == 0 && document != NULL)
		status = pd_send_file(fd, document);
	if (status != 0) {
		(void)close(fd);
		*detail = "io";
		if (status == ETIMEDOUT || status == EAGAIN)
			*detail = "timeout";
		return status;
	}

	/* The reader of the response. */
	reader = calloc(1, sizeof(*reader));
	if (reader == NULL) {
		(void)close(fd);
		*detail = "io";
		return ENOMEM;
	}

	/* The reader reads the connection. */
	reader->fd = fd;

	/* The final status and how the body comes. */
	status = ipp_read_header(reader, http, &declared, &chunked);

	/* A body of IPP is decoded; another status needs no body. */
	if (status == 0 && *http == 200)
		status = ipp_read_body(reader, declared, chunked, stream);

	/* The connection ends. */
	(void)close(fd);
	free(reader);

	/* Not read: the printer stood still, or broke the protocol. */
	if (status != 0) {
		*detail = "protocol";
		if (status == ETIMEDOUT || status == EAGAIN)
			*detail = "timeout";
		return status;
	}

	/* Succeeded: the response read. */
	*detail = "";
	return 0;
}

/*
 * Reads a response's header past any 1xx ones: the final status, the
 * body's declared length (-1 for none) and whether it is chunked.  Lines
 * are at most IPP_LINE_MAX bytes and the header IPP_HEADER_MAX.  Returns
 * 0, EPROTO, or the read's errno.
 */
static int
ipp_read_header(
	struct ipp_reader *reader,
	int *http,
	long long *length,
	int *chunked)
{
	char line[IPP_LINE_MAX];
	const char *value;
	const char *found;
	size_t total;
	size_t taken;
	int version;
	int named;
	int status;

	/* Until a final status. */
	total = 0;
	for (;;) {
		/* The status line. */
		status = ipp_line(reader, line, sizeof(line), &taken);
		if (status != 0)
			return status;
		total += taken;
		version = strncmp(line, "HTTP/1.", 7U);
		taken = strlen(line);
		if (version != 0 || taken < 12U)
			return EPROTO;
		*http = atoi(line + 9);
		*length = -1;
		*chunked = 0;

		/* Its fields, until the empty line. */
		for (;;) {
			status = ipp_line(reader, line, sizeof(line), &taken);
			if (status != 0)
				return status;
			total += taken;
			if (total > IPP_HEADER_MAX)
				return EPROTO;
			if (line[0] == '\0')
				break;

			/* The body's length. */
			named = ipp_header_is(line, "content-length", &value);
			if (named) {
				*length = strtoll(value, NULL, 10);
				continue;
			}

			/* The body in chunks. */
			named = ipp_header_is(line, "transfer-encoding", &value);
			found = NULL;
			if (named)
				found = strstr(value, "chunked");
			if (found != NULL) {
				/* Chunked: the body's size comes in its pieces. */
				*chunked = 1;
			}
		}

		/* A final status ends the search; an informational one is passed over. */
		if (*http >= 200)
			break;
	}

	/* Succeeded: the header read. */
	return 0;
}

/*
 * Reads a body and feeds it to the stream: in chunks, by its length, or
 * to the connection's end; at most IPP_BODY_MOST bytes.  Returns 0,
 * EPROTO, or the read's errno.
 */
static int
ipp_read_body(
	struct ipp_reader *reader,
	long long length,
	int chunked,
	struct ipp_stream *stream)
{
	unsigned long long total;
	unsigned long long size;
	char line[IPP_LINE_MAX];
	size_t taken;
	char *end;
	int status;

	/* By its length, or to the connection's end. */
	total = 0;
	if (!chunked) {
		size = ULLONG_MAX;
		if (length >= 0)
			size = (unsigned long long)length;
		status = ipp_feed_from(reader, size, stream, &total);
		if (status != 0)
			return status;

		/* Succeeded: the body read. */
		return 0;
	}

	/* Each chunk: its size's line, its bytes and the line's end after them. */
	for (;;) {
		status = ipp_line(reader, line, sizeof(line), &taken);
		if (status != 0)
			return status;
		size = strtoull(line, &end, 16);
		if (end == line)
			return EPROTO;

		/* The last chunk: the trailer's lines until the empty one. */
		if (size == 0U) {
			for (;;) {
				status = ipp_line(reader, line, sizeof(line), &taken);
				if (status != 0)
					return status;
				if (line[0] == '\0')
					break;
			}

			/* Succeeded: the body read. */
			return 0;
		}

		/* Its bytes. */
		status = ipp_feed_from(reader, size, stream, &total);
		if (status != 0)
			return status;

		/* The end of its line. */
		status = ipp_line(reader, line, sizeof(line), &taken);
		if (status != 0)
			return status;
		if (line[0] != '\0')
			return EPROTO;
	}
}

/*
 * Reads more of the connection into the reader (only when it holds
 * nothing).  Returns 0 (closed when the printer ended it), or the read's
 * errno.
 */
static int
ipp_fill(
	struct ipp_reader *reader)
{
	ssize_t got;

	/* Bytes still held, or the end reached. */
	if (reader->start < reader->end || reader->closed)
		return 0;

	/* One read. */
	got = pd_read_some(reader->fd, reader->data, sizeof(reader->data));
	if (got < 0)
		return errno;

	/* The printer ended the connection. */
	reader->start = 0;
	reader->end = 0;
	if (got == 0) {
		reader->closed = 1;
		return 0;
	}

	/* Succeeded: the bytes held. */
	reader->end = (size_t)got;
	return 0;
}

/*
 * Reads a line (its CR LF or LF taken off) of at most size - 1 bytes and
 * counts the bytes it took.  Returns 0, EPROTO for a line too long or a
 * connection that ended first, or the read's errno.
 */
static int
ipp_line(
	struct ipp_reader *reader,
	char *line,
	size_t size,
	size_t *taken)
{
	size_t length;
	unsigned char byte;
	int status;

	/* Byte by byte until the line feed. */
	length = 0;
	*taken = 0;
	for (;;) {
		status = ipp_fill(reader);
		if (status != 0)
			return status;
		if (reader->closed)
			return EPROTO;

		/* The next byte. */
		byte = reader->data[reader->start];
		reader->start++;
		(*taken)++;
		if (byte == '\n')
			break;

		/* Too long for a line. */
		if (length + 1U >= size)
			return EPROTO;
		line[length] = (char)byte;
		length++;
	}

	/* The carriage return goes with the line feed. */
	if (length > 0U && line[length - 1U] == '\r')
		length--;
	line[length] = '\0';

	/* Succeeded: a line. */
	return 0;
}

/*
 * Feeds the stream a number of the connection's bytes (ULLONG_MAX: to its
 * end), the body's total kept within IPP_BODY_MOST.  Returns 0, EPROTO,
 * or the read's errno.
 */
static int
ipp_feed_from(
	struct ipp_reader *reader,
	unsigned long long count,
	struct ipp_stream *stream,
	unsigned long long *total)
{
	size_t take;
	int status;

	/* Until the count is fed. */
	while (count > 0U) {
		status = ipp_fill(reader);
		if (status != 0)
			return status;

		/* The connection's end: the end of a body without a length, too early for one with. */
		if (reader->closed) {
			if (count == ULLONG_MAX)
				return 0;
			return EPROTO;
		}

		/* The bytes held, up to the count. */
		take = reader->end - reader->start;
		if ((unsigned long long)take > count)
			take = (size_t)count;
		*total += take;
		if (*total > IPP_BODY_MOST)
			return EPROTO;
		status = ipp_stream_feed(stream, reader->data + reader->start, take);
		if (status != 0)
			return status;
		reader->start += take;
		if (count != ULLONG_MAX)
			count -= take;
	}

	/* Succeeded: the count fed. */
	return 0;
}

/* Starts decoding a response's IPP message into an answer. */
static void
ipp_stream_start(
	struct ipp_stream *stream,
	struct ipp_answer *answer)
{
	/* Nothing read: the header's eight bytes first. */
	memset(stream, 0, sizeof(*stream));
	stream->answer = answer;
	stream->stage = IPP_STAGE_HEADER;
	stream->need = 8U;
}

/*
 * Feeds bytes of the IPP message to the stream.  Bytes after the end of
 * the attributes (a document) are passed over.  Returns 0 or EPROTO.
 */
static int
ipp_stream_feed(
	struct ipp_stream *stream,
	const unsigned char *bytes,
	size_t length)
{
	size_t take;
	size_t room;
	int status;

	/* Byte runs, a stage at a time. */
	while (length > 0U) {
		/* The message ended: the rest is not looked at. */
		if (stream->stage == IPP_STAGE_END)
			return 0;

		/* As many bytes as the stage still needs. */
		take = stream->need - stream->have;
		if (take > length)
			take = length;

		/* A name or a value keeps its first bytes; the small fields gather whole. */
		if (stream->stage == IPP_STAGE_NAME) {
			room = 0;
			if (stream->have < sizeof(stream->name) - 1U)
				room = sizeof(stream->name) - 1U - stream->have;
			if (room > take)
				room = take;
			memcpy(stream->name + stream->have, bytes, room);
		} else if (stream->stage == IPP_STAGE_VALUE) {
			room = 0;
			if (stream->have < sizeof(stream->value))
				room = sizeof(stream->value) - stream->have;
			if (room > take)
				room = take;
			memcpy(stream->value + stream->have, bytes, room);
		} else {
			memcpy(stream->small + stream->have, bytes, take);
		}

		/* The bytes taken. */
		stream->have += take;
		bytes += take;
		length -= take;

		/* A field complete moves the stage on. */
		if (stream->have == stream->need) {
			status = ipp_stream_field(stream);
			if (status != 0)
				return status;
		}
	}

	/* Succeeded: the bytes taken. */
	return 0;
}

/* Takes a field that is complete and sets the next stage.  Returns 0 or EPROTO. */
static int
ipp_stream_field(
	struct ipp_stream *stream)
{
	size_t length;

	/* The next field starts empty. */
	stream->have = 0;

	/* By the stage the field ends. */
	switch (stream->stage) {
	case IPP_STAGE_HEADER:
		/* The version, the status and the request-id. */
		stream->answer->status = (unsigned)stream->small[2] << 8 | stream->small[3];
		stream->answer->request = ipp_be32(stream->small + 4);
		stream->stage = IPP_STAGE_TAG;
		stream->need = 1U;
		break;
	case IPP_STAGE_TAG:
		/* The end, a group's delimiter, or an attribute's tag. */
		stream->tag = stream->small[0];
		if (stream->tag == IPP_END) {
			stream->stage = IPP_STAGE_END;
			break;
		}

		/* A group's delimiter: the next tag. */
		if (stream->tag <= 0x0fU) {
			stream->need = 1U;
			break;
		}

		/* An attribute: its name's length next. */
		stream->stage = IPP_STAGE_NAME_LENGTH;
		stream->need = 2U;
		break;
	case IPP_STAGE_NAME_LENGTH:
		/* The name's length: none for another value of the attribute before. */
		length = (size_t)stream->small[0] << 8 | stream->small[1];
		stream->name_length = length;
		memset(stream->name, 0, sizeof(stream->name));
		stream->stage = IPP_STAGE_NAME;
		stream->need = length;
		if (length == 0U) {
			stream->stage = IPP_STAGE_VALUE_LENGTH;
			stream->need = 2U;
		}

		/* The name, or the value's length. */
		break;
	case IPP_STAGE_NAME:
		/* A new attribute's name, counted. */
		(void)snprintf(stream->last, sizeof(stream->last), "%s", stream->name);
		stream->names++;
		stream->stage = IPP_STAGE_VALUE_LENGTH;
		stream->need = 2U;
		break;
	case IPP_STAGE_VALUE_LENGTH:
		/* The value's length. */
		length = (size_t)stream->small[0] << 8 | stream->small[1];
		stream->value_length = length;
		stream->stage = IPP_STAGE_VALUE;
		stream->need = length;
		if (length == 0U) {
			ipp_stream_attribute(stream);
			stream->stage = IPP_STAGE_TAG;
			stream->need = 1U;
		}

		/* The value, or the next tag. */
		break;
	case IPP_STAGE_VALUE:
		/* The value: taken, then the next tag. */
		ipp_stream_attribute(stream);
		stream->stage = IPP_STAGE_TAG;
		stream->need = 1U;
		break;
	default:
		return EPROTO;
	}

	/* Succeeded: the stage moved on. */
	return 0;
}

/*
 * Takes the values wanted from an attribute decoded: by the name they
 * belong to, while no more than IPP_NAMES_MAX names were met and the value
 * was kept whole.
 */
static void
ipp_stream_attribute(
	struct ipp_stream *stream)
{
	struct ipp_answer *answer;
	const unsigned char *value;
	size_t length;
	int which;
	int pdf;

	/* Past the names looked at, or a value not kept whole. */
	if (stream->names > IPP_NAMES_MAX)
		return;
	if (stream->value_length > sizeof(stream->value))
		return;

	/* The values wanted, by the attribute's name. */
	answer = stream->answer;
	value = stream->value;
	length = stream->value_length;
	which = ipp_named(stream->last);
	if (which == 1 && stream->tag == IPP_INTEGER && length == 4U) {
		answer->job_id = (int)ipp_be32(value);
	} else if (which == 2 && stream->tag == IPP_ENUM && length == 4U) {
		answer->job_state = (int)ipp_be32(value);
	} else if (which == 3 && stream->tag == IPP_MIME) {
		answer->has_formats = 1;
		pdf = 1;
		if (length == 15U)
			pdf = memcmp(value, "application/pdf", 15U);
		if (pdf == 0)
			answer->has_pdf = 1;
	} else if (which == 4 && answer->info[0] == '\0') {
		ipp_take_text(value, length, stream->tag, answer->info, sizeof(answer->info));
	} else if (which == 5 && answer->model[0] == '\0') {
		ipp_take_text(value, length, stream->tag, answer->model, sizeof(answer->model));
	}
}

/*
 * Tells whether a header line is a field of a name (letters in any case)
 * and where its value starts, past the spaces.
 */
static int
ipp_header_is(
	const char *line,
	const char *name,
	const char **value)
{
	size_t length;
	size_t index;
	int a;
	int b;

	/* The name, a letter at a time in any case. */
	length = strlen(name);
	for (index = 0; index < length; index++) {
		a = line[index];
		b = name[index];
		if (a >= 'A' && a <= 'Z')
			a = a - 'A' + 'a';
		if (a != b)
			return 0;
	}

	/* The colon after it. */
	if (line[length] != ':')
		return 0;

	/* The value past the spaces. */
	*value = line + length + 1U;
	while (**value == ' ' || **value == '\t')
		(*value)++;

	/* Succeeded: the field is named so. */
	return 1;
}

/*
 * Tells which attribute a name is, of those read: 1 job-id, 2 job-state,
 * 3 document-format-supported, 4 printer-info, 5 printer-make-and-model,
 * 0 another.
 */
static int
ipp_named(
	const char *name)
{
	static const char *const names[] = { "job-id", "job-state", "document-format-supported", "printer-info", "printer-make-and-model" };
	size_t index;
	int same;

	/* Each name read. */
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		same = strcmp(name, names[index]);
		if (same == 0)
			return (int)index + 1;
	}

	/* Another. */
	return 0;
}

/*
 * Takes a text value (its language left out for textWithLanguage and
 * nameWithLanguage): control characters become spaces, and it is cut at a
 * character's boundary to fit.
 */
static void
ipp_take_text(
	const unsigned char *value,
	size_t size,
	unsigned tag,
	char *text,
	size_t room)
{
	size_t skip;
	size_t index;
	size_t length;

	/* With a language: its length and bytes, then the text's length and bytes. */
	if (tag == IPP_TEXT_LANGUAGE || tag == IPP_NAME_LANGUAGE) {
		if (size < 2U)
			return;
		skip = ((size_t)value[0] << 8 | value[1]) + 2U;
		if (skip + 2U > size)
			return;
		value += skip + 2U;
		size -= skip + 2U;
	} else if (tag != IPP_TEXT && tag != IPP_NAME) {
		return;
	}

	/* As much as fits, cut where a character starts. */
	length = size;
	if (length > room - 1U) {
		length = room - 1U;
		while (length > 0U && (value[length] & 0xc0U) == 0x80U)
			length--;
	}

	/* The bytes, control characters made spaces. */
	for (index = 0; index < length; index++) {
		text[index] = (char)value[index];
		if (value[index] < 0x20U || value[index] == 0x7fU)
			text[index] = ' ';
	}

	/* Ended. */
	text[length] = '\0';
}

/* The word of a failed answer (plan/ws145/design.md §5.4). */
static const char *
ipp_detail(
	const struct ipp_answer *answer)
{
	/* HTTP's own failures. */
	if (answer->http == 401 || answer->http == 403)
		return "auth";
	if (answer->http == 426)
		return "tls";
	if (answer->http != 200)
		return "refused";

	/* IPP's. */
	if (answer->status == IPP_FORMAT)
		return "format";
	if (answer->status == IPP_NOT_AUTHENTICATED || answer->status == IPP_NOT_AUTHORIZED)
		return "auth";
	if (answer->status == IPP_BUSY)
		return "busy";
	if (answer->status == IPP_NOT_ACCEPTING)
		return "stopped";
	return "refused";
}

/* The next request-id (1 to 2^31 - 1). */
static uint32_t
ipp_request_id(void)
{
	uint32_t request;

	/* Under the lock: the threads share the count. */
	(void)pthread_mutex_lock(&ipp_lock);
	request = ipp_next_request;
	ipp_next_request++;
	if (ipp_next_request > 0x7fffffffU)
		ipp_next_request = 1;
	(void)pthread_mutex_unlock(&ipp_lock);
	return request;
}

/* Reads a big-endian 32-bit number. */
static uint32_t
ipp_be32(
	const unsigned char *bytes)
{
	uint32_t number;

	/* The most significant byte first. */
	number = (uint32_t)bytes[0] << 24;
	number |= (uint32_t)bytes[1] << 16;
	number |= (uint32_t)bytes[2] << 8;
	number |= (uint32_t)bytes[3];
	return number;
}
