# q152 admission manifest and capacity result

Date: 2026-09-09
Status: component verified; full p004 uncleared

`userland/base/zedinst/admission.noct` now validates the supported fixed source
configuration, constructs the six ordered file entries consumed by the existing
transaction, generates explicit payload PARTUUID configuration, parses one
complete df record and computes cluster-rounded staging capacity. The command
adapter exposes actual capacity observation/admission and validates controlled
absolute paths before splitting their parent. No public launcher is installed.

The loader's real grammar rejects spaces, comments and quoting, so source
admission does too. CRLF and an unterminated final line are handled within the
loader's bounds. Redirected artifacts, unsupported init/extra root or swap
directives, duplicate directives, aliased mount paths and inconsistent capacity
records are rejected. The fixed 32-MiB data and 64-MiB swap sizes are retained.

Capacity accounts only for already-verified missing files. File bodies round
up to the supplied cluster size; directory growth has the conservative
per-file/per-directory allowance in [the design](q152-design.md). It is not a
reservation. The caller still has to obtain verified BPBs, existing-file results
and directory counts. Manifest source records likewise require the forthcoming
immutable source identity/digest/format admission; this pure module does not
claim to establish that provenance itself.

## Evidence

- Actual production Noct module, host JIT and interpreter: 576 combinations
  (all 64 presence subsets × nine cluster sizes), source/layout/parser/error
  boundaries. Existing transaction policy retains 102 passing scenarios.
  `/tmp/zedbsd-q152-host.log` contains the final host run.
- The initial test wrapper used three protected-call arguments; current Noct
  supports two. Bundling test arguments corrected the fixture, with no Noct
  runtime change.
- Private amd64 image builds pass with `make -j16`, explicit CI config and
  `transaction-qemu.mk`; final log `/tmp/zedbsd-q152-fixture3.log`.
- `temp/q152-admission/result.json`: **PASS managed-file transaction**.
  Native Noct passes 576 admission cases and 102 transaction policy cases,
  reads actual df capacity, then uses the real command adapter for interrupted
  publication, recovery, identical rerun, conflicting configuration preservation,
  swap activation (16383 slots), removal and unmount.
- `temp/q152-capacity/result.json`: **PASS capacity admission**. The short
  `--capacity-only` run exercises real df admission, refusal of a 2-GiB staging
  requirement on the smaller target, and zero staging requirement when all
  finals have already been verified. This branch does not format/publish files.
- Both native runs preserve the runner's GPT/labels/unmanaged sentinels and
  ordinary source-image SHA-256. Tests use disposable QEMU USB/NVMe media.

The file transaction fixture still uses sample executable bytes for its first
three roles and one FAT partition. The generated distinct-ESP/payload manifest
is checked in Noct but is not yet a public bootable installation. No new
physical-machine, performance or installed-NVMe-only boot claim is made.

## Concrete resume

Continue [admission integration](admission-design.md): independently resolved
firmware/config source mounts, bounded raw GPT/BPB retention and revalidation,
actual PE/ELF/UFS source-format checks, owned directories/mount cleanup,
interactive confirmation, public packaging and full command acceptance.
Noct's 32-bit File.seek limit cannot read a real disk's backup GPT above 2 GiB;
the existing 64-bit amd64 dd path and exact-size small metadata files are the
planned command-based solution. p005 follows the completed public p004.
