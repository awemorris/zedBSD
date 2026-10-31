#!/bin/sh
# Prints the layout dump of FILE with the comparison fonts (the engine's own file:// view: no http, so no web fonts such as Ahem).
#   sh plan/ws074/phase260/tools/lay.sh FILE      (W=800 H=600 to change the viewport)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
cd "$(dirname -- "$0")/../../../.."
F=userland/desktop/fonts
exec build/ws074-host/plain/browser --dump=layout --width="${W:-800}" --height="${H:-600}" \
    --font=$F/Mahora-Regular.ttf --mono-font=$F/JetBrainsMono-Regular.ttf --fallback-font=$F/DroidSansFallbackFull.ttf "$@"
