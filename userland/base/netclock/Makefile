include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# netclock sets the clock from the network at boot (BUG-320), for a machine
# without a battery-backed clock of its own, whose clock otherwise starts at
# the beginning of the year and makes TLS refuse certificates as not yet
# valid.  It is the netclock service only: ntpdate -w pool.ntp.org, a daemon
# started after networking, so the boot does not wait for it; it tries the
# servers every 10 seconds for about ten minutes and then gives up, leaving the
# clock as it was.  It is not a default: the Raspberry Pi 4 configuration
# selects it, and rc.conf names the service as optional, so an image without
# it says nothing.  ntpdate itself is a default of every image.
NETCLOCK_DATA := /etc/service.d/netclock=userland/base/netclock/service
$(eval $(call USERLAND_PACKAGE,netclock,netclock - the clock from the network at boot,*,n,data,,y,base,base/netclock,,,data,$(NETCLOCK_DATA)))
