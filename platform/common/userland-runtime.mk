# Portable runtime library; compilation and ABI flags come from the platform.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# $(1): dynamic tree, $(2): linker, $(3): ABI link flags, $(4): ELF machine.
define ZEDBSD_LIBUTIL_RULE
$(1)/libutil.so: $(1)/obj/src/libc/libutil.o $(1)/libc.so \
 tools/build/check-dynamic-elf.py
	$(2) $(3) -shared -soname libutil.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 $(1)/obj/src/libc/libutil.o -L$(1) -l:libc.so -o $$@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(4) \
 --role shared-library --needed libc.so --soname libutil.so $$@
endef
