<!-- awesome-plan project=zedbsd record=ws307-p008 -->

# ws307-p008 boot loader が UFS から kernel を読む

- 親: [WS307](../ws.md)。仕様: [spec.md](../spec.md) §5.9（loader の FAT・firmware の FAT）・§5.10。Queue: q1665 / q1665-i01（担当 P4、2026-10-29）。
- Status: in-progress（2026-10-29 Q1 の判定: 512 と 4096 の段は受け入れを満たした。残り: 512 の disk の j3 の sync の確かめ。q1665-i07 で一度確かめた後に UFS の直しが入ったので、q1698-i01 で今の tree での確かめ直しを p004 の試験に入れた（未実行））
- 出典: 2026-10-28 ユーザー「P4に、ESPにカーネルをインストールすると回答してしまいましたが、ブートローダがUFSからカーネルをロードできるようにしたいです。そのphaseを作ってほしいです。」

## 結果（2026-10-29、P4）

- `bootloader/common/ufs-read.[ch]`（新規、freestanding、BIOS の loader でも後に使える形）: native（little endian）の UFS2 の読み取り専用の reader。superblock の検査（magic、逆の byte order は `SWAPPED` で拒む、block 4096〜65536 の 2 の冪、fragment、`nindir`・`inopb`、group と inode table、`fs_size`×`fs_fsize` ≤ partition）、fragment が media の block より小さい volume は `FRAGMENT_TOO_SMALL` で拒む、fragment の番号から byte の offset を出して media の `BlockSize` で割る、inode（256 byte）、直接 12・1〜3 段の間接 block（段ごとに 1 つの cache）、穴（0）は 0 の byte、範囲外の block 番号は `BAD_BLOCK`、directory（512 byte の directory block、record の長さの検査、16 MiB の上限）、絶対 path の成分ごとの lookup、symlink は辿らず `SYMLINK`、最後の成分は regular file だけ。memory の確保はせず、呼ぶ側の work（約 256 KiB）を使う。
- `bootloader/uefi/root-config.[ch]`（新規、UEFI だけ）: zedbsd.cfg の `kernel=rootpart:PATH`・`logo=rootpart:PATH` から `rootpart:` と続く `/` を外した写しを作り、共有の parser（`zedbsd-config.c`、変えていない）がその path を今の規則（`..`・`:`・長さ）で検査する。root に置く file がある時は `rootpart=PARTUUID=GUID` がちょうど 1 行要る（無い・2 行・`PARTLABEL=` などは拒む）。GUID は GPT の byte order（先頭 3 つの field は little endian）に直す。
- `bootloader/uefi/bootx64.c`: file の源の抽象（`struct boot_file`、開く・大きさ・offset からの読み）で FAT（`EFI_FILE_PROTOCOL`）と root の UFS を同じに読む。`load_kernel`・`show_logo` はこの口を使う。root の partition は、全ての controller を `ConnectController`（再帰）した後に `LocateHandleBuffer(ByProtocol, BlockIo)` で全ての block の handle を列挙し、device path の `HD()` の signature（PARTUUID）を byte で照合する。cfg の FAT と同じ物理 disk でちょうど 1 つなら、その `BlockIo` で読む。他の disk に同じ PARTUUID があれば `A64 KERN UFS duplicate PARTUUID`、無ければ `A64 KERN UFS root partition not found`、UFS の読みの失敗は `A64 KERN UFS <理由>` を画面に出して止まる（FAT の kernel に戻らない）。`include/uefi.h` の `ConnectController` に型を付けた。
- `platform/amd64/vmunix.mk`: loader に `root-config.o`・`common-ufs-read.o` を足した。
- BIOS の loader: 共有の parser と `logo-path.c` は変えていない。`kernel=rootpart:…` は今どおり `INVALID_KERNEL_PATH`、`logo=rootpart:…` は `zbl_logo_path` が -1（拒む）。host の試験で確かめた。
- 確かめた firmware の振る舞い（OVMF、spec §5.10 の 3 と §5.9 の未確認の 2 つ）: OVMF は型の分からない GPT の partition（zedBSD の UFS の型）にも `BlockIo` の handle を作る（root を見つけて起動した）。OVMF は 4096 byte の sector の FAT32 の ESP（cluster が 65525 以上、512 MiB）から loader を起動する（loader は今の規則どおり `unsupported-block-size` で cfg を見つけずに止まる、`build/ws307/p008-4k.S8R7vh/screen.png`）。cluster が 65525 未満の 256 MiB の 4Kn の FAT32 は OVMF が mount しなかった（FAT32 の cluster の数の下限、mkfs の geometry は既に下限を守る）。OVMF は boot の disk 以外の controller を自分で接続しないことがある（2 台目の NVMe の partition の handle が無く、重なりを見逃した）ので、`ConnectController` を足した。

## 確かめ（2026-10-29、P4）

- build: `make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/ws307 build/ws307/uefi/BOOTX64.EFI` と `ZEDBSD_CONFIG=plan/ws307/tests/config-amd64-p008.mk BUILD=build/ws307/p008` の vmunix・BOOTX64.EFI（`BOOTX64.EFI check: PASS`）・boot-logo.ppm・rootfs → rc 0、loader と reader の `error:`・`warning:` 0。
- host: `sh plan/ws307/tests/host/ufs-read-test.sh` → `ufs-read-test: PASS`（zedimage-host の UFS に 24 MiB の kernel（2 段の間接 block まで）・3 MB の logo・深い directory・400 entry の directory・空の file・symlink、prime の大きさの断片で読んで byte で照合、相対 path・空の成分・symlink・途中の file・無い名前、file の末尾の先、4096 の media（fragment 1024 を拒む）、逆の byte order・magic 無し・partition を越える `fs_size`・2 の冪でない block・範囲外の間接 block の拒み）。`plan/ws307/tests/host/root-config-test.c`（`root-config.c`・`zedbsd-config.c`・`logo-path.c` と）→ `root-config-test: PASS`。
- QEMU（OVMF、NVMe、KVM。`plan/ws307/tests/p008-root-boot.sh BUILD CASE`。ESP は loader と cfg だけ、kernel と logo は root の UFS）:
  - `boot` → boot-test.sh の login prompt PASS（`build/ws307/p008-boot.6AikPl/boot/login.png`、kernel の `rootpart selector PARTUUID=… resolved to /dev/nvme0n1p2`）。
  - `fat`（今の形: kernel と logo を cfg の FAT に）→ login prompt PASS（`build/ws307/p008-boot.pDiu1w`）。
  - `no-kernel` → 画面に root の UFS から読んだ logo と `A64 KERN UFS not found`（`build/ws307/p008-boot.QCinZ1/screen.png`、logo を UFS から読めたことの証拠にもなる）。
  - `wrong-uuid` → `A64 KERN UFS root partition not found`（`build/ws307/p008-boot.EBupSd/screen.png`）。
  - `duplicate`（同じ disk の写しを 2 台目の NVMe に）→ `A64 KERN UFS duplicate PARTUUID`（`build/ws307/p008-boot.0RFqtk/screen.png`）。
  - 失敗の 3 つは loader の文字（firmware の console の font）を boot-test.sh の読み手が読めないので、screen shot を目で見て判定した。
- 観察（直していない、この Phase の外の既存の振る舞い）: cfg に `video=640x480` がある時、loader の root の失敗の文字が画面に出なかった（同じ mode の FAT の kernel の失敗は出る。debug port には出て、CPU は `halt()` の `hlt` に居た、QEMU の monitor の `info registers`）。導入先の cfg は `video=` を持たないので、その形では出る。GOP の mode を変えた後の firmware の console の行の位置の問題と見られる。

## q1665-i04（2026-10-29、P4、worktree p4-ws307h）

- loader の FAT の探索（`bootloader/uefi/volume-discovery.c`、commit f0d3945fa）: media の block は 512〜4096 の 2 の冪、BPB の sector は 512〜4096 で media の block 以上（kernel の FAT と同じ契約）。host 試験 `plan/ws307/tests/host/fat-bpb-4k-test.c`（mkfs の FAT32 を 512・4096 で書き、512・2048・4096・8192 の media で）PASS。WS013 の `plan/ws013/tests/uefi-volume-discovery-test.c` の古い 2 つの期待を直した（d1482e51b、Q1 の指示）、O2 と ASan/UBSan で PASS。BOOTX64.EFI build warning 0・check PASS。
- 4096 の起動の試験: `plan/ws307/tests/p008-4k-boot.sh`（guest の中で 4Kn の NVMe に ESP と native UFS の root を作り root を写し、その disk だけを `BOOT_NVME_BLOCK_SIZE=4096` の boot-test.sh で起動、e799efd22 で boot-test.sh に足した）。ラップアップの時点で走行中（`build/ws307-test/p008-4k.4KNAxn`）、結果は未記録。
- j3 の sync の確かめ: 4096 の disk で確かめた（p006-4k、sync だけして mount のまま止めた file を host で loader の reader が同じ cksum で読んだ、`build/ws307-test/p006-4k.O52KLU`）。512 の disk では未実施。

## 残り

- 4096 byte の段（spec §5.9・§5.10 の p008 の 4096 の段）: loader の FAT の探索（`volume-discovery.c` の `supported_fat_block_size` と BPB の sector）を kernel の FAT（p006）と同じ契約で 4096 に、4Kn の disk の ESP＋root の UFS（fragment 4096、p005 の残り）で起動。p005・p006 の後。
- `sync` が j3 の journal の内容を home の位置まで書き終えるかの確かめ（spec §5.10 の 6、未確認）: guest で file を書いて `sync` し、host で UFS の home の block を reader で読む試験。p009 の QEMU の試験で行う。
- `video=640x480` の時に loader の失敗の文字が見えない件（上の観察）は Q1 の判断（Bug にするか）。

## 2026-10-29 05:0x p008-4k の起動の結果（P4 の返却の後、Q1 が記録）

- FAIL（build/ws307-test/p008-4k.4KNAxn/boot/login.png、p4-ws307h の worktree）。guest の段は全部 ok（P008K DONE、root に 216 MB）。
- loader は 4Kn の ESP の cfg を見つけ、root の UFS（fragment 4 KiB）から kernel を読んで起動した（loader の 4096 は動いた）。
- kernel は「vfs: scan nvme0n1 geometry error=21: 2 entries」の後「vfs: boot0 selector resolution failed (error 3)」「VFS initialization failed (3)」で止まる。boot0=UUID=4FAE-A0BC（ESP の FAT の serial）の解決が 4096 の disk で失敗。kernel の boot の volume の探索（geometry の scan と FAT の serial の照合）に 512 の前提が残っている見込み。
- 次の起動の最初の作業: src/kern の vfs の boot の探索（「geometry error」を出す所）を 4096 に。

## q1665-i05（2026-10-29、P4、worktree p4-ws307i）

- p008-4k の FAIL の原因（gdbstub で確かめた。`p008-4k.4KNAxn` の disk4k.img の qcow2 の写しを OVMF・4096 の NVMe で起動し、hbreak で `block_identity_get`・`fat_identify`・`ufs_identify`・`parse_bpb` の戻り値を見た）: ESP（nvme0n1p1）で `fat_identify` は 0、`block_identity_get` は EINVAL。512 の前提ではなく、guest の `mkfs -t fat32` の ESP の label が placeholder の "NO NAME" で、`fat_identify` が label の文字を残したまま flag を立てず、`filesystem_identity_valid`（flag の無い field は 0）が拒んでいた。host の tool で作る今の image の ESP は label を持つので出なかった。installer が作る ESP でも同じく起きる。
- 直し（commit f553c82ba、`src/drivers/fs/fat.c`）: placeholder と空白の label は 0 にして flag を立てない（空白の label で flag 付きの空の文字になる誤りも）。vmunix build warning 0。
- 再試験: 下に記録する。
- 再試験（2026-10-29 06:3x、image `build/ws307i-ssh/hdd-image.img` を f553c82ba の kernel で作り直した。`sh plan/tools/guest/build-ssh-image.sh build/ws307i-ssh` rc 0、zedBSD の code の warning 0（OpenSSL・OpenSSH の third-party の warning だけ））:
  - `sh plan/ws307/tests/p008-4k-boot.sh build/ws307i-ssh/hdd-image.img` → **PASS**（`build/ws307-test/p008-4k.we1BKe/boot/login.png`、p4-ws307i の worktree）。4Kn の NVMe の ESP（FAT32、sector 4096）の loader と cfg、root の native UFS（fragment 4 KiB）の kernel から起動し、kernel が `boot0 UUID=8E18-C5F3 -> /dev/nvme0n1p1 (private FAT)`・`rootpart … resolved to /dev/nvme0n1p2` で login prompt まで（画面で確かめた）。
  - 512 の回帰: 同じ image の `plan/tools/boot-test.sh`（uefi-nvme）login prompt PASS（`build/ws307-test/p006-boot512.yjgnrB/boot/login.png`）。
- これで 4096 の段の受け入れ（4Kn の ESP＋root の UFS で起動）を満たした（判定は Q1）。残り: 512 の disk の j3 の sync の確かめ（p009 の QEMU の試験で）、`video=640x480` の観察（Q1 の判断）。

## q1665-i07（2026-10-29、P4、worktree p4-ws307k）: 512 の disk の j3 の sync

- p009 の段 7 の QEMU（`plan/ws307/tests/p009-install.sh`、installer の image から SATA の 512 の disk に導入した後）: live の system で導入した root（native UFS、j3）を mount し、`/boot/p009-sync.new` に 2,867,200 byte を書き、`mv` で `/boot/p009-sync.bin` に rename、`sync`、mount のまま QEMU を止めた（SIGTERM）。host で loader の UFS の reader（`plan/ws307/tests/host/ufs-cat.c`＋`bootloader/common/ufs-read.c`、512 byte の block）が同じ cksum で読んだ（`HOST j3-sync ok`）。2 回（`build/ws307-test/p009.Gdm7dW`・`p009.hCvrjA`）とも ok。spec §5.10 の 6 の「書く→fsync→rename→sync で home の位置まで書かれる」を 512 でも確かめた（4096 は i04 の p006-4k）。
- これで p008 の残りの j3 の確かめは済み（判定は Q1）。`video=640x480` の観察は Q1 の判断のまま。

## q1698-i01（2026-10-30、P4、worktree p4-ws307o、branch agent/p4-ws307o）: 512 の j3 の sync の再確認

- 512 の j3 の sync は q1665-i07 の p009 の試験で確かめたが、その後に UFS の遅さの直し（6387b82a0: mkfs の native が tail journal を書かない、batched journal、cache の包み `ufs_buf_*`）が入ったので、今の tree で確かめ直す。
- 形: p004 の試験 `plan/ws307/tests/p004-gtk.sh` が兼ねる（install-admin で NVMe 512 に導入した root に 2,867,200 byte を書き rename・sync し、mount のまま guest を止め、host の `ufs-cat`（`plan/ws307/tests/host/ufs-cat.c`＋`bootloader/common/ufs-read.c`、512 byte）が同じ cksum で読むか。行 `HOST j3-sync ok|fail`）。script は commit 済み（2aad28963）、**未実行**（image の build の前にラップアップ）。
- 次の 1 手: p004 の phase.md の「次の 1 手」の 1 と 3（installer-image の build と p004-gtk.sh）。p004 を待たずに確かめるなら、同じ tree で `make … ZEDBSD_CONFIG=plan/ws307/tests/config-amd64-p009.mk … installer-image` の後 `sh plan/ws307/tests/p009-install.sh IMAGE`（SATA の 512 で同じ確かめ）。
