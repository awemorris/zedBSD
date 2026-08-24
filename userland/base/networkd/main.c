/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/net/netutil.h"
#include "userland/base/service/service-config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define NETWORKD_SOCKET "/run/networkd.sock"

struct managed_interface {
	char name[IFNAMSIZ];
	char mode[16];
	char address[64];
	int online;
};

static struct managed_interface interfaces[16];
static size_t interface_count;
static volatile sig_atomic_t stopping;
static volatile sig_atomic_t reloading;

static void
handle_signal(int signal_number)
{
	if (signal_number == SIGHUP)
		reloading = 1;
	else
		stopping = 1;
}

static int
set_flags(int descriptor, const char *name, int up)
{
	struct ifreq request;
	if (netutil_ifreq(&request, name) != 0 ||
	    ioctl(descriptor, SIOCGIFFLAGS, &request) != 0)
		return -1;
	if (up)
		request.ifr_flags |= IFF_UP;
	else
		request.ifr_flags &= ~IFF_UP;
	return ioctl(descriptor, SIOCSIFFLAGS, &request);
}

static int
set_address(int descriptor, const char *name, unsigned long operation,
	    struct in_addr address)
{
	struct ifreq request;
	struct sockaddr_in *inet;
	if (netutil_ifreq(&request, name) != 0)
		return -1;
	inet = (struct sockaddr_in *)&request.ifr_addr;
	inet->sin_family = AF_INET;
	inet->sin_addr = address;
	return ioctl(descriptor, operation, &request);
}

static int
configure_static(int descriptor, struct managed_interface *interface)
{
	struct in_addr address, mask, broadcast;
	unsigned prefix;
	if (netutil_parse_cidr(interface->address, &address, &mask, &prefix) !=
	    0)
		return -1;
	(void)prefix;
	broadcast.s_addr = address.s_addr | ~mask.s_addr;
	return set_flags(descriptor, interface->name, 1) != 0 ||
		       set_address(descriptor, interface->name, SIOCSIFNETMASK,
				   mask) != 0 ||
		       set_address(descriptor, interface->name, SIOCSIFBRDADDR,
				   broadcast) != 0 ||
		       set_address(descriptor, interface->name, SIOCSIFADDR,
				   address) != 0
		   ? -1
		   : 0;
}

static int
configure_dhcp(const char *name)
{
	char *arguments[] = {"dhcpcd", (char *)name, NULL};
	pid_t child = fork();
	int status;
	if (child == 0) {
		execv("/sbin/dhcpcd", arguments);
		_exit(127);
	}
	return child > 0 && waitpid(child, &status, 0) == child &&
		       WIFEXITED(status) && WEXITSTATUS(status) == 0
		   ? 0
		   : -1;
}

static int
load_configuration(void)
{
	char list[512], copy[512], *name;
	interface_count = 0;
	if (rcconf_get(ZEDBSD_RC_CONF, "network_interfaces", list,
		       sizeof(list)) != 0)
		return -1;
	strcpy(copy, list);
	for (name = strtok(copy, " \t,"); name != NULL;
	     name = strtok(NULL, " \t,")) {
		struct managed_interface *interface;
		char key[96];
		if (interface_count ==
			sizeof(interfaces) / sizeof(interfaces[0]) ||
		    strlen(name) >= IFNAMSIZ)
			return -1;
		interface = &interfaces[interface_count++];
		memset(interface, 0, sizeof(*interface));
		strcpy(interface->name, name);
		snprintf(key, sizeof(key), "if_%s_mode", name);
		if (rcconf_get(ZEDBSD_RC_CONF, key, interface->mode,
			       sizeof(interface->mode)) != 0)
			strcpy(interface->mode, "static");
		snprintf(key, sizeof(key), "if_%s_address", name);
		(void)rcconf_get(ZEDBSD_RC_CONF, key, interface->address,
				 sizeof(interface->address));
	}
	return 0;
}

static void
configure_all(int descriptor)
{
	size_t index;
	for (index = 0; index < interface_count; index++) {
		struct managed_interface *interface = &interfaces[index];
		int result = strcmp(interface->mode, "dhcp") == 0
				 ? configure_dhcp(interface->name)
				 : configure_static(descriptor, interface);
		interface->online = result == 0;
		printf("networkd: %s %s\n", interface->name,
		       result == 0 ? "online" : "failed");
	}
}

static struct managed_interface *
find_interface(const char *name)
{
	size_t index;
	for (index = 0; index < interface_count; index++)
		if (strcmp(interfaces[index].name, name) == 0)
			return &interfaces[index];
	return NULL;
}

static void
handle_request(int client, int control)
{
	char request[160], response[256], *command, *name;
	ssize_t length = read(client, request, sizeof(request) - 1);
	struct managed_interface *interface;
	size_t index;
	if (length <= 0)
		return;
	request[length] = '\0';
	command = strtok(request, " \t\r\n");
	name = strtok(NULL, " \t\r\n");
	if (command != NULL && strcmp(command, "reload") == 0) {
		reloading = 1;
		(void)write(client, "OK reload scheduled\n", 20);
		return;
	}
	if (command != NULL && strcmp(command, "show") == 0 && name == NULL) {
		for (index = 0; index < interface_count; index++) {
			snprintf(response, sizeof(response), "%s\t%s\t%s\n",
				 interfaces[index].name, interfaces[index].mode,
				 interfaces[index].online ? "online"
							  : "offline");
			(void)write(client, response, strlen(response));
		}
		return;
	}
	interface = name != NULL ? find_interface(name) : NULL;
	if (interface == NULL) {
		(void)write(client, "ERR unknown interface\n", 22);
		return;
	}
	if (strcmp(command, "show") == 0) {
		snprintf(response, sizeof(response), "OK %s %s %s\n",
			 interface->name, interface->mode,
			 interface->online ? "online" : "offline");
		(void)write(client, response, strlen(response));
	} else if (strcmp(command, "up") == 0) {
		interface->online = set_flags(control, interface->name, 1) == 0;
		(void)write(client,
			    interface->online ? "OK up\n" : "ERR up failed\n",
			    interface->online ? 6 : 14);
	} else if (strcmp(command, "down") == 0) {
		interface->online = set_flags(control, interface->name, 0) != 0;
		(void)write(client,
			    !interface->online ? "OK down\n"
					       : "ERR down failed\n",
			    !interface->online ? 8 : 16);
	} else if (strcmp(command, "dhcp") == 0) {
		interface->online = configure_dhcp(interface->name) == 0;
		(void)write(client,
			    interface->online ? "OK dhcp\n"
					      : "ERR dhcp failed\n",
			    interface->online ? 8 : 16);
	} else
		(void)write(client, "ERR unknown command\n", 20);
}

static int
open_listener(void)
{
	struct sockaddr_un address;
	int descriptor =
	    socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, NETWORKD_SOCKET);
	(void)unlink(NETWORKD_SOCKET);
	if (bind(descriptor, (struct sockaddr *)&address, sizeof(address)) !=
		0 ||
	    chmod(NETWORKD_SOCKET, 0600) != 0 || listen(descriptor, 8) != 0) {
		close(descriptor);
		return -1;
	}
	(void)fcntl(descriptor, F_SETFL, O_NONBLOCK);
	return descriptor;
}

int
main(void)
{
	int listener, control = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	(void)signal(SIGHUP, handle_signal);
	(void)signal(SIGTERM, handle_signal);
	(void)signal(SIGINT, handle_signal);
	if (control < 0 || load_configuration() != 0) {
		fprintf(stderr, "networkd: configuration failed: %s\n",
			strerror(errno));
		return 1;
	}
	configure_all(control);
	listener = open_listener();
	if (listener < 0)
		return 1;
	while (!stopping) {
		if (reloading) {
			reloading = 0;
			if (load_configuration() == 0)
				configure_all(control);
		}
		{
			int client = accept(listener, NULL, NULL);
			if (client >= 0) {
				handle_request(client, control);
				close(client);
				continue;
			}
		}
		sleep(1);
	}
	close(listener);
	close(control);
	unlink(NETWORKD_SOCKET);
	return 0;
}
