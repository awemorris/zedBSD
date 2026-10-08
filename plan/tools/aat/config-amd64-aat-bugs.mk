# q911 (2026-10-08): the AAT image of the UI bugs' reproduction sweep (tests/suites/bugs-ui.suite,
# plan/agents/bug-ui-sweep-20261008.md): the AAT image (config-amd64-aat.mk) with two test programs more.  A test
# image only.
#   AAT_CONFIG=plan/tools/aat/config-amd64-aat-bugs.mk plan/tools/aat/build-image.sh BUILD
include plan/tools/aat/config-amd64-aat.mk
# networkd's stand-in with a Wi-Fi radio (QEMU has none): bugs.settings-wifi-click-tap (BUG-184, BUG-188).
ZEDBSD_USER_PROGRAMS += network-probe
# An X11 client drawing with GLX into its window: bugs.x11-docked-close (BUG-273).
ZEDBSD_USER_PROGRAMS += glxtest
