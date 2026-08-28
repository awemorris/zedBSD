/* CMD646 primary-channel ATA PIO driver for QEMU sun4u. */
#include "drivers/sun4u-cmd646.h"
#include <errno.h>
#include <hal/hal.h>
#include <kern/disk.h>

#define ATA_DATA 0U
#define ATA_ERROR 1U
#define ATA_COUNT 2U
#define ATA_LBA0 3U
#define ATA_LBA1 4U
#define ATA_LBA2 5U
#define ATA_DRIVE 6U
#define ATA_STATUS 7U
#define ATA_COMMAND 7U
#define ATA_BSY 0x80U
#define ATA_DRDY 0x40U
#define ATA_DRQ 0x08U
#define ATA_ERR 0x01U
static uint16_t cmd,ctl;static struct disk*ata_disk;static uint64_t sectors;
static int wait_status(uint8_t set,uint8_t clear){unsigned long n=10000000UL;while(n--){uint8_t s=hal_io_inp8(cmd+ATA_STATUS);if(s&ATA_ERR)return EIO;if((s&set)==set&&!(s&clear))return 0;}return ETIMEDOUT;}
static void select_lba(uint32_t lba){hal_io_outp8(cmd+ATA_COUNT,1);hal_io_outp8(cmd+ATA_LBA0,(uint8_t)lba);hal_io_outp8(cmd+ATA_LBA1,(uint8_t)(lba>>8));hal_io_outp8(cmd+ATA_LBA2,(uint8_t)(lba>>16));hal_io_outp8(cmd+ATA_DRIVE,(uint8_t)(0xe0U|(lba>>24&15U)));}
static int identify(void){uint16_t words[256];int error;hal_io_outp8(cmd+ATA_DRIVE,0xa0);hal_io_outp8(cmd+ATA_COUNT,0);hal_io_outp8(cmd+ATA_LBA0,0);hal_io_outp8(cmd+ATA_LBA1,0);hal_io_outp8(cmd+ATA_LBA2,0);hal_io_outp8(cmd+ATA_COMMAND,0xec);if(hal_io_inp8(cmd+ATA_STATUS)==0)return ENODEV;error=wait_status(ATA_DRQ,ATA_BSY);if(error)return error;for(unsigned i=0;i<256U;i++)words[i]=hal_io_inp16(cmd+ATA_DATA);sectors=(uint32_t)words[60]|(uint64_t)words[61]<<16;return sectors?0:EIO;}
static int block(int write,uint32_t lba,uint8_t*data){int error;select_lba(lba);hal_io_outp8(cmd+ATA_COMMAND,write?0x30U:0x20U);error=wait_status(ATA_DRQ,ATA_BSY);if(error)return error;for(unsigned i=0;i<256U;i++){if(write){uint16_t word=(uint16_t)data[i*2U]|(uint16_t)data[i*2U+1U]<<8;hal_io_outp16(cmd+ATA_DATA,word);}else{uint16_t word=hal_io_inp16(cmd+ATA_DATA);data[i*2U]=(uint8_t)word;data[i*2U+1U]=(uint8_t)(word>>8);}}if(write){hal_io_outp8(cmd+ATA_COMMAND,0xe7);return wait_status(ATA_DRDY,ATA_BSY);}return 0;}
static int submit(struct disk*d,struct bio*b){int error=0;uint8_t*p=b->b_data;(void)d;if(b->b_op==BIO_FLUSH){hal_io_outp8(cmd+ATA_COMMAND,0xe7);error=wait_status(ATA_DRDY,ATA_BSY);}else if(b->b_op!=BIO_READ&&b->b_op!=BIO_WRITE)error=EOPNOTSUPP;else if(b->b_mapped_block>0x0fffffffULL||b->b_block_count>0x10000000ULL-b->b_mapped_block)error=EINVAL;else for(uint32_t i=0;i<b->b_block_count&&!error;i++)error=block(b->b_op==BIO_WRITE,(uint32_t)b->b_mapped_block+i,p+(size_t)i*512U);if(error)hal_printf("cmd646: op=%u lba=%llu count=%u error=%d status=%x ata=%x\n",(unsigned)b->b_op,b->b_mapped_block,b->b_block_count,error,hal_io_inp8(cmd+ATA_STATUS),hal_io_inp8(cmd+ATA_ERROR));bio_complete(b,error,error?0:(size_t)b->b_block_count*512U);return 0;}
static int ioctl(struct disk*d,unsigned long r,void*a){(void)d;(void)r;(void)a;return EOPNOTSUPP;}static const struct disk_ops ops={.submit=submit,.ioctl=ioctl};
int sun4u_cmd646_init(uint16_t command_port,uint16_t control_port){int error;cmd=command_port;ctl=control_port;ata_disk=NULL;hal_io_outp8(ctl+2U,0x04);for(volatile unsigned i=0;i<100000U;i++);hal_io_outp8(ctl+2U,0x02);hal_io_outp8(cmd+ATA_DRIVE,0xa0);error=wait_status(ATA_DRDY,ATA_BSY);if(error){hal_printf("cmd646: reset error=%d status=%x ata=%x\n",error,hal_io_inp8(cmd+ATA_STATUS),hal_io_inp8(cmd+ATA_ERROR));return error;}error=identify();if(error){hal_printf("cmd646: identify error=%d status=%x ata=%x\n",error,hal_io_inp8(cmd+ATA_STATUS),hal_io_inp8(cmd+ATA_ERROR));return error;}ata_disk=disk_alloc();if(!ata_disk)return ENOMEM;error=disk_alloc_sd_name(ata_disk);if(error){(void)disk_destroy(ata_disk);ata_disk=NULL;return error;}ata_disk->d_block_size=512;ata_disk->d_block_count=sectors;ata_disk->d_max_transfer_blocks=1;ata_disk->d_ops=&ops;error=disk_create(ata_disk);if(error){ata_disk=NULL;return error;}hal_printf("SPARCV9 IDE PASS sectors=%llu\n",sectors);return 0;}
struct disk *sun4u_cmd646_disk(void){return ata_disk;}
