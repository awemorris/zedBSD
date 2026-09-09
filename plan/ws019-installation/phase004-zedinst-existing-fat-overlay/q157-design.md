# q157 source and destination discovery

Date: 2026-09-09
Timebox: 120 active minutes

Connect the retained boot sysctl selectors and global device/blkid inventory to
q156 metadata capture and the existing strict ESP/payload selection. Require
one configuration match, uniquely resolved firmware/config PARTUUIDs on one
source disk, and a distinct writable destination with exactly one usable FAT32
ESP and the explicitly named distinct FAT32 payload. Compare source origins to
the retained source metadata, and take a second inventory/provenance observation
to reject observed replacement or new identity ambiguity.

This component only observes; destination reload, owned mounts, source-config
inspection, interactive confirmation and publication remain the next integration
boundary. Keep snapshot/revalidation callable so later phases can enforce these
checks after their own mount acquisition, without reloading a mounted disk.

Host injected observations cover unavailable/ambiguous provenance, changed
device/parent/partition identities, same-source destination, wrong or duplicate
ESP/payload, and healthy selection. QEMU uses actual USB provenance and a
disposable GPT NVMe with two FAT32 partitions, preserving GPT/labels/sentinels.
Do not claim installed boot or complete p004 from discovery acceptance.

## Native finding: supported read-only source profile

The ordinary generated boot USB is GPT plus one BIOS FAT32 MBR alias. Its
primary CRC and last-sector backup signature are valid; diskpart reports
DP_UNSUPPORTED (2) because it correctly refuses editing hybrid tables.
The first native discovery run therefore rejects source metadata before target
selection (`temp/q157-discovery`). Do not rewrite the fixture to hide this normal
producer profile or allow arbitrary unsupported destination tables.

Refine source-only admission: keep destination instTable strict. Permit flag 2
only provisionally for the read-only source, then validate retained raw MBR and
both GPT headers. Require 512-byte sectors, one exact protective entry covering
the disk, one BIOS FAT32 alias matching a GPT partition's extent, other MBR
entries zero, supported 92-byte headers/128-byte entries and backup at the last
sector. Reject degraded flags, unknown flags, wrong protective extent, extended
headers/entries, misplaced backup and unrelated/overlapping legacy aliases.
Both GPT copies remain subject to diskpart's CRC/content validation and the
repeated raw-byte/identity comparison. Explicit source policy cannot be passed
to destination selection. Test the distinctions before rerunning native discovery.
