/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One controller's session in bluetoothd (ws143-p003): the H4 packets read
 * and written on its node (/dev/btN, or a socket pair in the host tests),
 * one command at a time, the start (Intel's firmware load, then the HCI
 * core's set-up), and the scan.
 *
 * The session uses only read, write, poll, open of the firmware files and,
 * on a node, its ioctls; the host tests run it against a scripted
 * controller.
 */

#ifndef BLUETOOTHD_SESSION_H
#define BLUETOOTHD_SESSION_H

#include "userland/base/bluetoothd/hci.h"
#include "userland/base/bluetoothd/intel.h"

#include <stddef.h>
#include <stdint.h>
#include <uapi/bluetooth.h>

/* The longest reason kept for a state, and the longest node path. */
#define BTD_REASON_MAX		160U
#define BTD_PATH_MAX		64U

/* The longest firmware file read. */
#define BTD_FIRMWARE_MAX	(4U * 1024U * 1024U)

/* The longest scan, in seconds. */
#define BTD_SCAN_SECONDS_MAX	30U

/*
 * A session's state, as the socket's STATE line names it: no controller,
 * starting, ready, scanning, a firmware file missing, its load failed, a
 * controller this daemon cannot use, a step of the start failed, or the
 * node gone.
 */
enum btd_state {
	BTD_STATE_NONE = 0,
	BTD_STATE_STARTING,
	BTD_STATE_READY,
	BTD_STATE_SCANNING,
	BTD_STATE_FIRMWARE_NEEDED,
	BTD_STATE_FIRMWARE_FAILED,
	BTD_STATE_UNSUPPORTED,
	BTD_STATE_ERROR,
	BTD_STATE_LOST
};

/* How long the session waits: for a command's answer, for the download's end and for the boot (milliseconds). */
struct btd_timing {
	unsigned command_ms;
	unsigned download_ms;
	unsigned boot_ms;
};

/*
 * The node's ioctls (BT_IOC_*): the daemon's calls ioctl(2) on the node;
 * the host tests' stand-in answers with a made-up controller and records
 * the calls.  Returns 0 or an errno value.
 */
typedef int (*btd_control_fn)(void *context, unsigned long request, void *argument);

/*
 * A controller's session.  The daemon owns it while the node is open; the
 * fields after the controller's are what the start learnt.
 */
struct btd_session {
	/* The node, its ioctls (NULL: none), its path, and the firmware folder. */
	int descriptor;
	btd_control_fn control;
	void *control_context;
	char path[BTD_PATH_MAX];
	const char *firmware_folder;
	struct btd_timing timing;

	/*
	 * Whether the start may load Intel's firmware (the daemon says no for a
	 * controller whose load was tried and that came back in its bootloader,
	 * plan section 3), and whether this start sent a download.
	 */
	int load_allowed;
	int load_sent;

	/* The first answers to Secure Send, as hex, for the log (the form is checked on the 5330, i02). */
	char trace[BTD_REASON_MAX];

	/* The state and why. */
	enum btd_state state;
	char reason[BTD_REASON_MAX];

	/* The node's information (a node only). */
	int have_info;
	struct bt_info info;

	/* What the start learnt of the controller. */
	int have_address;
	uint8_t address[BTD_ADDRESS_BYTES];
	uint8_t hci_version;
	uint16_t hci_revision;
	uint16_t manufacturer;
	int have_commands;
	uint8_t commands[BTD_COMMANDS_BYTES];
	int le;
	int le_supported;
	int p256;
	int dhkey;
	uint16_t acl_length;
	int intel;
	struct btd_intel_version intel_version;
	int firmware_loaded;

	/* The scan: the devices found, whether one runs, which parts, and when it ends. */
	struct btd_devices devices;
	int scanning;
	int inquiring;
	int le_scanning;
	uint64_t scan_end_ms;

	/* What the session passed over: answers to another command, malformed packets, a reset under it. */
	unsigned stray_answers;
	unsigned malformed;
	unsigned resets;
	unsigned hardware_errors;

	/* The packet last read, the return parameters of the last answer, and a command being written. */
	size_t packet_length;
	uint8_t packet[BT_ACL_PACKET_MAX];
	size_t returned_length;
	uint8_t returned[256];
	uint8_t command[BT_COMMAND_PACKET_MAX];
};

void btd_session_init(struct btd_session *session, int descriptor, btd_control_fn control, void *control_context, const char *path, const char *firmware_folder);
int btd_session_start(struct btd_session *session);
int btd_session_input(struct btd_session *session);
int btd_session_scan_start(struct btd_session *session, unsigned seconds);
int btd_session_scan_stop(struct btd_session *session);
const char *btd_state_name(enum btd_state state);
uint64_t btd_now_ms(void);

#endif
