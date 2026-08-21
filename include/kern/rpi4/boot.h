#ifndef ZEDBSD_KERN_RPI4_BOOT_H
#define ZEDBSD_KERN_RPI4_BOOT_H

#include <kern/boot.h>

#define ZEDBSD_RPI4_HANDOFF_MAGIC 0x34495052U
struct rpi4_boot_handoff {
	struct boot_handoff common;
	uint32_t extension_magic;
	uint16_t extension_version;
	uint16_t extension_size;
	uint64_t fdt_phys;
	uint64_t framebuffer_phys;
	uint64_t framebuffer_size;
	uint64_t sdhci_phys;
	uint32_t framebuffer_width;
	uint32_t framebuffer_height;
	uint32_t framebuffer_pitch;
	uint32_t framebuffer_format;
	uint32_t sdhci_irq;
	uint32_t reserved[3];
} __attribute__((packed));

_Static_assert(__builtin_offsetof(struct rpi4_boot_handoff, extension_magic)==24,
    "Pi 4 handoff must preserve the common 24-byte prefix");

#endif
