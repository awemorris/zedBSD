#ifndef SYS_HAL_I386_I386_H
#define SYS_HAL_I386_I386_H

/*
 * Local Clock Interval
 */
#define CLOCK_HZ	(100)

/*
 * Maximum Supported Memory Size in MB.  128MB matches the initial direct map and is generous for every
 * PC-98 (and retro PC/AT) this kernel targets,; the page bitmap stays 4KB.
 */
#define PHYSICAL_MEGS	(128)

/*
 * Page Size
 */
#define PAGE_SIZE	(4096)

/*
 * Fixed System Address Base
 */
#define SYS_START	(0x80000000)

/*
 * Variable System Address Base
 */
#define SYS_V_START	(0xc0000000)

/*
 * Initial Fixed Area (Physical Address)
 */
#define ADDR_IDT		(0x00001000)	/* IDT */
#define ADDR_BOOT_INFO		(0x00002000)	/* Multiboot Info */
#define ADDR_INIT_STACK		(0x00003000)	/* Startup Stack */
#define ADDR_TEMP_PPAGE_MAP	(0x00004000)	/* pmem.c: work area */
#define ADDR_INIT_PDT		(0x00005000)	/* Initial PDT */
#define ADDR_FREE_TOP		(0x00007000)	/* includes AP trampoline */
#define ADDR_INIT_PT		(0x00020000)	/* Initial PT */
#define SIZE_INIT_STACK		(0x1000)	/* Startup Stack Size */

/*
 * Segment Selectors
 */
#define SEG_INVALID		(0x0000)	/* Invalid */
#define SEG_SYS_CODE		(0x0008)	/* Sys Code */
#define SEG_SYS_DATA		(0x0010)	/* Sys Data */
#define SEG_USER_CODE		(0x0018)	/* User Code */
#define SEG_USER_DATA		(0x0020)	/* User Data */
#define SEG_TSS			(0x0028)	/* TSS (sole tss for now) */
#define SEG_MAX			SEG_TSS

/*
 * RPL (request privilege level, low 2 bits of selector)
 */
#define SEG_RPL_0		(0)
#define SEG_RPL_1		(1)
#define SEG_RPL_2		(2)
#define SEG_RPL_3		(3)

/*
 * Interrupts
 */
#define INT_DIVBYZERO		(0x00)
#define INT_GPE			(0x0d)
#define INT_PAGEFAULT		(0x0e)
#define INT_SYSCALL		(0xc2)
#define INT_CPU_NOTIFY		(0xd0)
#define INT_CPU_PANIC		(0xd1)
#define INT_CPU_TLB		(0xd2)
#define INT_IRQ_BASE		(0xe0)
#define INT_UNDEF		(0xffffffff)

/*
 * EFLAGS
 */
#define EFLAGS_CF		(0x000001)	/* Carry*/
#define EFLAGS_RSV1		(0x000002)	/* Reserved (always 1) */
#define EFLAGS_PF		(0x000004)	/* Parity */
#define EFLAGS_AF		(0x000010)	/* Auxiliary (BCD carry/borrow) */
#define EFLAGS_ZF		(0x000040)	/* Zero */
#define EFLAGS_SF		(0x000080)	/* Sign */
#define EFLAGS_TF		(0x000100)	/* Trap (Single-Step Debug) */
#define EFLAGS_IF		(0x000200)	/* Interrupt Enable */
#define EFLAGS_DF		(0x000400)	/* Direction (String) */
#define EFLAGS_OF		(0x000800)	/* 0verflow */
#define EFLAGS_IOPL_0		(0x000000)	/* IOPL-0 */
#define EFLAGS_IOPL_1		(0x001000)	/* IOPL-1 */
#define EFLAGS_IOPL_2		(0x002000)	/* IOPL-2 */
#define EFLAGS_IOPL_3		(0x003000)	/* IOPL-3 */
#define EFLAGS_NT		(0x004000)	/* Nested Task */
#define EFLAGS_RF		(0x010000)	/* Resume (Debug Resume) */
#define EFLAGS_VM		(0x020000)	/* VM86 */
#define EFLAGS_AC		(0x040000)	/* Alignment Check */
#define EFLAGS_VIF		(0x080000)	/* Virtual Interrupt Flag (VME)*/
#define EFLAGS_VIP		(0x100000)	/* Virtual Interrupt Pending (VME) */
#define EFLAGS_ID		(0x200000)	/* Identification (CPUID Support) */

/*
 * PTE Flags
 */
#define PTE_PRESENT		(0x0001)	/* Present */
#define PTE_WRITE		(0x0002)	/* User Write */
#define PTE_USER		(0x0004)	/* User/Supervisor */
#define PTE_WRITEBACK		(0x0008)	/* Enable Cache-Writeback */
#define PTE_NOCACHE		(0x0010)	/* Disable Cache */
#define PTE_ACCESS		(0x0020)	/* Accessed */
#define PTE_DIRTY		(0x0040)	/* Written */
#define PTE_BIG			(0x0080)	/* 4MB Page Size (PDE) */
#define PTE_GLOBAL		(0x0100)	/* No TLB flush */

/*
 * extern "C"
 *  - Some compiler adds "_" to symbols.
 */
#define EXT_C(def) def

#endif
