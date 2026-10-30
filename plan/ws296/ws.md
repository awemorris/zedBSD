<!-- awesome-plan project=zedbsd record=ws296 -->

# WS296 src/drivers（gpu/ を除く）の comment から開発の管理情報を消し、XXX の規約の当て直しを済ませる

- Master: [plan/master.md](../master.md)（2026-10-27 00:22 作成）。Primary Milestone: MG001。Related: [WS251](../ws251/ws.md)（tree 全体の削除。drivers の gpu/ は WS251 p002 で済み、drivers の残りはこの WS に移す）、[Guardrail](../guardrail.md)「source の中の開発の管理情報」。
- Status: planned（2026-10-27 00:22 新設）
- 出典: 2026-10-27 00:22 ユーザー「drivers全体で、開発の管理情報が含まれているコードが多く残っています。たとえばusb-bt.cです。usb/はコーディングスタイルへのフィットもXXX:で指摘されています。drivers以下で、gpu/はすでにやったので除いて、開発管理情報をコメントから削除するタスクをWSとして追加してください。」

## 単一の到達目標

src/drivers の gpu/ 以外の C・header の comment（と log の文言）から、開発の管理情報（WS・Phase・Queue・BUG の番号、T1 などの試験の番号、plan/ の path、日付・担当の経緯、試験・運用の話）を消し、内容の説明だけを残す。「XXX: Need coding style fitting.」の付いた file は plan/coding-style.md の全文に合わせて直し、印を外す。動作は変えない。

## 規模（2026-10-27 00:22 Q1 の機械の数え。WS・ws-p・BUG・q・T1・plan/ の語）

- 管理情報: 96 file、約 360 箇所。platform 27・generic 17・acpi 12・usb 11・wifi 7・pci 7・typec 6・i2c 3・fs 3・mmc・isa・gpio 各 1。
- XXX の規約の当て直し: 11 file・約 30,000 行（usb.c 8,734、usb-rtl8822bu.c 8,534、hid-report.c 3,326、usb-cdc-ncm-net.c 2,343、usb-hid.c 2,094、usb-cdc-ecm.c 2,057 ほか）。RTL8822B の .inc はライセンスのため別の file のまま。

## 受け入れ

- gpu/ 以外の src/drivers で、上の語の機械の検査が 0（出典の Value(s) referenced の行など、管理情報でない物は除外の一覧で）。log の文字列を変えたら、それを照合する試験・道具も直すか、AGENTS.md の試験の整理の基準で消す。
- 「XXX: Need coding style fitting.」が 0。
- 変えた file の build の warning 0（amd64 と、その driver を build する platform）。動作の変更が無いことを diff の通読で確かめる。

## Phase と見積もり（LW、±50%）

| Phase | 内容 | 依存 | LW |
| --- | --- | --- | --- |
| p001 | 管理情報の削除: platform・generic・acpi | — | 3 |
| p002 | 管理情報の削除: usb・wifi・pci・typec・i2c・fs・mmc・isa・gpio | — | 3 |
| p003 | XXX の規約の当て直し: usb-uas*・usb-hid-checkpoint・usb-cdc-ncm・usb-cdc-ecm・usb-hid・usb-cdc-ncm-net | p002 | 5 |
| p004 | XXX の規約の当て直し: usb.c・usb-rtl8822bu.c・generic/hid-report.c | p002 | 6 |
| p005 | 機械の検査 0 の確認と build（複数の platform）、規約の全文の見直し | p001〜p004 | 1 |

- 合計 18 LW（実時間で約 4.5〜7.5 時間、担当 1 人）。p001・p002 と p003・p004 は file が分かれるので 2 人で並べられる。

## 実装だけの担当 P6（2026-10-29、q1667）

- 2026-10-29 01:3x ユーザー「P6は設計が決まっているWSについて、テストなしでひたすら実装だけ進めてテスト未完了でincompleteだけど実装はできていてマージされていない状態のWSを増やす。」
- ユーザー「調査しなくてよい、設計ができているか自明なWSについて、ひたすら書いてください。選択したもののうち、調査が必要なものはスキップします。WSごとにサブのサブエージェントを作ってよいです。」（選択: WS223・WS282〜WS284・WS243・WS296）
- 解釈（Q1）: 試験をせず実装だけを P6 の branch に積み、main に merge しない。Status は incomplete のまま（試験は未実施）。調べが要ると分かれば P6 は飛ばして Q1 に返す。

## 引き継ぎ: WS296 の調べ（2026-10-30 P1、q1705-i01。WS009 の phase028 から 2026-10-30 Q1 が移した。WS296 の設計の入力）

- P6（q1667）が WS296 の p001〜p005 を全部実装済み: branch の先頭 `1809873bd`（基点 main `61630bb0b`、src/drivers の 104 file・+14,854/−8,032、`plan/ws296/tests/check-mgmt.sh`・`check-mgmt.exclude`・`config-amd64-test-options.mk` を含む）。未試験・未 merge。P6 の記録では検査 0・XXX 0・12 の config の vmunix が warning 0、usb.c・usb-rtl8822bu.c は `-S` の関数ごとの比較をした。
- 今の main（00396e3da）で P6 の check-mgmt.sh を流すと **413 行**（platform 25・generic 18・usb 12・acpi 12・wifi 8・pci 8・typec 6・i2c 3・fs 3・mmc・isa・gpio 各 1 の file）、XXX の file は 11（31,308 行）。
- 基点の後に main で変わった P6 の file は 10: fs/fat.c・fs/ufs.c・pci/pci-pcat.c・pci/pci.c・platform/pcat/graphics/splash.c・.h・usb/usb-cdc-ecm.c（main 68 行）・usb/usb-cdc-ncm-net.c（**main 1,105 行**）・usb/usb-cdc-ncm.c（261 行）・usb/usb.c（175 行）。他の 94 file は P6 の版をそのまま持ってこられる。
- 組み直しの案（標準廉価実装手順の形）: 各 Phase は main から始め、自分の file 群について P6 の版を `git merge-file`（基点 61630bb0b、main、1809873bd）で取り込み、検査と「動作を変えない」の機械の確かめをする。
  - 道具: P6 の check-mgmt.sh（path を引数で絞れる形に）、XXX の数え、**asm-compare.sh**（基点と変更後の 2 つの tree を同じ config・`ZEDBSD_KERNEL_LTO=none` で vmunix まで build し、対象の .o を llvm-objdump の関数ごとの disassembly と rodata で比べる。差は名前の変わった static 関数・log の文字列・HAL_ASSERT/HAL_FATAL/KERN_FATAL の `__LINE__`（drivers の gpu/ 以外で 18 file・42 か所）だけのはずで、残りを読む）。下書き: `/home/awe/zedBSD-worktrees/p1-ws009r/build/tmp/ws296-draft/asm-compare.sh`（未試験。基点の tree の build/ には llvm・llvm-source・llvm-build・NoctLang・host-noct-state だけを link する案）、file ごとの P6 の差と main の差の数え `files.tsv`（列: path、P6 の変更行、基点から main の変更行）。
  - Phase の群（Sonnet 5.5 high、各 1 回、file が重ならないので並べられる）: (1) 管理情報だけの約 90 file（platform・acpi・wifi・typec・i2c・gpio・isa・mmc・fs・pci・generic の hid-report 以外・usb の小さな物。main で変わった 6 file は merge-file）、(2) HID: generic/hid-report.c・usb-hid.c・usb-hid-checkpoint.c（約 4,200 行の差）、(3) UAS: usb-uas.c・usb-uas-transport.c・usb-uas-disk.c、(4) usb-cdc-ecm.c・usb-cdc-ncm.c、(5) usb-cdc-ncm-net.c（main の変更が大きいので main から当て直し、P6 は参考）、(6) usb.c、(7) usb-rtl8822bu.c、(8) 最後: 全体の検査 0・XXX 0・P6 の 12 の config の build・規約の全文の見直し（Opus を推す）。旧 p001〜p005 は canceled（P6 の branch に置き換え）。
