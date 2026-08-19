/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/libc/resolver-internal.h"
#include <stdio.h>
#include <string.h>
static void w16(uint8_t*p,unsigned v){p[0]=v>>8;p[1]=v;}
static void w32(uint8_t*p,unsigned v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
int main(void)
{
	uint8_t q[512],a[512]; size_t ql,o; int truncated;
	struct resolver_result result;
	if (resolver_dns_build_query(q,sizeof(q),0x1234,"zedbsd.test",DNS_TYPE_A,&ql)!=0) return 1;
	memcpy(a,q,ql); w16(a+2,0x8180); w16(a+6,1); o=ql;
	a[o++]=0xc0;a[o++]=0x0c;w16(a+o,DNS_TYPE_A);w16(a+o+2,1);
	w32(a+o+4,300);w16(a+o+8,4);o+=10;memcpy(a+o,"\xc0\x00\x02\x7b",4);o+=4;
	memset(&result,0,sizeof(result));
	if (resolver_dns_parse(a,o,0x1234,"zedbsd.test",DNS_TYPE_A,&result,&truncated)!=0) return 1;
	if (truncated || result.address_count!=1 || result.ttl!=300) return 1;
	puts("DNS host test: PASS"); return 0;
}
