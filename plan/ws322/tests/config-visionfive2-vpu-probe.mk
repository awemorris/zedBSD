# The VisionFive 2 test kernel of the Wave511 decoder's probe: the port's
# configuration with /dev/vf2-vpu-probe, whose read powers the decoder, boots
# its firmware, decodes the streams under /var/vpu-probe and powers it off
# (ws322-p002).  The decoder is not brought up at boot.
#   make ZEDBSD_CONFIG=plan/ws322/tests/config-visionfive2-vpu-probe.mk BUILD=build/<dir> build/<dir>/vmunix
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/ci/config-visionfive2.mk
CONFIG_DRIVER_VF2_VPU_PROBE := y
