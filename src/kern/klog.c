/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/klog.h"
#include "kern/lock.h"
#include "kern/platform.h"
#include <stdarg.h>
#include <string.h>

#define KLOG_CAPACITY (32U * 1024U)
static struct spinlock klog_lock;
static char klog_buffer[KLOG_CAPACITY];
static size_t klog_oldest,klog_used;
static uint64_t klog_dropped;

void kern_log_init(void){spin_init(&klog_lock,LOCK_RANK_KLOG,"kernel log");klog_oldest=klog_used=0;klog_dropped=0;}
static void append_locked(const char*bytes,size_t length)
{
	size_t i;if(length>=KLOG_CAPACITY){uint64_t lost=(uint64_t)klog_used+(uint64_t)(length-KLOG_CAPACITY);klog_dropped=UINT64_MAX-klog_dropped<lost?UINT64_MAX:klog_dropped+lost;bytes+=length-KLOG_CAPACITY;length=KLOG_CAPACITY;klog_oldest=klog_used=0;}else if(length>KLOG_CAPACITY-klog_used){size_t lost=length-(KLOG_CAPACITY-klog_used);klog_oldest=(klog_oldest+lost)%KLOG_CAPACITY;klog_used-=lost;klog_dropped=UINT64_MAX-klog_dropped<(uint64_t)lost?UINT64_MAX:klog_dropped+(uint64_t)lost;}for(i=0;i<length;i++)klog_buffer[(klog_oldest+klog_used+i)%KLOG_CAPACITY]=bytes[i];klog_used+=length;
}
void kern_log_write(const char*bytes,size_t length)
{
	unsigned long irq;if(!bytes||!length)return;irq=spin_lock_irqsave(&klog_lock);append_locked(bytes,length);spin_unlock_irqrestore(&klog_lock,irq);{char chunk[257];size_t at=0;while(at<length){size_t n=length-at;if(n>sizeof(chunk)-1U)n=sizeof(chunk)-1U;memcpy(chunk,bytes+at,n);chunk[n]=0;kern_platform_debug_write(chunk);at+=n;}}
}
static void emit_char(char*out,size_t cap,size_t*used,char c){if(*used<cap)out[*used]=c;(*used)++;}
static void emit_text(char*out,size_t cap,size_t*used,const char*s){if(!s)s="(null)";while(*s)emit_char(out,cap,used,*s++);}
static void emit_number(char*out,size_t cap,size_t*used,uint64_t value,unsigned base,unsigned width,int zero,int upper)
{
	char digits[32];unsigned count=0;const char*alphabet=upper?"0123456789ABCDEF":"0123456789abcdef";do{digits[count++]=alphabet[value%base];value/=base;}while(value&&count<sizeof(digits));while(width>count){emit_char(out,cap,used,zero?'0':' ');width--;}while(count)emit_char(out,cap,used,digits[--count]);
}
void kern_logf(const char*format,...)
{
	char text[512];size_t used=0;va_list args;if(!format)return;va_start(args,format);while(*format){unsigned width=0,long_count=0;int zero=0;char spec;if(*format!='%'){emit_char(text,sizeof(text)-1U,&used,*format++);continue;}format++;if(*format=='%'){emit_char(text,sizeof(text)-1U,&used,*format++);continue;}if(*format=='0'){zero=1;format++;}while(*format>='0'&&*format<='9'){width=width*10U+(unsigned)(*format-'0');format++;}while(*format=='l'){long_count++;format++;}spec=*format?*format++:0;if(spec=='s')emit_text(text,sizeof(text)-1U,&used,va_arg(args,const char*));else if(spec=='c')emit_char(text,sizeof(text)-1U,&used,(char)va_arg(args,int));else if(spec=='u'||spec=='x'||spec=='X'){uint64_t value=long_count>=2?va_arg(args,unsigned long long):long_count?va_arg(args,unsigned long):va_arg(args,unsigned);emit_number(text,sizeof(text)-1U,&used,value,spec=='u'?10U:16U,width,zero,spec=='X');}else if(spec=='d'||spec=='i'){int64_t signed_value=long_count>=2?va_arg(args,long long):long_count?va_arg(args,long):va_arg(args,int);uint64_t value=(uint64_t)signed_value;if(signed_value<0){emit_char(text,sizeof(text)-1U,&used,'-');value=0U-value;}emit_number(text,sizeof(text)-1U,&used,value,10U,width,zero,0);}else if(spec=='p'){uintptr_t value=(uintptr_t)va_arg(args,void*);emit_text(text,sizeof(text)-1U,&used,"0x");emit_number(text,sizeof(text)-1U,&used,value,16U,(unsigned)(sizeof(uintptr_t)*2U),1,0);}else{emit_char(text,sizeof(text)-1U,&used,'%');if(spec)emit_char(text,sizeof(text)-1U,&used,spec);}}
	va_end(args);if(used>=sizeof(text))used=sizeof(text)-1U;text[used]=0;kern_log_write(text,used);
}
size_t kern_log_snapshot(char*buffer,size_t capacity,uint64_t*dropped)
{
	unsigned long irq;size_t i,needed;irq=spin_lock_irqsave(&klog_lock);needed=klog_used;if(dropped)*dropped=klog_dropped;if(buffer&&capacity>=needed)for(i=0;i<needed;i++)buffer[i]=klog_buffer[(klog_oldest+i)%KLOG_CAPACITY];spin_unlock_irqrestore(&klog_lock,irq);return needed;
}
size_t kern_log_capacity(void){return KLOG_CAPACITY;}
