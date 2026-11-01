# The VisionFive 2 kernel with the Wave511 decoder brought up at boot: the
# port's configuration with CONFIG_DRIVER_VF2_VPU, which powers the decoder
# and boots its firmware once the root file system is up (ws322-p002).
#   make ZEDBSD_CONFIG=plan/ws322/tests/config-visionfive2-vpu.mk BUILD=build/<dir> build/<dir>/vmunix
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/ci/config-visionfive2.mk
CONFIG_DRIVER_VF2_VPU := y
