# WS001 p022 production UFS pathname-socket rollback-failure fixture.

REPO := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
OUT ?= $(REPO)/build/ws001-p022-ufs-socket-fault-host
CC ?= cc
OBJCOPY ?= objcopy

CPPFLAGS := -DZEDBSD_USER_ABI_LP64 -I$(REPO)/include \
	-I$(REPO)/include/uapi -I$(REPO)/src -I$(REPO)/libc/include -I$(REPO)
CFLAGS := -std=c11 -O0 -Wall -Wextra -Werror -ffunction-sections \
	-fdata-sections
LDFLAGS := -Wl,--gc-sections $(REPO)/src/kern/io-stats.c
TEST := $(REPO)/plan/ws001/tests/credential-vfs-ufs-socket-fault-host-test.c
SELF := $(lastword $(MAKEFILE_LIST))
SANITIZER_CFLAGS := -std=c11 -O0 -g -Wall -Wextra -Werror \
	-ffunction-sections -fdata-sections -fsanitize=address,undefined \
	-fno-omit-frame-pointer
SANITIZER_LDFLAGS := -Wl,--gc-sections -fsanitize=address,undefined $(REPO)/src/kern/io-stats.c
ANALYZER_CFLAGS := -std=c11 -O0 -g -Wall -Wextra -Werror -fanalyzer \
	-ffunction-sections -fdata-sections

.PHONY: all run sanitize analyze
all: run

run:  $(OUT)/ufs-test

	$(OUT)/ufs-test

sanitize:
	$(MAKE) -f $(SELF) OUT=$(OUT)-sanitizer \
		CFLAGS='$(SANITIZER_CFLAGS)' LDFLAGS='$(SANITIZER_LDFLAGS)' run

analyze:  $(OUT)/ufs-analyzer-test

	$(OUT)/ufs-analyzer-test

$(OUT):
	mkdir -p $@





$(OUT)/ufs.o: $(REPO)/plan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-vfs.c $(SELF) | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
	$(OBJCOPY) \
		--redefine-sym restore_directory_block=ws001_ufs_restore_directory_block \
		--redefine-sym discard_new_inode_after_error=ws001_ufs_discard_new_inode_after_error \
		--redefine-sym ufs_mknod=ws001_ufs_mknod \
		$@
	$(OBJCOPY) \
		--globalize-symbol=ws001_ufs_restore_directory_block \
		--globalize-symbol=ws001_ufs_discard_new_inode_after_error \
		--globalize-symbol=ws001_ufs_mknod $@

$(OUT)/ufs-endian.o: $(REPO)/plan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-endian.c $(SELF) | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@



$(OUT)/ufs-test: $(TEST) $(OUT)/ufs.o $(OUT)/ufs-endian.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -DWS001_P022_UFS $^ $(LDFLAGS) -o $@

# Analyzer scope is the programmable fixture.  The UFS production/endian
# objects retain the ordinary warning profile and are linked into the binary
# that is executed after the analyzer pass.


$(OUT)/ufs-analyzer-test: $(TEST) $(OUT)/ufs.o $(OUT)/ufs-endian.o
	$(CC) $(CPPFLAGS) $(ANALYZER_CFLAGS) -DWS001_P022_UFS $^ \
		$(LDFLAGS) -o $@
