/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/net/dhcp.h"
#include <stdio.h>
#include <string.h>
int main(void)
{
	uint8_t mac[6] = {0x52,0x54,0,0x12,0x34,0x56}, packet[600];
	size_t length, offset; struct dhcp_lease lease;
	if (!(dhcp_build(packet,sizeof(packet),&length,DHCP_DISCOVER,
	    0x12345678U,mac,0,0)==0 && length>=300 && packet[242]==DHCP_DISCOVER)) return 1;
	memset(packet,0,sizeof(packet)); packet[0]=2; packet[1]=1; packet[2]=6;
	packet[4]=0x12; packet[5]=0x34; packet[6]=0x56; packet[7]=0x78;
	memcpy(packet+16,"\x0a\x00\x02\x0f",4); memcpy(packet+28,mac,6);
	memcpy(packet+236,"\x63\x82\x53\x63",4); offset=240;
#define OPT(c,n,b) do { packet[offset++]=(c); packet[offset++]=(n); memcpy(packet+offset,(b),(n)); offset+=(n); } while(0)
	{ uint8_t type=DHCP_ACK; OPT(53,1,&type); }
	OPT(1,4,"\xff\xff\xff\x00"); OPT(3,4,"\x0a\x00\x02\x02");
	OPT(6,8,"\x0a\x00\x02\x03\x01\x01\x01\x01");
	OPT(54,4,"\x0a\x00\x02\x02"); packet[offset++]=255;
	if (dhcp_parse(packet,offset,0x12345678U,mac,&lease)!=0) return 1;
	if (!(lease.message_type==DHCP_ACK && lease.router_count==1 &&
	    lease.dns_count==2 && lease.netmask!=0 && lease.broadcast!=0)) return 1;
	puts("DHCP host test: PASS"); return 0;
}
