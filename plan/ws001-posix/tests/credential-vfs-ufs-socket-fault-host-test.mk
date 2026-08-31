# WS001 p022 production UFS pathname-socket rollback-failure fixture.

REPO := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
OUT ?= $(REPO)/build/ws001-p022-ufs-socket-fault-host
CC ?= cc
OBJCOPY ?= objcopy

CPPFLAGS := -DZEDBSD_USER_ABI_LP64 -I$(REPO)/include \
	-I$(REPO)/include/uapi -I$(REPO)/src -I$(REPO)/libc/include -I$(REPO)
CFLAGS := -std=c11 -O0 -Wall -Wextra -Werror -ffunction-sections \
	-fdata-sections
LDFLAGS := -Wl,--gc-sections
TEST := $(REPO)/plan/ws001-posix/tests/credential-vfs-ufs-socket-fault-host-test.c
SELF := $(lastword $(MAKEFILE_LIST))
SANITIZER_CFLAGS := -std=c11 -O0 -g -Wall -Wextra -Werror \
	-ffunction-sections -fdata-sections -fsanitize=address,undefined \
	-fno-omit-frame-pointer
SANITIZER_LDFLAGS := -Wl,--gc-sections -fsanitize=address,undefined
ANALYZER_CFLAGS := -std=c11 -O0 -g -Wall -Wextra -Werror -fanalyzer \
	-ffunction-sections -fdata-sections

.PHONY: all run sanitize analyze
all: run

run: $(OUT)/ufs1-test $(OUT)/ufs2-test
	$(OUT)/ufs1-test
	$(OUT)/ufs2-test

sanitize:
	$(MAKE) -f $(SELF) OUT=$(OUT)-sanitizer \
		CFLAGS='$(SANITIZER_CFLAGS)' LDFLAGS='$(SANITIZER_LDFLAGS)' run

analyze: $(OUT)/ufs1-analyzer-test $(OUT)/ufs2-analyzer-test
	$(OUT)/ufs1-analyzer-test
	$(OUT)/ufs2-analyzer-test

$(OUT):
	mkdir -p $@

$(OUT)/ufs1.o: $(REPO)/src/drivers/fs/ufs1/ufs1-vfs.c $(SELF) | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
	$(OBJCOPY) \
		--redefine-sym restore_directory_block=ws001_ufs1_restore_directory_block \
		--redefine-sym discard_new_inode_after_error=ws001_ufs1_discard_new_inode_after_error \
		--redefine-sym ufs1_mknod=ws001_ufs1_mknod \
		$@
	$(OBJCOPY) \
		--globalize-symbol=ws001_ufs1_restore_directory_block \
		--globalize-symbol=ws001_ufs1_discard_new_inode_after_error \
		--globalize-symbol=ws001_ufs1_mknod $@

$(OUT)/ufs1-endian.o: $(REPO)/src/drivers/fs/ufs1/ufs1-endian.c $(SELF) | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/ufs2.o: $(REPO)/src/drivers/fs/ufs2/ufs2-vfs.c $(SELF) | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
	$(OBJCOPY) \
		--redefine-sym restore_directory_block=ws001_ufs2_restore_directory_block \
		--redefine-sym discard_new_inode_after_error=ws001_ufs2_discard_new_inode_after_error \
		--redefine-sym ufs2_mknod=ws001_ufs2_mknod \
		$@
	$(OBJCOPY) \
		--globalize-symbol=ws001_ufs2_restore_directory_block \
		--globalize-symbol=ws001_ufs2_discard_new_inode_after_error \
		--globalize-symbol=ws001_ufs2_mknod $@

$(OUT)/ufs2-endian.o: $(REPO)/src/drivers/fs/ufs2/ufs2-endian.c $(SELF) | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/ufs1-test: $(TEST) $(OUT)/ufs1.o $(OUT)/ufs1-endian.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -DWS001_P022_UFS1 $^ $(LDFLAGS) -o $@

$(OUT)/ufs2-test: $(TEST) $(OUT)/ufs2.o $(OUT)/ufs2-endian.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -DWS001_P022_UFS2 $^ $(LDFLAGS) -o $@

# Analyzer scope is the programmable fixture.  The UFS production/endian
# objects retain the ordinary warning profile and are linked into the binary
# that is executed after the analyzer pass.
$(OUT)/ufs1-analyzer-test: $(TEST) $(OUT)/ufs1.o $(OUT)/ufs1-endian.o
	$(CC) $(CPPFLAGS) $(ANALYZER_CFLAGS) -DWS001_P022_UFS1 $^ \
		$(LDFLAGS) -o $@

$(OUT)/ufs2-analyzer-test: $(TEST) $(OUT)/ufs2.o $(OUT)/ufs2-endian.o
	$(CC) $(CPPFLAGS) $(ANALYZER_CFLAGS) -DWS001_P022_UFS2 $^ \
		$(LDFLAGS) -o $@
