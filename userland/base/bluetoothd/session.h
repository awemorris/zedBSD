/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One controller's session in bluetoothd (ws143-p003, ws143-p004): the H4
 * packets read and written on its node (/dev/bluetoothN, or a socket pair in the
 * host tests), one command at a time, the start (Intel's firmware load,
 * then the HCI core's set-up), the scan, and the ACL data path with the
 * controller's buffers counted (the BR/EDR pool and LE's).
 *
 * A packet that comes while a command waits for its answer is queued and
 * handed out later, from btd_session_input, so the handler that gets the
 * connections' events and ACL packets is never called inside a command.
 *
 * The session uses only read, write, poll, open of the firmware files and,
 * on a node, its ioctls; the host tests run it against a scripted
 * controller.
 */

#ifndef BLUETOOTHD_SESSION_H
#define BLUETOOTHD_SESSION_H

#include "userland/base/bluetoothd/acl.h"
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

/* The bytes of the queue of packets that came while a command waited. */
#define BTD_QUEUE_BYTES		(32U * 1024U)

/*
 * The queue's room kept (ws197-p002, plan/ws197/phase002/phase.md section
 * 3.2): the last bytes for the connections' Connection and Disconnection
 * Complete alone, and more before them for the events, which ACL data and
 * the scans' reports do not take.
 */
#define BTD_QUEUE_COUNTED_RESERVE	1024U
#define BTD_QUEUE_EVENT_RESERVE		4096U

/*
 * The notice of packets dropped (ws197-p002 section 3.4), a packet of
 * bluetoothd's own that the handler gets after every packet that came
 * before the drop: the type 0xF0 (no H4 type, and the kernel's notices are
 * one byte long; it never comes from the node and is never traced), the
 * handle (0xFFFF for events not of one connection), the flags, the first
 * data channel dropped and the count.
 */
#define BTD_PACKET_DROP		0xf0U
#define BTD_DROP_LENGTH		8U
#define BTD_DROP_ALL		0xffffU
#define BTD_DROP_SIGNAL		0x01U
#define BTD_DROP_DATA		0x02U
#define BTD_DROP_UNKNOWN	0x04U
#define BTD_DROP_EVENT		0x08U
#define BTD_DROP_COUNTED	0x10U

/* How many L2CAP frames may wait for the controller's buffers (a phone's link, at most half of them), and the longest. */
#define BTD_SEND_FRAMES		16U
#define BTD_SEND_PHONE_FRAMES	8U
#define BTD_SEND_FRAME_MAX	(BTD_L2CAP_HEADER + BTD_L2CAP_MAX)

/* How many connections' sent packets are counted. */
#define BTD_LINKS_MAX		8U

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

struct btd_session;

/*
 * The handler of the packets the session does not take itself: the
 * connections' events (Connection Complete, the pairing's events, LE's
 * connection events) and every ACL packet.  It is called from
 * btd_session_input only, and may send commands.
 */
typedef void (*btd_handler_fn)(void *context, struct btd_session *session, const uint8_t *packet, size_t length);

/*
 * The trace of every H4 packet the session writes (received 0) or reads
 * (received 1), the type's byte first: the daemon's btsnoop record
 * (ws143-p005, plan/ws143/phase005/phase.md section 9.11).
 */
typedef void (*btd_trace_fn)(void *context, const uint8_t *packet, size_t length, int received);

/*
 * One pool of the controller's ACL buffers: the longest packet's data and
 * how many packets it holds, and how many are free now.  BR/EDR has one;
 * LE has its own unless LE Read Buffer Size said it shares BR/EDR's.
 */
struct btd_pool {
	uint16_t length;
	unsigned total;
	unsigned free;
};

/*
 * A connection whose sent packets are counted: its handle, its device's
 * address, whether it is LE, and how many of its packets the controller
 * has not completed yet.  The session adds it on a Connection Complete (or
 * LE's) and removes it on the Disconnection Complete, which gives its
 * packets back to the pool.
 *
 * ws197-p002: its frames waiting and the most it may have waiting and in
 * the controller (0: no limit but the pool); and its drops.  sealed says a
 * packet of it was dropped and every later one is dropped too until its
 * notice is handed (notice_after: the count of queued packets that came
 * before the drop, handed first); skip_continuing drops continuing packets
 * after the notice until a first one; last_cid is the channel of the last
 * first packet that came, in the order they came.
 */
struct btd_link_count {
	int used;
	uint16_t handle;
	uint8_t address[BTD_ADDRESS_BYTES];
	int le;
	unsigned outstanding;
	unsigned frames;
	unsigned frame_limit;
	unsigned inflight_limit;
	int sealed;
	int skip_continuing;
	uint8_t drop_flags;
	uint16_t drop_cid;
	unsigned drop_count;
	uint32_t notice_after;
	uint16_t last_cid;
};

/*
 * An L2CAP frame waiting for the controller's buffers: its connection and
 * how much of it was sent already.
 */
struct btd_frame {
	uint16_t handle;
	size_t length;
	size_t sent;
	uint8_t bytes[BTD_SEND_FRAME_MAX];
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
	int ssp;
	int secure_connections;
	int no_flush;
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
	unsigned buffer_overflows;

	/* The handler of the connections' packets (NULL: they are dropped), and its context. */
	btd_handler_fn handler;
	void *handler_context;

	/* The trace of the packets (NULL: none), and its context. */
	btd_trace_fn packet_trace;
	void *packet_trace_context;

	/*
	 * The packets that came while a command waited, each a 2-byte length
	 * (least significant first), a byte of flags and the packet, oldest
	 * at queue_head; and the packets dropped because the queue was full.
	 */
	size_t queue_head;
	size_t queue_used;
	unsigned queue_dropped;
	uint8_t queue[BTD_QUEUE_BYTES];

	/*
	 * ws197-p002: the packets queued and taken from the queue since the
	 * start (they wrap; compared by their difference), the events that are
	 * not of one connection dropped (their notice due after
	 * events_notice_after packets were taken, events_flags its flags), the
	 * scans' reports dropped, the continuing packets passed over after a
	 * notice, and the link the next flush starts with.
	 */
	uint32_t enqueued;
	uint32_t dequeued;
	int events_noticed;
	uint8_t events_flags;
	uint32_t events_notice_after;
	unsigned events_dropped;
	unsigned scan_dropped;
	unsigned continuing_skipped;
	unsigned flush_next;

	/* The controller's ACL buffers: BR/EDR's pool, LE's, and whether LE shares BR/EDR's. */
	struct btd_pool acl_pool;
	struct btd_pool le_pool;
	int le_shared;

	/* The connections whose packets are counted, and the frames waiting for buffers (oldest first). */
	struct btd_link_count links[BTD_LINKS_MAX];
	unsigned frame_count;
	unsigned frames_dropped;
	struct btd_frame frames[BTD_SEND_FRAMES];

	/*
	 * The packet last read, the return parameters of the last answer, a
	 * command being written, the packet handed to the handler (a copy, so
	 * the handler's commands do not overwrite it), and an ACL packet being
	 * written.
	 */
	size_t packet_length;
	int packet_counted;
	uint8_t packet[BT_ACL_PACKET_MAX];
	uint8_t status;
	size_t returned_length;
	uint8_t returned[256];
	uint8_t command[BT_COMMAND_PACKET_MAX];
	uint8_t handed[BT_ACL_PACKET_MAX];
	uint8_t outgoing[BT_ACL_PACKET_MAX];
};

void btd_session_init(struct btd_session *session, int descriptor, btd_control_fn control, void *control_context, const char *path, const char *firmware_folder);
int btd_session_start(struct btd_session *session);
int btd_session_input(struct btd_session *session);
int btd_session_scan_start(struct btd_session *session, unsigned seconds);
int btd_session_scan_stop(struct btd_session *session);
int btd_session_pending(const struct btd_session *session);
int btd_session_command(struct btd_session *session, uint16_t opcode, const uint8_t *parameters, size_t count);
int btd_session_send(struct btd_session *session, uint16_t handle, uint16_t cid, const uint8_t *payload, size_t length);
int btd_session_set_link_limits(struct btd_session *session, uint16_t handle, unsigned frame_limit, unsigned inflight_limit);
unsigned btd_session_link_room(const struct btd_session *session, uint16_t handle);
unsigned btd_session_links(const struct btd_session *session, uint16_t *handles, uint8_t (*addresses)[BTD_ADDRESS_BYTES], unsigned max);
const char *btd_state_name(enum btd_state state);
uint64_t btd_now_ms(void);

#endif
