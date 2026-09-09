# Canonical verification preparation

2026-09-09. Read-only source investigation while q140 owns runtime; no new
formatter feature or installer acceptance is claimed here.

The q134 `/run` scratch route cannot use the current exclusive formatter
reservation. Avoid bypassing it or requiring a second full installation image.
A candidate replacement is an explicit read-only pristine-image verification
mode on the existing `mkfs` and `mkswap` commands, called by Noct.

Current `ufs_format_verify` checks deterministic initialized metadata, journal,
inode, allocation-map and superblock regions, but its generation iterator does
not visit every unused data byte. `swap_format_verify` checks the canonical
header only and intentionally does not initialize or inspect unused slots.
Neither operation presently proves byte identity with a fresh zero-backed image.

A proper pristine mode must combine the existing deterministic comparisons with
complete coverage of unvisited bytes (zero for this canonical profile). Coverage
must be derived from the generator's actual extents, include partial blocks and
overlap checks, and use bounded metadata instead of an image-sized allocation.
Do not infer unused regions from just the allocation bitmap or silently reuse
the weaker format verification as exact comparison. Add independently corrupted
free-data, reserved-gap, partial-block/tail, journal and metadata cases; compare
against the maintained host-generated canonical image as an independent oracle.

The command mode must open an existing regular file without following symlinks,
retain descriptor identity/size, and perform no truncation, allocation, formatting
or activation. Report an error on short reads or content mismatch. Its success
is a descriptor-scoped observation, not a lease preventing later writes; the
Noct transaction still has to retain/revalidate selection, inode identity and
content at its publication boundary. Existing format writes retain the exclusive
reservation and flush/reopen checks.

New staging files can still be generated on the selected disk-backed FAT target,
where the formatter reservation is supported. Existing final data/swap files are
only verified and preserved; differing content must not be reformatted as a
repair. This is an existing-command feature proposal within user authorization,
not a new private installer helper and not a Noct ioctl binding.
