<!-- awesome-plan project=zedbsd record=ws187-p004 -->
# ws187-p004: 規約の全文の見直し

Status: cleared（2026-10-09 Q1 の判定: P3（Sonnet medium）の d148052da、host 試験 lock-clock 74・lock-swipe 45 ok（ASan・UBSan）、zedBSD・Linux の build 0、style-check 0）
Disposition: normal
Parent: [WS187](../ws.md)

## 範囲

WS187 の Phase の commit（77fe44dc8・dfeea795f・880ce4cbf・da62a2f0d）が変えた C を plan/coding-style.md の全文と照らして読み、書き方だけを直す。

## 見た file

- 新規: lock-clock.c/.h、lock-swipe.c/.h、試験 plan/ws187/tests/host-lock-clock.c・host-lock-swipe.c（全文）。
- WS187 の行: greeter.c（時計、lock の状態、swipe、方式の選択）、glass.c/.h（大きな数字）、input.c（lock_pad_gesture と SCROLL/GESTURE の分岐）、seat.c、sleep.c、kwl.h。

## 直した規則

- if ごとの purpose comment と空行（lock-clock.c、greeter.c の key・tick・layout_styles・style_choose、seat.c の 2 か所）。
- return の前の空行と comment（lock-swipe.c の press・wheel・pad・pad_gesture、release の 2 段落化）。
- 引数の中の入れ子の呼び出しを名前の付いた変数に（sleep.c の kwl_sleep_via_name、greeter.c の kl_tr、試験 host-lock-swipe.c の約 40 の check）。
- 宣言群の途中の空行（kwl_greeter_key）。
- public 関数の comment を複数行に（glass_large_text_width・glass_draw_large_text）。
- 入れ子の for を持つ large_fill を large_put に分け、各 loop に comment、static bitmap に理由の comment。
- 古い comment の訂正（LOCK_GRACE_SECONDS は「提案」でなくユーザーの決定）。

## 直さなかった物

- sleep.c:491 の printf の引数の kwl_sleep_via_name（他 WS の既存、WS の所有者の見直しで扱う）。greeter.c の他の kl_tr の引数の入れ子（login 画面の既存の行）。

## 確認（2026-10-09）

- `sh plan/ws187/tests/run-host-lock-clock.sh build/p3-ws187`: ok（74）。`run-host-lock-swipe.sh`: ok（45）、ASan・UBSan でも ok。
- `make -j16 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/p3-ws187 build/p3-ws187/bin/wayland`: rc 0、warning 0。`make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/p3-ws187-linux all`: rc 0、warning 0。
- style-check（変えた file と試験）: 0。`git diff --check`: 0。tr.py check: 167 entries、0 problems。
- 未実施: QEMU、FreeBSD の build。
