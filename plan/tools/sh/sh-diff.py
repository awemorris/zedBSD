#!/usr/bin/env python3
"""ws042: runs shell cases with a reference shell (dash) and the shell under
test, and compares standard output and exit status.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

Cases come from the oils spec tests (files whose compare_shells names dash;
a case that marks dash N-I or BUG is not scored) and from the tree's own
cases (plan/tools/sh/cases/*.sh, split at lines "#### name").

    sh-diff.py --shell build/ws042/host-sh [--ref /usr/bin/dash]
               [--oils build/ws042/oils] [--only FILE-SUBSTRING]
               [--report OUT.txt] [--jobs N]

Prints one line per file (passed/total) and the total.  --report writes
every failing case with both outputs.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
TIMEOUT = 5

# Cases whose result differs between two runs of dash itself (they depend on
# the user running them or on the host's defaults); they are not scored.
UNSTABLE = {
	("builtin-bracket.test.sh", "-x"),
	("builtin-bracket.test.sh", "-r"),
	("builtin-bracket.test.sh", "-w"),
	("errexit-osh.test.sh", "strict_errexit allows singleton pipeline"),
	("vars-special.test.sh", "$PATH is set if unset at startup"),
	("builtin-process.test.sh", "YSH readability: ulimit --all the same as ulimit -a"),
	("posix.test.sh", "Newlines in compound lists"),
}


# Cases about a POSIX feature dash does not have: dash never sets LINENO, so
# its output there is empty where POSIX (XCU 2.5.3) requires the line number.
# They are not scored against dash.
def dash_lacks(file_name: str, case_name: str) -> bool:
	return file_name == "vars-special.test.sh" and "$LINENO" in case_name


# Cases that use an extension WS065 added where POSIX leaves the result
# unspecified or calls it a syntax error ($'...', [[ ]], (( )), for ((;;)),
# >& file and the like): dash reads them otherwise (it runs (( as nested
# subshells, for one), so they are scored against bash --posix instead.
BASH_REFERENCE = {
	('var-op-len.test.sh', '${#s} respects LC_ALL - length in bytes or code points'),
	('arith.test.sh', 'Logical Ops Short Circuit'),
	('arith.test.sh', 'More 64-bit ops'),
	('bool-parse.test.sh', 'Allowed: [[ = ]] and [[ == ]]'),
	('bool-parse.test.sh', '[[ -f -f ]] and [[ -f == ]]'),
	('bugs.test.sh', 'for loop (issue #1446)'),
	('bugs.test.sh', 'for loop 2 (issue #1446)'),
	('bugs.test.sh', '(( status bug'),
	('builtin-bracket.test.sh', 'More negative numbers'),
	('builtin-printf.test.sh', 'printf %c unicode - prints the first BYTE of a string - it does not respect UTF-8'),
	('nul-bytes.test.sh', 'printf - literal NUL in format string'),
	('nul-bytes.test.sh', 'NUL bytes with test -n'),
	('nul-bytes.test.sh', 'NUL bytes with test -f'),
	('nul-bytes.test.sh', 'NUL bytes with ${#s} (OSH and zsh agree)'),
	('paren-ambiguity.test.sh', '(( closed with )) after multiple lines is parse error - #2337'),
	('quote.test.sh', "$'' with newlines"),
	('quote.test.sh', "$'' octal escapes don't have leading 0"),
	('quote.test.sh', "$'' octal escapes with fewer than 3 chars"),
	('quote.test.sh', "$'' supports \\cA escape for Ctrl-A - mask with 0x1f"),
	('redirect-command.test.sh', 'redirect bash extensions:   [[  ((  for (('),
	('redirect.test.sh', 'Descriptor redirect with filename'),
	('var-sub-quote.test.sh', "$'' allowed within VarSub arguments"),
	('var-sub.test.sh', 'Descriptor redirect to bad "$@"'),
	('xtrace.test.sh', 'xtrace with newlines'),
}
BASH = ["/usr/bin/bash", "--posix"]


def reference_for(file: str, name: str, ref: str) -> list[str]:
	"""The reference shell of one case, as a command."""
	if (file.rpartition("/")[2], name) in BASH_REFERENCE:
		return BASH
	if file.endswith("/bash-extensions.sh"):
		return BASH
	return [ref]


def parse_file(path: Path, require_dash: bool) -> list[tuple[str, str]]:
	"""Returns (name, code) of every scorable case of one spec file."""
	text = path.read_text(errors="replace")
	if require_dash:
		header = re.search(r"^## compare_shells:(.*)$", text, re.M)
		if header is None or "dash" not in header.group(1).split():
			return []
	cases = []
	for block in re.split(r"^#### ", text, flags=re.M)[1:]:
		name, _, body = block.partition("\n")
		lines = body.split("\n")
		code = []
		skip = False
		expected = False
		for line in lines:
			# A multi-line expectation ("## STDOUT:" ... "## END") is not code.
			if expected:
				if line.startswith("## END"):
					expected = False
				continue
			if re.match(r"^## (?:(?:OK|BUG|N-I)(?:-\d)? \S+ )?(?:STDOUT|STDERR):\s*$", line):
				expected = True
				continue
			if line.startswith("## "):
				words = line.split()
				if len(words) >= 3 and words[1] in ("N-I", "BUG") and "dash" in words[2].split("/"):
					skip = True
				continue
			code.append(line)
		if not skip and (path.name, name.strip()) not in UNSTABLE \
				and not dash_lacks(path.name, name.strip()):
			cases.append((name.strip(), "\n".join(code)))
	return cases


# The oils checkout; a few cases read files of its own under $REPO_ROOT.
REPO_ROOT = ""


def run(shell: str | list[str], code: str, work: Path) -> tuple[bytes, int]:
	"""Runs one case with one shell (a path, or a command) in a fresh directory."""
	command = shell if isinstance(shell, list) else [shell]
	script = work / "case.sh"
	script.write_text(code)
	env = {
		"PATH": "/usr/bin:/bin",
		"HOME": str(work),
		"TMP": str(work),
		"SH": command[0],
		"LC_ALL": "C",
		"REPO_ROOT": REPO_ROOT,
	}
	try:
		result = subprocess.run(command + [str(script)], cwd=work, env=env,
		    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
		    stderr=subprocess.DEVNULL, timeout=TIMEOUT)
		return result.stdout, result.returncode
	except subprocess.TimeoutExpired as exception:
		return (exception.stdout or b"") + b"<TIMEOUT>", -999


def compare(args: tuple[str, str, str, str, str]) -> tuple[str, str, bool, str]:
	file, name, code, ref, shell = args
	# Both shells run in the same directory, emptied in between, so that a
	# case printing its directory sees the same path.
	with tempfile.TemporaryDirectory(prefix="ws042-") as a:
		want = run(reference_for(file, name, ref), code, Path(a))
		subprocess.run(["find", a, "-mindepth", "1", "-delete"], check=False)
		subprocess.run(["chmod", "-R", "u+rwx", a], check=False)
		got = run(shell, code, Path(a))
	ok = want == got
	detail = ""
	if not ok:
		detail = ("--- %s :: %s\n%s\n--- ref status %d\n%s\n--- ours status %d\n%s\n"
		    % (file, name, code.strip(), want[1],
		       want[0].decode(errors="replace"), got[1],
		       got[0].decode(errors="replace")))
	return file, name, ok, detail


def export_one(args: tuple[int, tuple, Path]) -> None:
	index, (file, name, code, ref, _shell), out = args
	with tempfile.TemporaryDirectory(prefix="ws042-") as a:
		want = run(reference_for(file, name, ref), code, Path(a))
	# Two small files per case keep the bundle within a guest's tmpfs: the
	# code, and the expectation (status, name, then the output).
	# A guest's tmpfs holds a limited number of entries per directory, so
	# the cases go a hundred to a directory.
	group = out / ("%02d" % (index // 100))
	group.mkdir(exist_ok=True)
	(group / ("%04d.sh" % index)).write_text(code)
	(group / ("%04d.exp" % index)).write_bytes(
	    ("%d\n%s :: %s\n" % (want[1], file, name)).encode() + want[0])


def export_cases(work: list, out: Path) -> int:
	"""Writes every case with the reference output, for a guest to run."""
	out.mkdir(parents=True, exist_ok=True)
	with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
		list(pool.map(export_one, [(i, w, out) for i, w in enumerate(work)]))
	print("exported %d cases to %s" % (len(work), out))
	return 0


def main() -> int:
	parser = argparse.ArgumentParser()
	parser.add_argument("--shell", required=True)
	parser.add_argument("--ref", default="/usr/bin/dash")
	parser.add_argument("--oils", default="build/ws042/oils")
	parser.add_argument("--only", default="")
	parser.add_argument("--report", default="")
	parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
	parser.add_argument("--export", default="",
	    help="write each case and the reference shell's output to this "
	    "directory, for running on a guest (guest-diff.sh)")
	options = parser.parse_args()
	global REPO_ROOT
	REPO_ROOT = os.path.abspath(options.oils)

	work = []
	sources = []
	oils = Path(options.oils) / "spec"
	if oils.is_dir():
		sources += [(path, True) for path in sorted(oils.glob("*.test.sh"))]
	sources += [(path, False) for path in sorted((HERE / "cases").glob("*.sh"))]
	for path, require_dash in sources:
		label = ("oils/" if require_dash else "own/") + path.name
		if options.only and options.only not in label:
			continue
		for name, code in parse_file(path, require_dash):
			work.append((label, name, code, options.ref,
			    os.path.abspath(options.shell)))

	if options.export:
		return export_cases(work, Path(options.export))

	per_file: dict[str, list[int]] = {}
	failures = []
	with concurrent.futures.ThreadPoolExecutor(options.jobs) as pool:
		for file, name, ok, detail in pool.map(compare, work):
			entry = per_file.setdefault(file, [0, 0])
			entry[1] += 1
			if ok:
				entry[0] += 1
			else:
				failures.append(detail)
	passed = total = 0
	for file in sorted(per_file):
		good, count = per_file[file]
		passed += good
		total += count
		print("%-40s %4d/%4d" % (file, good, count))
	print("TOTAL %d/%d" % (passed, total))
	if options.report:
		Path(options.report).write_text("".join(failures))
	return 0 if passed == total else 1


if __name__ == "__main__":
	raise SystemExit(main())
