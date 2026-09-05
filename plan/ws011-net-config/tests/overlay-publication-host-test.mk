# WS011 p009: real overlay branches; programmable namespace/storage and locks.
REPO := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
OUT ?= $(REPO)/build/ws011-p009-overlay-publication-host
CC ?= cc
OBJCOPY ?= objcopy
SELF := $(lastword $(MAKEFILE_LIST))
TEST := $(REPO)/plan/ws011-net-config/tests/overlay-publication-host-test.c
FIXTURE := $(REPO)/plan/ws001-posix/tests/credential-vfs-overlay-fault-host-test.c
CPPFLAGS := -DZEDBSD_USER_ABI_LP64 -I$(REPO)/include \
	-I$(REPO)/include/uapi -I$(REPO)/src -I$(REPO)/libc/include -I$(REPO)
CFLAGS := -std=c11 -O0 -g -Wall -Wextra -Werror -ffunction-sections \
	-fdata-sections $(CFLAGS_EXTRA)
LDFLAGS := -Wl,--gc-sections $(LDFLAGS_EXTRA)

.PHONY: all run sanitize
all: run
run: $(OUT)/test
	timeout 15s $(OUT)/test

sanitize:
	$(MAKE) -f $(SELF) OUT=$(OUT)-sanitize \
		CFLAGS_EXTRA='-fsanitize=address,undefined -fno-omit-frame-pointer' \
		LDFLAGS_EXTRA='-fsanitize=address,undefined' run

$(OUT):
	mkdir -p $@

$(OUT)/overlay.o: $(REPO)/src/kern/overlayfs.c $(SELF) | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DZEDBSD_OVERLAY_CONTENT_HOST_TEST \
		-Wno-unused-const-variable -c $< -o $@
	$(OBJCOPY) \
		--redefine-sym overlay_create=ws001_overlay_create \
		--redefine-sym overlay_mknod=ws001_overlay_mknod \
		--redefine-sym overlay_ensure_upper_dir=ws001_overlay_ensure_upper_dir \
		--redefine-sym overlay_copy_up_regular=ws001_overlay_copy_up_regular \
		--redefine-sym overlay_alloc_inode=ws001_overlay_alloc_inode \
		--redefine-sym overlay_rename=ws011_overlay_rename \
		--redefine-sym overlay_regular_fsync=ws011_overlay_regular_fsync \
		--redefine-sym overlay_regular_close=ws011_overlay_regular_close \
		--redefine-sym overlay_sync_mount=ws011_overlay_sync_mount $@
	$(OBJCOPY) --globalize-symbol=ws001_overlay_create \
		--globalize-symbol=ws001_overlay_mknod \
		--globalize-symbol=ws001_overlay_ensure_upper_dir \
		--globalize-symbol=ws001_overlay_copy_up_regular \
		--globalize-symbol=ws001_overlay_alloc_inode \
		--globalize-symbol=ws011_overlay_rename \
		--globalize-symbol=ws011_overlay_regular_fsync \
		--globalize-symbol=ws011_overlay_regular_close \
		--globalize-symbol=ws011_overlay_sync_mount $@

$(OUT)/test: $(TEST) $(FIXTURE) $(OUT)/overlay.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST) $(OUT)/overlay.o $(LDFLAGS) -o $@
