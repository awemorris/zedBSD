/* BSD err(3) family. SPDX-License-Identifier: Zlib */
#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static FILE *error_file;
static void (*exit_hook)(int);

void err_set_file(void *file) { error_file = file; }
void err_set_exit(void (*hook)(int)) { exit_hook = hook; }
static FILE *destination(void) { return error_file ? error_file : stderr; }
static void report(int code, int show_error, const char *format, va_list ap)
{
	FILE *stream = destination();
	if (format && *format) { (void)vfprintf(stream, format, ap); if (show_error) (void)fputs(": ", stream); }
	if (show_error) (void)fputs(strerror(code), stream);
	(void)fputc('\n', stream);
}
void vwarnc(int code,const char *format,va_list ap) { report(code,1,format,ap); }
void vwarn(const char *format,va_list ap) { int code=errno; report(code,1,format,ap); }
void vwarnx(const char *format,va_list ap) { report(0,0,format,ap); }
void warnc(int code,const char *format,...) {va_list ap;va_start(ap,format);vwarnc(code,format,ap);va_end(ap);}
void warn(const char *format,...) {va_list ap;va_start(ap,format);vwarn(format,ap);va_end(ap);}
void warnx(const char *format,...) {va_list ap;va_start(ap,format);vwarnx(format,ap);va_end(ap);}
static void finish(int status) __attribute__((noreturn));
static void finish(int status) { if(exit_hook)exit_hook(status); exit(status); }
void verrc(int status,int code,const char *format,va_list ap) {vwarnc(code,format,ap);finish(status);}
void verr(int status,const char *format,va_list ap) {vwarn(format,ap);finish(status);}
void verrx(int status,const char *format,va_list ap) {vwarnx(format,ap);finish(status);}
void errc(int status,int code,const char *format,...) {va_list ap;va_start(ap,format);verrc(status,code,format,ap);}
void err(int status,const char *format,...) {va_list ap;va_start(ap,format);verr(status,format,ap);}
void errx(int status,const char *format,...) {va_list ap;va_start(ap,format);verrx(status,format,ap);}
