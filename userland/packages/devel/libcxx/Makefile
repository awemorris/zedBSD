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
LIBCXX_MAKEFILE := $(lastword $(MAKEFILE_LIST))
LIBCXX_DIR := $(patsubst %/,%,$(dir $(LIBCXX_MAKEFILE)))
include $(LIBCXX_DIR)/../../../base/package.mk
include $(LIBCXX_DIR)/../../external.mk
include $(LIBCXX_DIR)/../../../../toolchain/llvm/version.mk

# The compiler's own source is the source here; nothing is fetched again.
# The package patches a copy of it (external.mk), never the toolchain's tree.
LIBCXX_PATCHES := $(sort $(wildcard \
	$(abspath $(LIBCXX_DIR))/patches/*.patch))
LIBCXX_PATCH_LEVEL := zedbsd1
LIBCXX_SOURCE := $(EXTERNAL_WORKROOT)/libcxx/src
LIBCXX_SOURCE_STAMP := \
	$(LIBCXX_SOURCE)/.zedbsd-libcxx-source-$(LLVM_VERSION)-$(LLVM_PATCH_LEVEL)-$(LIBCXX_PATCH_LEVEL)

LIBCXX_BUILD := $(EXTERNAL_WORKROOT)/libcxx/build
LIBCXX_STAGE := $(EXTERNAL_WORKROOT)/libcxx/stage
LIBCXX_CONFIGURED := $(LIBCXX_BUILD)/.zedbsd-configured
LIBCXX_STAGED := $(LIBCXX_STAGE)/.zedbsd-staged

LIBCXX_UNWIND := $(LIBCXX_STAGE)/usr/lib/libunwind.so
LIBCXX_CXXABI := $(LIBCXX_STAGE)/usr/lib/libc++abi.so
LIBCXX_CXX := $(LIBCXX_STAGE)/usr/lib/libc++.so
LIBCXX_LICENSE := $(LIBCXX_SOURCE)/libcxx/LICENSE.TXT

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
LIBCXX_CMAKE_OPTIONS := \
	-DLIBCXX_EXTRA_SITE_DEFINES=_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE \
	-DLIBCXX_ENABLE_ABI_LINKER_SCRIPT=OFF \
	-DLIBCXX_HAS_GCC_LIB=OFF \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DLLVM_ENABLE_RUNTIMES='libunwind;libcxxabi;libcxx' \
	-DLLVM_TARGETS_TO_BUILD=$(EXTERNAL_LLVM_TARGET) \
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
$(LIBCXX_SOURCE_STAMP): $(EXTERNAL_LLVM_VERIFIED) \
	$(LIBCXX_PATCHES)
	$(call EXTERNAL_LLVM_COPY,$(LIBCXX_SOURCE),$(LIBCXX_PATCHES))
	@touch '$@'

$(LIBCXX_CONFIGURED): $(LIBCXX_SOURCE_STAMP) \
	$(EXTERNAL_CROSS_STAMP) $(LIBCXX_MAKEFILE)
	@rm -rf '$(LIBCXX_BUILD)'
	@mkdir -p '$(LIBCXX_BUILD)'
	cmake -S '$(LIBCXX_SOURCE)/runtimes' -B '$(LIBCXX_BUILD)' \
		-G Ninja \
		--toolchain '$(EXTERNAL_CROSS_DIR)/zedbsd.cmake' \
		$(LIBCXX_CMAKE_OPTIONS)
	@touch '$@'

$(LIBCXX_STAGED): $(LIBCXX_CONFIGURED)
	@rm -rf '$(LIBCXX_STAGE)'
	@sed -i 's/-Wl,-z,defs//g' '$(LIBCXX_BUILD)/build.ninja'
	cmake --build '$(LIBCXX_BUILD)' --parallel $(BUILD_JOBS)
	cmake --install '$(LIBCXX_BUILD)' \
		--prefix '$(LIBCXX_STAGE)/usr'
	@touch '$@'

$(LIBCXX_UNWIND) $(LIBCXX_CXXABI) $(LIBCXX_CXX): \
	$(LIBCXX_STAGED)
	@test -f '$@'

$(LIBCXX_LICENSE): $(LIBCXX_SOURCE_STAMP)
	@test -f '$@'

.PHONY: libcxx
libcxx: $(LIBCXX_STAGED)
	@:

$(eval $(call USERLAND_PACKAGE,libcxx,LLVM C++ runtime,*,n,package,,y,\
	packages/devel,devel/libcxx,))

PACKAGE_INPUTS += $(if $(filter libcxx,$(USER_PROGRAMS)),\
	$(LIBCXX_UNWIND) $(LIBCXX_CXXABI) $(LIBCXX_CXX) \
	$(LIBCXX_LICENSE))
# The loader opens a dependency by the name recorded in DT_NEEDED, which is
# the SONAME, so that is the name each library is installed under.  The
# unversioned link the link step looks for is among the development files
# below.
PACKAGE_FILES += $(if $(filter libcxx,$(USER_PROGRAMS)),\
	--file /usr/lib/libunwind.so.1=$(LIBCXX_UNWIND) \
	--file /usr/lib/libc++abi.so.1=$(LIBCXX_CXXABI) \
	--file /usr/lib/libc++.so.1=$(LIBCXX_CXX) \
	--file /usr/share/licenses/libcxx/LICENSE.TXT=$(LIBCXX_LICENSE))

# The development files: the C++ library's headers (include/c++/v1), the
# unwinder's, and the unversioned names the linker looks for.
$(eval $(call EXTERNAL_DEVELOPMENT,libcxx,$(LIBCXX_STAGE),\
	$(addprefix include/,__libunwind_config.h libunwind.h unwind.h unwind_arm_ehabi.h unwind_itanium.h),\
	include/c++,\
	libunwind.so.1 libc++abi.so.1 libc++.so.1))
