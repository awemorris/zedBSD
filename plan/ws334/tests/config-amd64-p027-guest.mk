# ws334-p027: the guest image for the browser's window on zedBSD amd64 (the Vulkan way, --no-gpu's CPU way,
# localStorage across restarts, and files, terminal and settings): the WS074 browser image
# (plan/ws074/tests/config-amd64-browser.mk) with the compositor's test-only capture and keiland-shot.
# Build and run:
#   sh plan/ws334/tests/p027-guest.sh [OUTDIR]
include plan/ws074/tests/config-amd64-browser.mk
ZEDBSD_TEST_SCREEN_CAPTURE := y
ZEDBSD_USER_PROGRAMS += keiland-shot settings
