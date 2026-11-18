/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Ethernet page's editor of a wired interface's IPv4 configuration
 * (ws089-p022), without its drawing (page-wired.c): opened on an
 * interface with what it has now, checked before it is sent, sent through
 * the desktop's network (network.c, kl_system_network_configure_wired),
 * and closed by a good answer.
 *
 * The checks here say early what is wrong with a field; the network daemon
 * checks every value again and is the one that decides.
 */

#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The words of the fields, for the messages. */
static const char *const wired_names[SE_WIRED_FIELDS] = { "address", "subnet mask", "router", "first DNS server", "second DNS server" };

static int wired_parse(const char *text, uint32_t *address);
static int wired_prefix(uint32_t mask);
static int wired_unicast(uint32_t address);
static void wired_set(struct kl_field *field, const char *text);
static void wired_copy(char *destination, size_t size, const char *source);

/*
 * Opens the editor on a wired interface, with how it is configured, its
 * address, netmask and router now, and the DNS servers now.
 */
void
se_wired_edit(
	struct se_app *app,
	const struct kl_network_link *link)
{
	struct se_wired *wired;
	const struct se_network *network;
	size_t index;

	/* The interface, and its mode (a static one stays static, anything else is DHCP). */
	wired = &app->wired;
	network = &app->network;
	memset(wired, 0, sizeof(*wired));
	(void)snprintf(wired->interface, sizeof(wired->interface), "%s", link->name);
	wired->mode = KL_WIRED_DHCP;
	if (link->wired_mode == KL_WIRED_STATIC)
		wired->mode = KL_WIRED_STATIC;

	/* What it has now, as the fields' start. */
	wired_set(&wired->fields[SE_WIRED_ADDRESS], link->address);
	wired_set(&wired->fields[SE_WIRED_NETMASK], link->netmask);
	wired_set(&wired->fields[SE_WIRED_ROUTER], link->router);
	for (index = 0; index < 2U && index < network->dns_count; index++)
		wired_set(&wired->fields[SE_WIRED_DNS1 + (int)index], network->dns[index]);

	/* The first field that counts has the keyboard. */
	wired->focus = SE_WIRED_DNS1;
	if (wired->mode == KL_WIRED_STATIC)
		wired->focus = SE_WIRED_ADDRESS;
	app->dirty = 1;
	se_log("WIRED edit interface=%s mode=%u", wired->interface, wired->mode);
}

/*
 * Closes the editor without asking anything.
 */
void
se_wired_cancel(
	struct se_app *app)
{
	/* Nothing edited from now on. */
	memset(&app->wired, 0, sizeof(app->wired));
	app->dirty = 1;
}

/*
 * Checks the editor's fields: with a static address, the address, the
 * netmask (contiguous, /1 to /30) and the router (empty, or an address of
 * the same subnet that is not the interface's); the DNS servers (empty, or
 * an address that can be reached; the second only with the first).
 * Returns 0 when they are good, or the field that is not plus one, with a
 * message.
 */
int
se_wired_check(
	const struct se_wired *wired,
	char *message,
	size_t size)
{
	uint32_t address;
	uint32_t mask;
	uint32_t router;
	uint32_t server;
	int prefix;
	int error;
	int index;
	int usable;

	/* A static address: the address and the netmask. */
	message[0] = '\0';
	address = 0U;
	mask = 0U;
	if (wired->mode == KL_WIRED_STATIC) {
		error = wired_parse(wired->fields[SE_WIRED_ADDRESS].text, &address);
		usable = wired_unicast(address);
		if (error != 0 || !usable) {
			(void)snprintf(message, size, "Type an IPv4 address such as 192.168.1.20.");
			return SE_WIRED_ADDRESS + 1;
		}

		/* The netmask. */
		error = wired_parse(wired->fields[SE_WIRED_NETMASK].text, &mask);
		prefix = wired_prefix(mask);
		if (error != 0 || prefix < 1 || prefix > 30) {
			(void)snprintf(message, size, "Type a subnet mask such as 255.255.255.0.");
			return SE_WIRED_NETMASK + 1;
		}

		/* Not the subnet's own address nor its broadcast. */
		if ((address & ~mask) == 0U || (address & ~mask) == ~mask) {
			(void)snprintf(message, size, "The address is the subnet's own or its broadcast; choose another.");
			return SE_WIRED_ADDRESS + 1;
		}

		/* The router, when there is one: in the subnet, and not the interface. */
		if (wired->fields[SE_WIRED_ROUTER].length != 0U) {
			error = wired_parse(wired->fields[SE_WIRED_ROUTER].text, &router);
			usable = wired_unicast(router);
			if (error != 0 || !usable || (router & mask) != (address & mask) || router == address ||
			    (router & ~mask) == 0U || (router & ~mask) == ~mask) {
				(void)snprintf(message, size, "The router must be another address of the same subnet.");
				return SE_WIRED_ROUTER + 1;
			}
		}
	}

	/* The DNS servers, each empty or an address that can be reached. */
	for (index = SE_WIRED_DNS1; index <= SE_WIRED_DNS2; index++) {
		if (wired->fields[index].length == 0U)
			continue;
		error = wired_parse(wired->fields[index].text, &server);
		usable = wired_unicast(server);
		if (error != 0 || !usable) {
			(void)snprintf(message, size, "The %s is not an IPv4 address that can be reached.", wired_names[index]);
			return index + 1;
		}
	}

	/* The second only with the first. */
	if (wired->fields[SE_WIRED_DNS2].length != 0U && wired->fields[SE_WIRED_DNS1].length == 0U) {
		(void)snprintf(message, size, "Type the first DNS server before the second.");
		return SE_WIRED_DNS1 + 1;
	}

	/* Succeeded: the fields are good. */
	return 0;
}

/*
 * Applies the editor's configuration: checked, then asked of the desktop;
 * a field that is not good, or an asking that fails, says so instead.
 */
void
se_wired_apply(
	struct se_app *app)
{
	struct kl_network_wired_config config;
	struct se_wired *wired;
	int field;
	int error;

	/* Nothing edited, or an answer still awaited. */
	wired = &app->wired;
	if (wired->interface[0] == '\0' || wired->asked)
		return;

	/* The fields, checked; the first that is not good takes the keyboard. */
	field = se_wired_check(wired, wired->message, sizeof(wired->message));
	if (field != 0) {
		wired->focus = field - 1;
		wired->message_bad = 1;
		app->dirty = 1;
		return;
	}

	/* The configuration: a static one's address, netmask and router, and the DNS servers. */
	memset(&config, 0, sizeof(config));
	wired_copy(config.interface, sizeof(config.interface), wired->interface);
	config.mode = wired->mode;
	if (wired->mode == KL_WIRED_STATIC) {
		wired_copy(config.address, sizeof(config.address), wired->fields[SE_WIRED_ADDRESS].text);
		wired_copy(config.netmask, sizeof(config.netmask), wired->fields[SE_WIRED_NETMASK].text);
		wired_copy(config.router, sizeof(config.router), wired->fields[SE_WIRED_ROUTER].text);
	}

	/* The DNS servers, with either mode. */
	wired_copy(config.dns[0], sizeof(config.dns[0]), wired->fields[SE_WIRED_DNS1].text);
	wired_copy(config.dns[1], sizeof(config.dns[1]), wired->fields[SE_WIRED_DNS2].text);

	/* Asked; the answer comes to se_wired_outcome. */
	error = se_network_configure_wired(app, &config);
	if (error != 0) {
		se_wired_outcome(app, error);
		return;
	}

	/* The editor waits for the answer, and says so. */
	wired->asked = 1;
	(void)snprintf(wired->message, sizeof(wired->message), "Applying the settings of %s...", wired->interface);
	wired->message_bad = 0;
	app->dirty = 1;
}

/*
 * Takes the answer of an Apply (or the failure of asking): a good one closes
 * the editor, any other says why it did not happen.
 */
void
se_wired_outcome(
	struct se_app *app,
	int error)
{
	struct se_wired *wired;
	const char *message;
	char name[KL_NETWORK_NAME_MAX];

	/* The editor waits no longer. */
	wired = &app->wired;
	wired->asked = 0;
	(void)snprintf(name, sizeof(name), "%s", wired->interface);
	se_log("WIRED result interface=%s errno=%d", name, error);
	app->dirty = 1;

	/* A good answer closes the editor and says it is done. */
	if (error == 0) {
		memset(wired, 0, sizeof(*wired));
		(void)snprintf(wired->message, sizeof(wired->message), "The settings of %s are applied and kept.", name);
		return;
	}

	/* Why it did not happen. */
	switch (error) {
	case EPERM:
	case EACCES:
		message = "Only a member of the network group may change the wired settings.";
		break;
	case EINVAL:
		message = "The network service did not accept these values. Check them and try again.";
		break;
	case ENOTSUP:
		message = "This desktop cannot change the wired settings.";
		break;
	case EBUSY:
		message = "Another network change is under way. Try again in a moment.";
		break;
	case ENODEV:
		message = "The network service is not running.";
		break;
	default:
		message = "The settings could not be applied.";
		break;
	}

	/* The reason, shown in red. */
	(void)snprintf(wired->message, sizeof(wired->message), "%s", message);
	wired->message_bad = 1;
}

/*
 * Types a key into the field with the keyboard: digits and dots only, up
 * to an address's 15 characters, the editing keys.  Returns 1 when it was
 * the field's.
 */
int
se_wired_type(
	struct se_wired *wired,
	const struct se_event *event)
{
	struct kl_field *field;
	uint32_t character;
	int used;

	/* A character other than a digit or a dot is taken and dropped. */
	field = &wired->fields[wired->focus];
	character = kl_key_character(event->key, event->modifiers);
	if (character != 0U) {
		if (character < '0' || character > '9') {
			if (character != '.')
				return 1;
		}
	}

	/* An address has at most fifteen characters. */
	if (character != 0U && field->length >= KL_NETWORK_ADDRESS_MAX - 1U)
		return 1;

	/* The field with the keyboard takes the key as any field does (in its next frame). */
	used = se_field_key(field, event);
	return used;
}

/* Reads a dotted IPv4 address (four numbers 0 to 255, no leading zero but a lone 0); returns 0 or EINVAL. */
static int
wired_parse(
	const char *text,
	uint32_t *address)
{
	const char *cursor;
	uint32_t value;
	unsigned part;
	unsigned digits;
	unsigned number;

	/* Four numbers between dots. */
	value = 0U;
	cursor = text;
	for (part = 0U; part < 4U; part++) {
		/* Each number: one to three digits, 0 to 255, no leading zero. */
		number = 0U;
		digits = 0U;
		while (*cursor >= '0' && *cursor <= '9') {
			if (digits == 1U && number == 0U)
				return EINVAL;
			number = number * 10U + (unsigned)(*cursor - '0');
			digits++;
			cursor++;
			if (digits > 3U || number > 255U)
				return EINVAL;
		}

		/* At least one digit makes the number. */
		if (digits == 0U)
			return EINVAL;
		value = (value << 8) | number;

		/* A dot between, nothing after the last. */
		if (part < 3U) {
			if (*cursor != '.')
				return EINVAL;
			cursor++;
		}
	}

	/* Nothing after the fourth number. */
	if (*cursor != '\0')
		return EINVAL;

	/* Succeeded: the address in host order. */
	*address = value;
	return 0;
}

/* Gives a netmask's prefix length, or -1 when its ones are not contiguous. */
static int
wired_prefix(
	uint32_t mask)
{
	uint32_t inverse;
	int prefix;

	/* The ones from the top, then only zeros. */
	inverse = ~mask;
	if ((inverse & (inverse + 1U)) != 0U)
		return -1;
	prefix = 0;
	while (prefix < 32 && (mask & (0x80000000U >> prefix)) != 0U)
		prefix++;

	/* Succeeded: the prefix length. */
	return prefix;
}

/* Tells whether an address can be given to a machine: not 0.x, the loopback, multicast or above, nor the broadcast. */
static int
wired_unicast(
	uint32_t address)
{
	uint32_t first;

	/* The first number. */
	first = address >> 24;
	if (first == 0U || first == 127U || first >= 224U)
		return 0;

	/* Succeeded: an address a machine may have. */
	return 1;
}

/* Puts a text into a field (empty for none). */
static void
wired_set(
	struct kl_field *field,
	const char *text)
{
	/* The text, as far as it fits. */
	se_field_clear(field);
	(void)snprintf(field->text, sizeof(field->text), "%s", text);
	field->length = strlen(field->text);
}

/* Copies a text into a room, cut to fit (a checked field always fits). */
static void
wired_copy(
	char *destination,
	size_t size,
	const char *source)
{
	size_t length;

	/* As much as fits, and the end. */
	length = strlen(source);
	if (length >= size)
		length = size - 1U;
	memcpy(destination, source, length);
	destination[length] = '\0';
}
