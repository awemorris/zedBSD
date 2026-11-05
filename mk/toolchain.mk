#
# Toolchain
#

include mk/noct/noct.mk
include mk/llvm/llvm.mk

toolchain: noct-toolchain llvm-toolchain

toolchain-cached: noct-toolchain-cached llvm-toolchain-cached
