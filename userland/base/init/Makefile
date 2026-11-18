# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

INIT_DATA := /etc/rc.conf=userland/base/etc/rc.conf \
	/etc/fstab=userland/base/etc/fstab \
	/etc/net.conf=userland/base/etc/net.conf \
	/etc/service.d/syslogd=userland/base/init/services/syslogd \
	/etc/service.d/getty_console=userland/base/init/services/getty_console \
	/etc/service.d/networkd=userland/base/init/services/networkd \
	/etc/service.d/networking=userland/base/init/services/networking \
	/etc/service.d/cron=userland/base/init/services/cron \
	/etc/service.d/ntpdate=userland/base/init/services/ntpdate \
	/etc/service.d/homes=userland/base/init/services/homes \
	/usr/libexec/make-homes=userland/base/init/make-homes

INIT_SOURCES := \
	userland/base/init/main.c \
	userland/base/init/root-check.c \
	userland/base/service/service-config.c \
	userland/base/service/rcconf.c \
	userland/base/service/zsv1-protocol.c \
	userland/base/service/zsv1-server.c

$(eval $(call USERLAND_PACKAGE,init,init,*,y,basic,$(INIT_SOURCES),n,base,,,0755,program,$(INIT_DATA),,sbin))
