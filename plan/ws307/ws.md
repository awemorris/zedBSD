<!-- awesome-plan project=zedbsd record=ws307 -->

# WS307 Keiland のアプリとしての zedBSD のグラフィカルなインストーラ

- Status: completed（2026-10-31、q1736 P3 の試験の後、Q1 の判定で確定）
- Primary Milestone: MG003。Related: MG006・MG007。Master: [plan/master.md](../master.md)。
- 仕様: [spec.md](spec.md)（U1〜U29・J1〜J5 の決定を反映した確定版。§5.8 は p007 の改訂 5 の形）。
- 前身: [WS119](../ws119/ws.md)（インストーラの作り直し、この WS の新設で canceled）。参考: [WS028](../ws028/ws.md)（canceled）、[WS019](../ws019/ws.md)・[WS062](../ws062/ws.md)（disk image の構成）。

## 目標

UEFI＋ACPI の PC 向けに、Keiland のアプリとして動くウィザード形式のグラフィカルなインストーラを作る。対応する disk（NVMe・USB Mass Storage・SATA など）に、disk 全体か選んだ partition へ zedBSD を入れる。入れる物は base（header 込み）・Keiland Desktop・Firmware・Packages と依存。base・desktop・firmware は tarball で `/opt/installer` の下、packages は package ごとの tarball。

## ユーザーの決定（要点）

2026-10-28 にユーザーから希望の仕様（画面 S1〜S11 の流れ、入れる項目、tarball の置き方）を受け、P4 が spec にまとめ、確認事項 U1〜U29・J1〜J5 にユーザーが答えた（問いと答えの一覧は [spec.md](spec.md) §15・§17）。主な物:

- installer の image は専用の `make installer-image`（全 package を build、menuconfig は kernel の config だけ）。その image で起動すると desktop で installer が自動で開く。image の形は ESP（loader）＋BOOT の FAT32（kernel・cfg）＋root＋swap。
- partition の編集はその場で disk に書く。作った partition は zedBSD 独自の「未割り当て」の型。GPT の disk だけを「partition を選ぶ」で扱う。disk 全体の時は ESP 512 MiB＋swap＋root。
- 導入先の kernel と logo は root の UFS（`/boot`）、ESP は loader と `zedbsd.cfg`。loader が UFS から kernel を読む（Phase を足す指示）。
- 4096 byte の logical sector の disk に対応する。
- `\EFI\BOOT\BOOTX64.EFI` が無ければそこへ、あれば `\EFI\ZEDBSD\BOOTX64.EFI` へ置き、UEFI の Boot####（名前 zedBSD、BootOrder の末尾）を作る所まで WS に含める。
- firmware の変数は kernel の driver が UEFI の runtime services で読み書きし、driver は HAL を知らず kernel の口だけを使う。値は byte の列。amd64 の `hal_space_map` が firmware の窓と実行可を受けるのは API の変更でなく実装の補完（2026-10-30）。
- setuid の helper（install-admin）を installer が自分で起動する。installer の image では helper は password を聞かない。installer の image を運用に使ってはいけない規則とし、その image での起動は root 起動相当と扱う（J5）。
- OS 名「Kei」を削除して zedBSD に統一する（tree 全体、起動の logo を含む）。Keiland・libkeiland の名前は残す。既定の利用者は user/user、hostname は zedbsd。
- 画面の lock と sleep の既定は never（2026-10-29・30）。
- 実機への導入は release の UAT で行い、WS の完了の条件に入れない（p012 canceled、2026-10-30）。

## 結果

- installer の app（`userland/desktop/installer/`、S1〜S11・S7a、ja の翻訳、自動起動）と setuid の helper `install-admin`（`userland/desktop/installer/admin/`）、`make installer-image`、unit の tarball と index、base の道具（`diskpart` の batch、`mkfs` の FAT32・UFS native、`mkswap` の partition、4096 byte の sector）、kernel と loader の 4096 byte の sector と loader の UFS からの kernel の読み込み、OS 名の zedBSD への統一、package の header と `.pc`（導入先で GTK4 の app を build できる）、画面の lock と sleep の既定 never。
- UEFI の boot entry（p007）: loader の handoff v8 と `SetVirtualAddressMap`、amd64 の HAL の名前と firmware の窓、`kern_firmware_map`、kernel の driver `src/drivers/platform/pcat/uefi-variables.c`、`/dev/system` の firmware の変数の ioctl、`efiboot`（`list`・`create`・`delete`・`order`・`next`・`prune`）。install-admin は導入の最後に `efiboot create -l zedBSD` で Boot#### を作り、続けて `efiboot prune -l zedBSD` で、名前が zedBSD で指す GPT の partition がどの disk にも無い古い entry を消す（spec §5.8。読めない disk があれば何も消さない）。
- 確かめ（QEMU、KVM、OVMF の書ける VARS の写し）:
  - 導入: `p011-install.py`（GUI の通し、25 STEP、日本語の画面も）、`p009-install.sh`（NVMe・SATA・USB・4096 の導入と導入先だけの起動）、`p009-edit-cancel.sh` が PASS（2026-10-29〜30）。
  - boot entry: `p007-efiboot.sh`（作成・読み戻し・再起動の後も残る・その entry で起動・削除、古い loader と SeaBIOS で runtime 無しのまま起動）PASS（2026-10-30、P6）。`p007-install-entry.sh` PASS（2026-10-31、P3、`/tmp/p3-seq/p007i.F85ktu`、STEP 29 ok・fail 0）: この tree の `make installer-image`（`config-amd64-p009.mk`）を USB で起動し、NVMe に古い GPT と entry（名前 zedBSD の OLD、名前 Other の OTHER、live の disk の ESP を指す zedBSD の KEEP）を作ってから install-admin で disk 全体を導入 → `STATE boot-entry 0006`、Boot0006 zedBSD `HD(1,GPT,新しい ESP の PARTUUID,0x800,0x100000)/File(\EFI\BOOT\BOOTX64.EFI)` が BootOrder の末尾、OLD は一覧と BootOrder から消え、OTHER と KEEP は残った。BootNext 0006 で導入先だけを起動し BootCurrent 0006、BootNext は使われて消え、`efiboot prune` は Boot0006 を残した。
  - build: efiboot と installer の image（install-admin を含む）の build で warning 0（2026-10-31）。CI の config の 14 port の kernel の build warning 0（2026-10-30、p007）。
- 今後も使う試験は [plan/tools/installer/](../tools/installer/) に移した（登録は plan/tools.md）。

## 制限・移管

- 実機への導入と実機の firmware での runtime の呼び出しは未確認（release の UAT）。
- push の後の CI の 1 回（p002）は未確認（push はユーザーの指示の時）。
- efiboot は amd64 の config にだけ入る。runtime の範囲が 64 MiB の窓や 8 KiB の記述子の写しに入らない firmware では Boot#### を作れない（起動はする）。
- QEMU の OVMF は、QEMU の boot order で起動する時に、無くなった device を指す entry を自分で消す（`p007-install-entry.sh` の 2 回目の起動で OTHER・KEEP が消えた）。prune が「他の disk の partition を指す entry を残す」事は 1 回目の起動（live の disk がある間）で確かめた。
- install-admin は efiboot の出力の番号を `sscanf` の `%[`（範囲）で読んでいたが、libc の scanf が範囲を扱わず `000` になった（2026-10-31）。install-admin は sscanf を使わない形に直した。libc の側は [BUG-344](../bugs/BUG-344.md) の直しに含まれる（Q1）。
- `p007-efiboot.sh` は efiboot の `delete` の分け直し（`entry_delete`）の後に流し直していない（`prune` の消す道は `p007-install-entry.sh` で通った）。
- p004 の C++ は `lang/clang` の `clang++.cfg` で通している。本来の形は F-WS307-clang-libcxx-driver。
- loader（p008）: cfg に `video=640x480` がある時、root の partition の失敗の文字が画面に出ない（debug port には出る）。導入先の cfg は `video=` を持たない。原因は未確定。
- native UFS の 4096 の書き込みの遅さと swap file の 0 書きの遅さは [BUG-355](../bugs/BUG-355.md)・[BUG-358](../bugs/BUG-358.md) で直した。

## Phase

| Phase | 内容 | 結果 |
| --- | --- | --- |
| p001 | 仕様の確定（調査、spec.md、確認事項、design-reviewer 3 回） | cleared（2026-10-28） |
| p002 | OS 名「Kei」を zedBSD に統一（logo・mark・既定の利用者と hostname・nightly の asset の名前と取得の道） | cleared（2026-10-30。push の後の CI は未確認） |
| p003 | `make installer-image`、manifest・unit・index・live の tree・image の形 | cleared（2026-10-29） |
| p004 | package の header と `.pc`、導入先で GTK4 の app を build | cleared（2026-10-30） |
| p005 | base の道具（mkswap の partition・diskpart の batch・mkfs の FAT32 と UFS・4096） | cleared（2026-10-29） |
| p006 | kernel の 4096 byte の sector（FAT・UFS・swap） | cleared（2026-10-29） |
| p007 | UEFI の runtime services と Boot####、efiboot、install-admin の entry と古い entry の掃除 | cleared（2026-10-31、q1736 P3 の試験の後、Q1 の判定） |
| p008 | loader が UFS から kernel を読む（512・4096） | cleared（2026-10-30） |
| p009 | install-admin | cleared（2026-10-29） |
| p010 | installer の app | cleared（2026-10-29） |
| p011 | 通しの試験（GUI の導入、導入先の起動） | cleared（2026-10-30） |
| p012 | 実機の導入 | canceled（release の UAT へ） |
| p013 | 文書（how-to と reference） | cleared（2026-10-29） |
| p014 | 規約の全文の見直し（WS307 が変えた C。q1736 の efiboot・install-admin の差分も P3 が規約で見直した） | cleared（2026-10-31、Q1 の判定） |
| p015 | 画面の lock と sleep の既定 never | cleared（2026-10-30） |

Phase の directory、design-review3.md、proposed/、この WS だけの試験は完了の時に消した（git の履歴に残る）。
