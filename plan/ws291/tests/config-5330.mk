# ws291-p022: the Latitude 5330 (i915, Alder Lake) measurement image: the 5330 regression image
# (plan/tools/hw5330/config-5330.mk: CI amd64, keiland-shot) with mview (its qs40 model under /usr/share/mview/qs40),
# the session of the account "user" logged in at boot, and root reached over SSH with the guest harness's key
# (plan/tmp/guest, made by plan/tools/guest/guest.py keys) and fixed host keys.  Written to the 5330's NVMe zedBSD
# partitions by plan/ws291/tests/5330-mview.sh.
#   make ZEDBSD_CONFIG=plan/ws291/tests/config-5330.mk BUILD=<dir> disk-image
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/tools/hw5330/config-5330.mk

# The 3D scene and the kernel's sysctl reads.
ZEDBSD_USER_PROGRAMS += $(filter-out $(ZEDBSD_USER_PROGRAMS),mview sysctl)

# The session logged in at boot, root's SSH.
I915_P022_GUEST := plan/tmp/guest
I915_P022_FILES := /etc/keiland/autologin=plan/ws291/tests/autologin-user \
	/root/.ssh/authorized_keys=$(I915_P022_GUEST)/id_ed25519.pub \
	$(foreach key,rsa ecdsa ed25519,/etc/ssh/ssh_host_$(key)_key=$(I915_P022_GUEST)/ssh_host_$(key)_key \
	/etc/ssh/ssh_host_$(key)_key.pub=$(I915_P022_GUEST)/ssh_host_$(key)_key.pub)
ZEDBSD_EXTRA_INPUTS += $(foreach item,$(I915_P022_FILES),$(lastword $(subst =, ,$(item))))
ZEDBSD_EXTRA_FILES += $(foreach item,$(I915_P022_FILES),--file $(item)) \
	--mode /root/.ssh=0700 --mode /root/.ssh/authorized_keys=0600 \
	$(foreach key,rsa ecdsa ed25519,--mode /etc/ssh/ssh_host_$(key)_key=0600)
