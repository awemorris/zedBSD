#!/bin/sh
# Exercise the whole-userland structural C-style audit with fixtures.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
audit=$repo/plan/ws001-posix/tests/userland-c-style-audit.py
control=$repo/plan/ws001-posix/tests/refactor-userland-control-comments.py
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-userland-style.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

mkdir -p "$temporary/userland"
cat >"$temporary/userland/good.c" <<'EOF'
/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */
static int helper(void);
/*
 * Runs the fixture.
 */
int
main(
	void)
{
	return helper();
}
/* Supports the fixture helper. */
static int
helper(
	void)
{
	return 0;
}
EOF
(
	cd "$temporary"
	python3 "$audit" --expected-c 1 --expected-h 0 --summary
)

cat >"$temporary/userland/bad.c" <<'EOF'
/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */
/* Supports the misplaced helper. */
static int helper(void) { return 0; }
/* Runs the misplaced public function. */
int main(void) { return helper(); }
EOF
if (
	cd "$temporary"
	python3 "$audit" --expected-c 1 --expected-h 0 --summary \
		>/dev/null 2>&1
); then
	echo "USERLAND-C-STYLE-T002 invalid structure was accepted" >&2
	exit 1
fi

echo "USERLAND-C-STYLE-T002 structural fixtures: PASS"

python3 - "$control" <<'PY'
import importlib.util
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
specification = importlib.util.spec_from_file_location("control_fixture", path)
control = importlib.util.module_from_spec(specification)
specification.loader.exec_module(control)
source = ('const char *accept = "Accept: */*\\r\\n";\n'
          '/* Explains the real comment.\n'
          ' * Continues its explanation.\n'
          ' */\n')
migrated, additions, labels, comments = control.refactor(source)
assert '"Accept: */*\\r\\n"' in migrated
assert '/*\n * Explains the real comment.\n' in migrated
assert (additions, labels, comments) == (0, 0, 1)
PY

echo "USERLAND-C-STYLE-T003 lexical comment fixture: PASS"
