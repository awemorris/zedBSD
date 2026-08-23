/* SPARC V9 memory and PCI-I/O accessors. */
#include <hal/hal.h>
#include "asi.h"
#include "tte.h"

#define SPARCV9_PCI_IO_VA 0xfffff90000000000ULL

static volatile uint8_t *pci_io;
void sparcv9_io_init(uint64_t base){uint64_t tag=SPARCV9_PCI_IO_VA,tte=sparcv9_tte(base,SPARCV9_TTE_SIZE_4M|SPARCV9_TTE_IE|SPARCV9_TTE_LOCKED|SPARCV9_TTE_SIDE_EFFECT|SPARCV9_TTE_PRIVILEGED|SPARCV9_TTE_WRITE|SPARCV9_TTE_GLOBAL);uintptr_t slot=63U*8U;__asm__ volatile("stxa %0, [%1] 0x58\n\tstxa %2, [%3] 0x5d\n\tmembar #Sync"::"r"(tag),"r"(SPARCV9_MMU_TAG_ACCESS),"r"(tte),"r"(slot):"memory");pci_io=(volatile uint8_t*)(uintptr_t)SPARCV9_PCI_IO_VA;}
uint8_t hal_io_inp8(uint16_t p){return pci_io[p];}
uint16_t hal_io_inp16(uint16_t p){return *(volatile uint16_t*)(pci_io+p);}
uint32_t hal_io_inp32(uint16_t p){return *(volatile uint32_t*)(pci_io+p);}
void hal_io_outp8(uint16_t p,uint8_t v){pci_io[p]=v;}
void hal_io_outp16(uint16_t p,uint16_t v){*(volatile uint16_t*)(pci_io+p)=v;}
void hal_io_outp32(uint16_t p,uint32_t v){*(volatile uint32_t*)(pci_io+p)=v;}
uint8_t hal_mmio_read8(const volatile void*a){return *(const volatile uint8_t*)a;}
uint16_t hal_mmio_read16(const volatile void*a){return *(const volatile uint16_t*)a;}
uint32_t hal_mmio_read32(const volatile void*a){return *(const volatile uint32_t*)a;}
uint64_t hal_mmio_read64(const volatile void*a){return *(const volatile uint64_t*)a;}
void hal_mmio_write8(volatile void*a,uint8_t v){*(volatile uint8_t*)a=v;}
void hal_mmio_write16(volatile void*a,uint16_t v){*(volatile uint16_t*)a=v;}
void hal_mmio_write32(volatile void*a,uint32_t v){*(volatile uint32_t*)a=v;}
void hal_mmio_write64(volatile void*a,uint64_t v){*(volatile uint64_t*)a=v;}
