# ws291-p021: the Raspberry Pi 5 (V3D 7.1) measurement image: the CI Pi 5 image with the GPU driver on and mview (its
# qs40 model under /usr/share/mview/qs40), the onboard Ethernet by DHCP and root reached over SSH with the guest
# harness's key (plan/tmp/guest, made by plan/tools/guest/guest.py keys) and fixed host keys.  The login stays the
# console's; the GPU starts with root's sysctl hw.gpu.start=1 (or rpi5gpu.start=boot in cmdline.txt).
# Build:  sh plan/ws291/tests/rpi5-image.sh BUILD
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/ci/config-rpi5.mk

CONFIG_DRIVER_BCM2712_GPU := y

# The 3D scene.
ZEDBSD_USER_PROGRAMS += $(filter-out $(ZEDBSD_USER_PROGRAMS),mview)

# The network and root's SSH.
RPI5_P021_GUEST := plan/tmp/guest
RPI5_P021_FILES := /etc/net.conf=plan/ws291/tests/net-rpi5.conf \
	/root/.ssh/authorized_keys=$(RPI5_P021_GUEST)/id_ed25519.pub \
	$(foreach key,rsa ecdsa ed25519,/etc/ssh/ssh_host_$(key)_key=$(RPI5_P021_GUEST)/ssh_host_$(key)_key \
	/etc/ssh/ssh_host_$(key)_key.pub=$(RPI5_P021_GUEST)/ssh_host_$(key)_key.pub)
ZEDBSD_EXTRA_INPUTS += $(foreach item,$(RPI5_P021_FILES),$(lastword $(subst =, ,$(item))))
ZEDBSD_EXTRA_FILES += $(foreach item,$(RPI5_P021_FILES),--file $(item)) \
	--mode /root/.ssh=0700 --mode /root/.ssh/authorized_keys=0600 \
	$(foreach key,rsa ecdsa ed25519,--mode /etc/ssh/ssh_host_$(key)_key=0600)
