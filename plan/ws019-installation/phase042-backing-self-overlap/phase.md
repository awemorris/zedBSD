# ws019-p042: reject self-overlapping backing ranges

Status: completed q175
Parent: [WS019](../ws.md)
Timebox: 60 active minutes

Canonicalize a preparing file claim's extents, sort its private physical range
array in-place with O(n log n) worst-case work, and reject overlapping adjacent
ranges with EINVAL before taking the registry spinlock. Adjacent ranges are
valid. Reordered ranges affect ownership only, not the caller's logical I/O map.
On failure retain the preparing claim so retry/release remains explicit. No
new array, filesystem mutation or user ABI. Test physical aliases, duplicates,
partial/nested overlaps, touching and reversed disjoint ranges, and a generated
oracle against exhaustive pair comparisons. Preserve existing claim lifetime,
raw exclusion and formatter/swap tests. Three builds verify integration.

UFS identity, indirect-metadata alias proof and snapshot exclusion remain next;
this prerequisite does not enable UFS swap or complete native installation.
