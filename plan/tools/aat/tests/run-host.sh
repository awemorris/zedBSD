#!/bin/sh
# ws173-p003: the host self-test of plan/tools/aat/aat.  --local runs every command on this host, with
# fake-aat-input.py for P1's aat-input (the same command line and answers) and fake-shot.py for keiland-shot, so the
# command line, the commands sent, the logs' marks and waits, the windows from KWL lines and the transfers are checked
# without a target.  The SSH transport is the same code with ssh in front (checked against QEMU by T1, README.md).
# Last line: "aat-host: PASS" or "aat-host: FAIL".
#   sh plan/tools/aat/tests/run-host.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
here=plan/tools/aat/tests
# The work directory stays in build/tmp (2026-10-06 user: deleting is Q1's step; plan/tools/q1-clean.sh removes it).
mkdir -p build/tmp
tmp=$(mktemp -d "$(pwd)/build/tmp/aat-run-host.XXXXXX")
export AAT_RUN_DIR="$tmp/run" AAT_STATE="$tmp/state" AAT_LOG="$tmp/session.log" FAKE_AAT_DIR="$tmp"
export AAT_INPUT="python3 $PWD/$here/fake-aat-input.py" AAT_SHOT="python3 $PWD/$here/fake-shot.py {path}"
aat() { timeout 60 python3 plan/tools/aat/aat --local "$@"; }
status=0
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1"; status=1; }
record="$tmp/record"
expect_record() {
	# The commands aat-input took since the last check, against what was expected.
	got=$(cat "$record" 2>/dev/null)
	: > "$record"
	if [ "$got" = "$2" ]; then ok "$1"; else bad "$1: got [$got] want [$2]"; fi
}

# The input, with the screen's size from a shot.  The fake server stays in the background on the descriptors it was
# started with, as the real one does: start returns at once all the same (T1-200).
began=$(date +%s)
aat start | grep -q 'screen 64x48' && ok start || bad start
[ $(( $(date +%s) - began )) -lt 20 ] && ok "start returns with the server in the background" || bad "start waited for the server"
: > "$record"

aat click 10 20 && expect_record click "click left 10 20"
aat click 1 2 --button right --count 2 && expect_record double "double-click right 1 2"
aat click 1 2 --count 3 && expect_record triple "click left 1 2
click left 1 2
click left 1 2"
aat drag 0 0 10 20 --steps 2 && expect_record drag "drag 0 0 10 20 2"
aat tap 10 20 && expect_record tap "tap 10 20"
aat tap 10 20 --count 2 && expect_record "double tap" "double-tap 10 20"
aat touch-drag 0 0 10 20 --steps 3 && expect_record "touch drag" "touch-drag 0 0 10 20 3"
aat touch-down 1 5 6 && aat touch-move 1 7 8 && aat touch-up 1 && expect_record fingers "touch-down 1 5 6
touch-move 1 7 8
touch-up 1"
aat wheel 3 4 -2 1 && expect_record wheel "move-to 3 4
wheel -2
hwheel 1"
aat move 5 6 && expect_record move "move-to 5 6"
aat rel -5 7 && expect_record rel "move -5 7"
aat down --button middle && aat up --button middle && expect_record buttons "down middle
up middle"
aat click 7 8 --with alt && expect_record "click with alt" "key-down alt
click left 7 8
key-up alt"
aat key ctrl+alt+t Enter && expect_record chord "key ctrl+alt+t
key enter"
aat type 'Hi there!
ok' && expect_record type "type Hi there!
key enter
type ok"

# Refusals here: off the screen, a button not known, a character a US layout lacks.
aat click 64 10 2>/dev/null && bad "off-screen accepted" || ok "off-screen refused"
aat down --button fourth 2>/dev/null && bad "unknown button accepted" || ok "unknown button refused"
aat type 'é' 2>/dev/null && bad "non-ASCII accepted" || ok "non-ASCII refused"
# aat-input's own refusal (a key it does not know) comes back as a failure with its answer.
aat key ctrl+nosuchkey 2>"$tmp/refused" && bad "aat-input refusal" || { grep -q 'error bad key' "$tmp/refused" && ok "aat-input refusal reported" || bad "aat-input refusal message"; }
: > "$record"

# The log: a mark, a line written after it is waited for and found; one before it is not.
printf 'KWL MAP client=3 surface=9 x=100 y=50\nold line\n' > "$AAT_LOG"
aat mark before >/dev/null
(sleep 1; printf 'KWL WINDOW centred surface=9 x=120 y=60 width=640 height=480\nNOTES APPEARANCE appearance=1\n' >> "$AAT_LOG") &
aat wait-log 'APPEARANCE appearance=1' --since before --timeout 10 | grep -q 'NOTES APPEARANCE' && ok wait-log || bad wait-log
aat lines 'old line' --since before >/dev/null && bad "lines before the mark" || ok "lines after the mark only"
aat lines 'old line' >/dev/null && ok "lines from the start" || bad "lines from the start"
aat wait-log 'never' --timeout 1 2>/dev/null && bad "wait-log timeout" || ok "wait-log timeout"
aat where 9 | grep -qx '120 60 640 480' && ok where || bad where
aat windows | grep -q "^9 3 True 120 60 640 480 0$" && ok windows || bad windows
printf 'KWL UNMAP client=3 surface=9\n' >> "$AAT_LOG"
aat windows | grep -q '^9 3 False ' && ok unmap || bad unmap
# A client that left without unmapping (an application ended by its close button): its window is no longer mapped.
printf 'KWL MAP client=5 surface=8 x=10 y=20\nKWL CLIENT gone client=5 reason=hangup\n' >> "$AAT_LOG"
aat windows | grep -q '^8 5 False 10 20 ' && ok "client gone" || bad "client gone: $(aat windows | grep '^8 5 ')"
# Where a window goes: launched, moved, docked, undocked; the press's point is not the window's place.
cat >> "$AAT_LOG" <<'EOF2'
KWL MAP client=4 surface=12 x=0 y=0
KWL GLASS launch surface=12 from=10,10 to=300,200 size=800x600
KWL GLASS press move surface=12 x=700 y=170
KWL GLASS moved surface=12 x=340 y=260
EOF2
aat where 12 | grep -qx '340 260 800 600' && ok "launch and move" || bad "launch and move: $(aat where 12)"
printf 'KWL GLASS dock surface=12 via=double-click buttons=1,2,3 title=60 x=0 y=48 w=1280 h=752\n' >> "$AAT_LOG"
aat windows | grep -q '^12 4 True 0 48 1280 752 1$' && ok dock || bad dock
printf 'KWL GLASS undock surface=12 via=drag x=200 y=150\nKWL GLASS resized surface=12 docked=0 width=800 height=600 after_ms=1 acked_ms=1 committed_ms=1 sent_at_ms=1\n' >> "$AAT_LOG"
aat windows | grep -q '^12 4 True 200 150 800 600 0$' && ok undock || bad undock
# Two clients with the same surface number: a line naming only the surface is the later window's.
printf 'KWL MAP client=7 surface=12 x=50 y=60\nKWL GLASS moved surface=12 x=70 y=80\n' >> "$AAT_LOG"
aat where 12 --client 7 | grep -qx '70 80 - -' && aat where 12 --client 4 | grep -qx '200 150 800 600' && ok "same surface, two clients" || bad "same surface, two clients"
# With client= on the lines (the compositor since 2026-10-05), the earlier client's window is followed too.
printf 'KWL GLASS moved surface=12 x=90 y=95 client=4\n' >> "$AAT_LOG"
aat where 12 --client 4 | grep -qx '90 95 800 600' && aat where 12 --client 7 | grep -qx '70 80 - -' && ok "client= on the lines" || bad "client= on the lines"
aat windows --json | python3 -c 'import json,sys; w={(x["client"], x["surface"]): x for x in json.load(sys.stdin)}; sys.exit(0 if w[(4, 12)]["x"] == 90 else 1)' && ok "windows --json" || bad "windows --json"

# A shot, a command, files both ways.
aat shot "$tmp/screen.png" | grep -q '64x48' && ok shot || bad shot
[ "$(aat run 'echo hi; exit 3'; echo " $?")" = "hi
 3" ] && ok run || bad run
echo payload > "$tmp/a.txt"
aat put "$tmp/a.txt" "$tmp/b.txt" >/dev/null && aat get "$tmp/b.txt" "$tmp/c.txt" >/dev/null && cmp -s "$tmp/a.txt" "$tmp/c.txt" && ok files || bad files

# A target other than --local: every command goes through ssh (a fake ssh that runs the command here and counts),
# get, put and shot too, whose LOCAL argument once overwrote --local (T1-200b).
mkdir -p "$tmp/fakebin" "$tmp/guest"
printf '#!/bin/sh\necho ssh >> "%s"\nfor last; do :; done\nexec sh -c "$last"\n' "$tmp/ssh-calls" > "$tmp/fakebin/ssh"
chmod +x "$tmp/fakebin/ssh"
echo '{"ssh_port": 2222}' > "$tmp/guest/session.json"
viassh() { PATH="$tmp/fakebin:$PATH" GUEST_RUNTIME="$tmp/guest" timeout 60 python3 plan/tools/aat/aat --qemu "$@" >/dev/null 2>&1; }
for words in "get $tmp/a.txt $tmp/d.txt" "put $tmp/a.txt $tmp/e.txt" "shot $tmp/f.png" "run true"; do
	: > "$tmp/ssh-calls"
	# shellcheck disable=SC2086
	viassh $words
	[ -s "$tmp/ssh-calls" ] && ok "over ssh: ${words%% *}" || bad "not over ssh: ${words%% *}"
done

# Stopped: the next command says there is no server.
aat stop >/dev/null
aat click 1 1 2>"$tmp/stopped" && bad "stopped input" || { grep -q 'no-server' "$tmp/stopped" && ok "stopped input refused" || bad "stopped input message"; }
# The scenarios: the documents and suites are well formed, and the runner runs a helper, marks a scenario that needs
# hands, and writes the record (the fake input and capture again; the host's own programs are never stopped).
python3 plan/tools/aat/check-scenarios.py | tail -1 | grep -q 'check-scenarios: PASS' && ok "scenarios and suites" || bad "scenarios and suites"
printf 'KWL READY socket=%s/wayland-0 width=64 height=48 timeout_ms=0 pid=7 role=normal\n' "$tmp" > "$AAT_LOG"
timeout 300 sh plan/tools/aat/run-aat.sh local "$tmp/runs" os.boot.session-up os.power.lid --no-samples > "$tmp/runner.txt" 2>&1
grep -q '| `os.boot.session-up` | \*\*pass\*\* | 64x48 |' "$tmp/runs/summary.md" && ok "runner: a helper's pass" || bad "runner: a helper's pass"
grep -q '| `os.power.lid` | \*\*needs-person\*\* |' "$tmp/runs/summary.md" && ok "runner: hands are a person's" || bad "runner: hands"
grep -q 'seen: `KWL READY' "$tmp/runs/records/os.boot.session-up.md" && [ -s "$tmp/runs/png/os.boot.session-up-desktop.png" ] && ok "runner: the step's record and screenshot" || bad "runner: record"
[ -f "$tmp/runs/logs/os.boot.session-up.log" ] && ok "runner: the scenario's log kept" || bad "runner: the scenario's log"
aat stop >/dev/null 2>&1

# The choice from a change (select-scenarios.py, ws173-p006): its git diff stood in for by a fixed list.
python3 - <<'PY' && ok "select: paths, documents, smoke, gaps" || bad "select"
import importlib.util, sys
spec = importlib.util.spec_from_file_location("s", "plan/tools/aat/select-scenarios.py")
s = importlib.util.module_from_spec(spec); spec.loader.exec_module(s)
s.changed_files = lambda r: ["userland/desktop/phone/view.c", "tests/scenarios/apps/calendar/navigate.md", "include/uapi/hidraw.h", "plan/master.md"]
chosen, why, gaps = s.select("x")
assert "apps.phone.browse" in chosen and "apps.phone.open-from-home" in chosen, chosen
assert "apps.calendar.navigate" in chosen and "apps.calendar.open-from-home" not in chosen, chosen
assert "os.boot.session-up" in chosen and gaps == ["include/uapi/hidraw.h"], (chosen, gaps)
chosen, _, _ = s.select("x", smoke=False)
assert "os.boot.session-up" not in chosen and chosen.index("apps.phone.open-from-home") < chosen.index("apps.phone.browse"), chosen
assert s.covers("userland/desktop/wayland/keyboard", "userland/desktop/wayland/keyboard-layout.c") and not s.covers("src/a/", "src/ab.c")
PY

[ $status -eq 0 ] && echo "aat-host: PASS" || echo "aat-host: FAIL"
exit $status
