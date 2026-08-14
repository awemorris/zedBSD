#include <hal/hal.h>
static int framebuffer_active;
void fb_set_active(int active){framebuffer_active=active!=0;}
int fb_is_active(void){return framebuffer_active;}
