/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 68040 Macintosh second stage.
 *
 * It runs on the ROM, after the boot block: it takes the screen the ROM
 * set up (and loads the RGB332 colours into an 8-bit one), asks Gestalt
 * what the machine is, finds the physical RAM by walking the memory the
 * ROM maps, reads the configuration and the kernel from the boot
 * partition through the File Manager (or, failing that, the kernel from
 * the sectors the image's tool recorded), builds the boot information,
 * plans the kernel's placement and leaves the ROM through the
 * trampoline, which places the kernel with translation and the caches
 * off and enters it.
 */

#include "bootloader/mac68k/boot-layout.h"
#include "bootloader/mac68k/stage2.h"
#include "bootloader/mac68k/screen.h"

/*
 * Low-memory globals.
 *
 * Values referenced: Apple, Inside Macintosh: Operating System Utilities
 * (1994), Inside Macintosh: Memory (1992) and Inside Macintosh: Imaging
 * With QuickDraw (1994) (MemTop, CPUFlag, whose processor numbers are
 * Gestalt's 'cput' answers, Ticks, VIA, SCCRd, Time, ROMBase and
 * MainDevice).
 */
#define LOW_MEM_TOP		0x0108U
#define LOW_CPU_FLAG		0x012fU
#define LOW_TICKS		0x016aU
#define LOW_VIA			0x01d4U
#define LOW_SCC_READ		0x01d8U
#define LOW_TIME		0x020cU
#define LOW_ROM_BASE		0x02aeU
#define LOW_MAIN_DEVICE		0x08a4U

/*
 * The graphics device and pixel map fields, and the video driver's
 * colour table call.
 *
 * Values referenced: Apple, Inside Macintosh: Imaging With QuickDraw
 * (1994) (the GDevice record's gdRefNum and gdPMap, the PixMap record's
 * baseAddr, rowBytes with its flag bits, bounds and pixelSize, the
 * ColorSpec record); Designing Cards and Drivers for the Macintosh
 * Family, third edition (1992) (the video driver's cscSetEntries control
 * call and its csTable, csStart and csCount parameters).
 */
#define GDEVICE_REF_NUM		0U
#define GDEVICE_PIXMAP		22U
#define PIXMAP_BASE		0U
#define PIXMAP_ROW_BYTES	4U
#define PIXMAP_BOUNDS		6U
#define PIXMAP_PIXEL_SIZE	32U
#define ROW_BYTES_MASK		0x3fffU
#define CSC_SET_ENTRIES		3U
#define COLOUR_SPEC_SIZE	8U
#define PALETTE_COLOURS		256U

/*
 * The parameter block fields of the File Manager and the Device Manager.
 *
 * Values referenced: Apple, Inside Macintosh: Files (1992) and
 * Inside Macintosh: Devices (1994) (ioNamePtr, ioVRefNum, ioRefNum,
 * ioPermssn, ioMisc, ioBuffer, ioReqCount, ioActCount, ioPosMode,
 * ioPosOffset, ioDirID and the control call's csCode and csParam; the
 * root folder's ID, fsRdPerm and fsFromStart; the results nsvErr, eofErr
 * and memFullErr).
 */
#define PB_SIZE			128U
#define PB_NAME			18U
#define PB_VREFNUM		22U
#define PB_REFNUM		24U
#define PB_CS_CODE		26U
#define PB_PERMISSION		27U
#define PB_MISC			28U
#define PB_CS_PARAM		28U
#define PB_BUFFER		32U
#define PB_REQUEST		36U
#define PB_ACTUAL		40U
#define PB_POSITION_MODE	44U
#define PB_POSITION		46U
#define PB_DIR_ID		48U
#define ROOT_FOLDER_ID		2U
#define PERMISSION_READ		1U
#define FROM_START		1U
#define NO_SUCH_VOLUME		(-35)
#define END_OF_FILE		(-39)
#define MEMORY_FULL		(-108)

/*
 * Gestalt's selectors and answers.
 *
 * Values referenced: Apple, Inside Macintosh: Operating System Utilities
 * (1994), chapter 1 (the selectors 'mach', 'cput', 'fpu ', 'mmu ' and
 * 'addr'; gestaltCPU68040, gestalt68040FPU, gestalt68040MMU and the
 * 32-bit addressing bit).
 */
#define GESTALT_MACHINE		0x6d616368U
#define GESTALT_CPU		0x63707574U
#define GESTALT_FPU		0x66707520U
#define GESTALT_MMU		0x6d6d7520U
#define GESTALT_ADDRESSING	0x61646472U
#define GESTALT_CPU_68040	4U
#define GESTALT_FPU_68040	3U
#define GESTALT_32BIT		0x00000001U

/*
 * The translation control's and the MMU status's bits.
 *
 * Values referenced: Motorola, MC68040 User's Manual (MC68040UM/AD,
 * 1993) (TC: E, P; MMUSR: B, T, R).
 */
#define TC_ENABLE		0x8000U
#define TC_PAGE_8K		0x4000U
#define MMUSR_BUS_ERROR		0x0800U
#define MMUSR_TRANSPARENT	0x0002U
#define MMUSR_RESIDENT		0x0001U

/* The VIA's interrupt enable register, which clears the bits written with bit 7 off. */
#define VIA_IER_OFFSET		0x1c00U
#define VIA_IER_ALL_OFF		0x7fU

/*
 * The boot information's tags and values.
 *
 * Values referenced: Linux master arch/m68k/include/uapi/asm/bootinfo.h
 * and bootinfo-mac.h (the tags, MACH_MAC, CPU_68040, FPU_68040 and
 * MMU_68040).
 */
#define BI_MACHTYPE		0x0001U
#define BI_CPUTYPE		0x0002U
#define BI_FPUTYPE		0x0003U
#define BI_MMUTYPE		0x0004U
#define BI_MEMCHUNK		0x0005U
#define BI_COMMAND_LINE		0x0007U
#define BI_MAC_MODEL		0x8000U
#define BI_MAC_VADDR		0x8001U
#define BI_MAC_VDEPTH		0x8002U
#define BI_MAC_VROW		0x8003U
#define BI_MAC_VDIM		0x8004U
#define BI_MAC_VLOGICAL		0x8005U
#define BI_MAC_SCCBASE		0x8006U
#define BI_MAC_BTIME		0x8007U
#define BI_MAC_MEMSIZE		0x8009U
#define BI_MAC_ROMBASE		0x800bU
#define MACH_MAC		3U
#define CPU_68040		4U
#define FPU_68040		4U
#define MMU_68040		4U

/* The room the boot information and the configuration file get. */
#define BOOTINFO_CAPACITY	4096U
#define CONFIG_LIMIT		1024U

/* How long the loader's lines stay on the screen before the kernel clears it: three seconds of ticks. */
#define PAUSE_TICKS		180U
#define PAUSE_SPIN_LIMIT	50000000U

/* How many blocks the loader takes looking for the handoff area. */
#define AREA_TRIES		32U

/* The configuration file's name. */
#define CONFIG_NAME		"zedbsd.cfg"

/*
 * Where the boot partition is and what the image's tool recorded in the
 * boot block; filled once at the entry, before the boot block's memory
 * can be reused.
 */
struct boot_place {
	int16_t drive;
	int16_t driver;
	uint32_t partition_offset;
	uint32_t kernel_offset;
	uint32_t kernel_bytes;
	uint32_t kernel_crc;
};

/*
 * The screen the ROM set up, as the boot information describes it.
 */
struct screen_info {
	uint32_t logical;
	uint32_t physical;
	uint32_t row_bytes;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	int16_t driver;
};

/*
 * What Gestalt said about the machine.
 */
struct machine_info {
	uint32_t model;
	uint32_t cpu;
	uint32_t fpu;
	uint32_t mmu;
	uint32_t addressing;
};

/* The boot partition, filled at the entry. */
static struct boot_place boot;

/* The translation control at the entry; the ROM does not change it while the loader runs. */
static uint32_t translation_control;

/* The machine's RAM, filled by the walk. */
static struct mac68k_memory memory;

/* The configuration, from the file or the defaults. */
static struct mac68k_config config;

extern int16_t mac68k_trap_close(void *block);
extern int16_t mac68k_trap_read(void *block);
extern int16_t mac68k_trap_control(void *block);
extern int16_t mac68k_trap_mount_volume(void *block);
extern int16_t mac68k_trap_get_eof(void *block);
extern int16_t mac68k_trap_hfs_open(void *block);
extern void *mac68k_trap_new_pointer(uint32_t size, int system);
extern void mac68k_trap_max_application_zone(void);
extern int32_t mac68k_trap_gestalt(uint32_t selector, uint32_t *response);
extern uint32_t mac68k_cpu_translation_control(void);
extern uint32_t mac68k_cpu_test_address(uint32_t logical);
extern void mac68k_cpu_push_caches(void);
extern void mac68k_cpu_mask_interrupts(void);
extern void mac68k_handoff_trampoline(const struct mac68k_plan *plan);
extern const uint8_t mac68k_handoff_trampoline_end[];

void mac68k_stage2_main(uint32_t drive, int32_t driver, uint32_t partition_offset, const uint8_t *boot_block);

static void take_screen(struct screen_info *screen);
static int load_rgb332(int16_t driver);
static void ask_machine(struct machine_info *machine);
static void walk_memory(void);
static void read_config(void);
static uint8_t *read_kernel(uint32_t *length);
static uint8_t *read_kernel_blocks(void);
static int names_default_kernel(const char *name);
static int16_t open_file(const char *name, int16_t *file);
static int16_t read_file(int16_t file, uint8_t **data, uint32_t *length, uint32_t limit);
static void close_file(int16_t file);
static void *take_memory(uint32_t size);
static uint32_t build_bootinfo(uint8_t *buffer, const struct machine_info *machine, const struct screen_info *screen);
static uint8_t *take_handoff_area(uint32_t size, const struct mac68k_load_range *ranges, unsigned count);
static int is_identity(uint32_t logical, uint32_t size);
static int translate(void *cookie, uint32_t logical, uint32_t *physical);
static void pause_for_reading(void);
static void stop(const char *problem) __attribute__((noreturn));
static void put_line(const char *text);
static void put_value(const char *label, uint32_t value);
static uint32_t read_long_at(uint32_t address);

/*
 * Runs the second stage: called from the entry with what the boot block
 * passed.  It enters the kernel, or stops with the reason on the screen.
 */
void
mac68k_stage2_main(
	uint32_t drive,
	int32_t driver,
	uint32_t partition_offset,
	const uint8_t *boot_block)
{
	struct mac68k_kernel_image image;
	struct mac68k_load_range ranges[MAC68K_RANGES_MAX];
	struct machine_info machine;
	struct screen_info screen;
	struct mac68k_plan *plan;
	struct mac68k_piece *pieces;
	const char *problem;
	uint8_t *kernel;
	uint8_t *bootinfo;
	uint8_t *area;
	uint32_t kernel_length;
	uint32_t bootinfo_size;
	uint32_t trampoline_size;
	uint32_t pieces_max;
	uint32_t area_size;
	uint32_t via;
	unsigned range_count;
	void (*enter)(const struct mac68k_plan *plan);

	/* Keeps where the partition is and what the boot block recorded. */
	boot.drive = (int16_t)drive;
	boot.driver = (int16_t)driver;
	boot.partition_offset = partition_offset;
	boot.kernel_offset = mac68k_read_be32(boot_block + MAC68K_BOOT_KERNEL_OFFSET_OFFSET);
	boot.kernel_bytes = mac68k_read_be32(boot_block + MAC68K_BOOT_KERNEL_BYTES_OFFSET);
	boot.kernel_crc = mac68k_read_be32(boot_block + MAC68K_BOOT_KERNEL_CRC_OFFSET);
	translation_control = mac68k_cpu_translation_control();

	/* Takes the screen and greets. */
	take_screen(&screen);
	put_line("zedBSD loader for the 68040 Macintosh");

	/* Describes the machine and refuses another processor. */
	ask_machine(&machine);
	if (machine.cpu != GESTALT_CPU_68040)
		stop("this loader needs a 68040 processor");

	/* Finds the RAM. */
	walk_memory();

	/* Reads the configuration and the kernel, and checks the kernel. */
	read_config();
	kernel = read_kernel(&kernel_length);
	problem = mac68k_kernel_parse(kernel, kernel_length, &image);
	if (problem != NULL)
		stop(problem);
	put_value("kernel entry ", image.entry);
	put_value("kernel end   ", image.end);

	/* Builds the boot information. */
	bootinfo = take_memory(BOOTINFO_CAPACITY);
	if (bootinfo == NULL)
		stop("no memory for the boot information");
	bootinfo_size = build_bootinfo(bootinfo, &machine, &screen);

	/* Lists what the handoff writes and finds the area for the trampoline and the plan. */
	range_count = mac68k_handoff_ranges(ranges, &image, (uint32_t)(uintptr_t)kernel, (uint32_t)(uintptr_t)bootinfo, bootinfo_size);
	trampoline_size = (uint32_t)(mac68k_handoff_trampoline_end - (const uint8_t *)mac68k_handoff_trampoline);
	trampoline_size = (trampoline_size + 15U) & ~15U;
	pieces_max = mac68k_handoff_pieces_max(ranges, range_count);
	area_size = trampoline_size + sizeof(struct mac68k_plan) + pieces_max * sizeof(struct mac68k_piece);
	area = take_handoff_area(area_size, ranges, range_count);

	/* Plans the handoff in the area, after the trampoline. */
	plan = (struct mac68k_plan *)(area + trampoline_size);
	pieces = (struct mac68k_piece *)(plan + 1);
	plan->entry = image.entry;
	plan->null_fpu_frame = 0;
	plan->flags = 0;

	/* The trampoline resets the floating-point unit only when the processor has one. */
	if (machine.fpu == GESTALT_FPU_68040)
		plan->flags |= MAC68K_PLAN_FPU;

	/* The steps follow the plan's header in the area. */
	plan->pieces = (uint32_t)(uintptr_t)pieces;
	problem = mac68k_handoff_plan(plan, pieces, pieces_max, ranges, range_count, translate, NULL, &memory, (uint32_t)(uintptr_t)area, area_size);
	if (problem != NULL)
		stop(problem);
	put_value("handoff steps ", plan->count);
	put_line("entering the kernel");
	pause_for_reading();

	/* Copies the trampoline into the area and makes the instruction cache see it. */
	mac68k_copy_bytes(area, (const void *)mac68k_handoff_trampoline, trampoline_size);
	mac68k_cpu_push_caches();

	/* Leaves the ROM: masks the interrupts and turns the VIA's off, which the ROM's handlers served. */
	mac68k_cpu_mask_interrupts();
	via = read_long_at(LOW_VIA);
	if (via != 0U)
		*(volatile uint8_t *)(uintptr_t)(via + VIA_IER_OFFSET) = VIA_IER_ALL_OFF;

	/* Enters the trampoline in the area; it does not return. */
	enter = (void (*)(const struct mac68k_plan *))(uintptr_t)area;
	enter(plan);
	stop("the handoff returned");
}

/*
 * Takes the main screen the ROM set up: its frame buffer, its shape and
 * its video driver.  An 8-bit screen gets the RGB332 colours, which the
 * kernel draws with; another depth stops the loader, after it says so.
 */
static void
take_screen(
	struct screen_info *screen)
{
	const uint8_t *device;
	const uint8_t *pixmap;
	uint32_t handle;
	uint32_t pixmap_handle;
	int16_t top;
	int16_t left;
	int16_t bottom;
	int16_t right;
	int loaded;
	int error;

	/* No screen until one is found. */
	mac68k_fill_bytes(screen, 0, sizeof(*screen));

	/* The main graphics device is a handle: a pointer to its record's pointer. */
	handle = read_long_at(LOW_MAIN_DEVICE);
	if (handle == 0U)
		return;
	device = (const uint8_t *)(uintptr_t)read_long_at(handle);
	if (device == NULL)
		return;

	/* Its pixel map, also a handle. */
	pixmap_handle = mac68k_read_be32(device + GDEVICE_PIXMAP);
	if (pixmap_handle == 0U)
		return;
	pixmap = (const uint8_t *)(uintptr_t)read_long_at(pixmap_handle);
	if (pixmap == NULL)
		return;

	/* The frame buffer's address, row length, size and depth. */
	screen->driver = (int16_t)mac68k_read_be16(device + GDEVICE_REF_NUM);
	screen->logical = mac68k_read_be32(pixmap + PIXMAP_BASE);
	screen->row_bytes = mac68k_read_be16(pixmap + PIXMAP_ROW_BYTES) & ROW_BYTES_MASK;
	top = (int16_t)mac68k_read_be16(pixmap + PIXMAP_BOUNDS);
	left = (int16_t)mac68k_read_be16(pixmap + PIXMAP_BOUNDS + 2U);
	bottom = (int16_t)mac68k_read_be16(pixmap + PIXMAP_BOUNDS + 4U);
	right = (int16_t)mac68k_read_be16(pixmap + PIXMAP_BOUNDS + 6U);
	screen->width = (uint32_t)(right - left);
	screen->height = (uint32_t)(bottom - top);
	screen->depth = mac68k_read_be16(pixmap + PIXMAP_PIXEL_SIZE);

	/* The kernel needs the frame buffer's physical address. */
	error = translate(NULL, screen->logical, &screen->physical);
	if (error != 0)
		screen->physical = screen->logical;

	/* An 8-bit screen gets the RGB332 colours and is cleared. */
	loaded = 0;
	if (screen->depth == 8U)
		loaded = load_rgb332(screen->driver);
	mac68k_screen_init((uint8_t *)(uintptr_t)screen->logical, screen->row_bytes, screen->width, screen->height, screen->depth, loaded);
	if (loaded)
		mac68k_screen_clear();

	/* Refuses another depth: the kernel's console draws 8 bits on this machine. */
	if (screen->depth != 8U)
		stop("set the monitor to 256 colours in the Monitors control panel");
	if (!loaded)
		stop("the video driver did not take the colour table");
}

/*
 * Loads the RGB332 colours into the video driver's colour table, entry i
 * holding red i >> 5, green (i >> 2) & 7 and blue i & 3 of their ranges.
 * Reports whether the driver took them.
 */
static int
load_rgb332(
	int16_t driver)
{
	uint8_t block[PB_SIZE];
	uint8_t *table;
	uint8_t *entry;
	uint32_t index;
	int16_t error;

	/* The colour table, 256 colour specifications. */
	table = take_memory(PALETTE_COLOURS * COLOUR_SPEC_SIZE);
	if (table == NULL)
		return 0;

	/* Each entry: its index and the 16-bit red, green and blue. */
	for (index = 0; index < PALETTE_COLOURS; index++) {
		entry = table + index * COLOUR_SPEC_SIZE;
		mac68k_write_be16(entry, (uint16_t)index);
		mac68k_write_be16(entry + 2, (uint16_t)(((index >> 5) & 7U) * 0xffffU / 7U));
		mac68k_write_be16(entry + 4, (uint16_t)(((index >> 2) & 7U) * 0xffffU / 7U));
		mac68k_write_be16(entry + 6, (uint16_t)((index & 3U) * 0xffffU / 3U));
	}

	/* The driver's control call: the table, the first entry and the count less one. */
	mac68k_fill_bytes(block, 0, sizeof(block));
	mac68k_write_be16(block + PB_REFNUM, (uint16_t)driver);
	mac68k_write_be16(block + PB_CS_CODE, CSC_SET_ENTRIES);
	mac68k_write_be32(block + PB_CS_PARAM, (uint32_t)(uintptr_t)table);
	mac68k_write_be16(block + PB_CS_PARAM + 4U, 0);
	mac68k_write_be16(block + PB_CS_PARAM + 6U, (uint16_t)(PALETTE_COLOURS - 1U));
	error = mac68k_trap_control(block);
	if (error != 0)
		return 0;

	/* Succeeded: the screen shows the RGB332 colours. */
	return 1;
}

/* Asks Gestalt what the machine is, and shows it. */
static void
ask_machine(
	struct machine_info *machine)
{
	int32_t error;

	/* Unknown answers stay zero. */
	mac68k_fill_bytes(machine, 0, sizeof(*machine));

	/* The model, the processor, the floating-point unit, the MMU and the addressing. */
	error = mac68k_trap_gestalt(GESTALT_MACHINE, &machine->model);
	if (error != 0)
		machine->model = 0;
	error = mac68k_trap_gestalt(GESTALT_CPU, &machine->cpu);
	if (error != 0)
		machine->cpu = *(const volatile uint8_t *)(uintptr_t)LOW_CPU_FLAG;
	error = mac68k_trap_gestalt(GESTALT_FPU, &machine->fpu);
	if (error != 0)
		machine->fpu = 0;
	error = mac68k_trap_gestalt(GESTALT_MMU, &machine->mmu);
	if (error != 0)
		machine->mmu = 0;
	error = mac68k_trap_gestalt(GESTALT_ADDRESSING, &machine->addressing);
	if (error != 0)
		machine->addressing = 0;

	/* Shows the answers. */
	put_value("machine ", machine->model);
	put_value("cpu ", machine->cpu);
	put_value("fpu ", machine->fpu);
	put_value("mmu ", machine->mmu);
	put_value("addressing ", machine->addressing);
	put_value("translation control ", translation_control);

	/* Warns when the ROM still addresses memory with 24 bits. */
	if ((machine->addressing & GESTALT_32BIT) == 0U)
		put_line("warning: 32-bit addressing is off");
}

/*
 * Walks the memory the ROM maps from logical 0 up to MemTop, page by
 * page, and records where each page is physically.
 *
 * Procedure referenced: EMILE (github.com/vivier/EMILE, master)
 * second/bank.c (the physical banks found by translating the logical
 * pages below MemTop).
 */
static void
walk_memory(
	void)
{
	uint32_t top;
	uint32_t logical;
	uint32_t physical;
	unsigned index;
	int error;

	/* Translates every page below the top of memory. */
	mac68k_memory_init(&memory);
	top = read_long_at(LOW_MEM_TOP);
	for (logical = 0; logical < top; logical += MAC68K_PAGE_SIZE) {
		error = translate(NULL, logical, &physical);
		if (error != 0)
			continue;
		mac68k_memory_add_page(&memory, physical & ~(MAC68K_PAGE_SIZE - 1U));
	}
	mac68k_memory_finish(&memory);

	/* Shows the banks. */
	put_value("memory top ", top);
	for (index = 0; index < memory.count; index++) {
		mac68k_screen_puts("bank ");
		mac68k_screen_put_hex(memory.banks[index].base);
		mac68k_screen_puts(" ");
		mac68k_screen_put_hex(memory.banks[index].size);
		mac68k_screen_puts("\n");
	}

	/* Refuses a walk that found nothing, and warns of pages it could not record. */
	if (memory.count == 0U)
		stop("no RAM was found");
	if (memory.dropped_pages != 0U)
		put_value("warning: pages left out ", memory.dropped_pages);
	if (memory.count > MAC68K_KERNEL_BANKS_MAX)
		put_line("warning: the kernel takes the first 8 banks only");
}

/* Reads the configuration file, or keeps the defaults when there is none. */
static void
read_config(
	void)
{
	uint8_t *text;
	uint32_t length;
	int16_t file;
	int16_t error;

	/* Starts from the defaults. */
	mac68k_config_parse(&config, "", 0);

	/* Reads the file when the partition has one. */
	error = open_file(CONFIG_NAME, &file);
	if (error != 0) {
		put_line("no zedbsd.cfg; using the defaults");
		return;
	}
	error = read_file(file, &text, &length, CONFIG_LIMIT);
	close_file(file);
	if (error != 0) {
		put_value("zedbsd.cfg could not be read: error ", (uint32_t)(int32_t)error);
		return;
	}

	/* Takes its settings. */
	mac68k_config_parse(&config, (const char *)text, length);
	mac68k_screen_puts("kernel file ");
	mac68k_screen_puts(config.kernel);
	mac68k_screen_puts("\ncommand line ");
	mac68k_screen_puts(config.command_line);
	mac68k_screen_puts("\n");
}

/*
 * Reads the kernel's file through the File Manager; when the file cannot
 * be opened and it is the default kernel, reads the sectors the image's
 * tool recorded instead.  Stops when neither works.
 */
static uint8_t *
read_kernel(
	uint32_t *length)
{
	uint8_t *kernel;
	int16_t file;
	int16_t error;
	int is_default;

	/* Opens and reads the file. */
	error = open_file(config.kernel, &file);
	if (error == 0) {
		error = read_file(file, &kernel, length, 0);
		close_file(file);
		if (error != 0)
			stop("the kernel's file could not be read");
		put_value("kernel bytes ", *length);
		return kernel;
	}
	put_value("the kernel's file did not open: error ", (uint32_t)(int32_t)error);

	/* Only the default kernel has recorded sectors. */
	is_default = names_default_kernel(config.kernel);
	if (!is_default)
		stop("the kernel named in zedbsd.cfg could not be opened");

	/* Succeeded: the kernel as the image's tool placed it. */
	kernel = read_kernel_blocks();
	*length = boot.kernel_bytes;
	return kernel;
}

/*
 * Reads the kernel from the sectors the image's tool recorded, through
 * the disk's driver, and checks its sum.
 */
static uint8_t *
read_kernel_blocks(
	void)
{
	uint8_t block[PB_SIZE];
	uint8_t *kernel;
	uint32_t rounded;
	uint32_t sum;
	int16_t error;

	/* Refuses an image that recorded no kernel. */
	if (boot.kernel_bytes == 0U)
		stop("the boot block records no kernel");

	/* Takes room for whole sectors. */
	rounded = (boot.kernel_bytes + MAC68K_SECTOR_SIZE - 1U) & ~(uint32_t)(MAC68K_SECTOR_SIZE - 1U);
	kernel = take_memory(rounded);
	if (kernel == NULL)
		stop("no memory for the kernel");

	/* Reads them through the driver, from the partition's offset. */
	mac68k_fill_bytes(block, 0, sizeof(block));
	mac68k_write_be16(block + PB_VREFNUM, (uint16_t)boot.drive);
	mac68k_write_be16(block + PB_REFNUM, (uint16_t)boot.driver);
	mac68k_write_be32(block + PB_BUFFER, (uint32_t)(uintptr_t)kernel);
	mac68k_write_be32(block + PB_REQUEST, rounded);
	mac68k_write_be16(block + PB_POSITION_MODE, FROM_START);
	mac68k_write_be32(block + PB_POSITION, boot.partition_offset + boot.kernel_offset);
	error = mac68k_trap_read(block);
	if (error != 0)
		stop("the kernel's sectors could not be read");

	/* Refuses sectors that no longer hold the recorded kernel. */
	sum = mac68k_crc32(kernel, boot.kernel_bytes);
	if (sum != boot.kernel_crc)
		stop("the kernel's sectors do not match the boot block's sum");

	/* Succeeded: the recorded kernel. */
	put_value("kernel bytes (sectors) ", boot.kernel_bytes);
	return kernel;
}

/* Tells whether a name is the default kernel's, whose sectors the boot block records. */
static int
names_default_kernel(
	const char *name)
{
	static const char default_name[] = "vmunix";
	uint32_t index;

	/* Compares up to and including the terminator. */
	for (index = 0; index < sizeof(default_name); index++) {
		if (name[index] != default_name[index])
			return 0;
	}

	/* Succeeded: it is the default kernel. */
	return 1;
}

/*
 * Opens a file of the boot partition's top folder for reading, mounting
 * the volume when the ROM has not.
 */
static int16_t
open_file(
	const char *name,
	int16_t *file)
{
	uint8_t block[PB_SIZE];
	uint8_t pascal_name[MAC68K_NAME_MAX + 1U];
	uint32_t length;
	int16_t error;

	/* The name as a Pascal string. */
	length = mac68k_string_length(name);
	if (length > MAC68K_NAME_MAX)
		return -1;
	pascal_name[0] = (uint8_t)length;
	mac68k_copy_bytes(pascal_name + 1, name, length);

	/* Opens the file in the volume's root folder, by the drive's number. */
	mac68k_fill_bytes(block, 0, sizeof(block));
	mac68k_write_be32(block + PB_NAME, (uint32_t)(uintptr_t)pascal_name);
	mac68k_write_be16(block + PB_VREFNUM, (uint16_t)boot.drive);
	block[PB_PERMISSION] = PERMISSION_READ;
	mac68k_write_be32(block + PB_DIR_ID, ROOT_FOLDER_ID);
	error = mac68k_trap_hfs_open(block);

	/* Mounts the volume and tries again when it is not mounted. */
	if (error == NO_SUCH_VOLUME) {
		mac68k_fill_bytes(block, 0, sizeof(block));
		mac68k_write_be16(block + PB_VREFNUM, (uint16_t)boot.drive);
		error = mac68k_trap_mount_volume(block);
		if (error != 0)
			return error;
		mac68k_fill_bytes(block, 0, sizeof(block));
		mac68k_write_be32(block + PB_NAME, (uint32_t)(uintptr_t)pascal_name);
		mac68k_write_be16(block + PB_VREFNUM, (uint16_t)boot.drive);
		block[PB_PERMISSION] = PERMISSION_READ;
		mac68k_write_be32(block + PB_DIR_ID, ROOT_FOLDER_ID);
		error = mac68k_trap_hfs_open(block);
	}
	if (error != 0)
		return error;

	/* Succeeded: the file's reference number. */
	*file = (int16_t)mac68k_read_be16(block + PB_REFNUM);
	return 0;
}

/*
 * Reads a whole open file into memory taken for it; a nonzero limit
 * cuts a longer file.
 */
static int16_t
read_file(
	int16_t file,
	uint8_t **data,
	uint32_t *length,
	uint32_t limit)
{
	uint8_t block[PB_SIZE];
	uint8_t *buffer;
	uint32_t size;
	uint32_t actual;
	int16_t error;

	/* The file's length. */
	mac68k_fill_bytes(block, 0, sizeof(block));
	mac68k_write_be16(block + PB_REFNUM, (uint16_t)file);
	error = mac68k_trap_get_eof(block);
	if (error != 0)
		return error;
	size = mac68k_read_be32(block + PB_MISC);
	if (limit != 0U && size > limit)
		size = limit;

	/* Memory for it, one byte more for a terminator. */
	buffer = take_memory(size + 1U);
	if (buffer == NULL)
		return MEMORY_FULL;

	/* Reads it from its start. */
	mac68k_fill_bytes(block, 0, sizeof(block));
	mac68k_write_be16(block + PB_REFNUM, (uint16_t)file);
	mac68k_write_be32(block + PB_BUFFER, (uint32_t)(uintptr_t)buffer);
	mac68k_write_be32(block + PB_REQUEST, size);
	mac68k_write_be16(block + PB_POSITION_MODE, FROM_START);
	mac68k_write_be32(block + PB_POSITION, 0);
	error = mac68k_trap_read(block);
	if (error != 0)
		return error;
	actual = mac68k_read_be32(block + PB_ACTUAL);
	if (actual != size)
		return END_OF_FILE;

	/* Succeeded: the file's bytes, terminated. */
	buffer[size] = 0;
	*data = buffer;
	*length = size;
	return 0;
}

/* Closes an open file. */
static void
close_file(
	int16_t file)
{
	uint8_t block[PB_SIZE];

	/* The file's reference number is all the call takes; a failure changes nothing for the loader. */
	mac68k_fill_bytes(block, 0, sizeof(block));
	mac68k_write_be16(block + PB_REFNUM, (uint16_t)file);
	(void)mac68k_trap_close(block);
}

/*
 * Takes memory from the ROM's heaps: the current heap, then the current
 * heap grown to its limit, then the system heap.  Reports NULL when none
 * has room.
 */
static void *
take_memory(
	uint32_t size)
{
	void *block;

	/* The current heap. */
	block = mac68k_trap_new_pointer(size, 0);
	if (block != NULL)
		return block;

	/* The application heap grown to its limit. */
	mac68k_trap_max_application_zone();
	block = mac68k_trap_new_pointer(size, 0);
	if (block != NULL)
		return block;

	/* The system heap, or nothing. */
	block = mac68k_trap_new_pointer(size, 1);
	return block;
}

/*
 * Writes the boot information records: the machine, the RAM banks, the
 * screen, the clock and the command line.  Reports their size.
 */
static uint32_t
build_bootinfo(
	uint8_t *buffer,
	const struct machine_info *machine,
	const struct screen_info *screen)
{
	struct mac68k_bootinfo info;
	unsigned index;

	/* The machine: a Macintosh with a 68040's processor and MMU, and its FPU when it has one. */
	mac68k_bootinfo_begin(&info, buffer, BOOTINFO_CAPACITY);
	mac68k_bootinfo_add_long(&info, BI_MACHTYPE, MACH_MAC);
	mac68k_bootinfo_add_long(&info, BI_CPUTYPE, CPU_68040);
	if (machine->fpu == GESTALT_FPU_68040)
		mac68k_bootinfo_add_long(&info, BI_FPUTYPE, FPU_68040);
	mac68k_bootinfo_add_long(&info, BI_MMUTYPE, MMU_68040);
	mac68k_bootinfo_add_long(&info, BI_MAC_MODEL, machine->model);

	/* The RAM banks, as many as the kernel reads. */
	for (index = 0; index < memory.count && index < MAC68K_KERNEL_BANKS_MAX; index++)
		mac68k_bootinfo_add_pair(&info, BI_MEMCHUNK, memory.banks[index].base, memory.banks[index].size);
	mac68k_bootinfo_add_long(&info, BI_MAC_MEMSIZE, read_long_at(LOW_MEM_TOP) >> 20);

	/* The screen: its physical and logical addresses, depth, row length and size. */
	mac68k_bootinfo_add_long(&info, BI_MAC_VADDR, screen->physical);
	mac68k_bootinfo_add_long(&info, BI_MAC_VLOGICAL, screen->logical);
	mac68k_bootinfo_add_long(&info, BI_MAC_VDEPTH, screen->depth);
	mac68k_bootinfo_add_long(&info, BI_MAC_VROW, screen->row_bytes);
	mac68k_bootinfo_add_long(&info, BI_MAC_VDIM, (screen->height << 16) | (screen->width & 0xffffU));

	/* The devices the ROM knows, the clock and the ROM. */
	mac68k_bootinfo_add_long(&info, BI_MAC_SCCBASE, read_long_at(LOW_SCC_READ));
	mac68k_bootinfo_add_long(&info, BI_MAC_BTIME, read_long_at(LOW_TIME));
	mac68k_bootinfo_add_long(&info, BI_MAC_ROMBASE, read_long_at(LOW_ROM_BASE));

	/* The command line, when there is one. */
	if (config.command_line[0] != '\0')
		mac68k_bootinfo_add_text(&info, BI_COMMAND_LINE, config.command_line);
	mac68k_bootinfo_end(&info);

	/* Refuses records that did not fit. */
	if (info.overflow)
		stop("the boot information does not fit its buffer");

	/* Succeeded: the records' size. */
	return info.used;
}

/*
 * Takes the area for the trampoline and the plan: RAM whose logical and
 * physical addresses are the same, so that the trampoline keeps running
 * when it turns translation off, and that no range is written to.  A
 * block that does not qualify is kept, so that the next is elsewhere; a
 * block where a range goes is followed by one that skips past it.
 */
static uint8_t *
take_handoff_area(
	uint32_t size,
	const struct mac68k_load_range *ranges,
	unsigned count)
{
	const struct mac68k_load_range *range;
	uint8_t *area;
	uint32_t base;
	uint32_t skip;
	unsigned tries;
	unsigned index;
	int fits;
	int identity;

	/* Takes blocks until one qualifies, within a limit. */
	for (tries = 0; tries < AREA_TRIES; tries++) {
		area = take_memory(size);
		if (area == NULL)
			break;
		base = (uint32_t)(uintptr_t)area;

		/* Skips past a range the block lies in or under. */
		fits = 1;
		for (index = 0; index < count; index++) {
			range = &ranges[index];
			if (base + size <= range->target)
				continue;
			if (base >= range->target + range->length)
				continue;
			fits = 0;
			skip = range->target + range->length - base;
			(void)take_memory(skip);
			break;
		}
		if (!fits)
			continue;

		/* Keeps a block whose addresses are its physical ones. */
		identity = is_identity(base, size);
		if (identity) {
			put_value("handoff area ", base);
			return area;
		}
	}

	/* No block qualified. */
	stop("no memory mapped at its own address for the handoff");
}

/* Tells whether every page of a range is at the same physical address. */
static int
is_identity(
	uint32_t logical,
	uint32_t size)
{
	uint32_t page;
	uint32_t end;
	uint32_t physical;
	int error;

	/* Translates each page the range touches. */
	page = logical & ~(MAC68K_PAGE_SIZE - 1U);
	end = logical + size;
	for (; page < end; page += MAC68K_PAGE_SIZE) {
		error = translate(NULL, page, &physical);
		if (error != 0)
			return 0;
		if (physical != page)
			return 0;
	}

	/* Succeeded: the range is where its addresses say. */
	return 1;
}

/*
 * Translates a logical address of the loader to its physical one, with
 * the MMU's own test: an address under a transparent window or with
 * translation off is its own physical address.  Reports zero when the
 * address is mapped.
 */
static int
translate(
	void *cookie,
	uint32_t logical,
	uint32_t *physical)
{
	uint32_t status;
	uint32_t page_mask;

	(void)cookie;

	/* Without translation every address is physical. */
	if ((translation_control & TC_ENABLE) == 0U) {
		*physical = logical;
		return 0;
	}

	/* Asks the MMU. */
	status = mac68k_cpu_test_address(logical);
	if ((status & MMUSR_BUS_ERROR) != 0U)
		return -1;
	if ((status & MMUSR_TRANSPARENT) != 0U) {
		*physical = logical;
		return 0;
	}
	if ((status & MMUSR_RESIDENT) == 0U)
		return -1;

	/* Succeeded: the page's frame and the offset in the page. */
	page_mask = 0x0fffU;
	if ((translation_control & TC_PAGE_8K) != 0U)
		page_mask = 0x1fffU;
	*physical = (status & ~page_mask) | (logical & page_mask);
	return 0;
}

/* Leaves the loader's lines on the screen for a moment, by the ROM's tick count. */
static void
pause_for_reading(
	void)
{
	uint32_t start;
	uint32_t now;
	uint32_t spins;

	/* Waits for the ticks to pass, or for a bound when they do not move. */
	start = read_long_at(LOW_TICKS);
	for (spins = 0; spins < PAUSE_SPIN_LIMIT; spins++) {
		now = read_long_at(LOW_TICKS);
		if (now - start >= PAUSE_TICKS)
			break;
	}
}

/* Shows why the loader stops, and stops. */
static void
stop(
	const char *problem)
{
	/* The reason stays on the screen; the ROM keeps running. */
	mac68k_screen_puts("zedBSD loader stopped: ");
	mac68k_screen_puts(problem);
	mac68k_screen_puts("\n");
	for (;;)
		;
}

/* Shows a line of text. */
static void
put_line(
	const char *text)
{
	/* The text and the line's end. */
	mac68k_screen_puts(text);
	mac68k_screen_puts("\n");
}

/* Shows a label and a value in hexadecimal. */
static void
put_value(
	const char *label,
	uint32_t value)
{
	/* The label, the value and the line's end. */
	mac68k_screen_puts(label);
	mac68k_screen_put_hex(value);
	mac68k_screen_puts("\n");
}

/* Reads a 32-bit word of the ROM's memory: a low-memory global or a handle's pointer. */
static uint32_t
read_long_at(
	uint32_t address)
{
	/* Succeeded: the word. */
	return *(const volatile uint32_t *)(uintptr_t)address;
}
