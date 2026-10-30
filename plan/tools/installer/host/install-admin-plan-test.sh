#!/bin/sh
# Install-admin's requests, plan checks, cfg text and installed settings on the host.  Builds
# plan/tools/installer/host/install-admin-plan.c with the helper's plan.c, target.c and index.c and the common account core
# (ASan and UBSan), then: install requests of both layouts with typed fields holding spaces and newlines, every prefix of
# a request being "not yet whole", two requests in one buffer, the refusals of the grammar (an unknown verb, two spaces,
# an extra token, an early newline, a field too long, a NUL in a field, a bad length, a number past 64 bits, a bad
# tuple, a null or malformed GUID, a request past the limit), the plan checks (names, display names, passwords,
# hostnames, layouts, overlaps, other disks, a unit twice, edits on and off the 1 MiB grid), the cfg text, and the
# settings written into a root made from userland/base/etc and the OpenSSH package's sshd_config: the default user
# replaced (passwd, shadow, group, home, automatic login), both hashes new and checked with crypt, a name another
# account or group has refused with the files left as they were, the hostname, PermitRootLogin, the swap file in fstab
# (once), and a symbolic link in the path refused.  The last line is "install-admin-plan-test: PASS|FAIL".  Output
# goes to a new build/tmp/install-admin-plan.*.
#   sh plan/tools/installer/host/install-admin-plan-test.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname "$0")/../../../.."
mkdir -p build/tmp
out=$(mktemp -d build/tmp/install-admin-plan.XXXXXX)
status=0
cc -std=c11 -D_DEFAULT_SOURCE -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I. \
	-o "$out/plan" plan/tools/installer/host/install-admin-plan.c userland/desktop/installer/admin/plan.c \
	userland/desktop/installer/admin/target.c userland/desktop/installer/admin/index.c \
	userland/base/common/account.c userland/base/common/sha256.c -lcrypt \
	|| { echo "install-admin-plan-test: FAIL (build)"; exit 1; }
check() {
	if [ "$2" = "$3" ]; then echo "ok $1"; else echo "fail $1: got '$2' want '$3'"; status=1; fi
}
python3 -I - "$out" <<'EOF'
import os, sys
out = sys.argv[1]
D = "a" * 64
E = "11111111-2222-4333-8444-555555555555"
R = "66666666-7777-4888-9999-aaaaaaaaaaaa"
S = "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff"
DISK = "disk 7 nvme0n1 512 33554432 " + D
def f(s):
    b = s.encode()
    return b"%d:" % len(b) + b
def people(user="alice", full="Alice A", pw="pa ss wd", root="se:cret", auto="yes", host="zedbsd"):
    return (b"user " + f(user) + b" fullname " + f(full) + b" password " + f(pw) + b" root-password " + f(root) +
            b" autologin " + auto.encode() + b" hostname " + f(host))
def install(layout, units="units 2 base desktop", **kw):
    return b"install " + DISK.encode() + b" " + layout.encode() + b" " + units.encode() + b" " + people(**kw) + b"\n"
cases = {
    "whole": install("whole 536870912 1073741824"),
    "whole-noswap": install("whole 536870912 0"),
    "chosen": install("chosen esp 7:2048:1048576:%s root 7:1050624:8388608:%s swap 7:9439232:2097152:%s" % (E, R.upper(), S)),
    "chosen-none": install("chosen esp 7:2048:1048576:%s root 7:1050624:8388608:%s swap none" % (E, R)),
    "probe": b"probe\n",
    "cancel": b"cancel\n",
    "delete": ("edit delete " + DISK + " part 7:2048:1048576:" + E + "\n").encode(),
    "create": ("edit create " + DISK + " new 2048 1048576\n").encode(),
    "create-esp": ("edit create-with-esp " + DISK + " esp 2048 1048576 new 1050624 2097152\n").encode(),
    # Grammar refusals.
    "verb": b"format\n",
    "spaces": b"probe  \n",
    "extra": b"probe now\n",
    "early": ("install " + DISK + "\n").encode(),
    "long-user": install("whole 536870912 0", user="a" * 33),
    "nul": install("whole 536870912 0", full="a\0b"),
    "length": install("whole 536870912 0").replace(b"user 5:", b"user 5x:"),
    "huge": install("whole 18446744073709551616 0"),
    "tuple": install("chosen esp 7:2048:1048576 root 7:1050624:8388608:%s swap none" % R),
    "guid": install("chosen esp 7:2048:1048576:%s root 7:1050624:8388608:%s swap none" % (E, "0" * 8 + "-0000-0000-0000-" + "0" * 12)),
    "guid-form": install("chosen esp 7:2048:1048576:%s root 7:1050624:8388608:%s swap none" % (E, "g" + R[1:])),
    "autologin": install("whole 536870912 0", auto="maybe"),
    # Check refusals.
    "user-root": install("whole 536870912 0", user="root"),
    "user-upper": install("whole 536870912 0", user="Alice"),
    "user-digit": install("whole 536870912 0", user="1alice"),
    "user-space": install("whole 536870912 0", user="al ice"),
    "full-colon": install("whole 536870912 0", full="A:B"),
    "full-newline": install("whole 536870912 0", full="A\nB"),
    "pw-newline": install("whole 536870912 0", pw="pa\nss"),
    "pw-empty": install("whole 536870912 0", pw=""),
    "pw-control": install("whole 536870912 0", root="a\tb"),
    "host-dash": install("whole 536870912 0", host="-zed"),
    "host-end": install("whole 536870912 0", host="zed-"),
    "host-upper": install("whole 536870912 0", host="Zed"),
    "host-long": install("whole 536870912 0", host="a" * 64),
    "host-63": install("whole 536870912 0", host="a" * 63),
    "esp-small": install("whole 268435456 0"),
    "esp-grid": install("whole 536870913 0"),
    "fit": install("whole 536870912 16642998272"),
    "fit-edge": install("whole 536870912 16639852544"),
    "overlap": install("chosen esp 7:2048:1048576:%s root 7:1048576:8388608:%s swap none" % (E, R)),
    "swap-overlap": install("chosen esp 7:2048:1048576:%s root 7:1050624:8388608:%s swap 7:9437184:2097152:%s" % (E, R, S)),
    "other-disk": install("chosen esp 7:2048:1048576:%s root 8:1050624:8388608:%s swap none" % (E, R)),
    "past-end": install("chosen esp 7:2048:1048576:%s root 7:1050624:33554432:%s swap none" % (E, R)),
    "twice": install("whole 536870912 0", units="units 2 base base"),
    "create-grid": ("edit create " + DISK + " new 2049 1048576\n").encode(),
    "create-start": ("edit create " + DISK + " new 0 1048576\n").encode(),
    "create-end": ("edit create " + DISK + " new 32505856 1048576\n").encode(),
    "create-last": ("edit create " + DISK + " new 32505856 1046528\n").encode(),
    "esp-new-small": ("edit create-with-esp " + DISK + " esp 2048 524288 new 1050624 2097152\n").encode(),
    "esp-new-overlap": ("edit create-with-esp " + DISK + " esp 2048 1048576 new 1048576 2097152\n").encode(),
    "delete-other": ("edit delete disk 7 nvme0n1 512 33554432 " + D + " part 9:2048:1048576:" + E + "\n").encode(),
    "sector": install("whole 536870912 0").replace(b" 512 33554432 ", b" 1024 33554432 "),
}
for name, data in cases.items():
    with open(os.path.join(out, name + ".req"), "wb") as fd:
        fd.write(data)
with open(os.path.join(out, "two.req"), "wb") as fd:
    fd.write(cases["chosen"] + b"cancel\n")
EOF
p="$out/plan"
r() { "$p" read "$out/$1.req" | sed -n 1,2p | tr '\n' ' ' | sed 's/ $//'; }
line() { "$p" read "$out/$1.req" | grep "^$2"; }
check whole "$(r whole)" "RESULT 0 $(wc -c < "$out/whole.req") 6 CHECK 0"
check whole-layout "$(line whole WHOLE)" "WHOLE 536870912 1073741824 swap=1"
check whole-user "$(line whole USER)" "USER [alice] [Alice A] [pa ss wd] [se:cret] 1 [zedbsd]"
check newline-field "$(r pw-newline | sed 's/^RESULT 0 [0-9]* 6 //') $("$p" read "$out/pw-newline.req" | grep -A1 '^USER' | tr '\n' ' ')" "CHECK 22 USER [alice] [Alice A] [pa ss] [se:cret] 1 [zedbsd] "
check whole-units "$(line whole UNITS)" "UNITS base desktop"
check whole-noswap "$(line whole-noswap WHOLE)" "WHOLE 536870912 0 swap=0"
check chosen "$(r chosen)" "RESULT 0 $(wc -c < "$out/chosen.req") 6 CHECK 0"
check chosen-lower "$(line chosen CHOSEN)" "CHOSEN esp=11111111-2222-4333-8444-555555555555 root=66666666-7777-4888-9999-aaaaaaaaaaaa swap=bbbbbbbb-cccc-4ddd-8eee-ffffffffffff"
check chosen-none "$(line chosen-none CHOSEN)" "CHOSEN esp=11111111-2222-4333-8444-555555555555 root=66666666-7777-4888-9999-aaaaaaaaaaaa swap=none"
check probe "$(r probe)" "RESULT 0 6 1 CHECK 0"
check cancel "$(r cancel)" "RESULT 0 7 2 CHECK 0"
check delete "$(r delete)" "RESULT 0 $(wc -c < "$out/delete.req") 3 CHECK 0"
check create "$(r create)" "RESULT 0 $(wc -c < "$out/create.req") 4 CHECK 0"
check create-esp "$(r create-esp)" "RESULT 0 $(wc -c < "$out/create-esp.req") 5 CHECK 0"
check forget "$("$p" read "$out/whole.req" | grep -c FORGET)" "0"
for c in whole chosen chosen-none delete create-esp probe; do check "prefixes-$c" "$("$p" prefixes "$out/$c.req")" "PREFIXES ok"; done
check two "$("$p" two "$out/two.req")" "TWO 6 2 $(wc -c < "$out/two.req")"
for c in verb spaces extra early long-user nul length huge tuple guid guid-form autologin host-long; do
	check "grammar-$c" "$(r $c)" "RESULT 22 0 0"
done
for c in user-root user-upper user-digit user-space full-colon full-newline pw-empty pw-control host-dash host-end \
	host-upper esp-small esp-grid fit overlap swap-overlap other-disk past-end twice create-grid create-start \
	create-end esp-new-small esp-new-overlap delete-other sector; do
	check "check-$c" "$(r $c | sed 's/^RESULT 0 [0-9]* [0-9]* //')" "CHECK 22"
done
check host-63 "$(r host-63 | sed 's/^RESULT 0 [0-9]* [0-9]* //')" "CHECK 0"
check fit-edge "$(r fit-edge | sed 's/^RESULT 0 [0-9]* [0-9]* //')" "CHECK 0"
check create-last "$(r create-last | sed 's/^RESULT 0 [0-9]* [0-9]* //')" "CHECK 0"
E=11111111-2222-4333-8444-555555555555
R=66666666-7777-4888-9999-aaaaaaaaaaaa
check cfg-full "$("$p" cfg $R $E 1)" "kernel=rootpart:/boot/vmunix
rootpart=PARTUUID=$R
swap0=PARTUUID=$E
logo=rootpart:/boot/logo.ppm
login=graphical
kmsg=quiet"
check cfg-bare "$("$p" cfg $R - 0)" "kernel=rootpart:/boot/vmunix
rootpart=PARTUUID=$R
logo=rootpart:/boot/logo.ppm
kmsg=quiet"
check cfg-bad "$("$p" cfg ROOT - 0)" "RESULT 22"
# The installed root: the base unit's /etc and the OpenSSH package's sshd_config.
make_root() {
	mkdir -p "$1/etc/ssh" "$1/etc/keiland" "$1/home/user"
	cp userland/base/etc/passwd userland/base/etc/shadow userland/base/etc/group userland/base/etc/rc.conf \
		userland/base/etc/fstab "$1/etc/"
	chmod 600 "$1/etc/shadow"
	cp userland/packages/network/openssh/files/sshd_config "$1/etc/ssh/"
	printf 'user\n' > "$1/etc/keiland/autologin"
	echo note > "$1/home/user/note"
}
t="$out/root-a"
make_root "$t"
check accounts "$("$p" accounts "$t" alice "Alice A" "pa ss wd" "se cret" yes)" "RESULT 0"
check passwd "$(grep 1000 "$t/etc/passwd")" "alice:x:1000:1000:Alice A:/home/alice:/bin/sh"
check passwd-rest "$(grep -vc 1000 "$t/etc/passwd")" "$(grep -vc 1000 userland/base/etc/passwd)"
check group "$(grep -E '^(wheel|network|alice|user):' "$t/etc/group" | tr '\n' ' ')" "wheel:x:0:root,alice network:x:69:alice alice:x:1000: "
check shadow-user "$("$p" crypt "pa ss wd" "$(grep '^alice:' "$t/etc/shadow" | cut -d: -f2)")" "CRYPT same"
check shadow-root "$("$p" crypt "se cret" "$(grep '^root:' "$t/etc/shadow" | cut -d: -f2)")" "CRYPT same"
check shadow-old "$(grep -c '^user:' "$t/etc/shadow")" "0"
check shadow-rest "$(grep -v -E '^(root|alice):' "$t/etc/shadow")" "$(grep -v -E '^(root|user):' userland/base/etc/shadow)"
check shadow-mode "$(stat -c %a "$t/etc/shadow") $(stat -c %a "$t/etc/passwd")" "600 $(stat -c %a userland/base/etc/passwd)"
check home "$(cat "$t/home/alice/note") $(test -e "$t/home/user" && echo left || echo gone)" "note gone"
check autologin "$(cat "$t/etc/keiland/autologin")" "alice"
check temporary "$(find "$t" -name '*.install-admin' | wc -l)" "0"
t="$out/root-same"
make_root "$t"
check same "$("$p" accounts "$t" user "User" "newpassword" "rootpw" no)" "RESULT 0"
check same-hash "$("$p" crypt "newpassword" "$(grep '^user:' "$t/etc/shadow" | cut -d: -f2)")" "CRYPT same"
check same-home "$(cat "$t/home/user/note")" "note"
check same-autologin "$(wc -c < "$t/etc/keiland/autologin")" "0"
for name in sshd network wheel bluetooth _greeter; do
	t="$out/root-$name"
	make_root "$t"
	before=$(cat "$t/etc/passwd" "$t/etc/shadow" "$t/etc/group" | sha256sum)
	result=$("$p" accounts "$t" "$name" "X" "password" "rootpw" yes)
	after=$(cat "$t/etc/passwd" "$t/etc/shadow" "$t/etc/group" | sha256sum)
	check "conflict-$name" "$result" "RESULT 17"
	check "conflict-$name-kept" "$after" "$before"
done
t="$out/root-nodesktop"
make_root "$t"
mv "$t/etc/keiland/autologin" "$t/autologin.moved"
check nodesktop-yes "$("$p" accounts "$t" bob "Bob" "password" "rootpw" yes)" "RESULT 2"
t="$out/root-nodesktop2"
make_root "$t"
mv "$t/etc/keiland/autologin" "$t/autologin.moved"
check nodesktop-no "$("$p" accounts "$t" bob "Bob" "password" "rootpw" no)" "RESULT 0"
t="$out/root-a"
check hostname "$("$p" hostname "$t" myhost) $(grep -c '^hostname: myhost$' "$t/etc/rc.conf") $(grep -c hostname "$t/etc/rc.conf")" \
	"RESULT 0 1 1"
check hostname-bad "$("$p" hostname "$t" My_Host)" "RESULT 22"
check sshd "$("$p" sshd "$t") $(grep -i '^ *PermitRootLogin' "$t/etc/ssh/sshd_config" | tr '\n' ' ')" "RESULT 0 PermitRootLogin no "
check sshd-rest "$(grep -vi PermitRootLogin "$t/etc/ssh/sshd_config")" \
	"$(grep -vi PermitRootLogin userland/packages/network/openssh/files/sshd_config)"
grep -vi PermitRootLogin userland/packages/network/openssh/files/sshd_config > "$t/etc/ssh/sshd_config"
check sshd-added "$("$p" sshd "$t") $(head -1 "$t/etc/ssh/sshd_config")" "RESULT 0 PermitRootLogin no"
check fstab "$("$p" fstab "$t") $(grep -c '^/swapfile none swap sw 0 0$' "$t/etc/fstab")" "RESULT 0 1"
check fstab-again "$("$p" fstab "$t") $(grep -c '^/swapfile' "$t/etc/fstab")" "RESULT 0 1"
t="$out/root-nossh"
make_root "$t"
mv "$t/etc/ssh/sshd_config" "$t/sshd_config.moved"
check sshd-none "$("$p" sshd "$t")" "RESULT 0"
t="$out/root-link"
make_root "$t"
mkdir -p "$out/outside"
cp userland/packages/network/openssh/files/sshd_config "$out/outside/"
mv "$t/etc/ssh" "$t/ssh.moved"
ln -s "$(pwd)/$out/outside" "$t/etc/ssh"
result=$("$p" sshd "$t")
case $result in "RESULT 20"|"RESULT 40") echo "ok link";; *) echo "fail link: $result"; status=1;; esac
check link-outside "$(grep -c '^PermitRootLogin yes' "$out/outside/sshd_config")" "1"
[ $status -eq 0 ] && echo "install-admin-plan-test: PASS $out" || echo "install-admin-plan-test: FAIL $out"
exit $status
