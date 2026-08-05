/* NEC PC-98 polled keyboard driver.  SPDX-License-Identifier: Zlib */
#ifndef BOOTS_DRIVERS_KBD_PC98_H
#define BOOTS_DRIVERS_KBD_PC98_H

#define BOOTS_KBD_EVENT_KEY_MASK 0x000001ffU
#define BOOTS_KBD_EVENT_SHIFT    0x00010000U
#define BOOTS_KBD_EVENT_CTRL     0x00020000U
#define BOOTS_KBD_EVENT_GRAPH    0x00040000U

int boots_kbd_pc98_read(void);
int boots_kbd_pc98_poll(void);
int boots_kbd_pc98_state(int key);
void boots_kbd_pc98_drain(void);
unsigned boots_kbd_pc98_modifiers(void);
int boots_kbd_pc98_read_event(void);
int boots_kbd_pc98_poll_event(void);

#endif
