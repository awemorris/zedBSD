#!/bin/sh
# WS018 KA-T090 consolidated FAT host runner.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# KA-T090 is the p010 historical checkpoint.  Native VFS superseded its
# executable compatibility fixture in p011, so this maintained entry point
# now delegates unconditionally to KA-T100/KA-T101.
echo "KA-T090: superseded by native FAT KA-T100/KA-T101"
exec "$test_dir/run-fat-native-vfs-host-test.sh"
