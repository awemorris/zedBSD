/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The wizard's flow: which screen follows which, what each screen needs
 * before Next is offered (and the one line that says why not), the
 * dialogs that confirm what cannot be undone, and the requests to the
 * helper with what their answers change.  S4's changes are written when
 * their buttons are pressed; everything else is written after S8, by one
 * install request.  The helper's lines arrive here (inst_flow_line) with
 * the request they answer.
 */

#include "installer.h"

#include "include/kern/os-name.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The longest login name, display name, password and hostname (admin/plan.h). */
#define FLOW_USER_MAX		32U
#define FLOW_FULLNAME_MAX	64U
#define FLOW_PASSWORD_MAX	256U
#define FLOW_HOSTNAME_MAX	63U

/* The uid of the image's default user, whom the installation's user replaces. */
#define FLOW_DEFAULT_UID	1000L

/* The words of the screens, for the log and the tests. */
static const char *const flow_screen_words[INST_SCREENS] = {
	"s1-welcome", "s2-disk", "s3-handling", "s4-edit", "s5-root", "s6-swap", "s7-items", "s7a-user",
	"s8-confirm", "s9-progress", "s10-failed", "s11-done"
};

static void flow_probe(struct inst_app *app);
static void flow_probe_line(struct inst_app *app, char *line);
static void flow_probe_read(struct inst_app *app);
static void flow_edit_line(struct inst_app *app, char *line);
static void flow_install_line(struct inst_app *app, char *line);
static void flow_failed(struct inst_app *app, const char *step, int error, const char *text);
static const struct inst_disk *flow_disk(const struct inst_app *app);
static uint64_t flow_root_bytes(const struct inst_app *app);
static int flow_disk_text(const struct inst_app *app, const struct inst_disk *disk, char *text, size_t size);
static void flow_tuple_text(const struct inst_tuple *tuple, char *text, size_t size);
static int flow_field(char *text, size_t size, size_t *used, const char *word, const char *value);
static int flow_append(char *text, size_t size, size_t *used, const char *format, ...);
static void flow_send_edit(struct inst_app *app, const char *request, const char *what);
static void flow_install(struct inst_app *app);
static void flow_record_write(struct inst_app *app, const char *what);
static void flow_forget_choices(struct inst_app *app);
static void flow_forget_passwords(struct inst_app *app);
static int flow_name_valid(const char *name);
static int flow_text_valid(const char *text, size_t most, int colon);
static int flow_hostname_valid(const char *name);
static int flow_system_user(const char *name);

/*
 * The edit's write being answered, described for S10's list (the
 * request's own words).  It is set when a write is sent and kept until
 * the write's answer ends; one write is in flight at a time.
 */
static char flow_pending_write[INST_MESSAGE_MAX];

/* Whether the write being answered changed the table already (its STATE table line came). */
static int flow_pending_wrote;

/*
 * Starts the wizard: the catalog, the system's name, the fields' first
 * values and the first probe, on S1.
 */
void
inst_flow_start(
	struct inst_app *app)
{
	FILE *release;
	char line[256];
	char *value;
	char *text;
	size_t length;
	int error;
	int same;

	/* The catalog of what can be installed. */
	error = inst_catalog_read(&app->catalog);
	inst_log("CATALOG error=%d units=%u items=%u licenses=%u", error, app->catalog.unit_count, app->catalog.item_count,
		 app->catalog.license_count);

	/* The system's name, from os-release's NAME (the build's when it has none). */
	(void)snprintf(app->os_name, sizeof(app->os_name), "%s", ZEDBSD_OS_NAME);
	release = fopen(INST_OS_RELEASE, "r");
	if (release != NULL) {
		for (;;) {
			/* Reads the next line of the file. */
			text = fgets(line, sizeof(line), release);
			if (text == NULL)
				break;

			/* Only the NAME line matters. */
			same = strncmp(line, "NAME=", 5U);
			if (same != 0)
				continue;

			/* Takes the name between its quotes. */
			value = line + 5;
			if (*value == '"')
				value++;
			length = strcspn(value, "\"\n");
			value[length] = '\0';
			if (length > 0U)
				(void)snprintf(app->os_name, sizeof(app->os_name), "%s", value);
		}

		/* The file is read to its end. */
		(void)fclose(release);
	}

	/* The choices at their start. */
	app->whole = -1;
	app->autologin = 1;
	kl_field_set(&app->hostname, "zedbsd");
	kl_field_set_limit(&app->user, FLOW_USER_MAX);
	kl_field_set_limit(&app->fullname, FLOW_FULLNAME_MAX);
	kl_field_set_limit(&app->hostname, FLOW_HOSTNAME_MAX);
	app->password.secret = 1;
	app->password_again.secret = 1;
	app->root_password.secret = 1;
	app->root_password_again.secret = 1;

	/* The first screen, and the disks asked for. */
	inst_show(app, INST_S1);
	flow_probe(app);
}

/* Takes a line of the helper's answer to a request. */
void
inst_flow_line(
	struct inst_app *app,
	enum inst_job job,
	char *line)
{
	/* By the request it answers. */
	switch (job) {
	case INST_JOB_PROBE:
		flow_probe_line(app, line);
		break;
	case INST_JOB_EDIT:
		flow_edit_line(app, line);
		break;
	case INST_JOB_INSTALL:
		flow_install_line(app, line);
		break;
	default:
		break;
	}

	/* The screen is drawn again for what the line changed. */
	app->dirty = 1;
}

/*
 * Takes the end of a request (status 0 when it ended by its own line,
 * else the helper's end): a probe that never ended failed, an edit asks
 * for the disks again, an installation that never ended failed.
 */
void
inst_flow_ended(
	struct inst_app *app,
	enum inst_job job,
	int status)
{
	/* By the request that ended. */
	app->dirty = 1;
	if (job == INST_JOB_PROBE) {
		if (app->probe_state == 1) {
			app->probe_state = 3;
			if (app->probe_message[0] == '\0')
				(void)snprintf(app->probe_message, sizeof(app->probe_message), "%s (%d)", kl_tr("The installer's helper did not answer."), status);
		}

		/* The probe is over. */
		inst_log("PROBE state=%d disks=%u", app->probe_state, app->probe.disk_count);
		return;
	}

	/* An edit: what it wrote is listed, and the disk is read again. */
	if (job == INST_JOB_EDIT) {
		if (flow_pending_wrote || status != 0)
			flow_record_write(app, flow_pending_write);
		flow_pending_write[0] = '\0';
		flow_pending_wrote = 0;
		flow_probe(app);
		return;
	}

	/* An installation the helper left without an answer. */
	if (job == INST_JOB_INSTALL && app->screen == INST_S9)
		flow_failed(app, "helper", EIO, kl_tr("The installer's helper ended."));
}

/*
 * Reports why Next is not offered on the screen shown (one line), or NULL
 * when it is.
 */
const char *
inst_next_refusal(
	struct inst_app *app)
{
	const struct inst_disk *disk;
	const struct inst_part *part;
	const char *reason;
	uint64_t needed;
	uint64_t bytes;
	int refused;
	int firmware;

	/* The helper's answer comes first. */
	disk = flow_disk(app);
	switch (app->screen) {
	case INST_S1:
		if (!app->catalog.read)
			return kl_tr("The installer's files could not be read.");
		if (app->probe_state == 1 || app->probe_state == 0)
			return kl_tr("Looking at the disks...");
		if (app->probe_state == 3)
			return kl_tr("The disks could not be read.");
		if (!app->probe.uefi)
			return kl_tr("The installer works only on a computer started with UEFI.");
		return NULL;
	case INST_S2:
		if (disk == NULL)
			return kl_tr("Choose a disk.");
		reason = inst_disk_refusal(app, disk);
		if (reason != NULL)
			return kl_tr(reason);
		if (app->probe_state == 1)
			return kl_tr("Looking at the disks...");
		return NULL;
	case INST_S3:
		if (disk == NULL)
			return kl_tr("The disk is gone.");
		if (app->whole < 0)
			return kl_tr("Choose how to use the disk.");
		if (app->whole == 0) {
			reason = inst_chosen_refusal(app, disk);
			if (reason != NULL)
				return kl_tr(reason);
		}

		/* The way of using the disk can go on. */
		return NULL;
	case INST_S4:
		refused = inst_helper_busy(app);
		if (refused)
			return kl_tr("Writing to the disk...");
		if (app->reload_failed)
			return kl_tr("The new partitions can be used only after a restart.");
		if (disk == NULL)
			return kl_tr("The disk is gone.");
		part = inst_disk_esp(app, disk);
		if (part == NULL)
			return kl_tr("The disk needs an EFI system partition: create a partition and one is made.");
		return NULL;
	case INST_S5:
		if (!app->has_root || disk == NULL)
			return kl_tr("Choose the partition for the system.");
		part = inst_part_find(&app->probe, &app->root);
		if (part == NULL)
			return kl_tr("Choose the partition for the system.");
		needed = inst_root_needed(app, 1);
		bytes = inst_part_bytes(disk, part);
		if (bytes < needed)
			return kl_tr("The partition is too small for the system.");
		return NULL;
	case INST_S6:
		if (app->has_swap && disk != NULL) {
			part = inst_part_find(&app->probe, &app->swap);
			if (part == NULL)
				return kl_tr("Choose a swap partition, or none.");
			bytes = inst_part_bytes(disk, part);
			if (bytes < INST_SWAP_MIN_BYTES)
				return kl_tr("A swap partition must be 64 MiB or larger.");
		}

		/* The swap partition is fine, or none is asked for. */
		return NULL;
	case INST_S7:
		needed = inst_root_needed(app, 0);
		bytes = flow_root_bytes(app);
		if (bytes < needed)
			return kl_tr("The system's partition is too small for what is chosen.");
		firmware = inst_catalog_has(&app->catalog, "firmware");
		if (firmware && app->catalog.license_count > 0U && !app->license_agreed)
			return kl_tr("Agree to the firmware's licences, or leave the firmware out.");
		return NULL;
	case INST_S7A:
		refused = inst_user_refusal(app, &reason);
		if (refused)
			return reason;
		return NULL;
	case INST_S8:
		if (app->whole == 1 && !app->erase_agreed)
			return kl_tr("Agree that everything on the disk is erased.");
		refused = inst_helper_busy(app);
		if (refused)
			return kl_tr("Waiting for the installer's helper...");
		return NULL;
	default:
		break;
	}

	/* The last screens have no Next. */
	return "";
}

/* Goes to the screen after the one shown, when its checks allow (some through a dialog). */
void
inst_next(
	struct inst_app *app)
{
	const struct inst_disk *disk;
	const char *reason;

	/* Only when the screen is ready. */
	reason = inst_next_refusal(app);
	if (reason != NULL)
		return;

	/* The screen that follows. */
	disk = flow_disk(app);
	switch (app->screen) {
	case INST_S1:
		inst_show(app, INST_S2);
		break;
	case INST_S2:
		/* A session bound to another disk ends. */
		if (app->helper.session != NULL && disk != NULL && app->helper.session_disk != disk->registration)
			inst_helper_end_session(app);
		inst_show(app, INST_S3);
		break;
	case INST_S3:
		if (app->whole == 1) {
			inst_show(app, INST_S7);
			break;
		}

		/* The partitions are edited on S4, with a notice the first time. */
		inst_show(app, INST_S4);
		if (!app->edit_notice_shown) {
			app->edit_notice_shown = 1;
			app->dialog = INST_DIALOG_EDIT_NOTICE;
		}

		break;
	case INST_S4:
		inst_show(app, INST_S5);
		break;
	case INST_S5:
		inst_show(app, INST_S6);
		break;
	case INST_S6:
		/* No swap partition is confirmed (a swap file is made instead). */
		if (!app->has_swap && !app->swap_none) {
			app->dialog = INST_DIALOG_NO_SWAP;
			break;
		}

		/* The swap is settled: goes on to the packages. */
		inst_show(app, INST_S7);
		break;
	case INST_S7:
		inst_show(app, INST_S7A);
		break;
	case INST_S7A:
		inst_show(app, INST_S8);
		break;
	case INST_S8:
		app->dialog = INST_DIALOG_START;
		break;
	default:
		break;
	}

	/* The screen is drawn again for the one now shown. */
	app->dirty = 1;
}

/* Goes to the screen before the one shown. */
void
inst_back(
	struct inst_app *app)
{
	/* The screen that comes before. */
	switch (app->screen) {
	case INST_S2:
		inst_show(app, INST_S1);
		break;
	case INST_S3:
		inst_show(app, INST_S2);
		break;
	case INST_S4:
		inst_show(app, INST_S3);
		break;
	case INST_S5:
		inst_show(app, INST_S4);
		break;
	case INST_S6:
		inst_show(app, INST_S5);
		break;
	case INST_S7:
		if (app->whole == 1)
			inst_show(app, INST_S3);
		else
			inst_show(app, INST_S6);
		break;
	case INST_S7A:
		inst_show(app, INST_S7);
		break;
	case INST_S8:
		inst_show(app, INST_S7A);
		break;
	default:
		break;
	}
}

/* Shows a screen from its top (logged). */
void
inst_show(
	struct inst_app *app,
	enum inst_screen screen)
{
	/* The screen, its dialog gone, its content from the top. */
	app->screen = screen;
	app->dialog = INST_DIALOG_NONE;
	app->license_view = 0;
	kl_scroll_move_to(&app->scroll, 0.0, 0.0, 0, kl_clock_us());
	kl_ui_clear_focus(app->ui);
	app->dirty = 1;
	inst_log("SCREEN name=%s", flow_screen_words[screen]);
}

/* Asks to cancel: the screens' Cancel, confirmed by a dialog (S9's stops the installation). */
void
inst_cancel_asked(
	struct inst_app *app)
{
	/* The last screens have nothing to cancel. */
	if (app->screen == INST_S10 || app->screen == INST_S11)
		return;

	/* S9 stops the installation; the others go to S10. */
	if (app->screen == INST_S9) {
		if (!app->stop_asked)
			app->dialog = INST_DIALOG_STOP;
		app->dirty = 1;
		return;
	}

	/* Asks to leave the wizard. */
	app->dialog = INST_DIALOG_CANCEL;
	app->dirty = 1;
}

/* Takes a dialog's answer: 0 the main button, any other the last (cancel). */
void
inst_dialog_answer(
	struct inst_app *app,
	int answer)
{
	enum inst_dialog dialog;
	int item;
	int same;

	/* The dialog goes either way. */
	dialog = app->dialog;
	app->dialog = INST_DIALOG_NONE;
	app->dirty = 1;
	inst_log("DIALOG answer=%d dialog=%d", answer, (int)dialog);
	if (answer != 0 && dialog != INST_DIALOG_EDIT_NOTICE)
		return;

	/* What the main button does. */
	switch (dialog) {
	case INST_DIALOG_CANCEL:
		app->canceled = 1;
		(void)snprintf(app->fail_step, sizeof(app->fail_step), "%s", "");
		app->fail_text[0] = '\0';
		inst_show(app, INST_S10);
		break;
	case INST_DIALOG_DELETE:
		inst_edit_delete(app);
		break;
	case INST_DIALOG_ERASE_ROOT:
		app->root = app->pending;
		app->has_root = 1;
		same = inst_tuple_same(&app->swap, &app->root);
		if (app->has_swap && same)
			app->has_swap = 0;
		break;
	case INST_DIALOG_ERASE_SWAP:
		app->swap = app->pending;
		app->has_swap = 1;
		app->swap_none = 0;
		break;
	case INST_DIALOG_NO_SWAP:
		app->swap_none = 1;
		inst_show(app, INST_S7);
		break;
	case INST_DIALOG_DROP_DESKTOP:
	case INST_DIALOG_DROP_FIRMWARE:
		item = app->pending_item;
		if (item >= 0 && (unsigned)item < app->catalog.item_count) {
			app->catalog.item[item].chosen = 0;
			inst_catalog_choose(&app->catalog);
		}

		break;
	case INST_DIALOG_START:
		flow_install(app);
		break;
	case INST_DIALOG_STOP:
		app->stop_asked = 1;
		(void)inst_helper_request(app, app->disk, INST_JOB_CANCEL, "cancel\n");
		inst_log("INSTALL cancel-asked");
		break;
	default:
		break;
	}
}

/* Deletes the partition selected on S4 (after its dialog). */
void
inst_edit_delete(
	struct inst_app *app)
{
	const struct inst_disk *disk;
	const struct inst_part *part;
	char request[INST_REQUEST_MAX];
	char what[INST_MESSAGE_MAX];
	char tuple[160];
	char size[32];
	size_t used;
	int error;

	/* The disk and the partition, still as the screen showed them. */
	disk = flow_disk(app);
	if (disk == NULL || !app->has_selected)
		return;
	part = inst_part_find(&app->probe, &app->selected);
	if (part == NULL)
		return;

	/* The request: the disk, and the partition by its tuple. */
	used = 0;
	error = flow_append(request, sizeof(request), &used, "edit delete ");
	if (error == 0)
		error = flow_disk_text(app, disk, request + used, sizeof(request) - used);
	used = strlen(request);
	flow_tuple_text(&app->selected, tuple, sizeof(tuple));
	if (error == 0)
		error = flow_append(request, sizeof(request), &used, " part %s\n", tuple);
	if (error != 0)
		return;

	/* What S10 lists of it, and the write. */
	inst_bytes_text(inst_part_bytes(disk, part), size, sizeof(size));
	(void)snprintf(what, sizeof(what), "%s %u (%s, %s)", kl_tr("Deleted partition"), part->slot, size, inst_type_name(part->type));
	app->has_selected = 0;
	flow_send_edit(app, request, what);
}

/* Creates a partition of the size asked on S4 (and an ESP before it when the disk has none). */
void
inst_edit_create(
	struct inst_app *app)
{
	const struct inst_disk *disk;
	char request[INST_REQUEST_MAX];
	char what[INST_MESSAGE_MAX];
	char size[32];
	uint64_t esp_start;
	uint64_t start;
	uint64_t sectors;
	uint64_t bytes;
	unsigned long long esp_sectors;
	size_t used;
	int with_esp;
	int error;
	int is_esp;
	unsigned i;

	/* The disk and the size. */
	disk = flow_disk(app);
	if (disk == NULL)
		return;
	error = inst_size_parse(app->create_size.text, &bytes);
	if (error != 0) {
		(void)snprintf(app->edit_message, sizeof(app->edit_message), "%s", kl_tr("Type a size such as 20 GiB or 512 MiB."));
		app->edit_failed = 1;
		app->dirty = 1;

		/* Says in the log why nothing was made. */
		inst_log("EDIT refused reason=size");
		return;
	}

	/* An ESP comes first when the disk has none. */
	with_esp = 1;
	for (i = 0; i < disk->part_count; i++) {
		is_esp = inst_part_esp(&app->probe.part[disk->first_part + i]);
		if (is_esp)
			with_esp = 0;
	}

	/* The place: the first free room it fits in. */
	error = inst_place(disk, &app->probe, bytes, with_esp, &esp_start, &start, &sectors);
	if (error != 0 || disk->entries + 1U + (unsigned)with_esp > 16U) {
		(void)snprintf(app->edit_message, sizeof(app->edit_message), "%s", kl_tr("There is no room for a partition of that size."));
		app->edit_failed = 1;
		app->dirty = 1;

		/* Says in the log why nothing was made. */
		inst_log("EDIT refused reason=room");
		return;
	}

	/* The request. */
	used = 0;
	if (with_esp)
		error = flow_append(request, sizeof(request), &used, "edit create-with-esp ");
	else
		error = flow_append(request, sizeof(request), &used, "edit create ");
	if (error == 0)
		error = flow_disk_text(app, disk, request + used, sizeof(request) - used);
	used = strlen(request);
	if (error == 0 && with_esp) {
		esp_sectors = (unsigned long long)(INST_ESP_BYTES / disk->sector_bytes);
		error = flow_append(request, sizeof(request), &used, " esp %llu %llu", (unsigned long long)esp_start, esp_sectors);
	}

	/* The new partition's place and size. */
	if (error == 0)
		error = flow_append(request, sizeof(request), &used, " new %llu %llu\n", (unsigned long long)start, (unsigned long long)sectors);

	/* A request that did not fit is not sent. */
	if (error != 0)
		return;

	/* What S10 lists of it, and the write. */
	inst_bytes_text(sectors * disk->sector_bytes, size, sizeof(size));
	if (with_esp)
		(void)snprintf(what, sizeof(what), "%s (%s) %s", kl_tr("Created a partition"), size, kl_tr("and an EFI system partition of 512 MiB"));
	else
		(void)snprintf(what, sizeof(what), "%s (%s)", kl_tr("Created a partition"), size);
	flow_send_edit(app, request, what);
}

/* Chooses the root partition on S5: at once for an unassigned one, after a dialog for another. */
void
inst_choose_root(
	struct inst_app *app,
	const struct inst_part *part)
{
	int unassigned;
	int same;

	/* A partition made here holds nothing. */
	unassigned = inst_part_unassigned(part);
	if (unassigned) {
		inst_tuple_of(part, &app->root);
		app->has_root = 1;
		same = inst_tuple_same(&app->swap, &app->root);
		if (app->has_swap && same)
			app->has_swap = 0;
		app->dirty = 1;
		return;
	}

	/* Another's contents are erased: asked first. */
	inst_tuple_of(part, &app->pending);
	app->dialog_part = (long)(part - app->probe.part);
	app->dialog = INST_DIALOG_ERASE_ROOT;
	app->dirty = 1;
}

/* Chooses the swap partition on S6 (NULL for none): as the root is chosen. */
void
inst_choose_swap(
	struct inst_app *app,
	const struct inst_part *part)
{
	int unassigned;

	/* None: a swap file on the root. */
	if (part == NULL) {
		app->has_swap = 0;
		app->swap_none = 1;
		app->dirty = 1;
		return;
	}

	/* A partition made here holds nothing. */
	unassigned = inst_part_unassigned(part);
	if (unassigned) {
		inst_tuple_of(part, &app->swap);
		app->has_swap = 1;
		app->swap_none = 0;
		app->dirty = 1;
		return;
	}

	/* Another's contents are erased: asked first. */
	inst_tuple_of(part, &app->pending);
	app->dialog_part = (long)(part - app->probe.part);
	app->dialog = INST_DIALOG_ERASE_SWAP;
	app->dirty = 1;
}

/*
 * Turns an item of S7 on or off: the required base stays, a heading opens
 * or closes, the desktop and the firmware go only after a warning, an
 * item that comes with another stays.
 */
void
inst_item_toggle(
	struct inst_app *app,
	unsigned item)
{
	struct inst_item *entry;
	int same;
	int needed;

	/* An item there is. */
	if (item >= app->catalog.item_count)
		return;
	entry = &app->catalog.item[item];
	app->dirty = 1;

	/* A heading: "Other packages" opens and closes. */
	if (entry->unit_count == 0U) {
		same = strcmp(entry->id, "other");
		if (same == 0)
			app->other_open = !app->other_open;
		return;
	}

	/* The base, and an item another needs, stay. */
	if (entry->fallback == 2)
		return;
	needed = inst_item_by_dependency(&app->catalog, item);
	if (needed)
		return;

	/* The desktop and the firmware go after a warning. */
	if (entry->chosen) {
		same = strcmp(entry->id, "desktop");
		if (same == 0) {
			app->pending_item = (int)item;
			app->dialog = INST_DIALOG_DROP_DESKTOP;
			return;
		}

		/* The firmware too. */
		same = strcmp(entry->id, "firmware");
		if (same == 0) {
			app->pending_item = (int)item;
			app->dialog = INST_DIALOG_DROP_FIRMWARE;
			return;
		}
	}

	/* Any other turns over. */
	entry->chosen = !entry->chosen;
	inst_catalog_choose(&app->catalog);
}

/*
 * Starts again from S2 (S10's "Start over"): the choices go, the disks
 * are read again; the session, if one runs, stays bound to its disk.
 */
void
inst_start_over(
	struct inst_app *app)
{
	/* The choices of the screens and of the last attempt. */
	app->disk = 0;
	app->whole = -1;
	flow_forget_choices(app);
	app->edit_message[0] = '\0';
	app->edit_failed = 0;
	app->write_count = 0;
	app->state_count = 0;
	app->canceled = 0;
	app->fail_step[0] = '\0';
	app->fail_text[0] = '\0';
	app->stop_asked = 0;
	app->step = 0;
	app->bytes_done = 0;
	app->bytes_total = 0;

	/* S2, the disks read again. */
	inst_show(app, INST_S2);
	flow_probe(app);
}

/* Asks for the disks again (S1's Try again, S2's Reload), the choices kept while their disk stays. */
void
inst_probe_again(
	struct inst_app *app)
{
	/* A new list. */
	flow_probe(app);
	app->dirty = 1;
}

/* Restarts the computer (S11), through the desktop's power rules. */
void
inst_reboot(
	struct inst_app *app)
{
	uint32_t request;
	int error;

	/* The session ends first. */
	inst_helper_end_session(app);

	/* The desktop's system restarts the computer. */
	if (app->system == NULL)
		app->system = kl_app_system(app->app);
	error = ENOTSUP;
	if (app->system != NULL)
		error = kl_system_power_action(app->system, KL_POWER_REBOOT, &request);

	/* A restart that was refused is told on S11. */
	inst_log("REBOOT error=%d", error);
	if (error != 0)
		(void)snprintf(app->power_message, sizeof(app->power_message), "%s (%s)", kl_tr("The computer could not be restarted."), strerror(error));

	/* The screen is drawn again for the message. */
	app->dirty = 1;
}

/* Gives a screen's word for the log. */
const char *
inst_screen_word(
	enum inst_screen screen)
{
	/* The word of a screen there is. */
	if ((unsigned)screen >= INST_SCREENS)
		return "unknown";
	return flow_screen_words[screen];
}

/*
 * Reports whether S7a's answers are refused (1, with the reason) or may
 * be installed (0).
 */
int
inst_user_refusal(
	const struct inst_app *app,
	const char **reason)
{
	int same;
	int valid;

	/* The login name. */
	valid = flow_name_valid(app->user.text);
	if (!valid) {
		*reason = kl_tr("The user name starts with a small letter; then small letters, digits, - and _.");
		return 1;
	}

	/* A name the system already uses. */
	valid = flow_system_user(app->user.text);
	if (valid) {
		*reason = kl_tr("That user name is the system's own.");
		return 1;
	}

	/* The display name. */
	valid = flow_text_valid(app->fullname.text, FLOW_FULLNAME_MAX, 0);
	if (!valid) {
		*reason = kl_tr("The full name cannot hold a colon or control characters.");
		return 1;
	}

	/* The user's password, set and typed twice alike. */
	valid = flow_text_valid(app->password.text, FLOW_PASSWORD_MAX, 1);
	if (app->password.length == 0U || !valid) {
		*reason = kl_tr("Type a password for the user.");
		return 1;
	}

	/* The two entries of the user's password. */
	same = strcmp(app->password.text, app->password_again.text);
	if (same != 0) {
		*reason = kl_tr("The user's two passwords differ.");
		return 1;
	}

	/* Root's password, set and typed twice alike. */
	valid = flow_text_valid(app->root_password.text, FLOW_PASSWORD_MAX, 1);
	if (app->root_password.length == 0U || !valid) {
		*reason = kl_tr("Type a password for root.");
		return 1;
	}

	/* The two entries of root's password. */
	same = strcmp(app->root_password.text, app->root_password_again.text);
	if (same != 0) {
		*reason = kl_tr("Root's two passwords differ.");
		return 1;
	}

	/* The hostname. */
	valid = flow_hostname_valid(app->hostname.text);
	if (!valid) {
		*reason = kl_tr("The computer's name takes small letters, digits and -, up to 63.");
		return 1;
	}

	/* The answers may be installed. */
	*reason = NULL;
	return 0;
}

/* Asks for the disks again, from a new list. */
static void
flow_probe(
	struct inst_app *app)
{
	int error;

	/* A new list, asked. */
	memset(&app->incoming, 0, sizeof(app->incoming));
	app->probe_message[0] = '\0';
	error = inst_helper_probe(app);
	if (error == EBUSY)
		return;
	if (error != 0) {
		app->probe_state = 3;
		(void)snprintf(app->probe_message, sizeof(app->probe_message), "%s (%s)", kl_tr("The installer's helper could not be started."), strerror(error));
		inst_log("PROBE start-failed errno=%d", error);
		return;
	}

	/* The probe is under way. */
	app->probe_state = 1;
}

/* Takes a line of a probe: a record, its end, or the helper's failure. */
static void
flow_probe_line(
	struct inst_app *app,
	char *line)
{
	int read;
	int same;

	/* A failure of the helper. */
	same = strncmp(line, "FAIL ", 5U);
	if (same == 0) {
		app->probe_state = 3;
		(void)snprintf(app->probe_message, sizeof(app->probe_message), "%s", line + 5);
		return;
	}

	/* A record, or the end that makes the list the one shown. */
	read = inst_probe_line(&app->incoming, line);
	if (read == 1)
		flow_probe_read(app);
}

/*
 * Makes a probe's list the one shown, and drops the choices it no longer
 * has: a disk that went, a root or swap partition no longer there.
 */
static void
flow_probe_read(
	struct inst_app *app)
{
	const struct inst_disk *disk;
	const struct inst_part *part;

	/* The list. */
	app->probe = app->incoming;
	app->probe_state = 2;

	/* A disk that went, and the partitions that went with it or alone. */
	disk = inst_disk_find(&app->probe, app->disk);
	if (disk == NULL)
		app->disk = 0;
	if (app->has_root) {
		part = inst_part_find(&app->probe, &app->root);
		if (part == NULL)
			app->has_root = 0;
	}

	/* The swap partition. */
	if (app->has_swap) {
		part = inst_part_find(&app->probe, &app->swap);
		if (part == NULL)
			app->has_swap = 0;
	}

	/* The partition selected on S4. */
	if (app->has_selected) {
		part = inst_part_find(&app->probe, &app->selected);
		if (part == NULL)
			app->has_selected = 0;
	}

	/* The new list is logged. */
	inst_log("PROBE read disks=%u parts=%u uefi=%d memory=%llu", app->probe.disk_count, app->probe.part_count, app->probe.uefi,
		 (unsigned long long)app->probe.memory);
}

/* Takes a line of an edit's answer. */
static void
flow_edit_line(
	struct inst_app *app,
	char *line)
{
	char *text;
	int error;
	int same;

	/* The table written, which S10 lists. */
	inst_log("EDIT %s", line);
	same = strncmp(line, "STATE table ", 12U);
	if (same == 0) {
		flow_pending_wrote = 1;
		return;
	}

	/* A reload of the table that failed. */
	same = strcmp(line, "STATE reload-failed");
	if (same == 0) {
		app->reload_failed = 1;
		return;
	}

	/* An ESP left behind. */
	same = strcmp(line, "STATE created-not-deleted");
	if (same == 0) {
		flow_record_write(app, kl_tr("A new EFI system partition and partition could not be removed after the failure."));
		return;
	}

	/* Done: the choices of S5 to S8 go (a slot may now name another partition). */
	same = strcmp(line, "DONE");
	if (same == 0) {
		app->edit_message[0] = '\0';
		app->edit_failed = 0;
		flow_forget_choices(app);
		return;
	}

	/* A failure, said under the buttons. */
	same = strncmp(line, "FAIL ", 5U);
	if (same == 0) {
		text = strchr(line + 5, ' ');
		error = 0;
		if (text != NULL)
			error = (int)strtol(text + 1, &text, 10);
		if (text == NULL)
			text = line + 5;
		while (*text == ' ')
			text++;

		/* The message says whether the table was already written. */
		(void)snprintf(app->edit_message, sizeof(app->edit_message), "%s: %s (%d)", kl_tr("The disk was not changed"), text, error);
		if (flow_pending_wrote)
			(void)snprintf(app->edit_message, sizeof(app->edit_message), "%s: %s (%d)", kl_tr("The change failed after the table was written"), text, error);
		app->edit_failed = 1;
	}
}

/* Takes a line of the installation's answer: its steps, bytes, units, states and end. */
static void
flow_install_line(
	struct inst_app *app,
	char *line)
{
	unsigned long long done;
	unsigned long long total;
	unsigned step;
	unsigned steps;
	char name[32];
	char word[64];
	char *text;
	int error;
	int same;
	int matched;

	/* A step begun. */
	matched = sscanf(line, "STEP %u %u %31s", &step, &steps, name);
	if (matched == 3) {
		app->step = step;
		app->steps = steps;
		(void)snprintf(app->step_name, sizeof(app->step_name), "%s", name);
		inst_log("INSTALL step=%u/%u name=%s", step, steps, name);
		return;
	}

	/* The bytes unpacked. */
	matched = sscanf(line, "BYTES %llu %llu", &done, &total);
	if (matched == 2) {
		app->bytes_done = done;
		app->bytes_total = total;
		return;
	}

	/* The unit being unpacked. */
	matched = sscanf(line, "UNIT %63s", word);
	if (matched == 1) {
		(void)snprintf(app->unit_name, sizeof(app->unit_name), "%s", word);
		return;
	}

	/* What the disk holds: the loader's place, the swap, and the rest for S10. */
	inst_log("INSTALL %s", line);
	matched = sscanf(line, "STATE loader %255s", app->loader_path);
	if (matched == 1)
		return;
	matched = sscanf(line, "STATE boot-entry %7s", app->boot_entry);
	if (matched == 1)
		return;
	matched = sscanf(line, "STATE swap %15s", app->swap_kind);
	if (matched == 1)
		return;
	same = strncmp(line, "STATE ", 6U);
	if (same == 0) {
		if (app->state_count < INST_STATES_MAX) {
			(void)snprintf(app->states[app->state_count], INST_MESSAGE_MAX, "%s", line + 6);
			app->state_count++;
		}

		/* The line is listed or dropped; either way it is taken. */
		return;
	}

	/* The end: S11. */
	same = strcmp(line, "DONE");
	if (same == 0) {
		app->ended_us = kl_clock_us();
		inst_helper_end_session(app);
		inst_show(app, INST_S11);
		return;
	}

	/* The failure: S10 with the step that failed. */
	same = strncmp(line, "FAIL ", 5U);
	if (same == 0) {
		name[0] = '\0';
		error = 0;
		text = line + 5;
		matched = sscanf(text, "%31s %d", name, &error);
		if (matched == 2) {
			text = strchr(text, ' ');
			if (text != NULL)
				text = strchr(text + 1, ' ');
		}

		/* The words after the code are the text. */
		if (text == NULL)
			text = (char *)"";
		while (*text == ' ')
			text++;
		flow_failed(app, name, error, text);
	}
}

/* Shows S10 for a failed (or stopped) installation. */
static void
flow_failed(
	struct inst_app *app,
	const char *step,
	int error,
	const char *text)
{
	/* What failed, and whether it was the cancellation. */
	app->ended_us = kl_clock_us();
	app->canceled = 0;
	if (error == ECANCELED)
		app->canceled = 1;

	/* What S10 shows of it. */
	(void)snprintf(app->fail_step, sizeof(app->fail_step), "%s", step);
	app->fail_errno = error;
	(void)snprintf(app->fail_text, sizeof(app->fail_text), "%s", text);
	inst_log("INSTALL failed step=%s errno=%d", step, error);
	inst_show(app, INST_S10);
}

/* Gives the disk chosen on S2 as the last probe told it, or NULL. */
static const struct inst_disk *
flow_disk(
	const struct inst_app *app)
{
	/* None chosen. */
	if (app->disk == 0U)
		return NULL;

	/* The disk, unless it went. */
	return inst_disk_find(&app->probe, app->disk);
}

/* Gives the room of the root: the partition chosen, or the whole disk's after the ESP and the swap. */
static uint64_t
flow_root_bytes(
	const struct inst_app *app)
{
	const struct inst_disk *disk;
	const struct inst_part *part;
	uint64_t bytes;
	uint64_t taken;

	/* The disk. */
	disk = flow_disk(app);
	if (disk == NULL)
		return 0;

	/* The whole disk's root: what the ESP, the swap and the tables' MiB leave. */
	if (app->whole == 1) {
		bytes = inst_disk_bytes(disk);
		taken = INST_ESP_BYTES + inst_whole_swap_bytes(app, disk) + 2U * INST_MIB;
		if (bytes <= taken)
			return 0;
		return bytes - taken;
	}

	/* The partition chosen. */
	if (!app->has_root)
		return 0;
	part = inst_part_find(&app->probe, &app->root);
	if (part == NULL)
		return 0;

	/* The partition's room. */
	bytes = inst_part_bytes(disk, part);
	return bytes;
}

/* Writes a request's disk: "disk REGISTRATION NAME SECTOR-BYTES SECTORS DIGEST"; returns 0 or ENOSPC. */
static int
flow_disk_text(
	const struct inst_app *app,
	const struct inst_disk *disk,
	char *text,
	size_t size)
{
	int written;

	/* The application is not needed to write the disk. */
	(void)app;

	/* The disk as the screen saw it. */
	written = snprintf(text, size, "disk %u %s %u %llu %s", (unsigned)disk->registration, disk->name, (unsigned)disk->sector_bytes,
			   (unsigned long long)disk->sectors, disk->digest);
	if (written < 0 || (size_t)written >= size)
		return ENOSPC;

	/* Succeeded: the disk is written. */
	return 0;
}

/* Writes a tuple: REGISTRATION:START:SECTORS:PARTUUID. */
static void
flow_tuple_text(
	const struct inst_tuple *tuple,
	char *text,
	size_t size)
{
	/* The four, apart by colons. */
	(void)snprintf(text, size, "%u:%llu:%llu:%s", (unsigned)tuple->disk, (unsigned long long)tuple->start,
		       (unsigned long long)tuple->sectors, tuple->partuuid);
}

/* Appends " WORD LENGTH:VALUE", a person's text that no space or newline can break; returns 0 or ENOSPC. */
static int
flow_field(
	char *text,
	size_t size,
	size_t *used,
	const char *word,
	const char *value)
{
	size_t length;
	int error;

	/* The word and the length. */
	length = strlen(value);
	error = flow_append(text, size, used, " %s %lu:", word, (unsigned long)length);
	if (error != 0)
		return error;

	/* The bytes themselves. */
	if (*used + length + 1U > size)
		return ENOSPC;
	memcpy(text + *used, value, length);
	*used += length;
	text[*used] = '\0';

	/* Succeeded: the field is written. */
	return 0;
}

/* Appends formatted text to a request; returns 0 or ENOSPC. */
static int
flow_append(
	char *text,
	size_t size,
	size_t *used,
	const char *format,
	...)
{
	va_list arguments;
	int written;

	/* The text after what is there. */
	va_start(arguments, format);
	written = vsnprintf(text + *used, size - *used, format, arguments);
	va_end(arguments);
	if (written < 0 || (size_t)written >= size - *used)
		return ENOSPC;
	*used += (size_t)written;

	/* Succeeded: the text is appended. */
	return 0;
}

/* Sends an edit to the session, remembering what it is for S10. */
static void
flow_send_edit(
	struct inst_app *app,
	const char *request,
	const char *what)
{
	int error;

	/* The request. */
	(void)snprintf(flow_pending_write, sizeof(flow_pending_write), "%s", what);
	flow_pending_wrote = 0;
	app->edit_message[0] = '\0';
	app->edit_failed = 0;
	inst_log("EDIT request %s", what);
	error = inst_helper_request(app, app->disk, INST_JOB_EDIT, request);
	if (error != 0) {
		(void)snprintf(app->edit_message, sizeof(app->edit_message), "%s (%s)", kl_tr("The disk was not changed"), strerror(error));
		app->edit_failed = 1;
		flow_pending_write[0] = '\0';
	}

	/* The screen is drawn again for the message or the busy state. */
	app->dirty = 1;
}

/* Sends the installation (after S8's dialog) and shows S9. */
static void
flow_install(
	struct inst_app *app)
{
	const struct inst_disk *disk;
	const struct inst_part *esp;
	struct inst_tuple esp_tuple;
	char *request;
	char tuple[160];
	size_t used;
	unsigned count;
	unsigned i;
	uint64_t swap_bytes;
	int desktop;
	int error;

	/* The disk; the request is large. */
	disk = flow_disk(app);
	if (disk == NULL)
		return;
	request = malloc(INST_REQUEST_MAX);
	if (request == NULL)
		return;

	/* "install DISK" and the layout. */
	used = 0;
	error = flow_append(request, INST_REQUEST_MAX, &used, "install ");
	if (error == 0)
		error = flow_disk_text(app, disk, request + used, INST_REQUEST_MAX - used);
	used = strlen(request);
	if (error == 0 && app->whole == 1) {
		swap_bytes = inst_whole_swap_bytes(app, disk);
		error = flow_append(request, INST_REQUEST_MAX, &used, " whole %llu %llu", (unsigned long long)INST_ESP_BYTES, (unsigned long long)swap_bytes);
	} else if (error == 0) {
		esp = inst_disk_esp(app, disk);
		error = EINVAL;
		if (esp != NULL) {
			inst_tuple_of(esp, &esp_tuple);
			flow_tuple_text(&esp_tuple, tuple, sizeof(tuple));
			error = flow_append(request, INST_REQUEST_MAX, &used, " chosen esp %s", tuple);
		}

		/* The root partition. */
		flow_tuple_text(&app->root, tuple, sizeof(tuple));
		if (error == 0)
			error = flow_append(request, INST_REQUEST_MAX, &used, " root %s", tuple);

		/* The swap partition, or none. */
		flow_tuple_text(&app->swap, tuple, sizeof(tuple));
		if (error == 0 && app->has_swap)
			error = flow_append(request, INST_REQUEST_MAX, &used, " swap %s", tuple);
		else if (error == 0)
			error = flow_append(request, INST_REQUEST_MAX, &used, " swap none");
	}

	/* The units: every one the choice installs. */
	count = 0;
	for (i = 0; i < app->catalog.unit_count; i++) {
		if (app->catalog.closure[i])
			count++;
	}

	/* How many, then each by its name. */
	if (error == 0)
		error = flow_append(request, INST_REQUEST_MAX, &used, " units %u", count);
	for (i = 0; i < app->catalog.unit_count && error == 0; i++) {
		if (app->catalog.closure[i])
			error = flow_append(request, INST_REQUEST_MAX, &used, " %s", app->catalog.unit[i].name);
	}

	/* The user, the passwords, the automatic login (only with the desktop) and the hostname. */
	desktop = inst_catalog_has(&app->catalog, "desktop");
	if (error == 0)
		error = flow_field(request, INST_REQUEST_MAX, &used, "user", app->user.text);
	if (error == 0)
		error = flow_field(request, INST_REQUEST_MAX, &used, "fullname", app->fullname.text);
	if (error == 0)
		error = flow_field(request, INST_REQUEST_MAX, &used, "password", app->password.text);
	if (error == 0)
		error = flow_field(request, INST_REQUEST_MAX, &used, "root-password", app->root_password.text);
	if (error == 0) {
		if (app->autologin && desktop)
			error = flow_append(request, INST_REQUEST_MAX, &used, " autologin yes");
		else
			error = flow_append(request, INST_REQUEST_MAX, &used, " autologin no");
	}

	/* The hostname, and the end of the request. */
	if (error == 0)
		error = flow_field(request, INST_REQUEST_MAX, &used, "hostname", app->hostname.text);
	if (error == 0)
		error = flow_append(request, INST_REQUEST_MAX, &used, "\n");

	/* The progress, and the request (the passwords forgotten once sent). */
	(void)snprintf(app->installed_user, sizeof(app->installed_user), "%s", app->user.text);
	app->step = 0;
	app->steps = 10;
	app->step_name[0] = '\0';
	app->bytes_done = 0;
	app->bytes_total = 0;
	app->unit_name[0] = '\0';
	app->stop_asked = 0;
	app->state_count = 0;
	app->loader_path[0] = '\0';
	app->boot_entry[0] = '\0';
	app->swap_kind[0] = '\0';
	app->started_us = kl_clock_us();
	if (error == 0) {
		inst_log("INSTALL request units=%u whole=%d", count, app->whole);
		error = inst_helper_request(app, disk->registration, INST_JOB_INSTALL, request);
	}

	/* The request held the passwords: wipes it, and the fields. */
	memset(request, 0, INST_REQUEST_MAX);
	free(request);
	flow_forget_passwords(app);
	if (error != 0) {
		flow_failed(app, "start", error, strerror(error));
		return;
	}

	/* The installation runs: shows its progress. */
	inst_show(app, INST_S9);
}

/* Lists a change written to the disk for S10. */
static void
flow_record_write(
	struct inst_app *app,
	const char *what)
{
	/* A change with words, while the list has room. */
	if (what[0] == '\0' || app->write_count >= INST_WRITES_MAX)
		return;
	(void)snprintf(app->writes[app->write_count], INST_MESSAGE_MAX, "%s", what);
	app->write_count++;
}

/* Forgets the choices of S5 to S8 (after a write of S4, or to start over). */
static void
flow_forget_choices(
	struct inst_app *app)
{
	/* The root, the swap, the confirmations. */
	app->has_root = 0;
	app->has_swap = 0;
	app->swap_none = 0;
	app->erase_agreed = 0;
	app->has_selected = 0;
}

/* Forgets the passwords typed (after the installation was sent). */
static void
flow_forget_passwords(
	struct inst_app *app)
{
	/* Each field's text, wiped. */
	memset(&app->password, 0, sizeof(app->password));
	memset(&app->password_again, 0, sizeof(app->password_again));
	memset(&app->root_password, 0, sizeof(app->root_password));
	memset(&app->root_password_again, 0, sizeof(app->root_password_again));
	app->password.secret = 1;
	app->password_again.secret = 1;
	app->root_password.secret = 1;
	app->root_password_again.secret = 1;
}

/* Reports whether a login name follows account-admin's rule (not root). */
static int
flow_name_valid(
	const char *name)
{
	size_t i;
	char c;
	int same;

	/* A small letter first. */
	if (name[0] < 'a' || name[0] > 'z')
		return 0;

	/* Then small letters, digits, '-' and '_', up to the most. */
	for (i = 1; name[i] != '\0'; i++) {
		if (i >= FLOW_USER_MAX)
			return 0;
		c = name[i];
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
			continue;
		return 0;
	}

	/* Not root. */
	same = strcmp(name, "root");
	if (same == 0)
		return 0;

	/* The name may be used. */
	return 1;
}

/* Reports whether a text has no control characters (nor a colon, unless allowed) and fits. */
static int
flow_text_valid(
	const char *text,
	size_t most,
	int colon)
{
	size_t i;
	unsigned char c;

	/* Each byte. */
	for (i = 0; text[i] != '\0'; i++) {
		if (i >= most)
			return 0;
		c = (unsigned char)text[i];
		if (c < 0x20U || c == 0x7fU)
			return 0;
		if (c == ':' && !colon)
			return 0;
	}

	/* The text may be used. */
	return 1;
}

/* Reports whether a hostname takes small letters, digits and '-', 1 to 63, not at either end. */
static int
flow_hostname_valid(
	const char *name)
{
	size_t length;
	size_t i;
	char c;

	/* Its length and its ends. */
	length = strlen(name);
	if (length == 0U || length > FLOW_HOSTNAME_MAX)
		return 0;
	if (name[0] == '-' || name[length - 1U] == '-')
		return 0;

	/* Each character. */
	for (i = 0; i < length; i++) {
		c = name[i];
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')
			continue;
		return 0;
	}

	/* The name may be used. */
	return 1;
}

/*
 * Reports whether a name is one of the system's own accounts: a name of
 * the image's /etc/passwd other than the default user's (uid 1000, whom
 * the installation's user replaces).
 */
static int
flow_system_user(
	const char *name)
{
	FILE *passwd;
	char line[512];
	char *colon;
	char *uid;
	long number;
	int same;
	char *text;
	int found;

	/* The image's accounts (the base's, as installed). */
	passwd = fopen(INST_PASSWD, "r");
	if (passwd == NULL)
		return 0;

	/* Each line: NAME:PASSWORD:UID:... */
	found = 0;
	for (;;) {
		text = fgets(line, sizeof(line), passwd);
		if (text == NULL)
			break;
		colon = strchr(line, ':');
		if (colon == NULL)
			continue;
		*colon = '\0';
		same = strcmp(line, name);
		if (same != 0)
			continue;
		uid = strchr(colon + 1, ':');
		number = -1;
		if (uid != NULL)
			number = strtol(uid + 1, NULL, 10);
		if (number != FLOW_DEFAULT_UID)
			found = 1;
	}

	/* The file is read to its end. */
	(void)fclose(passwd);

	/* Whether the name is taken. */
	return found;
}
