#!/usr/bin/env bash
# WS011 q075: ten user-authorized, slightly varied normal-build T021 cells.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
start=${NCOM_MATRIX_START:-1}
[[ $start =~ ^([1-9]|10)$ ]] || {
	echo 'NCOM_MATRIX_START must be between 1 and 10' >&2; exit 2;
}
if [[ $# -gt 1 ]]; then
	echo "usage: $0 [NEW-OUTPUT-DIRECTORY]" >&2
	exit 2
fi
if [[ $# == 1 ]]; then
	if [[ $start == 1 ]]; then
		[[ ! -e $1 ]] || { echo 'output path already exists' >&2; exit 2; }
		mkdir -p -- "$1"
	else
		[[ -d $1 && -f $1/matrix.tsv ]] || {
			echo 'resume requires the existing matrix directory' >&2; exit 2;
		}
	fi
	output=$(cd -- "$1" && pwd)
else
	[[ $start == 1 ]] || { echo 'resume requires an output directory' >&2; exit 2; }
	output=$(mktemp -d "$repo/plan/ws011-net-config/temp/q075-matrix.XXXXXX")
fi
variants=(baseline baseline baseline confirm-delay-1 confirm-delay-5
	show-candidate show-startup-twice edit-roundtrip upper-startup repeat-commit)
delays=(0.015 0.030 0.005 0.015 0.015 0.015 0.015 0.015 0.015 0.015)
if [[ $start == 1 ]]; then
	printf 'cell\tvariant\tkey_delay_seconds\tresult\n' >"$output/matrix.tsv"
else
	# Resume never repeats a consumed QEMU cell. A host-validator repair may
	# revalidate retained observations, but its original failure stays recorded.
	for ((prior = 1; prior < start; prior++)); do
		printf -v cell '%02d' "$prior"
		awk -F '\t' '$1 == "input-integrity" && $2 == "pass" { ok = 1 }
		    END { exit !ok }' "$output/$cell/results.tsv" || {
			echo "cell $cell has not passed input-integrity validation" >&2
			exit 1
		}
		if ! awk -F '\t' '$1 == "NCOM-T021" && $2 == "pass" { ok = 1 }
		    END { exit !ok }' "$output/$cell/results.tsv"; then
			[[ -f $output/$cell/revalidation.tsv ]] &&
				awk -F '\t' '$1 == "NCOM-T021" && $2 == "pass" { ok = 1 }
				END { exit !ok }' "$output/$cell/revalidation.tsv" || {
				echo "cell $cell has not passed retained-evidence validation" >&2
				exit 1
			}
			printf '%s\t%s\t%s\tpass-revalidated\n' "$cell" \
				"${variants[prior - 1]}" "${delays[prior - 1]}" \
				>>"$output/matrix.tsv"
		fi
	done
fi
echo "Q075 normal-build matrix: $output"
for index in "${!variants[@]}"; do
	((index + 1 >= start)) || continue
	printf -v cell '%02d' "$((index + 1))"
	echo "Starting cell $cell/10: ${variants[index]} (key spacing ${delays[index]} s)"
	set +e
	NCOM_DIAGNOSTIC=0 NCOM_CAPTURE_FAILURE=1 NCOM_CELL_SELECTION=t021 \
		NCOM_DIAGNOSTIC_HOLD_SECONDS=120 NCOM_VARIANT=${variants[index]} \
		KEY_DELAY_SECONDS=${delays[index]} \
		bash "$script_dir/run-confirmed-commit-qemu.sh" "$output/$cell" \
		2>&1 | tee "$output/$cell-run.log"
	statuses=("${PIPESTATUS[@]}")
	set -e
	if [[ ${statuses[0]} != 0 || ${statuses[1]} != 0 ]]; then
		printf '%s\t%s\t%s\tfail\n' "$cell" "${variants[index]}" \
			"${delays[index]}" >>"$output/matrix.tsv"
		echo "Matrix stopped at cell $cell; inspect retained failure evidence." >&2
		exit 1
	fi
	printf '%s\t%s\t%s\tpass\n' "$cell" "${variants[index]}" \
		"${delays[index]}" >>"$output/matrix.tsv"
	echo "Cell $cell/10: PASS"
done
echo "Q075 normal-build matrix: 10/10 PASS (non-reproduction, not a causal repair)"
