# WS052 (ws052-p003): the host test of the ACPI side of S0 idle.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
#   make -C plan/ws052/tests     build build/ws052/host/aml-host-sleep (ASan/UBSan)
#
# The AML host harness (plan/tools/aml-host/aml-host.c) with acpi-sleep.c and
# host-sleep.c, which plays the WS052_SLEEP steps before the harness frees
# the namespace (the linker's --wrap=drv_acpi_reset).

REPO := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
OUT ?= $(REPO)/build/ws052/host
CC ?= cc
HARNESS := $(REPO)/plan/tools/aml-host

SOURCES := $(sort $(wildcard $(REPO)/src/drivers/acpi/aml-*.c)) \
	$(REPO)/src/drivers/acpi/acpi-tables.c \
	$(REPO)/src/drivers/acpi/acpi-event.c \
	$(REPO)/src/drivers/acpi/acpi-ec.c \
	$(REPO)/src/drivers/acpi/acpi-text.c \
	$(REPO)/src/drivers/acpi/acpi-resource.c \
	$(REPO)/src/drivers/acpi/acpi-sleep.c \
	$(REPO)/src/drivers/acpi/acpi-wmi.c \
	$(REPO)/src/drivers/acpi/acpi-wmi-table.c \
	$(HARNESS)/aml-host.c \
	$(HARNESS)/aml-host-hardware.c \
	$(HARNESS)/aml-host-wmi.c \
	$(REPO)/plan/ws052/tests/host-sleep.c
HEADERS := $(wildcard $(REPO)/src/drivers/acpi/*.h) \
	$(REPO)/include/drivers/acpi/acpi.h \
	$(HARNESS)/aml-host-hardware.h

WARNINGS := -Wall -Wextra -Werror -Wdeclaration-after-statement \
	-Wstrict-prototypes -Wmissing-prototypes -Wshadow
CPPFLAGS := -I$(REPO)/include -I$(REPO)/src -I$(HARNESS)
SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer

.PHONY: all
all: $(OUT)/aml-host-sleep

$(OUT)/aml-host-sleep: $(SOURCES) $(HEADERS) $(lastword $(MAKEFILE_LIST))
	@mkdir -p $(OUT)
	$(CC) -std=gnu11 -g -O1 $(WARNINGS) $(SANITIZE) $(CPPFLAGS) \
		-Wl,--wrap=drv_acpi_reset -o $@ $(SOURCES)

# ws052-p004: pci-power.c against a simulated PCI layer.
#   make -C plan/ws052/tests pci-power && build/ws052/host/pci-power
.PHONY: pci-power
pci-power: $(OUT)/pci-power

$(OUT)/pci-power: $(REPO)/src/drivers/pci/pci-power.c $(REPO)/plan/ws052/tests/host-pci-power.c \
		$(REPO)/include/drivers/pci/pci.h $(lastword $(MAKEFILE_LIST))
	@mkdir -p $(OUT)
	$(CC) -std=gnu11 -g -O1 $(WARNINGS) $(SANITIZE) -I$(REPO)/include -I$(REPO)/src \
		-o $@ $(REPO)/src/drivers/pci/pci-power.c $(REPO)/plan/ws052/tests/host-pci-power.c

# ws052-p009: the i915 request worker's park (src/drivers/gpu/i915/park.c).
#   make -C plan/ws052/tests i915-park && build/ws052/host/i915-park
.PHONY: i915-park
i915-park: $(OUT)/i915-park

$(OUT)/i915-park: $(REPO)/src/drivers/gpu/i915/park.c $(REPO)/src/drivers/gpu/i915/park.h \
		$(REPO)/plan/ws052/tests/host-i915-park.c $(lastword $(MAKEFILE_LIST))
	@mkdir -p $(OUT)
	$(CC) -std=gnu11 -g -O1 $(WARNINGS) $(SANITIZE) -I$(REPO)/include -I$(REPO)/src \
		-o $@ $(REPO)/src/drivers/gpu/i915/park.c $(REPO)/plan/ws052/tests/host-i915-park.c

# ws052-p009 stage (b): the GGTT restore (ggtt.c).
#   make -C plan/ws052/tests i915-ggtt && build/ws052/host/i915-ggtt
I915 := $(REPO)/src/drivers/gpu/i915
I915_HOST_CPPFLAGS := -I$(REPO)/include -I$(REPO)/src -I$(REPO) -DKERN_USER_ABI_LP64
.PHONY: i915-ggtt
i915-ggtt: $(OUT)/i915-ggtt

$(OUT)/i915-ggtt: $(I915)/ggtt.c $(I915)/ggtt.h $(I915)/memory.h $(REPO)/plan/ws052/tests/host-i915-ggtt.c $(lastword $(MAKEFILE_LIST))
	@mkdir -p $(OUT)
	$(CC) -std=gnu11 -g -O1 $(WARNINGS) $(SANITIZE) $(I915_HOST_CPPFLAGS) \
		-o $@ $(I915)/ggtt.c $(REPO)/plan/ws052/tests/host-i915-ggtt.c
