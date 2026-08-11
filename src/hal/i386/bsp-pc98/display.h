/* Native PC-98 GDC display switching.  SPDX-License-Identifier: Zlib */
#ifndef BOOTS_PLATFORM_PC98_DISPLAY_H
#define BOOTS_PLATFORM_PC98_DISPLAY_H
int boots_pc98_display_graphics_start(void);
int boots_pc98_display_graphics_stop(void);
int boots_pc98_display_text_restore(void);
#endif
