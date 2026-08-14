#include <hal/hal.h>
#include "asm.h"

void arm64_irq_mask(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }
void arm64_irq_unmask(void) { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
void arm64_wfe(void) { __asm__ volatile("wfe"); }
void arm64_wfi(void) { __asm__ volatile("wfi"); }
uint64 arm64_current_el(void)
{
	uint64 value;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(value));
	return value >> 2;
}
void arm64_dsb_sy(void) { __asm__ volatile("dsb sy" ::: "memory"); }
void arm64_isb(void) { __asm__ volatile("isb" ::: "memory"); }
uint64 arm64_irq_save(void)
{
	uint64 state;
	__asm__ volatile("mrs %0, daif\n\tmsr daifset, #2" : "=r"(state) :: "memory");
	return state;
}
void arm64_irq_restore(uint64 state)
{
	if ((state & (1U << 7)) == 0) arm64_irq_unmask();
}
void arm64_write_ttbr0(uintptr_t physical)
{ __asm__ volatile("msr ttbr0_el1, %0\n\tisb" :: "r"(physical) : "memory"); }
void arm64_write_ttbr1(uintptr_t physical)
{ __asm__ volatile("msr ttbr1_el1, %0\n\tisb" :: "r"(physical) : "memory"); }
void arm64_flush_tlb(void)
{ __asm__ volatile("dsb ishst\n\ttlbi vmalle1is\n\tdsb ish\n\tisb" ::: "memory"); }

void hal_mb(void){__asm__ volatile("dmb ish":::"memory");}
void hal_rmb(void){__asm__ volatile("dmb ishld":::"memory");}
void hal_wmb(void){__asm__ volatile("dmb ishst":::"memory");}
void hal_io_mb(void){__asm__ volatile("dmb osh":::"memory");}
void hal_io_rmb(void){__asm__ volatile("dmb oshld":::"memory");}
void hal_io_wmb(void){__asm__ volatile("dmb oshst":::"memory");}
uint8 hal_mmio_read8(const volatile void*p){uint8 v=*(const volatile uint8*)p;hal_io_rmb();return v;}
uint16 hal_mmio_read16(const volatile void*p){uint16 v=*(const volatile uint16*)p;hal_io_rmb();return v;}
uint32 hal_mmio_read32(const volatile void*p){uint32 v=*(const volatile uint32*)p;hal_io_rmb();return v;}
uint64 hal_mmio_read64(const volatile void*p){uint64 v=*(const volatile uint64*)p;hal_io_rmb();return v;}
void hal_mmio_write8(volatile void*p,uint8 v){*(volatile uint8*)p=v;hal_io_wmb();}
void hal_mmio_write16(volatile void*p,uint16 v){*(volatile uint16*)p=v;hal_io_wmb();}
void hal_mmio_write32(volatile void*p,uint32 v){*(volatile uint32*)p=v;hal_io_wmb();}
void hal_mmio_write64(volatile void*p,uint64 v){*(volatile uint64*)p=v;hal_io_wmb();}
void hal_halt(void){arm64_wfi();}

static void port_io_fatal(void){HAL_FATAL("port I/O is unavailable on Raspberry Pi 4");}
uint8 hal_io_inp8(uint16 p){(void)p;port_io_fatal();return 0;}
uint16 hal_io_inp16(uint16 p){(void)p;port_io_fatal();return 0;}
uint32 hal_io_inp32(uint16 p){(void)p;port_io_fatal();return 0;}
void hal_io_outp8(uint16 p,uint8 v){(void)p;(void)v;port_io_fatal();}
void hal_io_outp16(uint16 p,uint16 v){(void)p;(void)v;port_io_fatal();}
void hal_io_outp32(uint16 p,uint32 v){(void)p;(void)v;port_io_fatal();}

static size_t cache_line_size(void)
{uint64 ctr;__asm__ volatile("mrs %0,ctr_el0":"=r"(ctr));return(size_t)4U<<(ctr>>16&15U);}
static uintptr_t cache_end(uintptr_t a,size_t n)
{return n>UINTPTR_MAX-a?UINTPTR_MAX:a+n;}
void hal_icache_invalidate_range(uintptr_t a,size_t n)
{
	size_t line=cache_line_size();uintptr_t end=cache_end(a,n);a&=~(uintptr_t)(line-1U);
	for(;a<end;a+=line)__asm__ volatile("ic ivau,%0"::"r"(a):"memory");
	__asm__ volatile("dsb ish\n\tisb":::"memory");
}
void hal_dcache_clean_range(uintptr_t a,size_t n)
{
	size_t line=cache_line_size();uintptr_t end=cache_end(a,n);a&=~(uintptr_t)(line-1U);
	for(;a<end;a+=line)__asm__ volatile("dc cvac,%0"::"r"(a):"memory");
	__asm__ volatile("dsb ish":::"memory");
}
void hal_dcache_invalidate_range(uintptr_t a,size_t n)
{
	/* CIVAC preserves dirty bytes sharing an edge cache line. */
	size_t line=cache_line_size();uintptr_t end=cache_end(a,n);a&=~(uintptr_t)(line-1U);
	for(;a<end;a+=line)__asm__ volatile("dc civac,%0"::"r"(a):"memory");
	__asm__ volatile("dsb ish":::"memory");
}
void hal_dcache_clean_invalidate_range(uintptr_t a,size_t n){hal_dcache_invalidate_range(a,n);}
void hal_sync_instruction_stream(void*p,size_t n)
{
	uintptr_t a=(uintptr_t)p,end=cache_end(a,n);size_t line=cache_line_size();a&=~(uintptr_t)(line-1U);
	for(uintptr_t q=a;q<end;q+=line)__asm__ volatile("dc cvau,%0"::"r"(q):"memory");
	__asm__ volatile("dsb ish":::"memory");
	for(;a<end;a+=line)__asm__ volatile("ic ivau,%0"::"r"(a):"memory");
	__asm__ volatile("dsb ish\n\tisb":::"memory");
}
void hal_reset(void)
{
	volatile uint8 *pm=(volatile uint8 *)(0xffff000000000000ULL+0xfe100000ULL);
	arm64_irq_mask();hal_mmio_write32(pm+0x24,0x5a00000aU);
	hal_mmio_write32(pm+0x1c,0x5a000020U);for(;;)arm64_wfi();
}
void hal_poweroff(void){arm64_irq_mask();for(;;)arm64_wfi();}
void hal_panic(void){arm64_irq_mask();for(;;)arm64_wfi();}
