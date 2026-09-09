# q155 Noct large-seek update acceptance

Date: 2026-09-09
Status: completed

Host and target now use immutable upstream commit
`bb239816e20294c073702dc127142f6767375072` (`Fix File.seek`). Upstream version
remains 2.0.1; the package records the post-release commit explicitly. Archive:
2528421 bytes, SHA-256
`fd3d9adc7da6de0e7de6e96a1e42edd08f2f07807b00ac5078e5f0457ca1887f`.
Both existing target-adapter patches apply with zero fuzz; source manifests and
identities verify for both extractions. Patch level is zedbsd3. Original verified
trees/builds are preserved in `../temp/q155-noct-old/{host,target}`.

The upstream change removes the historical INT32_MAX check when long is 64-bit.
It does not lift it on 32-bit-long platforms. The preceding upstream delta fixes
installed headers and adds optional module I/O callbacks; default module loading
and actual target builds are exercised below.

## Evidence

- Old and new extraction verification: `/tmp/zedbsd-q155-noct-old-verify.log`
  and `/tmp/zedbsd-q155-noct-new-verify.log`.
- Host rebuild: `/tmp/zedbsd-q155-noct-host2.log`. After replacing extracted
  trees, the first ordinary build tried to run the absent host tool; explicitly
  building the documented relative host target resolved this preparation gap.
- New amd64/PCAT/PC98 `make -j16` builds and private native fixture pass:
  `/tmp/zedbsd-q155-noct-amd64-2.log`, `-noct-pcat.log`, `-noct-pc98.log`,
  `-noct-fixture.log`. The installed target artifact matches the CMake artifact;
  hashes are retained in `../temp/q155-large-seek/noct-artifacts.json`.
- Eight host JIT/interpreter runs pass: admission (576 combinations), transaction
  (102 scenarios), actual built PE/ELF/UFS plus malformed/short reads, and large
  seek. `/tmp/zedbsd-q155-noct-host-regression2.log`.
- Large-seek fixture writes distinct bytes at 2147483646, 2147483647,
  2147483648, 4294967295, 4294967296 and 4294967552, closes/reopens and verifies
  data/tell, sparse-hole zeros, 32 negative seeks preserving position, and the
  production installer bounded reader. Independent host reads confirm every
  marker and 4294967553-byte length with only 16384 allocated bytes.
- QEMU `../temp/q155-large-seek/result.json`: **PASS Noct large seek and source
  inspection**. Actual native Noct passes the same large-file checks on tmpfs
  and directly reads `/dev/nvme0n1` at byte 5368708608, the backup GPT of a 5-GiB
  disk. Signature/current/alternate LBAs match; the host independently checks
  header CRC. GPT/FAT/sentinel hashes and the original production image remain
  unchanged. Native source capture, nested/populated tmpfs lifecycle and owned
  source unmounts also pass.

The installer reader now permits nonnegative signed-64 offsets while retaining
the 65536-byte read bound and checked offset+count. Its artifact-format limits
are separate loader/format admission constraints and are not silently removed.
The old dd metadata workaround is no longer needed on amd64. Full p004 discovery,
confirmation/public installation and p005 installed boot remain incomplete.
