<!-- awesome-plan project=zedbsd record=ws353-p018 -->

# ws353-p018: 入力の device の hotplug を sessiond に開かせる（100 ms の settle の scan を消す）

Status: planned
Parent: [WS353](../ws.md)

## 目的

2026-11-14 のユーザーの決定（ws.md「ユーザーの決定（続き）」）の形にする:

- compositor は device 全般の hotplug を見張る thread を 1 本持つ（監視の対象は入力の device だけではない）。
- sessiond 経由で compositor が動いている時: compositor の hotplug の thread が /dev/system を見張り、入力の device の追加を見たら sessiond に開く事を要求する。sessiond が開いて fd を compositor に渡す（unix socket の SCM_RIGHTS。POSIX の中核の機能だけ、kernel に機能を足さない）。
- sessiond 経由で起動していない時: hotplug の thread が入力の device を自分で open する。
- これで chown の追い越しを待つ 100 ms の settle の scan（`KWL_INPUT_SETTLE_*`、BUG-264）を消す。session の切り替え・Log Out の時に fd を取り上げる（revoke）形も設計に含める（Linux の backend の logind の TakeDevice の形が参考）。

## 範囲

1. 今の形の調べ: sessiond（`userland/desktop/sessiond/` の seat.c・session.c・greeter.c、control の protocol）、backend（`libkeiland-backend-zedbsd/` の events・session）、compositor（input・io-device・backend-host.c・main.c の settle）、p005 の申し送り（(A) の見込み）。
2. 設計（この phase.md に追記）: thread の構成（今の io-device と新しい hotplug の thread の関係）、sessiond との protocol（要求・答え・fd の受け渡し・取り上げ）、sessiond 無しの時の道、誤りの時の振る舞い。
3. 実装と試験: host の試験、QEMU（BUG-264 の hotplug-guest.sh を今の形に直すか同等の物で、login の後に挿した USB の keyboard・mouse がすぐ使える事、settle の scan が無い事、idle の起床が増えない事）。
4. HAL・Makefile・mk/・toolchain は変えない。

## 受け入れ

- `KWL_INPUT_SETTLE_*` と `input_settle_until` が無い。
- sessiond 経由で、挿した入力の device が 1 回の要求で使える（再試行無し）。sessiond 無しでも使える。
- 設計の対応表の 2-03 を「消えた」に直す。
