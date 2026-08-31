# WS001 p015 exact production creation-request locking fixture.

REPO := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
OUT ?= $(REPO)/build/ws001-p015-creation-request-host
CC ?= cc

CPPFLAGS := -DZEDBSD_USER_ABI_LP64 -I$(REPO)/include \
	-I$(REPO)/include/uapi -I$(REPO)/src -I$(REPO)/libc/include -I$(REPO)
CFLAGS := -std=c11 -O0 -Wall -Wextra -Werror -ffunction-sections \
	-fdata-sections
LDFLAGS := -Wl,--gc-sections
TEST := $(REPO)/plan/ws001-posix/tests/credential-creation-request-host-test.c

.PHONY: all run
all: run

run: $(OUT)/test
	$(OUT)/test

$(OUT):
	mkdir -p $@

$(OUT)/inode.o: $(REPO)/src/kern/inode.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/test: $(TEST) $(OUT)/inode.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDFLAGS) -o $@
