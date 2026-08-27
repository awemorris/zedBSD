include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk
$(eval $(call ZEDBSD_USERLAND_PACKAGE,halt,halt,*,y,basic,userland/base/shutdown-control/main.c userland/base/service/zsv1-protocol.c userland/base/service/zsv1-client.c,y,base,,,0755,program,,,sbin))
