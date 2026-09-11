#include <kern/io-destination.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
int main(void)
{
 unsigned char left[3],right[5];
 struct io_span spans[3]={{left,3},{NULL,0},{right,5}};
 struct io_destination d={spans,3},slice;
 struct io_span sliced[IO_DESTINATION_SPANS_MAX];
 struct io_destination_cursor cursor,saved;
 memset(left,0x66,sizeof(left));memset(right,0x66,sizeof(right));
 assert(io_destination_copy(&d,2,"abcd",4)==0);
 assert(left[0]==0x66 && left[1]==0x66 && left[2]=='a');
 assert(memcmp(right,"bcd",3)==0 && right[3]==0x66 && right[4]==0x66);
 assert(io_destination_copy(&d,7,"xx",2)==EINVAL && right[4]==0x66);
 assert(io_destination_copy(&d,SIZE_MAX,"x",1)==EINVAL);
 assert(io_destination_copy(&d,8,NULL,0)==0);
 assert(io_destination_slice(&d,2,4,sliced,&slice)==0 && slice.count==2);
 assert(io_destination_copy(&slice,0,"WXYZ",4)==0);
 assert(left[2]=='W' && memcmp(right,"XYZ",3)==0);
 assert(io_destination_slice(&d,8,0,sliced,&slice)==0 && slice.count==0);
 assert(io_destination_slice(&d,7,2,sliced,&slice)==EINVAL);
 assert(io_destination_slice(&d,SIZE_MAX,1,sliced,&slice)==EINVAL);
 assert(io_destination_cursor_init(&cursor,&d,8)==0);
 saved=cursor;
 assert(io_destination_cursor_copy(&cursor,"012345678",9)==EINVAL);
 assert(memcmp(&saved,&cursor,sizeof(cursor))==0);
 assert(io_destination_cursor_copy(&cursor,NULL,1)==EINVAL);
 assert(memcmp(&saved,&cursor,sizeof(cursor))==0);
 assert(io_destination_cursor_copy(&cursor,(void *)UINTPTR_MAX,2)==EINVAL);
 assert(memcmp(&saved,&cursor,sizeof(cursor))==0);
 /* Captured descriptor is independent of later source descriptor edits. */
 spans[0].address=NULL;
 assert(io_destination_cursor_copy(&cursor,"12",2)==0);
 assert(io_destination_cursor_copy(&cursor,"3456",4)==0);
 assert(io_destination_cursor_copy(&cursor,"78",2)==0);
 assert(memcmp(left,"123",3)==0 && memcmp(right,"45678",5)==0);
 assert(cursor.remaining==0 && io_destination_cursor_copy(&cursor,NULL,0)==0);
 assert(io_destination_cursor_copy(&cursor,"x",1)==EINVAL);
 spans[0].address=left;
 memset(left,0x66,sizeof(left));memset(right,0x66,sizeof(right));
 spans[2].address=NULL;
 assert(io_destination_slice(&d,0,1,sliced,&slice)==EINVAL);
 assert(io_destination_cursor_init(&cursor,&d,1)==EINVAL);
 assert(cursor.remaining==0 && io_destination_cursor_copy(&cursor,"x",1)==EINVAL);
 assert(io_destination_copy(&d,0,"x",1)==EINVAL && left[0]==0x66);
 spans[0].address=(void *)UINTPTR_MAX;spans[0].size=2;
 assert(io_destination_validate(&d,0)==EINVAL);
 spans[0].address=(void *)1;spans[0].size=SIZE_MAX;
 spans[2].address=(void *)1;spans[2].size=1;
 assert(io_destination_validate(&d,0)==EOVERFLOW);
 d.count=IO_DESTINATION_SPANS_MAX+1;
 assert(io_destination_validate(&d,0)==EINVAL);
 d.count=0;d.spans=NULL;
 assert(io_destination_copy(&d,0,NULL,0)==0);
 puts("destination boundaries: PASS offset, gaps, rejection before write, overflow");
 return 0;
}
