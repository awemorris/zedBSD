/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD networkd userland command.
 */

#include "userland/base/net/netconf.h"
#include "userland/base/net/netutil.h"
#include "userland/base/net/protocol.h"
#include "userland/base/net/publication-trace.h"
#include "userland/base/net/wifi-store.h"
#include "userland/base/networkd/confirmed.h"
#include "userland/base/networkd/ipv6.h"
#include "userland/base/networkd/lan-configure.h"
#include "userland/base/networkd/managed-lan.h"
#include "userland/base/networkd/managed-wlan.h"
#include "userland/base/networkd/sleep-state.h"
#include "userland/base/networkd/wifi-child.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <net/if.h>
#include <net/route.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define CHILD_OUTPUT_MAX 384

/* How often, and how far apart, an up of a wired interface is tried (BUG-168: an adapter still attaching refuses it). */
#define LAN_RAISE_ATTEMPTS	5U
#define LAN_RAISE_RETRY_NS	200000000L
#define NETWORKD_AUTH_LOG_MAX 512U
#define NETWORKD_GROUP_DATABASE_MAX 8192U
#define NETWORKD_GROUP_BUFFER_MAX 2048U

/* The most groups of one account that the Wi-Fi control check examines. */
#define NETWORKD_PEER_GROUP_MAX 64
#define NETWORKD_GROUP_FILE "/etc/group"
#define NETWORKD_GROUP_GID ((gid_t)69)
#define NETWORKD_GROUP_NAME "network"
#define NETWORKD_RESPONSE_FIXED_OVERHEAD 20U
#define NETWORKD_RESPONSE_OUTPUT_MAX \
	(NETWORKD_RESPONSE_MAX - NETWORKD_RESPONSE_FIXED_OVERHEAD)
#define NETWORKD_WLAN_RADIO_MAX	16U
#define NETWORKD_WLAN_SCAN_SECONDS	30U
#define NETWORKD_WLAN_CONNECT_SECONDS	35U
#define NETWORKD_WLAN_DHCP_SECONDS	10U
#define NETWORKD_WLAN_RESCAN_SECONDS	5U
#define NETWORKD_WLAN_ATTEMPTS	4U
#define NETWORKD_WIFI_STATUS_RESERVE	256U

/* The most login sessions whose Wi-Fi stores are candidates at one time. */
#define NETWORKD_WIFI_SESSION_MAX	8U

/* The most refused keys remembered at one time; a newer refusal pushes out the oldest. */
#define NETWORKD_WIFI_REJECTION_MAX	8U
#define NETWORKD_CONTROL_INPUT_MAX	4U

enum networkd_client_role {
	NETWORKD_CLIENT_ROOT,
	NETWORKD_CLIENT_MEMBER
};

struct networkd_listener {
	int descriptor;
	dev_t device;
	ino_t inode;
	int owns_path;
	const char *stage;
};

struct networkd_group_scan {
	unsigned network_records;
	unsigned gid_records;
};

struct networkd_request {
	struct networkd_protocol_header header;
	unsigned char payload[NETWORKD_REQUEST_MAX];
	char interface[IFNAMSIZ];
	/* An address either family, with an IPv6 prefix's length (ws130-p005). */
	char address[INET6_ADDRSTRLEN + 5];
	char netmask[INET_ADDRSTRLEN];
	char gateway[INET6_ADDRSTRLEN];
	char rollback_path[NETWORKD_ROLLBACK_PATH_MAX + 1U];
	char dns[8][INET6_ADDRSTRLEN];
	unsigned char ssid[WLAN_SSID_MAX];
	size_t ssid_length;
	unsigned dns_count;
	unsigned timeout;
	uint32_t token;
	/* The account a session request is about (NETWORKD_FIELD_ACCOUNT). */
	uint32_t account;
};

struct networkd_wlan_radio {
	char interface[IFNAMSIZ];
	uint32_t ifindex;
	int ready;
	int administrative_up;
	int association_active;
	uint32_t scan_state;
	uint64_t snapshot_generation;
	/* Last validated snapshot considered by selection for this device identity. */
	uint64_t consumed_snapshot_generation;
	uint32_t stop_flags;
	int observation_error;
};

/* Owns all temporary data for exactly one managed Wi-Fi request. */
struct networkd_wifi_request {
	struct wifi_conf_model profiles;
	struct networkd_wlan_radio radios[NETWORKD_WLAN_RADIO_MAX];
	size_t radio_count;
	/* Keep diagnostic and final policy records even when radio output fills. */
	char output[NETWORKD_RESPONSE_OUTPUT_MAX - NETWORKD_DIAGNOSTIC_MAX - 4U];
	size_t output_length;
	char diagnostic[WIFI_CONF_DIAGNOSTIC_MAX];
	const char *stage;
};

/* All subordinate waits consume one actor-owned interval. */
struct networkd_wifi_work {
	uint64_t deadline;
	int background;
	int cancelled;
	int cleanup;
	int profiles_changed;
};

/* Read-only observations let status clients inspect an ongoing RF operation. */
struct networkd_wifi_observation {
	char interface[IFNAMSIZ];
	uint32_t ifindex;
	uint64_t observed_at;
	char *output;
	size_t output_length;
};

struct networkd_wifi_pending {
	struct networkd_request request;
	struct kern_peercred peer;
	enum networkd_client_role role;
};

struct networkd_wifi_candidate {
	const struct wifi_conf_profile *profile;
	size_t radio;
};

/* A saved key a network refused: the account whose store holds it and the network's SSID. */
struct networkd_wifi_rejection {
	uid_t account;
	size_t ssid_length;
	unsigned char ssid[WLAN_SSID_MAX];
};

/* Partial request bytes belong to transport, never to an RF transaction. */
struct networkd_control_input {
	int active;
	int descriptor;
	uint64_t deadline;
	struct kern_peercred peer;
	enum networkd_client_role role;
	size_t used;
	unsigned char bytes[NETWORKD_PROTOCOL_HEADER_MAX + NETWORKD_REQUEST_MAX + 1U];
};

/*
 * One wired interface's lease: the default route and resolver its DHCP gave.
 *
 * The network preference keeps them so that it can withdraw the route while
 * another interface is preferred and put it back when that one goes.  A
 * record lives from a successful DHCP configuration of the interface until
 * the interface is configured again or taken down.
 */
struct networkd_lan_l3 {
	char interface[IFNAMSIZ];
	uint32_t ifindex;
	int route_present;
	struct networkd_managed_route route;
	size_t resolver_length;
	unsigned char resolver[NETWORKD_MANAGED_RESOLVER_MAX];
};

static volatile sig_atomic_t stopping;
static struct networkd_managed_wlan managed_wlan;

/*
 * The machine's sleep (ws052-p010): whether networkd is asleep for it and
 * the Wi-Fi policy it takes up again.  Only the event loop's thread uses it.
 */
static struct networkd_sleep sleep_state;

/*
 * What the end of a sleep takes up again, done by the event loop's next
 * pass (run_due_work) rather than in the request's own handler, which may
 * run inside other Wi-Fi work: whether one is waiting, what, and the end's
 * name for the log.  Only the event loop's thread uses them.
 */
static unsigned sleep_resume_due;
static enum networkd_sleep_resume sleep_resume_pending;
static const char *sleep_resume_via;

/*
 * The wired interfaces, and what is to happen to them.
 *
 * What the configuration says is handed over by the net command, which owns
 * the file it is written in; what the cables are doing is told by the
 * kernel on the route socket.  The daemon holds both and acts on them in
 * the background, because a cable moves when it moves and nobody is
 * waiting to be told.
 */
static struct networkd_lan managed_lan;

/*
 * The leases of the wired interfaces, one slot for each, an empty interface
 * name marking a free slot.  The network preference reads them; lan_configure
 * fills a slot and lan_take_down empties it.
 */
static struct networkd_lan_l3 lan_l3[NETWORKD_LAN_MAX];

/*
 * The watchers, and whether the network has moved since they were last told.
 *
 * The flag is set wherever the state changes and acted on once, at the top
 * of the loop: a single change often arrives as several smaller ones, and a
 * watcher wants the result rather than each step of it.
 */
static int subscribers[NETWORKD_SUBSCRIBER_MAX];
static uint32_t subscriber_ids[NETWORKD_SUBSCRIBER_MAX];
static size_t subscriber_count;
static int state_changed;

/* Set when an event has left the wired policy something to do. */
static int lan_work_due;

/*
 * The name servers a wired configuration named (ws089-p022, LAN_CONFIGURE):
 * while there are any they are the resolver's, over a lease's (the
 * network preference writes them after it chose a lease's).
 */
static char lan_static_dns[NETWORKD_LAN_CONFIGURE_DNS_MAX][INET_ADDRSTRLEN];
static unsigned lan_static_dns_count;

/* An interface an event asked to be taken down, done at the next turn. */
static char lan_down_pending[IFNAMSIZ];

/*
 * A wired interface whose device has gone, kept until the next turn.
 *
 * Its lease no longer carries anything, so the network preference is
 * decided again without it (B3: Wi-Fi takes the default route and the
 * resolver back).  An empty name means nothing is pending.
 */
static char lan_removed_pending[IFNAMSIZ];

static struct networkd_wlan_radio known_wlan_radios[NETWORKD_WLAN_RADIO_MAX];
static size_t known_wlan_radio_count;
static int wifi_disable_pending;
static struct networkd_confirmed confirmed;
static int route_events = -1;
/* The route socket of IPv6's events (ws130-p006), -1 without it. */
static int ipv6_events = -1;
static uint64_t route_event_sequence;
static uint64_t automatic_retry_at;
static unsigned retirement_retry_seconds = NETWORKD_WLAN_RESCAN_SECONDS;
static size_t automatic_candidate_skip;

/*
 * The accounts whose login sessions are open, the oldest first.
 *
 * sessiond announces a session when it opens and when it closes (ws005-p024,
 * the user's decision of 2026-10-02).  While an account is listed here, its
 * own store of saved networks is a candidate for automatic joining besides
 * the system's store.  Only the session requests change the list.  It is
 * empty when networkd starts, so a session that was open across a restart of
 * networkd counts again from its next login.
 */
static uid_t wifi_session_uids[NETWORKD_WIFI_SESSION_MAX];
static size_t wifi_session_count;

/*
 * The account whose store gave the profile of the current connection.
 *
 * A successful join records it, and it is read while that connection lasts:
 * a reconnect takes the key from the same store, and the close of that
 * account's session ends the connection.  Known is cleared when a new join
 * begins, so it never names the store of an earlier connection.
 */
static uid_t wifi_connection_store_uid;
static int wifi_connection_store_known;

/*
 * The saved keys a network refused during its handshake, the oldest first.
 *
 * Automatic joining passes over such a key instead of offering it to the
 * network again and again (ws005-p020, q631).  An entry goes when its store
 * changes (the key may have been corrected), when an explicit join of that
 * network with that store succeeds, and when Wi-Fi is turned off.  Only the
 * event loop touches the list.
 */
static struct networkd_wifi_rejection wifi_rejections[NETWORKD_WIFI_REJECTION_MAX];
static size_t wifi_rejection_count;

/*
 * Set when the store that gave the current connection's key changed: the
 * event loop then checks, outside any running Wi-Fi work, that the network
 * is still saved there, and leaves it when it was deleted (net wifi delete).
 */
static int wifi_connection_recheck_due;

/*
 * Until when a desktop asked for fresh scans (ws089-p021), on the monotonic
 * clock in microseconds; 0 when nobody asks.  WIFI_SCAN_START moves it
 * NETWORKD_WIFI_SCAN_LEASE_SECONDS ahead and WIFI_SCAN_STOP sets it to 0;
 * a time already past means the desktop that asked went without saying
 * so.  Only the event loop touches it.
 */
static uint64_t wifi_scan_until;

/*
 * When the next asked-for scan starts, on the same clock (0: none is
 * planned).  A scan is planned only while the lease above holds and the
 * policy is on and left unconnected (manual-disconnected); each one plans
 * the next NETWORKD_WLAN_RESCAN_SECONDS later.  Only the event loop
 * touches it.
 */
static uint64_t wifi_scan_due;
static struct networkd_wifi_work wifi_work;
static struct networkd_wifi_observation wifi_observations[NETWORKD_WLAN_RADIO_MAX];
static size_t wifi_observation_bytes;
static struct networkd_wifi_pending wifi_pending;
static int wifi_pending_client = -1;
static int control_listener = -1;
static int (*wifi_wait_pump)(void);
static struct networkd_control_input control_inputs[NETWORKD_CONTROL_INPUT_MAX];
static int control_input_pending(void);
static int receive_wait_request(struct networkd_request *, struct kern_peercred *, enum networkd_client_role *);
static int service_wifi_wait(void);
static int interrupts_background_work(const struct networkd_request *, const struct kern_peercred *);
static void remember_wifi_observation(const char *, const struct networkd_wifi_child_result *);
static void clear_wifi_observation(size_t);
static int append_wifi_snapshot(const char *, const char *, size_t, uint64_t, char *, size_t, size_t *);
static void send_wifi_observation(int, const struct networkd_request *);
static void wifi_profiles_changed(const struct kern_peercred *);
static void dispatch_pending_wifi(void);
static void wifi_work_begin(uint32_t, int);
static void wifi_work_end(void);
static void recover_managed_connection(void);
static void retry_managed_retirement(void);
static void schedule_retirement_retry(void);

static int scan_group_record(char *line, struct networkd_group_scan *scan);
static int validate_network_group_database(void);
static int resolve_network_group(gid_t *result);
static int listener_path_matches(const struct networkd_listener *listener);
static int close_listener(struct networkd_listener *listener);
static int remove_stale_listener(void);
static int open_listener(struct networkd_listener *listener);
static int open_route_events(void);
static int process_route_events(void);
static void process_route_event(const struct rtm_ifinfo *);
static int lan_policy_decode(const struct networkd_request *,
			     struct networkd_lan_policy *, size_t, size_t *);
static int lan_interface_name(uint32_t, char *, size_t);
static void lan_snapshot(void);
static int lan_interface_is_radio(int, const char *, uint32_t);
static int lan_address_usable(const char *);
static void lan_l3_record(const char *);
static void lan_l3_forget(const char *, int);
static void lan_l3_removed(uint32_t);
static struct networkd_lan_l3 *lan_l3_find(const char *);
static int lan_l3_eligible(const struct networkd_lan_l3 *);
static void apply_network_preference(void);
static int prefer_route(int, const struct networkd_managed_route *);
static int withdraw_route(int, const struct networkd_managed_route *);
static int route_present_exact(int, const struct networkd_managed_route *);
static int prefer_resolver(const unsigned char *, size_t);
static void lan_assign_link_local(const char *, uint64_t);
static void lan_configure(const struct networkd_lan_work *);
static void lan_take_down(const char *);
static void lan_clear_address(const char *);
static void lan_raise(const char *);
static void run_lan_work(void);
static void lan_note_configured(const char *);
static void recover_managed_wlan(void);
static void schedule_automatic_work(unsigned);
static int automatic_poll_timeout(void);
static void run_automatic_work(void);
static int event_poll_timeout(void);
static void run_confirmed_due(void);
static void run_due_work(void);
static int retire_managed_connection(enum networkd_managed_wlan_state, int);
static int retire_removed_connection(enum networkd_managed_wlan_state);
static int retire_managed_policy(void);
static void notify_init(const char *record);
static int write_all(int descriptor, const char *buffer, size_t length);
static void write_auth_log(const struct kern_peercred *peer,
			   enum networkd_client_role role, int error);
static int authenticate_client(int client, struct kern_peercred *peer,
			       enum networkd_client_role *role);
static int operation_allowed(enum networkd_client_role role,
			     const char *operation);
static int handle_request(int, enum networkd_client_role,
	const struct kern_peercred *);
static void dispatch_request(int, struct networkd_request *, enum networkd_client_role, const struct kern_peercred *);
static void send_wired_observation(int, const struct networkd_request *);
static void handle_wifi_request(int, struct networkd_request *,
	const struct kern_peercred *);
static int wifi_request_list(struct networkd_wifi_request *);
static int wifi_request_enable(struct networkd_wifi_request *, const struct kern_peercred *);
static int wifi_request_stop(struct networkd_wifi_request *, int);
static int wifi_request_connect(struct networkd_wifi_request *, const struct networkd_request *, const struct kern_peercred *);
static int wifi_policy_take(struct networkd_wifi_request *, uid_t);
static int wifi_request_prepare(struct networkd_wifi_request *);
static void process_wifi_request(int, struct networkd_request *, const struct kern_peercred *);
static int read_request(int descriptor, struct networkd_request *request);
static int read_request_end(int descriptor);
static int decode_request(struct networkd_request *request);
static int copy_request_text(char *, size_t, const struct networkd_field *);
static const char *operation_name(uint32_t opcode);
static void send_response(int, uint32_t, uint32_t, uint32_t, int,
	const char *, const void *, size_t);
static void send_error(int client, int error, const char *reason);
static void send_token_response(int, uint32_t, uint32_t, uint32_t);
static int execute_wired_request(struct networkd_request *, char *, size_t,
	char *, size_t, size_t *, int *, uint64_t);
static int rollback_validate(const char *, char *, size_t, void *);
static int rollback_execute(const char *, char *, size_t, void *);
static int rollback_parse(const char *, struct networkd_request *, char *,
	size_t);
static int show_interfaces(const char *name, char *output, size_t capacity);
static int subscriber_add(int client, uint32_t request_id);
static void subscriber_drop(size_t slot);
static void subscribers_close(void);
static void subscriber_send(size_t slot, const char *state, size_t length);
static void notify_subscribers(void);
static void notify_state_changed(void);
static void deliver_state_changes(void);
static int accept_subscriber(int client, enum networkd_client_role role,
    uint32_t request_id);
static int watch_state(char *state, size_t capacity, size_t *length);
static int watch_ssid_known(void);
static int append_interface_status(int descriptor, const char *name, char *output, const size_t capacity, size_t *used);
static int interface_exists(const char *name);
static int interface_index(const char *, uint32_t *);
static int interface_flags(const char *, int *);
static int interface_index_name_matches(uint32_t, const char *);
static int wlan_connected(const char *);
static int managed_connection_usable(void);
static int run_wifi(const char *, const char *, const struct wifi_conf_profile *, unsigned, struct networkd_wifi_child_result *);
static int append_wifi_records(const char *, const char *, size_t, char *, size_t, size_t *);
static int append_wifi_output(const char *, const struct networkd_wifi_child_result *, char *, size_t, size_t *);
static int enumerate_wlan_radios(struct networkd_wlan_radio *, size_t, size_t *);
static int prepare_wlan_radios(struct networkd_wlan_radio *, size_t, char *, size_t, size_t *);
static int run_wifi_append(const char *, const char *, char *, size_t, size_t *);
static int prepare_wlan_radio(struct networkd_wlan_radio *, char *, size_t, size_t *);
static void consume_wlan_snapshot(const struct networkd_wlan_radio *, uint64_t);
static int stop_wlan_radios(const struct networkd_wlan_radio *, size_t, int);
static int wlan_radio_status(const struct networkd_wlan_radio *, struct wlan_status_request *);
static int wifi_disable_defer(void);
static int collect_profile_radios(const struct networkd_wlan_radio *, size_t, const struct wifi_conf_model *, size_t, struct networkd_wifi_candidate *, size_t *, size_t *, uint64_t);
static unsigned wifi_selection_timeout(uint64_t);
static int select_manual_radio(const struct networkd_wlan_radio *, size_t, const struct wifi_conf_profile *, size_t *, uint64_t);
static int connect_automatic(const struct networkd_wlan_radio *, size_t, const struct wifi_conf_model *, const uid_t *, uint64_t, char *, size_t, size_t *, int *);
static void stop_losing_scans(const struct networkd_wlan_radio *, size_t, size_t);
static int run_managed_connect(const char *, const struct wifi_conf_profile *, enum networkd_managed_wlan_state, uint64_t, char *, size_t, size_t *, int *);
static int acquire_managed_l3(const char *, uint64_t);
static int reconcile_pending_l3(void);
static const struct wifi_conf_profile *find_profile(const struct wifi_conf_model *, const void *, size_t);
static int load_policy(uid_t, struct wifi_conf_model *, char *, size_t);
static void load_candidates(struct wifi_conf_model *, uid_t *);
static void candidate_store_add(struct wifi_conf_model *, uid_t *, uid_t);
static void connection_store_note(const struct wifi_conf_model *, const uid_t *);
static int connection_from_store(uid_t);
static int wifi_session_account(const struct networkd_request *, const struct kern_peercred *, uid_t *);
static int wifi_session_find(uid_t);
static void wifi_session_forget(uid_t);
static int wifi_session_open(const struct networkd_request *, const struct kern_peercred *);
static int wifi_session_close(struct networkd_wifi_request *, const struct networkd_request *, const struct kern_peercred *);
static void wifi_candidates_changed(void);
static void wifi_rejection_note(uid_t, const struct wifi_conf_profile *);
static int wifi_rejection_find(uid_t, const unsigned char *, size_t);
static void wifi_rejection_forget(uid_t, const unsigned char *, size_t);
static void wifi_rejection_forget_store(uid_t);
static void wifi_join_failed(void);
static void wifi_scan_request(int, const struct networkd_request *);
static int wifi_scan_wanted(void);
static int wifi_scan_poll_timeout(void);
static void run_requested_scan(void);
static void handle_sleep_request(int client, const struct networkd_request *request);
static int sleep_radios_off(void);
static void sleep_resume(enum networkd_sleep_resume resume, const char *via);
static void sleep_resume_later(enum networkd_sleep_resume resume, const char *via);
static int default_route_index(uint32_t *);
static void recheck_connection_profile(void);
static void wifi_off_remember(int);
static int append_hex(char *, size_t, size_t *, const unsigned char *, size_t);
static int owner_allowed(const struct kern_peercred *);
static int peer_in_network_group(const struct kern_peercred *);
static const char *managed_state_name(enum networkd_managed_wlan_state);
static int append_managed_status(char *, size_t, size_t *);
static int snapshot_interface_l3(const char *, uint32_t *, struct networkd_managed_l3 *);
static int snapshot_managed_l3(const struct networkd_managed_wlan *, struct networkd_managed_l3 *);
static int snapshot_resolver(struct networkd_managed_l3 *);
static void identify_l3_ownership(const char *, const struct networkd_managed_l3 *, struct networkd_managed_l3 *);
static int clear_interface_l3(struct networkd_managed_wlan *);
static int find_interface_default(int, uint32_t, struct networkd_managed_l3 *);
static int delete_interface_default_exact(int, const struct networkd_managed_route *);
static int route_matches_owned(const struct rtentry *, const struct networkd_managed_route *);
static int managed_routes_equal(const struct networkd_managed_route *, const struct networkd_managed_route *);
static int get_interface_ipv4(int, const char *, unsigned long, uint32_t *);
static int set_interface_ipv4(int, const char *, unsigned long, uint32_t);
static int unlink_owned_resolver(const struct networkd_managed_wlan *);
static int run_command_until(char *const [], unsigned, uint64_t,
	char [CHILD_OUTPUT_MAX]);
static void release_dhcp6(const char *interface, uint64_t deadline, char diagnostic[CHILD_OUTPUT_MAX]);
static void clean_diagnostic(char *text);
static int default_route_exists(void);
static int write_resolver(char *const addresses[], int count);
static void lan_static_resolver(void);
static void handle_lan_configure(int client, struct networkd_request *request, const struct kern_peercred *peer);
static int lan_configure_target(const struct networkd_request *request, char *reason, size_t capacity);
static int lan_configure_save(const struct networkd_lan_configure *configure, char *reason, size_t capacity);
static void lan_configure_policy(const struct networkd_request *request, const struct networkd_lan_configure *configure);
static int lan_configure_apply(struct networkd_request *request, const struct networkd_lan_configure *configure, char diagnostic[CHILD_OUTPUT_MAX]);
static void handle_signal(int signal_number);
static void ignore_signal(int signal_number);

/*
 * Runs the networkd command.
 */
int
main(
	void)
{
	char record[256];
	int saved;
	int client;
	int status;
	int poll_result;
	int poll_timeout;
	size_t index;
	struct pollfd descriptors[3U + NETWORKD_SUBSCRIBER_MAX];
	unsigned ipv6_slot;
	nfds_t descriptor_count;
	size_t slot;
	struct kern_peercred peer;
	enum networkd_client_role role;
	struct networkd_listener listener;

	(void)signal(SIGHUP, ignore_signal);
	(void)signal(SIGPIPE, ignore_signal);
	(void)signal(SIGTERM, handle_signal);
	(void)signal(SIGINT, handle_signal);
	status = 0;
	networkd_managed_wlan_init(&managed_wlan);
	networkd_sleep_init(&sleep_state);
	networkd_confirmed_init(&confirmed);

	/* Creates and verifies the privileged network control endpoint. */
	if (open_listener(&listener) != 0) {
		saved = errno;
		(void)snprintf(record, sizeof(record), "FAIL %d %s %s\n", saved,
			       listener.stage != NULL ? listener.stage : "unknown",
			       strerror(saved));
		notify_init(record);
		fprintf(stderr, "networkd: control socket %s: %s\n",
			listener.stage != NULL ? listener.stage : "unknown",
			strerror(saved));

		/* Reports operation failure. */
		return 1;
	}
	route_events = open_route_events();
	if (route_events < 0) {
		saved = errno != 0 ? errno : EIO;
		(void)close_listener(&listener);
		fprintf(stderr, "networkd: route event socket: %s\n",
		    strerror(saved));
		return 1;
	}
	if (retire_managed_policy() != 0) {
		saved = errno != 0 ? errno : EIO;
		(void)close(route_events);
		route_events = -1;
		(void)close_listener(&listener);
		(void)snprintf(record, sizeof(record), "FAIL %d wifi-normalize %s\n",
		    saved, strerror(saved));
		notify_init(record);
		fprintf(stderr, "networkd: initial Wi-Fi normalization: %s\n",
		    strerror(saved));
		return 1;
	}
	notify_init("READY\n");
	control_listener = listener.descriptor;

	/* IPv6 (ws130-p006): its events, and the interfaces already up. */
	ipv6_events = networkd_ipv6_open();
	if (ipv6_events < 0)
		fprintf(stderr, "networkd: IPv6 route socket: %s\n", strerror(errno));
	networkd_ipv6_start();

	/*
	 * Records the state as it is before anyone can watch, so that the
	 * first comparison after a watcher arrives measures a change and not
	 * the difference between knowing nothing and knowing something.
	 * Without this the first watcher is told the same state twice.
	 */
	deliver_state_changes();
	wifi_wait_pump = service_wifi_wait;
	networkd_wifi_child_set_pump(service_wifi_wait);

	/* Polls control and interface events without a one-second accept delay. */
	while (!stopping) {
		if (wifi_pending_client >= 0) {
			dispatch_pending_wifi();
			continue;
		}

		/* Continues partial requests accepted by the previous background wait. */
		if (control_input_pending())
			(void)service_wifi_wait();
		run_due_work();
		if (wifi_pending_client >= 0)
			continue;
		descriptors[0].fd = listener.descriptor;
		descriptors[0].events = POLLIN;
		descriptors[0].revents = 0;
		descriptors[1].fd = networkd_confirmed_active(&confirmed) ? -1 :
		    route_events;
		descriptors[1].events = POLLIN;
		descriptors[1].revents = 0;
		poll_timeout = event_poll_timeout();
		if (control_input_pending() && (poll_timeout < 0 || poll_timeout > 20))
			poll_timeout = 20;

		/*
		 * The watchers are polled too, not because they say anything
		 * -- they never do -- but so that one going away is noticed
		 * when it happens rather than at the next change.
		 */
		descriptor_count = 2U;

		/* Process each watcher. */
		for (slot = 0; slot < subscriber_count; slot++) {
			descriptors[descriptor_count].fd = subscribers[slot];
			descriptors[descriptor_count].events = 0;
			descriptors[descriptor_count].revents = 0;
			descriptor_count++;
		}

		/* The IPv6 events, after the watchers (ws130-p006). */
		ipv6_slot = descriptor_count;
		descriptors[descriptor_count].fd = ipv6_events;
		descriptors[descriptor_count].events = POLLIN;
		descriptors[descriptor_count].revents = 0;
		descriptor_count++;
		poll_result = poll(descriptors, descriptor_count, poll_timeout);
		if (poll_result < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (poll_result == 0) {
			run_due_work();
			deliver_state_changes();
			continue;
		}

		/*
		 * A watcher that has gone is dropped before anything is sent,
		 * so that a change is not written into a closed connection.
		 * The list is walked from the end because dropping refills a
		 * slot from there.
		 */
		slot = subscriber_count;

		/* Process each watcher, newest first. */
		while (slot > 0U) {
			slot--;

			/* Skips a watcher that is still connected. */
			if ((descriptors[2U + slot].revents &
			    (POLLERR | POLLHUP | POLLNVAL)) == 0)
				continue;
			subscriber_drop(slot);
		}
		if ((descriptors[ipv6_slot].revents & (POLLIN | POLLERR | POLLHUP)) != 0 &&
		    networkd_ipv6_events(ipv6_events) != 0) {
			(void)close(ipv6_events);
			ipv6_events = -1;
		}
		if ((descriptors[1].revents & (POLLIN | POLLERR | POLLHUP)) != 0 &&
		    process_route_events() != 0) {
			(void)close(route_events);
			route_events = -1;
		}
		if ((descriptors[0].revents & POLLIN) == 0) {
			deliver_state_changes();
			continue;
		}
		client = accept4(listener.descriptor, NULL, NULL, SOCK_CLOEXEC);

		/* Handles the client condition. */
		if (client >= 0) {
			/* Handles a failed authenticate client operation. */
			if (authenticate_client(client, &peer, &role) != 0) {
				send_error(client, EACCES,
				    "authentication failed");
				close(client);
			} else if (!handle_request(client, role, &peer)) {
				close(client);
			}
			deliver_state_changes();
			if (managed_wlan.state == NETWORKD_WLAN_AUTO_SEARCHING &&
			    automatic_retry_at == 0U)
				schedule_automatic_work(
				    NETWORKD_WLAN_RESCAN_SECONDS);
			continue;
		}

		/* Handles the reported system error. */
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			break;
	}
	status = stopping ? 0 : 1;
	if (wifi_pending_client >= 0) {
		send_response(wifi_pending_client, wifi_pending.request.header.request_id,
		    wifi_pending.request.header.opcode, NETWORKD_RESULT_ERROR, EINTR,
		    "networkd stopping", NULL, 0U);
		(void)close(wifi_pending_client);
		wifi_pending_client = -1;
		networkd_protocol_clear(&wifi_pending, sizeof(wifi_pending));
	}
	control_listener = -1;
	subscribers_close();

	/* Releases optional observations and incomplete transports on shutdown. */
	for (index = 0U; index < NETWORKD_WLAN_RADIO_MAX; index++)
		clear_wifi_observation(index);
	for (index = 0U; index < NETWORKD_CONTROL_INPUT_MAX; index++) {
		if (control_inputs[index].active)
			(void)close(control_inputs[index].descriptor);
	}
	networkd_protocol_clear(control_inputs, sizeof(control_inputs));
	wifi_wait_pump = NULL;
	networkd_wifi_child_set_pump(NULL);
	networkd_confirmed_reset(&confirmed);
	if (retire_managed_policy() != 0)
		status = 1;
	if (route_events >= 0) {
		(void)close(route_events);
		route_events = -1;
	}

	/* Removes only the socket pathname created by this daemon instance. */
	if (close_listener(&listener) != 0) {
		fprintf(stderr, "networkd: control socket cleanup: %s\n",
			strerror(errno));
		status = 1;
	}

	/* Returns the computed result. */
	return status;
}

/* Validates one record from the trusted network group database. */
static int
scan_group_record(
	char *line,
	struct networkd_group_scan *scan)
{
	char *field[4];
	char *cursor;
	char *end;
	unsigned long gid;

	/* Handles the line availability. */
	if (line == NULL || scan == NULL || line[0] == '\0' || line[0] == '#')
		return line != NULL && scan != NULL ? 0 : EINVAL;
	field[0] = line;
	cursor = strchr(field[0], ':');

	/* Handles the cursor availability. */
	if (cursor == NULL)
		return EINVAL;
	*cursor = '\0';
	field[1] = cursor + 1;
	cursor = strchr(field[1], ':');

	/* Handles the cursor availability. */
	if (cursor == NULL)
		return EINVAL;
	*cursor = '\0';
	field[2] = cursor + 1;
	cursor = strchr(field[2], ':');

	/* Handles the cursor availability. */
	if (cursor == NULL)
		return EINVAL;
	*cursor = '\0';
	field[3] = cursor + 1;

	/* Handles a failed strchr operation. */
	if (strchr(field[3], ':') != NULL || field[0][0] == '\0' ||
	    field[2][0] == '\0')

		/* Returns the computed result. */
		return EINVAL;
	for (cursor = field[0]; *cursor != '\0'; cursor++) {
		/* Checks the current cursor position. */
		if ((unsigned char)*cursor <= 32U ||
		    (unsigned char)*cursor == 127U)

			/* Returns the computed result. */
			return EINVAL;
	}
	for (cursor = field[2]; *cursor != '\0'; cursor++) {
		/* Checks the current cursor position. */
		if (*cursor < '0' || *cursor > '9')
			return EINVAL;
	}
	errno = 0;
	gid = strtoul(field[2], &end, 10);

	/* Handles the reported system error. */
	if (errno != 0 || end == field[2] || *end != '\0' || gid > UINT_MAX)
		return EINVAL;

	/* Selects the matching value. */
	if (strcmp(field[0], NETWORKD_GROUP_NAME) == 0)
		scan->network_records++;

	/* Handles the gid condition. */
	if (gid == (unsigned long)NETWORKD_GROUP_GID)
		scan->gid_records++;

	/* Reports successful completion. */
	return 0;
}

/* Ensures that name and numeric identity each have one unambiguous record. */
static int
validate_network_group_database(
	void)
{
	char database[NETWORKD_GROUP_DATABASE_MAX + 1U];
	struct networkd_group_scan scan;
	char *line;
	char *newline;
	char extra;
	size_t used;
	size_t start;
	size_t length;
	ssize_t count;
	int descriptor;
	int error;

	used = 0;
	error = 0;
	descriptor = open(NETWORKD_GROUP_FILE, O_RDONLY | O_CLOEXEC);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;
	while (used < NETWORKD_GROUP_DATABASE_MAX) {
		count = read(descriptor, database + used,
			     NETWORKD_GROUP_DATABASE_MAX - used);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count < 0) {
			error = errno;
			break;
		}

		/* Checks the remaining item count. */
		if (count == 0)
			break;
		used += (size_t)count;
	}

	/* Handles an operation failure. */
	if (error == 0 && used == NETWORKD_GROUP_DATABASE_MAX) {
		do {
			count = read(descriptor, &extra, 1U);
		} while (count < 0 && errno == EINTR);

		/* Checks the remaining item count. */
		if (count != 0)
			error = count < 0 ? errno : E2BIG;
	}

	/* Handles an operation failure. */
	if (close(descriptor) != 0 && error == 0)
		error = errno;

	/* Handles an operation failure. */
	if (error != 0) {
		errno = error;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed memchr operation. */
	if (memchr(database, '\0', used) != NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	database[used] = '\0';
	memset(&scan, 0, sizeof(scan));
	for (start = 0; start < used;) {
		line = database + start;
		newline = memchr(line, '\n', used - start);
		length = newline != NULL ? (size_t)(newline - line)
					 : used - start;
		line[length] = '\0';

		/* Checks the current data length. */
		if (length != 0 && line[length - 1U] == '\r')
			line[length - 1U] = '\0';

		/* Handles a failed scan group record operation. */
		if (scan_group_record(line, &scan) != 0) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
		start += length + (newline != NULL ? 1U : 0U);
	}

	/* Handles the scan condition. */
	if (scan.network_records != 1U || scan.gid_records != 1U) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Resolves the fixed network operator group without accepting ambiguity. */
static int
resolve_network_group(
	gid_t *result)
{
	char buffer[NETWORKD_GROUP_BUFFER_MAX];
	struct group storage;
	struct group *entry;
	int error;

	entry = NULL;

	/* Handles the result availability. */
	if (result == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	error = getgrnam_r(NETWORKD_GROUP_NAME, &storage, buffer,
			   sizeof(buffer), &entry);

	/* Handles an operation failure. */
	if (error != 0) {
		errno = error;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the entry availability. */
	if (entry == NULL) {
		errno = ENOENT;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the gr name availability. */
	if (entry->gr_name == NULL ||
	    strcmp(entry->gr_name, NETWORKD_GROUP_NAME) != 0 ||
	    entry->gr_gid != NETWORKD_GROUP_GID) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed validate network group database operation. */
	if (validate_network_group_database() != 0)
		return -1;
	*result = entry->gr_gid;
	/* Reports successful completion. */
	return 0;
}

/* Checks whether the public pathname still names this listener instance. */
static int
listener_path_matches(
	const struct networkd_listener *listener)
{
	int function_result;
	struct stat status;

	/* Handles the listener availability. */
	if (listener == NULL || !listener->owns_path)
		return 0;

	/* Handles a failed lstat operation. */
	if (lstat(NETWORKD_SOCKET, &status) != 0)
		return 0;

	/* Computes the function result. */
	function_result = S_ISSOCK(status.st_mode) && status.st_dev == listener->device &&
	       status.st_ino == listener->inode;

	/* Returns the computed result. */
	return function_result;
}

/* Closes a listener without unlinking a pathname replaced by another owner. */
static int
close_listener(
	struct networkd_listener *listener)
{
	struct stat status;
	int error;

	error = 0;

	/* Handles the listener availability. */
	if (listener == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed close operation. */
	if (listener->descriptor >= 0 && close(listener->descriptor) != 0)
		error = errno;
	listener->descriptor = -1;

	/* Handles the listener condition. */
	if (listener->owns_path) {
		/* Handles the listener path matches condition. */
		if (listener_path_matches(listener)) {
			/* Handles an operation failure. */
			if (unlink(NETWORKD_SOCKET) != 0 && error == 0)
				error = errno;
		} else if (lstat(NETWORKD_SOCKET, &status) == 0 && error == 0) {
			/*
			 * A replacement endpoint belongs to another daemon
			 * instance and must never be removed here.
			 */
			error = EBUSY;
		}
	}
	listener->owns_path = 0;

	/* Handles an operation failure. */
	if (error != 0) {
		errno = error;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Removes a stale socket, but refuses to replace a non-socket object. */
static int
remove_stale_listener(
	void)
{
	int function_result;
	struct stat status;

	/* Handles a failed lstat operation. */
	if (lstat(NETWORKD_SOCKET, &status) != 0)
		return errno == ENOENT ? 0 : -1;

	/* Handles a failed S ISSOCK operation. */
	if (!S_ISSOCK(status.st_mode)) {
		errno = EEXIST;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the unlink result. */
	function_result = unlink(NETWORKD_SOCKET);

	/* Returns the computed result. */
	return function_result;
}

/* Creates the group-accessible network control listener. */
static int
open_listener(
	struct networkd_listener *listener)
{
	struct sockaddr_un address;
	struct stat status;
	mode_t old_mask;
	gid_t group;
	int saved;

	/* Handles the listener availability. */
	if (listener == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	memset(listener, 0, sizeof(*listener));
	listener->descriptor = -1;
	listener->stage = "resolve-group";

	/* Handles a failed resolve network group operation. */
	if (resolve_network_group(&group) != 0)
		return -1;
	listener->stage = "remove-stale";

	/* Handles a failed remove stale listener operation. */
	if (remove_stale_listener() != 0)
		return -1;
	listener->stage = "socket";
	listener->descriptor =
	    socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	/* Handles the listener condition. */
	if (listener->descriptor < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, NETWORKD_SOCKET);
	listener->stage = "bind";
	old_mask = umask(0177);

	/* Handles a failed bind operation. */
	if (bind(listener->descriptor, (struct sockaddr *)&address,
		 sizeof(address)) != 0) {
		saved = errno;
		(void)umask(old_mask);
		errno = saved;
		goto fail;
	}
	(void)umask(old_mask);
	listener->stage = "identify";

	/* Handles a failed lstat operation. */
	if (lstat(NETWORKD_SOCKET, &status) != 0)
		goto fail;

	/* Handles a failed S ISSOCK operation. */
	if (!S_ISSOCK(status.st_mode)) {
		errno = EINVAL;
		goto fail;
	}
	listener->device = status.st_dev;
	listener->inode = status.st_ino;
	listener->owns_path = 1;
	listener->stage = "owner";

	/* Handles a failed lchown operation. */
	if (lchown(NETWORKD_SOCKET, 0, group) != 0)
		goto fail;
	listener->stage = "mode";

	/* Handles a failed chmod operation. */
	if (chmod(NETWORKD_SOCKET, 0660) != 0)
		goto fail;
	listener->stage = "verify";

	/* Handles a failed lstat operation. */
	if (lstat(NETWORKD_SOCKET, &status) != 0)
		goto fail;

	/* Handles a failed S ISSOCK operation. */
	if (!S_ISSOCK(status.st_mode) || status.st_dev != listener->device ||
	    status.st_ino != listener->inode || status.st_uid != 0 ||
	    status.st_gid != group || (status.st_mode & 07777U) != 0660U) {
		errno = EINVAL;
		goto fail;
	}
	listener->stage = "listen";

	/* Handles a failed listen operation. */
	if (listen(listener->descriptor, 8) != 0)
		goto fail;
	listener->stage = "ready";

	/* Reports successful completion. */
	return 0;

fail:
	saved = errno != 0 ? errno : EIO;
	(void)close_listener(listener);
	errno = saved;

	/* Reports operation failure. */
	return -1;
}

/* Opens the fixed read-only interface-event stream. */
static int
open_route_events(
	void)
{
	int descriptor;

	/* Subscribes before networkd publishes readiness or snapshots links. */
	descriptor = socket(PF_ROUTE,
	    SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	/* Returns the descriptor or the socket error unchanged. */
	return descriptor;
}

/* Drains every currently queued fixed-width interface event. */
static int
process_route_events(
	void)
{
	struct rtm_ifinfo event;
	ssize_t count;
	int saved;

	/* Consumes complete packet records until the nonblocking queue is empty. */
	for (;;) {
		count = read(route_events, &event, sizeof(event));
		if (count == (ssize_t)sizeof(event)) {
			if (event.rtm_sequence > route_event_sequence)
				route_event_sequence = event.rtm_sequence;
			process_route_event(&event);

			/*
			 * An interface coming up, an address arriving or a
			 * route changing is what a watcher is watching for.
			 */
			notify_state_changed();
			continue;
		}
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		if (count == 0)
			saved = EPIPE;
		else if (count > 0)
			saved = EINVAL;
		else
			saved = errno != 0 ? errno : EIO;
		/* Losing the event channel cannot recursively run policy teardown. */
		if (managed_wlan.connection.interface[0] != '\0') {
			if (managed_wlan.state != NETWORKD_WLAN_RETIRING)
				(void)networkd_managed_wlan_begin_retire(&managed_wlan,
				    NETWORKD_WLAN_AUTO_SEARCHING);
			schedule_automatic_work(0U);
		}
		errno = saved;
		return -1;
	}
}

/*
 * Supports the lan interface name operation.
 *
 * Names the interface an event was about.  An event carries the index
 * rather than the name, and the index is all that is left once a device has
 * gone, so this answers only for one that is still there.
 */
static int
lan_interface_name(
	uint32_t ifindex,
	char *output,
	size_t capacity)
{
	char name[IFNAMSIZ];
	int descriptor;
	int result;

	/* Rejects a buffer that could not hold a name. */
	if (output == NULL || capacity < sizeof(name))
		return -1;
	descriptor = socket(AF_INET, SOCK_DGRAM, 0);

	/* Handles a failed socket operation. */
	if (descriptor < 0)
		return -1;
	result = netutil_ifname(descriptor, ifindex, name);
	(void)close(descriptor);

	/* Handles an index that names nothing. */
	if (result != 0)
		return -1;
	strncpy(output, name, capacity - 1U);
	output[capacity - 1U] = '\0';

	/* Reports successful completion. */
	return 0;
}

/*
 * Supports the lan policy decode operation.
 *
 * Reads the wired policy out of a request.  It is carried as one text
 * record per interface, because the decoder the other operations share
 * admits one interface and no more, and what is described here is every
 * interface at once.
 */
static int
lan_policy_decode(
	const struct networkd_request *request,
	struct networkd_lan_policy *policy,
	size_t capacity,
	size_t *count)
{
	struct networkd_field_reader reader;
	struct networkd_field field;
	char record[128];
	char *word[5];
	char *token;
	unsigned words;
	int result;

	*count = 0U;
	networkd_field_reader_init(&reader, request->payload,
	    request->header.payload_length);

	/* Continue while the operation condition remains true. */
	while ((result = networkd_field_read(&reader, &field)) == 0) {
		/* Rejects anything that is not one of the records. */
		if (field.type != NETWORKD_FIELD_OUTPUT ||
		    field.length == 0U || field.length >= sizeof(record) ||
		    memchr(field.value, '\0', field.length) != NULL) {
			errno = EINVAL;
			return -1;
		}

		/* Rejects more interfaces than can be held. */
		if (*count == capacity) {
			errno = EOVERFLOW;
			return -1;
		}
		memcpy(record, field.value, field.length);
		record[field.length] = '\0';
		words = 0U;
		for (token = strtok(record, " "); token != NULL;
		    token = strtok(NULL, " ")) {
			if (words == sizeof(word) / sizeof(word[0])) {
				errno = EINVAL;
				return -1;
			}
			word[words++] = token;
		}

		/* Every record names an interface and what is asked of it. */
		if (words < 2U || strlen(word[0]) >= IFNAMSIZ) {
			errno = EINVAL;
			return -1;
		}
		memset(&policy[*count], 0, sizeof(policy[*count]));
		strncpy(policy[*count].interface, word[0],
			sizeof(policy[*count].interface) - 1U);

		/* Dispatch the selected kind of record. */
		if (strcmp(word[1], "dhcp") == 0 && words == 3U) {
			policy[*count].mode = NETWORKD_LAN_MODE_DHCP;
			policy[*count].dhcp_timeout =
			    (unsigned)strtoul(word[2], NULL, 10);
			if (policy[*count].dhcp_timeout == 0U ||
			    policy[*count].dhcp_timeout > 3600U) {
				errno = EINVAL;
				return -1;
			}
		} else if (strcmp(word[1], "static") == 0 && words == 4U) {
			policy[*count].mode = NETWORKD_LAN_MODE_STATIC;
			if (strlen(word[2]) >= sizeof(policy[*count].address) ||
			    strlen(word[3]) >= sizeof(policy[*count].netmask)) {
				errno = EINVAL;
				return -1;
			}
			strcpy(policy[*count].address, word[2]);
			strcpy(policy[*count].netmask, word[3]);
		} else if (strcmp(word[1], "disabled") == 0 && words == 2U) {
			policy[*count].mode = NETWORKD_LAN_MODE_DISABLED;
		} else {
			errno = EINVAL;
			return -1;
		}
		(*count)++;
	}

	/* Handles a payload that ended in the middle of a record. */
	if (result < 0)
		return -1;

	/* Reports successful completion. */
	return 0;
}

/*
 * Supports the lan snapshot operation.
 *
 * Reads every interface and tells the policy which of them are there and
 * which have a cable.  The running flag is what the kernel sets from the
 * carrier, so it is the same question asked a different way; asking it
 * outright is what an event that was lost leaves as the only option.
 */
static void
lan_snapshot(
	void)
{
	struct ifreq *list;
	unsigned count;
	unsigned index;
	uint32_t ifindex;
	int descriptor;
	int flags;
	int radio;

	descriptor = socket(AF_INET, SOCK_DGRAM, 0);

	/* Handles a failed socket operation. */
	if (descriptor < 0)
		return;
	list = NULL;
	count = 0U;

	/* Handles a failed enumeration, which leaves the snapshot for later. */
	if (netutil_interfaces(descriptor, &list, &count) != 0) {
		(void)close(descriptor);
		return;
	}
	networkd_lan_snapshot_begin(&managed_lan);

	/* Process each remaining element. */
	for (index = 0U; index < count; index++) {
		flags = 0;
		ifindex = 0U;

		/* Handles an interface that cannot be asked about. */
		if (interface_flags(list[index].ifr_name, &flags) != 0 ||
		    interface_index(list[index].ifr_name, &ifindex) != 0)
			continue;

		/* The loopback is nobody's cable and is never managed. */
		if ((flags & IFF_LOOPBACK) != 0)
			continue;

		/*
		 * A Wi-Fi radio is the Wi-Fi policy's: its carrier follows the
		 * association, and the wired policy would run DHCP on it when
		 * it joins and take it down when it moves to another network.
		 */
		radio = lan_interface_is_radio(descriptor, list[index].ifr_name,
		    ifindex);
		if (radio)
			continue;
		(void)networkd_lan_observe(&managed_lan, list[index].ifr_name,
					   ifindex, 0U,
					   (flags & IFF_RUNNING) != 0);
	}
	networkd_lan_snapshot_end(&managed_lan);
	free(list);
	(void)close(descriptor);
}

/*
 * Tells whether an interface is a Wi-Fi radio.
 *
 * An interface that answers the WLAN status request is one, and so is one
 * already known as a radio.  Only an interface that says it does not know
 * the request is taken for a cable: any other failure cannot prove that an
 * interface is not a radio, and a radio taken for a cable would be taken
 * down by the wired policy.
 */
static int
lan_interface_is_radio(
	int descriptor,
	const char *name,
	uint32_t ifindex)
{
	struct wlan_status_request status;
	size_t known;
	int error;
	int differs;

	/* Asks the interface for its WLAN status. */
	memset(&status, 0, sizeof(status));
	(void)snprintf(status.ifr_name, sizeof(status.ifr_name), "%s", name);
	status.version = WLAN_ABI_VERSION;
	status.size = sizeof(status);
	error = ioctl(descriptor, SIOCGWLANSTATUS, &status);
	if (error == 0)
		return 1;

	/* Counts a radio seen before, whatever it answers now. */
	for (known = 0U; known < known_wlan_radio_count; known++) {
		if (known_wlan_radios[known].ifindex != ifindex)
			continue;
		differs = strcmp(known_wlan_radios[known].interface, name);
		if (differs == 0)
			return 1;
	}

	/* An interface that does not know the request is a cable. */
	if (errno == EOPNOTSUPP || errno == ENOTTY)
		return 0;

	/* Any other failure leaves the interface to the Wi-Fi policy. */
	return 1;
}

/*
 * Supports the lan address usable operation.
 *
 * Reports whether an interface holds an address a machine can be reached
 * at.  A link-local address does not count: it is what is given to an
 * interface that could not be configured, so counting it would make the
 * failure look like a success.
 */
static int
lan_address_usable(
	const char *name)
{
	struct ifreq request;
	uint32_t value;
	int descriptor;
	int usable;

	descriptor = socket(AF_INET, SOCK_DGRAM, 0);

	/* Handles a failed socket operation. */
	if (descriptor < 0)
		return 0;
	memset(&request, 0, sizeof(request));
	strncpy(request.ifr_name, name, sizeof(request.ifr_name) - 1U);
	usable = 0;

	/* Handles an interface that holds no address at all. */
	if (ioctl(descriptor, SIOCGIFADDR, &request) == 0 &&
	    request.ifr_addr.sa_family == AF_INET) {
		value = ntohl(((struct sockaddr_in *)(void *)
		    &request.ifr_addr)->sin_addr.s_addr);
		usable = value != 0U && (value >> 24) != 127U &&
			 (value >> 16) != 0xa9feU;
	}
	(void)close(descriptor);

	/* Returns the computed result. */
	return usable;
}

/*
 * Supports the lan assign link local operation.
 *
 * Gives an interface the address that says it has none.  A machine whose
 * lease was not answered is still reachable by whoever is on the same
 * cable, and an address out of the link-local range says, to anything that
 * looks, exactly how it was arrived at.
 */
static void
lan_assign_link_local(
	const char *name,
	uint64_t deadline)
{
	char diagnostic[CHILD_OUTPUT_MAX];
	char address[NETWORKD_LAN_ADDRESS_MAX];
	struct ifreq request;
	char *arguments[7];
	int descriptor;

	descriptor = socket(AF_INET, SOCK_DGRAM, 0);

	/* Handles a failed socket operation. */
	if (descriptor < 0)
		return;
	memset(&request, 0, sizeof(request));
	strncpy(request.ifr_name, name, sizeof(request.ifr_name) - 1U);

	/* Handles hardware that will not say what its address is. */
	if (ioctl(descriptor, SIOCGIFHWADDR, &request) != 0) {
		(void)close(descriptor);
		return;
	}
	(void)close(descriptor);

	/* Handles an address that could not be derived. */
	if (networkd_lan_link_local(request.ifr_hwaddr,
				    sizeof(request.ifr_hwaddr), address,
				    sizeof(address)) != 0)
		return;
	diagnostic[0] = '\0';
	arguments[0] = (char *)"/sbin/ifconfig";
	arguments[1] = (char *)name;
	arguments[2] = (char *)"inet";
	arguments[3] = address;
	arguments[4] = (char *)"netmask";
	arguments[5] = (char *)"255.255.0.0";
	arguments[6] = NULL;
	(void)run_command_until(arguments, 10U, deadline, diagnostic);
}

/*
 * Supports the lan configure operation.
 *
 * Brings one interface up and gives it what the configuration asks for.
 * Whether that succeeded is answered by looking at the interface rather
 * than at what the command returned: a lease that was refused and a lease
 * that was never answered leave the same interface behind.
 */
static void
lan_configure(
	const struct networkd_lan_work *work)
{
	char diagnostic[CHILD_OUTPUT_MAX];
	char seconds[16];
	char *arguments[7];
	uint64_t deadline;
	int obtained;

	deadline = netutil_monotonic_us() +
	    (uint64_t)(work->policy.dhcp_timeout + 30U) * 1000000ULL;
	diagnostic[0] = '\0';
	arguments[0] = (char *)"/sbin/ifconfig";
	arguments[1] = (char *)work->interface;
	arguments[2] = (char *)"up";
	arguments[3] = NULL;

	/* Handles an interface that will not come up at all. */
	if (run_command_until(arguments, 10U, deadline, diagnostic) != 0) {
		(void)networkd_lan_configured(&managed_lan, work->interface, 0);
		notify_state_changed();
		return;
	}

	/* Dispatch the selected kind of configuration. */
	if (work->policy.mode == NETWORKD_LAN_MODE_STATIC) {
		diagnostic[0] = '\0';
		arguments[0] = (char *)"/sbin/ifconfig";
		arguments[1] = (char *)work->interface;
		arguments[2] = (char *)"inet";
		arguments[3] = (char *)work->policy.address;
		arguments[4] = (char *)"netmask";
		arguments[5] = (char *)work->policy.netmask;
		arguments[6] = NULL;
		(void)run_command_until(arguments, 10U, deadline, diagnostic);
	} else {
		(void)snprintf(seconds, sizeof(seconds), "%u",
			       work->policy.dhcp_timeout);
		diagnostic[0] = '\0';
		arguments[0] = (char *)"/sbin/dhcpc";
		arguments[1] = (char *)"-t";
		arguments[2] = seconds;
		arguments[3] = (char *)work->interface;
		arguments[4] = NULL;
		(void)run_command_until(arguments,
		    work->policy.dhcp_timeout + 5U, deadline, diagnostic);
	}
	obtained = lan_address_usable(work->interface);

	/* An interface that obtained nothing is told to say so. */
	if (!obtained)
		lan_assign_link_local(work->interface, deadline);
	(void)networkd_lan_configured(&managed_lan, work->interface, obtained);

	/* Keeps a DHCP lease's route and resolver for the network preference. */
	lan_l3_forget(work->interface, 0);
	if (obtained && work->policy.mode == NETWORKD_LAN_MODE_DHCP)
		lan_l3_record(work->interface);
	apply_network_preference();
	notify_state_changed();
}

/*
 * Supports the lan take down operation.
 */
static void
lan_take_down(
	const char *name)
{
	char diagnostic[CHILD_OUTPUT_MAX];
	char *arguments[4];

	/* Says in the log when a cable went, so that what follows (the Wi-Fi taking the default) can be read against it (BUG-212). */
	fprintf(stderr, "networkd: %s: cable out; taking it down\n", name);

	/* Takes the interface down. */
	diagnostic[0] = '\0';
	arguments[0] = (char *)"/sbin/ifconfig";
	arguments[1] = (char *)name;
	arguments[2] = (char *)"down";
	arguments[3] = NULL;
	(void)run_command_until(arguments, 10U,
	    netutil_monotonic_us() + 15000000ULL, diagnostic);
	(void)networkd_lan_down(&managed_lan, name);

	/* Gives up the address the configuration gave, and its subnet's route (BUG-212, BUG-213). */
	lan_clear_address(name);

	/* Withdraws the lease's route and lets another interface carry the default. */
	lan_l3_forget(name, 1);
	apply_network_preference();
	notify_state_changed();
}

/*
 * Takes the IPv4 address off a wired interface whose cable went.
 *
 * The kernel keeps an address, and the route to its subnet, on an interface
 * that is down, and the interface is raised again at once to see its next
 * cable.  Left there, the address told the user an interface without a cable
 * still had one (BUG-213), and the subnet's route stayed in the table: when
 * the Wi-Fi is on the same network, the kernel kept sending the network's
 * packets to the dead cable rather than to the radio (BUG-212).  The next
 * cable configures the interface again, from the lease or from the static
 * configuration, so nothing is lost by taking it off.  The netmask goes first,
 * which is what removes the subnet's route; then the broadcast and the address.
 */
static void
lan_clear_address(
	const char *name)
{
	uint32_t address;
	int descriptor;
	int error;

	/* A socket for the interface requests. */
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return;

	/* An interface without an address has nothing to give up. */
	address = 0U;
	error = get_interface_ipv4(descriptor, name, SIOCGIFADDR, &address);
	if (error != 0 || address == 0U) {
		(void)close(descriptor);
		return;
	}

	/* Removes the subnet's route with the netmask. */
	error = set_interface_ipv4(descriptor, name, SIOCSIFNETMASK, 0U);
	if (error != 0)
		fprintf(stderr, "networkd: %s: netmask not cleared: %s\n", name, strerror(errno));

	/* Clears the broadcast address. */
	error = set_interface_ipv4(descriptor, name, SIOCSIFBRDADDR, 0U);
	if (error != 0)
		fprintf(stderr, "networkd: %s: broadcast not cleared: %s\n", name, strerror(errno));

	/* Clears the address itself. */
	error = set_interface_ipv4(descriptor, name, SIOCSIFADDR, 0U);
	if (error != 0)
		fprintf(stderr, "networkd: %s: address not cleared: %s\n", name, strerror(errno));
	(void)close(descriptor);
}

/*
 * Keeps the default route and resolver one wired interface's DHCP gave.
 *
 * dhcpc installs both itself; the record lets the network preference move
 * the default between interfaces afterwards.  The resolver is kept only when
 * dhcpc wrote it for this interface, which its first line says.
 */
static void
lan_l3_record(
	const char *interface)
{
	struct networkd_managed_l3 snapshot;
	struct networkd_lan_l3 *record;
	char marker[80];
	uint32_t ifindex;
	size_t index;
	int differs;
	int length;
	int error;

	/* Reads what the interface holds now. */
	memset(&snapshot, 0, sizeof(snapshot));
	error = snapshot_interface_l3(interface, &ifindex, &snapshot);
	if (error != 0)
		return;

	/* Reuses the interface's slot, or takes a free one. */
	record = lan_l3_find(interface);
	for (index = 0U; record == NULL && index < NETWORKD_LAN_MAX; index++) {
		if (lan_l3[index].interface[0] == '\0')
			record = &lan_l3[index];
	}
	if (record == NULL) {
		networkd_protocol_clear(&snapshot, sizeof(snapshot));
		return;
	}

	/* Records the route the lease installed, if it gave a router. */
	memset(record, 0, sizeof(*record));
	(void)snprintf(record->interface, sizeof(record->interface), "%s",
	    interface);
	record->ifindex = ifindex;
	record->route_present = snapshot.default_route_present;
	record->route = snapshot.default_route;

	/* Tells whether the resolver file is large enough to carry the marker. */
	length = snprintf(marker, sizeof(marker),
	    "# Generated by dhcpc for %s\n", interface);
	differs = 1;
	if (length > 0 &&
	    (size_t)length < sizeof(marker) &&
	    snapshot.resolver_present &&
	    !snapshot.resolver_oversized &&
	    snapshot.resolver_length >= (size_t)length)
		differs = memcmp(snapshot.resolver, marker, (size_t)length);

	/* Records the resolver only when dhcpc wrote it for this interface. */
	if (differs == 0) {
		memcpy(record->resolver, snapshot.resolver,
		    snapshot.resolver_length);
		record->resolver_length = snapshot.resolver_length;
	}
	networkd_protocol_clear(&snapshot, sizeof(snapshot));
}

/*
 * Forgets one wired interface's lease.
 *
 * When the interface is taken down its route is withdrawn too: the kernel
 * keeps a route through a device that is down, and such a route could still
 * be chosen ahead of a working interface.
 */
static void
lan_l3_forget(
	const char *interface,
	int withdraw)
{
	struct networkd_lan_l3 *record;
	int descriptor;

	/* Nothing is kept for an interface without a record. */
	record = lan_l3_find(interface);
	if (record == NULL)
		return;

	/* Withdraws the recorded route when asked, if it is still there. */
	if (withdraw && record->route_present) {
		descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (descriptor >= 0) {
			(void)withdraw_route(descriptor, &record->route);
			(void)close(descriptor);
		}
	}

	/* Frees the slot. */
	networkd_protocol_clear(record, sizeof(*record));
}

/*
 * Notes that the device of a wired lease has gone.
 *
 * The name is all that is left to find the lease by, and the device can no
 * longer be asked for it, so it is taken from the record kept by index.
 * The work is done at the next turn of the loop, not while events are read.
 */
static void
lan_l3_removed(
	uint32_t ifindex)
{
	size_t index;

	/* Looks for the lease of the device that has gone. */
	for (index = 0U; index < NETWORKD_LAN_MAX; index++) {
		if (lan_l3[index].interface[0] == '\0')
			continue;
		if (lan_l3[index].ifindex != ifindex)
			continue;
		(void)snprintf(lan_removed_pending, sizeof(lan_removed_pending),
		    "%s", lan_l3[index].interface);
		lan_work_due = 1;
		return;
	}
}

/* Finds the lease record of one wired interface. */
static struct networkd_lan_l3 *
lan_l3_find(
	const char *interface)
{
	size_t index;
	int differs;

	/* Looks through the occupied slots for the interface's name. */
	for (index = 0U; index < NETWORKD_LAN_MAX; index++) {
		if (lan_l3[index].interface[0] == '\0')
			continue;
		differs = strcmp(lan_l3[index].interface, interface);
		if (differs == 0)
			return &lan_l3[index];
	}

	/* The interface has no record. */
	return NULL;
}

/* Tests whether a wired lease may carry the default route now. */
static int
lan_l3_eligible(
	const struct networkd_lan_l3 *record)
{
	size_t index;
	int differs;

	/* A lease without a router cannot carry the default route. */
	if (!record->route_present)
		return 0;

	/* Only an interface configured with its cable in is eligible. */
	for (index = 0U; index < managed_lan.interface_count; index++) {
		differs = strcmp(managed_lan.interfaces[index].name,
		    record->interface);
		if (differs != 0)
			continue;
		if (managed_lan.interfaces[index].state != NETWORKD_LAN_CONFIGURED)
			return 0;
		if (!managed_lan.interfaces[index].carrier)
			return 0;
		return 1;
	}

	/* An interface the daemon no longer sees is not eligible. */
	return 0;
}

/*
 * Decides which interface carries the default route and the resolver.
 *
 * The user decided on 2026-10-02 (ws005-p019, B3) that a wired connection
 * is preferred to Wi-Fi.  dhcpc still installs each lease's route and writes
 * the resolver as it finishes, so the last lease would otherwise win; this
 * one place puts the preferred interface's route and resolver in effect and
 * withdraws every other default this daemon knows of.  The first configured
 * wired interface with a router is preferred, then a Wi-Fi connection.
 * Routes this daemon did not record (an administrator's) are left alone.
 */
static void
apply_network_preference(
	void)
{
	const struct networkd_managed_l3 *wifi;
	struct networkd_lan_l3 *chosen;
	size_t index;
	int wifi_route;
	int descriptor;
	int eligible;

	/* Finds the first wired lease that may carry the default route. */
	chosen = NULL;
	for (index = 0U; chosen == NULL && index < NETWORKD_LAN_MAX; index++) {
		if (lan_l3[index].interface[0] == '\0')
			continue;
		eligible = lan_l3_eligible(&lan_l3[index]);
		if (eligible)
			chosen = &lan_l3[index];
	}

	/* Tells whether the Wi-Fi connection holds a route of its own lease. */
	wifi = &managed_wlan.connection.l3;
	wifi_route = 0;
	if (managed_wlan.connection.interface[0] != '\0' &&
	    managed_wlan.connection.owns_l3 &&
	    !managed_wlan.connection.l3_pending &&
	    managed_wlan.state != NETWORKD_WLAN_RETIRING &&
	    managed_wlan.state != NETWORKD_WLAN_DISABLED &&
	    wifi->default_route_present &&
	    wifi->default_route_owned)
		wifi_route = 1;

	/* Nothing to choose between. */
	if (chosen == NULL && !wifi_route)
		return;
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return;

	/* Withdraws the default of every wired lease that is not the chosen one. */
	for (index = 0U; index < NETWORKD_LAN_MAX; index++) {
		if (lan_l3[index].interface[0] == '\0')
			continue;
		if (&lan_l3[index] == chosen || !lan_l3[index].route_present)
			continue;
		(void)withdraw_route(descriptor, &lan_l3[index].route);
	}

	/* A wired lease is preferred: Wi-Fi gives up its default and resolver. */
	if (chosen != NULL) {
		if (wifi_route)
			(void)withdraw_route(descriptor, &wifi->default_route);
		(void)prefer_route(descriptor, &chosen->route);
		if (chosen->resolver_length != 0U)
			(void)prefer_resolver(chosen->resolver, chosen->resolver_length);
		fprintf(stderr, "networkd: default route and resolver: %s (wired preferred)\n",
		    chosen->interface);
	} else {
		(void)prefer_route(descriptor, &wifi->default_route);
		if (wifi->resolver_owned && wifi->resolver_length != 0U)
			(void)prefer_resolver(wifi->resolver, wifi->resolver_length);
		fprintf(stderr, "networkd: default route and resolver: %s (Wi-Fi)\n",
		    managed_wlan.connection.interface);
	}
	(void)close(descriptor);

	/* Name servers a wired configuration named stay the resolver's (ws089-p022). */
	lan_static_resolver();
}

/* Writes the name servers a wired configuration named, when there are any (ws089-p022). */
static void
lan_static_resolver(void)
{
	char *servers[NETWORKD_LAN_CONFIGURE_DNS_MAX];
	unsigned index;

	/* None named: the lease's stay. */
	if (lan_static_dns_count == 0U)
		return;

	/* The ones named. */
	for (index = 0U; index < lan_static_dns_count; index++)
		servers[index] = lan_static_dns[index];
	(void)write_resolver(servers, (int)lan_static_dns_count);
}

/* Installs one recorded default route unless it is already in the table. */
static int
prefer_route(
	int descriptor,
	const struct networkd_managed_route *route)
{
	struct rtentry entry;
	struct sockaddr_in *address;
	int present;
	int error;

	/* Looks for the route in the table. */
	present = route_present_exact(descriptor, route);
	if (present < 0)
		return -1;

	/* A route already in the table stays as it is. */
	if (present > 0)
		return 0;

	/* Rebuilds the route-table entry from the recorded fields. */
	memset(&entry, 0, sizeof(entry));
	entry.rt_flags = route->flags;
	entry.rt_ifindex = route->ifindex;
	address = (struct sockaddr_in *)&entry.rt_dst;
	address->sin_family = AF_INET;
	address->sin_addr.s_addr = route->destination;
	address = (struct sockaddr_in *)&entry.rt_gateway;
	address->sin_family = AF_INET;
	address->sin_addr.s_addr = route->gateway;
	address = (struct sockaddr_in *)&entry.rt_genmask;
	address->sin_family = AF_INET;
	address->sin_addr.s_addr = route->netmask;

	/* Adds it. */
	error = ioctl(descriptor, SIOCADDRT, &entry);
	if (error != 0)
		return -1;

	/* Succeeded: the route is in effect again. */
	return 0;
}

/* Removes one recorded default route if it is still in the table. */
static int
withdraw_route(
	int descriptor,
	const struct networkd_managed_route *route)
{
	int error;

	/* Deletes only the exact route; one already gone is not an error. */
	error = delete_interface_default_exact(descriptor, route);
	if (error != 0 && errno != ESTALE)
		return -1;

	/* Succeeded: the route is no longer in the table. */
	return 0;
}

/* Reports whether one recorded route is in the table (1), absent (0), or unreadable (-1). */
static int
route_present_exact(
	int descriptor,
	const struct networkd_managed_route *route)
{
	struct rtentry entry;
	unsigned ordinal;
	int error;
	int matches;

	/* Walks the route table until the kernel reports its end. */
	for (ordinal = 0U;; ordinal++) {
		memset(&entry, 0, sizeof(entry));
		entry.rt_index = ordinal;
		error = ioctl(descriptor, SIOCGRTENTRY, &entry);
		if (error != 0 && errno == ENOENT)
			break;
		if (error != 0)
			return -1;
		matches = route_matches_owned(&entry, route);
		if (matches)
			return 1;
	}

	/* The route is not in the table. */
	return 0;
}

/* Writes the preferred interface's resolver unless it is already in effect. */
static int
prefer_resolver(
	const unsigned char *resolver,
	size_t length)
{
	struct networkd_managed_l3 current;
	const char *temporary;
	ssize_t written;
	int descriptor;
	int differs;
	int error;

	/* Reads the resolver file in effect. */
	memset(&current, 0, sizeof(current));
	error = snapshot_resolver(&current);

	/* Compares it with the preferred bytes when the lengths agree. */
	differs = 1;
	if (error == 0 &&
	    current.resolver_present &&
	    !current.resolver_oversized &&
	    current.resolver_length == length)
		differs = memcmp(current.resolver, resolver, length);
	networkd_protocol_clear(&current, sizeof(current));

	/* A resolver file with the same bytes is left alone. */
	if (differs == 0)
		return 0;

	/* Writes the bytes to a file beside it. */
	temporary = "/etc/resolv.conf.networkd";
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (descriptor < 0)
		return -1;
	written = write(descriptor, resolver, length);
	error = close(descriptor);
	if (written < 0 || (size_t)written != length || error != 0) {
		(void)unlink(temporary);
		return -1;
	}

	/* Puts it in place in one step, so a reader sees one file or the other. */
	error = rename(temporary, "/etc/resolv.conf");
	if (error != 0) {
		(void)unlink(temporary);
		return -1;
	}

	/* Succeeded: the preferred interface's resolver is in effect. */
	return 0;
}

/*
 * Brings an interface up without configuring it, so that its driver can
 * report the link.  It is marked raised whatever happened, so that an
 * interface that will not come up is not tried in a loop.
 */
static void
lan_raise(
	const char *name)
{
	char diagnostic[CHILD_OUTPUT_MAX];
	char *arguments[4];
	struct timespec delay;
	unsigned attempt;
	int result;

	/*
	 * Runs ifconfig up, bounded like every other step.  An adapter plugged
	 * in while networkd runs is announced before its driver has finished
	 * attaching (the data interface's setting comes last), and an up in
	 * that moment is refused (ENETDOWN); it is tried again a few times
	 * before the interface is taken as raised (BUG-168).
	 */
	arguments[0] = (char *)"/sbin/ifconfig";
	arguments[1] = (char *)name;
	arguments[2] = (char *)"up";
	arguments[3] = NULL;
	delay.tv_sec = 0;
	delay.tv_nsec = LAN_RAISE_RETRY_NS;
	result = -1;
	for (attempt = 0U; attempt < LAN_RAISE_ATTEMPTS && result != 0; attempt++) {
		/* A pause before each try but the first. */
		if (attempt != 0U)
			(void)nanosleep(&delay, NULL);
		diagnostic[0] = '\0';
		result = run_command_until(arguments, 10U,
		    netutil_monotonic_us() + 15000000ULL, diagnostic);
	}

	/* Logged when every try failed; it counts as raised all the same (the next cable or removal starts again). */
	if (result != 0)
		fprintf(stderr, "networkd: %s up failed after %u tries: %s\n", name, LAN_RAISE_ATTEMPTS, diagnostic);
	(void)networkd_lan_raised(&managed_lan, name);
}

/*
 * Supports the run lan work operation.
 *
 * Carries out whatever the wired policy has decided, one interface at a
 * time, until it has decided nothing more.  Each step is bounded, so a
 * server that never answers holds up the interface it was asked about and
 * nothing else.
 */
static void
run_lan_work(
	void)
{
	struct networkd_lan_work work;
	unsigned steps;

	/* Handles the work that a lost event left. */
	if (lan_down_pending[0] != '\0') {
		lan_take_down(lan_down_pending);
		lan_down_pending[0] = '\0';
	}

	/*
	 * Forgets the lease of a wired device that has gone; the caller then
	 * decides the preference again without it.
	 */
	if (lan_removed_pending[0] != '\0') {
		lan_l3_forget(lan_removed_pending, 0);
		lan_removed_pending[0] = '\0';
		notify_state_changed();
	}

	/*
	 * The count bounds one turn of the loop rather than the work: an
	 * interface that is configured is not offered again, so the only way
	 * round twice is for something to have changed meanwhile, and the
	 * event that changed it will bring the daemon back here.
	 */
	for (steps = 0U; steps < NETWORKD_LAN_MAX; steps++) {
		/* Handles a policy that cannot say what to do. */
		if (networkd_lan_next(&managed_lan, &work) != 0)
			return;
		if (work.action == NETWORKD_LAN_ACTION_RESNAPSHOT) {
			lan_snapshot();
			continue;
		}
		if (work.action == NETWORKD_LAN_ACTION_RAISE) {
			lan_raise(work.interface);
			continue;
		}
		if (work.action != NETWORKD_LAN_ACTION_CONFIGURE)
			return;
		lan_configure(&work);
	}
}

/* Records event-driven intent; child work is owned only by the outer loop. */
static void
process_route_event(
	const struct rtm_ifinfo *event)
{
	struct networkd_managed_wlan_connection *connection;
	enum networkd_managed_wlan_action action;
	int flags;

	/*
	 * The wired policy sees every event too.  A carrier change means one
	 * thing to a radio and another to a cable, so each reads the event
	 * for itself rather than one deciding for both.  A wired device that
	 * has gone also gives up its lease's place in the network preference.
	 */
	if (event->rtm_transition == RTM_IFINFO_REMOVAL)
		lan_l3_removed(event->rtm_ifindex);

	/* Lets the wired policy decide what the event asks of a cable. */
	switch (networkd_lan_event(&managed_lan, event)) {
	case NETWORKD_LAN_ACTION_CONFIGURE:
	case NETWORKD_LAN_ACTION_RESNAPSHOT:
		lan_work_due = 1;
		break;
	case NETWORKD_LAN_ACTION_DOWN:
		/*
		 * Taking it down is done at the next turn of the loop, not
		 * here: this is called while events are being read, and
		 * running a command would stop them being read.
		 */
		if (lan_interface_name(event->rtm_ifindex, lan_down_pending,
				       sizeof(lan_down_pending)) == 0)
			lan_work_due = 1;
		break;
	default:
		break;
	}

	action = networkd_managed_wlan_event(&managed_wlan, event);
	connection = &managed_wlan.connection;

	/* Says in the log when the joined network's link went, which starts a reconnect (BUG-212). */
	if (action == NETWORKD_WLAN_ACTION_RECOVER)
		fprintf(stderr, "networkd: %s: Wi-Fi link lost; reconnecting\n", connection->interface);

	/* A manual teardown target survives carrier, overflow and removal events. */
	if (managed_wlan.state == NETWORKD_WLAN_RETIRING) {
		if (action == NETWORKD_WLAN_ACTION_RETIRE)
			connection->device_removed = 1;
		if (action != NETWORKD_WLAN_ACTION_NONE)
			schedule_automatic_work(0U);
		return;
	}
	if (action == NETWORKD_WLAN_ACTION_NONE) {
		if (managed_wlan.state == NETWORKD_WLAN_AUTO_SEARCHING)
			schedule_automatic_work(0U);
		return;
	}
	if (action == NETWORKD_WLAN_ACTION_RETIRE) {
		connection->device_removed = 1;
		(void)networkd_managed_wlan_begin_retire(&managed_wlan,
		    NETWORKD_WLAN_AUTO_SEARCHING);
		schedule_automatic_work(0U);
		return;
	}
	if (connection->interface[0] == '\0') {
		schedule_automatic_work(0U);
		return;
	}

	if (interface_index_name_matches(connection->ifindex,
	    connection->interface) == 0 &&
	    interface_flags(connection->interface, &flags) == 0 &&
	    (flags & IFF_UP) != 0) {
		if ((flags & IFF_RUNNING) != 0 && managed_connection_usable()) {
			networkd_managed_wlan_recovery_complete(&managed_wlan, 1);
			return;
		}
		if (action == NETWORKD_WLAN_ACTION_RECOVER) {
			schedule_automatic_work(0U);
			return;
		}
	}
	(void)networkd_managed_wlan_begin_retire(&managed_wlan,
	    NETWORKD_WLAN_AUTO_SEARCHING);
	schedule_automatic_work(0U);
}

/* Gives same-profile RF recovery the same cancellation and deadline contract. */
static void
recover_managed_wlan(
	void)
{
	if (managed_wlan.state != NETWORKD_WLAN_RECONNECTING)
		return;
	automatic_retry_at = 0U;
	wifi_work_begin(NETWORKD_OP_WIFI_CONNECT, 1);
	recover_managed_connection();
	wifi_work_end();
}

/* Retries only the retained teardown target, never a fresh RF connection. */
static void
retry_managed_retirement(
	void)
{
	enum networkd_managed_wlan_state target;
	struct networkd_wifi_request work;

	if (managed_wlan.state != NETWORKD_WLAN_RETIRING)
		return;
	automatic_retry_at = 0U;
	if (wifi_disable_pending) {
		memset(&work, 0, sizeof(work));
		wifi_work_begin(NETWORKD_OP_WIFI_DISABLE, 1);
		(void)wifi_request_stop(&work, 1);
		wifi_work_end();
		return;
	}
	target = managed_wlan.retire_target;
	wifi_work_begin(NETWORKD_OP_WIFI_DISCONNECT, 1);
	(void)retire_managed_connection(target, 1);
	wifi_work_end();

	/* Failed retirement already owns a backed-off retry; success may resume search. */
	if (managed_wlan.state == NETWORKD_WLAN_AUTO_SEARCHING)
		schedule_automatic_work(NETWORKD_WLAN_RESCAN_SECONDS);
}

/* Backs off persistent OS failures without abandoning an unresolved token. */
static void
schedule_retirement_retry(
	void)
{
	/* Retains a finite retry interval and allows explicit commands to retry sooner. */
	automatic_retry_at = netutil_monotonic_us() +
	    (uint64_t)retirement_retry_seconds * 1000000ULL;
	if (retirement_retry_seconds < 60U) {
		retirement_retry_seconds *= 2U;
		if (retirement_retry_seconds > 60U)
			retirement_retry_seconds = 60U;
	}
}

/* Runs one ordinary 30-second reconnect using a freshly loaded secret. */
static void
recover_managed_connection(
	void)
{
	struct networkd_wifi_child_result result;
	struct networkd_wlan_radio radios[NETWORKD_WLAN_RADIO_MAX];
	struct wifi_conf_model model;
	const struct wifi_conf_profile *profile;
	char diagnostic[WIFI_CONF_DIAGNOSTIC_MAX];
	char interface[IFNAMSIZ];
	unsigned char ssid[WLAN_SSID_MAX];
	size_t ssid_length;
	uid_t owner_uid;
	size_t radio_count;
	int flags;
	int succeeded;

	/* Snapshots only nonsecret identity before a fallible store load. */
	if (managed_wlan.state != NETWORKD_WLAN_RECONNECTING)
		return;
	memset(&result, 0, sizeof(result));
	memset(radios, 0, sizeof(radios));
	wifi_conf_model_init(&model);
	memset(diagnostic, 0, sizeof(diagnostic));
	memcpy(interface, managed_wlan.connection.interface,
	    sizeof(interface));
	ssid_length = managed_wlan.connection.ssid_length;
	memcpy(ssid, managed_wlan.connection.ssid, ssid_length);
	owner_uid = managed_wlan.owner_uid;

	/* The key is read again from the store the connection was made from. */
	if (wifi_connection_store_known)
		owner_uid = wifi_connection_store_uid;

	/* Reloads that store's exact current profile for this SSID. */
	succeeded = load_policy(owner_uid, &model, diagnostic,
	    sizeof(diagnostic)) == 0;
	profile = succeeded ? find_profile(&model, ssid, ssid_length) : NULL;
	if (profile == NULL)
		succeeded = 0;
	if (succeeded) {
		succeeded = run_wifi(interface, "connect", profile,
		    NETWORKD_WLAN_CONNECT_SECONDS, &result) == 0;
	}
	networkd_wifi_child_result_clear(&result);
	wifi_conf_model_clear(&model);
	wifi_conf_explicit_clear(diagnostic, sizeof(diagnostic));
	wifi_conf_explicit_clear(ssid, sizeof(ssid));

	/* Coalesces every event which arrived while the child owned recovery. */
	(void)process_route_events();
	if (managed_wlan.state != NETWORKD_WLAN_RECONNECTING)
		return;
	if (interface_flags(interface, &flags) != 0 ||
	    (flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING) ||
	    !managed_connection_usable())
		succeeded = 0;

	/* Retains L3 on success and returns failure to automatic searching. */
	if (succeeded) {
		networkd_managed_wlan_recovery_complete(&managed_wlan, 1);
	} else {
		fprintf(stderr, "networkd: %s: Wi-Fi reconnect failed; searching again\n", interface);
		if (retire_managed_connection(
		    NETWORKD_WLAN_AUTO_SEARCHING, 1) != 0)
			return;
		radio_count = 0U;
		if (enumerate_wlan_radios(radios, NETWORKD_WLAN_RADIO_MAX,
		    &radio_count) == 0) {
			(void)prepare_wlan_radios(radios, radio_count, NULL, 0U,
			    NULL);
		}
		schedule_automatic_work(NETWORKD_WLAN_RESCAN_SECONDS);
	}
	networkd_protocol_clear(radios, sizeof(radios));
}

/* Schedules one future automatic discovery generation without extending it. */
static void
schedule_automatic_work(
	unsigned delay_seconds)
{
	uint64_t candidate;
	uint64_t now;

	/* Keeps no background deadline outside the automatic-search state. */
	if (managed_wlan.state != NETWORKD_WLAN_AUTO_SEARCHING &&
	    managed_wlan.state != NETWORKD_WLAN_RECONNECTING &&
	    managed_wlan.state != NETWORKD_WLAN_RETIRING) {
		automatic_retry_at = 0U;
		return;
	}
	now = netutil_monotonic_us();
	candidate = now + (uint64_t)delay_seconds * 1000000ULL;
	if (automatic_retry_at == 0U || candidate < automatic_retry_at)
		automatic_retry_at = candidate;
}

/* Computes the event-loop wait until the next automatic generation. */
static int
automatic_poll_timeout(
	void)
{
	uint64_t milliseconds;
	uint64_t now;

	/* Disabled, connected, and manually disconnected policies do not wake. */
	if (managed_wlan.state != NETWORKD_WLAN_AUTO_SEARCHING &&
	    managed_wlan.state != NETWORKD_WLAN_RECONNECTING &&
	    managed_wlan.state != NETWORKD_WLAN_RETIRING) {
		automatic_retry_at = 0U;
		return -1;
	}
	if (automatic_retry_at == 0U)
		schedule_automatic_work(NETWORKD_WLAN_RESCAN_SECONDS);
	now = netutil_monotonic_us();
	if (automatic_retry_at <= now)
		return 0;
	milliseconds = (automatic_retry_at - now + 999ULL) / 1000ULL;
	if (milliseconds > (uint64_t)INT_MAX)
		return INT_MAX;
	return (int)milliseconds;
}

/* Runs one bounded automatic scan, selection, association, and DHCP attempt. */
static void
run_automatic_work(
	void)
{
	struct networkd_wlan_radio radios[NETWORKD_WLAN_RADIO_MAX];
	struct wifi_conf_model model;
	uid_t stores[WIFI_CONF_PROFILE_MAX];
	char diagnostic[WIFI_CONF_DIAGNOSTIC_MAX];
	size_t radio_count;
	uint64_t deadline;
	int no_candidate;
	int joined;
	int saved;
	int ready;
	int changed;

	/* Claims the current wakeup and initializes all secret-bearing storage. */
	if (managed_wlan.state != NETWORKD_WLAN_AUTO_SEARCHING) {
		automatic_retry_at = 0U;
		return;
	}
	automatic_retry_at = 0U;
	wifi_work_begin(NETWORKD_OP_WIFI_ENABLE, 1);
	memset(radios, 0, sizeof(radios));
	wifi_conf_model_init(&model);
	memset(diagnostic, 0, sizeof(diagnostic));
	radio_count = 0U;
	no_candidate = 0;

	/* Keeps an unconsumed completed scan available to the current profiles. */
	ready = enumerate_wlan_radios(radios, NETWORKD_WLAN_RADIO_MAX,
	    &radio_count) == 0 && radio_count != 0U;
	if (ready)
		ready = prepare_wlan_radios(radios, radio_count, NULL, 0U,
		    NULL) == 0;
	if (ready) {
		/*
		 * The candidates are the open sessions' stores, the policy
		 * owner's and the system's (ws005-p024), each profile with the
		 * store it came from.
		 */
		memset(stores, 0, sizeof(stores));
		load_candidates(&model, stores);

		/* One bounded wave of joins; a joined profile names its store. */
		deadline = wifi_work.deadline -
		    NETWORKD_WIFI_CLEANUP_SECONDS * 1000000ULL;
		joined = connect_automatic(radios, radio_count, &model, stores,
		    deadline, NULL, 0U, NULL, &no_candidate);
		if (joined == 0) {
			connection_store_note(&model, stores);
		} else if (!no_candidate && !wifi_work.cancelled &&
		    !wifi_work.profiles_changed) {
			saved = errno != 0 ? errno : EIO;
			fprintf(stderr, "networkd: automatic Wi-Fi attempt: %s\n",
			    strerror(saved));
		}
	}

	/* Releases credentials and retries only after a finite idle interval. */
	wifi_conf_model_clear(&model);
	wifi_conf_explicit_clear(diagnostic, sizeof(diagnostic));
	networkd_protocol_clear(radios, sizeof(radios));
	changed = wifi_work.profiles_changed;
	wifi_work_end();
	if (managed_wlan.state == NETWORKD_WLAN_AUTO_SEARCHING) {
		automatic_retry_at = 0U;
		schedule_automatic_work(changed ? 0U : NETWORKD_WLAN_RESCAN_SECONDS);
	}
}

/* Selects the earliest volatile networkd deadline. */
static int
event_poll_timeout(
	void)
{
	int automatic;
	int rollback;
	int scan;

	/* Wired work that is waiting is done before anything is waited for. */
	if (lan_work_due)
		return 0;
	automatic = networkd_confirmed_active(&confirmed) || sleep_state.asleep ? -1 :
	    automatic_poll_timeout();

	/* While asleep only the sleep's own end wakes the Wi-Fi side (ws052-p010). */
	scan = networkd_sleep_poll_timeout(&sleep_state, netutil_monotonic_us());
	if (scan >= 0 && (automatic < 0 || scan < automatic))
		automatic = scan;

	/* A scan a desktop asked for, or the end of its lease, wakes the loop too (ws089-p021). */
	scan = -1;
	if (!networkd_confirmed_active(&confirmed) && !sleep_state.asleep)
		scan = wifi_scan_poll_timeout();
	if (scan >= 0 && (automatic < 0 || scan < automatic))
		automatic = scan;
	/* A DHCPv6 run that is due wakes the loop too (ws130-p007). */
	scan = -1;
	if (!networkd_confirmed_active(&confirmed))
		scan = networkd_ipv6_poll_timeout();
	if (scan >= 0 && (automatic < 0 || scan < automatic))
		automatic = scan;
	rollback = networkd_confirmed_poll_timeout(&confirmed,
	    netutil_monotonic_us());
	if (automatic < 0)
		return rollback;
	if (rollback < 0)
		return automatic;
	return automatic < rollback ? automatic : rollback;
}

/* Executes every volatile deadline which is due at this event-loop turn. */
static void
run_confirmed_due(
	void)
{
	char diagnostic[NETWORKD_RESPONSE_MAX];
	int result;

	diagnostic[0] = '\0';
	result = networkd_confirmed_run_due(&confirmed, netutil_monotonic_us(),
	    rollback_execute, NULL, diagnostic, sizeof(diagnostic));
	if (result < 0) {
		fprintf(stderr, "networkd: confirmed rollback degraded: %s",
		    diagnostic[0] != '\0' ? diagnostic : "operation failed\n");
	} else if (result > 0) {
		fprintf(stderr, "networkd: confirmed rollback expired and ran\n");
	}
	networkd_protocol_clear(diagnostic, sizeof(diagnostic));
}

/* Executes confirmed expiry first, then any independent WLAN retry. */
static void
run_due_work(
	void)
{
	enum networkd_sleep_resume resume;
	int due;

	/* A sleep whose SLEEP_END never came ends by itself (ws052-p010). */
	due = networkd_sleep_due(&sleep_state, netutil_monotonic_us());
	if (due) {
		resume = networkd_sleep_end(&sleep_state);
		sleep_resume_later(resume, "safety");
	}

	/* The end of a sleep takes the recorded policy up again. */
	if (sleep_resume_due) {
		sleep_resume_due = 0U;
		sleep_resume(sleep_resume_pending, sleep_resume_via);
	}

	/* The wired work is done first: it is bounded and it is cheap. */
	if (lan_work_due) {
		lan_work_due = 0;
		run_lan_work();

		/* A wired interface that went away hands the default back (B3). */
		apply_network_preference();
	}

	/* A connection whose network was deleted from its store ends. */
	if (wifi_connection_recheck_due) {
		wifi_connection_recheck_due = 0;
		recheck_connection_profile();
	}
	run_confirmed_due();

	/* DHCPv6's Renew, or the information again (ws130-p007). */
	if (!networkd_confirmed_active(&confirmed))
		networkd_ipv6_run_due();
	if (!networkd_confirmed_active(&confirmed) && !sleep_state.asleep &&
	    automatic_poll_timeout() == 0) {
		if (managed_wlan.state == NETWORKD_WLAN_RETIRING)
			retry_managed_retirement();
		else if (managed_wlan.state == NETWORKD_WLAN_RECONNECTING)
			recover_managed_wlan();
		else
			run_automatic_work();
	}

	/* A scan a desktop asked for, while the radios are on and left unconnected (ws089-p021). */
	if (!networkd_confirmed_active(&confirmed) && !sleep_state.asleep &&
	    wifi_scan_poll_timeout() == 0)
		run_requested_scan();
}

/* Retires the sole connection while preserving its active policy owner. */
static int
retire_managed_connection(
	enum networkd_managed_wlan_state next_state,
	int normalize)
{
	struct networkd_managed_wlan_connection *connection;
	struct networkd_wifi_child_result result;
	const char *cleanup_stage;
	unsigned child_records;
	int child_terminal;
	int child_exit;
	int child_signal;
	int disconnect_error;
	int l3_error;
	int cleanup_error;

	/* Moves an already idle enabled policy directly to its requested state. */
	connection = &managed_wlan.connection;
	if (networkd_managed_wlan_begin_retire(&managed_wlan, next_state) != 0)
		return -1;
	if (connection->interface[0] == '\0')
		return networkd_managed_wlan_finish_connection(&managed_wlan,
		    next_state);
	if (!normalize || connection->device_removed)
		return retire_removed_connection(next_state);
	cleanup_error = 0;
	cleanup_stage = "identity";
	disconnect_error = 0;
	l3_error = 0;
	child_terminal = 0;
	child_exit = 0;
	child_signal = 0;
	child_records = 0U;

	/* Never mutates a later device which reused the recorded identity. */
	if (normalize) {
		if (interface_index_name_matches(connection->ifindex,
		    connection->interface) != 0) {
			cleanup_error = errno != 0 ? errno : ENODEV;
			if (cleanup_error == ENODEV || cleanup_error == ENXIO) {
				connection->device_removed = 1;
				return retire_removed_connection(next_state);
			}
		} else {
			memset(&result, 0, sizeof(result));
			wifi_work.cleanup = 1;
			if (run_wifi(connection->interface, "disconnect", NULL,
			    10U, &result) != 0)
				cleanup_error = errno != 0 ? errno : EIO;
			wifi_work.cleanup = 0;
			disconnect_error = cleanup_error;
			cleanup_stage = "wifi-disconnect";
			child_terminal = result.terminal_error;
			child_exit = result.child_exit_status;
			child_signal = result.child_term_signal;
			child_records = result.output_records;
			networkd_wifi_child_result_clear(&result);
			if (cleanup_error == 0 && connection->l3_pending &&
			    reconcile_pending_l3() != 0) {
				cleanup_error = errno != 0 ? errno : EIO;
				l3_error = cleanup_error;
				cleanup_stage = "l3-snapshot";
			}
			/* Preserve L3 until L2 retirement is proven, then retry as needed. */
			if (cleanup_error == 0 && connection->owns_l3) {
				if (clear_interface_l3(&managed_wlan) != 0) {
					l3_error = errno;
					if (cleanup_error == 0) {
						cleanup_error = l3_error;
						cleanup_stage = "l3";
					}
				} else {
					connection->owns_l3 = 0;
					networkd_protocol_clear(&connection->l3,
					    sizeof(connection->l3));
				}
			}
		}
		if (cleanup_error != 0) {
			fprintf(stderr,
			    "networkd: %s: managed cleanup degraded: %s\n",
			    connection->interface, strerror(cleanup_error));
			/* Distinguish child/setup failure from later L3 cleanup without
			 * exposing any child output, diagnostics, or connection identity. */
			fprintf(stderr,
			    "networkd: managed-cleanup stage=%s error=%d "
			    "disconnect-error=%d l3-error=%d child-error=%d "
			    "child-exit=%d child-signal=%d child-records=%u\n",
			    cleanup_stage, cleanup_error, disconnect_error, l3_error,
			    child_terminal, child_exit, child_signal, child_records);
		}
	}

	/* Retains the exact ownership token until cleanup can be retried. */
	if (cleanup_error != 0) {
		schedule_retirement_retry();
		errno = cleanup_error;
		return -1;
	}
	(void)networkd_managed_wlan_finish_connection(&managed_wlan,
	    next_state);
	return 0;
}

/* A removed device owns no live interface tuple; preserve any replacement. */
static int
retire_removed_connection(
	enum networkd_managed_wlan_state next_state)
{
	struct networkd_managed_wlan_connection *connection;
	struct networkd_managed_l3 current;
	int saved;

	connection = &managed_wlan.connection;
	/* Interface resources vanished; a partial DHCP resolver file may remain. */
	if (connection->l3_pending) {
		memset(&current, 0, sizeof(current));
		if (snapshot_resolver(&current) != 0) {
			saved = errno;
			networkd_protocol_clear(&current, sizeof(current));
			schedule_retirement_retry();
			errno = saved;
			return -1;
		}
		identify_l3_ownership(connection->interface, &connection->l3_before,
		    &current);
		(void)networkd_managed_wlan_track_l3(&managed_wlan, &current);
		networkd_protocol_clear(&current, sizeof(current));
	}
	if (connection->owns_l3 && connection->l3.resolver_owned &&
	    unlink_owned_resolver(&managed_wlan) != 0 &&
	    errno != ESTALE && errno != ENOENT) {
		saved = errno != 0 ? errno : EIO;
		schedule_retirement_retry();
		errno = saved;
		return -1;
	}
	return networkd_managed_wlan_finish_connection(&managed_wlan, next_state);
}

/* Clears policy only after bounded normalization succeeds; failures retain ownership. */
static int
retire_managed_policy(
	void)
{
	struct networkd_wlan_radio radios[NETWORKD_WLAN_RADIO_MAX];
	size_t radio_count;
	int first_error;

	/* Retires exact managed state before applying the global disabled state. */
	wifi_work_begin(NETWORKD_OP_WIFI_DISABLE, 0);
	memset(radios, 0, sizeof(radios));
	radio_count = 0U;
	first_error = 0;
	if (managed_wlan.state != NETWORKD_WLAN_DISABLED &&
	    retire_managed_connection(NETWORKD_WLAN_MANUAL_DISCONNECTED, 1) != 0)
		first_error = errno != 0 ? errno : EIO;

	/* A service start or stop leaves every discovered radio physically down. */
	if (enumerate_wlan_radios(radios, NETWORKD_WLAN_RADIO_MAX,
	    &radio_count) != 0) {
		if (first_error == 0)
			first_error = errno != 0 ? errno : EIO;
	} else if (stop_wlan_radios(radios, radio_count, 1) != 0 &&
	    first_error == 0) {
		first_error = errno != 0 ? errno : EIO;
	}
	networkd_protocol_clear(radios, sizeof(radios));
	wifi_work_end();
	if (first_error != 0) {
		errno = first_error;
		return -1;
	}
	networkd_managed_wlan_init(&managed_wlan);
	return 0;
}

/*
 * Keeps one watcher's connection open, or reports that there is no room.
 */
static int
subscriber_add(
	int client,
	uint32_t request_id)
{
	/* Handles a daemon already watching as many as it will. */
	if (subscriber_count >= NETWORKD_SUBSCRIBER_MAX)
		return -1;
	subscribers[subscriber_count] = client;
	subscriber_ids[subscriber_count] = request_id != 0U ? request_id : 1U;
	subscriber_count++;

	/* Succeeded: the connection now belongs to the watcher list. */
	return 0;
}

/* Closes one watcher and fills its place from the end. */
static void
subscriber_drop(
	size_t slot)
{
	/* Handles a slot outside the list. */
	if (slot >= subscriber_count)
		return;
	(void)close(subscribers[slot]);
	subscriber_count--;
	subscribers[slot] = subscribers[subscriber_count];
	subscriber_ids[slot] = subscriber_ids[subscriber_count];
}

/* Closes every watcher, which is what shutting down owes them. */
static void
subscribers_close(
	void)
{
	/* Process each watcher. */
	while (subscriber_count > 0U)
		subscriber_drop(subscriber_count - 1U);
}

/*
 * Sends one state to one watcher.
 *
 * The daemon does not queue for a watcher that is not reading: it is the
 * only thing that can configure the network, and waiting on a client that
 * has stopped would stop everything.  A watcher that cannot take the frame
 * is dropped and may connect again.
 */
static void
subscriber_send(
	size_t slot,
	const char *state,
	size_t length)
{
	struct networkd_protocol_header header;
	struct networkd_field_writer writer;
	unsigned char payload[NETWORKD_RESPONSE_MAX];

	networkd_field_writer_init(&writer, payload, sizeof(payload));

	/* Handles a state that does not fit the frame. */
	if (networkd_field_write_u32(&writer, NETWORKD_FIELD_STATUS,
	    NETWORKD_RESULT_OK) != 0 ||
	    networkd_field_write_u32(&writer, NETWORKD_FIELD_ERROR, 0U) != 0 ||
	    (length != 0U && networkd_field_write(&writer,
	    NETWORKD_FIELD_OUTPUT, state, length) != 0)) {
		networkd_protocol_clear(payload, sizeof(payload));
		return;
	}
	header.request_id = subscriber_ids[slot];
	header.opcode = NETWORKD_OP_SUBSCRIBE;
	header.payload_length = writer.used;

	/* Handles the watcher that is no longer taking what it asked for. */
	if (networkd_protocol_write_frame(subscribers[slot], &header,
	    payload) != 0)
		subscriber_drop(slot);
	networkd_protocol_clear(payload, sizeof(payload));
}

/* Tells every watcher what the network looks like now. */
static void
notify_subscribers(
	void)
{
	char state[NETWORKD_RESPONSE_OUTPUT_MAX];
	size_t slot, length;

	/* Handles the daemon nobody is watching. */
	if (subscriber_count == 0U)
		return;

	/* Handles a state that cannot be read at all. */
	if (watch_state(state, sizeof(state), &length) != 0)
		return;
	slot = 0;

	/* Process each watcher, which may leave its slot to be refilled. */
	while (slot < subscriber_count) {
		size_t before;

		before = subscriber_count;
		subscriber_send(slot, state, length);

		/* Advances only past a watcher that is still there. */
		if (subscriber_count == before)
			slot++;
	}
	networkd_protocol_clear(state, sizeof(state));
}

/*
 * Records that the network has moved.
 *
 * Called wherever the daemon changes an interface, an address, a route or
 * the Wi-Fi state.  The watchers are told once, at the top of the loop, so
 * that one operation reaching several of these places sends one frame.
 */
static void
notify_state_changed(
	void)
{
	state_changed = 1;
}

/*
 * Takes on one connection as a watcher.
 *
 * Reports whether the connection now belongs to the watcher list.  A client
 * with no right to look, or one arriving when the list is full, is answered
 * and left for the ordinary path to close.
 */
static int
accept_subscriber(
	int client,
	enum networkd_client_role role,
	uint32_t request_id)
{
	char state[NETWORKD_RESPONSE_OUTPUT_MAX];
	size_t length;

	/* Handles a client with no right to look at the network. */
	if (!operation_allowed(role, "SHOW")) {
		send_response(client, request_id, NETWORKD_OP_SUBSCRIBE,
		    NETWORKD_RESULT_ERROR, EPERM, "operation denied", NULL, 0U);
		return 0;
	}

	/*
	 * A watcher is written to without waiting.  One that has stopped
	 * reading fills its socket, the next write fails at once, and the
	 * watcher is dropped rather than holding up the daemon.
	 */
	if (fcntl(client, F_SETFL, fcntl(client, F_GETFL) | O_NONBLOCK) != 0) {
		send_response(client, request_id, NETWORKD_OP_SUBSCRIBE,
		    NETWORKD_RESULT_ERROR, errno, "cannot watch", NULL, 0U);
		return 0;
	}

	/* Handles a daemon already watching as many as it will. */
	if (subscriber_add(client, request_id) != 0) {
		send_response(client, request_id, NETWORKD_OP_SUBSCRIBE,
		    NETWORKD_RESULT_ERROR, EBUSY, "too many watchers",
		    NULL, 0U);
		return 0;
	}

	/*
	 * The first frame is the state as it stands, so that a watcher does
	 * not have to ask SHOW once and then watch: what it sees first and
	 * what it sees later have the same shape.
	 */
	if (watch_state(state, sizeof(state), &length) != 0)
		length = 0U;
	subscriber_send(subscriber_count - 1U, state, length);
	networkd_protocol_clear(state, sizeof(state));

	/* Succeeded: the connection is a watcher now. */
	return 1;
}

/*
 * Writes the state a watcher is told: the interfaces as "net show" prints
 * them, and a last line on the Wi-Fi for the desktop's system bar
 * (ws035-p013):
 *
 *     wifi state=NAME interface=IF ssid=HEX radios=N
 *
 * The SSID is the network the managed connection is on or joining, in
 * lower-case hexadecimal because an SSID is bytes, not text ("-" when there
 * is none); radios counts the WLAN interfaces, so that a machine without
 * one is told apart from one whose Wi-Fi is off; radio names the first
 * WLAN interface ("-" for none) even while no connection uses it, so that
 * a reader never takes the radio for a wired interface (BUG-169).
 */
static int
watch_state(
	char *state,
	size_t capacity,
	size_t *length)
{
	static const char digits[] = "0123456789abcdef";
	struct networkd_wlan_radio radios[NETWORKD_WLAN_RADIO_MAX];
	char ssid[WLAN_SSID_MAX * 2U + 1U];
	char route_name[IFNAMSIZ];
	const char *interface;
	const char *radio;
	size_t radio_count;
	size_t used;
	size_t index;
	uint32_t route_index;
	int descriptor;
	int known;
	int count;
	int error;
	int found;

	/* The interfaces. */
	error = show_interfaces(NULL, state, capacity);
	if (error != 0)
		return -1;
	used = strlen(state);

	/* The SSID of a connection being made or kept, each byte as two digits ("-" for none). */
	ssid[0] = '-';
	ssid[1] = '\0';
	known = watch_ssid_known();
	if (known) {
		for (index = 0; index < managed_wlan.connection.ssid_length; index++) {
			ssid[index * 2U] = digits[managed_wlan.connection.ssid[index] >> 4];
			ssid[index * 2U + 1U] = digits[managed_wlan.connection.ssid[index] & 0x0fU];
		}

		/* The digits end after the last byte. */
		ssid[index * 2U] = '\0';
	}

	/* How many radios there are (a count that fails is none). */
	radio_count = 0;
	error = enumerate_wlan_radios(radios, NETWORKD_WLAN_RADIO_MAX, &radio_count);
	if (error != 0)
		radio_count = 0;

	/* The Wi-Fi line after the interfaces. */
	interface = "-";
	if (managed_wlan.connection.interface[0] != '\0')
		interface = managed_wlan.connection.interface;
	radio = "-";
	if (radio_count > 0U && radios[0].interface[0] != '\0')
		radio = radios[0].interface;
	count = snprintf(state + used, capacity - used, "wifi state=%s interface=%s ssid=%s radios=%u scan=%u radio=%s\n",
	    managed_state_name(managed_wlan.state), interface, ssid, (unsigned)radio_count,
	    (unsigned)wifi_scan_wanted(), radio);
	if (count < 0 || (size_t)count >= capacity - used) {
		errno = EOVERFLOW;
		return -1;
	}
	used += (size_t)count;

	/* The interface the default route goes through ("-" for none or one that cannot be named, BUG-189). */
	(void)snprintf(route_name, sizeof(route_name), "-");
	found = default_route_index(&route_index);
	if (found > 0) {
		descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (descriptor >= 0) {
			error = netutil_ifname(descriptor, route_index, route_name);
			if (error != 0)
				(void)snprintf(route_name, sizeof(route_name), "-");
			(void)close(descriptor);
		}
	}

	/* The route's line, last. */
	count = snprintf(state + used, capacity - used, "route default=%s\n", route_name);
	if (count < 0 || (size_t)count >= capacity - used) {
		errno = EOVERFLOW;
		return -1;
	}

	/* Succeeded: the state is written, and this long. */
	*length = used + (size_t)count;
	return 0;
}

/* Tells whether the managed connection names a network: one being joined, kept or rejoined, with an SSID. */
static int
watch_ssid_known(
	void)
{
	/* An SSID of no length, or one longer than any, names nothing. */
	if (managed_wlan.connection.ssid_length == 0U)
		return 0;
	if (managed_wlan.connection.ssid_length > WLAN_SSID_MAX)
		return 0;

	/* Only these states have a network. */
	if (managed_wlan.state == NETWORKD_WLAN_CONNECTING)
		return 1;
	if (managed_wlan.state == NETWORKD_WLAN_CONNECTED)
		return 1;
	if (managed_wlan.state == NETWORKD_WLAN_RECONNECTING)
		return 1;

	/* Searching, off, left or leaving. */
	return 0;
}

/* Tells the watchers, once, about everything that has moved since last time. */
static void
deliver_state_changes(
	void)
{
	static enum networkd_managed_wlan_state told_wlan_state;
	static int told_wlan_valid;
	static int told_scan;
	static uint32_t told_route;
	uint32_t route;
	int scan;
	int found;

	/*
	 * The Wi-Fi state is compared rather than reported from each place
	 * that changes it.  It is moved from a dozen places -- connecting,
	 * retiring, the automatic search, the child actor finishing -- and
	 * one of them forgetting to say so would be a watcher that quietly
	 * stops being told.  Comparing cannot be forgotten.
	 */
	if (!told_wlan_valid || told_wlan_state != managed_wlan.state) {
		told_wlan_state = managed_wlan.state;
		told_wlan_valid = 1;
		state_changed = 1;
	}

	/* A desktop's asking for scans began or ended (ws089-p021), as the lease says now. */
	scan = wifi_scan_wanted();
	if (scan != told_scan) {
		told_scan = scan;
		state_changed = 1;
	}

	/*
	 * The default route moved to another interface (BUG-189: the desktop
	 * shows the connection that carries it).  Routes change from several
	 * places (dhcpc, the network preference, an administrator), so the
	 * table is compared, as the Wi-Fi state is.
	 */
	route = 0U;
	found = default_route_index(&route);
	if (found <= 0)
		route = 0U;
	if (route != told_route) {
		told_route = route;
		state_changed = 1;
	}

	/* Handles the network that has not moved. */
	if (!state_changed)
		return;
	state_changed = 0;
	notify_subscribers();
}

/* Supports the notify init operation. */
static void
notify_init(
	const char *record)
{
	const char *value;

	value = getenv("KERN_NOTIFY_FD");

	/* Handles the value availability. */
	if (value == NULL || strcmp(value, "3") != 0)
		return;
	(void)write_all(3, record, strlen(record));
	(void)close(3);
}

/* Supports the write all operation. */
static int
write_all(
	int descriptor,
	const char *buffer,
	size_t length)
{
	ssize_t count;
	size_t offset;

	/* Process each remaining element. */
	offset = 0;
	while (offset < length) {
		count = write(descriptor, buffer + offset, length - offset);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count <= 0)
			return -1;
		offset += (size_t)count;
	}

	/* Reports successful completion. */
	return 0;
}

/* Records the peer credential decision without exposing request contents. */
static void
write_auth_log(
	const struct kern_peercred *peer,
	enum networkd_client_role role,
	int error)
{
	const char fallback[] =
	    "networkd: auth result=denied reason=record-overflow\n";
	char record[NETWORKD_AUTH_LOG_MAX];
	int length;

	/* Handles an operation failure. */
	if (error != 0 || peer == NULL) {
		length = snprintf(record, sizeof(record),
		    "networkd: auth result=denied reason=peercred error=%d",
		    error != 0 ? error : EACCES);
	} else {
		length = snprintf(record, sizeof(record),
		    "networkd: auth result=admitted role=%s pid=%ld euid=%lu egid=%lu",
		    role == NETWORKD_CLIENT_ROOT ? "root-all" : "network-member",
		    (long)peer->pid, (unsigned long)peer->euid,
		    (unsigned long)peer->egid);
	}

	/* Checks the current data length. */
	if (length < 0 || (size_t)length > sizeof(record) - 2U) {
		(void)fputs(fallback, stderr);

		/* Returns the computed result. */
		return;
	}
	record[length++] = '\n';
	record[length] = '\0';
	(void)fputs(record, stderr);
}

/* Obtains immutable credentials for the connected Unix-domain peer. */
static int
authenticate_client(
	int client,
	struct kern_peercred *peer,
	enum networkd_client_role *role)
{
	socklen_t length;
	int error;

	/* Handles the peer availability. */
	if (peer == NULL || role == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	memset(peer, 0, sizeof(*peer));
	length = sizeof(*peer);

	/* Handles a failed getsockopt operation. */
	if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, peer, &length) != 0) {
		error = errno != 0 ? errno : EACCES;
		write_auth_log(NULL, NETWORKD_CLIENT_MEMBER, error);
		errno = error;

		/* Reports operation failure. */
		return -1;
	}

	/* Checks the current data length. */
	if (length != sizeof(*peer) || peer->pid < 0) {
		error = EINVAL;
		write_auth_log(NULL, NETWORKD_CLIENT_MEMBER, error);
		errno = error;

		/* Reports operation failure. */
		return -1;
	}
	*role = peer->euid == 0 ? NETWORKD_CLIENT_ROOT
				 : NETWORKD_CLIENT_MEMBER;
	write_auth_log(peer, *role, 0);

	/* Reports successful completion. */
	return 0;
}

/* Restricts non-root clients to read-only state inspection. */
static int
operation_allowed(
	enum networkd_client_role role,
	const char *operation)
{
	int function_result;

	/* Handles the role condition. */
	if (role == NETWORKD_CLIENT_ROOT)
		return 1;

	/* Admits inspection and global WLAN policy operations for socket members. */
	function_result = operation != NULL &&
	    (strcmp(operation, "SHOW") == 0 ||
	    strcmp(operation, "WIFI_ENABLE") == 0 ||
	    strcmp(operation, "WIFI_DISABLE") == 0 ||
	    strcmp(operation, "WIFI_LIST") == 0 ||
	    strcmp(operation, "WIFI_CONNECT") == 0 ||
	    strcmp(operation, "WIFI_DISCONNECT") == 0 ||
	    strcmp(operation, "WIFI_PROFILES_CHANGED") == 0 ||
	    strcmp(operation, "WIFI_SESSION_OPEN") == 0 ||
	    strcmp(operation, "WIFI_SESSION_CLOSE") == 0 ||
	    strcmp(operation, "WIFI_SCAN_START") == 0 ||
	    strcmp(operation, "WIFI_SCAN_STOP") == 0 ||
	    strcmp(operation, "LAN_CONFIGURE") == 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Reads one request before dispatching it through the common admission path.
 *
 * Reports whether the connection was kept: a watcher's outlives the request
 * that made it one, and the caller must not close it.
 */
static int
handle_request(
	int client,
	enum networkd_client_role role,
	const struct kern_peercred *peer)
{
	struct networkd_request request;
	int retained;

	/* Retains decoded storage until the synchronous dispatcher returns. */
	memset(&request, 0, sizeof(request));
	retained = 0;
	if (read_request(client, &request) != 0)
		send_error(client, errno, "malformed request");
	else if (request.header.opcode == NETWORKD_OP_SUBSCRIBE)
		retained = accept_subscriber(client, role,
		    request.header.request_id);
	else
		dispatch_request(client, &request, role, peer);
	networkd_protocol_clear(&request, sizeof(request));

	/* Returns whether the connection now belongs to the watcher list. */
	return retained;
}

/* Applies the same admission and transaction checks to immediate and queued work. */
static void
dispatch_request(
	int client,
	struct networkd_request *request,
	enum networkd_client_role role,
	const struct kern_peercred *peer)
{
	char response[NETWORKD_RESPONSE_MAX];
	char diagnostic[CHILD_OUTPUT_MAX];
	const char *operation;
	uint64_t mutation_deadline;
	size_t response_length;
	int admitted;
	int result;
	int error;

	/* Initializes one terminal response without consuming transport again. */
	result = -1;
	error = EINVAL;
	response_length = 0U;
	response[0] = '\0';
	diagnostic[0] = '\0';
	mutation_deadline = 0U;

	/* Applies peer authorization before operation dispatch. */
	NCOM_TRACE("daemon-read-done", request->header.opcode);
	operation = operation_name(request->header.opcode);
	if (operation == NULL || !operation_allowed(role, operation)) {
		send_response(client, request->header.request_id,
		    request->header.opcode, NETWORKD_RESULT_ERROR, EPERM,
		    "authorization", NULL, 0U);
		return;
	}

	/* A request arriving with the timer event cannot disarm an expired owner. */
	run_confirmed_due();

	/* The machine's sleep, asked by sessiond (ws052-p010). */
	if (request->header.opcode == NETWORKD_OP_SLEEP_PREPARE ||
	    request->header.opcode == NETWORKD_OP_SLEEP_END) {
		handle_sleep_request(client, request);
		networkd_protocol_clear(response, sizeof(response));
		networkd_protocol_clear(diagnostic, sizeof(diagnostic));
		return;
	}

	/* While asleep the Wi-Fi changes and a new confirmed transaction wait for the end. */
	admitted = networkd_sleep_admits(&sleep_state, request->header.opcode);
	if (!admitted) {
		send_response(client, request->header.request_id,
		    request->header.opcode, NETWORKD_RESULT_ERROR, EBUSY,
		    "asleep", NULL, 0U);
		networkd_protocol_clear(response, sizeof(response));
		networkd_protocol_clear(diagnostic, sizeof(diagnostic));
		return;
	}

	/*
	 * Wired management says what is to happen from now on.  The answer
	 * is sent as soon as that has been recorded, because the work
	 * itself belongs to the background: a cable that is not in yet will
	 * not go in any sooner for the caller waiting.
	 */
	if (request->header.opcode == NETWORKD_OP_LAN_ENABLE ||
	    request->header.opcode == NETWORKD_OP_LAN_DISABLE) {
		struct networkd_lan_policy policy[NETWORKD_LAN_MAX];
		size_t policy_count;

		if (request->header.opcode == NETWORKD_OP_LAN_DISABLE) {
			(void)networkd_lan_disable(&managed_lan);
			send_response(client, request->header.request_id,
			    request->header.opcode, NETWORKD_RESULT_OK, 0,
			    NULL, NULL, 0U);
			return;
		}
		policy_count = 0U;

		/* Handles a policy that could not be read. */
		if (lan_policy_decode(request, policy,
		    sizeof(policy) / sizeof(policy[0]), &policy_count) != 0 ||
		    networkd_lan_set_policy(&managed_lan, policy,
					    policy_count) != 0) {
			send_response(client, request->header.request_id,
			    request->header.opcode, NETWORKD_RESULT_ERROR,
			    EINVAL, "wired policy", NULL, 0U);
			return;
		}
		(void)networkd_lan_enable(&managed_lan);
		lan_work_due = 1;
		send_response(client, request->header.request_id,
		    request->header.opcode, NETWORKD_RESULT_OK, 0, NULL,
		    NULL, 0U);
		return;
	}

	/* One wired interface's configuration (ws089-p022): written to net.conf and applied now. */
	if (request->header.opcode == NETWORKD_OP_LAN_CONFIGURE) {
		handle_lan_configure(client, request, peer);
		return;
	}

	/* Owns confirmed-commit control without ever opening net.conf. */
	if (request->header.opcode >= NETWORKD_OP_CONFIRMED_ARM &&
	    request->header.opcode <= NETWORKD_OP_CONFIRMED_CHECK) {
		if (request->header.opcode == NETWORKD_OP_CONFIRMED_CHECK) {
			result = networkd_confirmed_check(&confirmed, request->token);
			error = result == 0 ? 0 : errno;
		} else if (request->header.opcode == NETWORKD_OP_CONFIRMED_ARM) {
			result = networkd_confirmed_arm(&confirmed,
			    request->rollback_path, peer->euid, request->timeout,
			    netutil_monotonic_us(), rollback_validate, NULL,
			    &request->token, diagnostic, sizeof(diagnostic));
			error = result == 0 ? 0 : errno;
			if (result == 0) {
				send_token_response(client, request->header.request_id,
				    request->header.opcode, request->token);
				networkd_protocol_clear(diagnostic,
				    sizeof(diagnostic));
				return;
			}
		} else if (request->header.opcode ==
		    NETWORKD_OP_CONFIRMED_DISARM) {
			result = networkd_confirmed_disarm(&confirmed,
			    request->token);
			error = result == 0 ? 0 : errno;
		} else {
			result = networkd_confirmed_rollback(&confirmed,
			    rollback_execute, NULL, response, sizeof(response));
			error = result == 0 ? 0 : errno;
			response_length = strlen(response);
		}
		if (result == 0) {
			send_response(client, request->header.request_id,
			    request->header.opcode, NETWORKD_RESULT_OK, 0, NULL,
			    response_length != 0U ? response : NULL,
			    response_length);
		} else {
			send_response(client, request->header.request_id,
			    request->header.opcode,
			    request->header.opcode == NETWORKD_OP_CONFIRMED_ROLLBACK &&
			    response_length != 0U ? NETWORKD_RESULT_DEGRADED :
			    NETWORKD_RESULT_ERROR, error != 0 ? error : EIO,
			    diagnostic[0] != '\0' ? diagnostic : operation,
			    response_length != 0U ? response : NULL,
			    response_length);
		}
		networkd_protocol_clear(response, sizeof(response));
		networkd_protocol_clear(diagnostic, sizeof(diagnostic));
		return;
	}

	/* Serializes every wired mutation with the volatile transaction owner. */
	if (((request->header.opcode >= NETWORKD_OP_UP &&
	    request->header.opcode <= NETWORKD_OP_STATIC6_REMOVE &&
	    request->header.opcode != NETWORKD_OP_RELOAD) ||
	    request->header.opcode == NETWORKD_OP_ROUTE6_REMOVE) &&
	    networkd_confirmed_check(&confirmed, request->token) != 0) {
		error = errno != 0 ? errno : EBUSY;
		send_response(client, request->header.request_id,
		    request->header.opcode, NETWORKD_RESULT_ERROR, error,
		    "confirmed transaction", NULL, 0U);
		return;
	}
	if (request->token != 0U)
		mutation_deadline = confirmed.deadline;

	/* WLAN mutations cannot occupy the loop past a wired rollback deadline. */
	if (networkd_confirmed_active(&confirmed) &&
	    request->header.opcode >= NETWORKD_OP_WIFI_ENABLE &&
	    request->header.opcode <= NETWORKD_OP_WIFI_SESSION_CLOSE &&
	    request->header.opcode != NETWORKD_OP_WIFI_LIST) {
		send_response(client, request->header.request_id,
		    request->header.opcode, NETWORKD_RESULT_ERROR, EBUSY,
		    "confirmed transaction", NULL, 0U);
		return;
	}

	/* A desktop asking for scans, or no longer, is only recorded (ws089-p021). */
	if (request->header.opcode == NETWORKD_OP_WIFI_SCAN_START ||
	    request->header.opcode == NETWORKD_OP_WIFI_SCAN_STOP) {
		wifi_scan_request(client, request);
		networkd_protocol_clear(response, sizeof(response));
		networkd_protocol_clear(diagnostic, sizeof(diagnostic));
		return;
	}

	/* Delegates the complete typed WLAN family to its bounded orchestrator. */
	if (request->header.opcode >= NETWORKD_OP_WIFI_ENABLE &&
	    request->header.opcode <= NETWORKD_OP_WIFI_SESSION_CLOSE) {
		handle_wifi_request(client, request, peer);
		networkd_protocol_clear(response, sizeof(response));
		networkd_protocol_clear(diagnostic, sizeof(diagnostic));
		return;
	}

	/* Dispatches the validated operation through absolute child paths. */
	NCOM_TRACE("daemon-execute-enter", request->header.opcode);
	result = execute_wired_request(request, response, sizeof(response),
	    diagnostic, sizeof(diagnostic), &response_length, &error,
	    mutation_deadline);
	NCOM_TRACE("daemon-execute-done", request->header.opcode);
	if (mutation_deadline != 0U &&
	    netutil_monotonic_us() >= mutation_deadline) {
		run_confirmed_due();
		result = -1;
		error = ETIMEDOUT;
		(void)snprintf(diagnostic, sizeof(diagnostic),
		    "confirmed transaction expired");
	}

	/* Sends one correlated terminal response and clears request storage. */
	NCOM_TRACE("daemon-response-enter", request->header.opcode);
	if (result == 0) {
		send_response(client, request->header.request_id,
		    request->header.opcode, NETWORKD_RESULT_OK, 0, NULL,
		    response_length != 0U ? response : NULL, response_length);
	} else {
		send_response(client, request->header.request_id,
		    request->header.opcode, NETWORKD_RESULT_ERROR,
		    error != 0 ? error : EIO,
		    diagnostic[0] != '\0' ? diagnostic : operation,
		    NULL, 0U);
	}
	NCOM_TRACE("daemon-response-done", request->header.opcode);
	networkd_protocol_clear(response, sizeof(response));
	networkd_protocol_clear(diagnostic, sizeof(diagnostic));
}

/* Executes one decoded non-WLAN request for a client or rollback program. */
static int
execute_wired_request(
	struct networkd_request *request,
	char *response,
	size_t response_capacity,
	char *diagnostic,
	size_t diagnostic_capacity,
	size_t *response_length,
	int *error,
	uint64_t deadline)
{
	struct in_addr address;
	struct in_addr mask;
	struct in_addr gateway;
	unsigned prefix;
	char seconds[16];
	char *arguments[16];
	char *dns[8];
	unsigned index;
	int present;
	int missing;
	int result;
	int off;

	if (request == NULL || diagnostic == NULL ||
	    diagnostic_capacity < CHILD_OUTPUT_MAX || response_length == NULL ||
	    error == NULL) {
		errno = EINVAL;
		return -1;
	}
	result = -1;
	*error = EINVAL;
	*response_length = 0U;
	diagnostic[0] = '\0';
	if (request->header.opcode == NETWORKD_OP_SHOW) {
		if (response != NULL && response_capacity != 0U &&
		    show_interfaces(request->interface[0] != '\0' ?
		    request->interface : NULL, response, response_capacity) == 0) {
			*response_length = strlen(response);
			result = 0;
		}
		*error = errno;
	} else if (request->header.opcode == NETWORKD_OP_UP ||
	    request->header.opcode == NETWORKD_OP_DOWN) {
		/* A DHCPv6 lease given back before the interface goes down (ws177-p046). */
		if (request->header.opcode == NETWORKD_OP_DOWN)
			release_dhcp6(request->interface, deadline, diagnostic);
		arguments[0] = "/sbin/ifconfig";
		arguments[1] = request->interface;
		arguments[2] = request->header.opcode == NETWORKD_OP_UP ?
		    "up" : "down";
		arguments[3] = NULL;
		if (interface_exists(request->interface) == 0)
			result = run_command_until(arguments, 10, deadline, diagnostic);
		*error = errno;
	} else if (request->header.opcode == NETWORKD_OP_STATIC) {
		if (interface_exists(request->interface) == 0 &&
		    netutil_parse_ipv4(request->address, &address) == 0 &&
		    netutil_parse_ipv4(request->netmask, &mask) == 0 &&
		    netutil_mask_prefix(mask, &prefix) == 0) {
			arguments[0] = "/sbin/ifconfig";
			arguments[1] = request->interface;
			arguments[2] = "inet";
			arguments[3] = request->address;
			arguments[4] = "netmask";
			arguments[5] = request->netmask;
			arguments[6] = NULL;
			result = run_command_until(arguments, 10, deadline, diagnostic);
		}
		*error = errno;
	} else if (request->header.opcode == NETWORKD_OP_DHCP) {
		(void)snprintf(seconds, sizeof(seconds), "%u", request->timeout);
		arguments[0] = "/sbin/dhcpc";
		arguments[1] = "-t";
		arguments[2] = seconds;
		arguments[3] = request->interface;
		arguments[4] = NULL;
		if (interface_exists(request->interface) == 0)
			result = run_command_until(arguments, request->timeout + 5U,
			    deadline, diagnostic);
		*error = errno;
	} else if (request->header.opcode == NETWORKD_OP_DEFAULT_ROUTE) {
		if (netutil_parse_ipv4(request->gateway, &gateway) == 0 &&
		    (present = default_route_exists()) >= 0) {
			if (present) {
				result = 0;
			} else {
				arguments[0] = "/sbin/route";
				arguments[1] = "add";
				arguments[2] = "default";
				arguments[3] = request->gateway;
				arguments[4] = NULL;
				result = run_command_until(arguments, 10, deadline,
				    diagnostic);
			}
		}
		*error = errno;
	} else if (request->header.opcode ==
	    NETWORKD_OP_DEFAULT_ROUTE_CLEAR) {
		present = default_route_exists();
		if (present == 0) {
			result = 0;
		} else if (present > 0) {
			arguments[0] = "/sbin/route";
			arguments[1] = "delete";
			arguments[2] = "default";
			arguments[3] = NULL;
			result = run_command_until(arguments, 10, deadline, diagnostic);
		}
		*error = errno;
	} else if (request->header.opcode == NETWORKD_OP_DNS) {
		for (index = 0U; index < request->dns_count; index++)
			dns[index] = request->dns[index];
		result = write_resolver(dns, (int)request->dns_count);
		*error = errno;
	} else if (request->header.opcode == NETWORKD_OP_DNS_CLEAR) {
		result = write_resolver(NULL, 0);
		*error = errno;
	} else if (request->header.opcode == NETWORKD_OP_IPV6) {
		/* IPv6 on or off at an interface (ws130-p005); off gives a DHCPv6 lease back first (ws177-p046). */
		off = strcmp(request->address, "off");
		if (off == 0)
			release_dhcp6(request->interface, deadline, diagnostic);
		arguments[0] = "/sbin/ifconfig";
		arguments[1] = request->interface;
		arguments[2] = "ipv6";
		arguments[3] = request->address;
		arguments[4] = NULL;
		if (interface_exists(request->interface) == 0)
			result = run_command_until(arguments, 10, deadline, diagnostic);
		*error = errno;

		/* On again: its link-local address back (the kernel says nothing of it). */
		if (result == 0 && strcmp(request->address, "on") == 0)
			(void)networkd_ipv6_link_local(request->interface);
	} else if (request->header.opcode == NETWORKD_OP_STATIC6) {
		/* A static IPv6 address, ADDRESS/LENGTH (ws130-p005). */
		arguments[0] = "/sbin/ifconfig";
		arguments[1] = request->interface;
		arguments[2] = "inet6";
		arguments[3] = request->address;
		arguments[4] = NULL;
		if (interface_exists(request->interface) == 0)
			result = run_command_until(arguments, 10, deadline, diagnostic);
		*error = errno;
	} else if (request->header.opcode == NETWORKD_OP_ROUTE6) {
		/* An IPv6 route, replacing one to the same destination (ws130-p005). */
		arguments[0] = "/sbin/route";
		arguments[1] = "-6";
		arguments[2] = "delete";
		arguments[3] = request->address;
		arguments[4] = NULL;
		(void)run_command_until(arguments, 10, deadline, diagnostic);
		arguments[2] = "add";
		arguments[3] = request->address;
		arguments[4] = request->gateway;
		arguments[5] = NULL;
		if (request->interface[0] != '\0') {
			arguments[5] = "-ifp";
			arguments[6] = request->interface;
			arguments[7] = NULL;
		}
		diagnostic[0] = '\0';
		result = run_command_until(arguments, 10, deadline, diagnostic);
		*error = errno;
	} else if (request->header.opcode == NETWORKD_OP_STATIC6_REMOVE) {
		/* A static IPv6 address net.conf no longer names, taken away; one already gone is the same (ws177-p045). */
		arguments[0] = "/sbin/ifconfig";
		arguments[1] = request->interface;
		arguments[2] = "-inet6";
		arguments[3] = request->address;
		arguments[4] = NULL;
		missing = interface_exists(request->interface);
		if (missing == 0)
			(void)run_command_until(arguments, 10, deadline, diagnostic);
		diagnostic[0] = '\0';
		result = 0;
		*error = 0;
	} else if (request->header.opcode == NETWORKD_OP_ROUTE6_REMOVE) {
		/* An IPv6 route net.conf no longer names, taken away; none there is the same (ws177-p045). */
		arguments[0] = "/sbin/route";
		arguments[1] = "-6";
		arguments[2] = "delete";
		arguments[3] = request->address;
		arguments[4] = NULL;
		(void)run_command_until(arguments, 10, deadline, diagnostic);
		diagnostic[0] = '\0';
		result = 0;
		*error = 0;
	} else if (request->header.opcode == NETWORKD_OP_ROUTE6_CLEAR) {
		/* The IPv6 default route removed; none there is the same. */
		arguments[0] = "/sbin/route";
		arguments[1] = "-6";
		arguments[2] = "delete";
		arguments[3] = "default";
		arguments[4] = NULL;
		(void)run_command_until(arguments, 10, deadline, diagnostic);
		diagnostic[0] = '\0';
		result = 0;
		*error = 0;
	} else if (request->header.opcode == NETWORKD_OP_RELOAD) {
		result = 0;
		*error = 0;
	} else {
		*error = EOPNOTSUPP;
	}
	if (result != 0 && *error == 0)
		*error = EIO;

	/*
	 * A manual DHCP lease or static address is what the wired policy
	 * would have given the interface.  Recording it as configured keeps
	 * the carrier event that bringing the interface up causes from
	 * configuring it a second time, which would drop the address the
	 * caller has just started to use.
	 */
	if (result == 0 &&
	    (request->header.opcode == NETWORKD_OP_DHCP ||
	     request->header.opcode == NETWORKD_OP_STATIC) &&
	    lan_address_usable(request->interface))
		lan_note_configured(request->interface);
	return result;
}

/*
 * Records that an interface was configured by a request rather than by
 * the wired policy.  An interface that has only just arrived may not be
 * in the policy's table yet, so the table is read again first.
 */
static void
lan_note_configured(
	const char *name)
{
	if (networkd_lan_configured(&managed_lan, name, 1) == 0)
		return;
	lan_snapshot();
	(void)networkd_lan_configured(&managed_lan, name, 1);
}

/* Validates one rollback line without changing running state. */
static int
rollback_validate(
	const char *line,
	char *diagnostic,
	size_t capacity,
	void *context)
{
	struct networkd_request request;

	(void)context;
	return rollback_parse(line, &request, diagnostic, capacity);
}

/* Executes one already prevalidated rollback line. */
static int
rollback_execute(
	const char *line,
	char *diagnostic,
	size_t capacity,
	void *context)
{
	struct networkd_request request;
	char output[1];
	size_t output_length;
	int error;

	(void)context;
	if (rollback_parse(line, &request, diagnostic, capacity) != 0)
		return -1;
	if (execute_wired_request(&request, output, sizeof(output), diagnostic,
	    capacity, &output_length, &error, 0U) != 0) {
		if (diagnostic[0] == '\0')
			(void)snprintf(diagnostic, capacity, "%s", strerror(error));
		errno = error;
		return -1;
	}
	return 0;
}

/* Parses the canonical one-command-per-line rollback language. */
static int
rollback_parse(
	const char *line,
	struct networkd_request *request,
	char *diagnostic,
	size_t capacity)
{
	char copy[NETWORKD_ROLLBACK_LINE_MAX + 1U];
	char *word[16];
	char *token;
	char *end;
	struct in_addr parsed;
	struct in6_addr parsed6;
	struct in_addr mask;
	unsigned prefix;
	unsigned long timeout;
	size_t length;
	unsigned count;
	unsigned index;

#define ROLLBACK_REJECT(text) do { \
	if (diagnostic != NULL && capacity != 0U) \
		(void)snprintf(diagnostic, capacity, "%s", text); \
	errno = EINVAL; \
	return -1; \
} while (0)
	if (line == NULL || request == NULL ||
	    (length = strlen(line)) == 0U || length > NETWORKD_ROLLBACK_LINE_MAX)
		ROLLBACK_REJECT("invalid rollback line length");
	for (index = 0U; index < length; index++) {
		if ((unsigned char)line[index] < 32U ||
		    (unsigned char)line[index] > 126U)
			ROLLBACK_REJECT("non-printable rollback byte");
	}
	strcpy(copy, line);
	count = 0U;
	for (token = strtok(copy, " "); token != NULL;
	    token = strtok(NULL, " ")) {
		if (count == sizeof(word) / sizeof(word[0]))
			ROLLBACK_REJECT("too many rollback operands");
		word[count++] = token;
	}
	if (count < 2U || strcmp(word[0], "V1") != 0)
		ROLLBACK_REJECT("invalid rollback version");
	memset(request, 0, sizeof(*request));
	if ((strcmp(word[1], "UP") == 0 || strcmp(word[1], "DOWN") == 0) &&
	    count == 3U) {
		request->header.opcode = strcmp(word[1], "UP") == 0 ?
		    NETWORKD_OP_UP : NETWORKD_OP_DOWN;
		if (strlen(word[2]) >= sizeof(request->interface))
			ROLLBACK_REJECT("invalid rollback interface");
		strcpy(request->interface, word[2]);
	} else if (strcmp(word[1], "DHCP") == 0 && count == 4U) {
		errno = 0;
		timeout = strtoul(word[3], &end, 10);
		if (errno != 0 || end == word[3] || *end != '\0' || timeout == 0U ||
		    timeout > 3600U || strlen(word[2]) >= sizeof(request->interface))
			ROLLBACK_REJECT("invalid rollback DHCP");
		request->header.opcode = NETWORKD_OP_DHCP;
		request->timeout = (unsigned)timeout;
		strcpy(request->interface, word[2]);
	} else if (strcmp(word[1], "STATIC") == 0 && count == 7U &&
	    strcmp(word[3], "ipv4") == 0 && strcmp(word[5], "netmask") == 0) {
		if (strlen(word[2]) >= sizeof(request->interface) ||
		    strlen(word[4]) >= sizeof(request->address) ||
		    strlen(word[6]) >= sizeof(request->netmask) ||
		    netutil_parse_ipv4(word[4], &parsed) != 0 ||
		    netutil_parse_ipv4(word[6], &mask) != 0 ||
		    netutil_mask_prefix(mask, &prefix) != 0)
			ROLLBACK_REJECT("invalid rollback static address");
		request->header.opcode = NETWORKD_OP_STATIC;
		strcpy(request->interface, word[2]);
		strcpy(request->address, word[4]);
		strcpy(request->netmask, word[6]);
	} else if (strcmp(word[1], "DEFAULTROUTE") == 0 && count == 3U) {
		if (strlen(word[2]) >= sizeof(request->gateway) ||
		    netutil_parse_ipv4(word[2], &parsed) != 0)
			ROLLBACK_REJECT("invalid rollback default route");
		request->header.opcode = NETWORKD_OP_DEFAULT_ROUTE;
		strcpy(request->gateway, word[2]);
	} else if ((strcmp(word[1], "IPV6") == 0 || strcmp(word[1], "STATIC6") == 0 ||
	    strcmp(word[1], "STATIC6_REMOVE") == 0) && count == 4U) {
		/* IPv6 (ws130-p005): the interface, and "on"/"off" or the address with its length (added or, ws177-p045, taken away). */
		if (strlen(word[2]) >= sizeof(request->interface) || strlen(word[3]) >= sizeof(request->address))
			ROLLBACK_REJECT("invalid rollback IPv6 operation");
		request->header.opcode = NETWORKD_OP_STATIC6;
		if (strcmp(word[1], "IPV6") == 0)
			request->header.opcode = NETWORKD_OP_IPV6;
		else if (strcmp(word[1], "STATIC6_REMOVE") == 0)
			request->header.opcode = NETWORKD_OP_STATIC6_REMOVE;
		strcpy(request->interface, word[2]);
		strcpy(request->address, word[3]);
	} else if (strcmp(word[1], "ROUTE6_REMOVE") == 0 && count == 3U) {
		/* An IPv6 route taken away: its destination (ws177-p045). */
		if (strlen(word[2]) >= sizeof(request->address))
			ROLLBACK_REJECT("invalid rollback IPv6 route");
		request->header.opcode = NETWORKD_OP_ROUTE6_REMOVE;
		strcpy(request->address, word[2]);
	} else if (strcmp(word[1], "ROUTE6") == 0 && (count == 4U || count == 5U)) {
		/* An IPv6 route: the destination, the gateway, and the interface of a link-local one. */
		if (strlen(word[2]) >= sizeof(request->address) || strlen(word[3]) >= sizeof(request->gateway))
			ROLLBACK_REJECT("invalid rollback IPv6 route");
		if (count == 5U && strlen(word[4]) >= sizeof(request->interface))
			ROLLBACK_REJECT("invalid rollback IPv6 route");
		request->header.opcode = NETWORKD_OP_ROUTE6;
		strcpy(request->address, word[2]);
		strcpy(request->gateway, word[3]);
		if (count == 5U)
			strcpy(request->interface, word[4]);
	} else if (strcmp(word[1], "ROUTE6_CLEAR") == 0 && count == 2U) {
		request->header.opcode = NETWORKD_OP_ROUTE6_CLEAR;
	} else if (strcmp(word[1], "DEFAULTROUTE_CLEAR") == 0 && count == 2U) {
		request->header.opcode = NETWORKD_OP_DEFAULT_ROUTE_CLEAR;
	} else if (strcmp(word[1], "DNS_CLEAR") == 0 && count == 2U) {
		request->header.opcode = NETWORKD_OP_DNS_CLEAR;
	} else if (strcmp(word[1], "DNS") == 0 && count >= 3U && count <= 10U) {
		request->header.opcode = NETWORKD_OP_DNS;
		request->dns_count = count - 2U;
		for (index = 0U; index < request->dns_count; index++) {
			if (strlen(word[index + 2U]) >= sizeof(request->dns[index]))
				ROLLBACK_REJECT("invalid rollback DNS");
			/* An IPv4 or IPv6 server (ws130-p005). */
			if (netutil_parse_ipv4(word[index + 2U], &parsed) != 0 &&
			    inet_pton(AF_INET6, word[index + 2U], &parsed6) != 1)
				ROLLBACK_REJECT("invalid rollback DNS");
			strcpy(request->dns[index], word[index + 2U]);
		}
	} else {
		ROLLBACK_REJECT("unsupported rollback operation");
	}
	if ((request->header.opcode == NETWORKD_OP_UP ||
	    request->header.opcode == NETWORKD_OP_DOWN ||
	    request->header.opcode == NETWORKD_OP_DHCP ||
	    request->header.opcode == NETWORKD_OP_STATIC ||
	    request->header.opcode == NETWORKD_OP_IPV6 ||
	    request->header.opcode == NETWORKD_OP_STATIC6 ||
	    request->header.opcode == NETWORKD_OP_STATIC6_REMOVE) &&
	    (request->interface[0] == '\0' ||
	    strspn(request->interface,
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
	    strlen(request->interface)))
		ROLLBACK_REJECT("invalid rollback interface");
	return 0;
#undef ROLLBACK_REJECT
}

/* Prepares radios using one request-owned response buffer. */
static int
wifi_request_prepare(
	struct networkd_wifi_request *work)
{
	/* Retirement may have changed the preflight administrative/scan snapshot. */
	work->stage = "refresh WLAN radios";
	memset(work->radios, 0, sizeof(work->radios));
	if (enumerate_wlan_radios(work->radios, NETWORKD_WLAN_RADIO_MAX,
	    &work->radio_count) != 0)
		return -1;
	work->stage = "prepare WLAN radios";
	return prepare_wlan_radios(work->radios, work->radio_count,
	    work->output, (sizeof(work->output) - NETWORKD_WIFI_STATUS_RESERVE), &work->output_length);
}

/* Lists every radio independently and retains useful partial observations. */
static int
wifi_request_list(
	struct networkd_wifi_request *work)
{
	struct networkd_wifi_child_result child;
	size_t index;
	int first_error;
	int error;
	int count;

	work->stage = "enumerate WLAN radios";
	if (enumerate_wlan_radios(work->radios, NETWORKD_WLAN_RADIO_MAX,
	    &work->radio_count) != 0)
		return -1;
	first_error = 0;
	for (index = 0U; index < work->radio_count; index++) {
		memset(&child, 0, sizeof(child));
		if (work->radios[index].observation_error != 0 ||
		    work->radios[index].stop_flags != 0U) {
			error = work->radios[index].observation_error != 0 ?
			    work->radios[index].observation_error : EBUSY;
			count = snprintf(work->output + work->output_length,
			    sizeof(work->output) - NETWORKD_WIFI_STATUS_RESERVE - work->output_length,
			    "interface=%s stop-pending=%u status-error=%d\n",
			    work->radios[index].interface,
			    (work->radios[index].stop_flags & WLAN_STATUS_STOP_PENDING) != 0U,
			    work->radios[index].observation_error);
			if (count < 0 || (size_t)count >= sizeof(work->output) -
			    NETWORKD_WIFI_STATUS_RESERVE - work->output_length) {
				errno = EOVERFLOW;
				return -1;
			}
			work->output_length += (size_t)count;
		} else if (run_wifi(work->radios[index].interface, "list", NULL,
		    5U, &child) == 0) {
			error = append_wifi_snapshot(work->radios[index].interface,
			    (const char *)child.output, child.output_length, 0U,
			    work->output, (sizeof(work->output) - NETWORKD_WIFI_STATUS_RESERVE),
			    &work->output_length) == 0 ? 0 : errno;
		} else {
			error = child.terminal_error != 0 ? child.terminal_error : EIO;
			count = snprintf(work->output + work->output_length,
			    (sizeof(work->output) - NETWORKD_WIFI_STATUS_RESERVE) - work->output_length,
			    "interface=%s list-error=%d\n",
			    work->radios[index].interface, error);
			if (count < 0 || (size_t)count >=
			    (sizeof(work->output) - NETWORKD_WIFI_STATUS_RESERVE) - work->output_length) {
				networkd_wifi_child_result_clear(&child);
				errno = EOVERFLOW;
				return -1;
			}
			work->output_length += (size_t)count;
		}
		networkd_wifi_child_result_clear(&child);

		/* Preserves the overflow result even if an earlier radio failed differently. */
		if (error == EOVERFLOW) {
			work->stage = "Wi-Fi list output truncated";
			errno = EOVERFLOW;
			return -1;
		}
		if (error != 0 && first_error == 0)
			first_error = error;
	}
	work->stage = "partial Wi-Fi list";
	errno = first_error;
	return first_error == 0 ? 0 : -1;
}

/* Validates a prospective owner before publishing its enabled intent. */
static int
wifi_request_enable(
	struct networkd_wifi_request *work,
	const struct kern_peercred *peer)
{
	if (wifi_disable_pending) {
		errno = EBUSY;
		return -1;
	}
	work->stage = "load Wi-Fi profiles";
	if (load_policy(peer->euid, &work->profiles, work->diagnostic,
	    sizeof(work->diagnostic)) != 0 && errno != ENOENT)
		return -1;
	work->stage = "enumerate WLAN radios";
	if (enumerate_wlan_radios(work->radios, NETWORKD_WLAN_RADIO_MAX,
	    &work->radio_count) != 0)
		return -1;

	/* An unchanged live owner is already enabled. */
	if (networkd_managed_wlan_owner_matches(&managed_wlan, peer->euid) &&
	    managed_wlan.state == NETWORKD_WLAN_CONNECTED &&
	    managed_connection_usable())
		return 0;

	/* Retires the prior connection and makes the sender the policy owner. */
	if (wifi_policy_take(work, peer->euid) != 0)
		return -1;

	/* RF association belongs to scheduled work, never to enable dispatch. */
	schedule_automatic_work(0U);
	return wifi_request_prepare(work);
}

/*
 * Moves the enabled Wi-Fi policy to one account.
 *
 * The prior connection, whoever owned it, is retired first, and the new
 * owner's store is what automatic and manual joins read afterwards.
 */
static int
wifi_policy_take(
	struct networkd_wifi_request *work,
	uid_t owner_uid)
{
	int error;

	/* Retires the connection the prior owner made, if any. */
	work->stage = "retire prior Wi-Fi connection";
	if (managed_wlan.state != NETWORKD_WLAN_DISABLED) {
		error = retire_managed_connection(NETWORKD_WLAN_AUTO_SEARCHING, 1);
		if (error != 0)
			return -1;
	}

	/* Publishes the new owner with automatic discovery in effect. */
	work->stage = "enable Wi-Fi policy";
	error = networkd_managed_wlan_enable(&managed_wlan, owner_uid);
	if (error != 0)
		return -1;

	/* A new owner's profiles are all candidates again. */
	automatic_candidate_skip = 0U;

	/* Succeeded: the account owns the enabled policy. */
	return 0;
}

/* Pauses automatic policy before fallible teardown and radio normalization. */
static int
wifi_request_stop(
	struct networkd_wifi_request *work,
	int disable)
{
	if (!disable && wifi_disable_pending) {
		errno = EBUSY;
		return -1;
	}
	if (disable)
		wifi_disable_pending = 1;
	automatic_retry_at = 0U;
	work->stage = "retire Wi-Fi connection";
	automatic_candidate_skip = 0U;
	if (managed_wlan.owner_valid &&
	    retire_managed_connection(NETWORKD_WLAN_MANUAL_DISCONNECTED, 1) != 0)
		return disable ? wifi_disable_defer() : -1;
	if (!disable && managed_wlan.state == NETWORKD_WLAN_DISABLED)
		return 0;
	if (!disable)
		return wifi_request_prepare(work);
	work->stage = "enumerate WLAN radios";
	if (enumerate_wlan_radios(work->radios, NETWORKD_WLAN_RADIO_MAX,
	    &work->radio_count) != 0)
		return wifi_disable_defer();
	/* Never report disabled while normalization has an unresolved failure. */
	work->stage = "normalize WLAN radios";
	if (stop_wlan_radios(work->radios, work->radio_count, 1) != 0)
		return wifi_disable_defer();
	work->stage = "disable Wi-Fi policy";
	if (networkd_managed_wlan_disable(&managed_wlan) != 0)
		return wifi_disable_defer();
	wifi_disable_pending = 0;

	/* Turning Wi-Fi off and on again offers every saved key once more. */
	wifi_rejection_count = 0U;
	memset(wifi_rejections, 0, sizeof(wifi_rejections));
	return 0;
}

/* Global radio normalization may remain pending even without an L2 token. */
static int
wifi_disable_defer(void)
{
	int saved;

	saved = errno != 0 ? errno : EIO;
	managed_wlan.state = NETWORKD_WLAN_RETIRING;
	managed_wlan.retire_target = NETWORKD_WLAN_MANUAL_DISCONNECTED;
	schedule_retirement_retry();
	errno = saved;
	return -1;
}

/* Performs an explicit target transaction after complete nonmutating preflight. */
static int
wifi_request_connect(
	struct networkd_wifi_request *work,
	const struct networkd_request *request,
	const struct kern_peercred *peer)
{
	const struct wifi_conf_profile *profile;
	uint64_t selection_deadline;
	uint64_t deadline;
	size_t winner;
	uid_t store_uid;
	int l2_succeeded;
	int selected;
	int joined;
	int transfer;
	int taken;
	int saved;

	if (wifi_disable_pending) {
		errno = EBUSY;
		return -1;
	}
	work->stage = "Wi-Fi is disabled";
	if (managed_wlan.state == NETWORKD_WLAN_DISABLED) {
		errno = EPERM;
		return -1;
	}

	/*
	 * A join by an account that does not own the policy moves the policy to
	 * that account (user decision 2026-10-02, ws005-p019), so the key comes
	 * from the joining account's own store.  Nothing moves until that store
	 * holds the profile and a radio exists.
	 */
	transfer = 0;
	store_uid = managed_wlan.owner_uid;
	if (!networkd_managed_wlan_owner_matches(&managed_wlan, peer->euid)) {
		transfer = 1;
		store_uid = peer->euid;
	}

	work->stage = "load Wi-Fi profiles";
	if (load_policy(store_uid, &work->profiles,
	    work->diagnostic, sizeof(work->diagnostic)) != 0)
		return -1;
	work->stage = "unknown Wi-Fi profile";
	profile = find_profile(&work->profiles, request->ssid, request->ssid_length);
	if (profile == NULL) {
		errno = ENOENT;
		return -1;
	}
	work->stage = "enumerate WLAN radios";
	if (enumerate_wlan_radios(work->radios, NETWORKD_WLAN_RADIO_MAX,
	    &work->radio_count) != 0)
		return -1;
	if (work->radio_count == 0U) {
		work->stage = "no WLAN radio";
		errno = ENODEV;
		return -1;
	}

	/* Moves the policy to the joining account through the enable path. */
	if (transfer) {
		taken = wifi_policy_take(work, peer->euid);
		if (taken != 0)
			return -1;
	}

	/* This covers connected, reconnecting, connecting and retiring identities. */
	automatic_retry_at = 0U;
	work->stage = "retire current Wi-Fi connection";
	if (retire_managed_connection(NETWORKD_WLAN_MANUAL_DISCONNECTED, 1) != 0)
		return -1;
	if (wifi_request_prepare(work) != 0)
		return -1;
	deadline = wifi_work.deadline -
	    NETWORKD_WIFI_CLEANUP_SECONDS * 1000000ULL;
	selection_deadline = netutil_monotonic_us() +
	    NETWORKD_WLAN_SCAN_SECONDS * 1000000ULL;
	if (selection_deadline > deadline)
		selection_deadline = deadline;
	/* A network no radio sees is out of reach, which a client tells apart from a missing key. */
	work->stage = "Wi-Fi SSID not visible";
	selected = select_manual_radio(work->radios, work->radio_count, profile,
	    &winner, selection_deadline);
	if (selected != 0) {
		saved = errno;
		if (saved == ENOENT || saved == ETIMEDOUT)
			saved = ENETUNREACH;
		wifi_join_failed();
		errno = saved;
		return -1;
	}

	/* The join; a key the network refused is remembered, and any failure leaves the policy unconnected (BUG-187). */
	l2_succeeded = 0;
	work->stage = "wifi connect";
	joined = run_managed_connect(work->radios[winner].interface, profile,
	    NETWORKD_WLAN_MANUAL_DISCONNECTED, deadline, work->output,
	    (sizeof(work->output) - NETWORKD_WIFI_STATUS_RESERVE), &work->output_length, &l2_succeeded);
	if (joined != 0) {
		saved = errno;
		if (l2_succeeded) {
			work->stage = "DHCP transaction";
		} else if (saved == EACCES) {
			work->stage = "Wi-Fi key refused";
			wifi_rejection_note(store_uid, profile);
		}
		wifi_join_failed();
		errno = saved;
		return -1;
	}
	stop_losing_scans(work->radios, work->radio_count, winner);

	/* A reconnect, and the close of a session, look up the store this key came from. */
	wifi_connection_store_uid = store_uid;
	wifi_connection_store_known = 1;

	/* The key worked: it is no longer remembered as refused. */
	wifi_rejection_forget(store_uid, profile->ssid, profile->ssid_length);

	/* Succeeded: the requested network is joined. */
	return 0;
}

/* Dispatches one policy operation, then emits and clears one terminal result. */
static void
process_wifi_request(
	int client,
	struct networkd_request *request,
	const struct kern_peercred *peer)
{
	struct networkd_wifi_request work;
	uint32_t opcode;
	uint32_t status;
	int result;
	int error;

	memset(&work, 0, sizeof(work));
	work.stage = "Wi-Fi policy";
	opcode = request->header.opcode;
	result = -1;
	error = EINVAL;

	if (route_events >= 0 && process_route_events() != 0) {
		(void)close(route_events);
		route_events = -1;
	}
	if (peer == NULL) {
		errno = EACCES;
	} else if (opcode != NETWORKD_OP_WIFI_LIST &&
	    opcode != NETWORKD_OP_WIFI_ENABLE &&
	    opcode != NETWORKD_OP_WIFI_PROFILES_CHANGED && !owner_allowed(peer)) {
		work.stage = "Wi-Fi policy owner";
		errno = EPERM;
	} else {
		switch (opcode) {
		case NETWORKD_OP_WIFI_LIST:
			result = wifi_request_list(&work);
			break;
		case NETWORKD_OP_WIFI_ENABLE:
			result = wifi_request_enable(&work, peer);
			break;
		case NETWORKD_OP_WIFI_DISABLE:
			result = wifi_request_stop(&work, 1);
			break;
		case NETWORKD_OP_WIFI_DISCONNECT:
			result = wifi_request_stop(&work, 0);
			break;
		case NETWORKD_OP_WIFI_CONNECT:
			result = wifi_request_connect(&work, request, peer);
			break;
		case NETWORKD_OP_WIFI_PROFILES_CHANGED:
			wifi_profiles_changed(peer);
			result = 0;
			break;
		case NETWORKD_OP_WIFI_SESSION_OPEN:
			work.stage = "Wi-Fi session";
			result = wifi_session_open(request, peer);
			break;
		case NETWORKD_OP_WIFI_SESSION_CLOSE:
			result = wifi_session_close(&work, request, peer);
			break;
		default:
			errno = EINVAL;
			break;
		}
	}
	error = result == 0 ? 0 : (errno != 0 ? errno : EIO);
	status = result == 0 ? NETWORKD_RESULT_OK : NETWORKD_RESULT_ERROR;
	if (result != 0 && (managed_wlan.state == NETWORKD_WLAN_RETIRING ||
	    opcode == NETWORKD_OP_WIFI_LIST))
		status = NETWORKD_RESULT_DEGRADED;

	/* An explicit off is remembered across a restart, and an explicit on forgets it. */
	if (result == 0 && opcode == NETWORKD_OP_WIFI_DISABLE) {
		wifi_off_remember(1);
	} else if (result == 0 && opcode == NETWORKD_OP_WIFI_ENABLE) {
		wifi_off_remember(0);
	}

	/* A failed control operation is told with its stage (never a key or an SSID). */
	if (error != 0 && opcode != NETWORKD_OP_WIFI_LIST) {
		fprintf(stderr, "networkd: Wi-Fi %s failed at %s: %s\n",
		    operation_name(opcode) != NULL ? operation_name(opcode) : "operation",
		    work.stage, strerror(error));
	}

	/* Makes output exhaustion visible even when complete earlier lines survive. */
	if (opcode == NETWORKD_OP_WIFI_LIST && error == EOVERFLOW) {
		memcpy(work.output + work.output_length, "wifi output-truncated=1\n", 24U);
		work.output_length += 24U;
	}

	/* A policy observation is useful even when one radio or stage failed. */
	if (append_managed_status(work.output, sizeof(work.output),
	    &work.output_length) != 0 && error == 0) {
		error = errno != 0 ? errno : EOVERFLOW;
		status = NETWORKD_RESULT_ERROR;
		work.stage = "Wi-Fi status output";
	}
	send_response(client, request->header.request_id, opcode, status, error,
	    error == 0 ? NULL : (work.diagnostic[0] != '\0' ?
	    work.diagnostic : work.stage), work.output, work.output_length);
	wifi_conf_model_clear(&work.profiles);
	networkd_protocol_clear(&work, sizeof(work));
}

/* Releases one cached radio without changing managed policy. */
static void
clear_wifi_observation(
	size_t slot)
{
	/* Accounts only allocated record bytes, never the metadata array. */
	wifi_observation_bytes -= wifi_observations[slot].output_length;
	free(wifi_observations[slot].output);
	memset(&wifi_observations[slot], 0, sizeof(wifi_observations[slot]));
}

/* Retains nonsecret records under one total response-sized allocation budget. */
static void
remember_wifi_observation(
	const char *interface,
	const struct networkd_wifi_child_result *result)
{
	uint32_t ifindex;
	size_t index;
	size_t slot;
	size_t oldest;
	size_t empty;
	char *output;

	/* Rejects an invalid result or vanished identity before replacing a cache. */
	if (result->output_length == 0U ||
	    result->output_length > NETWORKD_RESPONSE_OUTPUT_MAX ||
	    result->output_records > NETWORKD_WIFI_CHILD_RECORD_MAX ||
	    interface_index(interface, &ifindex) != 0)
		return;

	/* Finds an existing name before considering any earlier empty slot. */
	slot = NETWORKD_WLAN_RADIO_MAX;
	empty = NETWORKD_WLAN_RADIO_MAX;
	oldest = 0U;
	for (index = 0U; index < NETWORKD_WLAN_RADIO_MAX; index++) {
		if (wifi_observations[index].output != NULL &&
		    strcmp(wifi_observations[index].interface, interface) == 0) {
			slot = index;
			break;
		}
		if (wifi_observations[index].output == NULL)
			empty = index;
		if (wifi_observations[index].observed_at <
		    wifi_observations[oldest].observed_at)
			oldest = index;
	}
	if (slot == NETWORKD_WLAN_RADIO_MAX)
		slot = empty == NETWORKD_WLAN_RADIO_MAX ? oldest : empty;
	clear_wifi_observation(slot);

	/* Evicts old observations until the global allocation bound permits a copy. */
	while (wifi_observation_bytes + result->output_length >
	    NETWORKD_RESPONSE_OUTPUT_MAX) {
		oldest = NETWORKD_WLAN_RADIO_MAX;
		for (index = 0U; index < NETWORKD_WLAN_RADIO_MAX; index++) {
			if (wifi_observations[index].output != NULL &&
			    (oldest == NETWORKD_WLAN_RADIO_MAX ||
			    wifi_observations[index].observed_at <
			    wifi_observations[oldest].observed_at))
				oldest = index;
		}
		if (oldest == NETWORKD_WLAN_RADIO_MAX)
			return;
		clear_wifi_observation(oldest);
	}

	/* Allocation failure loses only optional observations, never policy state. */
	output = malloc(result->output_length);
	if (output == NULL)
		return;
	memcpy(output, result->output, result->output_length);
	strcpy(wifi_observations[slot].interface, interface);
	wifi_observations[slot].ifindex = ifindex;
	wifi_observations[slot].observed_at = netutil_monotonic_us();
	wifi_observations[slot].output = output;
	wifi_observations[slot].output_length = result->output_length;
	wifi_observation_bytes += result->output_length;
}

/* Emits the same snapshot metadata for fresh and cached observations. */
static int
append_wifi_snapshot(
	const char *interface,
	const char *records,
	size_t length,
	uint64_t age,
	char *output,
	size_t capacity,
	size_t *used)
{
	char metadata[128];
	int count;
	int result;

	/* Makes missing and evicted caches explicit within the ordinary list grammar. */
	if (records == NULL)
		count = snprintf(metadata, sizeof(metadata),
		    "interface=%s snapshot-age-ms=unknown scan-snapshot=not-yet-observed\n",
		    interface);
	else
		count = snprintf(metadata, sizeof(metadata),
		    "interface=%s snapshot-age-ms=%llu scan-snapshot=available\n",
		    interface, (unsigned long long)age);

	/* Publishes only a complete metadata line. */
	if (count < 0 || (size_t)count >= sizeof(metadata) ||
	    *used > capacity || (size_t)count > capacity - *used) {
		errno = EOVERFLOW;
		return -1;
	}
	memcpy(output + *used, metadata, (size_t)count);
	*used += (size_t)count;

	/* Appends available records using the same converter as fresh results. */
	if (records != NULL) {
		result = append_wifi_records(interface, records, length,
		    output, capacity, used);
		return result;
	}
	return 0;
}

/* Observes an ongoing actor without allocating another policy/credential frame. */
static void
send_wifi_observation(
	int client,
	const struct networkd_request *request)
{
	struct networkd_wlan_radio radios[NETWORKD_WLAN_RADIO_MAX];
	char *output;
	size_t capacity;
	size_t used;
	size_t count;
	size_t radio;
	size_t slot;
	uint64_t now;
	uint64_t age;
	int result;
	int saved;

	/* Keeps this callback's stack independent of the credential and record limits. */
	capacity = NETWORKD_RESPONSE_OUTPUT_MAX - NETWORKD_DIAGNOSTIC_MAX - 4U;
	output = malloc(capacity);
	if (output == NULL) {
		send_response(client, request->header.request_id, request->header.opcode,
		    NETWORKD_RESULT_ERROR, ENOMEM, "Wi-Fi observation", NULL, 0U);
		return;
	}
	used = 0U;
	count = 0U;
	result = enumerate_wlan_radios(radios, NETWORKD_WLAN_RADIO_MAX, &count);
	now = netutil_monotonic_us();

	/* Matches cached data against both the current name and interface identity. */
	for (radio = 0U; result == 0 && radio < count; radio++) {
		for (slot = 0U; slot < NETWORKD_WLAN_RADIO_MAX; slot++) {
			if (wifi_observations[slot].output != NULL &&
			    wifi_observations[slot].ifindex == radios[radio].ifindex &&
			    strcmp(wifi_observations[slot].interface,
			    radios[radio].interface) == 0)
				break;
		}
		age = 0U;
		if (slot != NETWORKD_WLAN_RADIO_MAX &&
		    now >= wifi_observations[slot].observed_at)
			age = (now - wifi_observations[slot].observed_at) / 1000U;
		result = append_wifi_snapshot(radios[radio].interface,
		    slot == NETWORKD_WLAN_RADIO_MAX ? NULL : wifi_observations[slot].output,
		    slot == NETWORKD_WLAN_RADIO_MAX ? 0U : wifi_observations[slot].output_length,
		    age, output, capacity - NETWORKD_WIFI_STATUS_RESERVE, &used);
	}

	/* Reserves an explicit truncation marker and final state even for partial output. */
	saved = result == 0 ? 0 : (errno != 0 ? errno : EIO);
	if (saved == EOVERFLOW) {
		memcpy(output + used, "wifi output-truncated=1\n", 24U);
		used += 24U;
	}
	if (append_managed_status(output, capacity, &used) != 0 && saved == 0)
		saved = errno != 0 ? errno : EOVERFLOW;
	send_response(client, request->header.request_id, request->header.opcode,
	    saved == 0 ? NETWORKD_RESULT_OK : NETWORKD_RESULT_DEGRADED, saved,
	    saved == 0 ? NULL : "Wi-Fi observation", output, used);
	free(output);
}

/* Serves read-only wired status without entering a request or child transaction. */
static void
send_wired_observation(
	int client,
	const struct networkd_request *request)
{
	char *output;
	int result;
	int error;

	/* Uses a bounded temporary buffer instead of growing the active RF stack. */
	output = malloc(NETWORKD_RESPONSE_OUTPUT_MAX);
	if (output == NULL) {
		send_response(client, request->header.request_id, request->header.opcode,
		    NETWORKD_RESULT_ERROR, ENOMEM, "SHOW", NULL, 0U);
		return;
	}
	result = show_interfaces(request->interface[0] != '\0' ?
	    request->interface : NULL, output, NETWORKD_RESPONSE_OUTPUT_MAX);
	error = result == 0 ? 0 : (errno != 0 ? errno : EIO);
	send_response(client, request->header.request_id, request->header.opcode,
	    result == 0 ? NETWORKD_RESULT_OK : NETWORKD_RESULT_ERROR, error,
	    result == 0 ? NULL : "SHOW", result == 0 ? output : NULL,
	    result == 0 ? strlen(output) : 0U);
	free(output);
}

/*
 * Wakes the automatic search when a store it reads has changed.
 *
 * The stores automatic joining reads are the system's, the policy owner's
 * and those of the accounts with an open session (ws005-p024); a change to
 * any other account's store changes nothing networkd would join.
 */
static void
wifi_profiles_changed(
	const struct kern_peercred *peer)
{
	int connected_store;
	int owner;
	int session;

	/* A notice from nobody known changes nothing. */
	if (peer == NULL)
		return;

	/* The changed store may hold a corrected key: its refusals are forgotten. */
	wifi_rejection_forget_store(peer->euid);

	/* The connection made from the changed store is checked against it later. */
	connected_store = connection_from_store(peer->euid);
	if (connected_store)
		wifi_connection_recheck_due = 1;

	/* Root's store is the system's, which is always a candidate. */
	if (peer->euid == 0) {
		wifi_candidates_changed();
		return;
	}

	/* The policy owner's store and an open session's store are candidates. */
	owner = networkd_managed_wlan_owner_matches(&managed_wlan, peer->euid);
	session = wifi_session_find(peer->euid);
	if (!owner && session < 0)
		return;

	/* Succeeded: the changed store is one automatic joining reads. */
	wifi_candidates_changed();
}

/* Starts the automatic search over with every candidate, at once. */
static void
wifi_candidates_changed(
	void)
{
	size_t index;

	/* Every candidate is tried again, against every radio's current scan. */
	automatic_candidate_skip = 0U;
	for (index = 0U; index < known_wlan_radio_count; index++)
		known_wlan_radios[index].consumed_snapshot_generation = 0U;

	/* A wave already running starts over with the new candidates. */
	if (wifi_work.background)
		wifi_work.profiles_changed = 1;

	/* A search waiting for its next wave runs it now. */
	if (managed_wlan.state == NETWORKD_WLAN_AUTO_SEARCHING)
		schedule_automatic_work(0U);
}

/* Remembers that a network refused the key one account's store holds for it. */
static void
wifi_rejection_note(
	uid_t account,
	const struct wifi_conf_profile *profile)
{
	struct networkd_wifi_rejection *rejection;
	int known;

	/* A refusal already remembered stays as it is. */
	known = wifi_rejection_find(account, profile->ssid, profile->ssid_length);
	if (known >= 0)
		return;

	/* A full list gives up its oldest refusal. */
	if (wifi_rejection_count == NETWORKD_WIFI_REJECTION_MAX) {
		memmove(&wifi_rejections[0], &wifi_rejections[1],
		    (NETWORKD_WIFI_REJECTION_MAX - 1U) * sizeof(wifi_rejections[0]));
		wifi_rejection_count--;
	}

	/* The refusal: the store and the SSID, never the key. */
	rejection = &wifi_rejections[wifi_rejection_count];
	memset(rejection, 0, sizeof(*rejection));
	rejection->account = account;
	rejection->ssid_length = profile->ssid_length;
	memcpy(rejection->ssid, profile->ssid, profile->ssid_length);
	wifi_rejection_count++;
	fprintf(stderr, "networkd: Wi-Fi key of account %u refused by the network; not tried again until it changes\n",
	    (unsigned)account);
}

/* Finds the refusal of one store's key for one SSID; returns its slot, or -1 when there is none. */
static int
wifi_rejection_find(
	uid_t account,
	const unsigned char *ssid,
	size_t ssid_length)
{
	const struct networkd_wifi_rejection *rejection;
	size_t index;
	int differs;

	/* Each remembered refusal, compared by store and SSID. */
	for (index = 0U; index < wifi_rejection_count; index++) {
		rejection = &wifi_rejections[index];
		if (rejection->account != account || rejection->ssid_length != ssid_length)
			continue;
		differs = memcmp(rejection->ssid, ssid, ssid_length);
		if (differs != 0)
			continue;

		/* Succeeded: this store's key for this SSID was refused. */
		return (int)index;
	}

	/* No refusal is remembered for them. */
	return -1;
}

/* Forgets the refusal of one store's key for one SSID. */
static void
wifi_rejection_forget(
	uid_t account,
	const unsigned char *ssid,
	size_t ssid_length)
{
	size_t slot;
	int found;

	/* Nothing to forget when no refusal is remembered. */
	found = wifi_rejection_find(account, ssid, ssid_length);
	if (found < 0)
		return;

	/* The later refusals move down over it. */
	slot = (size_t)found;
	memmove(&wifi_rejections[slot], &wifi_rejections[slot + 1U],
	    (wifi_rejection_count - slot - 1U) * sizeof(wifi_rejections[0]));
	wifi_rejection_count--;
	memset(&wifi_rejections[wifi_rejection_count], 0, sizeof(wifi_rejections[0]));
}

/* Forgets every refusal of one store's keys, whose contents may have changed. */
static void
wifi_rejection_forget_store(
	uid_t account)
{
	size_t index;

	/* Removes this store's refusals, keeping the others in their order. */
	index = 0U;
	while (index < wifi_rejection_count) {
		if (wifi_rejections[index].account != account) {
			index++;
			continue;
		}

		/* The later refusals move down over this one. */
		memmove(&wifi_rejections[index], &wifi_rejections[index + 1U],
		    (wifi_rejection_count - index - 1U) * sizeof(wifi_rejections[0]));
		wifi_rejection_count--;
		memset(&wifi_rejections[wifi_rejection_count], 0, sizeof(wifi_rejections[0]));
	}
}

/*
 * Records whether Wi-Fi was turned off by request, for the next boot.
 *
 * The record is an empty file that only root can change; net startup
 * leaves Wi-Fi off while it exists.  A failure to record is told and
 * otherwise ignored: the switch itself has already happened.
 */
static void
wifi_off_remember(
	int off)
{
	int descriptor;
	int made;
	int removed;

	/* On: the record goes (it may never have been made). */
	if (!off) {
		removed = unlink(NETWORKD_WIFI_OFF_PATH);
		if (removed != 0 && errno != ENOENT)
			fprintf(stderr, "networkd: cannot forget that Wi-Fi was off: %s\n", strerror(errno));
		return;
	}

	/* Off: the directory, which a fresh system may lack. */
	made = mkdir(NETWORKD_WIFI_OFF_DIRECTORY, 0755);
	if (made != 0 && errno != EEXIST) {
		fprintf(stderr, "networkd: cannot remember that Wi-Fi is off: %s\n", strerror(errno));
		return;
	}

	/* Then the record itself. */
	descriptor = open(NETWORKD_WIFI_OFF_PATH, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
	if (descriptor < 0) {
		fprintf(stderr, "networkd: cannot remember that Wi-Fi is off: %s\n", strerror(errno));
		return;
	}
	(void)close(descriptor);
}

/*
 * Ends the connection when its network is no longer saved in the store
 * that gave its key.
 *
 * Deleting a network one is on means leaving it (net wifi delete); the
 * search then goes on with the networks still saved.  A store that cannot
 * be read is left alone: the connection keeps going on what it has.
 */
static void
recheck_connection_profile(
	void)
{
	struct wifi_conf_model model;
	char diagnostic[WIFI_CONF_DIAGNOSTIC_MAX];
	const struct wifi_conf_profile *profile;
	int loaded;
	int saved;
	int retired;

	/* Only a connection that stands, with a known store, is checked. */
	if (managed_wlan.state != NETWORKD_WLAN_CONNECTED)
		return;
	if (!wifi_connection_store_known)
		return;

	/* The store as it is now; a missing store has no networks. */
	wifi_conf_model_init(&model);
	memset(diagnostic, 0, sizeof(diagnostic));
	loaded = load_policy(wifi_connection_store_uid, &model, diagnostic, sizeof(diagnostic));
	saved = errno;
	profile = NULL;
	if (loaded == 0)
		profile = find_profile(&model, managed_wlan.connection.ssid, managed_wlan.connection.ssid_length);
	wifi_conf_model_clear(&model);
	wifi_conf_explicit_clear(diagnostic, sizeof(diagnostic));

	/* A store that could not be read for another reason changes nothing. */
	if (loaded != 0 && saved != ENOENT)
		return;

	/* The network is still saved: the connection stays. */
	if (profile != NULL)
		return;

	/* The network was deleted: the connection ends and the search goes on. */
	fprintf(stderr, "networkd: the connected network was deleted from account %u's store; leaving it\n",
	    (unsigned)wifi_connection_store_uid);
	retired = retire_managed_connection(NETWORKD_WLAN_AUTO_SEARCHING, 1);
	if (retired != 0)
		return;
	automatic_candidate_skip = 0U;
	schedule_automatic_work(0U);
}

/*
 * Leaves the policy on and unconnected after an explicit join failed
 * (BUG-187, the user's decision of 2026-10-05).
 *
 * The join left the earlier connection on purpose, and a join the person
 * chose that failed is not followed by a join of another saved network:
 * the radio stays on and unconnected (manual-disconnected) until the
 * person acts again -- joins a network, or turns Wi-Fi off and on.  A new
 * boot and a new login search on their own as before.  This replaces
 * ws005-p020's searching again after a failed join (q631).
 */
static void
wifi_join_failed(
	void)
{
	/* No automatic wave is planned for a policy the person left unconnected. */
	if (managed_wlan.state == NETWORKD_WLAN_MANUAL_DISCONNECTED)
		automatic_retry_at = 0U;

	/* The failure is told; the policy stays as the join left it. */
	fprintf(stderr, "networkd: explicit Wi-Fi join failed; not joining another network until the user acts\n");
}

/*
 * Records a desktop's asking for scans, or its end, and answers it
 * (ws089-p021).
 *
 * A start asks for NETWORKD_WIFI_SCAN_LEASE_SECONDS from now and, when the
 * radios are on and left unconnected, starts a scan at once; a stop ends
 * the asking.  The scans themselves are read with WIFI_LIST.
 */
static void
wifi_scan_request(
	int client,
	const struct networkd_request *request)
{
	uint64_t now;
	int wanted;

	/* The lease: renewed by a start, ended by a stop. */
	now = netutil_monotonic_us();
	wanted = wifi_scan_wanted();
	if (request->header.opcode == NETWORKD_OP_WIFI_SCAN_START) {
		wifi_scan_until = now + (uint64_t)NETWORKD_WIFI_SCAN_LEASE_SECONDS * 1000000ULL;

		/* A first start scans at once; a renewal keeps the plan it has. */
		if (!wanted || wifi_scan_due == 0U)
			wifi_scan_due = now;
	} else {
		wifi_scan_until = 0U;
		wifi_scan_due = 0U;
	}

	/* The answer only says the daemon heard; the watchers see scan= change. */
	send_response(client, request->header.request_id, request->header.opcode,
	    NETWORKD_RESULT_OK, 0, NULL, NULL, 0U);
	notify_state_changed();
}

/* Tells whether a desktop's lease on fresh scans holds now. */
static int
wifi_scan_wanted(
	void)
{
	uint64_t now;

	/* Nobody asked, or the asking ended. */
	if (wifi_scan_until == 0U)
		return 0;

	/* The lease ran out: the desktop that asked went without saying so. */
	now = netutil_monotonic_us();
	if (now >= wifi_scan_until)
		return 0;

	/* Succeeded: the lease holds. */
	return 1;
}

/*
 * Reports how long the event loop may wait before an asked-for scan is due
 * or the lease ends (0: now), or -1 when nothing about scans is waited for.
 */
static int
wifi_scan_poll_timeout(
	void)
{
	uint64_t milliseconds;
	uint64_t deadline;
	uint64_t now;

	/* Nobody asks: nothing is waited for (an ended lease plans nothing more). */
	if (wifi_scan_until == 0U)
		return -1;

	/* A lease that ran out is forgotten now, so that the watchers hear scan=0. */
	now = netutil_monotonic_us();
	if (now >= wifi_scan_until) {
		wifi_scan_until = 0U;
		wifi_scan_due = 0U;
		return 0;
	}

	/* The lease's end, or the next scan when one is planned for an idle policy. */
	deadline = wifi_scan_until;
	if (wifi_scan_due != 0U &&
	    managed_wlan.state == NETWORKD_WLAN_MANUAL_DISCONNECTED &&
	    wifi_scan_due < deadline)
		deadline = wifi_scan_due;

	/* Due now. */
	if (deadline <= now)
		return 0;

	/* The milliseconds to wait, rounded up. */
	milliseconds = (deadline - now + 999ULL) / 1000ULL;
	if (milliseconds > (uint64_t)INT_MAX)
		return INT_MAX;
	return (int)milliseconds;
}

/*
 * Starts a scan on each radio that is on, idle and not scanning, while a
 * desktop asks for scans and the policy is on and left unconnected
 * (ws089-p021), and plans the next one.
 *
 * The automatic search scans on its own and a connected radio cannot scan,
 * so only manual-disconnected is served.  The work is background work: a
 * person's request that arrives meanwhile stops it.
 */
static void
run_requested_scan(
	void)
{
	struct networkd_wlan_radio radios[NETWORKD_WLAN_RADIO_MAX];
	struct networkd_wifi_child_result child;
	size_t radio_count;
	size_t index;
	uint64_t now;
	int error;
	int saved;

	/* The lease ended: nothing is planned any more. */
	now = netutil_monotonic_us();
	if (!wifi_scan_wanted()) {
		wifi_scan_until = 0U;
		wifi_scan_due = 0U;
		return;
	}

	/* Only an idle, unconnected policy is scanned for; the next scan waits for one. */
	if (managed_wlan.state != NETWORKD_WLAN_MANUAL_DISCONNECTED) {
		wifi_scan_due = now + (uint64_t)NETWORKD_WLAN_RESCAN_SECONDS * 1000000ULL;
		return;
	}

	/* The radios as they are now. */
	wifi_work_begin(NETWORKD_OP_WIFI_LIST, 1);
	memset(radios, 0, sizeof(radios));
	radio_count = 0U;
	error = enumerate_wlan_radios(radios, NETWORKD_WLAN_RADIO_MAX, &radio_count);
	if (error != 0)
		radio_count = 0U;

	/* A scan on each radio that is up, idle and not scanning already. */
	for (index = 0U; index < radio_count; index++) {
		if (wifi_work.cancelled)
			break;
		if (radios[index].observation_error != 0 || radios[index].stop_flags != 0U)
			continue;
		if (!radios[index].administrative_up || radios[index].association_active)
			continue;
		if (radios[index].scan_state == WLAN_SCAN_RUNNING)
			continue;

		/* The radio's scan; one that fails is tried again at the next plan. */
		memset(&child, 0, sizeof(child));
		error = run_wifi(radios[index].interface, "search-start", NULL, 10U, &child);
		if (error != 0) {
			saved = errno;
			if (saved == 0)
				saved = EIO;
			fprintf(stderr, "networkd: %s: asked-for scan: %s\n",
			    radios[index].interface, strerror(saved));
		}
		networkd_wifi_child_result_clear(&child);
	}

	/* The work ends, and the next scan is planned. */
	wifi_work_end();
	wifi_scan_due = netutil_monotonic_us() + (uint64_t)NETWORKD_WLAN_RESCAN_SECONDS * 1000000ULL;
}

/*
 * Finds the interface the default route goes through: 1 with its index,
 * 0 when the table has no default route, -1 when the table cannot be read.
 * The first default route of the table is the one in effect (BUG-189).
 */
static int
default_route_index(
	uint32_t *ifindex)
{
	const struct sockaddr_in *destination;
	const struct sockaddr_in *mask;
	struct rtentry route;
	unsigned ordinal;
	int descriptor;
	int error;
	int found;

	/* A socket for the route table. */
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return -1;

	/* Each route of the table until the first default one. */
	found = 0;
	error = 0;
	for (ordinal = 0U;; ordinal++) {
		memset(&route, 0, sizeof(route));
		route.rt_index = ordinal;
		error = ioctl(descriptor, SIOCGRTENTRY, &route);
		if (error != 0)
			break;

		/* A route to everything with no mask is the default one. */
		destination = (const struct sockaddr_in *)&route.rt_dst;
		mask = (const struct sockaddr_in *)&route.rt_genmask;
		if (destination->sin_addr.s_addr != 0U || mask->sin_addr.s_addr != 0U)
			continue;
		*ifindex = route.rt_ifindex;
		found = 1;
		break;
	}

	/* The end of the table is ENOENT; anything else is a table not read. */
	if (found == 0 && error != 0 && errno != ENOENT) {
		(void)close(descriptor);
		return -1;
	}
	(void)close(descriptor);

	/* Succeeded: whether a default route was found. */
	return found;
}

/* Resumes deferred control only after the interrupted actor has unwound. */
static void
dispatch_pending_wifi(
	void)
{
	int client;

	client = wifi_pending_client;
	wifi_pending_client = -1;
	dispatch_request(client, &wifi_pending.request, wifi_pending.role,
	    &wifi_pending.peer);
	(void)close(client);
	networkd_protocol_clear(&wifi_pending, sizeof(wifi_pending));
}

/* Reports whether the outer loop must continue receiving an accepted request. */
static int
control_input_pending(
	void)
{
	size_t index;

	/* Retains a short poll interval only while bounded ingress slots are occupied. */
	for (index = 0U; index < NETWORKD_CONTROL_INPUT_MAX; index++) {
		if (control_inputs[index].active)
			return 1;
	}
	return 0;
}

/* Receives available bytes without charging a stalled client to the RF wait. */
static int
receive_wait_request(
	struct networkd_request *request,
	struct kern_peercred *peer,
	enum networkd_client_role *role)
{
	struct networkd_control_input *input;
	struct pollfd ready;
	struct timeval timeout;
	unsigned char *newline;
	size_t index;
	size_t header_length;
	size_t iteration;
	ssize_t count;
	int client;
	int error;
	int complete;

	/* Admits at most one new peer per callback under a fixed four-client bound. */
	for (index = 0U; index < NETWORKD_CONTROL_INPUT_MAX; index++) {
		if (!control_inputs[index].active)
			break;
	}
	ready.fd = control_listener;
	ready.events = POLLIN;
	ready.revents = 0;
	if (index < NETWORKD_CONTROL_INPUT_MAX && poll(&ready, 1U, 0) > 0 &&
	    (ready.revents & POLLIN) != 0) {
		client = accept4(control_listener, NULL, NULL, SOCK_CLOEXEC);
		if (client >= 0) {
			input = &control_inputs[index];
			timeout.tv_sec = 0;
			timeout.tv_usec = 200000;

			/* Bounds response backpressure independently of partial request input. */
			if (setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout,
			    sizeof(timeout)) != 0 ||
			    authenticate_client(client, &input->peer, &input->role) != 0) {
				(void)close(client);
				memset(input, 0, sizeof(*input));
			} else {
				input->descriptor = client;
				input->deadline = netutil_monotonic_us() + 5000000ULL;
				input->active = 1;
			}
		}
	}

	/* Consumes bounded chunks, including EOF, without waiting for another byte. */
	for (index = 0U; index < NETWORKD_CONTROL_INPUT_MAX; index++) {
		input = &control_inputs[index];
		if (!input->active)
			continue;
		error = 0;
		complete = 0;
		if (netutil_monotonic_us() >= input->deadline)
			error = ETIMEDOUT;
		for (iteration = 0U; error == 0 && iteration < 2U; iteration++) {
			count = recv(input->descriptor, input->bytes + input->used,
			    sizeof(input->bytes) - input->used, MSG_DONTWAIT);
			if (count < 0) {
				if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
					error = errno != 0 ? errno : EIO;
				break;
			}
			if (count == 0) {
				complete = 1;
				break;
			}
			input->used += (size_t)count;
			if (input->used == sizeof(input->bytes))
				error = EMSGSIZE;
		}

		/* Requires an exact header/payload and write-side EOF before admission. */
		if (complete && error == 0) {
			newline = memchr(input->bytes, '\n', input->used);
			header_length = newline == NULL ? 0U :
			    (size_t)(newline - input->bytes) + 1U;
			if (header_length == 0U || header_length > NETWORKD_PROTOCOL_HEADER_MAX) {
				error = EINVAL;
			} else if (networkd_protocol_header_decode((char *)input->bytes,
			    header_length, &request->header) != 0) {
				error = errno != 0 ? errno : EINVAL;
			} else if (request->header.payload_length > NETWORKD_REQUEST_MAX ||
			    request->header.payload_length != input->used - header_length) {
				error = EMSGSIZE;
			} else {
				memcpy(request->payload, input->bytes + header_length,
				    request->header.payload_length);
				if (decode_request(request) != 0)
					error = errno != 0 ? errno : EINVAL;
			}
		}

		/* Detaches complete transport before any dispatcher can invoke another wait. */
		if (complete && error == 0) {
			client = input->descriptor;
			memcpy(peer, &input->peer, sizeof(*peer));
			*role = input->role;
			networkd_protocol_clear(input, sizeof(*input));
			return client;
		}

		/* Drops malformed or expired peers while continuing other accepted inputs. */
		if (error != 0) {
			send_error(input->descriptor, error, "incomplete or malformed request");
			(void)close(input->descriptor);
			networkd_protocol_clear(input, sizeof(*input));
			networkd_protocol_clear(request, sizeof(*request));
		}
	}

	/* Leaves incomplete peers owned by their original absolute ingress deadline. */
	return -1;
}

/* Services reads/notifications; defers stop intent without recursive mutation. */
static int
service_wifi_wait(
	void)
{
	struct networkd_request request;
	struct kern_peercred peer;
	enum networkd_client_role role;
	int client;
	int error;
	int deferred;
	int interrupts;
	uint64_t cleanup_deadline;

	/*
	 * The watchers hear a state the running work reached (connecting, while
	 * it joins), not only the one it ends in (BUG-185: the desktop never
	 * saw Connecting).
	 */
	deliver_state_changes();

	if (wifi_work.cleanup)
		return 0;
	if (stopping || wifi_work.cancelled)
		return EINTR;
	if (control_listener < 0)
		return 0;
	memset(&request, 0, sizeof(request));
	client = receive_wait_request(&request, &peer, &role);
	if (client < 0)
		return 0;
	deferred = 0;
	if (wifi_work.deadline == 0U) {
		dispatch_request(client, &request, role, &peer);
	} else if (operation_name(request.header.opcode) == NULL ||
	    !operation_allowed(role, operation_name(request.header.opcode))) {
		send_response(client, request.header.request_id, request.header.opcode,
		    NETWORKD_RESULT_ERROR, EPERM, "operation denied", NULL, 0U);
	} else if (request.header.opcode == NETWORKD_OP_SHOW) {
		send_wired_observation(client, &request);
	} else if (!networkd_confirmed_active(&confirmed) &&
	    (request.header.opcode == NETWORKD_OP_CONFIRMED_CHECK ||
	    request.header.opcode == NETWORKD_OP_CONFIRMED_DISARM)) {
		/* The inactive transaction cannot roll back or interrupt RF work. */
		if (request.header.opcode == NETWORKD_OP_CONFIRMED_CHECK)
			error = networkd_confirmed_check(&confirmed, request.token);
		else
			error = networkd_confirmed_disarm(&confirmed, request.token);
		error = error == 0 ? 0 : (errno != 0 ? errno : EIO);
		send_response(client, request.header.request_id, request.header.opcode,
		    error == 0 ? NETWORKD_RESULT_OK : NETWORKD_RESULT_ERROR, error,
		    error == 0 ? NULL : "confirmed transaction", NULL, 0U);
	} else if (request.header.opcode == NETWORKD_OP_WIFI_LIST ||
	    (request.header.opcode == NETWORKD_OP_WIFI_ENABLE &&
	    networkd_managed_wlan_owner_matches(&managed_wlan, peer.euid) &&
	    (managed_wlan.state == NETWORKD_WLAN_AUTO_SEARCHING ||
	    managed_wlan.state == NETWORKD_WLAN_CONNECTING ||
	    managed_wlan.state == NETWORKD_WLAN_CONNECTED ||
	    managed_wlan.state == NETWORKD_WLAN_RECONNECTING))) {
		send_wifi_observation(client, &request);
	} else if (request.header.opcode == NETWORKD_OP_WIFI_SCAN_START ||
	    request.header.opcode == NETWORKD_OP_WIFI_SCAN_STOP) {
		/* Asking for scans only records the lease; the running work goes on. */
		wifi_scan_request(client, &request);
	} else if (request.header.opcode == NETWORKD_OP_SLEEP_END) {
		/* The sleep's end is never kept waiting; what it takes up again waits for the loop (ws052-p010). */
		handle_sleep_request(client, &request);
	} else if (request.header.opcode == NETWORKD_OP_WIFI_PROFILES_CHANGED) {
		wifi_profiles_changed(&peer);
		send_response(client, request.header.request_id, request.header.opcode,
		    NETWORKD_RESULT_OK, 0, NULL, NULL, 0U);
	} else if (request.header.opcode == NETWORKD_OP_WIFI_SESSION_OPEN) {
		/* An opened session only adds candidates, so the running work is not stopped. */
		error = 0;
		if (wifi_session_open(&request, &peer) != 0)
			error = errno != 0 ? errno : EIO;
		send_response(client, request.header.request_id, request.header.opcode,
		    error == 0 ? NETWORKD_RESULT_OK : NETWORKD_RESULT_ERROR, error,
		    error == 0 ? NULL : "Wi-Fi session", NULL, 0U);
	} else {
		error = EBUSY;
		interrupts = 0;
		if (wifi_work.background)
			interrupts = interrupts_background_work(&request, &peer);
		if (interrupts) {
			wifi_pending_client = client;
			wifi_pending.role = role;
			memcpy(&wifi_pending.request, &request, sizeof(request));
			memcpy(&wifi_pending.peer, &peer, sizeof(peer));
			wifi_work.cancelled = 1;

			/* Queued control waits only for the reserved cleanup interval. */
			cleanup_deadline = netutil_monotonic_us() +
			    NETWORKD_WIFI_CLEANUP_SECONDS * 1000000ULL;
			if (wifi_work.deadline > cleanup_deadline)
				wifi_work.deadline = cleanup_deadline;
			deferred = 1;
		} else {
			if (request.header.opcode != NETWORKD_OP_WIFI_ENABLE &&
			    request.header.opcode >= NETWORKD_OP_WIFI_ENABLE &&
			    request.header.opcode <= NETWORKD_OP_WIFI_SESSION_CLOSE &&
			    !owner_allowed(&peer))
				error = EPERM;
			send_response(client, request.header.request_id, request.header.opcode,
			    NETWORKD_RESULT_ERROR, error,
			    "Wi-Fi operation in progress; retry", NULL, 0U);
		}
	}
	networkd_protocol_clear(&request, sizeof(request));
	if (!deferred)
		(void)close(client);
	return deferred ? EINTR : 0;
}

/*
 * Answers the machine's sleep (ws052-p010).  SLEEP_PREPARE records the
 * Wi-Fi policy, retires the connection and turns the radios off without
 * changing the policy; a second one while asleep only moves the sleep's
 * end; a confirmed transaction under way refuses it (EBUSY), and so does
 * a radio that would not go off (the policy is then taken up again).
 * SLEEP_END is never refused, even inside other Wi-Fi work: the recorded
 * policy is taken up again by the event loop's next pass (or nothing, when
 * networkd was not asleep).
 */
static void
handle_sleep_request(
	int client,
	const struct networkd_request *request)
{
	enum networkd_sleep_resume resume;
	uint32_t opcode;
	int active;
	int began;
	int error;

	/* The end: the recorded policy is taken up again by the loop's next pass. */
	opcode = request->header.opcode;
	if (opcode == NETWORKD_OP_SLEEP_END) {
		resume = networkd_sleep_end(&sleep_state);
		sleep_resume_later(resume, "end");
		send_response(client, request->header.request_id, opcode,
		    NETWORKD_RESULT_OK, 0, NULL, NULL, 0U);
		return;
	}

	/* A confirmed transaction under way keeps the network as it is until it is decided. */
	active = networkd_confirmed_active(&confirmed);
	if (active) {
		send_response(client, request->header.request_id, opcode,
		    NETWORKD_RESULT_ERROR, EBUSY, "confirmed transaction", NULL, 0U);
		return;
	}

	/* An end not yet taken up is taken up first, so that the record is the policy's own. */
	if (sleep_resume_due) {
		sleep_resume_due = 0U;
		sleep_resume(sleep_resume_pending, sleep_resume_via);
	}

	/* The policy recorded; asleep already, only the end moves. */
	began = networkd_sleep_begin(&sleep_state, managed_wlan.state,
	    managed_wlan.retire_target, netutil_monotonic_us());
	if (!began) {
		send_response(client, request->header.request_id, opcode,
		    NETWORKD_RESULT_OK, 0, NULL, NULL, 0U);
		return;
	}

	/* The connection retired and the radios off; one that stays on ends the sleep again. */
	error = sleep_radios_off();
	if (error != 0) {
		resume = networkd_sleep_end(&sleep_state);
		sleep_resume_later(resume, "failed");
		send_response(client, request->header.request_id, opcode,
		    NETWORKD_RESULT_ERROR, error, "sleep: Wi-Fi radio", NULL, 0U);
		return;
	}

	/* Succeeded: asleep, the radios off. */
	fprintf(stderr, "networkd: asleep (Wi-Fi state %d recorded)\n",
	    (int)sleep_state.recorded);
	send_response(client, request->header.request_id, opcode,
	    NETWORKD_RESULT_OK, 0, NULL, NULL, 0U);
}

/*
 * Retires the Wi-Fi connection, leaving the enabled policy disconnected
 * (not changed: SLEEP_END takes it up again), and turns every radio off,
 * checked.  Returns 0, or the errno of the step that failed.
 */
static int
sleep_radios_off(
	void)
{
	struct networkd_wlan_radio radios[NETWORKD_WLAN_RADIO_MAX];
	size_t radio_count;
	int result;

	/* The connection retires first; no automatic attempt is due while asleep. */
	automatic_retry_at = 0U;
	if (managed_wlan.owner_valid &&
	    managed_wlan.state != NETWORKD_WLAN_DISABLED) {
		result = retire_managed_connection(NETWORKD_WLAN_MANUAL_DISCONNECTED, 1);
		if (result != 0)
			return errno != 0 ? errno : EIO;
	}

	/* Every radio there is. */
	radio_count = 0U;
	result = enumerate_wlan_radios(radios, NETWORKD_WLAN_RADIO_MAX, &radio_count);
	if (result != 0)
		return errno != 0 ? errno : EIO;

	/* Each one stopped and seen to be off. */
	result = stop_wlan_radios(radios, radio_count, 1);
	if (result != 0)
		return errno != 0 ? errno : EIO;

	/* Succeeded: no radio is on. */
	return 0;
}

/* Leaves what the end of a sleep asks for to the event loop's next pass (run_due_work). */
static void
sleep_resume_later(
	enum networkd_sleep_resume resume,
	const char *via)
{
	/* One end is waiting; a later one replaces it. */
	sleep_resume_pending = resume;
	sleep_resume_via = via;
	sleep_resume_due = 1U;
}

/*
 * Takes up again what the end of a sleep asks for: the automatic search
 * for the saved networks (which brings the radios up), the radios up with
 * no connection, or nothing.  via names the end in the log ("end", the
 * safety's "safety", a failed SLEEP_PREPARE's "failed").
 */
static void
sleep_resume(
	enum networkd_sleep_resume resume,
	const char *via)
{
	struct networkd_wlan_radio radios[NETWORKD_WLAN_RADIO_MAX];
	size_t radio_count;
	const char *name;
	int result;

	/* What the recorded policy asks for. */
	name = "none";
	switch (resume) {
	case NETWORKD_SLEEP_RESUME_SEARCH:
		/* The automatic search, at once; it brings the radios up (run_automatic_work). */
		name = "search";
		result = networkd_managed_wlan_resume(&managed_wlan, NETWORKD_WLAN_AUTO_SEARCHING);
		if (result == 0)
			schedule_automatic_work(0U);
		break;
	case NETWORKD_SLEEP_RESUME_MANUAL:
		/* The radios up, and no connection of networkd's own. */
		name = "manual";
		radio_count = 0U;
		result = enumerate_wlan_radios(radios, NETWORKD_WLAN_RADIO_MAX, &radio_count);
		if (result == 0)
			(void)prepare_wlan_radios(radios, radio_count, NULL, 0U, NULL);
		break;
	case NETWORKD_SLEEP_RESUME_NONE:
	default:
		/* Nothing: Wi-Fi was off, or networkd was not asleep. */
		break;
	}

	/* The end, in the log. */
	fprintf(stderr, "networkd: awake via=%s resume=%s\n", via, name);
}

/*
 * Tests whether a request arriving during background Wi-Fi work stops it.
 *
 * Wired requests always do.  So does an explicit Wi-Fi on, off, join or
 * disconnect from a peer allowed to make it: the user decided on 2026-10-02
 * (ws005-p019) that a person's choice is never kept waiting behind the
 * automatic search, which is cancelled and resumes from the new state.
 */
static int
interrupts_background_work(
	const struct networkd_request *request,
	const struct kern_peercred *peer)
{
	uint32_t opcode;
	int allowed;

	/* A wired request is never held behind automatic Wi-Fi work. */
	opcode = request->header.opcode;
	if (opcode < NETWORKD_OP_WIFI_ENABLE)
		return 1;

	/* Turning Wi-Fi on is open to every admitted peer, as on dispatch. */
	if (opcode == NETWORKD_OP_WIFI_ENABLE)
		return 1;

	/* The machine's sleep stops the automatic search (ws052-p010; only root may ask it). */
	if (opcode == NETWORKD_OP_SLEEP_PREPARE)
		return 1;

	/*
	 * Listing and profile notices are served without stopping the work.
	 * The close of a session stops it, because a connection made from the
	 * closing account's store may have to end (ws005-p024).
	 */
	if (opcode != NETWORKD_OP_WIFI_DISABLE &&
	    opcode != NETWORKD_OP_WIFI_CONNECT &&
	    opcode != NETWORKD_OP_WIFI_DISCONNECT &&
	    opcode != NETWORKD_OP_WIFI_SESSION_CLOSE)
		return 0;

	/* Off, join, disconnect and a session's close stop the work only for a peer allowed them. */
	allowed = owner_allowed(peer);
	if (!allowed)
		return 0;

	/* Succeeded: the explicit request takes the place of the search. */
	return 1;
}

/* Starts one actor transaction; every exit goes through its outer wrapper. */
static void
wifi_work_begin(
	uint32_t opcode,
	int background)
{
	memset(&wifi_work, 0, sizeof(wifi_work));
	wifi_work.deadline = netutil_monotonic_us() +
	    NETWORKD_WIFI_REQUEST_SECONDS(opcode) * 1000000ULL;
	wifi_work.background = background;
}

/* Drops the completed transaction without changing persistent policy intent. */
static void
wifi_work_end(
	void)
{
	memset(&wifi_work, 0, sizeof(wifi_work));

	/* Starts the next independent retirement at the initial retry interval. */
	if (managed_wlan.state != NETWORKD_WLAN_RETIRING)
		retirement_retry_seconds = NETWORKD_WLAN_RESCAN_SECONDS;
}

/* Gives every primitive and transaction stage the same absolute request end. */
static void
handle_wifi_request(
	int client,
	struct networkd_request *request,
	const struct kern_peercred *peer)
{
	wifi_work_begin(request->header.opcode, 0);
	process_wifi_request(client, request, peer);
	wifi_work_end();
}

/* Reads and decodes one complete request frame. */
static int
read_request(
	int descriptor,
	struct networkd_request *request)
{
	struct timeval timeout;

	/* Installs a finite transport deadline before consuming bytes. */
	if (request == NULL) {
		errno = EINVAL;
		return -1;
	}
	timeout.tv_sec = 5;
	timeout.tv_usec = 0;
	if (setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
	    sizeof(timeout)) != 0 ||
	    setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout,
	    sizeof(timeout)) != 0)
		return -1;

	/* Reads and semantically validates one request payload. */
	if (networkd_protocol_read_frame_timed(descriptor, &request->header,
	    request->payload, sizeof(request->payload),
	    NETWORKD_REQUEST_MAX, 5U) != 0)
		return -1;

	/*
	 * Every request but a subscription ends with the client closing its
	 * direction, which is what proves there is nothing more behind it.
	 *
	 * A watcher must not do that.  Once one direction of a stream is
	 * closed the kernel reports the socket hung up on every poll, and the
	 * daemon would either spin on it or drop it at once.  A watcher keeps
	 * both directions open for as long as it watches, and its closing the
	 * connection is how the daemon learns that it has gone.
	 */
	if (request->header.opcode != NETWORKD_OP_SUBSCRIBE &&
	    read_request_end(descriptor) != 0)
		return -1;
	if (request->header.opcode == NETWORKD_OP_SUBSCRIBE)
		return 0;

	/*
	 * A wired-management request carries one record per interface, so it
	 * does not fit the decoder for operations that name a single one.
	 * It is read where it is acted on.
	 */
	if (request->header.opcode == NETWORKD_OP_LAN_ENABLE ||
	    request->header.opcode == NETWORKD_OP_LAN_DISABLE)
		return 0;
	if (decode_request(request) != 0)
		return -1;

	/* Reports successful completion. */
	return 0;
}

/* Requires one request frame followed by an orderly write-side close. */
static int
read_request_end(
	int descriptor)
{
	unsigned char byte;
	ssize_t count;

	/* Retries an interrupted EOF probe without accepting trailing data. */
	do {
		count = read(descriptor, &byte, sizeof(byte));
	} while (count < 0 && errno == EINTR);
	if (count == 0)
		return 0;
	if (count > 0)
		errno = EINVAL;

	/* Reports missing EOF or an additional frame as a protocol error. */
	return -1;
}

/* Decodes the exact fields admitted by one wired operation. */
static int
decode_request(
	struct networkd_request *request)
{
	struct networkd_field_reader reader;
	struct networkd_field field;
	unsigned seen;
	uint32_t timeout;
	int result;

	/* Reads every field while rejecting duplicates and unknown data. */
	seen = 0U;
	networkd_field_reader_init(&reader, request->payload,
	    request->header.payload_length);
	while ((result = networkd_field_read(&reader, &field)) == 0) {
		if (field.type == NETWORKD_FIELD_INTERFACE && (seen & 1U) == 0U &&
		    copy_request_text(request->interface,
		    sizeof(request->interface), &field) == 0) {
			seen |= 1U;
		} else if (field.type == NETWORKD_FIELD_TIMEOUT &&
		    (seen & 2U) == 0U &&
		    networkd_field_read_u32(&field, &timeout) == 0 &&
		    timeout >= 1U && timeout <= 3600U) {
			request->timeout = (unsigned)timeout;
			seen |= 2U;
		} else if (field.type == NETWORKD_FIELD_ADDRESS &&
		    (seen & 4U) == 0U &&
		    copy_request_text(request->address,
		    sizeof(request->address), &field) == 0) {
			seen |= 4U;
		} else if (field.type == NETWORKD_FIELD_NETMASK &&
		    (seen & 8U) == 0U &&
		    copy_request_text(request->netmask,
		    sizeof(request->netmask), &field) == 0) {
			seen |= 8U;
		} else if (field.type == NETWORKD_FIELD_GATEWAY &&
		    (seen & 16U) == 0U &&
		    copy_request_text(request->gateway,
		    sizeof(request->gateway), &field) == 0) {
			seen |= 16U;
		} else if (field.type == NETWORKD_FIELD_DNS &&
		    request->dns_count < sizeof(request->dns) /
		    sizeof(request->dns[0]) &&
		    copy_request_text(request->dns[request->dns_count],
		    sizeof(request->dns[request->dns_count]), &field) == 0) {
			request->dns_count++;
		} else if (field.type == NETWORKD_FIELD_PATH &&
		    (seen & 64U) == 0U && copy_request_text(
		    request->rollback_path, sizeof(request->rollback_path),
		    &field) == 0) {
			seen |= 64U;
		} else if (field.type == NETWORKD_FIELD_TOKEN &&
		    (seen & 128U) == 0U &&
		    networkd_field_read_u32(&field, &request->token) == 0 &&
		    request->token != 0U) {
			seen |= 128U;
		} else if (field.type == NETWORKD_FIELD_ACCOUNT &&
		    (seen & 256U) == 0U &&
		    networkd_field_read_u32(&field, &request->account) == 0) {
			seen |= 256U;
		} else if (field.type == NETWORKD_FIELD_SSID &&
		    (seen & 32U) == 0U && field.length != 0U &&
		    field.length <= sizeof(request->ssid) &&
		    memchr(field.value, '\0', field.length) == NULL) {
			memcpy(request->ssid, field.value, field.length);
			request->ssid_length = field.length;
			seen |= 32U;
		} else {
			errno = EINVAL;
			return -1;
		}
	}
	if (result < 0)
		return -1;

	/* Requires the exact field set belonging to the opcode. */
	if ((request->header.opcode == NETWORKD_OP_SHOW &&
	    (seen == 0U || seen == 1U) && request->dns_count == 0U) ||
	    ((request->header.opcode == NETWORKD_OP_UP ||
	    request->header.opcode == NETWORKD_OP_DOWN) &&
	    (seen == 1U || seen == (1U | 128U)) &&
	    request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_DHCP &&
	    (seen == 3U || seen == (3U | 128U)) &&
	    request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_STATIC &&
	    (seen == 13U || seen == (13U | 128U)) &&
	    request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_DEFAULT_ROUTE &&
	    (seen == 16U || seen == (16U | 128U)) &&
	    request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_DNS &&
	    (seen == 0U || seen == 128U) &&
	    request->dns_count != 0U) ||
	    (request->header.opcode == NETWORKD_OP_RELOAD && seen == 0U &&
	    request->dns_count == 0U) ||
	    ((request->header.opcode == NETWORKD_OP_DEFAULT_ROUTE_CLEAR ||
	    request->header.opcode == NETWORKD_OP_DNS_CLEAR ||
	    request->header.opcode == NETWORKD_OP_ROUTE6_CLEAR) &&
	    (seen == 0U || seen == 128U) && request->dns_count == 0U) ||
	    ((request->header.opcode == NETWORKD_OP_IPV6 ||
	    request->header.opcode == NETWORKD_OP_STATIC6 ||
	    request->header.opcode == NETWORKD_OP_STATIC6_REMOVE) &&
	    (seen == 5U || seen == (5U | 128U)) && request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_ROUTE6_REMOVE &&
	    (seen == 4U || seen == (4U | 128U)) && request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_ROUTE6 &&
	    (seen == 20U || seen == 21U || seen == (20U | 128U) || seen == (21U | 128U)) &&
	    request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_CONFIRMED_ROLLBACK &&
	    seen == 0U && request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_CONFIRMED_CHECK &&
	    (seen == 0U || seen == 128U) && request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_CONFIRMED_ARM &&
	    seen == (2U | 64U) && request->dns_count == 0U &&
	    request->timeout <= NETWORKD_CONFIRMED_MINUTES_MAX) ||
	    (request->header.opcode == NETWORKD_OP_CONFIRMED_DISARM &&
	    seen == 128U && request->dns_count == 0U) ||
	    ((request->header.opcode == NETWORKD_OP_WIFI_ENABLE ||
	    request->header.opcode == NETWORKD_OP_WIFI_DISABLE ||
	    request->header.opcode == NETWORKD_OP_WIFI_LIST ||
	    request->header.opcode == NETWORKD_OP_WIFI_DISCONNECT ||
	    request->header.opcode == NETWORKD_OP_WIFI_PROFILES_CHANGED ||
	    request->header.opcode == NETWORKD_OP_WIFI_SCAN_START ||
	    request->header.opcode == NETWORKD_OP_WIFI_SCAN_STOP) &&
	    seen == 0U && request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_WIFI_CONNECT &&
	    seen == 32U && request->dns_count == 0U) ||
	    ((request->header.opcode == NETWORKD_OP_WIFI_SESSION_OPEN ||
	    request->header.opcode == NETWORKD_OP_WIFI_SESSION_CLOSE) &&
	    seen == 256U && request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_LAN_CONFIGURE &&
	    (seen == 1U || seen == 13U || seen == 29U) &&
	    request->dns_count <= NETWORKD_LAN_CONFIGURE_DNS_MAX))
		return 0;
	errno = EINVAL;
	return -1;
}

/* Copies one nonempty field into a NUL-terminated command operand. */
static int
copy_request_text(
	char *output,
	size_t capacity,
	const struct networkd_field *field)
{
	/* Rejects empty, oversized, and embedded-NUL command operands. */
	if (output == NULL || field == NULL || field->length == 0U ||
	    field->length >= capacity ||
	    memchr(field->value, '\0', field->length) != NULL) {
		errno = EINVAL;
		return -1;
	}
	memcpy(output, field->value, field->length);
	output[field->length] = '\0';
	return 0;
}

/* Returns the authorization name for one stable opcode. */
static const char *
operation_name(
	uint32_t opcode)
{
	static const char *const names[] = {
		NULL, "SHOW", "UP", "DOWN", "DHCP", "STATIC",
		"DEFAULTROUTE", "DNS", "RELOAD"
	};

	/* Maps the contiguous wired range. */
	if (opcode < sizeof(names) / sizeof(names[0]))
		return names[opcode];
	if (opcode == NETWORKD_OP_DEFAULT_ROUTE_CLEAR)
		return "DEFAULTROUTE_CLEAR";
	if (opcode == NETWORKD_OP_DNS_CLEAR)
		return "DNS_CLEAR";
	if (opcode == NETWORKD_OP_IPV6)
		return "IPV6";
	if (opcode == NETWORKD_OP_STATIC6)
		return "STATIC6";
	if (opcode == NETWORKD_OP_ROUTE6)
		return "ROUTE6";
	if (opcode == NETWORKD_OP_ROUTE6_CLEAR)
		return "ROUTE6_CLEAR";
	if (opcode == NETWORKD_OP_STATIC6_REMOVE)
		return "STATIC6_REMOVE";
	if (opcode == NETWORKD_OP_ROUTE6_REMOVE)
		return "ROUTE6_REMOVE";
	if (opcode == NETWORKD_OP_LAN_ENABLE)
		return "LAN_ENABLE";
	if (opcode == NETWORKD_OP_LAN_DISABLE)
		return "LAN_DISABLE";
	if (opcode == NETWORKD_OP_LAN_CONFIGURE)
		return "LAN_CONFIGURE";
	if (opcode == NETWORKD_OP_CONFIRMED_ARM)
		return "CONFIRMED_ARM";
	if (opcode == NETWORKD_OP_CONFIRMED_DISARM)
		return "CONFIRMED_DISARM";
	if (opcode == NETWORKD_OP_CONFIRMED_ROLLBACK)
		return "CONFIRMED_ROLLBACK";
	if (opcode == NETWORKD_OP_CONFIRMED_CHECK)
		return "CONFIRMED_CHECK";

	/* The machine's sleep (ws052-p010): root only, so not in the members' list. */
	if (opcode == NETWORKD_OP_SLEEP_PREPARE)
		return "SLEEP_PREPARE";
	if (opcode == NETWORKD_OP_SLEEP_END)
		return "SLEEP_END";

	/* Maps the separately allocated WLAN range. */
	if (opcode == NETWORKD_OP_WIFI_ENABLE)
		return "WIFI_ENABLE";
	if (opcode == NETWORKD_OP_WIFI_DISABLE)
		return "WIFI_DISABLE";
	if (opcode == NETWORKD_OP_WIFI_LIST)
		return "WIFI_LIST";
	if (opcode == NETWORKD_OP_WIFI_CONNECT)
		return "WIFI_CONNECT";
	if (opcode == NETWORKD_OP_WIFI_DISCONNECT)
		return "WIFI_DISCONNECT";
	if (opcode == NETWORKD_OP_WIFI_PROFILES_CHANGED)
		return "WIFI_PROFILES_CHANGED";
	if (opcode == NETWORKD_OP_WIFI_SESSION_OPEN)
		return "WIFI_SESSION_OPEN";
	if (opcode == NETWORKD_OP_WIFI_SESSION_CLOSE)
		return "WIFI_SESSION_CLOSE";
	if (opcode == NETWORKD_OP_WIFI_SCAN_START)
		return "WIFI_SCAN_START";
	if (opcode == NETWORKD_OP_WIFI_SCAN_STOP)
		return "WIFI_SCAN_STOP";
	return NULL;
}

/* Sends one bounded correlated terminal response. */
static void
send_response(
	int client,
	uint32_t request_id,
	uint32_t opcode,
	uint32_t status,
	int error,
	const char *stage,
	const void *output,
	size_t output_length)
{
	struct networkd_protocol_header header;
	struct networkd_field_writer writer;
	unsigned char payload[NETWORKD_RESPONSE_MAX];
	size_t required;
	size_t stage_length;

	/* Converts an oversized composition into a correlated terminal error. */
	stage_length = stage != NULL ? strnlen(stage, NETWORKD_DIAGNOSTIC_MAX) :
	    0U;
	required = 16U + (stage_length != 0U ? 4U + stage_length : 0U) +
	    (output_length != 0U ? 4U + output_length : 0U);
	if (required > sizeof(payload)) {
		status = NETWORKD_RESULT_ERROR;
		error = EOVERFLOW;
		stage = "response overflow";
		stage_length = sizeof("response overflow") - 1U;
		output = NULL;
		output_length = 0U;
	}

	/* Encodes the required status and error fields first. */
	networkd_field_writer_init(&writer, payload, sizeof(payload));
	if (networkd_field_write_u32(&writer, NETWORKD_FIELD_STATUS, status) !=
	    0 || networkd_field_write_u32(&writer, NETWORKD_FIELD_ERROR,
	    error > 0 ? (uint32_t)error : 0U) != 0)
		return;

	/* Appends only bounded sanitized diagnostic and output data. */
	if ((stage_length != 0U && networkd_field_write(&writer,
	    NETWORKD_FIELD_STAGE, stage, stage_length) != 0) ||
	    (output_length != 0U && networkd_field_write(&writer,
	    NETWORKD_FIELD_OUTPUT, output, output_length) != 0)) {
		networkd_protocol_clear(payload, sizeof(payload));
		return;
	}
	header.request_id = request_id != 0U ? request_id : 1U;
	header.opcode = opcode != 0U ? opcode : NETWORKD_OP_SHOW;
	header.payload_length = writer.used;
	(void)networkd_protocol_write_frame(client, &header, payload);
	networkd_protocol_clear(payload, sizeof(payload));
}

/* Sends the sole successful arm response with its opaque token. */
static void
send_token_response(
	int client,
	uint32_t request_id,
	uint32_t opcode,
	uint32_t token)
{
	struct networkd_protocol_header header;
	struct networkd_field_writer writer;
	unsigned char payload[32];

	networkd_field_writer_init(&writer, payload, sizeof(payload));
	if (networkd_field_write_u32(&writer, NETWORKD_FIELD_STATUS,
	    NETWORKD_RESULT_OK) != 0 || networkd_field_write_u32(&writer,
	    NETWORKD_FIELD_ERROR, 0U) != 0 || networkd_field_write_u32(&writer,
	    NETWORKD_FIELD_TOKEN, token) != 0)
		return;
	header.request_id = request_id;
	header.opcode = opcode;
	header.payload_length = writer.used;
	(void)networkd_protocol_write_frame(client, &header, payload);
	networkd_protocol_clear(payload, sizeof(payload));
}

/* Sends an uncorrelated protocol or authentication error. */
static void
send_error(
	int client,
	int error,
	const char *reason)
{
	/* Uses the reserved correlation pair when no valid header exists. */
	send_response(client, 1U, NETWORKD_OP_SHOW, NETWORKD_RESULT_ERROR,
	    error != 0 ? error : EIO, reason, NULL, 0U);
}

/* Supports the show interfaces operation. */
static int
show_interfaces(
	const char *name,
	char *output,
	size_t capacity)
{
	struct ifreq *items;
	unsigned count, index;
	size_t used;
	int descriptor, result;

	items = NULL;
	count = 0;
	used = 0;
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	result = 0;

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	/* Handles the name availability. */
	if (name != NULL) {
		result = append_interface_status(descriptor, name, output,
						 capacity, &used);
	} else if (netutil_interfaces(descriptor, &items, &count) != 0)
		result = -1;
	else

		/* Process each remaining element. */
		for (index = 0; index < count; index++) {
			/* Handles a failed append interface status operation. */
			if (append_interface_status(
				descriptor, items[index].ifr_name, output,
				capacity, &used) != 0) {
				result = -1;
				break;
			}
		}
	free(items);
	close(descriptor);

	/* Returns the computed result. */
	return result;
}

/* Supports the append interface status operation. */
static int
append_interface_status(
	int descriptor,
	const char *name,
	char *output,
	const size_t capacity,
	size_t *used)
{
	struct ifreq flags, address;
	int count, has_address;

	/* Handles a failed netutil ifreq operation. */
	if (netutil_ifreq(&flags, name) != 0 ||
	    ioctl(descriptor, SIOCGIFFLAGS, &flags) != 0)

		/* Reports operation failure. */
		return -1;
	has_address =
	    netutil_ifreq(&address, name) == 0 &&
	    ioctl(descriptor, SIOCGIFADDR, &address) == 0 &&
	    ((struct sockaddr_in *)&address.ifr_addr)->sin_addr.s_addr != 0;
	count =
	    snprintf(output + *used, capacity - *used, "%s %s %s\n", name,
		     has_address ? "static" : "unconfigured",
		     (flags.ifr_flags & IFF_RUNNING) != 0 ? "online" : "offline");

	/* Checks the remaining item count. */
	if (count < 0 || (size_t)count >= capacity - *used) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	*used += (size_t)count;
	/* Reports successful completion. */
	return 0;
}

/* Supports the interface exists operation. */
static int
interface_exists(
	const char *name)
{
	uint32_t index;
	int descriptor;
	int result;

	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	result = descriptor >= 0
		? netutil_ifindex(descriptor, name, &index)
		: -1;

	/* Checks the file descriptor. */
	if (descriptor >= 0)
		close(descriptor);

	/* Returns the computed result. */
	return result;
}

/* Resolves one interface name to its stable current index. */
static int
interface_index(
	const char *name,
	uint32_t *index)
{
	int descriptor;
	int result;
	int saved;

	/* Uses the canonical generic network-interface ioctl. */
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return -1;
	result = netutil_ifindex(descriptor, name, index);
	saved = errno;
	(void)close(descriptor);
	errno = saved;
	return result;
}

/* Reads the current canonical interface flags. */
static int
interface_flags(
	const char *name,
	int *flags)
{
	struct ifreq request;
	int descriptor;
	int result;
	int saved;

	/* Issues one bounded name-based flags query. */
	if (flags == NULL || netutil_ifreq(&request, name) != 0)
		return -1;
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return -1;
	result = ioctl(descriptor, SIOCGIFFLAGS, &request);
	saved = errno;
	(void)close(descriptor);
	if (result == 0)
		*flags = request.ifr_flags;
	errno = saved;
	return result;
}

/* Verifies that an index still resolves to the managed interface name. */
static int
interface_index_name_matches(
	uint32_t index,
	const char *name)
{
	struct ifreq request;
	int descriptor;
	int result;
	int saved;

	/* Resolves the index independently of the possibly reused name. */
	memset(&request, 0, sizeof(request));
	request.ifr_ifindex = (int)index;
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return -1;
	result = ioctl(descriptor, SIOCGIFNAME, &request);
	saved = errno;
	(void)close(descriptor);
	if (result == 0 && strcmp(request.ifr_name, name) != 0) {
		errno = ENODEV;
		return -1;
	}
	errno = saved;
	return result;
}

/* Confirms that the WLAN controlled port is currently usable. */
static int
wlan_connected(
	const char *interface)
{
	struct wlan_status_request status;
	size_t length;
	int descriptor;
	int result;

	/* Queries one current public WLAN status snapshot. */
	length = strlen(interface);
	if (length == 0U || length >= IFNAMSIZ)
		return 0;
	memset(&status, 0, sizeof(status));
	memcpy(status.ifr_name, interface, length);
	status.version = WLAN_ABI_VERSION;
	status.size = sizeof(status);
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return 0;
	result = ioctl(descriptor, SIOCGWLANSTATUS, &status) == 0 &&
	    status.state == WLAN_STATE_CONNECTED &&
	    status.controlled_port != 0U;
	(void)close(descriptor);
	networkd_protocol_clear(&status, sizeof(status));
	return result;
}

/* A reusable connection needs its live L2 port and a completed usable L3 state. */
static int
managed_connection_usable(
	void)
{
	struct networkd_managed_l3 current;
	int flags;
	int usable;

	if (managed_wlan.connection.interface[0] == '\0' ||
	    !managed_wlan.connection.owns_l3 || managed_wlan.connection.l3_pending)
		return 0;
	memset(&current, 0, sizeof(current));
	usable = interface_index_name_matches(managed_wlan.connection.ifindex,
	    managed_wlan.connection.interface) == 0 &&
	    interface_flags(managed_wlan.connection.interface, &flags) == 0 &&
	    (flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING) &&
	    wlan_connected(managed_wlan.connection.interface) &&
	    snapshot_managed_l3(&managed_wlan, &current) == 0 && current.address != 0U;
	networkd_protocol_clear(&current, sizeof(current));
	return usable;
}

/* Runs one private machine-mode wifi primitive. */
static int
run_wifi(
	const char *interface,
	const char *operation,
	const struct wifi_conf_profile *profile,
	unsigned timeout,
	struct networkd_wifi_child_result *result)
{
	const void *ssid;
	const void *passphrase;
	size_t ssid_length;
	size_t passphrase_length;
	uint64_t remaining;
	uint64_t limit;
	uint64_t now;
	int function_result;

	if (!wifi_work.cleanup && wifi_wait_pump != NULL &&
	    (function_result = wifi_wait_pump()) != 0) {
		networkd_wifi_child_result_clear(result);
		result->terminal_error = function_result;
		errno = function_result;
		return -1;
	}

	/* Keep the last teardown interval available after selection expires. */
	if (wifi_work.deadline != 0U) {
		limit = wifi_work.deadline;
		if (strcmp(operation, "disconnect") != 0 &&
		    strcmp(operation, "search-stop") != 0 &&
		    strcmp(operation, "down") != 0)
			limit -= NETWORKD_WIFI_CLEANUP_SECONDS * 1000000ULL;
		now = netutil_monotonic_us();
		remaining = limit > now ? (limit - now) / 1000000ULL : 0U;
		/* Include the runner's one-second termination grace in this bound. */
		if (remaining <= 1U) {
			networkd_wifi_child_result_clear(result);
			result->terminal_error = ETIMEDOUT;
			errno = ETIMEDOUT;
			return -1;
		}
		if (timeout >= remaining)
			timeout = (unsigned)remaining - 1U;
	}

	/* Supplies counted credential views only to connect. */
	ssid = profile != NULL ? profile->ssid : NULL;
	ssid_length = profile != NULL ? profile->ssid_length : 0U;
	passphrase = profile != NULL ? profile->passphrase : NULL;
	passphrase_length = profile != NULL ? profile->passphrase_length : 0U;
	function_result = networkd_wifi_child_run(interface, operation, ssid,
	    ssid_length, passphrase, passphrase_length, timeout, result);
	if (function_result == 0 && strcmp(operation, "list") == 0)
		remember_wifi_observation(interface, result);
	if (function_result != 0)
		errno = result->terminal_error != 0 ?
		    result->terminal_error : EIO;
	return function_result;
}

/* Converts complete private records directly into public source-prefixed lines. */
static int
append_wifi_records(
	const char *interface,
	const char *records,
	size_t length,
	char *output,
	size_t capacity,
	size_t *used)
{
	static const char private_prefix[] = "WIFI1 ";
	static const char terminal[] = "terminal ";
	char prefix[IFNAMSIZ + 16U];
	size_t start;
	size_t end;
	size_t prefix_length;
	int count;

	/* Rejects invalid spans before computing remaining capacity. */
	if (records == NULL || output == NULL || used == NULL || *used > capacity) {
		errno = EINVAL;
		return -1;
	}
	count = snprintf(prefix, sizeof(prefix), "interface=%s ", interface);
	if (count < 0 || (size_t)count >= sizeof(prefix)) {
		errno = EOVERFLOW;
		return -1;
	}
	prefix_length = (size_t)count;

	/* Copies one complete record at a time without a second 32 KB stack buffer. */
	start = 0U;
	while (start < length) {
		end = start;
		while (end < length && records[end] != '\n')
			end++;

		/* Requires the producer's version marker and a complete line. */
		if (end == length || end - start < sizeof(private_prefix) - 1U ||
		    memcmp(records + start, private_prefix, sizeof(private_prefix) - 1U) != 0) {
			errno = EILSEQ;
			return -1;
		}
		start += sizeof(private_prefix) - 1U;

		/* Omits only the private terminal record from public output. */
		if (end - start >= sizeof(terminal) - 1U &&
		    memcmp(records + start, terminal, sizeof(terminal) - 1U) == 0)
			return 0;

		/* Leaves the output cursor at the last complete line on overflow. */
		if (prefix_length + end - start + 1U > capacity - *used) {
			errno = EOVERFLOW;
			return -1;
		}
		memcpy(output + *used, prefix, prefix_length);
		*used += prefix_length;
		memcpy(output + *used, records + start, end - start);
		*used += end - start;
		output[(*used)++] = '\n';
		start = end + 1U;
	}

	/* Reports complete conversion of the available records. */
	return 0;
}

/* Appends one primitive result while identifying its source radio. */
static int
append_wifi_output(
	const char *interface,
	const struct networkd_wifi_child_result *result,
	char *output,
	size_t capacity,
	size_t *output_length)
{
	int status;

	/* Shares conversion between fresh primitive results and cached records. */
	if (result == NULL || result->output_length > sizeof(result->output)) {
		errno = EINVAL;
		return -1;
	}
	status = append_wifi_records(interface, (const char *)result->output, result->output_length,
	    output, capacity, output_length);
	return status;
}

/* Enumerates WLAN interfaces in the kernel's stable interface order. */
static int
enumerate_wlan_radios(
	struct networkd_wlan_radio *radios,
	size_t capacity,
	size_t *radio_count)
{
	struct wlan_status_request status;
	struct ifreq *interfaces;
	unsigned interface_count;
	unsigned index;
	size_t count;
	size_t known;
	uint32_t ifindex;
	int status_error;
	int descriptor;
	int saved;

	/* Obtains the canonical interface list through the generic socket API. */
	if (radios == NULL || radio_count == NULL || capacity == 0U ||
	    capacity > NETWORKD_WLAN_RADIO_MAX) {
		errno = EINVAL;
		return -1;
	}
	interfaces = NULL;
	interface_count = 0U;
	count = 0U;
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return -1;
	if (netutil_interfaces(descriptor, &interfaces, &interface_count) != 0) {
		saved = errno;
		(void)close(descriptor);
		errno = saved;
		return -1;
	}

	/* A successful WLAN status ioctl classifies one interface as a radio. */
	for (index = 0U; index < interface_count; index++) {
		if (netutil_ifindex(descriptor, interfaces[index].ifr_name, &ifindex) != 0) {
			if (errno == ENODEV || errno == ENXIO)
				continue;
			saved = errno;
			free(interfaces);
			(void)close(descriptor);
			errno = saved;
			return -1;
		}
		memset(&status, 0, sizeof(status));
		memcpy(status.ifr_name, interfaces[index].ifr_name,
		    sizeof(status.ifr_name));
		status.version = WLAN_ABI_VERSION;
		status.size = sizeof(status);
		status_error = ioctl(descriptor, SIOCGWLANSTATUS, &status) == 0 ? 0 : errno;
		for (known = 0U; known < known_wlan_radio_count; known++) {
			if (known_wlan_radios[known].ifindex == ifindex &&
			    strcmp(known_wlan_radios[known].interface, status.ifr_name) == 0)
				break;
		}
		if (status_error != 0 && known == known_wlan_radio_count) {
			if (status_error == EOPNOTSUPP || status_error == ENOTTY)
				continue;
			/* Unknown observation failure cannot prove this is not a radio. */
			free(interfaces);
			(void)close(descriptor);
			errno = status_error;
			return -1;
		}
		if (count == capacity) {
			free(interfaces);
			(void)close(descriptor);
			errno = E2BIG;
			return -1;
		}
		memcpy(radios[count].interface, interfaces[index].ifr_name,
		    sizeof(radios[count].interface));
		radios[count].ifindex = ifindex;
		radios[count].observation_error = status_error;
		radios[count].stop_flags = status.stop_flags;
		radios[count].administrative_up = status.administrative_up != 0U;
		radios[count].association_active =
		    status.state >= WLAN_STATE_AUTHENTICATING &&
		    status.state <= WLAN_STATE_DISCONNECTING;
		radios[count].scan_state = status.scan_state;
		radios[count].snapshot_generation = status.snapshot_generation;
		radios[count].consumed_snapshot_generation = 0U;
		if (known < known_wlan_radio_count)
			radios[count].consumed_snapshot_generation =
			    known_wlan_radios[known].consumed_snapshot_generation;
		count++;
	}
	free(interfaces);
	saved = errno;
	if (close(descriptor) != 0)
		return -1;
	errno = saved;
	*radio_count = count;
	memcpy(known_wlan_radios, radios, count * sizeof(*radios));
	known_wlan_radio_count = count;

	/* Reports a possibly empty, stable-order radio list. */
	return 0;
}


/* Runs one nonsecret preparation primitive with uniform output and cleanup. */
static int
run_wifi_append(
	const char *interface,
	const char *operation,
	char *output,
	size_t capacity,
	size_t *output_length)
{
	struct networkd_wifi_child_result child;
	int result;
	int saved;

	memset(&child, 0, sizeof(child));
	result = run_wifi(interface, operation, NULL, 10U, &child);
	if (result == 0 && output != NULL && output_length != NULL)
		result = append_wifi_output(interface, &child, output, capacity,
		    output_length);
	saved = errno;
	networkd_wifi_child_result_clear(&child);
	errno = saved;
	return result;
}

/* Reuses administrative and scan state already observed during enumeration. */
static int
prepare_wlan_radio(
	struct networkd_wlan_radio *radio,
	char *output,
	size_t capacity,
	size_t *output_length)
{
	int reuse_snapshot;

	radio->ready = 0;
	if (radio->observation_error != 0 || radio->stop_flags != 0U) {
		errno = radio->observation_error != 0 ? radio->observation_error : EBUSY;
		return -1;
	}
	/* Existing direct associations cannot become a second managed L2 link. */
	if (radio->association_active) {
		if (run_wifi_append(radio->interface, "disconnect", output,
		    capacity, output_length) != 0)
			return -1;
		radio->association_active = 0;
		radio->scan_state = WLAN_SCAN_IDLE;
	}
	if (!radio->administrative_up) {
		if (run_wifi_append(radio->interface, "up", output,
		    capacity, output_length) != 0)
			return -1;
		radio->administrative_up = 1;
		radio->scan_state = WLAN_SCAN_IDLE;
	}
	/* A scan can finish after the previous selection deadline, during idle. */
	reuse_snapshot = radio->scan_state == WLAN_SCAN_COMPLETE &&
	    radio->snapshot_generation != 0U &&
	    radio->snapshot_generation != radio->consumed_snapshot_generation;
	if (radio->scan_state != WLAN_SCAN_RUNNING && !reuse_snapshot) {
		if (run_wifi_append(radio->interface, "search-start", output,
		    capacity, output_length) != 0)
			return -1;
		radio->scan_state = WLAN_SCAN_RUNNING;
	}
	radio->ready = 1;
	return 0;
}

/* Remembers consumption only for the exact interface observed by selection. */
static void
consume_wlan_snapshot(
	const struct networkd_wlan_radio *radio,
	uint64_t generation)
{
	size_t index;

	if (generation == 0U)
		return;
	for (index = 0U; index < known_wlan_radio_count; index++) {
		if (known_wlan_radios[index].ifindex != radio->ifindex)
			continue;
		if (strcmp(known_wlan_radios[index].interface, radio->interface) != 0)
			continue;
		known_wlan_radios[index].consumed_snapshot_generation = generation;
		return;
	}
}

/* Isolates each radio failure while requiring at least one usable radio. */
static int
prepare_wlan_radios(
	struct networkd_wlan_radio *radios,
	size_t radio_count,
	char *output,
	size_t capacity,
	size_t *output_length)
{
	size_t index;
	size_t prepared;
	int first_error;

	prepared = 0U;
	first_error = 0;
	for (index = 0U; index < radio_count; index++) {
		if (prepare_wlan_radio(&radios[index], output, capacity,
		    output_length) == 0)
			prepared++;
		else if (first_error == 0)
			first_error = errno != 0 ? errno : EIO;
	}
	if (radio_count != 0U && prepared == 0U) {
		errno = first_error != 0 ? first_error : EIO;
		return -1;
	}
	if (first_error != 0)
		fprintf(stderr, "networkd: some WLAN radios could not prepare: %s\n",
		    strerror(first_error));
	return 0;
}

/* Reads an exact retained radio identity; a transient status error is not absence. */
static int
wlan_radio_status(const struct networkd_wlan_radio *radio,
	struct wlan_status_request *status)
{
	int descriptor;
	int error;
	int saved;

	if (interface_index_name_matches(radio->ifindex, radio->interface) != 0)
		return -1;
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return -1;
	memset(status, 0, sizeof(*status));
	memcpy(status->ifr_name, radio->interface, sizeof(status->ifr_name));
	status->version = WLAN_ABI_VERSION;
	status->size = sizeof(*status);
	error = ioctl(descriptor, SIOCGWLANSTATUS, status);
	saved = errno;
	(void)close(descriptor);
	if (error == 0)
		return interface_index_name_matches(radio->ifindex, radio->interface);
	errno = saved;
	return -1;
}

/* Stops all radios, retaining failures until checked physical stop is observed. */
static int
stop_wlan_radios(
	const struct networkd_wlan_radio *radios,
	size_t radio_count,
	int lower)
{
	struct networkd_wifi_child_result result;
	struct wlan_status_request status;
	size_t index;
	int first_error;
	int error;

	memset(&result, 0, sizeof(result));
	first_error = 0;
	for (index = 0U; index < radio_count; index++) {
		/* An old identity disappearing permits retirement, never mutation of
		 * the replacement. Only the identity query can establish absence. */
		if (interface_index_name_matches(radios[index].ifindex,
		    radios[index].interface) != 0) {
			if (errno != ENODEV && errno != ENXIO && first_error == 0)
				first_error = errno;
			continue;
		}
		error = 0;
		if (wlan_radio_status(&radios[index], &status) != 0)
			error = errno;
		else if ((status.stop_flags & WLAN_STATUS_STOP_PENDING) != 0U)
			error = EBUSY;
		if (error == 0) {
			if (run_wifi(radios[index].interface, "disconnect", NULL, 10U,
			    &result) != 0)
				error = errno;
			networkd_wifi_child_result_clear(&result);
			if (run_wifi(radios[index].interface, "search-stop", NULL, 10U,
			    &result) != 0 && error == 0)
				error = errno;
			networkd_wifi_child_result_clear(&result);
			if (lower) {
				/* A checked full stop supersedes earlier inverse errors. */
				if (run_wifi(radios[index].interface, "down", NULL, 10U,
				    &result) != 0)
					error = errno;
				networkd_wifi_child_result_clear(&result);
				if (wlan_radio_status(&radios[index], &status) != 0)
					error = errno;
				else if (status.stop_flags != 0U || status.administrative_up ||
				    status.associated || status.key_installed ||
				    status.controlled_port || status.scan_state == WLAN_SCAN_RUNNING)
					error = EBUSY;
				else
					error = 0;
			}
		}
		if (error != 0 && first_error == 0)
			first_error = error;
	}
	if (first_error != 0) {
		errno = first_error;
		return -1;
	}
	return 0;
}

/* Stops scans on every prepared radio except the sole connected winner. */
static void
stop_losing_scans(
	const struct networkd_wlan_radio *radios,
	size_t radio_count,
	size_t winner)
{
	struct networkd_wifi_child_result result;
	size_t index;
	int saved;

	/* A committed connection remains usable if a losing scan cannot stop. */
	memset(&result, 0, sizeof(result));
	for (index = 0U; index < radio_count; index++) {
		if (index == winner || !radios[index].ready)
			continue;
		if (run_wifi(radios[index].interface, "search-stop", NULL, 10U,
		    &result) != 0) {
			saved = errno != 0 ? errno : EIO;
			fprintf(stderr, "networkd: %s: stop losing scan: %s\n",
			    radios[index].interface, strerror(saved));
		}
		networkd_wifi_child_result_clear(&result);
	}
}

/* Finds an exact saved profile without exposing its passphrase elsewhere. */
static const struct wifi_conf_profile *
find_profile(
	const struct wifi_conf_model *model,
	const void *ssid,
	size_t ssid_length)
{
	size_t index;

	/* Uses the file's stable record order for exact counted SSID matching. */
	if (model == NULL || ssid == NULL || ssid_length == 0U)
		return NULL;
	for (index = 0U; index < model->profile_count; index++) {
		if (model->profiles[index].ssid_length == ssid_length &&
		    memcmp(model->profiles[index].ssid, ssid, ssid_length) == 0)
			return &model->profiles[index];
	}

	/* Reports an absent profile without manufacturing a credential. */
	return NULL;
}

/* Loads one authenticated policy owner's fixed credential store. */
static int
load_policy(
	uid_t owner_uid,
	struct wifi_conf_model *model,
	char *diagnostic,
	size_t diagnostic_capacity)
{
	/* The store layer resolves passwd homes and enforces ownership itself. */
	return wifi_store_load_for_user(owner_uid, model, diagnostic,
	    diagnostic_capacity);
}

/*
 * Tests whether one peer may control the Wi-Fi policy, whoever owns it.
 *
 * The user decided on 2026-10-02 (ws005-p019): a member of the network group
 * may control Wi-Fi.  Turning it on or off, joining and disconnecting are
 * therefore not reserved to the account that enabled the policy; root and
 * every network member may do them.  A join moves the policy to the joining
 * account, whose own store supplies the key, so a key still never passes
 * through this daemon.  Any other account is refused.
 */
static int
owner_allowed(
	const struct kern_peercred *peer)
{
	int member;

	/* Refuses a request whose sender is unknown. */
	if (peer == NULL)
		return 0;

	/* Root may control the policy. */
	if (peer->euid == 0)
		return 1;

	/* Admits a member of the network group and nobody else. */
	member = peer_in_network_group(peer);
	if (!member)
		return 0;

	/* Succeeded: the peer is a network operator. */
	return 1;
}

/*
 * Tests whether one peer belongs to the network group.
 *
 * The listener's mode already keeps other accounts out; this check states the
 * same rule where the policy is decided.  The peer counts as a member when it
 * runs with the group as its effective group, or when the group database
 * lists its account in the group.
 */
static int
peer_in_network_group(
	const struct kern_peercred *peer)
{
	char buffer[NETWORKD_GROUP_BUFFER_MAX];
	gid_t groups[NETWORKD_PEER_GROUP_MAX];
	struct passwd storage;
	struct passwd *account;
	int group_count;
	int error;
	int index;

	/* A process running with the network group is a member. */
	if ((gid_t)peer->egid == NETWORKD_GROUP_GID)
		return 1;

	/* Finds the account behind the peer's effective user. */
	account = NULL;
	error = getpwuid_r((uid_t)peer->euid, &storage, buffer, sizeof(buffer),
	    &account);
	if (error != 0)
		return 0;

	/* An effective user with no account is not a member. */
	if (account == NULL || account->pw_name == NULL)
		return 0;

	/*
	 * Collects the groups the database gives the account.  A list longer
	 * than the buffer still stores its first entries, which are searched.
	 */
	memset(groups, 0, sizeof(groups));
	group_count = NETWORKD_PEER_GROUP_MAX;
	(void)getgrouplist(account->pw_name, account->pw_gid, groups,
	    &group_count);
	if (group_count > NETWORKD_PEER_GROUP_MAX)
		group_count = NETWORKD_PEER_GROUP_MAX;

	/* Looks for the network group among them. */
	for (index = 0; index < group_count; index++) {
		if (groups[index] == NETWORKD_GROUP_GID)
			return 1;
	}

	/* The account is not a member of the network group. */
	return 0;
}

/*
 * Loads the profiles automatic joining may use, each with its store.
 *
 * The user decided on 2026-10-02 (ws005-p024) that the system's store
 * (/etc/wifi.conf) is what a boot joins from, and that an account's own
 * store is used only while that account is logged in: sessiond announces
 * the login and the logout.  The stores are read in this order, and an SSID
 * an earlier store gave is not taken again:
 *
 *   the open sessions' stores, the newest session first (a logged-in
 *   person's own key is preferred to the system's);
 *   the store of the account that owns the policy (one that took it with
 *   an explicit join);
 *   the system's store.
 *
 * A store that is missing or cannot be read adds nothing; the others are
 * still used.  The keys are only held for this attempt: the caller clears
 * the model afterwards.
 */
static void
load_candidates(
	struct wifi_conf_model *model,
	uid_t *stores)
{
	size_t index;

	/* Starts from no candidate. */
	wifi_conf_model_init(model);

	/* The open sessions' stores, the newest first. */
	for (index = wifi_session_count; index > 0U; index--)
		candidate_store_add(model, stores, wifi_session_uids[index - 1U]);

	/* The store of the account that owns the policy. */
	candidate_store_add(model, stores, managed_wlan.owner_uid);

	/* The system's store. */
	candidate_store_add(model, stores, 0);
}

/* Adds the profiles of one account's store whose SSIDs are not candidates yet. */
static void
candidate_store_add(
	struct wifi_conf_model *model,
	uid_t *stores,
	uid_t account)
{
	struct wifi_conf_model store;
	char diagnostic[WIFI_CONF_DIAGNOSTIC_MAX];
	const struct wifi_conf_profile *profile;
	const struct wifi_conf_profile *present;
	size_t index;
	int rejected;
	int error;
	int saved;

	/* Reads the account's store; the store layer checks its owner and mode. */
	wifi_conf_model_init(&store);
	memset(diagnostic, 0, sizeof(diagnostic));
	error = load_policy(account, &store, diagnostic, sizeof(diagnostic));
	if (error != 0) {
		/* A missing store is an empty one; any other failure is told, and the store skipped. */
		saved = errno;
		if (saved != ENOENT) {
			fprintf(stderr, "networkd: Wi-Fi store of account %u skipped: %s\n",
			    (unsigned)account,
			    diagnostic[0] != '\0' ? diagnostic : strerror(saved));
		}
		wifi_conf_model_clear(&store);
		wifi_conf_explicit_clear(diagnostic, sizeof(diagnostic));
		return;
	}

	/* Takes each profile whose SSID no earlier store gave, while the model has room. */
	for (index = 0U; index < store.profile_count; index++) {
		profile = &store.profiles[index];

		/* An SSID an earlier store gave keeps that store's key. */
		present = find_profile(model, profile->ssid, profile->ssid_length);
		if (present != NULL)
			continue;

		/* A key the network refused waits for its store to change. */
		rejected = wifi_rejection_find(account, profile->ssid, profile->ssid_length);
		if (rejected >= 0)
			continue;

		/* The model is full: later profiles are not candidates this time. */
		if (model->profile_count == WIFI_CONF_PROFILE_MAX)
			break;
		if (profile->passphrase_length >
		    WIFI_CONF_PASSPHRASE_TOTAL_MAX - model->passphrase_bytes)
			break;

		/* The profile, and the account whose store it came from. */
		model->profiles[model->profile_count] = *profile;
		stores[model->profile_count] = account;
		model->profile_count++;
		model->passphrase_bytes += profile->passphrase_length;
	}

	/* The store's own copy of the keys goes. */
	wifi_conf_model_clear(&store);
	wifi_conf_explicit_clear(diagnostic, sizeof(diagnostic));
}

/* Records the store of the profile the automatic join just used. */
static void
connection_store_note(
	const struct wifi_conf_model *model,
	const uid_t *stores)
{
	const struct wifi_conf_profile *profile;
	size_t index;

	/* The joined network is the connection's SSID. */
	for (index = 0U; index < model->profile_count; index++) {
		profile = &model->profiles[index];
		if (profile->ssid_length != managed_wlan.connection.ssid_length)
			continue;
		if (memcmp(profile->ssid, managed_wlan.connection.ssid,
		    profile->ssid_length) != 0)
			continue;

		/* Its store is the connection's from now on. */
		wifi_connection_store_uid = stores[index];
		wifi_connection_store_known = 1;
		return;
	}
}

/* Tells whether the current connection was made with a key from one account's store. */
static int
connection_from_store(
	uid_t account)
{
	/* No join since the last connection began has named a store. */
	if (!wifi_connection_store_known)
		return 0;

	/* Only a connection being made, kept or remade has a store. */
	if (managed_wlan.state != NETWORKD_WLAN_CONNECTING &&
	    managed_wlan.state != NETWORKD_WLAN_CONNECTED &&
	    managed_wlan.state != NETWORKD_WLAN_RECONNECTING)
		return 0;

	/* Another account's store gave the key. */
	if (wifi_connection_store_uid != account)
		return 0;

	/* Succeeded: the connection's key is from this account's store. */
	return 1;
}

/*
 * Decides which account a session request is about, or refuses it.
 *
 * Root -- sessiond, which opens and closes the sessions -- names the account
 * of the session.  A member of the network group may name only itself, so no
 * account can make another one's store a candidate or take it away.  The
 * account must be in the account database, where its store is found.
 * Returns zero or an errno value.
 */
static int
wifi_session_account(
	const struct networkd_request *request,
	const struct kern_peercred *peer,
	uid_t *account)
{
	char buffer[NETWORKD_GROUP_BUFFER_MAX];
	struct passwd storage;
	struct passwd *entry;
	uid_t named;
	int allowed;
	int error;

	/* A request whose sender is unknown is refused. */
	if (peer == NULL)
		return EACCES;

	/* The account the request names. */
	named = (uid_t)request->account;

	/* Anyone but root must be a network operator naming itself. */
	if (peer->euid != 0) {
		allowed = owner_allowed(peer);
		if (!allowed)
			return EPERM;
		if (named != (uid_t)peer->euid)
			return EPERM;
	}

	/* The account's entry, where its home and so its store are found. */
	entry = NULL;
	error = getpwuid_r(named, &storage, buffer, sizeof(buffer), &entry);
	if (error != 0)
		return error;
	if (entry == NULL)
		return ENOENT;

	/* Succeeded: the request is about this account. */
	*account = named;
	return 0;
}

/* Finds an account among the open sessions; returns its place, or -1. */
static int
wifi_session_find(
	uid_t account)
{
	size_t index;

	/* The list is short and kept in the order the sessions opened. */
	for (index = 0U; index < wifi_session_count; index++) {
		if (wifi_session_uids[index] == account)
			return (int)index;
	}

	/* The account has no open session. */
	return -1;
}

/* Takes an account off the open sessions, keeping the others in their order. */
static void
wifi_session_forget(
	uid_t account)
{
	size_t index;
	int slot;

	/* An account that is not listed has nothing to take off. */
	slot = wifi_session_find(account);
	if (slot < 0)
		return;

	/* The later sessions move up by one. */
	for (index = (size_t)slot; index + 1U < wifi_session_count; index++)
		wifi_session_uids[index] = wifi_session_uids[index + 1U];
	wifi_session_count--;
}

/*
 * Makes an account's store a candidate because its session opened.
 *
 * The account becomes the newest session even when it was listed already
 * (a second login), so its store is read first.  The automatic search then
 * starts over with the new candidates.
 */
static int
wifi_session_open(
	const struct networkd_request *request,
	const struct kern_peercred *peer)
{
	uid_t account;
	int error;

	/* The account the request is about, if the sender may name it. */
	error = wifi_session_account(request, peer, &account);
	if (error != 0) {
		errno = error;
		return -1;
	}

	/* The account goes to the newest place; a full list takes no new one. */
	wifi_session_forget(account);
	if (wifi_session_count == NETWORKD_WIFI_SESSION_MAX) {
		errno = ENOSPC;
		return -1;
	}
	wifi_session_uids[wifi_session_count] = account;
	wifi_session_count++;
	fprintf(stderr, "networkd: Wi-Fi store of account %u is a candidate (session open)\n",
	    (unsigned)account);

	/* The search starts over with the account's saved networks. */
	wifi_candidates_changed();

	/* Succeeded: the account's store is a candidate. */
	return 0;
}

/*
 * Drops an account's store from the candidates because its session closed.
 *
 * What was joined with the account's key does not outlive the session: when
 * the account owns the policy (it joined a network itself), the policy goes
 * back to root, which ends the connection and searches again with the
 * system's store and the other sessions' stores; when the policy is root's
 * but the current connection's key came from the account's store, that
 * connection ends and the search resumes.  Root's store is the system's and
 * stays a candidate.
 */
static int
wifi_session_close(
	struct networkd_wifi_request *work,
	const struct networkd_request *request,
	const struct kern_peercred *peer)
{
	uid_t account;
	int owner;
	int error;

	/* The account the request is about, if the sender may name it. */
	work->stage = "Wi-Fi session";
	error = wifi_session_account(request, peer, &account);
	if (error != 0) {
		errno = error;
		return -1;
	}

	/* The account's store is no longer a candidate. */
	wifi_session_forget(account);
	fprintf(stderr, "networkd: Wi-Fi store of account %u is no candidate (session closed)\n",
	    (unsigned)account);

	/* The system's store stays; closing root's session ends nothing. */
	if (account == 0) {
		wifi_candidates_changed();
		return 0;
	}

	/* A policy the account took with a join goes back to root. */
	owner = networkd_managed_wlan_owner_matches(&managed_wlan, account);
	if (owner && managed_wlan.state != NETWORKD_WLAN_DISABLED) {
		error = wifi_policy_take(work, 0);
		if (error != 0)
			return -1;
		schedule_automatic_work(0U);
		return 0;
	}

	/* A connection made with the account's key ends; the search resumes. */
	if (connection_from_store(account)) {
		work->stage = "retire the closed session's Wi-Fi connection";
		error = retire_managed_connection(NETWORKD_WLAN_AUTO_SEARCHING, 1);
		if (error != 0)
			return -1;
	}

	/* The search starts over without the account's store. */
	wifi_candidates_changed();

	/* Succeeded: the account's store is no longer used. */
	return 0;
}

/* Returns the public spelling for one managed policy state. */
static const char *
managed_state_name(
	enum networkd_managed_wlan_state state)
{
	/* Maps every closed state-machine value explicitly. */
	if (state == NETWORKD_WLAN_DISABLED)
		return "disabled";
	if (state == NETWORKD_WLAN_AUTO_SEARCHING)
		return "auto-searching";
	if (state == NETWORKD_WLAN_CONNECTING)
		return "connecting";
	if (state == NETWORKD_WLAN_CONNECTED)
		return "connected";
	if (state == NETWORKD_WLAN_MANUAL_DISCONNECTED)
		return "manual-disconnected";
	if (state == NETWORKD_WLAN_RECONNECTING)
		return "reconnecting";
	if (state == NETWORKD_WLAN_RETIRING)
		return "disconnecting";

	/* Keeps a corrupted internal value visibly distinct. */
	return "invalid";
}

/*
 * Appends one nonsecret global managed-policy status record.
 *
 * Besides the state and the radio, it names the policy owner, the store the
 * current connection's key came from ("-" when there is no connection or it
 * is not known) and the accounts whose sessions are open, so that which
 * stores automatic joining reads can be seen (ws005-p024).  Account numbers
 * are not secret; keys never appear here.
 */
static int
append_managed_status(
	char *output,
	size_t capacity,
	size_t *output_length)
{
	char sessions[NETWORKD_WIFI_SESSION_MAX * 11U + 2U];
	char rejected[NETWORKD_WIFI_REJECTION_MAX * (11U + 1U + WLAN_SSID_MAX * 2U + 1U) + 2U];
	char ssid[WLAN_SSID_MAX * 2U + 2U];
	char store[16];
	const char *interface;
	size_t used;
	size_t index;
	int count;
	int appended;

	/* The radio of the managed connection, if any. */
	interface = "-";
	if (managed_wlan.connection.interface[0] != '\0')
		interface = managed_wlan.connection.interface;

	/* The store of the connection's key, named only while that connection lasts. */
	store[0] = '-';
	store[1] = '\0';
	if (connection_from_store(wifi_connection_store_uid))
		(void)snprintf(store, sizeof(store), "%u", (unsigned)wifi_connection_store_uid);

	/* The accounts with an open session, oldest first, separated by commas. */
	sessions[0] = '-';
	sessions[1] = '\0';
	used = 0U;
	for (index = 0U; index < wifi_session_count; index++) {
		count = snprintf(sessions + used, sizeof(sessions) - used, "%s%u",
		    index == 0U ? "" : ",", (unsigned)wifi_session_uids[index]);
		if (count < 0 || (size_t)count >= sizeof(sessions) - used)
			break;
		used += (size_t)count;
	}

	/* The network of the connection, in hex, while one is being made or kept. */
	ssid[0] = '-';
	ssid[1] = '\0';
	used = 0U;
	if (managed_wlan.connection.interface[0] != '\0' && managed_wlan.connection.ssid_length != 0U)
		(void)append_hex(ssid, sizeof(ssid), &used, managed_wlan.connection.ssid, managed_wlan.connection.ssid_length);

	/* The refused keys, each as ACCOUNT:SSID-IN-HEX, separated by commas. */
	rejected[0] = '-';
	rejected[1] = '\0';
	used = 0U;
	for (index = 0U; index < wifi_rejection_count; index++) {
		count = snprintf(rejected + used, sizeof(rejected) - used, "%s%u:",
		    index == 0U ? "" : ",", (unsigned)wifi_rejections[index].account);
		if (count < 0 || (size_t)count >= sizeof(rejected) - used)
			break;
		used += (size_t)count;
		appended = append_hex(rejected, sizeof(rejected), &used, wifi_rejections[index].ssid, wifi_rejections[index].ssid_length);
		if (appended != 0)
			break;
	}

	/* The record. */
	count = snprintf(output + *output_length,
	    capacity - *output_length,
	    "wifi state=%s interface=%s owner=%u store=%s sessions=%s ssid=%s rejected=%s scan=%u\n",
	    managed_state_name(managed_wlan.state), interface,
	    (unsigned)managed_wlan.owner_uid, store, sessions, ssid, rejected,
	    (unsigned)wifi_scan_wanted());
	if (count < 0 || (size_t)count >= capacity - *output_length) {
		errno = EOVERFLOW;
		return -1;
	}
	*output_length += (size_t)count;

	/* Succeeded: the record is appended. */
	return 0;
}

/* Appends bytes as lower-case hex to a text at *used, which stays terminated; fails when it does not fit. */
static int
append_hex(
	char *text,
	size_t capacity,
	size_t *used,
	const unsigned char *bytes,
	size_t length)
{
	static const char digits[] = "0123456789abcdef";
	size_t index;

	/* Two digits a byte and the terminator must fit. */
	if (*used + length * 2U + 1U > capacity) {
		errno = EOVERFLOW;
		return -1;
	}

	/* Each byte, high digit first. */
	for (index = 0U; index < length; index++) {
		text[*used] = digits[bytes[index] >> 4];
		text[*used + 1U] = digits[bytes[index] & 0x0fU];
		*used += 2U;
	}
	text[*used] = '\0';

	/* Succeeded: the text holds the bytes. */
	return 0;
}

/* Includes child termination grace in the remaining selection interval. */
static unsigned
wifi_selection_timeout(
	uint64_t deadline)
{
	uint64_t now;
	uint64_t seconds;

	now = netutil_monotonic_us();
	if (now >= deadline)
		return 0U;
	seconds = (deadline - now) / 1000000ULL;
	if (seconds <= 1U)
		return 0U;
	seconds--;
	return seconds > 5U ? 5U : (unsigned)seconds;
}

/* Freezes one bounded candidate wave in profile order then radio order. */
static int
collect_profile_radios(
	const struct networkd_wlan_radio *radios,
	size_t radio_count,
	const struct wifi_conf_model *model,
	size_t skip,
	struct networkd_wifi_candidate *candidates,
	size_t *candidate_count,
	size_t *total_count,
	uint64_t deadline)
{
	struct networkd_wifi_child_result result;
	struct networkd_wifi_list_result parsed;
	struct timespec delay;
	unsigned char terminal[NETWORKD_WLAN_RADIO_MAX];
	unsigned char visible[WIFI_CONF_PROFILE_MAX]
	    [NETWORKD_WLAN_RADIO_MAX];
	size_t profile_index;
	size_t radio_index;
	size_t candidate_index;
	size_t available;
	unsigned valid_scans;
	unsigned timeout;
	int first_error;
	int automatic_present;
	int all_terminal;
	int parse_error;

	/* Initializes a complete bounded visibility matrix. */
	if (radios == NULL || model == NULL || candidates == NULL ||
	    candidate_count == NULL || total_count == NULL || radio_count == 0U ||
	    radio_count > NETWORKD_WLAN_RADIO_MAX ||
	    model->profile_count > WIFI_CONF_PROFILE_MAX) {
		errno = EINVAL;
		return -1;
	}
	memset(&result, 0, sizeof(result));
	memset(&parsed, 0, sizeof(parsed));
	memset(terminal, 0, sizeof(terminal));
	memset(visible, 0, sizeof(visible));
	*candidate_count = 0U;
	*total_count = 0U;
	valid_scans = 0U;
	first_error = 0;
	all_terminal = 0;
	automatic_present = 0;
	for (profile_index = 0U; profile_index < model->profile_count;
	    profile_index++) {
		if (model->profiles[profile_index].automatic)
			automatic_present = 1;
	}
	if (!automatic_present) {
		/* No profile can consume this wave; still allow future discovery. */
		for (radio_index = 0U; radio_index < radio_count; radio_index++) {
			if (radios[radio_index].scan_state == WLAN_SCAN_COMPLETE)
				consume_wlan_snapshot(&radios[radio_index],
				    radios[radio_index].snapshot_generation);
		}
		errno = ENOENT;
		return -1;
	}
	delay.tv_sec = 0;
	delay.tv_nsec = 100000000L;
	for (radio_index = 0U; radio_index < radio_count; radio_index++) {
		if (!radios[radio_index].ready)
			terminal[radio_index] = 1U;
	}

	/* Observes each radio without making a ready candidate await another scan. */
	while (netutil_monotonic_us() < deadline) {
		if (wifi_wait_pump != NULL && wifi_wait_pump() != 0) {
			errno = EINTR;
			return -1;
		}
		for (radio_index = 0U; radio_index < radio_count;
		    radio_index++) {
			if (terminal[radio_index])
				continue;
			timeout = wifi_selection_timeout(deadline);
			if (timeout == 0U)
				break;
			if (run_wifi(radios[radio_index].interface, "list", NULL,
			    timeout, &result) != 0) {
				if (first_error == 0)
					first_error = errno != 0 ? errno : EIO;
				terminal[radio_index] = 1U;
				networkd_wifi_child_result_clear(&result);
				if (wifi_work.cancelled) {
					errno = EINTR;
					return -1;
				}
				continue;
			}
			parse_error = 0;
			for (profile_index = 0U;
			    profile_index < model->profile_count; profile_index++) {
				if (!model->profiles[profile_index].automatic)
					continue;
				if (networkd_wifi_child_parse_list(&result,
				    model->profiles[profile_index].ssid,
				    model->profiles[profile_index].ssid_length,
				    &parsed) != 0) {
					parse_error = errno;
					break;
				}
				if (parsed.scan_complete)
					consume_wlan_snapshot(&radios[radio_index],
					    parsed.snapshot_generation);
				if (parsed.scan_complete && parsed.ssid_supported)
					visible[profile_index][radio_index] = 1U;
				terminal[radio_index] = parsed.scan_terminal != 0;
			}
			networkd_wifi_child_result_clear(&result);
			if (parse_error == 0 && parsed.scan_complete)
				valid_scans++;
			if (parse_error != 0) {
				/* Retry a malformed completed observation with a fresh scan. */
				consume_wlan_snapshot(&radios[radio_index],
				    radios[radio_index].snapshot_generation);
				if (first_error == 0)
					first_error = parse_error;
				/* A malformed radio must not hide another usable radio. */
				terminal[radio_index] = 1U;
				for (profile_index = 0U;
				    profile_index < model->profile_count; profile_index++)
					visible[profile_index][radio_index] = 0U;
			}
		}

		/* Freeze usable observations now; profile/radio order breaks current ties. */
		available = 0U;
		for (profile_index = 0U; profile_index < model->profile_count;
		    profile_index++) {
			for (radio_index = 0U; radio_index < radio_count; radio_index++) {
				if (visible[profile_index][radio_index])
					available++;
			}
		}

		/* A skipped prefix still waits for later candidates or a terminal wave. */
		if (available > skip)
			break;

		all_terminal = 1;
		for (radio_index = 0U; radio_index < radio_count;
		    radio_index++) {
			if (!terminal[radio_index])
				all_terminal = 0;
		}
		if (all_terminal)
			break;
		(void)nanosleep(&delay, NULL);
	}

	/* A slow radio cannot permanently conceal another completed snapshot. */
	candidate_index = 0U;
	for (profile_index = 0U; profile_index < model->profile_count;
	    profile_index++) {
		if (!model->profiles[profile_index].automatic)
			continue;
		for (radio_index = 0U; radio_index < radio_count; radio_index++) {
			if (!visible[profile_index][radio_index])
				continue;
			if ((*total_count)++ >= skip &&
			    candidate_index < NETWORKD_WLAN_ATTEMPTS) {
				candidates[candidate_index].profile = &model->profiles[profile_index];
				candidates[candidate_index].radio = radio_index;
				candidate_index++;
			}
		}
	}
	*candidate_count = candidate_index;
	if (candidate_index != 0U)
		return 0;
	errno = valid_scans != 0U ? ENOENT :
	    (first_error != 0 ? first_error : (all_terminal ? EIO : ETIMEDOUT));
	return -1;
}

/* Selects the first stable-order radio which sees one manual target. */
static int
select_manual_radio(
	const struct networkd_wlan_radio *radios,
	size_t radio_count,
	const struct wifi_conf_profile *profile,
	size_t *selected_radio,
	uint64_t deadline)
{
	struct networkd_wifi_child_result result;
	struct networkd_wifi_list_result parsed;
	struct timespec delay;
	unsigned char terminal[NETWORKD_WLAN_RADIO_MAX];
	unsigned char visible[NETWORKD_WLAN_RADIO_MAX];
	size_t radio_index;
	unsigned timeout;
	int earlier_terminal;

	/* Initializes one bounded visibility result for each stable-order radio. */
	if (radios == NULL || profile == NULL || selected_radio == NULL ||
	    radio_count == 0U || radio_count > NETWORKD_WLAN_RADIO_MAX) {
		errno = EINVAL;
		return -1;
	}
	memset(&result, 0, sizeof(result));
	memset(&parsed, 0, sizeof(parsed));
	memset(terminal, 0, sizeof(terminal));
	memset(visible, 0, sizeof(visible));
	*selected_radio = 0U;
	delay.tv_sec = 0;
	delay.tv_nsec = 100000000L;
	for (radio_index = 0U; radio_index < radio_count; radio_index++) {
		if (!radios[radio_index].ready)
			terminal[radio_index] = 1U;
	}

	/* Polls all asynchronous scans without preferring the fastest radio. */
	while (netutil_monotonic_us() < deadline) {
		if (wifi_wait_pump != NULL && wifi_wait_pump() != 0) {
			errno = EINTR;
			return -1;
		}
		for (radio_index = 0U; radio_index < radio_count;
		    radio_index++) {
			if (terminal[radio_index])
				continue;
			timeout = wifi_selection_timeout(deadline);
			if (timeout == 0U)
				break;
			if (run_wifi(radios[radio_index].interface, "list", NULL,
			    timeout, &result) != 0) {
				terminal[radio_index] = 1U;
				networkd_wifi_child_result_clear(&result);
				if (wifi_work.cancelled) {
					errno = EINTR;
					return -1;
				}
				continue;
			}
			if (networkd_wifi_child_parse_list(&result, profile->ssid,
			    profile->ssid_length, &parsed) != 0) {
				networkd_wifi_child_result_clear(&result);
				terminal[radio_index] = 1U;
				continue;
			}
			terminal[radio_index] = parsed.scan_terminal != 0;
			visible[radio_index] = parsed.scan_complete &&
			    parsed.ssid_supported;
			networkd_wifi_child_result_clear(&result);
		}

		/* Select among completed observations, without waiting for unseen radios. */
		for (radio_index = 0U; radio_index < radio_count;
		    radio_index++) {
			if (visible[radio_index]) {
				*selected_radio = radio_index;
				return 0;
			}
		}
		earlier_terminal = 1;
		for (radio_index = 0U; radio_index < radio_count;
		    radio_index++) {
			if (!terminal[radio_index])
				earlier_terminal = 0;
		}
		if (earlier_terminal) {
			errno = ENOENT;
			return -1;
		}
		(void)nanosleep(&delay, NULL);
	}

	/* At the fixed limit, choose among the radios that actually completed. */
	for (radio_index = 0U; radio_index < radio_count; radio_index++) {
		if (visible[radio_index]) {
			*selected_radio = radio_index;
			return 0;
		}
	}
	errno = ETIMEDOUT;
	return -1;
}

/* Tries a frozen candidate wave without renewing selection or work deadlines. */
static int
connect_automatic(
	const struct networkd_wlan_radio *radios,
	size_t radio_count,
	const struct wifi_conf_model *model,
	const uid_t *stores,
	uint64_t deadline,
	char *output,
	size_t output_capacity,
	size_t *output_length,
	int *no_candidate)
{
	struct networkd_wifi_candidate candidates[NETWORKD_WLAN_ATTEMPTS];
	size_t count;
	size_t total;
	size_t attempt;
	size_t attempted;
	size_t radio;
	size_t profile_index;
	uint64_t selection_deadline;
	int l2_succeeded;
	int saved;

	if (no_candidate == NULL) {
		errno = EINVAL;
		return -1;
	}
	*no_candidate = 0;
	memset(candidates, 0, sizeof(candidates));
	selection_deadline = netutil_monotonic_us() +
	    NETWORKD_WLAN_SCAN_SECONDS * 1000000ULL;
	if (selection_deadline > deadline)
		selection_deadline = deadline;
	if (collect_profile_radios(radios, radio_count, model, automatic_candidate_skip,
	    candidates, &count, &total, selection_deadline) != 0) {
		automatic_candidate_skip = 0U;
		if (errno == ENOENT || errno == ETIMEDOUT)
			*no_candidate = 1;
		return -1;
	}
	if (wifi_work.profiles_changed) {
		automatic_candidate_skip = 0U;
		errno = EAGAIN;
		return -1;
	}
	saved = ENOENT;
	attempted = 0U;
	for (attempt = 0U; attempt < count; attempt++) {
		attempted++;
		radio = candidates[attempt].radio;
		l2_succeeded = 0;
		if (run_managed_connect(radios[radio].interface,
		    candidates[attempt].profile, NETWORKD_WLAN_AUTO_SEARCHING,
		    deadline, output, output_capacity, output_length, &l2_succeeded) == 0) {
			stop_losing_scans(radios, radio_count, radio);
			automatic_candidate_skip = 0U;
			return 0;
		}
		saved = errno != 0 ? errno : EIO;

		/* A refused key is not offered again until its store changes. */
		if (saved == EACCES && stores != NULL) {
			profile_index = (size_t)(candidates[attempt].profile - model->profiles);
			wifi_rejection_note(stores[profile_index], candidates[attempt].profile);
		}

		/* The wave ends when it was cancelled, its candidates changed, a connection exists or time ran out. */
		if (wifi_work.cancelled || wifi_work.profiles_changed ||
		    managed_wlan.connection.interface[0] != '\0' ||
		    netutil_monotonic_us() >= deadline)
			break;
	}
	/* Later waves must reach candidates beyond a repeatedly failing prefix. */
	if (wifi_work.profiles_changed)
		automatic_candidate_skip = 0U;
	else {
		automatic_candidate_skip += attempted;
		if (automatic_candidate_skip >= total)
			automatic_candidate_skip = 0U;
	}
	errno = saved;
	return -1;
}

/* Performs one L2/DHCP transaction with a single failure-retirement path. */
static int
run_managed_connect(
	const char *interface,
	const struct wifi_conf_profile *profile,
	enum networkd_managed_wlan_state failure_state,
	uint64_t deadline,
	char *output,
	size_t output_capacity,
	size_t *output_length,
	int *l2_succeeded)
{
	struct networkd_wifi_child_result child;
	uint64_t now;
	uint32_t ifindex;
	unsigned timeout;
	int result;
	int retired;
	int saved;

	*l2_succeeded = 0;
	if (route_events >= 0 && process_route_events() != 0) {
		(void)close(route_events);
		route_events = -1;
	}
	now = netutil_monotonic_us();
	if (now >= deadline || deadline - now < 1000000ULL) {
		errno = ETIMEDOUT;
		return -1;
	}
	if (interface_index(interface, &ifindex) != 0)
		return -1;
	if (networkd_managed_wlan_begin_connect(&managed_wlan, interface,
	    ifindex, route_event_sequence, profile->ssid, profile->ssid_length) != 0)
		return -1;

	/* A new connection's store is not known until its join succeeds. */
	wifi_connection_store_known = 0;

	memset(&child, 0, sizeof(child));
	timeout = (unsigned)((deadline - now) / 1000000ULL);
	if (timeout > NETWORKD_WLAN_CONNECT_SECONDS)
		timeout = NETWORKD_WLAN_CONNECT_SECONDS;
	result = run_wifi(interface, "connect", profile, timeout, &child);
	if (result == 0) {
		*l2_succeeded = 1;
		if (output != NULL && output_length != NULL)
			result = append_wifi_output(interface, &child, output,
			    output_capacity, output_length);
	}
	saved = errno;
	networkd_wifi_child_result_clear(&child);
	errno = saved;
	if (result == 0)
		result = acquire_managed_l3(interface, deadline);

	/* Reports a join that worked. */
	if (result == 0)
		return 0;

	/*
	 * A failed join is always retired: a stopped child is never substituted
	 * for a proven L2/L3 retirement.  The retirement does not rename the
	 * failure, though.  The caller and the user are told why the join failed
	 * (a key the network refused is EACCES), not why the cleanup after it
	 * failed: a radio that is recovering refuses the cleanup's disconnect with
	 * ENETDOWN, and that used to reach the user as "Network is down"
	 * (BUG-157).  A cleanup that could not finish keeps the connection's
	 * ownership, logs its stage and schedules its own retry.
	 */
	saved = errno;
	retired = retire_managed_connection(failure_state, 1);
	if (retired != 0) {
		fprintf(stderr,
		    "networkd: %s: join failed: %s; its cleanup is retried\n",
		    interface, strerror(saved));
	}

	/* Reports why the join failed. */
	errno = saved;
	return result;
}

/* Records the baseline before DHCP and commits only a complete transaction. */
static int
acquire_managed_l3(
	const char *interface,
	uint64_t deadline)
{
	struct networkd_managed_l3 before;
	struct networkd_managed_l3 after;
	char diagnostic[CHILD_OUTPUT_MAX];
	char seconds[16];
	char *arguments[5];
	uint64_t now;
	uint32_t ifindex;
	unsigned timeout;
	int result;
	int saved;

	memset(&before, 0, sizeof(before));
	memset(&after, 0, sizeof(after));
	memset(diagnostic, 0, sizeof(diagnostic));
	result = snapshot_interface_l3(interface, &ifindex, &before);
	if (result == 0 && ifindex != managed_wlan.connection.ifindex) {
		errno = ENODEV;
		result = -1;
	}
	if (result == 0)
		result = networkd_managed_wlan_begin_l3(&managed_wlan, &before);
	now = netutil_monotonic_us();
	if (result == 0 && (now >= deadline || deadline - now < 1000000ULL)) {
		errno = ETIMEDOUT;
		result = -1;
	}
	if (result == 0) {
		timeout = (unsigned)((deadline - now) / 1000000ULL);
		if (timeout > NETWORKD_WLAN_DHCP_SECONDS)
			timeout = NETWORKD_WLAN_DHCP_SECONDS;
		(void)snprintf(seconds, sizeof(seconds), "%u", timeout);
		arguments[0] = "/sbin/dhcpc";
		arguments[1] = "-t";
		arguments[2] = seconds;
		arguments[3] = (char *)interface;
		arguments[4] = NULL;
		fprintf(stderr, "networkd: %s: Wi-Fi authenticated; acquiring DHCP\n",
		    interface);
		result = run_command_until(arguments, timeout, deadline, diagnostic);
	}
	if (result == 0)
		result = snapshot_managed_l3(&managed_wlan, &after);
	if (result == 0 && (after.address == 0U || !wlan_connected(interface))) {
		errno = ENETDOWN;
		result = -1;
	}
	if (result == 0) {
		identify_l3_ownership(interface, &before, &after);
		result = networkd_managed_wlan_commit_l3(&managed_wlan, &after);
	}

	/* A wired connection keeps the default route and resolver (B3). */
	if (result == 0)
		apply_network_preference();

	/* On failure the retained baseline remains available to retirement. */
	saved = errno;
	networkd_protocol_clear(diagnostic, sizeof(diagnostic));
	networkd_protocol_clear(&before, sizeof(before));
	networkd_protocol_clear(&after, sizeof(after));
	errno = saved;
	return result;
}

/* Reconstructs partial DHCP ownership before retrying a failed transaction. */
static int
reconcile_pending_l3(
	void)
{
	struct networkd_managed_l3 current;
	int result;
	int saved;

	memset(&current, 0, sizeof(current));
	result = snapshot_managed_l3(&managed_wlan, &current);
	if (result == 0) {
		identify_l3_ownership(managed_wlan.connection.interface,
		    &managed_wlan.connection.l3_before, &current);
		result = networkd_managed_wlan_track_l3(&managed_wlan, &current);
	}
	saved = errno;
	networkd_protocol_clear(&current, sizeof(current));
	errno = saved;
	return result;
}

/* Captures one interface's exact current IPv4, route, and resolver state. */
static int
snapshot_interface_l3(
	const char *interface,
	uint32_t *ifindex,
	struct networkd_managed_l3 *snapshot)
{
	int descriptor;
	int saved;

	/* Initializes output before opening any fallible resource. */
	if (interface == NULL || ifindex == NULL || snapshot == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(snapshot, 0, sizeof(*snapshot));
	*ifindex = 0U;

	/* Captures the name identity and all three IPv4 interface fields. */
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return -1;
	if (netutil_ifindex(descriptor, interface, ifindex) != 0 ||
	    get_interface_ipv4(descriptor, interface, SIOCGIFADDR,
	    &snapshot->address) != 0 ||
	    get_interface_ipv4(descriptor, interface, SIOCGIFNETMASK,
	    &snapshot->netmask) != 0 ||
	    get_interface_ipv4(descriptor, interface, SIOCGIFBRDADDR,
	    &snapshot->broadcast) != 0 ||
	    find_interface_default(descriptor, *ifindex, snapshot) != 0) {
		saved = errno;
		(void)close(descriptor);
		networkd_protocol_clear(snapshot, sizeof(*snapshot));
		*ifindex = 0U;
		errno = saved;
		return -1;
	}
	if (close(descriptor) != 0) {
		saved = errno;
		networkd_protocol_clear(snapshot, sizeof(*snapshot));
		*ifindex = 0U;
		errno = saved;
		return -1;
	}

	/* Adds the bounded byte-exact resolver snapshot. */
	if (snapshot_resolver(snapshot) != 0) {
		saved = errno;
		networkd_protocol_clear(snapshot, sizeof(*snapshot));
		*ifindex = 0U;
		errno = saved;
		return -1;
	}

	/* Reports one complete coherent snapshot. */
	return 0;
}

/* Captures one live managed identity with bounded transient retries. */
static int
snapshot_managed_l3(
	const struct networkd_managed_wlan *record,
	struct networkd_managed_l3 *snapshot)
{
	const struct networkd_managed_wlan_connection *connection;
	uint32_t ifindex;
	unsigned attempt;
	int saved;

	/* Retries only the snapshot, never the already successful DHCP child. */
	if (record == NULL) {
		errno = EINVAL;
		return -1;
	}
	connection = &record->connection;
	saved = EIO;
	for (attempt = 0U; attempt < 3U; attempt++) {
		if (snapshot_interface_l3(connection->interface, &ifindex,
		    snapshot) == 0) {
			if (ifindex == connection->ifindex)
				return 0;
			saved = ENODEV;
			break;
		}
		saved = errno != 0 ? errno : EIO;
	}

	/* Reports identity loss or a persistent snapshot failure unchanged. */
	errno = saved;
	return -1;
}

/* Captures bounded resolver contents while preserving other L3 fields. */
static int
snapshot_resolver(
	struct networkd_managed_l3 *snapshot)
{
	unsigned char extra;
	ssize_t count;
	size_t used;
	int descriptor;
	int saved;

	/* Clears prior resolver ownership before reading current file state. */
	if (snapshot == NULL) {
		errno = EINVAL;
		return -1;
	}
	snapshot->resolver_present = 0;
	snapshot->resolver_oversized = 0;
	snapshot->resolver_owned = 0;
	snapshot->resolver_length = 0U;
	networkd_protocol_clear(snapshot->resolver, sizeof(snapshot->resolver));

	/* Treats absence as a complete and exact resolver snapshot. */
	descriptor = open("/etc/resolv.conf", O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		if (errno == ENOENT)
			return 0;
		return -1;
	}

	/* Reads the complete file without silently truncating ownership data. */
	used = 0U;
	while (used < sizeof(snapshot->resolver)) {
		count = read(descriptor, snapshot->resolver + used,
		    sizeof(snapshot->resolver) - used);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			saved = errno;
			(void)close(descriptor);
			networkd_protocol_clear(snapshot->resolver,
			    sizeof(snapshot->resolver));
			errno = saved;
			return -1;
		}
		if (count == 0)
			break;
		used += (size_t)count;
	}
	if (used == sizeof(snapshot->resolver)) {
		do {
			count = read(descriptor, &extra, sizeof(extra));
		} while (count < 0 && errno == EINTR);
		if (count < 0) {
			saved = errno;
			(void)close(descriptor);
			networkd_protocol_clear(snapshot->resolver,
			    sizeof(snapshot->resolver));
			errno = saved;
			return -1;
		}

		/* Preserve oversized external files without blocking unrelated L3 work. */
		if (count > 0) {
			snapshot->resolver_oversized = 1;
			used = 0U;
			networkd_protocol_clear(snapshot->resolver,
			    sizeof(snapshot->resolver));
		}
	}
	if (close(descriptor) != 0) {
		saved = errno;
		networkd_protocol_clear(snapshot->resolver,
		    sizeof(snapshot->resolver));
		errno = saved;
		return -1;
	}
	snapshot->resolver_present = 1;
	snapshot->resolver_length = used;

	/* Reports exact bytes, absence, or an explicitly unowned oversized file. */
	return 0;
}

/* Marks only post-DHCP values which differ from the saved pre-state. */
static void
identify_l3_ownership(
	const char *interface,
	const struct networkd_managed_l3 *previous,
	struct networkd_managed_l3 *committed)
{
	char marker[80];
	size_t marker_length;
	int changed;
	int length;

	/* Owns an IPv4 tuple only when DHCP changed at least one exact value. */
	committed->ipv4_owned =
	    previous->address != committed->address ||
	    previous->netmask != committed->netmask ||
	    previous->broadcast != committed->broadcast;

	/* Owns a default route only when DHCP changed its canonical value. */
	committed->default_route_owned = committed->default_route_present &&
	    (!previous->default_route_present ||
	    !managed_routes_equal(&previous->default_route,
	    &committed->default_route));

	/* Owns resolver data only when dhcpc replaced the preexisting file. */
	committed->resolver_owned = 0;

	/* An unbounded file can never become a bounded ownership token. */
	if (committed->resolver_oversized)
		return;
	changed = previous->resolver_present != committed->resolver_present ||
	    previous->resolver_oversized != committed->resolver_oversized ||
	    previous->resolver_length != committed->resolver_length;
	if (!changed && committed->resolver_length != 0U)
		changed = memcmp(previous->resolver, committed->resolver,
		    committed->resolver_length) != 0;
	if (!changed || !committed->resolver_present)
		return;

	/* Requires the exact interface-specific dhcpc marker. */
	length = snprintf(marker, sizeof(marker),
	    "# Generated by dhcpc for %s\n", interface);
	if (length < 0 || (size_t)length >= sizeof(marker))
		return;
	marker_length = (size_t)length;
	if (committed->resolver_length < marker_length)
		return;
	if (memcmp(committed->resolver, marker, marker_length) != 0)
		return;
	committed->resolver_owned = 1;
}

/* Clears only L3 resources still equal to one managed ownership token. */
static int
clear_interface_l3(
	struct networkd_managed_wlan *record)
{
	struct networkd_managed_wlan_connection *connection;
	struct networkd_managed_l3_cleanup cleanup;
	struct networkd_managed_l3 current;
	uint32_t current_ifindex;
	uint32_t address;
	uint32_t netmask;
	uint32_t broadcast;
	int descriptor;
	int first_error;
	int planned;

	/* Snapshots live state before authorizing any destructive operation. */
	memset(&cleanup, 0, sizeof(cleanup));
	memset(&current, 0, sizeof(current));
	if (record == NULL) {
		errno = EINVAL;
		return -1;
	}
	connection = &record->connection;
	if (snapshot_interface_l3(connection->interface,
	    &current_ifindex, &current) != 0)
		return -1;
	planned = networkd_managed_wlan_plan_l3_cleanup(record,
	    current_ifindex, &current, &cleanup);
	if (planned != 0 && errno != ESTALE) {
		networkd_protocol_clear(&current, sizeof(current));
		return -1;
	}
	/* A changed resource belongs to its new writer, not to this old token. */
	if (planned != 0) {
		if (!cleanup.clear_ipv4)
			connection->l3.ipv4_owned = 0;
		if (!cleanup.delete_default_route)
			connection->l3.default_route_owned = 0;
		if (!cleanup.unlink_resolver)
			connection->l3.resolver_owned = 0;
		fprintf(stderr, "networkd: preserved externally changed Wi-Fi L3 state\n");
	}
	first_error = 0;

	/* Revalidates and clears only an unchanged owned IPv4 tuple. */
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0) {
		if (first_error == 0)
			first_error = errno;
	} else if (cleanup.clear_ipv4) {
		if (interface_index_name_matches(connection->ifindex,
		    connection->interface) != 0 ||
		    get_interface_ipv4(descriptor, connection->interface,
		    SIOCGIFADDR, &address) != 0 ||
		    get_interface_ipv4(descriptor, connection->interface,
		    SIOCGIFNETMASK, &netmask) != 0 ||
		    get_interface_ipv4(descriptor, connection->interface,
		    SIOCGIFBRDADDR, &broadcast) != 0) {
			if (first_error == 0)
				first_error = errno;
		} else if (address != connection->l3.address ||
		    netmask != connection->l3.netmask ||
		    broadcast != connection->l3.broadcast) {
			connection->l3.ipv4_owned = 0;
		} else {
			if (set_interface_ipv4(descriptor, connection->interface,
			    SIOCSIFNETMASK, 0U) != 0)
				first_error = errno;
			else
				connection->l3.netmask = 0U;
			if (set_interface_ipv4(descriptor, connection->interface,
			    SIOCSIFBRDADDR, 0U) != 0) {
				if (first_error == 0)
					first_error = errno;
			} else
				connection->l3.broadcast = 0U;
			if (set_interface_ipv4(descriptor, connection->interface,
			    SIOCSIFADDR, 0U) != 0) {
				if (first_error == 0)
					first_error = errno;
			} else
				connection->l3.address = 0U;
			if (first_error == 0)
				connection->l3.ipv4_owned = 0;
		}
	}

	/* Deletes only the exact route still held by the same interface. */
	if (descriptor >= 0 && cleanup.delete_default_route) {
		if (interface_index_name_matches(connection->ifindex,
		    connection->interface) != 0 ||
		    delete_interface_default_exact(descriptor,
		    &connection->l3.default_route) != 0) {
			if (errno == ESTALE || errno == ENOENT)
				connection->l3.default_route_owned = 0;
			else if (first_error == 0)
				first_error = errno;
		} else
			connection->l3.default_route_owned = 0;
	}
	if (descriptor >= 0 && close(descriptor) != 0 && first_error == 0)
		first_error = errno;

	/* Unlinks only byte-identical resolver data after identity revalidation. */
	if (cleanup.unlink_resolver) {
		if (interface_index_name_matches(connection->ifindex,
		    connection->interface) != 0 ||
		    unlink_owned_resolver(record) != 0) {
			if (errno == ESTALE || errno == ENOENT)
				connection->l3.resolver_owned = 0;
			else if (first_error == 0)
				first_error = errno;
		} else
			connection->l3.resolver_owned = 0;
	}
	networkd_protocol_clear(&current, sizeof(current));
	networkd_protocol_clear(&cleanup, sizeof(cleanup));

	/* Reports exact success or a preserved external-state degradation. */
	if (first_error != 0) {
		errno = first_error;
		return -1;
	}
	return 0;
}

/* Finds at most one exact current default route for an interface. */
static int
find_interface_default(
	int descriptor,
	uint32_t ifindex,
	struct networkd_managed_l3 *snapshot)
{
	struct rtentry entry;
	struct sockaddr_in *destination;
	struct sockaddr_in *gateway;
	struct sockaddr_in *mask;
	unsigned ordinal;
	unsigned matches;

	/* Enumerates the complete route table and rejects ambiguous ownership. */
	matches = 0U;
	for (ordinal = 0U;; ordinal++) {
		memset(&entry, 0, sizeof(entry));
		entry.rt_index = ordinal;
		if (ioctl(descriptor, SIOCGRTENTRY, &entry) != 0) {
			if (errno == ENOENT)
				break;
			return -1;
		}
		destination = (struct sockaddr_in *)&entry.rt_dst;
		mask = (struct sockaddr_in *)&entry.rt_genmask;
		if (entry.rt_ifindex != ifindex ||
		    destination->sin_addr.s_addr != 0U ||
		    mask->sin_addr.s_addr != 0U)
			continue;
		matches++;
		if (matches > 1U) {
			errno = EBUSY;
			return -1;
		}
		gateway = (struct sockaddr_in *)&entry.rt_gateway;
		snapshot->default_route_present = 1;
		snapshot->default_route.flags = entry.rt_flags;
		snapshot->default_route.ifindex = entry.rt_ifindex;
		snapshot->default_route.destination =
		    destination->sin_addr.s_addr;
		snapshot->default_route.gateway = gateway->sin_addr.s_addr;
		snapshot->default_route.netmask = mask->sin_addr.s_addr;
	}

	/* Reports a canonical absent or uniquely present route. */
	return 0;
}

/* Deletes one default route only after a second exact table comparison. */
static int
delete_interface_default_exact(
	int descriptor,
	const struct networkd_managed_route *owned)
{
	struct rtentry entry;
	struct rtentry selected;
	unsigned ordinal;
	unsigned matches;

	/* Selects one byte-semantically identical route without broad deletion. */
	matches = 0U;
	memset(&selected, 0, sizeof(selected));
	for (ordinal = 0U;; ordinal++) {
		memset(&entry, 0, sizeof(entry));
		entry.rt_index = ordinal;
		if (ioctl(descriptor, SIOCGRTENTRY, &entry) != 0) {
			if (errno == ENOENT)
				break;
			return -1;
		}
		if (!route_matches_owned(&entry, owned))
			continue;
		matches++;
		selected = entry;
	}
	if (matches != 1U) {
		errno = matches == 0U ? ESTALE : EBUSY;
		return -1;
	}

	/* Deletes exactly the route selected by the stable semantic fields. */
	return ioctl(descriptor, SIOCDELRT, &selected);
}

/* Compares one route-table entry with a canonical managed route. */
static int
route_matches_owned(
	const struct rtentry *entry,
	const struct networkd_managed_route *owned)
{
	const struct sockaddr_in *destination;
	const struct sockaddr_in *gateway;
	const struct sockaddr_in *mask;

	/* Compares every stable field while ignoring the transient ordinal. */
	destination = (const struct sockaddr_in *)&entry->rt_dst;
	gateway = (const struct sockaddr_in *)&entry->rt_gateway;
	mask = (const struct sockaddr_in *)&entry->rt_genmask;
	if (entry->rt_flags != owned->flags)
		return 0;
	if (entry->rt_ifindex != owned->ifindex)
		return 0;
	if (destination->sin_addr.s_addr != owned->destination)
		return 0;
	if (gateway->sin_addr.s_addr != owned->gateway)
		return 0;
	if (mask->sin_addr.s_addr != owned->netmask)
		return 0;

	/* Reports exact canonical equality. */
	return 1;
}

/* Compares every canonical field of two managed default routes. */
static int
managed_routes_equal(
	const struct networkd_managed_route *left,
	const struct networkd_managed_route *right)
{
	/* Compares fields explicitly so no representation padding is relevant. */
	if (left->flags != right->flags)
		return 0;
	if (left->ifindex != right->ifindex)
		return 0;
	if (left->destination != right->destination)
		return 0;
	if (left->gateway != right->gateway)
		return 0;
	if (left->netmask != right->netmask)
		return 0;

	/* Reports exact canonical equality. */
	return 1;
}

/* Reads one IPv4 interface field into a network-order scalar. */
static int
get_interface_ipv4(
	int descriptor,
	const char *interface,
	unsigned long command,
	uint32_t *value)
{
	struct ifreq request;
	struct sockaddr_in *address;

	/* Issues one bounded generic interface query. */
	if (value == NULL || netutil_ifreq(&request, interface) != 0)
		return -1;
	if (ioctl(descriptor, command, &request) != 0)
		return -1;
	address = (struct sockaddr_in *)&request.ifr_addr;
	*value = address->sin_addr.s_addr;
	return 0;
}

/* Sets one IPv4 interface field to the supplied network-order value. */
static int
set_interface_ipv4(
	int descriptor,
	const char *interface,
	unsigned long command,
	uint32_t value)
{
	struct ifreq request;
	struct sockaddr_in *address;

	/* Builds the ordinary generic interface request. */
	if (netutil_ifreq(&request, interface) != 0)
		return -1;
	address = (struct sockaddr_in *)&request.ifr_addr;
	address->sin_family = AF_INET;
	address->sin_addr.s_addr = value;
	return ioctl(descriptor, command, &request);
}

/* Unlinks only a resolver file still equal to the owned bounded contents. */
static int
unlink_owned_resolver(
	const struct networkd_managed_wlan *record)
{
	const struct networkd_managed_wlan_connection *connection;
	struct networkd_managed_l3 current;
	int equal;
	int result;
	int saved;

	/* Re-reads the complete file immediately before the destructive step. */
	if (record == NULL) {
		errno = EINVAL;
		return -1;
	}
	connection = &record->connection;
	memset(&current, 0, sizeof(current));
	if (snapshot_resolver(&current) != 0)
		return -1;
	equal = current.resolver_present && !current.resolver_oversized &&
	    current.resolver_length == connection->l3.resolver_length;
	if (equal && current.resolver_length != 0U)
		equal = memcmp(current.resolver, connection->l3.resolver,
		    current.resolver_length) == 0;
	if (!equal) {
		networkd_protocol_clear(&current, sizeof(current));
		errno = ESTALE;
		return -1;
	}

	/* Removes only the file whose full content passed the second check. */
	result = unlink("/etc/resolv.conf");
	saved = errno;
	networkd_protocol_clear(&current, sizeof(current));
	errno = saved;
	return result;
}

/*
 * Gives an interface's DHCPv6 lease back (`dhcpc -6 -r`, which does
 * nothing without one) and forgets its DHCPv6 schedule (ws177-p046).
 */
static void
release_dhcp6(
	const char *interface,
	uint64_t deadline,
	char diagnostic[CHILD_OUTPUT_MAX])
{
	char *arguments[8];
	int missing;

	/* An interface there. */
	missing = interface_exists(interface);
	if (missing != 0)
		return;

	/* dhcpc -6 -r -t 2 IF, its outcome not the request's. */
	arguments[0] = "/sbin/dhcpc";
	arguments[1] = "-6";
	arguments[2] = "-r";
	arguments[3] = "-t";
	arguments[4] = "2";
	arguments[5] = (char *)interface;
	arguments[6] = NULL;
	(void)run_command_until(arguments, 4, deadline, diagnostic);
	diagnostic[0] = '\0';
	networkd_ipv6_forget(interface);
}

/*
 * Gives an interface's rank for IPv6's default route and DNS servers
 * (ws177-p045, ws005-p019's choice): a wired interface networkd manages
 * by its place, then any other interface, then the Wi-Fi connection's.
 */
unsigned
networkd_interface_rank(
	const char *name)
{
	size_t index;
	int same;

	/* A managed wired interface: its place. */
	for (index = 0U; index < managed_lan.interface_count; index++) {
		same = strcmp(managed_lan.interfaces[index].name, name);
		if (same == 0)
			return (unsigned)index;
	}

	/* The Wi-Fi connection's interface: last. */
	same = strcmp(managed_wlan.connection.interface, name);
	if (same == 0)
		return NETWORKD_LAN_MAX + 1U;

	/* Any other: after the managed wired ones. */
	return NETWORKD_LAN_MAX;
}

/* Runs one child with both an operation bound and an optional transaction deadline. */
static int
run_command_until(
	char *const arguments[],
	unsigned timeout_seconds,
	uint64_t deadline,
	char diagnostic[CHILD_OUTPUT_MAX])
{
	pid_t result;
	ssize_t count;
	char temporary[96];
	int output, status, child_done;
	int child_error;
	int standard_output, standard_error;
	pid_t child;
	uint64_t now;
	uint64_t operation_deadline;

	status = 0;
	child_done = 0;
	child_error = 0;
	now = netutil_monotonic_us();
	operation_deadline = now + (uint64_t)timeout_seconds * 1000000ULL;
	if (deadline == 0U || deadline > operation_deadline)
		deadline = operation_deadline;
	if (wifi_work.deadline != 0U && (deadline == 0U ||
	    deadline > wifi_work.deadline -
	    NETWORKD_WIFI_CLEANUP_SECONDS * 1000000ULL))
		deadline = wifi_work.deadline -
		    NETWORKD_WIFI_CLEANUP_SECONDS * 1000000ULL;
	diagnostic[0] = '\0';
	if (now >= deadline) {
		errno = ETIMEDOUT;
		return -1;
	}

	/* Handles a failed snprintf operation. */
	if (snprintf(temporary, sizeof(temporary), "/run/networkd-child.%ld",
		     (long)getpid()) >= (int)sizeof(temporary)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	output = open(temporary, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	/* Handles the output condition. */
	if (output < 0)
		return -1;
	child = fork();

	/* Checks the child process state. */
	if (child == 0) {
		/* Close every inherited or duplicated descriptor on failure. */
		standard_output = dup2(output, STDOUT_FILENO);

		/* Handles the standard output condition. */
		if (standard_output < 0) {
			close(STDOUT_FILENO);
			close(STDERR_FILENO);
			close(output);
			_exit(126);
		}
		standard_error = dup2(output, STDERR_FILENO);

		/* Handles an operation failure. */
		if (standard_error < 0) {
			close(STDOUT_FILENO);
			close(STDERR_FILENO);
			close(output);
			_exit(126);
		}
		close(output);
		execv(arguments[0], arguments);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
		_exit(127);
	}

	/* Checks the child process state. */
	if (child < 0) {
		child_error = errno;
		close(output);
		unlink(temporary);
		errno = child_error;

		/* Reports operation failure. */
		return -1;
	}
	while (!child_done) {
		if (wifi_wait_pump != NULL)
			child_error = wifi_wait_pump();
		if (child_error == 0 && netutil_monotonic_us() >= deadline)
			child_error = ETIMEDOUT;
		result = waitpid(child, &status, WNOHANG);

		/* Checks the operation result. */
		if (result == child)
			child_done = 1;
		else if (result < 0 && errno != EINTR) {
			child_error = errno != 0 ? errno : EIO;
			/* ECHILD means there is no remaining child to signal. */
			if (child_error != ECHILD) {
				(void)kill(child, SIGKILL);
				do {
					result = waitpid(child, &status, 0);
				} while (result < 0 && errno == EINTR);
			}
			child_done = 1;
		}

		/* Handles the child done condition. */
		if (child_done)
			break;

		/* Cancellation, deadline and wait errors retain their exact cause. */
		if (child_error != 0) {
			(void)kill(child, SIGKILL);
			do {
				result = waitpid(child, &status, 0);
			} while (result < 0 && errno == EINTR);
			break;
		}
		usleep(10000);
	}

	/* Handles a failed lseek operation. */
	if (lseek(output, 0, SEEK_SET) >= 0) {
		count = read(output, diagnostic, CHILD_OUTPUT_MAX - 1U);

		/* Checks the remaining item count. */
		if (count > 0)
			diagnostic[count] = '\0';
	}
	close(output);
	unlink(temporary);
	clean_diagnostic(diagnostic);

	/* Handles the ticks condition. */
	if (child_error != 0) {
		errno = child_error;
		return -1;
	}

	/* Handles a failed WIFEXITED operation. */
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		errno = WIFEXITED(status) && WEXITSTATUS(status) == 127 ? ENOENT
									: EIO;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the clean diagnostic operation. */
static void
clean_diagnostic(
	char *text)
{
	size_t index, length;

	/* Process each remaining element. */
	length = strlen(text);
	while (length != 0 &&
	       (text[length - 1U] == '\n' || text[length - 1U] == '\r'))

	/* Process each remaining element. */
		text[--length] = '\0';
	for (index = 0; index < length; index++) {
		/* Validates the current text. */
		if ((unsigned char)text[index] < 32U || text[index] == 127)
			text[index] = ' ';
	}
}

/* Supports the default route exists operation. */
static int
default_route_exists(
	void)
{
	const struct sockaddr_in *destination;
	const struct sockaddr_in *mask;
	struct rtentry route;
	int descriptor;

	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	/* Process each remaining element. */
	for (route.rt_index = 0; ioctl(descriptor, SIOCGRTENTRY, &route) == 0;
	     route.rt_index++) {
		destination = (const struct sockaddr_in *)&route.rt_dst;
		mask = (const struct sockaddr_in *)&route.rt_genmask;

		/* Handles the destination condition. */
		if (destination->sin_addr.s_addr == 0 &&
		    mask->sin_addr.s_addr == 0) {
			close(descriptor);

			/* Reports operation failure. */
			return 1;
		}
	}
	close(descriptor);

	/* Returns the computed result. */
	return errno == ENOENT ? 0 : -1;
}

/* Supports the write resolver operation. */
static int
write_resolver(
	char *const addresses[],
	int count)
{
	int length;
	struct in_addr parsed;
	struct in6_addr parsed6;
	char temporary[128], line[80];
	int descriptor, index, prior;

	/* Handles a failed snprintf operation. */
	if (snprintf(temporary, sizeof(temporary), "/etc/resolv.conf.tmp.%ld",
		     (long)getpid()) >= (int)sizeof(temporary)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	NCOM_TRACE("resolver-open-enter", 0);
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0644);
	NCOM_TRACE("resolver-open-done", 0);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	/* Handles a failed write all operation. */
	if (write_all(descriptor, "# Generated by networkd\n", 24) != 0)
		goto fail;

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		/* An IPv4 server, or an IPv6 one (ws130-p005). */
		if (netutil_parse_ipv4(addresses[index], &parsed) != 0 &&
		    inet_pton(AF_INET6, addresses[index], &parsed6) != 1)
			goto fail;

		/* Process each remaining element. */
		for (prior = 0; prior < index; prior++) {
			/* Selects the matching value. */
			if (strcmp(addresses[prior], addresses[index]) == 0)
				break;
		}

		/* Handles the prior condition. */
		if (prior != index)
			continue;
		length = snprintf(line, sizeof(line), "nameserver %s\n",
	     addresses[index]);

		/* Handles a failed write all operation. */
		if (length < 0 || (size_t)length >= sizeof(line) ||
		    write_all(descriptor, line, (size_t)length) != 0)
			goto fail;
	}

	/* Handles a failed fsync operation. */
	NCOM_TRACE("resolver-fsync-enter", 0);
	if (fsync(descriptor) != 0) {
		descriptor = -1;
		goto fail;
	}
	NCOM_TRACE("resolver-close-enter", 0);
	if (close(descriptor) != 0) {
		descriptor = -1;
		goto fail;
	}
	descriptor = -1;
	NCOM_TRACE("resolver-close-done", 0);

	/* Handles a failed rename operation. */
	NCOM_TRACE("resolver-rename-enter", 0);
	if (rename(temporary, "/etc/resolv.conf") != 0)
		goto fail;
	NCOM_TRACE("resolver-rename-done", 0);

	/* Reports successful completion. */
	return 0;

fail:

	/* Checks the file descriptor. */
	if (descriptor >= 0)
		close(descriptor);
	unlink(temporary);

	/* Reports operation failure. */
	return -1;
}

/* Supports the handle signal operation. */
static void
handle_signal(
	int signal_number)
{
	(void)signal_number;
	stopping = 1;
}

/* Supports the ignore signal operation. */
static void
ignore_signal(
	int signal_number)
{
	(void)signal_number;
}

/*
 * Carries out one wired interface's configuration (ws089-p022), from a
 * member of the network group or root: its fields checked, the interface
 * an existing wired one, then its entry in net.conf written atomically
 * under the writer's lock (nothing else of the file changes but the
 * default route and the name servers it names), the wired policy told,
 * and the configuration applied now.  Every outcome is logged.
 */
static void
handle_lan_configure(
	int client,
	struct networkd_request *request,
	const struct kern_peercred *peer)
{
	struct networkd_lan_configure configure;
	char diagnostic[CHILD_OUTPUT_MAX];
	char reason[160];
	unsigned long euid;
	unsigned index;
	const char *mode;
	int fixed;
	int result;
	int error;

	/* The request's fields as a configuration. */
	memset(&configure, 0, sizeof(configure));
	configure.interface = request->interface;
	configure.address = request->address;
	configure.netmask = request->netmask;
	configure.gateway = request->gateway;
	configure.dns_count = request->dns_count;
	for (index = 0U; index < request->dns_count && index < NETWORKD_LAN_CONFIGURE_DNS_MAX; index++)
		configure.dns[index] = request->dns[index];
	diagnostic[0] = '\0';
	reason[0] = '\0';

	/* Checked strictly. */
	error = 0;
	result = networkd_lan_configure_check(&configure, reason, sizeof(reason));
	if (result != 0)
		error = EINVAL;

	/* An existing wired interface, while no confirmed transaction owns the wired configuration. */
	if (error == 0)
		error = lan_configure_target(request, reason, sizeof(reason));

	/* Its entry in net.conf. */
	if (error == 0)
		error = lan_configure_save(&configure, reason, sizeof(reason));

	/* The wired policy follows, and the configuration is applied now. */
	if (error == 0) {
		lan_configure_policy(request, &configure);
		error = lan_configure_apply(request, &configure, diagnostic);
		if (error != 0)
			(void)snprintf(reason, sizeof(reason), "%s", "saved, but could not be applied now");
	}

	/* Who asked, and how. */
	euid = 0UL;
	if (peer != NULL)
		euid = (unsigned long)peer->euid;
	fixed = networkd_lan_configure_static(&configure);
	mode = "dhcp";
	if (fixed)
		mode = "static";

	/* Logged, every outcome, and answered. */
	if (error == 0) {
		syslog(LOG_NOTICE, "LAN_CONFIGURE interface=%s mode=%s address=%s netmask=%s router=%s dns=%u euid=%lu result=ok",
		    request->interface, mode, request->address, request->netmask, request->gateway, request->dns_count, euid);
		send_response(client, request->header.request_id, request->header.opcode, NETWORKD_RESULT_OK, 0, NULL, NULL, 0U);
	} else {
		syslog(LOG_WARNING, "LAN_CONFIGURE interface=%.15s euid=%lu result=error errno=%d reason=%s",
		    request->interface, euid, error, reason);
		send_response(client, request->header.request_id, request->header.opcode, NETWORKD_RESULT_ERROR, error, reason, NULL, 0U);
	}

	/* Nothing a child printed stays. */
	networkd_protocol_clear(diagnostic, sizeof(diagnostic));
}

/*
 * Checks that a configuration's interface is an existing wired one (not
 * the loopback, not a radio) and that no confirmed transaction owns the
 * wired configuration.  Returns 0, ENODEV or EBUSY with the reason.
 */
static int
lan_configure_target(
	const struct networkd_request *request,
	char *reason,
	size_t capacity)
{
	uint32_t ifindex;
	int descriptor;
	int flags;
	int radio;
	int status;
	int active;

	/* Its flags and index; the loopback is no cable. */
	flags = 0;
	ifindex = 0U;
	(void)snprintf(reason, capacity, "%s", "no such wired interface");
	status = interface_flags(request->interface, &flags);
	if (status != 0)
		return ENODEV;
	status = interface_index(request->interface, &ifindex);
	if (status != 0 || (flags & IFF_LOOPBACK) != 0)
		return ENODEV;

	/* Not a radio. */
	descriptor = socket(AF_INET, SOCK_DGRAM, 0);
	if (descriptor < 0)
		return ENODEV;
	radio = lan_interface_is_radio(descriptor, request->interface, ifindex);
	(void)close(descriptor);
	if (radio)
		return ENODEV;

	/* No confirmed transaction under way. */
	active = networkd_confirmed_active(&confirmed);
	if (active) {
		(void)snprintf(reason, capacity, "%s", "confirmed transaction");
		return EBUSY;
	}

	/* Succeeded: the interface may be configured. */
	reason[0] = '\0';
	return 0;
}

/*
 * Writes a configuration's entry in net.conf: read under the writer's lock
 * (a missing file is an empty configuration), edited for the interface,
 * and saved atomically.  Returns 0 or an errno value with the reason.
 */
static int
lan_configure_save(
	const struct networkd_lan_configure *configure,
	char *reason,
	size_t capacity)
{
	struct netconf *configuration;
	int writer;
	int result;
	int error;

	/* Room for the configuration. */
	configuration = calloc(1, sizeof(*configuration));
	if (configuration == NULL) {
		(void)snprintf(reason, capacity, "%s", "no memory");
		return ENOMEM;
	}

	/* The writer's lock, as the net command takes it. */
	writer = netconf_writer_lock(reason, capacity);
	if (writer < 0) {
		error = errno;
		free(configuration);
		if (error == 0)
			error = EBUSY;
		return error;
	}

	/* The file, or an empty configuration when there is none. */
	result = netconf_load(NETCONF_PATH, configuration, reason, capacity);
	error = errno;
	if (result != 0 && error == ENOENT) {
		memset(configuration, 0, sizeof(*configuration));
		configuration->version = 1;
		configuration->dns_mode = NETCONF_DNS_DHCP;
		result = 0;
	}

	/* Edited for the interface. */
	if (result == 0) {
		result = networkd_lan_configure_edit(configuration, configure, reason, capacity);
		error = errno;
	}

	/* Saved atomically. */
	if (result == 0) {
		result = netconf_save_atomic_locked(NETCONF_PATH, configuration, reason, capacity);
		error = errno;
	}

	/* The lock and the configuration go. */
	(void)netconf_writer_unlock(writer);
	free(configuration);

	/* Reports how it went. */
	if (result != 0) {
		if (error == 0)
			error = EIO;
		return error;
	}

	/* Succeeded: net.conf holds the configuration. */
	return 0;
}

/* Tells the wired policy what the configuration asks of its interface. */
static void
lan_configure_policy(
	const struct networkd_request *request,
	const struct networkd_lan_configure *configure)
{
	struct networkd_lan_policy policy;
	int fixed;

	/* DHCP with the default timeout, or the static address. */
	memset(&policy, 0, sizeof(policy));
	(void)snprintf(policy.interface, sizeof(policy.interface), "%s", request->interface);
	policy.mode = NETWORKD_LAN_MODE_DHCP;
	policy.dhcp_timeout = 10U;
	fixed = networkd_lan_configure_static(configure);
	if (fixed) {
		policy.mode = NETWORKD_LAN_MODE_STATIC;
		policy.dhcp_timeout = 0U;
		(void)snprintf(policy.address, sizeof(policy.address), "%s", request->address);
		(void)snprintf(policy.netmask, sizeof(policy.netmask), "%s", request->netmask);
	}

	/* The policy's record of the interface. */
	(void)networkd_lan_set_interface(&managed_lan, &policy);
}

/*
 * Applies a wired interface's configuration now: through the wired policy
 * while it manages the interfaces (its worker configures the address in
 * the background), else the address directly; the default route replaced
 * by a static one's router, and the name servers written when named.
 * Returns 0 or an errno value.
 */
static int
lan_configure_apply(
	struct networkd_request *request,
	const struct networkd_lan_configure *configure,
	char diagnostic[CHILD_OUTPUT_MAX])
{
	char *arguments[8];
	char seconds[16];
	unsigned index;
	int present;
	int result;
	int fixed;

	/* The address: the policy's worker, or now. */
	result = 0;
	fixed = networkd_lan_configure_static(configure);
	if (managed_lan.enabled) {
		lan_work_due = 1;
	} else if (fixed) {
		arguments[0] = "/sbin/ifconfig";
		arguments[1] = request->interface;
		arguments[2] = "inet";
		arguments[3] = request->address;
		arguments[4] = "netmask";
		arguments[5] = request->netmask;
		arguments[6] = NULL;
		result = run_command_until(arguments, 10, 0U, diagnostic);
	} else {
		(void)snprintf(seconds, sizeof(seconds), "%u", 10U);
		arguments[0] = "/sbin/dhcpc";
		arguments[1] = "-t";
		arguments[2] = seconds;
		arguments[3] = request->interface;
		arguments[4] = NULL;
		result = run_command_until(arguments, 15, 0U, diagnostic);
	}

	/* An address that could not be given fails the rest. */
	if (result != 0)
		return EIO;

	/* A static address's router replaces the default route (without one, another interface's stays). */
	present = 0;
	if (fixed && request->gateway[0] != '\0')
		present = default_route_exists();
	if (present > 0) {
		arguments[0] = "/sbin/route";
		arguments[1] = "delete";
		arguments[2] = "default";
		arguments[3] = NULL;
		(void)run_command_until(arguments, 10, 0U, diagnostic);
	}

	/* The router comes. */
	if (fixed && request->gateway[0] != '\0') {
		arguments[0] = "/sbin/route";
		arguments[1] = "add";
		arguments[2] = "default";
		arguments[3] = request->gateway;
		arguments[4] = NULL;
		result = run_command_until(arguments, 10, 0U, diagnostic);
		if (result != 0)
			return EIO;
	}

	/* The name servers named, kept over the leases' from now on (none named: the leases' again). */
	lan_static_dns_count = 0U;
	for (index = 0U; index < request->dns_count && index < NETWORKD_LAN_CONFIGURE_DNS_MAX; index++) {
		(void)snprintf(lan_static_dns[index], sizeof(lan_static_dns[index]), "%s", request->dns[index]);
		lan_static_dns_count++;
	}

	/* Written now. */
	if (lan_static_dns_count != 0U) {
		arguments[0] = lan_static_dns[0];
		arguments[1] = NULL;
		if (lan_static_dns_count > 1U)
			arguments[1] = lan_static_dns[1];
		result = write_resolver(arguments, (int)lan_static_dns_count);
		if (result != 0)
			return EIO;
	}

	/* Succeeded: the configuration is in effect (or on its way, through the policy). */
	return 0;
}
