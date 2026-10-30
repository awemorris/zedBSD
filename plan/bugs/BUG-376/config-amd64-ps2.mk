# BUG-376: the image of the PS/2 auxiliary port's checks (ps2-guest.sh): BUG-370's AAT image (no target clang, libcxx
# or emacs) with the evdev probe, so one image serves both checks.
#   AAT_CONFIG=plan/bugs/BUG-376/config-amd64-ps2.mk plan/tools/aat/build-image.sh BUILD
include plan/bugs/BUG-370/config-amd64-aat.mk
ZEDBSD_USER_PROGRAMS += evdev-probe
