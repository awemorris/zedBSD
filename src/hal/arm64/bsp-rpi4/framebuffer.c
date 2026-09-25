#include <hal/hal.h>
#include "../defs.h"
#include "../bsp.h"
#include "mailbox.h"
#include "framebuffer.h"
#include "font.h"
#include "led.h"

#define TAG_GET_PHYSICAL 0x00040003U
#define TAG_PHYSICAL 0x00048003U
#define TAG_VIRTUAL 0x00048004U
#define TAG_DEPTH 0x00048005U
#define TAG_ORDER 0x00048006U
#define TAG_ALLOCATE 0x00040001U
#define TAG_PITCH 0x00040008U

static uint32_t request[40] __attribute__((aligned(16)));
static volatile uint8_t *pixels;
static uint64_t pixels_phys,pixels_size;
static uint32_t width,height,pitch,order,x_origin,y_origin;

static const uint32_t palette[16]={
	0x000000,0x0000aa,0x00aa00,0x00aaaa,0xaa0000,0xaa00aa,0xaa5500,0xaaaaaa,
	0x555555,0x5555ff,0x55ff55,0x55ffff,0xff5555,0xff55ff,0xffff55,0xffffff
};

static uint32_t colour(uint8_t index)
{
	uint32_t c=palette[index&15U];
	if(order)return c;
	return ((c&0xffU)<<16)|(c&0xff00U)|((c>>16)&0xffU);
}

static void put_pixel(unsigned x,unsigned y,uint32_t value)
{
	volatile uint32_t *p=(volatile uint32_t *)(pixels+(size_t)y*pitch+(size_t)x*4U);
	*p=value;
}

/*
 * Makes rows of pixels reach memory.  The framebuffer is written through
 * the cacheable direct map, and the display reads memory past the CPU's
 * caches, so a pixel that stays in the data cache is never shown.
 */
static void
flush_rows(
	unsigned x,
	unsigned y,
	unsigned columns,
	unsigned rows)
{
	uintptr_t start;
	unsigned line;

	/* Each row's bytes, cleaned from the data cache to memory. */
	for (line = 0; line < rows; line++) {
		start = (uintptr_t)(pixels + (size_t)(y + line) * pitch + (size_t)x * 4U);
		hal_dcache_clean_range(start, (size_t)columns * 4U);
	}

	/* The display is outside the CPU's shareable domain: a full barrier. */
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Paints one character cell: a glyph of the 8x16 console font in the
 * attribute's foreground on its background.
 */
void
rpi4_framebuffer_cell(
	unsigned row,
	unsigned column,
	int character,
	uint8_t attribute)
{
	const uint8_t *glyph;
	uint32_t foreground;
	uint32_t background;
	uint32_t value;
	unsigned x;
	unsigned y;
	unsigned line;
	unsigned bit;
	uint8_t bits;

	/* Nothing to paint before the framebuffer exists, or off the grid. */
	if (pixels == 0 || row >= 25U || column >= 80U)
		return;

	/* The glyph of the character (the font covers every byte value). */
	glyph = &rpi4_font8x16[((unsigned)character & 0xffU) * RPI4_FONT_HEIGHT];
	foreground = colour(attribute & 15U);
	background = colour(attribute >> 4);
	x = x_origin + column * 8U;
	y = y_origin + row * RPI4_FONT_HEIGHT;

	/* Each row, from its leftmost pixel in the high bit. */
	for (line = 0; line < RPI4_FONT_HEIGHT; line++) {
		bits = glyph[line];
		for (bit = 0; bit < 8U; bit++) {
			value = background;
			if ((bits & (0x80U >> bit)) != 0)
				value = foreground;
			put_pixel(x + bit, y + line, value);
		}
	}

	/* The cell, out to the memory the display reads. */
	flush_rows(x, y, 8U, RPI4_FONT_HEIGHT);
}

void rpi4_framebuffer_cursor(unsigned row,unsigned column,int visible)
{
	if(!pixels||!visible||row>=25||column>=80)return;
	for(unsigned y=14;y<16;y++)for(unsigned x=0;x<8;x++)
		put_pixel(x_origin+column*8U+x,y_origin+row*16U+y,colour(15));
	flush_rows(x_origin+column*8U,y_origin+row*16U+14U,8U,2U);
}

int rpi4_framebuffer_init(uintptr_t mailbox_phys)
{
	unsigned i=0;
	uint32_t want_width=640U,want_height=480U;
#define WORD(v) request[i++]=(v)
	/*
	 * The size the firmware set up for the display (config.txt's
	 * framebuffer_width and framebuffer_height, or the mode's), so that the
	 * framebuffer is scanned out as it is, without scaling; 640x480 when the
	 * firmware reports none (QEMU).
	 */
	WORD(0);WORD(0);
	WORD(TAG_GET_PHYSICAL);WORD(8);WORD(0);WORD(0);WORD(0);
	WORD(0);
	request[0]=i*4U;
	if(rpi4_mailbox_property(mailbox_phys,request,request[0])==0&&
	   request[5]>=640U&&request[6]>=400U&&request[5]<=4096U&&request[6]<=4096U){
		want_width=request[5];want_height=request[6];
	}
	i=0;
	WORD(0);WORD(0);
	WORD(TAG_PHYSICAL);WORD(8);WORD(8);WORD(want_width);WORD(want_height);
	WORD(TAG_VIRTUAL);WORD(8);WORD(8);WORD(want_width);WORD(want_height);
	WORD(TAG_DEPTH);WORD(4);WORD(4);WORD(32);
	WORD(TAG_ORDER);WORD(4);WORD(4);WORD(1);
	WORD(TAG_ALLOCATE);WORD(8);WORD(8);WORD(4096);WORD(0);
	WORD(TAG_PITCH);WORD(4);WORD(4);WORD(0);WORD(0);
	request[0]=i*4U;
	if(rpi4_mailbox_property(mailbox_phys,request,request[0])!=0)return -1;
	width=request[5];height=request[6];order=request[19];
	pixels_phys=(uint64_t)(request[23]&0x3fffffffU);pixels_size=request[24];pitch=request[28];
	if(width<640||height<400||pitch<width*4U||pixels_size<(uint64_t)pitch*height||
	   pixels_phys==0||pixels_phys+pixels_size< pixels_phys)return -1;
	pixels=(volatile uint8_t *)(ARM64_DIRECT_BASE+pixels_phys);
	x_origin=(width-640U)/2U;y_origin=(height-400U)/2U;
	rpi4_boot_set_framebuffer(pixels_phys,pixels_size,width,height,pitch,order);
	for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++)put_pixel(x,y,0);
	hal_dcache_clean_range((uintptr_t)pixels,(size_t)pixels_size);
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
#undef WORD
}

int rpi4_framebuffer_ready(void){return pixels!=0;}

/*
 * Shows a test pattern for three seconds: the left third red, the middle
 * green and the right blue, with a white frame 16 pixels wide.  Seen on a
 * board, it tells whether the display shows what the CPU writes, where, and
 * in which colour order.
 */
void
rpi4_framebuffer_test_pattern(
	void)
{
	uint32_t value;
	unsigned x;
	unsigned y;

	/* Nothing to show without a framebuffer. */
	if (pixels == 0)
		return;

	/* Each pixel: the frame, or the colour of its third. */
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			value = colour(12);
			if (x >= width / 3U)
				value = colour(10);
			if (x >= width / 3U * 2U)
				value = colour(9);
			if (x < 16U || y < 16U || x >= width - 16U || y >= height - 16U)
				value = colour(15);
			put_pixel(x, y, value);
		}
	}

	/* Out to memory, then time to look at it. */
	hal_dcache_clean_range((uintptr_t)pixels, (size_t)pixels_size);
	__asm__ volatile("dsb sy" ::: "memory");
	rpi4_delay_ms(3000U);

	/* Black again for the console. */
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++)
			put_pixel(x, y, 0);
	}
	hal_dcache_clean_range((uintptr_t)pixels, (size_t)pixels_size);
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Reports what the firmware gave for the framebuffer.
 */
void
rpi4_framebuffer_describe(
	void)
{
	/* The size, the row length, the order and where it is. */
	hal_printf("RPI4 FRAMEBUFFER %ux%u pitch=%u order=%u phys=%llx size=%llx\n",
	    width, height, pitch, order,
	    (unsigned long long)pixels_phys, (unsigned long long)pixels_size);
}
