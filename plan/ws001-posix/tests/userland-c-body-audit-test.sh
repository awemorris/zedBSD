#!/bin/sh
# Exercise the ANSI C function-body audit with positive and negative fixtures.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
audit=$repo/plan/ws001-posix/tests/userland-c-body-audit.py
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-userland-body.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

mkdir -p "$temporary/userland"
cat >"$temporary/userland/good.c" <<'EOF'
int
main(
	void)
{
	int index;

	/* Initializes the fixture counter. */
	index = 0;

	/* Processes the fixture counter. */
	for (index = 0; index < 1; index++)
		index = index;

	/* Returns the fixture result. */
	return index;
}
EOF
(
	cd "$temporary"
	python3 "$audit" --expected-c 1 --expected-h 0 --summary
)

cat >"$temporary/userland/bad.c" <<'EOF'
int
main(
	void)
{
	int value;
	value = 0;
	if (value == 0) {
		int nested;
		nested = value;
	}
	for (int index = 0; index < 1; index++)
		value++;
	return value;
}
EOF
if (
	cd "$temporary"
	python3 "$audit" --expected-c 1 --expected-h 0 --summary \
		>/dev/null 2>&1
); then
	echo "USERLAND-C-BODY-STYLE-T002 invalid ANSI C body was accepted" >&2
	exit 1
fi

cat >"$temporary/userland/bad.c" <<'EOF'
int
main(
	void)
{
	int value;

	value = 0;
	if (value == 0)
		value++;
	return value;
}
EOF
if (
	cd "$temporary"
	python3 "$audit" --expected-c 1 --expected-h 0 --summary \
		>/dev/null 2>&1
); then
	echo "USERLAND-C-BODY-STYLE-T003 invalid semantic layout was accepted" >&2
	exit 1
fi

cat >"$temporary/userland/bad.c" <<'EOF'
int
main(
	void)
{
	int index, value;

	/* Initializes the fixture state. */
	index = 0;
	value = 1;

	/* Selects a fixture value. */
	if (value) {

			value++;
	} else
		value--;

	/* Processes matching fixture values. */
	for (index = 0; index < 1; index++)
		if (value) {
			value++;
		}

	/* Advances the fixture value. */
	while (value < 3)
		value = value +
			1;

	/* Returns the fixture result. */
	return value;
}
EOF
if (
	cd "$temporary"
	python3 "$audit" --expected-c 1 --expected-h 0 --summary \
		>"$temporary/control-block.out" 2>&1
); then
	echo "USERLAND-C-BODY-STYLE-T004 invalid control blocks were accepted" >&2
	exit 1
fi
if ! grep -q "control-block transformer would change" \
	"$temporary/control-block.out"; then
	echo "USERLAND-C-BODY-STYLE-T004 control-block diagnostic missing" >&2
	exit 1
fi
python3 - "$repo/plan/ws001-posix/tests/refactor-userland-control-blocks.py" \
	"$temporary/userland/bad.c" <<'PY'
import importlib.util
import pathlib
import sys

specification = importlib.util.spec_from_file_location(
    "control_block_fixture", sys.argv[1])
module = importlib.util.module_from_spec(specification)
sys.modules[specification.name] = module
specification.loader.exec_module(module)
source = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
migrated, counts = module.refactor(source)
if counts[0] < 1 or counts[1] < 2 or counts[2] < 1 or counts[3] < 1:
    raise SystemExit(
        "USERLAND-C-BODY-STYLE-T004 incomplete rule coverage: " +
        repr(counts))
second, second_counts = module.refactor(migrated)
if second != migrated or any(second_counts):
    raise SystemExit("USERLAND-C-BODY-STYLE-T004 transformer is not idempotent")
PY

echo "USERLAND-C-BODY-STYLE-T002 audit fixtures: PASS"
