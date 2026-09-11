# ws019-p025 results

Status: completed, q159, 2026-09-09

Noct is packaged at `/bin/noct`; `/bin/zedinst` invokes that absolute path.
The BIOS-image producer and checker agree. Standalone packages retain their
standard PREFIX/bin contract (PREFIX=/ installs into /bin).

The shell retains `$?` across input lines, preserves the full child status and
the final pipeline command's status, uses 128+signal for signal termination,
and reports 126/127 for rejected/missing executable paths. `exit` without an
operand uses the previous status. Script files continue after ordinary command
failures and return the final status; syntax failures still stop a script.
Existing boolean builtin return conventions are adapted at the execution
boundary. This acceptance is not a claim of complete POSIX shell coverage.

`src/drivers/generic/memory-device.c` implements null and zero. Common VFS
initialization registers both on every platform. Devfs exposes 0666 character
nodes; null reads EOF, zero fills the kernel transfer buffer, and writes discard
the supplied bytes. Normal read/write poll events are immediately ready.
Failed second-node registration removes the first node and releases references.

## Evidence

- `tests/shell-status-test.py`: PASS 20 host cases.
- Existing WS001 deterministic job control: PASS,
  `plan/ws001/temp/q159-status-jobs`, `/tmp/q159-status-jobs.log`.
- `tests/memory-device-test.c`: PASS boundaries, 64 KiB transfers, poll and
  registration rollback, ordinary and ASan/UBSan. LeakSanitizer cannot operate
  under this environment's ptrace, so the latter run used detect_leaks=0;
  this fixture allocates no dynamic storage.
- `make -j16 ZEDBSD_CONFIG=config/ci/config-{amd64,pcat,pc98}.mk disk-image`:
  PASS, logs `/tmp/zedbsd-q159-runtime-amd64-final2.log`,
  `/tmp/zedbsd-q159-runtime-pcat.log`, `/tmp/zedbsd-q159-runtime-pc98.log`.
- Normal UFS image checker: `/bin/noct` and `/bin/zedinst` match built files;
  launcher mode 0755.
- `tests/run-public-runtime-qemu.py`: PASS 16 normal-image checks,
  `temp/q159-runtime5/result.json` and `guest.log`. Separate-line status 42,
  pipeline statuses, null read/write, 64 KiB zero SHA-256, script continuation,
  and the installed launcher usage/noninteractive refusals all passed.

The accepted production disk SHA-256 before/after is
`99210b23c355c11a852e9aae9dcdacc07371b8c9f8a66d69c4366a9da10c2e93`.
Its normal rootfs SHA-256 is
`e55433f4b9fc0211a8ea67e2eb94050d8e0606414d55337899ca145b0ef95f41`.

Earlier runtime fixtures are retained as failed attempts. Runtime3 used cat
without an operand, unsupported by the current cat command. Runtime4 requested
4096-byte dd input blocks but the existing character syscall buffer returns
512-byte short reads, producing 8192 bytes in 16 reads. Runtime5 deliberately
uses bs=512 count=128 and verifies all 65536 zero bytes by SHA-256. The syscall
transfer-size policy was not changed by this driver implementation.

Full p004 installation/conflict acceptance and p005 installed boot remain
separate, incomplete phases.
