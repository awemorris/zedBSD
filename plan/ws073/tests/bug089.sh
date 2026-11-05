#!/bin/sh
# BUG-089: a second checkout must reuse a toolchain it links to (build/llvm,
# build/llvm-source, build/llvm-build pointing into another checkout's build)
# without rebuilding it because a checkout gave the patch a new time, and must
# never write into it.
#
# The test copies toolchain/llvm and build-jobs.mk into a scratch tree and
# builds small stand-in toolchains outside it: an accepted one (the identity
# records of the pinned LLVM release and patch, and scripts in place of the
# tools), a stale one, and a missing one.  Every make runs in the scratch tree
# (make -C tree/toolchain/llvm); nothing under the checkout's build/ is read
# or written.  No LLVM is fetched, extracted or built: a test that would
# reach one of those fails instead.
#
#   sh plan/ws073/tests/bug089.sh [SCRATCH]
#
# Prints "PASS bug089" when every case passes.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
root=$(cd "$(dirname "$0")/../../.." && pwd)
scratch=${1:-$(mktemp -d "${TMPDIR:-/tmp}/bug089.XXXXXX")}
tree=$scratch/tree
foreign=$scratch/foreign
failures=0

# Runs the toolchain's own makefile in the scratch tree.
tmake() {
	make --no-print-directory -C "$tree/toolchain/llvm" "$@"
}

# Prints the value of one of the toolchain makefile's variables.
value() {
	tmake -s --eval="bug089-value: ; @printf '%s\n' \$($1)" bug089-value
}

pass() {
	echo "PASS $1"
}

fail() {
	echo "FAIL $1"
	failures=$((failures + 1))
}

# Records every entry of a tree, to show that a case left it alone.
listing() {
	find "$1/" -printf '%P %y %s %T@\n' | LC_ALL=C sort
}

# Points build/NAME of the scratch tree at TARGET.
link() {
	rm -f "$tree/build/$1"
	ln -s "$2" "$tree/build/$1"
}

# Builds the scratch tree: the toolchain makefiles only, which is all the
# LLVM rules read.
rm -rf "$tree" "$foreign"
mkdir -p "$tree/toolchain" "$tree/build" "$foreign"
cp -R "$root/toolchain/llvm" "$tree/toolchain/llvm"
rm -rf "$tree/toolchain/llvm/distfiles"
cp "$root/build-jobs.mk" "$tree/build-jobs.mk"
version=$(value LLVM_VERSION)
level=$(value LLVM_PATCH_LEVEL)
patch=$tree/toolchain/llvm/patches/0001-add-zedbsd-x86-target.patch

# An accepted installation: its identity, the tools (scripts that answer the
# version checks), the license and the stamp.
mkdir -p "$foreign/llvm-ok/bin" "$foreign/llvm-ok/share/licenses/llvm"
printf 'version=%s patch=%s\n' "$version" "$level" \
	> "$foreign/llvm-ok/.zedbsd-install-identity"
for tool in $(value ZEDBSD_LLVM_INSTALLED_TOOL_NAMES); do
	printf '#!/bin/sh\necho "clang version %s LLD %s"\n' "$version" "$version" \
		> "$foreign/llvm-ok/bin/$tool"
	chmod +x "$foreign/llvm-ok/bin/$tool"
done
echo license > "$foreign/llvm-ok/share/licenses/llvm/LICENSE.TXT"
touch "$foreign/llvm-ok/.zedbsd-install-$version-$level"

# An accepted source tree: the identity of the release plus this patch, and
# the extraction and verification stamps.
mkdir -p "$foreign/source-ok"
tmake -s --eval="bug089-lines: ; @printf '%s\n' \$(ZEDBSD_LLVM_SOURCE_IDENTITY_LINES)" \
	bug089-lines > "$foreign/source-ok/.zedbsd-source-identity"
echo license > "$foreign/source-ok/LICENSE.TXT"
touch "$foreign/source-ok/.zedbsd-source-$version-$level"
touch "$foreign/source-ok/.zedbsd-source-verified-$version-$level"

# An accepted build tree: the configuration identity for this host, and host
# generators built after it.
mkdir -p "$foreign/build-ok/bin"
tmake -s --eval="bug089-config: ; @\$(ZEDBSD_LLVM_CONFIG_IDENTITY_COMMANDS); printf '%s\n' \"\$\$identity\"" \
	bug089-config > "$foreign/build-ok/.zedbsd-config-identity"
for tool in $(value ZEDBSD_LLVM_NATIVE_TOOLS); do
	printf '#!/bin/sh\n' > "$foreign/build-ok/bin/$tool"
	chmod +x "$foreign/build-ok/bin/$tool"
done
touch "$foreign/build-ok/.zedbsd-native-tools"

# The stale ones.
mkdir -p "$foreign/llvm-stale/bin"
printf 'version=%s patch=zedbsd0\n' "$version" > "$foreign/llvm-stale/.zedbsd-install-identity"
mkdir -p "$foreign/build-stale"
echo old > "$foreign/build-stale/.zedbsd-config-identity"
touch "$foreign/build-stale/.zedbsd-native-tools"

# A checkout gives the patch a newer time than any record.
touch "$patch"
source_stamp=$tree/build/llvm-source/.zedbsd-source-verified-$version-$level
native_stamp=$tree/build/llvm-build/.zedbsd-native-tools

# 1. A missing toolchain behind a link elsewhere: the install is refused at
# once, and nothing is fetched, extracted or configured first.
link llvm "$foreign/missing"
out=$(tmake install 2>&1)
if echo "$out" | grep -q 'refusing to install the LLVM toolchain' &&
    ! test -e "$tree/toolchain/llvm/distfiles" &&
    ! test -e "$tree/build/llvm-source" && ! test -e "$tree/build/llvm-build" &&
    ! test -e "$foreign/missing"; then
	pass "bug089 missing toolchain refused"
else
	echo "$out" | tail -5
	fail "bug089 missing toolchain refused"
fi

# 2. The binary cache is refused the same way, before it downloads.
out=$(tmake toolchain-cache 2>&1)
if echo "$out" | grep -q 'refusing to install the LLVM toolchain cache' &&
    ! test -e "$tree/build/releases" && ! test -e "$foreign/missing"; then
	pass "bug089 toolchain-cache refused"
else
	echo "$out" | tail -5
	fail "bug089 toolchain-cache refused"
fi

# 3. A stale toolchain elsewhere is refused and left as it was.
link llvm "$foreign/llvm-stale"
before=$(listing "$foreign/llvm-stale")
out=$(tmake install 2>&1)
if echo "$out" | grep -q 'refusing to install the LLVM toolchain' &&
    test "$before" = "$(listing "$foreign/llvm-stale")"; then
	pass "bug089 stale toolchain refused"
else
	echo "$out" | tail -5
	fail "bug089 stale toolchain refused"
fi

# 4. Accepted trees elsewhere are used as they are, although the patch is
# newer than every record in them: make -n schedules nothing, and a real
# make leaves them alone.
link llvm "$foreign/llvm-ok"
link llvm-source "$foreign/source-ok"
link llvm-build "$foreign/build-ok"
before=$(listing "$foreign")
out=$(tmake -n install "$source_stamp" "$native_stamp" 2>&1)
if echo "$out" | grep -Eq 'curl|tar -x|cmake|refusing|find .* -delete'; then
	echo "$out" | grep -E 'curl|tar -x|cmake|refusing|find .* -delete' | head -5
	fail "bug089 accepted trees reused (make -n)"
else
	pass "bug089 accepted trees reused (make -n)"
fi
out=$(tmake install "$source_stamp" "$native_stamp" 2>&1)
status=$?
if test "$status" = 0 && test "$before" = "$(listing "$foreign")"; then
	pass "bug089 accepted trees reused and untouched"
else
	echo "$out" | tail -5
	fail "bug089 accepted trees reused and untouched"
fi

# 5. A changed patch makes the source elsewhere stale: refused, not used and
# not replaced.  The explicit permission turns the refusal into extraction.
echo '# bug089' >> "$patch"
out=$(tmake llvm-source 2>&1)
if echo "$out" | grep -q 'refusing to extract the LLVM source' &&
    test "$before" = "$(listing "$foreign")"; then
	pass "bug089 changed patch refuses the source elsewhere"
else
	echo "$out" | tail -5
	fail "bug089 changed patch refuses the source elsewhere"
fi
out=$(tmake -n ZEDBSD_LLVM_ALLOW_FOREIGN=yes llvm-source 2>&1)
if echo "$out" | grep -q 'tar -xJf'; then
	pass "bug089 ZEDBSD_LLVM_ALLOW_FOREIGN=yes allows the extraction"
else
	echo "$out" | tail -5
	fail "bug089 ZEDBSD_LLVM_ALLOW_FOREIGN=yes allows the extraction"
fi
cp "$root/toolchain/llvm/patches/0001-add-zedbsd-x86-target.patch" "$patch"

# 6. A build tree elsewhere configured for something else: building the host
# generators in it is refused.
link llvm-build "$foreign/build-stale"
before=$(listing "$foreign/build-stale")
out=$(tmake "$native_stamp" 2>&1)
if echo "$out" | grep -q 'refusing to build the host generators' &&
    test "$before" = "$(listing "$foreign/build-stale")"; then
	pass "bug089 stale build tree refused"
else
	echo "$out" | tail -5
	fail "bug089 stale build tree refused"
fi

# 7. A link that stays inside the checkout (build/llvm -> llvm-zedbsd8, as the
# main checkout has) belongs to it; a link elsewhere does not.
rm -f "$tree/build/llvm"
mkdir -p "$tree/build/llvm-real"
ln -s llvm-real "$tree/build/llvm"
inside=$(value ZEDBSD_LLVM_INSTALL_OWNED)
link llvm "$foreign/llvm-ok"
outside=$(value ZEDBSD_LLVM_INSTALL_OWNED)
if test "$inside" = yes && test -z "$outside"; then
	pass "bug089 ownership of links"
else
	echo "inside=$inside outside=$outside"
	fail "bug089 ownership of links"
fi

if test "$failures" = 0; then
	echo "PASS bug089"
	rm -rf "$scratch"
	exit 0
fi
echo "FAIL bug089: $failures case(s); scratch kept in $scratch"
exit 1
