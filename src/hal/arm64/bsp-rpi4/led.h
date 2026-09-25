#ifndef KERN_HAL_ARM64_RPI4_LED_H
#define KERN_HAL_ARM64_RPI4_LED_H
void rpi4_led_init(void);
void rpi4_led_set(int on);
void rpi4_led_stage(unsigned stage);
void rpi4_delay_ms(unsigned milliseconds);
#endif
