/* Raspberry Pi 4 eMMC2 SDHCI PIO driver. */
#ifndef ZEDBSD_DRIVERS_RPI4_SDHCI_H
#define ZEDBSD_DRIVERS_RPI4_SDHCI_H

#include <stdint.h>
struct disk;

int rpi4_sdhci_init(uintptr_t physical_base);
struct disk *rpi4_sdhci_disk(void);

#endif
