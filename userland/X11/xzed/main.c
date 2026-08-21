/*
 * Xzed - small local X11 server for zedBSD
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Wire constants and layouts follow the public X11 core protocol.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <zedbsd/console.h>
#include <zedbsd/graphics.h>
#include <zedbsd/mouse.h>

#define MAX_CLIENTS 8
#define MAX_WINDOWS 64
#define INPUT_CAP (1024U * 1024U)
#define ROOT_XID 1U
#define COLORMAP_XID 2U
#define VISUAL_XID 3U

struct client {
	int fd, order, setup;
	uint16_t sequence;
	uint32_t base;
	uint8_t *input;
	size_t used, capacity;
};
struct window {
	uint32_t id, parent, owner, event_mask, background;
	int16_t x, y;
	uint16_t width, height, border;
	int mapped;
};
struct server {
	int listener, console, mouse, graphics;
	struct zedbsd_console_input_mode old_console_mode;
	struct zedbsd_graphics_mode mode;
	struct client clients[MAX_CLIENTS];
	struct window windows[MAX_WINDOWS];
	unsigned window_count;
	int pointer_x, pointer_y;
	uint32_t buttons, key_state;
	uint32_t focus;
};
static volatile int stopped;

static uint16_t rd16(const uint8_t *p, int msb)
{ return msb ? (uint16_t)((p[0] << 8) | p[1]) : (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p, int msb)
{ return msb ? ((uint32_t)p[0] << 24)|((uint32_t)p[1] << 16)|((uint32_t)p[2] << 8)|p[3] : (uint32_t)p[0]|((uint32_t)p[1] << 8)|((uint32_t)p[2] << 16)|((uint32_t)p[3] << 24); }
static void wr16(uint8_t *p, uint16_t v, int msb)
{ if(msb){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}else{p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);} }
static void wr32(uint8_t *p, uint32_t v, int msb)
{ if(msb){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}else{p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);} }
static int write_all(int fd,const void *v,size_t n)
{
	const uint8_t *p=v; while(n){ssize_t r=write(fd,p,n);if(r<0){if(errno==EINTR)continue;return -1;}if(!r){errno=EIO;return -1;}p+=r;n-=(size_t)r;}return 0;
}
static void on_signal(int sig){(void)sig;stopped=1;}

static struct window *find_window(struct server *s,uint32_t id)
{ unsigned i;for(i=0;i<s->window_count;i++)if(s->windows[i].id==id)return &s->windows[i];return NULL; }
static struct client *owner_client(struct server *s,uint32_t owner)
{ return owner<MAX_CLIENTS && s->clients[owner].fd>=0?&s->clients[owner]:NULL; }

static int flush_rect(struct server *s,uint32_t x,uint32_t y,uint32_t w,uint32_t h)
{
	struct zedbsd_graphics_rect r={x,y,w,h};
	struct zedbsd_graphics_flush f={(uintptr_t)&r,1};
	return ioctl(s->graphics,ZEDBSD_GRAPHICS_FLUSH,&f);
}
static void fill(struct server *s,int x,int y,int w,int h,uint32_t color)
{
	struct zedbsd_graphics_fill f;
	if(x<0){w+=x;x=0;}if(y<0){h+=y;y=0;}
	if(x+w>(int)s->mode.width)w=(int)s->mode.width-x;
	if(y+h>(int)s->mode.height)h=(int)s->mode.height-y;
	if(w<=0||h<=0)return;
	memset(&f,0,sizeof(f));f.rect.x=(uint32_t)x;f.rect.y=(uint32_t)y;
	f.rect.width=(uint32_t)w;f.rect.height=(uint32_t)h;f.color=color;
	(void)ioctl(s->graphics,ZEDBSD_GRAPHICS_FILL_RECT,&f);
}
static void repaint(struct server *s)
{
	unsigned i;fill(s,0,0,(int)s->mode.width,(int)s->mode.height,s->windows[0].background);
	for(i=1;i<s->window_count;i++){struct window *w=&s->windows[i];if(w->mapped)fill(s,w->x,w->y,w->width,w->height,w->background);}
	/* Fixed server cursor; it is presentation-only. */
	fill(s,s->pointer_x,s->pointer_y,2,12,0xffffff);
	fill(s,s->pointer_x,s->pointer_y,8,2,0xffffff);
	(void)flush_rect(s,0,0,s->mode.width,s->mode.height);
}

static void send_event(struct client *c,uint8_t type,uint32_t window,uint32_t detail,uint32_t time,int16_t rx,int16_t ry,uint16_t state)
{
	uint8_t e[32];memset(e,0,sizeof(e));e[0]=type;e[1]=(uint8_t)detail;wr16(e+2,c->sequence,c->order);wr32(e+4,time,c->order);
	wr32(e+8,ROOT_XID,c->order);wr32(e+12,window,c->order);wr32(e+16,0,c->order);
	wr16(e+20,(uint16_t)rx,c->order);wr16(e+22,(uint16_t)ry,c->order);wr16(e+24,(uint16_t)rx,c->order);wr16(e+26,(uint16_t)ry,c->order);wr16(e+28,state,c->order);e[30]=1;
	(void)write_all(c->fd,e,sizeof(e));
}
static void expose(struct server *s,struct window *w)
{
	struct client *c=owner_client(s,w->owner);uint8_t e[32];if(!c||!(w->event_mask&(1U<<15)))return;
	memset(e,0,sizeof(e));e[0]=12;wr16(e+2,c->sequence,c->order);wr32(e+4,w->id,c->order);wr16(e+12,w->width,c->order);wr16(e+14,w->height,c->order);(void)write_all(c->fd,e,32);
}
static struct window *hit(struct server *s,int x,int y)
{
	unsigned i=s->window_count;while(i-->1){struct window *w=&s->windows[i];if(w->mapped&&x>=w->x&&y>=w->y&&x<w->x+w->width&&y<w->y+w->height)return w;}return &s->windows[0];
}

static int setup_reply(struct server *s,struct client *c)
{
	static const char vendor[]="zedBSD Xzed";uint8_t out[8+32+12+8+40+8+24];uint8_t *p=out+8;uint16_t units;
	memset(out,0,sizeof(out));out[0]=1;wr16(out+2,11,c->order);wr16(out+4,0,c->order);units=(uint16_t)((sizeof(out)-8)/4);wr16(out+6,units,c->order);
	wr32(p,1,c->order);wr32(p+4,c->base,c->order);wr32(p+8,0x003fffff,c->order);wr16(p+16,sizeof(vendor)-1,c->order);wr16(p+18,65535,c->order);p[20]=1;p[21]=1;p[22]=c->order?1:0;p[23]=1;p[24]=32;p[25]=32;p[26]=8;p[27]=255;p+=32;
	memcpy(p,vendor,sizeof(vendor)-1);p+=12;p[0]=24;p[1]=32;p[2]=32;p+=8;
	wr32(p,ROOT_XID,c->order);wr32(p+4,COLORMAP_XID,c->order);wr32(p+8,0xffffff,c->order);wr32(p+12,0,c->order);wr32(p+16,0x00ffffff,c->order);wr16(p+20,(uint16_t)s->mode.width,c->order);wr16(p+22,(uint16_t)s->mode.height,c->order);wr16(p+24,(uint16_t)(s->mode.width*254/96/10),c->order);wr16(p+26,(uint16_t)(s->mode.height*254/96/10),c->order);wr16(p+28,1,c->order);wr16(p+30,1,c->order);wr32(p+32,VISUAL_XID,c->order);p[36]=0;p[38]=24;p[39]=1;p+=40;
	p[0]=24;wr16(p+2,1,c->order);p+=8;wr32(p,VISUAL_XID,c->order);p[4]=4;p[5]=8;wr16(p+6,256,c->order);wr32(p+8,0x00ff0000,c->order);wr32(p+12,0x0000ff00,c->order);wr32(p+16,0x000000ff,c->order);
	return write_all(c->fd,out,sizeof(out));
}
static void error_reply(struct client *c,uint8_t code,uint32_t resource,uint8_t opcode)
{uint8_t e[32];memset(e,0,32);e[0]=0;e[1]=code;wr16(e+2,c->sequence,c->order);wr32(e+4,resource,c->order);e[10]=opcode;(void)write_all(c->fd,e,32);}
static void simple_reply(struct client *c,uint8_t *r,size_t n)
{r[0]=1;wr16(r+2,c->sequence,c->order);if(n>=32)wr32(r+4,(uint32_t)((n-32)/4),c->order);(void)write_all(c->fd,r,n);}

static int request(struct server *s,unsigned ci,const uint8_t *q,size_t n)
{
	struct client *c=&s->clients[ci];uint8_t op=q[0];uint32_t id;struct window *w;c->sequence++;
	switch(op){
	case 1: /* CreateWindow */
		if(n<32||s->window_count==MAX_WINDOWS)break;
		id=rd32(q+4,c->order);if(find_window(s,id)){error_reply(c,14,id,op);return 0;}
		w=&s->windows[s->window_count++];memset(w,0,sizeof(*w));w->id=id;w->owner=ci;w->parent=rd32(q+8,c->order);w->x=(int16_t)rd16(q+12,c->order);w->y=(int16_t)rd16(q+14,c->order);w->width=rd16(q+16,c->order);w->height=rd16(q+18,c->order);w->border=rd16(q+20,c->order);w->background=0x607080;
		{uint32_t mask=rd32(q+28,c->order);size_t off=32;unsigned bit;for(bit=0;bit<32&&off+4<=n;bit++)if(mask&(1U<<bit)){uint32_t v=rd32(q+off,c->order);if(bit==1)w->background=v;if(bit==11)w->event_mask=v;off+=4;}}
		return 0;
	case 2: /* ChangeWindowAttributes */
		if(n<12||(w=find_window(s,rd32(q+4,c->order)))==NULL)break;
		{uint32_t mask=rd32(q+8,c->order);size_t off=12;unsigned bit;for(bit=0;bit<32&&off+4<=n;bit++)if(mask&(1U<<bit)){uint32_t v=rd32(q+off,c->order);if(bit==1)w->background=v;if(bit==11)w->event_mask=v;off+=4;}}return 0;
	case 4: /* DestroyWindow */ if((w=find_window(s,rd32(q+4,c->order)))!=NULL&&w!=&s->windows[0]){w->mapped=0;repaint(s);}return 0;
	case 8: /* MapWindow */ if((w=find_window(s,rd32(q+4,c->order)))!=NULL){w->mapped=1;repaint(s);expose(s,w);return 0;}break;
	case 10: /* UnmapWindow */ if((w=find_window(s,rd32(q+4,c->order)))!=NULL){w->mapped=0;repaint(s);return 0;}break;
	case 14: /* GetGeometry */
		if((w=find_window(s,rd32(q+4,c->order)))!=NULL){uint8_t r[32];memset(r,0,32);r[1]=24;wr32(r+8,ROOT_XID,c->order);wr16(r+12,(uint16_t)w->x,c->order);wr16(r+14,(uint16_t)w->y,c->order);wr16(r+16,w->width,c->order);wr16(r+18,w->height,c->order);wr16(r+20,w->border,c->order);simple_reply(c,r,32);return 0;}break;
	case 38: /* QueryPointer */
		{uint8_t r[32];struct window *h=hit(s,s->pointer_x,s->pointer_y);memset(r,0,32);r[1]=1;wr32(r+8,ROOT_XID,c->order);wr32(r+12,h->id==ROOT_XID?0:h->id,c->order);wr16(r+16,(uint16_t)s->pointer_x,c->order);wr16(r+18,(uint16_t)s->pointer_y,c->order);wr16(r+20,(uint16_t)(s->pointer_x-h->x),c->order);wr16(r+22,(uint16_t)(s->pointer_y-h->y),c->order);wr16(r+24,(uint16_t)s->key_state,c->order);simple_reply(c,r,32);return 0;}
	case 42: s->focus=rd32(q+4,c->order);return 0;
	case 43: {uint8_t r[32];memset(r,0,32);r[1]=0;wr32(r+8,s->focus,c->order);simple_reply(c,r,32);return 0;}
	case 55: case 56: case 60: return 0; /* GC bookkeeping is not yet visible. */
	case 65: /* PolyLine */
		if(n>=16){struct zedbsd_graphics_line l;size_t off;int x=0,y=0;memset(&l,0,sizeof(l));for(off=12;off+4<=n;off+=4){int nx=(int16_t)rd16(q+off,c->order),ny=(int16_t)rd16(q+off+2,c->order);if(off!=12){l.x0=(uint32_t)x;l.y0=(uint32_t)y;l.x1=(uint32_t)nx;l.y1=(uint32_t)ny;l.color=0xffffff;(void)ioctl(s->graphics,ZEDBSD_GRAPHICS_DRAW_LINE,&l);}x=nx;y=ny;}(void)flush_rect(s,0,0,s->mode.width,s->mode.height);}return 0;
	case 70: /* PolyFillRectangle */
		if(n>=12&&(w=find_window(s,rd32(q+4,c->order)))!=NULL){size_t off;for(off=12;off+8<=n;off+=8)fill(s,w->x+(int16_t)rd16(q+off,c->order),w->y+(int16_t)rd16(q+off+2,c->order),rd16(q+off+4,c->order),rd16(q+off+6,c->order),0xffffff);(void)flush_rect(s,0,0,s->mode.width,s->mode.height);}return 0;
	case 101: /* GetKeyboardMapping */ {uint8_t r[32+4*248];unsigned i;memset(r,0,sizeof(r));r[1]=1;for(i=0;i<q[5]&&i<248;i++){uint32_t ks=(uint32_t)(q[4]+i);wr32(r+32+i*4,ks,c->order);}simple_reply(c,r,32+(size_t)q[5]*4);return 0;}
	case 117: {uint8_t r[32+4];memset(r,0,sizeof(r));r[1]=3;r[32]=1;r[33]=2;r[34]=3;simple_reply(c,r,36);return 0;}
	case 127:return 0;
	default:error_reply(c,1,0,op);return 0;
	}
	error_reply(c,3,n>=8?rd32(q+4,c->order):0,op);return 0;
}

static void close_client(struct server *s,unsigned i)
{if(s->clients[i].fd>=0)close(s->clients[i].fd);free(s->clients[i].input);memset(&s->clients[i],0,sizeof(s->clients[i]));s->clients[i].fd=-1;}
static void read_client(struct server *s,unsigned i)
{
	struct client *c=&s->clients[i];uint8_t temp[4096];ssize_t nr;
	while((nr=read(c->fd,temp,sizeof(temp)))>0){if(c->used+(size_t)nr>INPUT_CAP){close_client(s,i);return;}if(c->used+(size_t)nr>c->capacity){size_t z=c->capacity?c->capacity*2:4096;while(z<c->used+(size_t)nr)z*=2;c->input=realloc(c->input,z);if(!c->input){close_client(s,i);return;}c->capacity=z;}memcpy(c->input+c->used,temp,(size_t)nr);c->used+=(size_t)nr;}
	/* zedBSD nonblocking local sockets may report an empty read while the
	 * connection is still live.  POLLHUP owns peer teardown; zero here only
	 * means that no complete input is available in this drain pass. */
	if(nr<0)return;
	for(;;){size_t need;if(!c->setup){uint16_t authn,authd;if(c->used<12)return;if(c->input[0]!='l'&&c->input[0]!='B'){close_client(s,i);return;}c->order=c->input[0]=='B';authn=rd16(c->input+6,c->order);authd=rd16(c->input+8,c->order);need=12+((authn+3)&~3U)+((authd+3)&~3U);if(c->used<need)return;if(setup_reply(s,c)){close_client(s,i);return;}c->setup=1;}else{uint16_t units;if(c->used<4)return;units=rd16(c->input+2,c->order);if(!units){close_client(s,i);return;}need=(size_t)units*4;if(c->used<need)return;(void)request(s,i,c->input,need);}memmove(c->input,c->input+need,c->used-need);c->used-=need;}
}

static uint32_t keycode(uint32_t key){return key<248?key+8:0;}
static void keyboard(struct server *s)
{
	struct zedbsd_console_input_event ev[16];ssize_t n;while((n=read(s->console,ev,sizeof(ev)))>0){size_t i;if((size_t)n%sizeof(ev[0])){stopped=1;return;}for(i=0;i<(size_t)n/sizeof(ev[0]);i++){struct window *w=find_window(s,s->focus);struct client *c;uint32_t kc;if(!w)w=hit(s,s->pointer_x,s->pointer_y);c=owner_client(s,w->owner);kc=keycode(ev[i].key);if(!c||!kc)continue;if(ev[i].state==ZEDBSD_CONSOLE_KEY_PRESS){s->key_state|=ev[i].modifiers;send_event(c,2,w->id,kc,(uint32_t)(ev[i].timestamp_ns/1000000),s->pointer_x,s->pointer_y,(uint16_t)s->key_state);}else if(ev[i].state==ZEDBSD_CONSOLE_KEY_RELEASE){send_event(c,3,w->id,kc,(uint32_t)(ev[i].timestamp_ns/1000000),s->pointer_x,s->pointer_y,(uint16_t)s->key_state);s->key_state=ev[i].modifiers;}else{send_event(c,3,w->id,kc,(uint32_t)(ev[i].timestamp_ns/1000000),s->pointer_x,s->pointer_y,(uint16_t)s->key_state);send_event(c,2,w->id,kc,(uint32_t)(ev[i].timestamp_ns/1000000),s->pointer_x,s->pointer_y,(uint16_t)s->key_state);}}}if(n<0&&errno!=EAGAIN&&errno!=EINTR)stopped=1;
}
static void mouse(struct server *s)
{
	struct zedbsd_mouse_event ev[16];ssize_t n;while((n=read(s->mouse,ev,sizeof(ev)))>0){size_t i;if((size_t)n%sizeof(ev[0])){stopped=1;return;}for(i=0;i<(size_t)n/sizeof(ev[0]);i++){uint32_t changed=s->buttons^ev[i].buttons;struct window *w;struct client *c;unsigned b;s->pointer_x+=ev[i].dx;s->pointer_y+=ev[i].dy;if(s->pointer_x<0)s->pointer_x=0;if(s->pointer_y<0)s->pointer_y=0;if(s->pointer_x>=(int)s->mode.width)s->pointer_x=(int)s->mode.width-1;if(s->pointer_y>=(int)s->mode.height)s->pointer_y=(int)s->mode.height-1;w=hit(s,s->pointer_x,s->pointer_y);c=owner_client(s,w->owner);if(c&&w->event_mask&(1U<<6))send_event(c,6,w->id,0,(uint32_t)(ev[i].timestamp_ns/1000000),s->pointer_x,s->pointer_y,(uint16_t)s->buttons);for(b=0;b<3;b++)if(changed&(1U<<b)){if(c)send_event(c,(ev[i].buttons&(1U<<b))?4:5,w->id,b+1,(uint32_t)(ev[i].timestamp_ns/1000000),s->pointer_x,s->pointer_y,(uint16_t)s->buttons);}s->buttons=ev[i].buttons;repaint(s);}}if(n<0&&errno!=EAGAIN&&errno!=EINTR)stopped=1;
}

static int initialize(struct server *s)
{
	struct sockaddr_un a;struct zedbsd_graphics_caps caps;struct zedbsd_console_input_mode m={ZEDBSD_CONSOLE_INPUT_EVENT,0};unsigned i;memset(s,0,sizeof(*s));s->listener=s->console=s->mouse=s->graphics=-1;for(i=0;i<MAX_CLIENTS;i++)s->clients[i].fd=-1;
	s->console=open("/dev/console",O_RDONLY|O_NONBLOCK);if(s->console<0)return -1;if(ioctl(s->console,ZEDBSD_CONSOLE_GET_INPUT_MODE,&s->old_console_mode)||ioctl(s->console,ZEDBSD_CONSOLE_SET_INPUT_MODE,&m))return -1;
	s->mouse=open("/dev/mouse",O_RDONLY|O_NONBLOCK);
	s->graphics=open("/dev/graphics",O_RDWR);if(s->graphics<0)return -1;if(ioctl(s->graphics,ZEDBSD_GRAPHICS_GET_CAPS,&caps))return -1;if(!(caps.capabilities&ZEDBSD_GRAPHICS_CAP_FILL)||!(caps.capabilities&ZEDBSD_GRAPHICS_CAP_FLUSH)){errno=ENOTSUP;return -1;}memset(&s->mode,0,sizeof(s->mode));s->mode.preferred_bits_per_pixel=24;if(ioctl(s->graphics,ZEDBSD_GRAPHICS_ENTER,&s->mode))return -1;
	(void)mkdir("/tmp/.X11-unix",0777);(void)unlink("/tmp/.X11-unix/X0");s->listener=socket(AF_UNIX,SOCK_STREAM,0);if(s->listener<0)return -1;memset(&a,0,sizeof(a));a.sun_family=AF_UNIX;strcpy(a.sun_path,"/tmp/.X11-unix/X0");if(bind(s->listener,(struct sockaddr*)&a,sizeof(a))||listen(s->listener,8))return -1;(void)fcntl(s->listener,F_SETFL,fcntl(s->listener,F_GETFL)|O_NONBLOCK);
	s->windows[0]=(struct window){ROOT_XID,0,0,0,0x203040,0,0,(uint16_t)s->mode.width,(uint16_t)s->mode.height,0,1};s->window_count=1;s->focus=ROOT_XID;s->pointer_x=(int)s->mode.width/2;s->pointer_y=(int)s->mode.height/2;repaint(s);return 0;
}
static void cleanup(struct server *s)
{
	unsigned i;for(i=0;i<MAX_CLIENTS;i++)close_client(s,i);if(s->listener>=0)close(s->listener);(void)unlink("/tmp/.X11-unix/X0");if(s->mouse>=0)close(s->mouse);if(s->console>=0){(void)ioctl(s->console,ZEDBSD_CONSOLE_SET_INPUT_MODE,&s->old_console_mode);close(s->console);}if(s->graphics>=0)close(s->graphics);
}
int main(int argc,char **argv)
{
	struct server s;struct pollfd p[3+MAX_CLIENTS];unsigned i,count;int arg=1;extern char **environ;
	if(arg<argc&&strcmp(argv[arg],":0")==0)arg++;
	if(arg<argc&&strcmp(argv[arg],"--")==0)arg++;
	else if(arg<argc){fprintf(stderr,"usage: Xzed [:0] [-- command [argument ...]]\n");return 2;}
	signal(SIGINT,on_signal);signal(SIGTERM,on_signal);if(initialize(&s)){fprintf(stderr,"Xzed: %s\n",strerror(errno));cleanup(&s);return 1;}
	if(arg<argc){pid_t pid=fork();if(pid<0){fprintf(stderr,"Xzed: fork: %s\n",strerror(errno));cleanup(&s);return 1;}if(pid==0){execve(argv[arg],&argv[arg],environ);fprintf(stderr,"Xzed: %s: %s\n",argv[arg],strerror(errno));_exit(127);}}
	while(!stopped){count=0;p[count++]=(struct pollfd){s.listener,POLLIN,0};p[count++]=(struct pollfd){s.console,POLLIN,0};if(s.mouse>=0)p[count++]=(struct pollfd){s.mouse,POLLIN,0};for(i=0;i<MAX_CLIENTS;i++)if(s.clients[i].fd>=0)p[count++]=(struct pollfd){s.clients[i].fd,POLLIN,0};if(poll(p,count,20)<0){if(errno==EINTR)continue;break;}count=0;if(p[count++].revents&POLLIN){int fd=accept(s.listener,NULL,NULL);if(fd>=0){for(i=0;i<MAX_CLIENTS&&s.clients[i].fd>=0;i++);if(i==MAX_CLIENTS)close(fd);else{s.clients[i].fd=fd;s.clients[i].base=(i+1U)<<22;(void)fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK);}}}if(p[count++].revents&POLLIN)keyboard(&s);if(s.mouse>=0&&p[count++].revents&POLLIN)mouse(&s);for(i=0;i<MAX_CLIENTS;i++)if(s.clients[i].fd>=0)read_client(&s,i);}
	cleanup(&s);return 0;
}
