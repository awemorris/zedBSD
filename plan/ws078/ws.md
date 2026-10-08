<!-- awesome-plan project=zedbsd record=ws078 -->

# WS078: Kei Operating System への名前の移行

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: p004・p007・p008 は cleared（2026-10-08、表の p004 を直した）。残りは古い p001（planning、Q1 の判定）と p005（規約、ベータ3）。WS の完了を Q1 が判定）
Primary Milestone: MG006
Related Milestones: MG002
Objectives: O1
Parent: [Master](../master.md)
Queue: なし
Resume point: 2026-09-28 の周期の終わり: p002・p003・p006 は cleared、BUG-080 の Desktop の分類、X11 の program と zedinst は `userland/x11/` へ（ユーザー決定）、画面の文字列・Kei の起動画面（p004 の主な部分）も済み。2026-09-29: 見える旧名の残り（wl_output の名前・Vulkan の application 名・usage・xkb の節の名前）は [ws035-p121](../ws035/phase121/phase.md) で済んだ。残り: 注釈の `zdesktop`（約 240 箇所、入力と browser の作業の merge の後に一度に）、試験の log の印（`ZWL`・`ZTERM`・`ZFILES`・`ZBROWSER`、全試験と同時に）、base の getty の hostname の既定・sh の `TERM=zed`、`/usr/libexec/keiland-x11` の見直し、p005（規約と回帰）
<!-- awesome-plan-current:end -->
作業の手引き（2026-10-01）: [guide.md](guide.md)

## 目標（2026-09-28 ユーザーの決定、要旨）

- プロジェクトの名前は **Kei Operating System**（Kei は日本語の「軽い」）。カーネルの内部名は **zedbsd**。
- デスクトップの名前は **Keiland**（Kei + Wayland）。カーネルからデスクトップまで OS として垂直統合しているので、「デスクトップ環境」
  「デスクトップ」とはあえて呼ばない。Keiland は内部名。
- OS の見えるところから zedBSD・zed・z の名前を**徐々に**外す。
- 実行ファイルの改名: zdesktop → `/bin/wayland`、zdesktop-x11server → `/bin/xserver`、zdesktop-browser → `/bin/browser`。
- シンボル名に zedbsd を含めない（これまでの方針）。カーネル・ドライバ・UAPI などに新規の実装で混入したものを一斉に改める。
  望ましい接頭辞は `ZEDBSD_` ではなく **`KERN_`**。
- ロゴなどは K の 1 文字にしない（KDE の商標の侵害のおそれ）。必ず **Kei** の 3 文字にする。

原文は [master.md](../master.md) の決定の行。

## 棚卸し（2026-09-28、main の概算。p001 で詳しくする）

- `zedbsd` を含む識別子は kernel・driver・HAL にはほとんど無い（src/kern の `__ZEDBSD_SIGEV_THREAD_SIGNAL` 等）。
  ヒットの大半は注釈・文字列の "zedBSD"（src/drivers 790 file 等）。
- UAPI・libc: `__ZEDBSD__`（toolchain の target が定義する OS の識別子）、`__ZEDBSD_LEGACY_VISIBLE`・`__ZEDBSD_POSIX_2024_VISIBLE`・
  `__zedbsd_float_bits`・`__zedbsd_atomic_*` 等。
- bootloader: `zbl_uefi_zedbsd_config*`・`ZBL_ZEDBSD_CONFIG_*`。
- make: `ZEDBSD_` の変数が約 1,400（`ZEDBSD_CONFIG`・rootfs の option の `ZEDBSD_GRAPHICAL_BOOT` 等）。
- userland: 1,118 file に 3,456（zdesktop 等の名前、Wayland の protocol の `zed_titlebar_v1`・`zed_glass_v1`・`zed_gpu_buffer_v1` 等）。
  `zwp_`・`zxdg_` は upstream の Wayland の接頭辞であり、改めない。

## 対応表（2026-09-28 ユーザーの回答で決定。p001 で参照の全体を確かめる）

| 今 | 新しい source | 実行ファイル・library |
| --- | --- | --- |
| userland/base/zdesktop | userland/desktop/wayland | /bin/wayland |
| userland/base/zdesktop-x11server | userland/desktop/xserver | /bin/xserver |
| userland/base/zdesktop-browser | userland/desktop/browser | /bin/browser |
| userland/base/zdesktop-terminal | userland/desktop/terminal | /bin/terminal |
| userland/base/zdesktop-files | userland/desktop/files | /bin/files |
| userland/base/zsessiond | userland/desktop/sessiond | /bin/sessiond |
| userland/base/libzdesktop | userland/desktop/libkeiland | libkeiland.so |
| libvulkan・libegl・libglesv2・libwayland・libwayland-egl・libtruetype・mview・egltest・wltest・wlshm・vkdemo | userland/desktop/<同じ名前>（2026-09-28 ユーザー「desktop へ移す」） | 名前は今のまま |

## Phase（案）

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| ws078-p001 | 棚卸しと対応表（識別子・path・見える文字列・protocol 名を分類し、新しい名前を決める。判断が要る点を列挙） | planning | なし |
| ws078-p002 | kernel・driver・UAPI・libc・bootloader の識別子の改名（`KERN_` 等。機械的。build の warning 0 と boot test） | cleared（2026-09-28、main。`__ZEDBSD_*`→`__KERN_*`、`__zedbsd_*`→`__kern_*`、`ZEDBSD_*`→`KERN_*`（header の guard・UAPI の古い名前・試験）、`zbl_uefi_zedbsd_config*`→`zbl_uefi_kern_config*`、`ZBL_ZEDBSD_CONFIG_*`→`ZBL_KERN_CONFIG_*`、`zedbsd_peercred`→`kern_peercred` 等。make の変数・`__ZEDBSD__`・file 名（zedbsd.cfg、bootloader/uefi/zedbsd-config.c）・toolchain の target は残す。amd64 の image と boot test PASS、pcat・rpi4 の build は未実施） | — |
| [ws078-p003](phase003/phase.md) | 実行ファイルと source の directory の改名（userland/desktop/、`/bin/wayland` 等）と参照 | cleared（2026-09-28、6d8ca152。Venus の graphical な確認は未実施） | — |
| ws078-p006 | データの path（/etc/keiland、/usr/share/keiland、font の keiland*.ttf、/usr/libexec/keiland-x11）、API・protocol（`keiland_`・`KEILAND_`・`keiland.h`、`zed_*_v1` → `keiland_*_v1`）の改名 | cleared（2026-09-28、main、35177e46。image の build と boot test PASS。Venus の graphical な確認は未実施） | p003 |
| ws078-p004 | 見える文字列: boot の logo（Kei）・greeter・lock・banner・os-release 等 | cleared（2026-10-08 Q1 判定、T1-397 PASS。p007・p008（make の変数・protocol の header の改名）も cleared（T1-401・403）（2026-10-08 q910 P2 の照合。旧: incomplete（2026-09-28: boot の logo を Kei の印・語・「powered by ze…） | p001 |
| ws078-p005 | 全文規約確認と回帰（必須の最終確認） | planning | 全 Phase |

改名は作業中の agent と衝突しやすい。p002・p003 は他の agent が merge を終えた静かな時点で main か 1 つの agent が一度に行い、
その後に各 agent へ新しい名前を知らせる。

## ユーザーの判断（2026-09-28）

0. 「"Keiland's System Menu"はSystem Menuで十分です。Keilandは内部コードネームです。」→ Keiland（と libkeiland）は内部のコードネームであり、
   画面に出る文字列には使わない（System Menu、File Manager のように機能の名前で呼ぶ）。

1. make の `ZEDBSD_` 変数（`ZEDBSD_CONFIG`・rootfs の option 等）: **今は残す**。
2. `__ZEDBSD__`（toolchain の target `zedbsd` が定義する OS の識別子）: **残す**（カーネルの内部名）。他の `__ZEDBSD_*`・`__zedbsd_*` の補助の識別子は改める。
3. source の directory: **実行ファイルと一緒に改名する**。さらに 2026-09-28 ユーザー:「baseを分離して、userland/desktop/という階層を
   作ってください。そこに userland/desktop/wayland のように置いてください。」→ Keiland の部品は `userland/base` から出して
   `userland/desktop/<name>` に置く（`userland/desktop/wayland` 等）。menuconfig の Desktop の分類（BUG-080）はこの階層と揃える。
4. Keiland の Wayland の protocol と library: 接頭辞は **`keiland_`**（`zed_titlebar_v1` → `keiland_titlebar_v1` 等、`libzdesktop` → `libkeiland`）。
   `zwp_`・`zxdg_` は upstream の名前であり改めない。

## 残り（2026-09-29 main、ws035-p121 の報告から）

ws035-p121 で見える旧名を直した（wl_output の name `DISPLAY-1`・description・make/model `Unknown`、Vulkan の application 名、usage、xkb の節の名前 等）。
main が getty の hostname の既定を `kei` にした（sh の prompt の既定と揃える）。残りは次のとおりで、どれもデモの画面には出ない:

| 項目 | 扱い（main の判断） |
| --- | --- |
| 注釈の `zdesktop`（約 240 箇所） | WS074・WS081 の作業が落ち着いた後、1 つの agent で一斉に置き換える（他の agent と衝突するので今はしない） |
| 試験の log の印 `ZWL `・`ZTERM `・`ZFILES `・`ZBROWSER `（約 340 行）、試験の `/tmp/zdesktop.log` | 内部の印で見えない。改名するなら全試験を同時に直す別の Phase。当面は変えない |
| X の core font の名前 `zed-unicode` | retro（zwm・zterm・zshell・Xzed）の内部の名前。変えない |
| `TERM=zed`（base の sh の既定） | terminfo の名前。変えるなら terminfo の entry と合わせて。低い優先度 |
| 古い p069 の demo（service `zdesktop`、`/etc/keiland/run-zdesktop.sh`） | 今のデモ（WS075 の graphical login）に置き換わった。廃止を検討（使っている試験の確認の後） |
