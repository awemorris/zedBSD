<!-- awesome-plan project=zedbsd record=ws156-p003 -->

# ws156-p003: popup の描画と動き、× と click、全画面・lock、system の通知

Phase ID: `ws156-p003`
Parent: [WS156](../ws.md)
Status: cleared（2026-10-08 Q1: T1-375b QEMU PASS、20 項目 ok、成果物は T1 の worktree build/t1-375b/OUT/）
Phase disposition: normal

## 範囲（Q1 の ACK 2026-10-08）

[p001](../phase001/phase.md) の §3（popup の大きさ・動き・続けて来た時・× と click・全画面と lock）、§4.4（URGENT）、§6（system の通知: 媒体（H7、bar の icon を置き換え）・Wi-Fi の接続の失敗・電池 10%・5%）。sleep の中止の通知は WS052 がベータ3 へ移ったので入れない（Q1）。log の画面と Super+N は p004。

## 実装

- `userland/desktop/wayland/notify-flow.[ch]`（新、純粋）: 入る 280 ms（ease-out `1-(1-t)^3`、右の外 → 中央、α 0→1）、とどまる 3000 ms（待ちがあれば 1500 ms、pointer が上の間は数えない、URGENT は触るまで最大 30 秒）、出る 280 ms（ease-in `t^3`、中央 → 左の外、α 1→0）。出始めで板は済み（log へ）、次がすぐ入る（2 枚の板が流れる）。
- `notify-popup.c`（新）: 下の中央（下端から 48 px 上）、幅は画面の 20%（320〜640 px）×76、影と白い glass（α .86、角 12）、左に icon（System は Kei の mark、app は window の app_id か名前の picture、無ければ accent の tile に頭文字）、app 名（薄く）と題 1 行、本文 2 行（空白で折り、2 行目は…で切る）、右上に ×。× の click で dismiss（log に残さない、closed DISMISSED）、ACTION の本文の click で activated を送って消す（System の通知は notify-system.c の command を起動）。lock 中は全て、全画面の window が上にある時は URGENT 以外を、popup に出さず log へ（`skip`）。client が withdraw した通知の板はすぐ消す。log の行 `KWL NOTIFY show|hide|gone|skip|dismiss|activate`。
- `notify-system.c`（新）: compositor の通知の action の表（`kwl_notify_system_post(server, title, body, flags, command)`、click で `kwl_spawn`）、電池の警告（`kwl_notify_battery`: 電池で放電中に 10% と 5% で URGENT を 1 回ずつ、充電・AC・mark より上で再び出せる）。
- `notify-shell.c`: `kwl_notify_hide_shown`・`kwl_notify_dismiss_id`・`kwl_notify_activate`（activated の event）。
- `media.c`・`media.h`: 新しい媒体（未 mount）は「USB drive connected」＋label の System の通知（click で `files --devices`）。bar の媒体の icon は無くした（H7。`kwl_media_width` は 0、描画と button は何もしない）。
- `network.c`: join の失敗（key の失敗を含む、key を聞く場合を除く）で「Could not join NAME」＋menu の文の System の通知（menu の文は今のまま）。
- `backend-host.c`: `kwl_power_read` の後に `kwl_notify_battery`。
- 差し込み: `shell.c`（tick・draw（corner の後、keyboard の前）・button の鎖（power dialog の直後）・`kwl_glass_still`・`kwl_glass_overlay`）、`kwl.h`・`glass.h` の宣言、Makefile 3 本。`plan/ws131/tests/host-system.c` に stub 1 つ。
- 翻訳: `userland/desktop/locale/ja/wayland.tr` に 5 行（USB drive connected・Could not join {1}・Battery low・Battery critically low・{1}% of the battery is left）。

## 確認

| コマンド | 結果 |
| --- | --- |
| `sh plan/ws156/tests/run-host-notify-flow.sh`（新） | 32 checks 0 failures（plain・ASan/UBSan）: 段の時間、入る・出る途中の位置と α、待ちがある時の 1.5 秒と次の即時、hover で止まり離れて続く、URGENT の 30 秒と touch の後の 3 秒、remove、model と合わせて log の順と dismiss の不在 |
| `sh plan/ws156/tests/run-host-notify-model.sh` | PASS |
| `sh plan/ws131/tests/host-system.sh` | PASS |
| zedBSD `make … build/p2-k/bin/wayland`、Linux `make -f userland/desktop/keiland-linux.mk … all` | warning 0 |
| `python3 plan/tools/style-check.py`（新しい file と変えた file） | 新しい違反 0（network.c の既存の 5 件は不変） |
| `python3 tools/i18n/tr.py check userland/desktop/locale/ja/wayland.tr userland/desktop/wayland userland/desktop/locale/wayland.keys` | 145 entries 145 translated 0 problems |

未実施: QEMU（板の入る・とどまる・出る の screenshot、×、ACTION、全画面・lock の skip、媒体の挿入の通知と click で Files、Wi-Fi の失敗）は p005 で T1 にまとめて依頼。実機の見た目と速さは UAT。

## 残り・移管

- `plan/ws132/tests/p005-guest.sh` は bar の媒体の icon（`KWL MEDIA icon`・click で `KWL MEDIA files`）を確かめる試験で、icon を無くしたので合わない。ws132-p005 は cleared で master の試験の一覧に無い → 試験の整理の基準で削除を Q1 に依頼（媒体の通知の確かめは WS156 p005 の QEMU の試験に入れる）。
- p004（log の画面、Super+N、ring、すべて消去、log の ×）。
