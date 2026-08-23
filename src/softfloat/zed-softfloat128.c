/* Integer-only IEEE 754 binary128 implementation for SPARC V9.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <fenv.h>
#include <stdint.h>

#include "src/softfloat/zed-softfloat.h"
#include "src/softfloat/zed-softfloat128.h"

#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif
#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif

struct quad_uint { uint64_t limb[4]; };

enum quad_class { QUAD_ZERO, QUAD_FINITE, QUAD_INFINITY, QUAD_NAN };
struct quad_number {
	struct quad_uint significand;
	int exponent;
	unsigned int sign;
	enum quad_class classification;
};

static int
quad_zero(struct quad_uint value)
{ return (value.limb[0]|value.limb[1]|value.limb[2]|value.limb[3]) == 0U; }

static int
quad_compare_uint(struct quad_uint left, struct quad_uint right)
{
	int index;
	for(index=3;index>=0;index--)
		if(left.limb[index]!=right.limb[index])
			return left.limb[index]<right.limb[index]?-1:1;
	return 0;
}

static void
quad_shift_left_one(struct quad_uint *value)
{
	unsigned int index; uint64_t carry=0;
	for(index=0;index<4;index++) {
		uint64_t next=value->limb[index]>>63;
		value->limb[index]=(value->limb[index]<<1)|carry;carry=next;
	}
}

static void
quad_shift_left(struct quad_uint *value, unsigned int distance)
{ while(distance-- != 0U) quad_shift_left_one(value); }

static void
quad_shift_right_jam(struct quad_uint *value, unsigned int distance)
{
	int jam=0;
	if(distance>=256U){jam=!quad_zero(*value);*value=(struct quad_uint){{0,0,0,0}};value->limb[0]=jam;return;}
	while(distance-- != 0U){
		int index;uint64_t carry=0;jam|=(value->limb[0]&1U)!=0U;
		for(index=3;index>=0;index--){uint64_t next=value->limb[index]<<63;value->limb[index]=(value->limb[index]>>1)|carry;carry=next;}
	}
	if(jam)value->limb[0]|=1U;
}

static void
quad_shift_right(struct quad_uint *value, unsigned int distance)
{
	while (distance-- != 0U) {
		int index;
		uint64_t carry = 0U;
		for (index = 3; index >= 0; index--) {
			uint64_t next = value->limb[index] << 63;
			value->limb[index] = (value->limb[index] >> 1) | carry;
			carry = next;
		}
	}
}

static void
quad_add_uint(struct quad_uint *left, struct quad_uint right)
{
	unsigned int i;uint64_t carry=0;
	for(i=0;i<4;i++){uint64_t a=left->limb[i],s=a+right.limb[i]+carry;uint64_t c1=s<a;uint64_t c2=carry&&s==a;left->limb[i]=s;carry=c1|c2;}
}

static void
quad_sub_uint(struct quad_uint *left, struct quad_uint right)
{
	unsigned int i;uint64_t borrow=0;
	for(i=0;i<4;i++){uint64_t a=left->limb[i],b=right.limb[i]+borrow;uint64_t extra=borrow&&b==0;left->limb[i]=a-b;borrow=(a<b)|extra;}
}

static int
quad_highest_bit(struct quad_uint value)
{
	int limb;
	for(limb=3;limb>=0;limb--)if(value.limb[limb]!=0){uint64_t x=value.limb[limb];int bit=0;while(x>>=1)bit++;return limb*64+bit;}
	return -1;
}

static int
quad_bit(struct quad_uint value,unsigned int bit)
{ return (int)((value.limb[bit/64U]>>(bit%64U))&1U); }

static void
quad_set_bit(struct quad_uint *value,unsigned int bit)
{ value->limb[bit/64U]|=UINT64_C(1)<<(bit%64U); }

static struct quad_number
quad_unpack(struct zsf128 bits)
{
	struct quad_number n={{{bits.low,bits.high&UINT64_C(0x0000ffffffffffff),0,0}},0,(unsigned int)(bits.high>>63),QUAD_FINITE};
	unsigned int e=(unsigned int)((bits.high>>48)&0x7fffU);
	if(e==0x7fffU){n.classification=quad_zero(n.significand)?QUAD_INFINITY:QUAD_NAN;return n;}
	if(e==0U){n.exponent=-16382;n.classification=quad_zero(n.significand)?QUAD_ZERO:QUAD_FINITE;return n;}
	n.exponent=(int)e-16383;quad_set_bit(&n.significand,112U);return n;
}

static void
quad_normalize(struct quad_number *n)
{ int highest=quad_highest_bit(n->significand);if(highest>=0&&highest<112){quad_shift_left(&n->significand,(unsigned int)(112-highest));n->exponent-=112-highest;} }

static struct zsf128
quad_nan(void)
{ return (struct zsf128){UINT64_C(0x7fff800000000000),0}; }

static struct zsf128
quad_pack(unsigned int sign,int exponent,struct quad_uint sig)
{
	int highest=quad_highest_bit(sig);uint64_t round_bits,exponent_field;
	if(highest<0)return(struct zsf128){(uint64_t)sign<<63,0};
	if(highest>115){quad_shift_right_jam(&sig,(unsigned int)(highest-115));exponent+=highest-115;}
	else if(highest<115&&exponent>-16382){unsigned int d=(unsigned int)(115-highest);if(exponent-(int)d < -16382)d=(unsigned int)(exponent+16382);quad_shift_left(&sig,d);exponent-=(int)d;}
	if(exponent < -16382){quad_shift_right_jam(&sig,(unsigned int)(-16382-exponent));exponent=-16382;}
	round_bits=sig.limb[0]&7U;quad_shift_right(&sig,3U);
	if(round_bits>4U||(round_bits==4U&&(sig.limb[0]&1U))){struct quad_uint one={{1,0,0,0}};quad_add_uint(&sig,one);}
	if(round_bits){(void)feraiseexcept(FE_INEXACT);}
	if(quad_bit(sig,113U)){quad_shift_right_jam(&sig,1U);exponent++;}
	if(exponent>16383){(void)feraiseexcept(FE_OVERFLOW|FE_INEXACT);return(struct zsf128){((uint64_t)sign<<63)|UINT64_C(0x7fff000000000000),0};}
	if(exponent==-16382&&!quad_bit(sig,112U)){exponent_field=0;if(round_bits)(void)feraiseexcept(FE_UNDERFLOW);}else exponent_field=(uint64_t)(exponent+16383);
	sig.limb[1]&=UINT64_C(0x0000ffffffffffff);
	return(struct zsf128){((uint64_t)sign<<63)|(exponent_field<<48)|sig.limb[1],sig.limb[0]};
}

static struct zsf128
quad_add_bits(struct zsf128 a,struct zsf128 b)
{
	struct quad_number x=quad_unpack(a),y=quad_unpack(b),t;struct quad_uint result;
	if(x.classification==QUAD_NAN||y.classification==QUAD_NAN)return quad_nan();
	if(x.classification==QUAD_INFINITY||y.classification==QUAD_INFINITY){if(x.classification==QUAD_INFINITY&&y.classification==QUAD_INFINITY&&x.sign!=y.sign){(void)feraiseexcept(FE_INVALID);return quad_nan();}return x.classification==QUAD_INFINITY?a:b;}
	if(x.classification==QUAD_ZERO)return y.classification==QUAD_ZERO&&x.sign!=y.sign?(struct zsf128){0,0}:b;
	if(y.classification==QUAD_ZERO)return a;
	quad_normalize(&x);quad_normalize(&y);if(x.exponent<y.exponent||(x.exponent==y.exponent&&quad_compare_uint(x.significand,y.significand)<0)){t=x;x=y;y=t;}
	quad_shift_left(&x.significand,3);quad_shift_left(&y.significand,3);quad_shift_right_jam(&y.significand,(unsigned int)(x.exponent-y.exponent));result=x.significand;
	if(x.sign==y.sign)quad_add_uint(&result,y.significand);else quad_sub_uint(&result,y.significand);
	return quad_pack(x.sign,x.exponent,result);
}

static struct quad_uint
quad_multiply_significands(struct quad_uint a,struct quad_uint b)
{
	struct quad_uint result={{0,0,0,0}},shifted=a;unsigned int bit;
	for(bit=0;bit<=112U;bit++){if(quad_bit(b,bit))quad_add_uint(&result,shifted);quad_shift_left_one(&shifted);}return result;
}

struct zsf128 zsf128_add(struct zsf128 a,struct zsf128 b){return quad_add_bits(a,b);}
struct zsf128 zsf128_sub(struct zsf128 a,struct zsf128 b){b.high^=UINT64_C(0x8000000000000000);return quad_add_bits(a,b);}

struct zsf128
zsf128_mul(struct zsf128 a,struct zsf128 b)
{
	struct quad_number x=quad_unpack(a),y=quad_unpack(b);unsigned int sign=x.sign^y.sign;struct quad_uint p;
	if(x.classification==QUAD_NAN||y.classification==QUAD_NAN)return quad_nan();
	if((x.classification==QUAD_INFINITY&&y.classification==QUAD_ZERO)||(y.classification==QUAD_INFINITY&&x.classification==QUAD_ZERO)){(void)feraiseexcept(FE_INVALID);return quad_nan();}
	if(x.classification==QUAD_INFINITY||y.classification==QUAD_INFINITY)return(struct zsf128){((uint64_t)sign<<63)|UINT64_C(0x7fff000000000000),0};
	if(x.classification==QUAD_ZERO||y.classification==QUAD_ZERO)return(struct zsf128){(uint64_t)sign<<63,0};
	quad_normalize(&x);quad_normalize(&y);p=quad_multiply_significands(x.significand,y.significand);quad_shift_right_jam(&p,109U);return quad_pack(sign,x.exponent+y.exponent,p);
}

struct zsf128
zsf128_div(struct zsf128 a,struct zsf128 b)
{
	struct quad_number x=quad_unpack(a),y=quad_unpack(b);unsigned int sign=x.sign^y.sign,i;int exponent;struct quad_uint q={{1,0,0,0}},r;
	if(x.classification==QUAD_NAN||y.classification==QUAD_NAN)return quad_nan();
	if((x.classification==QUAD_ZERO&&y.classification==QUAD_ZERO)||(x.classification==QUAD_INFINITY&&y.classification==QUAD_INFINITY)){(void)feraiseexcept(FE_INVALID);return quad_nan();}
	if(x.classification==QUAD_INFINITY)return(struct zsf128){((uint64_t)sign<<63)|UINT64_C(0x7fff000000000000),0};
	if(y.classification==QUAD_INFINITY||x.classification==QUAD_ZERO)return(struct zsf128){(uint64_t)sign<<63,0};
	if(y.classification==QUAD_ZERO){(void)feraiseexcept(FE_DIVBYZERO);return(struct zsf128){((uint64_t)sign<<63)|UINT64_C(0x7fff000000000000),0};}
	quad_normalize(&x);quad_normalize(&y);exponent=x.exponent-y.exponent;if(quad_compare_uint(x.significand,y.significand)<0){quad_shift_left_one(&x.significand);exponent--;}
	r=x.significand;quad_sub_uint(&r,y.significand);for(i=0;i<115U;i++){quad_shift_left_one(&q);quad_shift_left_one(&r);if(quad_compare_uint(r,y.significand)>=0){quad_sub_uint(&r,y.significand);q.limb[0]|=1U;}}
	if (!quad_zero(r))
		q.limb[0] |= 1U;
	return quad_pack(sign, exponent, q);
}

int
zsf128_compare(struct zsf128 a,struct zsf128 b,int *unordered)
{
	struct quad_number x=quad_unpack(a),y=quad_unpack(b);uint64_t ah=a.high&UINT64_C(0x7fffffffffffffff),bh=b.high&UINT64_C(0x7fffffffffffffff);int magnitude;
	*unordered=x.classification==QUAD_NAN||y.classification==QUAD_NAN;if(*unordered)return 0;if((ah|a.low)==0&&(bh|b.low)==0)return 0;if(x.sign!=y.sign)return x.sign?-1:1;magnitude=ah==bh?(a.low==b.low?0:(a.low<b.low?-1:1)):(ah<bh?-1:1);return x.sign?-magnitude:magnitude;
}

static struct zsf128
small_to_quad(uint64_t fraction,int exponent,unsigned int source_fraction,unsigned int sign)
{ struct quad_uint sig={{fraction,0,0,0}};quad_shift_left(&sig,115U-source_fraction);return quad_pack(sign,exponent,sig); }

struct zsf128 zsf32_to_128(uint32_t v){unsigned int e=(v>>23)&0xffU,s=v>>31;uint64_t f=v&0x7fffffU;if(e==0xffU)return(struct zsf128){((uint64_t)s<<63)|UINT64_C(0x7fff000000000000)|(f?UINT64_C(0x0000800000000000):0),0};if(!e&&!f)return(struct zsf128){(uint64_t)s<<63,0};if(!e){e=1;while(!(f&0x800000U)){f<<=1;e--;}}else f|=0x800000U;return small_to_quad(f,(int)e-127,23U,s);}
struct zsf128 zsf64_to_128(uint64_t v){unsigned int e=(unsigned int)((v>>52)&0x7ffU),s=(unsigned int)(v>>63);uint64_t f=v&UINT64_C(0xfffffffffffff);if(e==0x7ffU)return(struct zsf128){((uint64_t)s<<63)|UINT64_C(0x7fff000000000000)|(f?UINT64_C(0x0000800000000000):0),0};if(!e&&!f)return(struct zsf128){(uint64_t)s<<63,0};if(!e){e=1;while(!(f&UINT64_C(0x10000000000000))){f<<=1;e--;}}else f|=UINT64_C(0x10000000000000);return small_to_quad(f,(int)e-1023,52U,s);}

uint64_t zsf128_to_64(struct zsf128 v){struct quad_number n=quad_unpack(v);if(n.classification==QUAD_NAN)return UINT64_C(0x7ff8000000000000)|((uint64_t)n.sign<<63);if(n.classification==QUAD_INFINITY)return UINT64_C(0x7ff0000000000000)|((uint64_t)n.sign<<63);if(n.classification==QUAD_ZERO)return(uint64_t)n.sign<<63;quad_normalize(&n);quad_shift_right_jam(&n.significand,57U);return zsf64_round_pack(n.sign,n.exponent,n.significand.limb[0]);}
uint32_t zsf128_to_32(struct zsf128 v){struct quad_number n=quad_unpack(v);if(n.classification==QUAD_NAN)return UINT32_C(0x7fc00000)|(n.sign<<31);if(n.classification==QUAD_INFINITY)return UINT32_C(0x7f800000)|(n.sign<<31);if(n.classification==QUAD_ZERO)return n.sign<<31;quad_normalize(&n);quad_shift_right_jam(&n.significand,86U);return zsf32_round_pack(n.sign,n.exponent,n.significand.limb[0]);}

struct zsf128 zsf_u64_to_128(uint64_t v){int h=0;uint64_t s=v;struct quad_uint q={{v,0,0,0}};if(!v)return(struct zsf128){0,0};while(s>>=1)h++;quad_shift_left(&q,(unsigned int)(115-h));return quad_pack(0,h,q);}
struct zsf128 zsf_i64_to_128(int64_t v){uint64_t m=v<0?(uint64_t)(-(v+1))+1U:(uint64_t)v;struct zsf128 r=zsf_u64_to_128(m);if(v<0)r.high|=UINT64_C(0x8000000000000000);return r;}
