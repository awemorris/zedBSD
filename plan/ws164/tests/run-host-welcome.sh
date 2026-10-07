#!/bin/sh
# ws164-p002: the Welcome of Settings on the host, through Settings' host renderer (plan/ws089/tests/host-build.sh,
# build/ws089-host/settings-render): About's "Show Welcome again" starts it, Next goes through the six steps (Welcome,
# Network, Look, Languages, Keys, Done) and Back returns one, "Start using Kei" sets welcome.done (the stand-in settings store) and
# asks the window to close; Skip sets it from the first step.  Each step's frame is written as PNG.
#   sh plan/ws164/tests/run-host-welcome.sh [OUTPUT]   (default build/ws164-welcome)
# Each run gets a new directory behind OUTPUT (plan/tools/fresh-out.sh); nothing is removed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws164-welcome}
. plan/tools/fresh-out.sh
fresh_out "$out"
sh plan/ws089/tests/host-build.sh >/dev/null
render=build/ws089-host/settings-render
status=0
expect() {
	if grep -q -- "$2" "$1"; then
		echo "ok $3"
	else
		echo "FAIL $3 (no line: $2)"
		status=1
	fi
}

# The six steps from About (Languages between Look and Keys, ws164 H3), a switch on the Languages step is the
# Languages page's (the display language Japanese, control 5, then English again, control 4), Back once, and the end.
timeout 120 "$render" --page=about scroll=200 draw="$out/0-about.ppm" control=9003 draw="$out/1-welcome.ppm" \
	control=9001 draw="$out/2-network.ppm" control=9001 draw="$out/3-look.ppm" control=9001 draw="$out/4-languages.ppm" \
	control=5 draw="$out/4-languages-ja.ppm" control=4 control=9001 draw="$out/5-keys.ppm" control=9001 draw="$out/6-done.ppm" control=9000 draw="$out/7-back.ppm" \
	control=9001 control=9001 > "$out/steps.log" 2>&1 || status=1
expect "$out/steps.log" "WELCOME step=0 name=welcome" "Show Welcome again starts the Welcome"
expect "$out/steps.log" "WELCOME step=1 name=network" "Next: Network"
expect "$out/steps.log" "WELCOME step=2 name=look" "Next: Look"
expect "$out/steps.log" "WELCOME step=3 name=languages" "Next: Languages"
expect "$out/steps.log" "LANGUAGES ui language=ja" "the Languages step's switch is the Languages page's"
expect "$out/steps.log" "LANGUAGES ui language=en" "and English again"
expect "$out/steps.log" "WELCOME step=4 name=keys" "Next: Keys"
expect "$out/steps.log" "WELCOME step=5 name=done" "Next: Done"
lines=$(grep -c "WELCOME step=4 name=keys" "$out/steps.log" || true)
[ "$lines" = 2 ] && echo "ok Back returns to Keys" || { echo "FAIL Back (keys lines: $lines)"; status=1; }
expect "$out/steps.log" "WELCOME done error=0" "Start using Kei sets welcome.done"

# Skip from the first step.
timeout 120 "$render" --page=about scroll=200 control=9003 control=9002 > "$out/skip.log" 2>&1 || status=1
expect "$out/skip.log" "WELCOME skip error=0" "Skip sets welcome.done"

# ws177-p007: the keys (Enter Next, Alt+Left Back, Alt+Right Next, Esc closes without welcome.done), a narrow window
# (the bar's dots left out, the header broken into lines), and the Network step with no network and after a refused join.
timeout 120 "$render" --page=about scroll=200 control=9003 key=28 key=28 key=105:4 key=106:4 key=1 > "$out/keys.log" 2>&1 || status=1
lines=$(grep -c "WELCOME step=1 name=network" "$out/keys.log" || true)
expect "$out/keys.log" "WELCOME step=2 name=look" "Enter goes Next twice"
[ "$lines" = 2 ] && echo "ok Alt+Left goes Back to Network" || { echo "FAIL Alt+Left (network lines: $lines)"; status=1; }
lines=$(grep -c "WELCOME step=2 name=look" "$out/keys.log" || true)
[ "$lines" = 2 ] && echo "ok Alt+Right goes Next again" || { echo "FAIL Alt+Right (look lines: $lines)"; status=1; }
expect "$out/keys.log" "WELCOME closed step=2" "Esc closes the Welcome"
if grep -q "WELCOME done\|WELCOME skip" "$out/keys.log"; then echo "FAIL Esc set welcome.done"; status=1; else echo "ok Esc sets nothing"; fi
timeout 120 "$render" --size=560x620 --page=about scroll=200 control=9003 draw="$out/8-narrow-welcome.ppm" control=9001 draw="$out/9-narrow-network.ppm" \
	> "$out/narrow.log" 2>&1 || status=1
expect "$out/narrow.log" "WELCOME step=1 name=network" "a narrow window steps on"
timeout 120 "$render" --network=nonet --page=about scroll=200 control=9003 control=9001 draw="$out/10-nonet.ppm" > "$out/nonet.log" 2>&1 || status=1
expect "$out/nonet.log" "WELCOME step=1 name=network" "no network: the Network step"
timeout 120 "$render" --network=joinfail --page=about scroll=200 control=9003 control=9001 draw="$out/11-joinfail.ppm" > "$out/joinfail.log" 2>&1 || status=1
expect "$out/joinfail.log" "WELCOME step=1 name=network" "a refused join: the Network step"

# The frames as PNG.
for picture in "$out"/*.ppm; do
	convert "$picture" "${picture%.ppm}.png"
done
[ $status -eq 0 ] && echo "run-host-welcome: PASS" || echo "run-host-welcome: FAIL"
exit $status
