/* XSI formatted diagnostic messages. SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <fmtmsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
write_all(int descriptor,const char *text,size_t length)
{
	while(length){ssize_t count=write(descriptor,text,length);if(count<0){if(errno==EINTR)continue;return -1;}text+=count;length-=(size_t)count;}
	return 0;
}

int
fmtmsg(long classification,const char *label,int severity,const char *text,
    const char *action,const char *tag)
{
	static const char *const severity_names[]={"","HALT","ERROR","WARNING","INFO"};
	char message[1024];
	const char *name=(severity>=MM_HALT&&severity<=MM_INFO)?severity_names[severity]:"";
	const char *verb=getenv("MSGVERB");
	int result=MM_OK,length,saved=0,console;
	int show_label=verb==NULL||strstr(verb,"label")!=NULL;
	int show_severity=verb==NULL||strstr(verb,"severity")!=NULL;
	int show_text=verb==NULL||strstr(verb,"text")!=NULL;
	int show_action=verb==NULL||strstr(verb,"action")!=NULL;
	int show_tag=verb==NULL||strstr(verb,"tag")!=NULL;
	length=snprintf(message,sizeof(message),"%s%s%s%s%s%s%s%s%s%s\n",
	    show_label&&label?label:"",show_label&&label?": ":"",
	    show_severity&&*name?name:"",show_severity&&*name?": ":"",
	    show_text&&text?text:"",show_action&&action?"\nTO FIX: ":"",
	    show_action&&action?action:"",show_tag&&tag?"  ":"",
	    show_tag&&tag?tag:"","");
	if(length<0 || (size_t)length>=sizeof(message)){errno=EOVERFLOW;return MM_NOTOK;}
	if(classification&MM_PRINT){if(write_all(STDERR_FILENO,message,(size_t)length)!=0){saved=errno;result|=MM_NOMSG;}}
	if(classification&MM_CONSOLE){
		console=open("/dev/console",O_WRONLY|O_CLOEXEC);
		if(console<0||write_all(console,message,(size_t)length)!=0){if(!saved)saved=errno;result|=MM_NOCON;}
		if(console>=0)(void)close(console);
	}
	if(saved)errno=saved;
	return result;
}
