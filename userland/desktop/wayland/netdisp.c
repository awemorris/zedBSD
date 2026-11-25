/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Network Display discovery, encrypted offer, virtual extended kwl_head,
 * dedicated H.264 encoder thread, and direct seat input injection.
 */

#include "netdisp.h"
#include "compose.h"
#include "kwl.h"
#include "rdp-headless.h"
#include "apps/mikuterm-common/netdisp-proto.h"
#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include "userland/desktop/libmedia/media-encoder.h"

#include <arpa/inet.h>
#include <compat/crypto/crypto.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define NETDISP_EXPIRE_MS	7000ULL
#define NETDISP_CONNECT_TTL_MS	35000ULL

struct netdisp_entry {
	unsigned active;
	uint8_t device_id[NETDISP_DEVICE_ID_LEN];
	char device_id_hex[33];
	uint8_t nonce[NETDISP_NONCE_LEN];
	uint8_t pk[NETDISP_KEY_LEN];
	char name[NETDISP_MAX_NAME_LEN + 1U];
	char ip[64];
	char fingerprint[16];
	struct in_addr addr;
	uint16_t reply_port;
	uint32_t width;
	uint32_t height;
	uint32_t scale120;
	uint64_t last_seen_ms;
	uint64_t connect_ms;
	unsigned state;
};

struct kwl_rdp_ext {
	int fd;
	VkDevice device;
	VkPhysicalDevice physical;
	VkQueue queue;
	uint32_t queue_family;
	VkCommandPool cmd_pool;
	VkCommandBuffer cmd_buf;
	VkFence copy_fence;
	VkDeviceMemory image_mem;
	VkBuffer staging;
	VkDeviceMemory staging_mem;
	void *mapped;
	size_t staging_size;
	uint32_t width;
	uint32_t height;
	int is_rgba;
	uint8_t *cpu_bgra;
	uint8_t *y_plane;
	uint8_t *u_plane;
	uint8_t *v_plane;
	struct media_encoder *encoder;
	pthread_mutex_t mu;
	pthread_cond_t cond;
	pthread_t thread;
	int thread_running;
	int stop;
	int frame_ready;
	int force_key;
	uint64_t last_hash;
	uint8_t ctrl_buf[KWL_RDP_CTRL_HDR_SIZE];
	size_t ctrl_used;
	uint16_t buttons;
};

static struct {
	struct kwl_server *server;
	pthread_mutex_t lock;
	pthread_t thread;
	int thread_running;
	int udp_fd;
	int stop_pipe[2];
	int dirty;
	struct netdisp_entry entries[KWL_NETDISP_MAX];
	uint8_t pending_id[NETDISP_DEVICE_ID_LEN];
	int has_pending;
	uint8_t connected_id[NETDISP_DEVICE_ID_LEN];
	int has_connected;
	unsigned head_slot;
	int head_open;
} netdisp_state = {
	NULL,
	PTHREAD_MUTEX_INITIALIZER,
	0,
	0,
	-1,
	{ -1, -1 },
	0,
	{ { 0 } },
	{ 0 },
	0,
	{ 0 },
	0,
	0U,
	0
};

static void netdisp_detach_internal(struct kwl_server *server);

static uint64_t
netdisp_now_ms(
	void)
{
	return kwl_milliseconds();
}

static uint32_t
find_mem_type(
	VkPhysicalDevice physical,
	uint32_t filter,
	VkMemoryPropertyFlags props)
{
	VkPhysicalDeviceMemoryProperties mem_props;
	uint32_t i;

	vkGetPhysicalDeviceMemoryProperties(physical, &mem_props);
	for (i = 0U; i < mem_props.memoryTypeCount; i++) {
		if ((filter & (1U << i)) != 0U &&
		    (mem_props.memoryTypes[i].propertyFlags & props) == props)
			return i;
	}
	return UINT32_MAX;
}

static void
bgra_to_i420(
	const uint8_t *bgra,
	uint32_t width,
	uint32_t height,
	int is_rgba,
	uint8_t *y_out,
	uint8_t *u_out,
	uint8_t *v_out)
{
	uint32_t y;
	uint32_t x;
	size_t stride = (size_t)width * 4U;
	size_t r_off = is_rgba ? 0U : 2U;
	size_t b_off = is_rgba ? 2U : 0U;

	for (y = 0U; y < height; y++) {
		const uint8_t *row = bgra + (size_t)y * stride;
		uint8_t *yp = y_out + (size_t)y * (size_t)width;
		for (x = 0U; x < width; x++) {
			int b = row[x * 4U + b_off];
			int g = row[x * 4U + 1U];
			int r = row[x * 4U + r_off];
			int yv = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
			if (yv < 0)
				yv = 0;
			if (yv > 255)
				yv = 255;
			yp[x] = (uint8_t)yv;
		}
	}
	for (y = 0U; y < height; y += 2U) {
		const uint8_t *r0 = bgra + (size_t)y * stride;
		const uint8_t *r1 = bgra + (size_t)(y + 1U) * stride;
		uint8_t *up = u_out + ((size_t)y / 2U) * ((size_t)width / 2U);
		uint8_t *vp = v_out + ((size_t)y / 2U) * ((size_t)width / 2U);
		for (x = 0U; x < width; x += 2U) {
			size_t p0 = (size_t)x * 4U;
			size_t p1 = p0 + 4U;
			int b = (r0[p0 + b_off] + r0[p1 + b_off] + r1[p0 + b_off] + r1[p1 + b_off]) >> 2;
			int g = (r0[p0 + 1U] + r0[p1 + 1U] + r1[p0 + 1U] + r1[p1 + 1U]) >> 2;
			int r = (r0[p0 + r_off] + r0[p1 + r_off] + r1[p0 + r_off] + r1[p1 + r_off]) >> 2;
			int uv = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
			int vv = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
			if (uv < 0)
				uv = 0;
			if (uv > 255)
				uv = 255;
			if (vv < 0)
				vv = 0;
			if (vv > 255)
				vv = 255;
			up[x / 2U] = (uint8_t)uv;
			vp[x / 2U] = (uint8_t)vv;
		}
	}
}

static uint64_t
frame_hash(
	const uint8_t *y,
	size_t y_len)
{
	uint64_t h = 14695981039346656037ULL;
	size_t i;

	for (i = 0U; i < y_len; i += 16U) {
		h ^= (uint64_t)y[i];
		h *= 1099511628211ULL;
	}
	return h;
}

static int
write_all_fd(
	struct kwl_rdp_ext *ext,
	const uint8_t *buf,
	size_t len)
{
	size_t off = 0U;

	while (off < len) {
		ssize_t n = write(ext->fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				struct pollfd pfd;
				int prc;
				if (__atomic_load_n(&ext->stop, __ATOMIC_ACQUIRE))
					return -1;
				pfd.fd = ext->fd;
				pfd.events = POLLOUT;
				pfd.revents = 0;
				prc = poll(&pfd, 1, 200);
				if (prc < 0 && errno != EINTR)
					return -1;
				if (prc > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
					return -1;
				continue;
			}
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

static void *
netdisp_encode_thread(
	void *arg)
{
	struct kwl_rdp_ext *ext = (struct kwl_rdp_ext *)arg;
	size_t bgra_sz = (size_t)ext->width * (size_t)ext->height * 4U;
	uint8_t *local_bgra = (uint8_t *)malloc(bgra_sz);
	size_t y_len = (size_t)ext->width * (size_t)ext->height;
	uint8_t *au_buf = NULL;
	size_t au_cap = 0U;

	if (local_bgra == NULL)
		return NULL;

	for (;;) {
		int key;
		uint64_t hash;
		struct media_encoder_picture pic;
		struct media_encoded pkt;
		uint8_t hdr[KWL_RDP_FRAME_HDR_SIZE];
		uint32_t send_flags;
		uint32_t out_flags;
		int rc;

		pthread_mutex_lock(&ext->mu);
		while (!ext->frame_ready && !ext->stop)
			pthread_cond_wait(&ext->cond, &ext->mu);
		if (ext->stop) {
			pthread_mutex_unlock(&ext->mu);
			break;
		}
		memcpy(local_bgra, ext->cpu_bgra, bgra_sz);
		ext->frame_ready = 0;
		key = ext->force_key;
		ext->force_key = 0;
		pthread_mutex_unlock(&ext->mu);

		bgra_to_i420(local_bgra, ext->width, ext->height, ext->is_rgba,
			     ext->y_plane, ext->u_plane, ext->v_plane);
		hash = frame_hash(ext->y_plane, y_len);
		if (!key && hash == ext->last_hash)
			continue;
		ext->last_hash = hash;

		memset(&pic, 0, sizeof(pic));
		pic.plane[0] = ext->y_plane;
		pic.plane[1] = ext->u_plane;
		pic.plane[2] = ext->v_plane;
		pic.stride[0] = ext->width;
		pic.stride[1] = ext->width / 2U;
		pic.stride[2] = ext->width / 2U;
		send_flags = key ? MEDIA_ENCODE_FORCE_KEY : 0U;
		if (media_encoder_send(ext->encoder, &pic,
				       (int64_t)kwl_log_us(), send_flags) != 0)
			continue;
		memset(&pkt, 0, sizeof(pkt));
		rc = media_encoder_receive(ext->encoder, 0U, &pkt);
		if (rc != 0 || pkt.data == NULL || pkt.size == 0U)
			continue;
		if (pkt.size > au_cap) {
			uint8_t *nb = (uint8_t *)realloc(au_buf, pkt.size);
			if (nb != NULL) {
				au_buf = nb;
				au_cap = pkt.size;
			}
		}
		out_flags = ((pkt.flags & MEDIA_ENCODED_KEY) != 0U) ? KWL_RDP_FRAME_KEY : 0U;
		kwl_rdp_pack_frame_hdr(hdr, out_flags,
				       kwl_log_us(), (uint32_t)pkt.size);
		if (write_all_fd(ext, hdr, sizeof(hdr)) != 0 ||
		    write_all_fd(ext, (const uint8_t *)pkt.data, pkt.size) != 0) {
			break;
		}
	}
	free(au_buf);
	free(local_bgra);
	return NULL;
}

static void *
netdisp_discovery_thread(
	void *arg)
{
	struct kwl_server *server = (struct kwl_server *)arg;
	struct pollfd pfds[2];

	pfds[0].fd = netdisp_state.udp_fd;
	pfds[0].events = POLLIN;
	pfds[1].fd = netdisp_state.stop_pipe[0];
	pfds[1].events = POLLIN;

	for (;;) {
		int rc;
		uint8_t buf[512];
		struct sockaddr_in from;
		socklen_t from_len;
		ssize_t n;
		struct netdisp_adv adv;
		char ip[64];
		char fp[16];
		char id_hex[33];
		uint64_t now_ms;
		size_t i;
		struct netdisp_entry *slot;
		int wake = 0;

		pfds[0].revents = 0;
		pfds[1].revents = 0;
		rc = poll(pfds, 2, -1);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if ((pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
			break;
		if ((pfds[0].revents & POLLIN) == 0)
			continue;

		from_len = sizeof(from);
		n = recvfrom(netdisp_state.udp_fd, buf, sizeof(buf), 0,
			     (struct sockaddr *)&from, &from_len);
		if (n <= 0 || from.sin_family != AF_INET)
			continue;
		if (netdisp_adv_decode(buf, (size_t)n, &adv) != 0)
			continue;
		if (inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip)) == NULL)
			continue;
		netdisp_fingerprint_hex(adv.pk, adv.nonce, fp);
		netdisp_device_id_hex(adv.device_id, id_hex);
		now_ms = netdisp_now_ms();

		pthread_mutex_lock(&netdisp_state.lock);
		slot = NULL;
		for (i = 0U; i < KWL_NETDISP_MAX; i++) {
			if (netdisp_state.entries[i].active &&
			    memcmp(netdisp_state.entries[i].device_id,
				   adv.device_id, NETDISP_DEVICE_ID_LEN) == 0) {
				slot = &netdisp_state.entries[i];
				break;
			}
		}
		if (slot == NULL) {
			for (i = 0U; i < KWL_NETDISP_MAX; i++) {
				if (!netdisp_state.entries[i].active) {
					slot = &netdisp_state.entries[i];
					memset(slot, 0, sizeof(*slot));
					slot->active = 1U;
					slot->state = KWL_NETDISP_IDLE;
					memcpy(slot->device_id, adv.device_id,
					       NETDISP_DEVICE_ID_LEN);
					snprintf(slot->device_id_hex,
						 sizeof(slot->device_id_hex),
						 "%s", id_hex);
					wake = 1;
					printf("KWL NETDISP discovered name=%s ip=%s fp=%s\n",
					       adv.device_name, ip, fp);
					break;
				}
			}
		}
		if (slot != NULL) {
			if (strcmp(slot->name, adv.device_name) != 0 ||
			    strcmp(slot->ip, ip) != 0 ||
			    strcmp(slot->fingerprint, fp) != 0 ||
			    slot->width != adv.pref_width ||
			    slot->height != adv.pref_height)
				wake = 1;
			memcpy(slot->nonce, adv.nonce, NETDISP_NONCE_LEN);
			memcpy(slot->pk, adv.pk, NETDISP_KEY_LEN);
			snprintf(slot->name, sizeof(slot->name), "%s",
				 adv.device_name);
			snprintf(slot->ip, sizeof(slot->ip), "%s", ip);
			snprintf(slot->fingerprint, sizeof(slot->fingerprint),
				 "%s", fp);
			slot->addr = from.sin_addr;
			slot->reply_port = adv.reply_port;
			slot->width = adv.pref_width;
			slot->height = adv.pref_height;
			slot->scale120 = adv.pref_scale120 != 0U ?
					 adv.pref_scale120 : 120U;
			slot->last_seen_ms = now_ms;
		}
		if (wake)
			netdisp_state.dirty = 1;
		pthread_mutex_unlock(&netdisp_state.lock);

		if (wake)
			kwl_thread_wake(server->wake[1]);
	}
	return NULL;
}

void
kwl_netdisp_start(
	struct kwl_server *server)
{
	struct sockaddr_in addr;
	int yes = 1;
	int fd;

	if (server == NULL || server->headless)
		return;
	netdisp_state.server = server;
	if (pipe(netdisp_state.stop_pipe) != 0)
		return;
	(void)fcntl(netdisp_state.stop_pipe[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(netdisp_state.stop_pipe[1], F_SETFD, FD_CLOEXEC);

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		close(netdisp_state.stop_pipe[0]);
		close(netdisp_state.stop_pipe[1]);
		netdisp_state.stop_pipe[0] = -1;
		netdisp_state.stop_pipe[1] = -1;
		return;
	}
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	(void)setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)NETDISP_DISCOVERY_PORT);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		printf("KWL NETDISP bind failed port=%u errno=%d\n",
		       (unsigned)NETDISP_DISCOVERY_PORT, errno);
		close(fd);
		close(netdisp_state.stop_pipe[0]);
		close(netdisp_state.stop_pipe[1]);
		netdisp_state.stop_pipe[0] = -1;
		netdisp_state.stop_pipe[1] = -1;
		return;
	}
	netdisp_state.udp_fd = fd;
	if (pthread_create(&netdisp_state.thread, NULL,
			   netdisp_discovery_thread, server) == 0) {
		netdisp_state.thread_running = 1;
		printf("KWL NETDISP listening udp=%u\n",
		       (unsigned)NETDISP_DISCOVERY_PORT);
	}
}

void
kwl_netdisp_stop(
	struct kwl_server *server)
{
	netdisp_detach_internal(server);
	if (netdisp_state.stop_pipe[1] >= 0) {
		char b = 1;
		(void)write(netdisp_state.stop_pipe[1], &b, 1U);
	}
	if (netdisp_state.thread_running) {
		(void)pthread_join(netdisp_state.thread, NULL);
		netdisp_state.thread_running = 0;
	}
	if (netdisp_state.udp_fd >= 0) {
		close(netdisp_state.udp_fd);
		netdisp_state.udp_fd = -1;
	}
	if (netdisp_state.stop_pipe[0] >= 0) {
		close(netdisp_state.stop_pipe[0]);
		netdisp_state.stop_pipe[0] = -1;
	}
	if (netdisp_state.stop_pipe[1] >= 0) {
		close(netdisp_state.stop_pipe[1]);
		netdisp_state.stop_pipe[1] = -1;
	}
}

void
kwl_netdisp_tick(
	struct kwl_server *server,
	uint64_t now_ms)
{
	int changed = 0;
	size_t i;

	if (server == NULL || server->headless)
		return;
	pthread_mutex_lock(&netdisp_state.lock);
	if (netdisp_state.dirty) {
		netdisp_state.dirty = 0;
		changed = 1;
	}
	for (i = 0U; i < KWL_NETDISP_MAX; i++) {
		struct netdisp_entry *e = &netdisp_state.entries[i];
		if (!e->active)
			continue;
		if (e->state == KWL_NETDISP_CONNECTING &&
		    now_ms - e->connect_ms > NETDISP_CONNECT_TTL_MS) {
			printf("KWL NETDISP connect timeout id=%s\n",
			       e->device_id_hex);
			e->state = KWL_NETDISP_IDLE;
			if (netdisp_state.has_pending &&
			    memcmp(netdisp_state.pending_id, e->device_id,
				   NETDISP_DEVICE_ID_LEN) == 0)
				netdisp_state.has_pending = 0;
			changed = 1;
		}
		if (e->state == KWL_NETDISP_IDLE &&
		    now_ms - e->last_seen_ms > NETDISP_EXPIRE_MS) {
			memset(e, 0, sizeof(*e));
			changed = 1;
		}
	}
	pthread_mutex_unlock(&netdisp_state.lock);

	if (changed)
		kwl_displays_tell(server);
}

int
kwl_netdisp_timeout(
	const struct kwl_server *server,
	uint64_t now_ms)
{
	uint64_t earliest = UINT64_MAX;
	size_t i;

	if (server == NULL || server->headless)
		return -1;
	pthread_mutex_lock(&netdisp_state.lock);
	for (i = 0U; i < KWL_NETDISP_MAX; i++) {
		const struct netdisp_entry *e = &netdisp_state.entries[i];
		uint64_t due;
		if (!e->active)
			continue;
		if (e->state == KWL_NETDISP_IDLE)
			due = e->last_seen_ms + NETDISP_EXPIRE_MS;
		else if (e->state == KWL_NETDISP_CONNECTING)
			due = e->connect_ms + NETDISP_CONNECT_TTL_MS;
		else
			continue;
		if (due < earliest)
			earliest = due;
	}
	pthread_mutex_unlock(&netdisp_state.lock);
	if (earliest == UINT64_MAX)
		return -1;
	if (earliest <= now_ms)
		return 0;
	return (int)(earliest - now_ms);
}

size_t
kwl_netdisp_snapshot(
	const struct kwl_server *server,
	struct kwl_netdisp_info *out,
	size_t cap)
{
	size_t count = 0U;
	size_t i;

	(void)server;
	pthread_mutex_lock(&netdisp_state.lock);
	for (i = 0U; i < KWL_NETDISP_MAX; i++) {
		const struct netdisp_entry *e = &netdisp_state.entries[i];
		if (!e->active)
			continue;
		if (out != NULL && count < cap) {
			snprintf(out[count].device_id, sizeof(out[count].device_id),
				 "%s", e->device_id_hex);
			snprintf(out[count].name, sizeof(out[count].name),
				 "%s", e->name);
			snprintf(out[count].ip, sizeof(out[count].ip),
				 "%s", e->ip);
			snprintf(out[count].fingerprint, sizeof(out[count].fingerprint),
				 "%s", e->fingerprint);
			out[count].width = e->width;
			out[count].height = e->height;
			out[count].scale120 = e->scale120;
			out[count].state = e->state;
		}
		count++;
	}
	pthread_mutex_unlock(&netdisp_state.lock);
	return count;
}

int
kwl_netdisp_connect(
	struct kwl_server *server,
	const char *device_id_hex)
{
	uint8_t id[NETDISP_DEVICE_ID_LEN];
	size_t i;
	int found = 0;

	if (server == NULL || server->headless || device_id_hex == NULL)
		return EINVAL;
	if (netdisp_device_id_from_hex(device_id_hex, id) != 0)
		return EINVAL;

	pthread_mutex_lock(&netdisp_state.lock);
	if (netdisp_state.head_open || netdisp_state.has_pending) {
		pthread_mutex_unlock(&netdisp_state.lock);
		return EBUSY;
	}
	for (i = 0U; i < KWL_NETDISP_MAX; i++) {
		if (netdisp_state.entries[i].active &&
		    memcmp(netdisp_state.entries[i].device_id, id,
			   NETDISP_DEVICE_ID_LEN) == 0) {
			netdisp_state.entries[i].state = KWL_NETDISP_CONNECTING;
			netdisp_state.entries[i].connect_ms = netdisp_now_ms();
			memcpy(netdisp_state.pending_id, id, NETDISP_DEVICE_ID_LEN);
			netdisp_state.has_pending = 1;
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&netdisp_state.lock);
	if (!found)
		return ENODEV;

	if (kl_backend_session_extend_prepare(server->backend) != 0) {
		printf("KWL NETDISP connect id=%s extend_prepare failed\n",
		       device_id_hex);
		pthread_mutex_lock(&netdisp_state.lock);
		netdisp_state.has_pending = 0;
		for (i = 0U; i < KWL_NETDISP_MAX; i++) {
			if (netdisp_state.entries[i].active &&
			    memcmp(netdisp_state.entries[i].device_id, id,
				   NETDISP_DEVICE_ID_LEN) == 0)
				netdisp_state.entries[i].state = KWL_NETDISP_IDLE;
		}
		pthread_mutex_unlock(&netdisp_state.lock);
		return EIO;
	}
	printf("KWL NETDISP connect id=%s pending\n", device_id_hex);
	kwl_displays_tell(server);
	return 0;
}

int
kwl_netdisp_disconnect(
	struct kwl_server *server,
	const char *device_id_hex)
{
	(void)device_id_hex;
	if (server == NULL)
		return EINVAL;
	netdisp_detach_internal(server);
	return 0;
}

void
kwl_netdisp_extend_prepared(
	void *data,
	int error,
	const char *user,
	const char *password,
	unsigned port)
{
	struct kwl_server *server = (struct kwl_server *)data;
	struct netdisp_entry target;
	struct netdisp_offer_plain plain;
	uint8_t offer_buf[NETDISP_OFFER_MAX_LEN];
	size_t offer_len = 0U;
	size_t i;
	int found = 0;

	pthread_mutex_lock(&netdisp_state.lock);
	if (!netdisp_state.has_pending) {
		pthread_mutex_unlock(&netdisp_state.lock);
		return;
	}
	for (i = 0U; i < KWL_NETDISP_MAX; i++) {
		if (netdisp_state.entries[i].active &&
		    memcmp(netdisp_state.entries[i].device_id,
			   netdisp_state.pending_id,
			   NETDISP_DEVICE_ID_LEN) == 0) {
			target = netdisp_state.entries[i];
			found = 1;
			if (error != 0 || user == NULL || password == NULL)
				netdisp_state.entries[i].state = KWL_NETDISP_IDLE;
			break;
		}
	}
	if (error != 0 || !found)
		netdisp_state.has_pending = 0;
	pthread_mutex_unlock(&netdisp_state.lock);

	if (error != 0 || !found || user == NULL || password == NULL) {
		printf("KWL NETDISP extend_prepared failed error=%d found=%d\n",
		       error, found);
		if (server != NULL)
			kwl_displays_tell(server);
		return;
	}

	memset(&plain, 0, sizeof(plain));
	memcpy(plain.nonce, target.nonce, NETDISP_NONCE_LEN);
	plain.rdp_port = (uint16_t)port;
	snprintf(plain.username, sizeof(plain.username), "%s", user);
	snprintf(plain.password, sizeof(plain.password), "%s", password);

	if (netdisp_offer_encrypt(target.device_id, target.pk, &plain,
				  offer_buf, sizeof(offer_buf), &offer_len) == 0 &&
	    netdisp_state.udp_fd >= 0) {
		struct sockaddr_in dst;
		ssize_t sent;
		memset(&dst, 0, sizeof(dst));
		dst.sin_family = AF_INET;
		dst.sin_addr = target.addr;
		dst.sin_port = htons(target.reply_port);
		sent = sendto(netdisp_state.udp_fd, offer_buf, offer_len, 0,
			      (struct sockaddr *)&dst, sizeof(dst));
		printf("KWL NETDISP offer sent ip=%s port=%u len=%zu sent=%zd errno=%d\n",
		       target.ip, (unsigned)target.reply_port, offer_len,
		       sent, sent < 0 ? errno : 0);
	} else {
		printf("KWL NETDISP offer encrypt failed ip=%s udp_fd=%d\n",
		       target.ip, netdisp_state.udp_fd);
		pthread_mutex_lock(&netdisp_state.lock);
		netdisp_state.has_pending = 0;
		for (i = 0U; i < KWL_NETDISP_MAX; i++) {
			if (netdisp_state.entries[i].active &&
			    memcmp(netdisp_state.entries[i].device_id,
				   target.device_id,
				   NETDISP_DEVICE_ID_LEN) == 0) {
				netdisp_state.entries[i].state = KWL_NETDISP_IDLE;
				break;
			}
		}
		pthread_mutex_unlock(&netdisp_state.lock);
		if (server != NULL)
			kwl_displays_tell(server);
	}
	OPENSSL_cleanse(&plain, sizeof(plain));
	OPENSSL_cleanse(offer_buf, sizeof(offer_buf));
}

static struct kwl_rdp_ext *
rdp_ext_create(
	struct kwl_compose *compose,
	int rdp_fd,
	uint32_t width,
	uint32_t height,
	VkImage *out_image,
	VkDeviceMemory *out_mem)
{
	struct kwl_rdp_ext *ext;
	VkImageCreateInfo img_info;
	VkBufferCreateInfo buf_info;
	VkMemoryRequirements req;
	VkMemoryAllocateInfo alloc;
	VkCommandPoolCreateInfo pool_info;
	VkCommandBufferAllocateInfo cmd_info;
	VkFenceCreateInfo fence_info;
	struct media_encoder_config cfg;
	uint32_t mem_type;
	VkImage img = VK_NULL_HANDLE;
	VkDeviceMemory img_mem = VK_NULL_HANDLE;
	VkFormat img_fmt;
	VkResult res;

	img_fmt = compose->output.format != VK_FORMAT_UNDEFINED
		? compose->output.format : VK_FORMAT_B8G8R8A8_UNORM;
	ext = (struct kwl_rdp_ext *)calloc(1U, sizeof(*ext));
	if (ext == NULL)
		return NULL;
	ext->fd = rdp_fd;
	ext->device = compose->device;
	ext->physical = compose->physical;
	ext->queue = compose->queue;
	ext->queue_family = compose->family;
	ext->width = width;
	ext->height = height;
	ext->is_rgba = (img_fmt == VK_FORMAT_R8G8B8A8_UNORM ||
			img_fmt == VK_FORMAT_R8G8B8A8_SRGB);
	ext->staging_size = (size_t)width * (size_t)height * 4U;
	pthread_mutex_init(&ext->mu, NULL);
	pthread_cond_init(&ext->cond, NULL);

	memset(&img_info, 0, sizeof(img_info));
	img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	img_info.imageType = VK_IMAGE_TYPE_2D;
	img_info.format = img_fmt;
	img_info.extent.width = width;
	img_info.extent.height = height;
	img_info.extent.depth = 1U;
	img_info.mipLevels = 1U;
	img_info.arrayLayers = 1U;
	img_info.samples = VK_SAMPLE_COUNT_1_BIT;
	img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	img_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
			 VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	res = vkCreateImage(ext->device, &img_info, NULL, &img);
	if (res != VK_SUCCESS)
		goto fail;
	vkGetImageMemoryRequirements(ext->device, img, &req);
	mem_type = find_mem_type(ext->physical, req.memoryTypeBits,
				 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (mem_type == UINT32_MAX)
		goto fail;
	memset(&alloc, 0, sizeof(alloc));
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = mem_type;
	if (vkAllocateMemory(ext->device, &alloc, NULL, &img_mem) != VK_SUCCESS ||
	    vkBindImageMemory(ext->device, img, img_mem, 0U) != VK_SUCCESS)
		goto fail;

	memset(&buf_info, 0, sizeof(buf_info));
	buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buf_info.size = ext->staging_size;
	buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(ext->device, &buf_info, NULL, &ext->staging) != VK_SUCCESS)
		goto fail;
	vkGetBufferMemoryRequirements(ext->device, ext->staging, &req);
	mem_type = find_mem_type(ext->physical, req.memoryTypeBits,
				 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (mem_type == UINT32_MAX)
		goto fail;
	memset(&alloc, 0, sizeof(alloc));
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = mem_type;
	if (vkAllocateMemory(ext->device, &alloc, NULL, &ext->staging_mem) != VK_SUCCESS ||
	    vkBindBufferMemory(ext->device, ext->staging, ext->staging_mem, 0U) != VK_SUCCESS ||
	    vkMapMemory(ext->device, ext->staging_mem, 0U, ext->staging_size,
			0U, &ext->mapped) != VK_SUCCESS)
		goto fail;

	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = ext->queue_family;
	if (vkCreateCommandPool(ext->device, &pool_info, NULL, &ext->cmd_pool) != VK_SUCCESS)
		goto fail;
	memset(&cmd_info, 0, sizeof(cmd_info));
	cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cmd_info.commandPool = ext->cmd_pool;
	cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmd_info.commandBufferCount = 1U;
	if (vkAllocateCommandBuffers(ext->device, &cmd_info, &ext->cmd_buf) != VK_SUCCESS)
		goto fail;
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (vkCreateFence(ext->device, &fence_info, NULL, &ext->copy_fence) != VK_SUCCESS)
		goto fail;

	ext->cpu_bgra = (uint8_t *)malloc(ext->staging_size);
	ext->y_plane = (uint8_t *)malloc((size_t)width * (size_t)height);
	ext->u_plane = (uint8_t *)malloc(((size_t)width / 2U) * ((size_t)height / 2U));
	ext->v_plane = (uint8_t *)malloc(((size_t)width / 2U) * ((size_t)height / 2U));
	if (ext->cpu_bgra == NULL || ext->y_plane == NULL ||
	    ext->u_plane == NULL || ext->v_plane == NULL)
		goto fail;

	memset(&cfg, 0, sizeof(cfg));
	cfg.codec = MEDIA_CODEC_H264;
	cfg.profile = MEDIA_ENCODER_PROFILE_H264_CONSTRAINED_BASELINE;
	cfg.width = width;
	cfg.height = height;
	cfg.rate_num = 60U;
	cfg.rate_den = 1U;
	cfg.input = MEDIA_ENCODER_INPUT_I420;
	cfg.allow = MEDIA_ENCODER_SOFTWARE | MEDIA_ENCODER_HARDWARE;
	cfg.purpose = MEDIA_ENCODER_LOW_LATENCY;
	cfg.output = MEDIA_ENCODER_ANNEX_B;
	cfg.rate_control = MEDIA_ENCODER_RATE_CQP;
	cfg.qp = 26U;
	if (media_encoder_create(&cfg, &ext->encoder) != 0)
		goto fail;
	ext->force_key = 1;

	if (pthread_create(&ext->thread, NULL, netdisp_encode_thread, ext) != 0)
		goto fail;
	ext->thread_running = 1;
	*out_image = img;
	*out_mem = img_mem;
	return ext;

fail:
	if (ext->encoder != NULL)
		media_encoder_close(ext->encoder);
	free(ext->cpu_bgra);
	free(ext->y_plane);
	free(ext->u_plane);
	free(ext->v_plane);
	if (ext->copy_fence != VK_NULL_HANDLE)
		vkDestroyFence(ext->device, ext->copy_fence, NULL);
	if (ext->cmd_pool != VK_NULL_HANDLE)
		vkDestroyCommandPool(ext->device, ext->cmd_pool, NULL);
	if (ext->mapped != NULL && ext->staging_mem != VK_NULL_HANDLE)
		vkUnmapMemory(ext->device, ext->staging_mem);
	if (ext->staging != VK_NULL_HANDLE)
		vkDestroyBuffer(ext->device, ext->staging, NULL);
	if (ext->staging_mem != VK_NULL_HANDLE)
		vkFreeMemory(ext->device, ext->staging_mem, NULL);
	if (img != VK_NULL_HANDLE)
		vkDestroyImage(ext->device, img, NULL);
	if (img_mem != VK_NULL_HANDLE)
		vkFreeMemory(ext->device, img_mem, NULL);
	pthread_cond_destroy(&ext->cond);
	pthread_mutex_destroy(&ext->mu);
	free(ext);
	return NULL;
}

static void
rdp_ext_destroy(
	struct kwl_rdp_ext *ext,
	VkImage image)
{
	if (ext == NULL)
		return;
	if (ext->thread_running) {
		pthread_mutex_lock(&ext->mu);
		ext->stop = 1;
		pthread_cond_signal(&ext->cond);
		pthread_mutex_unlock(&ext->mu);
		(void)pthread_join(ext->thread, NULL);
		ext->thread_running = 0;
	}
	if (ext->encoder != NULL)
		media_encoder_close(ext->encoder);
	free(ext->cpu_bgra);
	free(ext->y_plane);
	free(ext->u_plane);
	free(ext->v_plane);
	if (ext->copy_fence != VK_NULL_HANDLE)
		vkDestroyFence(ext->device, ext->copy_fence, NULL);
	if (ext->cmd_pool != VK_NULL_HANDLE)
		vkDestroyCommandPool(ext->device, ext->cmd_pool, NULL);
	if (ext->mapped != NULL && ext->staging_mem != VK_NULL_HANDLE)
		vkUnmapMemory(ext->device, ext->staging_mem);
	if (ext->staging != VK_NULL_HANDLE)
		vkDestroyBuffer(ext->device, ext->staging, NULL);
	if (ext->staging_mem != VK_NULL_HANDLE)
		vkFreeMemory(ext->device, ext->staging_mem, NULL);
	if (image != VK_NULL_HANDLE)
		vkDestroyImage(ext->device, image, NULL);
	if (ext->fd >= 0)
		close(ext->fd);
	pthread_cond_destroy(&ext->cond);
	pthread_mutex_destroy(&ext->mu);
	free(ext);
}

void
kwl_netdisp_extend_attach(
	void *data,
	int rdp_fd,
	uint32_t width,
	uint32_t height)
{
	struct kwl_server *server = (struct kwl_server *)data;
	struct kwl_compose *compose;
	struct kwl_head *head;
	struct kwl_rdp_ext *ext;
	VkImage img = VK_NULL_HANDLE;
	VkDeviceMemory img_mem = VK_NULL_HANDLE;
	VkImageViewCreateInfo view_info;
	VkFramebufferCreateInfo fb_info;
	unsigned slot;
	int32_t right_x;
	size_t i;

	if (server == NULL || server->compose == NULL || rdp_fd < 0) {
		printf("KWL NETDISP extend_attach invalid server=%p fd=%d\n",
		       (void *)server, rdp_fd);
		if (rdp_fd >= 0)
			close(rdp_fd);
		return;
	}
	printf("KWL NETDISP extend_attach fd=%d %ux%u\n",
	       rdp_fd, (unsigned)width, (unsigned)height);
	compose = server->compose;
	kwl_rdp_clamp_size(&width, &height);
	(void)fcntl(rdp_fd, F_SETFD, FD_CLOEXEC);
	(void)fcntl(rdp_fd, F_SETFL, fcntl(rdp_fd, F_GETFL) | O_NONBLOCK);

	netdisp_detach_internal(server);
	kwl_compose_park(server);

	head = NULL;
	slot = 0U;
	for (i = 0U; i < KWL_HEADS; i++) {
		if (!compose->heads[i].open) {
			head = &compose->heads[i];
			slot = (unsigned)i;
			break;
		}
	}
	if (head == NULL) {
		printf("KWL NETDISP extend_attach no free head slot\n");
		kwl_compose_unpark(server);
		close(rdp_fd);
		return;
	}

	ext = rdp_ext_create(compose, rdp_fd, width, height, &img, &img_mem);
	if (ext == NULL) {
		printf("KWL NETDISP extend_attach rdp_ext_create failed\n");
		kwl_compose_unpark(server);
		close(rdp_fd);
		return;
	}

	memset(head, 0, sizeof(*head));
	head->virtual_rdp = 1U;
	head->rdp_ext = ext;
	head->output.format = compose->output.format != VK_FORMAT_UNDEFINED
		? compose->output.format : VK_FORMAT_B8G8R8A8_UNORM;
	head->output.width = width;
	head->output.height = height;
	head->output.image_count = 1U;
	head->output.images[0] = img;

	memset(&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = img;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = head->output.format;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1U;
	view_info.subresourceRange.layerCount = 1U;
	if (vkCreateImageView(compose->device, &view_info, NULL, &head->views[0]) != VK_SUCCESS) {
		printf("KWL NETDISP extend_attach vkCreateImageView failed fmt=%d\n",
		       (int)head->output.format);
		rdp_ext_destroy(ext, img);
		vkFreeMemory(compose->device, img_mem, NULL);
		memset(head, 0, sizeof(*head));
		kwl_compose_unpark(server);
		return;
	}
	memset(&fb_info, 0, sizeof(fb_info));
	fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fb_info.renderPass = compose->pass;
	fb_info.attachmentCount = 1U;
	fb_info.pAttachments = &head->views[0];
	fb_info.width = width;
	fb_info.height = height;
	fb_info.layers = 1U;
	if (vkCreateFramebuffer(compose->device, &fb_info, NULL, &head->framebuffers[0]) != VK_SUCCESS) {
		printf("KWL NETDISP extend_attach vkCreateFramebuffer failed fmt=%d\n",
		       (int)head->output.format);
		vkDestroyImageView(compose->device, head->views[0], NULL);
		rdp_ext_destroy(ext, img);
		vkFreeMemory(compose->device, img_mem, NULL);
		memset(head, 0, sizeof(*head));
		kwl_compose_unpark(server);
		return;
	}

	right_x = (int32_t)compose->output.width;
	for (i = 0U; i < KWL_HEADS; i++) {
		if (i != slot && compose->heads[i].open &&
		    compose->heads[i].x + (int32_t)compose->heads[i].width > right_x)
			right_x = compose->heads[i].x + (int32_t)compose->heads[i].width;
	}
	head->open = 1U;
	head->x = right_x;
	head->y = 0;
	head->width = width;
	head->height = height;
	head->pixel_width = width;
	head->pixel_height = height;
	head->scale120 = 120U;
	head->scale_wanted = 120U;
	head->refresh = 60000U;
	head->dirty = 1U;
	snprintf(head->name, sizeof(head->name), "NETDISP-1");
	if (compose->next_global < 100U)
		compose->next_global = 100U;
	head->global = compose->next_global++;
	kwl_output_global_add(server, head->global);
	ext->image_mem = img_mem;

	pthread_mutex_lock(&netdisp_state.lock);
	netdisp_state.head_open = 1;
	netdisp_state.head_slot = slot;
	if (netdisp_state.has_pending) {
		memcpy(netdisp_state.connected_id, netdisp_state.pending_id,
		       NETDISP_DEVICE_ID_LEN);
		netdisp_state.has_connected = 1;
		netdisp_state.has_pending = 0;
		for (i = 0U; i < KWL_NETDISP_MAX; i++) {
			if (netdisp_state.entries[i].active &&
			    memcmp(netdisp_state.entries[i].device_id,
				   netdisp_state.connected_id,
				   NETDISP_DEVICE_ID_LEN) == 0) {
				netdisp_state.entries[i].state = KWL_NETDISP_CONNECTED;
				break;
			}
		}
	}
	pthread_mutex_unlock(&netdisp_state.lock);

	kwl_compose_unpark(server);
	server->dirty = 1U;
	server->damaged = 1U;
	printf("KWL NETDISP extend_attach ok slot=%u %ux%u\n",
	       slot, (unsigned)width, (unsigned)height);
	kwl_displays_tell(server);
}

static void
netdisp_detach_internal(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	struct kwl_head *head;
	struct kwl_rdp_ext *ext;
	VkImage img;
	VkDeviceMemory img_mem;
	unsigned slot;
	size_t i;

	if (server == NULL || server->compose == NULL)
		return;
	if (!netdisp_state.head_open)
		return;
	compose = server->compose;
	slot = netdisp_state.head_slot;
	if (slot >= KWL_HEADS || !compose->heads[slot].open ||
	    !compose->heads[slot].virtual_rdp) {
		netdisp_state.head_open = 0;
		return;
	}
	head = &compose->heads[slot];

	kwl_compose_park(server);
	kwl_heads_evacuate_slot(server, slot + 1U);
	if (head->global != 0U)
		kwl_output_global_remove(server, head->global);
	kwl_compose_forget(server, 0U, 1U << slot);
	if (head->framebuffers[0] != VK_NULL_HANDLE)
		vkDestroyFramebuffer(compose->device, head->framebuffers[0], NULL);
	if (head->views[0] != VK_NULL_HANDLE)
		vkDestroyImageView(compose->device, head->views[0], NULL);
	ext = head->rdp_ext;
	img = head->output.images[0];
	img_mem = ext != NULL ? ext->image_mem : VK_NULL_HANDLE;
	memset(head, 0, sizeof(*head));

	pthread_mutex_lock(&netdisp_state.lock);
	netdisp_state.head_open = 0;
	if (netdisp_state.has_connected) {
		for (i = 0U; i < KWL_NETDISP_MAX; i++) {
			if (netdisp_state.entries[i].active &&
			    memcmp(netdisp_state.entries[i].device_id,
				   netdisp_state.connected_id,
				   NETDISP_DEVICE_ID_LEN) == 0) {
				netdisp_state.entries[i].state = KWL_NETDISP_IDLE;
				break;
			}
		}
		netdisp_state.has_connected = 0;
	}
	pthread_mutex_unlock(&netdisp_state.lock);

	kwl_compose_unpark(server);
	rdp_ext_destroy(ext, img);
	if (img_mem != VK_NULL_HANDLE)
		vkFreeMemory(compose->device, img_mem, NULL);
	server->dirty = 1U;
	kwl_displays_tell(server);
}

void
kwl_netdisp_copy_frame(
	struct kwl_compose *compose,
	struct kwl_head *head)
{
	struct kwl_rdp_ext *ext;
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barrier;
	VkBufferImageCopy region;
	VkSubmitInfo submit;
	VkImage image;

	if (compose == NULL || head == NULL || head->rdp_ext == NULL)
		return;
	ext = head->rdp_ext;
	image = head->output.images[0];
	if (image == VK_NULL_HANDLE)
		return;

	(void)vkResetFences(ext->device, 1U, &ext->copy_fence);
	(void)vkResetCommandBuffer(ext->cmd_buf, 0U);
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(ext->cmd_buf, &begin) != VK_SUCCESS)
		return;

	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(ext->cmd_buf,
			     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			     VK_PIPELINE_STAGE_TRANSFER_BIT,
			     0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	memset(&region, 0, sizeof(region));
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1U;
	region.imageExtent.width = ext->width;
	region.imageExtent.height = ext->height;
	region.imageExtent.depth = 1U;
	vkCmdCopyImageToBuffer(ext->cmd_buf, image,
			       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			       ext->staging, 1U, &region);

	barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	vkCmdPipelineBarrier(ext->cmd_buf,
			     VK_PIPELINE_STAGE_TRANSFER_BIT,
			     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			     0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	if (vkEndCommandBuffer(ext->cmd_buf) != VK_SUCCESS)
		return;

	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &ext->cmd_buf;
	kwl_queue_lock(compose);
	if (vkQueueSubmit(ext->queue, 1U, &submit, ext->copy_fence) != VK_SUCCESS) {
		kwl_queue_unlock(compose);
		return;
	}
	kwl_queue_unlock(compose);

	if (vkWaitForFences(ext->device, 1U, &ext->copy_fence, VK_TRUE,
			    UINT64_MAX) != VK_SUCCESS)
		return;

	pthread_mutex_lock(&ext->mu);
	memcpy(ext->cpu_bgra, ext->mapped, ext->staging_size);
	ext->frame_ready = 1;
	pthread_cond_signal(&ext->cond);
	pthread_mutex_unlock(&ext->mu);
}

int
kwl_netdisp_rdp_fd(
	const struct kwl_server *server)
{
	if (server == NULL || server->compose == NULL || !netdisp_state.head_open)
		return -1;
	if (netdisp_state.head_slot >= KWL_HEADS)
		return -1;
	if (!server->compose->heads[netdisp_state.head_slot].open ||
	    server->compose->heads[netdisp_state.head_slot].rdp_ext == NULL)
		return -1;
	return server->compose->heads[netdisp_state.head_slot].rdp_ext->fd;
}

int
kwl_netdisp_active(
	const struct kwl_server *server)
{
	return kwl_netdisp_rdp_fd(server) >= 0;
}

static uint32_t
ps2_to_evdev(
	uint32_t scancode,
	uint32_t flags)
{
	if ((flags & 0x0100U) != 0U) {
		switch (scancode) {
		case 0x1CU: return 96U;
		case 0x1DU: return 97U;
		case 0x35U: return 98U;
		case 0x38U: return 100U;
		case 0x47U: return 102U;
		case 0x48U: return 103U;
		case 0x49U: return 104U;
		case 0x4BU: return 105U;
		case 0x4DU: return 106U;
		case 0x4FU: return 107U;
		case 0x50U: return 108U;
		case 0x51U: return 109U;
		case 0x52U: return 110U;
		case 0x53U: return 111U;
		case 0x5BU: return 125U;
		case 0x5CU: return 126U;
		default: return 0U;
		}
	}
	if (scancode < 128U)
		return scancode;
	return 0U;
}

void
kwl_netdisp_poll(
	struct kwl_server *server)
{
	struct kwl_head *head;
	struct kwl_rdp_ext *ext;

	if (server == NULL || server->compose == NULL || !netdisp_state.head_open)
		return;
	head = &server->compose->heads[netdisp_state.head_slot];
	if (!head->open || head->rdp_ext == NULL)
		return;
	ext = head->rdp_ext;

	for (;;) {
		ssize_t n = read(ext->fd, ext->ctrl_buf + ext->ctrl_used,
				 KWL_RDP_CTRL_HDR_SIZE - ext->ctrl_used);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			netdisp_detach_internal(server);
			return;
		}
		if (n == 0) {
			netdisp_detach_internal(server);
			return;
		}
		ext->ctrl_used += (size_t)n;
		if (ext->ctrl_used < KWL_RDP_CTRL_HDR_SIZE)
			return;
		ext->ctrl_used = 0U;

		{
			uint32_t type = 0U;
			uint32_t arg1 = 0U;
			uint32_t arg2 = 0U;
			uint32_t now_ms = (uint32_t)netdisp_now_ms();
			kwl_rdp_unpack_ctrl(ext->ctrl_buf, &type, &arg1, &arg2);
			if (type == KWL_RDP_MSG_FORCE_KEY) {
				pthread_mutex_lock(&ext->mu);
				ext->force_key = 1;
				pthread_mutex_unlock(&ext->mu);
			} else if (type == KWL_RDP_MSG_POINTER) {
				uint32_t lx = (arg2 >> 16) & 0xFFFFU;
				uint32_t ly = arg2 & 0xFFFFU;
				int32_t gx = head->x + (int32_t)((lx * 120U) /
					(head->scale120 != 0U ? head->scale120 : 120U));
				int32_t gy = head->y + (int32_t)((ly * 120U) /
					(head->scale120 != 0U ? head->scale120 : 120U));
				uint16_t down = (arg1 & 0x8000U) != 0U ? 1U : 0U;
				kwl_pointer_warp_to(server, gx, gy);
				kwl_seat_motion(server, now_ms);
				if ((arg1 & 0x1000U) != 0U)
					kwl_seat_button(server, now_ms, 0x110U, down);
				if ((arg1 & 0x2000U) != 0U)
					kwl_seat_button(server, now_ms, 0x111U, down);
				if ((arg1 & 0x4000U) != 0U)
					kwl_seat_button(server, now_ms, 0x112U, down);
			} else if (type == KWL_RDP_MSG_KEY) {
				uint32_t key = ps2_to_evdev(arg1, arg2);
				uint32_t state = (arg2 & 0x8001U) != 0U ? 0U : 1U;
				if (key != 0U)
					kwl_seat_key(server, now_ms, key, state);
			}
		}
	}
}
