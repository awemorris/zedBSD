<!-- awesome-plan project=zedbsd record=ws062p001 -->

# ws062-p001: native の layout の image の生成と QEMU の起動

Phase ID: `ws062-p001`
Parent: [WS062](../ws.md)
Status: cleared
Queue: q436-i01（cleared）

## 目的

`zedimage-host disk` に layout `native` を足す: GPT に ESP（FAT、`EFI/BOOT/BOOTX64.EFI`・`vmunix`・`zedbsd.cfg`）、UFS の root partition（PARTLABEL `zedBSD-root`、root の全ての file、既定 1 GiB）、swap partition（PARTLABEL `zedBSD-swap`、swapfile と同じ header）。Makefile の amd64 の variant に `native` を足し、`platform/amd64/zedbsd-native-uefi.cfg` を作る。検査器（`check-amd64-gpt-image.noct`）に native の検査を足す。

調べること: UEFI の loader が ESP の `zedbsd.cfg` を選ぶか（`zbl_uefi_volume_order_handles`）。root の UFS の中身を arch の UFS の image（`AMD64_ARCH_UFS_IMAGE`、harness の extra-files を含む）と data の中身から作る方法（`make-arch-overlay-ufs.noct --size-mib`、producer の上限 2 GiB）。kernel の native root の起動（init、runtime の mount、`/etc` の書き込み）。

## 受け入れ

`make ZEDBSD_VARIANT=native disk-image` が image を作り、検査器が通る。QEMU（UEFI、USB）で login prompt（`boot-test.sh`）、root が UFS の partition（`mount`）、swap が partition（`swapctl` 相当の表示）。規約（新しい C は全文）。

## q436-i01（2026-09-25）: cleared

### 変更

| 変更 | 場所 |
| --- | --- |
| `zedimage-host disk --layout native`: GPT に ESP（FAT32 64 MiB、`EFI/BOOT/BOOTX64.EFI`・`vmunix`・`zedbsd.cfg`）、`zedBSD-root`（FreeBSD UFS の型、UFS の image をそのまま、0 の 1 MiB は穴のまま）、`zedBSD-swap`（FreeBSD swap の型、swap の image）。1 MiB 境界、primary と backup の GPT | `tools/build/zedimage-host.c`（`disk_create_native` ほか。規約の全文） |
| layout の検査器（MBR、両方の GPT の CRC、3 つの partition の型・名前・範囲、root と swap の中身、ESP の 3 file を入力と比べる。image を分けて読む） | `platform/amd64/tools/check-amd64-native-image.py` |
| `ZEDBSD_VARIANT=native`: root の UFS（`$(BUILD)/rootfs` の tree から 2000 MiB・inode 16000、`AMD64_NATIVE_ROOT_MIB`・`AMD64_NATIVE_ROOT_INODES`）、swap（1024 MiB、`AMD64_NATIVE_SWAP_MIB`）、`hdd-image.img` を image の道具で直接作って検査器を通す。`check-disk-image` も native を検査 | `Makefile`（variant の一覧）、`platform/amd64/vmunix.mk`、`platform/amd64/disk-image.mk` |
| `make-arch-overlay-ufs.noct --min-inodes N` | `tools/build/make-arch-overlay-ufs.noct` |
| 設定: `kernel=vmunix`、`video=640x480`、`rootpart=PARTLABEL=zedBSD-root`、`swap0=PARTLABEL=zedBSD-swap` | `platform/amd64/zedbsd-native-uefi.cfg` |

kernel・loader の変更は無い（既存の `rootpart=`・raw の swap・loader が自分の volume の `zedbsd.cfg` を最初に探すこと、で足りた）。

### 結果（QEMU、amd64 8 GiB、UEFI、USB 起動）

| 確認 | 結果 |
| --- | --- |
| build | `make ZEDBSD_VARIANT=native disk-image` rc 0、検査器 OK。image は見かけ 3.2 GB、実 301 MB（sparse） |
| boot | `boot-test.sh` PASS（`build/boot-test-amd64-q436/login.png`）。画面: `rootpart selector PARTLABEL=zedBSD-root resolved to /dev/sda2`、`swap0 source=PARTLABEL=zedBSD-swap slots=262143`（1 GiB） |
| root | `mount`: `/dev/sda2 on / type ufs (rw)`、`df`: 2 GB のうち 246 MB 使用。`/root` に書ける |
| harness | `guest.py wait`・`run`（SSH）が通る |
| 規約 | 変えた C の行に `style-check.py` の指摘 0 |

制限: `PARTLABEL=` は全 disk で 1 つに決まる必要がある（同じ機械に zedBSD の disk が 2 つあると曖昧）。root の inode は image の道具の上限 16384（`MAX_INODES`）に縛られる。実機: 未実施。
