/* SPARC V9 memory and PCI-I/O accessors. */
#include <hal/hal.h>
#include "asi.h"
#include "tte.h"

#define SPARCV9_PCI_IO_VA 0xfffff90000000000ULL

static volatile uint8 *pci_io;
void sparcv9_io_init(uint64 base){uint64 tag=SPARCV9_PCI_IO_VA,tte=sparcv9_tte(base,SPARCV9_TTE_SIZE_4M|SPARCV9_TTE_IE|SPARCV9_TTE_LOCKED|SPARCV9_TTE_SIDE_EFFECT|SPARCV9_TTE_PRIVILEGED|SPARCV9_TTE_WRITE|SPARCV9_TTE_GLOBAL);uintptr_t slot=63U*8U;__asm__ volatile("stxa %0, [%1] 0x58\n\tstxa %2, [%3] 0x5d\n\tmembar #Sync"::"r"(tag),"r"(SPARCV9_MMU_TAG_ACCESS),"r"(tte),"r"(slot):"memory");pci_io=(volatile uint8*)(uintptr_t)SPARCV9_PCI_IO_VA;}
uint8 hal_io_inp8(uint16 p){return pci_io[p];}
uint16 hal_io_inp16(uint16 p){return *(volatile uint16*)(pci_io+p);}
uint32 hal_io_inp32(uint16 p){return *(volatile uint32*)(pci_io+p);}
void hal_io_outp8(uint16 p,uint8 v){pci_io[p]=v;}
void hal_io_outp16(uint16 p,uint16 v){*(volatile uint16*)(pci_io+p)=v;}
void hal_io_outp32(uint16 p,uint32 v){*(volatile uint32*)(pci_io+p)=v;}
uint8 hal_mmio_read8(const volatile void*a){return *(const volatile uint8*)a;}
uint16 hal_mmio_read16(const volatile void*a){return *(const volatile uint16*)a;}
uint32 hal_mmio_read32(const volatile void*a){return *(const volatile uint32*)a;}
uint64 hal_mmio_read64(const volatile void*a){return *(const volatile uint64*)a;}
void hal_mmio_write8(volatile void*a,uint8 v){*(volatile uint8*)a=v;}
void hal_mmio_write16(volatile void*a,uint16 v){*(volatile uint16*)a=v;}
void hal_mmio_write32(volatile void*a,uint32 v){*(volatile uint32*)a=v;}
void hal_mmio_write64(volatile void*a,uint64 v){*(volatile uint64*)a=v;}
