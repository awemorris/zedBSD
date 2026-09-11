/* Link-only observation of media sense and retirement; no decision changes. */
#include <drivers/usb.h>
#include <kern/disk.h>
#include <kern/sched.h>
#include <hal/hal.h>
int __real_drv_usb_urb_wait_reusable(struct drv_usb_urb *urb);
int __real_disk_media_retire(struct disk *disk);
int __wrap_drv_usb_urb_wait_reusable(struct drv_usb_urb *urb)
{
 const unsigned char *bytes;
 int error = __real_drv_usb_urb_wait_reusable(urb);
 bytes = drv_usb_urb_buffer(urb);
 if (error == 0 && drv_usb_urb_length(urb) == 18 && bytes != NULL &&
     (bytes[0] == 0x70 || bytes[0] == 0x71))
  hal_printf("\nMEDIA PROBE sense tick=%u key=%u asc=%u ascq=%u\n",
      (unsigned)sched_ticks(), bytes[2]&15, bytes[12], bytes[13]);
 return error;
}
int __wrap_disk_media_retire(struct disk *disk)
{
 static unsigned calls;
 int error = __real_disk_media_retire(disk);
 if (calls++ < 10)
  hal_printf("\nMEDIA PROBE retire error=%d refs=%u buffers=%u open=%u cache=%u inflight=%u\n",
      error, refcount_load(&disk->d_refs), disk->d_buffer_refs,
      disk->d_open_count, disk->d_cache_users, disk->d_inflight);
 return error;
}
