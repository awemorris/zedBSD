/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/cred.h"
#include "kern/net/inet-socket.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/route.h"

#include <zedbsd/netif.h>
#include <zedbsd/route.h>
#include <zedbsd/wlan.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
test_assert_fail(const char *expression, const char *file, int line)
{
	fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line,
	    expression);
	abort();
}

#define assert(expression)                                                     \
	((expression) ? (void)0 :                                              \
	 test_assert_fail(#expression, __FILE__, __LINE__))

static struct ucred caller;
static unsigned route_calls;
static unsigned credential_refs;
static unsigned credential_releases;
static unsigned copyin_calls;
static unsigned copyout_calls;
static size_t last_copyin_size;
static size_t last_copyout_size;
static int copyin_error;
static int copyout_error;
static unsigned device_ioctl_calls;
static unsigned long device_ioctl_request;
static int device_ioctl_error;
static uintptr_t expected_user_argument;
static bool device_argument_was_kernel_local;
static bool copyout_source_was_kernel_local;
static bool connect_copyout_was_redacted;
static uint8_t captured_passphrase[WLAN_PASSPHRASE_STORAGE];
static uint32_t captured_passphrase_length;
static struct net_device *wlan_device;
static _Thread_local int irq_enabled = 1;

#define TEST_OUTPUT_GENERATION UINT64_C(0x1122334455667788)

struct ucred *
cred_current_ref(void)
{
	credential_refs++;
	return &caller;
}

void
cred_release(struct ucred *credential)
{
	assert(credential == &caller);
	credential_releases++;
}

int
cred_is_superuser(const struct ucred *credential)
{
	return credential != NULL && credential->euid == 0;
}

bool
hal_irq_disable(void)
{
	int previous = irq_enabled;

	irq_enabled = 0;
	return previous != 0;
}

void
hal_irq_enable(void)
{
	irq_enabled = 1;
}

int
copyin(uintptr_t source, void *destination, size_t size)
{
	copyin_calls++;
	last_copyin_size = size;
	if (source == 0)
		return EFAULT;
	if (copyin_error != 0)
		return copyin_error;
	memcpy(destination, (const void *)source, size);
	return 0;
}

int
copyout(const void *source, uintptr_t destination, size_t size)
{
	const struct wlan_connect_request *connect = source;
	unsigned index;

	copyout_calls++;
	last_copyout_size = size;
	copyout_source_was_kernel_local =
	    (uintptr_t)source != expected_user_argument;
	if (device_ioctl_request == SIOCSWLANCONNECT &&
	    size == sizeof(*connect)) {
		connect_copyout_was_redacted =
		    connect->passphrase_length == 0;
		for (index = 0; index < sizeof(connect->passphrase); index++)
			if (connect->passphrase[index] != 0)
				connect_copyout_was_redacted = false;
	}
	if (destination == 0)
		return EFAULT;
	if (copyout_error != 0)
		return copyout_error;
	memcpy((void *)destination, source, size);
	return 0;
}

void
route_purge_device(struct net_device *device)
{
	(void)device;
}

void
arp_purge_device(struct net_device *device)
{
	(void)device;
}

int
route_add_flags(uint32_t network, uint32_t netmask, uint32_t gateway,
		struct net_device *device, unsigned flags)
{
	(void)network;
	(void)netmask;
	(void)gateway;
	(void)device;
	(void)flags;
	return 0;
}

int
route_delete(uint32_t network, uint32_t netmask, struct net_device *device)
{
	(void)network;
	(void)netmask;
	(void)device;
	return 0;
}

int
route_ioctl(unsigned long request, uintptr_t argument)
{
	(void)request;
	route_calls++;
	return argument == 0 ? EFAULT : 0;
}

int
socket_family_register(int family, const struct socket_family_ops *ops)
{
	(void)family;
	(void)ops;
	return 0;
}

int
icmp_socket_create(int protocol, struct socket **result)
{
	(void)protocol;
	(void)result;
	return EOPNOTSUPP;
}

int
udp_socket_create(int protocol, struct socket **result)
{
	(void)protocol;
	(void)result;
	return EOPNOTSUPP;
}

int
tcp_socket_create(int protocol, struct socket **result)
{
	(void)protocol;
	(void)result;
	return EOPNOTSUPP;
}

int
net_input_enqueue(struct net_device *device, struct packet_buf *packet)
{
	(void)device;
	(void)packet;
	return 0;
}

void
net_worker_wakeup(void)
{
}

void
packet_buf_free(struct packet_buf *packet)
{
	(void)packet;
}

union test_wlan_request {
	struct wlan_ioctl_header header;
	struct wlan_scan_request scan;
	struct wlan_scan_status_request scan_status;
	struct wlan_bss_request bss;
	struct wlan_connect_request connect;
	struct wlan_disconnect_request disconnect;
	struct wlan_status_request status;
};

static int
fixture_transmit(struct net_device *device, struct packet_buf *packet)
{
	(void)device;
	(void)packet;
	return 0;
}

static int
fixture_ioctl(struct net_device *device, unsigned long request, void *argument)
{
	union test_wlan_request *wlan = argument;

	assert(device == wlan_device);
	device_ioctl_calls++;
	device_ioctl_request = request;
	device_argument_was_kernel_local =
	    (uintptr_t)argument != expected_user_argument;
	switch (request) {
	case SIOCSWLANSCAN:
		wlan->scan.generation = TEST_OUTPUT_GENERATION;
		break;
	case SIOCGWLANSCAN:
		wlan->scan_status.generation = TEST_OUTPUT_GENERATION;
		break;
	case SIOCGWLANBSS:
		wlan->bss.generation = TEST_OUTPUT_GENERATION;
		break;
	case SIOCSWLANCONNECT:
		captured_passphrase_length = wlan->connect.passphrase_length;
		memcpy(captured_passphrase, wlan->connect.passphrase,
		       sizeof(captured_passphrase));
		memset(wlan->connect.passphrase, 0xa5,
		       sizeof(wlan->connect.passphrase));
		wlan->connect.passphrase_length = WLAN_PASSPHRASE_MAX;
		wlan->connect.generation = TEST_OUTPUT_GENERATION;
		break;
	case SIOCSWLANDISCONNECT:
		wlan->disconnect.generation = TEST_OUTPUT_GENERATION;
		break;
	case SIOCGWLANSTATUS:
		wlan->status.operation_generation = TEST_OUTPUT_GENERATION;
		break;
	default:
		assert(false);
	}
	return device_ioctl_error;
}

static const struct net_device_ops fixture_ops = {
	.transmit = fixture_transmit,
	.ioctl = fixture_ioctl,
};

static struct net_device *
create_device(const char *name, unsigned capabilities)
{
	struct net_device *device = net_device_alloc();

	assert(device != NULL);
	strcpy(device->name, name);
	device->mtu = 1500;
	device->hwaddr_len = 6;
	device->hwaddr[5] = 1;
	device->capabilities = capabilities;
	device->ops = &fixture_ops;
	assert(net_device_create(device) == 0);
	return device;
}

static void
reset_observations(void)
{
	route_calls = 0;
	credential_refs = 0;
	credential_releases = 0;
	copyin_calls = 0;
	copyout_calls = 0;
	last_copyin_size = 0;
	last_copyout_size = 0;
	copyin_error = 0;
	copyout_error = 0;
	device_ioctl_calls = 0;
	device_ioctl_request = 0;
	device_ioctl_error = 0;
	expected_user_argument = 0;
	device_argument_was_kernel_local = false;
	copyout_source_was_kernel_local = false;
	connect_copyout_was_redacted = false;
	memset(captured_passphrase, 0, sizeof(captured_passphrase));
	captured_passphrase_length = 0;
}

static size_t
wlan_request_size(unsigned long command)
{
	switch (command) {
	case SIOCSWLANSCAN:
		return sizeof(struct wlan_scan_request);
	case SIOCGWLANSCAN:
		return sizeof(struct wlan_scan_status_request);
	case SIOCGWLANBSS:
		return sizeof(struct wlan_bss_request);
	case SIOCSWLANCONNECT:
		return sizeof(struct wlan_connect_request);
	case SIOCSWLANDISCONNECT:
		return sizeof(struct wlan_disconnect_request);
	case SIOCGWLANSTATUS:
		return sizeof(struct wlan_status_request);
	default:
		assert(false);
		return 0;
	}
}

static bool
wlan_command_is_query(unsigned long command)
{
	return command == SIOCGWLANSCAN || command == SIOCGWLANBSS ||
	       command == SIOCGWLANSTATUS;
}

static void
wlan_request_init(union test_wlan_request *request, unsigned long command,
		  const char *name)
{
	size_t size = wlan_request_size(command);

	memset(request, 0, sizeof(*request));
	strcpy(request->header.ifr_name, name);
	request->header.version = WLAN_ABI_VERSION;
	request->header.size = (uint32_t)size;
}

static uint64_t
wlan_request_generation(const union test_wlan_request *request,
			unsigned long command)
{
	switch (command) {
	case SIOCSWLANSCAN:
		return request->scan.generation;
	case SIOCGWLANSCAN:
		return request->scan_status.generation;
	case SIOCGWLANBSS:
		return request->bss.generation;
	case SIOCSWLANCONNECT:
		return request->connect.generation;
	case SIOCSWLANDISCONNECT:
		return request->disconnect.generation;
	case SIOCGWLANSTATUS:
		return request->status.operation_generation;
	default:
		assert(false);
		return 0;
	}
}

static bool
buffer_is_zero(const uint8_t *buffer, size_t size)
{
	while (size-- != 0)
		if (*buffer++ != 0)
			return false;
	return true;
}

static void
expect_nonroot_denied(unsigned long command)
{
	unsigned calls = route_calls;
	unsigned refs = credential_refs;
	unsigned releases = credential_releases;
	int error;

	caller.euid = 1000;
	error = inet_socket_ioctl(NULL, command, 0);
	if (error != EPERM)
		fprintf(stderr, "command %#lx returned %d, expected EPERM\n",
		    command, error);
	assert(error == EPERM);
	assert(route_calls == calls);
	assert(credential_refs == refs + 1U);
	assert(credential_releases == releases + 1U);
}

static void
expect_root_reaches_argument_check(unsigned long command)
{
	unsigned refs = credential_refs;
	unsigned releases = credential_releases;

	caller.euid = 0;
	assert(inet_socket_ioctl(NULL, command, 0) == EFAULT);
	assert(credential_refs == refs + 1U);
	assert(credential_releases == releases + 1U);
}

static void
expect_nonroot_query(unsigned long command)
{
	unsigned refs = credential_refs;
	unsigned releases = credential_releases;

	caller.euid = 1000;
	assert(inet_socket_ioctl(NULL, command, 0) == EFAULT);
	assert(credential_refs == refs);
	assert(credential_releases == releases);
}

static void
wlan_bad_encoding_test(void)
{
	static const unsigned long commands[] = {
		ZEDBSD_IOC(ZEDBSD_IOC_OUT, ZEDBSD_WLAN_IOCTL_GROUP, 1,
			    sizeof(struct wlan_scan_request)),
		ZEDBSD_IOC(ZEDBSD_IOC_INOUT, ZEDBSD_WLAN_IOCTL_GROUP, 1,
			    sizeof(struct wlan_scan_request) - 1U),
		ZEDBSD_IOC(ZEDBSD_IOC_INOUT, ZEDBSD_WLAN_IOCTL_GROUP, 7,
			    sizeof(struct wlan_ioctl_header)),
		SIOCSWLANSCAN | (1UL << 29),
	};
	union test_wlan_request request;
	unsigned index;

	wlan_request_init(&request, SIOCSWLANSCAN, "wlan0");
	for (index = 0; index < sizeof(commands) / sizeof(commands[0]); index++) {
		reset_observations();
		caller.euid = 1000;
		expected_user_argument = (uintptr_t)&request;
		assert(inet_socket_ioctl(NULL, commands[index],
					 (uintptr_t)&request) == EINVAL);
		assert(credential_refs == 0);
		assert(credential_releases == 0);
		assert(copyin_calls == 0);
		assert(copyout_calls == 0);
		assert(device_ioctl_calls == 0);
		assert(route_calls == 0);
	}
}

static void
expect_malformed_wlan_request(union test_wlan_request *request,
			      unsigned long command)
{
	reset_observations();
	caller.euid = 1000;
	expected_user_argument = (uintptr_t)request;
	assert(inet_socket_ioctl(NULL, command, (uintptr_t)request) == EINVAL);
	assert(credential_refs == 0);
	assert(credential_releases == 0);
	assert(copyin_calls == 1);
	assert(last_copyin_size == wlan_request_size(command));
	assert(copyout_calls == 0);
	assert(device_ioctl_calls == 0);
}

static void
wlan_common_abi_validation_test(void)
{
	union test_wlan_request request;

	wlan_request_init(&request, SIOCGWLANSTATUS, "wlan0");
	request.header.version++;
	expect_malformed_wlan_request(&request, SIOCGWLANSTATUS);

	wlan_request_init(&request, SIOCGWLANSTATUS, "wlan0");
	request.header.size--;
	expect_malformed_wlan_request(&request, SIOCGWLANSTATUS);

	wlan_request_init(&request, SIOCGWLANSTATUS, "wlan0");
	request.header.ifr_name[0] = '\0';
	expect_malformed_wlan_request(&request, SIOCGWLANSTATUS);

	wlan_request_init(&request, SIOCGWLANSTATUS, "wlan0");
	memset(request.header.ifr_name, 'x', sizeof(request.header.ifr_name));
	expect_malformed_wlan_request(&request, SIOCGWLANSTATUS);
}

static void
wlan_dispatch_test(void)
{
	static const unsigned long commands[] = {
		SIOCSWLANSCAN,       SIOCGWLANSCAN, SIOCGWLANBSS,
		SIOCSWLANCONNECT,    SIOCSWLANDISCONNECT,
		SIOCGWLANSTATUS,
	};
	static const uint8_t secret[] = {'p', 'a', 's', 's',
					 'w', 'o', 'r', 'd'};
	union test_wlan_request request;
	size_t size;
	unsigned index;

	for (index = 0; index < sizeof(commands) / sizeof(commands[0]); index++) {
		reset_observations();
		wlan_request_init(&request, commands[index], "wlan0");
		if (commands[index] == SIOCSWLANCONNECT) {
			memcpy(request.connect.passphrase, secret, sizeof(secret));
			request.connect.passphrase_length = sizeof(secret);
		}
		size = wlan_request_size(commands[index]);
		caller.euid = wlan_command_is_query(commands[index]) ? 1000 : 0;
		expected_user_argument = (uintptr_t)&request;
		assert(inet_socket_ioctl(NULL, commands[index],
					 (uintptr_t)&request) == 0);
		assert(copyin_calls == 1);
		assert(last_copyin_size == size);
		assert(device_ioctl_calls == 1);
		assert(device_ioctl_request == commands[index]);
		assert(device_argument_was_kernel_local);
		assert(copyout_calls == 1);
		assert(last_copyout_size == size);
		assert(copyout_source_was_kernel_local);
		assert(wlan_request_generation(&request, commands[index]) ==
		       TEST_OUTPUT_GENERATION);
		if (wlan_command_is_query(commands[index])) {
			assert(credential_refs == 0);
			assert(credential_releases == 0);
		} else {
			assert(credential_refs == 1);
			assert(credential_releases == 1);
		}
		if (commands[index] == SIOCSWLANCONNECT) {
			assert(captured_passphrase_length == sizeof(secret));
			assert(memcmp(captured_passphrase, secret,
				      sizeof(secret)) == 0);
			assert(request.connect.passphrase_length == 0);
			assert(buffer_is_zero(request.connect.passphrase,
					      sizeof(request.connect.passphrase)));
			assert(connect_copyout_was_redacted);
		}
	}
}

static void
wlan_error_path_test(void)
{
	static const uint8_t secret[] = {'p', 'a', 's', 's',
					 'w', 'o', 'r', 'd'};
	union test_wlan_request before;
	union test_wlan_request request;
	size_t size;

	/* A well-formed mutation is rejected before copying any user secret. */
	reset_observations();
	wlan_request_init(&request, SIOCSWLANCONNECT, "wlan0");
	memcpy(request.connect.passphrase, secret, sizeof(secret));
	request.connect.passphrase_length = sizeof(secret);
	caller.euid = 1000;
	expected_user_argument = (uintptr_t)&request;
	assert(inet_socket_ioctl(NULL, SIOCSWLANCONNECT,
				 (uintptr_t)&request) == EPERM);
	assert(credential_refs == 1);
	assert(credential_releases == 1);
	assert(copyin_calls == 0);
	assert(device_ioctl_calls == 0);
	assert(copyout_calls == 0);

	/* A real network device without WLAN capability is never dispatched. */
	reset_observations();
	wlan_request_init(&request, SIOCGWLANSTATUS, "eth0");
	caller.euid = 1000;
	expected_user_argument = (uintptr_t)&request;
	assert(inet_socket_ioctl(NULL, SIOCGWLANSTATUS,
				 (uintptr_t)&request) == EOPNOTSUPP);
	assert(credential_refs == 0);
	assert(copyin_calls == 1);
	assert(device_ioctl_calls == 0);
	assert(copyout_calls == 0);

	/* A valid ABI request still requires a live named device. */
	reset_observations();
	wlan_request_init(&request, SIOCGWLANSTATUS, "missing0");
	caller.euid = 1000;
	expected_user_argument = (uintptr_t)&request;
	assert(inet_socket_ioctl(NULL, SIOCGWLANSTATUS,
				 (uintptr_t)&request) == ENODEV);
	assert(copyin_calls == 1);
	assert(device_ioctl_calls == 0);
	assert(copyout_calls == 0);

	/* A driver failure must not publish the driver's partial output. */
	reset_observations();
	wlan_request_init(&request, SIOCGWLANSTATUS, "wlan0");
	before = request;
	caller.euid = 1000;
	device_ioctl_error = EIO;
	expected_user_argument = (uintptr_t)&request;
	assert(inet_socket_ioctl(NULL, SIOCGWLANSTATUS,
				 (uintptr_t)&request) == EIO);
	assert(device_ioctl_calls == 1);
	assert(copyout_calls == 0);
	assert(memcmp(&request, &before,
		      sizeof(struct wlan_status_request)) == 0);

	/* copyin failure terminates before lookup and dispatch. */
	reset_observations();
	wlan_request_init(&request, SIOCGWLANSTATUS, "wlan0");
	caller.euid = 1000;
	copyin_error = EFAULT;
	expected_user_argument = (uintptr_t)&request;
	assert(inet_socket_ioctl(NULL, SIOCGWLANSTATUS,
				 (uintptr_t)&request) == EFAULT);
	assert(copyin_calls == 1);
	assert(last_copyin_size == sizeof(struct wlan_status_request));
	assert(device_ioctl_calls == 0);
	assert(copyout_calls == 0);

	/* Even a failing copyout sees only a redacted kernel connect request. */
	reset_observations();
	wlan_request_init(&request, SIOCSWLANCONNECT, "wlan0");
	memcpy(request.connect.passphrase, secret, sizeof(secret));
	request.connect.passphrase_length = sizeof(secret);
	size = sizeof(struct wlan_connect_request);
	caller.euid = 0;
	copyout_error = EFAULT;
	expected_user_argument = (uintptr_t)&request;
	assert(inet_socket_ioctl(NULL, SIOCSWLANCONNECT,
				 (uintptr_t)&request) == EFAULT);
	assert(credential_refs == 1);
	assert(credential_releases == 1);
	assert(copyin_calls == 1);
	assert(last_copyin_size == size);
	assert(device_ioctl_calls == 1);
	assert(copyout_calls == 1);
	assert(last_copyout_size == size);
	assert(connect_copyout_was_redacted);
	assert(request.connect.passphrase_length == sizeof(secret));
	assert(memcmp(request.connect.passphrase, secret, sizeof(secret)) == 0);
}

int
main(void)
{
	static const unsigned long mutations[] = {
		SIOCADDRT, SIOCDELRT, SIOCSIFFLAGS, SIOCSIFADDR,
		SIOCSIFNETMASK, SIOCSIFBRDADDR, SIOCSWLANSCAN,
		SIOCSWLANCONNECT, SIOCSWLANDISCONNECT,
	};
	static const unsigned long queries[] = {
		SIOCGRTENTRY, SIOCGIFCONF, SIOCGIFNAME, SIOCGIFINDEX,
		SIOCGIFFLAGS, SIOCGIFHWADDR, SIOCGIFADDR,
		SIOCGIFNETMASK, SIOCGIFBRDADDR, SIOCGIFMTU,
		SIOCGIFSTATS, SIOCGWLANSCAN, SIOCGWLANBSS,
		SIOCGWLANSTATUS,
	};
	unsigned index;

	net_device_registry_init();
	wlan_device = create_device("wlan0", NET_DEVICE_CAP_WLAN);
	(void)create_device("eth0", 0);

	for (index = 0; index < sizeof(mutations) / sizeof(mutations[0]); index++) {
		expect_nonroot_denied(mutations[index]);
		expect_root_reaches_argument_check(mutations[index]);
	}
	for (index = 0; index < sizeof(queries) / sizeof(queries[0]); index++)
		expect_nonroot_query(queries[index]);

	/* Unknown/private commands are privileged by default. */
	expect_nonroot_denied(0xdeadbeefUL);
	wlan_bad_encoding_test();
	wlan_common_abi_validation_test();
	wlan_dispatch_test();
	wlan_error_path_test();
	puts("inet/WLAN ioctl authorization and dispatch tests: PASS");
	return 0;
}
