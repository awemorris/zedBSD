include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
INIT_DATA := /etc/rc.conf=userland/base/etc/rc.conf \
	/etc/fstab=userland/base/etc/fstab \
	/etc/net.conf=userland/base/etc/net.conf \
	/etc/service.d/syslogd=userland/base/init/services/syslogd \
	/etc/service.d/getty_console=userland/base/init/services/getty_console \
	/etc/service.d/networkd=userland/base/init/services/networkd \
	/etc/service.d/net=userland/base/init/services/net \
	/etc/service.d/cron=userland/base/init/services/cron \
	/etc/service.d/ntpdate=userland/base/init/services/ntpdate
$(eval $(call ZEDBSD_USERLAND_PACKAGE,init,init,*,y,basic,userland/base/init/main.c userland/base/service/service-config.c,n,base,,,0755,program,$(INIT_DATA),,sbin))
