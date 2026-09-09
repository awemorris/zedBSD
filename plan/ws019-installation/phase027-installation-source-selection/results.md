# ws019-p027 acceptance — q162

Status: completed, 2026-09-09. Full mode/provisioning/native install and BeUI
remain p006/p007/p029. No installation-complete claim from this phase.

- Implemented referenced live root/lower/loop backing observation via existing
  sysctl, strict Noct identity admission and revalidation, disk-only first source
  screen with unavailable HTTP, and early native/no-image refusal.
- installer-rootimage.noct: 30 cases PASS in JIT and interpreter, including
  malformed records, missing/readonly flags, replaced backing identity, stale
  observations and screen keys. console-csi-test.py: 77 production-parser cases
  across write boundaries; unsupported CSI no longer leaks 4;2m into the UI.
- Maintained builds passed: /tmp/zedbsd-q162-source2-amd64.log and
  /tmp/zedbsd-q162-source-{pcat,pc98}.log. Sessions 48215 and 80420 terminal.
- temp/q162-source2 PASS (66269 terminal): live record matches stat of source
  rootfs.img on read-only FAT; sysctl mutation denied; first-page cancel and
  continue-to-review/cancel both work. Whole target and production hashes match.
- temp/q162-native2 PASS (26022 terminal): native UFS partition boot reports
  1:0:0:0:0:0. USB still contains rootfs.img, but continuing from source selection
  reports Not booted from the installer disk before destination preparation.
  First-page cancel also works; selected USB target and production hashes match.
- Actual source/review screenshots from source2 were inspected; no escape text
  artifact. Native refusal screenshot retained in native2. No QEMU is live.

Production SHA-256:
`3507b05e3a78b4ddaec4972cb0b6d6add991739e65a4b0d6b001d7939ece9eec`.

Preserved harness failures: source1 lacked '%' key mapping; native1 supplied a
whole disk where rootpart requires a partition. The latter explicitly failed
VFS initialization and was stopped via the exact runner PID, not an observation
timeout. Fresh corrected fixtures supplied the acceptance above. No aggregate
make check, physical media writes, or private .internal data access.
