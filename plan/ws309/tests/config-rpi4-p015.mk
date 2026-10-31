# ws309-p015: the Raspberry Pi 4 image of the game mode's lent images: the CI Pi 4 image with mview (its qs40 model
# under /usr/share/mview/qs40), the onboard Ethernet by DHCP and root reached over SSH with the guest harness's key
# (plan/tmp/guest, made by plan/tools/guest/guest.py keys) and fixed host keys.  plan/ws309/tests/pi-p015.sh runs on it.
# Build:  flock /tmp/zedbsd-image-build.lock make ZEDBSD_CONFIG=plan/ws309/tests/config-rpi4-p015.mk BUILD=DIR disk-image
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/ci/config-rpi4.mk

# The 3D scene.
ZEDBSD_USER_PROGRAMS += $(filter-out $(ZEDBSD_USER_PROGRAMS),mview)

# The network and root's SSH.
RPI4_P015_GUEST := plan/tmp/guest
RPI4_P015_FILES := /etc/net.conf=plan/ws291/tests/net-rpi5.conf \
	/root/.ssh/authorized_keys=$(RPI4_P015_GUEST)/id_ed25519.pub \
	$(foreach key,rsa ecdsa ed25519,/etc/ssh/ssh_host_$(key)_key=$(RPI4_P015_GUEST)/ssh_host_$(key)_key \
	/etc/ssh/ssh_host_$(key)_key.pub=$(RPI4_P015_GUEST)/ssh_host_$(key)_key.pub)
ZEDBSD_EXTRA_INPUTS += $(foreach item,$(RPI4_P015_FILES),$(lastword $(subst =, ,$(item))))
ZEDBSD_EXTRA_FILES += $(foreach item,$(RPI4_P015_FILES),--file $(item)) \
	--mode /root/.ssh=0700 --mode /root/.ssh/authorized_keys=0600 \
	$(foreach key,rsa ecdsa ed25519,--mode /etc/ssh/ssh_host_$(key)_key=0600)
