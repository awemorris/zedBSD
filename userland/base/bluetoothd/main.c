/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Bluetooth daemon (ws143-p003 and ws143-p004, plan/ws143/phase003/
 * phase.md and plan/ws143/phase004/phase.md).
 *
 * It listens on /run/bluetoothd.sock as root, then separates (privsep.c):
 * a parent stays root to open the controller's node, and the child runs
 * the rest as the account _bluetooth.  The child opens the first
 * Bluetooth controller's node through the parent, starts it (Intel's
 * firmware when it needs it, then the HCI core), and answers on the
 * socket: the state and the controller, a scan, a pairing and its agent,
 * the bonds and their removal.  A node that goes (the controller was
 * pulled out, or re-enumerated after its firmware's boot) is closed, and
 * the nodes are looked for again every BTD_RETRY_MS; so is a controller
 * whose start failed in a way a new start may mend, BTD_FAILURES_MAX times
 * in a row at most.
 *
 * Intel's firmware is loaded once for a controller (by its USB vendor,
 * product and port): one that comes back in its bootloader after a load
 * is not loaded again until the daemon starts again, and one that does
 * not come back within BTD_REAPPEAR_MS of its boot is a failed load.
 *
 * Who may change things (D8, plan section 6): root, the seat's user (the
 * display's owner, not the greeter), and the members of wheel.  Anyone
 * may read the state, the devices and the bonds.
 *
 *   bluetoothd [-f /dev/bluetoothN]   (a node of its own; else the lowest that opens)
 */

#include "userland/base/bluetoothd/hid.h"
#include "userland/base/bluetoothd/keys.h"
#include "userland/base/bluetoothd/pair.h"
#include "userland/base/bluetoothd/phone.h"
#include "userland/base/bluetoothd/phonemux.h"
#include "userland/base/bluetoothd/pbap.h"
#include "userland/base/bluetoothd/privsep.h"
#include "userland/base/bluetoothd/protocol.h"
#include "userland/base/bluetoothd/linkmgr.h"
#include "userland/base/bluetoothd/map.h"
#include "userland/base/bluetoothd/outq.h"
#include "userland/base/bluetoothd/phoneio.h"
#include "userland/base/bluetoothd/router.h"
#include "userland/base/bluetoothd/session.h"
#include "userland/base/bluetoothd/snoop.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <uapi/system.h>
#include <unistd.h>

/* How often the nodes are looked for while there is no controller. */
#define BTD_RETRY_MS		2000U

/* How many starts in a row may fail before the daemon stops trying, and how long a booted controller may take to come back. */
#define BTD_FAILURES_MAX	3U
#define BTD_REAPPEAR_MS		10000U

/* How many controllers' loads are remembered, and the length of a controller's key. */
#define BTD_LOADS_MAX		8U
#define BTD_KEY_MAX		96U

/*
 * How many clients may be connected, and how many of the last free slots
 * are kept for root and the seat's user (the compositor's connections,
 * ws197-p003 section 4.2).
 */
#define BTD_CLIENTS_MAX		16U
#define BTD_CLIENTS_RESERVED	4U

/* How many connections a uid that is neither root nor the seat's user may hold (ws197-p004 section 5.4). */
#define BTD_CLIENTS_PER_UID	4U

/*
 * The phone's subscribers (ws197-p003 section 9.4, p004 section 5): how
 * many connections at once, the queue past which events are dropped (and
 * DROPPED told), and the queue under which DROPPED is told; the limit of a
 * PAGE when the request names none (Q4).
 */
#define BTD_SUBSCRIBERS_MAX	2U
#define BTD_SUBSCRIBE_DROP	196608U
#define BTD_SUBSCRIBE_RESUME	65536U
#define BTD_PAGE_LIMIT		500U

/* How many packets one round of the loop handles at most (the clients are not starved). */
#define BTD_PACKETS_A_ROUND	64U

/* Marks a parameter a function takes for its signature's sake. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* How many bonds BONDS lists. */
#define BTD_BONDS_MAX		64U

/* The display whose owner is the seat's user, the greeter's account, and the group whose members may change things. */
#define BTD_SEAT_NODE		"/dev/gpu0"

/* The file that keeps the user's switch (design D11a: the state before is kept, on the first time), in the keys' folder. */
#define BTD_POWER_FILE		BTD_KEYS_FOLDER "/power"
#define BTD_GREETER		"_greeter"

/* How often the seat's user is looked at again for the phone link (ws197-p003 section 3.3). */
#define BTD_SEAT_CHECK_MS	5000U

/* How many bonds of this run the daemon remembers the maker of (BUG-287). */
#define BTD_OWN_BONDS		8U
#define BTD_ADMIN_GROUP		"wheel"
#define BTD_GROUPS_MAX		64

/* No client (an index of btd_clients). */
#define BTD_NO_CLIENT		(-1)

/*
 * One client of the socket: its descriptor, its uid, its generation (a
 * number no client before it in the slot had, so an answer meant for a
 * client that went never reaches the next one, ws197-p003 section 4.2),
 * whether it stopped reading (it is closed at the end of the loop's
 * round, where nothing else uses it), what is written to it and not sent
 * yet, what it writes (a line not ended yet, or a PHONE SEND's text being
 * read), what it waits for (a scan's end, a pairing's end, a phone
 * request's DONE), and whether it is a phone's subscriber (it reads only
 * events, ws197-p003 section 9.4) and dropped events it must be told of.
 */
struct btd_client {
	int descriptor;
	uid_t uid;
	uint32_t generation;
	int dead;
	struct btd_outq output;
	struct btd_phoneio_input input;
	int waits_scan;
	int waits_pair;
	int waits_connect;
	int waits_phone;
	int subscribed;
	int sub_dropped;
	uint8_t connect_address[BTD_ADDRESS_BYTES];
};

/* A BR/EDR bond made in this run with an authenticated key: the slot in use, its address, the uid whose PAIR made it. */
struct btd_own_bond {
	int used;
	uint8_t address[BTD_ADDRESS_BYTES];
	uid_t uid;
};

static void btd_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
static int btd_node_control(void *context, unsigned long request, void *argument);
static void btd_random(void *context, uint8_t *bytes, size_t length);
static void btd_key(const struct bt_info *info, char *key, size_t size);
static int btd_loaded(const char *key);
static void btd_remember_load(const char *key);
static int btd_listen(void);
static void btd_open(void);
static void btd_close(void);
static void btd_packets(void);
static int btd_timeout(void);
static void btd_accept(int listener);
static void btd_read(int index);
static void btd_line(int index, char *line);
static void btd_show(struct btd_client *client);
static void btd_devices(struct btd_client *client);
static void btd_scan(struct btd_client *client, const char *argument);
static void btd_scan_end(void);
static void btd_pair(int index, const char *argument);
static void btd_agent(int index);
static void btd_answer(int index, int accepted);
static void btd_forget(struct btd_client *client, const char *argument);
static void btd_bonds(struct btd_client *client);
static void btd_power(struct btd_client *client, const char *argument);
static void btd_power_load(void);
static int btd_power_save(void);
static void btd_question_end(void);
static void btd_ask(void *context, unsigned kind, uint32_t number);
static void btd_paired(void *context, const char *answer);
static int btd_permitted(uid_t uid);
static int btd_seated(uid_t uid);
static int btd_account(void *context, uid_t uid, char *name, size_t size);
static void btd_phone_show_request(struct btd_client *client);
static void btd_phone_link_request(struct btd_client *client, const char *argument);
static void btd_connect(int index, const char *argument);
static void btd_disconnect(struct btd_client *client, const char *argument);
static void btd_status(struct btd_client *client);
static void btd_phone_request(int index, const char *argument);
static int btd_hid_bridge(void *context, int *descriptor);
static void btd_hid_told(void *context, const uint8_t *address, const char *line);
static void btd_hid_holding(void);
static void btd_trace(void *context, const uint8_t *packet, size_t length, int received);
static int btd_arguments(int argc, char **argv);
static void btd_system_open(void);
static void btd_system_events(void);
static void btd_seat_check(uint64_t now);
static void btd_own_bond_keep(const uint8_t *address, uid_t uid);
static int btd_own_bond_find(const uint8_t *address, uid_t uid);
static void btd_own_bond_forget(const uint8_t *address);
static int btd_parse_device(const char *text, uint8_t *address, unsigned *type);
static int btd_parse_pair(const char *text, uint8_t *address, unsigned *type, int *phone);
static void btd_write(struct btd_client *client, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void btd_client_close(int index);
static void btd_client_flush(int index);
static ssize_t btd_client_send(void *context, const uint8_t *data, size_t length);
static int btd_client_line(void *context, char *line);
static void btd_client_text(void *context, const char *line, const uint8_t *text, size_t length);
static void btd_phone_send_request(int index, const char *line, const uint8_t *text, size_t length);
static uint64_t btd_token(int index);
static struct btd_client *btd_token_client(uint64_t token);
static int btd_phone_allowed(uid_t uid);
static void btd_phone_owner(struct btd_phoneio_owner *owner);
static void btd_client_raw(struct btd_client *client, const char *line, const uint8_t *bytes, size_t length);
static void btd_subscriber_write(struct btd_client *client, const char *line, const uint8_t *bytes, size_t length);
static int btd_phone_line(uid_t uid, int permitted, const char *prefix, char *line, size_t size);
static void btd_phone_watch(void);
static void btd_phone_subscribe_request(int index);
static void btd_phone_page_request(int index, const char *argument);
static void btd_phone_read_request(int index, const char *argument);
static int btd_decimal(const char *text, unsigned long long most, unsigned long long *value);
static uint64_t btd_messages_clock(void *context);
static int64_t btd_messages_wall(void *context);
static int32_t btd_messages_offset(void *context, int64_t seconds);
static int btd_messages_wanted(void *context);
static int btd_messages_sdp(void *context, uint16_t uuid);
static int btd_messages_open(void *context, unsigned server_channel);
static int btd_profiles_sdp(void *context, uint16_t uuid);
static int btd_contacts_wanted(void *context);
static int btd_contacts_address(void *context, uint8_t *address);
static int btd_contacts_sdp(void *context, uint16_t uuid);
static int btd_contacts_open(void *context, unsigned server_channel);
static int btd_profiles_open(void *context, unsigned server_channel);
static void btd_profiles_close(void *context, unsigned dlci);
static int btd_messages_write(void *context, unsigned dlci, const uint8_t *data, size_t length, size_t *written);
static void btd_messages_close(void *context, unsigned dlci);
static void btd_messages_answer(void *context, uint64_t token, const char *line, const uint8_t *bytes, size_t length);
static void btd_messages_emit(void *context, const char *line, const uint8_t *bytes, size_t length);
static long btd_messages_room(void *context, uint64_t token);
static void btd_messages_up(void *context);
static void btd_messages_log(void *context, const char *line);

/* The clients; a free slot has descriptor -1.  The daemon's one thread uses them. */
static struct btd_client btd_clients[BTD_CLIENTS_MAX];

/* The generation the next client gets; it only grows (wrapping after four billion clients). */
static uint32_t btd_generation;

/*
 * The controller's session, open while session_open is nonzero, and when
 * the nodes were last looked for (0: never).
 */
static struct btd_session btd_session;
static int btd_session_open;
static uint64_t btd_looked_ms;

/*
 * The pairing of the controller, the session's handler while the node is
 * open.  It lives as long as the daemon; a closed node ends a pairing.
 */
static struct btd_pair btd_pairing;

/*
 * The router of the session's packets (ws143-p005): the session's handler
 * while the node is open, giving the pairing its device's packets and
 * refusing what nobody owns.  It lives as long as the daemon; a closed
 * node empties its routes.
 */
static struct btd_router btd_routing;

/*
 * The HID host (ws143-p005 i02c): the router's owner of the HID devices'
 * connections.  It lives as long as the daemon; a closed node ends its
 * connections (the devices stay wanted).
 */
static struct btd_hid btd_hid_host;

/*
 * The link manager (ws197-p002 section 6): the one that writes the page
 * scan the HID host wants, and the controller's one BR/EDR page that the
 * HID host and the pairing take in turn.  It lives as long as the daemon;
 * a closed node makes it forget what the controller was told and any page
 * out.
 */
static struct btd_linkmgr btd_links;

/*
 * The phone link (ws197-p002 section 11): the router's owner of a phone's
 * link that a phone's pairing handed over, and the SDP records it offers
 * (none in this Phase: the profiles register theirs).  They live as long as
 * the daemon; a closed node ends the phone's link.
 */
static struct btd_phone btd_phone_link;
static struct btd_sdps_db btd_records;

/*
 * The MAP client on the phone link (ws197-p003 section 8): started when
 * the link is ready for its owner, its answers and events going to the
 * clients through main's hooks.  It lives as long as the daemon.
 */
static struct btd_map btd_messages;

/*
 * The profiles' share of the phone link (ws197-p005 section 3.1): the
 * phone link's one profile, handing each child (MAP, later PBAP and HFP)
 * its SDP answers and DLCs; and MAP's index among the children.  Both
 * live as long as the daemon and are set up before the controller opens.
 */
static struct btd_phonemux btd_profiles;
static unsigned btd_messages_child;

/*
 * The PBAP client on the phone link (ws197-p005 section 5): started when
 * the link is ready and the owner wants contacts, its pages answered to
 * the clients through main's hooks (MAP's, which do not depend on the
 * profile); and its index among the mux's children.  They live as long as
 * the daemon.
 */
static struct btd_pbap btd_contacts;
static unsigned btd_contacts_child;

/*
 * What the phone's subscribers were told last (the line of PHONE STATE,
 * an empty one before the first) and the phone's owner then (none: no
 * valid record).  A change of either is acted on at the end of the loop's
 * round.
 */
static char btd_phone_told[BTD_PHONEIO_OUT_MAX];
static struct btd_phoneio_owner btd_owner_known;


/*
 * The clients of a pairing: the one that asked for it, the agent that
 * named itself (it answers for pairings of its uid, or of anyone when it
 * is the seat's user), and the one asked the question now
 * (BTD_NO_CLIENT: none).
 */
static int btd_pair_client = BTD_NO_CLIENT;
static int btd_agent_client = BTD_NO_CLIENT;
static int btd_asked_client = BTD_NO_CLIENT;

/* The child's ends of the privilege separation. */
static struct btd_privsep btd_separation;

/* The node given with -f, or NULL for the lowest that opens. */
static const char *btd_node;

/* The child's /dev/system, subscribed to the power events (-1: none). */
static int btd_system = -1;

/* The btsnoop record given with -s (ws143-p005 section 9.11): its path (NULL: none) and the child's open file (-1). */
static const char *btd_snoop_path;
static int btd_snoop = -1;

/*
 * A packet of a phone's link with its data hidden, as the btsnoop record
 * gets it (ws197-p002 section 11).  The trace fills and writes it in one
 * call on the daemon's one thread.
 */
static uint8_t btd_trace_copy[BT_ACL_PACKET_MAX];

/*
 * How many starts in a row failed (0 again after a ready start), and
 * whether the daemon stopped trying; whether the last open's busy node was
 * logged already.
 */
static unsigned btd_failures;
static int btd_stopped;
static int btd_busy_logged;

/*
 * The controllers whose firmware was loaded (their keys, see btd_key), and
 * when the last booted one is due back (0: none awaited).  Kept for the
 * daemon's life.
 */
static char btd_loads[BTD_LOADS_MAX][BTD_KEY_MAX];
static unsigned btd_load_count;
static uint64_t btd_reappear_ms;

/*
 * When the seat's user is next looked at for the phone link (ws197-p003
 * section 3.3; 0: at once).  Moved on by each look; kept for the
 * daemon's life.
 */
static uint64_t btd_seat_check_at;

/*
 * The BR/EDR bonds made with an authenticated key in this run, and the uid
 * whose PAIR made each (BUG-287): a phone's pairing of such a bond by the
 * same uid uses its stored key instead of asking the phone to pair again.
 * Forgotten with the bond; the oldest makes room.  Kept for the daemon's
 * life only.
 */
static struct btd_own_bond btd_own_bonds[BTD_OWN_BONDS];
static unsigned btd_own_next;

/*
 * Whether the user turned Bluetooth off (POWER off, ws143-p006): the
 * daemon then starts no scan and no pairing (they are answered
 * "ERROR off"), and SHOW says "STATE off" over a ready controller and
 * "POWER off".  The saved keys stay.  It is kept in BTD_POWER_FILE across
 * starts (design D11a, the user's decision); without the file it is on.
 */
static int btd_powered_off;

/*
 * The client last asked a question of a pairing (CONFIRM, CONSENT or the
 * PASSKEY shown; BTD_NO_CLIENT for none): it hears ASK-END when the
 * question is over (answered, or the pairing ended).
 */
static int btd_question_client = BTD_NO_CLIENT;

/*
 * Runs the daemon until it is killed.
 */
int
main(
	int argc,
	char **argv)
{
	struct pollfd descriptors[4U + BTD_CLIENTS_MAX];
	struct btd_hid_hooks hid_hooks;
	struct btd_router_hid router_hid;
	struct btd_router_phone router_phone;
	struct btd_phone_hooks phone_hooks;
	struct btd_phone_profile phone_profile;
	struct btd_phonemux_hooks mux_hooks;
	struct btd_map_hooks map_hooks;
	struct btd_pbap_hooks pbap_hooks;
	uint32_t first_session;
	uint32_t first_generation;
	char expired_text[24];
	size_t pending;
	unsigned count;
	unsigned index;
	uint64_t now;
	int listener;
	int timeout;
	int expired;
	int ready;
	int error;

	/* The node of its own and the record, when given; anything else is a misuse. */
	error = btd_arguments(argc, argv);
	if (error != 0) {
		(void)fprintf(stderr, "usage: bluetoothd [-f /dev/bluetoothN] [-s SNOOP-FILE]\n");
		return 64;
	}

	/* A client that goes away must not end the daemon; what it does goes to the system's log. */
	(void)signal(SIGPIPE, SIG_IGN);
	openlog("bluetoothd", LOG_PID, LOG_DAEMON);

	/* No client yet. */
	for (index = 0U; index < BTD_CLIENTS_MAX; index++)
		btd_clients[index].descriptor = -1;

	/* The socket, made as root. */
	listener = btd_listen();
	if (listener < 0) {
		btd_log("bluetoothd: %s: %s\n", BTD_SOCKET, strerror(errno));
		return 1;
	}

	/* The separation: from here on this is the child, running as the daemon's account. */
	error = btd_privsep_start(btd_node, BTD_KEYS_FOLDER, listener, &btd_separation);
	if (error != 0) {
		btd_log("bluetoothd: privilege separation: %s\n", strerror(error));
		return 1;
	}

	/* The record of the HCI traffic, opened by the child (a failure leaves the daemon without it). */
	if (btd_snoop_path != NULL) {
		btd_snoop = btd_snoop_open(btd_snoop_path);
		if (btd_snoop < 0)
			btd_log("bluetoothd: %s: %s\n", btd_snoop_path, strerror(errno));
	}

	/* The system's power events (the end of a sleep, phase005 section 4.9); without them the links are not checked after a sleep. */
	btd_system_open();

	/* The user's switch, as it was left (ws143-p006). */
	btd_power_load();

	/* The pairing, handed the session's connection packets from each start on. */
	btd_pair_init(&btd_pairing, &btd_session, BTD_KEYS_FOLDER, btd_ask, btd_paired, NULL, btd_random, NULL);
	btd_router_init(&btd_routing, &btd_pairing);

	/* The link manager, whose page the router ends on a Connection Complete and the pairing takes. */
	btd_linkmgr_init(&btd_links, &btd_session);
	btd_router_set_linkmgr(&btd_routing, &btd_links);
	btd_pair_set_linkmgr(&btd_pairing, &btd_links);

	/*
	 * The HID host, the router's owner of its connections, and the
	 * pairing's handoff: a paired device that looks like a HID device goes
	 * on to HID on the same link (phase005 section 9.2).
	 */
	hid_hooks.context = NULL;
	hid_hooks.open_bridge = btd_hid_bridge;
	hid_hooks.answer = btd_hid_told;
	btd_hid_init(&btd_hid_host, &btd_session, BTD_KEYS_FOLDER, &btd_routing, &hid_hooks);
	router_hid.context = &btd_hid_host;
	router_hid.wants = btd_hid_wants;
	router_hid.claims = btd_hid_claims;
	router_hid.handle = btd_hid_handle;
	btd_router_set_hid(&btd_routing, &router_hid);
	btd_pair_set_handoff(&btd_pairing, btd_hid_handoff, &btd_hid_host);

	/* The phone link, the router's owner of a phone's link and the pairing's phone hook (ws197-p002). */
	btd_sdps_db_init(&btd_records);
	phone_hooks.context = NULL;
	phone_hooks.account = btd_account;
	phone_hooks.log = btd_messages_log;
	btd_phone_init(&btd_phone_link, &btd_session, &btd_routing, &btd_hid_host, &btd_records, BTD_KEYS_FOLDER, &phone_hooks);
	router_phone.context = &btd_phone_link;
	router_phone.wants = btd_phone_wants;
	router_phone.claims = btd_phone_claims;
	router_phone.handle = btd_phone_handle;
	btd_router_set_phone(&btd_routing, &router_phone);
	btd_pair_set_phone_handoff(&btd_pairing, btd_phone_handoff, &btd_phone_link);

	/*
	 * The MAP client (ws197-p003 section 8): the phone link's profile, its
	 * hooks into the phone link and the clients, its sessions and requests
	 * counted from a random number (a handle or request of an earlier run
	 * is not taken for one of this run).
	 */
	memset(&map_hooks, 0, sizeof(map_hooks));
	map_hooks.clock = btd_messages_clock;
	map_hooks.wall = btd_messages_wall;
	map_hooks.local_offset = btd_messages_offset;
	map_hooks.wanted = btd_messages_wanted;
	map_hooks.sdp_query = btd_messages_sdp;
	map_hooks.dlc_open = btd_messages_open;
	map_hooks.dlc_write = btd_messages_write;
	map_hooks.dlc_close = btd_messages_close;
	map_hooks.answer = btd_messages_answer;
	map_hooks.emit = btd_messages_emit;
	map_hooks.room = btd_messages_room;
	map_hooks.up = btd_messages_up;
	map_hooks.log = btd_messages_log;
	btd_random(NULL, (uint8_t *)&first_session, sizeof(first_session));
	btd_map_init(&btd_messages, &map_hooks, first_session);

	/* The profiles' mux, the phone link's one profile. */
	memset(&mux_hooks, 0, sizeof(mux_hooks));
	mux_hooks.sdp_query = btd_profiles_sdp;
	mux_hooks.dlc_open = btd_profiles_open;
	mux_hooks.dlc_close = btd_profiles_close;
	mux_hooks.log = btd_messages_log;
	btd_phonemux_init(&btd_profiles, &mux_hooks);

	/* The MAP client as the mux's first child. */
	memset(&phone_profile, 0, sizeof(phone_profile));
	phone_profile.context = &btd_messages;
	phone_profile.ready = btd_map_ready;
	phone_profile.ended = btd_map_ended;
	phone_profile.sdp_done = btd_map_sdp_done;
	phone_profile.accept = btd_map_accept;
	phone_profile.opened = btd_map_opened;
	phone_profile.data = btd_map_data;
	phone_profile.writable = btd_map_writable;
	phone_profile.closed = btd_map_closed;
	phone_profile.open_failed = btd_map_open_failed;
	(void)btd_phonemux_add(&btd_profiles, &phone_profile, &btd_messages_child);

	/* The PBAP client, its cursors' generations from a number drawn now; MAP's hooks where they serve it too. */
	memset(&pbap_hooks, 0, sizeof(pbap_hooks));
	pbap_hooks.clock = btd_messages_clock;
	pbap_hooks.wall = btd_messages_wall;
	pbap_hooks.local_offset = btd_messages_offset;
	pbap_hooks.wanted = btd_contacts_wanted;
	pbap_hooks.address = btd_contacts_address;
	pbap_hooks.sdp_query = btd_contacts_sdp;
	pbap_hooks.dlc_open = btd_contacts_open;
	pbap_hooks.dlc_write = btd_messages_write;
	pbap_hooks.dlc_close = btd_messages_close;
	pbap_hooks.answer = btd_messages_answer;
	pbap_hooks.room = btd_messages_room;
	pbap_hooks.up = btd_messages_up;
	pbap_hooks.log = btd_messages_log;
	btd_random(NULL, (uint8_t *)&first_generation, sizeof(first_generation));
	btd_pbap_init(&btd_contacts, &pbap_hooks, first_generation);

	/* The PBAP client as the mux's second child. */
	memset(&phone_profile, 0, sizeof(phone_profile));
	phone_profile.context = &btd_contacts;
	phone_profile.ready = btd_pbap_ready;
	phone_profile.ended = btd_pbap_ended;
	phone_profile.sdp_done = btd_pbap_sdp_done;
	phone_profile.accept = btd_pbap_accept;
	phone_profile.opened = btd_pbap_opened;
	phone_profile.data = btd_pbap_data;
	phone_profile.writable = btd_pbap_writable;
	phone_profile.closed = btd_pbap_closed;
	phone_profile.open_failed = btd_pbap_open_failed;
	(void)btd_phonemux_add(&btd_profiles, &phone_profile, &btd_contacts_child);

	/* The mux as the phone link's profile. */
	btd_phonemux_profile(&btd_profiles, &phone_profile);
	btd_phone_set_profile(&btd_phone_link, &phone_profile);

	/* The controller there is now. */
	btd_open();
	btd_log("BLUETOOTHD READY state=%s uid=%u\n", btd_state_name(btd_session.state), (unsigned)getuid());

	/* Clients, the controller's packets, the deadlines and the look for nodes. */
	for (;;) {
		/* The listener, the controller's node, the parent's liveness and every client. */
		count = 0U;
		descriptors[count].fd = listener;
		descriptors[count].events = POLLIN;
		count++;
		descriptors[count].fd = -1;
		descriptors[count].events = POLLIN;
		if (btd_session_open)
			descriptors[count].fd = btd_session.descriptor;
		count++;
		descriptors[count].fd = btd_separation.liveness;
		descriptors[count].events = POLLIN;
		count++;

		/* Each client's descriptor, writable too while its queue holds something. */
		for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
			descriptors[count].fd = btd_clients[index].descriptor;
			descriptors[count].events = POLLIN;
			pending = btd_outq_pending(&btd_clients[index].output);
			if (pending != 0U)
				descriptors[count].events = POLLIN | POLLOUT;
			count++;
		}

		/* The system's events, at 3 + BTD_CLIENTS_MAX. */
		descriptors[count].fd = btd_system;
		descriptors[count].events = POLLIN;
		count++;

		/* The wait, as long as the earliest deadline allows. */
		timeout = btd_timeout();
		for (index = 0U; index < count; index++)
			descriptors[index].revents = 0;
		ready = poll(descriptors, count, timeout);
		if (ready < 0 && errno != EINTR)
			return 1;

		/* The parent went: nothing can be opened any more. */
		if ((descriptors[2].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
			btd_log("bluetoothd: the privileged parent went; ending\n");
			return 0;
		}

		/* The controller's packets: those the node has, and those queued while a command waited (review BL2). */
		if (btd_session_open) {
			if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
				btd_packets();
		}

		/* Packets queued while a command waited, even when the node has nothing new. */
		if (btd_session_open) {
			ready = btd_session_pending(&btd_session);
			if (ready)
				btd_packets();
		}

		/* The pairing's deadlines. */
		now = btd_now_ms();
		btd_pair_tick(&btd_pairing, now);

		/* The HID host's deadlines and pages (held while a pairing or a scan runs). */
		if (btd_session_open && btd_session.state == BTD_STATE_READY) {
			btd_hid_holding();
			btd_hid_tick(&btd_hid_host, now);
		}

		/* The phone link's seat every few seconds, its deadlines, and its frames moved on to the session. */
		if (btd_session_open && btd_session.state == BTD_STATE_READY) {
			if (now >= btd_seat_check_at)
				btd_seat_check(now);
			btd_phone_tick(&btd_phone_link, now);
			btd_phone_pump(&btd_phone_link);
		}

		/* MAP's timers (its DLCs to close, OBEX's answers, the attempts after failures, the slow clients). */
		btd_map_tick(&btd_messages, now);

		/* PBAP's timers, the same kinds. */
		btd_pbap_tick(&btd_contacts, now);

		/* The link manager's refused page scan write, and a page nobody ended in time. */
		if (btd_session_open && btd_session.state == BTD_STATE_READY) {
			expired = btd_linkmgr_tick(&btd_links, now);
			if (expired) {
				btd_format_address(btd_links.expired_address, expired_text, sizeof(expired_text));
				btd_log("bluetoothd: the page of %s was not ended in time\n", expired_text);
			}
		}

		/* A scan that is over answers the client that asked. */
		if (btd_session_open && btd_session.scanning && now >= btd_session.scan_end_ms)
			btd_scan_end();

		/* A controller reset under the daemon is started again. */
		if (btd_session_open && btd_session.state == BTD_STATE_ERROR)
			btd_close();

		/* A booted controller that did not come back is a failed load. */
		if (!btd_session_open && btd_reappear_ms != 0U && now >= btd_reappear_ms) {
			btd_reappear_ms = 0U;
			btd_session.state = BTD_STATE_FIRMWARE_FAILED;
			(void)snprintf(btd_session.reason, sizeof(btd_session.reason), "%s", "the controller did not come back after its boot");
			btd_log("bluetoothd: %s\n", btd_session.reason);
		}

		/* No controller: the nodes are looked for again, unless the daemon stopped trying. */
		if (!btd_session_open && !btd_stopped && now >= btd_looked_ms + BTD_RETRY_MS)
			btd_open();

		/* What the clients that can take it are sent (before a new client takes a slot). */
		for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
			if (btd_clients[index].descriptor < 0 || btd_clients[index].dead)
				continue;
			if ((descriptors[3U + index].revents & POLLOUT) == 0)
				continue;
			btd_client_flush((int)index);
		}

		/* A new client, then each client's lines. */
		if ((descriptors[0].revents & POLLIN) != 0)
			btd_accept(listener);
		for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
			if (btd_clients[index].descriptor < 0 || btd_clients[index].dead)
				continue;
			if ((descriptors[3U + index].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
				continue;
			btd_read((int)index);
		}

		/* The phone's owner and state as its subscribers know them, the pages waiting for room, the dropped events told. */
		btd_phone_watch();

		/* The clients that stopped reading are closed now. */
		for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
			if (btd_clients[index].descriptor >= 0 && btd_clients[index].dead)
				btd_client_close((int)index);
		}

		/* The system's events: the end of a sleep checks the HID links. */
		if (btd_system >= 0 && (descriptors[3U + BTD_CLIENTS_MAX].revents & POLLIN) != 0)
			btd_system_events();
	}
}

/* Writes a line to the system's log and to standard error. */
static void
btd_log(
	const char *format,
	...)
{
	va_list arguments;

	/* The system's log. */
	va_start(arguments, format);
	vsyslog(LOG_INFO, format, arguments);
	va_end(arguments);

	/* Standard error, which the service's log keeps. */
	va_start(arguments, format);
	(void)vfprintf(stderr, format, arguments);
	va_end(arguments);
}

/* Listens on the socket, which anyone may use (a change is checked by uid). */
static int
btd_listen(
	void)
{
	struct sockaddr_un address;
	int descriptor;
	int status;
	int error;

	/* The socket, in place of an old one. */
	descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", BTD_SOCKET);
	(void)unlink(BTD_SOCKET);

	/* Bound, readable and writable by everyone, listening. */
	status = bind(descriptor, (struct sockaddr *)&address, sizeof(address));
	if (status == 0)
		status = chmod(BTD_SOCKET, 0666);
	if (status == 0)
		status = listen(descriptor, (int)BTD_CLIENTS_MAX);
	if (status != 0) {
		error = errno;
		(void)close(descriptor);
		errno = error;
		return -1;
	}

	/* Succeeded: the listener. */
	return descriptor;
}

/*
 * Opens the controller's node through the parent and starts it.  A start
 * that failed with the node still there keeps it open (its state says
 * why); a node that went during the start is closed, and so is one whose
 * start a new start may mend.
 */
static void
btd_open(
	void)
{
	char key[BTD_KEY_MAX];
	char path[BTD_PATH_MAX];
	const char *trace_label;
	int descriptor;
	int loaded;
	int error;

	/* Looked for now. */
	btd_looked_ms = btd_now_ms();

	/* The node, from the parent. */
	error = btd_privsep_open(&btd_separation, path, sizeof(path), &descriptor);

	/* No controller (a busy node, another program's, is logged once). */
	if (error != 0) {
		if (error == EBUSY && !btd_busy_logged)
			btd_log("bluetoothd: the controller's node is open in another program\n");
		btd_busy_logged = (error == EBUSY);
		if (error != EBUSY && error != ENOENT)
			btd_log("bluetoothd: the parent could not open a node: %s\n", strerror(error));
		return;
	}

	/* A node that opened ends the busy note. */
	btd_busy_logged = 0;

	/* Its session, its packets handed to the router; a controller loaded before must not be loaded again. */
	btd_session_init(&btd_session, descriptor, btd_node_control, &btd_session, path, BTD_FIRMWARE_FOLDER);
	btd_session.handler = btd_router_handle;
	btd_session.handler_context = &btd_routing;
	btd_session.packet_trace = btd_trace;
	btd_session_open = 1;
	btd_reappear_ms = 0U;
	error = ioctl(descriptor, BT_IOC_GET_INFO, &btd_session.info);
	key[0] = '\0';
	if (error == 0)
		btd_key(&btd_session.info, key, sizeof(key));
	loaded = 0;
	if (key[0] != '\0')
		loaded = btd_loaded(key);
	if (loaded)
		btd_session.load_allowed = 0;

	/* The start, logged with the first Secure Send answers when a load was sent. */
	error = btd_session_start(&btd_session);
	trace_label = "";
	if (btd_session.trace[0] != '\0')
		trace_label = ", secure send answers ";
	btd_log("bluetoothd: %s state=%s %s (error %d, stray %u, malformed %u, ssp %d, sc %d%s%s)\n",
		path,
		btd_state_name(btd_session.state),
		btd_session.reason,
		error,
		btd_session.stray_answers,
		btd_session.malformed,
		btd_session.ssp,
		btd_session.secure_connections,
		trace_label,
		btd_session.trace);

	/* A load was sent: the controller is remembered, and one that went to boot is awaited. */
	if (btd_session.load_sent && key[0] != '\0')
		btd_remember_load(key);
	if (btd_session.load_sent && btd_session.state == BTD_STATE_LOST)
		btd_reappear_ms = btd_now_ms() + BTD_REAPPEAR_MS;

	/* A ready start ends a run of failures (the HID devices are read); a failed one counts, and too many stop the tries. */
	if (btd_session.state == BTD_STATE_READY) {
		btd_failures = 0U;
		if (!btd_powered_off)
			btd_hid_refresh(&btd_hid_host);
		error = btd_phone_load(&btd_phone_link);
		if (error == EEXIST)
			btd_log("bluetoothd: two phones' records are valid; neither is used until one is forgotten\n");
	} else if (btd_session.state == BTD_STATE_ERROR) {
		btd_failures++;
		if (btd_failures >= BTD_FAILURES_MAX) {
			btd_stopped = 1;
			btd_log("bluetoothd: %u starts failed in a row; not trying again until restarted\n", btd_failures);
		}
	}

	/* A node that went, or a step that a new start may mend, is closed (looked for again later). */
	if (btd_session.state == BTD_STATE_LOST || btd_session.state == BTD_STATE_ERROR)
		btd_close();
}

/* Closes the controller's node; a scan's waiting client hears that it ended, and a pairing ends as lost. */
static void
btd_close(
	void)
{
	unsigned index;

	/* Nothing open. */
	if (!btd_session_open)
		return;

	/* A pairing cannot go on without the controller (review S-f), and no connection or page is left. */
	btd_pair_lost(&btd_pairing);
	btd_hid_lost(&btd_hid_host);
	btd_phone_lost(&btd_phone_link);
	btd_router_clear(&btd_routing);
	btd_linkmgr_reset(&btd_links);

	/* A client waiting for a scan is answered. */
	for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
		if (btd_clients[index].descriptor < 0 || !btd_clients[index].waits_scan)
			continue;
		btd_clients[index].waits_scan = 0;
		btd_write(&btd_clients[index], "ERROR lost\nDONE\n");
	}

	/* The node closed; the nodes are looked for again after the retry time. */
	btd_log("bluetoothd: %s closed (%s %s)\n", btd_session.path, btd_state_name(btd_session.state), btd_session.reason);
	(void)close(btd_session.descriptor);
	btd_session_open = 0;
	if (!btd_stopped)
		btd_session.state = BTD_STATE_NONE;
	btd_looked_ms = btd_now_ms();
}

/*
 * Handles the controller's packets, queued ones first, BTD_PACKETS_A_ROUND
 * at most; a node that went is closed.
 */
static void
btd_packets(
	void)
{
	unsigned handled;
	int error;

	/* Packet by packet until none is left or the round is full. */
	for (handled = 0U; handled < BTD_PACKETS_A_ROUND; handled++) {
		error = btd_session_input(&btd_session);
		if (error == EAGAIN)
			return;

		/* The node went. */
		if (error != 0) {
			btd_close();
			return;
		}

		/* A handler that met a reset leaves the session in error (the loop closes it). */
		if (!btd_session_open)
			return;
	}
}

/*
 * Gives the poll's timeout: 0 while packets are queued, else the earliest
 * of the scan's end, the pairing's deadline, the next look for nodes and a
 * booted controller's return (-1: none).
 */
static int
btd_timeout(
	void)
{
	uint64_t now;
	uint64_t earliest;
	uint64_t deadline;
	int pending;
	int ready;

	/* Queued packets are handled at once. */
	if (btd_session_open) {
		pending = btd_session_pending(&btd_session);
		if (pending)
			return 0;
	}

	/* The earliest deadline. */
	now = btd_now_ms();
	earliest = 0U;
	if (btd_session_open && btd_session.scanning)
		earliest = btd_session.scan_end_ms;
	deadline = btd_pair_deadline(&btd_pairing);
	if (deadline != 0U && (earliest == 0U || deadline < earliest))
		earliest = deadline;
	deadline = btd_hid_deadline(&btd_hid_host);
	if (btd_session_open && deadline != 0U && (earliest == 0U || deadline < earliest))
		earliest = deadline;
	/*
	 * The phone link's deadlines and the seat's next look, while the
	 * controller is ready (only then does the loop tick them; a look not
	 * made yet is due at once).  A seat's look of 0 must not count as no
	 * deadline: it would wait for a descriptor and stop the HID host's
	 * ticks (T1-524).
	 */
	ready = 0;
	if (btd_session_open && btd_session.state == BTD_STATE_READY)
		ready = 1;
	deadline = btd_phone_deadline(&btd_phone_link);
	if (ready &&
	    deadline != 0U &&
	    (earliest == 0U || deadline < earliest))
		earliest = deadline;
	deadline = btd_seat_check_at;
	if (deadline == 0U)
		deadline = now;
	if (ready && (earliest == 0U || deadline < earliest))
		earliest = deadline;

	/* MAP's next timer (its own clock is the same). */
	deadline = btd_map_deadline(&btd_messages);
	if (deadline != 0U && (earliest == 0U || deadline < earliest))
		earliest = deadline;

	/* PBAP's next timer. */
	deadline = btd_pbap_deadline(&btd_contacts);
	if (deadline != 0U && (earliest == 0U || deadline < earliest))
		earliest = deadline;
	deadline = btd_linkmgr_deadline(&btd_links, now);
	if (btd_session_open &&
	    deadline != 0U &&
	    (earliest == 0U || deadline < earliest))
		earliest = deadline;
	if (!btd_session_open && !btd_stopped) {
		deadline = btd_looked_ms + BTD_RETRY_MS;
		if (earliest == 0U || deadline < earliest)
			earliest = deadline;
	}

	/* A booted controller's return. */
	if (btd_reappear_ms != 0U && (earliest == 0U || btd_reappear_ms < earliest))
		earliest = btd_reappear_ms;

	/* None: wait for a descriptor. */
	if (earliest == 0U)
		return -1;

	/* Due already. */
	if (earliest <= now)
		return 0;

	/* Succeeded: the milliseconds to the earliest. */
	return (int)(earliest - now);
}

/* Takes a new client with its uid, when there is a free slot. */
static void
btd_accept(
	int listener)
{
	unsigned free_slots;
	unsigned held;
	unsigned index;
	uid_t uid;
	gid_t gid;
	int descriptor;
	int seated;
	int taken;
	int status;

	/* The connection; its reads never block. */
	descriptor = accept(listener, NULL, NULL);
	if (descriptor < 0)
		return;
	(void)fcntl(descriptor, F_SETFL, O_NONBLOCK);
	(void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);

	/* Who it is; a client that cannot be told is nobody's. */
	status = getpeereid(descriptor, &uid, &gid);
	if (status != 0) {
		(void)close(descriptor);
		return;
	}

	/* The free slots. */
	free_slots = 0U;
	for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
		if (btd_clients[index].descriptor < 0)
			free_slots++;
	}

	/* The connections the uid holds already. */
	held = 0U;
	for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
		if (btd_clients[index].descriptor >= 0 && btd_clients[index].uid == uid)
			held++;
	}

	/* Whether it is the seat's user, looked at only when the rules need it (the last few slots, a uid at its share). */
	seated = 0;
	if (uid != 0 &&
	    (free_slots <= BTD_CLIENTS_RESERVED ||
	     held >= BTD_CLIENTS_PER_UID))
		seated = btd_seated(uid);

	/* Root and the seat's user while a slot is free; others not into the reserve nor past their share (sections 4.2, p004 5.4). */
	taken = btd_phoneio_accept(uid, seated, free_slots, BTD_CLIENTS_RESERVED, held, BTD_CLIENTS_PER_UID);
	if (!taken) {
		(void)close(descriptor);
		return;
	}

	/* A free slot, with a new generation and an empty queue. */
	for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
		if (btd_clients[index].descriptor >= 0)
			continue;
		btd_generation++;
		btd_clients[index].descriptor = descriptor;
		btd_clients[index].uid = uid;
		btd_clients[index].generation = btd_generation;
		btd_clients[index].dead = 0;
		btd_phoneio_input_init(&btd_clients[index].input);
		btd_clients[index].waits_scan = 0;
		btd_clients[index].waits_pair = 0;
		btd_clients[index].waits_connect = 0;
		btd_clients[index].waits_phone = 0;
		btd_clients[index].subscribed = 0;
		btd_clients[index].sub_dropped = 0;
		btd_outq_init(&btd_clients[index].output, BTD_OUTQ_MAX);
		return;
	}

	/* Too many clients. */
	(void)close(descriptor);
}

/*
 * Reads a client's bytes: each whole line is a request, and a PHONE SEND's
 * text is read whole before its request (its bytes are never read as
 * lines, section 4.3).  A line too long, or a malformed PHONE SEND line,
 * closes the client.
 */
static void
btd_read(
	int index)
{
	struct btd_phoneio_events events;
	struct btd_client *client;
	uint8_t *room;
	size_t size;
	ssize_t count;
	int error;

	/* What came, without waiting, where the input takes it. */
	client = &btd_clients[index];
	btd_phoneio_input_room(&client->input, &room, &size);
	count = recv(client->descriptor, room, size, 0);
	if (count < 0) {
		error = errno;
		if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR)
			return;
	}

	/* A client that went (or failed). */
	if (count <= 0) {
		btd_client_close(index);
		return;
	}

	/* The bytes taken: the requests they complete. */
	events.context = client;
	events.line = btd_client_line;
	events.text = btd_client_text;
	error = btd_phoneio_input_got(&client->input, (size_t)count, &events);

	/* A malformed PHONE SEND line: the stream cannot be read on, the client is told and goes. */
	if (error == EINVAL) {
		btd_write(client, "ERROR length\nDONE\n");
		client->dead = 1;
		return;
	}

	/* A line too long, or no room for a text: the client goes (a client gone already is left as it is). */
	if (error == EMSGSIZE || error == ENOMEM)
		btd_client_close(index);
}

/* Carries out a client's request line (its input's hook); tells whether the client went. */
static int
btd_client_line(
	void *context,
	char *line)
{
	struct btd_client *client;
	int index;

	/* The client and its slot. */
	client = context;
	index = (int)(client - btd_clients);

	/* The request. */
	btd_line(index, line);

	/* The client went with it: its input is not read on. */
	if (client->descriptor < 0)
		return 1;

	/* Read on. */
	return 0;
}

/*
 * Takes a PHONE SEND's whole text (the input's hook): its request, unless
 * the client waits for another answer (the text was read and is dropped
 * either way, section 4.3).
 */
static void
btd_client_text(
	void *context,
	const char *line,
	const uint8_t *text,
	size_t length)
{
	struct btd_client *client;
	int index;

	/* The client and its slot. */
	client = context;
	index = (int)(client - btd_clients);

	/* A client waiting for another answer asks nothing more until it is answered, and a subscriber asks nothing. */
	if (client->waits_scan ||
	    client->waits_pair ||
	    client->waits_connect ||
	    client->waits_phone ||
	    client->subscribed)
		return;

	/* Succeeded: the request. */
	btd_phone_send_request(index, line, text, length);
}

/*
 * Starts PHONE SEND to="NUMBER" length=N with its text whole (ws197-p004
 * section 5.1): the owner's and root's, answered by MAP (PHONE SENT and
 * DONE, or ERROR).
 */
static void
btd_phone_send_request(
	int index,
	const char *line,
	const uint8_t *text,
	size_t length)
{
	struct btd_client *client;
	char key[BTD_PHONEIO_KEY_MAX];
	char value[BTD_PHONEIO_VALUE_MAX];
	char number[BTD_PHONEIO_VALUE_MAX];
	const char *cursor;
	const char *refused;
	int allowed;
	int got;
	int same;

	/* The owner and root. */
	client = &btd_clients[index];
	allowed = btd_phone_allowed(client->uid);
	if (!allowed) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* The number among the arguments (the length was read by the input). */
	number[0] = '\0';
	cursor = line + strlen("PHONE SEND");
	for (;;) {
		got = btd_phoneio_next(&cursor, key, sizeof(key), value, sizeof(value));
		if (got != BTD_PHONEIO_ARGUMENT)
			break;

		/* to= is the number; length= was the input's. */
		same = strcmp(key, "to");
		if (same == 0) {
			(void)snprintf(number, sizeof(number), "%s", value);
			continue;
		}

		/* Any other key. */
		same = strcmp(key, "length");
		if (same != 0) {
			btd_write(client, "ERROR argument\nDONE\n");
			return;
		}
	}

	/* A number given. */
	if (number[0] == '\0') {
		btd_write(client, "ERROR number\nDONE\n");
		return;
	}

	/* Asked of MAP, the client waiting for its DONE (a refusal answered now). */
	client->waits_phone = 1;
	refused = btd_map_send(&btd_messages, btd_token(index), number, text, length);
	if (refused != NULL) {
		client->waits_phone = 0;
		btd_write(client, "ERROR %s\nDONE\n", refused);
	}
}

/* Carries out one request line, or takes an answer to the question asked. */
static void
btd_line(
	int index,
	char *line)
{
	struct btd_client *client;
	int same;

	/* An answer to the agent's question: YES or NO, from the client asked. */
	client = &btd_clients[index];
	same = strcmp(line, "YES");
	if (same == 0) {
		btd_answer(index, 1);
		return;
	}

	/* NO, likewise. */
	same = strcmp(line, "NO");
	if (same == 0) {
		btd_answer(index, 0);
		return;
	}

	/* A client waiting for its scan, its pairing, its connection or a phone request asks nothing more until it is answered; a subscriber asks nothing. */
	if (client->waits_scan ||
	    client->waits_pair ||
	    client->waits_connect ||
	    client->waits_phone ||
	    client->subscribed)
		return;

	/* SHOW. */
	same = strcmp(line, "SHOW");
	if (same == 0) {
		btd_show(client);
		return;
	}

	/* DEVICES. */
	same = strcmp(line, "DEVICES");
	if (same == 0) {
		btd_devices(client);
		return;
	}

	/* BONDS. */
	same = strcmp(line, "BONDS");
	if (same == 0) {
		btd_bonds(client);
		return;
	}

	/* AGENT. */
	same = strcmp(line, "AGENT");
	if (same == 0) {
		btd_agent(index);
		return;
	}

	/* SCAN SECONDS. */
	same = strncmp(line, "SCAN ", 5U);
	if (same == 0) {
		btd_scan(client, line + 5);
		return;
	}

	/* PAIR ADDRESS TYPE [phone=1]. */
	same = strncmp(line, "PAIR ", 5U);
	if (same == 0) {
		btd_pair(index, line + 5);
		return;
	}

	/* FORGET ADDRESS TYPE. */
	same = strncmp(line, "FORGET ", 7U);
	if (same == 0) {
		btd_forget(client, line + 7);
		return;
	}

	/* CONNECT ADDRESS TYPE (ws143-p005). */
	same = strncmp(line, "CONNECT ", 8U);
	if (same == 0) {
		btd_connect(index, line + 8);
		return;
	}

	/* DISCONNECT ADDRESS TYPE (ws143-p005). */
	same = strncmp(line, "DISCONNECT ", 11U);
	if (same == 0) {
		btd_disconnect(client, line + 11);
		return;
	}

	/* STATUS (ws143-p005). */
	same = strcmp(line, "STATUS");
	if (same == 0) {
		btd_status(client);
		return;
	}

	/* POWER on|off (ws143-p006). */
	same = strncmp(line, "POWER ", 6U);
	if (same == 0) {
		btd_power(client, line + 6);
		return;
	}

	/* PHONE SHOW and PHONE LINK (ws197-p003); PHONE PROBE ADDRESS uuid=0x1132|0x112F and PHONE DROP ADDRESS (ws197-p002, root). */
	same = strncmp(line, "PHONE ", 6U);
	if (same == 0) {
		btd_phone_request(index, line + 6);
		return;
	}

	/* Anything else. */
	btd_write(client, "ERROR request\nDONE\n");
}

/*
 * Answers POWER on|off (ws143-p006): those D8 permits turn Bluetooth on or
 * off.  Off is refused while a scan or a pairing runs (ERROR busy); the
 * saved keys stay either way.
 */
static void
btd_power(
	struct btd_client *client,
	const char *argument)
{
	int permitted;
	int pairing;
	int error;
	int on;
	int off;

	/* Only those D8 permits. */
	permitted = btd_permitted(client->uid);
	if (!permitted) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* on or off. */
	on = strcmp(argument, "on");
	off = strcmp(argument, "off");
	if (on != 0 && off != 0) {
		btd_write(client, "ERROR power\nDONE\n");
		return;
	}

	/* On: scans and pairings may start again; kept for the next start. */
	if (on == 0) {
		btd_powered_off = 0;
		btd_seat_check_at = 0U;
		error = btd_power_save();
		btd_log("BLUETOOTHD POWER on uid=%u saved=%d\n", (unsigned)client->uid, error == 0);
		btd_write(client, "POWER on\nDONE\n");
		return;
	}

	/* Off waits for no scan or pairing that runs. */
	pairing = btd_pair_active(&btd_pairing);
	if ((btd_session_open && btd_session.scanning) || pairing || btd_pair_client != BTD_NO_CLIENT) {
		btd_write(client, "ERROR busy\nDONE\n");
		return;
	}

	/* Succeeded: off until POWER on, kept for the next start; the phone link sees nobody at the seat at once. */
	btd_powered_off = 1;
	btd_seat_check_at = 0U;
	error = btd_power_save();
	btd_log("BLUETOOTHD POWER off uid=%u saved=%d\n", (unsigned)client->uid, error == 0);
	btd_write(client, "POWER off\nDONE\n");
}

/* Reads the user's switch as it was left: off when the file says so, else on. */
static void
btd_power_load(
	void)
{
	char text[8];
	ssize_t got;
	int descriptor;
	int same;

	/* On unless the file says off. */
	btd_powered_off = 0;
	descriptor = open(BTD_POWER_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return;
	got = read(descriptor, text, sizeof(text) - 1U);
	(void)close(descriptor);
	if (got <= 0)
		return;

	/* Its word. */
	text[got] = '\0';
	same = strncmp(text, "off", 3U);
	if (same == 0) {
		btd_powered_off = 1;
		btd_log("BLUETOOTHD POWER loaded=off\n");
	}
}

/*
 * Keeps the user's switch for the next start: written beside, synced and
 * renamed in place, as the keys are.  Returns 0 or an errno value (the
 * switch holds for this run either way).
 */
static int
btd_power_save(
	void)
{
	const char *text;
	ssize_t written;
	size_t length;
	int descriptor;
	int error;

	/* The word. */
	text = "on\n";
	if (btd_powered_off)
		text = "off\n";

	/* The temporary file (never a link), written and synced. */
	descriptor = open(BTD_POWER_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (descriptor < 0)
		return errno;
	length = strlen(text);
	written = write(descriptor, text, length);
	if (written != (ssize_t)length) {
		(void)close(descriptor);
		return EIO;
	}

	/* On the disk before the rename. */
	error = fsync(descriptor);
	(void)close(descriptor);
	if (error != 0)
		return EIO;

	/* In place of the old one (the folder is the keys', synced with them). */
	error = rename(BTD_POWER_FILE ".tmp", BTD_POWER_FILE);
	if (error != 0)
		return errno;

	/* Succeeded: kept. */
	return 0;
}

/* Tells the client last asked a question of a pairing that the question is over (ASK-END). */
static void
btd_question_end(
	void)
{
	int index;

	/* Only a question asked. */
	index = btd_question_client;
	btd_question_client = BTD_NO_CLIENT;
	if (index == BTD_NO_CLIENT || btd_clients[index].descriptor < 0)
		return;

	/* Succeeded: told. */
	btd_write(&btd_clients[index], "ASK-END\n");
}

/* Answers SHOW: the state, and the controller when one is open. */
static void
btd_show(
	struct btd_client *client)
{
	char address[24];
	char name[4U * BT_TEXT_MAX];
	const char *firmware;
	int pairing;
	int error;

	/* The state, and why; a ready controller the user turned off is "off" (ws143-p006). */
	if (btd_powered_off && btd_session_open && btd_session.state == BTD_STATE_READY)
		btd_write(client, "STATE off\n");
	else if (btd_session.reason[0] != '\0')
		btd_write(client, "STATE %s %s\n", btd_state_name(btd_session.state), btd_session.reason);
	else
		btd_write(client, "STATE %s\n", btd_state_name(btd_session.state));

	/* The controller. */
	firmware = "-";
	if (btd_session.firmware_loaded)
		firmware = "loaded";
	pairing = btd_pair_active(&btd_pairing);
	if (btd_session_open) {
		btd_format_address(btd_session.address, address, sizeof(address));
		if (!btd_session.have_address)
			(void)snprintf(address, sizeof(address), "%s", "-");
		error = btd_escape(btd_session.info.name, name, sizeof(name));
		if (error != 0)
			name[0] = '\0';
		btd_write(client,
			  "CONTROLLER node=%s vendor=%04x product=%04x name=\"%s\" address=%s hci=%u manufacturer=%u le=%d p256=%d dhkey=%d firmware=%s ssp=%d sc=%d pairing=%d hid=%u page_scan=%d le_auto=0\n",
			  btd_session.path,
			  (unsigned)btd_session.info.vendor,
			  (unsigned)btd_session.info.product,
			  name,
			  address,
			  (unsigned)btd_session.hci_version,
			  (unsigned)btd_session.manufacturer,
			  btd_session.le,
			  btd_session.p256,
			  btd_session.dhkey,
			  firmware,
			  btd_session.ssp,
			  btd_session.secure_connections,
			  pairing,
			  btd_hid_open_count(&btd_hid_host),
			  btd_hid_host.page_scan);
	}

	/* The user's switch (ws143-p006), whatever the controller's state. */
	if (btd_powered_off) {
		btd_write(client, "POWER off\n");
	} else {
		btd_write(client, "POWER on\n");
	}

	/* The end. */
	btd_write(client, "DONE\n");
}

/* Answers DEVICES: the last scan's devices. */
static void
btd_devices(
	struct btd_client *client)
{
	const struct btd_device *device;
	char address[24];
	char name[4U * BTD_NAME_MAX];
	char extra[64];
	unsigned index;
	size_t used;
	int error;

	/* Each device, as one line. */
	for (index = 0U; index < btd_session.devices.count; index++) {
		device = &btd_session.devices.entries[index];
		btd_format_address(device->address, address, sizeof(address));
		error = btd_escape(device->name, name, sizeof(name));
		if (error != 0)
			name[0] = '\0';

		/* The fields that were seen. */
		extra[0] = '\0';
		used = 0U;
		if (device->has_rssi)
			used += (size_t)snprintf(extra + used, sizeof(extra) - used, " rssi=%d", device->rssi);
		if (device->has_class && used < sizeof(extra))
			used += (size_t)snprintf(extra + used, sizeof(extra) - used, " class=0x%06x", (unsigned)device->class_of_device);
		if (device->has_appearance && used < sizeof(extra))
			(void)snprintf(extra + used, sizeof(extra) - used, " appearance=0x%04x", (unsigned)device->appearance);

		/* The line. */
		btd_write(client, "DEVICE address=%s type=%s%s name=\"%s\"\n", address, btd_address_type_name(device->type), extra, name);
	}

	/* Devices the table had no room for. */
	if (btd_session.devices.dropped != 0U)
		btd_write(client, "DROPPED %u\n", btd_session.devices.dropped);

	/* The end. */
	btd_write(client, "DONE\n");
}

/* Starts a scan for one who may change things; the client hears the devices when it ends. */
static void
btd_scan(
	struct btd_client *client,
	const char *argument)
{
	unsigned long seconds;
	char *end;
	int permitted;
	int pairing;
	int error;

	/* Only those D8 permits (plan section 6). */
	permitted = btd_permitted(client->uid);
	if (!permitted) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* A length in range. */
	errno = 0;
	seconds = strtoul(argument, &end, 10);
	if (errno != 0 || end == argument || *end != '\0' || seconds == 0U || seconds > BTD_SCAN_SECONDS_MAX) {
		btd_write(client, "ERROR seconds\nDONE\n");
		return;
	}

	/* A controller. */
	if (!btd_session_open) {
		btd_write(client, "ERROR no-controller\nDONE\n");
		return;
	}

	/* None while the user has Bluetooth off (ws143-p006). */
	if (btd_powered_off) {
		btd_write(client, "ERROR off\nDONE\n");
		return;
	}

	/* One scan at a time, and none while a pairing runs. */
	pairing = btd_pair_active(&btd_pairing);
	if (btd_session.scanning || pairing) {
		btd_write(client, "ERROR busy\nDONE\n");
		return;
	}

	/* The scan, started (a controller that is not ready says so). */
	error = btd_session_scan_start(&btd_session, (unsigned)seconds);
	if (error == EBUSY) {
		btd_write(client, "ERROR not-ready\nDONE\n");
		return;
	}

	/* Any other failure, named. */
	if (error != 0) {
		btd_write(client, "ERROR %s\nDONE\n", strerror(error));
		if (error == ENODEV)
			btd_close();
		return;
	}

	/* Succeeded: the client waits for its end. */
	client->waits_scan = 1;
	btd_log("bluetoothd: scan of %lu s started\n", seconds);
}

/* Ends a scan that is over and answers the client that asked for it. */
static void
btd_scan_end(
	void)
{
	unsigned index;
	int error;

	/* The scan, stopped. */
	error = btd_session_scan_stop(&btd_session);
	btd_log("bluetoothd: scan ended: %u devices (dropped %u, malformed %u, error %d)\n",
		btd_session.devices.count,
		btd_session.devices.dropped,
		btd_session.malformed,
		error);

	/* The waiting client hears the devices. */
	for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
		if (btd_clients[index].descriptor < 0 || !btd_clients[index].waits_scan)
			continue;
		btd_clients[index].waits_scan = 0;
		btd_devices(&btd_clients[index]);
	}

	/* A node that went during the stop is closed. */
	if (error == ENODEV)
		btd_close();
}

/*
 * Starts a pairing for one who may change things (PAIR ADDRESS TYPE
 * [phone=1]): the client hears the questions when no agent answers for it,
 * and the end.  phone=1 pairs a phone for the phone link (ws197-p002
 * section 7.1, BR/EDR only); its PAIRED line ends with phone=1, or phone=0
 * and why.
 */
static void
btd_pair(
	int index,
	const char *argument)
{
	struct btd_client *client;
	uint8_t address[BTD_ADDRESS_BYTES];
	const char *refusal;
	unsigned type;
	unsigned links;
	int permitted;
	int seated;
	int phone;
	int busy;
	int own;
	int error;

	/* Only those D8 permits. */
	client = &btd_clients[index];
	permitted = btd_permitted(client->uid);
	if (!permitted) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* The device, and whether it is a phone. */
	error = btd_parse_pair(argument, address, &type, &phone);
	if (error != 0) {
		btd_write(client, "ERROR address\nDONE\n");
		return;
	}

	/* A phone is paired over BR/EDR alone. */
	if (phone && type != BTD_ADDRESS_BREDR) {
		btd_write(client, "ERROR phone-le\nDONE\n");
		return;
	}

	/* A ready controller, which the user has not turned off (ws143-p006). */
	if (!btd_session_open || btd_session.state != BTD_STATE_READY) {
		btd_write(client, "ERROR not-ready\nDONE\n");
		return;
	}

	/* None while the user has Bluetooth off. */
	if (btd_powered_off) {
		btd_write(client, "ERROR off\nDONE\n");
		return;
	}

	/* A phone needs a link the HID host does not use (ws197-p002 section 7.5). */
	links = btd_hid_link_count(&btd_hid_host);
	if (phone && links >= BTD_HID_MAX) {
		btd_write(client, "ERROR busy-links\nDONE\n");
		return;
	}

	/* A phone's pairing is the seat's user's, and another owner's phone is not paired again (ws197-p003 section 3.2). */
	seated = btd_seated(client->uid);
	refusal = btd_phone_pair_check(&btd_phone_link, address, client->uid, phone, seated);
	if (refusal != NULL) {
		btd_write(client, "ERROR %s\nDONE\n", refusal);
		return;
	}

	/* One pairing at a time, none while a HID device's connection is under way (review B7). */
	busy = btd_hid_busy(&btd_hid_host, address);
	if (btd_pair_client != BTD_NO_CLIENT || busy) {
		btd_write(client, "ERROR busy\nDONE\n");
		return;
	}

	/* A HID device of that address (open or waiting) lets go first: the pairing makes it anew (review B7). */
	btd_hid_release(&btd_hid_host, address, type);

	/* A phone whose bond the same uid made in this run keeps its key (BUG-287). */
	if (phone) {
		phone = BTD_PAIR_PHONE;
		own = btd_own_bond_find(address, client->uid);
		if (own)
			phone = BTD_PAIR_PHONE_OWN;
	}

	/* The client waits for the end (which may come at once). */
	btd_pair_client = index;
	client->waits_pair = 1;
	btd_log("bluetoothd: pairing %s asked by uid %u\n", argument, (unsigned)client->uid);
	error = btd_pair_start(&btd_pairing, address, type, 1, phone, client->uid);
	if (error == EBUSY) {
		btd_pair_client = BTD_NO_CLIENT;
		client->waits_pair = 0;
		btd_write(client, "ERROR busy\nDONE\n");
		return;
	}

	/* A phone's pairing of an LE address (refused above already; the pairing says so too). */
	if (error == EINVAL) {
		btd_pair_client = BTD_NO_CLIENT;
		client->waits_pair = 0;
		btd_write(client, "ERROR phone-le\nDONE\n");
	}
}

/*
 * Makes the client the agent (AGENT): it is asked the questions of
 * pairings of its own uid, or of anyone when it is the seat's user.  An
 * agent of another uid is not displaced while it is there.
 */
static void
btd_agent(
	int index)
{
	struct btd_client *client;
	struct btd_client *agent;
	int permitted;

	/* Only those D8 permits. */
	client = &btd_clients[index];
	permitted = btd_permitted(client->uid);
	if (!permitted) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* Another uid's agent stays (review M-d); the same uid's is replaced. */
	if (btd_agent_client != BTD_NO_CLIENT && btd_agent_client != index) {
		agent = &btd_clients[btd_agent_client];
		if (agent->uid != client->uid) {
			btd_write(client, "ERROR busy\nDONE\n");
			return;
		}

		/* The old one hears that it is no longer the agent. */
		btd_write(agent, "AGENT-END\n");
	}

	/* The agent from now on. */
	btd_agent_client = index;
	btd_write(client, "AGENT ok\nDONE\n");
}

/* Takes YES or NO from the client asked; from anyone else it is passed over. */
static void
btd_answer(
	int index,
	int accepted)
{
	/* Only the client asked. */
	if (btd_asked_client != index)
		return;

	/* Answered; the question is over for the client asked. */
	btd_asked_client = BTD_NO_CLIENT;
	btd_question_end();
	btd_pair_answer(&btd_pairing, accepted);
}

/* Forgets a bond (FORGET ADDRESS TYPE) for one who may change things. */
static void
btd_forget(
	struct btd_client *client,
	const char *argument)
{
	uint8_t address[BTD_ADDRESS_BYTES];
	unsigned type;
	int phone_record;
	int permitted;
	int error;

	/* Only those D8 permits. */
	permitted = btd_permitted(client->uid);
	if (!permitted) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* The device. */
	error = btd_parse_device(argument, address, &type);
	if (error != 0) {
		btd_write(client, "ERROR address\nDONE\n");
		return;
	}

	/* The bonds are the controller's: one must be open. */
	if (!btd_session_open || !btd_session.have_address) {
		btd_write(client, "ERROR no-controller\nDONE\n");
		return;
	}

	/* A phone's record goes first: a valid one by its owner and root alone (ws197-p003 section 3.2). */
	phone_record = ENOENT;
	if (type == BTD_ADDRESS_BREDR)
		phone_record = btd_phone_forget(&btd_phone_link, address, client->uid);
	if (phone_record == EPERM) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	} else if (phone_record != 0 && phone_record != ENOENT) {
		btd_write(client, "ERROR %s\nDONE\n", strerror(phone_record));
		return;
	}

	/* The bond's file, gone (a phone's record without its bond was forgotten all the same). */
	error = btd_keys_forget(BTD_KEYS_FOLDER, btd_session.address, address, type);
	if (error == ENOENT && phone_record == 0) {
		btd_log("bluetoothd: forgot the phone record of %s\n", argument);
		btd_write(client, "DONE\n");
		return;
	} else if (error == ENOENT) {
		btd_write(client, "ERROR not-bonded\nDONE\n");
		return;
	}

	/* Any other failure, named. */
	if (error != 0) {
		btd_write(client, "ERROR %s\nDONE\n", strerror(error));
		return;
	}

	/* A HID device's record goes too; a connected one hears the unplug (BR/EDR) and is disconnected (design section 6.3). */
	btd_hid_forget(&btd_hid_host, address, type);

	/* The bond is nobody's any more (BUG-287). */
	if (type == BTD_ADDRESS_BREDR)
		btd_own_bond_forget(address);

	/* Succeeded: forgotten. */
	btd_log("bluetoothd: forgot %s\n", argument);
	btd_write(client, "DONE\n");
}

/* Answers BONDS: the controller's bonds, without their keys. */
static void
btd_bonds(
	struct btd_client *client)
{
	static struct btd_bond bonds[BTD_BONDS_MAX];
	char address[24];
	char name[4U * BTD_NAME_MAX];
	unsigned count;
	unsigned index;
	int error;

	/* The controller's bonds. */
	count = 0U;
	error = 0;
	if (btd_session_open && btd_session.have_address)
		error = btd_keys_list(BTD_KEYS_FOLDER, btd_session.address, bonds, BTD_BONDS_MAX, &count);
	if (error != 0) {
		btd_write(client, "ERROR %s\nDONE\n", strerror(error));
		return;
	}

	/* Each bond as one line. */
	for (index = 0U; index < count; index++) {
		btd_format_address(bonds[index].address, address, sizeof(address));
		error = btd_escape(bonds[index].name, name, sizeof(name));
		if (error != 0)
			name[0] = '\0';
		btd_write(client,
			  "BOND address=%s type=%s authenticated=%d secure=%d legacy=%d name=\"%s\"\n",
			  address,
			  btd_address_type_name(bonds[index].type),
			  bonds[index].authenticated,
			  bonds[index].secure,
			  bonds[index].legacy,
			  name);
	}

	/* The keys read are not kept. */
	memset(bonds, 0, sizeof(bonds));

	/* The end. */
	btd_write(client, "DONE\n");
}

/*
 * Asks the agent for the pairing (the pairing's hook): the agent when it
 * answers for the pairing's client, else the pairing's client itself.  A
 * question nobody can be asked is answered no.
 */
static void
btd_ask(
	void *context,
	unsigned kind,
	uint32_t number)
{
	struct btd_client *pairer;
	struct btd_client *agent;
	struct stat status;
	char address[24];
	uid_t seat;
	int asked;
	int error;

	UNUSED_PARAMETER(context);

	/* The pairing's client, which may be asked itself. */
	asked = btd_pair_client;
	if (asked == BTD_NO_CLIENT) {
		btd_pair_answer(&btd_pairing, 0);
		return;
	}

	/* Its client record. */
	pairer = &btd_clients[asked];

	/* The agent answers for its own uid, or for anyone when it is the seat's user. */
	if (btd_agent_client != BTD_NO_CLIENT) {
		agent = &btd_clients[btd_agent_client];
		seat = 0;
		error = stat(BTD_SEAT_NODE, &status);
		if (error == 0)
			seat = status.st_uid;
		if (agent->uid == pairer->uid || (seat != 0 && agent->uid == seat))
			asked = btd_agent_client;
	}

	/*
	 * The device being paired and who started the pairing, after the
	 * question's words (ws143-p006: the desktop's window names them, and
	 * a seat's user may be asked for another user's pairing).
	 */
	btd_format_address(btd_pairing.address, address, sizeof(address));
	btd_question_client = asked;

	/* The question: a number to confirm, an agreement, or a passkey to show (no answer). */
	if (kind == BTD_PAIR_ASK_PASSKEY) {
		btd_write(&btd_clients[asked], "PASSKEY %06u address=%s type=%s uid=%u\n", (unsigned)number, address, btd_address_type_name(btd_pairing.type), (unsigned)pairer->uid);
		return;
	}

	/* A question that waits for YES or NO from the client asked. */
	btd_asked_client = asked;
	if (kind == BTD_PAIR_ASK_CONSENT) {
		btd_write(&btd_clients[asked], "CONSENT address=%s type=%s uid=%u\n", address, btd_address_type_name(btd_pairing.type), (unsigned)pairer->uid);
	} else {
		btd_write(&btd_clients[asked], "CONFIRM %06u address=%s type=%s uid=%u\n", (unsigned)number, address, btd_address_type_name(btd_pairing.type), (unsigned)pairer->uid);
	}
}

/* Tells the pairing's client the end (the pairing's hook) and logs it. */
static void
btd_paired(
	void *context,
	const char *answer)
{
	const char *taken;
	int failed;
	int paired;
	int index;

	UNUSED_PARAMETER(context);

	/* Logged, a failure with what failed it (BUG-287); a phone taken without a known Class of Device says so (ws197-p002 section 7.4). */
	failed = strncmp(answer, "ERROR", 5U);
	if (failed == 0 && btd_pairing.fail_event != NULL) {
		btd_log("bluetoothd: pairing: %s (%s, status 0x%02x)\n", answer, btd_pairing.fail_event, btd_pairing.fail_status);
	} else {
		btd_log("bluetoothd: pairing: %s\n", answer);
	}

	/* A new authenticated BR/EDR bond is its uid's for a phone's pairing later in this run (BUG-287). */
	paired = strncmp(answer, "PAIRED", 6U);
	if (paired == 0 && btd_pairing.type == BTD_ADDRESS_BREDR && btd_pairing.new_authenticated)
		btd_own_bond_keep(btd_pairing.address, btd_pairing.uid);
	taken = strstr(answer, " phone=1");
	if (taken != NULL && btd_phone_link.class_unknown)
		btd_log("bluetoothd: phone: cod=unknown\n");

	/* No question waits any more (the one asked hears it is over); the client that asked hears the end. */
	btd_asked_client = BTD_NO_CLIENT;
	btd_question_end();
	index = btd_pair_client;
	btd_pair_client = BTD_NO_CLIENT;
	if (index == BTD_NO_CLIENT)
		return;
	btd_clients[index].waits_pair = 0;
	btd_write(&btd_clients[index], "%s\nDONE\n", answer);
}

/*
 * Answers PHONE: SHOW and LINK (ws197-p003), and for root DROP ADDRESS,
 * which ends the phone's link (a test's tool, ws197-p002).
 */
static void
btd_phone_request(
	int index,
	const char *argument)
{
	struct btd_client *client;
	char address_text[18];
	uint8_t address[BTD_ADDRESS_BYTES];
	const char *rest;
	size_t length;
	int same;
	int error;

	/* SHOW: anyone, each seeing what the phone link lets them. */
	client = &btd_clients[index];
	same = strcmp(argument, "SHOW");
	if (same == 0) {
		btd_phone_show_request(client);
		return;
	}

	/* LINK ADDRESS on|off [profiles=...]: the owner and root, as the phone link checks. */
	same = strncmp(argument, "LINK ", 5U);
	if (same == 0) {
		btd_phone_link_request(client, argument + 5);
		return;
	}

	/* SUBSCRIBE: the owner's and root's events (ws197-p004 section 5.2). */
	same = strcmp(argument, "SUBSCRIBE");
	if (same == 0) {
		btd_phone_subscribe_request(index);
		return;
	}

	/* PAGE messages ...: a page of the synchronisation. */
	same = strncmp(argument, "PAGE ", 5U);
	if (same == 0) {
		btd_phone_page_request(index, argument + 5);
		return;
	}

	/* READ handle=...: a message marked read on the phone. */
	same = strncmp(argument, "READ ", 5U);
	if (same == 0) {
		btd_phone_read_request(index, argument + 5);
		return;
	}

	/* DROP is root's alone. */
	same = strncmp(argument, "DROP ", 5U);
	if (same != 0) {
		btd_write(client, "ERROR request\nDONE\n");
		return;
	}

	/* Anyone but root is refused. */
	if (client->uid != 0) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* The address. */
	rest = argument + 5;
	length = strlen(rest);
	if (length != 17U) {
		btd_write(client, "ERROR address\nDONE\n");
		return;
	}

	/* Its seventeen characters, read. */
	memcpy(address_text, rest, 17U);
	address_text[17] = '\0';
	error = btd_address_parse(address_text, address);
	if (error != 0) {
		btd_write(client, "ERROR address\nDONE\n");
		return;
	}

	/* A ready controller. */
	if (!btd_session_open || btd_session.state != BTD_STATE_READY) {
		btd_write(client, "ERROR not-ready\nDONE\n");
		return;
	}

	/* The link ends (its end comes with the link's Disconnection Complete). */
	error = btd_phone_drop(&btd_phone_link, address);
	if (error != 0) {
		btd_write(client, "ERROR not-connected\nDONE\n");
		return;
	}

	/* Succeeded: asked. */
	btd_write(client, "DONE\n");
}

/* Answers PHONE SHOW (ws197-p003 section 9.2): the phone's line as the phone link lets the client see it, then DONE. */
static void
btd_phone_show_request(
	struct btd_client *client)
{
	char line[BTD_PHONEIO_OUT_MAX];
	int permitted;
	int error;

	/* What the client may see. */
	permitted = btd_permitted(client->uid);
	error = btd_phone_line(client->uid, permitted, "PHONE ", line, sizeof(line));
	if (error != 0) {
		btd_write(client, "DONE\n");
		return;
	}

	/* Succeeded: the line. */
	btd_client_raw(client, line, NULL, 0U);
	btd_write(client, "DONE\n");
}

/*
 * Answers PHONE LINK ADDRESS on|off [profiles=m,c,h] (ws197-p003 section
 * 3.2): the phone link of a phone with a valid record turned on or off by
 * its owner or root.
 */
static void
btd_phone_link_request(
	struct btd_client *client,
	const char *argument)
{
	char address_text[18];
	uint8_t address[BTD_ADDRESS_BYTES];
	const char *rest;
	const char *letters;
	const char *switched;
	size_t length;
	int profiles;
	int on_text;
	int off_text;
	int on;
	int same;
	int error;

	/* The address and a space after it. */
	length = strlen(argument);
	if (length < 18U || argument[17] != ' ') {
		btd_write(client, "ERROR address\nDONE\n");
		return;
	}

	/* Its seventeen characters, read. */
	memcpy(address_text, argument, 17U);
	address_text[17] = '\0';
	error = btd_address_parse(address_text, address);
	if (error != 0) {
		btd_write(client, "ERROR address\nDONE\n");
		return;
	}

	/* on, or off, or anything else refused. */
	rest = argument + 18;
	on_text = strncmp(rest, "on", 2U);
	off_text = strncmp(rest, "off", 3U);
	if (on_text == 0) {
		on = 1;
		rest += 2;
	} else if (off_text == 0) {
		on = 0;
		rest += 3;
	} else {
		btd_write(client, "ERROR argument\nDONE\n");
		return;
	}

	/* The profiles, "profiles=" and the letters m, c and h with commas (or none, kept as they are). */
	profiles = -1;
	same = strncmp(rest, " profiles=", 10U);
	if (same == 0) {
		profiles = 0;
		for (letters = rest + 10; *letters != '\0'; letters++) {
			if (*letters == 'm') {
				profiles |= (int)BTD_PHONEREC_MESSAGES;
			} else if (*letters == 'c') {
				profiles |= (int)BTD_PHONEREC_CONTACTS;
			} else if (*letters == 'h') {
				profiles |= (int)BTD_PHONEREC_CALLS;
			} else if (*letters != ',') {
				btd_write(client, "ERROR argument\nDONE\n");
				return;
			}
		}
	} else if (*rest != '\0') {
		btd_write(client, "ERROR argument\nDONE\n");
		return;
	}

	/* The switch, as the phone link allows it. */
	error = btd_phone_link_set(&btd_phone_link, address, client->uid, on, profiles);
	if (error == ENOENT) {
		btd_write(client, "ERROR not-phone\nDONE\n");
		return;
	} else if (error == EPERM) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	} else if (error != 0) {
		btd_write(client, "ERROR %s\nDONE\n", strerror(error));
		return;
	}

	/* Succeeded: switched, and logged. */
	switched = "off";
	if (on)
		switched = "on";
	btd_log("bluetoothd: phone link of %s %s by uid %u\n", address_text, switched, (unsigned)client->uid);
	btd_write(client, "DONE\n");

	/* MAP and PBAP follow the switch and the profiles (stopped, started, or a failed one tried again at once). */
	btd_map_check(&btd_messages);
	btd_pbap_check(&btd_contacts);
}

/* Gives the name of a uid's account (the phone link's hook).  Returns 0 or ENOENT. */
static int
btd_account(
	void *context,
	uid_t uid,
	char *name,
	size_t size)
{
	struct passwd *account;

	UNUSED_PARAMETER(context);

	/* The account. */
	account = getpwuid(uid);
	if (account == NULL)
		return ENOENT;

	/* Succeeded: its name. */
	(void)snprintf(name, size, "%s", account->pw_name);
	return 0;
}

/* Tells whether a uid is the seat's user: the display's owner, not the greeter. */
static int
btd_seated(
	uid_t uid)
{
	struct passwd *account;
	struct stat status;
	int same;
	int error;

	/* An account the system knows, and not the greeter's. */
	account = getpwuid(uid);
	if (account == NULL)
		return 0;
	same = strcmp(account->pw_name, BTD_GREETER);
	if (same == 0)
		return 0;

	/* The display's owner. */
	error = stat(BTD_SEAT_NODE, &status);
	if (error != 0)
		return 0;
	if (status.st_uid != uid)
		return 0;

	/* The seat's user. */
	return 1;
}

/*
 * Tells whether a uid may change things (D8): root, the seat's user (the
 * display's owner, not the greeter), or a member of wheel; the greeter
 * never.
 */
static int
btd_permitted(
	uid_t uid)
{
	struct passwd *account;
	struct group *admin;
	struct stat status;
	gid_t groups[BTD_GROUPS_MAX];
	gid_t admin_gid;
	int group_count;
	int index;
	int same;
	int error;

	/* Root. */
	if (uid == 0)
		return 1;

	/* An account the system knows, and not the greeter's. */
	account = getpwuid(uid);
	if (account == NULL)
		return 0;
	same = strcmp(account->pw_name, BTD_GREETER);
	if (same == 0)
		return 0;

	/* The seat's user. */
	error = stat(BTD_SEAT_NODE, &status);
	if (error == 0 && status.st_uid == uid)
		return 1;

	/* wheel's gid; without the group nobody is a member. */
	admin = getgrnam(BTD_ADMIN_GROUP);
	if (admin == NULL)
		return 0;
	admin_gid = admin->gr_gid;

	/* The account's groups (its own and the supplementary ones). */
	memset(groups, 0, sizeof(groups));
	group_count = BTD_GROUPS_MAX;
	(void)getgrouplist(account->pw_name, account->pw_gid, groups, &group_count);
	if (group_count > BTD_GROUPS_MAX)
		group_count = BTD_GROUPS_MAX;

	/* A member of wheel. */
	for (index = 0; index < group_count; index++) {
		if (groups[index] == admin_gid)
			return 1;
	}

	/* Anyone else. */
	return 0;
}

/* Reads "ADDRESS TYPE" (TYPE bredr, le-public or le-random). Returns 0 or EINVAL. */
static int
btd_parse_device(
	const char *text,
	uint8_t *address,
	unsigned *type)
{
	char address_text[18];
	const char *space;
	int error;

	/* The address, then one space, then the type. */
	space = strchr(text, ' ');
	if (space == NULL || space - text != 17)
		return EINVAL;
	memcpy(address_text, text, 17U);
	address_text[17] = '\0';
	error = btd_address_parse(address_text, address);
	if (error != 0)
		return EINVAL;
	error = btd_address_type_parse(space + 1, type);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the device. */
	return 0;
}

/*
 * Reads PAIR's argument: the device (ADDRESS TYPE) and the one option
 * there is, phone=1 (ws197-p002 section 7.1).  Returns 0, or EINVAL.
 */
static int
btd_parse_pair(
	const char *text,
	uint8_t *address,
	unsigned *type,
	int *phone)
{
	char device[40];
	const char *option;
	size_t length;
	int same;
	int error;

	/* The device's part: the text up to a space after the type, or all of it. */
	*phone = 0;
	option = NULL;
	length = strlen(text);
	if (length > 18U)
		option = strchr(text + 18, ' ');
	if (option != NULL)
		length = (size_t)(option - text);
	if (length >= sizeof(device))
		return EINVAL;
	memcpy(device, text, length);
	device[length] = '\0';

	/* The address and the type. */
	error = btd_parse_device(device, address, type);
	if (error != 0)
		return EINVAL;

	/* No option: an ordinary pairing. */
	if (option == NULL)
		return 0;

	/* The option, which must be the phone's. */
	same = strcmp(option + 1, "phone=1");
	if (same != 0)
		return EINVAL;
	*phone = 1;

	/* Succeeded: the device, for a phone. */
	return 0;
}

/*
 * Writes a formatted text to a client: added to its queue and sent as far
 * as it takes it now, never waiting (section 4.1); a client whose queue
 * would pass its limit is closed at the end of the round.
 */
static void
btd_write(
	struct btd_client *client,
	const char *format,
	...)
{
	char text[BTD_LINE_MAX + 4U * BTD_NAME_MAX];
	va_list arguments;
	size_t length;
	int written;
	int error;

	/* A client already closed, or that stopped reading. */
	if (client->descriptor < 0 || client->dead)
		return;

	/* The text. */
	va_start(arguments, format);
	written = vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	if (written < 0)
		return;
	length = (size_t)written;
	if (length >= sizeof(text))
		length = sizeof(text) - 1U;

	/* Queued; a client that does not read enough is closed at the end of the round. */
	error = btd_outq_append(&client->output, text, length);
	if (error != 0) {
		client->dead = 1;
		return;
	}

	/* Succeeded: sent as far as it goes now. */
	btd_client_flush((int)(client - btd_clients));
}

/* Sends a client's queue as far as it takes it now; a client that is gone is closed at the end of the round. */
static void
btd_client_flush(
	int index)
{
	struct btd_client *client;
	int error;

	/* A client there. */
	client = &btd_clients[index];
	if (client->descriptor < 0)
		return;

	/* Succeeded: sent, or marked to close. */
	error = btd_outq_flush(&client->output, btd_client_send, client);
	if (error != 0)
		client->dead = 1;
}

/* The queue's send: one send on the client's socket, never waiting. */
static ssize_t
btd_client_send(
	void *context,
	const uint8_t *data,
	size_t length)
{
	struct btd_client *client;
	ssize_t sent;

	/* The client. */
	client = context;

	/* Succeeded: what went, or -1 with errno. */
	sent = send(client->descriptor, data, length, 0);
	return sent;
}

/*
 * Closes a client and frees its slot: a pairing it asked for stops, an
 * agent's question is answered no.
 */
static void
btd_client_close(
	int index)
{
	struct btd_client *client;

	/* What is queued is sent once more if it can be (an ERROR before a close), then the queue and a text being read go. */
	client = &btd_clients[index];
	if (client->descriptor >= 0)
		(void)btd_outq_flush(&client->output, btd_client_send, client);
	btd_outq_clear(&client->output);
	btd_phoneio_input_clear(&client->input);

	/* The descriptor, and the slot. */
	if (client->descriptor >= 0)
		(void)close(client->descriptor);
	client->descriptor = -1;
	client->dead = 0;
	client->waits_scan = 0;
	client->waits_pair = 0;
	client->waits_connect = 0;

	/* Its phone request is forgotten (an answer still to come reaches nobody), and it subscribes no more. */
	if (client->waits_phone) {
		btd_map_cancel(&btd_messages, btd_token(index));
		btd_pbap_cancel(&btd_contacts, btd_token(index));
	}

	/* It waits for nothing and hears nothing more. */
	client->waits_phone = 0;
	client->subscribed = 0;
	client->sub_dropped = 0;

	/* The agent is gone; a question it was asked is no, and it hears no ASK-END. */
	if (btd_agent_client == index)
		btd_agent_client = BTD_NO_CLIENT;
	if (btd_question_client == index)
		btd_question_client = BTD_NO_CLIENT;
	if (btd_asked_client == index) {
		btd_asked_client = BTD_NO_CLIENT;
		btd_pair_answer(&btd_pairing, 0);
	}

	/* The client that asked for a pairing is gone: the pairing stops (its end has nobody to tell). */
	if (btd_pair_client == index) {
		btd_pair_client = BTD_NO_CLIENT;
		btd_pair_stop(&btd_pairing, "cancelled");
	}
}

/* Calls an ioctl of the controller's node (the session's control). */
static int
btd_node_control(
	void *context,
	unsigned long request,
	void *argument)
{
	struct btd_session *session;
	int status;

	/* The session's node. */
	session = context;
	status = ioctl(session->descriptor, request, argument);
	if (status != 0)
		return errno;

	/* Succeeded. */
	return 0;
}

/* Fills bytes with the system's random ones (the Security Manager's nonces and passkeys). */
static void
btd_random(
	void *context,
	uint8_t *bytes,
	size_t length)
{
	UNUSED_PARAMETER(context);

	/* The kernel's random source. */
	arc4random_buf(bytes, length);
}

/*
 * Writes a controller's key: its USB vendor and product and its port,
 * "VVVV:PPPP usbB/portN", without the USB address, which a re-enumeration
 * changes.  An empty key when the path is not of that form.
 */
static void
btd_key(
	const struct bt_info *info,
	char *key,
	size_t size)
{
	const char *device;
	size_t length;

	/* The path up to its device part ("usbB/portN/deviceD/interfaceI"). */
	key[0] = '\0';
	device = strstr(info->physical_path, "/device");
	if (device == NULL)
		return;
	length = (size_t)(device - info->physical_path);

	/* The key. */
	(void)snprintf(key, size, "%04x:%04x %.*s", (unsigned)info->vendor, (unsigned)info->product, (int)length, info->physical_path);
}

/* Tells whether a controller's firmware was loaded before. */
static int
btd_loaded(
	const char *key)
{
	unsigned index;
	int same;

	/* Each controller remembered. */
	for (index = 0U; index < btd_load_count; index++) {
		same = strcmp(btd_loads[index], key);
		if (same == 0)
			return 1;
	}

	/* Never loaded. */
	return 0;
}

/* Remembers that a controller's firmware was loaded (the oldest goes when the table is full). */
static void
btd_remember_load(
	const char *key)
{
	int loaded;

	/* Once. */
	loaded = btd_loaded(key);
	if (loaded)
		return;

	/* A full table drops its oldest. */
	if (btd_load_count >= BTD_LOADS_MAX) {
		memmove(btd_loads[0], btd_loads[1], sizeof(btd_loads[0]) * (BTD_LOADS_MAX - 1U));
		btd_load_count--;
	}

	/* At the end. */
	(void)snprintf(btd_loads[btd_load_count], sizeof(btd_loads[0]), "%s", key);
	btd_load_count++;
}

/*
 * Connects a bonded HID device (CONNECT ADDRESS TYPE, ws143-p005): the
 * client waits for the connection's end (CONNECTED or ERROR), unless it is
 * answered at once.  BR/EDR only for now (LE is i03's).
 */
static void
btd_connect(
	int index,
	const char *argument)
{
	struct btd_client *client;
	char answer[BTD_HID_ANSWER_MAX];
	uint8_t address[BTD_ADDRESS_BYTES];
	unsigned type;
	int permitted;
	int answered;
	int error;

	/* Only those D8 permits. */
	client = &btd_clients[index];
	permitted = btd_permitted(client->uid);
	if (!permitted) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* The device: BR/EDR's or LE's. */
	error = btd_parse_device(argument, address, &type);
	if (error != 0) {
		btd_write(client, "ERROR address\nDONE\n");
		return;
	}

	/* A ready controller, which the user has not turned off. */
	if (!btd_session_open || btd_session.state != BTD_STATE_READY) {
		btd_write(client, "ERROR not-ready\nDONE\n");
		return;
	}

	/* None while the user has Bluetooth off. */
	if (btd_powered_off) {
		btd_write(client, "ERROR off\nDONE\n");
		return;
	}

	/* The connection: answered now, or the client waits for its end. */
	btd_hid_holding();
	answered = btd_hid_connect(&btd_hid_host, address, type, answer, sizeof(answer));
	if (answered) {
		btd_write(client, "%s\nDONE\n", answer);
		return;
	}

	/* Succeeded: the client waits. */
	client->waits_connect = 1;
	memcpy(client->connect_address, address, BTD_ADDRESS_BYTES);
	btd_log("bluetoothd: connecting %s asked by uid %u\n", argument, (unsigned)client->uid);
}

/* Disconnects a HID device (DISCONNECT ADDRESS TYPE, ws143-p005); it is not wanted back until connected again. */
static void
btd_disconnect(
	struct btd_client *client,
	const char *argument)
{
	uint8_t address[BTD_ADDRESS_BYTES];
	unsigned type;
	int permitted;
	int error;

	/* Only those D8 permits. */
	permitted = btd_permitted(client->uid);
	if (!permitted) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* The device. */
	error = btd_parse_device(argument, address, &type);
	if (error != 0) {
		btd_write(client, "ERROR address\nDONE\n");
		return;
	}

	/* Its disconnection. */
	error = btd_hid_disconnect(&btd_hid_host, address, type);
	if (error != 0) {
		btd_write(client, "ERROR not-connected\nDONE\n");
		return;
	}

	/* Succeeded: disconnecting. */
	btd_log("bluetoothd: disconnecting %s\n", argument);
	btd_write(client, "DONE\n");
}

/* Answers STATUS: a HID line for each device of the HID host. */
static void
btd_status(
	struct btd_client *client)
{
	char line[512];
	unsigned index;

	/* Each device's line. */
	for (index = 0U; index < BTD_HID_MAX; index++) {
		btd_hid_status(&btd_hid_host, index, line, sizeof(line));
		if (line[0] != '\0')
			btd_write(client, "%s\n", line);
	}

	/* Succeeded: the end. */
	btd_write(client, "DONE\n");
}

/* Opens /dev/input/bridge for the HID host, through the privileged parent. */
static int
btd_hid_bridge(
	void *context,
	int *descriptor)
{
	int error;

	UNUSED_PARAMETER(context);

	/* The parent's open. */
	error = btd_privsep_open_bridge(&btd_separation, descriptor);
	if (error != 0)
		return error;

	/* Succeeded: the open. */
	return 0;
}

/* Tells the client waiting for a device's connection its end (the HID host's answer hook). */
static void
btd_hid_told(
	void *context,
	const uint8_t *address,
	const char *line)
{
	struct btd_client *client;
	unsigned index;
	int same;

	UNUSED_PARAMETER(context);

	/* Logged. */
	btd_log("bluetoothd: hid: %s\n", line);

	/* Each client waiting for that device. */
	for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
		client = &btd_clients[index];
		if (client->descriptor < 0 || !client->waits_connect)
			continue;
		same = memcmp(client->connect_address, address, BTD_ADDRESS_BYTES);
		if (same != 0)
			continue;

		/* The end. */
		client->waits_connect = 0;
		btd_write(client, "%s\nDONE\n", line);
	}
}

/* Holds the HID host's pages while a pairing or a scan runs (review B7). */
static void
btd_hid_holding(
	void)
{
	int pairing;
	int held;

	/* A pairing, a scan, or Bluetooth turned off (no page, no auto-connect, ws143-p006's note). */
	pairing = btd_pair_active(&btd_pairing);
	held = 0;
	if (pairing || btd_session.scanning || btd_powered_off)
		held = 1;

	/* Succeeded: told. */
	btd_hid_hold(&btd_hid_host, held);
}

/* Records one H4 packet of the session in the btsnoop file (the session's trace). */
static void
btd_trace(
	void *context,
	const uint8_t *packet,
	size_t length,
	int received)
{
	uint16_t handle;
	size_t copied;
	int hidden;

	UNUSED_PARAMETER(context);

	/* A packet with its type's byte, while the record is open. */
	if (btd_snoop < 0 || length < 1U)
		return;

	/* A phone's ACL data is hidden: its link's, and its pairing's (ws197-p002 section 11). */
	hidden = 0;
	if (packet[0] == BT_PACKET_ACL && length >= 3U) {
		handle = (uint16_t)(((unsigned)packet[1] | ((unsigned)packet[2] << 8)) & 0x0fffU);
		hidden = btd_phone_owns(&btd_phone_link, handle);
		if (btd_pairing.phone &&
		    btd_pairing.connected &&
		    btd_pairing.handle == handle)
			hidden = 1;
	}

	/* The phone's: its headers and zeros. */
	if (hidden) {
		copied = btd_snoop_hide(packet, length, btd_trace_copy, sizeof(btd_trace_copy));
		btd_snoop_write(btd_snoop, btd_trace_copy[0], received, btd_trace_copy + 1, copied - 1U);
		return;
	}

	/* Succeeded: appended. */
	btd_snoop_write(btd_snoop, packet[0], received, packet + 1, length - 1U);
}

/*
 * Reads the arguments: -f NODE (the node of its own) and -s FILE (the
 * btsnoop record), each once at most.  Returns 0, or EINVAL for anything
 * else.
 */
static int
btd_arguments(
	int argc,
	char **argv)
{
	int index;
	int is_node;
	int is_snoop;

	/* Each option and its value. */
	for (index = 1; index < argc; index += 2) {
		if (index + 1 >= argc)
			return EINVAL;
		is_node = strcmp(argv[index], "-f");
		is_snoop = strcmp(argv[index], "-s");
		if (is_node == 0 && btd_node == NULL) {
			btd_node = argv[index + 1];
		} else if (is_snoop == 0 && btd_snoop_path == NULL) {
			btd_snoop_path = argv[index + 1];
		} else {
			return EINVAL;
		}
	}

	/* Succeeded: understood. */
	return 0;
}

/* Opens /dev/system for its power events (the child's own open: the node is everyone's to read). */
static void
btd_system_open(
	void)
{
	struct system_event_subscription subscription;
	int status;

	/* The node, without waiting. */
	btd_system = open("/dev/system", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (btd_system < 0) {
		btd_log("bluetoothd: /dev/system: %s (no check after a sleep)\n", strerror(errno));
		return;
	}

	/* Succeeded: the power events only. */
	memset(&subscription, 0, sizeof(subscription));
	subscription.classes = KERN_SYSTEM_EVENT_POWER;
	status = ioctl(btd_system, KERN_SYSTEM_EVENT_SUBSCRIBE, &subscription);
	if (status != 0) {
		btd_log("bluetoothd: /dev/system: %s (no check after a sleep)\n", strerror(errno));
		(void)close(btd_system);
		btd_system = -1;
	}
}

/* Reads the waiting system events; the end of a sleep makes the HID host check its links (review S5). */
static void
btd_system_events(
	void)
{
	struct system_event event;
	ssize_t got;
	int same;

	/* Each whole record until none waits. */
	for (;;) {
		got = read(btd_system, &event, sizeof(event));
		if (got != (ssize_t)sizeof(event))
			return;
		event.subject[sizeof(event.subject) - 1U] = '\0';
		same = strcmp(event.subject, "sleep.end");
		if (same != 0)
			continue;

		/* A sleep ended: the links are checked and the phone paged again at once, while there is a controller (ws197-p003 section 5.6). */
		btd_log("bluetoothd: sleep.end, the HID links checked\n");
		if (btd_session_open && btd_session.state == BTD_STATE_READY) {
			btd_hid_resume(&btd_hid_host);
			btd_phone_resume(&btd_phone_link, btd_now_ms());
		}
	}
}

/*
 * Tells the phone link who sits at the seat (ws197-p003 section 3.3): the
 * display's owner unless it is the greeter, and nobody while the user
 * turned Bluetooth off.  Looked at again every BTD_SEAT_CHECK_MS.
 */
static void
btd_seat_check(
	uint64_t now)
{
	struct passwd *account;
	struct stat status;
	int same;
	int error;

	/* The next look. */
	btd_seat_check_at = now + BTD_SEAT_CHECK_MS;

	/* Nobody while Bluetooth is off. */
	if (btd_powered_off) {
		btd_phone_set_seat(&btd_phone_link, 0, 0, now);
		return;
	}

	/* The display's owner. */
	error = stat(BTD_SEAT_NODE, &status);
	if (error != 0) {
		btd_phone_set_seat(&btd_phone_link, 0, 0, now);
		return;
	}

	/* An account the system knows, and not the greeter's. */
	account = getpwuid(status.st_uid);
	same = 1;
	if (account != NULL)
		same = strcmp(account->pw_name, BTD_GREETER);
	if (account == NULL || same == 0) {
		btd_phone_set_seat(&btd_phone_link, 0, 0, now);
		return;
	}

	/* Succeeded: the seat's user. */
	btd_phone_set_seat(&btd_phone_link, 1, status.st_uid, now);
}

/* Keeps that a uid's PAIR made the BR/EDR bond of an address with an authenticated key (BUG-287); the oldest makes room. */
static void
btd_own_bond_keep(
	const uint8_t *address,
	uid_t uid)
{
	unsigned index;
	unsigned look;
	int found;
	int same;

	/* The address's entry there already. */
	found = 0;
	index = 0U;
	for (look = 0U; look < BTD_OWN_BONDS; look++) {
		same = memcmp(btd_own_bonds[look].address, address, BTD_ADDRESS_BYTES);
		if (btd_own_bonds[look].used && same == 0) {
			index = look;
			found = 1;
			break;
		}
	}

	/* Else the next one, the oldest. */
	if (!found) {
		index = btd_own_next;
		btd_own_next = (btd_own_next + 1U) % BTD_OWN_BONDS;
	}

	/* Succeeded: kept. */
	btd_own_bonds[index].used = 1;
	memcpy(btd_own_bonds[index].address, address, BTD_ADDRESS_BYTES);
	btd_own_bonds[index].uid = uid;
}

/* Tells whether the uid's PAIR made the address's BR/EDR bond in this run (BUG-287). */
static int
btd_own_bond_find(
	const uint8_t *address,
	uid_t uid)
{
	unsigned index;
	int same;

	/* Each entry. */
	for (index = 0U; index < BTD_OWN_BONDS; index++) {
		if (!btd_own_bonds[index].used || btd_own_bonds[index].uid != uid)
			continue;
		same = memcmp(btd_own_bonds[index].address, address, BTD_ADDRESS_BYTES);
		if (same == 0)
			return 1;
	}

	/* None. */
	return 0;
}

/* Forgets who made the address's BR/EDR bond (the bond is forgotten, BUG-287). */
static void
btd_own_bond_forget(
	const uint8_t *address)
{
	unsigned index;
	int same;

	/* Each entry of the address. */
	for (index = 0U; index < BTD_OWN_BONDS; index++) {
		same = memcmp(btd_own_bonds[index].address, address, BTD_ADDRESS_BYTES);
		if (same == 0)
			memset(&btd_own_bonds[index], 0, sizeof(btd_own_bonds[index]));
	}
}

/*
 * Gives a client's token for MAP's answers (ws197-p003 section 4.2): its
 * generation and its slot, so an answer for a client that went never
 * reaches the next one in the slot.
 */
static uint64_t
btd_token(
	int index)
{
	uint64_t token;

	/* The generation above the slot (the slot counted from 1, so no token is 0). */
	token = ((uint64_t)btd_clients[index].generation << 8) | (uint64_t)(index + 1);

	/* The token. */
	return token;
}

/* Finds the client a token names, or NULL when it went (or stopped reading). */
static struct btd_client *
btd_token_client(
	uint64_t token)
{
	struct btd_client *client;
	uint64_t slot;

	/* The slot. */
	slot = token & 0xffU;
	if (slot == 0U || slot > BTD_CLIENTS_MAX)
		return NULL;
	client = &btd_clients[slot - 1U];

	/* The same client: open, reading, of the same generation. */
	if (client->descriptor < 0 || client->dead)
		return NULL;
	if (client->generation != (uint32_t)(token >> 8))
		return NULL;

	/* Succeeded: the client. */
	return client;
}

/*
 * Tells whether a uid may use the phone's messages (ws197-p004 section
 * 5.4): root, and the owner of the valid record.
 */
static int
btd_phone_allowed(
	uid_t uid)
{
	struct btd_phoneio_owner owner;
	int allowed;

	/* The owner now, and the rule. */
	btd_phone_owner(&owner);
	allowed = btd_phoneio_allowed(&owner, uid);
	return allowed;
}

/* Gives the phone's owner as the socket's rules see it: the uid of a valid record, or none. */
static void
btd_phone_owner(
	struct btd_phoneio_owner *owner)
{
	/* None, unless a valid record names one. */
	owner->have_owner = 0;
	owner->owner = 0;
	if (btd_phone_link.have_record && btd_phone_link.record_valid) {
		owner->have_owner = 1;
		owner->owner = btd_phone_link.record.uid;
	}
}

/*
 * Writes a line (its newline added) and bytes after it to a client's
 * queue, as long as the line and the bytes are (no formatting); a client
 * whose queue would pass its limit is closed at the end of the round,
 * never here.
 */
static void
btd_client_raw(
	struct btd_client *client,
	const char *line,
	const uint8_t *bytes,
	size_t length)
{
	int error;

	/* A client already closed, or that stopped reading. */
	if (client->descriptor < 0 || client->dead)
		return;

	/* The line and its newline. */
	error = btd_outq_append(&client->output, line, strlen(line));
	if (error == 0)
		error = btd_outq_append(&client->output, "\n", 1U);

	/* The bytes after it. */
	if (error == 0 && length != 0U)
		error = btd_outq_append(&client->output, bytes, length);

	/* A client that does not read enough goes at the end of the round. */
	if (error != 0) {
		client->dead = 1;
		return;
	}

	/* Succeeded: sent as far as it goes now. */
	btd_client_flush((int)(client - btd_clients));
}

/*
 * Writes an event to a subscriber (ws197-p003 section 9.4): past
 * BTD_SUBSCRIBE_DROP queued, the event is dropped and the subscriber told
 * PHONE DROPPED once its queue is short again (btd_phone_watch).
 */
static void
btd_subscriber_write(
	struct btd_client *client,
	const char *line,
	const uint8_t *bytes,
	size_t length)
{
	size_t pending;
	size_t needed;

	/* Dropping already: this one too. */
	if (client->sub_dropped)
		return;

	/* Room for it under the mark, else dropped. */
	pending = btd_outq_pending(&client->output);
	needed = strlen(line) + 1U + length;
	if (pending + needed > BTD_SUBSCRIBE_DROP) {
		client->sub_dropped = 1;
		return;
	}

	/* Succeeded: written. */
	btd_client_raw(client, line, bytes, length);
}

/*
 * Writes the phone's line for a uid after prefix (PHONE SHOW's "PHONE ",
 * PHONE STATE's "PHONE STATE "): the phone link's part, then for those who
 * see it whole MAP's part and why the link or MAP stopped.  Returns 0, or
 * ENOENT when there is no record or nothing to show to that uid.
 */
static int
btd_phone_line(
	uid_t uid,
	int permitted,
	const char *prefix,
	char *line,
	size_t size)
{
	char phone[BTD_PHONEIO_OUT_MAX];
	char messages[96];
	char contacts[96];
	char both[200];
	const char *whole;
	const char *own_why;
	int error;

	/* The phone link's part ("PHONE address=..."). */
	error = btd_phone_show(&btd_phone_link, uid, permitted, phone, sizeof(phone));
	if (error != 0)
		return error;

	/* Seen in part: as it is. */
	whole = strstr(phone, " link=");
	if (whole == NULL) {
		(void)snprintf(line, size, "%s%s", prefix, phone + strlen("PHONE "));
		return 0;
	}

	/* MAP's part. */
	error = btd_map_state_text(&btd_messages, messages, sizeof(messages));
	if (error != 0)
		messages[0] = '\0';

	/* PBAP's part after it (ws197-p005 section 5.4). */
	error = btd_pbap_state_text(&btd_contacts, contacts, sizeof(contacts));
	if (error != 0)
		contacts[0] = '\0';
	(void)snprintf(both, sizeof(both), "%s %s", messages, contacts);

	/* Why the link stopped, when MAP says no why of its own. */
	own_why = strstr(messages, " why=");
	if (btd_phone_link.why != NULL &&
	    btd_phone_link.state != BTD_PHONE_READY &&
	    own_why == NULL) {
		(void)snprintf(line, size, "%s%s %s why=%s", prefix, phone + strlen("PHONE "), both, btd_phone_link.why);
		return 0;
	}

	/* Succeeded: the whole line. */
	(void)snprintf(line, size, "%s%s %s", prefix, phone + strlen("PHONE "), both);
	return 0;
}

/*
 * Keeps the phone's subscribers right, at the end of each round
 * (ws197-p004 section 5.4): when the owner changed, the subscribers and
 * waiting requests of anyone else end and MAP looks again whether it is
 * wanted; the pages that waited for room go on; a subscriber whose queue
 * is short again hears PHONE DROPPED; and a changed state is told.
 */
static void
btd_phone_watch(void)
{
	static char line[BTD_PHONEIO_OUT_MAX];
	struct btd_phoneio_client clients[BTD_CLIENTS_MAX];
	struct btd_phoneio_owner owner;
	struct btd_client *client;
	int closes[BTD_CLIENTS_MAX];
	unsigned index;
	unsigned subscribers;
	size_t pending;
	int changed;
	int same;
	int error;

	/* The owner now. */
	btd_phone_owner(&owner);
	changed = btd_phoneio_owner_changed(&btd_owner_known, &owner);

	/* A new owner, or none: the others' subscribers and waiting clients close, MAP looks again. */
	if (changed) {
		btd_owner_known = owner;
		for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
			clients[index].open = 0;
			if (btd_clients[index].descriptor >= 0 && !btd_clients[index].dead)
				clients[index].open = 1;
			clients[index].uid = btd_clients[index].uid;
			clients[index].subscribed = btd_clients[index].subscribed;
			clients[index].waits_phone = btd_clients[index].waits_phone;
		}

		/* Those that may no longer use the phone. */
		(void)btd_phoneio_to_close(&owner, clients, BTD_CLIENTS_MAX, closes);

		/*
		 * Each one marked closes at the end of the round; its request is
		 * forgotten first (closed, not refused: a SEND already pushed must
		 * read as not known, never as a refusal that invites a second send,
		 * ws197-p004 review-2 m4).
		 */
		for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
			if (!closes[index])
				continue;
			client = &btd_clients[index];
			if (client->waits_phone) {
				btd_map_cancel(&btd_messages, btd_token((int)index));
				btd_pbap_cancel(&btd_contacts, btd_token((int)index));
			}

			/* Closed at the round's end. */
			client->waits_phone = 0;
			client->dead = 1;
		}

		/* MAP and PBAP look again whether they are wanted. */
		btd_map_check(&btd_messages);
		btd_pbap_check(&btd_contacts);
	}

	/* The pages waiting for their clients' room. */
	btd_map_pump(&btd_messages);
	btd_pbap_pump(&btd_contacts);

	/* Each subscriber whose queue is short again hears what it lost. */
	subscribers = 0U;
	for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
		client = &btd_clients[index];
		if (client->descriptor < 0 || !client->subscribed || client->dead)
			continue;
		subscribers++;
		pending = btd_outq_pending(&client->output);
		if (client->sub_dropped && pending <= BTD_SUBSCRIBE_RESUME) {
			client->sub_dropped = 0;
			btd_client_raw(client, "PHONE DROPPED", NULL, 0U);
		}
	}

	/* The state as root sees it, told when it changed. */
	if (subscribers == 0U)
		return;
	error = btd_phone_line(0, 1, "PHONE STATE ", line, sizeof(line));
	if (error != 0)
		return;
	same = strcmp(line, btd_phone_told);
	if (same == 0)
		return;
	(void)snprintf(btd_phone_told, sizeof(btd_phone_told), "%s", line);

	/* Succeeded: told to each subscriber allowed. */
	btd_messages_emit(NULL, line, NULL, 0U);
}

/*
 * Answers PHONE SUBSCRIBE (ws197-p004 section 5.1): the owner and root,
 * two at a time; DONE, then the state now, then events alone.
 */
static void
btd_phone_subscribe_request(
	int index)
{
	char line[BTD_PHONEIO_OUT_MAX];
	struct btd_phoneio_owner owner;
	struct btd_client *client;
	unsigned count;
	unsigned slot;
	int answer;
	int error;

	/* The subscribers now. */
	client = &btd_clients[index];
	count = 0U;
	for (slot = 0U; slot < BTD_CLIENTS_MAX; slot++) {
		if (btd_clients[slot].descriptor >= 0 && btd_clients[slot].subscribed)
			count++;
	}

	/* The owner and root while there is a record, two at a time. */
	btd_phone_owner(&owner);
	answer = btd_phoneio_subscribe(&owner, client->uid, btd_phone_link.have_record, count, BTD_SUBSCRIBERS_MAX);
	if (answer == BTD_PHONEIO_SUBSCRIBE_PERMISSION) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	} else if (answer == BTD_PHONEIO_SUBSCRIBE_BUSY) {
		btd_write(client, "ERROR busy\nDONE\n");
		return;
	}

	/* Subscribed: DONE, then the state now. */
	client->subscribed = 1;
	client->sub_dropped = 0;
	btd_write(client, "DONE\n");
	error = btd_phone_line(0, 1, "PHONE STATE ", line, sizeof(line));
	if (error == 0)
		btd_client_raw(client, line, NULL, 0U);
}

/*
 * Starts PHONE PAGE messages since=N [limit=N] [cursor=C] count=N
 * (ws197-p004 section 5.1): the owner's and root's, answered by MAP.
 */
static void
btd_phone_page_request(
	int index,
	const char *argument)
{
	struct btd_client *client;
	char key[BTD_PHONEIO_KEY_MAX];
	char value[BTD_PHONEIO_VALUE_MAX];
	char cursor_text[BTD_PHONEIO_VALUE_MAX];
	const char *cursor;
	const char *refused;
	unsigned long long number;
	int64_t since;
	unsigned limit;
	unsigned count;
	unsigned what;
	int have_since;
	int have_limit;
	int allowed;
	int error;
	int got;
	int same;

	/* The owner and root. */
	client = &btd_clients[index];
	allowed = btd_phone_allowed(client->uid);
	if (!allowed) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* What is read: messages (MAP), contacts or calls (PBAP, ws197-p005 section 5.2). */
	what = 0U;
	cursor = argument;
	same = strncmp(argument, "messages", 8U);
	if (same == 0 && (argument[8] == ' ' || argument[8] == '\0'))
		cursor = argument + 8;
	same = strncmp(argument, "contacts", 8U);
	if (same == 0 && (argument[8] == ' ' || argument[8] == '\0')) {
		what = BTD_PBAP_WHAT_CONTACTS;
		cursor = argument + 8;
	}

	/* The calls. */
	same = strncmp(argument, "calls", 5U);
	if (same == 0 && (argument[5] == ' ' || argument[5] == '\0')) {
		what = BTD_PBAP_WHAT_CALLS;
		cursor = argument + 5;
	}

	/* None of them. */
	if (cursor == argument) {
		btd_write(client, "ERROR argument\nDONE\n");
		return;
	}

	/* Each argument. */
	have_since = 0;
	since = 0;
	limit = BTD_PAGE_LIMIT;
	have_limit = 0;
	count = 0U;
	cursor_text[0] = '\0';
	for (;;) {
		got = btd_phoneio_next(&cursor, key, sizeof(key), value, sizeof(value));
		if (got == BTD_PHONEIO_END)
			break;
		if (got != BTD_PHONEIO_ARGUMENT) {
			btd_write(client, "ERROR argument\nDONE\n");
			return;
		}

		/* The cursor, as it is. */
		same = strcmp(key, "cursor");
		if (same == 0) {
			(void)snprintf(cursor_text, sizeof(cursor_text), "%s", value);
			continue;
		}

		/* since: decimal seconds, not negative. */
		same = strcmp(key, "since");
		if (same == 0) {
			error = btd_decimal(value, 0x7fffffffffffffffULL, &number);
			if (error != 0) {
				btd_write(client, "ERROR argument\nDONE\n");
				return;
			}

			/* Taken. */
			since = (int64_t)number;
			have_since = 1;
			continue;
		}

		/* limit and count: small decimal numbers (their ranges are MAP's to check). */
		error = btd_decimal(value, 100000ULL, &number);
		if (error != 0) {
			btd_write(client, "ERROR argument\nDONE\n");
			return;
		}

		/* The limit. */
		same = strcmp(key, "limit");
		if (same == 0) {
			limit = (unsigned)number;
			have_limit = 1;
			continue;
		}

		/* The count. */
		same = strcmp(key, "count");
		if (same == 0) {
			count = (unsigned)number;
			continue;
		}

		/* Any other key. */
		btd_write(client, "ERROR argument\nDONE\n");
		return;
	}

	/* A count given; since too, except for contacts; a limit only for messages. */
	if (count == 0U) {
		btd_write(client, "ERROR argument\nDONE\n");
		return;
	}

	/* since for messages and calls. */
	if (!have_since && what != BTD_PBAP_WHAT_CONTACTS) {
		btd_write(client, "ERROR argument\nDONE\n");
		return;
	}

	/* A limit for messages alone. */
	if (have_limit && what != 0U) {
		btd_write(client, "ERROR argument\nDONE\n");
		return;
	}

	/* Asked of MAP or PBAP, the client waiting for its DONE (a refusal answered now). */
	client->waits_phone = 1;
	if (what == 0U) {
		refused = btd_map_page(&btd_messages, btd_token(index), since, limit, cursor_text, count);
	} else {
		refused = btd_pbap_page(&btd_contacts, btd_token(index), what, since, cursor_text, count);
	}

	/* A refusal answered now, the client waiting no more. */
	if (refused != NULL) {
		client->waits_phone = 0;
		btd_write(client, "ERROR %s\nDONE\n", refused);
	}
}

/* Starts PHONE READ handle=H (ws197-p004 section 5.1): the owner's and root's, answered by MAP. */
static void
btd_phone_read_request(
	int index,
	const char *argument)
{
	struct btd_client *client;
	char key[BTD_PHONEIO_KEY_MAX];
	char value[BTD_PHONEIO_VALUE_MAX];
	const char *cursor;
	const char *refused;
	int allowed;
	int got;
	int same;

	/* The owner and root. */
	client = &btd_clients[index];
	allowed = btd_phone_allowed(client->uid);
	if (!allowed) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* handle=, the one argument. */
	cursor = argument;
	got = btd_phoneio_next(&cursor, key, sizeof(key), value, sizeof(value));
	same = strcmp(key, "handle");
	if (got != BTD_PHONEIO_ARGUMENT || same != 0 || *cursor != '\0') {
		btd_write(client, "ERROR argument\nDONE\n");
		return;
	}

	/* Asked of MAP, the client waiting for its DONE (a refusal answered now). */
	client->waits_phone = 1;
	refused = btd_map_read(&btd_messages, btd_token(index), value);
	if (refused != NULL) {
		client->waits_phone = 0;
		btd_write(client, "ERROR %s\nDONE\n", refused);
	}
}

/* MAP's clock: the daemon's monotonic milliseconds. */
static uint64_t
btd_messages_clock(
	void *context)
{
	uint64_t now;

	UNUSED_PARAMETER(context);

	/* The daemon's clock. */
	now = btd_now_ms();
	return now;
}

/* MAP's time of day: seconds since 1970 UTC. */
static int64_t
btd_messages_wall(
	void *context)
{
	time_t now;

	UNUSED_PARAMETER(context);

	/* The system's time. */
	now = time(NULL);
	return (int64_t)now;
}

/* MAP's zone: zedBSD's offset from UTC at a time (0 when the zone cannot be read). */
static int32_t
btd_messages_offset(
	void *context,
	int64_t seconds)
{
	struct tm broken;
	struct tm *local;
	time_t moment;

	UNUSED_PARAMETER(context);

	/* The local time then. */
	moment = (time_t)seconds;
	local = localtime_r(&moment, &broken);
	if (local == NULL)
		return 0;

	/* Its offset. */
	return (int32_t)broken.tm_gmtoff;
}

/* Tells MAP whether messages are wanted: a valid record that is on, with its messages. */
static int
btd_messages_wanted(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* No valid record. */
	if (!btd_phone_link.have_record || !btd_phone_link.record_valid)
		return 0;

	/* Off, or without messages. */
	if (!btd_phone_link.record.enabled)
		return 0;
	if ((btd_phone_link.record.profiles & BTD_PHONEREC_MESSAGES) == 0U)
		return 0;

	/* Wanted. */
	return 1;
}

/* Tells PBAP whether contacts are wanted: a valid record that is on, with its contacts (ws197-p005 section 8). */
static int
btd_contacts_wanted(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* No valid record. */
	if (!btd_phone_link.have_record || !btd_phone_link.record_valid)
		return 0;

	/* Off, or without contacts. */
	if (!btd_phone_link.record.enabled)
		return 0;
	if ((btd_phone_link.record.profiles & BTD_PHONEREC_CONTACTS) == 0U)
		return 0;

	/* Wanted. */
	return 1;
}

/* Gives PBAP the phone's address on the link (its record's).  Returns 0, or ENOENT without a record. */
static int
btd_contacts_address(
	void *context,
	uint8_t *address)
{
	UNUSED_PARAMETER(context);

	/* No record. */
	if (!btd_phone_link.have_record)
		return ENOENT;

	/* Succeeded: the record's phone. */
	memcpy(address, btd_phone_link.record.address, BTD_ADDRESS_BYTES);
	return 0;
}

/* PBAP's SDP query, through the mux. */
static int
btd_contacts_sdp(
	void *context,
	uint16_t uuid)
{
	int error;

	UNUSED_PARAMETER(context);

	/* The mux's query for PBAP. */
	error = btd_phonemux_sdp_query(&btd_profiles, btd_contacts_child, uuid);
	return error;
}

/* PBAP's DLC asked for, through the mux. */
static int
btd_contacts_open(
	void *context,
	unsigned server_channel)
{
	int error;

	UNUSED_PARAMETER(context);

	/* The mux's DLC for PBAP. */
	error = btd_phonemux_dlc_open(&btd_profiles, btd_contacts_child, server_channel);
	return error;
}

/* MAP's SDP query, on the phone link. */
static int
btd_messages_sdp(
	void *context,
	uint16_t uuid)
{
	int error;

	UNUSED_PARAMETER(context);

	/* The mux's query for MAP. */
	error = btd_phonemux_sdp_query(&btd_profiles, btd_messages_child, uuid);
	return error;
}

/* MAP's DLC asked for, on the phone link. */
static int
btd_messages_open(
	void *context,
	unsigned server_channel)
{
	int error;

	UNUSED_PARAMETER(context);

	/* The mux's DLC for MAP. */
	error = btd_phonemux_dlc_open(&btd_profiles, btd_messages_child, server_channel);
	return error;
}

/* The mux's SDP query, on the phone link. */
static int
btd_profiles_sdp(
	void *context,
	uint16_t uuid)
{
	int error;

	UNUSED_PARAMETER(context);

	/* The phone link's query. */
	error = btd_phone_sdp_query(&btd_phone_link, uuid);
	return error;
}

/* The mux's DLC asked for, on the phone link. */
static int
btd_profiles_open(
	void *context,
	unsigned server_channel)
{
	int error;

	UNUSED_PARAMETER(context);

	/* The phone link's DLC. */
	error = btd_phone_dlc_open(&btd_phone_link, server_channel, btd_now_ms());
	return error;
}

/* The mux's close of a DLC no profile owns, on the phone link. */
static void
btd_profiles_close(
	void *context,
	unsigned dlci)
{
	UNUSED_PARAMETER(context);

	/* The phone link's DLC. */
	(void)btd_phone_dlc_close(&btd_phone_link, dlci, btd_now_ms());
}

/* MAP's bytes on a DLC, as far as its credits take them. */
static int
btd_messages_write(
	void *context,
	unsigned dlci,
	const uint8_t *data,
	size_t length,
	size_t *written)
{
	int error;

	UNUSED_PARAMETER(context);

	/* The phone link's DLC. */
	error = btd_phone_dlc_write(&btd_phone_link, dlci, data, length, written);
	return error;
}

/* MAP's DLC closed (one already gone is nothing). */
static void
btd_messages_close(
	void *context,
	unsigned dlci)
{
	UNUSED_PARAMETER(context);

	/* The phone link's DLC. */
	(void)btd_phone_dlc_close(&btd_phone_link, dlci, btd_now_ms());
}

/* Writes MAP's answer to the client a token names (DONE ends its wait); a client that went hears nothing. */
static void
btd_messages_answer(
	void *context,
	uint64_t token,
	const char *line,
	const uint8_t *bytes,
	size_t length)
{
	struct btd_client *client;
	int same;

	UNUSED_PARAMETER(context);

	/* The client. */
	client = btd_token_client(token);
	if (client == NULL)
		return;

	/* The line and its bytes. */
	btd_client_raw(client, line, bytes, length);

	/* Succeeded: DONE ends the wait. */
	same = strcmp(line, "DONE");
	if (same == 0)
		client->waits_phone = 0;
}

/* Writes an event to each subscriber that may hear it (the owner's and root's, checked now, ws197-p004 section 5.4). */
static void
btd_messages_emit(
	void *context,
	const char *line,
	const uint8_t *bytes,
	size_t length)
{
	struct btd_client *client;
	unsigned index;
	int allowed;

	UNUSED_PARAMETER(context);

	/* Each subscriber. */
	for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
		client = &btd_clients[index];
		if (client->descriptor < 0 || !client->subscribed || client->dead)
			continue;

		/* Only the owner and root, as they are now. */
		allowed = btd_phone_allowed(client->uid);
		if (!allowed)
			continue;

		/* Written, or dropped when its queue is full. */
		btd_subscriber_write(client, line, bytes, length);
	}
}

/* Tells MAP how many bytes a token's client still takes (-1: it went). */
static long
btd_messages_room(
	void *context,
	uint64_t token)
{
	struct btd_client *client;
	size_t pending;

	UNUSED_PARAMETER(context);

	/* The client. */
	client = btd_token_client(token);
	if (client == NULL)
		return -1;

	/* Its queue's room. */
	pending = btd_outq_pending(&client->output);
	return (long)(BTD_OUTQ_MAX - pending);
}

/* MAP came up: the phone link's waits start from the first. */
static void
btd_messages_up(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* The phone link's backoff. */
	btd_phone_profile_ok(&btd_phone_link);
}

/* A line of MAP's for the daemon's log. */
static void
btd_messages_log(
	void *context,
	const char *line)
{
	UNUSED_PARAMETER(context);

	/* Logged. */
	btd_log("bluetoothd: %s\n", line);
}

/* Reads a decimal number of 1 to 19 digits, at most most.  Returns 0 with it, or EINVAL. */
static int
btd_decimal(
	const char *text,
	unsigned long long most,
	unsigned long long *value)
{
	unsigned long long number;
	unsigned long long digit;
	size_t length;
	size_t index;

	/* 1 to 19 digits. */
	length = strlen(text);
	if (length == 0U || length > 19U)
		return EINVAL;

	/* Each digit, the number within most. */
	number = 0U;
	for (index = 0U; index < length; index++) {
		if (text[index] < '0' || text[index] > '9')
			return EINVAL;
		digit = (unsigned long long)(text[index] - '0');
		if (number > (most - digit) / 10U)
			return EINVAL;
		number = number * 10U + digit;
	}

	/* Succeeded: the number. */
	*value = number;
	return 0;
}
