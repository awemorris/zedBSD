#ifndef ZEDBSD_HAL_AMD64_PIC_H
#define ZEDBSD_HAL_AMD64_PIC_H
void pic_init(void);
void pic_set_irq_mask(int irq_num, int mask);
int pic_get_irq_in_service(void);
void pic_send_eoi(int irq_num);
#endif
