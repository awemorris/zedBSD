#include <hal/hal.h>
#include "../defs.h"
#include "uart.h"

#define MMIO32(pa) ((volatile uint32 *)(ARM64_DIRECT_BASE + (uintptr_t)(pa)))
#define GPIO_GPFSEL1 0x04
#define GPIO_PUP_PDN_CNTRL0 0xe4
#define UART_DR 0x00
#define UART_FR 0x18
#define UART_IBRD 0x24
#define UART_FBRD 0x28
#define UART_LCRH 0x2c
#define UART_CR 0x30
#define UART_IMSC 0x38
#define UART_ICR 0x44
#define UART_FR_RXFE (1U << 4)
#define UART_FR_TXFF (1U << 5)
#define UART_IRQ_RX  (1U << 4)
#define UART_IRQ_RT  (1U << 6)

static volatile uint32 *const gpio = MMIO32(RPI4_GPIO_BASE);
static volatile uint32 *const uart = MMIO32(RPI4_PL011_BASE);

void
rpi4_uart_init(void)
{
	uint32 value;

	uart[UART_CR / 4] = 0;
	value = gpio[GPIO_GPFSEL1 / 4];
	value &= ~((7U << 12) | (7U << 15));
	value |= (4U << 12) | (4U << 15);
	gpio[GPIO_GPFSEL1 / 4] = value;
	value = gpio[GPIO_PUP_PDN_CNTRL0 / 4];
	value &= ~((3U << 28) | (3U << 30));
	gpio[GPIO_PUP_PDN_CNTRL0 / 4] = value;
	uart[UART_ICR / 4] = 0x7ff;
	/* config.txt fixes the PL011 clock at the usual 48 MHz. */
	uart[UART_IBRD / 4] = 26;
	uart[UART_FBRD / 4] = 3;
	/* RX timeout IRQ handles a lone byte; the FIFO preserves burst input. */
	uart[UART_LCRH / 4] = (3U << 5) | (1U << 4);
	uart[UART_IMSC / 4] = 0;
	uart[UART_CR / 4] = (1U << 9) | (1U << 8) | 1U;
}

void
rpi4_uart_putc(int c)
{
	if (c == '\n')
		rpi4_uart_putc('\r');
	while ((uart[UART_FR / 4] & UART_FR_TXFF) != 0)
		;
	uart[UART_DR / 4] = (uint32)c;
}

int
rpi4_uart_poll(void)
{
	return (uart[UART_FR / 4] & UART_FR_RXFE) == 0;
}

int
rpi4_uart_getc(void)
{
	while (!rpi4_uart_poll())
		;
	return (int)(uart[UART_DR / 4] & 0xff);
}

void
rpi4_uart_enable_rx_irq(void)
{
	uart[UART_ICR / 4] = UART_IRQ_RX | UART_IRQ_RT;
	uart[UART_IMSC / 4] = UART_IRQ_RX | UART_IRQ_RT;
}

void
rpi4_uart_clear_rx_irq(void)
{
	uart[UART_ICR / 4] = UART_IRQ_RX | UART_IRQ_RT;
}
