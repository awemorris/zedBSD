/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Network Display discovery and encrypted offer receiver for mikuterm
 * (Win32, Android, and zedBSD).
 */

#include "netdisp-tab.h"
#include "netdisp-proto.h"

#include <compat/crypto/crypto.h>
#include <compat/crypto/rand.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET nd_sock_t;
#define ND_INVALID_SOCK INVALID_SOCKET
#define nd_close_sock(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int nd_sock_t;
#define ND_INVALID_SOCK (-1)
#define nd_close_sock(s) close(s)
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int
nd_set_nonblock(
	nd_sock_t sock)
{
#ifdef _WIN32
	u_long mode = 1UL;
	return ioctlsocket(sock, FIONBIO, &mode) == 0 ? 0 : -1;
#else
	int flags = fcntl(sock, F_GETFL, 0);
	if (flags < 0)
		return -1;
	return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

static int
nd_would_block(
	void)
{
#ifdef _WIN32
	int e = WSAGetLastError();
	return e == WSAEWOULDBLOCK || e == WSAECONNRESET ||
	       e == WSAENETRESET || e == WSAEMSGSIZE;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

static int nd_check_cancel(struct mikuterm_session *session, int *out_closing);

static int
nd_wait_cancel_after_error(
	struct mikuterm_session *session,
	const char *err_msg)
{
	struct kl_socket_wait wait;
	size_t ready = 0U;
	int closing = 0;

	session_say_line(session, "\033[0m\033[?2004l\033[2J\033[H\033[?25hNetwork Display -- Error");
	session_say_line(session, err_msg);
	session_say_line(session, "Press Esc or Ctrl+C to return.");
	for (;;) {
		if (nd_check_cancel(session, &closing))
			return ECANCELED;
		if (closing)
			return EPIPE;
		wait.socket = NULL;
		wait.waker = session->waker;
		wait.events = KL_SOCKET_READABLE;
		wait.ready = 0U;
		(void)kl_socket_poll(&wait, 1U, 200, &ready);
	}
}

static int
nd_check_cancel(
	struct mikuterm_session *session,
	int *out_closing)
{
	int cancelled = 0;
	int closing = 0;
	size_t i;

	kl_mutex_lock(&session->lock);
	closing = session->closing || session->ended;
	for (i = 0U; i < session->input_length; i++) {
		uint8_t c = session->input[i];
		if (c == 0x1BU || c == 0x03U || c == 0x04U) {
			cancelled = 1;
			break;
		}
	}
	session->input_length = 0U;
	kl_mutex_unlock(&session->lock);

	*out_closing = closing;
	return cancelled;
}

static void
nd_format_bcast_list(
	nd_sock_t sock,
	char *out,
	size_t cap)
{
	size_t off = 0U;

	(void)sock;
	off = (size_t)snprintf(out, cap, "255.255.255.255");
#ifdef _WIN32
	{
		INTERFACE_INFO ifaces[32];
		DWORD bytes_ret = 0;
		if (WSAIoctl(sock, SIO_GET_INTERFACE_LIST, NULL, 0,
			     ifaces, sizeof(ifaces), &bytes_ret, NULL, NULL) == 0) {
			DWORD count = bytes_ret / (DWORD)sizeof(INTERFACE_INFO);
			DWORD i;
			for (i = 0; i < count; i++) {
				u_long flags = ifaces[i].iiFlags;
				uint32_t ip;
				uint32_t mask;
				uint32_t dir_bcast;
				struct in_addr ba;
				char buf[32];
				if ((flags & IFF_UP) == 0 || (flags & IFF_LOOPBACK) != 0)
					continue;
				if (ifaces[i].iiAddress.Address.sa_family != AF_INET ||
				    ifaces[i].iiNetmask.Address.sa_family != AF_INET)
					continue;
				ip = ifaces[i].iiAddress.AddressIn.sin_addr.s_addr;
				mask = ifaces[i].iiNetmask.AddressIn.sin_addr.s_addr;
				dir_bcast = ip | ~mask;
				if (dir_bcast == htonl(INADDR_BROADCAST) || dir_bcast == 0U)
					continue;
				ba.s_addr = dir_bcast;
				if (inet_ntop(AF_INET, &ba, buf, sizeof(buf)) != NULL &&
				    off + 2U < cap) {
					int n = snprintf(out + off, cap - off, ", %s", buf);
					if (n > 0)
						off += (size_t)n;
				}
			}
		}
	}
#else
	{
		struct ifaddrs *ifap = NULL;
		if (getifaddrs(&ifap) == 0 && ifap != NULL) {
			struct ifaddrs *ifa;
			for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
				const struct sockaddr_in *sa;
				const struct sockaddr_in *nm;
				uint32_t dir_bcast;
				struct in_addr ba;
				char buf[32];
				if (ifa->ifa_addr == NULL || ifa->ifa_netmask == NULL)
					continue;
				if ((ifa->ifa_flags & IFF_UP) == 0 ||
				    (ifa->ifa_flags & IFF_LOOPBACK) != 0)
					continue;
				if (ifa->ifa_addr->sa_family != AF_INET ||
				    ifa->ifa_netmask->sa_family != AF_INET)
					continue;
				sa = (const struct sockaddr_in *)ifa->ifa_addr;
				nm = (const struct sockaddr_in *)ifa->ifa_netmask;
				dir_bcast = sa->sin_addr.s_addr | ~nm->sin_addr.s_addr;
				if (dir_bcast == htonl(INADDR_BROADCAST) || dir_bcast == 0U)
					continue;
				ba.s_addr = dir_bcast;
				if (inet_ntop(AF_INET, &ba, buf, sizeof(buf)) != NULL &&
				    off + 2U < cap) {
					int n = snprintf(out + off, cap - off, ", %s", buf);
					if (n > 0)
						off += (size_t)n;
				}
			}
			freeifaddrs(ifap);
		}
	}
#endif
}

static void
nd_send_broadcasts(
	nd_sock_t sock,
	const uint8_t *pkt,
	size_t len)
{
	struct sockaddr_in bcast_addr;

	memset(&bcast_addr, 0, sizeof(bcast_addr));
	bcast_addr.sin_family = AF_INET;
	bcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	bcast_addr.sin_port = htons((uint16_t)NETDISP_DISCOVERY_PORT);
	(void)sendto(sock, (const char *)pkt, (int)len, 0,
		     (const struct sockaddr *)&bcast_addr, sizeof(bcast_addr));

#ifdef _WIN32
	{
		INTERFACE_INFO ifaces[32];
		DWORD bytes_ret = 0;
		if (WSAIoctl(sock, SIO_GET_INTERFACE_LIST, NULL, 0,
			     ifaces, sizeof(ifaces), &bytes_ret, NULL, NULL) == 0) {
			DWORD count = bytes_ret / (DWORD)sizeof(INTERFACE_INFO);
			DWORD i;
			for (i = 0; i < count; i++) {
				u_long flags = ifaces[i].iiFlags;
				uint32_t ip;
				uint32_t mask;
				uint32_t dir_bcast;
				if ((flags & IFF_UP) == 0 || (flags & IFF_LOOPBACK) != 0)
					continue;
				if (ifaces[i].iiAddress.Address.sa_family != AF_INET ||
				    ifaces[i].iiNetmask.Address.sa_family != AF_INET)
					continue;
				ip = ifaces[i].iiAddress.AddressIn.sin_addr.s_addr;
				mask = ifaces[i].iiNetmask.AddressIn.sin_addr.s_addr;
				dir_bcast = ip | ~mask;
				if (dir_bcast == htonl(INADDR_BROADCAST) || dir_bcast == 0U)
					continue;
				bcast_addr.sin_addr.s_addr = dir_bcast;
				(void)sendto(sock, (const char *)pkt, (int)len, 0,
					     (const struct sockaddr *)&bcast_addr,
					     sizeof(bcast_addr));
			}
		}
	}
#else
	{
		struct ifaddrs *ifap = NULL;
		if (getifaddrs(&ifap) == 0 && ifap != NULL) {
			struct ifaddrs *ifa;
			for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
				const struct sockaddr_in *sa;
				const struct sockaddr_in *nm;
				uint32_t dir_bcast;
				if (ifa->ifa_addr == NULL || ifa->ifa_netmask == NULL)
					continue;
				if ((ifa->ifa_flags & IFF_UP) == 0 ||
				    (ifa->ifa_flags & IFF_LOOPBACK) != 0)
					continue;
				if (ifa->ifa_addr->sa_family != AF_INET ||
				    ifa->ifa_netmask->sa_family != AF_INET)
					continue;
				sa = (const struct sockaddr_in *)ifa->ifa_addr;
				nm = (const struct sockaddr_in *)ifa->ifa_netmask;
				dir_bcast = sa->sin_addr.s_addr | ~nm->sin_addr.s_addr;
				if (dir_bcast == htonl(INADDR_BROADCAST) || dir_bcast == 0U)
					continue;
				bcast_addr.sin_addr.s_addr = dir_bcast;
				(void)sendto(sock, (const char *)pkt, (int)len, 0,
					     (const struct sockaddr *)&bcast_addr,
					     sizeof(bcast_addr));
			}
			freeifaddrs(ifap);
		}
	}
#endif
}

int
netdisp_wait_and_receive(
	struct mikuterm_session *session,
	struct rdp_host_entry *out_target)
{
	uint8_t sk[NETDISP_KEY_LEN];
	uint8_t pk[NETDISP_KEY_LEN];
	uint8_t nonce[NETDISP_NONCE_LEN];
	uint8_t device_id[NETDISP_DEVICE_ID_LEN];
	char fp[10];
	char host_buf[128];
	struct netdisp_adv adv;
	uint8_t adv_pkt[NETDISP_ADV_MAX_LEN];
	size_t adv_len = 0U;
	nd_sock_t sock = ND_INVALID_SOCK;
	struct sockaddr_in bind_addr;
	socklen_t bind_len;
	uint16_t reply_port = 0U;
	int yes = 1;
	char msg[256];
	unsigned tick_ms = 2000U;
	int rc = EIO;

	if (session == NULL || out_target == NULL)
		return EINVAL;

#ifdef _WIN32
	{
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
			return nd_wait_cancel_after_error(session, "WSAStartup failed.");
	}
#endif

	memset(sk, 0, sizeof(sk));
	memset(pk, 0, sizeof(pk));
	memset(nonce, 0, sizeof(nonce));
	memset(device_id, 0, sizeof(device_id));

	if (netdisp_generate_keypair(sk, pk, nonce) != 0 ||
	    RAND_bytes(device_id, sizeof(device_id)) != 1) {
		OPENSSL_cleanse(sk, sizeof(sk));
		return nd_wait_cancel_after_error(session, "Failed to generate X25519 keypair.");
	}
	netdisp_fingerprint_hex(pk, nonce, fp);

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == ND_INVALID_SOCK) {
		OPENSSL_cleanse(sk, sizeof(sk));
		return nd_wait_cancel_after_error(session, "Failed to create UDP socket.");
	}
#ifdef _WIN32
	{
		BOOL connreset = FALSE;
		DWORD bytes_ret = 0;
		(void)WSAIoctl(sock, _WSAIOW(IOC_VENDOR, 12),
			       &connreset, sizeof(connreset),
			       NULL, 0, &bytes_ret, NULL, NULL);
	}
#endif
	(void)setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
			 (const char *)&yes, sizeof(yes));
	(void)nd_set_nonblock(sock);

	memset(&bind_addr, 0, sizeof(bind_addr));
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_addr.sin_port = 0;
	if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
		nd_close_sock(sock);
		OPENSSL_cleanse(sk, sizeof(sk));
		return nd_wait_cancel_after_error(session, "Failed to bind UDP socket.");
	}
	bind_len = sizeof(bind_addr);
	if (getsockname(sock, (struct sockaddr *)&bind_addr, &bind_len) != 0) {
		nd_close_sock(sock);
		OPENSSL_cleanse(sk, sizeof(sk));
		return nd_wait_cancel_after_error(session, "Failed to query UDP reply port.");
	}
	reply_port = ntohs(bind_addr.sin_port);

	memset(host_buf, 0, sizeof(host_buf));
	if (gethostname(host_buf, sizeof(host_buf) - 1U) != 0 || host_buf[0] == '\0')
		snprintf(host_buf, sizeof(host_buf), "mikuterm");

	memset(&adv, 0, sizeof(adv));
	memcpy(adv.device_id, device_id, NETDISP_DEVICE_ID_LEN);
	memcpy(adv.nonce, nonce, NETDISP_NONCE_LEN);
	memcpy(adv.pk, pk, NETDISP_KEY_LEN);
	adv.reply_port = reply_port;
	{
		unsigned sw;
		unsigned sh;

		kl_mutex_lock(&session->lock);
		sw = session->width;
		sh = session->height;
		kl_mutex_unlock(&session->lock);

		if (sw > 0U) {
			if (sw < 200U)
				sw = 200U;
			else if (sw > 8192U)
				sw = 8192U;
			adv.pref_width = (uint16_t)((sw + 1U) & ~1U);
		} else {
			adv.pref_width = 1280U;
		}
		if (sh > 0U) {
			if (sh < 200U)
				sh = 200U;
			else if (sh > 8192U)
				sh = 8192U;
			adv.pref_height = (uint16_t)((sh + 1U) & ~1U);
		} else {
			adv.pref_height = 720U;
		}
	}
	adv.pref_scale120 = 120U;
	snprintf(adv.device_name, sizeof(adv.device_name), "%s", host_buf);
	if (netdisp_adv_encode(&adv, adv_pkt, sizeof(adv_pkt), &adv_len) != 0) {
		nd_close_sock(sock);
		OPENSSL_cleanse(sk, sizeof(sk));
		return EINVAL;
	}

	session_say_line(session, "\033[0m\033[?2004l\033[2J\033[H\033[?25hNetwork Display -- Waiting for connection...");
	snprintf(msg, sizeof(msg), "Device: %s", adv.device_name);
	session_say_line(session, msg);
	snprintf(msg, sizeof(msg), "Verification Code: %s", fp);
	session_say_line(session, msg);
	{
		char bcast_list[192];
		nd_format_bcast_list(sock, bcast_list, sizeof(bcast_list));
		snprintf(msg, sizeof(msg), "Broadcast: %s (UDP %u)",
			 bcast_list, (unsigned)NETDISP_DISCOVERY_PORT);
		session_say_line(session, msg);
	}
	session_say_line(session, "Select this device in zedBSD Settings -> Display.");
	session_say_line(session, "Press Esc or Ctrl+C to return.");

	for (;;) {
		int closing = 0;
		struct kl_socket_wait wait;
		size_t ready = 0U;
		uint8_t rx[512];
		struct sockaddr_in from;
		socklen_t from_len;
		ssize_t n;

		if (nd_check_cancel(session, &closing)) {
			rc = ECANCELED;
			break;
		}
		if (closing) {
			rc = EPIPE;
			break;
		}

		if (tick_ms >= 2000U) {
			nd_send_broadcasts(sock, adv_pkt, adv_len);
			tick_ms = 0U;
		}

		from_len = sizeof(from);
		n = recvfrom(sock, (char *)rx, sizeof(rx), 0,
			     (struct sockaddr *)&from, &from_len);
		if (n > 0 && from.sin_family == AF_INET) {
			struct netdisp_offer_plain plain;
			if (netdisp_offer_decrypt(rx, (size_t)n, device_id,
						  sk, pk, nonce, &plain) == 0) {
				char sender_ip[64];
				memset(sender_ip, 0, sizeof(sender_ip));
				if (inet_ntop(AF_INET, &from.sin_addr,
					      sender_ip, sizeof(sender_ip)) != NULL) {
					memset(out_target, 0, sizeof(*out_target));
					snprintf(out_target->name, sizeof(out_target->name),
						 "%s", sender_ip);
					snprintf(out_target->host_name, sizeof(out_target->host_name),
						 "%s", sender_ip);
					out_target->port = plain.rdp_port;
					snprintf(out_target->user, sizeof(out_target->user),
						 "%s", plain.username);
					snprintf(out_target->password, sizeof(out_target->password),
						 "%s", plain.password);
					out_target->width = adv.pref_width;
					out_target->height = adv.pref_height;
					OPENSSL_cleanse(&plain, sizeof(plain));
					rc = 0;
					break;
				}
				OPENSSL_cleanse(&plain, sizeof(plain));
			}
		} else if (n < 0 && !nd_would_block()) {
			rc = EIO;
			break;
		}

		wait.socket = NULL;
		wait.waker = session->waker;
		wait.events = KL_SOCKET_READABLE;
		wait.ready = 0U;
		(void)kl_socket_poll(&wait, 1U, 200, &ready);
		tick_ms += 200U;
	}

	nd_close_sock(sock);
	OPENSSL_cleanse(sk, sizeof(sk));
	return rc;
}
