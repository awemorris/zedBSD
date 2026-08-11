/* Native PC-98 GDC display switching.  SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_PLATFORM_PC98_DISPLAY_H
#define ZEDBSD_PLATFORM_PC98_DISPLAY_H
int zedbsd_pc98_display_graphics_start(void);
int zedbsd_pc98_display_graphics_stop(void);
int zedbsd_pc98_display_text_restore(void);
#endif
