/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * keiland-system: a client of libkeiland's system (kl_system_*) for the
 * tests of WS131 p010.
 *
 *   keiland-system [--timeout-ms=N] COMMAND ...
 *
 * The commands run in order: "dump" (what the compositor offers and every
 * state), "scan", "join SSID", "disconnect", "wifi-on", "wifi-off",
 * "save-key SSID KEY", "details", "volume LEFT RIGHT MUTED", "feedback",
 * "power ACTION" (1 power off, 2 restart, 3 suspend), "eject ID",
 * "display-mode extended|mirror", "display-place KEY X Y" (the extended
 * mode with one display's place), "brightness KEY PERCENT" (ws113-p005;
 * each request waits up to the timeout for its result), "display-shown
 * KEY on|off" (ws113-p014: a display turned on or off), "display-rotate
 * KEY ANGLE" (ws217-p004: a display's picture turned by 0, 90, 180 or 270),
 * "displays" (the displays' snapshot, with each display's angle), "watch" (the
 * changes until the timeout) and "monitor" (the machine's monitor at
 * 250 ms until the timeout: its info and each frame, WS134 p012).  Each
 * line starts with KEILAND-SYSTEM:
 *
 *   KEILAND-SYSTEM open capabilities=0xB | failed step=open errno=E
 *   KEILAND-SYSTEM network reachable=... ssid=S        (and ap, link, dns, saved, audio, power, device)
 *   KEILAND-SYSTEM displays mode=extended|mirror count=N, then display key=K label=L x=X y=Y width=W
 *       height=H refresh_mhz=R flags=0xF brightness=B for each, and its rotation key=K rotation=R mask=M why=W
 *       (rotation unknown errno=E from a compositor before the angles, ws217-p004)
 *   KEILAND-SYSTEM result request=R error=E          (E: 0 or the errno's name)
 *   KEILAND-SYSTEM change bits=0xB
 *   KEILAND-SYSTEM monitor-info changes=N cpus=N host=H gpus=N disks=N links=N   (and monitor-disk, monitor-link, monitor-gpu)
 *   KEILAND-SYSTEM monitor-frame n=N seconds=S valid=0xB cpu=C ... | monitor failed errno=E
 *   KEILAND-SYSTEM done status=S
 */

#include <keiland/keiland.h>

#include <wayland-client.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The commands and how many words each takes after its name. */
struct probe_command {
	const char *name;
	int words;
};

/* Every command the probe knows. */
static const struct probe_command probe_commands[] = {
	{ "dump", 0 },
	{ "scan", 0 },
	{ "join", 1 },
	{ "disconnect", 0 },
	{ "wifi-on", 0 },
	{ "wifi-off", 0 },
	{ "save-key", 2 },
	{ "details", 0 },
	{ "volume", 3 },
	{ "feedback", 0 },
	{ "power", 1 },
	{ "eject", 1 },
	{ "display-mode", 1 },
	{ "display-place", 3 },
	{ "brightness", 2 },
	{ "display-shown", 2 },
	{ "display-rotate", 2 },
	{ "displays", 0 },
	{ "watch", 0 },
	{ "monitor", 0 },
};

static const struct probe_command *probe_find(const char *word);
static int probe_ask(struct kl_system *system, const char *name, char **words, uint32_t *request);
static void probe_dump(const struct kl_system *system);
static void probe_details(const struct kl_system *system);
static void probe_displays(const struct kl_system *system);
static int probe_display_ask(struct kl_system *system, const char *name, char **words, uint32_t *request);
static int probe_wait(struct wl_display *display, struct kl_system *system, uint32_t request, int timeout_ms);
static void probe_watch(struct wl_display *display, struct kl_system *system, int timeout_ms);
static void probe_monitor(struct wl_display *display, struct kl_system *system, int timeout_ms);
static void probe_monitor_info(const struct kl_system_monitor *monitor);
static int probe_round(struct wl_display *display, struct kl_system *system, int timeout_ms, unsigned *changed);
static unsigned long long probe_clock_ms(void);
static const char *probe_error(int error);

/*
 * Runs the commands on the command line against the desktop's system.
 *
 * Exits 0 when every command ran and every request was answered (whatever
 * its error), or 1.
 */
int
main(
	int argc,
	char **argv)
{
	const struct probe_command *command;
	struct wl_display *display;
	struct kl_system *system;
	uint32_t request;
	unsigned capabilities;
	int timeout_ms;
	int status;
	int error;
	int arg;
	int differs;

	/* Prints each line as it is made, so that a test reading the pipe sees it at once. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* The option; the commands follow it. */
	timeout_ms = 5000;
	arg = 1;
	if (arg < argc) {
		differs = strncmp(argv[arg], "--timeout-ms=", 13);
		if (differs == 0) {
			timeout_ms = atoi(argv[arg] + 13);
			arg++;
		}
	}

	/* Connects to the compositor. */
	display = wl_display_connect(NULL);
	if (display == NULL) {
		printf("KEILAND-SYSTEM failed step=connect errno=%d\n", errno);
		return 1;
	}

	/* Opens the system on it. */
	system = kl_system_open(display);
	if (system == NULL) {
		printf("KEILAND-SYSTEM failed step=open errno=%s\n", probe_error(errno));
		wl_display_disconnect(display);
		return 1;
	}

	/* The log line the tests read. */
	capabilities = kl_system_capabilities(system);
	printf("KEILAND-SYSTEM open capabilities=0x%x\n", capabilities);

	/* Runs each command in turn. */
	status = 0;
	while (arg < argc) {
		/* A word that is no command, or a command without its words, ends the run. */
		command = probe_find(argv[arg]);
		if (command == NULL || arg + command->words >= argc) {
			printf("KEILAND-SYSTEM failed step=usage word=%s\n", argv[arg]);
			status = 1;
			break;
		}

		/* The commands that ask nothing. */
		differs = strcmp(command->name, "dump");
		if (differs == 0) {
			probe_dump(system);
			arg++;
			continue;
		}

		/* The displays' snapshot. */
		differs = strcmp(command->name, "displays");
		if (differs == 0) {
			probe_displays(system);
			arg++;
			continue;
		}

		/* The watch. */
		differs = strcmp(command->name, "watch");
		if (differs == 0) {
			probe_watch(display, system, timeout_ms);
			arg++;
			continue;
		}

		/* The monitor. */
		differs = strcmp(command->name, "monitor");
		if (differs == 0) {
			probe_monitor(display, system, timeout_ms);
			arg++;
			continue;
		}

		/* A request, and its result. */
		error = probe_ask(system, command->name, argv + arg + 1, &request);
		if (error != 0) {
			printf("KEILAND-SYSTEM asked %s error=%s\n", command->name, probe_error(error));
		} else {
			printf("KEILAND-SYSTEM asked %s request=%u\n", command->name, request);
			error = probe_wait(display, system, request, timeout_ms);
			if (error != 0)
				status = 1;
		}

		/* The details come before their result. */
		differs = strcmp(command->name, "details");
		if (differs == 0)
			probe_details(system);
		arg += 1 + command->words;
	}

	/* The log line the tests read, then the system and the display go. */
	printf("KEILAND-SYSTEM done status=%d\n", status);
	kl_system_close(system);
	wl_display_disconnect(display);

	/* Reports a command that failed or was not answered. */
	if (status != 0)
		return 1;

	/* Succeeded: every command ran. */
	return 0;
}

/* Finds a command by its name. */
static const struct probe_command *
probe_find(
	const char *word)
{
	size_t index;
	int differs;

	/* Each command. */
	for (index = 0; index < sizeof(probe_commands) / sizeof(probe_commands[0]); index++) {
		differs = strcmp(probe_commands[index].name, word);
		if (differs == 0)
			return &probe_commands[index];
	}

	/* Not one. */
	return NULL;
}

/* Asks the system for a command's request. */
static int
probe_ask(
	struct kl_system *system,
	const char *name,
	char **words,
	uint32_t *request)
{
	int differs;

	/* The network's requests. */
	differs = strcmp(name, "scan");
	if (differs == 0)
		return kl_system_network_request(system, KL_NETWORK_SCAN, NULL, request);
	differs = strcmp(name, "join");
	if (differs == 0)
		return kl_system_network_request(system, KL_NETWORK_JOIN, words[0], request);
	differs = strcmp(name, "disconnect");
	if (differs == 0)
		return kl_system_network_request(system, KL_NETWORK_DISCONNECT, NULL, request);
	differs = strcmp(name, "wifi-on");
	if (differs == 0)
		return kl_system_network_request(system, KL_NETWORK_WIFI_ON, NULL, request);
	differs = strcmp(name, "wifi-off");
	if (differs == 0)
		return kl_system_network_request(system, KL_NETWORK_WIFI_OFF, NULL, request);
	differs = strcmp(name, "save-key");
	if (differs == 0)
		return kl_system_network_save_key(system, words[0], words[1], request);
	differs = strcmp(name, "details");
	if (differs == 0)
		return kl_system_network_query_details(system, request);

	/* The sound's. */
	differs = strcmp(name, "volume");
	if (differs == 0)
		return kl_system_audio_set_volume(system, (unsigned)atoi(words[0]), (unsigned)atoi(words[1]), (unsigned)atoi(words[2]), request);
	differs = strcmp(name, "feedback");
	if (differs == 0)
		return kl_system_audio_feedback(system, request);

	/* The power's. */
	differs = strcmp(name, "power");
	if (differs == 0)
		return kl_system_power_action(system, (unsigned)atoi(words[0]), request);

	/* The displays' (ws113-p005). */
	differs = strcmp(name, "eject");
	if (differs != 0)
		return probe_display_ask(system, name, words, request);

	/* The devices'. */
	return kl_system_devices_eject(system, words[0], request);
}

/* Asks the system for a displays' request: the mode, a place, a light (ws113-p005). */
static int
probe_display_ask(
	struct kl_system *system,
	const char *name,
	char **words,
	uint32_t *request)
{
	struct kl_display_place place;
	unsigned mode;
	unsigned shown;
	int differs;

	/* The mode alone. */
	differs = strcmp(name, "display-mode");
	if (differs == 0) {
		mode = KL_DISPLAYS_EXTENDED;
		differs = strcmp(words[0], "mirror");
		if (differs == 0)
			mode = KL_DISPLAYS_MIRROR;
		return kl_system_displays_apply(system, mode, NULL, 0U, request);
	}

	/* The extended mode with one display's place. */
	differs = strcmp(name, "display-place");
	if (differs == 0) {
		place.key = words[0];
		place.x = (int32_t)atol(words[1]);
		place.y = (int32_t)atol(words[2]);
		return kl_system_displays_apply(system, KL_DISPLAYS_EXTENDED, &place, 1U, request);
	}

	/* A display turned on or off (ws113-p014). */
	differs = strcmp(name, "display-shown");
	if (differs == 0) {
		differs = strcmp(words[1], "off");
		shown = (unsigned)(differs != 0);
		return kl_system_displays_set_shown(system, words[0], shown, request);
	}

	/* A display's picture turned (ws217-p004). */
	differs = strcmp(name, "display-rotate");
	if (differs == 0)
		return kl_system_displays_set_rotation(system, words[0], (unsigned)atoi(words[1]), request);

	/* A light. */
	return kl_system_displays_set_brightness(system, words[0], (unsigned)atoi(words[1]), request);
}

/* Prints the displays' snapshot (ws113-p005). */
static void
probe_displays(
	const struct kl_system *system)
{
	struct kl_display displays[KL_DISPLAYS_MAX];
	const char *mode;
	unsigned shown;
	unsigned rotation;
	unsigned mask;
	unsigned why;
	size_t count;
	size_t index;
	int error;

	/* The mode and the count. */
	count = kl_system_displays_get(system, displays, KL_DISPLAYS_MAX);
	shown = kl_system_displays_mode(system);
	mode = "extended";
	if (shown == KL_DISPLAYS_MIRROR)
		mode = "mirror";
	printf("KEILAND-SYSTEM displays mode=%s count=%u\n", mode, (unsigned)count);

	/* Each display. */
	for (index = 0; index < count; index++) {
		printf("KEILAND-SYSTEM display key=%s label=%s x=%ld y=%ld width=%u height=%u refresh_mhz=%u flags=0x%x brightness=%u\n",
		    displays[index].key,
		    displays[index].label,
		    (long)displays[index].x,
		    (long)displays[index].y,
		    displays[index].width,
		    displays[index].height,
		    displays[index].refresh_mhz,
		    displays[index].flags,
		    displays[index].brightness);

		/* Its angle (ws217-p004). */
		error = kl_system_displays_rotation(system, displays[index].key, &rotation);
		if (error == 0)
			error = kl_system_displays_rotations(system, displays[index].key, &mask, &why);
		if (error != 0) {
			printf("KEILAND-SYSTEM rotation key=%s unknown errno=%s\n", displays[index].key, probe_error(error));
			continue;
		}
		printf("KEILAND-SYSTEM rotation key=%s rotation=%u mask=%u why=%u\n", displays[index].key, rotation, mask, why);
	}
}

/* Prints every state the system has. */
static void
probe_dump(
	const struct kl_system *system)
{
	struct kl_network_state network;
	struct kl_network_ap aps[KL_NETWORK_SCAN_MAX];
	struct kl_audio_state audio;
	struct kl_power_state power;
	struct kl_device devices[KL_DEVICES_MAX];
	size_t count;
	size_t index;

	/* The network and its scan. */
	kl_system_network_get_state(system, &network);
	printf("KEILAND-SYSTEM network reachable=%u connected=%u kind=%u interface=%s wired=%s wifi=%u wifi_interface=%s ssid=%s\n",
	    network.reachable,
	    network.connected,
	    network.kind,
	    network.interface,
	    network.wired,
	    network.wifi,
	    network.wifi_interface,
	    network.ssid);
	count = kl_system_network_get_scan(system, aps, KL_NETWORK_SCAN_MAX);
	for (index = 0; index < count; index++)
		printf("KEILAND-SYSTEM ap ssid=%s rssi=%d secured=%u\n", aps[index].ssid, aps[index].rssi, aps[index].secured);

	/* The sound and the power. */
	kl_system_audio_get_state(system, &audio);
	printf("KEILAND-SYSTEM audio reachable=%u device=%u rate=%u channels=%u left=%u right=%u muted=%u\n", audio.reachable, audio.device, audio.rate, audio.channels, audio.left, audio.right, audio.muted);
	kl_system_power_get_state(system, &power);
	printf("KEILAND-SYSTEM power source=%u percent=%d charging=%u actions=0x%x\n", power.source, power.percent, power.charging, power.actions);

	/* The devices. */
	count = kl_system_devices_get(system, devices, KL_DEVICES_MAX);
	printf("KEILAND-SYSTEM devices count=%u\n", (unsigned)count);
}

/* Prints the details last asked for. */
static void
probe_details(
	const struct kl_system *system)
{
	struct kl_network_link links[KL_NETWORK_LINKS_MAX];
	char servers[KL_NETWORK_DNS_MAX][KL_NETWORK_ADDRESS_MAX];
	char saved[KL_NETWORK_SAVED_MAX][KL_NETWORK_SSID_MAX];
	size_t count;
	size_t index;

	/* The interfaces. */
	count = kl_system_network_get_links(system, links, KL_NETWORK_LINKS_MAX);
	for (index = 0; index < count; index++)
		printf("KEILAND-SYSTEM link name=%s up=%u running=%u loopback=%u address=%s netmask=%s hardware=%s mtu=%u\n", links[index].name, links[index].up, links[index].running, links[index].loopback, links[index].address, links[index].netmask, links[index].hardware, links[index].mtu);

	/* The DNS servers. */
	count = kl_system_network_get_dns(system, servers, KL_NETWORK_DNS_MAX);
	for (index = 0; index < count; index++)
		printf("KEILAND-SYSTEM dns address=%s\n", servers[index]);

	/* The saved networks. */
	count = kl_system_network_get_saved(system, saved, KL_NETWORK_SAVED_MAX);
	for (index = 0; index < count; index++)
		printf("KEILAND-SYSTEM saved ssid=%s\n", saved[index]);
}

/* Waits up to timeout_ms for a request's result and prints it; returns 0, or 1 when none came. */
static int
probe_wait(
	struct wl_display *display,
	struct kl_system *system,
	uint32_t request,
	int timeout_ms)
{
	unsigned long long until;
	unsigned long long now;
	unsigned changed;
	uint32_t finished;
	int answer;
	int taken;
	int error;

	/* Until the result comes, or the timeout. */
	until = probe_clock_ms() + (unsigned long long)timeout_ms;
	for (;;) {
		/* The results answered so far; another request's is printed and passed by. */
		taken = kl_system_take_result(system, &finished, &answer);
		if (taken) {
			printf("KEILAND-SYSTEM result request=%u error=%s\n", finished, probe_error(answer));
			if (finished == request)
				return 0;
			continue;
		}

		/* The timeout ends the wait. */
		now = probe_clock_ms();
		if (now >= until)
			break;

		/* Reads and dispatches for the time left; a display that went ends the wait. */
		error = probe_round(display, system, (int)(until - now), &changed);
		if (error != 0)
			break;
	}

	/* No answer came. */
	printf("KEILAND-SYSTEM result request=%u missing\n", request);
	return 1;
}

/* Prints the changes until the timeout, and the states after each. */
static void
probe_watch(
	struct wl_display *display,
	struct kl_system *system,
	int timeout_ms)
{
	unsigned long long until;
	unsigned long long now;
	unsigned changed;
	int error;

	/* Until the timeout. */
	until = probe_clock_ms() + (unsigned long long)timeout_ms;
	for (;;) {
		/* The timeout ends the watch. */
		now = probe_clock_ms();
		if (now >= until)
			break;

		/* Reads and dispatches; each change is printed with the states. */
		error = probe_round(display, system, (int)(until - now), &changed);
		if (error != 0)
			break;
		if (changed != 0U) {
			printf("KEILAND-SYSTEM change bits=0x%x\n", changed);
			probe_dump(system);
		}
	}
}

/* Follows the machine's monitor at 250 ms until the timeout: its info when it changes, and each frame. */
static void
probe_monitor(
	struct wl_display *display,
	struct kl_system *system,
	int timeout_ms)
{
	struct kl_system_monitor *monitor;
	struct kl_monitor_frame *frame;
	unsigned long long until;
	unsigned long long now;
	unsigned changes;
	unsigned shown;
	unsigned frames;
	unsigned changed;
	int taken;
	int error;

	/* The frame is large: on the heap. */
	frame = calloc(1, sizeof(*frame));
	if (frame == NULL) {
		printf("KEILAND-SYSTEM monitor failed errno=ENOMEM\n");
		return;
	}

	/* The monitor. */
	monitor = kl_system_monitor_open(system, 250U);
	if (monitor == NULL) {
		printf("KEILAND-SYSTEM monitor failed errno=%s\n", probe_error(errno));
		free(frame);
		return;
	}

	/* Until the timeout. */
	shown = 0;
	frames = 0;
	until = probe_clock_ms() + (unsigned long long)timeout_ms;
	for (;;) {
		/* The timeout ends the watch. */
		now = probe_clock_ms();
		if (now >= until)
			break;

		/* Reads and dispatches. */
		error = probe_round(display, system, (int)(until - now), &changed);
		if (error != 0)
			break;

		/* The info when it changed. */
		(void)kl_system_monitor_info(monitor, &changes);
		if (changes != shown) {
			shown = changes;
			probe_monitor_info(monitor);
		}

		/* A new frame. */
		taken = kl_system_monitor_take(monitor, frame);
		if (!taken)
			continue;
		frames++;
		printf("KEILAND-SYSTEM monitor-frame n=%u seconds=%.3f valid=0x%x cpu=%.3f cpus=%u core0=%.3f mem_total=%llu mem_free=%llu "
		       "cache=%llu swap_total=%llu rx=%.0f tx=%.0f read=%.0f write=%.0f latency_ms=%.3f disks=%u links=%u gpus=%u\n",
		       frames, frame->seconds, frame->valid, frame->cpu, frame->cpu_count, frame->cpu_core[0],
		       (unsigned long long)frame->memory_total, (unsigned long long)frame->memory_free,
		       (unsigned long long)frame->memory_cache, (unsigned long long)frame->swap_total, frame->rx_rate, frame->tx_rate,
		       frame->read_rate, frame->write_rate, frame->disk_latency_ms, frame->disk_count, frame->link_count, frame->gpu_count);
	}

	/* The monitor goes. */
	kl_system_monitor_close(monitor);
	(void)wl_display_flush(display);
	free(frame);
}

/* Prints the monitor's info and its devices. */
static void
probe_monitor_info(
	const struct kl_system_monitor *monitor)
{
	const struct kl_monitor_info *info;
	unsigned changes;
	unsigned index;

	/* The machine. */
	info = kl_system_monitor_info(monitor, &changes);
	printf("KEILAND-SYSTEM monitor-info changes=%u cpus=%u host=%s gpus=%u disks=%u links=%u\n", changes, info->cpu_count, info->host,
	       info->gpu_count, info->disk_count, info->link_count);

	/* Each disk. */
	for (index = 0; index < info->disk_count; index++)
		printf("KEILAND-SYSTEM monitor-disk name=%s kind=%u\n", info->disk[index].name, info->disk[index].kind);

	/* Each link. */
	for (index = 0; index < info->link_count; index++)
		printf("KEILAND-SYSTEM monitor-link name=%s\n", info->link[index].name);

	/* Each GPU. */
	for (index = 0; index < info->gpu_count; index++)
		printf("KEILAND-SYSTEM monitor-gpu name=%s driver=%s\n", info->gpu[index].name, info->gpu[index].driver);
}

/* Reads the display's events for up to timeout_ms, then dispatches the system; returns 0 or -1 when the display went. */
static int
probe_round(
	struct wl_display *display,
	struct kl_system *system,
	int timeout_ms,
	unsigned *changed)
{
	struct pollfd descriptor;
	int prepared;
	int status;

	/* Dispatches the events read before, until the display may be read. */
	for (;;) {
		prepared = wl_display_prepare_read(display);
		if (prepared == 0)
			break;
		(void)wl_display_dispatch_pending(display);
	}

	/* Sends what is queued, and waits for the display's events. */
	(void)wl_display_flush(display);
	descriptor.fd = wl_display_get_fd(display);
	descriptor.events = POLLIN;
	descriptor.revents = 0;
	status = poll(&descriptor, 1, timeout_ms);
	if (status > 0) {
		/* Reads the events that came. */
		status = wl_display_read_events(display);
	} else {
		/* Nothing came: the read is given up. */
		wl_display_cancel_read(display);
	}

	/* A wait or a read that failed means the display went. */
	if (status < 0)
		return -1;

	/* Dispatches the application's queue, then the system's. */
	(void)wl_display_dispatch_pending(display);
	status = kl_system_dispatch(system, changed);
	if (status != 0) {
		printf("KEILAND-SYSTEM dispatch error=%s\n", probe_error(status));
		return -1;
	}

	/* Succeeded: the round is over. */
	return 0;
}

/* Reads a steady clock in milliseconds. */
static unsigned long long
probe_clock_ms(
	void)
{
	struct timespec now;

	/* Reads the monotonic clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Reports it in milliseconds. */
	return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)now.tv_nsec / 1000000ULL;
}

/* Names an errno value the system gives, so that the tests read the same on every system. */
static const char *
probe_error(
	int error)
{
	/* Names each errno value the system gives. */
	switch (error) {
	case 0:
		return "0";
	case EINVAL:
		return "EINVAL";
	case EPERM:
		return "EPERM";
	case ENOTSUP:
		return "ENOTSUP";
	case EBUSY:
		return "EBUSY";
	case ENODEV:
		return "ENODEV";
	case EIO:
		return "EIO";
	case EPIPE:
		return "EPIPE";
	case ENOMEM:
		return "ENOMEM";
	case ESTALE:
		return "ESTALE";
	default:
		break;
	}

	/* Any other value is named alike. */
	return "other";
}
