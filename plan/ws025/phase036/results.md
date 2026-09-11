# q245 results: HAL naming correction

Date: 2026-09-10
Status: cleared for the 18 explicitly requested renames only

44 active source/test files changed. Header declarations, five architecture
implementations and common consumers use the new names. Scan found zero old
identifiers in active source/tests. Old archived plans/logs remain evidence.
Manifest: `../temp/q245/rename.json`; source hashes and identifier checks:
`../temp/q245/verification.json`.

Actual VM/file focused fixture: ordinary 191650 checks, ASan/UBSan/leaks 191654
checks (concurrent checks vary). Logs: `../temp/q245-vm/` and
`../temp/q245/vm-run.log`. Extracted production syscall stories: PASS default and
experimental input/output; `../temp/q245/syscall.log`.

Supported sequential disk-image builds all exit 0:
- `/tmp/zedbsd-q245-amd64.log`
- `/tmp/zedbsd-q245-pcat.log`
- `/tmp/zedbsd-q245-pc98.log`

No full test suite or non-x86 runtime test was run. No compatibility aliases.
User subsequently requested hal_pmem_request removal and then an overall proposal
before further implementation. That separate change remains unimplemented while
[the complete proposal](../hal-interface-proposal.md) is reviewed. No additional
hal.h API change was made after presenting it.
