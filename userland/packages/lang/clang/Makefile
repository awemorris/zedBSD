# zedBSD userland package: the clang compiler.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The compiler the target runs is built from the same LLVM release the cross
# compiler is, so there is one version of LLVM in the tree and one licence to
# carry.  Nothing is fetched again: the source tree the toolchain extracted is
# the source here, as it is for the C++ runtime beside it.
#
# LLVM is built for the host during the toolchain build, and that build leaves
# behind the generators LLVM uses to write parts of its own source.  Those
# generators must run on the machine doing the building, not on the target, so
# they are taken from there rather than built twice.
#
# The tools are linked against one shared LLVM rather than each carrying its
# own copy: a dozen statically linked LLVM tools would be larger than the disk
# they are meant to sit on.
CLANG_MAKEFILE := $(lastword $(MAKEFILE_LIST))
CLANG_DIR := $(patsubst %/,%,$(dir $(CLANG_MAKEFILE)))
include $(CLANG_DIR)/../../../base/package.mk
include $(CLANG_DIR)/../../external.mk
include $(CLANG_DIR)/../../../../toolchain/llvm/version.mk

# The cross compiler's own source; the toolchain build extracts it.  This
# package patches a copy of it (external.mk), never the toolchain's tree.
CLANG_SOURCE := $(EXTERNAL_WORKROOT)/clang/src

# The host build tree, for the generators.  It belongs to the toolchain build
# rather than to this package, which is why it is named and not made here.
CLANG_NATIVE_DIR := $(abspath $(EXTERNAL_ROOT)/build/llvm-build/bin)

CLANG_PATCHES := $(sort $(wildcard \
	$(abspath $(CLANG_DIR))/patches/*.patch))
CLANG_PATCH_LEVEL := zedbsd1
CLANG_SOURCE_STAMP := \
	$(CLANG_SOURCE)/.zedbsd-clang-source-$(LLVM_VERSION)-$(LLVM_PATCH_LEVEL)-$(CLANG_PATCH_LEVEL)

CLANG_BUILD := $(EXTERNAL_WORKROOT)/clang/build
CLANG_STAGE := $(EXTERNAL_WORKROOT)/clang/stage
CLANG_CONFIGURED := $(CLANG_BUILD)/.zedbsd-configured
CLANG_STAGED := $(CLANG_STAGE)/.zedbsd-staged

# The C++ runtime the compiler is linked against.  It is a package like this
# one, so it is named by the path it stages to rather than built from here.
CLANG_LIBCXX_STAGE := $(EXTERNAL_WORKROOT)/libcxx/stage/usr
CLANG_LIBCXX_STAMP := \
	$(EXTERNAL_WORKROOT)/libcxx/stage/.zedbsd-staged

# Where the C++ runtime put its headers and libraries.  It stages them for
# the image rather than into the sysroot, so a package that is written in
# C++ has to be told where they are; the cross wrapper carries the C
# library and knows nothing about this one.
#
# -nostdinc++ keeps the compiler from also offering its own copy, which is
# the host's and belongs to a different C library.
CLANG_CXX_FLAGS := \
	-nostdinc++ -isystem $(CLANG_LIBCXX_STAGE)/include/c++/v1
CLANG_LINK_FLAGS := \
	-L$(CLANG_LIBCXX_STAGE)/lib -lc++ -lc++abi -lunwind

CLANG_BINDIR := $(CLANG_STAGE)/usr/bin
CLANG_LIBDIR := $(CLANG_STAGE)/usr/lib
CLANG_CLANG := $(CLANG_BINDIR)/clang
CLANG_LLD := $(CLANG_BINDIR)/lld
CLANG_LICENSE := $(CLANG_SOURCE)/llvm/LICENSE.TXT

# What each option is for:
#   LLVM_NATIVE_TOOL_DIR   the generators that must run while building.
#   *_LINK_*_DYLIB         one shared LLVM and one shared clang, rather than a
#                          copy of each inside every tool.
#   LLVM_ENABLE_LIBCXX is off, and the runtime is named above instead.
#                          Turning it on adds -stdlib=libc++, which decides
#                          both the headers and the libraries; the headers
#                          are already settled by the two flags above, so
#                          what is left of it applies only to the link.  A
#                          compilation then reports it as an argument it did
#                          not use, and cmake reads that as the flag it was
#                          testing being unsupported -- which is how -fPIC
#                          came to be dropped from a build that had asked
#                          for it.
#   LLVM_DEFAULT_TARGET_TRIPLE  what the compiler builds for when not told.
#   LLVM_HOST_TRIPLE       what it is running on, which is not what cmake
#                          would work out by asking this machine.
#   PIC is left on         it is what a shared object needs, and the tools
#                          share one.  That an executable here is loaded at
#                          a fixed address is a separate question, settled
#                          by the driver at the link rather than by how the
#                          library objects were compiled.
#   The ZLIB/ZSTD/XML/TERMINFO/EDIT/FFI options name libraries this system
#   does not have; left to look for them, cmake finds the host's.
#   LLVM_INCLUDE_*=OFF     nothing here runs LLVM's own test suites.
CLANG_CMAKE_OPTIONS := \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DCMAKE_CXX_FLAGS='$(CLANG_CXX_FLAGS)' \
	-DCMAKE_EXE_LINKER_FLAGS='$(CLANG_LINK_FLAGS)' \
	-DCMAKE_SHARED_LINKER_FLAGS='$(CLANG_LINK_FLAGS)' \
	-DCMAKE_MODULE_LINKER_FLAGS='$(CLANG_LINK_FLAGS)' \
	-DLLVM_NATIVE_TOOL_DIR='$(CLANG_NATIVE_DIR)' \
	-DLLVM_ENABLE_PROJECTS='clang;lld;lldb' \
	-DLLDB_TABLEGEN='$(CLANG_NATIVE_DIR)/lldb-tblgen' \
	-DLLDB_ENABLE_PYTHON=OFF \
	-DLLDB_ENABLE_LUA=OFF \
	-DLLDB_ENABLE_LIBEDIT=OFF \
	-DLLDB_ENABLE_CURSES=OFF \
	-DLLDB_ENABLE_LZMA=OFF \
	-DLLDB_ENABLE_LIBXML2=OFF \
	-DLLDB_ENABLE_SWIG=OFF \
	-DLLDB_INCLUDE_TESTS=OFF \
	-DLLDB_BUILD_FRAMEWORK=OFF \
	-DLLVM_TARGETS_TO_BUILD=$(EXTERNAL_LLVM_TARGET) \
	-DLLVM_DEFAULT_TARGET_TRIPLE='$(EXTERNAL_TRIPLE)' \
	-DLLVM_HOST_TRIPLE='$(EXTERNAL_TRIPLE)' \
	-DLLVM_TARGET_ARCH=$(EXTERNAL_LLVM_TARGET) \
	-DLLVM_BUILD_LLVM_DYLIB=ON \
	-DLLVM_LINK_LLVM_DYLIB=ON \
	-DCLANG_LINK_CLANG_DYLIB=ON \
	-DLLVM_ENABLE_ZLIB=OFF \
	-DLLVM_ENABLE_ZSTD=OFF \
	-DLLVM_ENABLE_LIBXML2=OFF \
	-DLLVM_ENABLE_TERMINFO=OFF \
	-DLLVM_ENABLE_LIBEDIT=OFF \
	-DLLVM_ENABLE_FFI=OFF \
	-DLLVM_ENABLE_LIBPFM=OFF \
	-DLLVM_ENABLE_BACKTRACES=OFF \
	-DLLVM_ENABLE_CRASH_OVERRIDES=OFF \
	-DLLVM_ENABLE_PLUGINS=OFF \
	-DLLVM_INCLUDE_TESTS=OFF \
	-DLLVM_INCLUDE_EXAMPLES=OFF \
	-DLLVM_INCLUDE_BENCHMARKS=OFF \
	-DLLVM_INCLUDE_DOCS=OFF \
	-DLLVM_INCLUDE_UTILS=OFF \
	-DCLANG_INCLUDE_TESTS=OFF \
	-DCLANG_INCLUDE_DOCS=OFF \
	-DCLANG_ENABLE_STATIC_ANALYZER=OFF \
	-DCLANG_ENABLE_ARCMT=OFF \
	-DLLD_INCLUDE_TESTS=OFF \
	-DLLVM_TOOL_LLVM_SHLIB_BUILD=ON

# The copy is made from the tree the toolchain verified against the release
# and its own patch.  The host generators are built from that tree too (with
# the binary cache, here: llvm-native-tools).  The record names the LLVM
# release and both patch levels, so a change to any of them makes a new copy.
$(CLANG_SOURCE_STAMP): $(EXTERNAL_LLVM_VERIFIED) \
	$(CLANG_PATCHES) \
	$(if $(TOPLEVEL_BUILD),| $(LLVM_NATIVE_STAMP))
	$(call EXTERNAL_LLVM_COPY,$(CLANG_SOURCE),$(CLANG_PATCHES))
	@touch '$@'

# The generators are the toolchain build's output.  Saying so here turns a
# missing one into a sentence rather than into a cmake error about a path.
$(CLANG_CONFIGURED): $(CLANG_SOURCE_STAMP) \
	$(CLANG_LIBCXX_STAMP) \
	$(EXTERNAL_CROSS_STAMP) $(CLANG_MAKEFILE)
	@test -x '$(CLANG_NATIVE_DIR)/lldb-tblgen' || { \
		echo "clang: $(CLANG_NATIVE_DIR)/lldb-tblgen is missing;" >&2; \
		echo "clang: build the toolchain first (make toolchain)." >&2; \
		exit 1; }
	@test -x '$(CLANG_NATIVE_DIR)/llvm-tblgen' || { \
		echo "clang: $(CLANG_NATIVE_DIR)/llvm-tblgen is missing;" >&2; \
		echo "clang: build the toolchain first (make toolchain)." >&2; \
		exit 1; }
	@rm -rf '$(CLANG_BUILD)'
	@mkdir -p '$(CLANG_BUILD)'
	cmake -S '$(CLANG_SOURCE)/llvm' -B '$(CLANG_BUILD)' \
		-G Ninja \
		--toolchain '$(EXTERNAL_CROSS_DIR)/zedbsd.cmake' \
		$(CLANG_CMAKE_OPTIONS)
	@touch '$@'

# Two link options are taken out of the generated build.
#
# -z defs refuses a shared object that leaves a symbol undefined.  The
# runtime loader supplies __tls_get_addr, so a library that uses thread
# local storage leaves it undefined on purpose, and the shared C library
# does the same for the same reason.
#
# --version-script gives the shared library's symbols version nodes.  This
# loader resolves a symbol by name alone and has no notion of a version, so
# an object carrying one states a promise the system cannot keep; the link
# contract rejects it for that reason.
#
# Both are added by LLVM itself, after this file has said what the link
# flags are, so they are removed from the generated build rather than
# argued with -- which is how the C++ runtime beside this package deals
# with the first of them.
$(CLANG_STAGED): $(CLANG_CONFIGURED)
	@rm -rf '$(CLANG_STAGE)'
	@sed -i -e 's/-Wl,-z,defs//g' \
		-e 's|-Wl,--version-script,[^ ]*||g' \
		'$(CLANG_BUILD)/build.ninja'
	cmake --build '$(CLANG_BUILD)' --parallel $(BUILD_JOBS)
	cmake --install '$(CLANG_BUILD)' \
		--prefix '$(CLANG_STAGE)/usr'
	@touch '$@'

$(CLANG_CLANG) $(CLANG_LLD): $(CLANG_STAGED)
	@test -f '$@'

$(CLANG_LICENSE): $(CLANG_SOURCE_STAMP)
	@test -f '$@'

.PHONY: clang
clang: $(CLANG_STAGED)
	@:

# The compiler needs the C++ runtime it was built against, so choosing it
# chooses that too.
$(eval $(call USERLAND_PACKAGE,clang,clang compiler,*,n,package,,y,\
	packages/lang,lang/clang,devel/libcxx))

# What the image carries.
#
# The build produces every tool LLVM has; almost none of them belong on a
# machine whose purpose is to compile a program.  What is installed is the
# compiler, the linker it drives, and the few tools that read and change an
# object file, which is the set the plan named.
#
# clang decides what language it is compiling from the name it was run as,
# so clang++ is a link to it under the other name.
CLANG_TOOLS := ld.lld llvm-ar llvm-ranlib llvm-nm llvm-objcopy \
	llvm-objdump llvm-readelf llvm-strip lldb lldb-server
CLANG_LIBS := libLLVM.so.23.1 libclang-cpp.so.23.1 liblldb.so.23.1

# The headers the compiler supplies for itself -- stddef.h, stdarg.h and the
# rest, which belong to the compiler rather than to the C library.
#
# Which headers there are is known only once LLVM has installed them into the
# stage, so the list is read from the stage when it is used, and the root
# tree's recipe uses it after the stage is made (BUG-087).  find rather than
# $(wildcard): make keeps what it has read of a directory, and a directory
# read while parsing would not show the headers installed later.
CLANG_RESOURCE_DIR := $(CLANG_LIBDIR)/clang/23/include
CLANG_RESOURCE_HEADERS = $(shell test -d '$(CLANG_RESOURCE_DIR)' && \
	find '$(CLANG_RESOURCE_DIR)' -maxdepth 1 -type f -name '*.h' | LC_ALL=C sort)

# What the compiler needs besides itself to turn a source file into a program
# that runs here: the C library's headers, the start file a link begins with,
# the compiler runtime the code generator calls into, and the shared C library
# the link resolves -lc against.  The cross build reads all four from the
# sysroot; a compiler running on the machine has no sysroot to read, so the
# same files are placed where an empty sysroot leaves the driver looking --
# /usr/include and /usr/lib.
#
# cc and ld are links to clang and ld.lld, the names build scripts
# (autoconf, libtool) look for.  A link rather than a script that runs the
# tool: a compile runs cc hundreds of times in a configure, and a script
# costs a shell start on every one.
#
# clang++.cfg, beside the program, is the configuration file the driver reads
# when it runs as clang++: the target's driver adds no C++ library, so the
# file names libc++'s headers and the libraries a C++ link needs.
#
# The shared C library is copied rather than linked to.  The copy costs a
# megabyte and is not a second library -- the loader answers DT_NEEDED by
# soname, so a program linked against this copy loads the /lib one it was
# going to load anyway.
#
# The headers are named from the sysroot's manifest (toolchain/llvm/sysroot.mk)
# rather than found in the sysroot: the sysroot is remade in the same make
# that copies them when a header is renamed, and a list found in the old one
# names files the new one no longer has (BUG-084).  Deferred, because that
# file is read after this one.
CLANG_SYSROOT_INCLUDE := $(EXTERNAL_SYSROOT)/usr/include
CLANG_SYSROOT_HEADERS = \
	$(filter %.h,$(SYSROOT_INCLUDE_NAMES))
CLANG_LINK_OBJECTS := crt1.o libclang_rt.builtins.a
#
# The copy is the C library of this build, the one /lib holds: the external
# dynamic directory belongs to the default build directory, and taking it
# from there gave an image built elsewhere a library older than its own /lib.
CLANG_RUNTIME_LIBC := $(strip $(if $(TOPLEVEL_BUILD),$(BUILD)/dynamic/libc.so,$(EXTERNAL_DYNAMIC)/libc.so))

PACKAGE_INPUTS += $(if $(filter clang,$(USER_PROGRAMS)),\
	$(CLANG_STAGED) $(CLANG_LICENSE) $(CLANG_RUNTIME_LIBC))

PACKAGE_FILES += $(if $(filter clang,$(USER_PROGRAMS)),\
	--file /usr/bin/clang=$(CLANG_BINDIR)/clang-23 \
	$(foreach tool,$(CLANG_TOOLS),\
	 --file /usr/bin/$(tool)=$(CLANG_BINDIR)/$(tool)) \
	$(foreach library,$(CLANG_LIBS),\
	 --file /usr/lib/$(library)=$(CLANG_LIBDIR)/$(library)) \
	$(foreach header,$(CLANG_RESOURCE_HEADERS),\
	 --file /usr/lib/clang/23/include/$(notdir $(header))=$(header)) \
	$(foreach header,$(CLANG_SYSROOT_HEADERS),\
	 --file /usr/include/$(header)=$(CLANG_SYSROOT_INCLUDE)/$(header)) \
	$(foreach object,$(CLANG_LINK_OBJECTS),\
	 --file /usr/lib/$(object)=$(EXTERNAL_SYSROOT)/usr/lib/$(object)) \
	--file /usr/lib/libc.so=$(CLANG_RUNTIME_LIBC) \
	--file /usr/bin/clang++.cfg=$(abspath $(CLANG_DIR))/clang++.cfg \
	--file /usr/share/licenses/clang/LICENSE.TXT=$(CLANG_LICENSE) \
	--mode /usr/bin/clang=0755 \
	$(foreach tool,$(CLANG_TOOLS),--mode /usr/bin/$(tool)=0755) \
	$(foreach library,$(CLANG_LIBS),--mode /usr/lib/$(library)=0755) \
	--mode /usr/lib/libc.so=0755 \
	--mode /usr/bin/clang++.cfg=0644)
PACKAGE_LINKS += $(if $(filter clang,$(USER_PROGRAMS)),\
	/usr/bin/clang++=clang /usr/bin/cc=clang /usr/bin/ld=ld.lld)
