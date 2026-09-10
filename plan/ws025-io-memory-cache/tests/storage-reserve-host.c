/* Production BOT attach/limit/rollback with bounded model reservations.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define USB_STORAGE_CUSTOM_HOST
#define main retained_no_media_main
#include "../../ws004-hardware/tests/usb-storage-no-media-test.c"
#undef main
static unsigned caps,reserve_calls,reserve_failure,wire_stage,wire_opcode;
static uint32_t logical_size=512;
static size_t wire_size;
static unsigned data_commands;
static unsigned inject_ready_ua, inject_read_ua, command_failed;
static unsigned transport_faults, reset_commands;
static unsigned sense_key=6,sense_asc=0x29,sense_ascq,sense_code=0x70;
static unsigned mode_payload, mode_wp, sync_fail, sync_sense, last_write_flags;
static int publish_error;
static int retire_error;
static unsigned retire_calls,wire_commands,disk_serial;
static unsigned char wire_tag[4],medium[65536];
unsigned drv_usb_device_hcd_capabilities(const struct drv_usb_device *d)
{(void)d;return caps;}
int drv_usb_urb_reserve_transfer(struct drv_usb_urb *u,size_t n)
{
 reserve_calls++;CHECK(n==(u->endpoint?65536:8192));
 return reserve_failure==reserve_calls?ENOMEM:0;
}
struct disk *disk_alloc(void) {disk_calls++;return calloc(1,sizeof(struct disk));}
int disk_alloc_sd_name(struct disk *d) {strcpy(d->d_name,"sd0");return 0;}
int disk_create(struct disk *d) {if(!publish_error)d->d_dev=++disk_serial;return publish_error;}
int disk_gone_if_idle(struct disk *d) {(void)d;return 0;}
int disk_media_retire(struct disk *d) { CHECK(d->d_media_revoked);retire_calls++;return retire_error; }
int disk_destroy(struct disk *d) {free(d);return 0;}
static int wire(struct drv_usb_urb *u)
{
 unsigned char *b=u->buffer;
 if(!u->endpoint){
  if(u->control.request==USB_MASS_STORAGE_RESET){reset_commands++;wire_stage=0;u->actual=0;return 0;}
  CHECK(u->control.request==USB_MASS_STORAGE_GET_MAX_LUN);b[0]=0;u->actual=1;return 0;
 }
 if(wire_stage==0){wire_commands++;CHECK(u->length==31);wire_opcode=b[15];wire_size=get_le32(b+8);memcpy(wire_tag,b+4,4);wire_stage=wire_size?1:2;
  command_failed=0;
  if(wire_opcode==SCSI_READ_10 && transport_faults){transport_faults--;wire_stage=0;u->actual=0;return EIO;}
  if(wire_opcode==SCSI_TEST_UNIT_READY&&inject_ready_ua){inject_ready_ua--;command_failed=1;}
  if((wire_opcode==SCSI_READ_10||wire_opcode==0x2a)&&inject_read_ua){inject_read_ua--;command_failed=1;}
  if(wire_opcode==0x2a)last_write_flags=b[16];
  if(wire_opcode==SCSI_SYNCHRONIZE_CACHE_10 && sync_fail){sync_fail--;command_failed=1;sync_sense=1;}
 }
 else if(wire_stage==1){
  CHECK(u->length==wire_size);
  if(wire_opcode==SCSI_READ_10||wire_opcode==0x2aU){
   CHECK(wire_size<=sizeof(medium));data_commands++;
   if(wire_opcode==SCSI_READ_10)memcpy(b,medium,wire_size);else memcpy(medium,b,wire_size);
  }else{
   memset(b,0,u->length);
   if(wire_opcode==SCSI_REQUEST_SENSE){b[0]=sense_code;b[2]=sense_key;b[7]=10;b[12]=sense_asc;b[13]=sense_ascq;if(sync_sense){b[0]=0x70;b[2]=5;b[12]=0x20;b[13]=0;sync_sense=0;}}
   if(wire_opcode==0x1a && mode_payload){b[0]=6;b[2]=0x10|(mode_wp?0x80:0);b[4]=8;b[5]=1;b[6]=4;}
   if(wire_opcode==SCSI_READ_CAPACITY_10){put_be32(b,1023);put_be32(b+4,logical_size);}
  }
  wire_stage=2;
 }else{CHECK(u->length==13);memset(b,0,13);put_le32(b,BOT_CSW_SIGNATURE);memcpy(b+4,wire_tag,4);b[12]=command_failed?1:0;wire_stage=0;}
 u->actual=u->length;return 0;
}
int main(void)
{
 struct drv_usb_interface interface;struct usb_storage *s;struct bio bio;
 struct usb_storage staged;
 unsigned i;unsigned char input[65536],output[65536];
 /* A failed publication retains the caller-owned control object and URBs. */
 fixture_reset(&interface,0,0x70);memset(&staged,0,sizeof(staged));
 staged.interface=&interface;staged.device=&fixture_device;
 staged.bulk_in=&fixture_bulk_in;staged.bulk_out=&fixture_bulk_out;
 staged.block_size=512;staged.block_count=128;staged.flush_policy=DRV_USB_SCSI_FLUSH_WRITE_THROUGH;
 caps=DRV_USB_HCD_CAP_TRANSFER_RESERVE;reserve_calls=0;
 CHECK(storage_urbs_alloc(&staged)==0);
 publish_error=EIO;CHECK(storage_publish_disk(&staged)==EIO && staged.disk==NULL && live_urbs==3);
 publish_error=0;CHECK(storage_publish_disk(&staged)==0 && staged.disk!=NULL && live_urbs==3);
 CHECK(storage_publish_disk(&staged)==EBUSY);
 CHECK(disk_destroy(staged.disk)==0);staged.disk=NULL;storage_urbs_free(&staged);
 CHECK(live_urbs==0 && live_allocations==0);
 for(i=0;i<7;i++){
  fixture_reset(&interface,0,0x70);wire_stage=reserve_calls=0;
  transfer_override=wire;caps=i==0?0:DRV_USB_HCD_CAP_TRANSFER_RESERVE;
  reserve_failure=i>=2&&i<=4?i-1:0;logical_size=i==5?4096:i==6?131072:512;
  if(reserve_failure){
   CHECK(storage_attach(&interface,&storage_ids[0])==ENOMEM);
   CHECK(!interface.driver_data&&live_urbs==0&&live_allocations==0&&disk_calls==0);continue;
  }
  CHECK(storage_attach(&interface,&storage_ids[0])==0);s=interface.driver_data;CHECK(s&&s->disk);
  CHECK(s->disk->d_max_transfer_blocks==(i==0?16:i==5?16:i==6?1:128));
  CHECK(reserve_calls==(i==0||i==6?0:3));
  if(i==1||i==5){
   memset(input,0x71,sizeof(input));memset(&bio,0,sizeof(bio));bio.b_op=BIO_WRITE;
   bio.b_block_count=65536/logical_size;bio.b_data=input;data_commands=0;
   CHECK(storage_submit(s->disk,&bio)==0&&bio_error==0&&bio_bytes==65536&&data_commands==1);
   bio.b_op=BIO_READ;bio.b_data=output;
   CHECK(storage_submit(s->disk,&bio)==0&&bio_error==0&&data_commands==2);CHECK(!memcmp(input,output,sizeof(input)));
  }
  CHECK(storage_detach(&interface,0)==0);CHECK(live_urbs==0&&live_allocations==0);
 }
 /* Initial power-on attention precedes disk publication and must not poison I/O. */
 fixture_reset(&interface,0,0x70);wire_stage=reserve_calls=0;
 transfer_override=wire;caps=DRV_USB_HCD_CAP_TRANSFER_RESERVE;
 reserve_failure=0;logical_size=512;inject_ready_ua=1;
 CHECK(storage_attach(&interface,&storage_ids[0])==0);
 s=interface.driver_data;CHECK(s&&s->disk&&inject_ready_ua==0);
CHECK(s->media_state==STORAGE_ONLINE);
 memset(&bio,0,sizeof(bio));bio.b_op=BIO_READ;bio.b_block_count=1;bio.b_data=output;
 CHECK(storage_submit(s->disk,&bio)==0&&bio_error==0&&bio_bytes==512);
 /* Once published, an unexplained reset invalidates the existing media owner. */
 inject_read_ua=1;data_commands=0;
 CHECK(storage_submit(s->disk,&bio)==0&&bio_error==EIO&&data_commands==1);
CHECK(s->media_state==STORAGE_FAILED&&s->disk->d_media_revoked&&inject_read_ua==0);
 CHECK(storage_submit(s->disk,&bio)==0&&bio_error==EIO&&data_commands==1);
 CHECK(storage_detach(&interface,0)==0&&live_urbs==0&&live_allocations==0);
 /* Actual BOT REQUEST SENSE closes the published medium, not only the driver. */
 for(i=0;i<6;i++) {
  fixture_reset(&interface,0,0x70);wire_stage=reserve_calls=0;transfer_override=wire;
  CHECK(storage_attach(&interface,&storage_ids[0])==0);s=interface.driver_data;
  sense_key=i==2?2:6;sense_asc=i==0?0x28:(i==2||i==5)?0x3a:i==4?0x29:0x2a;
  sense_ascq=i==1?9:i==3?2:0;sense_code=i==4?0x71:0x70;
  inject_read_ua=1;data_commands=0;
  CHECK(storage_submit(s->disk,&bio)==0 && bio_error==EIO && data_commands==1);
  CHECK(s->media_state==(i<2?STORAGE_REVALIDATE:(i==2||i==5)?STORAGE_ABSENT:STORAGE_FAILED));
  CHECK(s->disk->d_media_revoked && s->last_sense.asc==sense_asc && s->last_sense.ascq==sense_ascq);
  CHECK(storage_submit(s->disk,&bio)==0 && bio_error==EIO && data_commands==1);
  CHECK(storage_detach(&interface,0)==0 && live_urbs==0 && live_allocations==0);
 }
 /* MODE refresh succeeds once, preserves old durability, and rebuilds WRITE flags. */
 for(i=0;i<5;i++) {
  fixture_reset(&interface,0,0x70);wire_stage=reserve_calls=0;transfer_override=wire;
  mode_payload=mode_wp=sync_fail=sync_sense=0;
  CHECK(storage_attach(&interface,&storage_ids[0])==0);s=interface.driver_data;
  sense_key=6;sense_asc=0x2a;sense_ascq=1;sense_code=0x70;
  mode_payload=1;mode_wp=i==1;sync_fail=i==2||i==3;
  if(i==3)s->flush_policy=DRV_USB_SCSI_FLUSH_WRITE_THROUGH;
  inject_read_ua=i==4?2:1;data_commands=0;
  bio.b_op=BIO_WRITE;bio.b_data=input;
  CHECK(storage_submit(s->disk,&bio)==0);
  if(i==0||i==3){CHECK(bio_error==0 && data_commands==2 && s->media_state==STORAGE_ONLINE && !s->disk->d_media_revoked);}
  if(i==1){CHECK(bio_error==EROFS && data_commands==1 && s->write_protected && (s->disk->d_flags&DISK_READ_ONLY));}
  if(i==2||i==4){CHECK(bio_error!=0 && s->media_state==STORAGE_FAILED && s->disk->d_media_revoked);}
  if(i==3)CHECK(last_write_flags&8);
  CHECK(storage_detach(&interface,0)==0 && live_urbs==0 && live_allocations==0);
 }
 /* Control readiness detects media change; busy old ownership prevents probing. */
 fixture_reset(&interface,0,0x70);wire_stage=reserve_calls=0;transfer_override=wire;
 mode_payload=mode_wp=sync_fail=sync_sense=0;
 CHECK(storage_attach(&interface,&storage_ids[0])==0);s=interface.driver_data;
 CHECK(storage_control_step(s)==0);i=s->disk->d_dev;
 sense_key=6;sense_asc=0x28;sense_ascq=0;sense_code=0x70;inject_ready_ua=1;
 CHECK(storage_control_step(s)!=0 && s->media_state==STORAGE_REVALIDATE && s->disk->d_media_revoked);
 retire_error=EBUSY;data_commands=wire_commands;s->flush_error=EIO;
 CHECK(storage_control_step(s)==EBUSY && s->disk->d_dev==i && s->flush_error==EIO && wire_commands==data_commands);
 retire_error=0;publish_error=EIO;
 CHECK(storage_control_step(s)==EIO && s->disk==NULL && live_urbs==3);
 publish_error=0;partition_reload_error=EBUSY;data_commands=partition_reload_calls;
 CHECK(storage_control_step(s)==EBUSY && s->media_state==STORAGE_ONLINE && s->disk->d_dev!=i);
 CHECK(s->partitions_pending && partition_reload_calls==data_commands+1);
 CHECK(s->disk->d_open_count==0);
 partition_open_error=EBUSY;
 CHECK(storage_control_step(s)==EBUSY && s->partitions_pending && s->disk->d_open_count==0);
 CHECK(partition_reload_calls==data_commands+1);
 partition_open_error=0;partition_reload_error=0;
 CHECK(storage_control_step(s)==0 && !s->partitions_pending && partition_reload_calls==data_commands+2);
 CHECK(s->flush_error==0 && live_urbs==3);
 device_disconnected=1;data_commands=wire_commands;
 CHECK(storage_control_step(s)==ENODEV && wire_commands==data_commands);device_disconnected=0;
 CHECK(storage_detach(&interface,0)==0 && live_urbs==0 && live_allocations==0);
 /* Worker allocation fails before publication; failed stop/join retain the owner. */
 fixture_reset(&interface,0,0x70);wire_stage=reserve_calls=0;transfer_override=wire;
 mode_payload=mode_wp=sync_fail=sync_sense=0;control_create_error=ENOMEM;
 CHECK(storage_attach(&interface,&storage_ids[0])==ENOMEM && disk_calls==0 && !control_live && live_urbs==0 && live_allocations==0);
 control_create_error=0;
 for(i=0;i<3;i++) {
  fixture_reset(&interface,0,0x70);wire_stage=reserve_calls=0;transfer_override=wire;
  publish_error=i==2?EIO:0;control_join_error=i==2?EBUSY:0;
  CHECK(storage_attach(&interface,&storage_ids[0])==0);s=interface.driver_data;
  if(i==2)CHECK(s->disk==NULL && s->media_state==STORAGE_FAILED);
  control_hold_stop=i==0;control_join_error=i==0?0:EBUSY;
  CHECK(storage_detach(&interface,0)==EBUSY && s->disk==NULL && live_urbs==3 && control_live);
  control_hold_stop=0;control_join_error=0;publish_error=0;
  CHECK(storage_detach(&interface,0)==0 && live_urbs==0 && live_allocations==0 && !control_live);
 }
 /* Execute the actual startup wait and one polling iteration at a controlled scheduler. */
 fixture_reset(&interface,0,0x70);wire_stage=reserve_calls=0;transfer_override=wire;
 CHECK(storage_attach(&interface,&storage_ids[0])==0);s=interface.driver_data;
 s->control_ready=0;control_run_loop=1;data_commands=wire_commands;
 control_thread.kernel_entry(control_thread.kernel_arg);control_run_loop=0;
 CHECK(s->control_ready && s->control_stopping && wire_commands==data_commands+1);
 CHECK(storage_detach(&interface,0)==0 && !control_live && live_urbs==0 && live_allocations==0);
 /* One owned BOT reset authorizes one current reset attention, never a loop. */
 for(i=0;i<3;i++) {
  fixture_reset(&interface,0,0x70);wire_stage=reserve_calls=0;transfer_override=wire;
  CHECK(storage_attach(&interface,&storage_ids[0])==0);s=interface.driver_data;
  sense_key=6;sense_asc=0x29;sense_ascq=0;sense_code=0x70;
  transport_faults=i==2?2:1;inject_read_ua=i==2?0:i+1;reset_commands=0;
  memset(&bio,0,sizeof(bio));bio.b_op=BIO_READ;bio.b_block_count=1;bio.b_data=output;
  CHECK(storage_submit(s->disk,&bio)==0 && reset_commands==1 && transport_faults==0 && inject_read_ua==0);
  if(i==0)CHECK(bio_error==0 && bio_bytes==512 && s->media_state==STORAGE_ONLINE && !s->disk->d_media_revoked);
  else CHECK(bio_error==EIO && bio_bytes==0);
  if(i==1)CHECK(s->media_state==STORAGE_FAILED && s->disk->d_media_revoked);
  CHECK(storage_detach(&interface,0)==0 && !control_live && live_urbs==0 && live_allocations==0);
 }
 puts("storage reserve PASS: attach/worker rollback, media/partition recovery, bounded self-reset, 64 KiB BOT data");return 0;
}
