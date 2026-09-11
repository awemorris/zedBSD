#!/usr/bin/env python3
"""Exercise the production console parser with split terminal control writes."""
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[3]


def main():
    text = (REPO / "src/kern/tty.c").read_text()
    start = text.index("static void\ntty_render(")
    end = text.index("/* Appends output", start)
    source = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
static unsigned console_escape_state[1], console_escape_parameter[1];
static unsigned console_escape_has_parameter[1], console_escape_ignored[1];
static char output[1024]; static size_t used; static unsigned commands;
static void hal_cons_write_n(const char *p,size_t n) { assert(used+n<sizeof(output)); memcpy(output+used,p,n); used+=n; output[used]=0; }
static void tty_console_csi(unsigned vt,unsigned char byte) { (void)vt; assert(byte=='J'); commands++; }
'''
    source += text[start:end]
    source += r'''
int main(void) {
 const char *controls[] = {"before\033[>4;2mafter", "before\033[?2004hafter", "before\033[1;2Hafter", "before\033[2Jafter"};
 size_t i,split,length; unsigned cases=0;
 for(i=0;i<sizeof(controls)/sizeof(controls[0]);i++) {
  length=strlen(controls[i]);
  for(split=0;split<=length;split++) {
   used=commands=0; output[0]=0; console_escape_state[0]=0;
   tty_render(0,controls[i],split); tty_render(0,controls[i]+split,length-split);
   assert(!strcmp(output,"beforeafter")); assert(commands==(i==3)); cases++;
  }
  used=commands=0; output[0]=0; console_escape_state[0]=0;
  for(split=0;split<length;split++) tty_render(0,controls[i]+split,1);
  assert(!strcmp(output,"beforeafter")); assert(commands==(i==3)); cases++;
 }
 printf("console CSI PASS %u cases\n",cases); return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="zedbsd-csi-") as temporary:
        path = Path(temporary)
        (path / "test.c").write_text(source)
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(path / "test.c"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    main()
