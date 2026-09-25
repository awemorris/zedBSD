<!-- awesome-plan project=zedbsd record=ws034p036 -->

# ws034-p036: rootfsのtree化

Phase ID: `ws034-p036`
Parent: [WS034](../ws.md)
Status: cleared（q323-i03、2026-09-23）
Phase disposition: normal
Queue: q323（q323-i03）
実行: メインセッション

## 範囲

host上の `build/<BUILD>/rootfs` のツリーを root の正本とし、UFSのdisk imageをそのツリーから作る。
`rootfs.tar.gz` は作らない。symlinkを扱えるようにする。開発用ファイル（`/usr/include`、link時のobject）を
ツリーへ入れる。

## 変更前の構造

rootのファイル一覧（`--file DEST=SRC` の並び）から、**2か所が別々にrootを組み立てていた**。

| 組み立てるもの | 場所 | 作るもの |
| --- | --- | --- |
| ツリー | `Makefile` の `ZEDBSD_ROOTFS_TAR_RULE` | `$(BUILD)/rootfs` と `rootfs.tar.gz` |
| UFS image | `tools/build/make-arch-overlay-ufs.noct` | `$(ARCH_IMAGE_DIR)/<arch>.ufs` |

同じ一覧から別々の規則で組むので、片方にしかない性質は image に届かない。実際、
ツリーが作るdirectoryは12個、UFS側は17個で、`dev`・`boot`・`shm`・`usr/sbin`・`usr/libexec`・
`etc/ssh`・`var/empty` はツリーに無かった。`lib/arch.id` と `etc/zedbsd-root`、`tmp`・`shm` の 1777 も
UFS側にしか無かった。

## 変更後

- `ZEDBSD_ROOTFS_TREE_RULE`（`Makefile`）が **完成したrootをツリーとして作る**。
  directory一覧と mode（`ZEDBSD_ROOTFS_DIRECTORIES` は 0755、`ZEDBSD_ROOTFS_STICKY_DIRECTORIES` は 1777）、
  `var/run` → `../run` のsymlink、`lib/arch.id`、`etc/zedbsd-root` を持つ。
- `ZEDBSD_ROOTFS_UFS_IMAGE_RULE` が `make-arch-overlay-ufs.noct --tree $(BUILD)/rootfs` を呼ぶ。
  **imageはそのツリーそのもの**である。
- `--tree` を `make-arch-overlay-ufs.noct` に追加した。`--file`・`--mode` とは排他で、
  与えられたdirectoryをそのまま `zedimage-host ufs` へ渡す。
- `rootfs.tar.gz` は作らない。`ZEDBSD_ROOTFS_TAR_RULE` は無い。

試験用のimage（`AMD64_DEFERRED_TEST_UFS`、POSIX phase2〜5）は独自のファイル一覧を持つので、
従来の `ZEDBSD_ARCH_UFS_IMAGE_RULE`（`--file` から一時rootを組む経路）をそのまま残した。

### 開発用ファイル

`ZEDBSD_ROOTFS_DEVELOPMENT`（既定 `y`）が `y` のとき、target sysrootの
`usr/include`（202ファイル）と `usr/lib`（`crt0.o`・`crt1.o`・`libc.a`・`libm.a`・compiler-rt 等）を
ツリーの `/usr/include`・`/usr/lib` へ入れる。ターゲット上でbuildできるようにするため。
組込み用に外せるよう `n` で無効になる。menuconfigのoptionにするのは p039。

## 途中で見つけた2件

### 1. `--mode` が `--file` より先に走っていた

ツリーの組み立ては `--file` と `--mode` を**1つの並びとして順に**処理していた。
`--mode /sbin/zedinst=0755` は `I386_ARCH_FILES` に、`--file /sbin/zedinst=...` は
後から連結される `ZEDBSD_PACKAGE_FILES` にあるため、chmodがcopyより先に走り、
`chmod: cannot access ... zedinst` を出したうえで**ファイルがツリーに入らなかった**。
UFS側は2つを別のループで処理していたので出ず、ツリーは image に使われていなかったので誰も気づかなかった。

`--file` を全部済ませてから `--mode` を回す2パスにし、**`--mode` が指すファイルが無ければ
その場で失敗する**ようにした（`rootfs: --mode names a file nothing installed: ...`）。
この種の取りこぼしが二度と黙って通らない。

### 2. UFSのinodeがバイト数だけで決まっていた

`zedimage-host` はinodeをcylinder group（1群 IPG=256）から取り、群の数はimageのバイト数だけで決まる。
`/usr/include` の202ファイルが加わったi386のツリー（540 entry）で **`inode table full`** になった。
バイト数は足りているのにinodeが尽きる。

`zedimage-host ufs` に `--inodes=N` を足し、必要数から群を増やすようにした。
Noct側は `find TREE | wc -l` で数え、`entry数×3/2+256` を要求する（後でpackageが増えても効くように）。

## 検証

| 構成 | build | 起動 |
| --- | --- | --- |
| amd64 | `disk-image` エラー0、ツリー806 entry / 269 MiB、image 595,591,168 byte | boot-test **PASS** |
| pcat | `disk-image` エラー0、ツリー496 entry / 16 MiB | boot-test（`BOOT_MODE=bios-ide`）**PASS** |
| pc98 | `disk-image` エラー0、ツリー540 entry / 23 MiB、image 65,011,712 byte | PC-98 QEMUで行頭の `login:` を確認 |

image の中身を直接読んで確かめた（`tools/build/check-ufs-image.py` の `UFS` で lookup）。

```
build/arch-images/amd64.ufs: canonical zedBSD UFS OK
/bin/sh /bin/which /usr/include/stdio.h /usr/lib/crt0.o
/lib/arch.id /etc/zedbsd-root /var/run /tmp /shm /dev   すべて存在
```

ツリー側も確認: `var/run` は `../run` へのsymlink、`tmp`・`shm` は 1777、`etc` は 0755、
`lib/arch.id` は `amd64`、`etc/zedbsd-root` は `zedBSD ufs root v1`。

`git diff --check` PASS。

## 影響を受けた既存の試験

`plan/ws003/tests/boot-parameter-qemu-acceptance.sh` と
`plan/ws016/tests/runtime-swap-qemu-acceptance.sh` が `rootfs.tar.gz` を展開していた。
ツリーを直接 `cp -R` する形へ直した。どちらも起動してconsole logをgrepするharnessで、
Masterの方針により実行はしていない（未実施）。

## 制限

- 開発用ファイルのmenuconfig option は p039。今は `ZEDBSD_ROOTFS_DEVELOPMENT` のmake変数だけ。
- packageの置き場所を `/usr` へ揃えるのは p037。今は WS032 のライブラリが `/lib` にある。
- `.pc`（pkg-config）はまだ誰も作っていないので入れていない。
- amd64のimageが 595 MB と大きい。ツリー269 MiB に対する倍＋余白の見積りで、
  `/usr/lib` にLLVM・libc++が入っているため。減らすのは p037・p039の話。

## 受け入れ

- `$(BUILD)/rootfs` が root の正本で、UFS image はそれから作られる。`rootfs.tar.gz` は無い。
- symlink・directory mode・`arch.id`・`zedbsd-root` が image に届く。
- `/usr/include` と link用objectが image に入る。
- amd64・pcat・pc98 の `disk-image` が通り、3つとも login prompt が出る。
