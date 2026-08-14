#include <hal/hal.h>
#include <kern/boot-device.h>
int boot_device_register(void){return 0;}
int kern_boot_pending(void){return 0;}
void kern_boot_execute_pending(void){HAL_FATAL("Pi 4 chain boot unavailable");for(;;);}
