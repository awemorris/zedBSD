/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares networkd's wired interface policy.
 *
 * What to do with a wired interface follows from two things: whether a
 * cable is in it, and what the configuration says it should be.  The first
 * is told to the daemon by the kernel, as a carrier change on the route
 * socket; the second is handed over by the net command, which owns the
 * file it is written in.  This holds both, and says what to do next.
 *
 * Nothing here opens a device or runs a command.  Deciding and doing are
 * kept apart so that the deciding can be read, and tested, on its own.
 */

#ifndef KERN_NETWORKD_MANAGED_LAN_H
#define KERN_NETWORKD_MANAGED_LAN_H

#include <net/if.h>
#include <net/route.h>
#include <stddef.h>
#include <stdint.h>

#define NETWORKD_LAN_MAX 16U
#define NETWORKD_LAN_ADDRESS_MAX 16U

/* What the configuration asks for an interface. */
enum networkd_lan_mode {
	/* Written down as disabled: the daemon leaves it alone. */
	NETWORKD_LAN_MODE_DISABLED,
	NETWORKD_LAN_MODE_DHCP,
	NETWORKD_LAN_MODE_STATIC
};

/* One interface as the configuration describes it. */
struct networkd_lan_policy {
	char interface[IFNAMSIZ];
	enum networkd_lan_mode mode;
	char address[NETWORKD_LAN_ADDRESS_MAX];
	char netmask[NETWORKD_LAN_ADDRESS_MAX];
	unsigned dhcp_timeout;
};

/* Where an interface has got to. */
enum networkd_lan_state {
	/* No cable, or nothing asked of it. */
	NETWORKD_LAN_IDLE,

	/* A cable is in it and it has not been configured yet. */
	NETWORKD_LAN_PENDING,

	/* Configured, and holding an address it can be reached at. */
	NETWORKD_LAN_CONFIGURED,

	/*
	 * A cable is in it, configuration was tried and did not succeed.
	 * It carries a link-local address, which says exactly that.
	 */
	NETWORKD_LAN_UNCONFIGURED
};

/* What the daemon is to do next, and to which interface. */
enum networkd_lan_action {
	NETWORKD_LAN_ACTION_NONE,

	/* Bring it up and give it what the configuration asks for. */
	NETWORKD_LAN_ACTION_CONFIGURE,

	/* The cable is out, or management stopped: take it down. */
	NETWORKD_LAN_ACTION_DOWN,

	/* An event was lost; read every interface again before deciding. */
	NETWORKD_LAN_ACTION_RESNAPSHOT,

	/*
	 * Bring an interface up that has no cable yet, without configuring
	 * it.  A USB adapter reports its link only once it is up, so an
	 * interface left down would never be seen to get a cable.
	 */
	NETWORKD_LAN_ACTION_RAISE
};

struct networkd_lan_interface {
	char name[IFNAMSIZ];
	uint32_t ifindex;
	uint64_t generation;
	enum networkd_lan_state state;
	int carrier;

	/*
	 * Brought up by the daemon to watch its link.  Cleared when the
	 * daemon takes it down, so it is raised again to see the next cable.
	 */
	int raised;

	/* Cleared before a snapshot and set again by it, to find removals. */
	int present;
};

struct networkd_lan {
	int enabled;
	int resnapshot_due;
	struct networkd_lan_policy policy[NETWORKD_LAN_MAX];
	size_t policy_count;
	struct networkd_lan_interface interfaces[NETWORKD_LAN_MAX];
	size_t interface_count;
};

/* One thing to do, as decided from the state and the policy. */
struct networkd_lan_work {
	enum networkd_lan_action action;
	char interface[IFNAMSIZ];
	struct networkd_lan_policy policy;
};

void networkd_lan_init(struct networkd_lan *);

/*
 * Replaces what the configuration says.  An interface the configuration no
 * longer names is no longer managed, and is left as it stands: the daemon
 * took no decision about it and undoes none.
 */
int networkd_lan_set_policy(struct networkd_lan *,
			    const struct networkd_lan_policy *, size_t);

int networkd_lan_enable(struct networkd_lan *);
int networkd_lan_disable(struct networkd_lan *);

/*
 * Records that an interface exists and whether a cable is in it.  Called
 * once per interface when the daemon reads them all, which it does when
 * management starts and whenever an event says one was lost.
 */
int networkd_lan_observe(struct networkd_lan *, const char *, uint32_t,
			 uint64_t, int carrier);

/* Marks every interface absent, before a snapshot names the ones that are there. */
void networkd_lan_snapshot_begin(struct networkd_lan *);

/* Forgets the interfaces the snapshot did not name. */
void networkd_lan_snapshot_end(struct networkd_lan *);

/* Folds one route-socket event into the state. */
enum networkd_lan_action networkd_lan_event(struct networkd_lan *,
					    const struct rtm_ifinfo *);

/*
 * Says what to do next, or that there is nothing.  The caller does it and
 * then reports what happened, so that the same thing is not decided twice.
 */
int networkd_lan_next(const struct networkd_lan *, struct networkd_lan_work *);

/*
 * Records the outcome of a configure.  Whether an address was obtained is
 * what tells a configured interface from one that only has a cable.
 */
int networkd_lan_configured(struct networkd_lan *, const char *, int obtained);

/* Records that an interface was taken down. */
int networkd_lan_down(struct networkd_lan *, const char *);
int networkd_lan_raised(struct networkd_lan *, const char *);

/*
 * Derives the link-local address an interface falls back to, from its
 * hardware address.  RFC 3927 leaves the choice to the implementation; a
 * value derived from the hardware address is stable, which means a machine
 * that could not be configured is at least found at the same place twice.
 * The first and last /24 of the range are reserved, and are skipped.
 */
int networkd_lan_link_local(const unsigned char *, size_t, char *, size_t);

#endif
