# q156 retained disk metadata

Date: 2026-09-09
Status: component accepted; complete p004 remains uncleared

Production `metadata.noct` now combines the real diskpart/blkid parsers with
bounded raw reads. It reconciles every GPT partition against exactly one live
child and identity, rejects unaccounted children, retains all five GPT metadata
ranges and each partition BPB, and compares complete repeated observations.
Reads are at most 64 KiB and total GPT retention is capped at 2 MiB. Comparison
keeps the original expected state; it does not silently refresh it after change.

Host JIT and interpreter pass stable/chunked capture above 4 GiB and eight
refusal cases: changed identity, changed device ID, wrong child extent, extra
live child, changed raw GPT, changed BPB, read error and short read.
`/tmp/zedbsd-q156-host.log`. The actual Noct module is exercised with injected
observation/read operations; mock bytes are not claimed as validated raw GPT.

Private amd64 fixture build passes (`/tmp/zedbsd-q156-fixture.log`). QEMU
`temp/q156-metadata/result.json` reports **PASS retained disk metadata and source
inspection**. Native tests run both the fault fixture and production command/
read operations, capturing and revalidating the real 5-GiB NVMe GPT and FAT32
BPB. Existing 576 admission combinations, 102 transaction scenarios, populated
tmpfs paths, large File.seek and actual USB source capture also pass. Retained
GPT/FAT/sentinel and original production image hashes are unchanged.

Only Noct modules and private test plumbing changed; the q155 maintained builds
remain the kernel/libc/package baseline. Focused diff and Python syntax checks
pass. No full installation or new physical acceptance is claimed.

Remaining integration: independently resolve firmware/config source devices,
select source/destination using these snapshots, own workspace and mount
lifetimes, inspect configuration uniqueness, confirm interactively, revalidate
before publication, connect the six-file transaction and ship the public
launcher/package. Complete the end-to-end p004 gates before p005 installed boot.
These observations are not a reservation against another privileged writer;
the installer must still apply its reload/claim/mount ownership design.
