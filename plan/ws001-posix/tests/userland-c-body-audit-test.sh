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

echo "USERLAND-C-BODY-STYLE-T002 audit fixtures: PASS"
