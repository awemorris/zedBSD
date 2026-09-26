<!-- awesome-plan project=zedbsd record=ws035p071 -->

# ws035-p071: App Home の続き（ページング、起動の animation、閉じる swipe、Tab とホイール）

Phase ID: `ws035-p071`
Parent: [WS035](../ws.md)
Status: in-progress（q480-i01）
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
