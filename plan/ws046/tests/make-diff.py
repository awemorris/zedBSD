#!/usr/bin/env python3
"""ws046: runs make cases with a reference make (GNU make) and the make
under test, and compares standard output and exit status.

The cases are plan/ws046/tests/cases/*.sh, split at lines "#### name".
Each case is a shell script run by dash in an empty directory: it writes
its Makefile (usually with a here-document) and any files it needs, then
runs "$MAKE ...".  $MAKE is the make under test, given as an absolute
path.  Standard error is not compared, because the two makes word their
messages differently; a case that wants to see an error writes the status.
Lines of standard output that are make's own messages ("make: ...",
"make[1]: Entering directory ...") are dropped from both.

  python3 plan/ws046/tests/make-diff.py [--make PATH] [--ref PATH]
      [--only NAME] [--report FILE] [--show]
  python3 plan/ws046/tests/make-diff.py --export DIR     (for a guest)
  python3 plan/ws046/tests/make-diff.py --compare DIR    (the guest's results)

--export writes each case as DIR/NNNN.sh and the reference make's status,
the case's name and its output as DIR/NNNN.exp; make-guest.sh runs them on
a guest and writes NNNN.out and NNNN.status beside them, and --compare
reads those back.

Without --make, only the reference make runs, and each case must end with
status 0 unless it has a line "## status N" giving the status it expects.
"""

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def read_cases(only: str) -> list:
	"""Returns (file, name, code, expected status or None) for each case."""
	cases = []
	for path in sorted((HERE / "cases").glob("*.sh")):
		if only and only not in path.name:
			continue
		name = None
		code = []
		for line in path.read_text().split("\n") + ["#### "]:
			if line.startswith("#### "):
				if name is not None:
					cases.append((path.name, name, "\n".join(code)))
				name = line[5:].strip()
				code = []
			elif name is not None:
				code.append(line)
	result = []
	for file_name, name, code in cases:
		expected = None
		for line in code.split("\n"):
			if line.startswith("## status "):
				expected = int(line.split()[2])
		result.append((file_name, name, code, expected))
	return result


def run_case(code: str, make: str) -> tuple:
	"""Runs a case with a make; returns (status, output)."""
	with tempfile.TemporaryDirectory(prefix="ws046-") as work:
		script = Path(work) / "case.sh"
		script.write_text(code + "\n")
		environment = {
			"PATH": os.environ.get("PATH", "/usr/bin:/bin"),
			"HOME": work,
			"LC_ALL": "C",
			"MAKE": make,
			"MAKEFLAGS": "",
		}
		try:
			done = subprocess.run(["dash", str(script)], cwd=work, env=environment,
					      stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=30)
		except subprocess.TimeoutExpired:
			return (-1, b"TIMEOUT")
		# $(MAKE) is each make's own path; the word MAKE stands for both.
		output = done.stdout.replace(make.encode(), b"MAKE")
		return (done.returncode, informational_removed(output))


def export_cases(cases: list, ref: str, directory: Path) -> int:
	"""Writes each case and the reference make's result for a guest."""
	directory.mkdir(parents=True, exist_ok=True)
	for index, (file_name, name, code, expected) in enumerate(cases):
		status, output = run_case(code, ref)
		(directory / ("%04d.sh" % index)).write_text(code + "\n")
		(directory / ("%04d.exp" % index)).write_bytes(
			b"%d\n%s :: %s\n" % (status, file_name.encode(), name.encode()) + output)
	print("exported %d cases to %s" % (len(cases), directory))
	return 0


def compare_guest(directory: Path) -> int:
	"""Compares the guest's results (NNNN.out, NNNN.status) with the exported ones."""
	totals = {}
	for expected in sorted(directory.glob("*.exp")):
		lines = expected.read_bytes().split(b"\n", 2)
		want_status = int(lines[0])
		label = lines[1].decode()
		want = lines[2] if len(lines) > 2 else b""
		file_name = label.split(" :: ")[0]
		out = expected.with_suffix(".out")
		status_file = expected.with_suffix(".status")
		got = b""
		got_status = -1
		if out.exists():
			got = informational_removed(out.read_bytes().replace(b"/usr/bin/make", b"MAKE"))
		if status_file.exists():
			got_status = int(status_file.read_text().strip() or "-1")
		total = totals.setdefault(file_name, [0, 0])
		total[1] += 1
		if got == want and got_status == want_status:
			total[0] += 1
			continue
		print("--- %s" % label)
		print("--- ref status %d" % want_status)
		print(want.decode("utf-8", "replace"))
		print("--- guest status %d" % got_status)
		print(got.decode("utf-8", "replace"))
	passed = 0
	count = 0
	for file_name in sorted(totals):
		print("%-26s %4d/%4d" % (file_name, totals[file_name][0], totals[file_name][1]))
		passed += totals[file_name][0]
		count += totals[file_name][1]
	print("TOTAL %d/%d" % (passed, count))
	if passed != count:
		return 1
	return 0


def informational_removed(output: bytes) -> bytes:
	"""
	Drops make's own informational lines ("make: Nothing to be done",
	"make[1]: Entering directory"), which carry the program's name and are
	not what a Makefile asks for.  Echoed commands and recipe output stay.
	"""
	kept = []
	for line in output.split(b"\n"):
		if re.match(rb"^[^ :]*make(\[[0-9]+\])?: ", line):
			continue
		kept.append(line)
	return b"\n".join(kept)


def main() -> int:
	"""Runs every case and prints the totals per file."""
	parser = argparse.ArgumentParser()
	parser.add_argument("--make", default="")
	parser.add_argument("--ref", default="/usr/bin/make")
	parser.add_argument("--only", default="")
	parser.add_argument("--report", default="")
	parser.add_argument("--show", action="store_true", help="print the reference output of each case")
	parser.add_argument("--export", default="")
	parser.add_argument("--compare", default="")
	arguments = parser.parse_args()
	cases = read_cases(arguments.only)
	if arguments.export:
		return export_cases(cases, arguments.ref, Path(arguments.export))
	if arguments.compare:
		return compare_guest(Path(arguments.compare))
	test = ""
	if arguments.make:
		test = str(Path(arguments.make).resolve())

	def one(case):
		file_name, name, code, expected = case
		ref = run_case(code, arguments.ref)
		if not test:
			want = expected if expected is not None else 0
			return (file_name, name, ref[0] == want, ref, None)
		ours = run_case(code, test)
		return (file_name, name, ref == ours, ref, ours)

	with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
		results = list(pool.map(one, cases))

	# The reference output of every case, to read what a case checks.
	if arguments.show:
		for file_name, name, passed, ref, ours in results:
			print("=== %s :: %s (status %d)" % (file_name, name, ref[0]))
			print(ref[1].decode("utf-8", "replace").rstrip("\n"))

	totals = {}
	report = []
	for file_name, name, passed, ref, ours in results:
		total = totals.setdefault(file_name, [0, 0])
		total[1] += 1
		if passed:
			total[0] += 1
			continue
		report.append("--- %s :: %s" % (file_name, name))
		report.append("--- ref status %d" % ref[0])
		report.append(ref[1].decode("utf-8", "replace"))
		if ours is not None:
			report.append("--- ours status %d" % ours[0])
			report.append(ours[1].decode("utf-8", "replace"))
	if arguments.report:
		Path(arguments.report).write_text("\n".join(report) + "\n")
	elif report:
		print("\n".join(report))
	passed = 0
	count = 0
	for file_name in sorted(totals):
		print("%-26s %4d/%4d" % (file_name, totals[file_name][0], totals[file_name][1]))
		passed += totals[file_name][0]
		count += totals[file_name][1]
	print("TOTAL %d/%d" % (passed, count))
	if passed != count:
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main())
