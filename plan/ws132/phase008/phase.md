<!-- awesome-plan project=zedbsd record=ws132-p008 -->

# ws132-p008: compositor の電源ボタン・蓋の動作

Status: in-progress（2026-10-05 P1 generation17 / q724。蓋の分（ベータ1）を実装し、build と host の試験まで。電源ボタンの S0i3 は WS052 の後。実機の確認まで cleared にしない）
Disposition: normal
Parent: [WS132](../ws.md)
Queue: q724（Q1 の投入、蓋の分を先に）

## 範囲

- **蓋（D2、S0i3 ができるまでの間）**: 蓋を閉じたら画面を消して session を lock する。閉じてから 15 分以内に開けたら password 無しで自動の unlock。画面を消すのは WS113 p013 の `kl_backend_backlight`（無ければ黒の画面）。— この Phase で実装（ベータ1）。
- **電源ボタン（D1）**: 2026-10-08 Q1: WS182 に置き換え（2026-10-07 ユーザー「電源ボタンのハンドリングは…ログオフ、電源オフ、などのメニュー…独立WSにして」）。押下は電源のメニューを開く（ws182-p002、main d66cd3ef7）。この Phase の残りから外す。旧案「短押しは dialog 無しで S0i3」は取り消し。

## 設計（蓋）

- 純粋な状態機械 `userland/desktop/wayland/lid.c`・`.h`（host で試せる）:
  - 閉じる: 画面を消す。login の画面でなく、まだ lock されていない session なら lock し、その lock を「蓋の lock」として閉じた時刻を覚える。既に lock されていた時（利用者の Super+L、無操作の lock）は蓋の lock ではない。
  - 開ける: 画面を点ける。蓋の lock がまだ立っていて、閉じてから 15 分（`ZWL_LID_GRACE_MS`）以内なら password 無しで unlock。15 分を過ぎた時、時計が戻った時は lock の画面のまま。
  - 同じ状態が 2 度来ても何もしない。lock できなかった時（session manager の無い compositor）と、間に password で unlock された時は、蓋の lock を忘れる（その後の別の lock を蓋が外さない）。
- compositor（`backend-host.c` の `zwl_backend_lid_changed`）:
  - lock は画面を消すより先に行い、desktop が見えないようにする（`zwl_lock(server, "lid")`）。
  - 画面を消す: `server->screen_off`（`zwl_glass_draw` が黒だけを描く）。backlight は最初の要る時に `kl_backend_backlight_open` で開き、今の明るさ（読めない・0 なら 100%）を覚えて 0 にする。backlight の無い machine は黒だけ。
  - 点ける: 覚えた明るさに戻し、黒をやめる（`zwl_lid_screen_restore`）。compositor の終わり（`main.c`）でも呼ぶので、蓋を閉じたまま session が終わっても次の利用者に暗い panel を残さない。
  - unlock は `greeter.c` の新しい `zwl_lock_release`（打った文字を消し、途中の答えを無効にし、無操作の時間を始め直す）。password での unlock は `zwl_lid_unlocked` で蓋の lock を忘れる。
- 安全: 自動の unlock は「蓋を閉じたことで立てた lock」だけに限る。利用者が自分で lock した後に蓋を閉じても、開けた時は password が要る。

## 実装（2026-10-05）

| 所 | 内容 |
| --- | --- |
| `userland/desktop/wayland/lid.c`・`.h`（新） | 上の状態機械 |
| `backend-host.c` | `zwl_backend_lid_changed`、`zwl_lid_screen_restore`、`lid_screen_off`。log `ZWL LID screen off|on`・`ZWL LID backlight off saved=N|on percent=N|none error=N` |
| `greeter.c` | `zwl_lock_release`、password の unlock で `zwl_lid_unlocked`。既存の規約の指摘 2 つ（`zwl_lock` の条件の中の呼び出し、閉じ括弧の後の空行）も直した |
| `shell.c` | `zwl_glass_draw` の先頭で `screen_off` なら黒 |
| `main.c` | 終わりに `zwl_lid_screen_restore` と backlight を閉じる |
| `zwl.h`・`Makefile*` | `struct zwl_lid lid`・`screen_off`・`backlight`・`backlight_saved`・`backlight_out`、宣言、`lid.c` の追加 |

## 確認

| 確認 | 結果 |
| --- | --- |
| zedBSD の compositor（`make BUILD=build/q713 build/q713/bin/wayland`） | 成功、warning 0 |
| Linux の Keiland（`keiland-linux.mk all`） | 成功、warning 0 |
| `sh plan/ws132/tests/run-host-lid.sh`（`lid.c` と `backend-host.c` をそのまま compile、lock・時計・backlight は偽物、ASan・UBSan） | 29 checks ok: 状態機械（閉じて lock、2 度目は無し、すぐ開けて unlock、ちょうど 15 分は unlock・1 ms 過ぎは lock のまま、前からの lock は残る、password の unlock の後の lock は残る、login の画面は lock しない、時計が戻ったら lock のまま、lock の失敗）と compositor（閉じる → lock・黒・backlight 0 で 70% を覚える、5 分で開ける → unlock・70% に戻る、16 分 → lock のまま点く、前からの lock は残る、password の unlock の後の lock は残る、login の画面、backlight 無しは黒だけ、2 度閉じても 1 回、lock できない session、終わりで点ける） |
| ws131 `host-power.sh` | 6/6 passed |
| style-check（変えた file） | 指摘 0 |
| `plan/tools/keiland-os-boundary/check.sh` | PASS |
| QEMU | **未実施**: QEMU に蓋が無く、system の事象を入れる inject も無い（Q1 の指示どおり host の試験で）。compositor が起動し他の動作が変わらないことは WS142 の回帰（T1）で見る |
| 実機（5330） | **未実施**。UAT の項目: 蓋を閉じる → 画面が消える（backlight が落ちる）・`ZWL LID backlight off`、5 秒で開ける → password 無しで desktop、閉じたまま 15 分を超えて開ける → lock の画面、Super+L で lock してから閉じて開ける → lock の画面のまま |

## 残り

- 実機の確認（上の UAT の項目）。
- ~~電源ボタンの短押し → S0i3（D1、WS052 の後）~~ → WS182（2026-10-08）。
- 15 分を過ぎたら S0i3 に入るのは WS052 の後に見直す（D2 は S0i3 ができるまでの扱い）。
