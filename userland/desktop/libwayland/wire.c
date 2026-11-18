/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Encodes Wayland messages and preserves SCM_RIGHTS across partial stream I/O.
 */

#include "internal.h"
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static int wlc_wire_measure(struct wl_proxy *proxy, const struct wl_message *message, union wl_argument *arguments, struct wl_proxy *created, size_t *length);
static int wlc_wire_encode(struct wl_proxy *proxy, const struct wl_message *message, union wl_argument *arguments, struct wl_proxy *created, struct wlc_packet *packet);
static int wlc_wire_parse(struct wl_display *display);
static int wlc_wire_event(struct wl_display *display, struct wl_proxy *proxy, uint32_t opcode, size_t length, struct wlc_event **output, size_t *fd_count);
static int wlc_wire_display(struct wl_display *display, uint32_t opcode, size_t length);
static int wlc_wire_control(struct wl_display *display, struct msghdr *message);

/*
 * Decodes the minimum object version prefix of a message signature.
 */
const char *
wlc_signature_start(
	const char *signature,
	uint32_t *version)
{
	uint32_t since;

	/* Versionless messages have existed since interface version one. */
	since = 0;
	while (*signature >= '0' && *signature <= '9') {
		since = since * 10U + (uint32_t)(*signature - '0');
		signature++;
	}

	/* Supplies the implicit version only when no numeric prefix appeared. */
	if (since == 0)
		since = 1;

	/* Publishes the negotiated minimum version with the remaining signature. */
	*version = since;

	/* Succeeded: reports the start of the argument signature. */
	return signature;
}

/*
 * Decodes one argument type and its nullable qualifier.
 */
const char *
wlc_signature_next(
	const char *signature,
	char *type,
	int *nullable)
{
	/* Nullable applies only to this argument, never to the following one. */
	*nullable = 0;
	if (*signature == '?') {
		*nullable = 1;
		signature++;
	}

	/* Advances only when an argument remains in the immutable description. */
	*type = *signature;
	if (*signature != '\0')
		signature++;

	/* Succeeded: reports the following argument position. */
	return signature;
}

/*
 * Counts actual arguments excluding signature version and nullable markers.
 */
size_t
wlc_signature_count(
	const char *signature)
{
	size_t count;
	uint32_t version;
	char type;
	int nullable;

	/* Skips the version prefix before traversing real wire arguments. */
	signature = wlc_signature_start(signature, &version);
	count = 0;
	while (*signature != '\0') {
		signature = wlc_signature_next(signature, &type, &nullable);
		count++;
	}

	/* Succeeded: reports the required union argument count. */
	return count;
}

/*
 * Queues one complete wire message while the display mutex is held.
 */
int
wlc_wire_queue(
	struct wl_proxy *proxy,
	uint32_t opcode,
	union wl_argument *arguments,
	struct wl_proxy *created)
{
	struct wl_display *display;
	struct wlc_packet *packet;
	const struct wl_message *message;
	uint32_t header;
	uint32_t version;
	size_t length;
	int error;

	/* Rejects opcodes outside the target's actual request table. */
	if (opcode >= (uint32_t)proxy->interface->method_count)
		return EINVAL;

	/* Rejects requests newer than the negotiated interface version. */
	message = &proxy->interface->methods[opcode];
	wlc_signature_start(message->signature, &version);
	if (version > proxy->version)
		return EINVAL;

	/* Validates and sizes every argument before allocating a packet. */
	error = wlc_wire_measure(proxy, message, arguments, created, &length);
	if (error != 0)
		return error;

	/* Creates the packet's descriptor ownership container. */
	packet = calloc(1, sizeof(*packet));
	if (packet == NULL)
		return ENOMEM;

	/* Allocates exactly the aligned message size after validation. */
	packet->bytes = calloc(1, length);
	if (packet->bytes == NULL) {
		free(packet);
		return ENOMEM;
	}

	/* Encodes the standard native-endian object and length/opcode words. */
	packet->length = length;
	header = ((uint32_t)length << 16) | opcode;
	memcpy(packet->bytes, &proxy->id, sizeof(proxy->id));
	memcpy(packet->bytes + 4, &header, sizeof(header));

	/* Retains independent fd references before publishing the packet. */
	error = wlc_wire_encode(proxy, message, arguments, created, packet);
	if (error != 0) {
		/* Retires only the now-detached complete packet and its remaining storage. */
		wlc_packet_destroy(packet);
		return error;
	}

	/* Appends one atomic request to the serialized connection stream. */
	display = proxy->display;
	if (display->output_tail == NULL) {
		display->output_head = packet;
	} else {
		display->output_tail->next = packet;
	}

	/* The tail identifies the final packet owned by this connection. */
	display->output_tail = packet;

	/* Succeeded: caller descriptors may now be closed independently. */
	return 0;
}

/*
 * Flushes buffered requests without blocking or retransmitting descriptor rights.
 */
int
wlc_wire_flush(
	struct wl_display *display)
{
	struct wlc_packet *packet;
	struct msghdr message;
	struct iovec vector;
	struct cmsghdr *control;
	union {
		struct cmsghdr alignment;
		unsigned char bytes[CMSG_SPACE(WLC_FD_MAX * sizeof(int))];
	} ancillary;
	ssize_t sent;
	size_t index;
	int total;
	int closed;
	int error;

	/* Sends queued packets in order while the socket accepts more bytes. */
	total = 0;
	while (display->output_head != NULL) {
		/* Describes only the unsent suffix of this packet. */
		packet = display->output_head;
		memset(&message, 0, sizeof(message));
		vector.iov_base = packet->bytes + packet->sent;
		vector.iov_len = packet->length - packet->sent;
		message.msg_iov = &vector;
		message.msg_iovlen = 1;

		/* Rights are attached only until the first positive send accepts them. */
		if (packet->descriptor_count != 0) {
			memset(&ancillary, 0, sizeof(ancillary));
			control = (struct cmsghdr *)ancillary.bytes;
			control->cmsg_level = SOL_SOCKET;
			control->cmsg_type = SCM_RIGHTS;
			control->cmsg_len = CMSG_LEN(packet->descriptor_count * sizeof(int));
			memcpy(CMSG_DATA(control), packet->descriptors, packet->descriptor_count * sizeof(int));
			message.msg_control = ancillary.bytes;
			message.msg_controllen = CMSG_SPACE(packet->descriptor_count * sizeof(int));
		}

		/* Uses nonblocking delivery and suppresses process-wide SIGPIPE. */
		sent = sendmsg(display->fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (sent < 0) {
			error = errno;

			/* Signals may interrupt before any bytes or rights are accepted. */
			if (error == EINTR)
				continue;

			/* Propagates the failed socket submission without consuming packet rights. */
			errno = error;
			return -1;
		}

		/* A zero-length send cannot advance this nonempty protocol message. */
		if (sent == 0) {
			errno = EPIPE;
			return -1;
		}

		/* Closes every packet duplicate after the kernel accepts the corresponding rights. */
		for (index = 0; index < packet->descriptor_count; index++) {
			closed = close(packet->descriptors[index]);
			if (closed != 0) {
				/* Accepted rights cannot be resent; a retired descriptor number cannot safely be retried. */
				continue;
			}
		}

		/* Accepted rights no longer belong to the queued packet, even after a partial send. */
		packet->descriptor_count = 0;
		packet->sent += (size_t)sent;

		/* Avoids overflowing the byte count when a very large queue is drained. */
		if (sent > INT_MAX - total) {
			total = INT_MAX;
		} else {
			total += (int)sent;
		}

		/* A partial send retains only the remaining byte suffix. */
		if (packet->sent != packet->length)
			continue;

		/* Drops complete packets after detaching them from the output list. */
		display->output_head = packet->next;
		if (display->output_head == NULL)
			display->output_tail = NULL;

		/* Retires only the now-detached complete packet and its remaining storage. */
		wlc_packet_destroy(packet);
	}

	/* Succeeded: every queued request has reached the socket. */
	return total;
}

/*
 * Reads available bytes and ancillary references into independent event queues.
 */
int
wlc_wire_read(
	struct wl_display *display)
{
	struct msghdr message;
	struct iovec vector;
	union {
		struct cmsghdr alignment;
		unsigned char bytes[CMSG_SPACE(WLC_FD_MAX * sizeof(int))];
	} ancillary;
	ssize_t received;
	int error;

	/* A valid message fits the protocol's aligned sixteen-bit size field. */
	if (display->input_size >= WLC_WIRE_MAX)
		return EPROTO;

	/* Receives rights independently of arbitrary stream packet boundaries. */
	memset(&message, 0, sizeof(message));
	memset(&ancillary, 0, sizeof(ancillary));
	vector.iov_base = display->input + display->input_size;
	vector.iov_len = WLC_WIRE_MAX - display->input_size;
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = ancillary.bytes;
	message.msg_controllen = sizeof(ancillary.bytes);
	received = recvmsg(display->fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
	if (received < 0) {
		/* Another prepared queue may already have consumed this readiness. */
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;

		/* Reports a failed receive before any input storage or rights are published. */
		return errno;
	}

	/* Captures returned descriptors even when a later payload check fails. */
	error = wlc_wire_control(display, &message);
	if (error != 0)
		return error;

	/* A closed peer can no longer complete a partial protocol message. */
	if (received == 0)
		return EPIPE;

	/* Makes newly received bytes visible to the incremental decoder. */
	display->input_size += (size_t)received;
	error = wlc_wire_parse(display);
	if (error != 0)
		return error;

	/* Succeeded: complete messages are queued and any partial tail is retained. */
	return 0;
}

/*
 * Releases unsent packet bytes and any descriptor references they still own.
 */
void
wlc_packet_destroy(
	struct wlc_packet *packet)
{
	size_t index;
	int closed;

	/* Recovers references never accepted by sendmsg. */
	for (index = 0; index < packet->descriptor_count; index++) {
		closed = close(packet->descriptors[index]);
		if (closed != 0) {
			/* Cleanup continues after one close refusal without risking a recycled descriptor number. */
			continue;
		}
	}

	/* Drops packet-owned byte storage after its rights are released. */
	free(packet->bytes);
	free(packet);

	/* Succeeded: the outgoing message owns no resources. */
	return;
}

/* Validates argument identities and computes an aligned message size. */
static int
wlc_wire_measure(
	struct wl_proxy *proxy,
	const struct wl_message *message,
	union wl_argument *arguments,
	struct wl_proxy *created,
	size_t *length)
{
	const char *signature;
	struct wl_proxy *object;
	const struct wl_interface *expected;
	size_t index;
	size_t bytes;
	size_t addition;
	size_t fd_count;
	size_t constructors;
	uint32_t version;
	char type;
	int nullable;
	int same;

	/* Measures each argument from the two mandatory protocol header words. */
	*length = 8;
	fd_count = 0;
	constructors = 0;
	index = 0;
	signature = wlc_signature_start(message->signature, &version);
	while (*signature != '\0') {
		/* An argument-bearing signature requires caller argument storage. */
		if (arguments == NULL)
			return EINVAL;

		/* Selects exactly one wire kind and its optional object interface. */
		signature = wlc_signature_next(signature, &type, &nullable);
		addition = 4;
		expected = NULL;
		if (message->types != NULL)
			expected = message->types[index];

		/* Checks variable-size data and object ownership before serialization. */
		switch (type) {
		case 'i':
		case 'u':
		case 'f':
			break;
		case 'h':
			/* The kernel's ancillary contract carries at most eight rights. */
			if (arguments[index].h < 0 || fd_count >= WLC_FD_MAX)
				return EINVAL;

			/* Counts the right that this request must transport separately from its payload. */
			fd_count++;
			addition = 0;
			break;
		case 's':
			/* The permitted null string contributes only its zero length prefix. */
			bytes = 0;

			/* Null strings are legal only with an explicit nullable signature. */
			if (arguments[index].s == NULL) {
				/* A non-nullable string cannot use the wire's null representation. */
				if (!nullable)
					return EINVAL;
			} else {
				/* Includes the string's protocol-mandated NUL terminator. */
				bytes = strlen(arguments[index].s);
				if (bytes >= WLC_WIRE_MAX)
					return E2BIG;

				/* The terminator contributes one byte to the string length on the wire. */
				bytes++;
			}

			/* Variable data contributes its full protocol-aligned extent to this argument. */
			addition += (bytes + 3U) & ~(size_t)3U;
			break;
		case 'a':
			/* Arrays always have a descriptor, including zero-length arrays. */
			if (arguments[index].a == NULL)
				return EINVAL;

			/* Reads the caller-provided array extent before validating its payload pointer. */
			bytes = arguments[index].a->size;

			/* Keeps length alignment and the message header representable. */
			if (bytes > WLC_WIRE_MAX)
				return E2BIG;

			/* Nonempty arrays require initialized caller data. */
			if (bytes != 0 && arguments[index].a->data == NULL)
				return EINVAL;

			/* Variable data contributes its full protocol-aligned extent to this argument. */
			addition += (bytes + 3U) & ~(size_t)3U;
			break;
		case 'n':
		case 'o':
			/* Caller object arguments identify retained local proxy generations. */
			object = (struct wl_proxy *)arguments[index].o;

			/* Constructor calls provide the allocated proxy independently of args. */
			if (type == 'n') {
				/* The constructor count must agree with the request's single created object. */
				constructors++;

				/* The legacy entry point instead supplies a precreated object. */
				if (created != NULL)
					object = created;
			}

			/* Only explicitly nullable ordinary object arguments may encode zero. */
			if (object == NULL) {
				/* New identities and non-nullable ordinary references cannot encode zero. */
				if (type == 'n' || !nullable)
					return EINVAL;

				break;
			}

			/* Resolves wrappers while retaining the original connection identity. */
			object = wlc_proxy_real(object);
			if (object->display != proxy->display || object->destroyed)
				return EINVAL;

			/* Typed arguments must match the interface declared by the protocol. */
			if (expected != NULL) {
				same = strcmp(expected->name, object->interface->name);
				if (same != 0)
					return EINVAL;
			}

			break;
		default:
			/* No other signature kind has a representable Wayland argument. */
			return EINVAL;
		}

		/* Rejects a total size that cannot fit the aligned wire size field. */
		if (addition > WLC_WIRE_MAX - *length)
			return E2BIG;

		/* Accumulates one validated argument and advances the matching caller union index. */
		*length += addition;
		index++;
	}

	/* A constructor call creates exactly the request's single new object. */
	if (created != NULL && constructors != 1)
		return EINVAL;

	/* Succeeded: every argument and the complete wire extent are representable. */
	return 0;
}

/* Serializes validated arguments and duplicates outgoing descriptors. */
static int
wlc_wire_encode(
	struct wl_proxy *proxy,
	const struct wl_message *message,
	union wl_argument *arguments,
	struct wl_proxy *created,
	struct wlc_packet *packet)
{
	const char *signature;
	const void *data;
	struct wl_proxy *object;
	size_t index;
	size_t offset;
	size_t bytes;
	uint32_t word;
	uint32_t version;
	char type;
	int nullable;
	int descriptor;

	/* Serializes every argument in host-native endian order without transport-side padding data. */
	(void)proxy;
	offset = 8;
	index = 0;
	signature = wlc_signature_start(message->signature, &version);
	while (*signature != '\0') {
		/* Resolves the next argument without consuming nullable marker bytes. */
		signature = wlc_signature_next(signature, &type, &nullable);
		word = 0;
		data = NULL;
		bytes = 0;

		/* Separates descriptor transport from byte-bearing argument kinds. */
		switch (type) {
		case 'h':
			/* The queued packet owns this duplicate even if the caller closes fd. */
			descriptor = fcntl(arguments[index].h, F_DUPFD_CLOEXEC, 0);
			if (descriptor < 0)
				return errno;

			/* The ownership count includes only duplicates that packet cleanup may close. */
			packet->descriptors[packet->descriptor_count] = descriptor;
			packet->descriptor_count++;
			index++;
			continue;
		case 's':
			/* A present string supplies its own terminator-bearing wire payload. */
			data = arguments[index].s;

			/* A zero length represents the permitted null string. */
			if (data != NULL) {
				bytes = strlen(data);
				bytes++;
			}

			/* The string prefix carries its byte extent including the terminator. */
			word = (uint32_t)bytes;
			break;
		case 'a':
			/* An array contributes its validated byte count and opaque caller data. */
			data = arguments[index].a->data;
			/* Reads the caller-provided array extent before validating its payload pointer. */
			bytes = arguments[index].a->size;
			/* The string prefix carries its byte extent including the terminator. */
			word = (uint32_t)bytes;
			break;
		case 'n':
			/* Modern constructors supply their new identity separately from legacy union arguments. */
			object = created;

			/* Legacy new_id arguments already hold a local proxy identity. */
			if (object == NULL)
				object = (struct wl_proxy *)arguments[index].o;

			/* A constructor encodes the selected retained local proxy identity. */
			word = object->id;
			break;
		case 'o':
			/* An ordinary object encodes its current identity or the permitted null value. */
			object = (struct wl_proxy *)arguments[index].o;

			/* Null objects serialize to the reserved zero identity. */
			if (object != NULL)
				word = object->id;

			break;
		default:
			/* Validated integer and fixed-point arguments each occupy one native-endian word. */
			word = arguments[index].u;
			break;
		}

		/* Writes the scalar or variable-length prefix at an aligned position. */
		memcpy(packet->bytes + offset, &word, sizeof(word));
		offset += sizeof(word);

		/* Padding is already zero from packet allocation. */
		if (bytes != 0) {
			memcpy(packet->bytes + offset, data, bytes);
			offset += (bytes + 3U) & ~(size_t)3U;
		}

		/* Consumes exactly one caller argument after its full wire payload is written. */
		index++;
	}

	/* Succeeded: the packet independently owns all serialized request data. */
	return 0;
}

/* Collects received ancillary descriptors while recovering malformed controls. */
static int
wlc_wire_control(
	struct wl_display *display,
	struct msghdr *message)
{
	struct cmsghdr *control;
	int *descriptors;
	int *grown;
	size_t offset;
	size_t count;
	size_t index;
	size_t needed;
	int error;
	int closed;

	/* Rejects truncated rights without losing any descriptors actually returned. */
	error = 0;
	if ((message->msg_flags & MSG_CTRUNC) != 0)
		error = EPROTO;

	/* Walks only complete control headers within the returned ancillary extent. */
	offset = 0;
	while (message->msg_controllen - offset >= sizeof(*control)) {
		control = (struct cmsghdr *)((unsigned char *)message->msg_control + offset);

		/* Refuses malformed control lengths before following their payload. */
		if (control->cmsg_len < CMSG_LEN(0) ||
		    control->cmsg_len > message->msg_controllen - offset)
			return EPROTO;

		/* Collects only the fd-transfer control type used by Wayland. */
		if (control->cmsg_level == SOL_SOCKET && control->cmsg_type == SCM_RIGHTS) {
			count = (control->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			descriptors = (int *)CMSG_DATA(control);

			/* Rejects malformed fd payload lengths while preserving valid integers. */
			if ((control->cmsg_len - CMSG_LEN(0)) % sizeof(int) != 0)
				error = EPROTO;

			/* Bounds queued rights by the largest supported wire message extent. */
			needed = display->input_descriptor_count + count;
			if (needed > WLC_WIRE_MAX)
				error = EPROTO;

			/* Extends the FIFO only when the complete control data is valid. */
			grown = NULL;
			if (error == 0) {
				grown = realloc(display->input_descriptors, needed * sizeof(int));
				if (grown == NULL && needed != 0)
					error = ENOMEM;
			}

			/* Recovers rights that cannot enter the connection-owned FIFO. */
			if (error != 0) {
				/* Returns every received reference while preserving the original control failure. */
				for (index = 0; index < count; index++) {
					closed = close(descriptors[index]);
					if (closed != 0) {
						/* Another close refusal cannot replace the malformed-control error or stop later cleanup. */
						continue;
					}
				}
			} else {
				/* Appends rights in receive order independently of byte framing. */
				display->input_descriptors = grown;
				memcpy(grown + display->input_descriptor_count, descriptors, count * sizeof(int));
				display->input_descriptor_count = needed;
			}
		}

		/* A final unpadded control header ends the returned ancillary sequence. */
		needed = CMSG_SPACE(control->cmsg_len - CMSG_LEN(0));
		if (needed > message->msg_controllen - offset)
			break;

		/* Advances to the next complete control header using native ancillary alignment. */
		offset += needed;
	}

	/* Reports invalid or unretainable ancillary data after recovering its rights. */
	if (error != 0)
		return error;

	/* Succeeded: the FIFO owns every received descriptor until event dispatch. */
	return 0;
}

/* Parses complete messages while retaining an arbitrary incomplete stream tail. */
static int
wlc_wire_parse(
	struct wl_display *display)
{
	struct wl_proxy *proxy;
	struct wlc_event *event;
	struct wl_event_queue *queue;
	uint32_t object_id;
	uint32_t header;
	uint32_t opcode;
	size_t length;
	size_t fd_count;
	int error;

	/* Every message requires the complete two-word header before decoding. */
	while (display->input_size >= 8) {
		/* Reads aligned-independent header words from the retained byte stream. */
		memcpy(&object_id, display->input, sizeof(object_id));
		memcpy(&header, display->input + 4, sizeof(header));
		length = header >> 16;
		opcode = header & 0xffffU;

		/* Rejects impossible extents before checking for a partial payload. */
		if (length < 8 || (length & 3U) != 0)
			return EPROTO;

		/* Leaves a valid partial message intact for the next socket read. */
		if (length > display->input_size)
			return 0;

		/* A wire identity must still belong to its current retained generation. */
		proxy = wlc_proxy_lookup(display, object_id);
		if (proxy == NULL)
			return EPROTO;

		/* Display errors and ID acknowledgements are processed for every queue. */
		fd_count = 0;
		if (object_id == 1) {
			error = wlc_wire_display(display, opcode, length);
			if (error != 0)
				return error;
		} else {
			/* Decodes argument ownership before routing the event to its queue. */
			event = NULL;
			error = wlc_wire_event(display, proxy, opcode, length, &event, &fd_count);
			if (error == EAGAIN)
				return 0;

			/* Malformed events cannot be skipped without losing stream ordering. */
			if (error != 0)
				return error;

			/* Suppresses callbacks on proxies destroyed before event receipt. */
			if (proxy->destroyed) {
				wlc_event_destroy(event);
			} else {
				/* A private queue never dispatches callbacks assigned elsewhere. */
				queue = proxy->queue;
				if (queue->tail == NULL) {
					queue->head = event;
				} else {
					queue->tail->next = event;
				}

				/* The tail identifies the last event still owned by this dispatch queue. */
				queue->tail = event;
			}
		}

		/* Advances bytes and rights only after this complete event is accepted. */
		display->input_size -= length;
		memmove(display->input, display->input + length, display->input_size);
		display->input_descriptor_count -= fd_count;

		/* Retains any descriptors belonging to subsequent messages. */
		if (display->input_descriptor_count != 0) {
			memmove(display->input_descriptors, display->input_descriptors + fd_count, display->input_descriptor_count * sizeof(int));
		}
	}

	/* Succeeded: the remaining bytes are an incomplete header only. */
	return 0;
}

/* Handles connection-wide error reporting and object-ID retirement. */
static int
wlc_wire_display(
	struct wl_display *display,
	uint32_t opcode,
	size_t length)
{
	struct wl_proxy *proxy;
	uint32_t id;
	uint32_t code;
	uint32_t bytes;

	/* Every non-acknowledgement opcode must describe a complete fatal protocol error. */
	if (opcode != 1) {
		/* Unknown opcodes and incomplete error payloads cannot describe this display interface. */
		if (opcode != 0 || length < 24)
			return EPROTO;

		/* Validates the complete NUL-terminated error description. */
		memcpy(&id, display->input + 8, sizeof(id));
		memcpy(&code, display->input + 12, sizeof(code));
		memcpy(&bytes, display->input + 16, sizeof(bytes));
		if (bytes == 0 || bytes > length - 20)
			return EPROTO;

		/* A trailing NUL and exact padded extent are required by the wire format. */
		if (display->input[20 + bytes - 1] != 0 ||
		    20 + ((bytes + 3U) & ~3U) != length)
			return EPROTO;

		/* The offending object may have already been destroyed by the client. */
		proxy = wlc_proxy_lookup(display, id);
		display->protocol_id = id;
		display->protocol_code = code;
		display->protocol_interface = NULL;
		if (proxy != NULL)
			display->protocol_interface = proxy->interface;

		/* Keeps the error's log line, which is logged once the mutex is released. */
		if (display->protocol_report == NULL) {
			display->protocol_report = wlc_protocol_report(display->protocol_interface, id, code,
								       (const char *)display->input + 20);
		}

		/* Reports the server's fatal protocol error with separately queryable detail. */
		return EPROTO;
	}

	/* An ID acknowledgement is exactly one payload word. */
	if (length != 12)
		return EPROTO;

	/* Rejects acknowledgements outside the client-owned ID namespace. */
	memcpy(&id, display->input + 8, sizeof(id));
	if (id < 2 || id >= WLC_SERVER_ID_START)
		return EPROTO;

	/* Keeps queued events attached to the old proxy, never the reused ID. */
	proxy = wlc_proxy_lookup(display, id);
	if (proxy == NULL)
		return EPROTO;

	/* Retires the acknowledged map identity while queued events retain their old generation. */
	wlc_proxy_remove(proxy);

	/* Succeeded: the server acknowledgement retired exactly one client object identity. */
	return 0;
}

/* Decodes a complete event while retaining its payload and argument objects. */
static int
wlc_wire_event(
	struct wl_display *display,
	struct wl_proxy *proxy,
	uint32_t opcode,
	size_t length,
	struct wlc_event **output,
	size_t *fd_count)
{
	struct wlc_event *event;
	struct wl_proxy *object;
	const struct wl_message *message;
	const struct wl_interface *expected;
	const char *signature;
	size_t count;
	size_t index;
	size_t offset;
	size_t padded;
	size_t descriptors;
	uint32_t word;
	uint32_t version;
	char type;
	int nullable;
	int same;
	int error;

	/* Rejects events outside the interface's negotiated message description. */
	if (opcode >= (uint32_t)proxy->interface->event_count)
		return EPROTO;

	/* Rejects events introduced after the version used to bind this proxy. */
	message = &proxy->interface->events[opcode];
	signature = wlc_signature_start(message->signature, &version);
	if (version > proxy->version)
		return EPROTO;

	/* Counts ancillary arguments before claiming any connection-owned rights. */
	descriptors = 0;
	while (*signature != '\0') {
		signature = wlc_signature_next(signature, &type, &nullable);

		/* Each h consumes one descriptor and no payload bytes. */
		if (type == 'h')
			descriptors++;
	}

	/* Rights can arrive with different stream bytes than their logical message. */
	if (descriptors > display->input_descriptor_count)
		return EAGAIN;

	/* Allocates the event envelope before retaining object or fd references. */
	event = calloc(1, sizeof(*event));
	if (event == NULL)
		return ENOMEM;

	/* Holds this exact proxy generation across delete_id and callback cleanup. */
	proxy->references++;
	event->proxy = proxy;
	event->message = message;
	event->opcode = opcode;
	count = wlc_signature_count(message->signature);

	/* Allocates independent decoded argument storage. */
	event->arguments = calloc(count + 1, sizeof(*event->arguments));
	if (event->arguments == NULL) {
		wlc_event_destroy(event);
		return ENOMEM;
	}

	/* Arrays use embedded descriptors but point into event-owned payload bytes. */
	event->arrays = calloc(count + 1, sizeof(*event->arrays));
	if (event->arrays == NULL) {
		wlc_event_destroy(event);
		return ENOMEM;
	}

	/* Retains object references independently of nullable callback arguments. */
	event->objects = calloc(count + 1, sizeof(*event->objects));
	if (event->objects == NULL) {
		wlc_event_destroy(event);
		return ENOMEM;
	}

	/* Copies the payload so future reads cannot invalidate listener arguments. */
	event->bytes = malloc(length);
	if (event->bytes == NULL) {
		wlc_event_destroy(event);
		return ENOMEM;
	}

	/* Retains the complete payload independently of the next connection read. */
	memcpy(event->bytes, display->input, length);

	/* Decodes one argument at a time while recording only acquired ownership. */
	offset = 8;
	descriptors = 0;
	error = EPROTO;
	signature = wlc_signature_start(message->signature, &version);
	for (index = 0; index < count; index++) {
		/* Normalizes qualifiers before inspecting payload bounds. */
		signature = wlc_signature_next(signature, &type, &nullable);

		/* Fd arguments are borrowed until the entire event validates. */
		if (type == 'h') {
			event->arguments[index].h = -1;
			event->argument_count = index + 1;
			descriptors++;
			continue;
		}

		/* Every other argument starts with one aligned payload word. */
		if (length - offset < 4)
			goto fail;

		/* Consumes one validated scalar prefix and marks this decoded argument for cleanup. */
		memcpy(&word, event->bytes + offset, sizeof(word));
		offset += 4;
		event->argument_count = index + 1;

		/* Interprets the scalar, reference, or variable-length argument. */
		switch (type) {
		case 'i':
		case 'u':
		case 'f':
			/* Scalar event arguments retain the exact native-endian wire bits. */
			event->arguments[index].u = word;
			break;
		case 's':
		case 'a':
			/* Bounds raw length before alignment to prevent wraparound. */
			if (word > length - offset)
				goto fail;

			/* Includes mandatory wire padding only after the raw extent has been bounded. */
			padded = ((size_t)word + 3U) & ~(size_t)3U;

			/* Padding is part of the message extent even when bytes are ignored. */
			if (padded > length - offset)
				goto fail;

			/* Strings require either an allowed null or a trailing terminator. */
			if (type == 's') {
				if (word == 0) {
					/* A non-nullable string must carry at least its terminator. */
					if (!nullable)
						goto fail;
				} else {
					/* The string's final byte must terminate the event-owned payload. */
					if (event->bytes[offset + word - 1] != 0)
						goto fail;

					/* Listener strings borrow only the validated event-owned terminated bytes. */
					event->arguments[index].s = (const char *)event->bytes + offset;
				}
			} else {
				/* Listener array descriptors borrow immutable event payload storage. */
				event->arrays[index].size = word;
				event->arrays[index].data = event->bytes + offset;
				event->arguments[index].a = &event->arrays[index];
			}

			/* Consumes the validated variable argument and its complete padding. */
			offset += padded;
			break;
		case 'o':
			/* Nullable zero references do not retain any object. */
			if (word == 0) {
				/* A non-nullable reference cannot omit its object identity. */
				if (!nullable)
					goto fail;

				break;
			}

			/* Retains the referenced generation even if its map is later retired. */
			object = wlc_proxy_lookup(display, word);
			if (object == NULL)
				goto fail;

			/* Untyped object references need no protocol-class constraint. */
			expected = NULL;

			/* An explicitly typed reference must match the described protocol class. */
			if (message->types != NULL)
				expected = message->types[index];

			/* Rejects a retained proxy whose class differs from the typed protocol reference. */
			if (expected != NULL) {
				same = strcmp(expected->name, object->interface->name);
				if (same != 0)
					goto fail;
			}

			/* The event keeps this exact object generation alive until dispatch or discard. */
			object->references++;
			event->objects[index] = object;
			event->arguments[index].o = (struct wl_object *)object;
			break;
		case 'n':
			/* A server-created object needs the interface the protocol names for it. */
			expected = NULL;
			if (message->types != NULL)
				expected = message->types[index];

			/* Its proxy, in the server's identity range, is made now so later events can name it. */
			object = NULL;
			if (expected != NULL)
				object = wlc_proxy_insert_server(proxy, expected, word);

			/* Without an interface, or with an identity outside the server's range or in use, the event is malformed. */
			if (object == NULL) {
				wlc_event_destroy(event);
				return EPROTO;
			}

			/* The event holds it until dispatch, and hands it to the listener. */
			event->objects[index] = object;
			event->arguments[index].o = (struct wl_object *)object;
			break;
		default:
			/* No other argument type exists in the wire format. */
			goto fail;
		}
	}

	/* Extra payload words imply an incompatible or malformed event signature. */
	if (offset != length)
		goto fail;

	/* Transfers fd ownership only once all payload and object checks succeed. */
	descriptors = 0;
	signature = wlc_signature_start(message->signature, &version);
	for (index = 0; index < count; index++) {
		signature = wlc_signature_next(signature, &type, &nullable);

		/* The FIFO prefix now belongs to the event until dispatch or discard. */
		if (type == 'h') {
			event->arguments[index].h = display->input_descriptors[descriptors];
			descriptors++;
		}
	}

	/* Publishes the complete event and the FIFO prefix whose ownership it acquired. */
	*fd_count = descriptors;
	*output = event;

	/* No partially acquired event resources need recovery after complete publication. */
	error = 0;

fail:
	/* Recovers only resources acquired by a partially decoded event before publication. */
	if (error != 0) {
		wlc_event_destroy(event);
		return error;
	}

	/* Succeeded: the caller can atomically advance the connection's input FIFO. */
	return 0;
}
