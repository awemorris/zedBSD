/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inside of libkeiland's system (system.c; WS131 p010, plan/ws131/
 * design.md section 4): the wire's interfaces, which the settings share
 * (system-protocol.c), and the state an application sees, kept as one
 * state at a time (system-view.c).  The view knows nothing of Wayland, so
 * that the host tests build it alone (plan/ws131/tests/host-system.sh).
 */

#ifndef KEILAND_SYSTEM_PRIVATE_H
#define KEILAND_SYSTEM_PRIVATE_H

#include <keiland/keiland.h>

#include "userland/desktop/libkeiland/system/kl-system-protocol.h"

#include <stddef.h>
#include <stdint.h>

/* The most answered requests kept until they are taken. */
#define SYSTEM_VIEW_RESULTS	32U

/* How many notification events wait for kl_system_take_notify_event (ws156-p002). */
#define SYSTEM_VIEW_NOTIFY_EVENTS	32U

/* How many arrivals of mail wait for kl_system_take_mail_event (ws169-p002). */
#define SYSTEM_VIEW_MAIL_EVENTS	8U

/* How many phone events wait for kl_system_take_phone_event (ws170-p004). */
#define SYSTEM_VIEW_PHONE_EVENTS	16U

/* The parts of the computer's answer (ws188-p002): about, the file systems, the users, the login language, the mounts (ws188-p004). */
#define SYSTEM_VIEW_MACHINE_PARTS	5U

/* How many prints' jobs are kept for kl_system_print_job_of (ws145-p003), as many as the results. */
#define SYSTEM_VIEW_PRINT_QUEUED	SYSTEM_VIEW_RESULTS

/* A print's request and its job (queued). */
struct system_view_queued {
	uint32_t request;
	uint32_t job;
};

/* One answered request and its error. */
struct system_view_result {
	uint32_t request;
	int error;
};

/*
 * The system as an application sees it.
 *
 * Each part has the state in effect and the one the compositor is sending
 * (pending), put into effect at its done; touched says the pending one has
 * something.  A list (the scan, the details, the devices) is sent whole:
 * open says its pending list was started since its end, so that the next
 * item starts a new list.  The details have no done: their end puts them
 * into effect.
 *
 * changed has the KL_SYSTEM_CHANGED_* bits since the last take; results is
 * a ring of answered requests, result_head the oldest, result_count how
 * many wait (the oldest is dropped when it is full).  The notification
 * events, the arrivals of mail and the phone's events are rings of the
 * same kind.  The printers and the print jobs are lists sent whole before
 * their done; queued keeps the jobs of the last prints asked (a ring).
 */
struct system_view {
	unsigned capabilities;
	struct kl_network_state network;
	struct kl_network_state network_pending;
	unsigned network_touched;
	struct kl_network_ap scan[KL_NETWORK_SCAN_MAX];
	size_t scan_count;
	struct kl_network_ap scan_pending[KL_NETWORK_SCAN_MAX];
	size_t scan_pending_count;
	unsigned scan_open;
	unsigned scan_touched;
	struct kl_network_link links[KL_NETWORK_LINKS_MAX];
	size_t link_count;
	char dns[KL_NETWORK_DNS_MAX][KL_NETWORK_ADDRESS_MAX];
	size_t dns_count;
	char saved[KL_NETWORK_SAVED_MAX][KL_NETWORK_SSID_MAX];
	size_t saved_count;
	struct kl_network_link links_pending[KL_NETWORK_LINKS_MAX];
	size_t links_pending_count;
	char dns_pending[KL_NETWORK_DNS_MAX][KL_NETWORK_ADDRESS_MAX];
	size_t dns_pending_count;
	char saved_pending[KL_NETWORK_SAVED_MAX][KL_NETWORK_SSID_MAX];
	size_t saved_pending_count;
	unsigned details_open;
	struct kl_audio_state audio;
	struct kl_audio_state audio_pending;
	unsigned audio_touched;
	struct kl_power_state power;
	struct kl_power_state power_pending;
	unsigned power_touched;
	struct kl_device devices[KL_DEVICES_MAX];
	size_t device_count;
	struct kl_device devices_pending[KL_DEVICES_MAX];
	size_t devices_pending_count;
	struct kl_device_info device_infos[KL_DEVICES_MAX];
	struct kl_device_info device_infos_pending[KL_DEVICES_MAX];
	unsigned devices_open;
	uint32_t busy_request;
	char busy_program[KL_DEVICE_TEXT_MAX];
	uint32_t refused_request;
	char refused_reason[KL_SYSTEM_REASON_MAX + 1U];
	unsigned enrolled_known;
	unsigned enrolled_pin;
	unsigned enrolled_keys;
	struct kl_system_key keys[KL_SYSTEM_KEYS_MAX];
	size_t key_count;
	struct kl_system_key keys_pending[KL_SYSTEM_KEYS_MAX];
	size_t keys_pending_count;
	unsigned touched;
	uint32_t touched_request;
	struct kl_sharing_state sharing;
	struct kl_sharing_state sharing_pending;
	unsigned sharing_touched;
	unsigned changed;
	struct system_view_result results[SYSTEM_VIEW_RESULTS];
	unsigned result_head;
	unsigned result_count;
	struct kl_notify_event notify_events[SYSTEM_VIEW_NOTIFY_EVENTS];
	unsigned notify_head;
	unsigned notify_count;
	/* The notification events a full ring dropped since the last take (told as one KL_NOTIFY_LOST, ws177-p005). */
	unsigned notify_lost;
	struct kl_mail_event mail_events[SYSTEM_VIEW_MAIL_EVENTS];
	unsigned mail_head;
	unsigned mail_count;
	/* Whether this reader is allowed to hear the arrivals (0 not told, 1 not allowed, 2 allowed; ws177-p005). */
	unsigned mail_allowed;
	struct kl_phone_event phone_events[SYSTEM_VIEW_PHONE_EVENTS];
	unsigned phone_head;
	unsigned phone_count;
	struct kl_printer printers[KL_PRINTERS_MAX];
	size_t printer_count;
	struct kl_printer printers_pending[KL_PRINTERS_MAX];
	size_t printers_pending_count;
	struct kl_print_job print_jobs[KL_PRINT_JOBS_MAX];
	size_t print_job_count;
	struct kl_print_job print_jobs_pending[KL_PRINT_JOBS_MAX];
	size_t print_jobs_pending_count;
	unsigned printers_open;
	struct system_view_queued queued[SYSTEM_VIEW_PRINT_QUEUED];
	unsigned queued_next;
	/*
	 * The displays (ws113-p005): the snapshot in effect and the one being
	 * sent (open since its first output), its serial and the mode.
	 */
	struct kl_display displays[KL_DISPLAYS_MAX];
	size_t display_count;
	struct kl_display displays_pending[KL_DISPLAYS_MAX];
	size_t displays_pending_count;
	unsigned displays_open;
	uint32_t displays_serial;
	unsigned displays_mode;
	/*
	 * What Settings reads of the computer (ws188-p002): the parts in
	 * effect, which of them were ever answered (machine_known, the
	 * KL_MACHINE_* bits) and how often each was put into effect
	 * (machine_serials, by the part's bit's place); and the answer being
	 * received: open from its parts event until its result, for request,
	 * holding the parts it named, whose events fill the pending copies.
	 */
	struct kl_machine_about machine_about;
	struct kl_machine_filesystem machine_filesystems[KL_MACHINE_FILESYSTEMS_MAX];
	size_t machine_filesystem_count;
	struct kl_machine_user machine_users[KL_MACHINE_USERS_MAX];
	size_t machine_user_count;
	char machine_language[KL_SYSTEM_MACHINE_CODE_MAX];
	unsigned machine_known;
	uint32_t machine_serials[SYSTEM_VIEW_MACHINE_PARTS];
	unsigned machine_open;
	uint32_t machine_request;
	unsigned machine_parts;
	struct kl_machine_about machine_about_pending;
	struct kl_machine_filesystem machine_filesystems_pending[KL_MACHINE_FILESYSTEMS_MAX];
	size_t machine_filesystems_pending_count;
	struct kl_machine_user machine_users_pending[KL_MACHINE_USERS_MAX];
	size_t machine_users_pending_count;
	char machine_language_pending[KL_SYSTEM_MACHINE_CODE_MAX];
	struct kl_machine_mount machine_mounts[KL_MACHINE_MOUNTS_MAX];
	size_t machine_mount_count;
	struct kl_machine_mount machine_mounts_pending[KL_MACHINE_MOUNTS_MAX];
	size_t machine_mounts_pending_count;
};

/*
 * One sample of the machine as the compositor tells it (WS134 p012): the
 * counters and present values of kl_system_monitor_v1's events, before
 * they are made into a frame's rates (system-monitor-rate.c, which knows
 * nothing of Wayland so that the host tests build it alone).
 */
struct system_monitor_cpu {
	uint64_t user;
	uint64_t system;
	uint64_t idle;
	uint64_t other;
};

/* One link's counters in a raw sample. */
struct system_monitor_link {
	uint64_t id;
	uint64_t rx;
	uint64_t tx;
	unsigned up;
};

/* One disk's counters in a raw sample. */
struct system_monitor_disk {
	uint64_t id;
	uint64_t read_ops;
	uint64_t write_ops;
	uint64_t read_bytes;
	uint64_t write_bytes;
	uint64_t read_ns;
	uint64_t write_ns;
	uint64_t busy_ns;
};

/* One GPU's counters and present values in a raw sample. */
struct system_monitor_gpu {
	uint64_t id;
	uint64_t time_ns;
	uint64_t busy_ns;
	uint64_t memory_used;
	uint64_t memory_total;
	unsigned cur_mhz;
	unsigned max_mhz;
	int milli_celsius;
	unsigned milli_watts;
};

/* A raw sample as a whole (valid has the KL_MONITOR_FRAME_* bits the compositor sent). */
struct system_monitor_raw {
	uint64_t time_ns;
	unsigned valid;
	unsigned cpu_hz;
	int cpu_milli_celsius;
	unsigned cpu_count;
	struct system_monitor_cpu cpu[KL_MONITOR_CPU_MAX];
	uint64_t memory_total;
	uint64_t memory_free;
	uint64_t memory_cache;
	uint64_t memory_reclaimable;
	uint64_t swap_total;
	uint64_t swap_used;
	unsigned link_count;
	struct system_monitor_link link[KL_MONITOR_LINK_MAX];
	unsigned disk_count;
	struct system_monitor_disk disk[KL_MONITOR_DISK_MAX];
	unsigned gpu_count;
	struct system_monitor_gpu gpu[KL_MONITOR_GPU_MAX];
};

/* system-monitor-rate.c */
void system_monitor_rates(const struct system_monitor_raw *previous, const struct system_monitor_raw *current, struct kl_monitor_frame *frame);

/* system.c, for system-monitor.c */
struct wl_proxy;
struct wl_proxy *system_monitor_make(struct kl_system *system, uint32_t period_ms, const void *listener, void *data);

/* system-view.c */
void system_view_init(struct system_view *view);
void system_view_network_state(struct system_view *view, const struct kl_network_state *state);
void system_view_access_point(struct system_view *view, const struct kl_network_ap *ap);
void system_view_scan_done(struct system_view *view);
void system_view_network_done(struct system_view *view);
void system_view_link(struct system_view *view, const struct kl_network_link *link);
void system_view_wired(struct system_view *view, const char *name, unsigned mode, const char *router);
void system_view_link_speed(struct system_view *view, const char *name, unsigned mbps);
void system_view_dns(struct system_view *view, const char *address);
void system_view_saved(struct system_view *view, const char *ssid);
void system_view_details_done(struct system_view *view);
void system_view_audio_state(struct system_view *view, const struct kl_audio_state *state);
void system_view_audio_done(struct system_view *view);
void system_view_power_state(struct system_view *view, const struct kl_power_state *state);
void system_view_sharing_state(struct system_view *view, const struct kl_sharing_state *state);
void system_view_sharing_done(struct system_view *view);
void system_view_power_done(struct system_view *view);
void system_view_device(struct system_view *view, const struct kl_device *device);
void system_view_devices_done(struct system_view *view);
void system_view_device_info(struct system_view *view, const char *id, const char *fs, uint64_t bytes);
void system_view_result(struct system_view *view, uint32_t request, uint32_t applied);
int system_view_take_result(struct system_view *view, uint32_t *request, int *error);
void system_view_notify_event(struct system_view *view, const struct kl_notify_event *event);
int system_view_take_notify_event(struct system_view *view, struct kl_notify_event *event);
void system_view_mail_event(struct system_view *view, const char *from, const char *subject, const char *code);
int system_view_take_mail_event(struct system_view *view, struct kl_mail_event *event);
void system_view_mail_allowed(struct system_view *view, unsigned on);
void system_view_phone_event(struct system_view *view, const struct kl_phone_event *event);
int system_view_take_phone_event(struct system_view *view, struct kl_phone_event *event);
void system_view_printer(struct system_view *view, const struct kl_printer *printer);
void system_view_print_job(struct system_view *view, const struct kl_print_job *job);
void system_view_printers_done(struct system_view *view);
void system_view_print_queued(struct system_view *view, uint32_t request, uint32_t job);
void system_view_display(struct system_view *view, const struct kl_display *display);
void system_view_displays_done(struct system_view *view, uint32_t serial, uint32_t mode);
void system_view_machine_parts(struct system_view *view, uint32_t request, uint32_t what);
void system_view_machine_about(struct system_view *view, const struct kl_machine_about *about);
void system_view_machine_filesystem(struct system_view *view, const struct kl_machine_filesystem *filesystem);
void system_view_machine_user(struct system_view *view, const struct kl_machine_user *user);
void system_view_machine_login_language(struct system_view *view, const char *code);
void system_view_machine_mount(struct system_view *view, const struct kl_machine_mount *mount);
void system_view_machine_result(struct system_view *view, uint32_t request, uint32_t applied);
int system_view_print_job_of(const struct system_view *view, uint32_t request, uint32_t *job);
unsigned system_view_take_changed(struct system_view *view);
int system_view_error_of(uint32_t applied);
void system_view_copy(char *to, size_t size, const char *from);

#endif
