/* ISO C and XSI time additions. SPDX-License-Identifier: Zlib */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int timespec_get(struct timespec *time, int base)
{ if (base != TIME_UTC) return 0; return clock_gettime(CLOCK_REALTIME, time) == 0 ? base : 0; }

int getdate_err;
static const char *const short_months[12]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
static const char *const long_months[12]={"January","February","March","April","May","June","July","August","September","October","November","December"};
static const char *const short_days[7]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *const long_days[7]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};

static const char *
number(const char *input,int minimum_digits,int maximum_digits,int minimum,int maximum,int *result)
{
	int digits=0,value=0;
	while(digits<maximum_digits && *input>='0'&&*input<='9'){value=value*10+*input++-'0';digits++;}
	if(digits<minimum_digits||value<minimum||value>maximum)return NULL;
	*result=value;return input;
}

static const char *
name_value(const char *input,const char *const *short_names,const char *const *long_names,int count,int *result)
{
	int index;
	for(index=0;index<count;index++){
		size_t length=strlen(long_names[index]);
		if(!strncmp(input,long_names[index],length)){*result=index;return input+length;}
		length=strlen(short_names[index]);
		if(!strncmp(input,short_names[index],length)){*result=index;return input+length;}
	}
	return NULL;
}

char *
strptime(const char *restrict input,const char *restrict format,struct tm *restrict value)
{
	int century=-1,year2=-1,hour12=-1,pm=-1,parsed;
	const char *next;
	if(input==NULL||format==NULL||value==NULL)return NULL;
	while(*format){
		if(isspace((unsigned char)*format)){while(isspace((unsigned char)*format))format++;while(isspace((unsigned char)*input))input++;continue;}
		if(*format!='%'){if(*input++!=*format++)return NULL;continue;}
		format++;if(*format=='E'||*format=='O')format++;
		switch(*format++){
		case '%':if(*input++!='%')return NULL;break;
		case 'n':case 't':while(isspace((unsigned char)*input))input++;break;
		case 'Y':next=number(input,1,4,0,9999,&parsed);if(!next)return NULL;value->tm_year=parsed-1900;input=next;break;
		case 'C':next=number(input,1,2,0,99,&century);if(!next)return NULL;input=next;break;
		case 'y':next=number(input,1,2,0,99,&year2);if(!next)return NULL;input=next;break;
		case 'm':next=number(input,1,2,1,12,&parsed);if(!next)return NULL;value->tm_mon=parsed-1;input=next;break;
		case 'b':case 'B':case 'h':next=name_value(input,short_months,long_months,12,&value->tm_mon);if(!next)return NULL;input=next;break;
		case 'd':case 'e':while(*input==' ')input++;next=number(input,1,2,1,31,&value->tm_mday);if(!next)return NULL;input=next;break;
		case 'H':next=number(input,1,2,0,23,&value->tm_hour);if(!next)return NULL;input=next;break;
		case 'I':next=number(input,1,2,1,12,&hour12);if(!next)return NULL;input=next;break;
		case 'M':next=number(input,1,2,0,59,&value->tm_min);if(!next)return NULL;input=next;break;
		case 'S':next=number(input,1,2,0,60,&value->tm_sec);if(!next)return NULL;input=next;break;
		case 'j':next=number(input,1,3,1,366,&parsed);if(!next)return NULL;value->tm_yday=parsed-1;input=next;break;
		case 'w':next=number(input,1,1,0,6,&value->tm_wday);if(!next)return NULL;input=next;break;
		case 'u':next=number(input,1,1,1,7,&parsed);if(!next)return NULL;value->tm_wday=parsed%7;input=next;break;
		case 'a':case 'A':next=name_value(input,short_days,long_days,7,&value->tm_wday);if(!next)return NULL;input=next;break;
		case 'p':if(!strncmp(input,"AM",2)||!strncmp(input,"am",2)){pm=0;input+=2;}else if(!strncmp(input,"PM",2)||!strncmp(input,"pm",2)){pm=1;input+=2;}else return NULL;break;
		case 'D':next=strptime(input,"%m/%d/%y",value);if(!next)return NULL;input=next;break;
		case 'F':next=strptime(input,"%Y-%m-%d",value);if(!next)return NULL;input=next;break;
		case 'R':next=strptime(input,"%H:%M",value);if(!next)return NULL;input=next;break;
		case 'T':case 'X':next=strptime(input,"%H:%M:%S",value);if(!next)return NULL;input=next;break;
		case 'r':next=strptime(input,"%I:%M:%S %p",value);if(!next)return NULL;input=next;break;
		case 'x':next=strptime(input,"%m/%d/%y",value);if(!next)return NULL;input=next;break;
		case 'c':next=strptime(input,"%a %b %e %H:%M:%S %Y",value);if(!next)return NULL;input=next;break;
		case 'U':case 'W':case 'V':next=number(input,1,2,0,53,&parsed);if(!next)return NULL;input=next;break;
		case 'G':next=number(input,1,4,0,9999,&parsed);if(!next)return NULL;input=next;break;
		case 'g':next=number(input,1,2,0,99,&parsed);if(!next)return NULL;input=next;break;
		case 'z':if(*input=='+'||*input=='-')input++;next=number(input,4,4,0,2359,&parsed);if(!next)return NULL;input=next;break;
		case 'Z':if(!isalpha((unsigned char)*input))return NULL;while(isalpha((unsigned char)*input))input++;break;
		default:return NULL;
		}
	}
	if(year2>=0)value->tm_year=(century>=0?century*100+year2:(year2<=68?2000+year2:1900+year2))-1900;
	else if(century>=0)value->tm_year=century*100-1900;
	if(hour12>=0)value->tm_hour=hour12%12+(pm>0?12:0);
	return(char*)input;
}

struct tm *
getdate(const char *input)
{
	static struct tm result;
	const char *mask=getenv("DATEMSK");
	FILE *file;
	char line[256];
	time_t now;
	struct tm base;
	if(mask==NULL||*mask=='\0'){getdate_err=1;return NULL;}
	file=fopen(mask,"r");if(file==NULL){getdate_err=2;return NULL;}
	now=time(NULL);if(localtime_r(&now,&base)==NULL)memset(&base,0,sizeof(base));
	while(fgets(line,sizeof(line),file)!=NULL){
		char *end;size_t length=strlen(line);while(length&& (line[length-1]=='\n'||line[length-1]=='\r'))line[--length]='\0';
		result=base;end=strptime(input,line,&result);
		if(end!=NULL){while(isspace((unsigned char)*end))end++;if(*end=='\0'&&mktime(&result)!=(time_t)-1){fclose(file);getdate_err=0;return &result;}}
	}
	if(ferror(file)){fclose(file);getdate_err=4;return NULL;}
	if(fclose(file)!=0){getdate_err=5;return NULL;}
	getdate_err=7;return NULL;
}
