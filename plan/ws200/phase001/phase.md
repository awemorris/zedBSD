<!-- awesome-plan project=zedbsd record=ws200-p001 -->
# ws200-p001: Change Password のウィザードと Sign-in Methods

Status: planned（2026-10-10 Q1。WS199 p002 の後、使用量に余裕があれば 10/14 の前、無ければ 10/14 以降）
Parent: [WS200](../ws.md)

## ゴール
- Users の頁の「Change Password」の button → popup のウィザード: 今の password → 新しい password を 2 回 → Done。処理中は操作できない表示。弱い password・不一致・今の password の誤りを 1 行で。
- Users の頁の「Sign-in Methods」: Password・PIN・Security Key の checkbox。外した方式は greeter・lock に出さず、出されても受けない。全部は外せない（最後の 1 つは灰色）。PIN・Security Key は登録が無ければ灰色。console・su・sudo・SSH は password のまま（変えない）。変更は password で確かめる。

## すること・やり方
1. Change Password: 今ある自分の password の変更の経路（Settings → compositor → backend → `passwd -s`、docs/architecture/security.md）を使う。popup は WS199 の `userland/desktop/settings/dialog.c`。
2. Sign-in Methods: WS199 p002 の options の行の `methods=` を使う（`set-options` で methods だけ変え、key-pin・key-touch は読んだまま書き戻す）。sessiond の styles の答え（greeter・lock が問う方式の一覧）を methods で絞り、AUTH・UNLOCK でも methods に無い style を `style-off` 等で拒む。
3. Settings: page-users.c に 2 つの button と popup。security.md に methods の規則。

## 確かめ
- host 試験（passkey の methods の読み書き、sessiond の styles の絞りと拒否、Settings のウィザードの遷移）。build warning 0。
- T1 の AAT（WS199 p004 の依頼にまとめてよい）: password の変更、methods で greeter・lock の pill が変わる。
