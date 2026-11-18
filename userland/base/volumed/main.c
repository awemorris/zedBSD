/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The removable media daemon (ws132-p004).
 *
 * It subscribes to the disks' events of /dev/system, and 300 ms after the
 * last one looks at every block device again: a removable leaf (a
 * partition, or a disk without partitions) whose filesystem is FAT or UFS
 * is a volume, unless it is on the disk the system started from (the disk
 * of the root filesystem, ws132-p009: a USB stick the machine boots from
 * is removable too, and its partitions are not the user's media).  It
 * never mounts one by itself (the user's decision D3):
 * the seat's user asks on /run/volumed.sock, and it is mounted under
 * /media with nosuid and noexec, FAT showing that user as the owner of its
 * files.  An eject unmounts it (a busy one is answered with the program
 * that uses it); a volume pulled out while mounted is unmounted by force
 * and its folder removed.  Each change goes to every client as VOLUME or
 * GONE lines and a DONE.
 */

#include "userland/base/volumed/volumed.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <uapi/blkid.h>
#include <uapi/block.h>
#include <uapi/mountinfo.h>
#include <uapi/system.h>

/* The quiet time after a disk's event before the disks are looked at again. */
#define VOLUMED_SETTLE_MS	300U

/* The most block devices looked at in one scan. */
#define VOLUMED_DEVICES_MAX	64U

/* The display whose owner is the seat's user. */
#define VOLUMED_SEAT_NODE	"/dev/gpu0"

/* The login screen's account (sessiond's), which has no session to mount for. */
#define VOLUMED_GREETER		"_greeter"

/* One block device found by a scan. */
struct volumed_device {
	char name[VOLUMED_NAME_MAX];
	uint32_t device;
	uint32_t parent;
	uint32_t flags;
	uint64_t bytes;
};

/* One client of the socket: its descriptor and the bytes of a line not ended yet. */
struct volumed_client {
	int descriptor;
	char input[VOLUMED_LINE_MAX];
	size_t used;
};

static int volumed_listen(void);
static int volumed_subscribe(void);
static void volumed_events(int descriptor, uint64_t *due);
static void volumed_scan(void);
static unsigned volumed_devices(struct volumed_device *devices, unsigned capacity);
static int volumed_candidate(const struct volumed_device *devices, unsigned count, unsigned index, uint32_t boot_disk, struct volumed_volume *volume);
static uint32_t volumed_boot_disk(const struct volumed_device *devices, unsigned count);
static uint32_t volumed_disk_of(const struct volumed_device *devices, unsigned count, unsigned index);
static void volumed_merge(const struct volumed_volume *found, unsigned count);
static void volumed_accept(int listener);
static void volumed_read(struct volumed_client *client);
static void volumed_line(struct volumed_client *client, const char *line);
static void volumed_hello(struct volumed_client *client);
static int volumed_mount(struct volumed_volume *volume, uid_t uid, gid_t gid);
static int volumed_eject(struct volumed_volume *volume, char *user, size_t size);
static void volumed_user(const char *path, char *user, size_t size);
static void volumed_tidy(struct volumed_volume *volume);
static void volumed_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
static struct volumed_volume *volumed_find(const char *id);
static uid_t volumed_seat(void);
static void volumed_send(struct volumed_client *client, const char *line);
static void volumed_broadcast(const char *line);
static void volumed_broadcast_volume(const struct volumed_volume *volume);
static void volumed_close(struct volumed_client *client);
static uint64_t volumed_now_ms(void);

/* The volumes known now. */
static struct volumed_volume volumed_volumes[VOLUMED_VOLUMES_MAX];
static unsigned volumed_volume_count;

/* The clients; a descriptor of -1 is a free slot. */
static struct volumed_client volumed_clients[VOLUMED_CLIENTS_MAX];

/* The login screen's uid (none: the largest uid). */
static uid_t volumed_greeter = (uid_t)-1;

/*
 * Serves the volumes until killed.
 */
int
main(
	void)
{
	struct pollfd descriptors[2U + VOLUMED_CLIENTS_MAX];
	struct passwd *greeter;
	uint64_t due;
	uint64_t now;
	unsigned count;
	unsigned index;
	int timeout;
	int listener;
	int events;
	int ready;

	/* A client that goes away must not end volumed; what it does goes to the system's log. */
	(void)signal(SIGPIPE, SIG_IGN);
	openlog("volumed", LOG_PID, LOG_DAEMON);

	/* The login screen's account. */
	greeter = getpwnam(VOLUMED_GREETER);
	if (greeter != NULL)
		volumed_greeter = greeter->pw_uid;

	/* No client yet. */
	for (index = 0U; index < VOLUMED_CLIENTS_MAX; index++)
		volumed_clients[index].descriptor = -1;

	/* The disks' events first, so a disk inserted during the first scan is not missed. */
	events = volumed_subscribe();
	if (events < 0)
		volumed_log("volumed: /dev/system events unavailable (%s); disks are looked at only at start\n", strerror(errno));

	/* The folder volumes are mounted under, and the socket. */
	(void)mkdir(VOLUMED_MEDIA, 0755);
	listener = volumed_listen();
	if (listener < 0) {
		volumed_log("volumed: %s: %s\n", VOLUMED_SOCKET, strerror(errno));
		return 1;
	}

	/* The volumes there are now. */
	volumed_scan();
	volumed_log("VOLUMED READY volumes=%u\n", volumed_volume_count);

	/* Events, clients and the settled scans. */
	due = 0U;
	for (;;) {
		/* The listener, the events and every client. */
		count = 0U;
		descriptors[count].fd = listener;
		descriptors[count].events = POLLIN;
		count++;
		if (events >= 0) {
			descriptors[count].fd = events;
			descriptors[count].events = POLLIN;
			count++;
		}

		/* Each client's descriptor. */
		for (index = 0U; index < VOLUMED_CLIENTS_MAX; index++) {
			if (volumed_clients[index].descriptor < 0)
				continue;
			descriptors[count].fd = volumed_clients[index].descriptor;
			descriptors[count].events = POLLIN;
			count++;
		}

		/* Waits for something, or for the scan that is due. */
		timeout = -1;
		if (due != 0U) {
			now = volumed_now_ms();
			timeout = 0;
			if (due > now)
				timeout = (int)(due - now);
		}

		/* The wait. */
		for (index = 0U; index < count; index++)
			descriptors[index].revents = 0;
		ready = poll(descriptors, count, timeout);
		if (ready < 0 && errno != EINTR)
			return 1;

		/* A scan whose quiet time has passed. */
		now = volumed_now_ms();
		if (due != 0U && now >= due) {
			due = 0U;
			volumed_scan();
		}

		/* A new client, the events, then each client's lines. */
		if ((descriptors[0].revents & POLLIN) != 0)
			volumed_accept(listener);
		if (events >= 0 && (descriptors[1].revents & (POLLIN | POLLERR | POLLHUP)) != 0)
			volumed_events(events, &due);
		for (index = 0U; index < VOLUMED_CLIENTS_MAX; index++) {
			if (volumed_clients[index].descriptor < 0)
				continue;
			volumed_read(&volumed_clients[index]);
		}
	}
}

/* Listens on /run/volumed.sock, which anyone may use (the requests are checked by uid). */
static int
volumed_listen(
	void)
{
	struct sockaddr_un address;
	int descriptor;
	int status;

	/* The socket, in place of an old one. */
	descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", VOLUMED_SOCKET);
	(void)unlink(VOLUMED_SOCKET);
	status = bind(descriptor, (struct sockaddr *)&address, sizeof(address));
	if (status == 0)
		status = listen(descriptor, 8);
	if (status != 0) {
		(void)close(descriptor);
		return -1;
	}

	/* Everyone may connect, and the listener never blocks. */
	(void)chmod(VOLUMED_SOCKET, 0666);
	(void)fcntl(descriptor, F_SETFL, O_NONBLOCK);

	/* Succeeded. */
	return descriptor;
}

/* Opens /dev/system and subscribes to the disks' events; -1 when it cannot. */
static int
volumed_subscribe(
	void)
{
	struct system_event_subscription subscription;
	int descriptor;
	int status;

	/* The node, non-blocking. */
	descriptor = open("/dev/system", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (descriptor < 0)
		return -1;

	/* The disks' events, and the USB devices' (a stick pulled out while mounted goes as a USB device). */
	memset(&subscription, 0, sizeof(subscription));
	subscription.classes = KERN_SYSTEM_EVENT_DISK | KERN_SYSTEM_EVENT_USB;
	status = ioctl(descriptor, KERN_SYSTEM_EVENT_SUBSCRIBE, &subscription);
	if (status != 0) {
		(void)close(descriptor);
		return -1;
	}

	/* Succeeded. */
	return descriptor;
}

/* Reads the events waiting and puts the next scan 300 ms after the last disk's. */
static void
volumed_events(
	int descriptor,
	uint64_t *due)
{
	struct system_event records[8];
	ssize_t count;
	size_t index;

	/* Every record there now. */
	for (;;) {
		count = read(descriptor, records, sizeof(records));
		if (count <= 0)
			return;

		/* A disk or a USB device added or removed, or events lost: the disks are looked at again once quiet. */
		for (index = 0U; index < (size_t)count / sizeof(records[0]); index++) {
			if (records[index].class_bit != KERN_SYSTEM_EVENT_DISK &&
			    records[index].class_bit != KERN_SYSTEM_EVENT_USB &&
			    records[index].class_bit != KERN_SYSTEM_EVENT_OVERFLOW)
				continue;
			volumed_log("VOLUMED EVENT class=0x%x subject=%s action=%u\n", records[index].class_bit, records[index].subject, records[index].action);
			*due = volumed_now_ms() + VOLUMED_SETTLE_MS;
		}
	}
}

/* Looks at every block device and brings the volumes up to date. */
static void
volumed_scan(
	void)
{
	struct volumed_device devices[VOLUMED_DEVICES_MAX];
	struct volumed_volume found[VOLUMED_VOLUMES_MAX];
	uint32_t boot_disk;
	unsigned count;
	unsigned index;
	unsigned volumes;
	int candidate;

	/* The devices, and the disk the system started from, whose partitions are not media. */
	count = volumed_devices(devices, VOLUMED_DEVICES_MAX);
	boot_disk = volumed_boot_disk(devices, count);

	/* The volumes among the devices. */
	volumes = 0U;
	for (index = 0U; index < count && volumes < VOLUMED_VOLUMES_MAX; index++) {
		candidate = volumed_candidate(devices, count, index, boot_disk, &found[volumes]);
		if (candidate)
			volumes++;
	}

	/* What changed goes to the clients. */
	volumed_merge(found, volumes);
}

/* Finds the block devices under /dev; returns how many. */
static unsigned
volumed_devices(
	struct volumed_device *devices,
	unsigned capacity)
{
	struct kern_block_info info;
	struct dirent *entry;
	struct stat status;
	char path[VOLUMED_NAME_MAX + 8U];
	DIR *directory;
	unsigned count;
	size_t length;
	int descriptor;
	int block;
	int error;

	/* Each entry of /dev that is a block device. */
	count = 0U;
	directory = opendir("/dev");
	if (directory == NULL)
		return 0U;
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL || count >= capacity)
			break;

		/* A block device by a name that fits. */
		length = strlen(entry->d_name);
		if (length >= VOLUMED_NAME_MAX)
			continue;
		(void)snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
		error = stat(path, &status);
		if (error != 0)
			continue;
		block = S_ISBLK(status.st_mode);
		if (!block)
			continue;

		/* Its geometry, parent and flags. */
		descriptor = open(path, O_RDONLY | O_CLOEXEC);
		if (descriptor < 0)
			continue;
		memset(&info, 0, sizeof(info));
		info.version = KERN_BLOCK_VERSION;
		info.struct_size = sizeof(info);
		error = ioctl(descriptor, BLKGETINFO, &info);
		(void)close(descriptor);
		if (error != 0)
			continue;

		/* Kept. */
		(void)snprintf(devices[count].name, sizeof(devices[count].name), "%s", entry->d_name);
		devices[count].device = info.device;
		devices[count].parent = info.parent_device;
		devices[count].flags = info.flags;
		devices[count].bytes = info.sector_count * info.sector_size;
		count++;
	}

	/* Succeeded: the devices found. */
	(void)closedir(directory);
	return count;
}

/*
 * Tells whether a device is a volume and describes it: a leaf (no other
 * device's parent), removable itself or by its parent, not file-backed,
 * not on the disk the system started from (boot_disk, 0 when unknown), and
 * FAT or UFS.  Returns 1 with the volume filled, or 0.
 */
static int
volumed_candidate(
	const struct volumed_device *devices,
	unsigned count,
	unsigned index,
	uint32_t boot_disk,
	struct volumed_volume *volume)
{
	struct block_identity identity;
	const struct volumed_device *device;
	char path[VOLUMED_NAME_MAX + 8U];
	uint32_t disk;
	unsigned other;
	unsigned removable;
	int descriptor;
	int error;
	int same;

	/* Not a file-backed device, and no device's parent. */
	device = &devices[index];
	if ((device->flags & KERN_BLOCK_FILE_BACKED) != 0U)
		return 0;
	removable = device->flags & KERN_BLOCK_REMOVABLE;
	for (other = 0U; other < count; other++) {
		if (devices[other].parent == device->device && other != index)
			return 0;
		if (device->parent != 0U && devices[other].device == device->parent)
			removable |= devices[other].flags & KERN_BLOCK_REMOVABLE;
	}

	/* Removable media only. */
	if (removable == 0U)
		return 0;

	/* Not a partition of the disk the system started from. */
	disk = volumed_disk_of(devices, count, index);
	if (boot_disk != 0U && disk == boot_disk)
		return 0;

	/* Its filesystem and label. */
	(void)snprintf(path, sizeof(path), "/dev/%s", device->name);
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return 0;
	memset(&identity, 0, sizeof(identity));
	error = ioctl(descriptor, BLKGETIDENTITY, &identity);
	(void)close(descriptor);
	if (error != 0 || (identity.flags & KERN_BLKID_TYPE) == 0U)
		return 0;

	/* FAT (the identity says vfat) and UFS are what zedBSD mounts. */
	memset(volume, 0, sizeof(*volume));
	same = strcmp(identity.type, "vfat");
	if (same == 0)
		(void)snprintf(volume->fs, sizeof(volume->fs), "fat");
	same = strcmp(identity.type, "ufs");
	if (same == 0)
		(void)snprintf(volume->fs, sizeof(volume->fs), "ufs");
	if (volume->fs[0] == '\0')
		return 0;

	/* The rest of it. */
	(void)snprintf(volume->id, sizeof(volume->id), "%s", device->name);
	if ((identity.flags & KERN_BLKID_LABEL) != 0U)
		(void)snprintf(volume->label, sizeof(volume->label), "%s", identity.label);
	volume->bytes = device->bytes;

	/* Succeeded: a volume. */
	return 1;
}

/*
 * Finds the disk the system started from: the disk (the topmost device)
 * under the root filesystem, as the kernel's mount table names its device.
 * Returns its device number, or 0 when the root is not on a block device
 * of the list (a RAM disk) or the table cannot be read.
 */
static uint32_t
volumed_boot_disk(
	const struct volumed_device *devices,
	unsigned count)
{
	struct kern_mount_query *query;
	const struct kern_mount_info *entry;
	char source[KERN_MOUNT_INFO_PATH_MAX + 1U];
	uint32_t disk;
	unsigned index;
	unsigned device;
	int descriptor;
	int status;
	int same;

	/* Room for the whole table. */
	query = calloc(1, sizeof(*query) + KERN_MOUNT_INFO_MAX * sizeof(query->entries[0]));
	if (query == NULL)
		return 0U;
	query->version = KERN_MOUNT_INFO_VERSION;
	query->struct_size = sizeof(*query);
	query->capacity = KERN_MOUNT_INFO_MAX;

	/* The kernel's mount table. */
	descriptor = open("/dev/system", O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		free(query);
		return 0U;
	}

	/* Read once; the descriptor goes at once. */
	status = ioctl(descriptor, KERN_SYSTEM_GET_MOUNTS, query);
	(void)close(descriptor);
	if (status < 0) {
		free(query);
		return 0U;
	}

	/* The root's entry, mounted from a block device (its source is the device's name under /dev). */
	disk = 0U;
	for (index = 0U; index < query->count; index++) {
		entry = &query->entries[index];
		same = strcmp(entry->target, "/");
		if (same != 0 ||
		    entry->device == 0U ||
		    (entry->kind & KERN_MOUNT_INFO_BIND) != 0U)
			continue;
		(void)snprintf(source, sizeof(source), "%.*s", (int)sizeof(entry->source), entry->source);

		/* That device among the scan's, and the disk it is on. */
		for (device = 0U; device < count; device++) {
			same = strcmp(devices[device].name, source);
			if (same == 0) {
				disk = volumed_disk_of(devices, count, device);
				break;
			}
		}

		/* The root has one entry. */
		break;
	}

	/* The table is no longer needed. */
	free(query);
	return disk;
}

/*
 * Finds the disk a device is on: its topmost parent among the devices
 * (a partition's slice's disk), or the device itself when it has none.
 */
static uint32_t
volumed_disk_of(
	const struct volumed_device *devices,
	unsigned count,
	unsigned index)
{
	uint32_t current;
	uint32_t parent;
	unsigned steps;
	unsigned other;

	/* Up the parents, at most as many steps as there are devices (a loop cannot hold it). */
	current = devices[index].device;
	parent = devices[index].parent;
	for (steps = 0U; parent != 0U && steps < count; steps++) {
		current = parent;
		parent = 0U;
		for (other = 0U; other < count; other++) {
			if (devices[other].device == current) {
				parent = devices[other].parent;
				break;
			}
		}
	}

	/* The topmost device reached. */
	return current;
}

/* Brings the volumes up to date with a scan's, telling the clients what changed. */
static void
volumed_merge(
	const struct volumed_volume *found,
	unsigned count)
{
	struct volumed_volume *volume;
	unsigned index;
	unsigned changed;
	char line[VOLUMED_LINE_MAX];

	/* Nothing seen yet. */
	changed = 0U;
	for (index = 0U; index < volumed_volume_count; index++)
		volumed_volumes[index].seen = 0U;

	/* Each volume found: known (its label may change), or new. */
	for (index = 0U; index < count; index++) {
		volume = volumed_find(found[index].id);
		if (volume == NULL && volumed_volume_count < VOLUMED_VOLUMES_MAX) {
			volume = &volumed_volumes[volumed_volume_count];
			volumed_volume_count++;
			*volume = found[index];
			volume->fresh = 1U;
			volumed_log("VOLUMED ADD id=%s fs=%s label=%s size=%llu\n",
			    volume->id, volume->fs, volume->label, (unsigned long long)volume->bytes);
			volumed_broadcast_volume(volume);
			changed = 1U;
		}

		/* A volume that could not be kept (too many) is not shown. */
		if (volume != NULL)
			volume->seen = 1U;
	}

	/* The volumes gone: a mounted one is unmounted by force and its folder removed. */
	index = 0U;
	while (index < volumed_volume_count) {
		volume = &volumed_volumes[index];
		if (volume->seen != 0U) {
			index++;
			continue;
		}

		/* Tidied, told and taken off the list. */
		volumed_log("VOLUMED REMOVE id=%s forced=%u\n", volume->id, volume->path[0] != '\0');
		volumed_tidy(volume);
		(void)snprintf(line, sizeof(line), "GONE id=%s", volume->id);
		volumed_broadcast(line);
		volumed_volume_count--;
		volumed_volumes[index] = volumed_volumes[volumed_volume_count];
		changed = 1U;
	}

	/* The changes are one state. */
	if (changed != 0U)
		volumed_broadcast("DONE");
}

/* Takes a new client. */
static void
volumed_accept(
	int listener)
{
	unsigned index;
	int descriptor;

	/* The connection; it never blocks. */
	descriptor = accept(listener, NULL, NULL);
	if (descriptor < 0)
		return;
	(void)fcntl(descriptor, F_SETFL, O_NONBLOCK);
	(void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);

	/* A free slot, or none. */
	for (index = 0U; index < VOLUMED_CLIENTS_MAX; index++) {
		if (volumed_clients[index].descriptor >= 0)
			continue;
		volumed_clients[index].descriptor = descriptor;
		volumed_clients[index].used = 0U;
		return;
	}

	/* Too many clients. */
	(void)close(descriptor);
}

/* Reads a client's bytes and carries out each whole line. */
static void
volumed_read(
	struct volumed_client *client)
{
	ssize_t count;
	char *end;
	size_t length;

	/* What came, without waiting. */
	count = recv(client->descriptor, client->input + client->used, sizeof(client->input) - 1U - client->used, 0);
	if (count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
		volumed_close(client);
		return;
	}

	/* Nothing yet, or the bytes added to the line. */
	if (count < 0)
		return;
	client->used += (size_t)count;
	client->input[client->used] = '\0';

	/* Each whole line. */
	for (;;) {
		end = strchr(client->input, '\n');
		if (end == NULL)
			break;
		*end = '\0';
		volumed_line(client, client->input);
		if (client->descriptor < 0)
			return;
		length = (size_t)(end + 1 - client->input);
		memmove(client->input, end + 1, client->used - length + 1U);
		client->used -= length;
	}

	/* A line longer than any is a broken client. */
	if (client->used >= sizeof(client->input) - 1U)
		volumed_close(client);
}

/* Carries out one line of a client. */
static void
volumed_line(
	struct volumed_client *client,
	const char *line)
{
	struct volumed_volume *volume;
	char id[VOLUMED_NAME_MAX];
	char user[64];
	char reply[VOLUMED_LINE_MAX];
	unsigned request;
	uid_t uid;
	gid_t gid;
	int permitted;
	int status;
	int error;
	int ask;

	/* What it asks. */
	error = volumed_parse(line, &ask, &request, id, sizeof(id));
	if (error != 0) {
		volumed_send(client, "RESULT 0 22");
		return;
	}

	/* The greeting: the volumes there are. */
	if (ask == VOLUMED_ASK_HELLO) {
		volumed_hello(client);
		return;
	}

	/* Who asks, and whether they may. */
	uid = (uid_t)-1;
	gid = (gid_t)-1;
	status = getpeereid(client->descriptor, &uid, &gid);
	permitted = 0;
	if (status == 0)
		permitted = volumed_permitted(uid, volumed_seat(), volumed_greeter);

	/* The volume. */
	volume = volumed_find(id);
	user[0] = '\0';
	if (!permitted) {
		error = EACCES;
	} else if (volume == NULL) {
		error = ENOENT;
	} else if (ask == VOLUMED_ASK_MOUNT) {
		error = volumed_mount(volume, uid, gid);
	} else {
		error = volumed_eject(volume, user, sizeof(user));
	}

	/* The answer, the program that holds a busy volume with it. */
	if (user[0] != '\0') {
		(void)snprintf(reply, sizeof(reply), "RESULT %u %d user=%s", request, error, user);
	} else {
		(void)snprintf(reply, sizeof(reply), "RESULT %u %d", request, error);
	}

	/* Sent. */
	volumed_send(client, reply);
}

/* Sends a client every volume and a DONE. */
static void
volumed_hello(
	struct volumed_client *client)
{
	char line[VOLUMED_LINE_MAX];
	unsigned index;
	int error;

	/* Each volume. */
	for (index = 0U; index < volumed_volume_count; index++) {
		error = volumed_format_volume(&volumed_volumes[index], line, sizeof(line));
		if (error == 0)
			volumed_send(client, line);
	}

	/* One state. */
	volumed_send(client, "DONE");
}

/*
 * Mounts a volume under /media for a user: nosuid and noexec, FAT showing
 * the user as the owner of its files.  Returns 0 (also when it is mounted
 * already) or an errno value.
 */
static int
volumed_mount(
	struct volumed_volume *volume,
	uid_t uid,
	gid_t gid)
{
	struct mount_args_owner arguments;
	struct stat status;
	char name[VOLUMED_LABEL_MAX];
	char path[VOLUMED_PATH_MAX];
	unsigned suffix;
	int error;

	/* Mounted already. */
	if (volume->path[0] != '\0')
		return 0;

	/* A folder name of its own: the label, then -2, -3 when it is taken. */
	error = volumed_mount_name(volume->label, volume->id, name, sizeof(name));
	if (error != 0)
		return error;
	(void)snprintf(path, sizeof(path), "%s/%s", VOLUMED_MEDIA, name);
	for (suffix = 2U; suffix < 100U; suffix++) {
		error = stat(path, &status);
		if (error != 0)
			break;
		(void)snprintf(path, sizeof(path), "%s/%s-%u", VOLUMED_MEDIA, name, suffix);
	}

	/* The folder. */
	(void)mkdir(VOLUMED_MEDIA, 0755);
	error = mkdir(path, 0755);
	if (error != 0) {
		error = errno;
		return error;
	}

	/* The mount: the disk, and the user as the owner FAT shows. */
	memset(&arguments, 0, sizeof(arguments));
	arguments.size = sizeof(arguments);
	arguments.version = KERN_MOUNT_ARGS_VERSION_OWNER;
	(void)snprintf(arguments.fspec, sizeof(arguments.fspec), "%s", volume->id);
	arguments.owner_uid = (uint32_t)uid;
	arguments.owner_gid = (uint32_t)gid;
	arguments.flags = KERN_MOUNT_ARGS_OWNER;
	error = mount(volume->fs, path, (int)(MNT_NOSUID | MNT_NOEXEC), &arguments);
	if (error != 0) {
		error = errno;
		(void)rmdir(path);
		volumed_log("VOLUMED MOUNT id=%s path=%s uid=%u error=%d\n", volume->id, path, (unsigned)uid, error);
		return error;
	}

	/* Mounted: told to the clients. */
	(void)snprintf(volume->path, sizeof(volume->path), "%s", path);
	volume->fresh = 0U;
	volumed_log("VOLUMED MOUNT id=%s path=%s uid=%u error=0\n", volume->id, path, (unsigned)uid);
	volumed_broadcast_volume(volume);
	volumed_broadcast("DONE");

	/* Succeeded. */
	return 0;
}

/*
 * Ejects a volume: unmounts it (not by force) and removes its folder.
 * Returns 0 (also when it is not mounted), or EBUSY with the program that
 * uses it in user, or another errno value.
 */
static int
volumed_eject(
	struct volumed_volume *volume,
	char *user,
	size_t size)
{
	int status;
	int error;

	/* Not mounted: nothing holds it, and it may be taken out. */
	if (volume->path[0] == '\0') {
		volumed_log("VOLUMED EJECT id=%s error=0\n", volume->id);
		return 0;
	}

	/* The unmount; a busy one names the program. */
	status = unmount(volume->path, 0);
	if (status != 0) {
		error = errno;
		if (error == EBUSY)
			volumed_user(volume->path, user, size);
		volumed_log("VOLUMED EJECT id=%s error=%d user=%s\n", volume->id, error, user);
		return error;
	}

	/* Unmounted: the folder goes, and the clients are told. */
	(void)rmdir(volume->path);
	volume->path[0] = '\0';
	volumed_log("VOLUMED EJECT id=%s error=0\n", volume->id);
	volumed_broadcast_volume(volume);
	volumed_broadcast("DONE");

	/* Succeeded: it may be taken out. */
	return 0;
}

/* Names the first program that uses a mount ("" when none is found). */
static void
volumed_user(
	const char *path,
	char *user,
	size_t size)
{
	struct system_file_usage usage;
	struct process_info process;
	int descriptor;
	int status;

	/* Nothing found yet. */
	user[0] = '\0';
	descriptor = open("/dev/system", O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return;

	/* The first process using the mount. */
	memset(&usage, 0, sizeof(usage));
	usage.version = KERN_SYSTEM_FILE_USAGE_VERSION;
	usage.struct_size = sizeof(usage);
	usage.cursor_pid = -1;
	usage.query_flags = KERN_SYSTEM_FILE_USAGE_QUERY_MOUNT;
	(void)snprintf(usage.path, sizeof(usage.path), "%s", path);
	status = ioctl(descriptor, KERN_SYSTEM_GET_FILE_USAGE, &usage);
	if (status != 0) {
		(void)close(descriptor);
		return;
	}

	/* Its command (the process after pid - 1 is that process). */
	memset(&process, 0, sizeof(process));
	process.pid = usage.pid - 1;
	status = ioctl(descriptor, KERN_SYSTEM_GET_PROCESS, &process);
	if (status == 0 && process.pid == usage.pid)
		(void)snprintf(user, size, "%.*s", (int)sizeof(process.command), process.command);
	(void)close(descriptor);

	/* Without a name, the pid. */
	if (user[0] == '\0')
		(void)snprintf(user, size, "pid%d", (int)usage.pid);
}

/* Unmounts a pulled-out volume by force and removes its folder. */
static void
volumed_tidy(
	struct volumed_volume *volume)
{
	int result;

	/* Not mounted: nothing to tidy. */
	if (volume->path[0] == '\0')
		return;

	/* By force: the disk is gone.  A failure is told, for the folder then stays. */
	result = unmount(volume->path, MNT_FORCE);
	if (result != 0)
		volumed_log("VOLUMED TIDY path=%s unmount=%d\n", volume->path, errno);
	result = rmdir(volume->path);
	if (result != 0)
		volumed_log("VOLUMED TIDY path=%s rmdir=%d\n", volume->path, errno);
	volume->path[0] = '\0';
}

/*
 * Writes one line of what volumed does to the system's log (/var/log/messages)
 * and to its standard error.
 */
static void
volumed_log(
	const char *format,
	...)
{
	char line[512];
	va_list arguments;
	size_t length;

	/* The line, without its newline. */
	va_start(arguments, format);
	(void)vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	length = strlen(line);
	if (length > 0U && line[length - 1U] == '\n')
		line[length - 1U] = '\0';

	/* Both places. */
	syslog(LOG_NOTICE, "%s", line);
	fprintf(stderr, "%s\n", line);
}

/* Finds a known volume by its ID. */
static struct volumed_volume *
volumed_find(
	const char *id)
{
	unsigned index;
	int same;

	/* Each volume. */
	for (index = 0U; index < volumed_volume_count; index++) {
		same = strcmp(volumed_volumes[index].id, id);
		if (same == 0)
			return &volumed_volumes[index];
	}

	/* None. */
	return NULL;
}

/* Reports the seat's user: the display's owner (root without a display). */
static uid_t
volumed_seat(
	void)
{
	struct stat status;
	int error;

	/* The owner sessiond gave the display. */
	error = stat(VOLUMED_SEAT_NODE, &status);
	if (error != 0)
		return 0;

	/* Succeeded. */
	return status.st_uid;
}

/* Sends a line to a client; a client that cannot take it is dropped. */
static void
volumed_send(
	struct volumed_client *client,
	const char *line)
{
	char buffer[VOLUMED_LINE_MAX + 1U];
	ssize_t sent;
	int length;

	/* The line with its newline, whole or not at all. */
	if (client->descriptor < 0)
		return;
	length = snprintf(buffer, sizeof(buffer), "%s\n", line);
	if (length < 0 || (size_t)length >= sizeof(buffer))
		return;
	sent = send(client->descriptor, buffer, (size_t)length, MSG_DONTWAIT);
	if (sent != (ssize_t)length)
		volumed_close(client);
}

/* Sends a line to every client. */
static void
volumed_broadcast(
	const char *line)
{
	unsigned index;

	/* Each client. */
	for (index = 0U; index < VOLUMED_CLIENTS_MAX; index++)
		volumed_send(&volumed_clients[index], line);
}

/* Sends a volume's line to every client. */
static void
volumed_broadcast_volume(
	const struct volumed_volume *volume)
{
	char line[VOLUMED_LINE_MAX];
	int error;

	/* The line, when it fits. */
	error = volumed_format_volume(volume, line, sizeof(line));
	if (error == 0)
		volumed_broadcast(line);
}

/* Drops a client. */
static void
volumed_close(
	struct volumed_client *client)
{
	/* The slot is free again. */
	if (client->descriptor >= 0)
		(void)close(client->descriptor);
	client->descriptor = -1;
	client->used = 0U;
}

/* The monotonic time in milliseconds. */
static uint64_t
volumed_now_ms(
	void)
{
	struct timespec now;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
