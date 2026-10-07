# ws177-p008: the image of the recent list's checks: the Settings image with the input method (Settings, Files) and Text
# Editor, built from the tree under test.  QEMU (T1): plan/ws177/tests/recents-p008.sh.
#   SETTINGS_CONFIG=plan/ws177/tests/config-amd64-p008.mk plan/ws089/tests/build-settings-image.sh BUILD
include plan/ws089/tests/config-amd64-settings-ime.mk
ZEDBSD_USER_PROGRAMS += textedit
