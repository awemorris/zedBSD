#include <hal/hal.h>
#include "../defs.h"
#include "../bsp.h"
#include "mailbox.h"
#include "framebuffer.h"

#define TAG_PHYSICAL 0x00048003U
#define TAG_VIRTUAL 0x00048004U
#define TAG_DEPTH 0x00048005U
#define TAG_ORDER 0x00048006U
#define TAG_ALLOCATE 0x00040001U
#define TAG_PITCH 0x00040008U

static uint32 request[40] __attribute__((aligned(16)));
static volatile uint8 *pixels;
static uint64 pixels_phys,pixels_size;
static uint32 width,height,pitch,order,x_origin,y_origin;

static const uint32 palette[16]={
	0x000000,0x0000aa,0x00aa00,0x00aaaa,0xaa0000,0xaa00aa,0xaa5500,0xaaaaaa,
	0x555555,0x5555ff,0x55ff55,0x55ffff,0xff5555,0xff55ff,0xffff55,0xffffff
};

static const uint8 digits[10][7]={
	{14,17,19,21,25,17,14},{4,12,4,4,4,4,14},
	{14,17,1,2,4,8,31},{30,1,1,14,1,1,30},
	{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
	{14,16,16,30,17,17,14},{31,1,2,4,8,8,8},
	{14,17,17,14,17,17,14},{14,17,17,15,1,1,14}
};
static const uint8 letters[26][7]={
	{14,17,17,31,17,17,17},{30,17,17,30,17,17,30},
	{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},
	{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
	{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},
	{14,4,4,4,4,4,14},{7,2,2,2,18,18,12},
	{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
	{17,27,21,21,17,17,17},{17,25,21,19,17,17,17},
	{14,17,17,17,17,17,14},{30,17,17,30,16,16,16},
	{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
	{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
	{17,17,17,17,17,17,14},{17,17,17,17,17,10,4},
	{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
	{17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
};

static uint8 glyph_row(int character,unsigned row)
{
	if(row>=7)return 0;
	if(character>='0'&&character<='9')return digits[character-'0'][row];
	if(character>='a'&&character<='z')character-='a'-'A';
	if(character>='A'&&character<='Z')return letters[character-'A'][row];
	switch(character){
	case '-':return row==3?31:0;case '_':return row==6?31:0;
	case '=':return row==2||row==4?31:0;case '|':return 4;
	case '.':return row==6?4:0;case ',':return row==5?4:row==6?8:0;
	case ':':return row==2||row==5?4:0;case ';':return row==2?4:row==5?4:row==6?8:0;
	case '/':return (uint8)(1U<<(4U-(row*5U/7U)));
	case '\\':return (uint8)(1U<<(row*5U/7U));
	case '+':return row==3?31:(row>=1&&row<=5?4:0);
	case '!':return row<5?4:row==6?4:0;case '?':return (uint8[]){14,17,1,2,4,0,4}[row];
	case '[':return row==0||row==6?14:8;case ']':return row==0||row==6?14:2;
	case '(':return row==0||row==6?2:row==1||row==5?4:8;
	case ')':return row==0||row==6?8:row==1||row==5?4:2;
	case '<':return row==3?8:row==2||row==4?4:row==1||row==5?2:0;
	case '>':return row==3?2:row==2||row==4?4:row==1||row==5?8:0;
	case '#':return row==2||row==4?31:row>0&&row<6?10:0;
	case '*':return row==2?21:row==3?14:row==4?21:0;
	case '"':return row<2?10:0;case '\'':return row<2?4:0;
	default:return 0;
	}
}

static uint32 colour(uint8 index)
{
	uint32 c=palette[index&15U];
	if(order)return c;
	return ((c&0xffU)<<16)|(c&0xff00U)|((c>>16)&0xffU);
}

static void put_pixel(unsigned x,unsigned y,uint32 value)
{
	volatile uint32 *p=(volatile uint32 *)(pixels+(size_t)y*pitch+(size_t)x*4U);
	*p=value;
}

void rpi4_framebuffer_cell(unsigned row,unsigned column,int character,uint8 attribute)
{
	uint32 fg=colour(attribute&15U),bg=colour(attribute>>4);
	unsigned x=x_origin+column*8U,y=y_origin+row*16U;
	if(!pixels||row>=25||column>=80)return;
	for(unsigned py=0;py<16;py++){
		uint8 bits=(py==0||py==15)?0:glyph_row(character,(py-1U)/2U);
		for(unsigned px=0;px<8;px++)
			put_pixel(x+px,y+py,(px>=1&&px<=5&&(bits&(1U<<(5U-px))))?fg:bg);
	}
}

void rpi4_framebuffer_cursor(unsigned row,unsigned column,int visible)
{
	if(!pixels||!visible||row>=25||column>=80)return;
	for(unsigned y=14;y<16;y++)for(unsigned x=0;x<8;x++)
		put_pixel(x_origin+column*8U+x,y_origin+row*16U+y,colour(15));
}

int rpi4_framebuffer_init(uintptr_t mailbox_phys)
{
	unsigned i=0;
#define WORD(v) request[i++]=(v)
	WORD(0);WORD(0);
	WORD(TAG_PHYSICAL);WORD(8);WORD(8);WORD(640);WORD(480);
	WORD(TAG_VIRTUAL);WORD(8);WORD(8);WORD(640);WORD(480);
	WORD(TAG_DEPTH);WORD(4);WORD(4);WORD(32);
	WORD(TAG_ORDER);WORD(4);WORD(4);WORD(1);
	WORD(TAG_ALLOCATE);WORD(8);WORD(8);WORD(4096);WORD(0);
	WORD(TAG_PITCH);WORD(4);WORD(4);WORD(0);WORD(0);
	request[0]=i*4U;
	if(rpi4_mailbox_property(mailbox_phys,request,request[0])!=0)return -1;
	width=request[5];height=request[6];order=request[19];
	pixels_phys=(uint64)(request[23]&0x3fffffffU);pixels_size=request[24];pitch=request[28];
	if(width<640||height<400||pitch<width*4U||pixels_size<(uint64)pitch*height||
	   pixels_phys==0||pixels_phys+pixels_size< pixels_phys)return -1;
	pixels=(volatile uint8 *)(ARM64_DIRECT_BASE+pixels_phys);
	x_origin=(width-640U)/2U;y_origin=(height-400U)/2U;
	rpi4_boot_set_framebuffer(pixels_phys,pixels_size,width,height,pitch,order);
	for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++)put_pixel(x,y,0);
	return 0;
#undef WORD
}

int rpi4_framebuffer_ready(void){return pixels!=0;}
