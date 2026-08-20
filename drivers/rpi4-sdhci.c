/*
 * Raspberry Pi 4 BCM2711 eMMC2 SDHCI PIO driver
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "drivers/rpi4-sdhci.h"
#include <errno.h>
#include <hal/hal.h>
#include <kern/disk.h>

#define DIRECT_BASE 0xffff000000000000ULL
#define REG_BLOCK_SIZE 0x04
#define REG_BLOCK_COUNT 0x06
#define REG_ARGUMENT 0x08
#define REG_TRANSFER_MODE 0x0c
#define REG_COMMAND 0x0e
#define REG_RESPONSE0 0x10
#define REG_BUFFER 0x20
#define REG_PRESENT 0x24
#define REG_HOST_CONTROL 0x28
#define REG_POWER_CONTROL 0x29
#define REG_CLOCK_CONTROL 0x2c
#define REG_TIMEOUT_CONTROL 0x2e
#define REG_SOFTWARE_RESET 0x2f
#define REG_INT_STATUS 0x30
#define REG_INT_ENABLE 0x34
#define REG_SIGNAL_ENABLE 0x38
#define REG_CAPABILITIES 0x40

#define PRESENT_CMD_INHIBIT 0x00000001U
#define PRESENT_DATA_INHIBIT 0x00000002U
#define INT_CMD_COMPLETE 0x00000001U
#define INT_TRANSFER_COMPLETE 0x00000002U
#define INT_BUFFER_WRITE_READY 0x00000010U
#define INT_BUFFER_READ_READY 0x00000020U
#define INT_ERROR 0x00008000U
#define INT_ERROR_MASK 0xffff0000U
#define CMD_RESP_LONG 0x0001U
#define CMD_RESP_SHORT 0x0002U
#define CMD_RESP_BUSY 0x0003U
#define CMD_CRC 0x0008U
#define CMD_INDEX 0x0010U
#define CMD_DATA 0x0020U
#define XFER_READ 0x0010U

struct rpi4_sd_unit {
	volatile uint8_t *base;
	struct disk *disk;
	uint32_t rca;
	int high_capacity;
};

static struct rpi4_sd_unit unit;
static uint32_t last_error_status;

static uint8_t r8(unsigned offset)
{ return hal_mmio_read8(unit.base + offset); }
static uint16_t r16(unsigned offset)
{ return hal_mmio_read16(unit.base + offset); }
static uint32_t r32(unsigned offset)
{ return hal_mmio_read32(unit.base + offset); }
static void w8(unsigned offset,uint8_t value)
{ hal_mmio_write8(unit.base + offset,value); }
static void w16(unsigned offset,uint16_t value)
{ hal_mmio_write16(unit.base + offset,value); }
static void w32(unsigned offset,uint32_t value)
{ hal_mmio_write32(unit.base + offset,value); }

static uint64_t counter(void)
{ uint64_t value;__asm__ volatile("mrs %0,cntpct_el0":"=r"(value));return value; }
static uint64_t frequency(void)
{ uint64_t value;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(value));return value; }

static int wait_bits(unsigned reg,uint32_t mask,uint32_t expected)
{
	uint64_t deadline=counter()+frequency()*2U;
	while((r32(reg)&mask)!=expected)
		if(counter()>=deadline)return ETIMEDOUT;
	return 0;
}

static int wait_interrupt(uint32_t wanted)
{
	uint64_t deadline=counter()+frequency()*2U;
	for(;;){
		uint32_t status=r32(REG_INT_STATUS);
		if(status&(INT_ERROR|INT_ERROR_MASK)){
			last_error_status=status;w32(REG_INT_STATUS,status);return EIO;
		}
		if(status&wanted){w32(REG_INT_STATUS,wanted);return 0;}
		if(counter()>=deadline)return ETIMEDOUT;
	}
}

static int command(uint32_t index,uint32_t argument,uint16_t flags,
	uint32_t *response)
{
	uint32_t inhibit=PRESENT_CMD_INHIBIT;
	int error;
	if(flags&CMD_DATA)inhibit|=PRESENT_DATA_INHIBIT;
	error=wait_bits(REG_PRESENT,inhibit,0);if(error)return error;
	w32(REG_INT_STATUS,0xffffffffU);
	w32(REG_ARGUMENT,argument);
	w16(REG_COMMAND,(uint16_t)((index<<8)|flags));
	error=wait_interrupt(INT_CMD_COMPLETE);if(error)return error;
	if(response)*response=r32(REG_RESPONSE0);
	return 0;
}

static int app_command(uint32_t command_index,uint32_t argument,uint16_t flags,
	uint32_t *response)
{
	int error=command(55,unit.rca<<16,CMD_RESP_SHORT|CMD_CRC|CMD_INDEX,0);
	return error?error:command(command_index,argument,flags,response);
}

static int set_clock(uint32_t target)
{
	uint32_t caps=r32(REG_CAPABILITIES),base=((caps>>8)&0xffU)*1000000U;
	uint32_t divisor=1,encoded;
	uint16_t clock;
	uint64_t deadline;
	if(base==0)base=100000000U;
	while(divisor<0x400U&&base/(2U*divisor)>target)divisor<<=1;
	encoded=(divisor&0xffU)<<8|(divisor&0x300U)>>2;
	w16(REG_CLOCK_CONTROL,0);
	w16(REG_CLOCK_CONTROL,(uint16_t)(encoded|1U));
	deadline=counter()+frequency();
	do{clock=r16(REG_CLOCK_CONTROL);if(clock&2U)break;}while(counter()<deadline);
	if(!(clock&2U))return ETIMEDOUT;
	w16(REG_CLOCK_CONTROL,(uint16_t)(clock|4U));
	return 0;
}

static int controller_init(void)
{
	uint32_t response=0;
	uint64_t deadline;
	int error;
	w8(REG_SOFTWARE_RESET,1U);
	deadline=counter()+frequency();
	while(r8(REG_SOFTWARE_RESET)&1U)if(counter()>=deadline)return ETIMEDOUT;
	w8(REG_POWER_CONTROL,0x0fU);
	w8(REG_TIMEOUT_CONTROL,0x0eU);
	w32(REG_INT_ENABLE,0xffffffffU);
	w32(REG_SIGNAL_ENABLE,0);
	if((error=set_clock(400000U))!=0){hal_printf("sdhci: clock identification %u\n",(unsigned)error);return error;}
	if((error=command(0,0,0,0))!=0){hal_printf("sdhci: CMD0 %u\n",(unsigned)error);return error;}
	error=command(8,0x1aaU,CMD_RESP_SHORT|CMD_CRC|CMD_INDEX,&response);
	if(error)hal_printf("sdhci: CMD8 error=%u irq=%x\n",(unsigned)error,last_error_status);
	else hal_printf("sdhci: CMD8 response=%x\n",response);
	unit.rca=0;
	deadline=counter()+frequency()*2U;
	do{
		error=app_command(41,0x40300000U,CMD_RESP_SHORT,&response);
		if(error==0&&(response&0x80000000U))break;
	}while(counter()<deadline);
	if(error!=0||!(response&0x80000000U)){hal_printf("sdhci: ACMD41 error=%u irq=%x ocr=%x\n",(unsigned)error,last_error_status,response);return ETIMEDOUT;}
	unit.high_capacity=(response&0x40000000U)!=0;
	if((error=command(2,0,CMD_RESP_LONG|CMD_CRC,0))!=0){hal_printf("sdhci: CMD2 %u\n",(unsigned)error);return error;}
	if((error=command(3,0,CMD_RESP_SHORT|CMD_CRC|CMD_INDEX,&response))!=0)
		{hal_printf("sdhci: CMD3 %u\n",(unsigned)error);return error;}
	unit.rca=response>>16;if(unit.rca==0)return EIO;
	if((error=command(9,unit.rca<<16,CMD_RESP_LONG|CMD_CRC,0))!=0)
		{hal_printf("sdhci: CMD9 %u\n",(unsigned)error);return error;}
	if((error=command(7,unit.rca<<16,CMD_RESP_BUSY|CMD_CRC|CMD_INDEX,0))!=0)
		{hal_printf("sdhci: CMD7 %u\n",(unsigned)error);return error;}
	if(!unit.high_capacity&&
	   (error=command(16,512,CMD_RESP_SHORT|CMD_CRC|CMD_INDEX,0))!=0)
		return error;
	if((error=set_clock(25000000U))!=0){hal_printf("sdhci: clock transfer %u\n",(unsigned)error);return error;}
	/* ACMD6 selects the four-bit bus; keep one-bit mode if it is rejected. */
	if(app_command(6,2,CMD_RESP_SHORT|CMD_CRC|CMD_INDEX,0)==0)
		w8(REG_HOST_CONTROL,(uint8_t)(r8(REG_HOST_CONTROL)|2U));
	return 0;
}

static int transfer_block(int write,uint64_t block,void *buffer)
{
	uint32_t argument,ready=write?INT_BUFFER_WRITE_READY:INT_BUFFER_READ_READY;
	uint32_t *words=buffer;
	int error;
	if(block>0xffffffffULL)return EINVAL;
	argument=(uint32_t)(unit.high_capacity?block:block*512U);
	w16(REG_BLOCK_SIZE,512);w16(REG_BLOCK_COUNT,1);
	w16(REG_TRANSFER_MODE,write?0:XFER_READ);
	error=command(write?24U:17U,argument,
		CMD_RESP_SHORT|CMD_CRC|CMD_INDEX|CMD_DATA,0);
	if(error)return error;
	if((error=wait_interrupt(ready))!=0)return error;
	for(unsigned i=0;i<128U;i++){
		if(write)w32(REG_BUFFER,words[i]);else words[i]=r32(REG_BUFFER);
	}
	return wait_interrupt(INT_TRANSFER_COMPLETE);
}

static int sd_submit(struct disk *disk,struct bio *bio)
{
	uint8_t *data=bio->b_data;int error=0;
	if(bio->b_op==BIO_FLUSH)error=0;
	else if(bio->b_op!=BIO_READ&&bio->b_op!=BIO_WRITE)error=EOPNOTSUPP;
	else for(uint32_t i=0;i<bio->b_block_count&&error==0;i++)
		error=transfer_block(bio->b_op==BIO_WRITE,bio->b_mapped_block+i,
			data+(size_t)i*512U);
	if(error)hal_printf("sdhci: op=%u lba=%llu count=%u error=%d status=%x\n",
		(unsigned)bio->b_op,bio->b_mapped_block,bio->b_block_count,error,
		r32(REG_INT_STATUS));
	bio_complete(bio,error,error?0:(size_t)bio->b_block_count*512U);
	(void)disk;return 0;
}

static int sd_ioctl(struct disk *disk,unsigned long request,void *argument)
{(void)disk;(void)request;(void)argument;return EOPNOTSUPP;}
static const struct disk_ops sd_ops={.submit=sd_submit,.ioctl=sd_ioctl};

int rpi4_sdhci_init(uintptr_t physical_base)
{
	int error;
	unit.base=(volatile uint8_t *)(DIRECT_BASE+physical_base);
	unit.disk=0;
	hal_printf("sdhci: base=%p caps=%x present=%x version=%x\n",
		(void *)physical_base,r32(REG_CAPABILITIES),r32(REG_PRESENT),r16(0xfe));
	error=controller_init();if(error){hal_printf("sdhci: init error=%u status=%x present=%x\n",
		(unsigned)error,r32(REG_INT_STATUS),r32(REG_PRESENT));return error;}
	unit.disk=disk_alloc();if(!unit.disk)return ENOMEM;
	unit.disk->d_name[0]='m';unit.disk->d_name[1]='m';
	unit.disk->d_name[2]='c';unit.disk->d_name[3]='b';
	unit.disk->d_name[4]='l';unit.disk->d_name[5]='k';
	unit.disk->d_name[6]='0';unit.disk->d_name[7]='\0';
	unit.disk->d_flags=DISK_REMOVABLE;
	unit.disk->d_block_size=512;unit.disk->d_block_count=0xffffffffULL;
	unit.disk->d_max_transfer_blocks=1;unit.disk->d_ops=&sd_ops;
	unit.disk->d_data=&unit;
	error=disk_create(unit.disk);if(error){unit.disk=0;return error;}
	hal_printf("sdhci: mmcblk0 ready (%s addressing)\n",
		unit.high_capacity?"block":"byte");
	return 0;
}

struct disk *rpi4_sdhci_disk(void){return unit.disk;}
