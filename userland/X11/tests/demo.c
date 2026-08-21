/* Xlib acceptance client for Xzed. SPDX-License-Identifier: Zlib */
#include <X11/Xlib.h>
#include <stdio.h>
int main(void){Display*d=XOpenDisplay(0);Window w;GC gc;XEvent e;if(!d){fprintf(stderr,"xzed-demo: cannot open display\n");return 1;}w=XCreateSimpleWindow(d,DefaultRootWindow(d),80,70,400,240,1,WhitePixel(d,0),0x406080);gc=XCreateGC(d,w,0,0);XSelectInput(d,w,ExposureMask|KeyPressMask|ButtonPressMask|PointerMotionMask);XMapWindow(d,w);for(;;){if(XNextEvent(d,&e)<0)break;if(e.type==Expose){int i;XSetForeground(d,gc,WhitePixel(d,0));for(i=0;i<5;i++)XFillRectangle(d,w,gc,20+i*70,40,50,150);XDrawLine(d,w,gc,10,10,390,220);}}XFreeGC(d,gc);XDestroyWindow(d,w);XCloseDisplay(d);return 0;}
