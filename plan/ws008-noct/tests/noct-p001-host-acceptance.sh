#!/usr/bin/env bash
# WS008 Phase 001 reusable host acceptance.
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib

set -euo pipefail
IFS=$'\n\t'
export LC_ALL=C

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd -P)

MODE=all
REQUIRE_PACKAGE=0
REQUIRE_PARITY_CHANGES=0
OFFICIAL_DIR=${NOCT_OFFICIAL_DIR:-/home/awe/NoctLang}
INTEGRATION_DIR=${NOCT_INTEGRATION_DIR:-$REPO_ROOT/userland/noct/NoctLang}
ZEDBSD_ROOT=${ZEDBSD_SOURCE_DIR:-$REPO_ROOT}
ARTIFACT_REL=${NOCT_CANONICAL_ARTIFACT_REL:-build-zedbsd/noct}
INSTALL_SOURCE_ARTIFACT=${NOCT_INSTALL_SOURCE_ARTIFACT:-}
PACKAGE_ARTIFACT=${NOCT_PACKAGE_ARTIFACT:-$REPO_ROOT/build/amd64/bin/noct}
OUTPUT_DIR=${NOCT_ACCEPTANCE_OUTPUT_DIR:-}

usage()
{
	cat <<'EOF'
Usage: noct-p001-host-acceptance.sh [options]

Modes:
  --mode all       Run preset/build/ELF/package checks and parity (default).
  --mode build     Run preset/build/ELF/package checks only.
  --mode parity    Generate and verify the official/integration parity manifest.

Options:
  --official-dir DIR              Canonical Noct working tree.
  --integration-dir DIR           Official userland/noct/NoctLang checkout.
  --zedbsd-source-dir DIR         Valid value used for ZEDBSD_SOURCE_DIR.
  --canonical-artifact-relative P Path below the clean canonical copy.
  --install-source-artifact FILE  Canonical CMake artifact consumed by make.
  --package-artifact FILE         Installed/package-side amd64 artifact.
  --require-package-artifact      Fail instead of SKIP when FILE is absent.
  --require-parity-changes        Fail when neither Noct tree has changed paths.
  --output-dir DIR                New directory for logs and manifests.
  -h, --help                      Show this help.

Environment equivalents:
  NOCT_OFFICIAL_DIR, NOCT_INTEGRATION_DIR, ZEDBSD_SOURCE_DIR,
  NOCT_CANONICAL_ARTIFACT_REL, NOCT_INSTALL_SOURCE_ARTIFACT,
  NOCT_PACKAGE_ARTIFACT,
  NOCT_ACCEPTANCE_OUTPUT_DIR, FILE, READELF, NM.

The build check always invokes these commands literally in a clean copy:
  cmake --preset zedbsd
  cmake --build --preset zedbsd --parallel 16

If the package artifact exists it is always compared by SHA-256. Use
--require-package-artifact for the package-install acceptance gate after the
zedBSD amd64 image/package build has produced build/amd64/bin/noct.
EOF
}

die()
{
	printf 'NOCT-p001: FAIL: %s\n' "$*" >&2
	exit 1
}

note()
{
	printf 'NOCT-p001: %s\n' "$*"
}

need_arg()
{
	[[ $# -ge 2 ]] || die "option $1 requires an argument"
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--mode)
		need_arg "$@"
		MODE=$2
		shift 2
		;;
	--official-dir)
		need_arg "$@"
		OFFICIAL_DIR=$2
		shift 2
		;;
	--integration-dir)
		need_arg "$@"
		INTEGRATION_DIR=$2
		shift 2
		;;
	--zedbsd-source-dir)
		need_arg "$@"
		ZEDBSD_ROOT=$2
		shift 2
		;;
	--canonical-artifact-relative)
		need_arg "$@"
		ARTIFACT_REL=$2
		shift 2
		;;
	--package-artifact)
		need_arg "$@"
		PACKAGE_ARTIFACT=$2
		shift 2
		;;
	--install-source-artifact)
		need_arg "$@"
		INSTALL_SOURCE_ARTIFACT=$2
		shift 2
		;;
	--output-dir)
		need_arg "$@"
		OUTPUT_DIR=$2
		shift 2
		;;
	--require-package-artifact)
		REQUIRE_PACKAGE=1
		shift
		;;
	--require-parity-changes)
		REQUIRE_PARITY_CHANGES=1
		shift
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		die "unknown option: $1"
		;;
	esac
done

case "$MODE" in
all | build | parity) ;;
*) die "--mode must be all, build, or parity" ;;
esac

case "$ARTIFACT_REL" in
"" | /* | ../* | */../* | */..)
	die "canonical artifact must be a non-escaping relative path"
	;;
esac

for directory in "$OFFICIAL_DIR" "$INTEGRATION_DIR"; do
	[[ -d "$directory" ]] || die "Noct tree is not a directory: $directory"
	[[ -f "$directory/CMakeLists.txt" ]] ||
		die "Noct tree lacks CMakeLists.txt: $directory"
	git -C "$directory" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
		die "Noct tree is not a Git working tree: $directory"
done

OFFICIAL_DIR=$(CDPATH= cd -- "$OFFICIAL_DIR" && pwd -P)
INTEGRATION_DIR=$(CDPATH= cd -- "$INTEGRATION_DIR" && pwd -P)
[[ -d "$ZEDBSD_ROOT" ]] || die "zedBSD source root is not a directory: $ZEDBSD_ROOT"
ZEDBSD_ROOT=$(CDPATH= cd -- "$ZEDBSD_ROOT" && pwd -P)
if [[ -z "$INSTALL_SOURCE_ARTIFACT" ]]; then
	INSTALL_SOURCE_ARTIFACT=$INTEGRATION_DIR/$ARTIFACT_REL
elif [[ "$INSTALL_SOURCE_ARTIFACT" != /* ]]; then
	INSTALL_SOURCE_ARTIFACT=$PWD/$INSTALL_SOURCE_ARTIFACT
fi

if [[ -z "$OUTPUT_DIR" ]]; then
	OUTPUT_DIR=$REPO_ROOT/plan/ws008-noct/temp/p001-host-$(date -u +%Y%m%dT%H%M%SZ)-$$
elif [[ "$OUTPUT_DIR" != /* ]]; then
	OUTPUT_DIR=$PWD/$OUTPUT_DIR
fi
[[ ! -e "$OUTPUT_DIR" ]] || die "output directory already exists: $OUTPUT_DIR"
mkdir -p -- "$OUTPUT_DIR"

WORK_DIR=
cleanup()
{
	if [[ -n "$WORK_DIR" && -d "$WORK_DIR" ]]; then
		case "$WORK_DIR" in
		"${TMPDIR:-/tmp}"/ws008-p001.*) rm -rf -- "$WORK_DIR" ;;
		*) printf 'NOCT-p001: refusing unsafe cleanup path: %s\n' "$WORK_DIR" >&2 ;;
		esac
	fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

require_command()
{
	command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

copy_clean_source()
{
	local destination=$1

	mkdir -p -- "$destination"
	(
		cd -- "$OFFICIAL_DIR"
		tar --exclude='./build' --exclude='./build-*' \
			--exclude='./cmake-build-*' --exclude='./CMakeCache.txt' \
			--exclude='./CMakeFiles' --exclude='./CMakeUserPresets.json' \
			-cf - .
	) | (
		cd -- "$destination"
		tar -xf -
	)
}

show_failure_log()
{
	local log=$1
	printf '%s\n' "----- $log -----" >&2
	tail -n 120 -- "$log" >&2 || true
	printf '%s\n' '----- end log -----' >&2
}

assert_actionable_root_failure()
{
	local label=$1
	local log=$2
	local source_copy=$3

	if ! grep -Eiq \
		'ZEDBSD_SOURCE_DIR.*(required|requires|must|unset|not set|missing|invalid|not found|does not exist)|((required|requires|must|unset|not set|missing|invalid|not found|does not exist).*)ZEDBSD_SOURCE_DIR' \
		"$log"; then
		show_failure_log "$log"
		die "$label failure does not name ZEDBSD_SOURCE_DIR and an actionable cause"
	fi
	if grep -Eiq \
		'The (C|CXX|ASM) compiler identification|Detecting .* compiler ABI|Check for working .* compiler|Building (C|CXX|ASM) object' \
		"$log"; then
		show_failure_log "$log"
		die "$label reached compiler discovery instead of failing in preflight"
	fi
	if [[ -n "$(find "$source_copy" -path '*/CMakeFiles/*CompilerId*' \
		-print -quit)" ]]; then
		die "$label created a compiler-identification probe before failing"
	fi
}

run_negative_root_case()
{
	local label=$1
	local root_value=${2-}
	local source_copy=$WORK_DIR/$label-source
	local log=$OUTPUT_DIR/$label-configure.log
	local status

	copy_clean_source "$source_copy"
	set +e
	if [[ "$label" == unset-root ]]; then
		(
			cd -- "$source_copy"
			unset ZEDBSD_SOURCE_DIR
			cmake --preset zedbsd
		) >"$log" 2>&1
		status=$?
	else
		(
			cd -- "$source_copy"
			export ZEDBSD_SOURCE_DIR=$root_value
			cmake --preset zedbsd
		) >"$log" 2>&1
		status=$?
	fi
	set -e
	[[ $status -ne 0 ]] || die "$label unexpectedly configured successfully"
	assert_actionable_root_failure "$label" "$log" "$source_copy"
	note "$label: PASS"
}

audit_elf()
{
	local artifact=$1
	local file_tool=${FILE:-file}
	local readelf_tool=${READELF:-readelf}
	local nm_tool=${NM:-nm}
	local file_log=$OUTPUT_DIR/artifact-file.txt
	local header_log=$OUTPUT_DIR/artifact-elf-header.txt
	local program_log=$OUTPUT_DIR/artifact-program-headers.txt
	local dynamic_log=$OUTPUT_DIR/artifact-dynamic.txt
	local undefined_log=$OUTPUT_DIR/artifact-undefined.txt
	local start_log=$OUTPUT_DIR/artifact-start-symbol.txt
	local entry
	local start

	require_command "$file_tool"
	require_command "$readelf_tool"
	require_command "$nm_tool"

	if ! "$file_tool" "$artifact" >"$file_log" 2>&1; then
		die "file(1) could not inspect the artifact (see $file_log)"
	fi
	grep -Eq 'ELF 64-bit LSB.*x86-64' "$file_log" ||
		die "artifact is not an ELF64 x86-64 executable (see $file_log)"
	grep -Eiq 'statically linked' "$file_log" ||
		die "file(1) does not classify the artifact as static (see $file_log)"

	if ! "$readelf_tool" -hW "$artifact" >"$header_log" 2>&1; then
		die "readelf could not read the ELF header (see $header_log)"
	fi
	grep -Eq 'Class:[[:space:]]+ELF64' "$header_log" ||
		die "ELF class is not ELF64"
	grep -Eq 'Type:[[:space:]]+EXEC([[:space:]]|$)' "$header_log" ||
		die "ELF type is not ET_EXEC"
	grep -Eq 'Machine:[[:space:]]+(Advanced Micro Devices X86-64|AMD x86-64)' \
		"$header_log" || die "ELF machine is not x86-64"

	if ! "$readelf_tool" -lW "$artifact" >"$program_log" 2>&1; then
		die "readelf could not read program headers (see $program_log)"
	fi
	if grep -Eq '(^|[[:space:]])INTERP([[:space:]]|$)|Requesting program interpreter' \
		"$program_log"; then
		die "static artifact contains PT_INTERP"
	fi
	if [[ $(grep -Ec '^[[:blank:]]+GNU_STACK[[:blank:]]' "$program_log") -ne 1 ]] ||
		! grep -Eq '^[[:blank:]]+GNU_STACK[[:blank:]].*[[:blank:]]0x100000[[:blank:]]+RW[[:blank:]]' \
		"$program_log"; then
		die "artifact does not have the zedBSD 1 MiB non-executable PT_GNU_STACK"
	fi
	if ! grep -Eq '^[[:blank:]]+LOAD[[:blank:]]+0x[0-9a-f]+[[:blank:]]+0x0*400000[[:blank:]]' \
		"$program_log"; then
		die "artifact does not start its zedBSD PT_LOAD layout at 0x400000"
	fi
	if ! "$readelf_tool" -dW "$artifact" >"$dynamic_log" 2>&1; then
		die "readelf could not audit the dynamic section (see $dynamic_log)"
	fi
	if grep -Eq '\(NEEDED\)' "$dynamic_log"; then
		die "artifact contains a DT_NEEDED host/runtime dependency"
	fi
	if ! "$nm_tool" -u "$artifact" >"$undefined_log" 2>&1; then
		die "nm could not audit undefined symbols (see $undefined_log)"
	fi
	if grep -Eq '[^[:space:]]' "$undefined_log"; then
		die "artifact has undefined symbols (see $undefined_log)"
	fi
	if ! "$nm_tool" -n "$artifact" >"$start_log" 2>&1; then
		die "nm could not inspect the entry symbol (see $start_log)"
	fi
	entry=$(awk '/Entry point address:/ { v = tolower($4); sub(/^0x0*/, "0x", v); print v }' \
		"$header_log")
	start=$(awk '$2 ~ /^[Tt]$/ && $3 == "_start" { v = tolower($1); sub(/^0*/, "", v); if (v == "") v = "0"; print "0x" v; exit }' \
		"$start_log")
	[[ -n "$entry" && "$entry" == "$start" ]] ||
		die "ELF entry point does not resolve to zedBSD _start"
}

audit_build_contract()
{
	local source_copy=$1
	local build=$source_copy/build-zedbsd
	local report=$OUTPUT_DIR/build-contract.txt
	local flags
	local link=$build/CMakeFiles/noctcli.dir/link.txt
	local required_flags=(
		CMakeFiles/noct.dir/flags.make
		CMakeFiles/noctapi.dir/flags.make
		CMakeFiles/noctcli.dir/flags.make
		CMakeFiles/noctcli_zedbsd_runtime.dir/flags.make
	)

	: >"$report"
	for flags in "${required_flags[@]}"; do
		[[ -f "$build/$flags" ]] || die "missing generated flags file: $flags"
		grep -F -- '-nostdinc' "$build/$flags" >>"$report" ||
			die "target compile flags omit -nostdinc: $flags"
		if grep -Eq '/usr/(include|lib)|/lib/x86_64-linux-gnu' "$build/$flags"; then
			die "host include/library path leaked into $flags"
		fi
	done
	grep -F -- '-U__linux__' \
		"$build/CMakeFiles/noctcli.dir/flags.make" >>"$report" ||
		die "zedBSD target does not suppress the host __linux__ macro"
	[[ -f "$link" ]] || die "missing generated Noct link command"
	for required in '-nostdlib' '-static' 'crt0-amd64.S.o' \
		"-Wl,-T,$ZEDBSD_ROOT/platform/amd64/user.ld"; do
		grep -F -- "$required" "$link" >>"$report" ||
			die "Noct link command omits required zedBSD input: $required"
	done
	if grep -Eq '(^|[[:space:]])(-lc|-ldl|-lutil|-lm|-lpthread)([[:space:]]|$)|/usr/(include|lib)|/lib/x86_64-linux-gnu|(^|/)crt(1|i|n)\.o' \
		"$link"; then
		die "host runtime input leaked into the Noct link command"
	fi
	if grep -R -E --include='*.d' \
		'/usr/(include|lib)|/lib/x86_64-linux-gnu' \
		"$build/CMakeFiles" >>"$report"; then
		die "host headers or libraries leaked into generated dependencies"
	fi
	printf 'zedbsd_build_contract=pass\n' >>"$report"
}

run_build_checks()
{
	local source_copy
	local invalid_root
	local artifact
	local canonical_sha
	local install_source_sha
	local package_sha

	for command_name in cmake git tar find grep tail sha256sum awk mktemp; do
		require_command "$command_name"
	done

	WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ws008-p001.XXXXXX")
	invalid_root=$WORK_DIR/not-a-zedbsd-source
	mkdir -p -- "$invalid_root"

	run_negative_root_case unset-root
	run_negative_root_case invalid-root "$invalid_root"

	source_copy=$WORK_DIR/valid-source
	copy_clean_source "$source_copy"
	note 'running: cmake --preset zedbsd'
	if ! (
		cd -- "$source_copy"
		export ZEDBSD_SOURCE_DIR=$ZEDBSD_ROOT
		cmake --preset zedbsd
	) >"$OUTPUT_DIR/valid-configure.log" 2>&1; then
		show_failure_log "$OUTPUT_DIR/valid-configure.log"
		die "valid zedBSD configure failed"
	fi
	note 'running: cmake --build --preset zedbsd --parallel 16'
	if ! (
		cd -- "$source_copy"
		export ZEDBSD_SOURCE_DIR=$ZEDBSD_ROOT
		cmake --build --preset zedbsd --parallel 16
	) >"$OUTPUT_DIR/valid-build.log" 2>&1; then
		show_failure_log "$OUTPUT_DIR/valid-build.log"
		die "valid zedBSD build failed"
	fi

	artifact=$source_copy/$ARTIFACT_REL
	[[ -f "$artifact" ]] || die "canonical artifact not found: $ARTIFACT_REL"
	[[ -x "$artifact" ]] || die "canonical artifact is not executable: $ARTIFACT_REL"
	audit_elf "$artifact"
	audit_build_contract "$source_copy"
	canonical_sha=$(sha256sum "$artifact" | awk '{print $1}')
	printf '%s  %s\n' "$canonical_sha" "$ARTIFACT_REL" \
		>"$OUTPUT_DIR/canonical-artifact.sha256"
	note "ELF audit: PASS ($canonical_sha)"

	if [[ -f "$PACKAGE_ARTIFACT" ]]; then
		[[ -f "$INSTALL_SOURCE_ARTIFACT" ]] ||
			die "canonical install-source artifact is absent: $INSTALL_SOURCE_ARTIFACT"
		audit_elf "$INSTALL_SOURCE_ARTIFACT"
		install_source_sha=$(sha256sum "$INSTALL_SOURCE_ARTIFACT" | awk '{print $1}')
		printf '%s  %s\n' "$install_source_sha" "$INSTALL_SOURCE_ARTIFACT" \
			>"$OUTPUT_DIR/install-source-artifact.sha256"
		note "install-source ELF audit: PASS ($install_source_sha)"
		package_sha=$(sha256sum "$PACKAGE_ARTIFACT" | awk '{print $1}')
		printf '%s  %s\n' "$package_sha" "$PACKAGE_ARTIFACT" \
			>"$OUTPUT_DIR/package-artifact.sha256"
		[[ "$install_source_sha" == "$package_sha" ]] ||
			die "install-source and package artifact SHA-256 values differ"
		note 'package artifact identity: PASS'
	elif [[ $REQUIRE_PACKAGE -eq 1 ]]; then
		die "required package artifact is absent: $PACKAGE_ARTIFACT"
	else
		note "package artifact identity: SKIP (absent; rerun with --require-package-artifact after make -j16)"
	fi
}

collect_changed_paths()
{
	local tree=$1
	local path
	local -n destination=$2

	while IFS= read -r -d '' path; do
		destination["$path"]=1
	done < <(git -C "$tree" diff --no-renames --name-only -z HEAD --)
	while IFS= read -r -d '' path; do
		destination["$path"]=1
	done < <(git -C "$tree" ls-files --others --exclude-standard -z --)
}

path_digest()
{
	local tree=$1
	local path=$2
	local target

	if [[ -L "$tree/$path" ]]; then
		target=$(readlink -- "$tree/$path")
		printf 'symlink:%s' "$target" | sha256sum | awk '{print $1}'
	elif [[ -f "$tree/$path" ]]; then
		sha256sum "$tree/$path" | awk '{print $1}'
	elif [[ ! -e "$tree/$path" ]]; then
		printf '%s\n' MISSING
	else
		printf '%s\n' NONREGULAR
	fi
}

run_parity_check()
{
	local official_head
	local integration_head
	local manifest=$OUTPUT_DIR/noct-path-parity.tsv
	local path
	local official_sha
	local integration_sha
	local mismatch=0
	local -a sorted_paths=()
	declare -A changed_paths=()

	for command_name in git sha256sum sort readlink awk; do
		require_command "$command_name"
	done
	official_head=$(git -C "$OFFICIAL_DIR" rev-parse HEAD)
	integration_head=$(git -C "$INTEGRATION_DIR" rev-parse HEAD)
	collect_changed_paths "$OFFICIAL_DIR" changed_paths
	collect_changed_paths "$INTEGRATION_DIR" changed_paths

	{
		printf '# official_head\t%s\n' "$official_head"
		printf '# integration_head\t%s\n' "$integration_head"
		printf 'official_sha256\tintegration_sha256\tpath\n'
	} >"$manifest"

	if [[ ${#changed_paths[@]} -gt 0 ]]; then
		mapfile -d '' sorted_paths < <(
			printf '%s\0' "${!changed_paths[@]}" | sort -z
		)
	fi
	for path in "${sorted_paths[@]}"; do
		case "$path" in
		*$'\n'* | *$'\t'*) die "changed path cannot be represented in TSV: $path" ;;
		esac
		official_sha=$(path_digest "$OFFICIAL_DIR" "$path")
		integration_sha=$(path_digest "$INTEGRATION_DIR" "$path")
		printf '%s\t%s\t%s\n' "$official_sha" "$integration_sha" "$path" \
			>>"$manifest"
		if [[ "$official_sha" != "$integration_sha" ]]; then
			mismatch=1
		fi
	done

	[[ "$official_head" == "$integration_head" ]] ||
		die "official/integration HEAD revisions differ (see $manifest)"
	[[ $mismatch -eq 0 ]] || die "official/integration path parity differs (see $manifest)"
	if [[ ${#sorted_paths[@]} -eq 0 && $REQUIRE_PARITY_CHANGES -eq 1 ]]; then
		die "parity manifest has no changed paths"
	fi
	note "path parity: PASS (${#sorted_paths[@]} changed paths; $manifest)"
}

case "$MODE" in
all)
	run_build_checks
	run_parity_check
	;;
build)
	run_build_checks
	;;
parity)
	run_parity_check
	;;
esac

note "PASS (evidence: $OUTPUT_DIR)"
