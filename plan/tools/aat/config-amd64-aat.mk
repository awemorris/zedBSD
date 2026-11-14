# ws173-p003: the AAT image (Agent Acceptance Test): the Latitude 5330 UAT image (plan/tools/aat/config-amd64-uat.mk,
# the release's configuration as a development build) with the test input injector and the screen's capture, so that
# plan/tools/aat/aat drives it over SSH.  A test image only: neither goes into a product's configuration.
# Build (root's SSH key and the host keys of the guest harness, not its net.conf, which would replace the machine's
# network):
#   plan/tools/aat/build-image.sh build/aat
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/aat/config-amd64-uat.mk
# /dev/input-inject, and aat-input (ws173-p001, P1): its server holds a mouse, an absolute pointer and a keyboard.
override CONFIG_INPUT_TEST_INJECT := y
override CONFIG_USERLAND_PROGRAMS += aat-input
# The compositor's test-only capture (ws173-p002, P1: userland/desktop/wayland/shot.c) and its client keiland-shot.
ZEDBSD_TEST_SCREEN_CAPTURE := y
override CONFIG_USERLAND_PROGRAMS += keiland-shot
# The desktop's settings from the command line (userland/tests/keiland-settings): desktop.language.lock-japanese sets
# ui.language with it (q809).
override CONFIG_USERLAND_PROGRAMS += keiland-settings
# The test pen (peninject, WS079): apps.notes.draw-tools draws strokes with a pressure (ws216-p007; 2026-10-13 Q1: in
# the standard AAT image so that the full suite runs it).
override CONFIG_USERLAND_PROGRAMS += peninject
