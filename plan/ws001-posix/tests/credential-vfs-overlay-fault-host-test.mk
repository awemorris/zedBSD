# WS001 p022 production overlay create/materialize/copy-up fault matrix.

REPO := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
OUT ?= $(REPO)/build/ws001-p022-overlay-fault-host
CC ?= cc
OBJCOPY ?= objcopy

CPPFLAGS := -DZEDBSD_USER_ABI_LP64 -I$(REPO)/include \
	-I$(REPO)/include/uapi -I$(REPO)/src -I$(REPO)/libc/include -I$(REPO)
CFLAGS := -std=c11 -O0 -Wall -Wextra -Werror -ffunction-sections \
	-fdata-sections
LDFLAGS := -Wl,--gc-sections
TEST := $(REPO)/plan/ws001-posix/tests/credential-vfs-overlay-fault-host-test.c
SELF := $(lastword $(MAKEFILE_LIST))
SANITIZER_CFLAGS := -std=c11 -O0 -g -Wall -Wextra -Werror \
	-ffunction-sections -fdata-sections -fsanitize=address,undefined \
	-fno-omit-frame-pointer
SANITIZER_LDFLAGS := -Wl,--gc-sections -fsanitize=address,undefined
ANALYZER_CFLAGS := -std=c11 -O0 -g -Wall -Wextra -Werror -fanalyzer \
	-ffunction-sections -fdata-sections

.PHONY: all run sanitize analyze
all: run

run: $(OUT)/test
	$(OUT)/test

sanitize:
	$(MAKE) -f $(SELF) OUT=$(OUT)-sanitizer \
		CFLAGS='$(SANITIZER_CFLAGS)' LDFLAGS='$(SANITIZER_LDFLAGS)' run

analyze: $(OUT)/analyzer-test
	$(OUT)/analyzer-test

$(OUT):
	mkdir -p $@

$(OUT)/overlay.o: $(REPO)/src/kern/overlayfs.c $(SELF) | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DZEDBSD_OVERLAY_CONTENT_HOST_TEST \
		-Wno-unused-const-variable \
		-c $< -o $@
	$(OBJCOPY) \
		--redefine-sym overlay_create=ws001_overlay_create \
		--redefine-sym overlay_mknod=ws001_overlay_mknod \
		--redefine-sym overlay_ensure_upper_dir=ws001_overlay_ensure_upper_dir \
		--redefine-sym overlay_copy_up_regular=ws001_overlay_copy_up_regular \
		--redefine-sym overlay_alloc_inode=ws001_overlay_alloc_inode $@
	$(OBJCOPY) \
		--globalize-symbol=ws001_overlay_create \
		--globalize-symbol=ws001_overlay_mknod \
		--globalize-symbol=ws001_overlay_ensure_upper_dir \
		--globalize-symbol=ws001_overlay_copy_up_regular \
		--globalize-symbol=ws001_overlay_alloc_inode $@

$(OUT)/test: $(TEST) $(OUT)/overlay.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDFLAGS) -o $@

# Keep the production object on the ordinary warning gate; GCC's analyzer is
# focused on the programmable fixture while the resulting production-linked
# executable proves that the analyzed harness still drives the real branches.
$(OUT)/analyzer-test: $(TEST) $(OUT)/overlay.o
	$(CC) $(CPPFLAGS) $(ANALYZER_CFLAGS) $^ $(LDFLAGS) -o $@
