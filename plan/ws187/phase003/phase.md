<!-- awesome-plan project=zedbsd record=ws187-p003 -->
# ws187-p003: lock の画面の認証の方式の選択（Password・PIN・Security Key）

Status: cleared（2026-10-08 Q1 判定、T1-379 QEMU PASS。実機の UAT は未実施）（旧: test-wait（T1 への依頼を 2026-10-08 Q1 へ。番号は Q1 が付ける）。in-progress（q864 の続き、P2、2026-10-08: 実装・build まで。QEMU は T1 へ依頼））
Disposition: normal
Parent: [WS187](../ws.md)
Queue: q864（Q1 2026-10-08「続けて p002・p003 も q864 の続きとして承認済み」）

## 範囲

- 猶予の外では、スワイプ（p002）の後に card を出す。card の field の下で、Password・PIN・Hardware Key（Security Key）を選べるようにする。
- 登録の無い方式は出さない。
  - sessiond の STYLES（/sbin/passkey）は、PIN を「登録があり使える時」だけ返し、fido2 を「key が登録済みで使える時」だけ返す（`userland/base/passkey/main.c`）。それをそのまま出す。
  - Q1 2026-10-08:「案のとおりで OK です。sessiond が返した方式だけを出してください（Hardware Key を無条件に隠す私の指示は事実の誤りだったので取り消します）」。
- PIN は ws172-p002 の口（UNLOCK の style）を使い、key は ws172-p003 の口を使う。どちらも既存の `kl_backend_session_unlock`。
- 時計（p001）と重ならないこと。選択は card の中の link の行に置くので、card の高さは変わらず、時計の配置も変わらない。

## 実装（2026-10-08、P2）

- `userland/desktop/wayland/greeter.c`:
  - `struct greeter_layout` に `styles[3][4]`・`style_bits`・`style_count` を足した。
  - `greeter_layout_styles`: ロック中で password 以外も出せる時は、link の行を方式の数で等分する。順は Password・PIN・Security Key。
  - `greeter_draw_styles`: field が使う方式を明るい pill で描き、pointer の下は淡い pill、他は名前だけを薄く描く。
  - `greeter_hit` に `GREETER_HIT_STYLE` を足した。押すと `greeter_style_choose` で方式を変え、打った物を消す。
  - ロック中の Tab は、次の方式へ移る（`greeter_style_switch`）。
  - login の画面は今の link のまま（変えない）。
  - log: `KWL GREETER style=… via=choice`、`KWL GREETER style-at style=… x= y= width= height=`（styles を受けた時、試験の pointer 用）。
- `userland/desktop/locale/ja/wayland.tr`: 「Security Key」→「セキュリティキー」（Password・PIN は既存の訳）。
- AAT: `tests/scenarios/desktop/lock/swipe-card.md`・`wheel-card.md`、`plan/tools/aat/scenarios/helpers_desktop.py` の `desktop.lock.swipe-card`・`desktop.lock.wheel-card`。
  - 既存の `desktop.lock.lock-unlock` は、最初の key が card を出して field に入るので、変えずに通る見込み。

## 確認（2026-10-08）

- zedBSD の compositor（`make -j16 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/ws181 build/ws181/bin/wayland`）と keiland-linux（`make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws181-linux all`）: どちらも rc 0、warning 0。
- `python3 plan/tools/style-check.py userland/desktop/wayland/greeter.c`: 0。`tr.py check`: 149 entries、0 problems。`plan/tools/aat/check-scenarios.py`: PASS（106）。
- 方式の選択の配置は server に依存するので、host 試験は作っていない。QEMU の PNG で確かめる。
- 未実施（T1 へ依頼）: AAT の `desktop.lock.swipe-card`・`desktop.lock.wheel-card`・`desktop.lock.lock-unlock`、縦長と横長の PNG。PIN の登録がある利用者の選択の表示（ws172 の `passkey-p002-guest.sh` の PIN の登録の手順を使う）。
- 未実施（実機）: Security Key（FIDO2 の key が要る）。


## Q1 の判定（2026-10-08）

T1-379 PASS（QEMU、AAT: swipe-card・wheel-card・lock-unlock・lock-japanese、縦長 1080x1920 も）。PNG を Q1 が目視: 縦長・横長とも時計が中央より上、下に案内、card は時計と重ならない（build/review/ws187/）。猶予の内の解除は host 試験、touchpad の 2 本指・Security Key・PIN の pill は実機の UAT（未実施）。
