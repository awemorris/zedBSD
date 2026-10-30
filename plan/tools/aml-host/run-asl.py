#!/usr/bin/env python3
"""Compiles the WS049 ASL tests and runs them in aml-host and in acpiexec.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    plan/tools/aml-host/run-asl.py [NAME...]

Each plan/tools/aml-host/asl/NAME.asl defines \\MAIN, which returns 0 when every
check holds or the number of the first failing check.  The test is compiled
with iasl -oa (no constant folding) into build/aml-host/asl/, run by aml-host
--main, and run by acpiexec as the reference; it passes when both return 0.

Optional files beside a test:
  NAME.args          more aml-host arguments; @SUPPORT@ is the directory of
                     the compiled support tables
  NAME.harness-only  do not run acpiexec (for what it cannot do, LoadTable)
  NAME.evals         lines "PATH EXPECTED": after MAIN, aml-host --eval PATH
                     must print EXPECTED (e.g. "Integer 0x1", or "error");
                     acpiexec must fail exactly when EXPECTED is "error"
  NAME.output        lines aml-host must print (e.g. "NOTIFY \\DEV0 0x80")

asl/support/*.asl are tables other tests use: each is compiled to
build/aml-host/asl/support/NAME.aml, and its bytes are written to NAME.inc as
a list a test can #include inside a Buffer.  Needs acpica-tools.
"""
import os
import re
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
SOURCE = os.path.join(REPO, "plan", "tools", "aml-host", "asl")
OUT = os.path.join(REPO, "build", "aml-host", "asl")
SUPPORT = os.path.join(OUT, "support")
HOST = os.path.join(REPO, "build", "aml-host", "aml-host")


def compile_asl(source, prefix):
	"""Compiles one ASL file; returns an error message or None."""
	result = subprocess.run(
		["iasl", "-oa", "-vi", "-I", SUPPORT, "-p", prefix, source],
		capture_output=True, text=True)
	with open(prefix + ".iasl.log", "w") as stream:
		stream.write(result.stdout + result.stderr)
	if result.returncode != 0 or not os.path.exists(prefix + ".aml"):
		return "iasl failed, see %s.iasl.log" % prefix
	return None


def build_support():
	"""Compiles the support tables and writes their byte lists."""
	os.makedirs(SUPPORT, exist_ok=True)
	directory = os.path.join(SOURCE, "support")
	if not os.path.isdir(directory):
		return None
	for name in sorted(os.listdir(directory)):
		if not name.endswith(".asl"):
			continue
		prefix = os.path.join(SUPPORT, name[:-4])
		problem = compile_asl(os.path.join(directory, name), prefix)
		if problem:
			return "support %s: %s" % (name, problem)
		with open(prefix + ".aml", "rb") as stream:
			data = stream.read()
		with open(prefix + ".inc", "w") as stream:
			for start in range(0, len(data), 16):
				stream.write(", ".join("0x%02X" % byte for byte in data[start:start + 16]))
				stream.write(",\n" if start + 16 < len(data) else "\n")
	return None


def read_lines(path):
	if not os.path.exists(path):
		return []
	with open(path) as stream:
		return [line.rstrip("\n") for line in stream if line.strip() and not line.startswith("#")]


def acpiexec(aml, commands):
	"""Runs acpiexec with commands on standard input; returns its output."""
	return subprocess.run(
		["acpiexec", aml], input="".join(command + "\n" for command in commands) + "quit\n",
		capture_output=True, text=True).stdout


def run_test(name):
	"""Runs one test; returns (passed, message)."""
	prefix = os.path.join(OUT, name)
	problem = compile_asl(os.path.join(SOURCE, name + ".asl"), prefix)
	if problem:
		return False, problem
	arguments = []
	for line in read_lines(os.path.join(SOURCE, name + ".args")):
		arguments.extend(line.replace("@SUPPORT@", SUPPORT).split())
	evals = [line.split(None, 1) for line in read_lines(os.path.join(SOURCE, name + ".evals"))]
	command = [HOST, "--quiet", "--main"] + arguments
	for path, _ in evals:
		command += ["--eval", path]
	output = subprocess.run(command + [prefix + ".aml"], capture_output=True, text=True).stdout
	lines = output.split("\n")
	failures = []
	if "MAIN passed" not in lines:
		main = [line for line in lines if line.startswith("MAIN")]
		failures.append("aml-host: " + (main[0] if main else "no result"))
	for path, expected in evals:
		found = [line for line in lines if line.startswith(path + " = ")]
		got = found[0][len(path) + 3:] if found else "nothing"
		got = re.sub(r"^error -?\d+$", "error", got)
		if got != expected:
			failures.append("aml-host %s = %s, expected %s" % (path, got, expected))
	for expected in read_lines(os.path.join(SOURCE, name + ".output")):
		if expected not in lines:
			failures.append("aml-host did not print: " + expected)
	if not os.path.exists(os.path.join(SOURCE, name + ".harness-only")):
		oracle = acpiexec(prefix + ".aml", ["evaluate MAIN"] + ["evaluate " + path for path, _ in evals])
		match = re.search(r"Evaluation of \\MAIN returned object.*\n\s*\[Integer\] = ([0-9A-F]+)", oracle)
		if not match or int(match.group(1), 16) != 0:
			failures.append("acpiexec: MAIN " + ("returned 0x" + match.group(1) if match else "gave no result"))
		for path, expected in evals:
			failed = re.search(r"Evaluation of \S*%s failed" % re.escape(path.lstrip("\\")), oracle) is not None
			if failed != (expected == "error"):
				failures.append("acpiexec %s %s" % (path, "failed" if failed else "did not fail"))
	if failures:
		return False, "; ".join(failures)
	if os.path.exists(os.path.join(SOURCE, name + ".harness-only")):
		return True, "passed (aml-host; acpiexec cannot run it)"
	return True, "passed (aml-host and acpiexec)"


def main():
	os.makedirs(OUT, exist_ok=True)
	problem = build_support()
	if problem:
		print(problem)
		return 1
	names = sys.argv[1:] or sorted(name[:-4] for name in os.listdir(SOURCE) if name.endswith(".asl"))
	passed = 0
	failed = 0
	for name in names:
		ok, message = run_test(name)
		print("%s: %s" % (name, message))
		if ok:
			passed += 1
		else:
			failed += 1
	print("asl: %d passed, %d failed" % (passed, failed))
	return 0 if failed == 0 else 1


if __name__ == "__main__":
	sys.exit(main())
