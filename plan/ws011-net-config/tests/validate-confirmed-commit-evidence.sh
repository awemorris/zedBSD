#!/usr/bin/env bash
# Revalidate retained normal-build T021 observations; never launches QEMU.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
runner=$script_dir/run-confirmed-commit-qemu.sh
[[ $# == 1 && -d $1 ]] || {
	echo "usage: $0 EXISTING-CELL-DIRECTORY" >&2; exit 2;
}
cell=$(cd -- "$1" && pwd)
metadata=$cell/run-metadata.txt
confirm_guest=$cell/ncom-t021-guest.log
confirm_qemu=$cell/ncom-t021-qemu.log
results=$cell/revalidation.tsv
[[ ! -e $results ]] || {
	echo "refusing to overwrite existing revalidation: $results" >&2; exit 2;
}
for required in "$metadata" "$confirm_guest" "$confirm_qemu" "$cell/results.tsv"; do
	[[ -s $required ]] || { echo "missing retained evidence: $required" >&2; exit 1; }
done
for predicate in '^diagnostic=0$' '^test_cppflags=$' \
	'^ncom-t021_controller_status=0$' '^ncom-t021_qemu_status=0$' \
	'^input_integrity_result=pass$' '^source_image_integrity_result=pass$'; do
	rg -q "$predicate" "$metadata" || {
		echo "retained metadata does not establish $predicate" >&2; exit 1;
	}
done

# Import only the named pure observation helpers, never runner initialization,
# build, controller, cleanup, or QEMU execution. Both paths use one validator.
source <(sed -n '/^marker_count()/,/^}/p;
    /^extract_section()/,/^}/p;
    /^require_state()/,/^}/p;
    /^validate_no_fatal()/,/^}/p;
    /^validate_confirmed_observations()/,/^}/p' "$runner")
output=$(mktemp -d "$cell/revalidation.XXXXXX")
confirm_logical=$output/ncom-t021-guest-logical.log
old_address=10.0.2.15
confirm_address=10.0.2.17
diagnostic=0
sha256sum "$confirm_guest" "$confirm_qemu" "$metadata" "$cell/results.tsv" \
	"$runner" "$script_dir/validate-confirmed-commit-evidence.sh" \
	>"$output/provenance.sha256"
printf 'case\tresult\tevidence\n' >"$results"
validate_confirmed_observations
printf 'provenance\tpass\t%s/provenance.sha256\n' "${output##*/}" >>"$results"
echo "Retained NCOM-T021 observations: PASS ($results); no QEMU rerun"
