/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <utmpx.h>

#include <stdio.h>
#include <stdint.h>

static int leap(int y){return y%4==0&&(y%100!=0||y%400==0);}
static void utc_fields(int64_t seconds,int *year,int *month,int *day,int *hour,int *minute)
{
	static const int mdays[12]={31,28,31,30,31,30,31,31,30,31,30,31};
	int64_t days=seconds/86400;int y=1970,m=0,d;
	if(seconds<0){*year=1970;*month=*day=*hour=*minute=0;return;}
	*hour=(int)((seconds%86400)/3600);*minute=(int)((seconds%3600)/60);
	while(days>=365+leap(y)){days-=365+leap(y);y++;}
	while(m<12){d=mdays[m]+(m==1&&leap(y));if(days<d)break;days-=d;m++;}
	*year=y;*month=m+1;*day=(int)days+1;
}

int main(void)
{
	struct utmpx *entry;
	setutxent();
	while((entry=getutxent())!=NULL) if(entry->ut_type==USER_PROCESS){
		int y,m,d,h,n;utc_fields(entry->ut_tv_sec,&y,&m,&d,&h,&n);
		printf("%-16s %-16s %04d-%02d-%02d %02d:%02d\n",entry->ut_user,
		    entry->ut_line,y,m,d,h,n);
	}
	endutxent();return 0;
}
