EFI_DIR := $(BUILD)/uefi

EFI_CFLAGS := \
	-std=c11 -march=rv64imac_zicsr_zifencei -mabi=lp64 \
	-mcmodel=medany -mno-relax -fPIC -fvisibility=hidden \
	-ffreestanding -fshort-wchar -fno-stack-protector -fno-builtin \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident \
	-ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror \
	-nostdlibinc -Iinclude -I.

EFI_C := \
	bootloader/uefi/bootriscv64.c \
	bootloader/uefi/elf64.c \
	bootloader/uefi/volume-discovery.c \
	bootloader/uefi/zedbsd-config.c \
	bootloader/uefi/fdt-hartid.c \
	bootloader/uefi/riscv64-reserved.c

EFI_S := \
	bootloader/uefi/riscv64/start.S \
	bootloader/uefi/riscv64/transition.S

EFI_OBJS := \
	$(patsubst bootloader/uefi/%.c,$(EFI_DIR)/%.o,$(EFI_C)) \
	$(patsubst bootloader/uefi/%.S,$(EFI_DIR)/%.o,$(EFI_S))

$(EFI_DIR)/%.o: bootloader/uefi/%.c
	@mkdir -p $(dir $@)
	$(CC) $(EFI_CFLAGS) -MMD -MP -c $< -o $@

$(EFI_DIR)/riscv64/%.o: bootloader/uefi/riscv64/%.S
	@mkdir -p $(dir $@)
	$(CC) -march=rv64imac_zicsr_zifencei -mabi=lp64 -mno-relax -fPIC -c $< -o $@

$(EFI_DIR)/bootriscv64.elf: $(EFI_OBJS) bootloader/uefi/riscv64/loader.ld
	$(LD) -m elf64lriscv -shared -Bsymbolic --no-relax --gc-sections -z norelro -z max-page-size=4096 --hash-style=sysv --no-undefined --pack-dyn-relocs=none -e _start -T bootloader/uefi/riscv64/loader.ld $(EFI_OBJS) -o $@

$(EFI_DIR)/bootriscv64.bin: $(EFI_DIR)/bootriscv64.elf
	$(OBJCOPY) -O binary $< $@

$(EFI_DIR)/BOOTRISCV64.EFI: $(EFI_DIR)/bootriscv64.elf $(EFI_DIR)/bootriscv64.bin platform/riscv64/tools/make-bootriscv64.py platform/riscv64/tools/check-bootriscv64.py
	$(PYTHON) platform/riscv64/tools/make-bootriscv64.py --force --elf $(EFI_DIR)/bootriscv64.elf --binary $(EFI_DIR)/bootriscv64.bin $@
	$(PYTHON) platform/riscv64/tools/check-bootriscv64.py --elf $(EFI_DIR)/bootriscv64.elf $@

-include $(EFI_OBJS:.o=.d)

.PHONY: bootloader-platform
bootloader-platform: $(EFI_DIR)/BOOTRISCV64.EFI
