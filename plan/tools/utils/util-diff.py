#!/usr/bin/env python3
"""ws043: runs the utility cases with the host's utilities (GNU, with
POSIXLY_CORRECT) and with zedBSD's (userland/base built for the host), and
compares standard output and exit status.

The cases are plan/tools/utils/cases/<utility>.sh, split at lines
"#### name".  Each runs under dash in an empty directory; the utility under
test is found first on PATH.  A line "## skip-status" in a case compares
only the output (for utilities whose error status POSIX leaves as ">0").

  python3 plan/tools/utils/util-diff.py [--bin build/ws043/bin] [--only sed]
      [--report FILE] [--export DIR]
"""

import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TIMEOUT = 10


def parse_file(path: Path) -> list[tuple[str, str, bool]]:
	"""Splits a case file into (name, code, compare_status)."""
	cases = []
	name = None
	code: list[str] = []
	status = True
	for line in path.read_text().split("\n"):
		if line.startswith("#### "):
			if name is not None:
				cases.append((name, "\n".join(code).strip() + "\n", status))
			name = line[5:].strip()
			code = []
			status = True
			continue
		if line.strip() == "## skip-status":
			status = False
			continue
		if name is not None:
			code.append(line)
	if name is not None:
		cases.append((name, "\n".join(code).strip() + "\n", status))
	return cases


def run(code: str, path: str) -> tuple[bytes, int]:
	"""Runs a case in a fresh directory with PATH as given."""
	work = Path(tempfile.mkdtemp(prefix="ws043-"))
	env = {
		"PATH": path,
		"HOME": str(work),
		"LC_ALL": "C",
		"POSIXLY_CORRECT": "1",
		"TZ": "UTC",
	}
	# Sanitizer builds are run with the caller's sanitizer options.
	for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS"):
		if name in os.environ:
			env[name] = os.environ[name]
	try:
		result = subprocess.run(["dash", "-c", code], cwd=work, env=env,
					stdin=subprocess.DEVNULL,
					stdout=subprocess.PIPE,
					stderr=subprocess.PIPE, timeout=TIMEOUT)
		return result.stdout, result.returncode
	except subprocess.TimeoutExpired:
		return b"<timeout>", -1
	finally:
		shutil.rmtree(work, ignore_errors=True)


def compare(item):
	label, name, code, status, ours_path = item
	ref_out, ref_status = run(code, "/usr/bin:/bin")
	our_out, our_status = run(code, ours_path + ":/usr/bin:/bin")
	same = ref_out == our_out and (not status or ref_status == our_status)
	detail = ""
	if not same:
		detail = ("--- %s :: %s\n%s--- ref status %d\n%s\n--- ours status %d\n%s\n"
			  % (label, name, code, ref_status,
			     ref_out.decode(errors="replace"), our_status,
			     our_out.decode(errors="replace")))
	return label, same, detail, ref_out, ref_status


def main() -> int:
	parser = argparse.ArgumentParser()
	parser.add_argument("--bin", default="build/ws043/bin")
	parser.add_argument("--only")
	parser.add_argument("--report")
	parser.add_argument("--export")
	parser.add_argument("--jobs", type=int, default=os.cpu_count())
	options = parser.parse_args()
	ours = str(Path(options.bin).resolve())

	work = []
	for path in sorted((HERE / "cases").glob("*.sh")):
		if options.only and path.stem != options.only:
			continue
		for name, code, status in parse_file(path):
			work.append((path.name, name, code, status, ours))

	with concurrent.futures.ThreadPoolExecutor(options.jobs) as pool:
		results = list(pool.map(compare, work))

	totals: dict[str, list[int]] = {}
	details = []
	for (label, same, detail, _, _) in results:
		entry = totals.setdefault(label, [0, 0])
		entry[1] += 1
		if same:
			entry[0] += 1
		else:
			details.append(detail)
	passed = 0
	count = 0
	for label in sorted(totals):
		good, total = totals[label]
		passed += good
		count += total
		print("%-24s %4d/%4d" % (label, good, total))
	print("TOTAL %d/%d" % (passed, count))
	if options.report:
		Path(options.report).write_text("\n".join(details))

	# For the guest: each case and the reference output, a hundred to a
	# directory, in the form plan/tools/sh/guest-diff.sh reads (the
	# status, the name, then the output).  A case that does not compare
	# the status runs in a subshell and exits 0.
	if options.export:
		out = Path(options.export)
		for index, (item, result) in enumerate(zip(work, results)):
			label, name, code, status, _ = item
			want_status = result[4]
			if not status:
				code = "(\n" + code + ")\nexit 0\n"
				want_status = 0
			group = out / ("%02d" % (index // 100))
			group.mkdir(parents=True, exist_ok=True)
			(group / ("%04d.sh" % index)).write_text(code)
			(group / ("%04d.exp" % index)).write_bytes(
			    ("%d\n%s :: %s\n" % (want_status, label, name)).encode()
			    + result[3])
		print("exported %d cases to %s" % (len(work), out))
	return 0


if __name__ == "__main__":
	sys.exit(main())
