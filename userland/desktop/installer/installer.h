/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The installer: a wizard of one window that puts the system on a disk of
 * this computer.  It runs as the live session's user and has no rights of
 * its own; what it learns of the disks and every write it asks for go
 * through its helper, install-admin, which it starts as a process and
 * talks to through its standard input and output, a line at a time (admin/plan.h, admin/probe.h,
 * admin/session.h).  The files share this header: main.c (the window,
 * its input and the log the tests read), helper.c (the helper's
 * processes and their lines), catalog.c (the index of what can be
 * installed and the choice of it), disk.c (the disks as the probe told
 * them, and the rules of what may be chosen), flow.c (the screens' order,
 * their checks and the requests) and screens.c (the drawing).
 */

#ifndef USERLAND_DESKTOP_INSTALLER_INSTALLER_H
#define USERLAND_DESKTOP_INSTALLER_INSTALLER_H

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* The helper, the index of what it installs, and the files of the image. */
#define INST_HELPER		"/usr/libexec/install-admin"
#ifndef INST_INDEX
#define INST_INDEX		"/opt/installer/index"
#endif
#define INST_OS_RELEASE		"/etc/os-release"
#define INST_PASSWD		"/etc/passwd"

/* The most disks, partitions, units, items and licences the installer keeps. */
#define INST_DISKS_MAX		32U
#define INST_PARTS_MAX		256U
#define INST_UNITS_MAX		128U
#define INST_ITEMS_MAX		192U
#define INST_LICENSES_MAX	32U
#define INST_ITEM_UNITS_MAX	8U
#define INST_DEPENDENCIES_MAX	32U

/* The longest names, labels, GUIDs and digests (with their NUL). */
#define INST_NAME_MAX		64U
#define INST_LABEL_MAX		128U
#define INST_PATH_MAX		256U
#define INST_GUID_MAX		40U
#define INST_DIGEST_MAX		72U
#define INST_DEVICE_MAX		40U

/* The longest line from the helper, a request to it, and a line of the screens' messages. */
#define INST_LINE_MAX		4096U
#define INST_REQUEST_MAX	12288U
#define INST_MESSAGE_MAX	256U

/* The changes S4 wrote, and the states the helper reported, that S10 lists. */
#define INST_WRITES_MAX		16U
#define INST_STATES_MAX		8U

/* The alignment of the partitions the installer makes, a new ESP, and the smallest swap partition. */
#define INST_MIB		(1024ULL * 1024ULL)
#define INST_GIB		(1024ULL * INST_MIB)
#define INST_ESP_BYTES		(512ULL * INST_MIB)
#define INST_SWAP_MIN_BYTES	(64ULL * INST_MIB)

/* The room an existing ESP needs besides the loader and its configuration. */
#define INST_ESP_SPARE		(16ULL * INST_MIB)

/* The largest swap file (mkswap's file mode) and the spare room a root keeps. */
#define INST_SWAPFILE_MAX	((2ULL << 30) - 4096ULL)
#define INST_ROOT_SPARE		INST_GIB

/* The screens, in the order of the wizard (S7a between S7 and S8). */
enum inst_screen {
	INST_S1 = 0,
	INST_S2,
	INST_S3,
	INST_S4,
	INST_S5,
	INST_S6,
	INST_S7,
	INST_S7A,
	INST_S8,
	INST_S9,
	INST_S10,
	INST_S11,
	INST_SCREENS
};

/* The dialogs a screen may show over itself. */
enum inst_dialog {
	INST_DIALOG_NONE = 0,
	INST_DIALOG_CANCEL,
	INST_DIALOG_EDIT_NOTICE,
	INST_DIALOG_DELETE,
	INST_DIALOG_ERASE_ROOT,
	INST_DIALOG_ERASE_SWAP,
	INST_DIALOG_NO_SWAP,
	INST_DIALOG_DROP_DESKTOP,
	INST_DIALOG_DROP_FIRMWARE,
	INST_DIALOG_START,
	INST_DIALOG_STOP
};

/* What the helper says of a file a FAT may hold: no, yes, or unknown (taken as yes). */
enum inst_answer {
	INST_NO = 0,
	INST_YES,
	INST_UNKNOWN
};

/* What a request to the helper is waiting for. */
enum inst_job {
	INST_JOB_NONE = 0,
	INST_JOB_PROBE,
	INST_JOB_EDIT,
	INST_JOB_INSTALL,
	INST_JOB_CANCEL
};

/*
 * One partition, as the probe told it: its disk's registration number,
 * its slot, its sectors, type, PARTUUID and attributes; the device the
 * kernel made of it and what it holds; and, for a FAT or an ESP, its
 * free room and the files that make it another system's.
 */
struct inst_part {
	uint32_t disk;
	unsigned slot;
	uint64_t start;
	uint64_t sectors;
	char type[INST_GUID_MAX];
	char partuuid[INST_GUID_MAX];
	uint64_t attributes;
	char device[INST_DEVICE_MAX];
	char fstype[INST_NAME_MAX];
	char fsuuid[INST_NAME_MAX];
	int mounted;
	int swap;
	char name[INST_LABEL_MAX];
	char fslabel[INST_LABEL_MAX];
	int fat;
	uint64_t fat_free;
	enum inst_answer cfg;
	enum inst_answer boot_loader;
	enum inst_answer zedbsd_loader;
};

/*
 * One disk, as the probe told it, with the partitions that follow its
 * record in the probe's list.
 */
struct inst_disk {
	char name[INST_DEVICE_MAX];
	uint32_t registration;
	char kind[16];
	uint32_t sector_bytes;
	uint64_t sectors;
	unsigned flags;
	int live;
	char table[8];
	unsigned restrictions;
	unsigned entries;
	char digest[INST_DIGEST_MAX];
	unsigned first_part;
	unsigned part_count;
};

/* What one probe found: the firmware, the memory, the disks and their partitions. */
struct inst_probe {
	int uefi;
	uint64_t memory;
	struct inst_disk disk[INST_DISKS_MAX];
	unsigned disk_count;
	struct inst_part part[INST_PARTS_MAX];
	unsigned part_count;
};

/* One unit of the index: its name, label, group, size and what it needs. */
struct inst_unit {
	char name[INST_NAME_MAX];
	char label[INST_LABEL_MAX];
	char group[16];
	uint64_t bytes;
	uint64_t entries;
	unsigned dependency[INST_DEPENDENCIES_MAX];
	unsigned dependency_count;
};

/*
 * One item of the page of what to install: its id, the item it is under
 * (-1 at the top), its default (0 off, 1 on, 2 required), its label, the
 * units it installs (none for a heading), and whether the person chose it.
 */
struct inst_item {
	char id[INST_NAME_MAX * 2U];
	int parent;
	int fallback;
	char label[INST_LABEL_MAX];
	unsigned unit[INST_ITEM_UNITS_MAX];
	unsigned unit_count;
	int chosen;
};

/* One licence file of the firmware: its package and its path in the live system. */
struct inst_license {
	char package[INST_NAME_MAX];
	char path[INST_PATH_MAX];
};

/*
 * The index of what can be installed (/opt/installer/index), read once at
 * the start, and the units the items chosen install with what they need.
 */
struct inst_catalog {
	int read;
	int error;
	struct inst_unit unit[INST_UNITS_MAX];
	unsigned unit_count;
	struct inst_item item[INST_ITEMS_MAX];
	unsigned item_count;
	struct inst_license license[INST_LICENSES_MAX];
	unsigned license_count;
	uint64_t loader_bytes;
	int closure[INST_UNITS_MAX];
	uint64_t chosen_bytes;
	uint64_t chosen_entries;
};

/*
 * A partition named by what cannot be mistaken for another one's: its
 * disk's registration number, its first sector, its size and its
 * PARTUUID (admin/plan.h's tuple).
 */
struct inst_tuple {
	uint32_t disk;
	uint64_t start;
	uint64_t sectors;
	char partuuid[INST_GUID_MAX];
};

/*
 * The helper's processes: a probe that answers and ends, and the session
 * that lives from the first write until the installer ends it (its input
 * open until then), bound to a disk.  One request is answered at a time;
 * the line being read, and whether the rest of a line too long is dropped.
 */
struct inst_helper {
	struct kl_process *probe;
	struct kl_process *session;
	int session_open;
	uint32_t session_disk;
	enum inst_job job;
	char line[INST_LINE_MAX];
	int line_long;
};

/* The place of a control the log names for the tests, as the last frame drew it. */
struct inst_logged {
	char name[32];
	struct kl_rect rect;
	int enabled;
};

/* The most controls one frame logs. */
#define INST_LOGGED_MAX		96U

/*
 * The installer: its window and what it draws with, the helper and what
 * it told, the catalog, every choice of the screens, and the progress and
 * the end of an installation.  One lives for the program's run.
 */
struct inst_app {
	/* The application, its window, its input, the desktop's settings and system. */
	struct kl_app *app;
	struct kl_window *window;
	struct kl_ui *ui;
	struct kl_settings *language;
	struct kl_system *system;

	/* The frame and what it is drawn with. */
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	struct kl_canvas canvas;
	int canvas_made;
	struct kl_text text;
	struct kl_style style;
	char os_name[INST_NAME_MAX];

	/* A frame is due, something moves, the window changed size, closed or was asked to quit. */
	int dirty;
	int moving;
	int resized;
	int closed;
	int quit;

	/* The screen shown, the dialog over it, and the scroll of its content. */
	enum inst_screen screen;
	enum inst_dialog dialog;
	long dialog_part;
	struct kl_scroll scroll;
	struct kl_rect content;
	int content_height;

	/* The helper, the probe it gave last, and the probe's state (0 none, 1 asked, 2 read, 3 failed). */
	struct inst_helper helper;
	struct inst_probe probe;
	struct inst_probe incoming;
	int probe_state;
	char probe_message[INST_MESSAGE_MAX];

	/* The catalog. */
	struct inst_catalog catalog;

	/* S2: the disk chosen (its registration number, 0 for none). */
	uint32_t disk;

	/* S3: the whole disk (1), partitions chosen (0), or nothing yet (-1). */
	int whole;

	/* S4: the notice shown, the partition selected, the size asked, what the last write said, a reload that failed. */
	int edit_notice_shown;
	struct inst_tuple selected;
	int has_selected;
	struct kl_field create_size;
	char edit_message[INST_MESSAGE_MAX];
	int edit_failed;
	int reload_failed;

	/* S5 and S6: the root and swap partitions, swap none (S6's explicit choice), the erasures agreed. */
	struct inst_tuple root;
	int has_root;
	struct inst_tuple swap;
	int has_swap;
	int swap_none;
	struct inst_tuple pending;

	/* S7: the firmware's licences agreed, the other packages shown, and the licence viewer. */
	int license_agreed;
	int other_open;
	int license_view;
	unsigned license_index;
	char *license_text;
	size_t license_length;
	size_t license_offset;
	size_t license_page_end;
	size_t license_history[64];
	unsigned license_depth;
	int pending_item;

	/* S7a: the user, the passwords, the automatic login and the hostname. */
	struct kl_field user;
	struct kl_field fullname;
	struct kl_field password;
	struct kl_field password_again;
	struct kl_field root_password;
	struct kl_field root_password_again;
	struct kl_field hostname;
	int autologin;

	/* S8: the erasure of the whole disk agreed. */
	int erase_agreed;

	/* S9: the step, the bytes unpacked, the unit, when it started, and the cancellation asked. */
	unsigned step;
	unsigned steps;
	char step_name[32];
	uint64_t bytes_done;
	uint64_t bytes_total;
	char unit_name[INST_NAME_MAX];
	uint64_t started_us;
	uint64_t ended_us;
	int stop_asked;

	/*
	 * S10 and S11: what was written, the failure, the helper's states, the
	 * loader's place, the firmware's boot entry ("none" when none was made)
	 * and the swap.
	 */
	char writes[INST_WRITES_MAX][INST_MESSAGE_MAX];
	unsigned write_count;
	int canceled;
	char fail_step[32];
	int fail_errno;
	char fail_text[INST_MESSAGE_MAX];
	char states[INST_STATES_MAX][INST_MESSAGE_MAX];
	unsigned state_count;
	char loader_path[INST_PATH_MAX];
	char boot_entry[8];
	char swap_kind[16];
	char installed_user[INST_NAME_MAX];
	char power_message[INST_MESSAGE_MAX];

	/* The controls the log named last, and those of the frame being drawn. */
	struct inst_logged logged[INST_LOGGED_MAX];
	unsigned logged_count;
	struct inst_logged drawn[INST_LOGGED_MAX];
	unsigned drawn_count;
	enum inst_screen logged_screen;
	enum inst_dialog logged_dialog;
	int logged_once;
};

/* main.c */
void inst_log(const char *format, ...);
void inst_control(struct inst_app *app, const char *name, const struct kl_rect *rect, int enabled);
void inst_redraw(struct inst_app *app);

/* helper.c */
int inst_helper_probe(struct inst_app *app);
int inst_helper_request(struct inst_app *app, uint32_t disk, enum inst_job job, const char *request);
void inst_helper_end_session(struct inst_app *app);
void inst_helper_ready(struct inst_app *app, struct kl_process *process, unsigned ready);
int inst_helper_busy(const struct inst_app *app);

/* catalog.c */
int inst_catalog_read(struct inst_catalog *catalog);
void inst_catalog_choose(struct inst_catalog *catalog);
int inst_item_by_dependency(const struct inst_catalog *catalog, unsigned item);
int inst_item_find(const struct inst_catalog *catalog, const char *id);
int inst_catalog_unit_find(const struct inst_catalog *catalog, const char *name);
int inst_catalog_has(const struct inst_catalog *catalog, const char *unit);
int inst_license_load(struct inst_app *app, unsigned index);
void inst_license_close(struct inst_app *app);

/* disk.c */
int inst_probe_line(struct inst_probe *probe, char *line);
const struct inst_disk *inst_disk_find(const struct inst_probe *probe, uint32_t registration);
const struct inst_part *inst_part_find(const struct inst_probe *probe, const struct inst_tuple *tuple);
void inst_tuple_of(const struct inst_part *part, struct inst_tuple *tuple);
int inst_tuple_same(const struct inst_tuple *left, const struct inst_tuple *right);
const char *inst_disk_refusal(const struct inst_app *app, const struct inst_disk *disk);
const char *inst_chosen_refusal(const struct inst_app *app, const struct inst_disk *disk);
const struct inst_part *inst_disk_esp(const struct inst_app *app, const struct inst_disk *disk);
int inst_part_esp(const struct inst_part *part);
int inst_part_unassigned(const struct inst_part *part);
const char *inst_type_name(const char *type);
void inst_bytes_text(uint64_t bytes, char *text, size_t size);
uint64_t inst_part_bytes(const struct inst_disk *disk, const struct inst_part *part);
uint64_t inst_disk_bytes(const struct inst_disk *disk);
int inst_largest_gap(const struct inst_disk *disk, const struct inst_probe *probe, uint64_t *start, uint64_t *sectors);
int inst_place(const struct inst_disk *disk, const struct inst_probe *probe, uint64_t bytes, int with_esp, uint64_t *esp_start, uint64_t *start, uint64_t *sectors);
uint64_t inst_whole_swap_bytes(const struct inst_app *app, const struct inst_disk *disk);
uint64_t inst_swapfile_bytes(const struct inst_app *app);
uint64_t inst_root_needed(const struct inst_app *app, int base_only);
int inst_size_parse(const char *text, uint64_t *bytes);

/* flow.c */
void inst_flow_start(struct inst_app *app);
void inst_flow_line(struct inst_app *app, enum inst_job job, char *line);
void inst_flow_ended(struct inst_app *app, enum inst_job job, int status);
const char *inst_next_refusal(struct inst_app *app);
void inst_next(struct inst_app *app);
void inst_back(struct inst_app *app);
void inst_show(struct inst_app *app, enum inst_screen screen);
void inst_cancel_asked(struct inst_app *app);
void inst_dialog_answer(struct inst_app *app, int answer);
void inst_edit_delete(struct inst_app *app);
void inst_edit_create(struct inst_app *app);
void inst_choose_root(struct inst_app *app, const struct inst_part *part);
void inst_choose_swap(struct inst_app *app, const struct inst_part *part);
void inst_item_toggle(struct inst_app *app, unsigned item);
void inst_start_over(struct inst_app *app);
void inst_probe_again(struct inst_app *app);
void inst_reboot(struct inst_app *app);
const char *inst_screen_word(enum inst_screen screen);
int inst_user_refusal(const struct inst_app *app, const char **reason);

/* screens.c */
void inst_draw(struct inst_app *app, uint64_t now_us);

#endif
