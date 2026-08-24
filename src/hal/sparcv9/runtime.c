/* Freestanding C runtime and diagnostics for SPARC V9. */
#include <hal/hal.h>

int hal_strlen(const char*s){int n=0;while(s[n])n++;return n;}
void *hal_memset(void*p,int c,size_t n){uint8_t*d=p;while(n--)*d++=(uint8_t)c;return p;}
void *hal_memset16(uint16_t*p,uint16_t c,size_t n){uint16_t*d=p;while(n--)*d++=c;return p;}
void *hal_memset32(uint32_t*p,uint32_t c,size_t n){uint32_t*d=p;while(n--)*d++=c;return p;}
void *hal_memcpy(void*d0,const void*s0,size_t n){uint8_t*d=d0;const uint8_t*s=s0;while(n--)*d++=*s++;return d0;}
static void *(*allocator)(size_t);static void(*deallocator)(void*);
void hal_set_allocator(void*(*a)(size_t),void(*f)(void*))
{if(!a||!f||allocator||deallocator)HAL_FATAL("hal_set_allocator must be called exactly once");allocator=a;deallocator=f;}
void *hal_malloc(size_t n){if(!allocator)HAL_FATAL("allocator unset");return allocator(n);}
void hal_free(void*p){if(!deallocator)HAL_FATAL("allocator unset");deallocator(p);}
int hal_putchar(int c){hal_cons_putc(c);return c;}int hal_puts(const char*s){hal_cons_write(s);return 0;}

static uint64_t divide(uint64_t value,unsigned base,uint64_t *remainder)
{
	uint64_t quotient=0,rem=0;int bit;
	for(bit=63;bit>=0;bit--){rem=(rem<<1)|((value>>(unsigned)bit)&1U);if(rem>=base){rem-=base;quotient|=1ULL<<(unsigned)bit;}}
	*remainder=rem;return quotient;
}
static void put_number(uint64_t value,unsigned base,int width)
{
	char b[24];int n=0;do{uint64_t rem;value=divide(value,base,&rem);b[n++]=(char)(rem<10?'0'+rem:'a'+rem-10);}while(value);
	while(width-->n)hal_cons_putc('0');
	while(n)hal_cons_putc(b[--n]);
}
int hal_printf(const char*fmt,...)
{
	__builtin_va_list ap;__builtin_va_start(ap,fmt);
	while(*fmt){int width=0,long_arg=0;if(*fmt!='%'){hal_cons_putc(*fmt++);continue;}fmt++;if(*fmt=='0')fmt++;
		while(*fmt>='0'&&*fmt<='9')width=width*10+(*fmt++-'0');
		if(*fmt=='l'){long_arg=1;fmt++;if(*fmt=='l')fmt++;}
		switch(*fmt++){
		case 'c':hal_cons_putc(__builtin_va_arg(ap,int));break;
		case 's':{const char*s=__builtin_va_arg(ap,const char*);hal_cons_write(s?s:"(null)");break;}
		case 'u':put_number(long_arg?__builtin_va_arg(ap,uint64_t):__builtin_va_arg(ap,uint32_t),10,width);break;
		case 'x':put_number(long_arg?__builtin_va_arg(ap,uint64_t):__builtin_va_arg(ap,uint32_t),16,width);break;
		case 'd':{int64_t v=long_arg?__builtin_va_arg(ap,int64_t):__builtin_va_arg(ap,int32_t);uint64_t u;if(v<0){hal_cons_putc('-');u=(uint64_t)(-(v+1))+1;}else u=(uint64_t)v;put_number(u,10,width);break;}
		case 'p':put_number((uintptr_t)__builtin_va_arg(ap,void*),16,16);break;
		case '%':hal_cons_putc('%');break;default:hal_cons_putc('?');break;}
	}
	__builtin_va_end(ap);return 0;
}
void hal_assert(const char*f,int l,const char*e){hal_printf("assert: %s:%u: %s\n",f,(uint32_t)l,e);(void)hal_irq_disable();for(;;)__asm__ volatile("nop");}
void hal_fatal(const char*f,int l,const char*s){hal_printf("fatal: %s:%u: %s\n",f,(uint32_t)l,s);(void)hal_irq_disable();for(;;)__asm__ volatile("nop");}
void hal_mb(void){__asm__ volatile("membar #Sync":::"memory");}
void hal_rmb(void){__asm__ volatile("membar #LoadLoad | #LoadStore":::"memory");}
void hal_wmb(void){__asm__ volatile("membar #StoreStore | #LoadStore":::"memory");}
void hal_io_mb(void){hal_mb();}void hal_io_rmb(void){hal_rmb();}void hal_io_wmb(void){hal_wmb();}
void hal_icache_invalidate_range(uintptr_t a,size_t n){uintptr_t e=a+n;for(a&=~31UL;a<e;a+=32)__asm__ volatile("flush %0"::"r"(a):"memory");}
void hal_dcache_clean_range(uintptr_t a,size_t n){hal_icache_invalidate_range(a,n);}
void hal_dcache_invalidate_range(uintptr_t a,size_t n){hal_icache_invalidate_range(a,n);}
void hal_dcache_clean_invalidate_range(uintptr_t a,size_t n){hal_icache_invalidate_range(a,n);}
void hal_sync_instruction_stream(void*a,size_t n){hal_icache_invalidate_range((uintptr_t)a,n);hal_mb();}
void hal_halt(void){for(;;)__asm__ volatile("nop");}
void hal_reset(void){HAL_FATAL("sun4u reset is not implemented");}void hal_poweroff(void){HAL_FATAL("sun4u poweroff is not implemented");}void hal_panic(void){HAL_FATAL("kernel panic");}
bool hal_entropy_fill(void *buffer,size_t size)
{(void)buffer;(void)size;return false;}
void hal_pc98_enable_high_memory(void){}void hal_pc98_memory_segments(uint32_t*l,uint32_t*h){if(l)*l=0;if(h)*h=0;}
