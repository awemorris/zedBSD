/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The wire of Keiland's system extension (WS135, plan/ws135/design.md
 * section 4.2; WS131 section 4): the opcodes of kl_system_manager_v1 and
 * kl_system_settings_v1, and the values their events carry.  The
 * compositor serves them and libkeiland speaks them; both include this
 * header and neither the other's code (WS131 D4 (c)).
 *
 * kl_system_manager_v1 (a global, version 23; its objects are made at its version)
 *   request 0 destroy
 *   request 1 get_settings(new_id kl_system_settings_v1)
 *   request 2 get_network(new_id kl_system_network_v1)    (WS131 p010)
 *   request 3 get_audio(new_id kl_system_audio_v1)
 *   request 4 get_power(new_id kl_system_power_v1)
 *   request 5 get_devices(new_id kl_system_devices_v1)
 *   request 6 get_monitor(new_id kl_system_monitor_v1, uint period_ms)    since version 2 (WS134 p012)
 *   request 7 get_account(new_id kl_system_account_v1)    since version 4 (ws160-p002)
 *   request 8 get_sharing(new_id kl_system_sharing_v1)    since version 7 (ws089-p025)
 *   request 9 get_notify(new_id kl_system_notify_v1)      since version 13 (ws156-p002)
 *   request 10 get_mail(new_id kl_system_mail_v1)         since version 15 (ws169-p002)
 *   request 11 get_phone(new_id kl_system_phone_v1)       since version 16 (ws170-p004)
 *   request 12 get_printers(new_id kl_system_printers_v1) since version 17 (ws145-p003)
 *   request 13 get_displays(new_id kl_system_displays_v1) since version 18 (ws113-p005)
 *   request 14 get_machine(new_id kl_system_machine_v1)   since version 21 (ws188-p002)
 *   request 15 get_bluetooth(new_id kl_system_bluetooth_v1) since version 23 (ws143-p006)
 *   event   0 capabilities(uint bits)              sent when it is bound
 *
 * kl_system_settings_v1
 *   request 0 destroy
 *   request 1 set(uint request, string key, string value)
 *   request 2 reset(uint request, string key)       back to the default
 *   event   0 value(string key, string value, uint flags)
 *   event   1 done(uint serial)                     the values before it are one state
 *   event   2 result(uint request, uint applied, uint saved)
 *
 * When made, a settings object hears every compositor setting's value and
 * a done; afterwards each change made by anyone (a client, the system bar,
 * audiod) comes to every settings object as its value and a done.  Every
 * set and reset is answered by one result.
 *
 * kl_system_network_v1 (WS131 p010)
 *   request 0 destroy
 *   request 1 request(uint request, uint what, string ssid)   scan, join (ssid), disconnect, Wi-Fi on, off
 *   request 2 save_key(uint request, string ssid, string key) saves the key, tells the daemon, joins
 *   request 3 query_details(uint request)                      the links, the DNS servers, the saved networks
 *   request 4 set_scanning(uint on)                            since version 3 (ws089-p021): the client shows the
 *                                                              networks around (1) or no longer (0)
 *   request 5 configure_wired(uint request, string interface, uint mode, string address, string netmask,
 *                             string router, string dns1, string dns2)
 *                                                              since version 6 (ws089-p022): a wired interface by
 *                                                              DHCP (mode 1; the strings empty but the DNS servers,
 *                                                              which may name static ones) or a static IPv4
 *                                                              address (mode 2; router and DNS may be empty), kept
 *                                                              by the network daemon for the next start too
 *   event   0 state(uint reachable, uint connected, uint kind, string interface, string wired, uint wifi,
 *                   string wifi_interface, string ssid)
 *   event   1 access_point(string ssid, int rssi, uint secured)
 *   event   2 scan_done()                           the access points before it are the whole scan
 *   event   3 link(string name, uint flags, string address, string netmask, string hardware, uint mtu,
 *                  uint received_high, uint received_low, uint sent_high, uint sent_low)
 *   event   4 dns(string address)
 *   event   5 saved_network(string ssid)
 *   event   6 details_done()                        the links, servers and networks before it are the whole details
 *   event   7 done(uint serial)
 *   event   8 result(uint request, uint applied, uint saved)
 *   event   9 wired(string name, uint mode, string router)    since version 6: after a wired interface's link in
 *                                                              the details, how it is configured (KL_SYSTEM_WIRED_*)
 *                                                              and the router it was given (empty when none)
 *   event  10 link_speed(string name, uint mbps)  since version 12 (BUG-222): after an interface's link in the
 *                                                              details, the speed its driver last heard, in Mb/s
 *                                                              (not sent while it is not known)
 *   One request of the network is outstanding at a time, the system bar's
 *   included; another is answered busy.  query_details is no request of
 *   the daemon's: every object that asked hears the next reading.  A join
 *   (and save_key's join) is answered no_key, refused or unreachable for
 *   its own failures.  set_scanning is no request and has no answer: the
 *   compositor keeps the radios scanning while any object asked for it
 *   (or the system bar's menu is open), and a new scan comes as the
 *   access points and a scan_done; the object's going ends its asking, and
 *   so does a minute without set_scanning(1) again (a client renews it
 *   while it shows the networks).
 *
 * kl_system_sharing_v1 (ws089-p025: the Sharing page's Remote Login, sshd)
 *   request 0 destroy
 *   request 1 set_ssh(uint request, uint on)          turns Remote Login on (1) or off (0), now and at
 *                                                     every start; the result follows the new state
 *   request 2 query(uint request)                     reads the state again
 *   event   0 state(uint available, uint enabled, uint running, uint port, uint allowed, string fingerprint)
 *                                                     whether the system has it, starts it, runs it, its
 *                                                     port, whether this user may change it (root or wheel),
 *                                                     the host key's fingerprint ("SHA256:...", or empty)
 *   event   1 done(uint serial)
 *   event   2 result(uint request, uint applied, uint saved)
 *   A new object hears the state last known and a done, and the state is
 *   read again for it; every change comes to every object.
 *
 * kl_system_displays_v1 (ws113-p005: the Display page's two modes, places and brightness)
 *   request 0 destroy
 *   request 1 apply(uint request, uint serial, uint mode, string places)
 *                                                     the mode (KL_SYSTEM_DISPLAYS_EXTENDED or _MIRROR)
 *                                                     and, for the extended mode, places: lines
 *                                                     "KEY X Y" (a display's key, which may hold
 *                                                     spaces, and its signed place in the logical
 *                                                     plane); a display not named keeps its place.
 *                                                     serial is the snapshot the client saw
 *   request 2 set_brightness(uint request, string key, uint percent)
 *                                                     a built-in panel's light, 0 to 100
 *   request 3 set_shown(uint request, string key, uint shown)   (since 19, ws113-p014)
 *                                                     a display turned off (0) or on (1) in the
 *                                                     extended mode; the anchor turned off moves to
 *                                                     a display on
 *   event   0 output(string key, string label, int x, int y, uint width, uint height, uint refresh_mhz,
 *                    uint flags, uint brightness)     one display connected: KL_SYSTEM_DISPLAY_* flags, the
 *                                                     light in percent (0 without one)
 *   event   1 done(uint serial, uint mode)            the outputs before it are the whole snapshot
 *   event   2 result(uint request, uint applied, uint saved)
 *   A new object hears the snapshot (every display connected, then a
 *   done); each change (a display plugged or unplugged, a choice applied,
 *   the light changed by anyone or by the light keys) comes to every
 *   object as a whole snapshot.  An apply whose serial is not the last
 *   snapshot's is answered stale and changes nothing; places that overlap,
 *   do not join or are out of range are answered invalid; a session not
 *   active (the login screen, a locked one) is denied.  A choice applied
 *   but not written to displays.conf is answered applied 1, saved 0.  The
 *   light of a display without one is unsupported.  A display turned off
 *   carries the flag OFF (also in the mirror, which shows it still); turning
 *   off the last display on, or any in the mirror, is invalid; a key not
 *   connected is not found.
 *
 * kl_system_machine_v1 (ws188-p002, plan/ws188/phase001/phase.md section D1: what Settings reads of the computer)
 *   request 0 destroy
 *   request 1 query(uint request, uint what)          what: KL_SYSTEM_MACHINE_ABOUT, _FILESYSTEMS, _USERS,
 *                                                     _LOGIN_LANGUAGE (0 or another bit is invalid)
 *   event   0 parts(uint request, uint what)          the answer of request starts: the parts it holds
 *   event   1 about(string system, string kernel, string architecture, string processor, string host, uint cpus)
 *   event   2 filesystem(string path, uint total_high, uint total_low, uint available_high, uint available_low,
 *                        uint used_high, uint used_low)
 *   event   3 user(string name, string full_name, string home, uint flags)   KL_SYSTEM_MACHINE_USER_*
 *   event   4 login_language(string code)             "en", "ja", or "" (no file, or another word)
 *   event   5 result(uint request, uint applied, uint saved)
 *   event   6 mount(string path, string type)         since version 22 (ws188-p004): a mounted file system a
 *                                                     user may keep files on (the part _MOUNTS, a query of an
 *                                                     object of version 21 asking it is invalid)
 *   The compositor reads on a thread of its own (a file system's size or a directory service may wait),
 *   one reading at a time; a query that comes during one waits for the next (at most 16 waiting, 4 of a
 *   client; one more is answered busy), and a reading reads what its waiting queries asked together.  An
 *   answer is parts, the events of the parts asked (one about, the file systems and the users each whole,
 *   replacing the earlier list, none meaning there are none, one login_language), then result ok; nothing
 *   else of the same client comes between them.  A failure is a result alone: busy, unavailable (the
 *   client's queue has no room for the whole answer), invalid, failed (no thread).  The users are the
 *   people's accounts and the client's own (SELF), whose home alone is sent; a name that does not fit is
 *   left out, never cut.
 *
 * kl_system_audio_v1
 *   request 0 destroy
 *   request 1 set_volume(uint request, uint left, uint right, uint muted)
 *   request 2 feedback(uint request)
 *   event   0 state(uint reachable, uint device, uint rate, uint channels, uint left, uint right, uint muted)
 *   event   1 done(uint serial)
 *   event   2 result(uint request, uint applied, uint saved)
 *
 * kl_system_power_v1
 *   request 0 destroy
 *   request 1 action(uint request, uint action)              1 power off, 2 restart, 3 suspend
 *   event   0 state(uint source, int percent, uint charging, uint actions)
 *   event   1 done(uint serial)
 *   event   2 result(uint request, uint applied, uint saved)
 *
 * kl_system_devices_v1 (WS132: the removable media, ws132-p004)
 *   request 0 destroy
 *   request 1 eject(uint request, string id)
 *   request 2 mount(uint request, string id)                  since version 5
 *   event   0 device(string id, uint kind, uint state, string name, string location)
 *   event   1 done(uint serial)
 *   event   2 result(uint request, uint applied, uint saved)
 *   event   3 busy(uint request, string program)              since version 5: before a busy result, the program
 *                                                             that keeps the volume from being ejected
 *   event   4 volume(string id, string fs, uint bytes_high, uint bytes_low)
 *                                                             since version 9 (ws132-p009): after each device, its
 *                                                             file system ("fat", "ufs") and size in bytes, which
 *                                                             a mount's confirmation shows
 *   The devices are the volumes volumed lists (zedBSD): kind 1 (removable storage), state the
 *   KL_SYSTEM_DEVICE_* bits (mounted; new: inserted and never mounted since), name the label (the
 *   disk's name without one), location where it is mounted ("" when it is not).  Each object hears
 *   the whole list and a done when it is made and whenever it changes (a device not in the list is
 *   gone).  A mount puts the volume under /media (nosuid, noexec, the user its owner); an eject
 *   unmounts it (busy while a program uses it).  Only the seat's user may (denied otherwise).
 *
 * kl_system_account_v1 (ws160-p002: the user's own account)
 *   request 0 destroy
 *   request 1 set_password(uint request, string current, string new)
 *   event   0 result(uint request, uint applied, uint saved)
 *   request 2 administer(uint request, string password, string operation)   since version 8 (ws089-p026)
 *   event   1 refused(uint request, string reason)                         since version 8 (ws089-p026)
 *   request 3 set_pin(uint request, string current, string pin)            since version 10 (ws163-p003)
 *   event   2 enrolled(uint pin, uint keys)                                since version 11 (ws172-p002)
 *   request 4 add_key(uint request, string password, string label, string pin)  since version 14 (ws172-p003)
 *   request 5 remove_key(uint request, string password, string ref)       since version 14 (ws172-p003)
 *   event   3 key(string ref, string label)                                since version 14 (ws172-p003)
 *   event   4 touch(uint request)                                          since version 14 (ws172-p003)
 *   The compositor changes the password of the user it runs as, through
 *   the system (zedBSD: passwd; elsewhere unsupported), on a thread of its
 *   own, and answers ok, denied (the current password is wrong), invalid
 *   (the new one breaks the system's rules, or a password is empty or
 *   longer than KL_SYSTEM_PASSWORD_MAX), unsupported, busy (one change is
 *   under way) or failed.  Neither password is logged or kept.  The object
 *   has no state, and so no done.
 *   administer carries an administrator's change of the people's accounts
 *   (docs/architecture/security.md): the caller's password and the
 *   operation's lines (the operation, then its arguments, each ended by a
 *   line end, at most KL_SYSTEM_OPERATION_MAX bytes), which the compositor
 *   gives the system's tool (zedBSD: /usr/libexec/account-admin) on the
 *   same thread, one change at a time.  A refusal comes as refused with
 *   the tool's word (not-administrator, bad-password, ...) before the
 *   result; the result is ok, or denied for not-administrator and
 *   bad-password, invalid for the other refusals, unsupported, busy or
 *   failed.  The manager's capabilities have KL_SYSTEM_CAPABILITY_ADMINISTER
 *   where the system has the tool.
 *   set_pin sets the six-digit PIN of the user the compositor runs as, or
 *   removes it when pin is empty: the compositor passes both to the
 *   session manager (zedBSD: sessiond's ENROLL pin or REMOVE pin, which
 *   /sbin/passkey carries out in /etc/passkey; ws172-p002).  The result is
 *   ok, denied (current is wrong; a refusal's word comes first as refused:
 *   bad-secret, locked, ...), invalid (pin is not six digits), unsupported
 *   (no session manager), busy (another request is under way) or failed.
 *   Neither is logged or kept.  The manager's capabilities have
 *   KL_SYSTEM_CAPABILITY_PIN where a session manager runs.
 *   enrolled tells whether the user has a PIN and how many security keys
 *   (version 11): sent when the object is made, as soon as the session
 *   manager has answered, and again after each change.  Until it comes
 *   neither is known.
 *   add_key registers the security key plugged in for the user (version
 *   14): its label (1 to KL_SYSTEM_KEY_LABEL_MAX bytes, no colon), the
 *   key's own PIN, checked by the user's password (zedBSD: sessiond's
 *   ENROLL fido2, /usr/libexec/passkey-fido2).  While the key waits to be
 *   touched, touch(request) comes; then a refusal's word (bad-secret,
 *   no-key, many-keys, key-locked, timeout, ...) and the result, as
 *   set_pin's.  remove_key removes one by the reference key gave.  Before
 *   each enrolled, an object of version 14 hears key(ref, label) for each
 *   of the user's keys.  Neither secret is logged or kept.
 *
 * kl_system_monitor_v1 (WS134 p012, plan/ws134/design.md section 1.3)
 *   request 0 destroy
 *   request 1 ack(uint serial)                       the sample of that serial is taken
 *   request 2 set_period(uint period_ms)             250 to 10000
 *   event   0 info(uint cpu_count, string host, uint generation_high, uint generation_low)
 *   event   1 device(uint kind, uint id_high, uint id_low, uint generation_high, uint generation_low, uint subkind,
 *                    string name, string driver)
 *   event   2 info_done(uint serial)               the info and devices before it are the whole info
 *   event   3 cpu(uint index, uint user_high, uint user_low, uint system_high, uint system_low, uint idle_high,
 *                 uint idle_low, uint other_high, uint other_low)
 *   event   4 memory(uint total_high, uint total_low, uint free_high, uint free_low, uint cache_high, uint cache_low,
 *                    uint reclaimable_high, uint reclaimable_low, uint swap_total_high, uint swap_total_low,
 *                    uint swap_used_high, uint swap_used_low)
 *   event   5 link(uint id_high, uint id_low, uint rx_high, uint rx_low, uint tx_high, uint tx_low, uint up)
 *   event   6 disk(uint id_high, uint id_low, uint read_ops_high, uint read_ops_low, uint write_ops_high,
 *                  uint write_ops_low, uint read_bytes_high, uint read_bytes_low, uint write_bytes_high,
 *                  uint write_bytes_low, uint read_ns_high, uint read_ns_low, uint write_ns_high, uint write_ns_low,
 *                  uint busy_ns_high, uint busy_ns_low)
 *   event   7 gpu(uint id_high, uint id_low, uint time_high, uint time_low, uint busy_high, uint busy_low,
 *                 uint memory_used_high, uint memory_used_low, uint memory_total_high, uint memory_total_low,
 *                 uint cur_mhz, uint max_mhz, int milli_celsius, uint milli_watts)
 *   event   8 sample_done(uint serial, uint time_high, uint time_low, uint valid_high, uint valid_low, uint cpu_hz,
 *                         int cpu_milli_celsius)
 *   The compositor samples the machine (libkeiland-backend's monitor area) on a thread of its own while a monitor
 *   object exists, as often as the shortest period asked (at most 4 a second).  A sample is the counters (they only
 *   grow; a client makes the rates) and the present values, its cpu, memory, link, disk and gpu events and a
 *   sample_done; the info comes first and again when the devices change (info, a device a GPU, disk or link,
 *   info_done).  A u64 travels as its high and low halves.  No sample is sent before the client acked the last
 *   one, nor when the client's queue has no room for a whole one: a sample is sent whole or not at all, and one
 *   skipped is not owed (the next counters cover it).
 *
 * Every object hears its first state and a done when it is made, and each
 * change afterwards as the changed events and a done.
 */

#ifndef KEILAND_KL_SYSTEM_PROTOCOL_H
#define KEILAND_KL_SYSTEM_PROTOCOL_H

/* The interfaces' names and versions. */
#define KL_SYSTEM_MANAGER_NAME			"kl_system_manager_v1"
#define KL_SYSTEM_MANAGER_VERSION		23U
#define KL_SYSTEM_SETTINGS_NAME			"kl_system_settings_v1"

/* kl_system_manager_v1's requests and event. */
#define KL_SYSTEM_MANAGER_DESTROY		0U
#define KL_SYSTEM_MANAGER_GET_SETTINGS		1U
#define KL_SYSTEM_MANAGER_GET_NETWORK		2U
#define KL_SYSTEM_MANAGER_GET_AUDIO		3U
#define KL_SYSTEM_MANAGER_GET_POWER		4U
#define KL_SYSTEM_MANAGER_GET_DEVICES		5U
#define KL_SYSTEM_MANAGER_GET_MONITOR		6U
#define KL_SYSTEM_MANAGER_GET_ACCOUNT		7U
#define KL_SYSTEM_MANAGER_GET_SHARING		8U
#define KL_SYSTEM_MANAGER_GET_NOTIFY		9U
#define KL_SYSTEM_MANAGER_GET_MAIL		10U
#define KL_SYSTEM_MANAGER_GET_PHONE		11U
#define KL_SYSTEM_MANAGER_GET_PRINTERS		12U
#define KL_SYSTEM_MANAGER_GET_DISPLAYS		13U
#define KL_SYSTEM_MANAGER_GET_MACHINE		14U
#define KL_SYSTEM_MANAGER_GET_BLUETOOTH		15U
#define KL_SYSTEM_MANAGER_EVENT_CAPABILITIES	0U

/* The capabilities' bits. */
#define KL_SYSTEM_CAPABILITY_SETTINGS		0x1U
#define KL_SYSTEM_CAPABILITY_NETWORK		0x2U
#define KL_SYSTEM_CAPABILITY_AUDIO		0x4U
#define KL_SYSTEM_CAPABILITY_POWER		0x8U
#define KL_SYSTEM_CAPABILITY_DEVICES		0x10U
#define KL_SYSTEM_CAPABILITY_MONITOR		0x20U
#define KL_SYSTEM_CAPABILITY_ACCOUNT		0x40U
#define KL_SYSTEM_CAPABILITY_SHARING		0x80U
#define KL_SYSTEM_CAPABILITY_ADMINISTER		0x100U
#define KL_SYSTEM_CAPABILITY_PIN		0x200U
#define KL_SYSTEM_CAPABILITY_NOTIFY		0x400U
#define KL_SYSTEM_CAPABILITY_MAIL		0x800U
#define KL_SYSTEM_CAPABILITY_PHONE		0x1000U
#define KL_SYSTEM_CAPABILITY_PRINTERS		0x2000U
#define KL_SYSTEM_CAPABILITY_DISPLAYS		0x4000U
#define KL_SYSTEM_CAPABILITY_MACHINE		0x8000U
#define KL_SYSTEM_CAPABILITY_BLUETOOTH		0x10000U

/* Since when the manager has get_sharing (ws089-p025), and the account administer and refused (ws089-p026). */
#define KL_SYSTEM_SINCE_SHARING			7U
#define KL_SYSTEM_SINCE_ADMINISTER		8U

/* Since when the account has set_pin (ws163-p003), and enrolled (ws172-p002). */
#define KL_SYSTEM_SINCE_PIN			10U
#define KL_SYSTEM_SINCE_ENROLLED		11U

/* Since when the manager has get_notify (ws156-p002). */
#define KL_SYSTEM_SINCE_NOTIFY			13U

/* Since when the account has add_key, remove_key, key and touch (ws172-p003). */
#define KL_SYSTEM_SINCE_KEYS			14U

/* Since when the manager has get_mail (ws169-p002). */
#define KL_SYSTEM_SINCE_MAIL			15U

/* Since when the manager has get_phone (ws170-p004). */
#define KL_SYSTEM_SINCE_PHONE			16U

/* Since when the manager has get_printers (ws145-p003). */
#define KL_SYSTEM_SINCE_PRINTERS		17U

/* The displays (ws113-p005). */
#define KL_SYSTEM_SINCE_DISPLAYS		18U

/* Since when the displays have set_shown (ws113-p014). */
#define KL_SYSTEM_SINCE_SHOWN			19U

/* Since when the mail object tells a reader whether it is allowed (ws177-p005). */
#define KL_SYSTEM_SINCE_MAIL_ALLOWED		20U

/* Since when the manager has get_machine (ws188-p002). */
#define KL_SYSTEM_SINCE_MACHINE			21U

/* Since when the computer's object reads the mounts (ws188-p004). */
#define KL_SYSTEM_SINCE_MOUNTS			22U

/* Since when the manager has get_bluetooth (ws143-p006). */
#define KL_SYSTEM_SINCE_BLUETOOTH		23U

/* The interfaces' names (WS131 p010). */
#define KL_SYSTEM_NETWORK_NAME			"kl_system_network_v1"
#define KL_SYSTEM_AUDIO_NAME			"kl_system_audio_v1"
#define KL_SYSTEM_POWER_NAME			"kl_system_power_v1"
#define KL_SYSTEM_DEVICES_NAME			"kl_system_devices_v1"
#define KL_SYSTEM_MONITOR_NAME			"kl_system_monitor_v1"
#define KL_SYSTEM_ACCOUNT_NAME			"kl_system_account_v1"
#define KL_SYSTEM_SHARING_NAME			"kl_system_sharing_v1"
#define KL_SYSTEM_NOTIFY_NAME			"kl_system_notify_v1"
#define KL_SYSTEM_MAIL_NAME			"kl_system_mail_v1"
#define KL_SYSTEM_PHONE_NAME			"kl_system_phone_v1"
#define KL_SYSTEM_PRINTERS_NAME			"kl_system_printers_v1"
#define KL_SYSTEM_DISPLAYS_NAME			"kl_system_displays_v1"
#define KL_SYSTEM_MACHINE_NAME			"kl_system_machine_v1"
#define KL_SYSTEM_BLUETOOTH_NAME		"kl_system_bluetooth_v1"

/*
 * kl_system_bluetooth_v1's requests and events (ws143-p006,
 * plan/ws143/phase006/phase.md section 5):
 *   request 0 destroy
 *   request 1 watch(uint on)          the state read often while some object watches (a page or a menu shown)
 *   request 2 scan(uint on)           the devices around looked for while some object asks
 *   request 3 power(uint request, uint on)
 *   request 4 device(uint request, uint action, string address, uint type)   action: KL_SYSTEM_BT_PAIR and the others
 *   event   0 state(uint reachable, uint state, uint flags, uint features, string address, string name)
 *                                     flags: scanning, pairing, the user's switch on, this desktop answers the pairings' questions
 *   event   1 device(string address, uint type, string name, uint kind, uint flags, int battery, int rssi)
 *   event   2 done(uint serial)       the state and the devices before it are the whole state
 *   event   3 result(uint request, uint applied, uint saved)
 * Sent whole when the object is made and each time something changed.
 * A pairing's questions are the compositor's own window's, never a
 * client's: no client answers them.  The values are keiland.h's
 * KL_BLUETOOTH_* (the same numbers as libkeiland-backend's).
 */
#define KL_SYSTEM_BLUETOOTH_DESTROY		0U
#define KL_SYSTEM_BLUETOOTH_WATCH		1U
#define KL_SYSTEM_BLUETOOTH_SCAN		2U
#define KL_SYSTEM_BLUETOOTH_POWER		3U
#define KL_SYSTEM_BLUETOOTH_DEVICE		4U
#define KL_SYSTEM_BLUETOOTH_EVENT_STATE		0U
#define KL_SYSTEM_BLUETOOTH_EVENT_DEVICE	1U
#define KL_SYSTEM_BLUETOOTH_EVENT_DONE		2U
#define KL_SYSTEM_BLUETOOTH_EVENT_RESULT	3U

/* A device's actions, the state's flags, a device's flags, and the longest texts (with their NULs). */
#define KL_SYSTEM_BT_PAIR			1U
#define KL_SYSTEM_BT_FORGET			2U
#define KL_SYSTEM_BT_CONNECT			3U
#define KL_SYSTEM_BT_DISCONNECT			4U
#define KL_SYSTEM_BT_SCANNING			0x1U
#define KL_SYSTEM_BT_PAIRING			0x2U
#define KL_SYSTEM_BT_POWERED			0x4U
#define KL_SYSTEM_BT_ANSWERS			0x8U
#define KL_SYSTEM_BT_PAIRED			0x1U
#define KL_SYSTEM_BT_LEGACY			0x2U
#define KL_SYSTEM_BT_CONNECTED			0x4U
#define KL_SYSTEM_BT_ADDRESS_MAX		18U
#define KL_SYSTEM_BT_NAME_MAX			64U
#define KL_SYSTEM_BT_DEVICES_MAX		32U

/*
 * kl_system_machine_v1's requests and events (ws188-p002), the parts a
 * query asks, a user's flags (a person's account, the client's own, an
 * administrator's, allowed to control Wi-Fi), and the longest texts the
 * events carry (with their NULs).
 */
#define KL_SYSTEM_MACHINE_DESTROY		0U
#define KL_SYSTEM_MACHINE_QUERY			1U
#define KL_SYSTEM_MACHINE_EVENT_PARTS		0U
#define KL_SYSTEM_MACHINE_EVENT_ABOUT		1U
#define KL_SYSTEM_MACHINE_EVENT_FILESYSTEM	2U
#define KL_SYSTEM_MACHINE_EVENT_USER		3U
#define KL_SYSTEM_MACHINE_EVENT_LOGIN_LANGUAGE	4U
#define KL_SYSTEM_MACHINE_EVENT_RESULT		5U
#define KL_SYSTEM_MACHINE_EVENT_MOUNT		6U
#define KL_SYSTEM_MACHINE_ABOUT			0x1U
#define KL_SYSTEM_MACHINE_FILESYSTEMS		0x2U
#define KL_SYSTEM_MACHINE_USERS			0x4U
#define KL_SYSTEM_MACHINE_LOGIN_LANGUAGE	0x8U
#define KL_SYSTEM_MACHINE_MOUNTS		0x10U
#define KL_SYSTEM_MACHINE_PARTS			0x1fU
#define KL_SYSTEM_MACHINE_PARTS_21		0xfU
#define KL_SYSTEM_MACHINE_USER_PERSON		0x1U
#define KL_SYSTEM_MACHINE_USER_SELF		0x2U
#define KL_SYSTEM_MACHINE_USER_ADMIN		0x4U
#define KL_SYSTEM_MACHINE_USER_NETWORK		0x8U
#define KL_SYSTEM_MACHINE_SYSTEM_MAX		128U
#define KL_SYSTEM_MACHINE_KERNEL_MAX		160U
#define KL_SYSTEM_MACHINE_ARCH_MAX		32U
#define KL_SYSTEM_MACHINE_PROCESSOR_MAX		64U
#define KL_SYSTEM_MACHINE_HOST_MAX		64U
#define KL_SYSTEM_MACHINE_PATH_MAX		64U
#define KL_SYSTEM_MACHINE_NAME_MAX		64U
#define KL_SYSTEM_MACHINE_FULL_NAME_MAX		128U
#define KL_SYSTEM_MACHINE_HOME_MAX		256U
#define KL_SYSTEM_MACHINE_CODE_MAX		8U
#define KL_SYSTEM_MACHINE_FILESYSTEMS_MAX	8U
#define KL_SYSTEM_MACHINE_USERS_MAX		64U
#define KL_SYSTEM_MACHINE_MOUNT_PATH_MAX	256U
#define KL_SYSTEM_MACHINE_MOUNT_TYPE_MAX	32U
#define KL_SYSTEM_MACHINE_MOUNTS_MAX		64U

/*
 * kl_system_displays_v1's requests and events (ws113-p005), the modes, a
 * display's flags (built in, the desktop's anchor, shown now, with a light
 * the compositor can set, held back by the limit of the displays shown at
 * once, turned off by the user, ws113-p014), and the longest key and label an output carries and places a
 * request carries (with their NULs).
 */
#define KL_SYSTEM_DISPLAYS_DESTROY		0U
#define KL_SYSTEM_DISPLAYS_APPLY		1U
#define KL_SYSTEM_DISPLAYS_SET_BRIGHTNESS	2U
#define KL_SYSTEM_DISPLAYS_SET_SHOWN		3U
#define KL_SYSTEM_DISPLAYS_EVENT_OUTPUT		0U
#define KL_SYSTEM_DISPLAYS_EVENT_DONE		1U
#define KL_SYSTEM_DISPLAYS_EVENT_RESULT		2U
#define KL_SYSTEM_DISPLAYS_EXTENDED		0U
#define KL_SYSTEM_DISPLAYS_MIRROR		1U
#define KL_SYSTEM_DISPLAY_INTERNAL		0x1U
#define KL_SYSTEM_DISPLAY_ANCHOR		0x2U
#define KL_SYSTEM_DISPLAY_SHOWN			0x4U
#define KL_SYSTEM_DISPLAY_BACKLIGHT		0x8U
#define KL_SYSTEM_DISPLAY_LIMITED		0x10U
#define KL_SYSTEM_DISPLAY_OFF			0x20U
#define KL_SYSTEM_DISPLAY_KEY_MAX		64U
#define KL_SYSTEM_DISPLAY_LABEL_MAX		64U
#define KL_SYSTEM_DISPLAY_PLACES_MAX		1024U

/*
 * kl_system_notify_v1's requests and events (ws156-p002,
 * plan/ws156/phase001/phase.md section 2): post(request, replaces, app,
 * title, body, flags), withdraw(request, id); posted(request, id),
 * activated(id), closed(id, reason), result(request, applied, saved).
 */
#define KL_SYSTEM_NOTIFY_DESTROY		0U
#define KL_SYSTEM_NOTIFY_POST			1U
#define KL_SYSTEM_NOTIFY_WITHDRAW		2U
#define KL_SYSTEM_NOTIFY_EVENT_POSTED		0U
#define KL_SYSTEM_NOTIFY_EVENT_ACTIVATED	1U
#define KL_SYSTEM_NOTIFY_EVENT_CLOSED		2U
#define KL_SYSTEM_NOTIFY_EVENT_RESULT		3U

/* A notification's flags, and why one closed (closed's reason). */
#define KL_SYSTEM_NOTIFY_URGENT			0x1U
#define KL_SYSTEM_NOTIFY_ACTION			0x2U
#define KL_SYSTEM_NOTIFY_DISMISSED		1U
#define KL_SYSTEM_NOTIFY_EXPIRED		2U
#define KL_SYSTEM_NOTIFY_CLEARED		3U
#define KL_SYSTEM_NOTIFY_WITHDRAWN		4U

/*
 * kl_system_mail_v1's requests and events (ws169-p002,
 * plan/ws169/phase001/phase.md section 1):
 *   request 0 destroy
 *   request 1 arrived(uint request, string account, string from, string subject, string code)
 *       the mail program tells of a new message (no body; code is a
 *       sign-in code found in it, or empty); since ws177-p005 only a
 *       client with a window of the mail program (app_id "mailer") is
 *       heard, any other is answered INVALID
 *   request 2 listen(uint request, string app)
 *       a reader asks for the messages' arrivals under its name; only a
 *       name the settings know (mail.codes.<app>) is taken
 *   event   0 mail(string from, string subject, string code)
 *       a message arrived, told to each listener whose mail.codes.<app>
 *       setting is on when it arrives
 *   event   1 result(uint request, uint applied, uint saved)
 *   event   2 allowed(uint on)  (since 20, ws177-p005)
 *       whether the user lets the reader hear the arrivals now: told once
 *       its listen is taken, and again whenever its mail.codes.<app>
 *       setting changes
 */
#define KL_SYSTEM_MAIL_DESTROY			0U
#define KL_SYSTEM_MAIL_ARRIVED			1U
#define KL_SYSTEM_MAIL_LISTEN			2U
#define KL_SYSTEM_MAIL_EVENT_MAIL		0U
#define KL_SYSTEM_MAIL_EVENT_RESULT		1U
#define KL_SYSTEM_MAIL_EVENT_ALLOWED		2U

/* The setting that lets a reader hear the arrivals, before the reader's name (mail.codes.browser). */
#define KL_SYSTEM_MAIL_SETTING_PREFIX		"mail.codes."

/*
 * kl_system_phone_v1's requests and events (ws170-p004,
 * plan/ws170/phase001/phase.md section 3):
 *   request 0 destroy
 *   request 1 send(uint request, uint channel, string to, string text)
 *   request 2 call(uint request, uint channel, string to)
 *   event   0 received(uint channel, string from, string text, uint time_high, uint time_low)
 *       a message came (to every phone object of the user)
 *   event   1 status(uint request, uint state)
 *       a sent message's or a call's state (KL_SYSTEM_PHONE_*)
 *   event   2 result(uint request, uint applied, uint saved)
 *       the request taken (OK), or not: UNAVAILABLE without a backend
 * The backend is the desktop's setting phone.backend (0 none, 1 loopback).
 */
#define KL_SYSTEM_PHONE_DESTROY			0U
#define KL_SYSTEM_PHONE_SEND			1U
#define KL_SYSTEM_PHONE_CALL			2U
#define KL_SYSTEM_PHONE_EVENT_RECEIVED		0U
#define KL_SYSTEM_PHONE_EVENT_STATUS		1U
#define KL_SYSTEM_PHONE_EVENT_RESULT		2U

/* A message's or call's state (status), and the channels (keiland.h's KL_PHONE_*). */
#define KL_SYSTEM_PHONE_SENT			1U
#define KL_SYSTEM_PHONE_DELIVERED		2U
#define KL_SYSTEM_PHONE_FAILED			3U
#define KL_SYSTEM_PHONE_ANSWERED		4U
#define KL_SYSTEM_PHONE_NO_ANSWER		5U
#define KL_SYSTEM_PHONE_CHANNELS		5U

/* The desktop's setting that chooses the backend. */
#define KL_SYSTEM_PHONE_SETTING			"phone.backend"
#define KL_SYSTEM_PHONE_BACKEND_NONE		0
#define KL_SYSTEM_PHONE_BACKEND_LOOPBACK	1

/*
 * kl_system_printers_v1's requests and events (ws145-p003,
 * plan/ws145/design.md section 3):
 *   request 0 destroy
 *   request 1 add(uint request, uint protocol, string host, uint port, string path)
 *       path: the IPP path or the LPD queue ("" for the protocol's usual one)
 *   request 2 remove(uint request, uint printer)
 *   request 3 set_default(uint request, uint printer)
 *   request 4 print(uint request, uint printer, string title, fd document)
 *       printer 0: the default; the document is a PDF the client opened
 *   request 5 cancel(uint request, uint job)
 *   event   0 printer(uint id, uint protocol, string host, uint port, string path, string name, uint flags)
 *   event   1 job(uint job, uint printer, uint state, string title, string detail)
 *   event   2 done(uint serial)    the printers and jobs before it are the whole state
 *   event   3 queued(uint request, uint job)    before print's result, when the job was taken
 *   event   4 result(uint request, uint applied, uint saved)
 * Sent whole when the object is made and each time something changed.
 */
#define KL_SYSTEM_PRINTERS_DESTROY		0U
#define KL_SYSTEM_PRINTERS_ADD			1U
#define KL_SYSTEM_PRINTERS_REMOVE		2U
#define KL_SYSTEM_PRINTERS_SET_DEFAULT		3U
#define KL_SYSTEM_PRINTERS_PRINT		4U
#define KL_SYSTEM_PRINTERS_CANCEL		5U
#define KL_SYSTEM_PRINTERS_EVENT_PRINTER	0U
#define KL_SYSTEM_PRINTERS_EVENT_JOB		1U
#define KL_SYSTEM_PRINTERS_EVENT_DONE		2U
#define KL_SYSTEM_PRINTERS_EVENT_QUEUED		3U
#define KL_SYSTEM_PRINTERS_EVENT_RESULT		4U

/* The protocols, a printer's flags and a job's states (keiland.h's KL_PRINTER_* and KL_PRINT_*). */
#define KL_SYSTEM_PRINTER_IPP			1U
#define KL_SYSTEM_PRINTER_LPD			2U
#define KL_SYSTEM_PRINTER_DEFAULT		0x1U
#define KL_SYSTEM_PRINT_QUEUED			1U
#define KL_SYSTEM_PRINT_SENDING			2U
#define KL_SYSTEM_PRINT_WAITING			3U
#define KL_SYSTEM_PRINT_DONE			4U
#define KL_SYSTEM_PRINT_FAILED			5U
#define KL_SYSTEM_PRINT_CANCELLED		6U

/* kl_system_sharing_v1's requests and events (ws089-p025), and the longest fingerprint it carries. */
#define KL_SYSTEM_SHARING_DESTROY		0U
#define KL_SYSTEM_SHARING_SET_SSH		1U
#define KL_SYSTEM_SHARING_QUERY			2U
#define KL_SYSTEM_SHARING_EVENT_STATE		0U
#define KL_SYSTEM_SHARING_EVENT_DONE		1U
#define KL_SYSTEM_SHARING_EVENT_RESULT		2U

/* kl_system_account_v1's requests and event, and the longest password it carries (without its NUL). */
#define KL_SYSTEM_ACCOUNT_DESTROY		0U
#define KL_SYSTEM_ACCOUNT_SET_PASSWORD		1U
#define KL_SYSTEM_ACCOUNT_ADMINISTER		2U
#define KL_SYSTEM_ACCOUNT_SET_PIN		3U
#define KL_SYSTEM_ACCOUNT_ADD_KEY		4U
#define KL_SYSTEM_ACCOUNT_REMOVE_KEY		5U
#define KL_SYSTEM_ACCOUNT_EVENT_RESULT		0U
#define KL_SYSTEM_ACCOUNT_EVENT_REFUSED		1U
#define KL_SYSTEM_ACCOUNT_EVENT_ENROLLED	2U
#define KL_SYSTEM_ACCOUNT_EVENT_KEY		3U
#define KL_SYSTEM_ACCOUNT_EVENT_TOUCH		4U
#define KL_SYSTEM_KEY_LABEL_MAX			32U
#define KL_SYSTEM_KEY_REF_MAX			16U
#define KL_SYSTEM_PASSWORD_MAX			256U

/* The longest operation administer carries, and the longest refusal's word (without their NULs). */
#define KL_SYSTEM_OPERATION_MAX			1024U
#define KL_SYSTEM_REASON_MAX			31U

/* kl_system_network_v1's requests and events. */
#define KL_SYSTEM_NETWORK_DESTROY		0U
#define KL_SYSTEM_NETWORK_REQUEST		1U
#define KL_SYSTEM_NETWORK_SAVE_KEY		2U
#define KL_SYSTEM_NETWORK_QUERY_DETAILS		3U
#define KL_SYSTEM_NETWORK_SET_SCANNING		4U
#define KL_SYSTEM_NETWORK_CONFIGURE_WIRED	5U
#define KL_SYSTEM_NETWORK_EVENT_STATE		0U
#define KL_SYSTEM_NETWORK_EVENT_ACCESS_POINT	1U
#define KL_SYSTEM_NETWORK_EVENT_SCAN_DONE	2U
#define KL_SYSTEM_NETWORK_EVENT_LINK		3U
#define KL_SYSTEM_NETWORK_EVENT_DNS		4U
#define KL_SYSTEM_NETWORK_EVENT_SAVED		5U
#define KL_SYSTEM_NETWORK_EVENT_DETAILS_DONE	6U
#define KL_SYSTEM_NETWORK_EVENT_DONE		7U
#define KL_SYSTEM_NETWORK_EVENT_RESULT		8U
#define KL_SYSTEM_NETWORK_EVENT_WIRED		9U
#define KL_SYSTEM_NETWORK_EVENT_LINK_SPEED	10U

/* Since when the network has configure_wired and wired (ws089-p022), and link_speed (BUG-222). */
#define KL_SYSTEM_NETWORK_SINCE_WIRED		6U
#define KL_SYSTEM_NETWORK_SINCE_LINK_SPEED	12U

/* How a wired interface is configured (configure_wired's mode and wired's). */
#define KL_SYSTEM_WIRED_UNKNOWN			0U
#define KL_SYSTEM_WIRED_DHCP			1U
#define KL_SYSTEM_WIRED_STATIC			2U

/* The network's requests (request's what; the backend's KL_BACKEND_NETWORK_REQUEST_* values). */
#define KL_SYSTEM_NETWORK_SCAN			1U
#define KL_SYSTEM_NETWORK_JOIN			2U
#define KL_SYSTEM_NETWORK_DISCONNECT		3U
#define KL_SYSTEM_NETWORK_WIFI_ON		4U
#define KL_SYSTEM_NETWORK_WIFI_OFF		5U

/* A link's flags. */
#define KL_SYSTEM_LINK_UP			0x1U
#define KL_SYSTEM_LINK_RUNNING			0x2U
#define KL_SYSTEM_LINK_LOOPBACK			0x4U

/* kl_system_audio_v1's requests and events. */
#define KL_SYSTEM_AUDIO_DESTROY			0U
#define KL_SYSTEM_AUDIO_SET_VOLUME		1U
#define KL_SYSTEM_AUDIO_FEEDBACK		2U
#define KL_SYSTEM_AUDIO_EVENT_STATE		0U
#define KL_SYSTEM_AUDIO_EVENT_DONE		1U
#define KL_SYSTEM_AUDIO_EVENT_RESULT		2U

/* kl_system_power_v1's requests and events, and its actions (the backend's KL_BACKEND_POWER_* values). */
#define KL_SYSTEM_POWER_DESTROY			0U
#define KL_SYSTEM_POWER_ACTION			1U
#define KL_SYSTEM_POWER_EVENT_STATE		0U
#define KL_SYSTEM_POWER_EVENT_DONE		1U
#define KL_SYSTEM_POWER_EVENT_RESULT		2U
#define KL_SYSTEM_POWER_POWEROFF		1U
#define KL_SYSTEM_POWER_REBOOT			2U
#define KL_SYSTEM_POWER_SUSPEND			3U

/* kl_system_devices_v1's requests and events. */
#define KL_SYSTEM_DEVICES_DESTROY		0U
#define KL_SYSTEM_DEVICES_EJECT			1U
#define KL_SYSTEM_DEVICES_MOUNT			2U
#define KL_SYSTEM_DEVICES_EVENT_DEVICE		0U
#define KL_SYSTEM_DEVICES_EVENT_DONE		1U
#define KL_SYSTEM_DEVICES_EVENT_RESULT		2U
#define KL_SYSTEM_DEVICES_EVENT_BUSY		3U
#define KL_SYSTEM_DEVICES_EVENT_VOLUME		4U
#define KL_SYSTEM_DEVICES_SINCE_MOUNT		5U
#define KL_SYSTEM_DEVICES_SINCE_VOLUME		9U

/* A device's kind and its state's bits (ws132-p004). */
#define KL_SYSTEM_DEVICE_KIND_STORAGE		1U
#define KL_SYSTEM_DEVICE_MOUNTED		0x1U
#define KL_SYSTEM_DEVICE_NEW			0x2U

/* kl_system_monitor_v1's requests and events (WS134 p012). */
#define KL_SYSTEM_MONITOR_DESTROY		0U
#define KL_SYSTEM_MONITOR_ACK			1U
#define KL_SYSTEM_MONITOR_SET_PERIOD		2U
#define KL_SYSTEM_MONITOR_EVENT_INFO		0U
#define KL_SYSTEM_MONITOR_EVENT_DEVICE		1U
#define KL_SYSTEM_MONITOR_EVENT_INFO_DONE	2U
#define KL_SYSTEM_MONITOR_EVENT_CPU		3U
#define KL_SYSTEM_MONITOR_EVENT_MEMORY		4U
#define KL_SYSTEM_MONITOR_EVENT_LINK		5U
#define KL_SYSTEM_MONITOR_EVENT_DISK		6U
#define KL_SYSTEM_MONITOR_EVENT_GPU		7U
#define KL_SYSTEM_MONITOR_EVENT_SAMPLE_DONE	8U

/* A monitor device's kind (the device event's kind). */
#define KL_SYSTEM_MONITOR_DEVICE_GPU		1U
#define KL_SYSTEM_MONITOR_DEVICE_DISK		2U
#define KL_SYSTEM_MONITOR_DEVICE_LINK		3U

/* The bounds of a monitor's period, in milliseconds, and the period without one. */
#define KL_SYSTEM_MONITOR_PERIOD_MIN		250U
#define KL_SYSTEM_MONITOR_PERIOD_MAX		10000U
#define KL_SYSTEM_MONITOR_PERIOD_DEFAULT	1000U

/* kl_system_settings_v1's requests and events. */
#define KL_SYSTEM_SETTINGS_DESTROY		0U
#define KL_SYSTEM_SETTINGS_SET			1U
#define KL_SYSTEM_SETTINGS_RESET		2U
#define KL_SYSTEM_SETTINGS_EVENT_VALUE		0U
#define KL_SYSTEM_SETTINGS_EVENT_DONE		1U
#define KL_SYSTEM_SETTINGS_EVENT_RESULT		2U

/* A value's flags: the resolver's default (not chosen), and not known yet (the value is empty). */
#define KL_SYSTEM_SETTINGS_DEFAULT		0x1U
#define KL_SYSTEM_SETTINGS_UNKNOWN		0x2U

/* The results of a request (WS131 section 4.1). */
#define KL_SYSTEM_RESULT_OK			0U
#define KL_SYSTEM_RESULT_DENIED			1U
#define KL_SYSTEM_RESULT_UNSUPPORTED		2U
#define KL_SYSTEM_RESULT_BUSY			3U
#define KL_SYSTEM_RESULT_INVALID		4U
#define KL_SYSTEM_RESULT_UNAVAILABLE		5U
#define KL_SYSTEM_RESULT_FAILED			6U
#define KL_SYSTEM_RESULT_NOT_SAVED		7U

/* A network join's own failures (WS131 p011): no key is saved, the network refused the key, the network is out of reach. */
#define KL_SYSTEM_RESULT_NO_KEY			8U
#define KL_SYSTEM_RESULT_REFUSED		9U
#define KL_SYSTEM_RESULT_UNREACHABLE		10U

/* A displays' apply made against a snapshot that is no longer the last (ws113-p005). */
#define KL_SYSTEM_RESULT_STALE			11U

#endif
