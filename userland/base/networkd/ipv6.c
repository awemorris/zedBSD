/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * networkd's IPv6 (ws130-p006, plan/ws130/phase001/phase.md section 4).
 *
 * When a wired interface comes up (or is up when networkd starts), its
 * IPv6 is turned on or off as net.conf says (on by default, H3); when on,
 * it gets its link-local address with a stable interface identifier
 * (RFC 7217, H4: no MAC address shown) and the static addresses of its
 * "ipv6:" section.  The kernel then does duplicate address detection and
 * solicits a router.  Each Router Advertisement the kernel publishes
 * (RTM_ROUTERADV) gives, as net.conf allows: for each autonomous /64
 * prefix a stable address (RFC 7217, or EUI-64 with stable-address false)
 * and a temporary one (RFC 8981), with the prefix's lifetimes; the router
 * as the default route for its lifetime; and its DNS servers (RDNSS) in
 * resolv.conf after the IPv4 ones (H5).  The kernel removes what runs out.
 *
 * ws177-p045: an address whose duplicate address detection failed is made
 * again (a stable one with the next DAD counter, a temporary one with
 * another random identifier, three times at most); a temporary address
 * lives from when it was made, and the next is made before it stops being
 * preferred; the default route is the router of the preferred interface
 * only (wired before Wi-Fi), and an interface that loses its carrier loses
 * its SLAAC addresses, its routers and its DNS servers; the RDNSS servers
 * are put back into resolv.conf whenever another writer (dhcpc, a DNS
 * command) rewrote it, keeping its first comment line; and `dhcpc -6` runs
 * as a child that the loop waits for without stopping.
 */

#include "userland/base/networkd/ipv6.h"
#include "userland/base/networkd/resolver6.h"
#include "userland/base/networkd/slaac.h"
#include "userland/base/net/netconf.h"
#include "userland/base/net/netutil.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <net/route.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* The secret of the stable identifiers, and where it is kept (made the first time, 0600; /var/db too, which images lack). */
#define IPV6_STATE_PARENT	"/var/db"
#define IPV6_SECRET_DIRECTORY	"/var/db/networkd"
#define IPV6_SECRET_PATH	"/var/db/networkd/ipv6-secret"
#define IPV6_SECRET_LENGTH	32U

/* How many prefixes (an interface's each, the link-local one too) networkd keeps the identifiers and counters of. */
#define IPV6_PREFIXES		24U

/* How many routers, RDNSS servers and search lists networkd keeps. */
#define IPV6_ROUTERS		8U
#define IPV6_DNS_ENTRIES	8U
#define IPV6_SEARCHES		4U

/* The resolver's file, its temporary twin, and the most of it read. */
#define IPV6_RESOLV_PATH	"/etc/resolv.conf"
#define IPV6_RESOLV_TEMPORARY	"/etc/resolv.conf.ipv6"
#define IPV6_RESOLV_SIZE	8192U

/* A microsecond's, a millisecond's and a second's worth of the monotonic clock. */
#define IPV6_US_PER_MS		1000U
#define IPV6_US_PER_SECOND	1000000U

/*
 * DHCPv6 (ws130-p007): how many interfaces' runs networkd keeps, dhcpc's
 * time and the most a run may take, the record dhcpc leaves (when to run
 * again), and the waits after a run that failed (doubled up to the last).
 * ws177-p045: while dhcpc runs, the loop looks at it every 200 ms; past
 * its time it is sent SIGTERM, and SIGKILL 2 seconds later.
 */
#define IPV6_DHCP_INTERFACES	8U
#define IPV6_DHCP_TIMEOUT	"10"
#define IPV6_DHCP_RUN_SECONDS	15U
#define IPV6_DHCP_KILL_SECONDS	2U
#define IPV6_DHCP_POLL_MS	200
#define IPV6_DHCP_RECORD	"/var/db/dhcpc/%s.dhcp6"
#define IPV6_DHCP_RETRY_FIRST	60U
#define IPV6_DHCP_RETRY_LAST	3600U

/*
 * A prefix of an interface (fe80:: for its link-local address): the DAD
 * counter of its stable identifier (RFC 7217), the lifetimes its last
 * advertisement gave and when (monotonic microseconds; 0 for none), and
 * its temporary identifier (RFC 8981): when it was made, its
 * desynchronization, and how many were made again after a duplicate.
 */
struct ipv6_prefix_state {
	int used;
	unsigned ifindex;
	struct in6_addr prefix;
	unsigned dad_counter;
	uint32_t valid;
	uint32_t preferred;
	uint64_t seen;
	int temporary;
	uint8_t iid[8];
	uint64_t created;
	uint32_t desync;
	unsigned temporary_retries;
};

/* An interface's RDNSS server and when its lifetime ends (monotonic microseconds). */
struct ipv6_dns {
	int used;
	unsigned ifindex;
	char text[RESOLVER6_SERVER_TEXT];
	uint64_t expires;
};

/* An interface's search list (DNSSL) and when its lifetime ends. */
struct ipv6_search {
	int used;
	unsigned ifindex;
	char text[SLAAC_SEARCH_MAX];
	uint64_t expires;
};

/* What networkd last saw of resolv.conf: whether it was there, and which file it was. */
struct ipv6_file_identity {
	int known;
	int present;
	dev_t device;
	ino_t inode;
	off_t size;
	struct timespec modified;
};

/* What DHCPv6 an interface's advertisements ask for. */
enum ipv6_dhcp_mode {
	IPV6_DHCP_NONE,
	IPV6_DHCP_STATELESS,
	IPV6_DHCP_STATEFUL
};

/*
 * An interface's DHCPv6: its mode, whether dhcpc writes the resolver,
 * when it runs again (monotonic microseconds; 0: not again), and the wait
 * after the last failure; and the dhcpc that runs (its process, when it
 * started, when it was told to stop, and whether to run again when it
 * ends), and whether the next run declines the leased address.
 */
struct ipv6_dhcp {
	int used;
	char name[IF_NAMESIZE];
	enum ipv6_dhcp_mode mode;
	int resolver;
	uint64_t due;
	unsigned retry;
	pid_t child;
	uint64_t started;
	uint64_t terminated;
	int again;
	int decline;
};

static uint8_t ipv6_secret[IPV6_SECRET_LENGTH];
static int ipv6_secret_ready;
static struct ipv6_prefix_state ipv6_prefixes[IPV6_PREFIXES];
static struct slaac_router ipv6_routers[IPV6_ROUTERS];
static int ipv6_route_set;
static unsigned ipv6_route_ifindex;
static struct in6_addr ipv6_route_router;
static struct ipv6_dns ipv6_dns_entries[IPV6_DNS_ENTRIES];
static struct ipv6_search ipv6_searches[IPV6_SEARCHES];
static struct resolver6_servers ipv6_dns_owned;
static struct ipv6_file_identity ipv6_resolv_seen;
static char ipv6_resolv_current[IPV6_RESOLV_SIZE];
static char ipv6_resolv_output[IPV6_RESOLV_SIZE + RESOLVER6_SERVERS * (RESOLVER6_SERVER_TEXT + 12U) + SLAAC_SEARCH_MAX + 64U];
static struct netconf ipv6_configuration;
static struct ipv6_dhcp ipv6_dhcps[IPV6_DHCP_INTERFACES];

static int ipv6_secret_load(void);
static void ipv6_config(const char *name, struct netconf_interface *item, int *dns_dynamic);
static int ipv6_request(unsigned long command, void *argument);
static int ipv6_add(const char *name, const struct in6_addr *address, unsigned prefix, unsigned flags, uint32_t valid, uint32_t preferred);
static void ipv6_remove(const char *name, const struct in6_addr *address);
static void ipv6_setup(const char *name);
static void ipv6_carrier_down(unsigned ifindex, const char *name);
static void ipv6_duplicate(const struct rtm_addrinfo *record);
static void ipv6_advertisement(const struct rtm_routeradv *record, const uint8_t *message);
static void ipv6_prefix(const char *name, unsigned ifindex, const struct netconf_interface *item, const struct slaac_prefix *prefix);
static int ipv6_eui64(const char *name, uint8_t *iid);
static struct ipv6_prefix_state *ipv6_prefix_find(unsigned ifindex, const struct in6_addr *prefix, int make);
static void ipv6_prefix_left(const struct ipv6_prefix_state *state, uint64_t now, uint32_t *valid, uint32_t *preferred);
static void ipv6_stable_add(const char *name, const struct ipv6_prefix_state *state, uint64_t now);
static int ipv6_temporary_make(struct ipv6_prefix_state *state, uint64_t now);
static void ipv6_temporary_add(const char *name, const struct ipv6_prefix_state *state, uint64_t now);
static void ipv6_temporary_due(uint64_t now);
static void ipv6_router(unsigned ifindex, const struct in6_addr *router, uint32_t lifetime);
static void ipv6_routes_apply(int refreshed);
static void ipv6_route_put(unsigned ifindex, const struct in6_addr *router, uint64_t expires, uint64_t now);
static void ipv6_route_delete(unsigned ifindex);
static void ipv6_dns_advertised(unsigned ifindex, const char *name, const struct slaac_ra *ra);
static int ipv6_dns_forget_servers(unsigned ifindex);
static int ipv6_dns_forget(unsigned ifindex);
static void ipv6_dns_wanted(struct resolver6_servers *wanted, char *search, size_t capacity);
static void ipv6_resolver_sync(int changed);
static int ipv6_resolver_write(const char *text);
static void ipv6_resolver_look(struct ipv6_file_identity *identity);
static void ipv6_dhcp_advertised(const char *name, const struct netconf_interface *item, const struct slaac_ra *ra, int dns_dynamic);
static struct ipv6_dhcp *ipv6_dhcp_find(const char *name, int make);
static void ipv6_dhcp_forget(struct ipv6_dhcp *entry);
static void ipv6_dhcp_run(struct ipv6_dhcp *entry);
static void ipv6_dhcp_wait(struct ipv6_dhcp *entry, uint64_t now);
static void ipv6_dhcp_done(struct ipv6_dhcp *entry, int succeeded);
static unsigned ipv6_dhcp_renew(const char *name);
static void ipv6_soonest(uint64_t *soonest, uint64_t when);

/*
 * Opens the route socket of IPv6's events (RTM_ROUTERADV, RTM_ADDRINFO,
 * and RTM_IFINFO).  Returns the descriptor, or -1 with errno set.
 */
int
networkd_ipv6_open(void)
{
	int descriptor;

	/* Not blocking, closed across exec. */
	descriptor = socket(PF_ROUTE, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, AF_INET6);
	return descriptor;
}

/*
 * Reads every record waiting on the IPv6 route socket and acts on it: an
 * interface's arrival or carrier sets its IPv6 up and its loss of the
 * carrier takes away what the network gave, an address found duplicated
 * is made again, an advertisement is applied.  Returns 0, or -1 when the
 * socket failed.
 */
int
networkd_ipv6_events(
	int descriptor)
{
	static uint8_t record[sizeof(struct rtm_routeradv) + RTM_ROUTERADV_MESSAGE_MAX];
	const struct rtm_header *header;
	const struct rtm_ifinfo *info;
	const struct rtm_routeradv *advert;
	const struct rtm_addrinfo *address;
	char name[IF_NAMESIZE];
	const char *named;
	ssize_t count;

	/* Each record until none waits. */
	for (;;) {
		count = read(descriptor, record, sizeof(record));
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		if (count <= 0)
			return -1;
		if ((size_t)count < sizeof(*header))
			continue;
		header = (const struct rtm_header *)record;

		/* An interface that arrived or got its carrier: its IPv6 set up; one that lost it or went: what the network gave goes. */
		if (header->rtm_type == RTM_IFINFO && (size_t)count >= sizeof(*info)) {
			info = (const struct rtm_ifinfo *)record;
			named = if_indextoname(info->rtm_ifindex, name);
			if (info->rtm_transition == RTM_IFINFO_CARRIER_UP || info->rtm_transition == RTM_IFINFO_ARRIVAL) {
				if (named != NULL)
					ipv6_setup(name);
			} else if (info->rtm_transition == RTM_IFINFO_CARRIER_DOWN ||
			    info->rtm_transition == RTM_IFINFO_REMOVAL) {
				ipv6_carrier_down(info->rtm_ifindex, named);
			}

			/* Done with it. */
			continue;
		}

		/* An address another node has (duplicate address detection failed). */
		if (header->rtm_type == RTM_ADDRINFO && (size_t)count >= sizeof(*address)) {
			address = (const struct rtm_addrinfo *)record;
			if (address->rtm_transition == RTM_ADDRINFO_DUPLICATE)
				ipv6_duplicate(address);
			continue;
		}

		/* A router's advertisement. */
		if (header->rtm_type == RTM_ROUTERADV && (size_t)count >= sizeof(*advert)) {
			advert = (const struct rtm_routeradv *)record;
			if (sizeof(*advert) + advert->rtm_message_length <= (size_t)count)
				ipv6_advertisement(advert, record + sizeof(*advert));
		}
	}
}

/*
 * Sets up the IPv6 of each wired interface that is up when networkd
 * starts (the loopback keeps ::1, which the kernel gives it).
 */
void
networkd_ipv6_start(void)
{
	struct ifreq *interfaces;
	struct ifreq flags;
	unsigned count;
	unsigned index;
	int descriptor;
	int status;

	/* The interfaces. */
	descriptor = socket(AF_INET, SOCK_DGRAM, 0);
	if (descriptor < 0)
		return;
	status = netutil_interfaces(descriptor, &interfaces, &count);
	if (status != 0) {
		close(descriptor);
		return;
	}

	/* Each one up, not the loopback. */
	for (index = 0; index < count; index++) {
		status = netutil_ifreq(&flags, interfaces[index].ifr_name);
		if (status == 0)
			status = ioctl(descriptor, SIOCGIFFLAGS, &flags);
		if (status != 0)
			continue;
		if ((flags.ifr_flags & IFF_UP) == 0 || (flags.ifr_flags & IFF_LOOPBACK) != 0)
			continue;
		ipv6_setup(interfaces[index].ifr_name);
	}

	/* The list and the socket let go. */
	free(interfaces);
	close(descriptor);
}

/*
 * Forgets an interface's DHCPv6 (its lease was given back as it went
 * down or its IPv6 off: no Renew at T1); the next advertisement starts it
 * again.
 */
void
networkd_ipv6_forget(
	const char *name)
{
	struct ipv6_dhcp *entry;

	/* Its entry, when it has one. */
	entry = ipv6_dhcp_find(name, 0);
	if (entry != NULL)
		ipv6_dhcp_forget(entry);
}

/*
 * Gives an interface whose IPv6 is on its link-local address: fe80::/64
 * with the stable identifier (RFC 7217) and the DAD counter its duplicates
 * raised.  networkd does it when the interface comes up, and when a commit
 * turns IPv6 on again (the kernel says nothing of that).  Returns 0, or -1
 * without the secret.
 */
int
networkd_ipv6_link_local(
	const char *name)
{
	struct ipv6_prefix_state *state;
	struct in6_addr link;
	struct in6_addr address;
	uint8_t iid[8];
	unsigned counter;
	unsigned ifindex;
	int status;

	/* The secret of the stable identifiers. */
	status = ipv6_secret_load();
	if (status != 0)
		return -1;

	/* The counter of fe80::/64 on the interface; past the last try there is none to make. */
	memset(&link, 0, sizeof(link));
	link.s6_addr[0] = 0xfe;
	link.s6_addr[1] = 0x80;
	ifindex = if_nametoindex(name);
	state = ipv6_prefix_find(ifindex, &link, 1);
	counter = 0;
	if (state != NULL)
		counter = state->dad_counter;
	if (counter > SLAAC_IDGEN_RETRIES)
		return 0;

	/* Succeeded: fe80::/64 and the identifier; an address it has already is renewed. */
	slaac_stable_iid(ipv6_secret, sizeof(ipv6_secret), &link, name, "", counter, iid);
	slaac_address(&link, iid, &address);
	(void)ipv6_add(name, &address, 64U, 0U, IN6_LIFETIME_INFINITE, IN6_LIFETIME_INFINITE);
	return 0;
}

/*
 * Gives the milliseconds until IPv6 has work due (a DHCPv6 run, a dhcpc
 * to look at, a temporary address to make, a router, a DNS server or a
 * search list whose lifetime ends), or -1 when none is.
 */
int
networkd_ipv6_poll_timeout(void)
{
	uint64_t regenerate;
	uint64_t soonest;
	uint64_t now;
	uint64_t wait;
	unsigned index;

	/* The DHCPv6 runs, and each dhcpc running. */
	soonest = 0;
	now = netutil_monotonic_us();
	for (index = 0; index < IPV6_DHCP_INTERFACES; index++) {
		if (!ipv6_dhcps[index].used)
			continue;
		if (ipv6_dhcps[index].child > 0)
			ipv6_soonest(&soonest, now + (uint64_t)IPV6_DHCP_POLL_MS * IPV6_US_PER_MS);
		else if (ipv6_dhcps[index].due != 0U)
			ipv6_soonest(&soonest, ipv6_dhcps[index].due);
	}

	/* The temporary addresses to make again. */
	for (index = 0; index < IPV6_PREFIXES; index++) {
		if (!ipv6_prefixes[index].used || !ipv6_prefixes[index].temporary)
			continue;
		regenerate = slaac_temporary_regenerate(ipv6_prefixes[index].desync);
		ipv6_soonest(&soonest, ipv6_prefixes[index].created + regenerate * IPV6_US_PER_SECOND);
	}

	/* The routers whose lifetimes end. */
	for (index = 0; index < IPV6_ROUTERS; index++) {
		if (ipv6_routers[index].used)
			ipv6_soonest(&soonest, ipv6_routers[index].expires);
	}

	/* The DNS servers whose lifetimes end. */
	for (index = 0; index < IPV6_DNS_ENTRIES; index++) {
		if (ipv6_dns_entries[index].used)
			ipv6_soonest(&soonest, ipv6_dns_entries[index].expires);
	}

	/* The search lists whose lifetimes end. */
	for (index = 0; index < IPV6_SEARCHES; index++) {
		if (ipv6_searches[index].used)
			ipv6_soonest(&soonest, ipv6_searches[index].expires);
	}

	/* Nothing due. */
	if (soonest == 0U)
		return -1;

	/* Succeeded: from now, rounded up, at most a day at a time. */
	if (soonest <= now)
		return 0;
	wait = (soonest - now + IPV6_US_PER_MS - 1U) / IPV6_US_PER_MS;
	if (wait > 86400000U)
		wait = 86400000U;
	return (int)wait;
}

/*
 * Does IPv6's work that is due: each dhcpc that ended (or ran too long),
 * each DHCPv6 run due (a Renew, or the information again), the temporary
 * addresses to make again, the routers, DNS servers and search lists whose
 * lifetimes ended, and the RDNSS servers put back into resolv.conf when
 * another writer rewrote it.  The loop calls it every turn.
 */
void
networkd_ipv6_run_due(void)
{
	struct ipv6_dhcp *entry;
	uint64_t now;
	unsigned index;
	int changed;
	int expired;

	/* Each dhcpc running, then each run due. */
	now = netutil_monotonic_us();
	for (index = 0; index < IPV6_DHCP_INTERFACES; index++) {
		entry = &ipv6_dhcps[index];
		if (!entry->used)
			continue;
		if (entry->child > 0)
			ipv6_dhcp_wait(entry, now);
		if (!entry->used || entry->child > 0 || entry->due == 0U || entry->due > now)
			continue;
		entry->due = 0;
		ipv6_dhcp_run(entry);
	}

	/* The temporary addresses. */
	ipv6_temporary_due(now);

	/* The routers whose lifetimes ended: another chosen. */
	expired = 0;
	for (index = 0; index < IPV6_ROUTERS; index++) {
		if (!ipv6_routers[index].used || ipv6_routers[index].expires > now)
			continue;
		ipv6_routers[index].used = 0;
		expired = 1;
	}

	/* Another router chosen when one went. */
	if (expired)
		ipv6_routes_apply(-1);

	/* The DNS servers whose lifetimes ended. */
	changed = 0;
	for (index = 0; index < IPV6_DNS_ENTRIES; index++) {
		if (!ipv6_dns_entries[index].used || ipv6_dns_entries[index].expires > now)
			continue;
		ipv6_dns_entries[index].used = 0;
		changed = 1;
	}

	/* The search lists whose lifetimes ended. */
	for (index = 0; index < IPV6_SEARCHES; index++) {
		if (!ipv6_searches[index].used || ipv6_searches[index].expires > now)
			continue;
		ipv6_searches[index].used = 0;
		changed = 1;
	}

	/* resolv.conf as networkd wants it. */
	ipv6_resolver_sync(changed);
}

/* Reads the secret of the stable identifiers, making and keeping it the first time; 0, or -1. */
static int
ipv6_secret_load(void)
{
	ssize_t count;
	int descriptor;
	int status;

	/* Read once. */
	if (ipv6_secret_ready)
		return 0;

	/* The kept one. */
	descriptor = open(IPV6_SECRET_PATH, O_RDONLY | O_CLOEXEC);
	if (descriptor >= 0) {
		count = read(descriptor, ipv6_secret, sizeof(ipv6_secret));
		close(descriptor);
		if (count != (ssize_t)sizeof(ipv6_secret))
			return -1;
		ipv6_secret_ready = 1;
		return 0;
	}

	/* A new one, kept for the next boot. */
	status = getentropy(ipv6_secret, sizeof(ipv6_secret));
	if (status != 0)
		return -1;
	(void)mkdir(IPV6_STATE_PARENT, 0755);
	(void)mkdir(IPV6_SECRET_DIRECTORY, 0700);
	descriptor = open(IPV6_SECRET_PATH, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (descriptor >= 0) {
		count = write(descriptor, ipv6_secret, sizeof(ipv6_secret));
		(void)fsync(descriptor);
		close(descriptor);
		if (count != (ssize_t)sizeof(ipv6_secret))
			fprintf(stderr, "networkd: %s: cannot keep the IPv6 secret\n", IPV6_SECRET_PATH);
	}

	/* Succeeded: this run uses it in any case. */
	ipv6_secret_ready = 1;
	return 0;
}

/*
 * Finds an interface's entry of net.conf (the defaults when net.conf or the
 * entry is missing) and whether the DNS servers are the network's (dhcp or
 * merge mode) rather than static.
 */
static void
ipv6_config(
	const char *name,
	struct netconf_interface *item,
	int *dns_dynamic)
{
	char error[128];
	size_t index;
	int status;
	int same;

	/* The defaults. */
	memset(item, 0, sizeof(*item));
	*dns_dynamic = 1;

	/* net.conf's entry. */
	status = netconf_load(NETCONF_PATH, &ipv6_configuration, error, sizeof(error));
	if (status != 0)
		return;
	*dns_dynamic = ipv6_configuration.dns_mode != NETCONF_DNS_STATIC;
	for (index = 0; index < ipv6_configuration.interface_count; index++) {
		same = strcmp(ipv6_configuration.interfaces[index].name, name);
		if (same == 0) {
			*item = ipv6_configuration.interfaces[index];
			return;
		}
	}
}

/* Makes one IPv6 ioctl on a socket of its own; 0, or -1 with errno set. */
static int
ipv6_request(
	unsigned long command,
	void *argument)
{
	int descriptor;
	int status;
	int saved;

	/* The socket, the request, and the socket closed. */
	descriptor = socket(AF_INET6, SOCK_DGRAM, 0);
	if (descriptor < 0)
		return -1;
	status = ioctl(descriptor, command, argument);
	saved = errno;
	close(descriptor);
	errno = saved;
	return status;
}

/* Adds (or renews) an address of an interface with its flags and lifetimes. */
static int
ipv6_add(
	const char *name,
	const struct in6_addr *address,
	unsigned prefix,
	unsigned flags,
	uint32_t valid,
	uint32_t preferred)
{
	struct in6_aliasreq request;
	char text[INET6_ADDRSTRLEN];
	const char *written;
	int status;

	/* The request. */
	memset(&request, 0, sizeof(request));
	(void)snprintf(request.ifra_name, sizeof(request.ifra_name), "%s", name);
	request.ifra_addr.sin6_family = AF_INET6;
	request.ifra_addr.sin6_addr = *address;
	request.ifra_prefixlen = prefix;
	request.ifra_flags = flags;
	request.ifra_valid = valid;
	request.ifra_preferred = preferred;
	status = ipv6_request(SIOCAIFADDR_IN6, &request);

	/* A failure is said in the log; the next event tries again. */
	if (status != 0) {
		written = inet_ntop(AF_INET6, address, text, sizeof(text));
		if (written == NULL)
			(void)snprintf(text, sizeof(text), "?");
		fprintf(stderr, "networkd: %s: IPv6 address %s/%u: %s\n", name, text, prefix, strerror(errno));
	}

	/* The kernel's answer. */
	return status;
}

/* Takes an address off an interface (one already gone is the same). */
static void
ipv6_remove(
	const char *name,
	const struct in6_addr *address)
{
	struct in6_aliasreq request;

	/* The interface and the address. */
	memset(&request, 0, sizeof(request));
	(void)snprintf(request.ifra_name, sizeof(request.ifra_name), "%s", name);
	request.ifra_addr.sin6_family = AF_INET6;
	request.ifra_addr.sin6_addr = *address;
	(void)ipv6_request(SIOCDIFADDR_IN6, &request);
}

/* Sets an interface's IPv6 up: on or off as net.conf says, and when on its link-local and static addresses. */
static void
ipv6_setup(
	const char *name)
{
	struct netconf_interface item;
	struct ifreq request;
	struct in6_addr address;
	size_t index;
	int dns_dynamic;
	int on;
	struct ipv6_dhcp *entry;
	int status;

	/* DHCPv6 starts again with the next advertisement. */
	entry = ipv6_dhcp_find(name, 0);
	if (entry != NULL)
		ipv6_dhcp_forget(entry);

	/* On or off. */
	ipv6_config(name, &item, &dns_dynamic);
	on = netconf_ipv6_enabled(&item);
	status = netutil_ifreq(&request, name);
	if (status != 0)
		return;
	request.ifr_flags = on;
	status = ipv6_request(SIOCSIFINET6, &request);
	if (status != 0 || !on)
		return;

	/* The link-local address. */
	status = networkd_ipv6_link_local(name);
	if (status != 0)
		return;

	/* The static addresses. */
	for (index = 0; index < item.ipv6.address_count; index++) {
		status = inet_pton(AF_INET6, item.ipv6.addresses[index].address, &address);
		if (status != 1)
			continue;
		(void)ipv6_add(name, &address, item.ipv6.addresses[index].prefix_length, 0U, IN6_LIFETIME_INFINITE,
		    IN6_LIFETIME_INFINITE);
	}

	/* Said in the log. */
	printf("networkd: %s: IPv6 on\n", name);
	fflush(stdout);
}

/*
 * Takes away what the network gave an interface that lost its carrier or
 * went (RFC 4862 leaves it to the node; the next network may be another
 * one): its SLAAC addresses, its routers (another interface's router
 * becomes the default), its DNS servers and search list, and its DHCPv6
 * schedule.  The DAD counters stay while the interface does.  name is
 * NULL for an interface that went, whose addresses went with it.
 */
static void
ipv6_carrier_down(
	unsigned ifindex,
	const char *name)
{
	struct in6_ifaddrs addresses;
	struct ipv6_dhcp *entry;
	unsigned index;
	unsigned removed;
	int routers;
	int status;

	/* The SLAAC addresses (stable, EUI-64 and temporary); the link-local, static and DHCPv6 ones stay. */
	removed = 0;
	if (name != NULL) {
		memset(&addresses, 0, sizeof(addresses));
		(void)snprintf(addresses.ifa_name, sizeof(addresses.ifa_name), "%s", name);
		status = ipv6_request(SIOCGIFADDRS_IN6, &addresses);
		if (status != 0)
			addresses.ifa_count = 0;
		for (index = 0; index < addresses.ifa_count && index < IN6_IFADDRS_MAX; index++) {
			if ((addresses.ifa_list[index].ife_flags & IN6_IFF_AUTOCONF) == 0U)
				continue;
			ipv6_remove(name, &addresses.ifa_list[index].ife_addr);
			removed++;
		}
	}

	/*
	 * The prefixes' lifetimes and temporary identifiers; an entry with no
	 * counter to keep is freed, and every entry of an interface that went
	 * (its index may name another one later).
	 */
	for (index = 0; index < IPV6_PREFIXES; index++) {
		if (!ipv6_prefixes[index].used || ipv6_prefixes[index].ifindex != ifindex)
			continue;
		ipv6_prefixes[index].valid = 0;
		ipv6_prefixes[index].preferred = 0;
		ipv6_prefixes[index].seen = 0;
		ipv6_prefixes[index].temporary = 0;
		ipv6_prefixes[index].temporary_retries = 0;
		if (ipv6_prefixes[index].dad_counter == 0U || name == NULL)
			ipv6_prefixes[index].used = 0;
	}

	/* Its routers: another one chosen. */
	routers = 0;
	for (index = 0; index < IPV6_ROUTERS; index++) {
		if (!ipv6_routers[index].used || ipv6_routers[index].ifindex != ifindex)
			continue;
		ipv6_routers[index].used = 0;
		routers = 1;
	}

	/* Another router chosen when the interface had the default route. */
	if (routers || (ipv6_route_set && ipv6_route_ifindex == ifindex))
		ipv6_routes_apply(-1);

	/* Its DNS servers and search list, out of resolv.conf. */
	status = ipv6_dns_forget(ifindex);
	if (status)
		ipv6_resolver_sync(1);

	/* Its DHCPv6 starts again with the next advertisement. */
	if (name != NULL) {
		entry = ipv6_dhcp_find(name, 0);
		if (entry != NULL)
			ipv6_dhcp_forget(entry);
	}

	/* Said in the log when anything went. */
	if (name != NULL && (removed != 0U || routers)) {
		printf("networkd: %s: carrier down, %u SLAAC addresses and the routers taken away\n", name, removed);
		fflush(stdout);
	}
}

/*
 * Makes again an address of networkd's whose duplicate address detection
 * failed (RFC 4862 section 5.4.5): it is taken off, and a temporary one
 * gets another random identifier (RFC 8981 section 3.4), a stable one
 * (and the link-local one) the next DAD counter (RFC 7217 section 6),
 * three times at most.  A DHCPv6 address is declined to its server and
 * another one asked for (`dhcpc -6 -D`).  An EUI-64 address and a static
 * one are left as the kernel marked them (duplicated, not used).
 */
static void
ipv6_duplicate(
	const struct rtm_addrinfo *record)
{
	struct netconf_interface item;
	struct ipv6_prefix_state *state;
	struct ipv6_dhcp *entry;
	struct in6_addr prefix;
	char name[IF_NAMESIZE];
	char text[INET6_ADDRSTRLEN];
	const char *named;
	const char *written;
	uint8_t iid[8];
	uint64_t now;
	int dns_dynamic;
	int on_link;
	int stable;
	int status;
	int same;

	/* The interface, and the duplicate said in the log. */
	named = if_indextoname(record->rtm_ifindex, name);
	if (named == NULL)
		return;
	written = inet_ntop(AF_INET6, &record->rtm_address, text, sizeof(text));
	if (written == NULL)
		(void)snprintf(text, sizeof(text), "?");
	printf("networkd: %s: duplicate IPv6 address %s\n", name, text);
	fflush(stdout);

	/* DHCPv6's: declined, and another asked for (ws177-p046). */
	if ((record->rtm_addr_flags & IN6_IFF_DHCP) != 0U) {
		entry = ipv6_dhcp_find(name, 0);
		if (entry == NULL || entry->mode != IPV6_DHCP_STATEFUL)
			return;
		entry->decline = 1;
		ipv6_dhcp_run(entry);
		return;
	}

	/* Only the /64 addresses networkd makes: SLAAC's, and the link-local one. */
	if (record->rtm_prefixlen != 64U)
		return;
	memset(&prefix, 0, sizeof(prefix));
	memcpy(prefix.s6_addr, record->rtm_address.s6_addr, 8U);
	on_link = IN6_IS_ADDR_LINKLOCAL(&prefix);
	if ((record->rtm_addr_flags & IN6_IFF_AUTOCONF) == 0U && !on_link)
		return;
	state = ipv6_prefix_find(record->rtm_ifindex, &prefix, 0);
	if (state == NULL)
		return;
	ipv6_config(name, &item, &dns_dynamic);
	now = netutil_monotonic_us();

	/* A temporary one: another random identifier, while tries are left. */
	if ((record->rtm_addr_flags & IN6_IFF_TEMPORARY) != 0U) {
		same = memcmp(state->iid, record->rtm_address.s6_addr + 8, 8U);
		if (!state->temporary || same != 0)
			return;
		ipv6_remove(name, &record->rtm_address);
		state->temporary_retries++;
		state->temporary = 0;
		if (state->temporary_retries > SLAAC_IDGEN_RETRIES) {
			printf("networkd: %s: no temporary address for %s after %u duplicates\n", name, text,
			    SLAAC_IDGEN_RETRIES);
			fflush(stdout);
			return;
		}

		/* Another identifier, and its address. */
		status = ipv6_temporary_make(state, now);
		if (status == 0)
			ipv6_temporary_add(name, state, now);
		return;
	}

	/* A stable one made with this counter (EUI-64's identifier is the MAC address's: no other to make). */
	stable = netconf_ipv6_stable_address(&item);
	if (!on_link && !stable)
		return;
	status = ipv6_secret_load();
	if (status != 0)
		return;
	slaac_stable_iid(ipv6_secret, sizeof(ipv6_secret), &prefix, name, "", state->dad_counter, iid);
	same = memcmp(iid, record->rtm_address.s6_addr + 8, 8U);
	if (same != 0)
		return;

	/* Succeeded: off, and made again with the next counter while tries are left. */
	ipv6_remove(name, &record->rtm_address);
	state->dad_counter++;
	if (state->dad_counter > SLAAC_IDGEN_RETRIES) {
		printf("networkd: %s: no stable address for %s after %u duplicates\n", name, text, SLAAC_IDGEN_RETRIES);
		fflush(stdout);
		return;
	}

	/* The link-local one, or the prefix's. */
	if (on_link)
		(void)networkd_ipv6_link_local(name);
	else
		ipv6_stable_add(name, state, now);
}

/* Applies a Router Advertisement of an interface: its prefixes' addresses, the default route, the DNS servers. */
static void
ipv6_advertisement(
	const struct rtm_routeradv *record,
	const uint8_t *message)
{
	struct netconf_interface item;
	struct slaac_ra ra;
	char name[IF_NAMESIZE];
	const char *named;
	unsigned index;
	int dns_dynamic;
	int status;
	int on;

	/* The interface and what net.conf says of it. */
	named = if_indextoname(record->rtm_ifindex, name);
	if (named == NULL)
		return;
	ipv6_config(name, &item, &dns_dynamic);
	on = netconf_ipv6_enabled(&item);
	if (!on)
		return;

	/* The advertisement. */
	status = slaac_parse(message, record->rtm_message_length, &ra);
	if (status != 0)
		return;

	/* Each prefix's addresses. */
	on = netconf_ipv6_autoconf(&item);
	if (on) {
		for (index = 0; index < ra.prefix_count; index++)
			ipv6_prefix(name, record->rtm_ifindex, &item, &ra.prefixes[index]);
	}

	/* The router, the DNS servers, DHCPv6, and the log. */
	ipv6_router(record->rtm_ifindex, &record->rtm_source, ra.router_lifetime);
	if (dns_dynamic)
		ipv6_dns_advertised(record->rtm_ifindex, name, &ra);
	ipv6_dhcp_advertised(name, &item, &ra, dns_dynamic);
	printf("networkd: %s: router advertisement prefixes=%u router=%u dns=%u\n", name, ra.prefix_count,
	    (unsigned)ra.router_lifetime, ra.dns_count);
	fflush(stdout);
}

/*
 * Makes (or renews) a prefix's addresses as net.conf allows: the stable
 * one (with its DAD counter) or the EUI-64 one, with the prefix's
 * lifetimes; and the temporary one, with lifetimes counted from when it
 * was made (a new one once it is no longer preferred).
 */
static void
ipv6_prefix(
	const char *name,
	unsigned ifindex,
	const struct netconf_interface *item,
	const struct slaac_prefix *prefix)
{
	struct ipv6_prefix_state *state;
	struct ipv6_prefix_state fallback;
	struct in6_addr address;
	uint32_t valid;
	uint32_t preferred;
	uint64_t now;
	uint8_t iid[8];
	int status;
	int wanted;

	/* An autonomous /64 with lifetimes that make sense (RFC 4862 section 5.5.3). */
	if (!prefix->autonomous || prefix->length != 64U)
		return;
	if (prefix->valid == 0U || prefix->preferred > prefix->valid)
		return;

	/* The prefix's entry and its lifetimes (a full table: a counter of 0 and no temporary address). */
	now = netutil_monotonic_us();
	state = ipv6_prefix_find(ifindex, &prefix->prefix, 1);
	if (state == NULL) {
		memset(&fallback, 0, sizeof(fallback));
		fallback.ifindex = ifindex;
		state = &fallback;
	}

	/* The lifetimes it gives now. */
	memset(&state->prefix, 0, sizeof(state->prefix));
	memcpy(state->prefix.s6_addr, prefix->prefix.s6_addr, 8U);
	state->valid = prefix->valid;
	state->preferred = prefix->preferred;
	state->seen = now;

	/* The stable address, or the EUI-64 one. */
	wanted = netconf_ipv6_stable_address(item);
	if (wanted) {
		ipv6_stable_add(name, state, now);
	} else {
		status = ipv6_eui64(name, iid);
		if (status == 0) {
			slaac_address(&state->prefix, iid, &address);
			(void)ipv6_add(name, &address, 64U, IN6_IFF_AUTOCONF, prefix->valid, prefix->preferred);
		}
	}

	/* The temporary one, while the prefix is preferred; one no longer preferred is left to its lifetimes. */
	wanted = netconf_ipv6_temporary(item);
	if (!wanted || prefix->preferred == 0U || state == &fallback)
		return;
	if (state->temporary) {
		slaac_temporary_aged(prefix->valid, prefix->preferred, state->desync, (now - state->created) / IPV6_US_PER_SECOND,
		    &valid, &preferred);
		if (preferred == 0U)
			state->temporary = 0;
	}

	/* None (or none preferred): a new one, unless its duplicates used up the tries. */
	if (!state->temporary) {
		if (state->temporary_retries > SLAAC_IDGEN_RETRIES)
			return;
		status = ipv6_temporary_make(state, now);
		if (status != 0)
			return;
	}

	/* Succeeded: added or renewed. */
	ipv6_temporary_add(name, state, now);
}

/* Makes an interface's modified EUI-64 identifier from its MAC address (stable-address false); 0, or -1. */
static int
ipv6_eui64(
	const char *name,
	uint8_t *iid)
{
	struct ifreq request;
	int descriptor;
	int status;

	/* The MAC address. */
	status = netutil_ifreq(&request, name);
	if (status != 0)
		return -1;
	descriptor = socket(AF_INET, SOCK_DGRAM, 0);
	if (descriptor < 0)
		return -1;
	status = ioctl(descriptor, SIOCGIFHWADDR, &request);
	close(descriptor);
	if (status != 0)
		return -1;

	/* Succeeded: its halves around ff:fe, the universal bit flipped. */
	iid[0] = (uint8_t)(request.ifr_hwaddr[0] ^ 0x02U);
	iid[1] = (uint8_t)request.ifr_hwaddr[1];
	iid[2] = (uint8_t)request.ifr_hwaddr[2];
	iid[3] = 0xff;
	iid[4] = 0xfe;
	iid[5] = (uint8_t)request.ifr_hwaddr[3];
	iid[6] = (uint8_t)request.ifr_hwaddr[4];
	iid[7] = (uint8_t)request.ifr_hwaddr[5];
	return 0;
}

/* Finds an interface's prefix (its first 64 bits), taking a free entry with make; NULL when there is none. */
static struct ipv6_prefix_state *
ipv6_prefix_find(
	unsigned ifindex,
	const struct in6_addr *prefix,
	int make)
{
	struct ipv6_prefix_state *entry;
	unsigned index;
	int same;

	/* The one kept. */
	for (index = 0; index < IPV6_PREFIXES; index++) {
		entry = &ipv6_prefixes[index];
		if (!entry->used || entry->ifindex != ifindex)
			continue;
		same = memcmp(entry->prefix.s6_addr, prefix->s6_addr, 8U);
		if (same == 0)
			return entry;
	}

	/* Not kept, and none to make. */
	if (!make || ifindex == 0U)
		return NULL;

	/* A free one. */
	for (index = 0; index < IPV6_PREFIXES; index++) {
		entry = &ipv6_prefixes[index];
		if (entry->used)
			continue;
		memset(entry, 0, sizeof(*entry));
		entry->used = 1;
		entry->ifindex = ifindex;
		memcpy(entry->prefix.s6_addr, prefix->s6_addr, 8U);
		return entry;
	}

	/* The table is full. */
	return NULL;
}

/* Gives what is left now of the lifetimes a prefix's last advertisement gave (an infinite one stays). */
static void
ipv6_prefix_left(
	const struct ipv6_prefix_state *state,
	uint64_t now,
	uint32_t *valid,
	uint32_t *preferred)
{
	uint64_t elapsed;

	/* The seconds since the advertisement. */
	elapsed = 0;
	if (state->seen != 0U && now > state->seen)
		elapsed = (now - state->seen) / IPV6_US_PER_SECOND;

	/* Each lifetime less them. */
	*valid = 0;
	if (state->valid == IN6_LIFETIME_INFINITE)
		*valid = IN6_LIFETIME_INFINITE;
	else if (state->valid > elapsed)
		*valid = (uint32_t)(state->valid - elapsed);
	*preferred = 0;
	if (state->preferred == IN6_LIFETIME_INFINITE)
		*preferred = IN6_LIFETIME_INFINITE;
	else if (state->preferred > elapsed)
		*preferred = (uint32_t)(state->preferred - elapsed);
}

/* Adds (or renews) a prefix's stable address with its DAD counter and what is left of the prefix's lifetimes. */
static void
ipv6_stable_add(
	const char *name,
	const struct ipv6_prefix_state *state,
	uint64_t now)
{
	struct in6_addr address;
	uint32_t valid;
	uint32_t preferred;
	uint8_t iid[8];
	int status;

	/* Past the last try, or the prefix's lifetime over: none. */
	if (state->dad_counter > SLAAC_IDGEN_RETRIES)
		return;
	ipv6_prefix_left(state, now, &valid, &preferred);
	if (valid == 0U)
		return;
	status = ipv6_secret_load();
	if (status != 0)
		return;

	/* Succeeded: the identifier and the address. */
	slaac_stable_iid(ipv6_secret, sizeof(ipv6_secret), &state->prefix, name, "", state->dad_counter, iid);
	slaac_address(&state->prefix, iid, &address);
	(void)ipv6_add(name, &address, 64U, IN6_IFF_AUTOCONF, valid, preferred);
}

/* Makes a prefix a new temporary identifier: random (not universal), made now, with its desynchronization; 0, or -1. */
static int
ipv6_temporary_make(
	struct ipv6_prefix_state *state,
	uint64_t now)
{
	uint32_t random;
	int status;

	/* The identifier and the desynchronization. */
	status = getentropy(state->iid, sizeof(state->iid));
	if (status != 0)
		return -1;
	status = getentropy(&random, sizeof(random));
	if (status != 0)
		return -1;

	/* Succeeded. */
	state->iid[0] &= 0xfdU;
	state->desync = slaac_temporary_desync(random);
	state->created = now;
	state->temporary = 1;
	return 0;
}

/* Adds (or renews) a prefix's temporary address, its lifetimes counted from when it was made. */
static void
ipv6_temporary_add(
	const char *name,
	const struct ipv6_prefix_state *state,
	uint64_t now)
{
	struct in6_addr address;
	uint32_t valid;
	uint32_t preferred;
	uint32_t temporary_valid;
	uint32_t temporary_preferred;
	uint64_t age;

	/* What is left of the prefix's lifetimes, and of the temporary address's own. */
	ipv6_prefix_left(state, now, &valid, &preferred);
	age = 0;
	if (now > state->created)
		age = (now - state->created) / IPV6_US_PER_SECOND;
	slaac_temporary_aged(valid, preferred, state->desync, age, &temporary_valid, &temporary_preferred);
	if (temporary_valid == 0U)
		return;

	/* Succeeded: the address. */
	slaac_address(&state->prefix, state->iid, &address);
	(void)ipv6_add(name, &address, 64U, IN6_IFF_AUTOCONF | IN6_IFF_TEMPORARY, temporary_valid, temporary_preferred);
}

/*
 * Makes each temporary address that is REGEN_ADVANCE from no longer being
 * preferred anew (RFC 8981 section 3.4, step 6), while its prefix is
 * preferred; the old one is left to its lifetimes, which the kernel counts
 * down and no advertisement renews.
 */
static void
ipv6_temporary_due(
	uint64_t now)
{
	struct netconf_interface item;
	struct ipv6_prefix_state *state;
	char name[IF_NAMESIZE];
	const char *named;
	uint64_t regenerate;
	uint32_t valid;
	uint32_t preferred;
	unsigned index;
	int dns_dynamic;
	int wanted;
	int status;

	/* Each temporary address due. */
	for (index = 0; index < IPV6_PREFIXES; index++) {
		state = &ipv6_prefixes[index];
		if (!state->used || !state->temporary)
			continue;
		regenerate = slaac_temporary_regenerate(state->desync);
		if (state->created + regenerate * IPV6_US_PER_SECOND > now)
			continue;

		/* No new one for a prefix no longer preferred, an interface gone, or temporary addresses turned off. */
		state->temporary = 0;
		ipv6_prefix_left(state, now, &valid, &preferred);
		named = if_indextoname(state->ifindex, name);
		if (preferred == 0U || named == NULL)
			continue;
		ipv6_config(name, &item, &dns_dynamic);
		wanted = netconf_ipv6_temporary(&item);
		if (!wanted)
			continue;

		/* The new one. */
		state->temporary_retries = 0;
		status = ipv6_temporary_make(state, now);
		if (status != 0)
			continue;
		ipv6_temporary_add(name, state, now);
		printf("networkd: %s: new temporary IPv6 address\n", name);
		fflush(stdout);
	}
}

/* Keeps (or forgets, with a lifetime of 0) a router an advertisement named, and chooses the default route again. */
static void
ipv6_router(
	unsigned ifindex,
	const struct in6_addr *router,
	uint32_t lifetime)
{
	struct slaac_router *entry;
	uint64_t now;
	unsigned index;
	int found;
	int same;

	/* The router's entry. */
	found = -1;
	for (index = 0; index < IPV6_ROUTERS && found < 0; index++) {
		entry = &ipv6_routers[index];
		if (!entry->used || entry->ifindex != ifindex)
			continue;
		same = memcmp(&entry->address, router, sizeof(*router));
		if (same == 0)
			found = (int)index;
	}

	/* A lifetime of 0: not a default router (any more). */
	if (lifetime == 0U) {
		if (found < 0)
			return;
		ipv6_routers[found].used = 0;
		ipv6_routes_apply(-1);
		return;
	}

	/* Otherwise kept, in a free entry the first time (a full table: not kept). */
	for (index = 0; index < IPV6_ROUTERS && found < 0; index++) {
		if (!ipv6_routers[index].used)
			found = (int)index;
	}

	/* Succeeded unless the table is full: kept until its lifetime ends. */
	if (found < 0)
		return;
	now = netutil_monotonic_us();
	entry = &ipv6_routers[found];
	entry->used = 1;
	entry->ifindex = ifindex;
	entry->address = *router;
	entry->expires = now + (uint64_t)lifetime * IPV6_US_PER_SECOND;
	ipv6_routes_apply(found);
}

/*
 * Puts the default route through the router chosen (ws005-p019's choice:
 * a wired interface before Wi-Fi, the wired ones in networkd's order) and
 * takes the other interfaces' default routes away.  refreshed is the
 * router an advertisement just renewed (its route is put again with the
 * new lifetime), or -1.
 */
static void
ipv6_routes_apply(
	int refreshed)
{
	const struct slaac_router *entry;
	char name[IF_NAMESIZE];
	const char *named;
	uint64_t now;
	unsigned index;
	int current;
	int chosen;
	int same;

	/* Each router's rank; one whose interface went is forgotten. */
	current = -1;
	for (index = 0; index < IPV6_ROUTERS; index++) {
		if (!ipv6_routers[index].used)
			continue;
		named = if_indextoname(ipv6_routers[index].ifindex, name);
		if (named == NULL) {
			ipv6_routers[index].used = 0;
			continue;
		}

		/* Its rank, and whether it is the one in effect. */
		ipv6_routers[index].rank = networkd_interface_rank(name);
		same = memcmp(&ipv6_routers[index].address, &ipv6_route_router, sizeof(ipv6_route_router));
		if (ipv6_route_set && ipv6_routers[index].ifindex == ipv6_route_ifindex && same == 0)
			current = (int)index;
	}

	/* The choice. */
	now = netutil_monotonic_us();
	chosen = slaac_router_choose(ipv6_routers, IPV6_ROUTERS, now, current);

	/* None: the route put before goes. */
	if (chosen < 0) {
		if (ipv6_route_set)
			ipv6_route_delete(ipv6_route_ifindex);
		ipv6_route_set = 0;
		return;
	}

	/* The same router: its route put again only when renewed. */
	entry = &ipv6_routers[chosen];
	if (chosen == current) {
		if (refreshed == chosen)
			ipv6_route_put(entry->ifindex, &entry->address, entry->expires, now);
		return;
	}

	/* Another one: every other interface's default route goes. */
	if (ipv6_route_set)
		ipv6_route_delete(ipv6_route_ifindex);
	for (index = 0; index < IPV6_ROUTERS; index++) {
		if (ipv6_routers[index].used && ipv6_routers[index].ifindex != entry->ifindex)
			ipv6_route_delete(ipv6_routers[index].ifindex);
	}

	/* Succeeded: the chosen one's put, and said in the log. */
	ipv6_route_put(entry->ifindex, &entry->address, entry->expires, now);
	ipv6_route_set = 1;
	ipv6_route_ifindex = entry->ifindex;
	ipv6_route_router = entry->address;
	named = if_indextoname(ipv6_route_ifindex, name);
	if (named == NULL)
		(void)snprintf(name, sizeof(name), "?");
	printf("networkd: IPv6 default route: %s\n", name);
	fflush(stdout);
}

/* Puts the router as the default route of its interface until its lifetime ends (the kernel removes it then). */
static void
ipv6_route_put(
	unsigned ifindex,
	const struct in6_addr *router,
	uint64_t expires,
	uint64_t now)
{
	struct in6_rtentry route;
	uint64_t left;
	int status;

	/* The seconds left, rounded up. */
	left = 0;
	if (expires > now)
		left = (expires - now + IPV6_US_PER_SECOND - 1U) / IPV6_US_PER_SECOND;
	if (left == 0U)
		return;
	if (left >= IN6_LIFETIME_INFINITE)
		left = IN6_LIFETIME_INFINITE - 1U;

	/* The route to ::/0 through the router; one already there is replaced. */
	memset(&route, 0, sizeof(route));
	route.rt6_gateway = *router;
	route.rt6_flags = RTF_UP | RTF_GATEWAY | RTF_DYNAMIC;
	route.rt6_ifindex = ifindex;
	route.rt6_lifetime = (uint32_t)left;
	ipv6_route_delete(ifindex);
	status = ipv6_request(SIOCADDRT_IN6, &route);
	if (status != 0)
		fprintf(stderr, "networkd: IPv6 default route: %s\n", strerror(errno));
}

/* Takes away an interface's default route (none there is the same). */
static void
ipv6_route_delete(
	unsigned ifindex)
{
	struct in6_rtentry route;

	/* ::/0 on the interface. */
	memset(&route, 0, sizeof(route));
	route.rt6_ifindex = ifindex;
	(void)ipv6_request(SIOCDELRT_IN6, &route);
}

/*
 * Keeps an advertisement's DNS servers (RDNSS) and search list (DNSSL)
 * for their lifetimes (a lifetime of 0 takes them away), in place of the
 * interface's earlier ones, and puts them in resolv.conf.  An
 * advertisement without the option leaves the earlier ones to their
 * lifetimes.
 */
static void
ipv6_dns_advertised(
	unsigned ifindex,
	const char *name,
	const struct slaac_ra *ra)
{
	char text[INET6_ADDRSTRLEN];
	const char *written;
	uint64_t now;
	unsigned index;
	unsigned slot;
	int changed;
	int on_link;

	/* The servers: the interface's earlier ones out, these in while there is room. */
	changed = 0;
	now = netutil_monotonic_us();
	if (ra->dns_count != 0U) {
		changed = ipv6_dns_forget_servers(ifindex);
		slot = 0;
		for (index = 0; index < ra->dns_count && ra->dns_lifetime != 0U; index++) {
			while (slot < IPV6_DNS_ENTRIES && ipv6_dns_entries[slot].used)
				slot++;
			if (slot == IPV6_DNS_ENTRIES)
				break;
			written = inet_ntop(AF_INET6, &ra->dns[index], text, sizeof(text));
			if (written == NULL)
				continue;

			/* Kept, one on the link with its interface. */
			on_link = IN6_IS_ADDR_LINKLOCAL(&ra->dns[index]);
			if (on_link)
				(void)snprintf(ipv6_dns_entries[slot].text, RESOLVER6_SERVER_TEXT, "%s%%%s", text, name);
			else
				(void)snprintf(ipv6_dns_entries[slot].text, RESOLVER6_SERVER_TEXT, "%s", text);
			ipv6_dns_entries[slot].used = 1;
			ipv6_dns_entries[slot].ifindex = ifindex;
			ipv6_dns_entries[slot].expires = now + (uint64_t)ra->dns_lifetime * IPV6_US_PER_SECOND;
			changed = 1;
		}
	}

	/* The search list, likewise. */
	if (ra->search[0] != '\0') {
		for (index = 0; index < IPV6_SEARCHES; index++) {
			if (ipv6_searches[index].used && ipv6_searches[index].ifindex == ifindex)
				ipv6_searches[index].used = 0;
		}

		/* This one in a free entry, unless its lifetime is 0. */
		for (index = 0; index < IPV6_SEARCHES && ra->search_lifetime != 0U; index++) {
			if (ipv6_searches[index].used)
				continue;
			(void)snprintf(ipv6_searches[index].text, sizeof(ipv6_searches[index].text), "%s", ra->search);
			ipv6_searches[index].used = 1;
			ipv6_searches[index].ifindex = ifindex;
			ipv6_searches[index].expires = now + (uint64_t)ra->search_lifetime * IPV6_US_PER_SECOND;
			break;
		}

		/* Changed in any case. */
		changed = 1;
	}

	/* resolv.conf as it now should be. */
	if (changed)
		ipv6_resolver_sync(1);
}

/* Forgets an interface's DNS servers; 1 when it had any, or 0. */
static int
ipv6_dns_forget_servers(
	unsigned ifindex)
{
	unsigned index;
	int forgot;

	/* Each of the interface's. */
	forgot = 0;
	for (index = 0; index < IPV6_DNS_ENTRIES; index++) {
		if (!ipv6_dns_entries[index].used || ipv6_dns_entries[index].ifindex != ifindex)
			continue;
		ipv6_dns_entries[index].used = 0;
		forgot = 1;
	}

	/* Whether any went. */
	return forgot;
}

/* Forgets an interface's DNS servers and search list; 1 when it had any, or 0. */
static int
ipv6_dns_forget(
	unsigned ifindex)
{
	unsigned index;
	int forgot;

	/* The servers, then the search list. */
	forgot = ipv6_dns_forget_servers(ifindex);
	for (index = 0; index < IPV6_SEARCHES; index++) {
		if (!ipv6_searches[index].used || ipv6_searches[index].ifindex != ifindex)
			continue;
		ipv6_searches[index].used = 0;
		forgot = 1;
	}

	/* Whether any went. */
	return forgot;
}

/*
 * Gives the RDNSS servers networkd wants in resolv.conf (the preferred
 * interface's first, each once, three at most) and the first search list,
 * none when net.conf names the servers (dns static).
 */
static void
ipv6_dns_wanted(
	struct resolver6_servers *wanted,
	char *search,
	size_t capacity)
{
	struct netconf_interface item;
	char name[IF_NAMESIZE];
	unsigned ranks[IPV6_DNS_ENTRIES];
	int taken[IPV6_DNS_ENTRIES];
	const char *named;
	unsigned index;
	unsigned prior;
	int best;
	int dns_dynamic;
	int same;
	int fresh;

	/* None yet; none at all when the servers are net.conf's. */
	wanted->count = 0;
	search[0] = '\0';
	ipv6_config("", &item, &dns_dynamic);
	if (!dns_dynamic)
		return;

	/* Each server's interface's rank. */
	for (index = 0; index < IPV6_DNS_ENTRIES; index++) {
		taken[index] = !ipv6_dns_entries[index].used;
		ranks[index] = 0;
		if (taken[index])
			continue;
		named = if_indextoname(ipv6_dns_entries[index].ifindex, name);
		ranks[index] = ~0U;
		if (named != NULL)
			ranks[index] = networkd_interface_rank(name);
	}

	/* The best ranked left, each once, until three. */
	while (wanted->count < RESOLVER6_SERVERS) {
		best = -1;
		for (index = 0; index < IPV6_DNS_ENTRIES; index++) {
			if (!taken[index] && (best < 0 || ranks[index] < ranks[best]))
				best = (int)index;
		}

		/* None left. */
		if (best < 0)
			break;

		/* Taken, unless it is listed already. */
		taken[best] = 1;
		fresh = 1;
		for (prior = 0; prior < wanted->count; prior++) {
			same = strcmp(wanted->text[prior], ipv6_dns_entries[best].text);
			if (same == 0)
				fresh = 0;
		}

		/* Listed. */
		if (fresh)
			(void)snprintf(wanted->text[wanted->count++], RESOLVER6_SERVER_TEXT, "%s", ipv6_dns_entries[best].text);
	}

	/* The first search list. */
	for (index = 0; index < IPV6_SEARCHES; index++) {
		if (!ipv6_searches[index].used)
			continue;
		(void)snprintf(search, capacity, "%s", ipv6_searches[index].text);
		return;
	}
}

/*
 * Puts the RDNSS servers networkd wants into resolv.conf (resolver6.c's
 * merge) when they changed or another writer rewrote the file since
 * networkd last looked at it.
 */
static void
ipv6_resolver_sync(
	int changed)
{
	struct resolver6_servers wanted;
	struct resolver6_servers owned;
	struct ipv6_file_identity identity;
	char search[SLAAC_SEARCH_MAX];
	ssize_t count;
	size_t length;
	int descriptor;
	int status;
	int same;

	/* The file as it is: the same one networkd last looked at and nothing new, nothing to do. */
	ipv6_resolver_look(&identity);
	same = 0;
	if (ipv6_resolv_seen.known && identity.present == ipv6_resolv_seen.present)
		same = 1;
	if (same && identity.present) {
		if (identity.device != ipv6_resolv_seen.device || identity.inode != ipv6_resolv_seen.inode)
			same = 0;
		else if (identity.size != ipv6_resolv_seen.size)
			same = 0;
		else if (identity.modified.tv_sec != ipv6_resolv_seen.modified.tv_sec)
			same = 0;
		else if (identity.modified.tv_nsec != ipv6_resolv_seen.modified.tv_nsec)
			same = 0;
	}

	/* Nothing to do. */
	if (same && !changed)
		return;

	/* What networkd wants there; nothing wanted and nothing put there before, nothing to do. */
	ipv6_dns_wanted(&wanted, search, sizeof(search));
	if (wanted.count == 0U && ipv6_dns_owned.count == 0U && search[0] == '\0') {
		ipv6_resolv_seen = identity;
		return;
	}

	/* The file's text (none when it is not there). */
	length = 0;
	descriptor = open(IPV6_RESOLV_PATH, O_RDONLY | O_CLOEXEC);
	if (descriptor >= 0) {
		count = read(descriptor, ipv6_resolv_current, sizeof(ipv6_resolv_current) - 1U);
		close(descriptor);
		if (count > 0)
			length = (size_t)count;
	}

	/* Ended as a string. */
	ipv6_resolv_current[length] = '\0';

	/* The merge, written when it changes the file. */
	status = resolver6_merge(ipv6_resolv_current, length, &ipv6_dns_owned, &wanted, search, ipv6_resolv_output,
	    sizeof(ipv6_resolv_output), &owned);
	if (status > 0)
		status = ipv6_resolver_write(ipv6_resolv_output);
	if (status < 0)
		fprintf(stderr, "networkd: %s: the IPv6 DNS servers: %s\n", IPV6_RESOLV_PATH, strerror(errno));
	else
		ipv6_dns_owned = owned;

	/* Succeeded or not, looked at as it is now (a failure is tried again on the next change). */
	ipv6_resolver_look(&ipv6_resolv_seen);
}

/* Puts resolv.conf's new text in place: a twin written and synced, renamed over it; 0, or -1 with errno set. */
static int
ipv6_resolver_write(
	const char *text)
{
	FILE *output;
	int written;
	int status;
	int saved;

	/* The twin. */
	output = fopen(IPV6_RESOLV_TEMPORARY, "w");
	if (output == NULL)
		return -1;
	written = fputs(text, output);
	status = 0;
	if (written < 0)
		status = -1;
	if (status == 0)
		status = fflush(output);
	if (status == 0)
		status = fsync(fileno(output));
	saved = errno;
	written = fclose(output);
	if (written != 0 && status == 0) {
		status = -1;
		saved = errno;
	}

	/* Failed: the cause kept. */
	if (status != 0) {
		errno = saved;
		return -1;
	}

	/* Succeeded when it is renamed over the file. */
	status = rename(IPV6_RESOLV_TEMPORARY, IPV6_RESOLV_PATH);
	if (status != 0)
		return -1;
	printf("networkd: %s: the IPv6 DNS servers put in\n", IPV6_RESOLV_PATH);
	fflush(stdout);
	return 0;
}

/* Looks at which file resolv.conf is now (or that it is not there). */
static void
ipv6_resolver_look(
	struct ipv6_file_identity *identity)
{
	struct stat information;
	int status;

	/* Not there. */
	memset(identity, 0, sizeof(*identity));
	identity->known = 1;
	status = stat(IPV6_RESOLV_PATH, &information);
	if (status != 0)
		return;

	/* Succeeded: the file. */
	identity->present = 1;
	identity->device = information.st_dev;
	identity->inode = information.st_ino;
	identity->size = information.st_size;
	identity->modified = information.st_mtim;
}

/*
 * Starts DHCPv6 as an advertisement and net.conf ask (section 4): with
 * dhcp auto, stateful on the M flag, or only the information on the O flag
 * when there is no RDNSS; stateless or stateful as net.conf says
 * otherwise.  An interface already run in the same mode is left to its
 * schedule.
 */
static void
ipv6_dhcp_advertised(
	const char *name,
	const struct netconf_interface *item,
	const struct slaac_ra *ra,
	int dns_dynamic)
{
	struct ipv6_dhcp *entry;
	enum ipv6_dhcp_mode mode;

	/* The mode. */
	mode = IPV6_DHCP_NONE;
	switch (netconf_ipv6_dhcp(item)) {
	case NETCONF_IPV6_DHCP_AUTO:
		if ((ra->flags & SLAAC_RA_MANAGED) != 0U)
			mode = IPV6_DHCP_STATEFUL;
		else if ((ra->flags & SLAAC_RA_OTHER) != 0U && ra->dns_count == 0U)
			mode = IPV6_DHCP_STATELESS;
		break;
	case NETCONF_IPV6_DHCP_STATELESS:
		mode = IPV6_DHCP_STATELESS;
		break;
	case NETCONF_IPV6_DHCP_STATEFUL:
		mode = IPV6_DHCP_STATEFUL;
		break;
	case NETCONF_IPV6_DHCP_OFF:
		break;
	}

	/* None: what was kept goes. */
	entry = ipv6_dhcp_find(name, mode != IPV6_DHCP_NONE);
	if (entry == NULL)
		return;
	if (mode == IPV6_DHCP_NONE) {
		ipv6_dhcp_forget(entry);
		return;
	}

	/* Run in this mode already: its schedule runs it again. */
	if (entry->mode == mode)
		return;

	/* Succeeded: run now. */
	entry->mode = mode;
	entry->resolver = dns_dynamic;
	entry->retry = 0;
	ipv6_dhcp_run(entry);
}

/* Finds an interface's DHCPv6, taking a free slot with make; NULL when there is none. */
static struct ipv6_dhcp *
ipv6_dhcp_find(
	const char *name,
	int make)
{
	struct ipv6_dhcp *entry;
	unsigned index;
	int same;

	/* The interface's. */
	for (index = 0; index < IPV6_DHCP_INTERFACES; index++) {
		entry = &ipv6_dhcps[index];
		if (!entry->used)
			continue;
		same = strcmp(entry->name, name);
		if (same == 0)
			return entry;
	}

	/* Not there, and none to make. */
	if (!make)
		return NULL;

	/* A free one. */
	for (index = 0; index < IPV6_DHCP_INTERFACES; index++) {
		entry = &ipv6_dhcps[index];
		if (entry->used)
			continue;
		memset(entry, 0, sizeof(*entry));
		entry->used = 1;
		(void)snprintf(entry->name, sizeof(entry->name), "%s", name);
		return entry;
	}

	/* The table is full. */
	return NULL;
}

/*
 * Forgets an interface's DHCPv6 (the next advertisement starts it again).
 * One whose dhcpc still runs keeps its entry, without a mode or a
 * schedule, until the child is waited for.
 */
static void
ipv6_dhcp_forget(
	struct ipv6_dhcp *entry)
{
	/* No dhcpc running: the entry is free. */
	if (entry->child <= 0) {
		entry->used = 0;
		return;
	}

	/* Otherwise kept for the child alone. */
	entry->mode = IPV6_DHCP_NONE;
	entry->due = 0;
	entry->again = 0;
	entry->decline = 0;
}

/*
 * Starts `dhcpc -6` for an interface as a child that the loop waits for
 * (ipv6_dhcp_wait) without stopping; one already running runs again when
 * it ends.
 */
static void
ipv6_dhcp_run(
	struct ipv6_dhcp *entry)
{
	char *arguments[9];
	unsigned count;
	pid_t child;

	/* One running: again after it. */
	if (entry->child > 0) {
		entry->again = 1;
		return;
	}

	/* dhcpc -6 [-i | -D] [-n] -t SECONDS IF. */
	count = 0;
	arguments[count++] = "/sbin/dhcpc";
	arguments[count++] = "-6";
	if (entry->mode == IPV6_DHCP_STATELESS)
		arguments[count++] = "-i";
	else if (entry->decline)
		arguments[count++] = "-D";
	if (!entry->resolver)
		arguments[count++] = "-n";
	arguments[count++] = "-t";
	arguments[count++] = IPV6_DHCP_TIMEOUT;
	arguments[count++] = entry->name;
	arguments[count] = NULL;

	/* The child, its output networkd's. */
	child = fork();
	if (child == 0) {
		execv(arguments[0], arguments);
		_exit(127);
	}

	/* No child: a failed run. */
	if (child < 0) {
		fprintf(stderr, "networkd: %s: dhcpc -6: %s\n", entry->name, strerror(errno));
		ipv6_dhcp_done(entry, 0);
		return;
	}

	/* Succeeded: running (declining, when asked, done by this run). */
	entry->child = child;
	entry->started = netutil_monotonic_us();
	entry->terminated = 0;
	entry->again = 0;
	entry->decline = 0;
	entry->due = 0;
}

/*
 * Looks at an interface's dhcpc: one that ended is done with (its status
 * decides the next run); one past its time is sent SIGTERM, and SIGKILL
 * when it still runs after that.
 */
static void
ipv6_dhcp_wait(
	struct ipv6_dhcp *entry,
	uint64_t now)
{
	pid_t ended;
	int status;
	int exited;
	int code;
	int succeeded;

	/* Ended? */
	status = 0;
	ended = waitpid(entry->child, &status, WNOHANG);
	if (ended == 0 || (ended < 0 && errno == EINTR)) {
		/* Still running: past its time, told to stop, then made to. */
		if (entry->terminated == 0U && now - entry->started >= (uint64_t)IPV6_DHCP_RUN_SECONDS * IPV6_US_PER_SECOND) {
			(void)kill(entry->child, SIGTERM);
			entry->terminated = now;
		} else if (entry->terminated != 0U &&
		    now - entry->terminated >= (uint64_t)IPV6_DHCP_KILL_SECONDS * IPV6_US_PER_SECOND) {
			(void)kill(entry->child, SIGKILL);
		}

		/* Looked at again later. */
		return;
	}

	/* Ended (or lost): the next run as its status says. */
	succeeded = 0;
	exited = 0;
	code = -1;
	if (ended == entry->child) {
		exited = WIFEXITED(status);
		code = WEXITSTATUS(status);
	}

	/* Succeeded when it exited 0; no child now. */
	if (exited && code == 0)
		succeeded = 1;
	entry->child = 0;

	/* Forgotten while it ran: the entry is free now. */
	if (entry->mode == IPV6_DHCP_NONE) {
		entry->used = 0;
		return;
	}

	/* The next run. */
	ipv6_dhcp_done(entry, succeeded);

	/* Asked to run again while it ran: now. */
	if (entry->again) {
		entry->again = 0;
		entry->due = now;
	}
}

/*
 * Schedules an interface's next DHCPv6 run after one ended: at T1 (or the
 * information refresh time) as dhcpc recorded, or after a wait that
 * doubles while it fails.
 */
static void
ipv6_dhcp_done(
	struct ipv6_dhcp *entry,
	int succeeded)
{
	const char *mode;
	unsigned renew;

	/* Failed: again after a wait twice the last. */
	if (!succeeded) {
		entry->retry *= 2U;
		if (entry->retry == 0U)
			entry->retry = IPV6_DHCP_RETRY_FIRST;
		if (entry->retry > IPV6_DHCP_RETRY_LAST)
			entry->retry = IPV6_DHCP_RETRY_LAST;
		entry->due = netutil_monotonic_us() + (uint64_t)entry->retry * IPV6_US_PER_SECOND;
		printf("networkd: %s: DHCPv6 failed, again in %u seconds\n", entry->name, entry->retry);
		fflush(stdout);
		return;
	}

	/* Succeeded: again when dhcpc said (not again for 0). */
	entry->retry = 0;
	renew = ipv6_dhcp_renew(entry->name);
	entry->due = 0;
	if (renew != 0U)
		entry->due = netutil_monotonic_us() + (uint64_t)renew * IPV6_US_PER_SECOND;
	mode = "stateless";
	if (entry->mode == IPV6_DHCP_STATEFUL)
		mode = "stateful";
	printf("networkd: %s: DHCPv6 %s, again in %u seconds\n", entry->name, mode, renew);
	fflush(stdout);
}

/* Reads when dhcpc said to run again ("renew SECONDS" in its record); 0 when not again or not said. */
static unsigned
ipv6_dhcp_renew(
	const char *name)
{
	char path[64];
	char line[64];
	unsigned long value;
	unsigned renew;
	FILE *input;
	char *got;
	char *end;
	int same;

	/* The record. */
	(void)snprintf(path, sizeof(path), IPV6_DHCP_RECORD, name);
	input = fopen(path, "r");
	if (input == NULL)
		return 0;

	/* Its renew line. */
	renew = 0;
	for (;;) {
		got = fgets(line, sizeof(line), input);
		if (got == NULL)
			break;
		same = strncmp(line, "renew ", 6);
		if (same != 0)
			continue;
		value = strtoul(line + 6, &end, 10);
		if ((*end == '\n' || *end == '\0') && value <= 0xffffffffUL)
			renew = (unsigned)value;
	}

	/* The record let go. */
	fclose(input);

	/* Succeeded. */
	return renew;
}

/* Keeps the sooner of a time and the soonest so far (0: none yet). */
static void
ipv6_soonest(
	uint64_t *soonest,
	uint64_t when)
{
	/* The first, or a sooner one. */
	if (*soonest == 0U || when < *soonest)
		*soonest = when;
}
