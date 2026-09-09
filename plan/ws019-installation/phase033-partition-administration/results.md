# q166 / ws019-p033 results

Completed 2026-09-09. Direct physical partitions now support fd-owned
BLKRESERVE. Canonical sector intervals exclude parent/overlapping aliases;
disjoint siblings remain usable. An administrative reservation count avoids
registry scans in ordinary I/O when no reservation exists. Admission retains
the conservative leaf-wide idle frontier and explicit per-operation owner
scope. Partition reload remains invalid.

| Acceptance | Evidence |
| --- | --- |
| Host production paths | 41991 assertions each in ordinary and ASan/UBSan/leak modes; amd64/i386 ABI pass, `/tmp/zedbsd-q166-host.log` |
| Range behavior | Parent/overlap exclusion, disjoint opens and I/O, two disjoint reservations, boundary writes, owner scope and final-close release |
| Actual backing claims | Production claim and revoked-media regressions pass, `/tmp/zedbsd-q166-claims.log` |
| Native partition | `temp/q166-partition4`: sibling mounted read-only and writable, new file creation/write/fsync, reserved target first/last sector read/write/restore, bounds refusal, dup and child termination release |
| Native whole disk | `temp/q166-whole1`: existing whole-disk reservation, busy mount refusal, owner I/O/reload and release regression pass |
| Preserved bytes | Partition target plus GPT front/rear before/after hashes identical; whole-disk probe target hash identical; production image unchanged in both runs |
| Builds | `/tmp/zedbsd-q166-{amd64,pcat,pc98}.log`, all exit 0 |

Failed experiments remain recorded. `q166-partition1` failed creating a file
with mode 0644 on a plain FAT mount. Error 21 is zedBSD EOPNOTSUPP, **not**
Linux EISDIR; the mount presents fixed 0755 and intentionally rejects an
unrepresentable creation mode. No FAT production change was required.
`q166-partition2` caught the diagnostic fixture's missing LP64 ABI define at
compile time, before starting QEMU. Both native probe runners now select the
proper ABI explicitly. `q166-partition3` proved unreserved creation with 0755
succeeds and reproduced the reserved 0644 refusal. `q166-partition4` uses 0755
and passes the entire reserved sibling creation/write test. The test does not
bypass new-file creation by using a preexisting file.

Formatter implementation and dedicated installation remain p006/p007 work;
graphical frontend p029 remains mandatory after text installation.
