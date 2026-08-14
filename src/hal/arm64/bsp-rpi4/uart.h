#ifndef ZEDBSD_HAL_ARM64_RPI4_UART_H
#define ZEDBSD_HAL_ARM64_RPI4_UART_H

void rpi4_uart_init(void);
void rpi4_uart_putc(int c);
int rpi4_uart_getc(void);
int rpi4_uart_poll(void);

#endif
