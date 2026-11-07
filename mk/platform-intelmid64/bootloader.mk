ZEDBSD_CONFIG        := mk/platform-intelmid64/zedbsd.cfg
IMAGE_CONTRACT_STAMP := $(BUILD)/.disk-image-contract
IMAGE_STAGE1         := $(if $(filter bios,$(ZEDBSD_VARIANT)), $(BUILD)/bootloader/stage1-native.bin,$(BUILD)/bootloader/stage1.bin)

UEFI_LOADER := bootloader/uefi
BIOS_LOADER := bootloader/pcat

EFI_CC     := $(ZEDBSD_TARGET_LLVM_BIN)/clang --target=x86_64-unknown-windows
EFI_LD     := $(ZEDBSD_TARGET_LLVM_BIN)/lld-link
EFI_NM     := $(ZEDBSD_TARGET_LLVM_BIN)/llvm-nm

EFI_CFLAGS := \
	-std=c11 \
	-ffreestanding \
	-fshort-wchar \
	-mno-red-zone \
	-fno-stack-protector \
	-fno-builtin \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-fno-ident \
	-ffunction-sections \
	-fdata-sections \
	-Os \
	-Wall \
	-Wextra \
	-Werror -I.

.PHONY: bootloader-platform
bootloader-platform: $(BUILD)/uefi/BOOTX64.EFI

#
# UEFI bootloader
#

$(BUILD)/uefi/BOOTX64.EFI: $(BUILD)/uefi/bootx64.o \
			   $(BUILD)/uefi/elf64.o \
			   $(BUILD)/uefi/framebuffer.o \
			   $(BUILD)/uefi/video.o \
			   $(BUILD)/uefi/logo.o \
			   $(BUILD)/uefi/common-logo-path.o \
			   $(BUILD)/uefi/common-boot-override.o \
			   $(BUILD)/uefi/boot-keys.o \
			   $(BUILD)/uefi/common-memory-map.o \
			   $(BUILD)/uefi/memory-map.o \
			   $(BUILD)/uefi/memory-map-v6.o \
			   $(BUILD)/uefi/volume-discovery.o \
			   $(BUILD)/uefi/zedbsd-config.o \
			   $(BUILD)/uefi/root-config.o \
			   $(BUILD)/uefi/common-ufs-read.o \
			   $(BUILD)/uefi/transition.o
	$(EFI_LD) /subsystem:efi_application /entry:efi_main /base:0 /fixed:no /timestamp:0 /nodefaultlib /out:$@ $(filter %.o,$^)
	@test -z "$$($(EFI_NM) -u $@ | grep -Ev ' (__bss_start__|__bss_end__|__end__|___tls_start__|___tls_end__)$$')" || { $(EFI_NM) -u $@; exit 1; }
	$(NOCT) --path=mk/tools:tools/build mk/platform-intelmid/tools/check-bootx64.noct $@

$(BUILD)/uefi/bootx64.o: $(UEFI_LOADER)/bootx64.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/volume-discovery.o: $(UEFI_LOADER)/volume-discovery.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/framebuffer.o: $(UEFI_LOADER)/framebuffer.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/logo.o: $(UEFI_LOADER)/logo.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/common-logo-path.o: bootloader/common/logo-path.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -I. -c $< -o $@

$(BUILD)/uefi/common-ufs-read.o: bootloader/common/ufs-read.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -I. -c $< -o $@

$(BUILD)/uefi/root-config.o: $(UEFI_LOADER)/root-config.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/common-boot-override.o: bootloader/common/boot-override.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -I. -c $< -o $@

$(BUILD)/uefi/boot-keys.o: $(UEFI_LOADER)/boot-keys.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/memory-map-v6.o: $(UEFI_LOADER)/memory-map-v6.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/common-memory-map.o: $(UEFI_LOADER)/../common/memory-map.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/video.o: $(UEFI_LOADER)/video.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/zedbsd-config.o: $(UEFI_LOADER)/zedbsd-config.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/elf64.o: $(UEFI_LOADER)/elf64.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/memory-map.o: $(UEFI_LOADER)/memory-map.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/transition.o: $(UEFI_LOADER)/transition.S
	@mkdir -p $(dir $@)
	$(EFI_CC) -m64 -mno-red-zone -c $< -o $@
