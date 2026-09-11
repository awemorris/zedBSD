# ws019-p044: complete file claim finalization

Status: completed q177
Parent: [WS019](../ws.md)
Timebox: 90 active minutes

Add a file-aware wrapper to the shared claim finalizer. Validate that the held
file matches the preparing claim, count optional owned metadata, allocate one
canonical array for data plus metadata, fill and validate both, and use existing
self-overlap/registry checks before publication. No logical map changes. Retain
preparing protection on error, reject count overflow/count-fill mismatch and
foreign ranges. Raw finalization keeps its existing interface.

Migrate formatter, swap and loop file consumers together. Test data/metadata
and metadata/metadata collisions, normal metadata protection, provider failure,
wrong identity, count change and cleanup using the production registry. Run
formatter/swap regressions, three builds, and disposable USB/loop QEMU boot.
UFS identity and snapshot exclusion remain before enabling UFS swap.
