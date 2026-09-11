# ws019-p043: file-owned indirect metadata extents

Status: completed q176
Parent: [WS019](../ws.md)
Timebox: 90 active minutes

Add an optional filesystem capability to enumerate file-owned metadata sectors,
separate from logical data extents. No provider means no file-exclusive metadata
(e.g. FAT tables are shared). UFS enumerates every reachable indirect block in
the file-size-bounded tree, at depth at most three, after the same geometry,
allocation and snapshot admission checks as data mappings. Do not read unused
pointers past EOF, allocate file blocks or allocate an in-memory tree. Propagate
callback/read failures and release the mount lock. Data callbacks remain unchanged.

Test exact single/double/triple metadata sequences, EOF cutoff, empty metadata
for direct-only files, failures/holes and native sector scaling against the
actual complete UFS source. Test common optional-provider dispatch. Three builds.
Final claim consumers must combine these ranges with data ranges before UFS
backing admission; canonical identity and snapshot exclusion are still required.
No UFS swap completion claim in this prerequisite.
