#!/bin/sh
# ws074: prepares a Debian 13 host for the host tests and for measuring Chromium on the Interop 2025
# selection (interop2025-chrome-gap.py --kind chrome).  Nothing outside the worktree's build/ is
# changed except the apt packages.
#
#   sh plan/ws074/tests/debian-setup.sh
#   export NODE_PATH=$PWD/build/ws074-chrome/node/node_modules PATH=$PWD/build/ws074-chrome/bin:$PATH
#
# What it does:
#   1. installs clang, fontconfig, python3-pil, nodejs, npm and chromium-headless-shell (apt);
#   2. installs playwright-core under build/ws074-chrome/node and a `playwright` module there that
#      forwards to it with Debian's chromium-headless-shell as the executable, because chrome-run.js
#      does require("playwright") and Playwright's own browser download is not used;
#   3. writes build/ws074-chrome/bin/chromium (a wrapper for the headless shell, for the runners that
#      call `chromium` by name) and build/ws074-chrome/fonts.conf (chrome-fonts.sh).
# Build with clang afterwards: CC=clang sh plan/ws074/tests/host-build.sh plain
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
root=$(pwd)

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq clang libvulkan-dev mesa-vulkan-drivers \
	fontconfig python3-pil nodejs npm chromium-headless-shell >/dev/null

shell=/usr/bin/chromium-headless-shell
if [ ! -x "$shell" ]; then
	echo "debian-setup: $shell is missing" >&2
	exit 1
fi

node_dir=build/ws074-chrome/node
mkdir -p "$node_dir"
(cd "$node_dir" && npm install --no-audit --no-fund playwright-core >/dev/null)
mkdir -p "$node_dir/node_modules/playwright"
cat > "$node_dir/node_modules/playwright/index.js" <<EOT
// Written by plan/ws074/tests/debian-setup.sh: playwright-core with the system headless shell.
"use strict";
const core = require("playwright-core");
const chromium = core.chromium;
function withShell(options) {
	return Object.assign({ executablePath: "$shell", headless: true, args: [] }, options,
		{ args: ["--no-sandbox"].concat((options && options.args) || []) });
}
const launch = chromium.launch.bind(chromium);
const launchServer = chromium.launchServer.bind(chromium);
chromium.launch = (options) => launch(withShell(options));
chromium.launchServer = (options) => launchServer(withShell(options));
module.exports = core;
EOT

mkdir -p build/ws074-chrome/bin
cat > build/ws074-chrome/bin/chromium <<EOT
#!/bin/sh
# Written by plan/ws074/tests/debian-setup.sh: the runners' \`chromium\` is Debian's chromium-headless-shell.
exec "$shell" --no-sandbox "\$@"
EOT
chmod 755 build/ws074-chrome/bin/chromium
"$root/build/ws074-chrome/bin/chromium" --version
sh plan/ws074/tests/chrome-fonts.sh
