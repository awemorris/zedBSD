#!/usr/bin/env python3
# ws035-p047: lists the DHCP messages in a pcap from QEMU filter-dump, with times.
# Usage: pcap-dhcp.py FILE
import struct,sys
d=open(sys.argv[1],'rb').read()
off=24; t0=None
names={1:'DISCOVER',2:'OFFER',3:'REQUEST',5:'ACK',6:'NAK',7:'RELEASE'}
while off+16<=len(d):
    ts,us,incl,orig=struct.unpack_from('<IIII',d,off); off+=16
    pkt=d[off:off+incl]; off+=incl
    t=ts+us/1e6
    if t0 is None: t0=t
    if len(pkt)<42 or struct.unpack('>H',pkt[12:14])[0]!=0x0800: continue
    ihl=(pkt[14]&15)*4
    if pkt[23]!=17: continue
    sp,dp=struct.unpack('>HH',pkt[14+ihl:14+ihl+4])
    if not ({sp,dp}&{67,68}): continue
    b=pkt[14+ihl+8:]; opts=b[240:]; mt=None; i=0
    while i<len(opts):
        o=opts[i]
        if o==255: break
        if o==0: i+=1; continue
        if o==53: mt=opts[i+2]
        i+=2+opts[i+1]
    print('%7.3f %s xid=%s'%(t-t0,names.get(mt,mt),b[4:8].hex()))
