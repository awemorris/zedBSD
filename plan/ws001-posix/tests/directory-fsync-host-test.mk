# WS001 p016 deterministic directory-fsync host test.

REPO := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
OUT ?= $(REPO)/build/ws001-p016-host
CC ?= cc
OBJCOPY ?= objcopy

CPPFLAGS := -DZEDBSD_USER_ABI_LP64 -I$(REPO)/include \
	-I$(REPO)/include/uapi -I$(REPO)/src -I$(REPO)/libc/include -I$(REPO)
CFLAGS := -std=c11 -O0 -Wall -Wextra -Werror -ffunction-sections \
	-fdata-sections
LDFLAGS := -Wl,--gc-sections
TEST := $(REPO)/plan/ws001-posix/tests/directory-fsync-host-test.c

.PHONY: all run
all: run

run: $(OUT)/vfs-test $(OUT)/ufs-test $(OUT)/overlay-test
	$(OUT)/vfs-test
	$(OUT)/ufs-test
	$(OUT)/overlay-test

$(OUT):
	mkdir -p $@

$(OUT)/file.o: $(REPO)/src/kern/file.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/vfs-test: $(TEST) $(OUT)/file.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -DWS001_P016_VFS $^ $(LDFLAGS) -o $@

$(OUT)/ufs1.o: $(REPO)/src/drivers/fs/ufs1/ufs1-vfs.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
	$(OBJCOPY) --globalize-symbol=ufs1_file_sync $@

$(OUT)/ufs2.o: $(REPO)/src/drivers/fs/ufs2/ufs2-vfs.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
	$(OBJCOPY) --globalize-symbol=ufs2_file_sync $@

$(OUT)/ufs-test: $(TEST) $(OUT)/ufs1.o $(OUT)/ufs2.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -DWS001_P016_UFS $^ $(LDFLAGS) -o $@

$(OUT)/overlay.o: $(REPO)/src/kern/overlayfs.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		-Wno-unused-const-variable \
		-DZEDBSD_OVERLAY_CONTENT_HOST_TEST -c $< -o $@
	$(OBJCOPY) --globalize-symbol=overlay_directory_fsync $@

$(OUT)/overlay-test: $(TEST) $(OUT)/overlay.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -DWS001_P016_OVERLAY $^ $(LDFLAGS) -o $@
