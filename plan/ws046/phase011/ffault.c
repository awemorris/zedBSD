#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static long long now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000000000LL+t.tv_nsec;}
int main(int c,char**v){int fd=open(v[1],O_RDONLY);struct stat s;fstat(fd,&s);long pages=s.st_size/4096;int passes=c>2?atoi(v[2]):5;volatile char sink;
for(int p=0;p<passes;p++){long long t=now();char*m=mmap(0,pages*4096,PROT_READ,MAP_PRIVATE,fd,0);for(long i=0;i<pages;i++)sink=m[i*4096];munmap(m,pages*4096);long long e=now();printf("pass %d: %ld pages %lld ns/page\n",p,pages,(e-t)/pages);}
(void)sink;return 0;}
