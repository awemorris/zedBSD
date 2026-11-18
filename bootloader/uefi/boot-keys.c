/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot keys as the UEFI loader reads them.
 *
 * Ctrl held while the loader runs selects the kernel messages on the
 * console, and Shift selects the console login.  Only the extended console
 * input reports the modifier keys' state reliably, so a firmware without it
 * boots as if no key were held.  Nothing here waits: the loader samples the
 * key queue and the modifier keys held at that moment at fixed points, and
 * the boot is never slowed down.
 */

#include "boot-keys.h"

#include "../common/boot-override.h"

/*
 * The most key events one sample reads.
 *
 * A firmware may keep answering with an empty event and the current
 * modifier state while modifier keys are exposed, so draining the queue
 * needs a bound to end at all.
 */
#define BOOT_KEYS_SAMPLE_MAX	32U

/*
 * Finds the firmware's extended console input and asks for modifier keys.
 *
 * Asking for exposed modifier keys lets a firmware report Ctrl or Shift
 * pressed alone; without it only a modifier held with another key, such as
 * Space, is reported.  The request also rewrites the lock keys' state.  No
 * failure here stops the boot: the keys are then simply not read.
 */
void
zbl_uefi_boot_keys_open(
	struct zbl_uefi_boot_keys *keys,
	EFI_SYSTEM_TABLE *system)
{
	EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *input;
	EFI_BOOT_SERVICES *boot;
	EFI_STATUS status;
	uint8_t toggle;
	int failed;

	/* Starts with no input found and no key seen. */
	keys->input = 0;
	keys->exposed = 0;
	keys->held = 0U;

	/* A system table without boot services offers no keyboard. */
	if (system == 0 || system->BootServices == 0)
		return;

	/* Asks the console input handle for the extended protocol. */
	boot = system->BootServices;
	input = 0;
	status = EFI_NOT_FOUND;
	if (system->ConsoleInHandle != 0) {
		status = boot->HandleProtocol(
			system->ConsoleInHandle,
			&EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID,
			(void **)&input);
	}

	/* Falls back to any instance the firmware has. */
	failed = EFI_ERROR(status);
	if (failed || input == 0) {
		input = 0;
		status = boot->LocateProtocol(
			&EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID,
			0,
			(void **)&input);
	}

	/* A firmware without the extended input boots with no keys. */
	failed = EFI_ERROR(status);
	if (failed || input == 0)
		return;

	/* The samples read this input from now on. */
	keys->input = input;

	/* Asks for modifier keys pressed alone to be reported too. */
	toggle = EFI_TOGGLE_STATE_VALID | EFI_KEY_STATE_EXPOSED;
	status = input->SetState(input, &toggle);
	failed = EFI_ERROR(status);
	if (failed)
		return;

	/* The firmware reports modifier keys pressed alone. */
	keys->exposed = 1;
}

/*
 * Reads the queued key events and the modifier keys held now, and reports
 * every boot key seen so far.
 *
 * The events read are consumed; the kernel's keyboard driver starts from
 * its own state, so the keys typed while the loader runs never reached the
 * kernel anyway.  The held modifiers come with the answer that the queue
 * is empty, which the extended input fills with the current key state.
 */
unsigned
zbl_uefi_boot_keys_sample(
	struct zbl_uefi_boot_keys *keys)
{
	EFI_KEY_DATA data;
	EFI_STATUS status;
	unsigned count;
	unsigned seen;
	int failed;

	/* Without the extended input nothing new can be seen. */
	if (keys->input == 0)
		return keys->held;

	/* Drains the queue, a bounded number of events at most. */
	for (count = 0U; count < BOOT_KEYS_SAMPLE_MAX; count++) {
		/* Clears the event, so a firmware that fills only part of it adds no stale bits. */
		data.Key.ScanCode = 0U;
		data.Key.UnicodeChar = 0U;
		data.KeyState.KeyShiftState = 0U;
		data.KeyState.KeyToggleState = 0U;

		/* Reads the next event, or learns that the queue is empty. */
		status = keys->input->ReadKeyStrokeEx(keys->input, &data);

		/*
		 * An empty queue still reports the modifier keys held now, so a
		 * key held down is seen even when its events never reached the
		 * loader: the firmware may have read them before starting it.
		 * A firmware that leaves the state alone adds nothing, as the
		 * cleared state is not marked valid.
		 */
		if (status == EFI_NOT_READY) {
			seen = zbl_uefi_boot_keys_from_state(&data);
			keys->held |= seen;
			break;
		}

		/* Any other error ends the sample; its data is not trusted. */
		failed = EFI_ERROR(status);
		if (failed)
			break;

		/* Gathers the boot keys this event was typed with. */
		seen = zbl_uefi_boot_keys_from_state(&data);
		keys->held |= seen;
	}

	/* Reports every boot key seen in this and the earlier samples. */
	return keys->held;
}

/*
 * Reports the boot keys one key event was typed with.
 *
 * Only the modifier state counts; the key itself (none, Space, or anything
 * else) does not.  Either Ctrl selects the kernel messages, and either
 * Shift selects the console login.
 */
unsigned
zbl_uefi_boot_keys_from_state(
	const EFI_KEY_DATA *data)
{
	UINT32 shift_state;
	unsigned bits;

	/* The modifier bits mean nothing unless the firmware marks them valid. */
	shift_state = data->KeyState.KeyShiftState;
	if ((shift_state & EFI_SHIFT_STATE_VALID) == 0U)
		return 0U;

	/* Either Ctrl key selects the kernel messages on the console. */
	bits = 0U;
	if ((shift_state & (EFI_LEFT_CONTROL_PRESSED | EFI_RIGHT_CONTROL_PRESSED)) != 0U)
		bits |= ZBL_BOOT_OVERRIDE_KMSG;

	/* Either Shift key selects the console login. */
	if ((shift_state & (EFI_LEFT_SHIFT_PRESSED | EFI_RIGHT_SHIFT_PRESSED)) != 0U)
		bits |= ZBL_BOOT_OVERRIDE_LOGIN;

	/* Reports the boot keys of the event. */
	return bits;
}
