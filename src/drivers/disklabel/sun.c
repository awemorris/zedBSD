/* Big-endian Sun disklabel partition scanner. */
#include <drivers/disklabel.h>

static uint16_t be16(const uint8_t*p){return(uint16_t)((uint16_t)p[0]<<8|p[1]);}
static uint32_t be32(const uint8_t*p){return(uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int scan(const struct partition_scheme*s,struct disk*d,struct partition*e,unsigned capacity){uint8_t sector[512];uint16_t sum=0,heads,sectors;unsigned count=capacity<8U?capacity:8U,i;(void)s;
	if(d->d_block_size!=512U||disk_read(d,0,1,sector)!=0||be16(sector+508)!=0xdabeU)return -1;
	for(i=0;i<256U;i++)sum^=be16(sector+i*2U);
	if(sum!=0)return -1;
	heads=be16(sector+436);sectors=be16(sector+438);if(!heads||!sectors)return -1;
	for(i=0;i<count;i++){uint64_t start=(uint64_t)be32(sector+444U+i*8U)*heads*sectors;uint64_t blocks=be32(sector+448U+i*8U);e[i].p_parent=d;e[i].p_disk=NULL;e[i].p_index=i;e[i].p_start_block=start;e[i].p_data_block=start;e[i].p_block_count=0;e[i].p_flags=0;e[i].p_uuid[0]='\0';e[i].p_label[0]='s';e[i].p_label[1]='l';e[i].p_label[2]='i';e[i].p_label[3]='c';e[i].p_label[4]='e';e[i].p_label[5]=(char)('a'+i);e[i].p_label[6]='\0';if(blocks&&start<d->d_block_count&&blocks<=d->d_block_count-start)e[i].p_block_count=blocks;if(i==1U)e[i].p_flags|=PARTITION_BOOTABLE;}return(int)count;}
const struct partition_scheme partition_scheme_sun={.name="sun",.scan=scan};
