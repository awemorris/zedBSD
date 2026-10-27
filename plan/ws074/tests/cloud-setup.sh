#!/bin/sh
# ws074: prepares a fresh Ubuntu 24.04 cloud container (Claude Code on the web) for the host tests
# and for comparing browser's drawing with Chromium's.
#
#   sh plan/ws074/tests/cloud-setup.sh
#
# What it does:
#   1. installs the host packages host-build.sh and the runners need (apt, sudo without a password);
#   2. puts a `chromium` command on PATH (/usr/local/bin/chromium) that runs the Chrome for Testing
#      headless shell the container ships under /opt/pw-browsers (dl.google.com and the snap of
#      Ubuntu's chromium are not reachable there, and the full Chrome's --headless --screenshot
#      hangs without a D-Bus);
#   3. writes build/ws074-chrome/fonts.conf (chrome-fonts.sh) so Chromium draws with browser's fonts.
# Build with clang afterwards (CC=clang sh plan/ws074/tests/host-build.sh plain): Ubuntu's gcc
# defines _FORTIFY_SOURCE and rejects the engine's (void)write(...) under -Werror.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."

sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq clang libvulkan-dev mesa-vulkan-drivers \
	fontconfig python3-pil >/dev/null

shell=
for candidate in /opt/pw-browsers/chromium_headless_shell-*/chrome-headless-shell-linux64/chrome-headless-shell; do
	if [ -x "$candidate" ]; then
		shell=$candidate
	fi
done
if [ -z "$shell" ]; then
	echo "cloud-setup: no chrome-headless-shell under /opt/pw-browsers" >&2
	exit 1
fi
# The wrapper lives under build/; /usr/local/bin/chromium is only a link to it (ln -sf replaces the
# link itself, never writes through it into a browser's binary).
mkdir -p build/ws074-chrome/bin
cat > build/ws074-chrome/bin/chromium <<EOF
#!/bin/sh
# Written by plan/ws074/tests/cloud-setup.sh: the runners' \`chromium\` is Chrome for Testing's headless shell.
exec "$shell" "\$@"
EOF
chmod 755 build/ws074-chrome/bin/chromium
sudo ln -sf "$(pwd)/build/ws074-chrome/bin/chromium" /usr/local/bin/chromium
chromium --version

sh plan/ws074/tests/chrome-fonts.sh
