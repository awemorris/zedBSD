# BUG-370: the AAT image (plan/tools/aat/config-amd64-aat.mk) without the target's clang, libcxx and emacs, which the
# Photos scenario does not use (the toolchain's packages are not built for this test).
#   AAT_CONFIG=plan/bugs/BUG-370/config-amd64-aat.mk plan/tools/aat/build-image.sh BUILD
include plan/tools/aat/config-amd64-aat.mk
ZEDBSD_USER_PROGRAMS := $(filter-out clang libcxx emacs,$(ZEDBSD_USER_PROGRAMS))
