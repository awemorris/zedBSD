# ws025-p032 作業記録

## 2026-09-10 現行状態

pending / 未着手（2026-09-10ユーザー指定、現行修正後の対応）。
V13の実機停止現象は未解決。旧QEMU合格・過去の調査は履歴であり、現行対応の着手/完了と扱わない。
今回の確認はソース読取りのみ。詳細は[修正後照合](../post-rollback-review.md)。

Status: completed (q124)。2026-09-09。

## 確認した原因と修正

1. mkswap の package が削除済み src/kern/swap-format.c を参照していた。
   現行 swap.c のヘッダ codec を userland/base/mkswap/swap-codec.c へコピーし、
   巨大なカーネル統合ユニットを userland に link しない構成へ修正した。
2. loop/input/system-device の callback table が使用箇所より後にあり、
   input には静的関数の前方宣言不足もあった。定義を前方宣言の後へ移動し、
   不足宣言を補った。処理本体は変更していない。
3. Xlib.h の #ifndef と #define の名前が一致しておらず多重定義になった。
   ガード名を統一した。
4. curses が空のローカルヘッダを取り込んでいた。実体のある libc/include/curses.h
   をコンパイル・package の公開ヘッダとして使うようにした。
5. 通常ビルド生成後も、PC98 は init 起動後 kern_free のリスト走査で page fault。
   QEMU GDB stub でタスクの stack allocation と下端への書込みを監視し、
   8 KiB の kernel stack が overflow して直前の large-allocation header を
   書き換えることを確認した。i386 SYS_STACK_SIZE を 16 KiB に拡大した。
   amd64 の既存サイズと一致し、i386 AP の既存 16 KiB、boot の 64 KiB とも整合する。

最初に観測した VM の zero-fill は既にヘッダが壊れた後のものだった。
物理アロケータ自体の二重割当てを原因とした修正はしていない。
実際の下端書込みはタイマー割込み内の asm_get_eflags 等で確認した。
診断ログを追加した別の観測では深い exec 読取り経路の末端で下端を超えた。
診断用 entry.c/page.c の変更は元へ戻しており、受け入れは通常ビルドで行う。

保持された呼出し経路:

syscall_dispatch → process_execve/process_exec_file → exec_target_resolve
→ file_content_lease_pread → overlay → file I/O → UFS → disk cache
→ loop → file I/O → FAT → disk cache/physical allocation。

診断時の frame walk では syscall_dispatch 約 1572 bytes、exec_target_resolve
約 2124 bytes に加え、上記の各層と割込み・下位処理の frame が同時に残った。
これは一関数のサイズだけでは見えない call chain のスタック消費である。

## 現時点の証拠

- 通常 PC98 ビルド・overlay/native イメージ生成: PASS。
- [PC98 loader/runtime](../temp/p032-pc98-runtime-2/results.tsv): 16/16 PASS。
  overlay/native/1024-byte-sector LFN、設定欠落・不正設定・不正 kernel・entry/chain 等。
- swap formatter: ordinary と ASan/UBSan それぞれ 49,333 checks PASS、
  maintained Noct image との byte equality PASS。
- [通常 boot の失敗画面](../temp/p032-pc98-runtime-1/overlay-screen.log)。
- 診断ログは ../temp/p032-watch/。guard-watch.log は stack 下端監視と frame walk、
  trace-header-watch.log はタイマー割込み中の下端書込みを記録する。
- 対話・永続化 runner: ../tests/run-p032-pc98-session.py。最終結果は下記。
  初期 runner の準備ミス（存在しない /etc/hostname と /bin/true の選択）は修正した。
  初期試行は合格に数えない。

## 最終受け入れ

- R01: PC98 通常 overlay/native image build PASS。
- R02–R06/R08: overlay と native UFS の各 disposable image を二度起動し、
  root ログイン、各 boot 12 回の外部 uname 実行、ファイルコピー、cksum 一致、
  再起動後の内容維持、kern_platform_halt 内の HLT 到達を確認した。
  [overlay](../temp/p032-overlay-session-5/results.json)、
  [native](../temp/p032-native-session-1/results.json)。原本ハッシュ不変。
- R07: 上記 loader 16/16 PASS。
- R09: PCAT と amd64 の make -j16 通常 build PASS。
  [PCAT 起動](../temp/p032-x86-smoke/results.json)、
  [amd64 UEFI/xHCI 起動](../temp/p032-amd64-smoke/results.json) とも login 到達。
  最初の共通 smoke は amd64 image 名を誤指定して中断したため、正しい
  hdd-image.img で amd64 だけ再実行した。起動障害ではなかった。

PCAT/amd64 の build では USB/PCI/ISA の operation table が参照より後にある
不整合も修正した。callback/helper 宣言を補い、NVMe controller に埋め込む
lifecycle struct 定義を controller より前へ移動した。DMA helper も前方宣言を補った。
処理本体の意味は変更していない。
最終 build/formatter log は ../temp/p032-final-logs/ に保存した。

これは stack の無制限な消費を防止する guard page の実装ではない。
実際に破壊を起こした経路と割込みの余裕を回復した修正である。
初期 fixture の存在しないコマンド実行時にシェルの $? が 0 と見えた点は
本 Phase では未修正。受け入れは戻り値だけでなく、内容と halt を検証している。
PC98 実機試験および未採用 p027–p030 は本結果に含めない。
