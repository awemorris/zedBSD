#ifndef _X11_XLIB_H_
#define _X11_XLIB_H_
#include <X11/X.h>
typedef struct _XDisplay Display;
typedef struct _XGC *GC;
typedef struct { short lbearing,rbearing,width,ascent,descent; unsigned short attributes; } XCharStruct;
typedef struct { Font fid; unsigned direction,min_char_or_byte2,max_char_or_byte2,min_byte1,max_byte1; Bool all_chars_exist; unsigned default_char; int n_properties; void *properties; XCharStruct min_bounds,max_bounds; void *per_char; int ascent,descent; } XFontStruct;
typedef struct { unsigned char byte1,byte2; } XChar2b;
typedef struct { short x,y; unsigned short width,height; } XRectangle;
typedef struct { int type; unsigned long serial; Bool send_event; Display *display; Window window; } XAnyEvent;
typedef struct { int type; unsigned long serial; Bool send_event; Display *display; Window window; int x,y,width,height,count; } XExposeEvent;
typedef struct { int type; unsigned long serial; Bool send_event; Display *display; Window parent,window; } XMapRequestEvent;
typedef struct { int type; unsigned long serial; Bool send_event; Display *display; Window window,root,subwindow; Time time; int x,y,x_root,y_root; unsigned int state,keycode; Bool same_screen; } XKeyEvent;
typedef XKeyEvent XButtonEvent;
typedef XKeyEvent XMotionEvent;
typedef union _XEvent { int type; XAnyEvent xany; XExposeEvent xexpose; XKeyEvent xkey; XButtonEvent xbutton; XMotionEvent xmotion; XMapRequestEvent xmaprequest; long pad[24]; } XEvent;
Display *XOpenDisplay(const char *);
int XCloseDisplay(Display *);
int XDefaultScreen(Display *);
Window XRootWindow(Display *,int);
unsigned long XBlackPixel(Display *,int);
unsigned long XWhitePixel(Display *,int);
Window XCreateSimpleWindow(Display *,Window,int,int,unsigned int,unsigned int,unsigned int,unsigned long,unsigned long);
int XSelectInput(Display *,Window,long);
int XMapWindow(Display *,Window);
int XUnmapWindow(Display *,Window);
int XDestroyWindow(Display *,Window);
int XReparentWindow(Display *,Window,Window,int,int);
int XMoveResizeWindow(Display *,Window,int,int,unsigned int,unsigned int);
int XGetGeometry(Display *,Drawable,Window *,int *,int *,unsigned int *,unsigned int *,unsigned int *,unsigned int *);
GC XCreateGC(Display *,Drawable,unsigned long,void *);
int XFreeGC(Display *,GC);
int XSetForeground(Display *,GC,unsigned long);
int XSetFont(Display *,GC,Font);
Font XLoadFont(Display *,const char *);
XFontStruct *XLoadQueryFont(Display *,const char *);
int XFreeFont(Display *,XFontStruct *);
char **XListFonts(Display *,const char *,int,int *);
int XFreeFontNames(char **);
int XFillRectangle(Display *,Drawable,GC,int,int,unsigned int,unsigned int);
int XFillRectangles(Display *,Drawable,GC,XRectangle *,int);
int XDrawLine(Display *,Drawable,GC,int,int,int,int);
int XDrawString(Display *,Drawable,GC,int,int,const char *,int);
int XDrawString16(Display *,Drawable,GC,int,int,const XChar2b *,int);
int XNextEvent(Display *,XEvent *);
int XPending(Display *);
int XFlush(Display *);
int XSync(Display *,Bool);
#define DefaultScreen(d) XDefaultScreen(d)
#define RootWindow(d,s) XRootWindow((d),(s))
#define DefaultRootWindow(d) XRootWindow((d),XDefaultScreen(d))
#define BlackPixel(d,s) XBlackPixel((d),(s))
#define WhitePixel(d,s) XWhitePixel((d),(s))
#endif
