/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Startup selection and automatic boot policy.
 */

#include "kern/internal.h"
#include "kern/clock.h"
#include "kern/exec.h"
#include "kern/messages.h"
#include "kern/platform.h"
#include "kern/vfs.h"
#include "hal/hal.h"

/* The startup menu exposes only the first four fixed disks. The full stable
 * discovery order remains addressable through device and disk. */
static int menu_device(unsigned ordinal)
{
	unsigned found = 0;
	for (unsigned i = 0; i < device_count; i++) {
		if (++found == ordinal)
			return (int)i;
	}
	return -1;
}

/* Resolve the startup configuration file on the mounted BOOT volume.
 * BOOT.CFG is the pre-1.0 name; keep reading it for one release. */
const char *startup_config_file(void)
{
	struct inode *inode;

	if (namei_at(&kern_cwdinfo, "ZEDBSD.CFG", &inode) == 0) {
		inode_release(inode);
		return "ZEDBSD.CFG";
	}
	if (namei_at(&kern_cwdinfo, "BOOT.CFG", &inode) == 0) {
		inode_release(inode);
		return "BOOT.CFG";
	}
	return NULL;
}

static enum startup_config_kind boot_volume_startup_kind(void)
{
	struct inode *inode;

	if (namei_at(&kern_cwdinfo, "AUTOEXEC.NCT", &inode) == 0) {
		inode_release(inode);
		return STARTUP_CONFIG_AUTOEXEC;
	}
	if (startup_config_file())
		return STARTUP_CONFIG_BOOTCFG;
	return STARTUP_CONFIG_NONE;
}

static unsigned fixed_device_ordinal(int device)
{
	unsigned ordinal = 0;

	for (int i = 0; i <= device; i++)
		ordinal++;
	return ordinal;
}

static void consider_automatic_device(struct startup_state *state, int device)
{
	int first_bootable = -1;
	int config_partition = -1;
	enum startup_config_kind config_kind = STARTUP_CONFIG_NONE;

	if (device < 0 || !(devs[device].flags & ZEDBSD_DEV_HAS_GEOMETRY) ||
	    !scanparts(device))
		return;
	register_scanned_disk(device);
	for (int partition = 0; partition < MAX_PARTS; partition++) {
		if (!parts[partition].valid)
			continue;
		/* The BOOT volume's PBR reloads this loader.  It must not be the
		 * fallback target when ZEDBSD.CFG is absent, or Auto loops forever. */
		if (first_bootable < 0 && parts[partition].bootable &&
		    !streq(parts[partition].name, "BOOT"))
			first_bootable = partition;
		if (config_partition < 0 && streq(parts[partition].name, "BOOT") &&
		    mountpart(device, partition) &&
		    (config_kind = boot_volume_startup_kind()) !=
			    STARTUP_CONFIG_NONE)
			config_partition = partition;
	}
	if (config_partition >= 0 && state->auto_priority > 1) {
		/* Discovery order is stable: keep the first BOOT volume found. */
		state->auto_priority = 1;
		state->auto_kind = STARTUP_AUTO_CONFIG;
		state->auto_config_kind = config_kind;
		state->auto_device = device;
		state->auto_partition = config_partition;
	}
	if (first_bootable >= 0 && 3 < state->auto_priority) {
		state->auto_priority = 3;
		state->auto_kind = STARTUP_AUTO_PBR;
		state->auto_device = device;
		state->auto_partition = first_bootable;
	}
}

static int activate_automatic_target(const struct startup_state *state)
{
	if (state->auto_kind == STARTUP_AUTO_NONE ||
	    !scanparts(state->auto_device) ||
	    !parts[state->auto_partition].valid)
		return 0;
	if (state->auto_kind == STARTUP_AUTO_CONFIG &&
	    (!mountpart(state->auto_device, state->auto_partition) ||
	     boot_volume_startup_kind() == STARTUP_CONFIG_NONE))
		return 0;
	curdev = state->auto_device;
	curpart = state->auto_partition;
	select_disk_home(curdev);
	kernel_name[0] = kernel_arg[0] = 0;
	return 1;
}

/* AUTOEXEC.NCT may select one action, but it cannot inject a second shell
 * line or leave a stale action behind for a later VM invocation. */
static int valid_boot_action(const char *action)
{
	unsigned length = 0;
	int non_space = 0;

	if (action == 0)
		return 0;
	while (action[length] != 0) {
		unsigned char ch = (unsigned char)action[length++];

		if (length >= LINE_MAX || ch < 0x20U || ch == 0x7fU)
			return 0;
		if (ch != ' ' && ch != '\t')
			non_space = 1;
	}
	return non_space;
}

/* Return zero when no graphical startup script exists, one after executing
 * its selected action, and -1 when the script/action failed validation. */
int run_autoexec(void)
{
	struct inode *inode;
	char action[LINE_MAX];
	int script_ok;

	if (namei_at(&kern_cwdinfo, "AUTOEXEC.NCT", &inode) != 0)
		return 0;
	inode_release(inode);
	action[0] = '\0';
	script_ok = run_noct_user("AUTOEXEC.NCT", 0, NULL,
				  PROCESS_SPAWN_RESULT, action, sizeof(action));
	/* A graphical script may have owned Cirrus or GDC graphics.  Restore the
	 * firmware text display and erase every GDC graphics plane before its
	 * selected zedBSD command runs.  Real Cirrus-equipped machines retain the
	 * old graphics VRAM contents when the display is switched back to GDC. */
	kern_platform_restore_text();
	hal_cons_reset();
	hal_cons_set_mode(HAL_CONS_TERMINAL);
	if (!script_ok) {
		puts("AUTOEXEC.NCT failed (status ");
		dec((unsigned)(kern_noct_last_status < 0 ?
			-kern_noct_last_status : kern_noct_last_status));
		puts(kern_noct_last_status < 0 ?
			", launch error); returning to the text shell.\n" :
			"); returning to the text shell.\n");
		if (action[0] != '\0') {
			puts("NOCT.ELF: ");
			puts(action);
			putc('\n');
		}
		return -1;
	}
	if (!valid_boot_action(action)) {
		puts("AUTOEXEC.NCT did not select a valid BOOT_ACTION.\n");
		return -1;
	}
	if (!command(action)) {
		puts("BOOT_ACTION failed: ");
		puts(action);
		putc('\n');
		return -1;
	}
	return 1;
}

static void draw_startup_header(void)
{
	hal_cons_write_at(0, 0, zedbsd_msg_machine);
	hal_cons_write_at(2, 0, zedbsd_msg_loader);
	hal_cons_write_at(3, 0, zedbsd_msg_copyright);
	hal_cons_write_at(5, 0, zedbsd_msg_probing);
}

static void draw_probe_bar(unsigned current, unsigned total)
{
	char filled[HAL_CONS_COLUMNS + 1U];
	char empty[HAL_CONS_COLUMNS + 1U];
	unsigned columns = total ? current * HAL_CONS_COLUMNS / total : 0;
	unsigned index;

	if (columns > HAL_CONS_COLUMNS)
		columns = HAL_CONS_COLUMNS;
	for (index = 0; index < columns; index++)
		filled[index] = ' ';
	filled[index] = 0;
	for (index = 0; index < HAL_CONS_COLUMNS - columns; index++)
		empty[index] = ' ';
	empty[index] = 0;
	hal_cons_write_at_attr(HAL_CONS_ROWS - 1U, 0,
				   filled,
				   HAL_CONS_NORMAL_ATTRIBUTE | 0x04U);
	if (columns < HAL_CONS_COLUMNS)
		hal_cons_write_at_attr(HAL_CONS_ROWS - 1U, columns,
					   empty,
					   HAL_CONS_NORMAL_ATTRIBUTE);
}

static void draw_probe_progress(unsigned current, unsigned total,
				uint8_t device_class, uint8_t bios_id)
{

	hal_cons_clear_row(5);
	hal_cons_write_at(5, 0, zedbsd_msg_probing);
	putc(' ');
	puts(device_class == ZEDBSD_DEV_IDE ? "IDE " : "SCSI ");
	dec((unsigned)bios_id -
	    (device_class == ZEDBSD_DEV_IDE ? 0x80U : 0xa0U) + 1U);
	puts(" (");
	dec(current);
	putc('/');
	dec(total);
	putc(')');
	draw_probe_bar(current, total);
}

static void draw_automatic_status(const struct startup_state *state)
{
	draw_probe_bar(state->probe_total, state->probe_total);
	hal_cons_clear_row(5);
	hal_cons_write_at(5, 0, zedbsd_msg_automatic_run);
	if (state->auto_config_kind == STARTUP_CONFIG_AUTOEXEC)
		puts(" AUTOEXEC.NCT");
	else if (state->auto_config_kind == STARTUP_CONFIG_BOOTCFG)
		puts(" ZEDBSD.CFG");
}

static void draw_startup_menu(const struct startup_state *state)
{
	for (unsigned menu_row = 6; menu_row <= 17; menu_row++)
		hal_cons_clear_row(menu_row);
	hal_cons_write_at(6, 0, "");
	dec(state->fixed_count);
	puts((const char *)zedbsd_msg_found_suffix);
	hal_cons_write_at(8, 0, zedbsd_msg_boot_from);
	hal_cons_write_at(9, 0, zedbsd_msg_auto_prefix);
	if (state->phase == STARTUP_DRAW || state->phase == STARTUP_PROBE) {
		puts((const char *)zedbsd_msg_searching);
	} else if (state->auto_kind != STARTUP_AUTO_NONE) {
		puts(devs[state->auto_device].device_class == ZEDBSD_DEV_FDD ?
		     "FDD " : "HDD ");
		dec(fixed_device_ordinal(state->auto_device));
		puts((const char *)zedbsd_msg_partition);
		dec((unsigned)state->auto_partition + 1);
		if (state->auto_kind == STARTUP_AUTO_CONFIG) {
			if (state->auto_config_kind == STARTUP_CONFIG_AUTOEXEC)
				puts((const char *)zedbsd_msg_run_autoexec);
			else
				puts((const char *)zedbsd_msg_run_cfg);
		}
	} else {
		puts((const char *)zedbsd_msg_unavailable);
	}
	putc(')');

	for (unsigned ordinal = 1; ordinal <= 4; ordinal++) {
		if (menu_device(ordinal) < 0)
			continue;
		hal_cons_write_at(9 + ordinal, 0,
					"  ");
		putc((char)('1' + ordinal));
		puts((const char *)zedbsd_msg_fixed_disk_prefix);
		dec(ordinal);
	}
	hal_cons_write_at(15, 0, zedbsd_msg_esc_shell);
	hal_cons_write_at(17, 0, zedbsd_msg_select);
	update_cursor();
}

static void accept_startup_selection(int key_code)
{
	hal_cons_clear_row(HAL_CONS_ROWS - 1U);
	if (key_code == 0x1b)
		puts("ESC");
	else if (key_code >= 0)
		putc((char)key_code);
	putc('\n');
	hal_cons_set_mode(HAL_CONS_TERMINAL);
}

static void chain_menu_device(unsigned ordinal)
{
	int di = menu_device(ordinal);
	if (di < 0) {
		puts("Device is not present.\n");
		return;
	}
	curdev = di;
	curpart = -1;
	kernel_name[0] = kernel_arg[0] = 0;
	puts("Chain boot is not available on the HAL yet.\n");
}

static void chain_automatic_partition(const struct startup_state *state)
{
	if (!activate_automatic_target(state)) {
		puts("Automatic target is no longer readable.\n");
		return;
	}
	puts("Chain boot is not available on the HAL yet.\n");
}

/* Return -1 for an ignored key, zero for Shell, and one for ZEDBSD.CFG. */
static int handle_startup_key(struct startup_state *state, int key_code)
{
	if (key_code == 0x1b) {
		accept_startup_selection(key_code);
		state->phase = STARTUP_SHELL;
		if (state->auto_kind == STARTUP_AUTO_CONFIG)
			activate_automatic_target(state);
		return 0;
	}
	if (key_code == '1') {
		if (state->auto_kind == STARTUP_AUTO_NONE)
			return -1;
		accept_startup_selection(key_code);
		state->phase = STARTUP_SELECTED;
		if (state->auto_kind == STARTUP_AUTO_CONFIG)
			return activate_automatic_target(state) ? 1 : 0;
		chain_automatic_partition(state);
		state->phase = STARTUP_SHELL;
		return 0;
	}
	if (key_code >= '2' && key_code <= '5') {
		unsigned ordinal = (unsigned)(key_code - '1');

		if (menu_device(ordinal) < 0)
			return -1;
		accept_startup_selection(key_code);
		state->phase = STARTUP_SELECTED;
		chain_menu_device(ordinal);
		state->phase = STARTUP_SHELL;
		return 0;
	}
	return -1;
}

static int pending_startup_key(void)
{
	return poll() >= 0 ? key() : -1;
}

/* Process one stable candidate; at most one invocation reaches INT 1Bh. */
static void probe_next_startup_device(struct startup_state *state)
{
	unsigned candidate;
	uint8_t device_class;
	uint8_t bios_id;
	int new_device;

	for (;;) {
		if (state->next_candidate >= MAX_FIXED_DEVICES)
			return;
		candidate = state->next_candidate++;
		if (candidate < MAX_IDE_DEVICES) {
			device_class = ZEDBSD_DEV_IDE;
			bios_id = 0x80 + candidate;
			if (state->ide_bitmap & (1U << candidate))
				break;
			continue;
		}
		device_class = ZEDBSD_DEV_SCSI;
		bios_id = 0xa0 + candidate - MAX_IDE_DEVICES;
		if (state->scsi_bitmap &
		    (1U << (candidate - MAX_IDE_DEVICES)))
			break;
	}
	state->probe_done++;
	draw_probe_progress(state->probe_done, state->probe_total,
			    device_class, bios_id);
	new_device = probe_fixed_device(device_class, bios_id);
	state->fixed_count = device_count;
	if (new_device >= 0)
		consider_automatic_device(state, new_device);
}

/* Explicit cooperative startup state machine.  BIOS SENSE itself may block,
 * but keyboard input is checked immediately before and after every candidate. */
int startup_menu(struct startup_state *state)
{
	curdev = curpart = -1;
	zedbsd_namespace_init(&mounted_namespace);
	state->phase = STARTUP_DRAW;
	state->next_candidate = 0;
	state->ide_bitmap = ide_reported_drives();
	state->scsi_bitmap = scsi_reported_targets();
	state->probe_total = bit_count(state->ide_bitmap) +
			     bit_count(state->scsi_bitmap);
	state->probe_done = 0;
	state->fixed_count = device_count;
	state->auto_device = state->auto_partition = -1;
	state->auto_priority = 4;
	state->auto_kind = STARTUP_AUTO_NONE;
	state->auto_config_kind = STARTUP_CONFIG_NONE;
	state->automatic_cancelled = 0;
	state->timeout_start = -1;
	state->timeout_budget = 0x20000;

	hal_cons_reset();
	draw_startup_header();
	for (unsigned device = 0; device < device_count; device++)
		consider_automatic_device(state, device);
	draw_startup_menu(state);
	state->phase = STARTUP_PROBE;
	for (;;) {
		int key_code;
		int result;

		if (state->phase == STARTUP_PROBE) {
			key_code = pending_startup_key();
			if (key_code >= 0) {
				state->automatic_cancelled = 1;
				if ((result = handle_startup_key(state, key_code)) >= 0)
					return result;
			}
			if (state->next_candidate < MAX_FIXED_DEVICES) {
				probe_next_startup_device(state);
				draw_startup_menu(state);
				key_code = pending_startup_key();
				if (key_code >= 0) {
					state->automatic_cancelled = 1;
					if ((result = handle_startup_key(state,
								 key_code)) >= 0)
						return result;
				}
				continue;
			}
			state->phase = STARTUP_TIMEOUT;
			state->timeout_start = clock_second();
			if (!state->automatic_cancelled &&
			    state->auto_kind != STARTUP_AUTO_NONE)
				draw_automatic_status(state);
			draw_startup_menu(state);
			continue;
		}

		if (state->auto_kind == STARTUP_AUTO_NONE ||
		    state->automatic_cancelled) {
			key_code = key();
			result = handle_startup_key(state, key_code);
			if (result >= 0)
				return result;
			continue;
		}

		key_code = pending_startup_key();
		if (key_code >= 0) {
			state->automatic_cancelled = 1;
			if ((result = handle_startup_key(state, key_code)) >= 0)
				return result;
			continue;
		}
		int now = clock_second();
		if ((state->timeout_start >= 0 && now >= 0 &&
		     (now - state->timeout_start + 60) % 60 >=
		     STARTUP_TIMEOUT_SECONDS) || !--state->timeout_budget) {
			accept_startup_selection(-1);
			state->phase = STARTUP_SELECTED;
			if (state->auto_kind == STARTUP_AUTO_CONFIG)
				return activate_automatic_target(state) ? 1 : 0;
			chain_automatic_partition(state);
			state->phase = STARTUP_SHELL;
			return 0;
		}
	}
}

