#define _POSIX_C_SOURCE 200809L
#include "userland/base/mkfs/ufs-format.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc,char **argv)
{
 int fd,error,profile;uint64_t bytes;
 if(argc!=4)return 2;
 bytes=strtoull(argv[2],NULL,10);profile=atoi(argv[3]);
 fd=open(argv[1],O_CREAT|O_EXCL|O_RDWR,0600);if(fd<0)return 3;
 if(ftruncate(fd,(off_t)bytes))return 4;
 error=profile?ufs_format_feature_write(fd,bytes):ufs_format_write(fd,bytes);
 if(!error)error=profile?ufs_format_feature_verify(fd,bytes):ufs_format_verify(fd,bytes);
 printf("target write/verify: %d\n",error);close(fd);return error?1:0;
}
