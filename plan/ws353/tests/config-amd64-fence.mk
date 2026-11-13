# The x86_64 guest image of the display-event fence test (qemu-display-fence.sh): a text console mirrored on the
# serial line (the test drives the guest with plan/tools/guest/serial.py), the programs the test's script uses, the
# test's program (built by the script first, into build/ws353-p002-prog), the zedBSD SSH server and the guest
# harness's key and network files.  The kernel is the default x86_64 one (venus among its GPU drivers).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the nested makes read it from their own directories.
WS353_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../..)
include $(WS353_TREE)/config/config.x86_64.mk

override CONFIG_GRAPHICAL_LOGIN := n
override CONFIG_ROOTFS_DEVELOPMENT := n
override CONFIG_PCAT_SERIAL_MIRROR := y
override CONFIG_USERLAND_PROGRAMS := sh init cron getty syslogd networkd mount fsck sysctl net \
	login dhcpc ssh libcrypto-compat libz-compat etc ls cat echo sleep test true false mkdir rm touch \
	kill ps dmesg sync poweroff halt reboot service env id uname date timeout chmod chown grep

# The guest harness (plan/tools/guest/guest.py extra-files): root's authorized key, the host keys and DHCP on ue0.
WS353_GUEST_KEYS := $(WS353_TREE)/plan/tmp/guest
WS353_HARNESS_FILES := \
	--file /root/.ssh/authorized_keys=$(WS353_GUEST_KEYS)/id_ed25519.pub \
	--mode /root/.ssh/authorized_keys=0600 --mode /root/.ssh=0700 \
	--file /etc/ssh/ssh_host_rsa_key=$(WS353_GUEST_KEYS)/ssh_host_rsa_key \
	--file /etc/ssh/ssh_host_rsa_key.pub=$(WS353_GUEST_KEYS)/ssh_host_rsa_key.pub \
	--mode /etc/ssh/ssh_host_rsa_key=0600 \
	--file /etc/ssh/ssh_host_ecdsa_key=$(WS353_GUEST_KEYS)/ssh_host_ecdsa_key \
	--file /etc/ssh/ssh_host_ecdsa_key.pub=$(WS353_GUEST_KEYS)/ssh_host_ecdsa_key.pub \
	--mode /etc/ssh/ssh_host_ecdsa_key=0600 \
	--file /etc/ssh/ssh_host_ed25519_key=$(WS353_GUEST_KEYS)/ssh_host_ed25519_key \
	--file /etc/ssh/ssh_host_ed25519_key.pub=$(WS353_GUEST_KEYS)/ssh_host_ed25519_key.pub \
	--mode /etc/ssh/ssh_host_ed25519_key=0600
# The harness's net.conf takes the place of the base one: the rootfs copies init's data (the base net.conf) over
# the entries below, so init's own list names the harness's file (init's Makefile reads this configuration).
override INIT_DATA := /etc/rc.conf=userland/base/etc/rc.conf \
	/etc/fstab=userland/base/etc/fstab \
	/etc/net.conf=plan/tools/guest/net.conf \
	/etc/service.d/syslogd=userland/base/init/services/syslogd \
	/etc/service.d/getty_console=userland/base/init/services/getty_console \
	/etc/service.d/networkd=userland/base/init/services/networkd \
	/etc/service.d/networking=userland/base/init/services/networking \
	/etc/service.d/cron=userland/base/init/services/cron \
	/etc/service.d/ntpdate=userland/base/init/services/ntpdate \
	/etc/service.d/homes=userland/base/init/services/homes \
	/usr/libexec/make-homes=userland/base/init/make-homes
X86_64_ARCH_FILES = $(subst /etc/net.conf=userland/base/etc/net.conf,/etc/net.conf=$(WS353_TREE)/plan/tools/guest/net.conf,$(ROOTFS_FILES)) $(WS353_HARNESS_FILES) \
	--file /root/display-fence-test=$(WS353_PROGRAM) --mode /root/display-fence-test=0755

# The test's program, which qemu-display-fence.sh builds before the image.
WS353_PROGRAM := $(WS353_TREE)/build/ws353-p002-prog/display-fence-test
# The rootfs is made again when this file or a harness file changes.
X86_64_ARCH_INPUTS = $(ROOTFS_INPUTS) $(WS353_TREE)/plan/ws353/tests/config-amd64-fence.mk \
	$(WS353_TREE)/plan/tools/guest/net.conf $(WS353_GUEST_KEYS)/id_ed25519.pub $(WS353_PROGRAM)
