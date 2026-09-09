# q156 retained disk metadata

Date: 2026-09-09
Timebox: 120 active minutes

Integrate existing strict diskpart/blkid parsers with the now-64-bit bounded
reader. Capture a whole disk and its exact live children, a healthy parsed GPT,
all five raw metadata ranges, and each partition's first 512 BPB bytes. Resolve
every GPT partition to exactly one live identity/extent and reject unaccounted
children. Keep immutable record fields and bytes for later revalidation; no
mount, reload, write, partitioning or publication belongs in this component.

Read in at most 64-KiB chunks and retain at most 2 MiB of GPT data, matching a
bounded supported table rather than allocating arbitrary input-sized buffers.
Reject inconsistent ranges/over-limit tables before reading. Re-read table and
device records around capture, then compare a second complete bounded capture
against the retained state. This detects observed changes; it is not an atomic
reservation against another privileged writer. The full installer still owns
reload/claim/mount admission and revalidates immediately before publication.

Production operations call existing commands with argv and instSourceRead.
Inject observation/read operations only into the internal orchestration for
host fault tests: table/device/partition replacement, raw GPT or BPB change,
short reads, error propagation and bounded reads above 4 GiB. Reject a changed
source snapshot rather than replacing the expected state with the new one.

Use the real 5-GiB QEMU NVMe fixture to capture and revalidate raw primary/backup
GPT and BPB through production commands and File.seek. Preserve on-disk tables,
labels and sentinel. This contributes to p004 discovery, but public source/target
selection, owned workspace, confirmation, publication and full acceptance remain.
