# WS001 p016 deterministic directory-fsync host test.

REPO := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
OUT ?= $(REPO)/build/ws001-p016-host
CC ?= cc
OBJCOPY ?= objcopy

CPPFLAGS := -DZEDBSD_USER_ABI_LP64 -I$(REPO)/include \
	-I$(REPO)/include/uapi -I$(REPO)/src -I$(REPO)/libc/include -I$(REPO)
BASE_CFLAGS := -std=c11 -O0 -Wall -Wextra -Werror -ffunction-sections \
	-fdata-sections
CFLAGS := $(BASE_CFLAGS) $(CFLAGS_EXTRA)
PRODUCTION_CFLAGS_EXTRA ?= $(CFLAGS_EXTRA)
PRODUCTION_CFLAGS := $(BASE_CFLAGS) $(PRODUCTION_CFLAGS_EXTRA)
LDFLAGS := -Wl,--gc-sections $(LDFLAGS_EXTRA) $(REPO)/src/kern/io-stats.c
TEST := $(REPO)/plan/ws001/tests/directory-fsync-host-test.c
SELF := $(lastword $(MAKEFILE_LIST))

.PHONY: all run mutation-run run-sanitize analyze
all: run

run: $(OUT)/vfs-test $(OUT)/ufs-test  \
	$(OUT)/ufs-mutation-test $(OUT)/overlay-test
	$(OUT)/vfs-test
	$(OUT)/ufs-test

	$(OUT)/ufs-mutation-test
	$(OUT)/overlay-test

mutation-run:  $(OUT)/ufs-mutation-test

	$(OUT)/ufs-mutation-test

run-sanitize:
	$(MAKE) -f $(firstword $(MAKEFILE_LIST)) \
		OUT=$(OUT)-sanitize \
		CFLAGS_EXTRA='-fsanitize=address,undefined -fno-omit-frame-pointer --param=asan-globals=0' \
		LDFLAGS_EXTRA='-fsanitize=address,undefined' mutation-run

analyze:
	$(MAKE) -f $(firstword $(MAKEFILE_LIST)) \
		OUT=$(OUT)-analyze CFLAGS_EXTRA=-fanalyzer \
		PRODUCTION_CFLAGS_EXTRA= mutation-run

$(OUT):
	mkdir -p $@

$(OUT)/file.o: $(REPO)/src/kern/file.c $(SELF) | $(OUT)
	$(CC) $(CPPFLAGS) $(PRODUCTION_CFLAGS) -c $< -o $@

$(OUT)/vfs-test: $(TEST) $(OUT)/file.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -DWS001_P016_VFS $^ $(LDFLAGS) -o $@



$(OUT)/ufs.o: $(REPO)/plan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-vfs.c $(SELF) | $(OUT)
	$(CC) $(CPPFLAGS) $(PRODUCTION_CFLAGS) -c $< -o $@
	$(OBJCOPY) --globalize-symbol=ufs_file_sync $@

$(OUT)/ufs-test: $(TEST)  $(OUT)/ufs.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -DWS001_P016_UFS $^ $(LDFLAGS) -o $@







$(OUT)/ufs-mutation.o: $(REPO)/plan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-vfs.c $(SELF) | $(OUT)
	$(CC) $(CPPFLAGS) $(PRODUCTION_CFLAGS) -c $< -o $@
	$(OBJCOPY) --redefine-sym=dir_replace=ufs_dir_replace $@
	$(OBJCOPY) --globalize-symbol=ufs_dir_replace $@

$(OUT)/ufs-endian.o: $(REPO)/plan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-endian.c $(SELF) | $(OUT)
	$(CC) $(CPPFLAGS) $(PRODUCTION_CFLAGS) -c $< -o $@

$(OUT)/ufs-mutation-test: $(TEST) $(OUT)/ufs-mutation.o \
	$(OUT)/ufs-endian.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -DWS001_P023_UFS_MUTATION $^ \
		$(LDFLAGS) -o $@

$(OUT)/overlay.o: $(REPO)/src/drivers/fs/overlayfs.c $(SELF) | $(OUT)
	$(CC) $(CPPFLAGS) $(PRODUCTION_CFLAGS) \
		-Wno-unused-const-variable \
		-DZEDBSD_OVERLAY_CONTENT_HOST_TEST -c $< -o $@
	$(OBJCOPY) --globalize-symbol=overlay_directory_fsync $@

$(OUT)/overlay-test: $(TEST) $(OUT)/overlay.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -DWS001_P016_OVERLAY $^ $(LDFLAGS) -o $@
