include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# SPDX-License-Identifier: Zlib

ZEDBSD_BC_SOURCES := userland/base/bc/src/args.c \
	userland/base/bc/src/bc.c userland/base/bc/src/bc_lex.c \
	userland/base/bc/src/bc_parse.c userland/base/bc/src/data.c \
	userland/base/bc/src/file.c userland/base/bc/src/lang.c \
	userland/base/bc/src/lex.c userland/base/bc/src/main.c \
	userland/base/bc/src/num.c userland/base/bc/src/opt.c \
	userland/base/bc/src/parse.c userland/base/bc/src/program.c \
	userland/base/bc/src/rand.c userland/base/bc/src/read.c \
	userland/base/bc/src/vector.c userland/base/bc/src/vm.c \
	userland/base/bc/gen/bc_help.c userland/base/bc/gen/lib.c \
	userland/base/bc/gen/lib2.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,bc,bc,*,y,basic,$(ZEDBSD_BC_SOURCES),y,base,,))
