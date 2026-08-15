WATCOM ?= /home/awe/openwatcom-1.9
WCL := $(WATCOM)/binl/wcl

all: inst.exe

inst.exe: inst.c
	WATCOM=$(WATCOM) PATH=$(WATCOM)/binl:$$PATH \
	INCLUDE=$(WATCOM)/h $(WCL) -q -l=dos -3 -ms -fe=$@ $<

clean:
	rm -f inst.exe *.o *.obj *.err

.PHONY: all clean
