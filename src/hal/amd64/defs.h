/* amd64 HAL private constants. */
#ifndef ZEDBSD_HAL_AMD64_DEFS_H
#define ZEDBSD_HAL_AMD64_DEFS_H

#define CLOCK_HZ               100
#define PAGE_SIZE              4096
#define AMD64_DIRECT_BASE      0xffffffff80000000
#define AMD64_DIRECT_LIMIT     0x40000000

#define SEG_KERNEL_CODE        0x08
#define SEG_KERNEL_DATA        0x10
#define SEG_USER_CODE          0x18
#define SEG_USER_DATA          0x20
#define SEG_TSS                0x28

#define INT_PAGEFAULT          0x0e
#define INT_SYSCALL            0xc2
#define INT_IRQ_BASE           0xe0
#define INT_UNDEF              0xffffffff

#define AMD64_PTE_PRESENT      0x001
#define AMD64_PTE_WRITE        0x002
#define AMD64_PTE_USER         0x004
#define AMD64_PTE_WRITETHRU    0x008
#define AMD64_PTE_NOCACHE      0x010
#define AMD64_PTE_ACCESSED     0x020
#define AMD64_PTE_DIRTY        0x040
#define AMD64_PTE_LARGE        0x080
#define AMD64_PTE_GLOBAL       0x100
#define AMD64_PTE_NX           0x8000000000000000
#define AMD64_PTE_ADDR_MASK    0x000ffffffffff000

#define AMD64_MSR_FS_BASE      0xc0000100U
#define AMD64_MSR_GS_BASE      0xc0000101U

#define AMD64_SMP_MAX_CPUS     64U
#define AMD64_AP_TRAMPOLINE    0x00008000U
#define AMD64_AP_STACK_SIZE    16384U

#define AMD64_VECTOR_NOTIFY    0xf0U
#define AMD64_VECTOR_TLB       0xf1U
#define AMD64_VECTOR_ERROR     0xfeU
#define AMD64_VECTOR_SPURIOUS  0xffU

#endif
