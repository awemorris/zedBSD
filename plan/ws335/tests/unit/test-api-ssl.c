/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the public SSL and SSL_CTX functions of libssl-compat
 * (<compat/ssl/ssl.h>): the context's settings, the connection's names
 * and bounds, the handshake over a real socket (a socketpair on POSIX, a
 * loopback TCP connection on Windows), the verification results and the
 * verify callback, application data in both directions, SSL_get_error and
 * the error queue, and the closing exchange.
 *
 * The other end is tlspeer.h in its record mode.  The test moves bytes
 * between the SSL's socket and the peer from the same thread (rig_pump),
 * so every wait of the SSL is a WANT_READ or WANT_WRITE that the test
 * answers by pumping, and the order of events is the test's.
 *
 * The expected values come from the specifications: the alert numbers of
 * RFC 8446 Appendix B.2, the record size limit of RFC 8446 5.1 (a write
 * of n bytes is ceil(n / 16384) records), the extensions a ClientHello
 * must or must not carry for the versions it offers (RFC 8446 4.2.1,
 * RFC 5246 7.4.1.2), the SNI form of RFC 6066 3 (no trailing dot), the
 * X509_V_ codes of the verification failures (OpenSSL 3.5.8
 * x509_vfy.h.in) and the SSL_R_ numbers of OpenSSL 3.5.8 sslerr.h (each
 * written here as a number, not through the header under test).
 */

#include "check.h"
#include "tlspeer.h"

#include <compat/ssl/ssl.h>

#include "userland/base/libcrypto-compat/cryc-api.h"
#include "userland/base/libssl-compat/sslc-api.h"

#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* The number of reasons the library registers: 29 SSL_R_ reasons and 28 alerts. */
#define REASON_COUNT		57U

/* The most rounds a wait spends before it is a hang. */
#define TRIES			20000U

/* The size of the buffer for large writes (1 MiB). */
#define BIG			(1U << 20)

/* The record plaintext limit of RFC 8446 5.1. */
#define RECORD			16384U

/* The alert descriptions the test expects (RFC 8446 Appendix B.2). */
#define A_CLOSE_NOTIFY		0U
#define A_HANDSHAKE_FAILURE	40U
#define A_BAD_CERTIFICATE	42U
#define A_CERTIFICATE_EXPIRED	45U
#define A_CERTIFICATE_UNKNOWN	46U
#define A_UNKNOWN_CA		48U
#define A_PROTOCOL_VERSION	70U

/* The SSL_R_ numbers of OpenSSL 3.5.8 sslerr.h, written as numbers. */
#define R_BAD_LENGTH			271
#define R_BAD_WRITE_RETRY		127
#define R_CERTIFICATE_VERIFY_FAILED	134
#define R_NO_PROTOCOLS_AVAILABLE	191
#define R_PROTOCOL_IS_SHUTDOWN		207
#define R_SHUTDOWN_WHILE_IN_INIT	407
#define R_SSL_HANDSHAKE_FAILURE		229
#define R_UNINITIALIZED			276
#define R_UNEXPECTED_EOF		294
#define R_UNSUPPORTED_PROTOCOL		258

/* The X509_V_ numbers of OpenSSL 3.5.8 x509_vfy.h.in. */
#define V_OK				0
#define V_NOT_YET_VALID			9
#define V_HAS_EXPIRED			10
#define V_UNABLE_TO_GET_ISSUER_LOCALLY	20
#define V_HOSTNAME_MISMATCH		62
#define V_IP_ADDRESS_MISMATCH		64
#define V_UNSPECIFIED			1

/* The ERR_LIB_ numbers of OpenSSL 3.5.8 err.h.in. */
#define L_SYS			2
#define L_X509			11
#define L_SSL			20

/* The host name of the test certificates. */
#define HOST			"server.test"

/* Times to verify at: 2040-01-01 and 2025-06-01 00:00:00 UTC (seconds since 1970). */
#define TIME_AFTER		((int64_t)2208988800UL)
#define TIME_BEFORE		1748736000L

/* The flags of rig_open. */
#define RIG_TRUST		1U
#define RIG_NAMES		2U
#define RIG_PEER		4U
#define RIG_STD			(RIG_TRUST | RIG_NAMES | RIG_PEER)

/*
 * One test connection: the SSL's socket, the pump's socket, the scripted
 * peer, the context and the SSL, the bytes the peer made and the socket
 * did not take yet, and what the sink saw of the client's application data.
 */
struct rig {
	intptr_t client;
	intptr_t server;
	struct tlspeer *peer;
	SSL_CTX *ctx;
	SSL *ssl;
	uint8_t carry[65536];
	size_t carry_used;
	size_t carry_sent;
	unsigned long bytes_in;
	unsigned long sink_total;
	unsigned long sink_errors;
	unsigned long written_total;
	unsigned long read_total;
	unsigned long peer_total;
	int peer_error;
};

/* The root and leaf of P-256 and of P-384 for HOST. */
static struct tlspeer_pki pki256;
static struct tlspeer_pki pki384;

/* The directory of the test's files. */
static char test_dir[1024];

/* The buffers of the large reads and writes. */
static uint8_t big_buffer[BIG];

/* What the verify callback saw and what it answers. */
static unsigned callback_calls;
static int callback_preverify;
static long callback_error;
static int callback_depth;
static int callback_store_seen;
static int callback_answer;

/* The byte of a pattern at an offset: the application data of both directions. */
static uint8_t
pattern_at(
	unsigned long offset)
{
	/* A function of the whole offset, so that a repeated or a skipped run shows. */
	return (uint8_t)((offset * 251UL + (offset >> 7) * 13UL + 7UL) & 0xffUL);
}

/* Makes a connected pair of stream sockets; returns 1 on success. */
static int
make_pair(
	intptr_t *first,
	intptr_t *second)
{
#ifdef _WIN32
	SOCKET listener;
	SOCKET client;
	SOCKET server;
	struct sockaddr_in address;
	int length;

	/* A loopback listener on a port the system picks. */
	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener == INVALID_SOCKET)
		return 0;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	length = (int)sizeof(address);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 1) != 0 ||
	    getsockname(listener, (struct sockaddr *)&address, &length) != 0) {
		closesocket(listener);
		return 0;
	}

	/* The client connects (the backlog completes it) and the server accepts. */
	client = socket(AF_INET, SOCK_STREAM, 0);
	if (client == INVALID_SOCKET || connect(client, (struct sockaddr *)&address, sizeof(address)) != 0) {
		closesocket(listener);
		return 0;
	}
	server = accept(listener, NULL, NULL);
	closesocket(listener);
	if (server == INVALID_SOCKET) {
		closesocket(client);
		return 0;
	}
	*first = (intptr_t)client;
	*second = (intptr_t)server;
	return 1;
#else
	int fds[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
		return 0;
	*first = fds[0];
	*second = fds[1];
	return 1;
#endif
}

/* Puts a descriptor in non-blocking mode (1) or blocking mode (0). */
static void
set_nonblocking(
	intptr_t descriptor,
	int nonblocking)
{
#ifdef _WIN32
	u_long mode;

	mode = nonblocking ? 1UL : 0UL;
	ioctlsocket((SOCKET)descriptor, FIONBIO, &mode);
#else
	int flags;

	flags = fcntl((int)descriptor, F_GETFL, 0);
	if (nonblocking)
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;
	fcntl((int)descriptor, F_SETFL, flags);
#endif
}

/* Asks for small socket buffers on both ends, so that a large write fills them. */
static void
shrink_buffers(
	struct rig *rig)
{
	int size;

	size = 4096;
#ifdef _WIN32
	setsockopt((SOCKET)rig->client, SOL_SOCKET, SO_SNDBUF, (const char *)&size, (int)sizeof(size));
	setsockopt((SOCKET)rig->server, SOL_SOCKET, SO_RCVBUF, (const char *)&size, (int)sizeof(size));
#else
	setsockopt((int)rig->client, SOL_SOCKET, SO_SNDBUF, &size, (socklen_t)sizeof(size));
	setsockopt((int)rig->server, SOL_SOCKET, SO_RCVBUF, &size, (socklen_t)sizeof(size));
#endif
}

/* Closes a descriptor. */
static void
close_descriptor(
	intptr_t descriptor)
{
#ifdef _WIN32
	closesocket((SOCKET)descriptor);
#else
	close((int)descriptor);
#endif
}

/* Sleeps for a number of milliseconds. */
static void
pause_milliseconds(
	unsigned milliseconds)
{
#ifdef _WIN32
	Sleep(milliseconds);
#else
	struct timespec request;

	request.tv_sec = (time_t)(milliseconds / 1000U);
	request.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	nanosleep(&request, NULL);
#endif
}

/* Writes the DER of a certificate as a PEM block of 64-column lines (RFC 7468 2); returns the length. */
static size_t
pem_block(
	const uint8_t *der,
	size_t size,
	char *out)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t used;
	size_t index;
	size_t column;
	unsigned long group;
	unsigned count;
	unsigned digit;

	/* The BEGIN line, then the base64 of RFC 4648 4 with padding. */
	used = (size_t)sprintf(out, "-----BEGIN CERTIFICATE-----\n");
	column = 0;
	for (index = 0; index < size; index += 3U) {
		count = (unsigned)(size - index < 3U ? size - index : 3U);
		group = (unsigned long)der[index] << 16;
		if (count > 1U)
			group |= (unsigned long)der[index + 1U] << 8;
		if (count > 2U)
			group |= (unsigned long)der[index + 2U];
		for (digit = 0; digit < 4U; digit++) {
			if (digit > count)
				out[used++] = '=';
			else
				out[used++] = alphabet[(group >> (18U - 6U * digit)) & 0x3fUL];
			column++;
			if (column == 64U) {
				out[used++] = '\n';
				column = 0;
			}
		}
	}
	if (column != 0U)
		out[used++] = '\n';

	/* The END line. */
	used += (size_t)sprintf(out + used, "-----END CERTIFICATE-----\n");
	return used;
}

/* Writes a file in the test directory in binary mode; returns 1 on success. */
static int
write_file(
	const char *name,
	const char *bytes,
	size_t size,
	char *path_out)
{
	FILE *file;
	size_t put;

	/* The path is the directory, a slash and the name. */
	sprintf(path_out, "%s/%s", test_dir, name);
	file = fopen(path_out, "wb");
	if (file == NULL)
		return 0;
	put = size == 0U ? 0U : fwrite(bytes, 1, size, file);
	if (fclose(file) != 0)
		return 0;
	return put == size;
}

/* Takes the first queued error and checks its library and reason; then the queue must be empty. */
static void
expect_error(
	int library,
	int reason)
{
	unsigned long code;

	code = ERR_get_error();
	CHECK(code != 0UL);
	CHECK(ERR_GET_LIB(code) == library);
	CHECK(ERR_GET_REASON(code) == reason);
	CHECK(ERR_get_error() == 0UL);
}

/* Checks that the queue is empty. */
static void
expect_no_error(void)
{
	CHECK(ERR_get_error() == 0UL);
}

/* The verify callback: records what it was given and answers callback_answer. */
static int
verify_callback(
	int preverify_ok,
	X509_STORE_CTX *store)
{
	callback_calls++;
	callback_preverify = preverify_ok;
	callback_store_seen = store != NULL;
	if (store != NULL) {
		callback_error = store->error;
		callback_depth = store->depth;
	}
	return callback_answer;
}

/* Fills the peer's script for a version with TLS 1.3's middlebox change_cipher_spec as a server sends it. */
static void
peer_config(
	struct tlspeer_config *config,
	uint16_t version,
	const struct tlspeer_pki *pki)
{
	CHECK(tlspeer_config_init(config, version, pki) == 0);
	config->options = TLSPEER_OPT_CCS | TLSPEER_OPT_SNI_ACK;
	if (version == SSLC_TLS12)
		config->options = TLSPEER_OPT_SNI_ACK;
}

/* Moves the sink's check over what the peer received as application data, then forgets it. */
static void
rig_sink(
	struct rig *rig)
{
	size_t index;

	/* Each byte against the pattern at its place in the stream. */
	for (index = 0; index < rig->peer->data_size; index++) {
		if (rig->peer->data[index] != pattern_at(rig->sink_total + (unsigned long)index))
			rig->sink_errors++;
	}
	rig->sink_total += (unsigned long)rig->peer->data_size;
	rig->peer->data_size = 0;
}

/* Moves bytes between the socket and the peer once each way; returns 1 when anything moved. */
static int
rig_pump(
	struct rig *rig)
{
	uint8_t chunk[8192];
	size_t done;
	size_t taken;
	size_t put;
	int os_error;
	int error;
	int moved;
	int round;

	/* Nothing to do for a rig whose pump end is closed. */
	moved = 0;
	if (rig->server < 0)
		return 0;

	/* The bytes the peer made earlier and the socket did not take. */
	if (rig->carry_sent < rig->carry_used) {
		put = 0;
		error = cryc_os_socket_write(rig->server, rig->carry + rig->carry_sent, rig->carry_used - rig->carry_sent, &put, &os_error);
		if (error == 0 && put > 0U) {
			rig->carry_sent += put;
			moved = 1;
		}
	}
	if (rig->carry_sent == rig->carry_used) {
		rig->carry_sent = 0;
		rig->carry_used = 0;
	}

	/* The bytes the client wrote, to the peer. */
	for (round = 0; round < 16; round++) {
		done = 0;
		error = cryc_os_socket_read(rig->server, chunk, sizeof(chunk), &done, &os_error);
		if (error != 0 || done == 0U)
			break;
		rig->bytes_in += (unsigned long)done;
		error = tlspeer_input(rig->peer, chunk, done);
		if (error != 0 && rig->peer_error == 0)
			rig->peer_error = error;
		rig_sink(rig);
		moved = 1;
	}

	/* What the peer has to say, to the socket. */
	if (rig->carry_used == 0U) {
		taken = 0;
		error = tlspeer_take(rig->peer, rig->carry, sizeof(rig->carry), &taken);
		if (error != 0 && rig->peer_error == 0)
			rig->peer_error = error;
		rig->carry_used = taken;
		rig->carry_sent = 0;
		if (taken > 0U) {
			put = 0;
			error = cryc_os_socket_write(rig->server, rig->carry, taken, &put, &os_error);
			if (error == 0 && put > 0U)
				rig->carry_sent = put;
			moved = 1;
		}
	}
	return moved;
}

/* Pumps a number of rounds with a short sleep between, so bytes in flight arrive. */
static void
rig_settle(
	struct rig *rig,
	unsigned rounds)
{
	unsigned round;

	for (round = 0; round < rounds; round++) {
		rig_pump(rig);
		pause_milliseconds(1);
	}
}

/* Pumps until the sink holds at least total bytes (or the tries run out). */
static void
rig_settle_sink(
	struct rig *rig,
	unsigned long total)
{
	unsigned tries;

	for (tries = 0; tries < TRIES && rig->sink_total < total; tries++) {
		if (!rig_pump(rig))
			pause_milliseconds(1);
	}
}

/* Pumps until the peer has seen at least count alerts (or the tries run out), then a few rounds more to see any extra. */
static void
rig_settle_alerts(
	struct rig *rig,
	unsigned count)
{
	unsigned tries;

	for (tries = 0; tries < TRIES && rig->peer->alert_count < count; tries++) {
		if (!rig_pump(rig))
			pause_milliseconds(1);
	}
	rig_settle(rig, 3);
}

/* Makes a rig for a peer script; flags say whether the context trusts the root and the SSL has its names. */
static struct rig *
rig_open(
	const struct tlspeer_config *config,
	const struct tlspeer_pki *pki,
	unsigned flags)
{
	struct rig *rig;
	const unsigned char *cursor;
	X509 *root;
	X509_VERIFY_PARAM *param;
	int result;

	/* The rig, its sockets and its peer. */
	rig = calloc(1, sizeof(*rig));
	if (rig == NULL) {
		CHECK(0);
		return NULL;
	}
	CHECK(make_pair(&rig->client, &rig->server));
	set_nonblocking(rig->client, 1);
	set_nonblocking(rig->server, 1);
	CHECK(tlspeer_new(&rig->peer, config) == 0);

	/* The context: the client method, the root when trusted, peer verification when asked. */
	rig->ctx = SSL_CTX_new(TLS_client_method());
	CHECK(rig->ctx != NULL);
	if ((flags & RIG_TRUST) != 0U) {
		cursor = pki->root;
		root = d2i_X509(NULL, &cursor, (long)pki->root_size);
		CHECK(root != NULL);
		result = X509_STORE_add_cert(SSL_CTX_get_cert_store(rig->ctx), root);
		CHECK(result == 1);
		X509_free(root);
	}
	if ((flags & RIG_PEER) != 0U)
		SSL_CTX_set_verify(rig->ctx, SSL_VERIFY_PEER, NULL);

	/* The SSL on the socket, verifying at a fixed time inside the certificates' validity. */
	rig->ssl = SSL_new(rig->ctx);
	CHECK(rig->ssl != NULL);
	CHECK(SSL_set_fd(rig->ssl, (int)rig->client) == 1);
	param = SSL_get0_param(rig->ssl);
	CHECK(param != NULL);
	param->params.has_time = 1;
	param->params.time = TLSPEER_PKI_TIME;

	/* The SNI name and the host name the certificate must match. */
	if ((flags & RIG_NAMES) != 0U) {
		CHECK(SSL_set_tlsext_host_name(rig->ssl, HOST) == 1);
		CHECK(SSL_set1_host(rig->ssl, HOST) == 1);
	}
	return rig;
}

/* Frees a rig and everything in it. */
static void
rig_close(
	struct rig *rig)
{
	/* The SSL first, then the context it held, the sockets and the peer. */
	SSL_free(rig->ssl);
	SSL_CTX_free(rig->ctx);
	if (rig->client >= 0)
		close_descriptor(rig->client);
	if (rig->server >= 0)
		close_descriptor(rig->server);
	tlspeer_free(rig->peer);
	free(rig);
}

/* Runs SSL_connect with pumping until it is not waiting; returns its last result. */
static int
rig_connect(
	struct rig *rig)
{
	unsigned tries;
	int result;
	int error;

	/* Each wait is answered by moving bytes. */
	for (tries = 0; tries < TRIES; tries++) {
		result = SSL_connect(rig->ssl);
		if (result == 1)
			return 1;
		error = SSL_get_error(rig->ssl, result);
		if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
			return result;
		if (!rig_pump(rig))
			pause_milliseconds(1);
	}
	CHECK(0);
	return -2;
}

/* Completes the handshake and lets the peer see the client's last flight; the result must be 1. */
static void
rig_up(
	struct rig *rig)
{
	CHECK(rig_connect(rig) == 1);
	rig_settle(rig, 5);
	CHECK(rig->peer->state == TLSPEER_CONNECTED);
	CHECK(rig->peer_error == 0);
}

/* Reads exactly size bytes the peer sent (pumping while the SSL waits); the bytes must be the pattern of the peer's stream. */
static int
rig_read_exact(
	struct rig *rig,
	unsigned long size)
{
	unsigned long got;
	unsigned tries;
	int result;
	int error;
	int index;
	int room;

	/* Reads record after record until the count is there. */
	got = 0;
	for (tries = 0; tries < TRIES && got < size; tries++) {
		room = (int)(size - got < (unsigned long)BIG ? size - got : (unsigned long)BIG);
		result = SSL_read(rig->ssl, big_buffer, room);
		if (result > 0) {
			CHECK((unsigned long)result <= size - got);
			for (index = 0; index < result; index++) {
				if (big_buffer[index] != pattern_at(rig->read_total + got + (unsigned long)index)) {
					CHECK(0);
					return 0;
				}
			}
			got += (unsigned long)result;
			continue;
		}
		error = SSL_get_error(rig->ssl, result);
		if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
			CHECK(0);
			return 0;
		}
		if (!rig_pump(rig))
			pause_milliseconds(1);
	}
	rig->read_total += got;
	return got == size;
}

/* Has the peer send size bytes of its pattern and reads them all back; returns 1 when they arrived intact. */
static int
rig_peer_sends(
	struct rig *rig,
	unsigned long size)
{
	unsigned long index;
	unsigned long start;
	int error;

	/* The peer's bytes are the pattern from where its stream stands. */
	start = rig->peer_total;
	for (index = 0; index < size; index++)
		big_buffer[index] = pattern_at(start + index);
	error = tlspeer_send_data(rig->peer, big_buffer, (size_t)size);
	CHECK(error == 0);
	rig->peer_total += size;

	/* The client reads them. */
	return rig_read_exact(rig, size);
}

/* Writes size bytes of the client's pattern with SSL_write until all are taken (pumping while it waits); returns the final result. */
static int
rig_send(
	struct rig *rig,
	unsigned long size)
{
	unsigned long index;
	unsigned tries;
	int result;
	int error;

	/* The bytes, from where the client's stream stands. */
	for (index = 0; index < size; index++)
		big_buffer[index] = pattern_at(rig->written_total + index);

	/* The same call is repeated while the SSL waits (OpenSSL's rule). */
	result = -2;
	for (tries = 0; tries < TRIES; tries++) {
		result = SSL_write(rig->ssl, big_buffer, (int)size);
		if (result == (int)size)
			break;
		error = SSL_get_error(rig->ssl, result);
		if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
			return result;
		if (!rig_pump(rig))
			pause_milliseconds(1);
	}
	rig->written_total += size;
	rig_settle_sink(rig, rig->written_total);
	return result;
}

/* Tells whether the peer's last ClientHello offered a version in supported_versions. */
static int
hello_offers(
	const struct tlspeer *peer,
	uint16_t version)
{
	size_t index;

	for (index = 0; index < peer->hello.version_count; index++) {
		if (peer->hello.versions[index] == version)
			return 1;
	}
	return 0;
}

/* Counts the suites of the last ClientHello that are TLS 1.3 suites (0x13xx) and those that are not (the SCSV is kept apart). */
static void
hello_suites(
	const struct tlspeer *peer,
	size_t *tls13,
	size_t *tls12)
{
	size_t index;

	*tls13 = 0;
	*tls12 = 0;
	for (index = 0; index < peer->hello.suite_count; index++) {
		if ((peer->hello.suites[index] & 0xff00U) == 0x1300U)
			(*tls13)++;
		else
			(*tls12)++;
	}
}

/* The table of reasons: 57 numbers that are the sslerr.h numbers and the alerts plus 1000, each with a text. */
static void
test_tables(void)
{
	static const unsigned expected[REASON_COUNT] = {
		306, 110, 332, 108, 271, 123, 127, 134, 281, 149, 152, 156, 159, 191, 193, 101, 198, 207, 213, 407,
		229, 276, 294, 244, 258, 261, 378, 370, 267,
		1000, 1010, 1020, 1022, 1040, 1042, 1043, 1044, 1045, 1046, 1047, 1048, 1049, 1050, 1051, 1070, 1071,
		1080, 1086, 1090, 1100, 1109, 1110, 1112, 1113, 1115, 1116, 1120
	};
	char text[160];
	unsigned long code;
	size_t index;
	size_t other;
	SSL_CTX *ctx;

	/* The count and the numbers, in order. */
	CHECK(sslc_reason_count == REASON_COUNT);
	for (index = 0; index < REASON_COUNT && index < sslc_reason_count; index++)
		CHECK(sslc_reason_numbers[index] == expected[index]);

	/* Every text is there, plain, and different from every other. */
	for (index = 0; index < sslc_reason_count; index++) {
		CHECK(sslc_reason_texts[index] != NULL);
		CHECK(strlen(sslc_reason_texts[index]) >= 8U);
		CHECK(strlen(sslc_reason_texts[index]) < 80U);
		for (other = index + 1U; other < sslc_reason_count; other++)
			CHECK(strcmp(sslc_reason_texts[index], sslc_reason_texts[other]) != 0);
	}

	/* A context registers the texts, and ERR_error_string_n then shows each one in the OpenSSL 3 layout. */
	ctx = SSL_CTX_new(TLS_client_method());
	CHECK(ctx != NULL);
	for (index = 0; index < sslc_reason_count; index++) {
		code = ((unsigned long)L_SSL << 23) | (unsigned long)expected[index];
		ERR_error_string_n(code, text, sizeof(text));
		CHECK(strncmp(text, "error:", 6) == 0);
		CHECK(strstr(text, sslc_reason_texts[index]) != NULL);
		CHECK(strstr(text, "reason(") == NULL);
	}
	SSL_CTX_free(ctx);
}

/* The context: the method, options, ctrls, the store, references, and null arguments. */
static void
test_ctx(void)
{
	SSL_CTX *ctx;
	SSL *first;
	SSL *second;
	X509_STORE *store;
	const SSL_METHOD *method;
	struct ssl_method_st foreign;
	uint64_t options;

	/* The client method is one constant object. */
	method = TLS_client_method();
	CHECK(method != NULL);
	CHECK(TLS_client_method() == method);

	/* A missing method and a method that is not the client method fail with one error and no context. */
	ERR_clear_error();
	CHECK(SSL_CTX_new(NULL) == NULL);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);
	foreign.client = 1;
	CHECK(SSL_CTX_new(&foreign) == NULL);
	expect_error(L_SSL, (int)CRYC_R_UNSUPPORTED);

	/* A new context: no error, an empty store, no options, versions unset. */
	ctx = SSL_CTX_new(method);
	CHECK(ctx != NULL);
	expect_no_error();
	store = SSL_CTX_get_cert_store(ctx);
	CHECK(store != NULL);
	CHECK(SSL_CTX_get_cert_store(ctx) == store);
	CHECK(store->store.count == 0U);
	CHECK(SSL_CTX_set_options(ctx, 0) == 0U);
	CHECK(SSL_CTX_ctrl(ctx, SSL_CTRL_GET_MIN_PROTO_VERSION, 0, NULL) == 0L);
	CHECK(SSL_CTX_ctrl(ctx, SSL_CTRL_GET_MAX_PROTO_VERSION, 0, NULL) == 0L);

	/* Options are added, never removed, and the whole set comes back; bits the library does not act on are kept. */
	options = SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
	CHECK(options == SSL_OP_IGNORE_UNEXPECTED_EOF);
	options = SSL_CTX_set_options(ctx, ((uint64_t)1 << 40) | 1U);
	CHECK(options == (SSL_OP_IGNORE_UNEXPECTED_EOF | ((uint64_t)1 << 40) | 1U));
	options = SSL_CTX_set_options(ctx, 0);
	CHECK(options == (SSL_OP_IGNORE_UNEXPECTED_EOF | ((uint64_t)1 << 40) | 1U));
	options = SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
	CHECK(options == (SSL_OP_IGNORE_UNEXPECTED_EOF | ((uint64_t)1 << 40) | 1U));

	/* The version ctrls: 0, TLS 1.2 and TLS 1.3 are accepted and read back; another value is 0 with one error and no change. */
	CHECK(SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) == 1L);
	CHECK(SSL_CTX_ctrl(ctx, SSL_CTRL_GET_MIN_PROTO_VERSION, 0, NULL) == TLS1_3_VERSION);
	CHECK(SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) == 1L);
	CHECK(SSL_CTX_ctrl(ctx, SSL_CTRL_GET_MIN_PROTO_VERSION, 0, NULL) == TLS1_2_VERSION);
	CHECK(SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION) == 1L);
	CHECK(SSL_CTX_ctrl(ctx, SSL_CTRL_GET_MAX_PROTO_VERSION, 0, NULL) == TLS1_2_VERSION);
	CHECK(SSL_CTX_ctrl(ctx, SSL_CTRL_GET_MIN_PROTO_VERSION, 0, NULL) == TLS1_2_VERSION);
	ERR_clear_error();
	CHECK(SSL_CTX_set_min_proto_version(ctx, 0x0301) == 0L);
	expect_error(L_SSL, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	CHECK(SSL_CTX_set_max_proto_version(ctx, 0x0305) == 0L);
	expect_error(L_SSL, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	CHECK(SSL_CTX_set_max_proto_version(ctx, 0x0302) == 0L);
	expect_error(L_SSL, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	CHECK(SSL_CTX_ctrl(ctx, SSL_CTRL_GET_MIN_PROTO_VERSION, 0, NULL) == TLS1_2_VERSION);
	CHECK(SSL_CTX_ctrl(ctx, SSL_CTRL_GET_MAX_PROTO_VERSION, 0, NULL) == TLS1_2_VERSION);
	CHECK(SSL_CTX_set_min_proto_version(ctx, 0) == 1L);
	CHECK(SSL_CTX_set_max_proto_version(ctx, 0) == 1L);
	CHECK(SSL_CTX_ctrl(ctx, SSL_CTRL_GET_MIN_PROTO_VERSION, 0, NULL) == 0L);
	CHECK(SSL_CTX_ctrl(ctx, SSL_CTRL_GET_MAX_PROTO_VERSION, 0, NULL) == 0L);
	expect_no_error();

	/* Another command is 0. */
	CHECK(SSL_CTX_ctrl(ctx, 0, 0, NULL) == 0L);
	CHECK(SSL_CTX_ctrl(ctx, SSL_CTRL_SET_TLSEXT_HOSTNAME, 0, NULL) == 0L);
	CHECK(SSL_CTX_ctrl(ctx, 122, 0, NULL) == 0L);
	CHECK(SSL_CTX_ctrl(ctx, 125, 0, NULL) == 0L);
	CHECK(SSL_CTX_ctrl(ctx, 129, 0, NULL) == 0L);
	CHECK(SSL_CTX_ctrl(ctx, 132, 0, NULL) == 0L);

	/* An SSL holds the context: freeing the caller's reference first leaves it working, and the last SSL frees it. */
	first = SSL_new(ctx);
	second = SSL_new(ctx);
	CHECK(first != NULL);
	CHECK(second != NULL);
	CHECK(ctx->references == 3L);
	SSL_CTX_free(ctx);
	CHECK(first->ctx->references == 2L);
	SSL_free(first);
	CHECK(second->ctx->references == 1L);
	SSL_free(second);

	/* Null arguments: a missing context or SSL fails with one error, and the free functions ignore NULL. */
	ERR_clear_error();
	SSL_CTX_free(NULL);
	SSL_free(NULL);
	expect_no_error();
	CHECK(SSL_new(NULL) == NULL);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);
	CHECK(SSL_CTX_get_cert_store(NULL) == NULL);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);
	CHECK(SSL_CTX_set_options(NULL, 1) == 0U);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);
	CHECK(SSL_CTX_ctrl(NULL, SSL_CTRL_GET_MIN_PROTO_VERSION, 0, NULL) == 0L);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);
	SSL_CTX_set_verify(NULL, SSL_VERIFY_PEER, NULL);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);
}

/* Loading trust anchors: PEM files, bad blocks, missing files, the directory argument, and the default paths. */
static void
test_ctx_files(void)
{
	static char text[8192];
	static char path[1200];
	static char missing[1200];
	SSL_CTX *ctx;
	size_t used;
	struct tlspeer_config config;
	struct rig *rig;

	ctx = SSL_CTX_new(TLS_client_method());
	CHECK(ctx != NULL);
	ERR_clear_error();

	/* A file with one certificate: 1, and the store holds it; the same file again changes nothing. */
	used = pem_block(pki256.root, pki256.root_size, text);
	CHECK(write_file("api-ssl-one.pem", text, used, path));
	CHECK(SSL_CTX_load_verify_locations(ctx, path, NULL) == 1);
	CHECK(ctx->store->store.count == 1U);
	CHECK(SSL_CTX_load_verify_locations(ctx, path, NULL) == 1);
	CHECK(ctx->store->store.count == 1U);
	expect_no_error();

	/* A file with two certificates adds the new one. */
	used = pem_block(pki256.root, pki256.root_size, text);
	used += pem_block(pki384.root, pki384.root_size, text + used);
	CHECK(write_file("api-ssl-two.pem", text, used, path));
	CHECK(SSL_CTX_load_verify_locations(ctx, path, NULL) == 1);
	CHECK(ctx->store->store.count == 2U);
	expect_no_error();
	SSL_CTX_free(ctx);

	/* A bad block between two good ones: 0 with one error, and the good ones are added. */
	ctx = SSL_CTX_new(TLS_client_method());
	CHECK(ctx != NULL);
	used = pem_block(pki256.root, pki256.root_size, text);
	used += (size_t)sprintf(text + used, "-----BEGIN CERTIFICATE-----\nAAAAAAAA\n-----END CERTIFICATE-----\n");
	used += pem_block(pki384.root, pki384.root_size, text + used);
	CHECK(write_file("api-ssl-bad.pem", text, used, path));
	CHECK(SSL_CTX_load_verify_locations(ctx, path, NULL) == 0);
	expect_error(L_X509, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	CHECK(ctx->store->store.count == 2U);
	SSL_CTX_free(ctx);

	/* A file without a certificate, an empty file and a missing file: 0 with one error each, the store as it was. */
	ctx = SSL_CTX_new(TLS_client_method());
	CHECK(ctx != NULL);
	used = (size_t)sprintf(text, "this is not a PEM file\n");
	CHECK(write_file("api-ssl-none.pem", text, used, path));
	CHECK(SSL_CTX_load_verify_locations(ctx, path, NULL) == 0);
	expect_error(L_X509, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	CHECK(write_file("api-ssl-empty.pem", text, 0U, path));
	CHECK(SSL_CTX_load_verify_locations(ctx, path, NULL) == 0);
	expect_error(L_X509, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	sprintf(missing, "%s/api-ssl-missing.pem", test_dir);
	CHECK(SSL_CTX_load_verify_locations(ctx, missing, NULL) == 0);
	expect_error(L_X509, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	CHECK(ctx->store->store.count == 0U);

	/* A directory argument is not supported, even beside a good file; nothing is added. */
	used = pem_block(pki256.root, pki256.root_size, text);
	CHECK(write_file("api-ssl-one.pem", text, used, path));
	CHECK(SSL_CTX_load_verify_locations(ctx, path, test_dir) == 0);
	expect_error(L_SSL, (int)CRYC_R_UNSUPPORTED);
	CHECK(SSL_CTX_load_verify_locations(ctx, NULL, test_dir) == 0);
	expect_error(L_SSL, (int)CRYC_R_UNSUPPORTED);
	CHECK(ctx->store->store.count == 0U);

	/* Neither place named, and no context. */
	CHECK(SSL_CTX_load_verify_locations(ctx, NULL, NULL) == 0);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);
	CHECK(SSL_CTX_load_verify_locations(NULL, path, NULL) == 0);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);

	/* The default paths: 1 whatever the system holds, no error; no context is 0 with one error. */
	CHECK(SSL_CTX_set_default_verify_paths(ctx) == 1);
	expect_no_error();
	CHECK(SSL_CTX_set_default_verify_paths(NULL) == 0);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);
	SSL_CTX_free(ctx);

	/* A context that trusts the root through a file completes a handshake. */
	used = pem_block(pki256.root, pki256.root_size, text);
	CHECK(write_file("api-ssl-trust.pem", text, used, path));
	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_NAMES | RIG_PEER);
	CHECK(SSL_CTX_load_verify_locations(rig->ctx, path, NULL) == 1);
	rig_up(rig);
	CHECK(SSL_get_verify_result(rig->ssl) == V_OK);
	rig_close(rig);
}

/* The SSL object: the socket, the names, the bounds, the parameters, and a connection that never started. */
static void
test_ssl(void)
{
	SSL_CTX *ctx;
	SSL *ssl;
	X509_VERIFY_PARAM *param;
	X509_VERIFY_PARAM *other;
	char buffer[16];
	char name[400];
	size_t index;

	ctx = SSL_CTX_new(TLS_client_method());
	CHECK(ctx != NULL);
	ssl = SSL_new(ctx);
	CHECK(ssl != NULL);
	ERR_clear_error();

	/* A new SSL: no socket, no verification yet, nothing buffered. */
	CHECK(ssl->descriptor == -1);
	CHECK(SSL_get_verify_result(ssl) == V_OK);
	CHECK(SSL_pending(ssl) == 0);
	CHECK(SSL_get_error(ssl, 1) == SSL_ERROR_NONE);

	/* The socket: a negative value is 0 with one error, a value is kept. */
	CHECK(SSL_set_fd(ssl, -1) == 0);
	expect_error(L_SSL, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	CHECK(ssl->descriptor == -1);
	CHECK(SSL_set_fd(NULL, 3) == 0);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);

	/* Without a socket nothing can start: -1, SSL_ERROR_SSL, SSL_R_UNINITIALIZED, and the SSL stays unstarted. */
	CHECK(SSL_connect(ssl) == -1);
	CHECK(SSL_get_error(ssl, -1) == SSL_ERROR_SSL);
	expect_error(L_SSL, R_UNINITIALIZED);
	CHECK(SSL_read(ssl, buffer, 8) == -1);
	CHECK(SSL_get_error(ssl, -1) == SSL_ERROR_SSL);
	expect_error(L_SSL, R_UNINITIALIZED);
	CHECK(SSL_write(ssl, buffer, 8) == -1);
	CHECK(SSL_get_error(ssl, -1) == SSL_ERROR_SSL);
	expect_error(L_SSL, R_UNINITIALIZED);
	CHECK(ssl->started == 0);

	/* A shutdown before the handshake: -1 with SSL_R_SHUTDOWN_WHILE_IN_INIT, nothing sent. */
	CHECK(SSL_shutdown(ssl) == -1);
	CHECK(SSL_get_error(ssl, -1) == SSL_ERROR_SSL);
	expect_error(L_SSL, R_SHUTDOWN_WHILE_IN_INIT);

	/* Bad lengths and buffers are refused before anything starts, whatever the socket. */
	CHECK(SSL_read(ssl, buffer, 0) == -1);
	expect_error(L_SSL, R_BAD_LENGTH);
	CHECK(SSL_read(ssl, buffer, -1) == -1);
	expect_error(L_SSL, R_BAD_LENGTH);
	CHECK(SSL_write(ssl, buffer, 0) == -1);
	expect_error(L_SSL, R_BAD_LENGTH);
	CHECK(SSL_write(ssl, buffer, -7) == -1);
	expect_error(L_SSL, R_BAD_LENGTH);
	CHECK(SSL_read(ssl, NULL, 4) == -1);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);
	CHECK(SSL_write(ssl, NULL, 4) == -1);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);

	/* The SNI name: a name is kept in its sent form (no trailing dot, lower case). */
	CHECK(SSL_set_tlsext_host_name(ssl, "Example.COM.") == 1);
	CHECK(strcmp(ssl->server_name, "example.com") == 0);
	CHECK(SSL_set_tlsext_host_name(ssl, "a-b.example.org") == 1);
	CHECK(strcmp(ssl->server_name, "a-b.example.org") == 0);

	/* A name that is not valid is 0 with one error and the old name stays. */
	CHECK(SSL_set_tlsext_host_name(ssl, "bad name") == 0);
	expect_error(L_SSL, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	CHECK(SSL_set_tlsext_host_name(ssl, "") == 0);
	expect_error(L_SSL, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	CHECK(SSL_set_tlsext_host_name(ssl, "a..b") == 0);
	expect_error(L_SSL, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	for (index = 0; index < sizeof(name) - 1U; index++)
		name[index] = 'a';
	name[sizeof(name) - 1U] = '\0';
	CHECK(SSL_set_tlsext_host_name(ssl, name) == 0);
	expect_error(L_SSL, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	CHECK(strcmp(ssl->server_name, "a-b.example.org") == 0);

	/* The wrong name type is 0; an address is accepted and nothing will be sent for it; NULL clears. */
	CHECK(SSL_ctrl(ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, 1, (void *)"example.com") == 0L);
	expect_error(L_SSL, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	CHECK(SSL_set_tlsext_host_name(ssl, "192.0.2.7") == 1);
	CHECK(ssl->server_name[0] == '\0');
	CHECK(SSL_set_tlsext_host_name(ssl, "x.example") == 1);
	CHECK(SSL_set_tlsext_host_name(ssl, "2001:db8::1") == 1);
	CHECK(ssl->server_name[0] == '\0');
	CHECK(SSL_set_tlsext_host_name(ssl, "x.example") == 1);
	CHECK(SSL_ctrl(ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, NULL) == 1L);
	CHECK(ssl->server_name[0] == '\0');
	expect_no_error();

	/* A text that looks like an IPv4 address but is not one is a host name (a label of digits is valid). */
	CHECK(SSL_set_tlsext_host_name(ssl, "256.1.1.1") == 1);
	CHECK(strcmp(ssl->server_name, "256.1.1.1") == 0);
	CHECK(SSL_set_tlsext_host_name(ssl, "01.2.3.4") == 1);
	CHECK(strcmp(ssl->server_name, "01.2.3.4") == 0);
	CHECK(SSL_set_tlsext_host_name(ssl, "1.2.3") == 1);
	CHECK(strcmp(ssl->server_name, "1.2.3") == 0);
	CHECK(SSL_set_tlsext_host_name(ssl, "1.2.3.4.5") == 1);
	CHECK(strcmp(ssl->server_name, "1.2.3.4.5") == 0);
	CHECK(SSL_set_tlsext_host_name(ssl, "1.2.3.") == 1);
	CHECK(strcmp(ssl->server_name, "1.2.3") == 0);
	CHECK(SSL_set_tlsext_host_name(ssl, "255.255.255.255") == 1);
	CHECK(ssl->server_name[0] == '\0');
	CHECK(SSL_set_tlsext_host_name(ssl, "0.0.0.0") == 1);
	CHECK(ssl->server_name[0] == '\0');
	CHECK(SSL_set_tlsext_host_name(ssl, "1.2.3.4") == 1);
	CHECK(ssl->server_name[0] == '\0');
	CHECK(SSL_set_tlsext_host_name(ssl, "1.2.3.04") == 1);
	CHECK(strcmp(ssl->server_name, "1.2.3.04") == 0);
	CHECK(SSL_set_tlsext_host_name(ssl, "1.2..4") == 0);
	expect_error(L_SSL, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	CHECK(strcmp(ssl->server_name, "1.2.3.04") == 0);
	expect_no_error();

	/* The host name to match: a name, an address, a name replacing an address, and the empty text and NULL clearing both. */
	param = SSL_get0_param(ssl);
	CHECK(param != NULL);
	CHECK(SSL_set1_host(ssl, "Host.Example") == 1);
	CHECK(param->params.has_host == 1);
	CHECK(strcmp(param->params.host, "host.example") == 0);
	CHECK(param->params.ip_size == 0U);
	CHECK(SSL_set1_host(ssl, "192.0.2.9") == 1);
	CHECK(param->params.has_host == 0);
	CHECK(param->params.ip_size == 4U);
	CHECK(param->params.ip[0] == 192U && param->params.ip[1] == 0U && param->params.ip[2] == 2U && param->params.ip[3] == 9U);
	CHECK(SSL_set1_host(ssl, "2001:db8::2") == 1);
	CHECK(param->params.has_host == 0);
	CHECK(param->params.ip_size == 16U);
	CHECK(param->params.ip[0] == 0x20U && param->params.ip[1] == 0x01U && param->params.ip[2] == 0x0dU && param->params.ip[3] == 0xb8U);
	CHECK(param->params.ip[15] == 2U);
	CHECK(SSL_set1_host(ssl, "host.example") == 1);
	CHECK(param->params.has_host == 1);
	CHECK(param->params.ip_size == 0U);
	CHECK(SSL_set1_host(ssl, NULL) == 1);
	CHECK(param->params.has_host == 0);
	CHECK(param->params.ip_size == 0U);
	CHECK(SSL_set1_host(ssl, "host.example") == 1);
	CHECK(SSL_set1_host(ssl, "") == 1);
	CHECK(param->params.has_host == 0);
	expect_no_error();

	/* A text that is neither: 0 with one error and both settings cleared. */
	CHECK(SSL_set1_host(ssl, "host.example") == 1);
	CHECK(SSL_set1_host(ssl, "bad name") == 0);
	expect_error(L_X509, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	CHECK(param->params.has_host == 0);
	CHECK(param->params.ip_size == 0U);
	CHECK(SSL_set1_host(ssl, "host.example") == 1);
	CHECK(SSL_set1_host(ssl, "1:2:3") == 0);
	expect_error(L_X509, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	CHECK(param->params.has_host == 0);
	CHECK(param->params.ip_size == 0U);
	CHECK(SSL_set1_host(NULL, "x.example") == 0);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);

	/* The parameters are the SSL's own and the same object each time. */
	other = SSL_get0_param(ssl);
	CHECK(other == param);
	CHECK(SSL_get0_param(NULL) == NULL);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);

	/* The SSL's own version bounds override the context's and are read back; the context keeps its own. */
	CHECK(SSL_ctrl(ssl, SSL_CTRL_GET_MIN_PROTO_VERSION, 0, NULL) == 0L);
	CHECK(SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) == 1L);
	CHECK(SSL_ctrl(ssl, SSL_CTRL_GET_MIN_PROTO_VERSION, 0, NULL) == TLS1_2_VERSION);
	CHECK(SSL_ctrl(ssl, SSL_CTRL_SET_MIN_PROTO_VERSION, TLS1_3_VERSION, NULL) == 1L);
	CHECK(SSL_ctrl(ssl, SSL_CTRL_GET_MIN_PROTO_VERSION, 0, NULL) == TLS1_3_VERSION);
	CHECK(SSL_ctrl(ssl, SSL_CTRL_SET_MAX_PROTO_VERSION, TLS1_3_VERSION, NULL) == 1L);
	CHECK(SSL_ctrl(ssl, SSL_CTRL_GET_MAX_PROTO_VERSION, 0, NULL) == TLS1_3_VERSION);
	CHECK(SSL_CTX_ctrl(ctx, SSL_CTRL_GET_MIN_PROTO_VERSION, 0, NULL) == TLS1_2_VERSION);
	CHECK(SSL_CTX_ctrl(ctx, SSL_CTRL_GET_MAX_PROTO_VERSION, 0, NULL) == 0L);
	CHECK(SSL_ctrl(ssl, SSL_CTRL_SET_MIN_PROTO_VERSION, 0x0301, NULL) == 0L);
	expect_error(L_SSL, (int)CRYC_R_PASSED_INVALID_ARGUMENT);
	CHECK(SSL_ctrl(ssl, SSL_CTRL_GET_MIN_PROTO_VERSION, 0, NULL) == TLS1_3_VERSION);
	CHECK(SSL_ctrl(ssl, 0, 0, NULL) == 0L);
	CHECK(SSL_ctrl(ssl, 125, 0, NULL) == 0L);
	CHECK(SSL_ctrl(NULL, SSL_CTRL_GET_MIN_PROTO_VERSION, 0, NULL) == 0L);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);

	/* Null arguments of the I/O functions: -1 with one error. */
	CHECK(SSL_connect(NULL) == -1);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);
	CHECK(SSL_read(NULL, buffer, 1) == -1);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);
	CHECK(SSL_write(NULL, buffer, 1) == -1);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);
	CHECK(SSL_shutdown(NULL) == -1);
	expect_error(L_SSL, (int)CRYC_R_PASSED_NULL_PARAMETER);
	CHECK(SSL_pending(NULL) == 0);
	CHECK(SSL_get_error(NULL, 0) == SSL_ERROR_SSL);
	CHECK(SSL_get_verify_result(NULL) == V_UNSPECIFIED);
	expect_no_error();

	/* Freeing a never-started SSL and its context. */
	SSL_free(ssl);
	SSL_CTX_free(ctx);
}

/* One handshake of a version, suite, group and certificate curve; the SSL must verify and the peer must be connected. */
static void
handshake_case(
	uint16_t version,
	uint16_t suite,
	uint16_t group,
	const struct tlspeer_pki *pki,
	int retry)
{
	struct tlspeer_config config;
	struct rig *rig;
	int first;

	/* The script. */
	peer_config(&config, version, pki);
	config.suite = suite;
	config.group = group;
	config.hello_retry = retry;
	rig = rig_open(&config, pki, RIG_STD);
	ERR_clear_error();

	/* The first call has nothing to read yet and says so. */
	first = SSL_connect(rig->ssl);
	CHECK(first == -1);
	CHECK(SSL_get_error(rig->ssl, first) == SSL_ERROR_WANT_READ);
	expect_no_error();

	/* The rest of the handshake. */
	rig_up(rig);
	CHECK(SSL_get_error(rig->ssl, 1) == SSL_ERROR_NONE);
	CHECK(SSL_get_verify_result(rig->ssl) == V_OK);
	CHECK(SSL_connect(rig->ssl) == 1);
	expect_no_error();

	/* The hello carried the name in its sent form, and the peer saw the retry it asked for. */
	CHECK(rig->peer->hello.has_server_name == 1);
	CHECK(strcmp(rig->peer->hello.server_name, HOST) == 0);
	CHECK(rig->peer->retries == (unsigned)(retry != 0));
	rig_close(rig);
}

/* Handshakes: TLS 1.3 and 1.2, the suites, both groups, a retry, both curves of certificate. */
static void
test_handshake(void)
{
	/* TLS 1.3: the three suites with x25519, with P-256 after a HelloRetryRequest, P-384 certificates. */
	handshake_case(SSLC_TLS13, SSLC_SUITE_AES_128_GCM_SHA256, SSLC_GROUP_X25519, &pki256, 0);
	handshake_case(SSLC_TLS13, SSLC_SUITE_AES_256_GCM_SHA384, SSLC_GROUP_X25519, &pki256, 0);
	handshake_case(SSLC_TLS13, SSLC_SUITE_CHACHA20_POLY1305_SHA256, SSLC_GROUP_X25519, &pki384, 0);
	handshake_case(SSLC_TLS13, SSLC_SUITE_AES_128_GCM_SHA256, SSLC_GROUP_SECP256R1, &pki256, 1);
	handshake_case(SSLC_TLS13, SSLC_SUITE_AES_256_GCM_SHA384, SSLC_GROUP_SECP256R1, &pki384, 1);

	/* TLS 1.2 with ECDHE: GCM and ChaCha20-Poly1305, both groups, both curves. */
	handshake_case(SSLC_TLS12, SSLC_SUITE_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, SSLC_GROUP_X25519, &pki256, 0);
	handshake_case(SSLC_TLS12, SSLC_SUITE_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, SSLC_GROUP_SECP256R1, &pki256, 0);
	handshake_case(SSLC_TLS12, SSLC_SUITE_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, SSLC_GROUP_X25519, &pki384, 0);
}

/* The verification results: untrusted, name mismatch, address mismatch, expired, not yet valid, no verification. */
static void
test_verify(void)
{
	struct tlspeer_config config;
	struct rig *rig;
	X509_VERIFY_PARAM *param;
	int result;

	/* An untrusted root: the handshake fails, with X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY and the alert unknown_ca. */
	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_NAMES | RIG_PEER);
	ERR_clear_error();
	result = rig_connect(rig);
	CHECK(result == -1);
	CHECK(SSL_get_error(rig->ssl, result) == SSL_ERROR_SSL);
	expect_error(L_SSL, R_CERTIFICATE_VERIFY_FAILED);
	CHECK(SSL_get_verify_result(rig->ssl) == V_UNABLE_TO_GET_ISSUER_LOCALLY);
	rig_settle_alerts(rig, 1U);
	CHECK(rig->peer->alert_count == 1U);
	CHECK(rig->peer->alert_level == 2U);
	CHECK(rig->peer->alert_description == A_UNKNOWN_CA);

	/* After the failure the connection is shut down: every later call says so, once each, and sends nothing. */
	CHECK(SSL_connect(rig->ssl) == -1);
	CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_SSL);
	expect_error(L_SSL, R_PROTOCOL_IS_SHUTDOWN);
	CHECK(SSL_read(rig->ssl, big_buffer, 10) == -1);
	CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_SSL);
	expect_error(L_SSL, R_PROTOCOL_IS_SHUTDOWN);
	CHECK(SSL_write(rig->ssl, big_buffer, 10) == -1);
	CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_SSL);
	expect_error(L_SSL, R_PROTOCOL_IS_SHUTDOWN);
	CHECK(SSL_shutdown(rig->ssl) == -1);
	CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_SSL);
	expect_error(L_SSL, R_PROTOCOL_IS_SHUTDOWN);
	CHECK(SSL_get_verify_result(rig->ssl) == V_UNABLE_TO_GET_ISSUER_LOCALLY);
	CHECK(SSL_pending(rig->ssl) == 0);
	rig_settle(rig, 3);
	CHECK(rig->peer->alert_count == 1U);
	rig_close(rig);

	/* A name that does not match: X509_V_ERR_HOSTNAME_MISMATCH and bad_certificate. */
	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_STD);
	CHECK(SSL_set1_host(rig->ssl, "other.test") == 1);
	result = rig_connect(rig);
	CHECK(result == -1);
	CHECK(SSL_get_error(rig->ssl, result) == SSL_ERROR_SSL);
	expect_error(L_SSL, R_CERTIFICATE_VERIFY_FAILED);
	CHECK(SSL_get_verify_result(rig->ssl) == V_HOSTNAME_MISMATCH);
	rig_settle_alerts(rig, 1U);
	CHECK(rig->peer->alert_description == A_BAD_CERTIFICATE);
	rig_close(rig);

	/* An address the certificate does not hold: X509_V_ERR_IP_ADDRESS_MISMATCH. */
	peer_config(&config, SSLC_TLS12, &pki256);
	rig = rig_open(&config, &pki256, RIG_STD);
	CHECK(SSL_set1_host(rig->ssl, "192.0.2.1") == 1);
	result = rig_connect(rig);
	CHECK(result == -1);
	expect_error(L_SSL, R_CERTIFICATE_VERIFY_FAILED);
	CHECK(SSL_get_verify_result(rig->ssl) == V_IP_ADDRESS_MISMATCH);
	rig_settle_alerts(rig, 1U);
	CHECK(rig->peer->alert_description == A_BAD_CERTIFICATE);
	rig_close(rig);

	/* After the validity period, and before it: X509_V_ERR_CERT_HAS_EXPIRED and _NOT_YET_VALID, both certificate_expired. */
	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_STD);
	param = SSL_get0_param(rig->ssl);
	param->params.time = TIME_AFTER;
	result = rig_connect(rig);
	CHECK(result == -1);
	expect_error(L_SSL, R_CERTIFICATE_VERIFY_FAILED);
	CHECK(SSL_get_verify_result(rig->ssl) == V_HAS_EXPIRED);
	rig_settle_alerts(rig, 1U);
	CHECK(rig->peer->alert_description == A_CERTIFICATE_EXPIRED);
	rig_close(rig);
	rig = rig_open(&config, &pki256, RIG_STD);
	param = SSL_get0_param(rig->ssl);
	param->params.time = TIME_BEFORE;
	result = rig_connect(rig);
	CHECK(result == -1);
	expect_error(L_SSL, R_CERTIFICATE_VERIFY_FAILED);
	CHECK(SSL_get_verify_result(rig->ssl) == V_NOT_YET_VALID);
	rig_settle_alerts(rig, 1U);
	CHECK(rig->peer->alert_description == A_CERTIFICATE_EXPIRED);
	rig_close(rig);

	/* SSL_VERIFY_NONE: an untrusted chain and a wrong name go through, and the result is still recorded. */
	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_NAMES);
	CHECK(SSL_set1_host(rig->ssl, "other.test") == 1);
	rig_up(rig);
	CHECK(SSL_get_verify_result(rig->ssl) == V_UNABLE_TO_GET_ISSUER_LOCALLY);
	expect_no_error();
	rig_close(rig);

	/* Other mode bits are ignored: PEER with FAIL_IF_NO_PEER_CERT and CLIENT_ONCE still verifies the server. */
	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_NAMES);
	SSL_CTX_set_verify(rig->ctx, SSL_VERIFY_PEER | 2 | 4, NULL);
	result = rig_connect(rig);
	CHECK(result == -1);
	expect_error(L_SSL, R_CERTIFICATE_VERIFY_FAILED);
	rig_close(rig);
	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_NAMES);
	SSL_CTX_set_verify(rig->ctx, SSL_VERIFY_PEER, NULL);
	SSL_CTX_set_verify(rig->ctx, 8 | 2, NULL);
	rig_up(rig);
	CHECK(SSL_get_verify_result(rig->ssl) == V_UNABLE_TO_GET_ISSUER_LOCALLY);
	rig_close(rig);
}

/* The verify callback: when it is called, what it is told, and how its answer decides. */
static void
test_callback(void)
{
	struct tlspeer_config config;
	struct rig *rig;
	int result;

	/* An untrusted chain: the callback is told 0 and the code, and 1 lets the handshake go on with the code kept. */
	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_NAMES | RIG_PEER);
	callback_calls = 0;
	callback_answer = 1;
	SSL_CTX_set_verify(rig->ctx, SSL_VERIFY_PEER, verify_callback);
	rig_up(rig);
	CHECK(callback_calls == 1U);
	CHECK(callback_preverify == 0);
	CHECK(callback_store_seen == 1);
	CHECK(callback_error == V_UNABLE_TO_GET_ISSUER_LOCALLY);
	CHECK(callback_depth == 0);
	CHECK(SSL_get_verify_result(rig->ssl) == V_UNABLE_TO_GET_ISSUER_LOCALLY);
	expect_no_error();
	rig_close(rig);

	/* The same chain with the answer 0: the handshake fails with the verification reason and unknown_ca. */
	peer_config(&config, SSLC_TLS12, &pki256);
	rig = rig_open(&config, &pki256, RIG_NAMES | RIG_PEER);
	callback_calls = 0;
	callback_answer = 0;
	SSL_CTX_set_verify(rig->ctx, SSL_VERIFY_PEER, verify_callback);
	result = rig_connect(rig);
	CHECK(result == -1);
	CHECK(SSL_get_error(rig->ssl, result) == SSL_ERROR_SSL);
	expect_error(L_SSL, R_CERTIFICATE_VERIFY_FAILED);
	CHECK(callback_calls == 1U);
	CHECK(callback_preverify == 0);
	CHECK(SSL_get_verify_result(rig->ssl) == V_UNABLE_TO_GET_ISSUER_LOCALLY);
	rig_settle_alerts(rig, 1U);
	CHECK(rig->peer->alert_description == A_UNKNOWN_CA);
	rig_close(rig);

	/* A valid chain: the callback is told 1 and X509_V_OK; its answer 1 goes on, its answer 0 refuses with certificate_unknown. */
	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_STD);
	callback_calls = 0;
	callback_answer = 1;
	SSL_CTX_set_verify(rig->ctx, SSL_VERIFY_PEER, verify_callback);
	rig_up(rig);
	CHECK(callback_calls == 1U);
	CHECK(callback_preverify == 1);
	CHECK(callback_error == V_OK);
	CHECK(SSL_get_verify_result(rig->ssl) == V_OK);
	rig_close(rig);
	rig = rig_open(&config, &pki256, RIG_STD);
	callback_calls = 0;
	callback_answer = 0;
	SSL_CTX_set_verify(rig->ctx, SSL_VERIFY_PEER, verify_callback);
	result = rig_connect(rig);
	CHECK(result == -1);
	expect_error(L_SSL, R_CERTIFICATE_VERIFY_FAILED);
	CHECK(callback_calls == 1U);
	CHECK(callback_preverify == 1);
	rig_settle_alerts(rig, 1U);
	CHECK(rig->peer->alert_description == A_CERTIFICATE_UNKNOWN);
	rig_close(rig);

	/* A callback set and then cleared (NULL) leaves the chain's own verdict. */
	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_NAMES | RIG_PEER);
	callback_calls = 0;
	callback_answer = 1;
	SSL_CTX_set_verify(rig->ctx, SSL_VERIFY_PEER, verify_callback);
	SSL_CTX_set_verify(rig->ctx, SSL_VERIFY_PEER, NULL);
	result = rig_connect(rig);
	CHECK(result == -1);
	expect_error(L_SSL, R_CERTIFICATE_VERIFY_FAILED);
	CHECK(callback_calls == 0U);
	rig_close(rig);

	/* SSL_VERIFY_NONE never calls the callback. */
	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_NAMES);
	callback_calls = 0;
	callback_answer = 0;
	SSL_CTX_set_verify(rig->ctx, SSL_VERIFY_NONE, verify_callback);
	rig_up(rig);
	CHECK(callback_calls == 0U);
	rig_close(rig);
}

/* The version bounds and options: what the ClientHello offers, and the failures of a range with nothing left. */
static void
test_versions(void)
{
	struct tlspeer_config config;
	struct rig *rig;
	size_t tls13;
	size_t tls12;
	int result;

	/* The default range offers TLS 1.3 and TLS 1.2 (supported_versions names both, and both kinds of suite). */
	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_STD);
	rig_up(rig);
	CHECK(hello_offers(rig->peer, 0x0304));
	CHECK(hello_offers(rig->peer, 0x0303));
	hello_suites(rig->peer, &tls13, &tls12);
	CHECK(tls13 == 3U);
	CHECK(tls12 >= 6U);
	rig_close(rig);

	/* A minimum of TLS 1.3 offers only TLS 1.3 and its suites. */
	rig = rig_open(&config, &pki256, RIG_STD);
	CHECK(SSL_CTX_set_min_proto_version(rig->ctx, TLS1_3_VERSION) == 1L);
	rig_up(rig);
	CHECK(rig->peer->hello.version_count == 1U);
	CHECK(hello_offers(rig->peer, 0x0304));
	hello_suites(rig->peer, &tls13, &tls12);
	CHECK(tls13 == 3U);
	CHECK(tls12 == 0U);
	rig_close(rig);

	/* SSL_OP_NO_TLSv1_2 does the same. */
	rig = rig_open(&config, &pki256, RIG_STD);
	SSL_CTX_set_options(rig->ctx, SSL_OP_NO_TLSv1_2);
	rig_up(rig);
	CHECK(rig->peer->hello.version_count == 1U);
	CHECK(hello_offers(rig->peer, 0x0304));
	rig_close(rig);

	/* The SSL's own minimum overrides the context's, which stays unchanged. */
	rig = rig_open(&config, &pki256, RIG_STD);
	CHECK(SSL_ctrl(rig->ssl, SSL_CTRL_SET_MIN_PROTO_VERSION, TLS1_3_VERSION, NULL) == 1L);
	rig_up(rig);
	CHECK(rig->peer->hello.version_count == 1U);
	CHECK(SSL_CTX_ctrl(rig->ctx, SSL_CTRL_GET_MIN_PROTO_VERSION, 0, NULL) == 0L);
	CHECK(SSL_ctrl(rig->ssl, SSL_CTRL_SET_MIN_PROTO_VERSION, TLS1_2_VERSION, NULL) == 0L);
	ERR_clear_error();
	CHECK(SSL_ctrl(rig->ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, (void *)"later.test") == 0L);
	ERR_clear_error();
	rig_close(rig);

	/* A maximum of TLS 1.2 offers no TLS 1.3 (no TLS 1.3 suite, no supported_versions of 1.3) and a TLS 1.2 server completes. */
	peer_config(&config, SSLC_TLS12, &pki256);
	rig = rig_open(&config, &pki256, RIG_STD);
	CHECK(SSL_CTX_set_max_proto_version(rig->ctx, TLS1_2_VERSION) == 1L);
	rig_up(rig);
	CHECK(!hello_offers(rig->peer, 0x0304));
	hello_suites(rig->peer, &tls13, &tls12);
	CHECK(tls13 == 0U);
	CHECK(tls12 >= 6U);
	rig_close(rig);

	/* SSL_OP_NO_TLSv1_3 does the same. */
	rig = rig_open(&config, &pki256, RIG_STD);
	SSL_CTX_set_options(rig->ctx, SSL_OP_NO_TLSv1_3);
	rig_up(rig);
	CHECK(!hello_offers(rig->peer, 0x0304));
	rig_close(rig);

	/* A TLS 1.2 server answering a client whose minimum is 1.3: protocol_version, and SSL_R_UNSUPPORTED_PROTOCOL. */
	peer_config(&config, SSLC_TLS12, &pki256);
	rig = rig_open(&config, &pki256, RIG_STD);
	CHECK(SSL_CTX_set_min_proto_version(rig->ctx, TLS1_3_VERSION) == 1L);
	result = rig_connect(rig);
	CHECK(result == -1);
	CHECK(SSL_get_error(rig->ssl, result) == SSL_ERROR_SSL);
	expect_error(L_SSL, R_UNSUPPORTED_PROTOCOL);
	rig_settle_alerts(rig, 1U);
	CHECK(rig->peer->alert_description == A_PROTOCOL_VERSION);
	rig_close(rig);

	/* No version left: both switched off, a minimum above the maximum, a minimum of 1.3 with 1.3 switched off. Nothing is sent. */
	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_STD);
	SSL_CTX_set_options(rig->ctx, SSL_OP_NO_TLSv1_2 | SSL_OP_NO_TLSv1_3);
	CHECK(SSL_connect(rig->ssl) == -1);
	CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_SSL);
	expect_error(L_SSL, R_NO_PROTOCOLS_AVAILABLE);
	rig_settle(rig, 3);
	CHECK(rig->bytes_in == 0UL);
	CHECK(rig->ssl->started == 0);
	rig_close(rig);
	rig = rig_open(&config, &pki256, RIG_STD);
	CHECK(SSL_CTX_set_min_proto_version(rig->ctx, TLS1_3_VERSION) == 1L);
	CHECK(SSL_CTX_set_max_proto_version(rig->ctx, TLS1_2_VERSION) == 1L);
	CHECK(SSL_connect(rig->ssl) == -1);
	expect_error(L_SSL, R_NO_PROTOCOLS_AVAILABLE);
	rig_close(rig);
	rig = rig_open(&config, &pki256, RIG_STD);
	CHECK(SSL_CTX_set_min_proto_version(rig->ctx, TLS1_3_VERSION) == 1L);
	SSL_CTX_set_options(rig->ctx, SSL_OP_NO_TLSv1_3);
	CHECK(SSL_connect(rig->ssl) == -1);
	expect_error(L_SSL, R_NO_PROTOCOLS_AVAILABLE);
	rig_close(rig);
	rig = rig_open(&config, &pki256, RIG_STD);
	CHECK(SSL_CTX_set_max_proto_version(rig->ctx, TLS1_2_VERSION) == 1L);
	SSL_CTX_set_options(rig->ctx, SSL_OP_NO_TLSv1_2);
	CHECK(SSL_connect(rig->ssl) == -1);
	expect_error(L_SSL, R_NO_PROTOCOLS_AVAILABLE);
	rig_close(rig);

	/* A setting made on the context after SSL_new and before the first call is seen (the settings are read at the start). */
	rig = rig_open(&config, &pki256, RIG_STD);
	CHECK(SSL_CTX_set_min_proto_version(rig->ctx, TLS1_3_VERSION) == 1L);
	rig_up(rig);
	CHECK(rig->peer->hello.version_count == 1U);
	rig_close(rig);

	/* The address in the SNI position sends no server_name; the host name of an address is checked against its SAN only. */
	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_TRUST | RIG_PEER);
	CHECK(SSL_set_tlsext_host_name(rig->ssl, "192.0.2.1") == 1);
	CHECK(SSL_set1_host(rig->ssl, NULL) == 1);
	rig_up(rig);
	CHECK(rig->peer->hello.has_server_name == 0);
	rig_close(rig);
}

/* Writing: sizes around the record limit, the records made, retries after a wait. */
static void
test_write(void)
{
	static const unsigned long sizes[] = { 1UL, 2UL, 15UL, 16UL, 17UL, 255UL, 256UL, 1000UL, 16383UL, 16384UL, 16385UL, 32768UL, 40000UL };
	struct tlspeer_config config;
	struct rig *rig;
	unsigned long before;
	unsigned long expected;
	size_t index;
	int version;
	int result;

	for (version = 0; version < 2; version++) {
		peer_config(&config, version == 0 ? SSLC_TLS13 : SSLC_TLS12, &pki256);
		rig = rig_open(&config, &pki256, RIG_STD);
		rig_up(rig);

		/* Each size: all of it is written, in ceil(size / 16384) records, and the peer reads the same bytes. */
		for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
			before = rig->peer->records_in;
			result = rig_send(rig, sizes[index]);
			CHECK(result == (int)sizes[index]);
			CHECK(SSL_get_error(rig->ssl, result) == SSL_ERROR_NONE);
			expected = (sizes[index] + (unsigned long)RECORD - 1UL) / (unsigned long)RECORD;
			CHECK(rig->peer->records_in - before == expected);
			CHECK(rig->sink_total == rig->written_total);
		}
		CHECK(rig->sink_errors == 0UL);
		CHECK(rig->peer_error == 0);
		expect_no_error();
		rig_close(rig);
	}
}

/* A write that fills the socket: WANT_WRITE with part of it taken, the retry rule, and no byte lost or repeated. */
static void
test_write_wait(void)
{
	struct tlspeer_config config;
	struct rig *rig;
	unsigned long index;
	unsigned long taken;
	unsigned tries;
	int result;
	int blocked;
	int error;

	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_STD);
	rig_up(rig);
	shrink_buffers(rig);

	/* A megabyte into small buffers with nobody reading: the call waits for the socket. */
	for (index = 0; index < BIG; index++)
		big_buffer[index] = pattern_at(index);
	result = SSL_write(rig->ssl, big_buffer, (int)BIG);
	error = SSL_get_error(rig->ssl, result);
	blocked = result == -1 && error == SSL_ERROR_WANT_WRITE;
#ifndef _WIN32
	CHECK(blocked);
#endif
	if (blocked) {
		/* Whole records were taken and kept; nothing else has been taken. */
		taken = (unsigned long)rig->ssl->conn.taken;
		CHECK(taken > 0UL);
		CHECK(taken < (unsigned long)BIG);
		CHECK(taken % (unsigned long)RECORD == 0UL);
		expect_no_error();
		CHECK(SSL_pending(rig->ssl) == 0);

		/* A retry naming fewer bytes than were taken is refused, and is not a failure of the connection. */
		CHECK(SSL_write(rig->ssl, big_buffer, (int)(taken - 1UL)) == -1);
		CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_SSL);
		expect_error(L_SSL, R_BAD_WRITE_RETRY);
		CHECK(SSL_write(rig->ssl, big_buffer, 1) == -1);
		CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_SSL);
		expect_error(L_SSL, R_BAD_WRITE_RETRY);
		CHECK((unsigned long)rig->ssl->conn.taken == taken);

		/* A retry naming exactly the bytes taken is not refused (it may complete or wait). */
		result = SSL_write(rig->ssl, big_buffer, (int)taken);
		error = SSL_get_error(rig->ssl, result);
		CHECK(result == (int)taken || (result == -1 && error == SSL_ERROR_WANT_WRITE));
		expect_no_error();
	}

	/* The same call repeated while the peer reads: the whole megabyte arrives, in order, once. */
	rig->written_total = 0UL;
	for (tries = 0; tries < TRIES; tries++) {
		if (rig->sink_total >= (unsigned long)BIG)
			break;
		if (!rig_pump(rig))
			pause_milliseconds(1);
	}
	if (blocked) {
		/* The retry above ended at taken bytes only if it completed; the full write is repeated to its end. */
		for (tries = 0; tries < TRIES; tries++) {
			result = SSL_write(rig->ssl, big_buffer, (int)BIG);
			if (result == (int)BIG)
				break;
			error = SSL_get_error(rig->ssl, result);
			CHECK(error == SSL_ERROR_WANT_WRITE || error == SSL_ERROR_WANT_READ);
			if (!rig_pump(rig))
				pause_milliseconds(1);
		}
		CHECK(result == (int)BIG);
	}
	rig_settle_sink(rig, (unsigned long)BIG);
	CHECK(rig->sink_total == (unsigned long)BIG);
	CHECK(rig->sink_errors == 0UL);
	CHECK(rig->peer_error == 0);
	rig_close(rig);
}

/* Reading: sizes, partial reads and SSL_pending, a wait with nothing to read, bad lengths that leave the connection usable. */
static void
test_read(void)
{
	struct tlspeer_config config;
	struct rig *rig;
	unsigned long total;
	int version;
	int result;

	for (version = 0; version < 2; version++) {
		peer_config(&config, version == 0 ? SSLC_TLS13 : SSLC_TLS12, &pki256);
		rig = rig_open(&config, &pki256, RIG_STD);
		rig_up(rig);

		/* Nothing to read yet: the call waits, with nothing queued. */
		ERR_clear_error();
		result = SSL_read(rig->ssl, big_buffer, 100);
		CHECK(result == -1);
		CHECK(SSL_get_error(rig->ssl, result) == SSL_ERROR_WANT_READ);
		expect_no_error();
		CHECK(SSL_pending(rig->ssl) == 0);

		/* Bad lengths are refused and the connection goes on. */
		CHECK(SSL_read(rig->ssl, big_buffer, 0) == -1);
		CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_SSL);
		expect_error(L_SSL, R_BAD_LENGTH);
		CHECK(SSL_read(rig->ssl, big_buffer, -3) == -1);
		expect_error(L_SSL, R_BAD_LENGTH);
		CHECK(SSL_write(rig->ssl, big_buffer, 0) == -1);
		CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_SSL);
		expect_error(L_SSL, R_BAD_LENGTH);

		/* A small record is read whole; nothing stays buffered. */
		CHECK(rig_peer_sends(rig, 10UL));
		CHECK(SSL_pending(rig->ssl) == 0);

		/* A record read in pieces: SSL_pending is the rest of the record, exactly (the connection reads no further). */
		total = rig->peer_total;
		for (result = 0; result < 100; result++)
			big_buffer[result] = pattern_at(total + (unsigned long)result);
		CHECK(tlspeer_send_data(rig->peer, big_buffer, 100U) == 0);
		rig->peer_total += 100UL;
		do {
			result = SSL_read(rig->ssl, big_buffer + 1000, 30);
			if (result < 0)
				rig_pump(rig);
		} while (result < 0);
		CHECK(result == 30);
		CHECK(SSL_pending(rig->ssl) == 70);
		CHECK(SSL_read(rig->ssl, big_buffer + 1030, 30) == 30);
		CHECK(SSL_pending(rig->ssl) == 40);
		CHECK(SSL_read(rig->ssl, big_buffer + 1060, 100) == 40);
		CHECK(SSL_pending(rig->ssl) == 0);
		CHECK(memcmp(big_buffer + 1000, big_buffer, 0) == 0);
		for (result = 0; result < 100; result++)
			CHECK(big_buffer[1000 + result] == pattern_at(total + (unsigned long)result));
		rig->read_total += 100UL;

		/* Sizes around the record limit: a read gives at most one record. */
		CHECK(rig_peer_sends(rig, 1UL));
		CHECK(rig_peer_sends(rig, 16383UL));
		CHECK(rig_peer_sends(rig, 16384UL));
		CHECK(rig_peer_sends(rig, 16385UL));
		CHECK(rig_peer_sends(rig, 40000UL));
		CHECK(rig->peer_error == 0);
		expect_no_error();
		rig_close(rig);
	}

	/* A read after a TLS 1.3 NewSessionTicket and a KeyUpdate goes on with the data after them. */
	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_STD);
	rig_up(rig);
	CHECK(tlspeer_send_ticket(rig->peer) == 0);
	CHECK(rig_peer_sends(rig, 100UL));
	CHECK(tlspeer_send_key_update(rig->peer, 1) == 0);
	CHECK(rig_peer_sends(rig, 100UL));
	rig_settle(rig, 5);
	CHECK(rig->peer_error == 0);
	CHECK(rig_send(rig, 50UL) == 50);
	expect_no_error();
	rig_close(rig);
}


/* A read or a write first starts the handshake; the first call waits and the handshake completes through the calls. */
static void
test_implicit_handshake(void)
{
	struct tlspeer_config config;
	struct rig *rig;
	unsigned tries;
	int result;
	int error;
	int version;
	int index;

	for (version = 0; version < 2; version++) {
		/* SSL_write first: the handshake runs inside it, and the write completes. */
		peer_config(&config, version == 0 ? SSLC_TLS13 : SSLC_TLS12, &pki256);
		rig = rig_open(&config, &pki256, RIG_STD);
		ERR_clear_error();
		for (index = 0; index < 100; index++)
			big_buffer[index] = pattern_at((unsigned long)index);
		result = SSL_write(rig->ssl, big_buffer, 100);
		CHECK(result == -1);
		CHECK(SSL_get_error(rig->ssl, result) == SSL_ERROR_WANT_READ);
		CHECK(rig->ssl->started == 1);
		result = -1;
		for (tries = 0; tries < TRIES && result != 100; tries++) {
			if (!rig_pump(rig))
				pause_milliseconds(1);
			result = SSL_write(rig->ssl, big_buffer, 100);
			if (result != 100) {
				error = SSL_get_error(rig->ssl, result);
				CHECK(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
			}
		}
		CHECK(result == 100);
		rig->written_total = 100UL;
		rig_settle_sink(rig, 100UL);
		CHECK(rig->sink_total == 100UL);
		CHECK(rig->sink_errors == 0UL);
		CHECK(SSL_connect(rig->ssl) == 1);
		expect_no_error();
		rig_close(rig);

		/* SSL_read first: it waits through the handshake and after it, and the connection is complete. */
		rig = rig_open(&config, &pki256, RIG_STD);
		for (tries = 0; tries < 200U; tries++) {
			result = SSL_read(rig->ssl, big_buffer, 10);
			CHECK(result == -1);
			CHECK(SSL_get_error(rig->ssl, result) == SSL_ERROR_WANT_READ);
			rig_pump(rig);
			pause_milliseconds(1);
		}
		CHECK(rig->peer->state == TLSPEER_CONNECTED);
		CHECK(SSL_connect(rig->ssl) == 1);
		CHECK(SSL_get_verify_result(rig->ssl) == V_OK);
		CHECK(rig_peer_sends(rig, 20UL));
		expect_no_error();
		rig_close(rig);
	}
}

/* Writes bytes straight into the pump's end of the socket (what the peer would never send). */
static void
rig_inject(
	struct rig *rig,
	const uint8_t *bytes,
	size_t size)
{
	size_t put;
	int os_error;
	int error;

	error = cryc_os_socket_write(rig->server, bytes, size, &put, &os_error);
	CHECK(error == 0);
	CHECK(put == size);
}

/* Reads and drops what the client wrote, without giving it to the peer. */
static void
rig_discard(
	struct rig *rig)
{
	uint8_t chunk[4096];
	size_t done;
	unsigned tries;
	int os_error;
	int error;

	/* The bytes are in flight for a moment on Windows; stop at the first quiet read after some bytes. */
	for (tries = 0; tries < 50U; tries++) {
		done = 0;
		error = cryc_os_socket_read(rig->server, chunk, sizeof(chunk), &done, &os_error);
		if (error == 0 && done > 0U) {
			rig->bytes_in += (unsigned long)done;
			continue;
		}
		if (rig->bytes_in > 0UL)
			break;
		pause_milliseconds(2);
	}
}

/* The closing exchange, ends of the stream, alerts, socket failures and a connection that is shut down. */
static void
test_close(void)
{
	static const uint8_t close_notify[7] = { 0x15, 0x03, 0x03, 0x00, 0x02, 0x01, 0x00 };
	static const uint8_t fatal_failure[7] = { 0x15, 0x03, 0x03, 0x00, 0x02, 0x02, 0x28 };
	struct tlspeer_config config;
	struct rig *rig;
	unsigned tries;
	int result;
	int version;

	for (version = 0; version < 2; version++) {
		peer_config(&config, version == 0 ? SSLC_TLS13 : SSLC_TLS12, &pki256);

		/* The peer sends data and then its close_notify: the data first, then 0 and ZERO_RETURN, repeatedly. */
		rig = rig_open(&config, &pki256, RIG_STD);
		rig_up(rig);
		CHECK(rig_peer_sends(rig, 10UL));
		CHECK(tlspeer_send_alert(rig->peer, 1U, A_CLOSE_NOTIFY) == 0);
		result = -1;
		for (tries = 0; tries < TRIES && result == -1; tries++) {
			result = SSL_read(rig->ssl, big_buffer, 10);
			if (result == -1) {
				CHECK(SSL_get_error(rig->ssl, result) == SSL_ERROR_WANT_READ);
				rig_pump(rig);
			}
		}
		CHECK(result == 0);
		CHECK(SSL_get_error(rig->ssl, result) == SSL_ERROR_ZERO_RETURN);
		CHECK(SSL_read(rig->ssl, big_buffer, 10) == 0);
		CHECK(SSL_get_error(rig->ssl, 0) == SSL_ERROR_ZERO_RETURN);
		expect_no_error();

		/* Our close then completes the exchange: 1, and the peer sees exactly one close_notify however often it is called. */
		CHECK(SSL_shutdown(rig->ssl) == 1);
		CHECK(SSL_get_error(rig->ssl, 1) == SSL_ERROR_NONE);
		rig_settle_alerts(rig, 1U);
		CHECK(rig->peer->close_notify == 1);
		CHECK(rig->peer->alert_count == 1U);
		CHECK(rig->peer->alert_level == 1U);
		CHECK(rig->peer->alert_description == A_CLOSE_NOTIFY);
		CHECK(SSL_shutdown(rig->ssl) == 1);
		rig_settle_alerts(rig, 1U);
		CHECK(rig->peer->alert_count == 1U);

		/* After our close_notify a write is refused with SSL_R_PROTOCOL_IS_SHUTDOWN. */
		CHECK(SSL_write(rig->ssl, big_buffer, 10) == -1);
		CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_SSL);
		expect_error(L_SSL, R_PROTOCOL_IS_SHUTDOWN);
		rig_close(rig);

		/* Our close first: 0 (not an error, nothing queued), once on the wire; data may still be read; then the peer's close makes the next call 1. */
		rig = rig_open(&config, &pki256, RIG_STD);
		rig_up(rig);
		ERR_clear_error();
		CHECK(SSL_shutdown(rig->ssl) == 0);
		expect_no_error();
		rig_settle_alerts(rig, 1U);
		CHECK(rig->peer->close_notify == 1);
		CHECK(rig->peer->alert_count == 1U);
		CHECK(SSL_shutdown(rig->ssl) == 0);
		rig_settle_alerts(rig, 1U);
		CHECK(rig->peer->alert_count == 1U);
		CHECK(rig_peer_sends(rig, 7UL));
		CHECK(tlspeer_send_alert(rig->peer, 1U, A_CLOSE_NOTIFY) == 0);
		result = -1;
		for (tries = 0; tries < TRIES && result == -1; tries++) {
			result = SSL_read(rig->ssl, big_buffer, 10);
			if (result == -1)
				rig_pump(rig);
		}
		CHECK(result == 0);
		CHECK(SSL_get_error(rig->ssl, result) == SSL_ERROR_ZERO_RETURN);
		CHECK(SSL_shutdown(rig->ssl) == 1);
		expect_no_error();
		rig_close(rig);

		/* The stream ends without a close_notify after some data: the data first, then SSL_ERROR_SSL with the unexpected-EOF reason. */
		rig = rig_open(&config, &pki256, RIG_STD);
		rig_up(rig);
		CHECK(tlspeer_send_data(rig->peer, big_buffer, 10U) == 0);
		rig->peer_total += 10UL;
		rig_settle(rig, 5);
		close_descriptor(rig->server);
		rig->server = -1;
		pause_milliseconds(20);
		ERR_clear_error();
		CHECK(SSL_read(rig->ssl, big_buffer, 100) == 10);
		CHECK(SSL_read(rig->ssl, big_buffer, 100) == -1);
		CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_SSL);
		expect_error(L_SSL, R_UNEXPECTED_EOF);
		CHECK(SSL_read(rig->ssl, big_buffer, 100) == -1);
		CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_SSL);
		expect_error(L_SSL, R_UNEXPECTED_EOF);
		rig_close(rig);

		/* With SSL_OP_IGNORE_UNEXPECTED_EOF the same end is a clean close: 0 and ZERO_RETURN, nothing queued. */
		rig = rig_open(&config, &pki256, RIG_STD);
		SSL_CTX_set_options(rig->ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
		rig_up(rig);
		CHECK(tlspeer_send_data(rig->peer, big_buffer, 10U) == 0);
		rig->peer_total += 10UL;
		rig_settle(rig, 5);
		close_descriptor(rig->server);
		rig->server = -1;
		pause_milliseconds(20);
		ERR_clear_error();
		CHECK(SSL_read(rig->ssl, big_buffer, 100) == 10);
		CHECK(SSL_read(rig->ssl, big_buffer, 100) == 0);
		CHECK(SSL_get_error(rig->ssl, 0) == SSL_ERROR_ZERO_RETURN);
		expect_no_error();
		rig_close(rig);

		/* A fatal alert from the peer after the handshake: SSL_ERROR_SSL with 1000 + the description, then the connection is shut down and sends nothing. */
		rig = rig_open(&config, &pki256, RIG_STD);
		rig_up(rig);
		CHECK(tlspeer_send_alert(rig->peer, 2U, A_HANDSHAKE_FAILURE) == 0);
		result = -1;
		for (tries = 0; tries < TRIES && result == -1; tries++) {
			result = SSL_read(rig->ssl, big_buffer, 10);
			if (result == -1 && SSL_get_error(rig->ssl, result) == SSL_ERROR_WANT_READ)
				rig_pump(rig);
			else
				break;
		}
		CHECK(result == -1);
		CHECK(SSL_get_error(rig->ssl, result) == SSL_ERROR_SSL);
		expect_error(L_SSL, 1000 + (int)A_HANDSHAKE_FAILURE);
		CHECK(SSL_read(rig->ssl, big_buffer, 10) == -1);
		expect_error(L_SSL, R_PROTOCOL_IS_SHUTDOWN);
		CHECK(SSL_write(rig->ssl, big_buffer, 10) == -1);
		expect_error(L_SSL, R_PROTOCOL_IS_SHUTDOWN);
		CHECK(SSL_shutdown(rig->ssl) == -1);
		CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_SSL);
		expect_error(L_SSL, R_PROTOCOL_IS_SHUTDOWN);
		rig_settle(rig, 5);
		CHECK(rig->peer->alert_count == 0U);
		rig_close(rig);

		/* The socket fails under a read: SSL_ERROR_SYSCALL, errno given back by SSL_get_error, the system's code queued; then shut down. */
		rig = rig_open(&config, &pki256, RIG_STD);
		rig_up(rig);
		close_descriptor(rig->client);
		rig->client = -1;
		ERR_clear_error();
		errno = 0;
		result = SSL_read(rig->ssl, big_buffer, 10);
		CHECK(result == -1);
		errno = 0;
		CHECK(SSL_get_error(rig->ssl, result) == SSL_ERROR_SYSCALL);
		CHECK(errno != 0);
#ifndef _WIN32
		CHECK(errno == EBADF);
#endif
		{
			unsigned long code;

			code = ERR_get_error();
			CHECK(ERR_GET_LIB(code) == L_SYS);
			CHECK(ERR_GET_REASON(code) == errno);
			CHECK(ERR_get_error() == 0UL);
		}
		CHECK(SSL_get_error(rig->ssl, 3) == SSL_ERROR_NONE);
		CHECK(SSL_read(rig->ssl, big_buffer, 10) == -1);
		CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_SSL);
		expect_error(L_SSL, R_PROTOCOL_IS_SHUTDOWN);
		rig_close(rig);

		/* The socket fails under a write. */
		rig = rig_open(&config, &pki256, RIG_STD);
		rig_up(rig);
		close_descriptor(rig->client);
		rig->client = -1;
		ERR_clear_error();
		result = SSL_write(rig->ssl, big_buffer, 10);
		CHECK(result == -1);
		errno = 0;
		CHECK(SSL_get_error(rig->ssl, result) == SSL_ERROR_SYSCALL);
		CHECK(errno != 0);
		ERR_clear_error();
		rig_close(rig);
	}

	/* The stream ends during the handshake: SSL_ERROR_SSL with the unexpected-EOF reason. */
	peer_config(&config, SSLC_TLS13, &pki256);
	rig = rig_open(&config, &pki256, RIG_STD);
	CHECK(SSL_connect(rig->ssl) == -1);
	CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_WANT_READ);
	rig_discard(rig);
	close_descriptor(rig->server);
	rig->server = -1;
	pause_milliseconds(20);
	ERR_clear_error();
	CHECK(SSL_connect(rig->ssl) == -1);
	CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_SSL);
	expect_error(L_SSL, R_UNEXPECTED_EOF);
	rig_close(rig);

	/* A close_notify during the handshake is a clean close: 0 and ZERO_RETURN. */
	rig = rig_open(&config, &pki256, RIG_STD);
	CHECK(SSL_connect(rig->ssl) == -1);
	CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_WANT_READ);
	rig_inject(rig, close_notify, sizeof(close_notify));
	pause_milliseconds(5);
	ERR_clear_error();
	result = SSL_connect(rig->ssl);
	CHECK(result == 0);
	CHECK(SSL_get_error(rig->ssl, result) == SSL_ERROR_ZERO_RETURN);
	expect_no_error();
	rig_close(rig);

	/* A fatal alert during the handshake: SSL_ERROR_SSL with 1000 + the description; shut down after; nothing sent in answer. */
	rig = rig_open(&config, &pki256, RIG_STD);
	CHECK(SSL_connect(rig->ssl) == -1);
	CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_WANT_READ);
	rig_inject(rig, fatal_failure, sizeof(fatal_failure));
	pause_milliseconds(5);
	ERR_clear_error();
	result = SSL_connect(rig->ssl);
	CHECK(result == -1);
	CHECK(SSL_get_error(rig->ssl, result) == SSL_ERROR_SSL);
	expect_error(L_SSL, 1000 + (int)A_HANDSHAKE_FAILURE);
	CHECK(SSL_connect(rig->ssl) == -1);
	expect_error(L_SSL, R_PROTOCOL_IS_SHUTDOWN);
	CHECK(SSL_shutdown(rig->ssl) == -1);
	expect_error(L_SSL, R_PROTOCOL_IS_SHUTDOWN);
	rig_settle(rig, 5);
	CHECK(rig->peer->alert_count == 0U);
	rig_close(rig);

	/* A shutdown while the handshake is in progress sends nothing and says so. */
	rig = rig_open(&config, &pki256, RIG_STD);
	CHECK(SSL_connect(rig->ssl) == -1);
	CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_WANT_READ);
	ERR_clear_error();
	CHECK(SSL_shutdown(rig->ssl) == -1);
	CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_SSL);
	expect_error(L_SSL, R_SHUTDOWN_WHILE_IN_INIT);
	rig_settle(rig, 5);
	CHECK(rig->peer->alert_count == 0U);
	rig_close(rig);

	/* An SSL freed in the middle of a handshake, after its context was released first. */
	rig = rig_open(&config, &pki256, RIG_STD);
	CHECK(SSL_connect(rig->ssl) == -1);
	SSL_CTX_free(rig->ctx);
	rig->ctx = NULL;
	CHECK(SSL_get_error(rig->ssl, -1) == SSL_ERROR_WANT_READ);
	CHECK(rig_connect(rig) == 1);
	rig_close(rig);
}

/* The state one thread of the thread test works on. */
struct thread_work {
	SSL_CTX *ctx;
	unsigned bit;
	unsigned failures;
};

#ifdef _WIN32
typedef HANDLE test_thread;
#define THREAD_RETURN	DWORD
#define THREAD_CALL	WINAPI
#else
typedef pthread_t test_thread;
#define THREAD_RETURN	void *
#define THREAD_CALL
#endif

/* Makes and frees SSL objects on a shared context while setting its options and reading its bounds. */
static THREAD_RETURN THREAD_CALL
thread_main(
	void *argument)
{
	struct thread_work *work;
	SSL *ssl;
	unsigned round;

	/* The same context in every thread; each thread makes its own SSL objects. */
	work = (struct thread_work *)argument;
	for (round = 0; round < 300U; round++) {
		ssl = SSL_new(work->ctx);
		if (ssl == NULL) {
			work->failures++;
			continue;
		}
		if (SSL_set1_host(ssl, "thread.example") != 1)
			work->failures++;
		if (SSL_ctrl(ssl, SSL_CTRL_GET_MIN_PROTO_VERSION, 0, NULL) != 0L)
			work->failures++;
		SSL_CTX_set_options(work->ctx, (uint64_t)1 << work->bit);
		SSL_CTX_set_verify(work->ctx, SSL_VERIFY_PEER, NULL);
		if (SSL_CTX_ctrl(work->ctx, SSL_CTRL_GET_MAX_PROTO_VERSION, 0, NULL) != 0L)
			work->failures++;
		SSL_free(ssl);
	}
#ifdef _WIN32
	return 0;
#else
	return NULL;
#endif
}

/* Several threads share one context: references come back to one and every option bit is set. */
static void
test_threads(void)
{
	struct thread_work work[4];
	test_thread threads[4];
	SSL_CTX *ctx;
	unsigned index;
	int started[4];

	ctx = SSL_CTX_new(TLS_client_method());
	CHECK(ctx != NULL);
	for (index = 0; index < 4U; index++) {
		work[index].ctx = ctx;
		work[index].bit = 40U + index;
		work[index].failures = 0;
#ifdef _WIN32
		threads[index] = CreateThread(NULL, 0, thread_main, &work[index], 0, NULL);
		started[index] = threads[index] != NULL;
#else
		started[index] = pthread_create(&threads[index], NULL, thread_main, &work[index]) == 0;
#endif
		CHECK(started[index]);
	}
	for (index = 0; index < 4U; index++) {
		if (!started[index])
			continue;
#ifdef _WIN32
		WaitForSingleObject(threads[index], INFINITE);
		CloseHandle(threads[index]);
#else
		pthread_join(threads[index], NULL);
#endif
		CHECK(work[index].failures == 0U);
	}
	CHECK(ctx->references == 1L);
	CHECK((SSL_CTX_set_options(ctx, 0) & (((uint64_t)15) << 40)) == (((uint64_t)15) << 40));
	SSL_CTX_free(ctx);
}

/* The directory of the program is where the test's files go; Windows runs in the directory of its own. */
static void
find_test_directory(
	int argc,
	char **argv)
{
	size_t length;

	UNUSED_PARAMETER(argc);
	strcpy(test_dir, ".");
#ifndef _WIN32
	length = strlen(argv[0]);
	while (length > 0U && argv[0][length - 1U] != '/')
		length--;
	if (length > 1U && length < sizeof(test_dir)) {
		memcpy(test_dir, argv[0], length - 1U);
		test_dir[length - 1U] = '\0';
	}
#else
	UNUSED_PARAMETER(argv);
	UNUSED_PARAMETER(length);
#endif
}

int
main(
	int argc,
	char **argv)
{
#ifdef _WIN32
	WSADATA wsa;
#endif

	/* The files go beside the program; a closed peer must not kill the test with SIGPIPE. */
	setvbuf(stdout, NULL, _IOLBF, 0);
	find_test_directory(argc, argv);
#ifdef _WIN32
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		printf("WSAStartup failed\n");
		return EXIT_FAILURE;
	}
#else
	signal(SIGPIPE, SIG_IGN);
#endif

	/* The certificates of the scripted peers: P-256 and P-384 chains for the test host. */
	CHECK(tlspeer_pki_init(&pki256, CRYC_CURVE_P256, HOST) == 0);
	CHECK(tlspeer_pki_init(&pki384, CRYC_CURVE_P384, HOST) == 0);

	test_tables();
	test_ctx();
	test_ctx_files();
	test_ssl();
	test_handshake();
	test_verify();
	test_callback();
	test_versions();
	test_write();
	test_write_wait();
	test_read();
	test_implicit_handshake();
	test_close();
	test_threads();
	return check_finish("test-api-ssl");
}
