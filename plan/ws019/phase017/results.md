# q132 staging results — uncleared

2026-09-09. cp implementation and ordinary amd64 build passed; host cp tests
passed ordinary and ASan/UBSan (`/tmp/zedbsd-q132-cp-host.log`). Host Noct argv
runner passed before the target fixture corrections. Corrected target command
fixture passes exact argv, exit 7, timeout 124 and bounded output capture.

Final runtime: `plan/ws019/temp/q132-staging5/result.json` and
`guest.log`. Fresh exclusive FAT creation, conflict refusal, 32MiB growth/UFS
format and 64MiB growth/swap format/publication succeeded. Data growth timing
was printed as `real 31.-530`: existing time incorrectly normalizes nanosecond
subtraction; do not present that literal as an accurate decimal. Swap growth
printed 62.670s; formatters printed 0.430s and 0.070s.

Capacity refusal failed: truncate of 256MiB on the partially used FAT volume
returned timeout status 124, not expected command failure 1. Return to Noct
was delayed beyond the command timeout while the syscall completed. The final
file size is not proven by the failing short-circuit assertion. No successful
swap activation, generated-data reboot, or full staging acceptance is claimed.
Production image, GPT tables, FAT boot sector and sentinel hashes were unchanged.

Static evidence: fat_raw_truncate grows through fat_raw_write_bytes, allocating
and zeroing until an allocation fails, then rolls back. It has no capacity
preflight. A follow-up kernel capacity/rollback phase must cover existing-chain
accounting, allocation under the mount lock, failure injection, and unchanged
size/data/cluster ownership on refusal. Deadline expiry cannot be treated as
proof that a mutating syscall has stopped. Preserve command-child ownership.

Resume p017 after bounded capacity refusal is implemented and verified. Then
rerun fresh staging, target swap activation and generated-data reboot/readback,
and complete PCAT/PC98 build gates. Cross-command staging ownership and source
identity remain explicit requirements for p004. q132 is not installer acceptance.
