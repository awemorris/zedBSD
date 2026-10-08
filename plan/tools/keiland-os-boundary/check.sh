#!/bin/sh
# Checks Keiland's common-source OS boundary and installation paths.
# Usage: sh plan/tools/keiland-os-boundary/check.sh (from any directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
# The work directory stays in build/tmp (2026-10-06 user: deleting is Q1's step; plan/tools/q1-clean.sh removes it).
mkdir -p build/tmp
work=$(mktemp -d "$(pwd)/build/tmp/os-boundary.XXXXXX")
status=0

# Collect the compositor's and the system library's sources: all of them, since their OS code is libkeiland-backend's
# (ws131-p009 moved the last of it, the GPU buffers).
find userland/desktop/libkeiland userland/desktop/wayland -name '*.[ch]' -print | LC_ALL=C sort > "$work/common"

# The compositor and libkeiland include no OS header (the evdev header choice is
# libkeiland-backend's keiland-backend-evdev.h since ws131-p007).
while IFS= read -r file; do
    awk '/^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](uapi\/|userland\/base\/)/ {print FILENAME ":" FNR ": " $0}' "$file"
done < "$work/common" > "$work/C1"

# No device ioctl in the compositor or libkeiland: the input devices are libkeiland-backend's (ws131-p007).
while IFS= read -r file; do
    awk '/ioctl[[:space:]]*\(/ {print FILENAME ":" FNR ": " $0}' "$file"
done < "$work/common" > "$work/C2"

# The zedBSD GPU wire layout and the kernel's GPU types stay inside libkeiland-backend-zedbsd (WS131 C3, ws131-p009):
# not in the compositor, libkeiland or the other backend trees (libvulkan, the driver, is not the desktop's boundary).
find userland/desktop/wayland userland/desktop/libkeiland userland/desktop/libkeiland-backend \
    userland/desktop/libkeiland-backend-linux userland/desktop/libkeiland-backend-freebsd -name '*.[ch]' -print |
while IFS= read -r file; do
    awk '/kwl_buffer_layout|gpu_image_descriptor|[<"]uapi\/gpu/ {print FILENAME ":" FNR ": " $0}' "$file"
done > "$work/C3"

# Inspect each literal so a system-shell exception cannot hide another path.
find userland/desktop -path '*/sessiond' -prune -o -path '*/keiland' -prune \
    -o -name '*.[ch]' -print |
while IFS= read -r file; do
    [ "$file" != userland/desktop/paths.h ] || continue
    awk '{
        remaining = $0
        while (match(remaining, /"([^"\\]|\\.)*"/)) {
            literal = substr(remaining, RSTART + 1, RLENGTH - 2)
            if (literal ~ /^\/(usr\/share|usr\/libexec|etc\/keiland|bin\/|usr\/bin\/)/ &&
                literal != "/bin/sh" && literal !~ /^\/bin\/sh /) {
                print FILENAME ":" FNR ": " $0
                break
            }
            remaining = substr(remaining, RSTART + RLENGTH)
        }
    }' "$file"
done > "$work/C4"

# Desktop headers are owned by desktop rather than libc.
find include/libc \( -name 'keiland.h' -o -name 'keiui.h' -o -name 'keiland-ui.h' -o -name 'truetype.h' \
    -o -name 'browser.h' -o -name 'wayland*' -o -name 'xdg-shell*' \
    -o -name 'primary-selection*' -o -name 'tablet-unstable*' \) -print > "$work/C5"

# Linux selection belongs to the OS modules; the evdev header bridges constants.
find userland/desktop \
    \( -path '*/zedbsd' -o -path '*/linux' -o -path '*/freebsd' -o -path '*/wpa' \
    -o -path 'userland/desktop/libkeiland-backend-*' \) -prune \
    -o -name '*.[ch]' -print |
while IFS= read -r file; do
    [ "$file" != userland/desktop/libkeiland-backend/keiland-backend-evdev.h ] || continue
    # D14 (2026-10-03 user): Terminal's pty header, <pty.h> or FreeBSD's <libutil.h>, is chosen by one macro block
    # (ws131-p018 moved it into terminal/main.c); the block holds only those includes.
    if [ "$file" = userland/desktop/terminal/main.c ]; then
        awk '/^[[:space:]]*#[[:space:]]*(if|ifdef|elif).*(__linux__|__FreeBSD__)/ {
            getline next_line
            if (next_line ~ /^[[:space:]]*#[[:space:]]*include[[:space:]]*<libutil\.h>/) next
            print FILENAME ":" FNR - 1 ": " $0
        }' "$file"
        continue
    fi
    awk '/^[[:space:]]*#[[:space:]]*(if|ifdef|elif).*(__linux__|__FreeBSD__)/ {print FILENAME ":" FNR ": " $0}' "$file"
done > "$work/L1"

# Each OS module consumes only its own kernel and service interfaces (libkeiland-backend's trees, WS131).
find userland/desktop/libkeiland-backend-linux userland/desktop/libkeiland-backend-freebsd \
    userland/desktop/libkeiland-backend/wpa -name '*.[ch]' -print 2>/dev/null |
while IFS= read -r file; do
    awk '/^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](uapi\/|userland\/base\/(net|audiod)\/)/ {print FILENAME ":" FNR ": " $0}' "$file"
done > "$work/L2"
find userland/desktop/libkeiland-backend-zedbsd -name '*.[ch]' -print 2>/dev/null |
while IFS= read -r file; do
    awk '/^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](linux\/|drm\/|sound\/)/ {print FILENAME ":" FNR ": " $0}' "$file"
done > "$work/L3"

# The Linux Vulkan frontend never includes or compiles zedBSD Vulkan sources.
find userland/desktop/libvulkan-compat -type f \
    \( -name '*.[ch]' -o -name 'Makefile.linux' \) -print |
while IFS= read -r file; do
    awk '/^[[:space:]]*#[[:space:]]*include.*(userland\/desktop\/libvulkan\/|\.\.\/libvulkan\/)/ ||
         /^[^#]*userland\/desktop\/libvulkan\/.*\.c/ {print FILENAME ":" FNR ": " $0}' "$file"
done > "$work/L4"
make -s -f userland/desktop/keiland-linux.mk print-sources > "$work/linux-sources"
awk '/^userland\/desktop\/libvulkan\// {print "compiled Linux source: " $0}' "$work/linux-sources" >> "$work/L4"

# libkeiland-backend never reaches into the compositor or the applications' library (WS131 B1).
find userland/desktop/libkeiland-backend userland/desktop/libkeiland-backend-zedbsd \
    userland/desktop/libkeiland-backend-linux userland/desktop/libkeiland-backend-freebsd \
    -name '*.[ch]' -print 2>/dev/null |
while IFS= read -r file; do
    awk '/^[[:space:]]*#[[:space:]]*include[[:space:]]*([<"]userland\/desktop\/wayland\/|"[^"]*zwl[^"]*\.h"|<keiland\/keiland\.h>|<keiland\.h>|<keiui\.h>)/ {print FILENAME ":" FNR ": " $0}' "$file"
done > "$work/B1"

# Only the compositor uses libkeiland-backend: no other desktop source includes its headers, and no other Makefile
# builds or links its sources (the top-level lists that include the backend's own Makefiles aside) (WS131 B3; since ws131-p011 libkeiland reaches the system only through the compositor).
find userland/desktop -path 'userland/desktop/wayland' -prune \
    -o -path 'userland/desktop/libkeiland-backend*' -prune \
    -o -name '*.[ch]' -print |
while IFS= read -r file; do
    awk '/^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"].*keiland-backend[a-z-]*\.h[>"]/ {print FILENAME ":" FNR ": " $0}' "$file"
done > "$work/B3"
find userland/desktop -path 'userland/desktop/wayland' -prune \
    -o -path 'userland/desktop/libkeiland-backend*' -prune \
    -o \( -name 'Makefile*' -o -name '*.mk' \) -print |
while IFS= read -r file; do
    # A Makefile's comment lines name things without building them; sessiond, the system's session manager, links the
    # network service's protocol for itself (sleep.c), which is no use of libkeiland-backend (ws188, Q1 2026-10-08).
    awk -v sessiond="$(case $file in userland/desktop/sessiond/*) echo 1 ;; *) echo 0 ;; esac)" '
        /^[[:space:]]*#/ {next}
        /libkeiland-backend/ && !/libkeiland-backend[a-z-]*\/Makefile\.(linux|freebsd)/ {print FILENAME ":" FNR ": " $0; next}
        /userland\/base\/net\// && sessiond == 0 {print FILENAME ":" FNR ": " $0}' "$file"
done >> "$work/B3"

# The compositor takes from libkeiland only what D4 allows: the touch motion, the scroller, the gestures, the
# version and the translations (kl_tr*, translate.c: files and strings, no Wayland client part; q811) (WS131 B2, ws131-p011; `nm -u` of each compositor binary that is built: zedBSD's under $BUILD, default
# build/amd64, Linux's under $KEILAND_LINUX_BUILD, default build/keiland-linux, and any in $B2_BINARIES).
for binary in "${BUILD:-build/amd64}/bin/wayland" "${KEILAND_LINUX_BUILD:-build/keiland-linux}/bin/wayland" ${B2_BINARIES:-}; do
    if [ ! -f "$binary" ]; then
        echo "check: B2 note: $binary not built, not looked at" >&2
        continue
    fi
    nm -u "$binary" | awk -v binary="$binary" '{name = $NF; sub(/@.*/, "", name)}
        name ~ /^(keiland_|kl_)/ && name !~ /^(keiland_motion_|keiland_scroller_|keiland_gesture_|kl_motion_|kl_scroller_|kl_gesture_|keiland_version$|kl_version$|kl_tr$|kl_trc$|kl_tr_)/ {
            print binary ": uses " name " of libkeiland"
        }'
done > "$work/B2"

# libkeiland keeps no operating-system directory: its OS code is libkeiland-backend's (WS131 L6, ws131-p004).
for os_dir in zedbsd linux freebsd; do
    if [ -e "userland/desktop/libkeiland/$os_dir" ]; then
        echo "userland/desktop/libkeiland/$os_dir: an OS directory in libkeiland"
    fi
done > "$work/L6"

# The X server reads the Wayland keyboard's key codes from its own header, not an OS header (WS131 D14, ws131-p009).
find userland/desktop/xserver -name '*.[ch]' -print |
while IFS= read -r file; do
    awk '/^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](uapi\/|linux\/|dev\/)/ {print FILENAME ":" FNR ": " $0}' "$file"
done > "$work/X1"

# The compositor keeps no operating-system directory either: its OS code is libkeiland-backend's (WS131, ws131-p009).
for os_dir in zedbsd linux freebsd dmabuf evdev session drm wpa; do
    if [ -e "userland/desktop/wayland/$os_dir" ]; then
        echo "userland/desktop/wayland/$os_dir: an OS directory in the compositor"
    fi
done > "$work/L7"

# The compositor's three Makefiles build the same compositor sources (libkeiland-backend's are each OS's own; ws131-p009;
# the zedBSD test image's screen capture, shot.c in place of shot-none.c, is zedBSD's alone, ws173-p002).
for makefile in Makefile Makefile.linux Makefile.freebsd; do
    grep -oE 'userland/[A-Za-z0-9_/.-]*\.c\b' "userland/desktop/wayland/$makefile" | grep -v '^userland/desktop/libkeiland-backend' |
        grep -vx 'userland/desktop/wayland/shot\.c' |
        LC_ALL=C sort -u > "$work/sources-$makefile"
done
{
    diff "$work/sources-Makefile" "$work/sources-Makefile.linux" | sed -n 's/^[<>] /Makefile vs Makefile.linux: /p'
    diff "$work/sources-Makefile" "$work/sources-Makefile.freebsd" | sed -n 's/^[<>] /Makefile vs Makefile.freebsd: /p'
} > "$work/M1"

# desktop.conf is the compositor's own file (WS135): no other desktop source names it.
find userland/desktop -name '*.[ch]' -print |
while IFS= read -r file; do
    [ "$file" != userland/desktop/wayland/settings-store.c ] || continue
    awk '/"[^"]*desktop\.conf[^"]*"/ {print FILENAME ":" FNR ": " $0}' "$file"
done > "$work/S1"

# Inspect the actual target package membership and wildcard filename boundary.
# The database only (a target that does not exist, so no recipe of the package builds runs; 2026-10-04, a fresh build/ has no package trees).
make -pn -q zedbsd-make-database-only > "$work/make-database" 2>/dev/null || true
python3 - "$work/make-database" > "$work/L5" <<'PY'
from pathlib import Path
import fnmatch
import re
import sys
text = Path('Makefile').read_text()
patterns = re.findall(r'\$\(wildcard ([^)]+)\)', text)
for path in Path('userland').rglob('Makefile.linux'):
    for pattern in patterns:
        if fnmatch.fnmatchcase(str(path), pattern):
            print(f'{path}: matches top-level wildcard {pattern}')
for line in Path(sys.argv[1]).read_text().splitlines():
    if line.startswith('USERLAND_PACKAGE_MAKEFILES :=') and 'Makefile.linux' in line:
        print(line)
    if line.startswith('MAKEFILE_LIST :='):
        for name in line.split()[2:]:
            if name.endswith('Makefile.linux'):
                print(f'target includes Linux rules: {name}')
PY

# The old names are gone (WS131 B5, ws131-p023): no C source of the tree or of the plan's tests (the history and
# libbrowser, another component, aside) uses kui_, KUI_, keiland_ or KEILAND_, and nothing includes or copies the
# old headers keiui.h and keiland-ui.h, or names the public headers as they were before their directories
# (<keiland.h>, <truetype.h>, <browser.h>, userland/desktop/keiland/; ws131-p026).  Kept: the install paths' macros (paths.h, KEILAND_PREFIX), the environment's
# variables (KEILAND_DRM_DEVICE, KEILAND_SEAT, KEILAND_DESKTOP_TOKEN, KEILAND_VULKAN_*), include guards (D16), and the
# WS035 p075 test's own protocol (keiland_generic_*_v1, ws131-p021).
git ls-files userland plan | grep -E '\.(c|h|inc)$' | grep -v -e '^plan/history/' -e '^userland/desktop/libbrowser/' |
while IFS= read -r file; do
    grep -noE '\b(kui|KUI|keiland|KEILAND)_[A-Za-z0-9_]*|[<"](keiui|keiland-ui|keiland|truetype|browser)\.h[>"]' "$file" |
        grep -vE ':(KEILAND_(BINDIR|DATADIR|LIBEXECDIR|SYSCONFDIR|PREFIX|FONT_BOLD|FONT_FALLBACK_MONO|DRM_DEVICE|SEAT|DESKTOP_TOKEN|VULKAN_BACKEND|VULKAN_BACKEND_PATHS|VULKAN_NO_DEEPBIND)|KEILAND_([A-Z0-9_]+_)?H|keiland_generic_[a-z0-9_]+)$' |
        sed "s|^|$file:|"
done > "$work/B5"
git ls-files userland plan | grep -E '(\.sh|\.py|Makefile[a-z.]*|\.mk)$' | grep -v -e '^plan/history/' -e '^plan/ws131/tools/rename-map\.py$' -e '^plan/tools/keiland-os-boundary/check\.sh$' |
while IFS= read -r file; do
    grep -nE 'keiland/(keiui|keiland-ui)\.h|userland/desktop/keiland([^-a-z.]|$)' "$file" | sed "s|^|$file:|"
done >> "$work/B5"

# The applications and libkeiland reach the system only through libkeiland's kl_system_* (WS188 p003; Guardrail
# "app と設定" and "Bluetooth と Display も compositor 経由", 2026-10-08 user): no literal of the system's trees
# (/dev /proc /sys /run /var /etc), no daemon's socket or name (A1), no local socket (A2), no account or group database,
# file system size, mount table or sysctl (A3), no process started (A4), no OS or daemon header (A5).  Their own
# functions are in app-allow.tsv, each with its decision.  The compositor, libkeiland-backend, the system's services
# (sessiond, printd), the X server and the graphics and compatibility libraries are not applications.  Comments are
# left out; a literal is looked at only inside its quotes.
python3 - plan/tools/keiland-os-boundary/app-allow.tsv "$work" <<'PY'
import os
import re
import sys

allow_path = sys.argv[1]
work = sys.argv[2]
not_applications = {
    'wayland', 'sessiond', 'printd', 'xserver', 'libvulkan', 'libvulkan-compat', 'libGL', 'libegl', 'libglesv2',
    'libwayland', 'libwayland-egl', 'linux-compat', 'freebsd-compat', 'include', 'fonts', 'artwork', 'wallpapers',
    'locale',
}
calls = {
    'A3': re.compile(r'\b(getpw\w*|getgr\w*|setpwent|endpwent|setgrent|endgrent|statvfs|fstatvfs|statfs|fstatfs|'
                     r'getfsstat|getmntinfo|getmntent|setmntent|sysctl\w*)\s*\('),
    'A4': re.compile(r'\b(fork|vfork|execl|execlp|execle|execv|execvp|execve|execvpe|fexecve|posix_spawn|posix_spawnp|'
                     r'system|popen)\s*\('),
    'A2': re.compile(r'\b(sockaddr_un|AF_UNIX|AF_LOCAL|PF_UNIX|PF_LOCAL)\b'),
}
literal = re.compile(r'^/(dev|proc|sys|run|var|etc)(/|$)|\.sock$|^(netd|audiod|volumed|bluetoothd|sessiond|printd|wpa_supplicant)\b')
header = re.compile(r'^\s*#\s*include\s*[<"]((uapi|linux|dev)/[^>"]*|sys/sysctl\.h|mntent\.h|userland/base/[^>"]*)[>"]')

# The allowed operations: (check, source, name); a row without its reason fails A1.
allowed = []
malformed = []
with open(allow_path) as table:
    for line in table:
        if line.startswith('#') or not line.strip():
            continue
        fields = line.rstrip('\n').split('\t')
        if len(fields) < 4 or not fields[3].strip():
            malformed.append(f'{allow_path}: a row without its four columns: {line.strip()}')
            continue
        allowed.append((fields[0], fields[1], fields[2]))

def is_allowed(check, path, name):
    for row_check, source, row_name in allowed:
        if row_check != check:
            continue
        if source.endswith('/'):
            if not path.startswith(source):
                continue
        elif path != source:
            continue
        if row_name == '*' or row_name == name:
            return True
    return False

def strip(text):
    """The code without comments and with each literal emptied, and the literals with their lines."""
    code = []
    literals = []
    index = 0
    length = len(text)
    while index < length:
        if text.startswith('/*', index):
            end = text.find('*/', index + 2)
            end = length if end < 0 else end + 2
            code.append('\n' * text.count('\n', index, end))
            index = end
        elif text.startswith('//', index):
            end = text.find('\n', index)
            index = length if end < 0 else end
        elif text[index] in '"\'':
            quote = text[index]
            end = index + 1
            while end < length and text[end] != quote:
                if text[end] == '\\':
                    end += 1
                end += 1
            if quote == '"':
                literals.append((text.count('\n', 0, index) + 1, text[index + 1:end]))
            code.append(quote + quote)
            index = end + 1
        else:
            code.append(text[index])
            index += 1
    return ''.join(code), literals

sources = []
for name in sorted(os.listdir('userland/desktop')):
    folder = os.path.join('userland/desktop', name)
    if not os.path.isdir(folder) or name in not_applications or name.startswith('libkeiland-backend'):
        continue
    for directory, _, files in os.walk(folder):
        for file in sorted(files):
            if file.endswith(('.c', '.h', '.inc')):
                sources.append(os.path.join(directory, file))

found = {'A1': list(malformed), 'A2': [], 'A3': [], 'A4': [], 'A5': []}
for path in sorted(sources):
    with open(path, errors='replace') as source:
        text = source.read()
    code, literals = strip(text)
    for number, line in enumerate(code.split('\n'), 1):
        for check, pattern in calls.items():
            for match in pattern.finditer(line):
                if not is_allowed(check, path, match.group(1)):
                    found[check].append(f'{path}:{number}: {match.group(1)}')
    for number, line in enumerate(text.split('\n'), 1):
        match = header.match(line)
        if match and not is_allowed('A5', path, match.group(1)):
            found['A5'].append(f'{path}:{number}: #include {match.group(1)}')
    for number, value in literals:
        if literal.search(value) and not is_allowed('A1', path, value):
            found['A1'].append(f'{path}:{number}: "{value}"')

for check, lines in found.items():
    with open(os.path.join(work, check), 'w') as out:
        for line in lines:
            out.write(line + '\n')
PY

# Report every violated condition before returning the aggregate outcome.
for check in C1 C2 C3 C4 C5 L1 L2 L3 L4 L5 L6 L7 M1 X1 B1 B2 B3 B5 S1 A1 A2 A3 A4 A5; do
    if [ -s "$work/$check" ]; then
        while IFS= read -r detail; do
            printf 'check: %s FAIL %s\n' "$check" "$detail"
        done < "$work/$check"
        status=1
    else
        printf 'check: %s PASS\n' "$check"
    fi
done
if [ "$status" -ne 0 ]; then
    echo 'keiland-os-boundary: FAIL'
    exit 1
fi
echo 'keiland-os-boundary: PASS'
