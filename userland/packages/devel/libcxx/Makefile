# zedBSD userland package: the LLVM C++ runtime.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# libunwind, libc++abi and libc++ come from the same LLVM release the compiler
# does, so there is one version of LLVM in the tree and one licence to carry.
# They are built together through the runtimes tree, which is how LLVM expects
# a set of runtimes to be built for a target that is not the host.
#
# They are installed to /usr/lib rather than /lib: /lib holds what the base
# system itself needs, and the C++ runtime arrives with a package.  The loader
# searches both (src/rtld/rtld.c).
ZEDBSD_LIBCXX_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_LIBCXX_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_LIBCXX_MAKEFILE)))
include $(ZEDBSD_LIBCXX_DIR)/../../../base/package.mk
include $(ZEDBSD_LIBCXX_DIR)/../../external.mk
include $(ZEDBSD_LIBCXX_DIR)/../../../../toolchain/llvm/version.mk

# The compiler's own source is the source here; nothing is fetched again.
# The package patches a copy of it (external.mk), never the toolchain's tree.
ZEDBSD_LIBCXX_PATCHES := $(sort $(wildcard \
	$(abspath $(ZEDBSD_LIBCXX_DIR))/patches/*.patch))
ZEDBSD_LIBCXX_PATCH_LEVEL := zedbsd1
ZEDBSD_LIBCXX_SOURCE := $(ZEDBSD_EXTERNAL_WORKROOT)/libcxx/src
ZEDBSD_LIBCXX_SOURCE_STAMP := \
	$(ZEDBSD_LIBCXX_SOURCE)/.zedbsd-libcxx-source-$(ZEDBSD_LLVM_VERSION)-$(ZEDBSD_LLVM_PATCH_LEVEL)-$(ZEDBSD_LIBCXX_PATCH_LEVEL)

ZEDBSD_LIBCXX_BUILD := $(ZEDBSD_EXTERNAL_WORKROOT)/libcxx/build
ZEDBSD_LIBCXX_STAGE := $(ZEDBSD_EXTERNAL_WORKROOT)/libcxx/stage
ZEDBSD_LIBCXX_CONFIGURED := $(ZEDBSD_LIBCXX_BUILD)/.zedbsd-configured
ZEDBSD_LIBCXX_STAGED := $(ZEDBSD_LIBCXX_STAGE)/.zedbsd-staged

ZEDBSD_LIBCXX_UNWIND := $(ZEDBSD_LIBCXX_STAGE)/usr/lib/libunwind.so
ZEDBSD_LIBCXX_CXXABI := $(ZEDBSD_LIBCXX_STAGE)/usr/lib/libc++abi.so
ZEDBSD_LIBCXX_CXX := $(ZEDBSD_LIBCXX_STAGE)/usr/lib/libc++.so
ZEDBSD_LIBCXX_LICENSE := $(ZEDBSD_LIBCXX_SOURCE)/libcxx/LICENSE.TXT

# What each option is for:
#   LIBCXX_HAS_MUSL_LIBC   is off: it does not mean "not glibc", it means the
#                          libc is musl, and libc++ then reaches for musl's
#                          own internal headers, which are not here.
#   -z defs is removed     the runtime loader supplies __tls_get_addr, and the
#                          shared C library already leaves it undefined for
#                          the same reason; a link that forbids an undefined
#                          symbol cannot express that.
#   *_ENABLE_SHARED/STATIC only shared objects are installed.
#   LIBCXXABI_USE_LLVM_UNWINDER ties the ABI library to libunwind rather than
#                          to a system unwinder, which zedBSD does not have.
#   *_ENABLE_THREADS       zedBSD's libc carries pthreads.
#   LIBCXX_HAS_*_LIB       none of these separate libraries exist here.
# _LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE: the ctype facet classifies characters
# from a table that each system supplies in its own way -- a BSD rune table,
# glibc's __ctype_b_loc, and so on.  zedBSD has none of those, so libc++ uses
# the table it carries for exactly this case.
#
# It is a site define rather than a flag on this build, because the headers
# read it too: a program that includes <locale> has to agree with the library
# about where the table comes from.  A site define is written into
# __config_site, which every consumer of these headers includes, so the
# agreement holds without every such program being told to repeat the flag.
ZEDBSD_LIBCXX_CMAKE_OPTIONS := \
	-DLIBCXX_EXTRA_SITE_DEFINES=_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE \
	-DLIBCXX_ENABLE_ABI_LINKER_SCRIPT=OFF \
	-DLIBCXX_HAS_GCC_LIB=OFF \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DLLVM_ENABLE_RUNTIMES='libunwind;libcxxabi;libcxx' \
	-DLLVM_TARGETS_TO_BUILD=$(ZEDBSD_EXTERNAL_LLVM_TARGET) \
	-DLIBCXX_HAS_MUSL_LIBC=OFF \
	-DLIBCXX_ENABLE_SHARED=ON \
	-DLIBCXX_ENABLE_STATIC=OFF \
	-DLIBCXX_ENABLE_THREADS=ON \
	-DLIBCXX_HAS_PTHREAD_API=ON \
	-DLIBCXX_ENABLE_FILESYSTEM=ON \
	-DLIBCXX_ENABLE_LOCALIZATION=ON \
	-DLIBCXX_USE_COMPILER_RT=ON \
	-DLIBCXX_CXX_ABI=libcxxabi \
	-DLIBCXXABI_ENABLE_SHARED=ON \
	-DLIBCXXABI_ENABLE_STATIC=OFF \
	-DLIBCXXABI_ENABLE_THREADS=ON \
	-DLIBCXXABI_USE_LLVM_UNWINDER=ON \
	-DLIBCXXABI_USE_COMPILER_RT=ON \
	-DLIBUNWIND_ENABLE_SHARED=ON \
	-DLIBUNWIND_ENABLE_STATIC=OFF \
	-DLIBUNWIND_ENABLE_THREADS=ON \
	-DLIBUNWIND_USE_COMPILER_RT=ON \
	-DLIBCXX_HAS_GCC_S_LIB=OFF \
	-DLIBCXX_HAS_ATOMIC_LIB=OFF \
	-DLIBCXXABI_HAS_GCC_S_LIB=OFF \
	-DLIBUNWIND_HAS_GCC_S_LIB=OFF \
	-DLIBCXX_INCLUDE_BENCHMARKS=OFF \
	-DLIBCXX_INCLUDE_TESTS=OFF \
	-DLIBCXXABI_INCLUDE_TESTS=OFF \
	-DLIBUNWIND_INCLUDE_TESTS=OFF

# The options below are read when the build tree is configured, not when it is
# built, so this file is one of the inputs: changing an option has to start a
# new configuration rather than build the old one again.
# The record names the LLVM release and both patch levels, so a change to
# any of them makes a new patched copy.
$(ZEDBSD_LIBCXX_SOURCE_STAMP): $(ZEDBSD_EXTERNAL_LLVM_VERIFIED) \
	$(ZEDBSD_LIBCXX_PATCHES)
	$(call ZEDBSD_EXTERNAL_LLVM_COPY,$(ZEDBSD_LIBCXX_SOURCE),$(ZEDBSD_LIBCXX_PATCHES))
	@touch '$@'

$(ZEDBSD_LIBCXX_CONFIGURED): $(ZEDBSD_LIBCXX_SOURCE_STAMP) \
	$(ZEDBSD_EXTERNAL_CROSS_STAMP) $(ZEDBSD_LIBCXX_MAKEFILE)
	@rm -rf '$(ZEDBSD_LIBCXX_BUILD)'
	@mkdir -p '$(ZEDBSD_LIBCXX_BUILD)'
	cmake -S '$(ZEDBSD_LIBCXX_SOURCE)/runtimes' -B '$(ZEDBSD_LIBCXX_BUILD)' \
		-G Ninja \
		--toolchain '$(ZEDBSD_EXTERNAL_CROSS_DIR)/zedbsd.cmake' \
		$(ZEDBSD_LIBCXX_CMAKE_OPTIONS)
	@touch '$@'

$(ZEDBSD_LIBCXX_STAGED): $(ZEDBSD_LIBCXX_CONFIGURED)
	@rm -rf '$(ZEDBSD_LIBCXX_STAGE)'
	@sed -i 's/-Wl,-z,defs//g' '$(ZEDBSD_LIBCXX_BUILD)/build.ninja'
	cmake --build '$(ZEDBSD_LIBCXX_BUILD)' --parallel $(ZEDBSD_BUILD_JOBS)
	cmake --install '$(ZEDBSD_LIBCXX_BUILD)' \
		--prefix '$(ZEDBSD_LIBCXX_STAGE)/usr'
	@touch '$@'

$(ZEDBSD_LIBCXX_UNWIND) $(ZEDBSD_LIBCXX_CXXABI) $(ZEDBSD_LIBCXX_CXX): \
	$(ZEDBSD_LIBCXX_STAGED)
	@test -f '$@'

$(ZEDBSD_LIBCXX_LICENSE): $(ZEDBSD_LIBCXX_SOURCE_STAMP)
	@test -f '$@'

.PHONY: libcxx
libcxx: $(ZEDBSD_LIBCXX_STAGED)
	@:

$(eval $(call ZEDBSD_USERLAND_PACKAGE,libcxx,LLVM C++ runtime,*,n,package,,y,\
	packages/devel,devel/libcxx,))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter libcxx,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_LIBCXX_UNWIND) $(ZEDBSD_LIBCXX_CXXABI) $(ZEDBSD_LIBCXX_CXX) \
	$(ZEDBSD_LIBCXX_LICENSE))
# The loader opens a dependency by the name recorded in DT_NEEDED, which is
# the SONAME, so that is the name each library is installed under.  The
# unversioned link the link step looks for is among the development files
# below.
ZEDBSD_PACKAGE_FILES += $(if $(filter libcxx,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/lib/libunwind.so.1=$(ZEDBSD_LIBCXX_UNWIND) \
	--file /usr/lib/libc++abi.so.1=$(ZEDBSD_LIBCXX_CXXABI) \
	--file /usr/lib/libc++.so.1=$(ZEDBSD_LIBCXX_CXX) \
	--file /usr/share/licenses/libcxx/LICENSE.TXT=$(ZEDBSD_LIBCXX_LICENSE))

# The development files: the C++ library's headers (include/c++/v1), the
# unwinder's, and the unversioned names the linker looks for.
$(eval $(call ZEDBSD_EXTERNAL_DEVELOPMENT,libcxx,$(ZEDBSD_LIBCXX_STAGE),\
	$(addprefix include/,__libunwind_config.h libunwind.h unwind.h unwind_arm_ehabi.h unwind_itanium.h),\
	include/c++,\
	libunwind.so.1 libc++abi.so.1 libc++.so.1))
