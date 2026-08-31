/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Test-only HW-T22/NET-T42 production-network guest probe. */

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define INTERFACE "ue0"
#define PEER "10.0.2.2"
#define STATIC_ADDRESS "10.0.2.16"
#define DHCP_ADDRESS "10.0.2.15"
#define POLL_COUNT 400U
#define POLL_NANOSECONDS 100000000L

struct interface_snapshot {
	int flags;
	struct in_addr address;
	struct if_data statistics;
};

static int run_command(char *const arguments[]);
static int request(int descriptor, unsigned long command,
	struct ifreq *request_);
static int snapshot(struct interface_snapshot *result, int need_address);
static int wait_present(void);
static int wait_absent(void);
static int wait_carrier(void);
static int run_generation(const char *mode, unsigned generation);
static int fail(const char *mode, const char *stage, int error);
static void delay(void);

int
main(
	int argc,
	char **argv)
{
	const char *mode;

	if (argc != 2 || (strcmp(argv[1], "static") != 0 &&
			   strcmp(argv[1], "dhcp") != 0)) {
		puts("ECM-QEMU FAIL mode=invalid stage=arguments error=2");
		return 2;
	}
	mode = argv[1];
	if (access("/sbin/net", X_OK) != 0 ||
	    access("/sbin/ifconfig", X_OK) != 0 ||
	    access("/sbin/dhcpc", X_OK) != 0 ||
	    access("/bin/ping", X_OK) != 0)
		return fail(mode, "production-command", errno);
	if (run_generation(mode, 1U) != 0)
		return 1;
	printf("ECM-QEMU READY-DETACH mode=%s\n", mode);
	fflush(stdout);
	if (wait_absent() != 0)
		return fail(mode, "detach", errno);
	printf("ECM-QEMU DETACHED mode=%s\n", mode);
	fflush(stdout);
	if (run_generation(mode, 2U) != 0)
		return 1;
	printf("ECM-QEMU PASS mode=%s generations=2\n", mode);
	fflush(stdout);
	return 0;
}

static int
run_command(
	char *const arguments[])
{
	pid_t child, waited;
	int status;

	child = fork();
	if (child == 0) {
		execv(arguments[0], arguments);
		_exit(127);
	}
	if (child < 0)
		return -1;
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited != child)
		return -1;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return -1;
}

static int
request(
	int descriptor,
	unsigned long command,
	struct ifreq *request_)
{
	memset(request_, 0, sizeof(*request_));
	strcpy(request_->ifr_name, INTERFACE);
	return ioctl(descriptor, command, request_);
}

static int
snapshot(
	struct interface_snapshot *result,
	int need_address)
{
	struct ifreq flags, address, statistics;
	const struct sockaddr_in *internet;
	int descriptor, saved;

	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return -1;
	if (request(descriptor, SIOCGIFFLAGS, &flags) != 0 ||
	    request(descriptor, SIOCGIFSTATS, &statistics) != 0 ||
	    (need_address && request(descriptor, SIOCGIFADDR, &address) != 0)) {
		saved = errno;
		close(descriptor);
		errno = saved;
		return -1;
	}
	result->flags = flags.ifr_flags;
	result->statistics = statistics.ifr_data;
	result->address.s_addr = 0;
	if (need_address) {
		internet = (const struct sockaddr_in *)&address.ifr_addr;
		result->address = internet->sin_addr;
	}
	close(descriptor);
	return 0;
}

static int
wait_present(
	void)
{
	struct interface_snapshot current;
	unsigned attempt;

	for (attempt = 0; attempt < POLL_COUNT; attempt++) {
		if (snapshot(&current, 0) == 0)
			return 0;
		delay();
	}
	errno = ETIMEDOUT;
	return -1;
}

static int
wait_absent(
	void)
{
	struct interface_snapshot current;
	unsigned absent, attempt;

	absent = 0;
	for (attempt = 0; attempt < POLL_COUNT; attempt++) {
		if (snapshot(&current, 0) != 0) {
			if ((errno == ENODEV || errno == ENXIO) &&
			    ++absent == 3U)
				return 0;
			if (errno != ENODEV && errno != ENXIO)
				absent = 0;
		} else {
			absent = 0;
		}
		delay();
	}
	errno = ETIMEDOUT;
	return -1;
}

static int
wait_carrier(
	void)
{
	struct interface_snapshot current;
	unsigned attempt;

	for (attempt = 0; attempt < POLL_COUNT; attempt++) {
		if (snapshot(&current, 0) == 0 &&
		    (current.flags & (IFF_UP | IFF_RUNNING)) ==
			(IFF_UP | IFF_RUNNING))
			return 0;
		delay();
	}
	errno = ETIMEDOUT;
	return -1;
}

static int
run_generation(
	const char *mode,
	unsigned generation)
{
	char *up[] = {"/sbin/net", "up", INTERFACE, NULL};
	char *static_address[] = {"/sbin/net", "static", INTERFACE, "ipv4",
				  STATIC_ADDRESS, "netmask", "255.255.255.0",
				  NULL};
	char *default_route[] = {"/sbin/net", "defaultroute", PEER, NULL};
	char *dhcp[] = {"/sbin/net", "dhcp", INTERFACE, "--timeout=15",
			NULL};
	char *show[] = {"/sbin/net", "show", INTERFACE, NULL};
	char *ifconfig[] = {"/sbin/ifconfig", INTERFACE, NULL};
	char *ping[] = {"/bin/ping", "-c", "1", "-W", "3000", PEER,
			NULL};
	struct interface_snapshot before, after;
	struct in_addr expected;
	const char *address;
	int status;

	if (wait_present() != 0)
		return fail(mode, generation == 1U ? "enumerate-1" :
			"enumerate-2", errno);
	status = run_command(up);
	if (status != 0)
		return fail(mode, generation == 1U ? "up-1" : "up-2",
			status);
	if (wait_carrier() != 0)
		return fail(mode, generation == 1U ? "carrier-1" :
			"carrier-2", errno);
	if (strcmp(mode, "static") == 0) {
		status = run_command(static_address);
		if (status == 0)
			status = run_command(default_route);
		address = STATIC_ADDRESS;
	} else {
		status = run_command(dhcp);
		address = DHCP_ADDRESS;
	}
	if (status != 0)
		return fail(mode, generation == 1U ? "configure-1" :
			"configure-2", status);
	if (run_command(show) != 0 || run_command(ifconfig) != 0)
		return fail(mode, generation == 1U ? "inspect-1" :
			"inspect-2", errno);
	if (inet_pton(AF_INET, address, &expected) != 1 ||
	    snapshot(&before, 1) != 0 ||
	    before.address.s_addr != expected.s_addr ||
	    (before.flags & (IFF_UP | IFF_RUNNING)) !=
		(IFF_UP | IFF_RUNNING))
		return fail(mode, generation == 1U ? "state-1" : "state-2",
			errno);
	status = run_command(ping);
	if (status != 0)
		return fail(mode, generation == 1U ? "ping-1" : "ping-2",
			status);
	if (snapshot(&after, 1) != 0 ||
	    after.address.s_addr != expected.s_addr ||
	    after.statistics.ifi_ipackets <= before.statistics.ifi_ipackets ||
	    after.statistics.ifi_opackets <= before.statistics.ifi_opackets)
		return fail(mode, generation == 1U ? "counters-1" :
			"counters-2", errno);
	printf("ECM-QEMU GENERATION mode=%s generation=%u address=%s "
	       "rx=%llu tx=%llu\n",
	       mode, generation, address,
	       (unsigned long long)after.statistics.ifi_ipackets,
	       (unsigned long long)after.statistics.ifi_opackets);
	fflush(stdout);
	return 0;
}

static int
fail(
	const char *mode,
	const char *stage,
	int error)
{
	printf("ECM-QEMU FAIL mode=%s stage=%s error=%d\n", mode, stage,
	       error);
	fflush(stdout);
	return 1;
}

static void
delay(
	void)
{
	struct timespec interval;

	interval.tv_sec = 0;
	interval.tv_nsec = POLL_NANOSECONDS;
	(void)nanosleep(&interval, NULL);
}
