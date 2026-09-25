<!-- awesome-plan project=zedbsd record=ws059 -->

# WS059: disk の無い mount にも `st_dev` を与える（BUG-047）

<!-- awesome-plan-current:start -->
Status: completed（2026-09-25）
Primary Milestone: MG004
Related Milestones: MG002
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: なし（q429 finished）
Resume point: —
<!-- awesome-plan-current:end -->

## 結果

[BUG-047](../bugs/BUG-047.md) を直した: `src/kern/mount.c` の `mount_device_number()` が disk の mount には disk の番号、bind mount には source の番号、disk の無い mount（overlay の root・tmpfs・devfs）には `0x80000000 | (1 + slot)` を返し、generic の `inode_getattr`・tmpfs・devfs・overlayfs の getattr がそれを `st_dev` に入れる（`include/kern/mount.h` に宣言。ufs・fat は不変。HAL は不変）。
QEMU amd64 8 GiB: 全 mount で `stat -c %d` が異なり（`/` 2147483652、`/tmp` …53、`/run` …54、`/dev` …55、`/dev/shm` = `/shm` …56、ufs の `/work` 2）、cross build した coreutils の `df` が overlay `/`・tmpfs・`/shm`・`/work` を大きさ付きで出す。
回帰: build warning 0、規約 0、boot PASS（`build/boot-test-amd64-q429/login.png`）、sh 1388/1425（同じ集合）、make 91/91、SMP stress 0。実機は未実施。

## 制限・移管

- root の inode 番号は全 mount で 1 のまま（`st_dev` で区別できるので POSIX の一意性は満たす）。
- `statvfs` の `f_fsid` は従来の `1 + slot` のまま（`st_dev` とは別の値）。揃えるなら別の判断。

## Phase の一覧

| Phase | 内容 | 結果 |
| --- | --- | --- |
| ws059-p001 | `mount_device_number()` を足し、generic・tmpfs・devfs・overlay の getattr が使う | cleared（q429-i01、[history](../history/queue-q429.md)） |
