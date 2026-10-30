# The host harness of the AML interpreter (src/drivers/acpi/aml-*.c).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
#   make -C plan/tools/aml-host            build build/aml-host/aml-host (ASan/UBSan)
#   make -C plan/tools/aml-host release    build build/aml-host/aml-host-O2 (stack measurement)

REPO := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
OUT ?= $(REPO)/build/aml-host
CC ?= cc

SOURCES := $(sort $(wildcard $(REPO)/src/drivers/acpi/aml-*.c)) \
	$(REPO)/src/drivers/acpi/acpi-tables.c \
	$(REPO)/src/drivers/acpi/acpi-event.c \
	$(REPO)/src/drivers/acpi/acpi-ec.c \
	$(REPO)/src/drivers/acpi/acpi-text.c \
	$(REPO)/src/drivers/acpi/acpi-resource.c \
	$(REPO)/plan/tools/aml-host/aml-host.c \
	$(REPO)/plan/tools/aml-host/aml-host-hardware.c
HEADERS := $(wildcard $(REPO)/src/drivers/acpi/*.h) \
	$(REPO)/include/drivers/acpi/acpi.h \
	$(REPO)/plan/tools/aml-host/aml-host-hardware.h

WARNINGS := -Wall -Wextra -Werror -Wdeclaration-after-statement \
	-Wstrict-prototypes -Wmissing-prototypes -Wshadow
CPPFLAGS := -I$(REPO)/include -I$(REPO)/src
SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer

.PHONY: all release clean
all: $(OUT)/aml-host
release: $(OUT)/aml-host-O2

$(OUT)/aml-host: $(SOURCES) $(HEADERS) $(lastword $(MAKEFILE_LIST))
	@mkdir -p $(OUT)
	$(CC) -std=gnu11 -g -O1 $(WARNINGS) $(SANITIZE) $(CPPFLAGS) \
		-o $@ $(SOURCES)

$(OUT)/aml-host-O2: $(SOURCES) $(HEADERS) $(lastword $(MAKEFILE_LIST))
	@mkdir -p $(OUT)
	$(CC) -std=gnu11 -O2 $(WARNINGS) $(CPPFLAGS) -o $@ $(SOURCES)

# The interpreter built as close to the kernel as a host program can be
# (clang, -Os, no red zone, no stack protector), for measuring its stack.
.PHONY: stack
stack: $(OUT)/aml-host-Os
$(OUT)/aml-host-Os: $(SOURCES) $(HEADERS) $(lastword $(MAKEFILE_LIST))
	@mkdir -p $(OUT)
	clang -std=gnu11 -Os -mno-red-zone -fno-stack-protector \
		-fno-asynchronous-unwind-tables $(WARNINGS) $(CPPFLAGS) -o $@ $(SOURCES)

clean:
	rm -f $(OUT)/aml-host $(OUT)/aml-host-O2

# Compiles the interpreter as the amd64 kernel does (same target and flags,
# warning 0) and prints the largest stack frames (-fstack-usage).
KERNEL_CC := $(REPO)/build/llvm/bin/clang --target=x86_64-unknown-zedbsd
KERNEL_CFLAGS := -nostdlibinc -I$(REPO)/include -I$(REPO)/src -I$(REPO) \
	-DHAL_ARCH_AMD64 -DHAL_BOARD_PCAT -DKERN_USER_ABI_LP64 \
	-m64 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
	-ffreestanding -fno-pic -fno-pie -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror \
	-Wframe-larger-than=8192 -fno-builtin -fstack-usage
KERNEL_OBJECTS := $(patsubst $(REPO)/src/drivers/acpi/%.c,$(OUT)/kernel/%.o,\
	$(sort $(wildcard $(REPO)/src/drivers/acpi/*.c)))

.PHONY: kernel-check
kernel-check: $(KERNEL_OBJECTS)
	@sort -t'	' -k2 -n -r $(OUT)/kernel/*.su | head -12

$(OUT)/kernel/%.o: $(REPO)/src/drivers/acpi/%.c $(HEADERS)
	@mkdir -p $(OUT)/kernel
	$(KERNEL_CC) $(KERNEL_CFLAGS) -c $< -o $@
