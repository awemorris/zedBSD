# WS013 CPAR architecture review cases

Parent: [WS013](../ws.md)

The Runtime rows remain design reviews. p002 and p003 turn the Boot rows into
focused host and QEMU fixtures.

| Case | Required design result |
| --- | --- |
| `CT-D001` | The threat model states what host resources a container cannot observe or modify |
| `CT-D002` | Mount, image, loop/file backing, exit, crash, and forced-stop lifetimes have no leak or recursion gap |
| `CT-D003` | The read-only base/app composition, read-only config, writable data mounts, and private tmpfs roles are unambiguous |
| `CT-D004` | Traditional services, Runtime CPAR services, and interactive Runtime CPAR instances remain distinguishable |
| `CT-D005` | Package manifests cover dependencies, modes, owners, hashes, licenses, upgrade, and rollback |
| `CT-D006` | PID 1, runtime, service console, networking, and later resource-control ownership do not overlap |
| `CT-D007` | UEFI FAT16/FAT32 long-name discovery and the exact section-based `/boot.cfg` timed/default/manual selection contract pass while legacy PC/AT and PC-98 retain their fixed FAT16 behavior |
| `CT-D008` | Boot CPAR remains administrable by direct `/boot` file operations without a second manifest, ABI registry, or `cpar boot` command |
| `CT-D009` | Runtime `cpar` grammar names base, app, data source and guest path, command, instance, failure, and cleanup behavior |
| `CT-D010` | A later `cpar build` format can reproducibly create the fixed app-image role without changing the two-image runtime contract |
| `CT-D011` | A loader on the ESP selects exactly one same-physical-disk non-ESP FAT32 containing `/vmunix` and `/boot.cfg`, injects its PARTUUID as `boot0`, and visibly rejects zero, duplicate, cross-disk, non-GPT, and malformed candidates |
| `CT-D012` | Installer-style two-partition boot reaches the overlay with no zedBSD-created `Boot####`; any firmware menu/file selection used by the test is recorded separately from installer behavior |
