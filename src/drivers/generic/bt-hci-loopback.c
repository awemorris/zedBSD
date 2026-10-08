/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test kernel's loopback Bluetooth controller (ws143-p002): an HCI
 * class controller with no hardware, so that /dev/bluetoothN can be tried in QEMU,
 * which has no Bluetooth controller.  Test builds only
 * (CONFIG_BT_TEST_LOOPBACK, as the loopback security key and card of
 * ws161 are).
 *
 * It answers as far as the class's tests need:
 *
 *   a command 0xFC01   Command Complete after an event, an ACL packet, an
 *                      event and an ACL packet, in that order (the two
 *                      queues' order)
 *   a command 0xFC02   Command Complete, then N vendor events (0xFF) of 255
 *                      bytes, N the two parameter bytes (little-endian),
 *                      each carrying its index: more than the events' queue
 *                      holds (the backpressure and the notices' reserve)
 *   a command 0xFC03   Command Complete; LOOPBACK_WITHDRAW_MS (or the
 *                      milliseconds of its two parameter bytes, at most
 *                      LOOPBACK_DELAY_MOST, ws143-p003) later the
 *                      controller is withdrawn and released (a detach), and
 *                      LOOPBACK_RETURN_MS after that it is registered again
 *   0x1001, 0x1009     Read Local Version Information, Read BD_ADDR
 *   0x1002, 0x1003     Read Local Supported Commands (Inquiry, LE's
 *                      event mask and scan, P-256 and DHKey), Read Local
 *                      Supported Features (LE), ws143-p003
 *   0x1005             Read Buffer Size (ACL 1021 bytes, 8 packets)
 *   0x0401             Inquiry: Command Status, then an Extended Inquiry
 *                      Result ("Loopback Keyboard", class 0x002540), an
 *                      Inquiry Result with RSSI (class 0x002580) and
 *                      Inquiry Complete (ws143-p003)
 *   0x200C on          LE Set Scan Enable: Command Complete, then two
 *                      advertising reports: a public address with
 *                      "Loopback Mouse" and appearance 0x03C2, a random one
 *                      without a name (ws143-p003)
 *   any other command  Command Complete with status 0
 *   an ACL packet      the same packet back (handle 0x001)
 *
 * and plays devices to pair with (ws143-p004, plan/ws143/phase004/
 * phase.md section 8), the controller's side of Secure Simple Pairing
 * included.  The event masks and Simple Pairing's mode are kept: an event
 * the masks leave out is not sent (they start as the Core's defaults, and
 * Reset puts them back), and without Simple Pairing a link key's absence
 * asks for a PIN.  The BR/EDR devices are 0A:0B:0C:0D:0E:xx with xx 01
 * (DisplayYesNo, Numeric Comparison 123456) on handle 0x040, 02
 * (NoInputNoOutput, Just Works) on handle 0x043, and 05 (gives the debug
 * key), 06 (a key of 7 bytes) and 07 (NoInputNoOutput, Just Works) on
 * handle 0x045, each answering L2CAP's Information Request; any other
 * address is a page timeout.  The LE device 0A:0B:0C:0D:0E:03 connects on
 * handle 0x041 and refuses pairing (Pairing Not Supported); any other LE
 * address never connects (a cancel ends it).  Each ACL packet to a
 * device's handle is completed (Number Of Completed Packets).
 *
 * 01 and 02 are HID devices too (ws143-p005, plan/ws143/phase005/phase.md
 * section 6), each with its own connection: they serve SDP (their HID and
 * PnP records, in fragments of 60 bytes over continuations), the HID
 * control and interrupt channels, and HANDSHAKE to SET_PROTOCOL.  01, a
 * boot keyboard, presses 'a' as its interrupt channel opens, lets it go
 * 200 ms later, holds 'b' from 300 ms, and then presses and lets go 'a'
 * every 3 seconds.  02, a boot mouse that reconnects by itself and cannot
 * be paged once paired, moves X by 5 every second; 4 seconds into its first
 * connection it goes (supervision timeout) and a second later, when page
 * scan is on, it connects again by itself and asks for its control channel
 * before any security.  The worker sends these timed reports.
 *
 * 0A:0B:0C:0D:0E:04 is an LE HOG mouse (ws143-p005 i03), bonded already
 * (the test writes its bond: LTK 00 11 .. FF, EDIV 0, Rand 0) and not
 * advertised in the scan: it connects on handle 0x044 to LE Create
 * Connection at its address, or from the filter accept list when it is
 * listed (with Enhanced Connection Complete when its LE mask bit is on),
 * asks its MTU at once, takes LE Enable Encryption with that key only,
 * serves a fixed GATT table (the HID service's reads need encryption), and
 * once its report's CCC is on notifies X +5 every second.
 *
 * The reset (BT_IOC_RESET) drops what is not delivered yet and queues the
 * reset's notice.  A worker thread delivers everything; a packet the class
 * has no room for waits for the room call, so nothing is dropped.
 */

#include <drivers/generic/bt-hci.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

/* Marks a parameter a function takes for its signature's sake. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* The test commands (OGF 0x3F, vendor-specific). */
#define LOOPBACK_OP_ORDER		0xfc01U
#define LOOPBACK_OP_FLOOD		0xfc02U
#define LOOPBACK_OP_WITHDRAW		0xfc03U

/* The standard commands it answers with their own return parameters. */
#define LOOPBACK_OP_INQUIRY		0x0401U
#define LOOPBACK_OP_CONNECT		0x0405U
#define LOOPBACK_OP_DISCONNECT		0x0406U
#define LOOPBACK_OP_KEY_REPLY		0x040bU
#define LOOPBACK_OP_KEY_NEGATIVE	0x040cU
#define LOOPBACK_OP_PIN_NEGATIVE	0x040eU
#define LOOPBACK_OP_AUTHENTICATE	0x0411U
#define LOOPBACK_OP_ENCRYPT		0x0413U
#define LOOPBACK_OP_IO_REPLY		0x042bU
#define LOOPBACK_OP_CONFIRM_REPLY	0x042cU
#define LOOPBACK_OP_CONFIRM_NEGATIVE	0x042dU
#define LOOPBACK_OP_SET_EVENT_MASK	0x0c01U
#define LOOPBACK_OP_RESET		0x0c03U
#define LOOPBACK_OP_SSP_MODE		0x0c56U
#define LOOPBACK_OP_KEY_SIZE		0x1408U
#define LOOPBACK_OP_LE_EVENT_MASK	0x2001U
#define LOOPBACK_OP_LE_BUFFER_SIZE	0x2002U
#define LOOPBACK_OP_LE_CONNECT		0x200dU
#define LOOPBACK_OP_LE_CANCEL		0x200eU
#define LOOPBACK_OP_LOCAL_VERSION	0x1001U
#define LOOPBACK_OP_COMMANDS		0x1002U
#define LOOPBACK_OP_FEATURES		0x1003U
#define LOOPBACK_OP_BUFFER_SIZE		0x1005U
#define LOOPBACK_OP_BD_ADDR		0x1009U
#define LOOPBACK_OP_LE_SCAN_ENABLE	0x200cU

/* The events it sends: Inquiry Complete, Command Complete and Status, the inquiry results, LE meta, and a vendor event. */
#define LOOPBACK_EVENT_INQUIRY_COMPLETE	0x01U
#define LOOPBACK_EVENT_COMPLETE		0x0eU
#define LOOPBACK_EVENT_STATUS		0x0fU
#define LOOPBACK_EVENT_INQUIRY_RSSI	0x22U
#define LOOPBACK_EVENT_EXTENDED_INQUIRY	0x2fU
#define LOOPBACK_EVENT_LE_META		0x3eU
#define LOOPBACK_EVENT_VENDOR		0xffU

/* The pairing's events. */
#define LOOPBACK_EVENT_CONNECTED	0x03U
#define LOOPBACK_EVENT_DISCONNECTED	0x05U
#define LOOPBACK_EVENT_AUTHENTICATED	0x06U
#define LOOPBACK_EVENT_ENCRYPTION	0x08U
#define LOOPBACK_EVENT_COMPLETED	0x13U
#define LOOPBACK_EVENT_PIN		0x16U
#define LOOPBACK_EVENT_KEY_REQUEST	0x17U
#define LOOPBACK_EVENT_KEY		0x18U
#define LOOPBACK_EVENT_IO_REQUEST	0x31U
#define LOOPBACK_EVENT_IO_RESPONSE	0x32U
#define LOOPBACK_EVENT_CONFIRM		0x33U
#define LOOPBACK_EVENT_SIMPLE_DONE	0x36U

/* The events the masks may leave out: codes 0x01 to 0x3E (bit code - 1); LE's subevents by the LE mask (bit subevent - 1). */
#define LOOPBACK_MASKABLE_LAST		0x3eU

/* The Core's default masks (Vol 4 Part E §7.3.1 and §7.8.1). */
#define LOOPBACK_MASK_DEFAULT		0x00001fffffffffffULL
#define LOOPBACK_LE_MASK_DEFAULT	0x000000000000001fULL

/* The devices' handles, and the last byte of each BR/EDR device's address 0A:0B:0C:0D:0E:xx. */
#define LOOPBACK_HANDLE_BREDR		0x0040U
#define LOOPBACK_HANDLE_LE		0x0041U
#define LOOPBACK_HANDLE_MOUSE		0x0043U
#define LOOPBACK_HANDLE_HOG		0x0044U
#define LOOPBACK_HANDLE_PAIRING		0x0045U
#define LOOPBACK_DEVICE_NUMERIC		0x01U
#define LOOPBACK_DEVICE_HID_MOUSE	0x02U
#define LOOPBACK_DEVICE_HOG		0x04U
#define LOOPBACK_DEVICE_MOUSE		0x03U
#define LOOPBACK_DEVICE_DEBUG		0x05U
#define LOOPBACK_DEVICE_SHORT		0x06U
#define LOOPBACK_DEVICE_JUST_WORKS	0x07U

/* The IO capabilities, and the link key types (debug, unauthenticated and authenticated P-256). */
#define LOOPBACK_IO_DISPLAY_YES_NO	0x01U
#define LOOPBACK_IO_NONE		0x03U
#define LOOPBACK_KEY_DEBUG		0x03U
#define LOOPBACK_KEY_P256		0x07U
#define LOOPBACK_KEY_P256_MITM		0x08U

/* The most return parameters a Command Complete carries (its length byte holds 3 more). */
#define LOOPBACK_RETURN_MOST		252U

/* The longest delay 0xFC03 may ask for. */
#define LOOPBACK_DELAY_MOST		10000U

/* A flood's events' parameters, and the most a flood may ask for. */
#define LOOPBACK_FLOOD_PARAMETERS	255U
#define LOOPBACK_FLOOD_MOST		4096U

/* The packets waiting to be delivered (records of bt-hci-proto.c's ring). */
#define LOOPBACK_RING			(64U * 1024U)

/* How long after 0xFC03 the controller goes, and how long after that it comes back. */
#define LOOPBACK_WITHDRAW_MS		300U
#define LOOPBACK_RETURN_MS		500U

/* How often the worker looks at a withdrawal or a timed report that is not due yet, delivering meanwhile. */
#define LOOPBACK_POLL_MS		10U

/* The commands of the HID devices' connections: Accept and Reject Connection Request, Write Scan Enable. */
#define LOOPBACK_OP_ACCEPT		0x0409U
#define LOOPBACK_OP_REJECT		0x040aU
#define LOOPBACK_OP_SCAN_ENABLE		0x0c1aU

/* The event of a device connecting, and the page scan bit of Write Scan Enable. */
#define LOOPBACK_EVENT_REQUEST		0x04U
#define LOOPBACK_SCAN_PAGE		0x02U

/*
 * The BR/EDR connections: the keyboard's (01), the mouse's (02), and the
 * one of the devices that only pair (05, 06, 07); each has SDP's, the
 * control and the interrupt channel, whose CIDs are the device's.
 */
#define LOOPBACK_LINK_KEYBOARD		0U
#define LOOPBACK_LINK_MOUSE		1U
#define LOOPBACK_LINK_PAIRING		2U
#define LOOPBACK_LINKS			3U
#define LOOPBACK_CHANNEL_SDP		0U
#define LOOPBACK_CHANNEL_CONTROL	1U
#define LOOPBACK_CHANNEL_INTERRUPT	2U
#define LOOPBACK_CHANNELS		3U
#define LOOPBACK_CID_FIRST		0x0040U

/* The PSMs, and L2CAP's signalling channel and commands (Core Vol 3 Part A §4). */
#define LOOPBACK_PSM_SDP		0x0001U
#define LOOPBACK_PSM_CONTROL		0x0011U
#define LOOPBACK_PSM_INTERRUPT		0x0013U
#define LOOPBACK_CID_SIGNALLING		0x0001U
#define LOOPBACK_SIGNAL_CONNECT		0x02U
#define LOOPBACK_SIGNAL_CONNECTED	0x03U
#define LOOPBACK_SIGNAL_CONFIGURE	0x04U
#define LOOPBACK_SIGNAL_CONFIGURED	0x05U
#define LOOPBACK_SIGNAL_DISCONNECT	0x06U
#define LOOPBACK_SIGNAL_DISCONNECTED	0x07U
#define LOOPBACK_SIGNAL_ECHO		0x08U
#define LOOPBACK_SIGNAL_ECHO_ANSWER	0x09U
#define LOOPBACK_SIGNAL_INFORMATION	0x0aU
#define LOOPBACK_SIGNAL_INFORMED	0x0bU

/* The longest L2CAP payload the loopback sends in one packet, and the fragment of its SDP answers. */
#define LOOPBACK_FRAME_MOST		128U
#define LOOPBACK_SDP_FRAGMENT		60U

/* The HID devices' timing (milliseconds): the keyboard's steps, the mouse's moves, when it goes and comes back. */
#define LOOPBACK_KEY_RELEASE_MS		200U
#define LOOPBACK_KEY_HOLD_MS		100U
#define LOOPBACK_KEY_REPEAT_MS		3000U
#define LOOPBACK_MOUSE_MS		1000U
#define LOOPBACK_MOUSE_MOVES		4U

/* LE's commands of the HOG mouse: the filter accept list, LE Enable Encryption. */
#define LOOPBACK_OP_LE_CLEAR_LIST	0x2010U
#define LOOPBACK_OP_LE_ADD_LIST		0x2011U
#define LOOPBACK_OP_LE_ENCRYPT		0x2019U

/* The HOG mouse's ATT: its MTU, the opcodes it serves, its errors, its report's and its CCC's handles, its table's size. */
#define LOOPBACK_ATT_MTU		23U
#define LOOPBACK_ATT_ERROR		0x01U
#define LOOPBACK_ATT_MTU_REQUEST	0x02U
#define LOOPBACK_ATT_MTU_RESPONSE	0x03U
#define LOOPBACK_ATT_FIND_INFO		0x04U
#define LOOPBACK_ATT_READ_BY_TYPE	0x08U
#define LOOPBACK_ATT_READ		0x0aU
#define LOOPBACK_ATT_READ_BLOB		0x0cU
#define LOOPBACK_ATT_READ_GROUP		0x10U
#define LOOPBACK_ATT_WRITE		0x12U
#define LOOPBACK_ATT_WRITE_RESPONSE	0x13U
#define LOOPBACK_ATT_NOTIFICATION	0x1bU
#define LOOPBACK_ATT_INVALID_HANDLE	0x01U
#define LOOPBACK_ATT_INVALID_OFFSET	0x07U
#define LOOPBACK_ATT_NOT_SUPPORTED	0x06U
#define LOOPBACK_ATT_NOT_FOUND		0x0aU
#define LOOPBACK_ATT_INSUFFICIENT	0x0fU
#define LOOPBACK_HOG_REPORT		0x0016U
#define LOOPBACK_HOG_REPORT_CCC		0x0017U
#define LOOPBACK_HOG_ATTRIBUTES		21U

/* The steps of the timed reports: none, the keyboard's four, the mouse's moves and its coming back. */
#define LOOPBACK_STEP_NONE		0U
#define LOOPBACK_STEP_RELEASE		1U
#define LOOPBACK_STEP_HOLD_B		2U
#define LOOPBACK_STEP_PRESS_A		3U
#define LOOPBACK_STEP_LET_GO_A		4U
#define LOOPBACK_STEP_MOVE		5U
#define LOOPBACK_STEP_COME_BACK		6U

/*
 * One L2CAP channel of a device: its PSM (0: free), the device's CID and
 * the host's, which of the two configurations are done, whether it is
 * open, and whether the device asked for it (else the host did).
 */
struct loopback_channel {
	uint16_t psm;
	uint16_t local;
	uint16_t remote;
	unsigned ours_done;
	unsigned theirs_done;
	unsigned open;
	unsigned asked;
};

/*
 * One BR/EDR device's connection: its address, its handle, whether it is
 * up and encrypted, the host's IO capability and the key type given at its
 * pairing, whether it was paired (the mouse is not paged from then on) and
 * came back by itself once, its channels, the identifier of its next
 * request, and its timed report (the step and when it is due, in ticks).
 */
struct loopback_link {
	uint8_t address[6];
	uint16_t handle;
	unsigned connected;
	unsigned encrypted;
	uint8_t host_io;
	uint8_t key_type;
	unsigned paired;
	unsigned came_back;
	struct loopback_channel channels[LOOPBACK_CHANNELS];
	uint8_t next_identifier;
	unsigned step;
	unsigned moves;
	uint64_t due;
};

/*
 * The LE HOG mouse's connection: up, encrypted, in the filter accept
 * list, a connection from the list waiting, its report's CCC on, and its
 * next notification (ticks, 0: none).
 */
struct loopback_hog {
	unsigned connected;
	unsigned encrypted;
	unsigned listed;
	unsigned armed;
	unsigned notifying;
	uint64_t due;
};

/* One attribute of the HOG mouse's table: its handle, type, group end (a service), value, and whether reading it needs encryption. */
struct loopback_attribute {
	uint16_t handle;
	uint16_t type;
	uint16_t end;
	const uint8_t *value;
	uint8_t length;
	uint8_t encrypted_only;
};

/*
 * The loopback controller's state, one for the kernel's life.
 *
 * lock guards the waiting packets (ring), the flood still to send, the
 * worker's requests (work, withdraw), the masks and Simple Pairing's mode
 * (event_mask, le_mask, simple_pairing) and hci.  peers guards the devices
 * the loopback plays: the device a command is about (device), the
 * connections (links), and the scan mode the host wrote (scan_enable).
 * The command path holds it while it answers a command or an ACL packet,
 * the worker while it sends the timed reports and while it forgets the
 * connections at a withdrawal; peers is taken before lock, never after.
 * deliver is held by the worker while it hands one packet to the class,
 * and by the reset while it drops what waits and queues the notice, so
 * that no packet from before the reset is delivered after its notice.
 * packet is the worker's own copy of the packet it delivers.
 */
struct loopback_controller {
	struct spinlock lock;
	struct mutex deliver;
	struct bt_hci_ring ring;
	uint8_t ring_bytes[LOOPBACK_RING];
	unsigned flood_remaining;
	unsigned flood_next;
	unsigned work;
	unsigned withdraw;
	uint64_t withdraw_due;
	unsigned stall_counted;
	uint64_t event_mask;
	uint64_t le_mask;
	unsigned simple_pairing;
	struct mutex peers;
	uint8_t device[6];
	struct loopback_link links[LOOPBACK_LINKS];
	struct loopback_hog hog;
	uint8_t scan_enable;
	struct drv_bt_hci *hci;
	struct thread *worker;
	uint8_t packet[BT_ACL_PACKET_MAX];
};

/* The one loopback controller, zero until it is registered. */
static struct loopback_controller loopback;

/*
 * The HOG mouse's GATT table (phase005 section 6): GAP's name; the HID
 * service (information, the report map of a boot mouse with report ID 1,
 * one input report with its CCC and its reference {1, input}, protocol
 * mode); Battery (level 80, its CCC); Device Information (PnP ID: source
 * 1, vendor 0x1209, product 0x4842, version 0x0100).  The values are as
 * GATT has them (little-endian).
 */
static const uint8_t loopback_hog_gap[2] = { 0x00U, 0x18U };
static const uint8_t loopback_hog_name_declaration[5] = { 0x02U, 0x03U, 0x00U, 0x00U, 0x2aU };
static const uint8_t loopback_hog_name[9] = { 'H', 'O', 'G', ' ', 'M', 'o', 'u', 's', 'e' };
static const uint8_t loopback_hog_hid[2] = { 0x12U, 0x18U };
static const uint8_t loopback_hog_information_declaration[5] = { 0x02U, 0x12U, 0x00U, 0x4aU, 0x2aU };
static const uint8_t loopback_hog_information[4] = { 0x11U, 0x01U, 0x00U, 0x02U };
static const uint8_t loopback_hog_map_declaration[5] = { 0x02U, 0x14U, 0x00U, 0x4bU, 0x2aU };
static const uint8_t loopback_hog_map[52] = {
	0x05U, 0x01U, 0x09U, 0x02U, 0xa1U, 0x01U, 0x85U, 0x01U, 0x09U, 0x01U, 0xa1U, 0x00U, 0x05U, 0x09U, 0x19U, 0x01U,
	0x29U, 0x03U, 0x15U, 0x00U, 0x25U, 0x01U, 0x95U, 0x03U, 0x75U, 0x01U, 0x81U, 0x02U, 0x95U, 0x01U, 0x75U, 0x05U,
	0x81U, 0x01U, 0x05U, 0x01U, 0x09U, 0x30U, 0x09U, 0x31U, 0x15U, 0x81U, 0x25U, 0x7fU, 0x75U, 0x08U, 0x95U, 0x02U,
	0x81U, 0x06U, 0xc0U, 0xc0U,
};
static const uint8_t loopback_hog_report_declaration[5] = { 0x12U, 0x16U, 0x00U, 0x4dU, 0x2aU };
static const uint8_t loopback_hog_ccc[2] = { 0x00U, 0x00U };
static const uint8_t loopback_hog_reference[2] = { 0x01U, 0x01U };
static const uint8_t loopback_hog_mode_declaration[5] = { 0x06U, 0x1aU, 0x00U, 0x4eU, 0x2aU };
static const uint8_t loopback_hog_mode[1] = { 0x01U };
static const uint8_t loopback_hog_battery[2] = { 0x0fU, 0x18U };
static const uint8_t loopback_hog_level_declaration[5] = { 0x12U, 0x32U, 0x00U, 0x19U, 0x2aU };
static const uint8_t loopback_hog_level[1] = { 80U };
static const uint8_t loopback_hog_info[2] = { 0x0aU, 0x18U };
static const uint8_t loopback_hog_pnp_declaration[5] = { 0x02U, 0x42U, 0x00U, 0x50U, 0x2aU };
static const uint8_t loopback_hog_pnp[7] = { 0x01U, 0x09U, 0x12U, 0x42U, 0x48U, 0x00U, 0x01U };
static const struct loopback_attribute loopback_hog_table[LOOPBACK_HOG_ATTRIBUTES] = {
	{ 0x0001U, 0x2800U, 0x0005U, loopback_hog_gap, 2U, 0U },
	{ 0x0002U, 0x2803U, 0U, loopback_hog_name_declaration, 5U, 0U },
	{ 0x0003U, 0x2a00U, 0U, loopback_hog_name, 9U, 0U },
	{ 0x0010U, 0x2800U, 0x0020U, loopback_hog_hid, 2U, 0U },
	{ 0x0011U, 0x2803U, 0U, loopback_hog_information_declaration, 5U, 0U },
	{ 0x0012U, 0x2a4aU, 0U, loopback_hog_information, 4U, 1U },
	{ 0x0013U, 0x2803U, 0U, loopback_hog_map_declaration, 5U, 0U },
	{ 0x0014U, 0x2a4bU, 0U, loopback_hog_map, 52U, 1U },
	{ 0x0015U, 0x2803U, 0U, loopback_hog_report_declaration, 5U, 0U },
	{ 0x0016U, 0x2a4dU, 0U, NULL, 0U, 1U },
	{ 0x0017U, 0x2902U, 0U, loopback_hog_ccc, 2U, 0U },
	{ 0x0018U, 0x2908U, 0U, loopback_hog_reference, 2U, 1U },
	{ 0x0019U, 0x2803U, 0U, loopback_hog_mode_declaration, 5U, 0U },
	{ 0x001aU, 0x2a4eU, 0U, loopback_hog_mode, 1U, 1U },
	{ 0x0030U, 0x2800U, 0x0033U, loopback_hog_battery, 2U, 0U },
	{ 0x0031U, 0x2803U, 0U, loopback_hog_level_declaration, 5U, 0U },
	{ 0x0032U, 0x2a19U, 0U, loopback_hog_level, 1U, 0U },
	{ 0x0033U, 0x2902U, 0U, loopback_hog_ccc, 2U, 0U },
	{ 0x0040U, 0x2800U, 0x0042U, loopback_hog_info, 2U, 0U },
	{ 0x0041U, 0x2803U, 0U, loopback_hog_pnp_declaration, 5U, 0U },
	{ 0x0042U, 0x2a50U, 0U, loopback_hog_pnp, 7U, 0U },
};

/*
 * The HID devices' SDP records, as the attribute lists of a
 * ServiceSearchAttributeResponse (a sequence of one record): the
 * keyboard's and the mouse's HID records (HID 1.1.1: their channels, name,
 * subclass, country, virtual cable, reconnect initiate, the boot
 * descriptor of HID 1.11 Appendix E.6 and E.10, normally connectable, boot
 * device) and PnP records (vendor 0x1209, products 0x4B42 and 0x4D53).
 * plan/ws143/phase005 checked them with bluetoothd's SDP client.
 */
static const uint8_t loopback_keyboard_hid[176] = {
	0x35U, 0xaeU, 0x35U, 0xacU, 0x09U, 0x00U, 0x01U, 0x35U, 0x03U, 0x19U, 0x11U, 0x24U, 0x09U, 0x00U, 0x04U, 0x35U,
	0x0dU, 0x35U, 0x06U, 0x19U, 0x01U, 0x00U, 0x09U, 0x00U, 0x11U, 0x35U, 0x03U, 0x19U, 0x00U, 0x11U, 0x09U, 0x00U,
	0x0dU, 0x35U, 0x0fU, 0x35U, 0x0dU, 0x35U, 0x06U, 0x19U, 0x01U, 0x00U, 0x09U, 0x00U, 0x13U, 0x35U, 0x03U, 0x19U,
	0x00U, 0x11U, 0x09U, 0x01U, 0x00U, 0x25U, 0x11U, 0x4cU, 0x6fU, 0x6fU, 0x70U, 0x62U, 0x61U, 0x63U, 0x6bU, 0x20U,
	0x4bU, 0x65U, 0x79U, 0x62U, 0x6fU, 0x61U, 0x72U, 0x64U, 0x09U, 0x02U, 0x02U, 0x08U, 0x40U, 0x09U, 0x02U, 0x03U,
	0x08U, 0x21U, 0x09U, 0x02U, 0x04U, 0x28U, 0x01U, 0x09U, 0x02U, 0x05U, 0x28U, 0x00U, 0x09U, 0x02U, 0x06U, 0x35U,
	0x45U, 0x35U, 0x43U, 0x08U, 0x22U, 0x25U, 0x3fU, 0x05U, 0x01U, 0x09U, 0x06U, 0xa1U, 0x01U, 0x05U, 0x07U, 0x19U,
	0xe0U, 0x29U, 0xe7U, 0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U, 0x95U, 0x08U, 0x81U, 0x02U, 0x95U, 0x01U, 0x75U,
	0x08U, 0x81U, 0x01U, 0x95U, 0x05U, 0x75U, 0x01U, 0x05U, 0x08U, 0x19U, 0x01U, 0x29U, 0x05U, 0x91U, 0x02U, 0x95U,
	0x01U, 0x75U, 0x03U, 0x91U, 0x01U, 0x95U, 0x06U, 0x75U, 0x08U, 0x15U, 0x00U, 0x25U, 0x65U, 0x05U, 0x07U, 0x19U,
	0x00U, 0x29U, 0x65U, 0x81U, 0x00U, 0xc0U, 0x09U, 0x02U, 0x0dU, 0x28U, 0x01U, 0x09U, 0x02U, 0x0eU, 0x28U, 0x01U,
};
static const uint8_t loopback_mouse_hid[160] = {
	0x35U, 0x9eU, 0x35U, 0x9cU, 0x09U, 0x00U, 0x01U, 0x35U, 0x03U, 0x19U, 0x11U, 0x24U, 0x09U, 0x00U, 0x04U, 0x35U,
	0x0dU, 0x35U, 0x06U, 0x19U, 0x01U, 0x00U, 0x09U, 0x00U, 0x11U, 0x35U, 0x03U, 0x19U, 0x00U, 0x11U, 0x09U, 0x00U,
	0x0dU, 0x35U, 0x0fU, 0x35U, 0x0dU, 0x35U, 0x06U, 0x19U, 0x01U, 0x00U, 0x09U, 0x00U, 0x13U, 0x35U, 0x03U, 0x19U,
	0x00U, 0x11U, 0x09U, 0x01U, 0x00U, 0x25U, 0x0eU, 0x4cU, 0x6fU, 0x6fU, 0x70U, 0x62U, 0x61U, 0x63U, 0x6bU, 0x20U,
	0x4dU, 0x6fU, 0x75U, 0x73U, 0x65U, 0x09U, 0x02U, 0x02U, 0x08U, 0x80U, 0x09U, 0x02U, 0x03U, 0x08U, 0x21U, 0x09U,
	0x02U, 0x04U, 0x28U, 0x01U, 0x09U, 0x02U, 0x05U, 0x28U, 0x01U, 0x09U, 0x02U, 0x06U, 0x35U, 0x38U, 0x35U, 0x36U,
	0x08U, 0x22U, 0x25U, 0x32U, 0x05U, 0x01U, 0x09U, 0x02U, 0xa1U, 0x01U, 0x09U, 0x01U, 0xa1U, 0x00U, 0x05U, 0x09U,
	0x19U, 0x01U, 0x29U, 0x03U, 0x15U, 0x00U, 0x25U, 0x01U, 0x95U, 0x03U, 0x75U, 0x01U, 0x81U, 0x02U, 0x95U, 0x01U,
	0x75U, 0x05U, 0x81U, 0x01U, 0x05U, 0x01U, 0x09U, 0x30U, 0x09U, 0x31U, 0x15U, 0x81U, 0x25U, 0x7fU, 0x75U, 0x08U,
	0x95U, 0x02U, 0x81U, 0x06U, 0xc0U, 0xc0U, 0x09U, 0x02U, 0x0dU, 0x28U, 0x00U, 0x09U, 0x02U, 0x0eU, 0x28U, 0x01U,
};
static const uint8_t loopback_keyboard_pnp[36] = {
	0x35U, 0x22U, 0x35U, 0x20U, 0x09U, 0x00U, 0x01U, 0x35U, 0x03U, 0x19U, 0x12U, 0x00U, 0x09U, 0x02U, 0x01U, 0x09U,
	0x12U, 0x09U, 0x09U, 0x02U, 0x02U, 0x09U, 0x4bU, 0x42U, 0x09U, 0x02U, 0x03U, 0x09U, 0x01U, 0x00U, 0x09U, 0x02U,
	0x05U, 0x09U, 0x00U, 0x01U,
};
static const uint8_t loopback_mouse_pnp[36] = {
	0x35U, 0x22U, 0x35U, 0x20U, 0x09U, 0x00U, 0x01U, 0x35U, 0x03U, 0x19U, 0x12U, 0x00U, 0x09U, 0x02U, 0x01U, 0x09U,
	0x12U, 0x09U, 0x09U, 0x02U, 0x02U, 0x09U, 0x4dU, 0x53U, 0x09U, 0x02U, 0x03U, 0x09U, 0x01U, 0x00U, 0x09U, 0x02U,
	0x05U, 0x09U, 0x00U, 0x01U,
};

static int loopback_publish(void);
static void loopback_worker(void *argument);
static int loopback_deliver_next(void);
static void loopback_withdraw_and_return(void);
static void loopback_wake(void);
static int loopback_queue(uint8_t type, const uint8_t *body, size_t length);
static int loopback_complete(uint16_t opcode, const uint8_t *parameters, size_t count);
static int loopback_order(uint16_t opcode);
static int loopback_send(void *context, const uint8_t *packet, size_t length);
static int loopback_set_bootloader(void *context, int on);
static int loopback_reset(void *context);
static void loopback_room(void *context);
static void loopback_sleep_ms(unsigned milliseconds);
static int loopback_status(uint16_t opcode);
static int loopback_inquiry(uint16_t opcode);
static int loopback_advertise(uint16_t opcode);
static void loopback_defaults(void);
static int loopback_masked(const uint8_t *body, size_t length);
static int loopback_pairing(uint16_t opcode, const uint8_t *parameters, size_t length, int *handled);
static int loopback_event(uint8_t code, const uint8_t *parameters, size_t length);
static int loopback_address_event(uint8_t code, const uint8_t *more, size_t more_length);
static int loopback_complete_address(uint16_t opcode);
static int loopback_peer_acl(const uint8_t *packet, size_t length);
static int loopback_frame(uint16_t handle, uint16_t cid, const uint8_t *payload, size_t length);
static uint8_t loopback_role(void);
static int loopback_answer(const uint8_t *packet, size_t length);
static void loopback_select(uint16_t opcode, const uint8_t *parameters, size_t length);
static struct loopback_link *loopback_link_of_role(uint8_t role);
static struct loopback_link *loopback_link_of_handle(unsigned handle);
static uint16_t loopback_device_handle(void);
static void loopback_link_reset(struct loopback_link *link);
static void loopback_links_init(void);
static int loopback_connect(const uint8_t *parameters);
static int loopback_accept(uint16_t opcode);
static int loopback_signal(struct loopback_link *link, const uint8_t *payload, size_t length);
static int loopback_signal_one(struct loopback_link *link, uint8_t code, uint8_t identifier, const uint8_t *data, size_t length);
static int loopback_channel_request(struct loopback_link *link, uint8_t identifier, const uint8_t *data);
static int loopback_signal_send(struct loopback_link *link, uint8_t code, uint8_t identifier, const uint8_t *data, size_t length);
static int loopback_configure(struct loopback_link *link, struct loopback_channel *channel);
static int loopback_channel_ready(struct loopback_link *link, struct loopback_channel *channel);
static int loopback_ask_channel(struct loopback_link *link, uint16_t psm);
static struct loopback_channel *loopback_channel_of_cid(struct loopback_link *link, uint16_t cid);
static int loopback_sdp(struct loopback_link *link, const struct loopback_channel *channel, const uint8_t *pdu, size_t length);
static int loopback_control(struct loopback_link *link, const struct loopback_channel *channel, const uint8_t *frame, size_t length);
static int loopback_report(struct loopback_link *link, const uint8_t *report, size_t length);
static int loopback_reports_start(struct loopback_link *link);
static int loopback_step(struct loopback_link *link, uint64_t now);
static int loopback_peers_tick(void);
static int loopback_hog_connect(void);
static int loopback_hog_notify(void);
static int loopback_hog_peer(uint16_t cid, const uint8_t *payload, size_t length);
static int loopback_hog_att(const uint8_t *pdu, size_t length);
static size_t loopback_hog_list(const uint8_t *request, uint8_t *out);
static size_t loopback_att_error(uint8_t *out, uint8_t request, uint16_t handle, uint8_t code);
static uint16_t loopback_get16(const uint8_t *bytes);
static void loopback_put16(uint8_t *bytes, uint16_t value);

/* What the loopback controller does for the HCI class. */
static const struct drv_bt_hci_ops loopback_ops = {
	.send = loopback_send,
	.set_bootloader = loopback_set_bootloader,
	.reset = loopback_reset,
	.room = loopback_room
};

/*
 * Publishes the loopback controller and starts its worker (test builds:
 * called once at boot).  Returns 0 or the error of publishing it.
 */
int
drv_bt_hci_loopback_register(
	void)
{
	int error;

	/* The locks and the empty queue of waiting packets. */
	spin_init(&loopback.lock, LOCK_RANK_DEVICE, "bt-loopback");
	error = mutex_init(&loopback.deliver, LOCK_RANK_DEVICE, "bt-loopback deliver");
	if (error != 0)
		return error;
	error = mutex_init(&loopback.peers, LOCK_RANK_DEVICE, "bt-loopback peers");
	if (error != 0)
		return error;
	bt_hci_ring_init(&loopback.ring, loopback.ring_bytes, LOOPBACK_RING, 0U);
	loopback_defaults();

	/* The devices' connections, none up. */
	mutex_lock(&loopback.peers);

	loopback_links_init();

	mutex_unlock(&loopback.peers);

	/* The node. */
	error = loopback_publish();
	if (error != 0)
		return error;

	/* The worker that delivers the packets. */
	error = kthread_create(loopback_worker, NULL, SCHED_PRIORITY_DEFAULT, &loopback.worker);
	if (error != 0)
		return error;
	thread_start(loopback.worker);

	/* Succeeded: the test controller is there. */
	kern_logf("bt-loopback: test controller published\n");
	return 0;
}

/* Registers the controller with the class (at boot, and again after a test's withdrawal). */
static int
loopback_publish(
	void)
{
	struct drv_bt_hci_description description;
	struct drv_bt_hci *hci;
	unsigned long irq;
	int error;

	/* What the controller is: a test device of the pid.codes test range. */
	kern_memset(&description, 0, sizeof(description));
	description.vendor = 0x1209U;
	description.product = 0xb7e5U;
	description.version = 0x0100U;
	description.bus = BT_BUS_USB;
	description.name = "Loopback Bluetooth controller (test)";
	description.physical_path = "loopback0";

	/* The class publishes bluetoothN. */
	error = drv_bt_hci_register(&description, &loopback_ops, &loopback, &hci);
	if (error != 0)
		return error;

	/* The worker delivers to it from now on. */
	irq = spin_lock_irqsave(&loopback.lock);

	loopback.hci = hci;

	spin_unlock_irqrestore(&loopback.lock, irq);

	/* Succeeded: published. */
	return 0;
}

/* Delivers the waiting packets to the class, and does the test's withdrawal, for the kernel's life. */
static void
loopback_worker(
	void *argument)
{
	unsigned long irq;
	unsigned withdraw;
	unsigned due;
	uint64_t now;
	int delivered;
	int timed;

	UNUSED_PARAMETER(argument);

	/* Each round: the timed reports and the packets, then a withdrawal asked for (after its command's answer), else a sleep until a request. */
	for (;;) {
		irq = spin_lock_irqsave(&loopback.lock);

		loopback.work = 0U;

		spin_unlock_irqrestore(&loopback.lock, irq);

		/* The HID devices' reports that are due. */
		timed = loopback_peers_tick();

		/* Every packet the class takes now. */
		delivered = loopback_deliver_next();
		while (delivered)
			delivered = loopback_deliver_next();

		/* A withdrawal asked for, and whether it is due. */
		irq = spin_lock_irqsave(&loopback.lock);

		withdraw = loopback.withdraw;
		due = 0U;
		if (withdraw) {
			now = sched_ticks();
			if (now >= loopback.withdraw_due) {
				due = 1U;
				loopback.withdraw = 0U;
			}
		}

		spin_unlock_irqrestore(&loopback.lock, irq);

		/* The test's detach once it is due. */
		if (due) {
			loopback_withdraw_and_return();
			continue;
		}

		/* Not due yet (a withdrawal or a timed report): the packets keep going meanwhile, looked at every LOOPBACK_POLL_MS. */
		if (withdraw || timed) {
			loopback_sleep_ms(LOOPBACK_POLL_MS);
			continue;
		}

		/* Nothing more until a request (a send, the room call, a withdrawal; one made just before is kept). */
		kern_thread_block();
	}
}

/*
 * Hands the next waiting packet to the class.  Returns nonzero when one was
 * delivered (or dropped as the class refused its type), 0 when nothing
 * waits or the class has no room for it now.
 */
static int
loopback_deliver_next(
	void)
{
	struct drv_bt_hci_counts counts;
	struct drv_bt_hci *hci;
	unsigned long irq;
	unsigned from_flood;
	uint32_t sequence;
	size_t length;
	uint8_t type;
	int error;

	/* No reset drops what waits while one packet is being delivered. */
	mutex_lock(&loopback.deliver);

	/* The next packet: from the queue, else the flood's next event. */
	from_flood = 0U;
	length = 0U;
	irq = spin_lock_irqsave(&loopback.lock);

	hci = loopback.hci;
	if (loopback.ring.count != 0U) {
		(void)bt_hci_ring_peek(&loopback.ring, &type, &sequence, &length);
		length = bt_hci_ring_copy(&loopback.ring, loopback.packet);
	} else if (loopback.flood_remaining != 0U) {
		loopback.packet[0] = BT_PACKET_EVENT;
		loopback.packet[1] = LOOPBACK_EVENT_VENDOR;
		loopback.packet[2] = (uint8_t)LOOPBACK_FLOOD_PARAMETERS;
		kern_memset(loopback.packet + 3, 0x5a, LOOPBACK_FLOOD_PARAMETERS);
		loopback.packet[3] = (uint8_t)(loopback.flood_next & 0xffU);
		loopback.packet[4] = (uint8_t)(loopback.flood_next >> 8);
		length = 3U + LOOPBACK_FLOOD_PARAMETERS;
		from_flood = 1U;
	}

	spin_unlock_irqrestore(&loopback.lock, irq);

	/* Nothing waits, or no controller is published. */
	if (length == 0U || hci == NULL) {
		mutex_unlock(&loopback.deliver);
		return 0;
	}

	/* The class queues it, or has no room now. */
	error = drv_bt_hci_input(hci, loopback.packet[0], loopback.packet + 1, length - 1U);
	if (error == ENOSPC) {
		/* Counted once a refusal, as the USB transport counts its stalls. */
		irq = spin_lock_irqsave(&loopback.lock);

		counts.stalls = 0U;
		if (!loopback.stall_counted)
			counts.stalls = 1U;
		loopback.stall_counted = 1U;

		spin_unlock_irqrestore(&loopback.lock, irq);

		/* The class's counts. */
		counts.malformed = 0U;
		drv_bt_hci_count(hci, &counts);
		mutex_unlock(&loopback.deliver);
		return 0;
	}

	/* Taken (or refused for its type): it waits no more. */
	irq = spin_lock_irqsave(&loopback.lock);

	loopback.stall_counted = 0U;
	if (from_flood) {
		loopback.flood_remaining--;
		loopback.flood_next++;
	} else {
		bt_hci_ring_pop(&loopback.ring);
	}

	spin_unlock_irqrestore(&loopback.lock, irq);

	/* Succeeded: one packet is done. */
	mutex_unlock(&loopback.deliver);
	return 1;
}

/*
 * Does the test's detach: withdraws and releases the controller as a USB
 * transport's detach does, drops what waits, and registers it again after
 * a while (the class gives it the lowest free number, its old one).
 */
static void
loopback_withdraw_and_return(
	void)
{
	struct drv_bt_hci *hci;
	unsigned long irq;
	int error;

	/* The controller the class has, which no delivery uses from now on. */
	irq = spin_lock_irqsave(&loopback.lock);

	hci = loopback.hci;
	loopback.hci = NULL;

	spin_unlock_irqrestore(&loopback.lock, irq);

	/* No delivery is under way with it. */
	mutex_lock(&loopback.deliver);
	mutex_unlock(&loopback.deliver);

	/* The class's detach: no operation after the withdrawal, then the node goes. */
	if (hci != NULL) {
		drv_bt_hci_withdraw(hci);
		drv_bt_hci_release(hci);
		kern_logf("bt-loopback: test controller withdrawn\n");
	}

	/* What waited went with it, and every device's connection (phase005 review B5). */
	mutex_lock(&loopback.peers);

	loopback_links_init();
	irq = spin_lock_irqsave(&loopback.lock);

	bt_hci_ring_clear(&loopback.ring);
	loopback.flood_remaining = 0U;
	loopback.stall_counted = 0U;

	spin_unlock_irqrestore(&loopback.lock, irq);
	mutex_unlock(&loopback.peers);

	/* Some time without a controller, then it comes back. */
	loopback_sleep_ms(LOOPBACK_RETURN_MS);
	error = loopback_publish();
	if (error != 0) {
		kern_logf("bt-loopback: test controller not published again (%d)\n", error);
		return;
	}

	/* Logged for the test. */
	kern_logf("bt-loopback: test controller published again\n");
}

/* Wakes the worker for a request (it does not sleep). */
static void
loopback_wake(
	void)
{
	struct thread *worker;
	unsigned long irq;

	/* The request, and the worker to wake. */
	irq = spin_lock_irqsave(&loopback.lock);

	loopback.work = 1U;
	worker = loopback.worker;

	spin_unlock_irqrestore(&loopback.lock, irq);

	/* A worker not started yet finds the request when it starts. */
	if (worker != NULL)
		kern_thread_wakeup(worker);
}

/* Queues one packet (header first, without the type's byte) to be delivered; ENOSPC when the queue is full. */
static int
loopback_queue(
	uint8_t type,
	const uint8_t *body,
	size_t length)
{
	unsigned long irq;
	int masked;
	int error;

	/* At the end of the waiting packets, unless the masks leave the event out. */
	irq = spin_lock_irqsave(&loopback.lock);

	error = 0;
	masked = 0;
	if (type == BT_PACKET_EVENT)
		masked = loopback_masked(body, length);
	if (!masked)
		error = bt_hci_ring_push(&loopback.ring, type, 0U, body, length, 0);

	spin_unlock_irqrestore(&loopback.lock, irq);

	/* Reports a full queue. */
	if (error != 0)
		return error;

	/* Succeeded: queued. */
	return 0;
}

/* Queues a Command Complete event for an opcode with its return parameters (the status first). */
static int
loopback_complete(
	uint16_t opcode,
	const uint8_t *parameters,
	size_t count)
{
	uint8_t event[2U + 3U + LOOPBACK_RETURN_MOST];
	int error;

	/* The event: its code, its length, one credit, the opcode and the return parameters. */
	if (count > LOOPBACK_RETURN_MOST)
		return EINVAL;
	event[0] = LOOPBACK_EVENT_COMPLETE;
	event[1] = (uint8_t)(3U + count);
	event[2] = 1U;
	event[3] = (uint8_t)(opcode & 0xffU);
	event[4] = (uint8_t)(opcode >> 8);
	kern_memcpy(event + 5, parameters, count);

	/* Queued. */
	error = loopback_queue(BT_PACKET_EVENT, event, 5U + count);
	if (error != 0)
		return error;

	/* Succeeded: the answer waits to be delivered. */
	return 0;
}

/*
 * Queues the order test's packets: an event, an ACL packet, an event and
 * an ACL packet, each carrying its place (1 to 4) in its last byte, then
 * the command's answer.
 */
static int
loopback_order(
	uint16_t opcode)
{
	static const uint8_t status_ok[1] = { 0x00U };
	uint8_t event[3];
	uint8_t acl[5];
	int error;

	/* The first event: a vendor event of one byte, 1. */
	event[0] = LOOPBACK_EVENT_VENDOR;
	event[1] = 1U;
	event[2] = 1U;
	error = loopback_queue(BT_PACKET_EVENT, event, sizeof(event));
	if (error != 0)
		return error;

	/* The first ACL packet: handle 0x001, one byte of data, 2. */
	acl[0] = 0x01U;
	acl[1] = 0x20U;
	acl[2] = 1U;
	acl[3] = 0U;
	acl[4] = 2U;
	error = loopback_queue(BT_PACKET_ACL, acl, sizeof(acl));
	if (error != 0)
		return error;

	/* The second event, 3. */
	event[2] = 3U;
	error = loopback_queue(BT_PACKET_EVENT, event, sizeof(event));
	if (error != 0)
		return error;

	/* The second ACL packet, 4. */
	acl[4] = 4U;
	error = loopback_queue(BT_PACKET_ACL, acl, sizeof(acl));
	if (error != 0)
		return error;

	/* The answer last. */
	error = loopback_complete(opcode, status_ok, sizeof(status_ok));
	if (error != 0)
		return error;

	/* Succeeded: all five wait for the worker. */
	return 0;
}

/* Takes one checked H4 packet (the class's operation): answers a command, sends an ACL packet back. */
static int
loopback_send(
	void *context,
	const uint8_t *packet,
	size_t length)
{
	int error;

	UNUSED_PARAMETER(context);

	/* The answer, while the devices' state is the command path's. */
	mutex_lock(&loopback.peers);

	error = loopback_answer(packet, length);

	mutex_unlock(&loopback.peers);

	/* Reports a full queue of waiting packets (or a command it refused). */
	if (error != 0)
		return error;

	/* Succeeded: the answers wait for the worker. */
	loopback_wake();
	return 0;
}

/* Answers one checked H4 packet (peers held): a command's answer, an ACL packet's echo or a device's answer. */
static int
loopback_answer(
	const uint8_t *packet,
	size_t length)
{
	static const uint8_t status_ok[1] = { 0x00U };
	static const uint8_t local_version[9] = { 0x00U, 0x0cU, 0x00U, 0x01U, 0x0cU, 0xffU, 0xffU, 0x01U, 0x00U };
	static const uint8_t bd_addr[7] = { 0x00U, 0x55U, 0x44U, 0x33U, 0x22U, 0x11U, 0x00U };
	static const uint8_t features[9] = { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x40U, 0x00U, 0x40U, 0x00U };
	static const uint8_t buffer_size[8] = { 0x00U, 0xfdU, 0x03U, 0x00U, 0x08U, 0x00U, 0x00U, 0x00U };
	uint8_t commands[1U + 64U];
	struct loopback_link *link;
	unsigned long irq;
	uint16_t opcode;
	unsigned handle;
	unsigned count;
	unsigned delay;
	int handled;
	int error;

	/* An ACL packet goes to a device, or comes back as it went; a command has its opcode. */
	error = 0;
	opcode = 0U;
	if (packet[0] == BT_PACKET_ACL) {
		handle = (unsigned)packet[1] | ((unsigned)(packet[2] & 0x0fU) << 8);
		link = loopback_link_of_handle(handle);
		if (handle == LOOPBACK_HANDLE_LE || handle == LOOPBACK_HANDLE_HOG || link != NULL) {
			error = loopback_peer_acl(packet, length);
		} else {
			error = loopback_queue(BT_PACKET_ACL, packet + 1, length - 1U);
		}
	} else {
		opcode = (uint16_t)(packet[1] | (packet[2] << 8));
	}

	/* The pairing's commands, and the masks and modes. */
	handled = 0;
	if (opcode != 0U)
		error = loopback_pairing(opcode, packet + 4, length - 4U, &handled);
	if (handled)
		opcode = 0U;

	/* Each command the tests use (none for an ACL packet). */
	switch (opcode) {
	case 0U:
		/* The ACL packet's echo, or the pairing's answers, are queued already. */
		break;
	case LOOPBACK_OP_ORDER:
		/* An event, an ACL packet, an event and an ACL packet, then the answer. */
		error = loopback_order(opcode);
		break;
	case LOOPBACK_OP_FLOOD:
		/* The answer first, then the events the parameters ask for. */
		if (length < 6U)
			return EINVAL;
		count = (unsigned)(packet[4] | (packet[5] << 8));
		if (count > LOOPBACK_FLOOD_MOST)
			return EINVAL;
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		if (error == 0) {
			/* The flood, counted from 0. */
			irq = spin_lock_irqsave(&loopback.lock);

			loopback.flood_remaining = count;
			loopback.flood_next = 0U;

			spin_unlock_irqrestore(&loopback.lock, irq);
		}

		/* The flood is under way, or the answer did not fit. */
		break;
	case LOOPBACK_OP_WITHDRAW:
		/* The delay: the two parameter bytes when given, else LOOPBACK_WITHDRAW_MS. */
		delay = LOOPBACK_WITHDRAW_MS;
		if (length >= 6U)
			delay = (unsigned)(packet[4] | (packet[5] << 8));
		if (delay > LOOPBACK_DELAY_MOST)
			return EINVAL;

		/* The answer, then the withdrawal after the delay (by the worker, which may take the operations' lock). */
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		if (error == 0) {
			/* Asked of the worker. */
			irq = spin_lock_irqsave(&loopback.lock);

			loopback.withdraw = 1U;
			loopback.withdraw_due = sched_ticks() + kern_ms_to_ticks(delay);

			spin_unlock_irqrestore(&loopback.lock, irq);
		}

		/* The withdrawal is asked for, or the answer did not fit. */
		break;
	case LOOPBACK_OP_LOCAL_VERSION:
		/* Bluetooth 5.3, the test's manufacturer 0xFFFF. */
		error = loopback_complete(opcode, local_version, sizeof(local_version));
		break;
	case LOOPBACK_OP_BD_ADDR:
		/* 00:11:22:33:44:55. */
		error = loopback_complete(opcode, bd_addr, sizeof(bd_addr));
		break;
	case LOOPBACK_OP_COMMANDS:
		/* Inquiry (octet 0), LE Set Event Mask (25), LE Set Scan Parameters and Enable (26), P-256 and DHKey (34). */
		kern_memset(commands, 0, sizeof(commands));
		commands[1U + 0U] = 0x03U;
		commands[1U + 25U] = 0x01U;
		commands[1U + 26U] = 0x0cU;
		commands[1U + 34U] = 0x06U;
		error = loopback_complete(opcode, commands, sizeof(commands));
		break;
	case LOOPBACK_OP_FEATURES:
		/* LE Supported (Controller): byte 4, bit 6; Non-flushable Packet Boundary Flag: byte 6, bit 6. */
		error = loopback_complete(opcode, features, sizeof(features));
		break;
	case LOOPBACK_OP_BUFFER_SIZE:
		/* ACL 1021 bytes, no SCO, 8 ACL packets. */
		error = loopback_complete(opcode, buffer_size, sizeof(buffer_size));
		break;
	case LOOPBACK_OP_INQUIRY:
		/* Command Status, the two results and the inquiry's end. */
		error = loopback_inquiry(opcode);
		break;
	case LOOPBACK_OP_LE_SCAN_ENABLE:
		/* On: the answer and two reports; off: the answer alone. */
		if (length >= 5U && packet[4] == 1U) {
			error = loopback_advertise(opcode);
		} else {
			error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		}

		break;
	default:
		/* Done, nothing to say. */
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		break;
	}

	/* Reports a full queue of waiting packets. */
	if (error != 0)
		return error;

	/* Succeeded: the answers are queued. */
	return 0;
}

/* Turns the bootloader's path on or off (nothing changes for the loopback controller). */
static int
loopback_set_bootloader(
	void *context,
	int on)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(on);

	/* Succeeded: the class keeps the flag. */
	return 0;
}

/*
 * Resets the controller (the class's operation): what waits is dropped and
 * the reset's notice is queued, both while no packet is being delivered.
 */
static int
loopback_reset(
	void *context)
{
	struct drv_bt_hci *hci;
	unsigned long irq;

	UNUSED_PARAMETER(context);

	/* No packet is being delivered meanwhile. */
	mutex_lock(&loopback.deliver);

	/* Nothing from before the reset is delivered. */
	irq = spin_lock_irqsave(&loopback.lock);

	bt_hci_ring_clear(&loopback.ring);
	loopback.flood_remaining = 0U;
	loopback.stall_counted = 0U;
	hci = loopback.hci;

	spin_unlock_irqrestore(&loopback.lock, irq);

	/* The masks and Simple Pairing's mode as at power-on. */
	loopback_defaults();

	/* The notice, after everything from before. */
	if (hci != NULL)
		drv_bt_hci_notice(hci, BT_PACKET_NOTICE_RESET);

	/* Succeeded: reset. */
	mutex_unlock(&loopback.deliver);
	return 0;
}

/* Tells the worker that a read has made room (the class's operation; it does not sleep). */
static void
loopback_room(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* The worker tries the waiting packet again. */
	loopback_wake();
}

/* Queues a Command Status event with status 0 for an opcode (a command whose work goes on). */
static int
loopback_status(
	uint16_t opcode)
{
	uint8_t event[6];
	int error;

	/* The event: its code, its length 4, the status, one credit and the opcode. */
	event[0] = LOOPBACK_EVENT_STATUS;
	event[1] = 4U;
	event[2] = 0x00U;
	event[3] = 1U;
	event[4] = (uint8_t)(opcode & 0xffU);
	event[5] = (uint8_t)(opcode >> 8);

	/* Queued. */
	error = loopback_queue(BT_PACKET_EVENT, event, sizeof(event));
	if (error != 0)
		return error;

	/* Succeeded: the status waits to be delivered. */
	return 0;
}

/*
 * Queues an inquiry's answer and results: Command Status, an Extended
 * Inquiry Result (address 0A:0B:0C:0D:0E:01, class 0x002540, RSSI -40,
 * the complete name "Loopback Keyboard"), an Inquiry Result with RSSI
 * (0A:0B:0C:0D:0E:02, class 0x002580, RSSI -60), another for the Just
 * Works device (0A:0B:0C:0D:0E:07, class 0x240404, RSSI -50; T1-438: so
 * that Settings can pair with it) and Inquiry Complete.
 */
static int
loopback_inquiry(
	uint16_t opcode)
{
	static const char name[] = "Loopback Keyboard";
	uint8_t extended[2U + 255U];
	uint8_t plain[2U + 15U];
	uint8_t complete[3];
	size_t name_length;
	int error;

	/* The command goes on. */
	error = loopback_status(opcode);
	if (error != 0)
		return error;

	/* The extended result: one response and 240 bytes of data, the name first. */
	kern_memset(extended, 0, sizeof(extended));
	name_length = sizeof(name) - 1U;
	extended[0] = LOOPBACK_EVENT_EXTENDED_INQUIRY;
	extended[1] = 255U;
	extended[2] = 1U;
	extended[3] = 0x01U;
	extended[4] = 0x0eU;
	extended[5] = 0x0dU;
	extended[6] = 0x0cU;
	extended[7] = 0x0bU;
	extended[8] = 0x0aU;
	extended[11] = 0x40U;
	extended[12] = 0x25U;
	extended[13] = 0x00U;
	extended[16] = (uint8_t)(int8_t)-40;
	extended[17] = (uint8_t)(1U + name_length);
	extended[18] = 0x09U;
	kern_memcpy(extended + 19, name, name_length);
	error = loopback_queue(BT_PACKET_EVENT, extended, sizeof(extended));
	if (error != 0)
		return error;

	/* The result with RSSI: one response of 14 bytes. */
	kern_memset(plain, 0, sizeof(plain));
	plain[0] = LOOPBACK_EVENT_INQUIRY_RSSI;
	plain[1] = 15U;
	plain[2] = 1U;
	plain[3] = 0x02U;
	plain[4] = 0x0eU;
	plain[5] = 0x0dU;
	plain[6] = 0x0cU;
	plain[7] = 0x0bU;
	plain[8] = 0x0aU;
	plain[11] = 0x80U;
	plain[12] = 0x25U;
	plain[13] = 0x00U;
	plain[16] = (uint8_t)(int8_t)-60;
	error = loopback_queue(BT_PACKET_EVENT, plain, sizeof(plain));
	if (error != 0)
		return error;

	/* The Just Works device's result with RSSI, the same shape. */
	plain[3] = LOOPBACK_DEVICE_JUST_WORKS;
	plain[11] = 0x04U;
	plain[12] = 0x04U;
	plain[13] = 0x24U;
	plain[16] = (uint8_t)(int8_t)-50;
	error = loopback_queue(BT_PACKET_EVENT, plain, sizeof(plain));
	if (error != 0)
		return error;

	/* The inquiry's end. */
	complete[0] = LOOPBACK_EVENT_INQUIRY_COMPLETE;
	complete[1] = 1U;
	complete[2] = 0x00U;
	error = loopback_queue(BT_PACKET_EVENT, complete, sizeof(complete));
	if (error != 0)
		return error;

	/* Succeeded: the inquiry's packets wait. */
	return 0;
}

/*
 * Queues LE Set Scan Enable's answer and two advertising reports: a
 * public address 0A:0B:0C:0D:0E:03 with the flags, the complete name
 * "Loopback Mouse" and appearance 0x03C2 (RSSI -50), and a random address
 * 4A:0B:0C:0D:0E:04 with the flags alone (RSSI -70).
 */
static int
loopback_advertise(
	uint16_t opcode)
{
	static const uint8_t status_ok[1] = { 0x00U };
	static const char name[] = "Loopback Mouse";
	uint8_t first[2U + 3U + 9U + 31U + 1U];
	uint8_t second[2U + 3U + 9U + 3U + 1U];
	size_t name_length;
	size_t data;
	size_t at;
	int error;

	/* The answer. */
	error = loopback_complete(opcode, status_ok, sizeof(status_ok));
	if (error != 0)
		return error;

	/* The first report's data: the flags, the name, the appearance. */
	name_length = sizeof(name) - 1U;
	data = 3U + (2U + name_length) + 4U;
	at = 0U;
	first[at++] = LOOPBACK_EVENT_LE_META;
	first[at++] = (uint8_t)(2U + 9U + data + 1U);
	first[at++] = 0x02U;
	first[at++] = 1U;
	first[at++] = 0x00U;
	first[at++] = 0x00U;
	first[at++] = 0x03U;
	first[at++] = 0x0eU;
	first[at++] = 0x0dU;
	first[at++] = 0x0cU;
	first[at++] = 0x0bU;
	first[at++] = 0x0aU;
	first[at++] = (uint8_t)data;
	first[at++] = 2U;
	first[at++] = 0x01U;
	first[at++] = 0x06U;
	first[at++] = (uint8_t)(1U + name_length);
	first[at++] = 0x09U;
	kern_memcpy(first + at, name, name_length);
	at += name_length;
	first[at++] = 3U;
	first[at++] = 0x19U;
	first[at++] = 0xc2U;
	first[at++] = 0x03U;
	first[at++] = (uint8_t)(int8_t)-50;
	error = loopback_queue(BT_PACKET_EVENT, first, at);
	if (error != 0)
		return error;

	/* The second report: a random address, the flags alone. */
	at = 0U;
	second[at++] = LOOPBACK_EVENT_LE_META;
	second[at++] = (uint8_t)(2U + 9U + 3U + 1U);
	second[at++] = 0x02U;
	second[at++] = 1U;
	second[at++] = 0x00U;
	second[at++] = 0x01U;
	second[at++] = 0x04U;
	second[at++] = 0x0eU;
	second[at++] = 0x0dU;
	second[at++] = 0x0cU;
	second[at++] = 0x0bU;
	second[at++] = 0x4aU;
	second[at++] = 3U;
	second[at++] = 2U;
	second[at++] = 0x01U;
	second[at++] = 0x06U;
	second[at++] = (uint8_t)(int8_t)-70;
	error = loopback_queue(BT_PACKET_EVENT, second, at);
	if (error != 0)
		return error;

	/* Succeeded: the reports wait. */
	return 0;
}

/* Sleeps the calling thread for about the milliseconds given. */
static void
loopback_sleep_ms(
	unsigned milliseconds)
{
	uint64_t ticks;

	/* At least one tick. */
	ticks = kern_ms_to_ticks(milliseconds);
	if (ticks == 0U)
		ticks = 1U;

	/* Until the tick count passes the deadline. */
	sched_sleep(sched_ticks() + ticks);
}

/* Puts the event masks and Simple Pairing's mode as at power-on (the Core's defaults, Simple Pairing off). */
static void
loopback_defaults(
	void)
{
	unsigned long irq;

	/* The defaults. */
	irq = spin_lock_irqsave(&loopback.lock);

	loopback.event_mask = LOOPBACK_MASK_DEFAULT;
	loopback.le_mask = LOOPBACK_LE_MASK_DEFAULT;
	loopback.simple_pairing = 0U;

	spin_unlock_irqrestore(&loopback.lock, irq);
}

/*
 * Tells whether the masks leave an event out (the lock is held): an event
 * of code 1 to 0x3E whose bit (code - 1) is clear, or an LE subevent whose
 * LE bit (subevent - 1) is clear.  Command Complete and Status, Number Of
 * Completed Packets and the vendor events are never left out.
 */
static int
loopback_masked(
	const uint8_t *body,
	size_t length)
{
	unsigned code;
	unsigned subevent;

	/* The events the masks do not cover. */
	code = body[0];
	if (code == 0U || code > LOOPBACK_MASKABLE_LAST)
		return 0;
	if (code == LOOPBACK_EVENT_COMPLETE || code == LOOPBACK_EVENT_STATUS || code == LOOPBACK_EVENT_COMPLETED)
		return 0;

	/* The event's bit. */
	if ((loopback.event_mask & (1ULL << (code - 1U))) == 0U)
		return 1;

	/* An LE subevent's bit, too. */
	if (code == LOOPBACK_EVENT_LE_META && length >= 3U) {
		subevent = body[2];
		if (subevent >= 1U && subevent <= 64U && (loopback.le_mask & (1ULL << (subevent - 1U))) == 0U)
			return 1;
	}

	/* Sent. */
	return 0;
}

/*
 * Answers the commands of the masks, the modes and the pairing as the
 * devices it plays (see the file's comment), and of the HID devices'
 * connections.  Sets *handled for a command it answered.  Returns 0 or the
 * queue's error.
 */
static int
loopback_pairing(
	uint16_t opcode,
	const uint8_t *parameters,
	size_t length,
	int *handled)
{
	static const uint8_t status_ok[1] = { 0x00U };
	static const uint8_t le_buffers[4] = { 0x00U, 27U, 0x00U, 4U };
	struct loopback_link *link;
	uint8_t event[24];
	uint8_t key[17];
	unsigned long irq;
	uint64_t mask;
	unsigned index;
	uint16_t handle;
	uint8_t role;
	int differs;
	int error;

	/* The device the command is about, its connection and its handle. */
	loopback_select(opcode, parameters, length);
	role = loopback_role();
	link = loopback_link_of_role(role);
	handle = loopback_device_handle();

	/* Each command of the pairing; any other is the caller's. */
	*handled = 1;
	error = 0;
	switch (opcode) {
	case LOOPBACK_OP_SET_EVENT_MASK:
	case LOOPBACK_OP_LE_EVENT_MASK:
		/* A mask of 8 bytes, least significant first, kept. */
		if (length < 8U)
			return EINVAL;
		mask = 0U;
		for (index = 0U; index < 8U; index++)
			mask |= (uint64_t)parameters[index] << (8U * index);
		irq = spin_lock_irqsave(&loopback.lock);

		if (opcode == LOOPBACK_OP_SET_EVENT_MASK)
			loopback.event_mask = mask;
		else
			loopback.le_mask = mask;

		spin_unlock_irqrestore(&loopback.lock, irq);

		/* Done. */
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		break;
	case LOOPBACK_OP_SSP_MODE:
		/* Simple Pairing's mode, kept. */
		if (length < 1U)
			return EINVAL;
		irq = spin_lock_irqsave(&loopback.lock);

		loopback.simple_pairing = parameters[0];

		spin_unlock_irqrestore(&loopback.lock, irq);

		/* Done. */
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		break;
	case LOOPBACK_OP_RESET:
		/* The masks and the mode as at power-on, and no connection (the devices keep their bonds). */
		loopback_defaults();
		for (index = 0U; index < LOOPBACK_LINKS; index++)
			loopback_link_reset(&loopback.links[index]);
		kern_memset(&loopback.hog, 0, sizeof(loopback.hog));
		loopback.scan_enable = 0U;
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		break;
	case LOOPBACK_OP_LE_BUFFER_SIZE:
		/* LE's own buffers: 27 bytes, 4 packets. */
		error = loopback_complete(opcode, le_buffers, sizeof(le_buffers));
		break;
	case LOOPBACK_OP_SCAN_ENABLE:
		/* The scan mode, kept (the mouse comes back only to page scan). */
		if (length < 1U)
			return EINVAL;
		loopback.scan_enable = parameters[0];
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		break;
	case LOOPBACK_OP_CONNECT:
		/* The device paged; a device the loopback does not play does not answer (Page Timeout). */
		if (length < 6U)
			return EINVAL;
		error = loopback_connect(parameters);
		break;
	case LOOPBACK_OP_ACCEPT:
	case LOOPBACK_OP_REJECT:
		/* A device's own connection, taken or refused. */
		if (length < 7U)
			return EINVAL;
		error = loopback_accept(opcode);
		break;
	case LOOPBACK_OP_AUTHENTICATE:
		/* The controller asks the host for a link key. */
		error = loopback_status(opcode);
		if (error == 0)
			error = loopback_address_event(LOOPBACK_EVENT_KEY_REQUEST, NULL, 0U);
		break;
	case LOOPBACK_OP_KEY_REPLY:
		/* The host's key: the device's own gives the authentication, another is a missing key. */
		if (length < 22U)
			return EINVAL;
		error = loopback_complete_address(opcode);
		kern_memset(key, (int)(0xa0U + role), 16U);
		differs = kern_memcmp(parameters + 6, key, 16U);
		event[0] = 0x00U;
		if (differs != 0)
			event[0] = 0x06U;
		loopback_put16(event + 1, handle);
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_AUTHENTICATED, event, 3U);
		break;
	case LOOPBACK_OP_KEY_NEGATIVE:
		/* No key: Simple Pairing asks for the IO capability, or else a PIN. */
		error = loopback_complete_address(opcode);
		if (error != 0)
			break;
		irq = spin_lock_irqsave(&loopback.lock);

		index = loopback.simple_pairing;

		spin_unlock_irqrestore(&loopback.lock, irq);

		/* The next question. */
		if (index != 0U)
			error = loopback_address_event(LOOPBACK_EVENT_IO_REQUEST, NULL, 0U);
		else
			error = loopback_address_event(LOOPBACK_EVENT_PIN, NULL, 0U);
		break;
	case LOOPBACK_OP_PIN_NEGATIVE:
		/* No PIN: the authentication fails (PIN or Key Missing). */
		error = loopback_complete_address(opcode);
		event[0] = 0x06U;
		loopback_put16(event + 1, handle);
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_AUTHENTICATED, event, 3U);
		break;
	case LOOPBACK_OP_IO_REPLY:
		/* The host's IO capability; the device's, then the number to compare (0 for Just Works). */
		if (length < 9U || link == NULL)
			return EINVAL;
		link->host_io = parameters[6];
		error = loopback_complete_address(opcode);
		event[0] = LOOPBACK_IO_DISPLAY_YES_NO;
		if (role == LOOPBACK_DEVICE_JUST_WORKS || role == LOOPBACK_DEVICE_HID_MOUSE)
			event[0] = LOOPBACK_IO_NONE;
		event[1] = 0x00U;
		event[2] = 0x03U;
		if (error == 0)
			error = loopback_address_event(LOOPBACK_EVENT_IO_RESPONSE, event, 3U);

		/* The number: 123456, or 0 for Just Works. */
		event[0] = 0x00U;
		event[1] = 0x00U;
		event[2] = 0x00U;
		event[3] = 0x00U;
		if (role != LOOPBACK_DEVICE_JUST_WORKS && role != LOOPBACK_DEVICE_HID_MOUSE) {
			event[0] = 0x40U;
			event[1] = 0xe2U;
			event[2] = 0x01U;
		}

		/* The question to the host. */
		if (error == 0)
			error = loopback_address_event(LOOPBACK_EVENT_CONFIRM, event, 4U);
		break;
	case LOOPBACK_OP_CONFIRM_REPLY:
		/* Paired: the link key (its type from the association) and the authentication's end. */
		if (link == NULL)
			return EINVAL;
		error = loopback_complete_address(opcode);
		link->key_type = LOOPBACK_KEY_P256_MITM;
		if (role == LOOPBACK_DEVICE_JUST_WORKS || role == LOOPBACK_DEVICE_HID_MOUSE || link->host_io == LOOPBACK_IO_NONE)
			link->key_type = LOOPBACK_KEY_P256;
		if (role == LOOPBACK_DEVICE_DEBUG)
			link->key_type = LOOPBACK_KEY_DEBUG;
		link->paired = 1U;
		event[0] = 0x00U;
		kern_memcpy(event + 1, loopback.device, 6U);
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_SIMPLE_DONE, event, 7U);
		kern_memset(key, (int)(0xa0U + role), 16U);
		key[16] = link->key_type;
		if (error == 0)
			error = loopback_address_event(LOOPBACK_EVENT_KEY, key, 17U);
		event[0] = 0x00U;
		loopback_put16(event + 1, handle);
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_AUTHENTICATED, event, 3U);
		break;
	case LOOPBACK_OP_CONFIRM_NEGATIVE:
		/* Refused: Simple Pairing and the authentication fail (Authentication Failure). */
		error = loopback_complete_address(opcode);
		event[0] = 0x05U;
		kern_memcpy(event + 1, loopback.device, 6U);
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_SIMPLE_DONE, event, 7U);
		event[0] = 0x05U;
		loopback_put16(event + 1, handle);
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_AUTHENTICATED, event, 3U);
		break;
	case LOOPBACK_OP_ENCRYPT:
		/* On: AES-CCM (0x02) on a Secure Connections key, E0 (0x01) otherwise. */
		error = loopback_status(opcode);
		event[0] = 0x00U;
		loopback_put16(event + 1, handle);
		event[3] = 0x01U;
		if (link != NULL && (link->key_type == LOOPBACK_KEY_P256 || link->key_type == LOOPBACK_KEY_P256_MITM))
			event[3] = 0x02U;
		if (link != NULL)
			link->encrypted = 1U;
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_ENCRYPTION, event, 4U);
		break;
	case LOOPBACK_OP_KEY_SIZE:
		/* Status, handle, size: 16, or 7 for the short device. */
		if (length < 2U)
			return EINVAL;
		event[0] = 0x00U;
		event[1] = parameters[0];
		event[2] = parameters[1];
		event[3] = 16U;
		if (role == LOOPBACK_DEVICE_SHORT)
			event[3] = 7U;
		error = loopback_complete(opcode, event, 4U);
		break;
	case LOOPBACK_OP_DISCONNECT:
		/* Ended by the local host: the device's side of the connection is forgotten. */
		if (length < 2U)
			return EINVAL;
		handle = (uint16_t)(loopback_get16(parameters) & 0x0fffU);
		link = loopback_link_of_handle(handle);
		if (link != NULL)
			loopback_link_reset(link);
		if (handle == LOOPBACK_HANDLE_HOG)
			kern_memset(&loopback.hog, 0, sizeof(loopback.hog));
		error = loopback_status(opcode);
		event[0] = 0x00U;
		event[1] = parameters[0];
		event[2] = parameters[1];
		event[3] = 0x16U;
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_DISCONNECTED, event, 4U);
		break;
	case LOOPBACK_OP_LE_CONNECT:
		/* From the filter accept list: the HOG mouse when listed, else a wait for a cancel. */
		if (length < 12U)
			return EINVAL;
		error = loopback_status(opcode);
		if (error == 0 && parameters[4] == 0x01U) {
			loopback.hog.armed = 1U;
			if (loopback.hog.listed)
				error = loopback_hog_connect();
			break;
		}

		/* The HOG mouse (public) at its address. */
		if (error == 0 &&
		    parameters[5] == 0x00U &&
		    parameters[6] == LOOPBACK_DEVICE_HOG &&
		    parameters[7] == 0x0eU &&
		    parameters[8] == 0x0dU &&
		    parameters[9] == 0x0cU &&
		    parameters[10] == 0x0bU &&
		    parameters[11] == 0x0aU) {
			error = loopback_hog_connect();
			break;
		}

		/* The LE mouse (public) connects; any other address never does. */
		if (error != 0 ||
		    parameters[5] != 0x00U ||
		    parameters[6] != LOOPBACK_DEVICE_MOUSE ||
		    parameters[7] != 0x0eU ||
		    parameters[8] != 0x0dU ||
		    parameters[9] != 0x0cU ||
		    parameters[10] != 0x0bU ||
		    parameters[11] != 0x0aU)
			break;
		kern_memset(event, 0, sizeof(event));
		event[0] = 0x01U;
		event[1] = 0x00U;
		event[2] = (uint8_t)(LOOPBACK_HANDLE_LE & 0xffU);
		event[3] = (uint8_t)(LOOPBACK_HANDLE_LE >> 8);
		event[4] = 0x00U;
		event[5] = 0x00U;
		kern_memcpy(event + 6, parameters + 6, 6U);
		event[12] = 0x18U;
		event[16] = 0xf4U;
		event[17] = 0x01U;
		error = loopback_event(LOOPBACK_EVENT_LE_META, event, 19U);
		break;
	case LOOPBACK_OP_LE_CANCEL:
		/* The connection under way ends (one from the list too): LE Connection Complete, Unknown Connection Identifier. */
		loopback.hog.armed = 0U;
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		kern_memset(event, 0, sizeof(event));
		event[0] = 0x01U;
		event[1] = 0x02U;
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_LE_META, event, 19U);
		break;
	case LOOPBACK_OP_LE_CLEAR_LIST:
		/* The filter accept list emptied. */
		loopback.hog.listed = 0U;
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		break;
	case LOOPBACK_OP_LE_ADD_LIST:
		/* The HOG mouse (public) listed; any other address is kept nowhere. */
		if (length < 7U)
			return EINVAL;
		if (parameters[0] == 0x00U && parameters[1] == LOOPBACK_DEVICE_HOG && parameters[2] == 0x0eU && parameters[6] == 0x0aU)
			loopback.hog.listed = 1U;
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		break;
	case LOOPBACK_OP_LE_ENCRYPT:
		/* The HOG mouse's encryption, with its bond's key alone (another connection's is done, nothing to say). */
		if (length < 28U)
			return EINVAL;
		handle = loopback_get16(parameters);
		if (handle != LOOPBACK_HANDLE_HOG || !loopback.hog.connected) {
			error = loopback_complete(opcode, status_ok, sizeof(status_ok));
			break;
		}

		/* Rand and EDIV 0, the LTK 00 11 .. FF: on; any other: PIN or Key Missing. */
		error = loopback_status(opcode);
		kern_memset(key, 0, sizeof(key));
		for (index = 0U; index < 16U; index++)
			key[index] = (uint8_t)(index * 0x11U);
		differs = kern_memcmp(parameters + 12, key, 16U);
		event[0] = 0x00U;
		event[3] = 0x01U;
		if (differs != 0 || parameters[10] != 0U || parameters[11] != 0U) {
			event[0] = 0x06U;
			event[3] = 0x00U;
		} else {
			loopback.hog.encrypted = 1U;
		}

		/* Encryption Change. */
		loopback_put16(event + 1, LOOPBACK_HANDLE_HOG);
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_ENCRYPTION, event, 4U);
		break;
	default:
		/* Not the pairing's. */
		*handled = 0;
		break;
	}

	/* Reports a full queue. */
	if (error != 0)
		return error;

	/* Succeeded: the answers wait for the worker. */
	return 0;
}

/* Queues an event of a code with its parameters. */
static int
loopback_event(
	uint8_t code,
	const uint8_t *parameters,
	size_t length)
{
	uint8_t event[2U + 64U];
	int error;

	/* The code, the length, the parameters. */
	if (length > 64U)
		return EINVAL;
	event[0] = code;
	event[1] = (uint8_t)length;
	kern_memcpy(event + 2, parameters, length);

	/* Queued (unless the masks leave it out). */
	error = loopback_queue(BT_PACKET_EVENT, event, 2U + length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Queues an event led by the device's address, with more parameters. */
static int
loopback_address_event(
	uint8_t code,
	const uint8_t *more,
	size_t more_length)
{
	uint8_t parameters[6U + 24U];
	int error;

	/* The address, then the rest. */
	if (more_length > 24U)
		return EINVAL;
	kern_memcpy(parameters, loopback.device, 6U);
	if (more_length != 0U)
		kern_memcpy(parameters + 6, more, more_length);

	/* Queued. */
	error = loopback_event(code, parameters, 6U + more_length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Queues a Command Complete whose return parameters are the status and the device's address. */
static int
loopback_complete_address(
	uint16_t opcode)
{
	uint8_t returned[7];
	int error;

	/* Status 0, the address. */
	returned[0] = 0x00U;
	kern_memcpy(returned + 1, loopback.device, 6U);
	error = loopback_complete(opcode, returned, sizeof(returned));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Takes an ACL packet the host sent to a device: completed at once, and
 * its L2CAP frame (one packet in this test's frames) answered: a BR/EDR
 * device's signalling, SDP and HID control channels, LE's Pairing Request
 * with Pairing Failed (Pairing Not Supported).
 */
static int
loopback_peer_acl(
	const uint8_t *packet,
	size_t length)
{
	struct loopback_channel *channel;
	struct loopback_link *link;
	uint8_t completed[5];
	uint8_t answer[2];
	const uint8_t *payload;
	size_t payload_length;
	unsigned handle;
	uint16_t cid;
	int error;

	/* The packet's handle, completed. */
	handle = (unsigned)packet[1] | ((unsigned)(packet[2] & 0x0fU) << 8);
	completed[0] = 1U;
	loopback_put16(completed + 1, (uint16_t)handle);
	loopback_put16(completed + 3, 1U);
	error = loopback_event(LOOPBACK_EVENT_COMPLETED, completed, sizeof(completed));
	if (error != 0)
		return error;

	/* An L2CAP header and a payload (HIDP's messages may be one byte), or nothing to answer. */
	if (length < 1U + 4U + 4U + 1U)
		return 0;
	cid = loopback_get16(packet + 7);
	payload = packet + 9;
	payload_length = loopback_get16(packet + 5);
	if (payload_length > length - 9U)
		payload_length = length - 9U;

	/* The HOG mouse's fixed channels. */
	if (handle == LOOPBACK_HANDLE_HOG) {
		error = loopback_hog_peer(cid, payload, payload_length);
		return error;
	}

	/* LE's Pairing Request: refused. */
	if (handle == LOOPBACK_HANDLE_LE && cid == 0x0006U && payload[0] == 0x01U) {
		answer[0] = 0x05U;
		answer[1] = 0x05U;
		error = loopback_frame(LOOPBACK_HANDLE_LE, 0x0006U, answer, 2U);
		return error;
	}

	/* A BR/EDR device's connection that is up. */
	link = loopback_link_of_handle(handle);
	if (link == NULL || !link->connected)
		return 0;

	/* Its signalling. */
	if (cid == LOOPBACK_CID_SIGNALLING) {
		error = loopback_signal(link, payload, payload_length);
		return error;
	}

	/* One of its channels: SDP's or the control channel (the interrupt channel takes nothing). */
	channel = loopback_channel_of_cid(link, cid);
	if (channel == NULL || !channel->open)
		return 0;
	if (channel->psm == LOOPBACK_PSM_SDP) {
		error = loopback_sdp(link, channel, payload, payload_length);
		return error;
	} else if (channel->psm == LOOPBACK_PSM_CONTROL) {
		error = loopback_control(link, channel, payload, payload_length);
		return error;
	}

	/* Nothing to answer. */
	return 0;
}

/* Queues an L2CAP frame to the host in one ACL packet (a first, flushable packet). */
static int
loopback_frame(
	uint16_t handle,
	uint16_t cid,
	const uint8_t *payload,
	size_t length)
{
	uint8_t body[4U + 4U + LOOPBACK_FRAME_MOST];
	int error;

	/* The ACL header, the L2CAP header, the payload. */
	if (length > LOOPBACK_FRAME_MOST)
		return EINVAL;
	body[0] = (uint8_t)(handle & 0xffU);
	body[1] = (uint8_t)(((handle >> 8) & 0x0fU) | 0x20U);
	loopback_put16(body + 2, (uint16_t)(4U + length));
	loopback_put16(body + 4, (uint16_t)length);
	loopback_put16(body + 6, cid);
	kern_memcpy(body + 8, payload, length);

	/* Queued. */
	error = loopback_queue(BT_PACKET_ACL, body, 8U + length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Notes the device a command is about (peers held): a command led by an
 * address names it, a command of a connection names it by the handle's
 * device; any other leaves the last one.
 */
static void
loopback_select(
	uint16_t opcode,
	const uint8_t *parameters,
	size_t length)
{
	struct loopback_link *link;

	/* Each command's way of naming the device. */
	switch (opcode) {
	case LOOPBACK_OP_KEY_REPLY:
	case LOOPBACK_OP_KEY_NEGATIVE:
	case LOOPBACK_OP_PIN_NEGATIVE:
	case LOOPBACK_OP_IO_REPLY:
	case LOOPBACK_OP_CONFIRM_REPLY:
	case LOOPBACK_OP_CONFIRM_NEGATIVE:
	case LOOPBACK_OP_CONNECT:
	case LOOPBACK_OP_ACCEPT:
	case LOOPBACK_OP_REJECT:
		/* Its address first. */
		if (length >= 6U)
			kern_memcpy(loopback.device, parameters, 6U);
		break;
	case LOOPBACK_OP_AUTHENTICATE:
	case LOOPBACK_OP_ENCRYPT:
	case LOOPBACK_OP_KEY_SIZE:
		/* Its connection's handle first. */
		if (length < 2U)
			break;
		link = loopback_link_of_handle((unsigned)loopback_get16(parameters) & 0x0fffU);
		if (link != NULL)
			kern_memcpy(loopback.device, link->address, 6U);
		break;
	default:
		break;
	}
}

/* Gives the connection of a BR/EDR device the loopback plays (by its address's last byte), or NULL. */
static struct loopback_link *
loopback_link_of_role(
	uint8_t role)
{
	/* The keyboard, the mouse, and the devices that only pair. */
	switch (role) {
	case LOOPBACK_DEVICE_NUMERIC:
		return &loopback.links[LOOPBACK_LINK_KEYBOARD];
	case LOOPBACK_DEVICE_HID_MOUSE:
		return &loopback.links[LOOPBACK_LINK_MOUSE];
	case LOOPBACK_DEVICE_DEBUG:
	case LOOPBACK_DEVICE_SHORT:
	case LOOPBACK_DEVICE_JUST_WORKS:
		return &loopback.links[LOOPBACK_LINK_PAIRING];
	default:
		break;
	}

	/* A device it does not play. */
	return NULL;
}

/* Gives the BR/EDR connection of a handle, or NULL. */
static struct loopback_link *
loopback_link_of_handle(
	unsigned handle)
{
	unsigned index;

	/* Each connection's handle. */
	for (index = 0U; index < LOOPBACK_LINKS; index++) {
		if (loopback.links[index].handle == handle)
			return &loopback.links[index];
	}

	/* No device's. */
	return NULL;
}

/* Gives the handle of the device a command is about (that of the pairing's connection for a device it does not play). */
static uint16_t
loopback_device_handle(
	void)
{
	struct loopback_link *link;
	uint8_t role;

	/* The device's connection. */
	role = loopback_role();
	link = loopback_link_of_role(role);
	if (link == NULL)
		return LOOPBACK_HANDLE_PAIRING;

	/* Succeeded: its handle. */
	return link->handle;
}

/* Forgets a connection's state (peers held): down, no channel, no timed report; what the device keeps stays. */
static void
loopback_link_reset(
	struct loopback_link *link)
{
	/* Nothing of the connection is left. */
	link->connected = 0U;
	link->encrypted = 0U;
	kern_memset(link->channels, 0, sizeof(link->channels));
	link->step = LOOPBACK_STEP_NONE;
	link->moves = 0U;
	link->due = 0U;
}

/* Puts every BR/EDR device as at power-on (peers held): no connection, nothing paired, no scan. */
static void
loopback_links_init(
	void)
{
	static const uint16_t handles[LOOPBACK_LINKS] = { LOOPBACK_HANDLE_BREDR, LOOPBACK_HANDLE_MOUSE, LOOPBACK_HANDLE_PAIRING };
	static const uint8_t roles[LOOPBACK_LINKS] = { LOOPBACK_DEVICE_NUMERIC, LOOPBACK_DEVICE_HID_MOUSE, LOOPBACK_DEVICE_JUST_WORKS };
	struct loopback_link *link;
	unsigned index;

	/* Each connection: its handle and its device's address. */
	for (index = 0U; index < LOOPBACK_LINKS; index++) {
		link = &loopback.links[index];
		kern_memset(link, 0, sizeof(*link));
		link->handle = handles[index];
		link->address[0] = roles[index];
		link->address[1] = 0x0eU;
		link->address[2] = 0x0dU;
		link->address[3] = 0x0cU;
		link->address[4] = 0x0bU;
		link->address[5] = 0x0aU;
	}

	/* No scan until the host asks, the HOG mouse not connected nor listed. */
	loopback.scan_enable = 0U;
	kern_memset(&loopback.hog, 0, sizeof(loopback.hog));
}

/*
 * Answers Create Connection as the device paged (peers held): the devices
 * the loopback plays connect on their own handles (the mouse only until it
 * is paired: it is not connectable then), any other is a page timeout.
 */
static int
loopback_connect(
	const uint8_t *parameters)
{
	struct loopback_link *link;
	uint8_t event[11];
	uint16_t handle;
	uint8_t role;
	int answers;
	int error;

	/* The device, its connection and its handle. */
	kern_memcpy(loopback.device, parameters, 6U);
	role = loopback_role();
	link = loopback_link_of_role(role);
	handle = loopback_device_handle();
	error = loopback_status(LOOPBACK_OP_CONNECT);
	if (error != 0)
		return error;

	/* A device the loopback plays answers its page, but the mouse once paired (it is not connectable then). */
	answers = 0;
	if (link != NULL)
		answers = 1;
	if (role == LOOPBACK_DEVICE_HID_MOUSE && link->paired)
		answers = 0;

	/* Connection Complete: up, or a page timeout. */
	event[0] = 0x04U;
	loopback_put16(event + 1, handle);
	kern_memcpy(event + 3, loopback.device, 6U);
	event[9] = 0x01U;
	event[10] = 0x00U;
	if (answers) {
		loopback_link_reset(link);
		kern_memcpy(link->address, loopback.device, 6U);
		link->connected = 1U;
		event[0] = 0x00U;
	}

	/* Queued. */
	error = loopback_event(LOOPBACK_EVENT_CONNECTED, event, sizeof(event));
	if (error != 0)
		return error;

	/* Succeeded: the page answered. */
	return 0;
}

/*
 * Answers Accept or Reject Connection Request of the device noted (peers held): the mouse
 * coming back connects, and asks for its control channel at once, before
 * any security (phase005 section 9.8); a refusal leaves it down.
 */
static int
loopback_accept(
	uint16_t opcode)
{
	struct loopback_link *link;
	uint8_t event[11];
	uint8_t role;
	int error;

	/* The command goes on. */
	error = loopback_status(opcode);
	if (error != 0)
		return error;

	/* Only the mouse comes back by itself; a refusal is the end of it. */
	role = loopback_role();
	link = loopback_link_of_role(role);
	if (opcode == LOOPBACK_OP_REJECT || role != LOOPBACK_DEVICE_HID_MOUSE || link->connected)
		return 0;

	/* Connection Complete, the host the central (its role byte is taken as it is). */
	loopback_link_reset(link);
	link->connected = 1U;
	event[0] = 0x00U;
	loopback_put16(event + 1, link->handle);
	kern_memcpy(event + 3, link->address, 6U);
	event[9] = 0x01U;
	event[10] = 0x00U;
	error = loopback_event(LOOPBACK_EVENT_CONNECTED, event, sizeof(event));
	if (error != 0)
		return error;

	/* The control channel asked for before the link is encrypted. */
	error = loopback_ask_channel(link, LOOPBACK_PSM_CONTROL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Answers each command of a signalling frame the host sent to a device (peers held). */
static int
loopback_signal(
	struct loopback_link *link,
	const uint8_t *payload,
	size_t length)
{
	size_t offset;
	size_t data_length;
	int error;

	/* Each command whose data the frame holds. */
	offset = 0U;
	while (offset + 4U <= length) {
		data_length = loopback_get16(payload + offset + 2U);
		if (data_length > length - offset - 4U)
			return 0;
		error = loopback_signal_one(link, payload[offset], payload[offset + 1U], payload + offset + 4U, data_length);
		if (error != 0)
			return error;
		offset += 4U + data_length;
	}

	/* Succeeded: every command answered. */
	return 0;
}

/* Answers one signalling command as the device (peers held). */
static int
loopback_signal_one(
	struct loopback_link *link,
	uint8_t code,
	uint8_t identifier,
	const uint8_t *data,
	size_t length)
{
	struct loopback_channel *channel;
	uint8_t answer[12];
	uint16_t result;
	int error;

	/* Each command the devices answer; any other is passed over. */
	error = 0;
	switch (code) {
	case LOOPBACK_SIGNAL_CONNECT:
		/* Connection Request (PSM, the host's CID). */
		if (length >= 4U)
			error = loopback_channel_request(link, identifier, data);
		break;
	case LOOPBACK_SIGNAL_CONNECTED:
		/* Connection Response to the device's request: pending (wait), accepted (configured next), refused. */
		if (length < 8U)
			break;
		channel = loopback_channel_of_cid(link, loopback_get16(data + 2));
		if (channel == NULL)
			break;
		result = loopback_get16(data + 4);
		if (result == 0x0001U)
			break;
		if (result != 0x0000U) {
			kern_memset(channel, 0, sizeof(*channel));
			break;
		}

		/* Accepted: the host's CID, the device's configuration. */
		channel->remote = loopback_get16(data);
		error = loopback_configure(link, channel);
		break;
	case LOOPBACK_SIGNAL_CONFIGURE:
		/* Configure Request: accepted as it is (the host's MTU suits the device). */
		if (length < 4U)
			break;
		channel = loopback_channel_of_cid(link, loopback_get16(data));
		if (channel == NULL)
			break;
		loopback_put16(answer, channel->remote);
		loopback_put16(answer + 2, 0U);
		loopback_put16(answer + 4, 0U);
		error = loopback_signal_send(link, LOOPBACK_SIGNAL_CONFIGURED, identifier, answer, 6U);
		channel->theirs_done = 1U;
		if (error == 0)
			error = loopback_channel_ready(link, channel);
		break;
	case LOOPBACK_SIGNAL_CONFIGURED:
		/* Configure Response: the device's configuration taken. */
		if (length < 6U)
			break;
		channel = loopback_channel_of_cid(link, loopback_get16(data));
		if (channel == NULL)
			break;
		channel->ours_done = 1U;
		error = loopback_channel_ready(link, channel);
		break;
	case LOOPBACK_SIGNAL_DISCONNECT:
		/* Disconnection Request: answered with the same CIDs, the channel freed (its reports stop with the interrupt channel). */
		if (length < 4U)
			break;
		channel = loopback_channel_of_cid(link, loopback_get16(data));
		error = loopback_signal_send(link, LOOPBACK_SIGNAL_DISCONNECTED, identifier, data, 4U);
		if (channel == NULL)
			break;
		if (channel->psm == LOOPBACK_PSM_INTERRUPT && link->step != LOOPBACK_STEP_COME_BACK) {
			link->step = LOOPBACK_STEP_NONE;
			link->due = 0U;
		}

		/* The channel is free again. */
		kern_memset(channel, 0, sizeof(*channel));
		break;
	case LOOPBACK_SIGNAL_ECHO:
		/* Echo Request: answered empty. */
		error = loopback_signal_send(link, LOOPBACK_SIGNAL_ECHO_ANSWER, identifier, NULL, 0U);
		break;
	case LOOPBACK_SIGNAL_INFORMATION:
		/* Information Request: the extended features (fixed channels), success. */
		if (length < 2U)
			break;
		kern_memset(answer, 0, sizeof(answer));
		answer[0] = data[0];
		answer[1] = data[1];
		answer[4] = 0x80U;
		error = loopback_signal_send(link, LOOPBACK_SIGNAL_INFORMED, identifier, answer, 8U);
		break;
	default:
		break;
	}

	/* Reports a full queue. */
	if (error != 0)
		return error;

	/* Succeeded: answered. */
	return 0;
}

/*
 * Answers the host's Connection Request (PSM, the host's CID) as the
 * device: SDP's, the control and the interrupt channels are accepted with
 * the device's configuration next; any other PSM is not supported.
 */
static int
loopback_channel_request(
	struct loopback_link *link,
	uint8_t identifier,
	const uint8_t *data)
{
	struct loopback_channel *channel;
	uint8_t answer[8];
	uint16_t psm;
	unsigned index;
	int error;

	/* The channel of the PSM. */
	psm = loopback_get16(data);
	index = LOOPBACK_CHANNELS;
	if (psm == LOOPBACK_PSM_SDP)
		index = LOOPBACK_CHANNEL_SDP;
	else if (psm == LOOPBACK_PSM_CONTROL)
		index = LOOPBACK_CHANNEL_CONTROL;
	else if (psm == LOOPBACK_PSM_INTERRUPT)
		index = LOOPBACK_CHANNEL_INTERRUPT;

	/* A PSM the device does not serve. */
	if (index == LOOPBACK_CHANNELS) {
		loopback_put16(answer, 0U);
		loopback_put16(answer + 2, loopback_get16(data + 2));
		loopback_put16(answer + 4, 0x0002U);
		loopback_put16(answer + 6, 0U);
		error = loopback_signal_send(link, LOOPBACK_SIGNAL_CONNECTED, identifier, answer, sizeof(answer));
		return error;
	}

	/* The channel, the device's CID fixed by the PSM. */
	channel = &link->channels[index];
	kern_memset(channel, 0, sizeof(*channel));
	channel->psm = psm;
	channel->local = (uint16_t)(LOOPBACK_CID_FIRST + index);
	channel->remote = loopback_get16(data + 2);

	/* Connection Response: success. */
	loopback_put16(answer, channel->local);
	loopback_put16(answer + 2, channel->remote);
	loopback_put16(answer + 4, 0U);
	loopback_put16(answer + 6, 0U);
	error = loopback_signal_send(link, LOOPBACK_SIGNAL_CONNECTED, identifier, answer, sizeof(answer));
	if (error != 0)
		return error;

	/* The device's configuration next. */
	error = loopback_configure(link, channel);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Queues one signalling command of a device to the host (its data at most 12 bytes). */
static int
loopback_signal_send(
	struct loopback_link *link,
	uint8_t code,
	uint8_t identifier,
	const uint8_t *data,
	size_t length)
{
	uint8_t command[4U + 12U];
	int error;

	/* Code, identifier, length, data. */
	if (length > 12U)
		return EINVAL;
	command[0] = code;
	command[1] = identifier;
	loopback_put16(command + 2, (uint16_t)length);
	if (length != 0U)
		kern_memcpy(command + 4, data, length);

	/* Queued on the signalling channel. */
	error = loopback_frame(link->handle, LOOPBACK_CID_SIGNALLING, command, 4U + length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Sends the device's Configure Request: MTU 672, and a Flush Timeout (the host takes it, phase005 Q10). */
static int
loopback_configure(
	struct loopback_link *link,
	struct loopback_channel *channel)
{
	uint8_t request[12];
	int error;

	/* The host's CID, no flags, the two options. */
	loopback_put16(request, channel->remote);
	loopback_put16(request + 2, 0U);
	request[4] = 0x01U;
	request[5] = 0x02U;
	loopback_put16(request + 6, 672U);
	request[8] = 0x02U;
	request[9] = 0x02U;
	loopback_put16(request + 10, 0xffffU);
	link->next_identifier++;
	if (link->next_identifier == 0U)
		link->next_identifier = 1U;

	/* Queued. */
	error = loopback_signal_send(link, LOOPBACK_SIGNAL_CONFIGURE, link->next_identifier, request, sizeof(request));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Goes on when a channel's two configurations are done: the device's own
 * control channel is followed by its interrupt channel, and the interrupt
 * channel starts the device's reports.
 */
static int
loopback_channel_ready(
	struct loopback_link *link,
	struct loopback_channel *channel)
{
	int error;

	/* Open once. */
	if (!channel->ours_done || !channel->theirs_done || channel->open)
		return 0;
	channel->open = 1U;

	/* The device asked for its control channel: its interrupt channel next. */
	if (channel->psm == LOOPBACK_PSM_CONTROL && channel->asked) {
		error = loopback_ask_channel(link, LOOPBACK_PSM_INTERRUPT);
		return error;
	}

	/* Only the interrupt channel starts the reports. */
	if (channel->psm != LOOPBACK_PSM_INTERRUPT)
		return 0;
	error = loopback_reports_start(link);
	if (error != 0)
		return error;

	/* Succeeded: open. */
	return 0;
}

/* Asks the host for a channel as the device (its CID fixed by the PSM). */
static int
loopback_ask_channel(
	struct loopback_link *link,
	uint16_t psm)
{
	struct loopback_channel *channel;
	uint8_t request[4];
	unsigned index;
	int error;

	/* The channel of the PSM, asked by the device. */
	index = LOOPBACK_CHANNEL_CONTROL;
	if (psm == LOOPBACK_PSM_INTERRUPT)
		index = LOOPBACK_CHANNEL_INTERRUPT;
	channel = &link->channels[index];
	kern_memset(channel, 0, sizeof(*channel));
	channel->psm = psm;
	channel->local = (uint16_t)(LOOPBACK_CID_FIRST + index);
	channel->asked = 1U;

	/* Connection Request. */
	loopback_put16(request, psm);
	loopback_put16(request + 2, channel->local);
	link->next_identifier++;
	if (link->next_identifier == 0U)
		link->next_identifier = 1U;

	/* Queued. */
	error = loopback_signal_send(link, LOOPBACK_SIGNAL_CONNECT, link->next_identifier, request, sizeof(request));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Gives a device's channel of its CID, or NULL. */
static struct loopback_channel *
loopback_channel_of_cid(
	struct loopback_link *link,
	uint16_t cid)
{
	unsigned index;

	/* Each channel in use. */
	for (index = 0U; index < LOOPBACK_CHANNELS; index++) {
		if (link->channels[index].psm != 0U && link->channels[index].local == cid)
			return &link->channels[index];
	}

	/* None. */
	return NULL;
}

/*
 * Answers a ServiceSearchAttributeRequest of bluetoothd's form (the UUID
 * at bytes 8 and 9, the continuation's length at 19) with the device's
 * records of that class, LOOPBACK_SDP_FRAGMENT bytes at a time; the
 * continuation state is the offset (one byte).
 */
static int
loopback_sdp(
	struct loopback_link *link,
	const struct loopback_channel *channel,
	const uint8_t *pdu,
	size_t length)
{
	uint8_t answer[7U + LOOPBACK_SDP_FRAGMENT + 2U];
	const uint8_t *lists;
	size_t lists_size;
	size_t offset;
	size_t count;
	size_t used;
	uint16_t uuid;
	int error;

	/* A whole request. */
	if (length < 20U || pdu[0] != 0x06U)
		return 0;
	uuid = (uint16_t)((pdu[8] << 8) | pdu[9]);
	offset = 0U;
	if (pdu[19] == 1U && length >= 21U)
		offset = pdu[20];

	/* The records of that class (PnP, else HID), the mouse's or the keyboard's. */
	if (link == &loopback.links[LOOPBACK_LINK_MOUSE] && uuid == 0x1200U) {
		lists = loopback_mouse_pnp;
		lists_size = sizeof(loopback_mouse_pnp);
	} else if (link == &loopback.links[LOOPBACK_LINK_MOUSE]) {
		lists = loopback_mouse_hid;
		lists_size = sizeof(loopback_mouse_hid);
	} else if (uuid == 0x1200U) {
		lists = loopback_keyboard_pnp;
		lists_size = sizeof(loopback_keyboard_pnp);
	} else {
		lists = loopback_keyboard_hid;
		lists_size = sizeof(loopback_keyboard_hid);
	}

	/* The fragment from the offset. */
	if (offset > lists_size)
		offset = lists_size;
	count = lists_size - offset;
	if (count > LOOPBACK_SDP_FRAGMENT)
		count = LOOPBACK_SDP_FRAGMENT;

	/* The response: PDU 0x07, the transaction, the parameters' length, the count, the fragment, the continuation. */
	answer[0] = 0x07U;
	answer[1] = pdu[1];
	answer[2] = pdu[2];
	answer[5] = (uint8_t)(count >> 8);
	answer[6] = (uint8_t)count;
	kern_memcpy(answer + 7, lists + offset, count);
	used = 7U + count;
	if (offset + count < lists_size) {
		answer[used] = 1U;
		answer[used + 1U] = (uint8_t)(offset + count);
		used += 2U;
	} else {
		answer[used] = 0U;
		used++;
	}

	/* The parameters' length (big-endian, as SDP is), then queued. */
	answer[3] = (uint8_t)((used - 5U) >> 8);
	answer[4] = (uint8_t)(used - 5U);
	error = loopback_frame(link->handle, channel->remote, answer, used);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Answers a HIDP message on the control channel: SET_PROTOCOL with HANDSHAKE SUCCESSFUL; anything else is taken silently. */
static int
loopback_control(
	struct loopback_link *link,
	const struct loopback_channel *channel,
	const uint8_t *frame,
	size_t length)
{
	uint8_t handshake[1];
	int error;

	/* Only SET_PROTOCOL is answered. */
	if (length < 1U || (frame[0] >> 4) != 0x7U)
		return 0;

	/* HANDSHAKE SUCCESSFUL. */
	handshake[0] = 0x00U;
	error = loopback_frame(link->handle, channel->remote, handshake, sizeof(handshake));
	if (error != 0)
		return error;

	/* Succeeded: answered. */
	return 0;
}

/* Sends an input report on a device's interrupt channel (DATA INPUT, the header first). */
static int
loopback_report(
	struct loopback_link *link,
	const uint8_t *report,
	size_t length)
{
	struct loopback_channel *channel;
	uint8_t message[1U + 8U];
	int error;

	/* Only on an open interrupt channel. */
	channel = &link->channels[LOOPBACK_CHANNEL_INTERRUPT];
	if (channel->psm == 0U || !channel->open || length > 8U)
		return 0;

	/* DATA INPUT, then the report. */
	message[0] = 0xa1U;
	kern_memcpy(message + 1, report, length);

	/* Queued. */
	error = loopback_frame(link->handle, channel->remote, message, 1U + length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Starts a device's reports as its interrupt channel opens (review S11:
 * the first one at once): the keyboard presses 'a', the mouse moves.
 */
static int
loopback_reports_start(
	struct loopback_link *link)
{
	static const uint8_t press_a[8] = { 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
	static const uint8_t move[3] = { 0x00U, 0x05U, 0x00U };
	uint64_t now;
	int error;

	/* The keyboard: 'a' now, let go LOOPBACK_KEY_RELEASE_MS later. */
	now = sched_ticks();
	if (link == &loopback.links[LOOPBACK_LINK_KEYBOARD]) {
		link->step = LOOPBACK_STEP_RELEASE;
		link->due = now + kern_ms_to_ticks(LOOPBACK_KEY_RELEASE_MS);
		error = loopback_report(link, press_a, sizeof(press_a));
		return error;
	}

	/* Only the keyboard and the mouse report. */
	if (link != &loopback.links[LOOPBACK_LINK_MOUSE])
		return 0;

	/* The mouse moves now, and every LOOPBACK_MOUSE_MS. */
	link->step = LOOPBACK_STEP_MOVE;
	link->moves = 1U;
	link->due = now + kern_ms_to_ticks(LOOPBACK_MOUSE_MS);
	error = loopback_report(link, move, sizeof(move));
	if (error != 0)
		return error;

	/* Succeeded: started. */
	return 0;
}

/*
 * Does a device's timed step that is due (peers held): the keyboard's
 * keys, the mouse's moves, its going after LOOPBACK_MOUSE_MOVES moves of
 * its first connection (supervision timeout), and its coming back a
 * second later when page scan is on.
 */
static int
loopback_step(
	struct loopback_link *link,
	uint64_t now)
{
	static const uint8_t none[8] = { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
	static const uint8_t hold_b[8] = { 0x00U, 0x00U, 0x05U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
	static const uint8_t both[8] = { 0x00U, 0x00U, 0x05U, 0x04U, 0x00U, 0x00U, 0x00U, 0x00U };
	static const uint8_t move[3] = { 0x00U, 0x05U, 0x00U };
	uint8_t event[10];
	int error;

	/* Each step: its report, and the next one's time. */
	error = 0;
	switch (link->step) {
	case LOOPBACK_STEP_RELEASE:
		link->step = LOOPBACK_STEP_HOLD_B;
		link->due = now + kern_ms_to_ticks(LOOPBACK_KEY_HOLD_MS);
		error = loopback_report(link, none, sizeof(none));
		break;
	case LOOPBACK_STEP_HOLD_B:
		link->step = LOOPBACK_STEP_PRESS_A;
		link->due = now + kern_ms_to_ticks(LOOPBACK_KEY_REPEAT_MS);
		error = loopback_report(link, hold_b, sizeof(hold_b));
		break;
	case LOOPBACK_STEP_PRESS_A:
		link->step = LOOPBACK_STEP_LET_GO_A;
		link->due = now + kern_ms_to_ticks(LOOPBACK_KEY_RELEASE_MS);
		error = loopback_report(link, both, sizeof(both));
		break;
	case LOOPBACK_STEP_LET_GO_A:
		link->step = LOOPBACK_STEP_PRESS_A;
		link->due = now + kern_ms_to_ticks(LOOPBACK_KEY_REPEAT_MS - LOOPBACK_KEY_RELEASE_MS);
		error = loopback_report(link, hold_b, sizeof(hold_b));
		break;
	case LOOPBACK_STEP_MOVE:
		/* The first connection ends after so many moves: the device goes, and comes back a second later. */
		if (link->moves >= LOOPBACK_MOUSE_MOVES && !link->came_back) {
			loopback_link_reset(link);
			link->came_back = 1U;
			link->step = LOOPBACK_STEP_COME_BACK;
			link->due = now + kern_ms_to_ticks(LOOPBACK_MOUSE_MS);
			event[0] = 0x00U;
			loopback_put16(event + 1, link->handle);
			event[3] = 0x08U;
			error = loopback_event(LOOPBACK_EVENT_DISCONNECTED, event, 4U);
			break;
		}

		/* Another move. */
		link->moves++;
		link->due = now + kern_ms_to_ticks(LOOPBACK_MOUSE_MS);
		error = loopback_report(link, move, sizeof(move));
		break;
	case LOOPBACK_STEP_COME_BACK:
		/* Connection Request (its address, a mouse's class, ACL), only to page scan. */
		link->step = LOOPBACK_STEP_NONE;
		link->due = 0U;
		if ((loopback.scan_enable & LOOPBACK_SCAN_PAGE) == 0U)
			break;
		kern_memcpy(event, link->address, 6U);
		event[6] = 0x80U;
		event[7] = 0x25U;
		event[8] = 0x00U;
		event[9] = 0x01U;
		error = loopback_event(LOOPBACK_EVENT_REQUEST, event, sizeof(event));
		break;
	default:
		link->due = 0U;
		break;
	}

	/* Reports a full queue. */
	if (error != 0)
		return error;

	/* Succeeded: the step done. */
	return 0;
}

/*
 * Does the devices' timed steps that are due (the worker's).  Returns
 * nonzero while a step waits (the worker then looks again soon).
 */
static int
loopback_peers_tick(
	void)
{
	struct loopback_link *link;
	uint64_t now;
	unsigned index;
	int waiting;
	int error;

	/* Each connection's step, while the command path does not answer. */
	mutex_lock(&loopback.peers);

	now = sched_ticks();
	waiting = 0;
	for (index = 0U; index < LOOPBACK_LINKS; index++) {
		link = &loopback.links[index];
		if (link->due != 0U && now >= link->due) {
			error = loopback_step(link, now);
			if (error != 0)
				link->due = now + kern_ms_to_ticks(LOOPBACK_POLL_MS);
		}

		/* A step still to come keeps the worker looking. */
		if (link->due != 0U)
			waiting = 1;
	}

	/* The HOG mouse's notification (X +5) every second while its CCC is on. */
	if (loopback.hog.due != 0U && now >= loopback.hog.due) {
		loopback.hog.due = now + kern_ms_to_ticks(LOOPBACK_MOUSE_MS);
		error = loopback_hog_notify();
		if (error != 0)
			loopback.hog.due = now + kern_ms_to_ticks(LOOPBACK_POLL_MS);
	}

	/* A notification still to come keeps the worker looking. */
	if (loopback.hog.due != 0U)
		waiting = 1;

	mutex_unlock(&loopback.peers);

	/* Succeeded: whether a step still waits. */
	return waiting;
}

/*
 * Connects the HOG mouse (peers held): LE's (Enhanced, when its mask bit
 * is on) Connection Complete on its handle, and its own Exchange MTU
 * Request at once (phase005 review B3).
 */
static int
loopback_hog_connect(
	void)
{
	static const uint8_t mtu[3] = { LOOPBACK_ATT_MTU_REQUEST, LOOPBACK_ATT_MTU, 0x00U };
	uint8_t event[31];
	unsigned long irq;
	uint64_t mask;
	size_t length;
	int error;

	/* Connected once. */
	if (loopback.hog.connected)
		return 0;
	loopback.hog.connected = 1U;
	loopback.hog.encrypted = 0U;
	loopback.hog.armed = 0U;
	loopback.hog.notifying = 0U;
	loopback.hog.due = 0U;

	/* Enhanced (subevent 10) when the host asked for it (LE mask bit 9). */
	irq = spin_lock_irqsave(&loopback.lock);

	mask = loopback.le_mask;

	spin_unlock_irqrestore(&loopback.lock, irq);

	/* The event: status 0, the handle, central, a public peer, its address, 30 ms, no latency, 5 s. */
	kern_memset(event, 0, sizeof(event));
	event[0] = 0x01U;
	length = 19U;
	if ((mask & (1ULL << 9)) != 0U) {
		event[0] = 0x0aU;
		length = 31U;
	}

	/* Its fields. */
	loopback_put16(event + 2, LOOPBACK_HANDLE_HOG);
	event[6] = LOOPBACK_DEVICE_HOG;
	event[7] = 0x0eU;
	event[8] = 0x0dU;
	event[9] = 0x0cU;
	event[10] = 0x0bU;
	event[11] = 0x0aU;
	loopback_put16(event + length - 7U, 0x0018U);
	loopback_put16(event + length - 3U, 0x01f4U);
	error = loopback_event(LOOPBACK_EVENT_LE_META, event, length);
	if (error != 0)
		return error;

	/* Its MTU request. */
	error = loopback_frame(LOOPBACK_HANDLE_HOG, 0x0004U, mtu, sizeof(mtu));
	if (error != 0)
		return error;

	/* Succeeded: connected. */
	return 0;
}

/* Takes a frame of the host on the HOG mouse's fixed channels: ATT (its server), the rest passed over. */
static int
loopback_hog_peer(
	uint16_t cid,
	const uint8_t *payload,
	size_t length)
{
	int error;

	/* Only ATT. */
	if (cid != 0x0004U || length == 0U || !loopback.hog.connected)
		return 0;

	/* Succeeded: answered. */
	error = loopback_hog_att(payload, length);
	if (error != 0)
		return error;
	return 0;
}

/*
 * Answers one ATT PDU of the host as the HOG mouse's server (MTU 23): the
 * discovery's requests from the table, reads (the HID service's before
 * encryption refused, phase005 section 6), Write Request (the report's
 * CCC turns the notifications on).  Responses and commands are passed
 * over.
 */
static int
loopback_hog_att(
	const uint8_t *pdu,
	size_t length)
{
	const struct loopback_attribute *attribute;
	uint8_t out[LOOPBACK_ATT_MTU];
	uint16_t handle;
	uint16_t offset;
	unsigned index;
	size_t used;
	size_t count;
	int error;

	/* Each request; anything else needs no answer. */
	used = 0U;
	switch (pdu[0]) {
	case LOOPBACK_ATT_MTU_REQUEST:
		out[0] = LOOPBACK_ATT_MTU_RESPONSE;
		loopback_put16(out + 1, LOOPBACK_ATT_MTU);
		used = 3U;
		break;
	case LOOPBACK_ATT_READ_GROUP:
	case LOOPBACK_ATT_READ_BY_TYPE:
	case LOOPBACK_ATT_FIND_INFO:
		/* The range's attributes. */
		if (length < 5U)
			return 0;
		used = loopback_hog_list(pdu, out);
		break;
	case LOOPBACK_ATT_READ:
	case LOOPBACK_ATT_READ_BLOB:
		/* The value from the offset, 22 bytes at most. */
		if (length < 3U)
			return 0;
		handle = loopback_get16(pdu + 1);
		offset = 0U;
		if (pdu[0] == LOOPBACK_ATT_READ_BLOB && length >= 5U)
			offset = loopback_get16(pdu + 3);
		used = loopback_att_error(out, pdu[0], handle, LOOPBACK_ATT_INVALID_HANDLE);
		for (index = 0U; index < LOOPBACK_HOG_ATTRIBUTES; index++) {
			attribute = &loopback_hog_table[index];
			if (attribute->handle != handle)
				continue;
			if (attribute->encrypted_only && !loopback.hog.encrypted) {
				used = loopback_att_error(out, pdu[0], handle, LOOPBACK_ATT_INSUFFICIENT);
				break;
			}

			/* Past its end. */
			if (offset > attribute->length) {
				used = loopback_att_error(out, pdu[0], handle, LOOPBACK_ATT_INVALID_OFFSET);
				break;
			}

			/* The part. */
			count = attribute->length - offset;
			if (count > LOOPBACK_ATT_MTU - 1U)
				count = LOOPBACK_ATT_MTU - 1U;
			out[0] = (uint8_t)(pdu[0] + 1U);
			if (count != 0U)
				kern_memcpy(out + 1, attribute->value + offset, count);
			used = 1U + count;
			break;
		}

		/* Answered. */
		break;
	case LOOPBACK_ATT_WRITE:
		/* A CCC written: the report's turns the notifications on (now and every second). */
		if (length < 5U)
			return 0;
		handle = loopback_get16(pdu + 1);
		if (handle == LOOPBACK_HOG_REPORT_CCC && pdu[3] == 0x01U && !loopback.hog.notifying) {
			loopback.hog.notifying = 1U;
			loopback.hog.due = sched_ticks() + kern_ms_to_ticks(LOOPBACK_MOUSE_MS);
		}

		/* Write Response. */
		out[0] = LOOPBACK_ATT_WRITE_RESPONSE;
		used = 1U;
		break;
	default:
		/* An even opcode the server does not serve is answered so; the rest (responses, commands, confirmations) not. */
		if ((pdu[0] & 0x41U) == 0U && pdu[0] != 0x1eU)
			used = loopback_att_error(out, pdu[0], 0U, LOOPBACK_ATT_NOT_SUPPORTED);
		break;
	}

	/* The answer queued, when there is one. */
	if (used == 0U)
		return 0;
	error = loopback_frame(LOOPBACK_HANDLE_HOG, 0x0004U, out, used);
	if (error != 0)
		return error;

	/* Succeeded: answered. */
	return 0;
}

/*
 * Writes the answer to a discovery's request of a range (Read By Group
 * Type of 0x2800, Read By Type of 0x2803, Find Information): the table's
 * attributes in it, of one element length, as many as fit; none is
 * Attribute Not Found.  Returns the answer's length.
 */
static size_t
loopback_hog_list(
	const uint8_t *request,
	uint8_t *out)
{
	const struct loopback_attribute *attribute;
	uint16_t start;
	uint16_t end;
	uint16_t type;
	unsigned index;
	size_t element;
	size_t used;

	/* The range and (but for Find Information) the type. */
	start = loopback_get16(request + 1);
	end = loopback_get16(request + 3);
	type = 0U;
	if (request[0] != LOOPBACK_ATT_FIND_INFO)
		type = loopback_get16(request + 5);

	/* The response's head: Find Information's format 1 (16-bit UUIDs), else the element's length to come. */
	out[0] = (uint8_t)(request[0] + 1U);
	out[1] = 0x01U;
	used = 2U;
	element = 0U;
	for (index = 0U; index < LOOPBACK_HOG_ATTRIBUTES; index++) {
		attribute = &loopback_hog_table[index];
		if (attribute->handle < start || attribute->handle > end)
			continue;
		if (request[0] != LOOPBACK_ATT_FIND_INFO && attribute->type != type)
			continue;

		/* Its element: handle and type (Find Information), handle and value, or handle, group end and value. */
		if (request[0] == LOOPBACK_ATT_FIND_INFO) {
			if (used + 4U > LOOPBACK_ATT_MTU)
				break;
			loopback_put16(out + used, attribute->handle);
			loopback_put16(out + used + 2U, attribute->type);
			used += 4U;
			continue;
		}

		/* One length in one response. */
		if (element == 0U)
			element = 2U + attribute->length;
		if (element == 2U + attribute->length && request[0] == LOOPBACK_ATT_READ_GROUP && used == 2U)
			element += 2U;
		if (used + element > LOOPBACK_ATT_MTU)
			break;
		loopback_put16(out + used, attribute->handle);
		used += 2U;
		if (request[0] == LOOPBACK_ATT_READ_GROUP) {
			loopback_put16(out + used, attribute->end);
			used += 2U;
		}

		/* The value. */
		kern_memcpy(out + used, attribute->value, attribute->length);
		used += attribute->length;
	}

	/* None in the range. */
	if (used == 2U)
		return loopback_att_error(out, request[0], start, LOOPBACK_ATT_NOT_FOUND);

	/* Succeeded: the element's length (but Find Information's format), and the answer's. */
	if (request[0] != LOOPBACK_ATT_FIND_INFO)
		out[1] = (uint8_t)element;
	return used;
}

/* Writes an Error Response (the request, its handle, the code); returns its length. */
static size_t
loopback_att_error(
	uint8_t *out,
	uint8_t request,
	uint16_t handle,
	uint8_t code)
{
	/* The five bytes. */
	out[0] = LOOPBACK_ATT_ERROR;
	out[1] = request;
	loopback_put16(out + 2, handle);
	out[4] = code;
	return 5U;
}

/* Notifies the HOG mouse's report (button 0, X +5, Y 0; no report ID, phase005 Q12) while connected (peers held). */
static int
loopback_hog_notify(
	void)
{
	static const uint8_t notification[6] = { LOOPBACK_ATT_NOTIFICATION, 0x16U, 0x00U, 0x00U, 0x05U, 0x00U };
	int error;

	/* Only while connected with its CCC on. */
	if (!loopback.hog.connected || !loopback.hog.notifying) {
		loopback.hog.due = 0U;
		return 0;
	}

	/* Succeeded: queued. */
	error = loopback_frame(LOOPBACK_HANDLE_HOG, 0x0004U, notification, sizeof(notification));
	if (error != 0)
		return error;
	return 0;
}

/* Reads a 16-bit value least significant byte first. */
static uint16_t
loopback_get16(
	const uint8_t *bytes)
{
	/* The two bytes. */
	return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

/* Writes a 16-bit value least significant byte first. */
static void
loopback_put16(
	uint8_t *bytes,
	uint16_t value)
{
	/* The two bytes. */
	bytes[0] = (uint8_t)(value & 0xffU);
	bytes[1] = (uint8_t)(value >> 8);
}

/* Gives the last byte of the device's address when it is 0A:0B:0C:0D:0E:xx, else 0 (a device the loopback does not play). */
static uint8_t
loopback_role(
	void)
{
	/* The address's fixed part. */
	if (loopback.device[5] != 0x0aU ||
	    loopback.device[4] != 0x0bU ||
	    loopback.device[3] != 0x0cU ||
	    loopback.device[2] != 0x0dU ||
	    loopback.device[1] != 0x0eU)
		return 0U;

	/* The device's byte. */
	return loopback.device[0];
}
