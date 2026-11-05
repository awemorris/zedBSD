/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements queries of the caller's own making.
 *
 * getaddrinfo answers one question; this answers any the protocol can
 * carry, and hands the reply back as it arrived so that a caller may read
 * records this library knows nothing about.  The message format is RFC 1035,
 * including the name compression a reply uses to avoid repeating a domain.
 */

#include "resolver-internal.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct __res_state _res;

/* Supports the ns get16 operation. */
unsigned int
ns_get16(
	const unsigned char *at)
{
	/* Returns the computed result. */
	return (unsigned int)(((unsigned)at[0] << 8) | (unsigned)at[1]);
}

/* Supports the ns get32 operation. */
unsigned long
ns_get32(
	const unsigned char *at)
{
	/* Returns the computed result. */
	return ((unsigned long)at[0] << 24) | ((unsigned long)at[1] << 16) |
	       ((unsigned long)at[2] << 8) | (unsigned long)at[3];
}

/* Supports the ns put16 operation. */
void
ns_put16(
	unsigned int value,
	unsigned char *at)
{
	at[0] = (unsigned char)((value >> 8) & 0xffU);
	at[1] = (unsigned char)(value & 0xffU);
}

/* Supports the ns put32 operation. */
void
ns_put32(
	unsigned long value,
	unsigned char *at)
{
	at[0] = (unsigned char)((value >> 24) & 0xffUL);
	at[1] = (unsigned char)((value >> 16) & 0xffUL);
	at[2] = (unsigned char)((value >> 8) & 0xffUL);
	at[3] = (unsigned char)(value & 0xffUL);
}

/*
 * Implements the res init operation.
 *
 * Reads the servers and the search list once, so that a caller may then
 * change them before asking anything.
 */
int
res_init(
	void)
{
	struct resolver_config config;
	unsigned index;

	memset(&_res, 0, sizeof(_res));
	_res.retrans = 2;
	_res.retry = 2;
	_res.ndots = 1;
	_res.options = RES_DEFAULT | RES_INIT;
	_res.id = (unsigned short)(getpid() & 0xffff);

	/* Takes the servers the system was configured with. */
	if (resolver_load_config(&config) == 0) {
		for (index = 0; index < config.count && index < MAXNS;
		     index++) {
			_res.nsaddr_list[index].sin_family = AF_INET;
			_res.nsaddr_list[index].sin_port = htons(53);
			_res.nsaddr_list[index].sin_addr = config.servers[index];
			_res.nscount++;
		}
	}

	/* Falls back to the local host, which is where a server often is. */
	if (_res.nscount == 0) {
		_res.nsaddr_list[0].sin_family = AF_INET;
		_res.nsaddr_list[0].sin_port = htons(53);
		_res.nsaddr_list[0].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		_res.nscount = 1;
	}
	_res.dnsrch[0] = NULL;

	/* Reports successful completion. */
	return 0;
}

/*
 * Supports the encode name operation.
 *
 * Writes a name as the protocol carries it: each label preceded by its
 * length, and a zero length to end.  A trailing dot is the root and adds
 * nothing.
 */
static int
encode_name(
	const char *name,
	unsigned char *out,
	size_t size)
{
	size_t used;
	size_t label;
	size_t index;

	used = 0;
	index = 0;

	/* Continue while the operation condition remains true. */
	while (name[index] != '\0') {
		/* Measures the label up to the next dot. */
		label = 0;
		while (name[index + label] != '\0' &&
		       name[index + label] != '.')
			label++;

		/* Rejects a label that is empty in the middle, or too long. */
		if (label == 0U || label > NS_MAXLABEL)
			return -1;
		if (used + label + 2U > size)
			return -1;
		out[used++] = (unsigned char)label;
		memcpy(out + used, name + index, label);
		used += label;
		index += label;

		/* Steps over the dot that ended the label. */
		if (name[index] == '.')
			index++;
	}

	/* Handles a destination with no room for the root. */
	if (used + 1U > size)
		return -1;
	out[used++] = 0;

	/* Returns the computed result. */
	return (int)used;
}

/*
 * Implements the dn comp operation.
 *
 * The name is written out in full.  Compression saves room in a reply of
 * many records; a query carries one name, so there is nothing to point back
 * at and nothing to save.
 */
int
dn_comp(
	const char *name,
	unsigned char *out,
	int size,
	unsigned char **pointers,
	unsigned char **last)
{
	(void)pointers;
	(void)last;

	/* Handles the arguments availability. */
	if (name == NULL || out == NULL || size <= 0)
		return -1;

	/* Returns the computed result. */
	return encode_name(name, out, (size_t)size);
}

/*
 * Implements the dn expand operation.
 *
 * Follows the pointers a reply uses to avoid repeating a domain.  Each
 * pointer must lead strictly backwards, which is what stops a reply from
 * sending the reader round a loop.
 */
int
dn_expand(
	const unsigned char *message,
	const unsigned char *end,
	const unsigned char *at,
	char *out,
	int size)
{
	const unsigned char *cursor;
	size_t produced;
	size_t label;
	unsigned offset;
	int consumed;
	int followed;

	/* Handles the arguments availability. */
	if (message == NULL || end == NULL || at == NULL || out == NULL ||
	    size <= 0)
		return -1;
	cursor = at;
	produced = 0;
	consumed = -1;
	followed = 0;

	/* Continue while the operation condition remains true. */
	for (;;) {
		/* Handles a name that runs past the end of the message. */
		if (cursor >= end)
			return -1;
		label = *cursor;

		/* The root ends the name. */
		if (label == 0U) {
			cursor++;
			if (consumed < 0)
				consumed = (int)(cursor - at);
			break;
		}

		/* A pointer leads to where the rest of the name was written. */
		if ((label & 0xc0U) == 0xc0U) {
			if (cursor + 1 >= end)
				return -1;
			offset = (unsigned)((label & 0x3fU) << 8) |
				 (unsigned)cursor[1];

			/* Records where the name itself ended. */
			if (consumed < 0)
				consumed = (int)(cursor + 2 - at);

			/* Refuses a pointer that does not lead backwards. */
			if (message + offset >= cursor)
				return -1;
			cursor = message + offset;

			/* Refuses a reply that points on without end. */
			if (++followed > NS_MAXCDNAME)
				return -1;
			continue;
		}

		/* Rejects a length that is neither a label nor a pointer. */
		if ((label & 0xc0U) != 0U)
			return -1;
		if (cursor + 1 + label > end)
			return -1;

		/* Writes the label, with the dot that separates it. */
		if (produced + label + 2U > (size_t)size)
			return -1;
		if (produced != 0U)
			out[produced++] = '.';
		memcpy(out + produced, cursor + 1, label);
		produced += label;
		cursor += 1 + label;
	}
	out[produced] = '\0';

	/* Returns the computed result. */
	return consumed;
}

/*
 * Implements the dn skipname operation.
 */
int
dn_skipname(
	const unsigned char *at,
	const unsigned char *end)
{
	const unsigned char *cursor;
	size_t label;

	cursor = at;

	/* Continue while the operation condition remains true. */
	while (cursor < end) {
		label = *cursor;
		if (label == 0U)
			return (int)(cursor + 1 - at);

		/* A pointer is the last thing in a name. */
		if ((label & 0xc0U) == 0xc0U)
			return (int)(cursor + 2 - at);
		if ((label & 0xc0U) != 0U)
			return -1;
		cursor += 1 + label;
	}

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the res mkquery operation.
 */
int
res_mkquery(
	int operation,
	const char *name,
	int class_value,
	int type,
	const unsigned char *data,
	int data_length,
	const unsigned char *existing,
	unsigned char *buffer,
	int size)
{
	int used;

	(void)data;
	(void)data_length;
	(void)existing;

	/* Only a query is ever sent from here. */
	if (operation != QUERY || name == NULL || buffer == NULL ||
	    size < NS_HFIXEDSZ + NS_QFIXEDSZ + 2)
		return -1;
	if ((_res.options & RES_INIT) == 0 && res_init() != 0)
		return -1;

	/* The header: one question, and a request to be answered in full. */
	memset(buffer, 0, NS_HFIXEDSZ);
	_res.id = (unsigned short)(_res.id + 1U);
	ns_put16(_res.id, buffer);
	if ((_res.options & RES_RECURSE) != 0)
		buffer[2] = 0x01U;
	ns_put16(1U, buffer + 4);
	used = encode_name(name, buffer + NS_HFIXEDSZ,
			   (size_t)size - NS_HFIXEDSZ);

	/* Handles a name that could not be written. */
	if (used < 0 || NS_HFIXEDSZ + used + NS_QFIXEDSZ > size)
		return -1;
	ns_put16((unsigned)type, buffer + NS_HFIXEDSZ + used);
	ns_put16((unsigned)class_value, buffer + NS_HFIXEDSZ + used + 2);

	/* Returns the computed result. */
	return NS_HFIXEDSZ + used + NS_QFIXEDSZ;
}

/*
 * Supports the stream exchange operation.
 *
 * A reply too large for a datagram is asked for again over a stream, where
 * it is preceded by its length.
 */
static int
stream_exchange(
	const struct sockaddr_in *server,
	const unsigned char *query,
	int query_length,
	unsigned char *answer,
	int size)
{
	unsigned char framed[2];
	struct timeval timeout;
	ssize_t count;
	int descriptor;
	int expected;
	int received;

	descriptor = socket(AF_INET, SOCK_STREAM, 0);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;
	timeout.tv_sec = _res.retrans > 0 ? _res.retrans : 2;
	timeout.tv_usec = 0;
	(void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
			 sizeof(timeout));
	if (connect(descriptor, (const struct sockaddr *)server,
		    sizeof(*server)) != 0) {
		(void)close(descriptor);
		return -1;
	}
	ns_put16((unsigned)query_length, framed);
	if (write(descriptor, framed, 2) != 2 ||
	    write(descriptor, query, (size_t)query_length) != query_length) {
		(void)close(descriptor);
		return -1;
	}

	/* Reads the length that precedes the reply. */
	if (read(descriptor, framed, 2) != 2) {
		(void)close(descriptor);
		return -1;
	}
	expected = (int)ns_get16(framed);
	if (expected > size) {
		(void)close(descriptor);
		return -1;
	}

	/* Continue while the operation condition remains true. */
	received = 0;
	while (received < expected) {
		count = read(descriptor, answer + received,
			     (size_t)(expected - received));
		if (count <= 0) {
			(void)close(descriptor);
			return -1;
		}
		received += (int)count;
	}
	(void)close(descriptor);

	/* Returns the computed result. */
	return received;
}

/*
 * Implements the res send operation.
 *
 * Each server is asked in turn, and a truncated reply is asked for again
 * over a stream unless the caller said not to bother.
 */
int
res_send(
	const unsigned char *query,
	int query_length,
	unsigned char *answer,
	int size)
{
	struct sockaddr_in source;
	struct timeval timeout;
	socklen_t source_length;
	ssize_t count;
	int descriptor;
	int server;
	int attempt;
	int result;

	/* Handles the arguments availability. */
	if (query == NULL || answer == NULL || query_length <= 0 || size <= 0)
		return -1;
	if ((_res.options & RES_INIT) == 0 && res_init() != 0)
		return -1;

	/* Process each remaining element. */
	for (server = 0; server < _res.nscount; server++) {
		/* A caller may insist on a stream from the start. */
		if ((_res.options & RES_USEVC) != 0) {
			result = stream_exchange(&_res.nsaddr_list[server],
						 query, query_length, answer,
						 size);
			if (result > 0)
				return result;
			continue;
		}

		/* Process each remaining element. */
		for (attempt = 0; attempt < (_res.retry > 0 ? _res.retry : 2);
		     attempt++) {
			descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (descriptor < 0)
				return -1;
			timeout.tv_sec = _res.retrans > 0 ? _res.retrans : 2;
			timeout.tv_usec = 0;
			(void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO,
					 &timeout, sizeof(timeout));
			if (sendto(descriptor, query, (size_t)query_length, 0,
			    (const struct sockaddr *)&_res.nsaddr_list[server],
			    sizeof(_res.nsaddr_list[server])) < 0) {
				(void)close(descriptor);
				continue;
			}
			source_length = sizeof(source);
			count = recvfrom(descriptor, answer, (size_t)size, 0,
					 (struct sockaddr *)&source,
					 &source_length);
			(void)close(descriptor);

			/* Ignores anything that did not come from the server. */
			if (count < NS_HFIXEDSZ ||
			    source.sin_addr.s_addr !=
			    _res.nsaddr_list[server].sin_addr.s_addr ||
			    source.sin_port !=
			    _res.nsaddr_list[server].sin_port)
				continue;

			/* Ignores a reply to some other question. */
			if (ns_get16(answer) != ns_get16(query))
				continue;

			/* A truncated reply is asked for again over a stream. */
			if ((answer[2] & 0x02U) != 0 &&
			    (_res.options & RES_IGNTC) == 0) {
				result = stream_exchange(
				    &_res.nsaddr_list[server], query,
				    query_length, answer, size);
				if (result > 0)
					return result;
			}

			/* Returns the computed result. */
			return (int)count;
		}
	}

	/* Reports operation failure. */
	return -1;
}

/*
 * Supports the reply outcome operation.
 *
 * Turns what the server said into the error a caller reads from h_errno.
 */
static int
reply_outcome(
	const unsigned char *answer,
	int length)
{
	unsigned code;

	/* Handles a reply too short to hold a header. */
	if (length < NS_HFIXEDSZ) {
		h_errno = NO_RECOVERY;
		return -1;
	}
	code = (unsigned)(answer[3] & 0x0fU);

	/* Dispatch the selected reply code. */
	if (code == NXDOMAIN) {
		h_errno = HOST_NOT_FOUND;
		return -1;
	}
	if (code == SERVFAIL) {
		h_errno = TRY_AGAIN;
		return -1;
	}
	if (code != NOERROR) {
		h_errno = NO_RECOVERY;
		return -1;
	}

	/* A name that exists but has no such record is not a failure. */
	if (ns_get16(answer + 6) == 0U) {
		h_errno = NO_DATA;
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the res query operation.
 */
int
res_query(
	const char *name,
	int class_value,
	int type,
	unsigned char *answer,
	int size)
{
	unsigned char query[NS_PACKETSZ];
	int query_length;
	int length;

	if ((_res.options & RES_INIT) == 0 && res_init() != 0) {
		h_errno = NO_RECOVERY;
		return -1;
	}
	query_length = res_mkquery(QUERY, name, class_value, type, NULL, 0,
				   NULL, query, (int)sizeof(query));

	/* Handles a question that could not be written. */
	if (query_length < 0) {
		h_errno = NO_RECOVERY;
		return -1;
	}
	length = res_send(query, query_length, answer, size);

	/* Handles a server that did not answer. */
	if (length < 0) {
		h_errno = TRY_AGAIN;
		return -1;
	}

	/* Handles a reply that says no. */
	if (reply_outcome(answer, length) != 0)
		return -1;

	/* Returns the computed result. */
	return length;
}

/*
 * Implements the res querydomain operation.
 */
int
res_querydomain(
	const char *name,
	const char *domain,
	int class_value,
	int type,
	unsigned char *answer,
	int size)
{
	char joined[NS_MAXDNAME];

	/* Handles the name availability. */
	if (name == NULL) {
		h_errno = NO_RECOVERY;
		return -1;
	}

	/* A name with no domain is asked for as it stands. */
	if (domain == NULL)
		return res_query(name, class_value, type, answer, size);
	if (strlen(name) + strlen(domain) + 2U > sizeof(joined)) {
		h_errno = NO_RECOVERY;
		return -1;
	}
	(void)snprintf(joined, sizeof(joined), "%s.%s", name, domain);

	/* Returns the computed result. */
	return res_query(joined, class_value, type, answer, size);
}

/*
 * Implements the res search operation.
 *
 * A name with enough dots in it is asked for as it stands; one without is
 * tried against each domain in the search list first, which is what makes a
 * short name mean the local one.
 */
int
res_search(
	const char *name,
	int class_value,
	int type,
	unsigned char *answer,
	int size)
{
	const char *walk;
	int dots;
	int index;
	int result;
	int saved;

	/* Handles the name availability. */
	if (name == NULL) {
		h_errno = NO_RECOVERY;
		return -1;
	}
	if ((_res.options & RES_INIT) == 0 && res_init() != 0) {
		h_errno = NO_RECOVERY;
		return -1;
	}

	/* Counts what makes the name qualified already. */
	dots = 0;
	for (walk = name; *walk != '\0'; walk++)
		if (*walk == '.')
			dots++;

	/* A name that ends in a dot names the root and is never completed. */
	if (dots >= _res.ndots || (walk != name && walk[-1] == '.')) {
		result = res_query(name, class_value, type, answer, size);
		if (result >= 0)
			return result;
	}

	/* Process each remaining element. */
	saved = h_errno;
	if ((_res.options & RES_DNSRCH) != 0) {
		for (index = 0; index < MAXDNSRCH &&
		     _res.dnsrch[index] != NULL; index++) {
			result = res_querydomain(name, _res.dnsrch[index],
						 class_value, type, answer,
						 size);
			if (result >= 0)
				return result;
		}
	}

	/* A name not yet asked for on its own is asked for last. */
	if (dots < _res.ndots) {
		result = res_query(name, class_value, type, answer, size);
		if (result >= 0)
			return result;
	}
	h_errno = saved != 0 ? saved : HOST_NOT_FOUND;

	/* Reports operation failure. */
	return -1;
}

/*
 * Supports the collect rdata operation.
 *
 * Copies one record's data into the set being built.
 */
static int
collect_rdata(
	struct rdatainfo *into,
	const unsigned char *data,
	unsigned length)
{
	into->rdi_data = malloc(length != 0U ? length : 1U);

	/* Handles a failed malloc operation. */
	if (into->rdi_data == NULL)
		return -1;
	memcpy(into->rdi_data, data, length);
	into->rdi_length = length;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the getrrsetbyname operation.
 *
 * Reports every record of one type under one name, together with the
 * signatures over them, because a caller checking a signature needs both
 * and needs to know they came from the same reply.  Whether the server
 * says it checked them itself is reported in the flags.
 */
int
getrrsetbyname(
	const char *name,
	unsigned int rdclass,
	unsigned int rdtype,
	unsigned int flags,
	struct rrsetinfo **result)
{
	unsigned char answer[NS_PACKETSZ * 4];
	char owner[NS_MAXDNAME];
	struct rrsetinfo *set;
	const unsigned char *cursor;
	const unsigned char *end;
	unsigned answers;
	unsigned questions;
	unsigned index;
	unsigned type;
	unsigned record_class;
	unsigned rdlength;
	unsigned covered;
	unsigned rdatas;
	unsigned signatures;
	int length;
	int consumed;
	int outcome;

	/* Handles arguments this call cannot act on. */
	if (name == NULL || result == NULL || flags != 0U)
		return ERRSET_INVAL;
	*result = NULL;
	h_errno = 0;
	length = res_query(name, (int)rdclass, (int)rdtype, answer,
			   (int)sizeof(answer));

	/* Reports what the server said, told apart by h_errno. */
	if (length < 0) {
		if (h_errno == HOST_NOT_FOUND)
			return ERRSET_NONAME;
		if (h_errno == NO_DATA)
			return ERRSET_NODATA;
		return ERRSET_FAIL;
	}
	end = answer + length;
	questions = (unsigned)ns_get16(answer + 4);
	answers = (unsigned)ns_get16(answer + 6);
	cursor = answer + NS_HFIXEDSZ;

	/* Steps over the questions, whose answers follow. */
	for (index = 0; index < questions; index++) {
		consumed = dn_skipname(cursor, end);
		if (consumed < 0 || cursor + consumed + NS_QFIXEDSZ > end)
			return ERRSET_FAIL;
		cursor += consumed + NS_QFIXEDSZ;
	}

	/* Counts what is there before anything is allocated. */
	rdatas = 0;
	signatures = 0;
	owner[0] = '\0';
	{
		const unsigned char *walk = cursor;

		for (index = 0; index < answers; index++) {
			consumed = dn_expand(answer, end, walk, owner,
					     (int)sizeof(owner));
			if (consumed < 0 ||
			    walk + consumed + NS_RRFIXEDSZ > end)
				return ERRSET_FAIL;
			walk += consumed;
			type = (unsigned)ns_get16(walk);
			record_class = (unsigned)ns_get16(walk + 2);
			rdlength = (unsigned)ns_get16(walk + 8);
			walk += NS_RRFIXEDSZ;
			if (walk + rdlength > end)
				return ERRSET_FAIL;
			if (record_class == rdclass && type == rdtype) {
				rdatas++;
			} else if (record_class == rdclass &&
				   (type == T_RRSIG || type == T_SIG) &&
				   rdlength >= 2U) {
				/* A signature says which type it covers. */
				covered = (unsigned)ns_get16(walk);
				if (covered == rdtype)
					signatures++;
			}
			walk += rdlength;
		}
	}

	/* Handles a name that exists with no record of this type. */
	if (rdatas == 0U)
		return ERRSET_NODATA;
	set = calloc(1, sizeof(*set));

	/* Handles a failed calloc operation. */
	if (set == NULL)
		return ERRSET_NOMEMORY;
	set->rri_rdclass = rdclass;
	set->rri_rdtype = rdtype;
	set->rri_rdatas = calloc(rdatas, sizeof(*set->rri_rdatas));
	if (signatures != 0U)
		set->rri_sigs = calloc(signatures, sizeof(*set->rri_sigs));

	/* The server's own word on whether it checked the signatures. */
	if ((answer[3] & 0x20U) != 0U)
		set->rri_flags |= RRSET_VALIDATED;
	if (set->rri_rdatas == NULL ||
	    (signatures != 0U && set->rri_sigs == NULL)) {
		freerrset(set);
		return ERRSET_NOMEMORY;
	}

	/* Process each remaining element. */
	outcome = ERRSET_SUCCESS;
	for (index = 0; index < answers && outcome == ERRSET_SUCCESS;
	     index++) {
		consumed = dn_expand(answer, end, cursor, owner,
				     (int)sizeof(owner));
		if (consumed < 0) {
			outcome = ERRSET_FAIL;
			break;
		}
		cursor += consumed;
		type = (unsigned)ns_get16(cursor);
		record_class = (unsigned)ns_get16(cursor + 2);
		rdlength = (unsigned)ns_get16(cursor + 8);

		/* The set takes its name and time to live from its records. */
		if (record_class == rdclass && type == rdtype &&
		    set->rri_name == NULL) {
			set->rri_ttl = (unsigned)ns_get32(cursor + 4);
			set->rri_name = strdup(owner);
			if (set->rri_name == NULL)
				outcome = ERRSET_NOMEMORY;
		}
		cursor += NS_RRFIXEDSZ;
		if (outcome != ERRSET_SUCCESS)
			break;
		if (record_class == rdclass && type == rdtype) {
			if (collect_rdata(&set->rri_rdatas[set->rri_nrdatas],
					  cursor, rdlength) != 0)
				outcome = ERRSET_NOMEMORY;
			else
				set->rri_nrdatas++;
		} else if (record_class == rdclass &&
			   (type == T_RRSIG || type == T_SIG) &&
			   rdlength >= 2U &&
			   (unsigned)ns_get16(cursor) == rdtype) {
			if (collect_rdata(&set->rri_sigs[set->rri_nsigs],
					  cursor, rdlength) != 0)
				outcome = ERRSET_NOMEMORY;
			else
				set->rri_nsigs++;
		}
		cursor += rdlength;
	}

	/* Handles a set that could not be completed. */
	if (outcome != ERRSET_SUCCESS) {
		freerrset(set);
		return outcome;
	}
	*result = set;

	/* Reports successful completion. */
	return ERRSET_SUCCESS;
}

/*
 * Implements the freerrset operation.
 */
void
freerrset(
	struct rrsetinfo *set)
{
	unsigned index;

	/* Handles the set availability. */
	if (set == NULL)
		return;

	/* Process each remaining element. */
	if (set->rri_rdatas != NULL) {
		for (index = 0; index < set->rri_nrdatas; index++)
			free(set->rri_rdatas[index].rdi_data);
		free(set->rri_rdatas);
	}

	/* Process each remaining element. */
	if (set->rri_sigs != NULL) {
		for (index = 0; index < set->rri_nsigs; index++)
			free(set->rri_sigs[index].rdi_data);
		free(set->rri_sigs);
	}
	free(set->rri_name);
	free(set);
}
