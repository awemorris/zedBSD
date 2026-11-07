ZEDBSD_CONFIG        := mk/platform-x86_64/zedbsd.cfg
NATIVE_ZEDBSD_CONFIG := mk/platform-x86_64/zedbsd-native.cfg
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
bootloader-platform: $(BUILD)/uefi/BOOTX64.EFI \
                     $(BUILD)/bootloader/stage1.bin \
                     $(BUILD)/bootloader/stage1-native.bin \
                     $(BUILD)/bootloader/stage2-chain.bin \
                     $(BUILD)/bootloader/partition-pbr.bin \
                     $(BUILD)/bootloader/BOOTZBSD.EXE

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
	$(NOCT) --path=tools/build mk/platform-x86_64/tools/check-bootx64.noct $@

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

#
# BIOS bootloaders
#

# Stage1: MBR
$(BUILD)/bootloader/stage1.bin: $(BUILD)/bootloader/stage1.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512

$(BUILD)/bootloader/stage1.elf: $(BUILD)/bootloader/stage1.o
	$(LD) -m elf_i386 -T $(BIOS_LOADER)/stage1.ld $< -o $@

$(BUILD)/bootloader/stage1.o: $(BIOS_LOADER)/stage1.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -DZBL_STAGE2_LBA_OVERRIDE=34 -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/stage1-native.bin: $(BUILD)/bootloader/stage1-native.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512

$(BUILD)/bootloader/stage1-native.elf: $(BUILD)/bootloader/stage1-native.o
	$(LD) -m elf_i386 -T $(BIOS_LOADER)/stage1.ld $< -o $@

$(BUILD)/bootloader/stage1-native.o: $(BIOS_LOADER)/stage1.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

# Stage2: Sectors after MBR. Loads the PBR.
$(BUILD)/bootloader/stage2-chain.bin: $(BUILD)/bootloader/stage2-chain.raw
	$(NOCT) --path=tools/build tools/build/finalize-bios-stage2.noct --machine pcat $< $@

$(BUILD)/bootloader/stage2-chain.raw: $(BUILD)/bootloader/stage2-chain.elf
	$(OBJCOPY) -O binary -j .text $< $@

$(BUILD)/bootloader/stage2-chain.elf: $(BUILD)/bootloader/stage2-chain.o
	$(LD) -m elf_i386 -T $(BIOS_LOADER)/stage2.ld $< -o $@

$(BUILD)/bootloader/stage2-chain.o: $(BIOS_LOADER)/stage2-chain.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

# Stage3: PBR. Loads BOOTZBSD.EXE.
$(BUILD)/bootloader/partition-pbr.bin: $(BUILD)/bootloader/partition-pbr.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 2048

$(BUILD)/bootloader/partition-pbr.elf: $(BUILD)/bootloader/partition-pbr.o
	$(LD) -m elf_i386 --image-base=0 -Ttext=0 -e _start $< -o $@

$(BUILD)/bootloader/partition-pbr.o: $(BIOS_LOADER)/partition-pbr.S \
	bootloader/include/stage2-header.inc
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

# Final Stage: BIOS/DOS dual-purpose bootloader.
$(BUILD)/bootloader/BOOTZBSD.EXE: $(BUILD)/bootloader/bootzbsd.bin
	$(NOCT) --path=tools/build tools/build/make-mz-exe.noct --entry 0x20 $< $@

$(BUILD)/bootloader/bootzbsd.bin: $(BUILD)/bootloader/bootzbsd.raw
	$(NOCT) --path=tools/build tools/build/finalize-bios-stage2.noct --machine pcat $< $@

$(BUILD)/bootloader/bootzbsd.raw: $(BUILD)/bootloader/bootzbsd.elf
	$(OBJCOPY) -O binary -j .text $< $@

$(BUILD)/bootloader/bootzbsd.elf: $(BUILD)/bootloader/bootzbsd.o \
				  $(BUILD)/bootloader/bios-zedbsd-config.i386.o \
				  $(BUILD)/bootloader/bios-logo.i386.o \
				  $(BUILD)/bootloader/common-logo-path.i386.o \
				  $(BUILD)/bootloader/bios-fat-directory.i386.o \
				  $(BUILD)/bootloader/bios-memory-map.i386.o \
				  $(BUILD)/bootloader/common-memory-map.i386.o
	$(LD) -m elf_i386 -T $(BIOS_LOADER)/bootzbsd.ld $(filter %.o,$^) -o $@

$(BUILD)/bootloader/bootzbsd.o: $(BIOS_LOADER)/bootzbsd.S \
				bootloader/bios/fat-directory.h \
				bootloader/bios/logo.h \
				bootloader/common/logo-path.h \
				bootloader/include/disk-layout.inc \
				bootloader/include/stage2-header.inc \
				bootloader/include/mbr.inc \
				bootloader/include/fat16.inc \
				bootloader/include/elf.inc \
				bootloader/include/amd64-handoff.h \
				bootloader/include/boot-parameter-handoff.h \
				bootloader/include/boot-parameter-record.inc \
				bootloader/uefi/zedbsd-config.h \
				include/kern/boot.h
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/bios-zedbsd-config.i386.o: bootloader/uefi/zedbsd-config.c
	@mkdir -p $(dir $@)
	$(CC) -m16 -march=i386 -mtune=i386 -Os -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-builtin -Wall -Wextra -Werror -I. -c $< -o $@

$(BUILD)/bootloader/bios-fat-directory.i386.o: bootloader/bios/fat-directory.c
	@mkdir -p $(dir $@)
	$(CC) -m16 -march=i386 -mtune=i386 -Os -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-builtin -Wall -Wextra -Werror -I. -c $< -o $@

$(BUILD)/bootloader/bios-logo.i386.o: bootloader/bios/logo.c
	@mkdir -p $(dir $@)
	$(CC) -m16 -march=i386 -mtune=i386 -Os -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-builtin -Wall -Wextra -Werror -I. -c $< -o $@

$(BUILD)/bootloader/common-logo-path.i386.o: bootloader/common/logo-path.c
	@mkdir -p $(dir $@)
	$(CC) -m16 -march=i386 -mtune=i386 -Os -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-builtin -Wall -Wextra -Werror -I. -c $< -o $@

$(BUILD)/bootloader/bios-memory-map.i386.o: bootloader/bios/memory-map.c bootloader/common/memory-map.h bootloader/bios/memory-map.h bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(CC) -m16 -march=i386 -mtune=i386 -Os -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-builtin -Wall -Wextra -Werror -I. -c $< -o $@

$(BUILD)/bootloader/common-memory-map.i386.o: bootloader/common/memory-map.c bootloader/common/memory-map.h bootloader/bios/memory-map.h bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(CC) -m16 -march=i386 -mtune=i386 -Os -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-builtin -Wall -Wextra -Werror -I. -c $< -o $@


