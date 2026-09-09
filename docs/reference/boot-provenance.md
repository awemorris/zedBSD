# Retained boot-source identities

Status: current for UEFI V7 provenance; legacy limitations described below

The following read-only sysctl leaves describe the sources selected by the UEFI
loader, independently of later boot0 configuration:

```sh
sysctl kern.boot.firmware_partition
sysctl kern.boot.config_partition
sysctl kern.boot.config_matches
```

The first two values are `PARTUUID=...` strings for GPT partitions.
`firmware_partition` identifies the partition from which firmware executed the
loader. `config_partition` identifies the selected zedbsd.cfg volume, from
which the configured kernel path is opened. They may be different partitions.
The match count records how many candidate configuration files the loader found
on its boot disk; ordinary boot still selects the first, while an installer can
refuse an ambiguous count.

UEFI handoff V7 appends a pointer-free record after the unchanged V6 memory
ownership envelope. Firmware paths are decoded and their partition signatures
copied before firmware resources expire. The HAL validates the envelope and
passes an aligned copy to generic boot ownership. The kernel checks version,
count, partition geometry and nonzero GPT signatures before exposing selectors.
An invalid record clears both selectors and fails the V7 handoff.

Legacy boot without this record reports `unavailable` and zero matches.
Currently MBR provenance also reports `unavailable` selectors; its valid
configuration count is retained. This limitation does not disable MBR boot.
V6 BIOS memory ownership and legacy parameter handling are unchanged.

Consumers must resolve selectors against present devices and reject duplicate
identities. A selector establishes origin; it does not freeze file contents,
claim an installation destination or prove that removable media has remained
attached. These remain separate installer checks.

[WS019-p016](../../plan/ws019-installation/phase016-boot-source-provenance/phase.md)
owns acceptance evidence.

## Live root-image observation

`sysctl kern.boot.root_image` reports the current root rather than a loader
configuration string. Its colon-separated fields are version, flags, loop
device ID, backing device ID, backing inode and backing bytes. Version is 1.
Flag bits are 1 (overlay root), 2 (lower mount present), 4 (lower mount read-only)
and 8 (loop attachment read-only). A normal read-only root image reports 15;
native-root boot reports zero flags and identities. Partial flags cannot prove
an installer root image. Writes to this leaf are refused.

The query retains the live root, lower path and loop backing file while reading
their relationship and identity. It does not reconstruct a private pathname.
The installer separately resolves rootfs.img on the admitted source partition
and compares device, inode and size, alongside the retained physical provenance
above. A file with that name merely existing is insufficient. Recheck the live
record and source identity after interaction before committing changes.

[WS019-p027](../../plan/ws019-installation/phase027-installation-source-selection/phase.md)
owns the private-root positive and native-root refusal acceptance.
