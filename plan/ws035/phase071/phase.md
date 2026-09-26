<!-- awesome-plan project=zedbsd record=ws035p071 -->

# ws035-p071: App Home の続き（ページング、起動の animation、閉じる swipe、Tab とホイール）

Phase ID: `ws035-p071`
Parent: [WS035](../ws.md)
Status: cleared（q480-i01、2026-09-26）
Phase disposition: normal
Queue: q480-i01
承認: 2026-09-26 ユーザーの自律実行の指示（ホーム画面を優先）
設計: [app-home-design.md](../app-home-design.md)（ページング、起動、キーボードとマウス）。p069 の「範囲外・残り」から。

## 範囲

1. ページング: 1 ページ 6 列 × 4 行 = 24。検索が無いときだけ（検索中は 1 つの連続した格子）。Home の上の横の drag で 1:1 に
   追従し、離すと 200 ms でページへ（画面幅の 25 % 以上、または速い払いで次・前）。icon の上で押しても動かせば drag（離したとき
   動いていなければ起動）。下中央のドット（1 ページなら出さない）。ページはセッション中保つ。ホイール（縦も横も）と
   PageUp/PageDown でページ送り、矢印の選択がページをまたげばページが追う。
2. 起動の animation: 押した icon が拡大しながら Home と一緒に退き、起動した app の最初の窓（5 秒以内に map）が icon の位置と大きさ
   から自分の位置へ 220 ms で育つ（題名の bar は fade in）。
3. 閉じる: Home の上の右下から左上への drag（左上へ 120 px 以上、対角の成分）で閉じる。
4. キーボード: Tab で次の icon（Shift は無し）。
5. 試験: 30 の app の `/etc/zdesktop/apps.conf` でページング（drag、ドット、ホイール、キー）、起動の animation の log、閉じる drag。

## 受け入れ

1. Venus で 30 の app が 2 ページになり、drag・ホイール・キーでページが替わり（画面と log）、2 ページ目の icon から起動できる。
2. 起動した窓が icon から育つ（log の anim と画面）。右下から左上への drag で閉じる。
3. p069・p070 の試験が通る。build warning 0、home.c・shell.c の style-check は新しい部分 0、legacy は悪くしない。
4. i915 実機は未実施でよい。

## 結果（2026-09-26、q480-i01）

cleared。受け入れ 1〜4 を満たした（2 の animation の途中の frame は Venus では見えない、下記）。

### 実装（`userland/base/zwl/home.c` ほか）

- ページング: 1 ページ 24（6×4）、検索が無いときだけ。ページは画面幅ずつ横に並び、drag で 1:1（10 px 動いてから drag、icon の上で
  押しても）、離すと画面幅の 25 % で次・前、そうでなければ戻る（200 ms、cubic ease-out）。ホイール（`zwl_home_axis`、seat.c から）・
  PageUp/PageDown・選択（矢印・Tab）がページをまたぐとページが追う。下中央のドット。ページは Home を閉じても保つ（開いたときの選択は
  そのページの先頭）。icon の起動は press でなく release（動いていなければ）に。
- 閉じる drag: Home の上で左上へ（dy ≤ -120、dx ≤ -60）。
- Tab: 次の icon（最後から先頭へ）。
- 起動の animation: 起動した icon が Home と共に退きながら 1.3 倍まで育つ。起動から 5 秒以内に最初に map した窓（`zwl_glass_mapped`、
  display.c から）は icon の矩形から自分の矩形へ 220 ms で育ち、題名の bar は fade in（shell.c の anim の種類 `ANIM_LAUNCH`）。
- qmp-pointer.py に wheel-down・wheel-up。

### 検証（QEMU・Venus。i915 実機は未実施）

- `plan/ws035/tests/zdesktop-p071.sh` PASS（build/ws035-p071.log）: 30 の app で 2 ページ（`opened apps=30 pages=2`）、左への drag の
  途中（build/ws035-p071/drag.png、2 つのページが並んで動く）と 2 ページ目（page2.png、ドット）、ホイールの上下、PageUp、Tab でページが
  替わる。2 ページ目の「Second page shm」の click で起動（grow.png: icon が育ちながら Home が退く）、`ZWL GLASS launch surface=8
  from=964,117 to=462,335`、窓（launched.png）。左上への drag で閉じる（closed.png）。
- Venus では launch の animation の frame は t=1.00 だけが描かれた（map の直後の frame が 220 ms より遅い）。途中の frame は実機で
  確かめる（未実施）。
- 回帰: zdesktop-p069 PASS、zdesktop-p070 PASS。build warning 0。style-check: home.c・shell.c・display.c 0、seat.c 3（HEAD と同じ）。
- boot test PASS（build/ws035-p071-boot/login.png）。

### 残り

並べ替え、single-instance、通知 badge、Home 中の仮想デスクトップの切り替え、速度での払い（今は距離だけ）。
