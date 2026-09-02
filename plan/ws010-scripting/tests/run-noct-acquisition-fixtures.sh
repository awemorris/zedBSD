#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

# Exercise the real Noct acquisition and extraction recipes in an isolated
# miniature repository.  Production release identities remain immutable: each
# miniature repository receives its own generated, override-qualified fixture
# identity and the fetch helper refuses every non-fixture URL.

set -eu
LC_ALL=C
export LC_ALL

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
production_noct=$repo/userland/base/noct
production_version=$production_noct/version.mk
fixture_fetch=$script_dir/noct-acquisition-fixture-fetch.sh
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

fixture_version=1.0
fixture_root=NoctFixture-$fixture_version
fixture_archive_name=$fixture_root.tar.gz
fixture_url=fixture://$fixture_archive_name

fail()
{
	echo "Noct lifecycle fixture: FAIL: $*" >&2
	exit 1
}

pass()
{
	echo "Noct lifecycle fixture: PASS: $*"
}

assert_absent()
{
	path=$1
	label=$2
	if test -e "$path" || test -L "$path"; then
		fail "$label remains at $path"
	fi
}

assert_no_pipeline_residue()
{
	package=$1
	archive=$package/distfiles/$fixture_archive_name
	source=$package/noct

	assert_absent "$archive.lock" "archive lock"
	assert_absent "$source.lock" "source lock"
	for residue in \
		"$package/distfiles"/."$fixture_archive_name".* \
		"$package"/.noct-"$fixture_version".*; do
		assert_absent "$residue" "pipeline temporary"
	done
	if find "$package" \( -name '*.rej' -o -name '*.orig' \) -print | grep -q .; then
		fail "patch residue remains below $package"
	fi
}

archive_size()
{
	wc -c < "$1" | tr -d '[:space:]'
}

archive_sha256()
{
	sha256sum "$1" | awk '{print $1}'
}

write_fixture_version()
{
	destination=$1
	expected_archive=$2
	size=$(archive_size "$expected_archive")
	digest=$(archive_sha256 "$expected_archive")
	{
		printf '%s\n' \
			'override ZEDBSD_NOCT_VERSION := 1.0' \
			'override ZEDBSD_NOCT_TAG := fixture-v1.0' \
			'override ZEDBSD_NOCT_TAG_COMMIT := fixture-commit' \
			'override ZEDBSD_NOCT_ARCHIVE_ROOT := NoctFixture-1.0' \
			'override ZEDBSD_NOCT_ARCHIVE_NAME := NoctFixture-1.0.tar.gz' \
			'override ZEDBSD_NOCT_ARCHIVE_URL := fixture://NoctFixture-1.0.tar.gz'
		printf '%s\n' \
			"override ZEDBSD_NOCT_ARCHIVE_SIZE := $size" \
			"override ZEDBSD_NOCT_ARCHIVE_SHA256 := $digest" \
			'override ZEDBSD_NOCT_PATCH_LEVEL := fixture1'
	} > "$destination"
}

prepare_case()
{
	case_name=$1
	expected_archive=$2
	patch_file=$3
	case_root=$temporary/cases/$case_name/repo
	case_package=$case_root/userland/base/noct
	case_archive=$case_package/distfiles/$fixture_archive_name
	case_source=$case_package/noct
	case_log=$temporary/cases/$case_name/make.log

	mkdir -p "$case_package/patches"
	cp "$production_noct/Makefile" "$case_package/Makefile"
	cp "$repo/userland/base/package.mk" "$case_root/userland/base/package.mk"
	cp "$repo/userland/download.mk" "$case_root/userland/download.mk"
	cp "$patch_file" \
		"$case_package/patches/0001-connect-zedbsd-target-adapter.patch"
	write_fixture_version "$case_package/version.mk" "$expected_archive"
}

run_patch()
{
	delivered_archive=$1
	fetch_mode=$2
	MAKEFLAGS= \
	ZEDBSD_NOCT_FIXTURE_FETCH_SOURCE=$delivered_archive \
	ZEDBSD_NOCT_FIXTURE_FETCH_MODE=$fetch_mode \
	ZEDBSD_NOCT_FETCH="sh $fixture_fetch" \
	make -C "$case_package" --no-print-directory \
		ZEDBSD_STANDALONE_CONFIG="$temporary/missing-config.mk" patch \
		> "$case_log" 2>&1
}

expect_download_failure()
{
	case_name=$1
	expected_archive=$2
	delivered_archive=$3
	fetch_mode=$4
	expected_message=$5

	prepare_case "$case_name" "$expected_archive" "$success_patch"
	if run_patch "$delivered_archive" "$fetch_mode"; then
		fail "$case_name unexpectedly succeeded"
	fi
	grep -F "$expected_message" "$case_log" >/dev/null || {
		sed -n '1,160p' "$case_log" >&2
		fail "$case_name did not reach its intended rejection"
	}
	assert_absent "$case_archive" "canonical archive"
	assert_absent "$case_source" "canonical source"
	assert_no_pipeline_residue "$case_package"
	pass "$case_name rejects input without publishing archive or source"
}

for identity in \
	ZEDBSD_NOCT_ARCHIVE_URL \
	ZEDBSD_NOCT_ARCHIVE_SIZE \
	ZEDBSD_NOCT_ARCHIVE_SHA256; do
	grep -Eq "^override[[:space:]]+$identity[[:space:]]*:=" \
		"$production_version" ||
		fail "production identity is not override-qualified: $identity"
done

fixtures=$temporary/fixtures
mkdir -p "$fixtures/safe/$fixture_root" "$fixtures/strict/$fixture_root"
printf '%s\n' alpha beta target gamma delta \
	> "$fixtures/safe/$fixture_root/fixture.txt"
printf '%s\n' ALPHA beta target gamma delta \
	> "$fixtures/strict/$fixture_root/fixture.txt"

success_patch=$fixtures/success.patch
printf '%s\n' \
	'--- a/fixture.txt' \
	'+++ b/fixture.txt' \
	'@@ -1,5 +1,5 @@' \
	' alpha' \
	' beta' \
	'-target' \
	'+patched' \
	' gamma' \
	' delta' > "$success_patch"

safe_archive=$fixtures/safe.tar.gz
strict_archive=$fixtures/strict.tar.gz
tar -czf "$safe_archive" -C "$fixtures/safe" "$fixture_root"
tar -czf "$strict_archive" -C "$fixtures/strict" "$fixture_root"

wrong_size=$fixtures/wrong-size.bin
printf 'truncated fixture\n' > "$wrong_size"

wrong_digest=$fixtures/wrong-digest.tar.gz
cp "$safe_archive" "$wrong_digest"
printf X | dd of="$wrong_digest" bs=1 seek=0 conv=notrunc 2>/dev/null
test "$(archive_size "$wrong_digest")" = "$(archive_size "$safe_archive")" ||
	fail "wrong-digest fixture changed archive size"
test "$(archive_sha256 "$wrong_digest")" != "$(archive_sha256 "$safe_archive")" ||
	fail "wrong-digest fixture did not change archive digest"

unsafe_path_root=$fixtures/unsafe-path-root
mkdir -p "$unsafe_path_root/$fixture_root"
printf 'escape\n' > "$unsafe_path_root/$fixture_root/payload"
unsafe_path_archive=$fixtures/unsafe-path.tar.gz
tar -czf "$unsafe_path_archive" \
	--transform="s#$fixture_root/payload#$fixture_root/../escape#" \
	-C "$unsafe_path_root" "$fixture_root"

unsafe_type_root=$fixtures/unsafe-type-root
mkdir -p "$unsafe_type_root/$fixture_root"
printf 'payload\n' > "$unsafe_type_root/$fixture_root/payload"
ln -s payload "$unsafe_type_root/$fixture_root/link"
unsafe_type_archive=$fixtures/unsafe-type.tar.gz
tar -czf "$unsafe_type_archive" -C "$unsafe_type_root" "$fixture_root"

expect_download_failure wrong-size "$safe_archive" "$wrong_size" copy \
	"archive size mismatch"
expect_download_failure wrong-digest "$safe_archive" "$wrong_digest" copy \
	"archive SHA-256 mismatch"
expect_download_failure unsafe-member-path "$unsafe_path_archive" \
	"$unsafe_path_archive" copy "unsafe archive member"
expect_download_failure unsafe-member-type "$unsafe_type_archive" \
	"$unsafe_type_archive" copy "unsupported archive member type"
expect_download_failure partial-download "$safe_archive" "$safe_archive" partial \
	"simulated interrupted transfer"

# Positive control: the same copied production recipes must acquire, extract,
# patch, verify, and publish a well-formed fixture.
prepare_case success "$safe_archive" "$success_patch"
run_patch "$safe_archive" copy || {
	sed -n '1,160p' "$case_log" >&2
	fail "positive control failed"
}
cmp -s "$safe_archive" "$case_archive" ||
	fail "positive control published the wrong archive"
test -d "$case_source" && test ! -L "$case_source" ||
	fail "positive control did not publish source"
grep -Fx patched "$case_source/fixture.txt" >/dev/null ||
	fail "positive control did not apply the tracked patch"
assert_no_pipeline_residue "$case_package"
pass "valid archive and exact patch publish a verified source"

# This patch is intentionally applicable only with fuzz.  Its successful
# fuzz=1 dry run proves the fixture is not an unrelated malformed patch; the
# production recipe must reject it because it explicitly requests fuzz=0.
strict_probe=$temporary/strict-probe
mkdir -p "$strict_probe"
cp "$fixtures/strict/$fixture_root/fixture.txt" "$strict_probe/fixture.txt"
(cd "$strict_probe" && \
	patch --dry-run --batch --forward --fuzz=1 -p1 < "$success_patch" \
		>/dev/null) || fail "strict-patch fixture is not fuzz-applicable"

prepare_case strict-patch "$strict_archive" "$success_patch"
if run_patch "$strict_archive" copy; then
	fail "strict patch unexpectedly succeeded"
fi
grep -F "Hunk #1 FAILED" "$case_log" >/dev/null || {
	sed -n '1,160p' "$case_log" >&2
	fail "strict patch did not fail at the intended hunk"
}
cmp -s "$strict_archive" "$case_archive" ||
	fail "strict-patch case did not retain its verified archive cache"
assert_absent "$case_source" "canonical source"
assert_no_pipeline_residue "$case_package"
pass "fuzz-only patch is rejected without publishing source"

printf '%s\n' \
	"Noct lifecycle fixtures: 7 cases passed without network access" \
	"Noct lifecycle fixtures: failed acquisition left no canonical bytes, locks, or temporary paths" \
	"Noct lifecycle fixtures: failed strict patch left no canonical source, lock, reject, or temporary path"
