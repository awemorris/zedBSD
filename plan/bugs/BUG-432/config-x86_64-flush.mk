# The x86_64 guest image of the stdio flush test (qemu-flush.sh): a text console mirrored on the serial line (the
# test drives the guest with plan/tools/guest/serial.py), the few programs the guest script uses, and the test's
# programs (built by the script first, into build/bug432/prog).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
BUG432_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../..)
include $(BUG432_TREE)/config/config.x86_64.mk

override CONFIG_GRAPHICAL_LOGIN := n
override CONFIG_ROOTFS_DEVELOPMENT := n
override CONFIG_PCAT_SERIAL_MIRROR := y
override CONFIG_USERLAND_PROGRAMS := sh init cron getty syslogd mount fsck sysctl login etc ls cat echo sleep \
	test true false mkdir rm touch kill ps dmesg sync poweroff halt reboot service env id uname date timeout \
	chmod grep cmp wc tr printf

BUG432_PROGRAMS := $(BUG432_TREE)/build/bug432/prog
ROOTFS_EXTRA_FILES := \
	--file /root/flush-static=$(BUG432_PROGRAMS)/flush-static --mode /root/flush-static=0755 \
	--file /root/flush-dynamic=$(BUG432_PROGRAMS)/flush-dynamic --mode /root/flush-dynamic=0755 \
	--file /root/flush-driver=$(BUG432_PROGRAMS)/flush-driver --mode /root/flush-driver=0755 \
	--file /root/tls-check-static=$(BUG432_PROGRAMS)/tls-check-static --mode /root/tls-check-static=0755 \
	--file /root/ptyrun=$(BUG432_PROGRAMS)/ptyrun --mode /root/ptyrun=0755 \
	--file /root/run.sh=$(BUG432_TREE)/plan/bugs/BUG-432/guest-run.sh --mode /root/run.sh=0755
