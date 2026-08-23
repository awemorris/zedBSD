/* Focused known-answer tests for the newly added pure-libc SUSv4 APIs. */
#include <search.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int test_errno;
int *__libc_errno_location(void) { return &test_errno; }
#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (0)

static int compare_int(const void *left,const void *right)
{int a=*(const int*)left,b=*(const int*)right;return(a>b)-(a<b);}

static void bytes_to_bits(const unsigned char bytes[8],char bits[64])
{unsigned i;for(i=0;i<64;i++)bits[i]=(char)((bytes[i/8]>>(7-i%8))&1);}
static void bits_to_bytes(const char bits[64],unsigned char bytes[8])
{unsigned i;memset(bytes,0,8);for(i=0;i<64;i++)bytes[i/8]|=(unsigned char)((unsigned)bits[i]<<(7-i%8));}

int
main(void)
{
	unsigned short state[3]={0x330e,0x1234,0};
	uint64_t expected=(0x1234330eULL*0x5deece66dULL+0xb)&0xffffffffffffULL;
	char random_state[128],*old_state;
	long first;
	int values[8]={1,3,5},key=3,new_value=4;
	size_t count=3;
	void *root=NULL;
	unsigned char key_bytes[8]={0x13,0x34,0x57,0x79,0x9b,0xbc,0xdf,0xf1};
	unsigned char plain[8]={0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};
	const unsigned char cipher[8]={0x85,0xe8,0x13,0x54,0x0f,0x0a,0xb4,0x05};
	char key_bits[64],block_bits[64];unsigned char result[8];

	CHECK(nrand48(state)==(long)(expected>>17));
	srandom(1);CHECK(random()==1804289383L);
	old_state=initstate(7,random_state,sizeof(random_state));CHECK(old_state!=NULL);
	first=random();CHECK(setstate(old_state)==random_state);CHECK(setstate(random_state)==old_state);CHECK(random()!=first);
	CHECK(lfind(&key,values,&count,sizeof(values[0]),compare_int)==&values[1]);
	CHECK(lsearch(&new_value,values,&count,sizeof(values[0]),compare_int)==&values[3]);CHECK(count==4);
	CHECK(tsearch(&values[0],&root,compare_int)!=NULL);CHECK(tsearch(&values[1],&root,compare_int)!=NULL);CHECK(tfind(&values[1],&root,compare_int)!=NULL);CHECK(tdelete(&values[0],&root,compare_int)!=NULL);
	bytes_to_bits(key_bytes,key_bits);bytes_to_bits(plain,block_bits);setkey(key_bits);encrypt(block_bits,0);bits_to_bytes(block_bits,result);CHECK(memcmp(result,cipher,8)==0);
	encrypt(block_bits,1);bits_to_bytes(block_bits,result);CHECK(memcmp(result,plain,8)==0);
	return 0;
}
