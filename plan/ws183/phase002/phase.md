<!-- awesome-plan project=zedbsd record=ws183-p002 -->
# ws183-p002: 1 本指のタップのクリックの遅れ

Status: in-progress（q863、P2、2026-10-08: 実装・build・host 試験まで。実機（5320・5330）の確認の依頼を Q1 へ）
Disposition: normal
Parent: [WS183](../ws.md)
Queue: q863（ユーザー 2026-10-08「起動して作業開始してください」、P2）

## 発端（2026-10-08 ユーザーの UAT）

「タップ判定が150msくらいかかってる気がします。これは調整可能なんですか？」

## 原因

`userland/desktop/wayland/touchpad.c` の 1 本指の tap は、指を離した時に左の press を送り、release は TAP_DRAG_MS（300 ms）の後
（`kwl_touchpad_tick` → `tap_finish`）に送っていた。次の touch が 300 ms の内に来たら button を押したまま drag にするため。
client の大半は click を release で受ける（button の動作、window の focus の後の動作）ので、tap の click は離してから 300 ms 遅れて完了する。
libinput の tap の state machine（TAPPED の間 press を保ち、timeout で release）と同じ形で、その timeout が長い分だけ遅く感じる。

## 修正

- 1 本指の tap は離した時に press・release を続けて送る（click がその場で完了する）。state は PENDING（TAP_DRAG_MS まで、次の touch を待つ。button は持たない）。
- PENDING の内の 1 本指の touch は SECOND。button を持たず、その間の pointer の動きは pixel にして溜める（送らない）。
  - 指が DRAG_START_UM（1 mm）動いたら drag: 左を press し、溜めた動きを送り、以後は動きを送る。離した時に release。press は最初の tap の位置で起きる（以前と同じ所から drag が始まる）。
  - 速く離したら（tap）2 回目の click（press・release）: double click。溜めた動きは捨てる（pointer は 2 回の click の間で動かない）。
  - 動かずに長く置いて離したら何もしない（以前は 1 回目の click の release が出るだけだった。今は 1 回目は完了済み）。
- SECOND の間に pad を押し込んだら、その touch は押し込み（tap の続きでない）。
- `kwl_touchpad_tick` は PENDING の期限を過ぎた state を戻すだけ（button の動作は出さない）。`kwl_touchpad_release_all` は DRAG の時だけ左を release。
- 2 本・3 本の tap、押し込み、scroll、gesture は変えない。TAP_MS（180）・TAP_DRAG_MS（300）・DRAG_START_UM は変えない。

## 実装（2026-10-08、P2）

- `userland/desktop/wayland/touchpad.c`: `touch_end` の 1 本指の tap は press・release を続けて送り PENDING。SECOND の `touch_end` は release を送らず、tap なら 2 回目の click。
  `pointer_motion` は SECOND の間は pixel を `tap_held_x`・`tap_held_y` に溜める。`take_motion` は SECOND で DRAG_START_UM を越えたら `tap_drag_begin`（press → 溜めた動き → DRAG）。
  `tap_finish` は PENDING を NONE に戻すだけ（button を出さない）。`take_button` の押し込みは SECOND を NONE に。`kwl_touchpad_release_all` は DRAG の時だけ release。
  `touch_begin` は action を出さなくなったので引数 `actions` を外した。file の先頭の説明を直した。
- `userland/desktop/wayland/touchpad.h`: tap の state の説明、`tap_held_x`・`tap_held_y`。
- `plan/ws159/tests/host-touchpad.c`: case 2（tap の click は離した時に完了、その後 400 ms 何も無い）・case 3（tap の click → drag の press が動きより先、release は 2 回）を新しい挙動に直し、
  case 10（tap の後の長く動かない touch は click を増やさず pointer も動かさない）・11（SECOND の間の release_all は何も出さない）・12（TAP_DRAG_MS の後の touch は drag でない）を追加。

## 確認

- `sh plan/ws159/tests/run-host-touchpad.sh build/tmp/ws183p002.*/t`: `host-touchpad: ok (32 checks)`。`EXTRA_CFLAGS=-fsanitize=address,undefined` でも ok（32）。
- `sh plan/ws142/tests/run-host-gesture.sh …/g`: `host-gesture: ok (82 checks)`。
- `make -j16 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/ws181 build/ws181/bin/wayland`: rc 0、warning 0（touchpad.c を compile し直した）。
- `make -j16 -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws181-linux all`: rc 0、warning 0。
- `python3 plan/tools/style-check.py`（touchpad.c・touchpad.h・host-touchpad.c）: 0。`git diff --check`: 0。

## 確認の計画（未実施）

- host: `plan/ws159/tests/run-host-touchpad.sh`（tap は離した時に press・release、drag は press の前に pointer が動かない、
  double click、tap の後の長い touch で click が増えない、SECOND の間の release_all は何も出さない）、`plan/ws142/tests/run-host-gesture.sh`。
- build: compositor（wayland）の build、warning 0。
- 実機（5320・5330、ユーザーの手）: tap の click の反応、double click、tap-drag（window の移動、text の選択）。QEMU には touchpad が無い（未実施）。
