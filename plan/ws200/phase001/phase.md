<!-- awesome-plan project=zedbsd record=ws200-p001 -->
# ws200-p001: Change Password のウィザードと Sign-in Methods

Status: cleared（2026-10-10 Q1 判定: T1-525（Change Password・Sign-in Methods の全手順）・T1-528（手順 8）。実機は p002）
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

## 進み（P1、2026-10-10）

| 内容 | 検証 |
| --- | --- |
| passkey: methods の bit（PASSKEY_METHOD_*）、`passkey_methods_parse`・`_text`・`_effective`（設定済みの方式だけ、password も鍵も無ければ password を足す: PIN だけは start の後の最初の sign-in にならない）・`passkey_options_methods`（record.c）。`styles` は効く方式を答える、`enrolled` に ` methods=N`（bit）、`auth` は外した方式を秘密を見る前に `fail style-off`、新しい操作 `set-methods NAME PASSWORD METHODS`（password で確かめ、password か鍵を含む、password が無ければ鍵が要る、key-pin・key-touch は保つ、既定なら行を消す）。set-options と書き込みを `passkey_options_write` にまとめた。passkey-fido2: 鍵を外した account の auth は `style-off`、KEYOWNER の持ち主から外す | `plan/ws200/tests/passkey-methods-host-test.sh`（新）PASS、passkey・passkey-options・fido2 の host 試験 PASS |
| sessiond: `SETMETHODS METHODS` + password の行 → `set-methods`（password の試みとして数える、成功は syslog）、`style-off` は greeter・lock にそのまま。backend: ENROLLED の `methods=`、`kl_backend_session_methods_get`・`_set_methods`（session-none は ENOTSUP）。compositor: account の request 11 `set_methods`・event 10 `methods`（version 26、KL_SYSTEM_MANAGER_VERSION 26）、lock 中は EBUSY。libkeiland: `kl_system_account_methods`・`_set_methods`、`KL_SYSTEM_HAS_METHODS`、`KL_SYSTEM_METHOD_*`、**KL_VERSION 78**（Q1 が merge の時に確かめる）。greeter: `style-off` で「That way to sign in is turned off. Use another.」と styles を問い直す（日本語 1 行） | `plan/ws199/tests/sessiond-keys-host-test.sh` に SETMETHODS と style-off を足して PASS、sessiond-auth PASS |
| Settings: 新 `page-users-password.c`。Password の card の Change Password → popup（Step 1 今の password → Step 2 新しい password 2 回 → Done、不一致・8 文字未満・今と同じはその場で 1 行、今の password の誤り・拒否は Step 1 へ、2 分の idle で保持した password を消す、busy は取り消せない）。Sign-in Methods の card（Password・PIN・Security Key の switch、未設定は灰色と理由、password と鍵の最後の 1 つは灰色と理由）→ popup（password を外す時は console の警告 → password → Change → Done、他は password だけ）。page-users.c の 3 つの欄を除いた。host-kl-system.c に HOST_METHODS・HOST_ENROLLED・HOST_REFUSAL | `plan/ws200/tests/settings-password-host-test.sh`（新）PASS、settings-keys PASS、host の renderer で頁と popup（build/p1-ws200/users.png・popups.png） |
| docs: security.md の「Sign-in methods」。AAT: `apps.settings.change-password` を popup に直した、新 `apps.settings.sign-in-methods`、`apps.settings.users-page` の行 | — |

build: zedBSD の passkey・passkey-fido2・sessiond・settings・wayland warning 0、Linux の keiland-linux.mk all warning 0、style-check（変えた file で増えた所 0、新しい file 0）。commit 9b2ac1b99（実装）。

設計との違い: 「Sign-in Methods」は button の先の popup の checkbox ではなく、Users の頁の card の switch にし、押すと password の popup（Security Keys の頁の「Sign in with a security key」と同じ形）。console・su・sudo・SSH は passkey を通らないので変えていない。

再開点: T1 の結果を Q1 が判定（PASS で cleared）。5330 の UAT は p002（beta2.md の「次の UAT」の 8）。

## T1-523 の FAIL の直し（2026-10-10 P1、agent/p1）

- sign-in-methods 手順 8（password を外しても lock に Password の pill、styles=7）: **製品**。libkeiland-backend-zedbsd の `session_take_styles` が STYLES の答えに password を常に足し、greeter の `greeter_styles_take` も `| PASSWORD` していた。backend は `password` の語も読んで 0 から組み立て（空の答えは 0 → `kl_backend_session_styles_get` が password）、greeter は答えのままにし（何も無ければ password）、提示されない style からは最初に提示される style（password → PIN → 鍵）へ移る（`greeter_first_style`）。`greeter_next_style` は順の輪で提示される次の物。PIN の止め（`pin-off`）で password も無ければ「That way to sign in is turned off. Use another.」と最初の style。試験: `plan/ws131/tests/host-session.c` に `STYLES pin fido2` → PIN|KEY と空の STYLES → password（67/67 PASS）。
- change-password の AAT の helper（`plan/tools/aat/scenarios/helpers_apps.py`）を popup のウィザード（control 400、Step 1 の今の password → Step 2 の新しい物 2 回、Tab、Esc）に書き直した: 1 開く、2 不一致・3 短い（request が出ない）、4 今の password の誤り（request と errno≠0、shadow 不変）、5 正しい変更（errno=0、shadow が変わる）、Done、shadow を戻す。
- build: zedBSD の wayland（config/current-uat.mk）warning 0、style-check の増え 0。QEMU は T1 の再試験待ち。
