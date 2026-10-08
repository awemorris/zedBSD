/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's HID host over BR/EDR (ws143-p005 i02c, see hid.h).
 *
 * Everything comes from the daemon's one thread: the router hands the HID
 * host its connections' packets from btd_session_input, the daemon calls
 * the requests and the tick.  Every command is one at a time
 * (session.c).  The kernel's input device is made through the bridge
 * descriptor the daemon's parent opened; closing it removes the device
 * and releases its keys (include/uapi/input-bridge.h).
 */

#include "userland/base/bluetoothd/hid.h"
#include "userland/base/bluetoothd/crypto.h"
#include "userland/base/bluetoothd/hidp.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <uapi/bluetooth.h>
#include <uapi/input-bridge.h>
#include <uapi/input.h>
#include <unistd.h>

/* The commands of the HID host's connections (OGF and OCF as the Core names them). */
#define HID_CREATE_CONNECTION		0x0405U
#define HID_DISCONNECT			0x0406U
#define HID_ACCEPT_CONNECTION		0x0409U
#define HID_REJECT_CONNECTION		0x040aU
#define HID_LINK_KEY_REPLY		0x040bU
#define HID_LINK_KEY_NEGATIVE		0x040cU
#define HID_AUTHENTICATION		0x0411U
#define HID_SET_ENCRYPTION		0x0413U
#define HID_WRITE_SCAN_ENABLE		0x0c1aU
#define HID_READ_KEY_SIZE		0x1408U
#define HID_CREATE_CANCEL		0x0408U
#define HID_LE_CREATE			0x200dU
#define HID_LE_CANCEL			0x200eU
#define HID_LE_CLEAR_LIST		0x2010U
#define HID_LE_ADD_LIST			0x2011U
#define HID_LE_UPDATE			0x2013U
#define HID_LE_ENCRYPT			0x2019U
#define HID_LE_ADD_RESOLVING		0x2027U
#define HID_LE_CLEAR_RESOLVING		0x2029U
#define HID_LE_RESOLUTION		0x202dU

/* The events the HID host takes. */
#define HID_EVENT_CONNECTED		0x03U
#define HID_EVENT_REQUEST		0x04U
#define HID_EVENT_DISCONNECTED		0x05U
#define HID_EVENT_AUTHENTICATED		0x06U
#define HID_EVENT_ENCRYPTION		0x08U
#define HID_EVENT_KEY_REQUEST		0x17U
#define HID_EVENT_ENCRYPTION_V2		0x59U
#define HID_EVENT_LE_META		0x3eU

/* LE's subevents the HID host takes: Connection Complete, Enhanced Connection Complete. */
#define HID_LE_CONNECTED		0x01U
#define HID_LE_ENHANCED			0x0aU

/* LE's fixed channels: ATT, the LE signalling, the Security Manager; SMP's codes and the refusal's reason (Pairing Not Supported). */
#define HID_CID_ATT			0x0004U
#define HID_CID_LE_SIGNALLING		0x0005U
#define HID_CID_SMP			0x0006U
#define HID_SMP_PAIRING_REQUEST		0x01U
#define HID_SMP_PAIRING_FAILED		0x05U
#define HID_SMP_SECURITY_REQUEST	0x0bU
#define HID_SMP_NOT_SUPPORTED		0x05U

/* The appearance's category of a HID device (bits 15 to 6, review M5). */
#define HID_APPEARANCE_HID		0x00fU

/* The status of an authentication whose key the device does not have (PIN or Key Missing). */
#define HID_STATUS_KEY_MISSING		0x06U

/* The reason of a disconnection by the user, of a connection refused (unacceptable address, as the router's), and the role taken when a device connects (central, review S6). */
#define HID_REASON_USER			0x13U
#define HID_REASON_REFUSED		0x0fU
#define HID_ROLE_CENTRAL		0x00U

/* Write Scan Enable's value: page scan only (inquiry scan is the pairing's mode's, D11b). */
#define HID_SCAN_PAGE			0x02U

/* The only key size taken (KNOB). */
#define HID_KEY_SIZE			16U

/* The major device class of a peripheral (keyboards, mice), bits 12 to 8 of the class of device. */
#define HID_MAJOR_PERIPHERAL		0x05U

/* The longest name given to the kernel: 63 bytes less " Touchscreen" (phase005 Q16, review M3). */
#define HID_NAME_BYTES			51U

/* How many bonds are looked through for the records. */
#define HID_BONDS_MAX			32U

static struct btd_hid_device *hid_find(struct btd_hid *hid, const uint8_t *address);
static struct btd_hid_device *hid_find_type(struct btd_hid *hid, const uint8_t *address, unsigned type);
static struct btd_hid_device *hid_by_handle(struct btd_hid *hid, uint16_t handle);
static struct btd_hid_device *hid_slot(struct btd_hid *hid, const uint8_t *address, unsigned type);
static void hid_le_meta(struct btd_hid *hid, const uint8_t *parameters, size_t length);
static void hid_le_connected(struct btd_hid *hid, const uint8_t *parameters, size_t length);
static struct btd_hid_device *hid_le_resolve(struct btd_hid *hid, const uint8_t *address);
static void hid_le_encrypt(struct btd_hid *hid, struct btd_hid_device *device);
static void hid_le_frame(struct btd_hid *hid, struct btd_hid_device *device, uint16_t cid, const uint8_t *payload, size_t length);
static void hid_le_page(struct btd_hid *hid, struct btd_hid_device *device);
static void hid_le_arm(struct btd_hid *hid);
static void hid_le_disarm(struct btd_hid *hid);
static int hid_le_waiting(const struct btd_hid *hid);
static void hid_gatt_start(struct btd_hid *hid, struct btd_hid_device *device);
static void hid_gatt(struct btd_hid *hid, struct btd_hid_device *device, unsigned actions);
static void hid_open(struct btd_hid *hid, struct btd_hid_device *device);
static void hid_cancel_page(struct btd_hid *hid, struct btd_hid_device *device);
static int hid_le_type(unsigned type);
static void hid_send_fixed(struct btd_hid *hid, struct btd_hid_device *device, uint16_t cid, const uint8_t *payload, size_t length);
static void hid_event(struct btd_hid *hid, const uint8_t *parameters, size_t length, uint8_t code);
static void hid_request(struct btd_hid *hid, const uint8_t *parameters, size_t length);
static void hid_connected(struct btd_hid *hid, const uint8_t *parameters, size_t length);
static void hid_key_request(struct btd_hid *hid, const uint8_t *address);
static void hid_authenticated(struct btd_hid *hid, const uint8_t *parameters, size_t length);
static void hid_encryption(struct btd_hid *hid, const uint8_t *parameters, size_t length);
static void hid_key_size(struct btd_hid *hid, struct btd_hid_device *device);
static void hid_disconnected(struct btd_hid *hid, const uint8_t *parameters, size_t length);
static void hid_acl(struct btd_hid *hid, const uint8_t *packet, size_t length);
static void hid_signal(struct btd_hid *hid, struct btd_hid_device *device, const uint8_t *payload, size_t length);
static void hid_channel_opened(struct btd_hid *hid, struct btd_hid_device *device, uint16_t cid);
static void hid_channel_closed(struct btd_hid *hid, struct btd_hid_device *device, uint16_t cid);
static void hid_sdp_start(struct btd_hid *hid, struct btd_hid_device *device);
static void hid_sdp_send(struct btd_hid *hid, struct btd_hid_device *device);
static void hid_sdp_input(struct btd_hid *hid, struct btd_hid_device *device, const uint8_t *pdu, size_t length);
static void hid_sdp_done(struct btd_hid *hid, struct btd_hid_device *device);
static void hid_open_channel(struct btd_hid *hid, struct btd_hid_device *device, uint16_t psm);
static uint16_t hid_asked_control(struct btd_hid_device *device);
static void hid_channels_open(struct btd_hid *hid, struct btd_hid_device *device);
static void hid_control(struct btd_hid *hid, struct btd_hid_device *device, const uint8_t *frame, size_t length);
static void hid_interrupt(struct btd_hid_device *device, const uint8_t *frame, size_t length);
static void hid_report(struct btd_hid_device *device, const uint8_t *report, size_t length);
static void hid_setup(struct btd_hid *hid, struct btd_hid_device *device);
static int hid_setup_write(struct btd_hid *hid, struct btd_hid_device *device);
static void hid_numbers(struct btd_hid_device *device);
static void hid_fail(struct btd_hid *hid, struct btd_hid_device *device, const char *why);
static void hid_ended(struct btd_hid *hid, struct btd_hid_device *device, const char *why);
static void hid_close_bridge(struct btd_hid_device *device);
static void hid_drop(struct btd_hid *hid, struct btd_hid_device *device, int unplug);
static void hid_page(struct btd_hid *hid, struct btd_hid_device *device);
static void hid_page_scan(struct btd_hid *hid);
static void hid_retry_later(struct btd_hid_device *device, uint64_t now);
static void hid_answer(struct btd_hid *hid, struct btd_hid_device *device, const char *line);
static void hid_connected_line(const struct btd_hid *hid, const struct btd_hid_device *device, char *line, size_t size);
static int hid_command(struct btd_hid *hid, uint16_t opcode, const uint8_t *parameters, size_t count);
static void hid_send(struct btd_hid *hid, struct btd_hid_device *device, uint16_t cid, const uint8_t *payload, size_t length);
static int hid_accept(void *context, uint16_t handle, uint16_t psm, uint16_t *result, uint16_t *status);
static void hid_reset_link(struct btd_hid_device *device);
static void hid_name(const char *from, char *to, size_t size);
static void hid_put16(uint8_t *bytes, uint16_t value);
static uint16_t hid_handle_at(const uint8_t *bytes);
static int hid_paging(const struct btd_hid *hid);
static const char *hid_state_word(const struct btd_hid_device *device);

/*
 * Prepares the HID host: no device yet (btd_hid_refresh reads the records
 * once the controller is ready), the router's hooks and the pairing's
 * handoff are the daemon's to give (btd_hid_wants, btd_hid_claims,
 * btd_hid_handle, btd_hid_handoff with the HID host as their context).
 */
void
btd_hid_init(
	struct btd_hid *hid,
	struct btd_session *session,
	const char *keys_folder,
	struct btd_router *router,
	const struct btd_hid_hooks *hooks)
{
	unsigned index;

	/* Nothing in the table. */
	memset(hid, 0, sizeof(*hid));
	hid->session = session;
	hid->keys_folder = keys_folder;
	hid->router = router;
	hid->hooks = *hooks;

	/* No input device yet. */
	for (index = 0U; index < BTD_HID_MAX; index++)
		hid->devices[index].bridge = -1;
}

/*
 * Reads the records of the controller's bonded HID devices into the table
 * (each wanted back, a device in the table kept as it is), forgets the
 * records whose bond went, turns page scan on when a BR/EDR device may
 * come by itself, and pages at once those bluetoothd connects to; LE's
 * come through the auto-connect (the tick sets it).
 */
void
btd_hid_refresh(
	struct btd_hid *hid)
{
	static struct btd_bond bonds[HID_BONDS_MAX];
	struct btd_hid_device *device;
	struct btd_hidcache record;
	unsigned count;
	unsigned index;
	unsigned found;
	int error;

	/* The controller's bonds; records without one go (phase005 section 9.3). */
	error = btd_keys_list(hid->keys_folder, hid->session->address, bonds, HID_BONDS_MAX, &count);
	if (error != 0)
		count = 0U;
	if (error == 0)
		(void)btd_hidcache_prune(hid->keys_folder, hid->session->address, bonds, count);

	/* Each bond with a HID record: in the table. */
	found = 0U;
	for (index = 0U; index < count; index++) {
		/* Its record, when it has one. */
		error = btd_hidcache_read(hid->keys_folder, hid->session->address, bonds[index].address, bonds[index].type, &record);
		if (error != 0)
			continue;
		if (bonds[index].type == BTD_ADDRESS_BREDR)
			found++;

		/* A device in the table already stays as it is. */
		device = hid_find_type(hid, bonds[index].address, bonds[index].type);
		if (device != NULL)
			continue;

		/* A new one, wanted back; one that does not fit is not taken. */
		device = hid_slot(hid, bonds[index].address, bonds[index].type);
		if (device == NULL)
			continue;
		device->record = record;
		if (record.descriptor_size != 0U)
			device->have_descriptor = 1;
		device->wanted = 1;

		/* bluetoothd pages a BR/EDR device that does not connect by itself, or that says it can be connected to. */
		if (bonds[index].type == BTD_ADDRESS_BREDR && (!record.reconnect_initiate || record.normally_connectable))
			device->retry_at = btd_now_ms();
	}

	/* The keys read are not kept; the auto-connect is set again with the table as it is now. */
	memset(bonds, 0, sizeof(bonds));
	hid_le_disarm(hid);

	/* Succeeded: page scan while a device may connect by itself. */
	if (found != 0U)
		hid_page_scan(hid);
}

/*
 * Connects a bonded device (CONNECT): the answer comes through the hook
 * when the connection ends (open or failed), or is written to answer now
 * (an open device, or a refusal).  Returns 1 when answered now, 0 when the
 * connection started.
 */
int
btd_hid_connect(
	struct btd_hid *hid,
	const uint8_t *address,
	unsigned type,
	char *answer,
	size_t size)
{
	struct btd_hid_device *device;
	struct btd_bond bond;
	int keyed;
	int le;
	int error;

	/* The bond and its key (a link key, or LE's LTK of 16 bytes, KNOB). */
	le = hid_le_type(type);
	error = btd_keys_read(hid->keys_folder, hid->session->address, address, type, &bond);
	keyed = 0;
	if (error == 0 && !le && bond.have_link_key)
		keyed = 1;
	if (error == 0 && le && bond.have_ltk)
		keyed = 1;
	memset(bond.link_key, 0, sizeof(bond.link_key));
	memset(bond.ltk, 0, sizeof(bond.ltk));
	if (!keyed) {
		(void)snprintf(answer, size, "%s", "ERROR not-bonded");
		return 1;
	}

	/* An LE bond with a shorter key is refused at once. */
	if (le && bond.key_size != HID_KEY_SIZE) {
		(void)snprintf(answer, size, "%s", "ERROR key-size");
		return 1;
	}

	/* Its device, in the table or new. */
	device = hid_find_type(hid, address, type);
	if (device == NULL)
		device = hid_slot(hid, address, type);
	if (device == NULL) {
		(void)snprintf(answer, size, "%s", "ERROR limit");
		return 1;
	}

	/* Named after its bond (the name the user saw when pairing), else after its HID record. */
	if (bond.name[0] != '\0')
		(void)snprintf(device->record.name, sizeof(device->record.name), "%s", bond.name);

	/* Wanted back from now on, and its pages start again (review S5). */
	device->wanted = 1;
	device->paused = 0;
	device->retries = 0U;

	/* An open device is answered as it is. */
	if (device->state == BTD_HID_OPEN) {
		hid_connected_line(hid, device, answer, size);
		return 1;
	}

	/* One connection at a time, none during a pairing or a scan, one page at a time. */
	if (device->state != BTD_HID_IDLE || hid->held) {
		(void)snprintf(answer, size, "%s", "ERROR busy");
		return 1;
	}

	/* The controller pages (or connects LE to) one device at a time. */
	error = hid_paging(hid);
	if (error) {
		(void)snprintf(answer, size, "%s", "ERROR busy");
		return 1;
	}

	/* Succeeded: paged (LE: connected directly), the answer at the connection's end. */
	device->asked = 1;
	if (le) {
		hid_le_page(hid, device);
		return 0;
	}
	hid_page(hid, device);
	return 0;
}

/*
 * Disconnects a device (DISCONNECT): its channels, then the link; it is
 * not wanted back until connected again (phase005 Q5).  Returns 0, or
 * ENOTCONN for a device not connected (it is not wanted back either).
 */
int
btd_hid_disconnect(
	struct btd_hid *hid,
	const uint8_t *address,
	unsigned type)
{
	struct btd_hid_device *device;
	uint8_t request[16];
	uint8_t disconnect[3];
	size_t length;
	int error;

	/* The device, not wanted back from now on (the auto-connect is set again without it). */
	device = hid_find_type(hid, address, type);
	if (device == NULL)
		return ENOTCONN;
	device->wanted = btd_hid_policy_after_disconnect();
	device->retry_at = 0U;
	if (hid_le_type(type))
		hid_le_disarm(hid);

	/* Not connected. */
	if (!device->connected || device->state == BTD_HID_CLOSING)
		return ENOTCONN;

	/* BR/EDR's interrupt channel, then its control channel (no VIRTUAL_CABLE_UNPLUG: that would unpair). */
	error = btd_l2cap_disconnect(&device->l2cap, device->interrupt_cid, request, sizeof(request), &length);
	if (error == 0)
		hid_send(hid, device, BTD_CID_SIGNALLING, request, length);
	error = btd_l2cap_disconnect(&device->l2cap, device->control_cid, request, sizeof(request), &length);
	if (error == 0)
		hid_send(hid, device, BTD_CID_SIGNALLING, request, length);

	/* The link: the user ended it. */
	hid_put16(disconnect, device->handle);
	disconnect[2] = HID_REASON_USER;
	(void)hid_command(hid, HID_DISCONNECT, disconnect, sizeof(disconnect));
	device->last = "user";
	device->state = BTD_HID_CLOSING;
	device->state_deadline = btd_now_ms() + BTD_HID_CLOSE_MS;

	/* Succeeded: closing. */
	return 0;
}

/*
 * Forgets a device whose bond goes (FORGET, or the device's own unplug):
 * a connected BR/EDR one hears VIRTUAL_CABLE_UNPLUG on its control channel
 * (design section 6.3) and is disconnected; its record goes and its slot
 * is freed.
 */
void
btd_hid_forget(
	struct btd_hid *hid,
	const uint8_t *address,
	unsigned type)
{
	struct btd_hid_device *device;

	/* The record goes in any case. */
	(void)btd_hidcache_forget(hid->keys_folder, hid->session->address, address, type);

	/* Nothing more for a device not in the table. */
	device = hid_find_type(hid, address, type);
	if (device == NULL)
		return;

	/* Succeeded: a connected device hears the unplug, and the device goes (and leaves the auto-connect). */
	hid_drop(hid, device, 1);
	if (hid_le_type(type))
		hid_le_disarm(hid);
}

/*
 * Lets a device go before it is paired again (PAIR of a device in the
 * table, review B7): its link ends, its input device and its record go,
 * its slot is freed; the pairing's handoff makes it anew.
 */
void
btd_hid_release(
	struct btd_hid *hid,
	const uint8_t *address,
	unsigned type)
{
	struct btd_hid_device *device;

	/* Nothing for a device not in the table. */
	device = hid_find_type(hid, address, type);
	if (device == NULL)
		return;

	/* Succeeded: its record goes, and the device without an unplug (its bond stays for the pairing). */
	(void)btd_hidcache_forget(hid->keys_folder, hid->session->address, address, type);
	hid_drop(hid, device, 0);
	if (hid_le_type(type))
		hid_le_disarm(hid);
}

/*
 * Writes the STATUS line of the table's slot index, or an empty line for a
 * free slot.
 */
void
btd_hid_status(
	const struct btd_hid *hid,
	unsigned index,
	char *line,
	size_t size)
{
	const struct btd_hid_device *device;
	char address[24];
	char name[BTD_NAME_MAX * 4U];
	char input[32];
	char battery[16];
	const char *reconnect;
	const char *last;
	const char *transport;
	uint64_t seconds;
	int error;

	/* A free slot has no line. */
	line[0] = '\0';
	if (index >= BTD_HID_MAX || !hid->devices[index].used)
		return;
	device = &hid->devices[index];

	/* The address, the name (quoted safely) and the input device. */
	btd_format_address(device->address, address, sizeof(address));
	error = btd_escape(device->record.name, name, sizeof(name));
	if (error != 0)
		name[0] = '\0';
	(void)snprintf(input, sizeof(input), "%s", "-");
	if (device->bridge >= 0 && device->event >= 0)
		(void)snprintf(input, sizeof(input), "/dev/input/event%d", (int)device->event);

	/* How it comes back, why it last ended, and since when it is in its state. */
	reconnect = "off";
	if (device->wanted)
		reconnect = "auto";
	if (device->wanted && device->paused)
		reconnect = "paused";
	last = "-";
	if (device->last != NULL)
		last = device->last;
	seconds = 0U;
	if (device->since_ms != 0U)
		seconds = (btd_now_ms() - device->since_ms) / 1000U;

	/* HID's or HOGP's, and LE's battery when it was read. */
	transport = "hid";
	(void)snprintf(battery, sizeof(battery), "%s", "-");
	if (hid_le_type(device->type))
		transport = "hog";
	if (hid_le_type(device->type) && device->hog.battery >= 0)
		(void)snprintf(battery, sizeof(battery), "%d", device->hog.battery);

	/* Succeeded: the line. */
	(void)snprintf(line,
		       size,
		       "HID address=%s type=%s transport=%s state=%s input=%s name=\"%s\" reconnect=%s battery=%s since=%llu last=%s reports=%u malformed=%u oversize=%u",
		       address,
		       btd_address_type_name(device->type),
		       transport,
		       hid_state_word(device),
		       input,
		       name,
		       reconnect,
		       battery,
		       (unsigned long long)seconds,
		       last,
		       device->reports,
		       device->malformed,
		       device->oversize + device->hog.oversize);
}

/*
 * Tells how many devices are open.
 */
unsigned
btd_hid_open_count(
	const struct btd_hid *hid)
{
	unsigned index;
	unsigned count;

	/* Each open device. */
	count = 0U;
	for (index = 0U; index < BTD_HID_MAX; index++) {
		if (hid->devices[index].used && hid->devices[index].state == BTD_HID_OPEN)
			count++;
	}

	/* Succeeded: the count. */
	return count;
}

/*
 * Tells whether a pairing must wait (review B7): a connection under way
 * (paging to closing, not open) of any device.  An open or waiting device
 * of the same address is the caller's to disconnect first.
 */
int
btd_hid_busy(
	const struct btd_hid *hid,
	const uint8_t *address)
{
	const struct btd_hid_device *device;
	unsigned index;

	/* Each device whose connection is under way. */
	(void)address;
	for (index = 0U; index < BTD_HID_MAX; index++) {
		device = &hid->devices[index];
		if (!device->used || device->state == BTD_HID_IDLE || device->state == BTD_HID_OPEN)
			continue;

		/* A connection under way. */
		return 1;
	}

	/* None. */
	return 0;
}

/*
 * Holds the pages while a pairing or a scan runs (held 1), and lets them
 * go on after (held 0).
 */
void
btd_hid_hold(
	struct btd_hid *hid,
	int held)
{
	/* A hold that starts cancels the auto-connect (the tick sets it again after, review S4). */
	if (held && !hid->held)
		hid_le_disarm(hid);

	/* The pages wait while it is set. */
	hid->held = held;
}

/*
 * Ends what passed its deadline (the connection's parts, the handshake,
 * LE's discovery, a resumed link's Echo Request), pages a wanted device
 * whose time came, and sets LE's auto-connect when a device waits for it.
 */
void
btd_hid_tick(
	struct btd_hid *hid,
	uint64_t now)
{
	struct btd_hid_device *device;
	unsigned actions;
	unsigned index;
	int paging;
	int waiting;

	/* Each device in the table. */
	for (index = 0U; index < BTD_HID_MAX; index++) {
		device = &hid->devices[index];
		if (!device->used)
			continue;

		/* A resumed link that did not answer its Echo Request: ended, to be paged again (review S5). */
		if (device->state == BTD_HID_OPEN && device->echo_deadline != 0U && now >= device->echo_deadline) {
			device->echo_deadline = 0U;
			hid_fail(hid, device, "lost");
			continue;
		}

		/* LE's discovery's own times. */
		if (device->state == BTD_HID_GATT || device->state == BTD_HID_SUBSCRIBE) {
			actions = btd_hog_tick(&device->hog, now);
			if (actions != 0U) {
				hid_gatt(hid, device, actions);
				continue;
			}
		}

		/* A handshake that did not come, or a setup to write again: the input device is made now. */
		if (device->state == BTD_HID_HANDSHAKE || device->state == BTD_HID_SETUP) {
			if (device->state_deadline != 0U && now >= device->state_deadline)
				hid_setup(hid, device);
			continue;
		}

		/* A disconnection that did not come: ended as far as bluetoothd knows. */
		if (device->state == BTD_HID_CLOSING && device->state_deadline != 0U && now >= device->state_deadline) {
			hid_ended(hid, device, device->last);
			continue;
		}

		/* A part, or the whole connection, that took too long. */
		if (device->state != BTD_HID_IDLE && device->state != BTD_HID_OPEN) {
			if (device->state_deadline != 0U && now >= device->state_deadline)
				hid_fail(hid, device, "timeout");
			else if (device->total_deadline != 0U && now >= device->total_deadline)
				hid_fail(hid, device, "timeout");
			continue;
		}

		/* A wanted device's page, when its time came and nothing holds it. */
		if (device->state != BTD_HID_IDLE || !device->wanted || device->paused || device->retry_at == 0U)
			continue;
		if (now < device->retry_at || hid->held)
			continue;
		paging = hid_paging(hid);
		if (paging)
			continue;
		hid_page(hid, device);
	}

	/* Succeeded: LE's auto-connect, when a device waits for it and nothing holds it. */
	waiting = hid_le_waiting(hid);
	if (waiting && !hid->le_armed && !hid->held)
		hid_le_arm(hid);
}

/*
 * Gives the earliest deadline of the HID host (the daemon's loop wakes
 * then), or 0 when nothing waits.
 */
uint64_t
btd_hid_deadline(
	const struct btd_hid *hid)
{
	const struct btd_hid_device *device;
	uint64_t earliest;
	uint64_t discovery;
	unsigned index;

	/* Each device's deadlines (its discovery's and its Echo Request's too) and its page's time. */
	earliest = 0U;
	for (index = 0U; index < BTD_HID_MAX; index++) {
		device = &hid->devices[index];
		if (!device->used)
			continue;
		if (device->state_deadline != 0U && (earliest == 0U || device->state_deadline < earliest))
			earliest = device->state_deadline;
		if (device->total_deadline != 0U && (earliest == 0U || device->total_deadline < earliest))
			earliest = device->total_deadline;
		if (device->echo_deadline != 0U && (earliest == 0U || device->echo_deadline < earliest))
			earliest = device->echo_deadline;
		discovery = btd_hog_deadline(&device->hog);
		if (discovery != 0U && (earliest == 0U || discovery < earliest))
			earliest = discovery;
		if (device->state == BTD_HID_IDLE && device->wanted && !device->paused && device->retry_at != 0U) {
			if (earliest == 0U || device->retry_at < earliest)
				earliest = device->retry_at;
		}
	}

	/* Succeeded: the earliest, or 0. */
	return earliest;
}

/*
 * Forgets every connection because the controller went or was reset
 * (review B5): the input devices go (the kernel releases their keys), the
 * connections are idle, the devices stay wanted for the next controller.
 */
void
btd_hid_lost(
	struct btd_hid *hid)
{
	struct btd_hid_device *device;
	unsigned index;

	/* Each device in the table. */
	for (index = 0U; index < BTD_HID_MAX; index++) {
		device = &hid->devices[index];
		if (!device->used)
			continue;

		/* A client waiting hears the end. */
		if (device->asked)
			hid_answer(hid, device, "ERROR lost");

		/* Nothing open any more. */
		hid_close_bridge(device);
		hid_reset_link(device);
		device->state = BTD_HID_IDLE;
		device->last = "lost";
		device->retries = 0U;
		device->retry_at = 0U;
	}

	/* The next controller's page scan and auto-connect are set again. */
	hid->page_scan = 0;
	hid->le_armed = 0;
}

/*
 * Checks the links after the system's sleep (sleep.end, review S5): each
 * open BR/EDR link is asked an Echo Request (no answer within
 * BTD_HID_ECHO_MS ends it, to be paged again), the pages that stopped
 * start again, and LE's auto-connect is set again.
 */
void
btd_hid_resume(
	struct btd_hid *hid)
{
	struct btd_hid_device *device;
	uint8_t request[16];
	size_t length;
	uint64_t now;
	unsigned index;
	int error;

	/* Each device in the table. */
	now = btd_now_ms();
	for (index = 0U; index < BTD_HID_MAX; index++) {
		device = &hid->devices[index];
		if (!device->used)
			continue;

		/* A wanted device's pages from the first again. */
		if (device->wanted && device->state == BTD_HID_IDLE) {
			device->paused = 0;
			device->retries = 0U;
			if (!hid_le_type(device->type) && (!device->record.reconnect_initiate || device->record.normally_connectable))
				device->retry_at = now;
		}

		/* An open BR/EDR link is asked whether its device is still there. */
		if (device->state != BTD_HID_OPEN || hid_le_type(device->type))
			continue;
		error = btd_l2cap_echo(&device->l2cap, request, sizeof(request), &length);
		if (error != 0)
			continue;
		hid_send(hid, device, BTD_CID_SIGNALLING, request, length);
		device->echo_deadline = now + BTD_HID_ECHO_MS;
	}

	/* Succeeded: the auto-connect is set again by the tick. */
	hid_le_disarm(hid);
}

/*
 * Takes over the connection of a pairing that succeeded (the pairing's
 * handoff, phase005 section 9.2): a BR/EDR device whose class is a
 * peripheral's goes on to its HID records on the same link, an LE device
 * whose appearance is a HID device's to its attributes; its record is a
 * candidate until that confirms it.  Returns 1 when taken, 0 when the
 * pairing should end the connection.
 */
int
btd_hid_handoff(
	void *context,
	const uint8_t *address,
	unsigned type,
	uint16_t handle,
	const struct btd_bond *bond)
{
	struct btd_hid *hid;
	struct btd_hid_device *device;
	const struct btd_device *seen;
	uint32_t device_class;
	uint16_t appearance;
	unsigned index;
	int keyed;
	int same;
	int policy;
	int le;
	int error;

	/* A bond with its key: BR/EDR's link key, or LE's LTK of 16 bytes. */
	hid = context;
	le = hid_le_type(type);
	keyed = 0;
	if (!le && bond->have_link_key)
		keyed = 1;
	if (le && bond->have_ltk && bond->key_size == HID_KEY_SIZE)
		keyed = 1;
	if (!keyed)
		return 0;

	/* The class or the appearance the last scan saw for it. */
	device_class = 0U;
	appearance = 0U;
	for (index = 0U; index < hid->session->devices.count; index++) {
		seen = &hid->session->devices.entries[index];
		same = memcmp(seen->address, address, BTD_ADDRESS_BYTES);
		if (same != 0 || seen->type != type)
			continue;
		if (seen->has_class)
			device_class = seen->class_of_device;
		if (seen->has_appearance)
			appearance = seen->appearance;
	}

	/* Only a device that looks like a HID device (phase005 Q4, Q26). */
	policy = btd_hid_policy_after_pair(device_class);
	if (le)
		policy = btd_hid_policy_after_le_pair(appearance);
	if (!policy)
		return 0;

	/* Its slot, under its bond's address (an LE device's identity) (a full table leaves the connection to the pairing to end). */
	device = hid_find_type(hid, bond->address, bond->type);
	if (device == NULL)
		device = hid_slot(hid, bond->address, bond->type);
	if (device == NULL || device->state != BTD_HID_IDLE)
		return 0;

	/* Its record: a candidate, named after the bond. */
	memset(&device->record, 0, sizeof(device->record));
	memcpy(device->record.address, bond->address, BTD_ADDRESS_BYTES);
	device->record.type = bond->type;
	device->record.le = le;
	device->record.device_class = device_class;
	device->record.appearance = appearance;
	(void)snprintf(device->record.name, sizeof(device->record.name), "%s", bond->name);
	device->have_descriptor = 0;
	error = btd_hidcache_write(hid->keys_folder, hid->session->address, &device->record);
	if (error != 0)
		return 0;

	/* The link: encrypted with a key of 16 bytes by the pairing, now the HID host's. */
	hid_reset_link(device);
	device->connected = 1;
	device->handle = handle;
	device->encrypted = 1;
	device->key_size = HID_KEY_SIZE;
	device->inbound = 0;
	device->wanted = 1;
	device->asked = 0;
	device->total_deadline = btd_now_ms() + BTD_HID_TOTAL_MS;
	error = btd_router_assign(hid->router, handle, BTD_OWNER_HID);
	if (error != 0) {
		device->connected = 0;
		return 0;
	}

	/* LE: its attributes next. */
	if (le) {
		hid_gatt_start(hid, device);
		return 1;
	}

	/* Succeeded: BR/EDR's HID records next; from now on it may connect by itself (section 9.2). */
	hid_page_scan(hid);
	hid_sdp_start(hid, device);
	return 1;
}

/*
 * Tells the router whether a device connecting to bluetoothd (or whose
 * controller asks for its key) is wanted: in the table with a record, and
 * not disconnected by the user.
 */
int
btd_hid_wants(
	void *context,
	const uint8_t *address)
{
	struct btd_hid *hid;
	struct btd_hid_device *device;

	/* The device. */
	hid = context;
	device = hid_find(hid, address);
	if (device == NULL)
		return 0;

	/* Wanted, or connecting already (its key is asked during its own connection). */
	if (device->wanted || device->state != BTD_HID_IDLE)
		return 1;

	/* Not wanted. */
	return 0;
}

/*
 * Tells the router whether a connection to a device is the HID host's: it
 * paged it or accepted it.
 */
int
btd_hid_claims(
	void *context,
	const uint8_t *address)
{
	struct btd_hid *hid;
	struct btd_hid_device *device;

	/* The device (an LE one may come under its resolvable private address), paging or accepting. */
	hid = context;
	device = hid_find(hid, address);
	if (device == NULL)
		device = hid_le_resolve(hid, address);
	if (device == NULL)
		return 0;
	if (device->state == BTD_HID_PAGING)
		return 1;

	/* An LE device the auto-connect waits for (the controller connects it from the list). */
	if (hid_le_type(device->type) && hid->le_armed && device->wanted && device->state == BTD_HID_IDLE)
		return 1;

	/* Not the HID host's. */
	return 0;
}

/*
 * Takes a packet the router gives the HID host (its handler): an event of
 * its connections or devices, or ACL data.
 */
void
btd_hid_handle(
	void *context,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length)
{
	struct btd_hid *hid;

	/* The HID host this handler serves. */
	(void)session;
	hid = context;

	/* ACL data of a connection. */
	if (length >= 1U && packet[0] == BT_PACKET_ACL) {
		hid_acl(hid, packet, length);
		return;
	}

	/* Succeeded: an event, whose parameters' length the session checked. */
	if (length < 3U || packet[0] != BT_PACKET_EVENT)
		return;
	hid_event(hid, packet + 3, (size_t)packet[2], packet[1]);
}

/*
 * Tells whether a device just paired goes on to HID on the same link
 * (phase005 section 9.2, the user's decision Q4: connect after pairing):
 * a device whose major class is a peripheral's.
 */
int
btd_hid_policy_after_pair(
	uint32_t device_class)
{
	unsigned major;

	/* The major device class. */
	major = (unsigned)((device_class >> 8) & 0x1fU);
	if (major == HID_MAJOR_PERIPHERAL)
		return 1;

	/* Not a HID device as far as its class says. */
	return 0;
}

/*
 * Tells whether an LE device just paired goes on to HOGP on the same link
 * (phase005 section 9.2): a device whose appearance's category is a HID
 * device's (0x03C0 to 0x03FF, review M5).
 */
int
btd_hid_policy_after_le_pair(
	uint16_t appearance)
{
	/* The category, the top 10 bits. */
	if ((appearance >> 6) == HID_APPEARANCE_HID)
		return 1;

	/* Not a HID device as far as its appearance says. */
	return 0;
}

/*
 * Tells whether a device the user disconnected is wanted back (the user's
 * decision Q5: no, until connected again).
 */
int
btd_hid_policy_after_disconnect(
	void)
{
	/* Not wanted back. */
	return 0;
}

/* Finds a device of the table by its address, or NULL. */
static struct btd_hid_device *
hid_find(
	struct btd_hid *hid,
	const uint8_t *address)
{
	unsigned index;
	int same;

	/* Each slot in use. */
	for (index = 0U; index < BTD_HID_MAX; index++) {
		if (!hid->devices[index].used)
			continue;
		same = memcmp(hid->devices[index].address, address, BTD_ADDRESS_BYTES);
		if (same == 0)
			return &hid->devices[index];
	}

	/* None. */
	return NULL;
}

/* Finds a device of the table by its address and type, or NULL. */
static struct btd_hid_device *
hid_find_type(
	struct btd_hid *hid,
	const uint8_t *address,
	unsigned type)
{
	unsigned index;
	int same;

	/* Each slot in use. */
	for (index = 0U; index < BTD_HID_MAX; index++) {
		if (!hid->devices[index].used || hid->devices[index].type != type)
			continue;
		same = memcmp(hid->devices[index].address, address, BTD_ADDRESS_BYTES);
		if (same == 0)
			return &hid->devices[index];
	}

	/* None. */
	return NULL;
}

/* Finds the device of a connection, or NULL. */
static struct btd_hid_device *
hid_by_handle(
	struct btd_hid *hid,
	uint16_t handle)
{
	unsigned index;

	/* Each connected device. */
	for (index = 0U; index < BTD_HID_MAX; index++) {
		if (hid->devices[index].used && hid->devices[index].connected && hid->devices[index].handle == handle)
			return &hid->devices[index];
	}

	/* None. */
	return NULL;
}

/* Takes a free slot for a device, or NULL when the table is full. */
static struct btd_hid_device *
hid_slot(
	struct btd_hid *hid,
	const uint8_t *address,
	unsigned type)
{
	struct btd_hid_device *device;
	unsigned index;

	/* The first free slot. */
	for (index = 0U; index < BTD_HID_MAX; index++) {
		device = &hid->devices[index];
		if (device->used)
			continue;

		/* Succeeded: a new device, nothing connected. */
		memset(device, 0, sizeof(*device));
		device->used = 1;
		memcpy(device->address, address, BTD_ADDRESS_BYTES);
		memcpy(device->record.address, address, BTD_ADDRESS_BYTES);
		device->type = type;
		device->record.type = type;
		if (hid_le_type(type))
			device->record.le = 1;
		btd_hog_init(&device->hog);
		device->bridge = -1;
		device->event = -1;
		device->touch_event = -1;
		device->state = BTD_HID_IDLE;
		btd_l2cap_init(&device->l2cap);
		btd_l2cap_set_accept(&device->l2cap, hid_accept, device);
		return device;
	}

	/* The table is full. */
	return NULL;
}

/* Takes one event of the HID host's connections or devices. */
static void
hid_event(
	struct btd_hid *hid,
	const uint8_t *parameters,
	size_t length,
	uint8_t code)
{
	/* Each event the HID host knows; anything else is passed over. */
	switch (code) {
	case HID_EVENT_REQUEST:
		hid_request(hid, parameters, length);
		break;
	case HID_EVENT_CONNECTED:
		hid_connected(hid, parameters, length);
		break;
	case HID_EVENT_KEY_REQUEST:
		if (length >= BTD_ADDRESS_BYTES)
			hid_key_request(hid, parameters);
		break;
	case HID_EVENT_AUTHENTICATED:
		hid_authenticated(hid, parameters, length);
		break;
	case HID_EVENT_ENCRYPTION:
	case HID_EVENT_ENCRYPTION_V2:
		hid_encryption(hid, parameters, length);
		break;
	case HID_EVENT_DISCONNECTED:
		hid_disconnected(hid, parameters, length);
		break;
	case HID_EVENT_LE_META:
		hid_le_meta(hid, parameters, length);
		break;
	default:
		hid->refused++;
		break;
	}
}

/*
 * Takes Connection Request (address, class, link type) of a wanted device
 * (the router asked btd_hid_wants): accepted with bluetoothd as central
 * (review S6), unless its connection is under way already.
 */
static void
hid_request(
	struct btd_hid *hid,
	const uint8_t *parameters,
	size_t length)
{
	struct btd_hid_device *device;
	uint8_t accept[BTD_ADDRESS_BYTES + 1U];
	uint8_t reject[BTD_ADDRESS_BYTES + 1U];
	int error;

	/* A whole event of a device in the table. */
	if (length < 10U)
		return;
	device = hid_find(hid, parameters);

	/* A device whose connection is under way already (its page crossed this) is refused, as the router refuses strangers. */
	if (device == NULL || device->state != BTD_HID_IDLE) {
		memcpy(reject, parameters, BTD_ADDRESS_BYTES);
		reject[BTD_ADDRESS_BYTES] = HID_REASON_REFUSED;
		(void)hid_command(hid, HID_REJECT_CONNECTION, reject, sizeof(reject));
		hid->refused++;
		return;
	}

	/* Accept Connection Request: the address, the central's role. */
	memcpy(accept, parameters, BTD_ADDRESS_BYTES);
	accept[BTD_ADDRESS_BYTES] = HID_ROLE_CENTRAL;
	error = hid_command(hid, HID_ACCEPT_CONNECTION, accept, sizeof(accept));
	if (error != 0)
		return;

	/* Succeeded: its connection comes (Connection Complete), the device's own. */
	device->inbound = 1;
	device->state = BTD_HID_PAGING;
	device->state_deadline = btd_now_ms() + BTD_HID_PAGE_MS;
	device->total_deadline = btd_now_ms() + BTD_HID_TOTAL_MS;
	device->retry_at = 0U;
}

/*
 * Takes Connection Complete (status, handle, address, link type,
 * encryption) of a device paged or accepted: bluetoothd authenticates the
 * link it paged; a device that connected by itself authenticates it, or
 * asks for its channels first (answered Pending, section 9.8).
 */
static void
hid_connected(
	struct btd_hid *hid,
	const uint8_t *parameters,
	size_t length)
{
	struct btd_hid_device *device;
	uint8_t handle[2];
	int error;

	/* A whole event of a device paged or accepted. */
	if (length < 11U)
		return;
	device = hid_find(hid, parameters + 3);
	if (device == NULL || device->state != BTD_HID_PAGING)
		return;

	/* The page failed: tried again later. */
	if (parameters[0] != 0U) {
		hid_ended(hid, device, "unreachable");
		return;
	}

	/* Connected. */
	hid_reset_link(device);
	device->connected = 1;
	device->handle = hid_handle_at(parameters + 1);
	device->state = BTD_HID_AUTHENTICATING;
	device->state_deadline = btd_now_ms() + BTD_HID_SECURITY_MS;

	/* A device that connected by itself starts the security (or asks for a channel first). */
	if (device->inbound)
		return;

	/* Succeeded: Authentication Requested (the controller asks for the key). */
	hid_put16(handle, device->handle);
	device->authenticating = 1;
	error = hid_command(hid, HID_AUTHENTICATION, handle, sizeof(handle));
	if (error != 0)
		hid_fail(hid, device, "security");
}

/* Answers Link Key Request with the bond's key, or none (the device then fails to authenticate). */
static void
hid_key_request(
	struct btd_hid *hid,
	const uint8_t *address)
{
	struct btd_bond bond;
	uint8_t reply[BTD_ADDRESS_BYTES + 16U];
	int error;

	/* The bond's key. */
	error = btd_keys_read(hid->keys_folder, hid->session->address, address, BTD_ADDRESS_BREDR, &bond);
	if (error != 0 || !bond.have_link_key) {
		memset(&bond, 0, sizeof(bond));
		(void)hid_command(hid, HID_LINK_KEY_NEGATIVE, address, BTD_ADDRESS_BYTES);
		return;
	}

	/* Succeeded: Link Key Request Reply; the keys are not kept. */
	memcpy(reply, address, BTD_ADDRESS_BYTES);
	memcpy(reply + BTD_ADDRESS_BYTES, bond.link_key, sizeof(bond.link_key));
	(void)hid_command(hid, HID_LINK_KEY_REPLY, reply, sizeof(reply));
	memset(&bond, 0, sizeof(bond));
	memset(reply, 0, sizeof(reply));
}

/* Takes Authentication Complete (status, handle): the encryption next, or why it failed. */
static void
hid_authenticated(
	struct btd_hid *hid,
	const uint8_t *parameters,
	size_t length)
{
	struct btd_hid_device *device;
	uint8_t encryption[3];
	int error;

	/* A whole event of a device's connection. */
	if (length < 3U)
		return;
	device = hid_by_handle(hid, hid_handle_at(parameters + 1));
	if (device == NULL)
		return;
	device->authenticating = 0;

	/* The key the device no longer has, or any other failure. */
	if (parameters[0] == HID_STATUS_KEY_MISSING) {
		hid_fail(hid, device, "key-missing");
		return;
	}

	/* Any other failure is the security's. */
	if (parameters[0] != 0U) {
		hid_fail(hid, device, "security");
		return;
	}

	/* An encrypted link needs no more. */
	if (device->encrypted)
		return;

	/* Succeeded: Set Connection Encryption, on. */
	device->state = BTD_HID_ENCRYPTING;
	hid_put16(encryption, device->handle);
	encryption[2] = 0x01U;
	error = hid_command(hid, HID_SET_ENCRYPTION, encryption, sizeof(encryption));
	if (error != 0)
		hid_fail(hid, device, "security");
}

/* Takes Encryption Change (status, handle, enabled): the key's size is checked next. */
static void
hid_encryption(
	struct btd_hid *hid,
	const uint8_t *parameters,
	size_t length)
{
	struct btd_hid_device *device;

	/* A whole event of a device's connection. */
	if (length < 4U)
		return;
	device = hid_by_handle(hid, hid_handle_at(parameters + 1));
	if (device == NULL)
		return;

	/* The key the device no longer has (LE's PIN or Key Missing), or not encrypted (or no longer). */
	device->le_encrypting = 0;
	if (parameters[0] == HID_STATUS_KEY_MISSING) {
		hid_fail(hid, device, "key-missing");
		return;
	} else if (parameters[0] != 0U || parameters[3] == 0U) {
		hid_fail(hid, device, "security");
		return;
	}

	/* A link encrypted already (a key refreshed, the pairing's link handed over) needs nothing more. */
	if (device->state != BTD_HID_AUTHENTICATING && device->state != BTD_HID_ENCRYPTING)
		return;

	/* LE: the bond's key was 16 bytes (CONNECT checked it); its attributes next. */
	if (hid_le_type(device->type)) {
		device->encrypted = 1;
		device->key_size = HID_KEY_SIZE;
		hid_gatt_start(hid, device);
		return;
	}

	/* Succeeded: BR/EDR's key's size. */
	hid_key_size(hid, device);
}

/*
 * Reads the encryption key's size (KNOB: only 16 bytes) and goes on: the
 * channels the device asked for before are accepted, then the HID
 * records, or (a device that connected by itself with its descriptor
 * kept) its channels.
 */
static void
hid_key_size(
	struct btd_hid *hid,
	struct btd_hid_device *device)
{
	uint8_t handle[2];
	uint8_t answer[BTD_SIGNAL_MAX];
	size_t length;
	int error;

	/* Read Encryption Key Size: the handle, then the size. */
	hid_put16(handle, device->handle);
	error = hid_command(hid, HID_READ_KEY_SIZE, handle, sizeof(handle));
	if (error != 0 || hid->session->returned_length < 3U) {
		hid_fail(hid, device, "key-size");
		return;
	}

	/* A shorter key is refused. */
	device->key_size = hid->session->returned[2];
	if (device->key_size != HID_KEY_SIZE) {
		hid_fail(hid, device, "key-size");
		return;
	}

	/* Encrypted: the channels the device asked for meanwhile are accepted. */
	device->encrypted = 1;
	error = btd_l2cap_answer_pending(&device->l2cap, device->handle, BTD_L2CAP_SUCCESS, answer, sizeof(answer), &length);
	if (error == 0 && length != 0U)
		hid_send(hid, device, BTD_CID_SIGNALLING, answer, length);

	/* A device that connected by itself with its descriptor kept opens its channels itself. */
	if (device->inbound && device->have_descriptor) {
		device->state = BTD_HID_CHANNELS;
		device->state_deadline = btd_now_ms() + BTD_HID_CHANNELS_MS;
		return;
	}

	/* Succeeded: its HID records next. */
	hid_sdp_start(hid, device);
}

/* Takes Disconnection Complete (status, handle, reason). */
static void
hid_disconnected(
	struct btd_hid *hid,
	const uint8_t *parameters,
	size_t length)
{
	struct btd_hid_device *device;

	/* A whole event of a device's connection that ended. */
	if (length < 4U || parameters[0] != 0U)
		return;
	device = hid_by_handle(hid, hid_handle_at(parameters + 1));
	if (device == NULL)
		return;

	/* Succeeded: ended (a disconnection asked for keeps its reason). */
	if (device->state == BTD_HID_CLOSING) {
		hid_ended(hid, device, device->last);
		return;
	}

	/* A device that went by itself. */
	hid_ended(hid, device, "lost");
}

/* Takes an ACL packet of a device's connection: a frame, once whole, goes to its channel. */
static void
hid_acl(
	struct btd_hid *hid,
	const uint8_t *packet,
	size_t length)
{
	struct btd_hid_device *device;
	struct btd_acl acl;
	const uint8_t *payload;
	size_t payload_length;
	uint16_t cid;
	int whole;
	int error;

	/* The packet's connection. */
	error = btd_acl_parse(packet, length, &acl);
	if (error != 0)
		return;
	device = hid_by_handle(hid, acl.handle);
	if (device == NULL)
		return;

	/* The frame, once whole. */
	whole = btd_reassembly_feed(&device->reassembly, &acl);
	if (whole <= 0)
		return;
	payload = device->reassembly.frame + BTD_L2CAP_HEADER;
	payload_length = device->reassembly.expected - BTD_L2CAP_HEADER;
	cid = (uint16_t)(device->reassembly.frame[2] | (device->reassembly.frame[3] << 8));

	/* LE's fixed channels. */
	if (hid_le_type(device->type)) {
		hid_le_frame(hid, device, cid, payload, payload_length);
		return;
	}

	/* Each channel: the signalling, SDP's, the control's, the interrupt's; anything else is passed over. */
	if (cid == BTD_CID_SIGNALLING) {
		hid_signal(hid, device, payload, payload_length);
	} else if (cid == device->sdp_cid && cid != 0U) {
		hid_sdp_input(hid, device, payload, payload_length);
	} else if (cid == device->control_cid && cid != 0U) {
		hid_control(hid, device, payload, payload_length);
	} else if (cid == device->interrupt_cid && cid != 0U) {
		hid_interrupt(device, payload, payload_length);
	}
}

/*
 * Takes a signalling frame of a device's connection: its answers go back,
 * a channel asked for before the encryption starts the security, and the
 * channels that opened or closed move the connection on.
 */
static void
hid_signal(
	struct btd_hid *hid,
	struct btd_hid_device *device,
	const uint8_t *payload,
	size_t length)
{
	struct btd_signal_effect effect;
	uint8_t answer[BTD_SIGNAL_MAX];
	uint8_t handle[2];
	size_t answer_length;
	unsigned index;
	uint16_t cid;
	int error;

	/* The commands, answered; the answer to a resumed link's Echo Request ends its wait. */
	(void)btd_l2cap_signal(&device->l2cap, device->handle, 0, payload, length, answer, sizeof(answer), &answer_length, &effect);
	if (answer_length != 0U)
		hid_send(hid, device, BTD_CID_SIGNALLING, answer, answer_length);
	if (effect.echo)
		device->echo_deadline = 0U;

	/* A channel asked for before the encryption: bluetoothd authenticates the link now (section 9.8). */
	if (!device->encrypted && !device->authenticating && device->state == BTD_HID_AUTHENTICATING) {
		for (index = 0U; index < BTD_CHANNELS_MAX; index++) {
			if (device->l2cap.channels[index].state != BTD_CHANNEL_PENDING)
				continue;
			hid_put16(handle, device->handle);
			device->authenticating = 1;
			error = hid_command(hid, HID_AUTHENTICATION, handle, sizeof(handle));
			if (error != 0)
				hid_fail(hid, device, "security");
			break;
		}
	}

	/* The channels that closed, then those that opened. */
	for (index = 0U; index < effect.closed_count; index++) {
		cid = effect.closed[index];
		hid_channel_closed(hid, device, cid);
	}

	/* Then those that opened. */
	for (index = 0U; index < effect.opened_count; index++) {
		cid = effect.opened[index];
		hid_channel_opened(hid, device, cid);
	}
}

/* Moves the connection on for a channel that opened: SDP's query, or the HID channels. */
static void
hid_channel_opened(
	struct btd_hid *hid,
	struct btd_hid_device *device,
	uint16_t cid)
{
	struct btd_channel *channel;

	/* The channel and its PSM. */
	channel = btd_l2cap_channel(&device->l2cap, cid);
	if (channel == NULL)
		return;

	/* SDP's: the first request. */
	if (cid == device->sdp_cid) {
		hid_sdp_send(hid, device);
		return;
	}

	/* The control channel (ours or the device's): ours goes on to the interrupt channel. */
	if (channel->psm == BTD_SDP_PSM_CONTROL) {
		device->control_cid = cid;
		if (!channel->inbound && device->interrupt_cid == 0U)
			hid_open_channel(hid, device, BTD_SDP_PSM_INTERRUPT);
	} else if (channel->psm == BTD_SDP_PSM_INTERRUPT) {
		device->interrupt_cid = cid;
	}

	/* Both open: the input device next. */
	if (device->control_cid == 0U || device->interrupt_cid == 0U)
		return;
	channel = btd_l2cap_channel(&device->l2cap, device->control_cid);
	if (channel == NULL || channel->state != BTD_CHANNEL_OPEN)
		return;
	channel = btd_l2cap_channel(&device->l2cap, device->interrupt_cid);
	if (channel == NULL || channel->state != BTD_CHANNEL_OPEN)
		return;

	/* Succeeded: both HID channels open. */
	hid_channels_open(hid, device);
}

/* Moves the connection on for a channel that closed: SDP's after its query, or a HID channel that went. */
static void
hid_channel_closed(
	struct btd_hid *hid,
	struct btd_hid_device *device,
	uint16_t cid)
{
	/* SDP's: forgotten (the HID channels were asked for when its query ended). */
	if (cid == device->sdp_cid) {
		device->sdp_cid = 0U;
		if (device->state == BTD_HID_SDP)
			hid_fail(hid, device, "no-hid");
		return;
	}

	/* A HID channel that went: the connection is of no use (a disconnection asked for goes on). */
	if (cid != device->control_cid && cid != device->interrupt_cid)
		return;
	if (cid == device->control_cid)
		device->control_cid = 0U;
	if (cid == device->interrupt_cid)
		device->interrupt_cid = 0U;
	if (device->state == BTD_HID_CLOSING)
		return;

	/* Succeeded: ended. */
	hid_fail(hid, device, "lost");
}

/* Opens SDP's channel (its query starts when it is open). */
static void
hid_sdp_start(
	struct btd_hid *hid,
	struct btd_hid_device *device)
{
	uint8_t request[16];
	size_t length;
	uint16_t cid;
	int error;

	/* The HID record first. */
	device->state = BTD_HID_SDP;
	device->state_deadline = btd_now_ms() + BTD_HID_SDP_MS;
	device->pnp_asked = 0;
	btd_sdp_init(&device->sdp, BTD_SDP_UUID_HID, 1U);

	/* The channel to PSM 1. */
	error = btd_l2cap_connect(&device->l2cap, device->handle, BTD_SDP_PSM_SDP, request, sizeof(request), &length, &cid);
	if (error != 0) {
		hid_fail(hid, device, "protocol");
		return;
	}

	/* Succeeded: asked for. */
	device->sdp_cid = cid;
	hid_send(hid, device, BTD_CID_SIGNALLING, request, length);
}

/* Sends the query's next request on SDP's channel. */
static void
hid_sdp_send(
	struct btd_hid *hid,
	struct btd_hid_device *device)
{
	uint8_t request[BTD_SDP_REQUEST_MAX];
	size_t length;
	int error;

	/* The request (the continuation state, when one came). */
	error = btd_sdp_request(&device->sdp, request, sizeof(request), &length);
	if (error != 0) {
		hid_fail(hid, device, "protocol");
		return;
	}

	/* Succeeded: sent. */
	hid_send(hid, device, device->sdp_cid, request, length);
}

/* Takes an SDP response: the next request, or the query's end. */
static void
hid_sdp_input(
	struct btd_hid *hid,
	struct btd_hid_device *device,
	const uint8_t *pdu,
	size_t length)
{
	int result;

	/* Only during the query. */
	if (device->state != BTD_HID_SDP)
		return;

	/* The response's meaning. */
	result = btd_sdp_input(&device->sdp, pdu, length);
	if (result == BTD_SDP_MORE) {
		hid_sdp_send(hid, device);
		return;
	}

	/* A query that failed ends the connection with why. */
	if (result != BTD_SDP_DONE) {
		hid_fail(hid, device, device->sdp.why);
		return;
	}

	/* Succeeded: the record read. */
	hid_sdp_done(hid, device);
}

/*
 * Reads the record of a query that ended: the HID record (its descriptor,
 * its flags), then the PnP record; SDP's channel is closed and the HID
 * channels asked for.
 */
static void
hid_sdp_done(
	struct btd_hid *hid,
	struct btd_hid_device *device)
{
	struct btd_hid_record record;
	uint8_t request[16];
	size_t length;
	uint16_t cid;
	int error;

	/* The HID record: none is no HID device (its record goes); a descriptor past 4096 bytes is refused. */
	memset(&record, 0, sizeof(record));
	if (!device->pnp_asked) {
		error = btd_sdp_hid(&device->sdp, &record);
		if (error == ENOENT) {
			(void)btd_hidcache_forget(hid->keys_folder, hid->session->address, device->address, BTD_ADDRESS_BREDR);
			hid_fail(hid, device, "no-hid");
			return;
		}

		/* A descriptor too long, or a record of the wrong form. */
		if (error == E2BIG) {
			hid_fail(hid, device, "descriptor");
			return;
		}

		/* Any other: the protocol broken. */
		if (error != 0) {
			hid_fail(hid, device, "protocol");
			return;
		}

		/* Kept in the record. */
		device->record.reconnect_initiate = record.reconnect_initiate;
		device->record.normally_connectable = record.normally_connectable;
		device->record.virtual_cable = record.virtual_cable;
		device->record.boot_device = record.boot_device;
		device->record.country = record.country;
		if (device->record.name[0] == '\0')
			(void)snprintf(device->record.name, sizeof(device->record.name), "%s", record.name);
		memcpy(device->record.descriptor, record.descriptor, record.descriptor_size);
		device->record.descriptor_size = record.descriptor_size;
		device->have_descriptor = 1;

		/* The PnP record next, on the same channel. */
		device->pnp_asked = 1;
		btd_sdp_init(&device->sdp, BTD_SDP_UUID_PNP, 2U);
		hid_sdp_send(hid, device);
		return;
	}

	/* The PnP numbers, when there is a record (none: 0). */
	error = btd_sdp_pnp(&device->sdp, &record);
	if (error == 0) {
		device->record.vendor = record.vendor;
		device->record.product = record.product;
		device->record.version = record.version;
	}

	/* SDP's channel closes; the HID channels are asked for. */
	device->state = BTD_HID_CHANNELS;
	device->state_deadline = btd_now_ms() + BTD_HID_CHANNELS_MS;
	error = btd_l2cap_disconnect(&device->l2cap, device->sdp_cid, request, sizeof(request), &length);
	if (error == 0)
		hid_send(hid, device, BTD_CID_SIGNALLING, request, length);

	/* A device that connected by itself asked for its channels meanwhile: they are taken as they open. */
	cid = hid_asked_control(device);
	if (cid != 0U) {
		hid_channel_opened(hid, device, cid);
		return;
	}

	/* Succeeded: the control channel first. */
	hid_open_channel(hid, device, BTD_SDP_PSM_CONTROL);
}

/* Asks for a HID channel (control or interrupt). */
static void
hid_open_channel(
	struct btd_hid *hid,
	struct btd_hid_device *device,
	uint16_t psm)
{
	uint8_t request[16];
	size_t length;
	uint16_t cid;
	int error;

	/* The request. */
	error = btd_l2cap_connect(&device->l2cap, device->handle, psm, request, sizeof(request), &length, &cid);
	if (error != 0) {
		hid_fail(hid, device, "protocol");
		return;
	}

	/* Succeeded: asked for, its CID known before it opens. */
	if (psm == BTD_SDP_PSM_CONTROL)
		device->control_cid = cid;
	if (psm == BTD_SDP_PSM_INTERRUPT)
		device->interrupt_cid = cid;
	hid_send(hid, device, BTD_CID_SIGNALLING, request, length);
}

/* Finds the control channel the device asked for itself (any state but free), or 0. */
static uint16_t
hid_asked_control(
	struct btd_hid_device *device)
{
	const struct btd_channel *channel;
	unsigned index;

	/* Each channel of the table. */
	for (index = 0U; index < BTD_CHANNELS_MAX; index++) {
		channel = &device->l2cap.channels[index];
		if (channel->state != BTD_CHANNEL_FREE && channel->inbound && channel->psm == BTD_SDP_PSM_CONTROL)
			return channel->local_cid;
	}

	/* None. */
	return 0U;
}

/*
 * Goes on once both HID channels are open: a boot device is set to the
 * report protocol first (its handshake awaited BTD_HID_HANDSHAKE_MS),
 * then the input device is made.
 */
static void
hid_channels_open(
	struct btd_hid *hid,
	struct btd_hid_device *device)
{
	uint8_t message[1];

	/* Only once, while the channels are being opened. */
	if (device->state != BTD_HID_CHANNELS && device->state != BTD_HID_AUTHENTICATING && device->state != BTD_HID_ENCRYPTING)
		return;

	/* A device without the boot protocol needs no SET_PROTOCOL (phase005 Q15). */
	if (!device->record.boot_device) {
		hid_setup(hid, device);
		return;
	}

	/* Succeeded: SET_PROTOCOL (report), its handshake awaited. */
	message[0] = btd_hidp_header(BTD_HIDP_SET_PROTOCOL, BTD_HIDP_PROTOCOL_REPORT);
	hid_send(hid, device, device->control_cid, message, sizeof(message));
	device->state = BTD_HID_HANDSHAKE;
	device->state_deadline = btd_now_ms() + BTD_HID_HANDSHAKE_MS;
}

/*
 * Takes a message on the control channel: the handshake of SET_PROTOCOL,
 * the device's unplug (it forgets bluetoothd: the bond goes, Q9), and
 * requests of the host that are not supported.
 */
static void
hid_control(
	struct btd_hid *hid,
	struct btd_hid_device *device,
	const uint8_t *frame,
	size_t length)
{
	struct btd_hidp message;
	uint8_t address[BTD_ADDRESS_BYTES];
	uint8_t answer[1];
	int error;

	/* The message. */
	error = btd_hidp_parse(frame, length, &message);
	if (error != 0)
		return;

	/* Each type: the handshake awaited makes the input device (any result: a refusal changes nothing). */
	switch (message.type) {
	case BTD_HIDP_HANDSHAKE:
		if (device->state == BTD_HID_HANDSHAKE)
			hid_setup(hid, device);
		break;
	case BTD_HIDP_CONTROL:
		/* The device unplugged: its bond and record go, and the link. */
		if (message.parameter != BTD_HIDP_VIRTUAL_CABLE_UNPLUG)
			break;
		memcpy(address, device->address, BTD_ADDRESS_BYTES);
		(void)btd_keys_forget(hid->keys_folder, hid->session->address, address, BTD_ADDRESS_BREDR);
		(void)btd_hidcache_forget(hid->keys_folder, hid->session->address, address, BTD_ADDRESS_BREDR);
		hid_drop(hid, device, 0);
		break;
	case BTD_HIDP_GET_REPORT:
	case BTD_HIDP_SET_REPORT:
	case BTD_HIDP_GET_PROTOCOL:
	case BTD_HIDP_SET_PROTOCOL:
		/* A host is not asked such things. */
		answer[0] = btd_hidp_header(BTD_HIDP_HANDSHAKE, BTD_HIDP_UNSUPPORTED);
		hid_send(hid, device, device->control_cid, answer, sizeof(answer));
		break;
	default:
		break;
	}
}

/* Takes a message on the interrupt channel: an input report goes to the input device. */
static void
hid_interrupt(
	struct btd_hid_device *device,
	const uint8_t *frame,
	size_t length)
{
	struct btd_hidp message;
	int error;

	/* The message; one too long for the bridge is dropped and counted (phase005 Q22). */
	error = btd_hidp_parse(frame, length, &message);
	if (error == E2BIG) {
		device->oversize++;
		return;
	} else if (error != 0) {
		return;
	}

	/* Only DATA of an input report goes on. */
	if (message.type != BTD_HIDP_DATA || message.parameter != BTD_HIDP_REPORT_INPUT)
		return;

	/* Succeeded: the report, as the device sent it (its report ID first when it numbers them). */
	hid_report(device, message.data, message.length);
}

/*
 * Passes one input report to the input device, or keeps it until the
 * device is made (the first reports come with the channels, review S11).
 */
static void
hid_report(
	struct btd_hid_device *device,
	const uint8_t *report,
	size_t length)
{
	struct btd_hid_report *kept;
	ssize_t written;

	/* A report too long for the bridge is dropped (phase005 Q22). */
	if (length == 0U || length > BTD_HIDP_REPORT_MAX) {
		device->oversize++;
		return;
	}

	/* Before the input device: kept, while there is room. */
	if (device->bridge < 0) {
		if (device->queued >= BTD_HID_QUEUE || length > BTD_HID_QUEUE_BYTES) {
			device->dropped++;
			return;
		}

		/* Kept in order. */
		kept = &device->queue[device->queued];
		kept->length = length;
		memcpy(kept->bytes, report, length);
		device->queued++;
		return;
	}

	/* Written: one report a write; one the kernel refuses is counted (the link stays, review M7). */
	written = write(device->bridge, report, length);
	if (written != (ssize_t)length) {
		device->malformed++;
		return;
	}

	/* Succeeded: passed on. */
	device->reports++;
}

/*
 * Makes the input device: the bridge opened by the parent, the setup with
 * the descriptor written, its nodes read; a BR/EDR device is open then, an
 * LE one turns its notifications on first.
 */
static void
hid_setup(
	struct btd_hid *hid,
	struct btd_hid_device *device)
{
	unsigned actions;
	int error;

	/* A device without its descriptor cannot be made. */
	if (!device->have_descriptor) {
		hid_fail(hid, device, "descriptor");
		return;
	}

	/* The setup; the kernel's refusal names why. */
	error = hid_setup_write(hid, device);
	if (error == EINVAL && device->setup_tries < BTD_HID_SETUP_TRIES) {
		/* The kernel could not copy it (section 3): written again a little later, from the tick. */
		device->setup_tries++;
		device->state = BTD_HID_SETUP;
		device->state_deadline = btd_now_ms() + BTD_HID_SETUP_MS;
		return;
	} else if (error == ENOSPC || error == EBUSY) {
		hid_fail(hid, device, "input-full");
		return;
	}

	/* Any other refusal is the descriptor's. */
	if (error != 0) {
		hid_fail(hid, device, "descriptor");
		return;
	}

	/* LE: its notifications are turned on before it is open. */
	if (hid_le_type(device->type)) {
		device->state = BTD_HID_SUBSCRIBE;
		device->state_deadline = 0U;
		actions = btd_hog_resume(&device->hog, btd_now_ms());
		hid_gatt(hid, device, actions);
		return;
	}

	/* Succeeded: BR/EDR is open. */
	hid_open(hid, device);
}

/*
 * Opens a device whose input device is made: the record is confirmed (LE's
 * without its map, read at each connection), the connection's deadlines
 * end, its pages start again from the first, the reports kept meanwhile go
 * on and a client waiting hears it.
 */
static void
hid_open(
	struct btd_hid *hid,
	struct btd_hid_device *device)
{
	static struct btd_hidcache record;
	char line[BTD_HID_ANSWER_MAX];
	unsigned index;
	int next;

	/* The record is confirmed (written whole again). */
	device->record.confirmed = 1;
	record = device->record;
	if (hid_le_type(device->type))
		record.descriptor_size = 0U;
	(void)btd_hidcache_write(hid->keys_folder, hid->session->address, &record);

	/* Open: the connection's deadlines end, its pages start again from the first. */
	device->state = BTD_HID_OPEN;
	device->state_deadline = 0U;
	device->total_deadline = 0U;
	device->retries = 0U;
	device->retry_at = 0U;
	device->paused = 0;
	device->last = NULL;
	device->since_ms = btd_now_ms();

	/* The reports kept meanwhile, in order (LE's notifications too). */
	for (index = 0U; index < device->queued; index++)
		hid_report(device, device->queue[index].bytes, device->queue[index].length);
	device->queued = 0U;
	for (;;) {
		next = btd_hog_next(&device->hog);
		if (!next)
			break;
		hid_report(device, device->hog.report, device->hog.report_length);
	}

	/* Succeeded: a client waiting hears it. */
	if (device->asked) {
		hid_connected_line(hid, device, line, sizeof(line));
		hid_answer(hid, device, line);
	}
}

/* Opens the bridge and writes the setup; returns 0, or the errno value of the open or of the write. */
static int
hid_setup_write(
	struct btd_hid *hid,
	struct btd_hid_device *device)
{
	static struct input_bridge_setup setup;
	char controller[24];
	char peer[24];
	ssize_t written;
	int descriptor;
	int error;

	/* An open of the bridge, from the parent. */
	if (hid->hooks.open_bridge == NULL)
		return ENODEV;
	error = hid->hooks.open_bridge(hid->hooks.context, &descriptor);
	if (error != 0)
		return error;

	/* The setup: Bluetooth, the PnP numbers, the names, the descriptor. */
	memset(&setup, 0, sizeof(setup));
	setup.magic = INPUT_BRIDGE_MAGIC;
	setup.version = INPUT_BRIDGE_VERSION;
	setup.bus = BUS_BLUETOOTH;
	setup.vendor = device->record.vendor;
	setup.product = device->record.product;
	setup.release = device->record.version;
	setup.descriptor_size = (uint32_t)device->record.descriptor_size;
	hid_name(device->record.name, setup.name, sizeof(setup.name));
	btd_format_address(hid->session->address, controller, sizeof(controller));
	btd_format_address(device->address, peer, sizeof(peer));
	(void)snprintf(setup.physical_path, sizeof(setup.physical_path), "bluetooth/%s/%s", controller, peer);
	(void)snprintf(setup.unique_id, sizeof(setup.unique_id), "%s", peer);
	memcpy(setup.descriptor, device->record.descriptor, device->record.descriptor_size);

	/* Written in one write. */
	written = write(descriptor, &setup, sizeof(setup));
	if (written != (ssize_t)sizeof(setup)) {
		error = EIO;
		if (written < 0)
			error = errno;
		(void)close(descriptor);
		return error;
	}

	/* Succeeded: the input device, and its nodes. */
	device->bridge = descriptor;
	hid_numbers(device);
	return 0;
}

/*
 * Reads which /dev/input/eventN nodes the input device has
 * (INPUT_BRIDGE_GET_DEVICE, the user's decision Q2); without the answer
 * the nodes are not known (-1).
 */
static void
hid_numbers(
	struct btd_hid_device *device)
{
	struct input_bridge_device made;
	int reported;
	int status;

	/* Not known unless the kernel says. */
	device->event = -1;
	device->touch_event = -1;
	memset(&made, 0, sizeof(made));
	status = ioctl(device->bridge, INPUT_BRIDGE_GET_DEVICE, &made);
	if (status != 0)
		return;

	/* The nodes. */
	device->event = made.event;
	device->touch_event = made.touch_event;

	/* Succeeded: LE's reports carry an ID as the kernel read the map (review M8). */
	if (!hid_le_type(device->type))
		return;
	reported = 0;
	if ((made.flags & INPUT_BRIDGE_FLAG_REPORT_IDS) != 0U)
		reported = 1;
	if (reported != device->hog.uses_ids)
		btd_hog_set_ids(&device->hog, reported);
}

/*
 * Ends a connection that failed with why: a connected link is
 * disconnected (the end comes on its Disconnection Complete), anything
 * else ends now.
 */
static void
hid_fail(
	struct btd_hid *hid,
	struct btd_hid_device *device,
	const char *why)
{
	uint8_t answer[BTD_SIGNAL_MAX];
	uint8_t disconnect[3];
	size_t length;
	int error;

	/* Ending already, or nothing under way. */
	if (device->state == BTD_HID_CLOSING || device->state == BTD_HID_IDLE)
		return;
	if (why == NULL)
		why = "protocol";
	device->last = why;

	/* Channels still pending are refused for security. */
	error = btd_l2cap_answer_pending(&device->l2cap, device->handle, BTD_L2CAP_SECURITY_BLOCK, answer, sizeof(answer), &length);
	if (error == 0 && length != 0U && device->connected)
		hid_send(hid, device, BTD_CID_SIGNALLING, answer, length);

	/* No connection: a page under way is cancelled, and the end is now. */
	if (!device->connected) {
		if (device->state == BTD_HID_PAGING && !device->inbound)
			hid_cancel_page(hid, device);
		hid_ended(hid, device, why);
		return;
	}

	/* Disconnect; the end comes with its Disconnection Complete (or the close's deadline). */
	hid_put16(disconnect, device->handle);
	disconnect[2] = HID_REASON_USER;
	error = hid_command(hid, HID_DISCONNECT, disconnect, sizeof(disconnect));
	if (error != 0) {
		hid_ended(hid, device, why);
		return;
	}

	/* Succeeded: closing. */
	hid_close_bridge(device);
	device->state = BTD_HID_CLOSING;
	device->state_deadline = btd_now_ms() + BTD_HID_CLOSE_MS;
}

/*
 * Ends a connection (it went, or failed before it was made): the input
 * device goes (the kernel releases its keys), a client waiting hears why,
 * and a wanted device is paged again later.
 */
static void
hid_ended(
	struct btd_hid *hid,
	struct btd_hid_device *device,
	const char *why)
{
	char line[BTD_HID_ANSWER_MAX];
	uint64_t now;

	/* Nothing open any more. */
	if (why == NULL)
		why = "lost";
	hid_close_bridge(device);
	hid_reset_link(device);
	device->state = BTD_HID_IDLE;
	device->state_deadline = 0U;
	device->total_deadline = 0U;
	device->last = why;
	now = btd_now_ms();
	device->since_ms = now;

	/* A client waiting hears why. */
	if (device->asked) {
		(void)snprintf(line, sizeof(line), "ERROR %s", why);
		hid_answer(hid, device, line);
	}

	/* Succeeded: a wanted BR/EDR device that bluetoothd pages is paged again later (LE's come through the auto-connect). */
	if (hid_le_type(device->type))
		return;
	if (device->wanted && (!device->record.reconnect_initiate || device->record.normally_connectable))
		hid_retry_later(device, now);
}

/*
 * Drops a device whose bond went (FORGET, or the device's own unplug): a
 * connected one hears VIRTUAL_CABLE_UNPLUG first when unplug is set
 * (design section 6.3), its link is ended, its input device goes and its
 * slot is freed (its Disconnection Complete then finds no device).
 */
static void
hid_drop(
	struct btd_hid *hid,
	struct btd_hid_device *device,
	int unplug)
{
	uint8_t disconnect[3];
	uint8_t message[1];

	/* The device hears the unplug on its control channel. */
	if (device->connected && unplug && device->control_cid != 0U) {
		message[0] = btd_hidp_header(BTD_HIDP_CONTROL, BTD_HIDP_VIRTUAL_CABLE_UNPLUG);
		hid_send(hid, device, device->control_cid, message, sizeof(message));
	}

	/* The link: the user (or the device) ended the bond. */
	if (device->connected) {
		hid_put16(disconnect, device->handle);
		disconnect[2] = HID_REASON_USER;
		(void)hid_command(hid, HID_DISCONNECT, disconnect, sizeof(disconnect));
	}

	/* A client waiting hears the end. */
	if (device->asked)
		hid_answer(hid, device, "ERROR forgotten");

	/* Succeeded: the input device goes, and the slot is free. */
	hid_close_bridge(device);
	memset(device, 0, sizeof(*device));
	device->bridge = -1;
}

/* Closes the input device (the kernel removes it and releases its keys). */
static void
hid_close_bridge(
	struct btd_hid_device *device)
{
	/* Nothing open. */
	if (device->bridge < 0)
		return;

	/* Succeeded: closed. */
	(void)close(device->bridge);
	device->bridge = -1;
	device->event = -1;
	device->touch_event = -1;
}

/*
 * Pages a device: Create Connection with the pairing's parameters (DM1 to
 * DH5, R1, no clock offset, the role switch allowed).
 */
static void
hid_page(
	struct btd_hid *hid,
	struct btd_hid_device *device)
{
	uint8_t parameters[13];
	uint64_t now;
	int error;

	/* The page's command. */
	memset(parameters, 0, sizeof(parameters));
	memcpy(parameters, device->address, BTD_ADDRESS_BYTES);
	hid_put16(parameters + 6, 0xcc18U);
	parameters[8] = 0x01U;
	parameters[9] = 0x00U;
	hid_put16(parameters + 10, 0x0000U);
	parameters[12] = 0x01U;
	now = btd_now_ms();
	device->inbound = 0;
	device->retry_at = 0U;
	device->state = BTD_HID_PAGING;
	device->state_deadline = now + BTD_HID_PAGE_MS;
	device->total_deadline = now + BTD_HID_TOTAL_MS;
	error = hid_command(hid, HID_CREATE_CONNECTION, parameters, sizeof(parameters));
	if (error == 0)
		return;

	/* Refused: tried again later. */
	hid_ended(hid, device, "unreachable");
}

/* Turns page scan on (once; the controller's refusal is tried again at the next refresh or handoff). */
static void
hid_page_scan(
	struct btd_hid *hid)
{
	uint8_t scan[1];
	int error;

	/* On already. */
	if (hid->page_scan)
		return;

	/* Write Scan Enable: page scan only (inquiry scan is the pairing's mode's, D11b). */
	scan[0] = HID_SCAN_PAGE;
	error = hid_command(hid, HID_WRITE_SCAN_ENABLE, scan, sizeof(scan));
	if (error != 0)
		return;

	/* Succeeded: on. */
	hid->page_scan = 1;
}

/*
 * Sets when a wanted device is paged again: 5, 10, 20, 40 then 60
 * seconds, and BTD_HID_RETRIES pages at most before it is paused (a
 * CONNECT, a new controller or the device's own connection starts them
 * again, review S5).
 */
static void
hid_retry_later(
	struct btd_hid_device *device,
	uint64_t now)
{
	uint64_t wait;
	unsigned index;

	/* Paused after so many. */
	device->retries++;
	if (device->retries > BTD_HID_RETRIES) {
		device->paused = 1;
		device->retry_at = 0U;
		return;
	}

	/* The wait doubles up to the longest. */
	wait = BTD_HID_RETRY_FIRST_MS;
	for (index = 1U; index < device->retries && wait < BTD_HID_RETRY_LAST_MS; index++)
		wait *= 2U;
	if (wait > BTD_HID_RETRY_LAST_MS)
		wait = BTD_HID_RETRY_LAST_MS;

	/* Succeeded: the next page's time. */
	device->retry_at = now + wait;
}

/* Gives a client waiting for a device's connection its answer. */
static void
hid_answer(
	struct btd_hid *hid,
	struct btd_hid_device *device,
	const char *line)
{
	/* No client waits any more. */
	device->asked = 0;

	/* Succeeded: told. */
	if (hid->hooks.answer != NULL)
		hid->hooks.answer(hid->hooks.context, device->address, line);
}

/* Writes the CONNECTED line of an open device. */
static void
hid_connected_line(
	const struct btd_hid *hid,
	const struct btd_hid_device *device,
	char *line,
	size_t size)
{
	char address[24];
	char name[BTD_NAME_MAX * 4U];
	char touch[40];
	int error;

	/* The address, the name (quoted safely), a touch device when there is one. */
	(void)hid;
	btd_format_address(device->address, address, sizeof(address));
	error = btd_escape(device->record.name, name, sizeof(name));
	if (error != 0)
		name[0] = '\0';
	touch[0] = '\0';
	if (device->touch_event >= 0)
		(void)snprintf(touch, sizeof(touch), " touch=/dev/input/event%d", (int)device->touch_event);

	/* Succeeded: the line. */
	(void)snprintf(line,
		       size,
		       "CONNECTED address=%s type=%s transport=%s input=/dev/input/event%d%s name=\"%s\" legacy=0 vendor=%04X product=%04X",
		       address,
		       btd_address_type_name(device->type),
		       hid_le_type(device->type) ? "hog" : "hid",
		       (int)device->event,
		       touch,
		       name,
		       device->record.vendor,
		       device->record.product);
}

/* Sends a command of the HID host; returns 0 or the command's error. */
static int
hid_command(
	struct btd_hid *hid,
	uint16_t opcode,
	const uint8_t *parameters,
	size_t count)
{
	int error;

	/* The command and its answer. */
	error = btd_session_command(hid->session, opcode, parameters, count);
	if (error != 0)
		return error;

	/* Succeeded: answered. */
	return 0;
}

/*
 * Sends an L2CAP frame on a device's connection: on the signalling
 * channel, or on one of the device's channels named by its local CID (the
 * frame goes to the device's end of it, its remote CID).  A failure is the
 * connection's end to show.
 */
static void
hid_send(
	struct btd_hid *hid,
	struct btd_hid_device *device,
	uint16_t cid,
	const uint8_t *payload,
	size_t length)
{
	struct btd_channel *channel;
	uint16_t destination;

	/* The signalling channel is fixed; a dynamic channel goes to the device's CID, and only once known. */
	destination = cid;
	if (cid != BTD_CID_SIGNALLING) {
		channel = btd_l2cap_channel(&device->l2cap, cid);
		if (channel == NULL || channel->remote_cid == 0U)
			return;
		destination = channel->remote_cid;
	}

	/* Succeeded: queued for the controller's buffers. */
	(void)btd_session_send(hid->session, device->handle, destination, payload, length);
}

/*
 * Answers a channel the device asks for (the L2CAP table's accept hook):
 * the control and interrupt channels are accepted on an encrypted link and
 * pending before (bluetoothd then authenticates the link, section 9.8);
 * any other PSM is not supported.
 */
static int
hid_accept(
	void *context,
	uint16_t handle,
	uint16_t psm,
	uint16_t *result,
	uint16_t *status)
{
	struct btd_hid_device *device;

	/* The device of the table. */
	(void)handle;
	device = context;
	*status = 0U;

	/* Only the HID channels. */
	if (psm != BTD_SDP_PSM_CONTROL && psm != BTD_SDP_PSM_INTERRUPT) {
		*result = BTD_L2CAP_PSM_NOT_SUPPORTED;
		return 0;
	}

	/* Pending until the link is encrypted with a key of 16 bytes. */
	if (!device->encrypted || device->key_size != HID_KEY_SIZE) {
		*result = BTD_L2CAP_PENDING;
		return 0;
	}

	/* Succeeded: accepted. */
	*result = BTD_L2CAP_SUCCESS;
	return 0;
}

/* Forgets a device's link: its frames, its channels, its query and its security. */
static void
hid_reset_link(
	struct btd_hid_device *device)
{
	/* Nothing of the link is left. */
	device->connected = 0;
	device->authenticating = 0;
	device->encrypted = 0;
	device->key_size = 0U;
	device->sdp_cid = 0U;
	device->control_cid = 0U;
	device->interrupt_cid = 0U;
	device->pnp_asked = 0;
	device->queued = 0U;
	device->setup_tries = 0U;
	device->le_encrypting = 0;
	device->echo_deadline = 0U;
	btd_hog_init(&device->hog);
	memset(&device->reassembly, 0, sizeof(device->reassembly));
	btd_l2cap_init(&device->l2cap);
	btd_l2cap_set_accept(&device->l2cap, hid_accept, device);
}

/* Copies a name for the kernel: at most HID_NAME_BYTES bytes, not cut inside a UTF-8 character. */
static void
hid_name(
	const char *from,
	char *to,
	size_t size)
{
	size_t length;
	size_t whole;

	/* As much as fits. */
	whole = strlen(from);
	length = whole;
	if (length > HID_NAME_BYTES)
		length = HID_NAME_BYTES;
	if (length > size - 1U)
		length = size - 1U;

	/* A cut inside a character goes back to its first byte. */
	if (length < whole) {
		while (length > 0U && ((unsigned char)from[length] & 0xc0U) == 0x80U)
			length--;
	}

	/* Succeeded: the name. */
	memcpy(to, from, length);
	to[length] = '\0';
}

/* Takes LE's meta events of the HID host's connections: its connections made. */
static void
hid_le_meta(
	struct btd_hid *hid,
	const uint8_t *parameters,
	size_t length)
{
	/* A subevent at all. */
	if (length < 1U)
		return;

	/* Connection Complete, or Enhanced Connection Complete; any other is passed over. */
	if (parameters[0] == HID_LE_CONNECTED || parameters[0] == HID_LE_ENHANCED)
		hid_le_connected(hid, parameters, length);
}

/*
 * Takes LE's (Enhanced) Connection Complete (subevent, status, handle,
 * role, the device's address type and address, ...) of a device connected
 * directly or by the auto-connect: the link is encrypted with the bond's
 * LTK next.  A failed direct connection is tried again only by the user.
 */
static void
hid_le_connected(
	struct btd_hid *hid,
	const uint8_t *parameters,
	size_t length)
{
	struct btd_hid_device *device;
	size_t least;
	uint64_t now;

	/* A whole event of its subevent. */
	least = 19U;
	if (parameters[0] == HID_LE_ENHANCED)
		least = 31U;
	if (length < least)
		return;

	/* The device, by its address or its resolvable private address. */
	device = hid_find(hid, parameters + 6);
	if (device == NULL)
		device = hid_le_resolve(hid, parameters + 6);

	/* A failed one: a direct connection ends (a cancelled auto-connect carries no device). */
	if (parameters[1] != 0U) {
		if (device != NULL && hid_le_type(device->type) && device->state == BTD_HID_PAGING)
			hid_ended(hid, device, "unreachable");
		return;
	}

	/* Only an LE device of the table, connected directly or waited for. */
	if (device == NULL || !hid_le_type(device->type))
		return;
	if (device->state != BTD_HID_PAGING && device->state != BTD_HID_IDLE)
		return;

	/* The controller left its initiating state; a device waited for came by the auto-connect. */
	hid->le_armed = 0;
	now = btd_now_ms();
	if (device->state == BTD_HID_IDLE) {
		device->inbound = 1;
		device->total_deadline = now + BTD_HID_TOTAL_MS;
	}

	/* Succeeded: connected, the encryption next. */
	hid_reset_link(device);
	device->connected = 1;
	device->handle = hid_handle_at(parameters + 2);
	device->state = BTD_HID_ENCRYPTING;
	device->state_deadline = now + BTD_HID_SECURITY_MS;
	hid_le_encrypt(hid, device);
}

/*
 * Finds the LE device of the table whose IRK resolves a resolvable
 * private address (its top two bits 01), or NULL.
 */
static struct btd_hid_device *
hid_le_resolve(
	struct btd_hid *hid,
	const uint8_t *address)
{
	struct btd_hid_device *device;
	struct btd_bond bond;
	uint8_t irk[16];
	uint8_t prand[3];
	uint8_t hash[3];
	uint8_t expected[3];
	unsigned index;
	unsigned byte;
	int same;
	int error;

	/* Only a resolvable private address. */
	if ((address[5] & 0xc0U) != 0x40U)
		return NULL;

	/* prand (the top three bytes) and the hash (the bottom three), most significant first. */
	prand[0] = address[5];
	prand[1] = address[4];
	prand[2] = address[3];
	expected[0] = address[2];
	expected[1] = address[1];
	expected[2] = address[0];

	/* Each LE device's bond with an IRK (stored least significant first). */
	for (index = 0U; index < BTD_HID_MAX; index++) {
		device = &hid->devices[index];
		if (!device->used || !hid_le_type(device->type))
			continue;
		error = btd_keys_read(hid->keys_folder, hid->session->address, device->address, device->type, &bond);
		if (error != 0 || !bond.have_irk)
			continue;
		for (byte = 0U; byte < sizeof(irk); byte++)
			irk[byte] = bond.irk[sizeof(irk) - 1U - byte];
		memset(&bond, 0, sizeof(bond));
		btd_smp_ah(irk, prand, hash);
		memset(irk, 0, sizeof(irk));
		same = memcmp(hash, expected, sizeof(hash));
		if (same == 0)
			return device;
	}

	/* No device's. */
	return NULL;
}

/* Starts LE's encryption with the bond's key (LE Enable Encryption: handle, Rand, EDIV, the LTK); Encryption Change answers it. */
static void
hid_le_encrypt(
	struct btd_hid *hid,
	struct btd_hid_device *device)
{
	struct btd_bond bond;
	uint8_t encrypt[28];
	int error;

	/* The bond's key. */
	error = btd_keys_read(hid->keys_folder, hid->session->address, device->address, device->type, &bond);
	if (error != 0 || !bond.have_ltk) {
		memset(&bond, 0, sizeof(bond));
		hid_fail(hid, device, "key-missing");
		return;
	}

	/* The command; the keys are not kept. */
	hid_put16(encrypt, device->handle);
	memcpy(encrypt + 2, bond.rand, 8U);
	hid_put16(encrypt + 10, bond.ediv);
	memcpy(encrypt + 12, bond.ltk, 16U);
	memset(&bond, 0, sizeof(bond));
	device->le_encrypting = 1;
	error = hid_command(hid, HID_LE_ENCRYPT, encrypt, sizeof(encrypt));
	memset(encrypt, 0, sizeof(encrypt));
	if (error != 0) {
		hid_fail(hid, device, "security");
		return;
	}
}

/*
 * Takes a frame of an LE device's fixed channels: ATT (the device's
 * requests answered by the smallest server, the rest to the discovery),
 * LE signalling (a connection parameter update carried out), and the
 * Security Manager (a pairing refused, review B6; a Security Request
 * answered by the encryption).
 */
static void
hid_le_frame(
	struct btd_hid *hid,
	struct btd_hid_device *device,
	uint16_t cid,
	const uint8_t *payload,
	size_t length)
{
	struct btd_signal_effect effect;
	uint8_t answer[BTD_ATT_MTU];
	uint8_t update[14];
	size_t answer_length;
	unsigned actions;
	int error;

	/* ATT: a request of the device (an even opcode) to bluetoothd's server, the rest to the discovery. */
	if (cid == HID_CID_ATT && length != 0U) {
		if ((payload[0] & 1U) == 0U) {
			error = btd_att_answer(payload, length, answer, sizeof(answer), &answer_length);
			if (error == 0 && answer_length != 0U)
				hid_send_fixed(hid, device, HID_CID_ATT, answer, answer_length);
			return;
		}

		/* A response, a notification, an indication. */
		actions = btd_hog_input(&device->hog, payload, length, btd_now_ms());
		hid_gatt(hid, device, actions);
		return;
	}

	/* LE signalling: answered, and a parameter update carried out (as the pairing does). */
	if (cid == HID_CID_LE_SIGNALLING) {
		(void)btd_l2cap_signal(&device->l2cap, device->handle, 1, payload, length, answer, sizeof(answer), &answer_length, &effect);
		if (answer_length != 0U)
			hid_send_fixed(hid, device, HID_CID_LE_SIGNALLING, answer, answer_length);
		if (!effect.update)
			return;
		hid_put16(update, device->handle);
		hid_put16(update + 2, effect.interval_min);
		hid_put16(update + 4, effect.interval_max);
		hid_put16(update + 6, effect.latency);
		hid_put16(update + 8, effect.timeout);
		hid_put16(update + 10, 0x0000U);
		hid_put16(update + 12, 0x0000U);
		(void)hid_command(hid, HID_LE_UPDATE, update, sizeof(update));
		return;
	}

	/* Anything but the Security Manager is passed over. */
	if (cid != HID_CID_SMP || length == 0U)
		return;

	/* A pairing outside the pairing's mode is refused (design section 6.5). */
	if (payload[0] == HID_SMP_PAIRING_REQUEST) {
		answer[0] = HID_SMP_PAIRING_FAILED;
		answer[1] = HID_SMP_NOT_SUPPORTED;
		hid_send_fixed(hid, device, HID_CID_SMP, answer, 2U);
		return;
	}

	/* Succeeded: a Security Request is answered by the bond's encryption, once. */
	if (payload[0] == HID_SMP_SECURITY_REQUEST && !device->encrypted && !device->le_encrypting && device->connected)
		hid_le_encrypt(hid, device);
}

/*
 * Connects an LE device directly (CONNECT): the auto-connect is cancelled
 * first, then LE Create Connection with the pairing's parameters.
 */
static void
hid_le_page(
	struct btd_hid *hid,
	struct btd_hid_device *device)
{
	uint8_t parameters[25];
	uint64_t now;
	int error;

	/* The controller initiates one connection at a time. */
	hid_le_disarm(hid);

	/*
	 * LE Create Connection: scan interval 60 ms and window 30 ms, no
	 * filter, the device, bluetoothd's public address, a connection
	 * interval of 30 to 50 ms, no latency, a supervision timeout of 5 s.
	 */
	memset(parameters, 0, sizeof(parameters));
	hid_put16(parameters + 0, 0x0060U);
	hid_put16(parameters + 2, 0x0030U);
	parameters[4] = 0x00U;
	parameters[5] = 0x00U;
	if (device->type == BTD_ADDRESS_LE_RANDOM)
		parameters[5] = 0x01U;
	memcpy(parameters + 6, device->address, BTD_ADDRESS_BYTES);
	parameters[12] = 0x00U;
	hid_put16(parameters + 13, 0x0018U);
	hid_put16(parameters + 15, 0x0028U);
	hid_put16(parameters + 17, 0x0000U);
	hid_put16(parameters + 19, 0x01f4U);

	/* The connection under way. */
	now = btd_now_ms();
	device->inbound = 0;
	device->state = BTD_HID_PAGING;
	device->state_deadline = now + BTD_HID_PAGE_MS;
	device->total_deadline = now + BTD_HID_TOTAL_MS;
	error = hid_command(hid, HID_LE_CREATE, parameters, sizeof(parameters));
	if (error == 0)
		return;

	/* Refused. */
	hid_ended(hid, device, "unreachable");
}

/*
 * Sets LE's auto-connect (section 4.9): the filter accept list holds the
 * LE devices waited for (their IRKs in the resolving list), and LE Create
 * Connection from that list waits with a low duty (review S4).  A refusal
 * is tried again later.
 */
static void
hid_le_arm(
	struct btd_hid *hid)
{
	static struct btd_bond bond;
	struct btd_hid_device *device;
	uint8_t parameters[39];
	unsigned index;
	unsigned resolving;
	int error;

	/* A list of its own, and the resolving list emptied while resolution is off. */
	hid->le_retry_at = btd_now_ms() + BTD_HID_RETRY_FIRST_MS;
	error = hid_command(hid, HID_LE_CLEAR_LIST, NULL, 0U);
	if (error != 0)
		return;
	parameters[0] = 0x00U;
	(void)hid_command(hid, HID_LE_RESOLUTION, parameters, 1U);
	(void)hid_command(hid, HID_LE_CLEAR_RESOLVING, NULL, 0U);

	/* Each LE device waited for: its identity in the list, its IRK in the resolving list. */
	resolving = 0U;
	for (index = 0U; index < BTD_HID_MAX; index++) {
		device = &hid->devices[index];
		if (!device->used || !hid_le_type(device->type) || !device->wanted || device->state != BTD_HID_IDLE)
			continue;
		parameters[0] = 0x00U;
		if (device->type == BTD_ADDRESS_LE_RANDOM)
			parameters[0] = 0x01U;
		memcpy(parameters + 1, device->address, BTD_ADDRESS_BYTES);
		error = hid_command(hid, HID_LE_ADD_LIST, parameters, 7U);
		if (error != 0)
			continue;

		/* Its IRK, when the bond has one (the local IRK is none, 0). */
		error = btd_keys_read(hid->keys_folder, hid->session->address, device->address, device->type, &bond);
		if (error != 0 || !bond.have_irk) {
			memset(&bond, 0, sizeof(bond));
			continue;
		}
		memset(parameters, 0, sizeof(parameters));
		parameters[0] = 0x00U;
		if (device->type == BTD_ADDRESS_LE_RANDOM)
			parameters[0] = 0x01U;
		memcpy(parameters + 1, device->address, BTD_ADDRESS_BYTES);
		memcpy(parameters + 7, bond.irk, 16U);
		memset(&bond, 0, sizeof(bond));
		error = hid_command(hid, HID_LE_ADD_RESOLVING, parameters, sizeof(parameters));
		memset(parameters, 0, sizeof(parameters));
		if (error == 0)
			resolving++;
	}

	/* Resolution on when an IRK went in (a controller without it resolves nothing: such a bond's device comes only by CONNECT). */
	if (resolving != 0U) {
		parameters[0] = 0x01U;
		(void)hid_command(hid, HID_LE_RESOLUTION, parameters, 1U);
	}

	/* LE Create Connection from the list: 1.28 s and 11.25 ms, the pairing's connection parameters. */
	memset(parameters, 0, sizeof(parameters));
	hid_put16(parameters + 0, BTD_HID_LE_INTERVAL);
	hid_put16(parameters + 2, BTD_HID_LE_WINDOW);
	parameters[4] = 0x01U;
	parameters[12] = 0x00U;
	hid_put16(parameters + 13, 0x0018U);
	hid_put16(parameters + 15, 0x0028U);
	hid_put16(parameters + 17, 0x0000U);
	hid_put16(parameters + 19, 0x01f4U);
	error = hid_command(hid, HID_LE_CREATE, parameters, 25U);
	if (error != 0)
		return;

	/* Succeeded: waiting for the devices. */
	hid->le_armed = 1;
	hid->le_arms++;
	hid->le_retry_at = 0U;
}

/* Cancels LE's auto-connect (before a pairing, a scan, a direct connection or a change of the list); the tick sets it again. */
static void
hid_le_disarm(
	struct btd_hid *hid)
{
	/* Nothing set. */
	hid->le_retry_at = 0U;
	if (!hid->le_armed)
		return;

	/* Succeeded: LE Create Connection Cancel (a refusal means it ended already). */
	(void)hid_command(hid, HID_LE_CANCEL, NULL, 0U);
	hid->le_armed = 0;
}

/* Tells whether an LE device waits for the auto-connect (wanted, not connected) and its time came. */
static int
hid_le_waiting(
	const struct btd_hid *hid)
{
	const struct btd_hid_device *device;
	unsigned index;

	/* After a refusal, a while. */
	if (hid->le_retry_at != 0U && btd_now_ms() < hid->le_retry_at)
		return 0;

	/* Each LE device. */
	for (index = 0U; index < BTD_HID_MAX; index++) {
		device = &hid->devices[index];
		if (!device->used || !hid_le_type(device->type) || !device->wanted || device->state != BTD_HID_IDLE)
			continue;

		/* One waits, unless a direct connection is under way. */
		if (hid_paging(hid))
			return 0;
		return 1;
	}

	/* None waits. */
	return 0;
}

/* Starts LE's discovery on the encrypted link (notifications that came already wait in it). */
static void
hid_gatt_start(
	struct btd_hid *hid,
	struct btd_hid_device *device)
{
	unsigned actions;

	/* Its own times (ATT's transaction, the whole discovery) rule now. */
	device->state = BTD_HID_GATT;
	device->state_deadline = 0U;

	/* Succeeded: the first request. */
	actions = btd_hog_start(&device->hog, btd_now_ms());
	hid_gatt(hid, device, actions);
}

/*
 * Carries out what the discovery asks: a PDU sent, the input device made
 * (the map as its descriptor, the PnP numbers), open, a report passed on,
 * or the end with why.
 */
static void
hid_gatt(
	struct btd_hid *hid,
	struct btd_hid_device *device,
	unsigned actions)
{
	/* A PDU to send first (the report protocol's command comes with the setup). */
	if ((actions & BTD_HOG_SEND) != 0U)
		hid_send_fixed(hid, device, HID_CID_ATT, device->hog.out, device->hog.out_length);

	/* The end with why (a changed service too: the device is waited for again). */
	if ((actions & BTD_HOG_FAILED) != 0U) {
		hid_fail(hid, device, device->hog.why);
		return;
	}

	/* The input device, from the map. */
	if ((actions & BTD_HOG_SETUP) != 0U) {
		memcpy(device->record.descriptor, device->hog.map, device->hog.map_size);
		device->record.descriptor_size = device->hog.map_size;
		device->record.vendor = device->hog.vendor;
		device->record.product = device->hog.product;
		device->record.version = device->hog.version;
		device->have_descriptor = 1;
		hid_setup(hid, device);
		return;
	}

	/* Open. */
	if ((actions & BTD_HOG_OPEN) != 0U) {
		hid_open(hid, device);
		return;
	}

	/* Succeeded: a report passed on. */
	if ((actions & BTD_HOG_REPORT) != 0U)
		hid_report(device, device->hog.report, device->hog.report_length);
}

/* Cancels a page that did not end in time: Create Connection Cancel (BR/EDR), LE Create Connection Cancel (LE). */
static void
hid_cancel_page(
	struct btd_hid *hid,
	struct btd_hid_device *device)
{
	/* LE's. */
	if (hid_le_type(device->type)) {
		(void)hid_command(hid, HID_LE_CANCEL, NULL, 0U);
		return;
	}

	/* Succeeded: BR/EDR's, by the device's address. */
	(void)hid_command(hid, HID_CREATE_CANCEL, device->address, BTD_ADDRESS_BYTES);
}

/* Tells whether an address type is LE's. */
static int
hid_le_type(
	unsigned type)
{
	/* LE's public or random. */
	if (type == BTD_ADDRESS_LE_PUBLIC || type == BTD_ADDRESS_LE_RANDOM)
		return 1;

	/* BR/EDR's. */
	return 0;
}

/* Sends a frame on one of LE's fixed channels (a failure is the connection's end to show). */
static void
hid_send_fixed(
	struct btd_hid *hid,
	struct btd_hid_device *device,
	uint16_t cid,
	const uint8_t *payload,
	size_t length)
{
	/* Succeeded: queued for the controller's LE buffers. */
	(void)btd_session_send(hid->session, device->handle, cid, payload, length);
}

/* Writes a 16-bit value least significant byte first. */
static void
hid_put16(
	uint8_t *bytes,
	uint16_t value)
{
	/* The two bytes. */
	bytes[0] = (uint8_t)(value & 0xffU);
	bytes[1] = (uint8_t)(value >> 8);
}

/* Reads a connection handle (two bytes, least significant first, without the flags above its 12 bits). */
static uint16_t
hid_handle_at(
	const uint8_t *bytes)
{
	unsigned value;

	/* The two bytes, then the handle's bits alone. */
	value = (unsigned)bytes[0] | ((unsigned)bytes[1] << 8);
	value &= 0x0fffU;

	/* Succeeded: the handle. */
	return (uint16_t)value;
}

/* Tells whether a page is under way (the controller pages one device at a time). */
static int
hid_paging(
	const struct btd_hid *hid)
{
	unsigned index;

	/* Each device paging. */
	for (index = 0U; index < BTD_HID_MAX; index++) {
		if (hid->devices[index].used && hid->devices[index].state == BTD_HID_PAGING && !hid->devices[index].inbound)
			return 1;
	}

	/* None. */
	return 0;
}

/* Names a device's state for STATUS: idle, connecting, open, or waiting (to be paged again). */
static const char *
hid_state_word(
	const struct btd_hid_device *device)
{
	/* Open. */
	if (device->state == BTD_HID_OPEN)
		return "open";

	/* A connection under way. */
	if (device->state != BTD_HID_IDLE)
		return "connecting";

	/* Wanted back. */
	if (device->wanted && !device->paused)
		return "waiting";

	/* Succeeded: nothing under way. */
	return "idle";
}
