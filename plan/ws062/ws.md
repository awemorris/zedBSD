<!-- awesome-plan project=zedbsd record=ws062 -->

# WS062: amd64 の disk image を ESP の vmunix・UFS の root partition・swap partition の構成にする

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG004
Related Milestones: MG002（fg011: configure の性能）
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: q456（ws062-p003）
Resume point: p003（既定の切り替え）。configure（`/root`）の tmpfs との差（UFS の同期書き）は F-015 の判断待ち
<!-- awesome-plan-current:end -->

## 目標

2026-09-25 ユーザー指示: 「オーバレイファイルシステムでこれ以上の改善が見られない場合、ディスクイメージを変更して、vmunixをEFIシステムパーティションに置いて、rootfsはUFSのパーティションにしましょう。スワップは単独パーティションにしましょう。これでかなり性能が上がると思います。」

今の amd64 の image（`hybrid`）は GPT に ESP と FAT の payload partition を持ち、payload に `vmunix`・`zedbsd.cfg`・`rootfs.img`（UFS、read-only の lower）・`data.img`（UFS、upper）・`swapfile` を置く。root は overlay で、書き込みは UFS → loop → FAT の file → USB と 3 段を通り、loop が request ごとに FAT の fsync（USB の flush）を出す（ws061-p002 の標本）。expat の configure は tmpfs で 31 秒、root の overlay で 55 秒（ws061-p003）。

新しい構成（layout `native`、amd64 UEFI）:

| partition | 型 | 中身 |
| --- | --- | --- |
| 1 | ESP（FAT） | `EFI/BOOT/BOOTX64.EFI`、`vmunix`、`zedbsd.cfg`（`kernel=vmunix`、`rootpart=PARTLABEL=zedBSD-root`、`swap0=PARTLABEL=zedBSD-swap`） |
| 2 | UFS（`zedBSD-root`） | root の全体（今の rootfs と data の中身）。読み書き |
| 3 | swap（`zedBSD-swap`） | swapfile と同じ header の raw の swap |

既存の土台（調べた範囲）: kernel は `rootpart=`（native の UFS root）と raw partition の swap（`kern_swap_source_prepare_raw`、header が要る）を持ち、selector は `/dev/`・`UUID=`・`LABEL=`・`PARTUUID=`・`PARTLABEL=`（`block_identity_resolve`）。UEFI の loader は `rootpart=` をそのまま kernel に渡し、raw の swap の selector を知っている。BIOS だけの native の image（`ufs-root-hdd-image.img`、MBR）は既にある。無いのは UEFI の GPT の native の layout の生成（`zedimage-host`）、ESP から `zedbsd.cfg` と `vmunix` を読むことの確認、root の UFS の大きさ、swap partition の生成、検査器、既定の切り替え。

design policy 2.3（installer v1: payload は FAT、native root は後の仕事）はこの指示で native root を前に出す。installer と `hybrid`・`uefi`・`bios` の layout は残す（既定だけを変える）。

範囲外: pcat（BIOS）・pc98・rpi4 の image（Future Work に記録）、installer の変更。

2026-09-25 ユーザーの追加指示（記録して後回し、[F-014](../future-work.md)）: `zedbsd.cfg` は ESP を先に、無ければ BOOT という FAT partition を探す。vmunix は 2 番目の partition（UFS root）が正しく、vmunix も loopback の image も UFS から直接 load できるようにする。試験は root 4 GB・swap 4 GB、USB ではなく NVMe の emulation で起動して、比較は tmpfs ではなく Linux host の時間と（tmpfs は RAM disk なので UFS では出ない）。virtio の disk の実装も可。

受け入れ: amd64 の既定の image が native で、QEMU の UEFI・USB 起動で login prompt、SSH の harness、swap partition が有効、root に書ける。expat の configure（`/root`）が tmpfs の +20% 以内。sh・make の差分試験、`SMP-STRESS.ELF`、COW、boot の試験が今と同じ。規約。

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws062-p001](phase001/phase.md) | native の layout の image の生成（`zedimage-host`・Makefile の variant・検査器）と QEMU の起動 | cleared（q436-i01。`--layout native`、検査器、`ZEDBSD_VARIANT=native`。QEMU で boot・root は `/dev/sda2` の UFS（rw）・swap は partition・SSH） | — |
| [ws062-p002](phase002/phase.md) | native の image で harness・swap・性能（configure）・回帰を確かめ、直す | cleared（q437-i01。NVMe の起動、4 GiB の root・swap、BUG-053 の修正。configure は tmpfs 12.8 秒・`/root` 23〜25 秒（p005 の後）で +20% は未達、原因は UFS の write-through と flush（F-015、判断待ち）） | p001 |
| [ws062-p003](phase003/phase.md) | amd64 の既定と試験の道具（guest.py・boot-test・guest-batches）を native にする。文書 | in-progress（q456-i01） | p002 |
| [ws062-p004](phase004/phase.md) | 規約の全文との照合（変えた全 source） | planned | p003 |
