#!/bin/sh
# ws196-p002: account-admin's root form, the whole program built for the Linux host (ASan and UBSan) with every file it
# touches moved into a fresh folder (the account files, the lock, /etc/passkey, the homes, the skeleton, the record of
# the last user ID, cron's and at's spools, the automatic login), run as root through sudo in a private mount namespace
# (for the tmpfs mounts of the rollback and the mount-point checks; the host's files are not touched).  Checks: useradd's
# request and its files, an unlock without a password (F1), the last administrator who can log in kept against -G, -s,
# -L, removal and -g (S1, S2), user IDs never given twice (S3), a removal's jobs and automatic login cleaned (S3), the
# lock held by another process (S4, flock), the files put back after a failed write (S5), what is left of a name without
# a passwd line cleaned (S5), a mount point in a home left (S6), a home the user does not own kept (F2), another user's
# primary group kept (M7), and Settings' form from kei through a set-user-ID copy (ws089-p026's add, busy, removal
# keeping the home and home-exists, each answered within 20 s; T1-600).  The last line is
# "account-admin-root-host-test: PASS".
#   sh plan/tools/accounts/account-admin-root-host-test.sh   (from the repository's top; needs sudo, unshare, flock, openssl, perl,
#   setpriv)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
mkdir -p build/tmp
OUT=$(realpath "$(mktemp -d build/tmp/ws196-account-admin.XXXXXX)")
mkdir -p "$OUT/etc" "$OUT/shadowdir" "$OUT/home" "$OUT/skel" "$OUT/cron" "$OUT/at"
cc -std=gnu17 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -Wno-format-truncation \
	-fsanitize=address,undefined -fno-sanitize-recover=all -I. -Iinclude \
	-DACCOUNT_PASSWD_FILE="\"$OUT/etc/passwd\"" -DACCOUNT_GROUP_FILE="\"$OUT/etc/group\"" \
	-DACCOUNT_SHADOW_PATH="\"$OUT/shadowdir/shadow\"" -DACCOUNT_SHADOW_LOCK="\"$OUT/etc/shadow.lock\"" \
	-DPASSKEY_FILE="\"$OUT/etc/passkey\"" -DADMIN_HOME_BASE="\"$OUT/home\"" -DADMIN_SKELETON="\"$OUT/skel\"" \
	-DADMIN_LAST_ID_FILE="\"$OUT/etc/account-admin.last-id\"" -DADMIN_CRON_SPOOL="\"$OUT/cron\"" \
	-DADMIN_AT_SPOOL="\"$OUT/at\"" -DADMIN_AUTOLOGIN_FILE="\"$OUT/etc/autologin\"" \
	-DADMIN_LANGUAGE_FILE="\"$OUT/etc/language\"" -DADMIN_LANGUAGE_DIRECTORY="\"$OUT/etc\"" \
	-o "$OUT/account-admin" userland/base/account-admin/main.c userland/base/account-admin/edit.c \
	userland/base/common/account.c userland/base/login/verify.c userland/base/passkey/record.c -lcrypt
# A login recorded in utmp (Settings' busy on Linux, where there is no /dev/system).
cat > "$OUT/utmp-login.c" <<'EOF'
#include <string.h>
#include <unistd.h>
#include <utmpx.h>
int
main(int argc, char **argv)
{
	struct utmpx record;

	if (argc != 2)
		return 2;
	memset(&record, 0, sizeof(record));
	record.ut_type = USER_PROCESS;
	record.ut_pid = getpid();
	strncpy(record.ut_user, argv[1], sizeof(record.ut_user));
	strncpy(record.ut_line, "pts/9", sizeof(record.ut_line));
	strncpy(record.ut_id, "p9", sizeof(record.ut_id));
	setutxent();
	if (pututxline(&record) == NULL)
		return 1;
	endutxent();
	return 0;
}
EOF
cc -std=gnu17 -D_GNU_SOURCE -O1 -Wall -Wextra -Werror -o "$OUT/utmp-login" "$OUT/utmp-login.c"
hash=$(openssl passwd -6 -salt p002salt secret123)
cat > "$OUT/etc/passwd" <<EOF
root:x:0:0:root:/root:/bin/sh
sshd:x:22:22::/var/empty:/usr/sbin/nologin
kei:x:1000:1000:Kei:$OUT/home/kei:/bin/sh
hal:x:1002:1002::$OUT/home/hal:/bin/sh
ivy:x:1004:1002::$OUT/home/ivy:/bin/sh
dave:x:1005:0:Dave:$OUT/home/dave:/bin/sh
EOF
cat > "$OUT/etc/group" <<EOF
wheel:x:0:root,ghost
network:x:69:kei
sshd:x:22:
kei:x:1000:
hal:x:1002:
ivy:x:1004:
ghost:x:1009:
EOF
cat > "$OUT/shadowdir/shadow" <<EOF
root:*:20000:0:99999:7:::
kei:$hash:20000:0:99999:7:::
hal:$hash:20000:0:99999:7:::
ivy:$hash:20000:0:99999:7:::
dave:$hash:20000:0:99999:7:::
ghost:$hash:20000:0:99999:7:::
EOF
printf 'echo hello\n' > "$OUT/skel/.profile"
cat > "$OUT/run.sh" <<EOF
set -u
O="$OUT"
A="\$O/account-admin"
chmod 0400 "\$O/shadowdir/shadow"
mkdir -p "\$O/home/old" "\$O/home/jay"
chown 1010:1010 "\$O/home/old"
: > "\$O/cron/1020"
ask() { printf '%b' "\$1" | "\$A"; }
expect() {
	got=\$(ask "\$2")
	if [ "\$got" = "\$3" ]; then echo "ok \$1"; else echo "FAIL \$1 (got '\$got', wanted '\$3')"; fi
}
has() { if grep -q -e "\$3" "\$2"; then echo "ok \$1"; else echo "FAIL \$1 (no '\$3' in \$2)"; fi; }
lacks() { if grep -q -e "\$3" "\$2"; then echo "FAIL \$1 ('\$3' in \$2)"; else echo "ok \$1"; fi; }
is() { if [ "\$2" = "\$3" ]; then echo "ok \$1"; else echo "FAIL \$1 (got '\$2', wanted '\$3')"; fi; }
there() { if [ -e "\$2" ]; then echo "ok \$1"; else echo "FAIL \$1 (\$2 missing)"; fi; }
gone() { if [ -e "\$2" ]; then echo "FAIL \$1 (\$2 there)"; else echo "ok \$1"; fi; }

# useradd: above every used ID (a home owned by 1010, cron's 1020), private group, locked password, home with the skeleton.
expect add-bob 'user-add\nname bob\ngroups wheel\n' ok
has add-bob-passwd "\$O/etc/passwd" "^bob:x:1021:1021::\$O/home/bob:/bin/sh\\\$"
has add-bob-group "\$O/etc/group" '^bob:x:1021:\$'
has add-bob-wheel "\$O/etc/group" '^wheel:x:0:root,ghost,bob\$'
has add-bob-shadow "\$O/shadowdir/shadow" '^bob:!:'
is add-bob-home-owner "\$(stat -c %u:%g:%a "\$O/home/bob")" 1021:1021:700
there add-bob-skeleton "\$O/home/bob/.profile"
is add-bob-record "\$(cat "\$O/etc/account-admin.last-id")" 1021
is add-bob-shadow-mode "\$(stat -c %a "\$O/shadowdir/shadow")" 400
# A person's new account gets the first PIN, 0000, in /etc/passkey (root's alone).
has add-bob-pin "\$O/etc/passkey" '^bob:1021:pin:\\\$6\\\$'
pin=\$(grep '^bob:1021:pin:' "\$O/etc/passkey" | cut -d: -f4-)
is add-bob-pin-0000 "\$(LC_ALL=C perl -e 'print crypt("0000", \$ARGV[0])' "\$pin")" "\$pin"
is add-bob-passkey-mode "\$(stat -c %a "\$O/etc/passkey")" 600
expect add-bob-again 'user-add\nname bob\n' 'error name-taken'

# F1: an account made without a password is not unlocked into an empty one.
expect unlock-no-password 'user-modify\nname bob\npassword unlock\n' 'error no-password'
has unlock-no-password-kept "\$O/shadowdir/shadow" '^bob:!:'

# kei into wheel; dave's own group is not there; dave out of wheel as its primary group (kei stays an administrator).
expect kei-wheel 'user-modify\nname kei\ngroups-add wheel\n' ok
expect dave-own-missing 'user-modify\nname dave\ngroup dave\n' 'error bad-value'
expect dave-network 'user-modify\nname dave\ngroup network\n' ok
has dave-network-passwd "\$O/etc/passwd" '^dave:x:1005:69:'

# S1, S2: kei is the last administrator who can log in (bob, in wheel, has no password).
expect last-groups 'user-modify\nname kei\ngroups network\n' 'error last-administrator'
expect last-shell 'user-modify\nname kei\nshell /usr/sbin/nologin\n' 'error last-administrator'
expect last-lock 'user-modify\nname kei\npassword lock\n' 'error last-administrator'
expect last-remove 'user-remove\nname kei\n' 'error last-administrator'
has last-kept "\$O/etc/group" '^wheel:x:0:root,ghost,bob,kei\$'

# With dave back in wheel, kei can be locked and unlocked to the same hash.
expect dave-wheel 'user-modify\nname dave\ngroup wheel\n' ok
expect kei-lock 'user-modify\nname kei\npassword lock\n' ok
has kei-locked "\$O/shadowdir/shadow" '^kei:!\\\$6\\\$'
expect kei-unlock 'user-modify\nname kei\npassword unlock\n' ok
is kei-unlocked "\$(grep '^kei:' "\$O/shadowdir/shadow" | cut -d: -f2)" '$hash'
expect bad-shell 'user-modify\nname kei\nshell /nonexistent/sh\n' 'error bad-value'
expect kei-comment 'user-modify\nname kei\ncomment Kei K\n' ok
has kei-comment-passwd "\$O/etc/passwd" '^kei:x:1000:1000:Kei K:'

# The user ID root gives: one in use refused, a free one taken and recorded.
expect uid-taken 'user-add\nname erin\nuid 1005\n' 'error id-taken'
expect uid-group-taken 'user-add\nname erin\nuid 1009\n' 'error id-taken'
expect uid-free 'user-add\nname erin\nuid 1500\nhome none\ncomment Erin E\nshell /bin/sh\n' ok
has uid-free-passwd "\$O/etc/passwd" '^erin:x:1500:1500:Erin E:'
gone uid-free-no-home "\$O/home/erin"
is uid-free-record "\$(cat "\$O/etc/account-admin.last-id")" 1500

# A removal: its home, cron table, at job and automatic login go; another's cron table stays.
: > "\$O/home/bob/notes"
: > "\$O/cron/1021"
: > "\$O/at/1.1021.1.a"
chown 1021 "\$O/at/1.1021.1.a"
printf 'bob\n' > "\$O/etc/autologin"
expect remove-bob 'user-remove\nname bob\nhome remove\n' ok
lacks remove-bob-passwd "\$O/etc/passwd" '^bob:'
lacks remove-bob-shadow "\$O/shadowdir/shadow" '^bob:'
lacks remove-bob-group "\$O/etc/group" '^bob:'
lacks remove-bob-pin "\$O/etc/passkey" '^bob:'
has remove-bob-wheel "\$O/etc/group" '^wheel:x:0:root,ghost,kei\$'
gone remove-bob-home "\$O/home/bob"
gone remove-bob-cron "\$O/cron/1021"
gone remove-bob-at "\$O/at/1.1021.1.a"
gone remove-bob-autologin "\$O/etc/autologin"
there remove-bob-other-cron "\$O/cron/1020"

# S3: the highest ID removed is not given again.
expect add-carol 'user-add\nname carol\n' ok
has add-carol-uid "\$O/etc/passwd" '^carol:x:1501:1501:'
expect remove-carol 'user-remove\nname carol\nhome remove\n' ok
expect add-dan 'user-add\nname dan\nhome none\n' ok
has add-dan-uid "\$O/etc/passwd" '^dan:x:1502:1502:'

# M7: hal's group is ivy's primary group, so it stays when hal goes.
expect remove-hal 'user-remove\nname hal\n' ok
lacks remove-hal-passwd "\$O/etc/passwd" '^hal:'
has remove-hal-group-kept "\$O/etc/group" '^hal:x:1002:\$'

# S5: what is left of ghost without a passwd line is cleaned, then there is nothing.
expect remove-ghost 'user-remove\nname ghost\n' ok
lacks remove-ghost-shadow "\$O/shadowdir/shadow" '^ghost:'
lacks remove-ghost-group "\$O/etc/group" '^ghost:'
has remove-ghost-wheel "\$O/etc/group" '^wheel:x:0:root,kei\$'
expect remove-ghost-again 'user-remove\nname ghost\n' 'error no-such-user'
expect remove-system 'user-remove\nname sshd\n' 'error root'

# Forms refused: a home removed under a forced removal, Settings' form from root.
expect remove-home-force 'user-remove\nname ivy\nhome remove\nforce yes\n' 'error bad-request'
expect settings-form 'secret\nremove\nivy\nkeep-home\n' 'error bad-request'

# F2: a home that was there before the user, not the user's, is kept.
expect add-jay 'user-add\nname jay\nhome none\n' ok
expect remove-jay 'user-remove\nname jay\nhome remove\n' ok
there remove-jay-home-kept "\$O/home/jay"

# S6: a file system mounted in a home is left; the rest goes.
expect add-kim 'user-add\nname kim\n' ok
mkdir "\$O/home/kim/mnt"
mount -t tmpfs -o size=64k tmpfs "\$O/home/kim/mnt"
: > "\$O/home/kim/mnt/keep"
: > "\$O/home/kim/file"
expect remove-kim 'user-remove\nname kim\nhome remove\n' ok
gone remove-kim-file "\$O/home/kim/file"
there remove-kim-mount-kept "\$O/home/kim/mnt/keep"
umount "\$O/home/kim/mnt"

# Settings' form (ws089-p026's steps 3 and 7; T1-600): kei (uid 1000, in wheel, password secret123) through the
# set-user-ID copy, the host's passwd, group and shadow bound to the test's (bound again before each request: the tool
# replaces the files, and a bind keeps the old one) for getpwuid, getgrnam and getspnam.  alice is added, busy (a login
# recorded in utmp: /dev/system, the guest's list of processes, is not on Linux), removed keeping her home once the
# login is gone, and her name again is home-exists.  Every answer comes within 20 s (timeout's 124 would be a hang).
S="\$O/account-admin-settings"
cp "\$A" "\$S"
chown root:root "\$S"
chmod 4755 "\$S"
mount -t tmpfs -o size=64k tmpfs /run
: > /run/utmp
bind() {
	for f in passwd group; do umount "/etc/\$f" 2>/dev/null; mount --bind "\$O/etc/\$f" "/etc/\$f"; done
	umount /etc/shadow 2>/dev/null
	mount --bind "\$O/shadowdir/shadow" /etc/shadow
}
kei() {
	bind
	printf '%b' "\$1" | ASAN_OPTIONS=detect_leaks=0 timeout 20 setpriv --reuid=1000 --regid=1000 --clear-groups "\$S"
	echo "status=\$?"
}
settings() {
	got=\$(kei "\$2" | tr '\n' ' ')
	if [ "\$got" = "\$3" ]; then echo "ok \$1"; else echo "FAIL \$1 (got '\$got', wanted '\$3')"; fi
}
settings settings-wrong-password 'wrong\nreset-password\nkei\nwhatever12\n' 'error bad-password status=1 '
settings settings-add-alice 'secret123\nadd\nalice\nAlice Tanaka\nalicepass1\nuser\n' 'ok status=0 '
has settings-alice-passwd "\$O/etc/passwd" '^alice:x:[0-9]*:[0-9]*:Alice Tanaka:'
is settings-alice-home "\$(stat -c %u:%a "\$O/home/alice" 2>&1)" "\$(grep '^alice:' "\$O/etc/passwd" | cut -d: -f3):700"
settings settings-name-taken 'secret123\nadd\nalice\nAlice\nalicepass1\nuser\n' 'error name-taken status=1 '
"\$O/utmp-login" alice
settings settings-busy 'secret123\nremove\nalice\nkeep-home\n' 'error busy status=1 '
: > /run/utmp
settings settings-remove-alice 'secret123\nremove\nalice\nkeep-home\n' 'ok status=0 '
lacks settings-alice-gone "\$O/etc/passwd" '^alice:'
lacks settings-alice-gone-shadow "\$O/shadowdir/shadow" '^alice:'
lacks settings-alice-gone-group "\$O/etc/group" '^alice:'
there settings-alice-home-kept "\$O/home/alice"
settings settings-home-exists 'secret123\nadd\nalice\nAlice\nalicepass1\nuser\n' 'error home-exists status=1 '
settings settings-self 'secret123\nremove\nkei\nkeep-home\n' 'error self status=1 '
umount /etc/passwd /etc/group /etc/shadow
umount /run

# S4: the lock held by another process for longer than the wait is not taken over; once given back it is taken.
flock -x "\$O/etc/shadow.lock" sleep 8 &
holder=\$!
sleep 1
expect lock-held 'user-add\nname lou\n' 'error failed'
wait \$holder
expect lock-free 'user-add\nname lou\nhome none\n' ok
there lock-file-stays "\$O/etc/shadow.lock"

# S5: shadow cannot be written (a full file system): group and passwd are put back as they were.
cp "\$O/etc/passwd" "\$O/passwd.before"
cp "\$O/etc/group" "\$O/group.before"
cp "\$O/shadowdir/shadow" "\$O/shadow.copy"
mount -t tmpfs -o size=8k tmpfs "\$O/shadowdir"
cp "\$O/shadow.copy" "\$O/shadowdir/shadow"
chmod 0400 "\$O/shadowdir/shadow"
dd if=/dev/zero of="\$O/shadowdir/filler" bs=1024 count=64 2>/dev/null || true
expect rollback 'user-add\nname max\nhome none\n' 'error failed'
if cmp -s "\$O/passwd.before" "\$O/etc/passwd"; then echo "ok rollback-passwd"; else echo "FAIL rollback-passwd"; fi
if cmp -s "\$O/group.before" "\$O/etc/group"; then echo "ok rollback-group"; else echo "FAIL rollback-group"; fi
umount "\$O/shadowdir"
echo "== done"
EOF
sudo unshare -m sh "$OUT/run.sh" > "$OUT/out.txt" 2>&1 || true
failed=0
if grep -q '^FAIL' "$OUT/out.txt"; then failed=1; fi
if grep -q 'AddressSanitizer\|runtime error' "$OUT/out.txt"; then failed=1; fi
if ! grep -q '^== done' "$OUT/out.txt"; then failed=1; fi
grep -v '^ok ' "$OUT/out.txt" | grep -v '^== done' || true
echo "$(grep -c '^ok ' "$OUT/out.txt") checks passed"
if [ $failed -eq 0 ]; then echo "account-admin-root-host-test: PASS"; else echo "account-admin-root-host-test: FAIL"; fi
exit $failed
