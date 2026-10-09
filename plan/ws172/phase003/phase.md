<!-- awesome-plan project=zedbsd record=ws172-p003 -->
# ws172-p003: 鍵（FIDO2）の login と登録: passkey-fido2 と機器の helper、UI

Status: cleared（2026-10-07 Q1 の判定: T1-324 (c) で fido2-p003-guest PASS（段 A・B）。段 C はユーザーの決定で実機の鍵の UAT）（旧: uncleared（2026-10-07 Q1 の判定: T1-278 で fido2-p003-guest FAIL: 初回の login で Settings の Welcome が開き Users の頁に届かない（試験の前提）。passkey-fido2・greeter の styles=5・PIN の行は ok。P2 が直す）（旧: in-progress（2026-10-06 P2: 段 A（passkey-fido2・helper・`_passkey`）と段 B（鍵の一覧・登録・削除の口と UI）を実装し、host 試験 PASS。段 C はユーザーの決定で実機の UAT（WS161 p006）。QEMU の鍵の無い所の確認は T1 待ち）））
WS: [ws172](../ws.md)
設計: [phase001](../phase001/phase.md) の §7・§12（B1・B2・M3・M6・M7）と判断 P3・P4・P5・P8・P9、`docs/architecture/security.md` の「The parts」「The request」「The security key」
Queue: Q1 の P2 の列（2026-10-06、q824 → q826 → WS161 → **WS172**）

## 範囲

設計 §12.1 の p003。`/usr/libexec/passkey-fido2` と機器の helper（`_passkey`）、鍵の選び（PIN の前）、登録は 1 本だけ、Settings の鍵、greeter・lock の「Use security key」と CANCEL。第 1 段の規則で正常系だけ。準正常・異常は [backlog-p2](../../ws177/backlog-p2.md)。

## 段

| 段 | 内容 | 状態 |
| --- | --- | --- |
| A | `passkey-fido2`（auth・enroll-fido2・remove-fido2）、機器の helper、`_passkey` の account、`/etc/passkey` の鍵の行の追加・数の更新・削除、build の登録、host 試験 | 実装済み（2026-10-06） |
| B | sessiond の `ENROLLED` に鍵の一覧（id・label）、libkeiland-backend と compositor の口、greeter・lock の「Use security key」と TOUCH の表示と CANCEL、Settings の Users の鍵の card（登録・削除） | 実装済み（2026-10-06） |
| C | QEMU で鍵の流れを確かめる手段 | ユーザーの決定（下）で作らない。鍵の流れは実機の UAT |

## 段 A の実装（2026-10-06 P2）

- `userland/base/passkey-fido2/`（`/usr/libexec/passkey-fido2`、root、0500、package `passkey-fido2`、`base/passkey` と `security/openssl` を要る、amd64、既定は n）:
  - `main.c`（root）: 要求（`/sbin/passkey` と同じ形、`request.c`）、account（auth は名前だけ、登録・削除は `login_verify` で password）、使える account（uid 1000 以上、shadow の lock・期限）、`/etc/passkey` の鍵の行（private でなければ断る）。
    - auth: 32 byte の challenge、clientDataHash = SHA-256(`"zedbsd.login" NUL name NUL challenge`)、helper の答えを libpasskey の `verify.c` で確かめる（公開鍵は account の行から、UP と UV、署名、WebAuthn §7.2 の count、`cloned`）。大きくなった count は書き戻す。
    - enroll-fido2: label（1〜32 byte、制御文字と `:` 無し）、5 本まで、helper の authData を `pk_ctap2_read_made` で自分で読み直す（relying party、UP、UV、AT、ES256 の鍵が曲線の上）、行 `name:uid:fido2:ID:KEY:COUNT:zedbsd.login:LABEL:DATE`（base64url）を足し、`ok uid=N id=ID`。
    - remove-fido2: その account の ID の行を消す。
    - `/etc/passkey` の変更は account の lock の下で読み直し、signal を止め、版を確かめ、`passkey_record_edit`（新しい、`record.c`）で 1 行だけを足す・替える・消す。
  - `device.c`（root）: 鍵の node を開けて `HIDRAW_GRAB` で claim し、helper を fork して pipe の行を読む。`touch` は `status touch` としてすぐ出す。触れる待ちの 30 秒＋3 秒を越えたら helper を KILL して `timeout`。
  - `helper.c`（子）: 標準の fd を `/dev/null` に、core 無し、alarm、chroot `/var/empty`、`setgroups`・`setgid`・`setuid` で `_passkey`。
    - auth: PIN 無しの GetAssertion（`up:false`、allowList）で credential を持つ鍵を探し（B1）、その鍵にだけ PIN（PIN/UV の token、`getPinUvAuthTokenUsingPinWithPermissions` の GetAssertion の許可）を送り、触れて GetAssertion。鍵の PIN が無い鍵は使わない（P3）。
    - enroll: 鍵がちょうど 1 本（0 本は `no-key`、2 本以上は `many-keys`、B2・P9）、PIN の token（MakeCredential の許可）、MakeCredential（user id = 名前の SHA-256 の先頭 16 byte、登録済みの ID を excludeList に）。
    - 鍵の答えを passkey の理由の語に（`bad-secret`・`key-locked`・`no-key`・`timeout`・`device`）。
  - `wire.c`（純粋）: base64url、16 進、helper の行、鍵の行の作り・読み・count の書き替え、label、clientDataHash、user id。
- `userland/base/passkey/record.c`: `passkey_record_edit`（name と kind の行のうち 4 番目の欄が ID の行だけを替える・消す、足す）。`passkey_record_replace` は同じ下請けを使う形にした（動作は同じ）。
- libpasskey: `struct pk_made_credential` に authData そのものを持たせ、`pk_ctap2_read_made` を公開した（登録の authData を root が読み直すため）。
- `_passkey` の account（uid・gid 79、`/var/empty`、nologin、shadow `*`）を `userland/base/etc/passwd`・`group`・`shadow` に足した（P4）。
- build: `platform/amd64/vmunix.mk` に fidoctl と同じ形の link の規則（OpenSSL の staged の header と `libcrypto.so`）。試験の config `plan/ws172/tests/config-amd64-fido2.mk`（passkey の image ＋ openssl・passkey-fido2・fidoctl・loopback の鍵）。
- zedBSD に `RLIMIT_NPROC` が無いので、helper の「process を作れない」は rlimit ではなく空の root（exec する program が無い）で代えた（backlog に記録）。

## 確認（2026-10-06 P2、host）

| 確認 | 結果 |
| --- | --- |
| `plan/ws172/tests/fido2-host-test.sh`（`fido2-wire-host-test.c`: base64url の RFC 4648 の例と拒む物、16 進、helper の行、鍵の行の作り・読み・count、label、clientDataHash と user id を手で hash した物と比べる。Linux 向けの passkey-fido2 の全体を build し、root でなければ何も出さず終了状態 2） | PASS |
| `plan/ws172/tests/passkey-host-test.sh`（`passkey_record_edit` の追加・count の替え・削除・他の名前の ID を足した） | PASS |
| `plan/ws161/tests/libpasskey-host-test.sh`・`fidoctl-host-test.sh`（libpasskey の変更の後） | PASS |
| zedBSD の build（`ZEDBSD_CONFIG=plan/ws172/tests/config-amd64-fido2.mk BUILD=build/p2-fido`: passkey-fido2・fidoctl・passkey） | warning 0 |
| style-check（passkey-fido2 の 4 file と試験） | 0（`record.c`・`ctap2.c` の既存の指摘の数は変わらない: 10・33、p006 の全文規約で直す） |

未実施: 鍵との実際のやり取り（helper の sandbox、GRAB、GetAssertion・MakeCredential）は host では動かせない。QEMU は段 C の手段が要る。実物の YubiKey は WS161 p006 の UAT。

## 残り

- 段 B・段 C（上の表）。
- WS161 p005（NFC の transport）が入ったら、helper が `/dev/smartcard*` の鍵も使う。

## 2026-10-06 夜 ユーザーの決定（段 C、P5 の置き換え）

P2 の案 (a) kernel の試験の driver に CTAP2 の応答器、(b) userland の応答器と kernel の中継、(c) 実機の鍵だけ、へのクリックの回答「(c) 実機の鍵だけで確かめる」: QEMU では鍵の流れを試さず、実機の YubiKey（WS161 p006 の UAT）で確かめる。承認済みの P5（kernel の loopback を CTAP2 の応答器に広げる）は行わない。

## 段 B の実装（2026-10-06 P2）

- **鍵の一覧**: `/sbin/passkey` の `enrolled` の答えに、鍵ごとに ` key=REF/LABEL` を足した。
  - REF は鍵の参照で、credential ID の base64url の text の 64 bit FNV-1a を 16 桁の 16 進で表したもの（`passkey_record_ref`）。
  - LABEL は label の bytes を 16 進で書いたもの。空白を含まず、5 本でも sessiond の 1 行（512 byte）に入る。
  - `remove-fido2` は ID の代わりに参照も受ける（`passkey-fido2` の `main_remove`）。
  - sessiond の `AUTH_EXTRA_MAX` を 480 にした。
  - `docs/architecture/security.md` の要求の表を直した。
- **libkeiland-backend**:
  - `kl_backend_session_keys_get`、`kl_backend_session_add_key`（`ENROLL fido2 LABEL` ＋ password ＋ 鍵の PIN）、`kl_backend_session_remove_key`（`REMOVE fido2 REF`）、`kl_backend_session_cancel`（`CANCEL`）。
  - `TOUCH` は `session_answer(KL_BACKEND_SESSION_TOUCH, 0)` として host に伝える。待っている要求は終わらない。
  - Linux・FreeBSD の `session-none.c` は ENOTSUP。
- **compositor**:
  - `kl_system_account_v1` の版 14（`KL_SYSTEM_MANAGER_VERSION` 14、`KL_SYSTEM_SINCE_KEYS`）に、request `add_key`・`remove_key`、event `key(ref, label)`（enrolled の前に鍵ごと）・`touch(request)` を足した。
  - 鍵の変更の答えは PIN の変更と同じ待ちで返す（`pin_key`）。`TOUCH` は鍵の追加の待ちがあれば `kwl_system_key_touch` が、無ければ greeter・lock が受ける（`handoff.c`）。
- **greeter・lock**:
  - 下の link は、password → PIN → 鍵 → password の順に、提供されている方式を巡る（「Use a security key」）。
  - 鍵の方式では field の hint が「Security key PIN」になり、4 文字以上で送る。
  - `TOUCH` で「Touch your security key.」と出す。待つ間に Esc を押すと `CANCEL` を送る。
  - 鍵の失敗の語（no-key・key-locked・device・cloned・bad-secret）を言葉にした。
  - 日本語の訳 9 行を `locale/ja/wayland.tr` に足した（`tr.py update`、check は 127 件で 0 problems）。
- **libkeiland**（KL_VERSION 52）:
  - 関数 `kl_system_account_keys`・`kl_system_account_add_key`・`kl_system_account_remove_key`・`kl_system_account_touched`、型 `struct kl_system_key`、bit `KL_SYSTEM_HAS_KEYS`・`KL_SYSTEM_CHANGED_TOUCH` を足した。`exports.map` を作り直した。
- **Settings**: Users の頁の PIN の card の下に「Security keys」の card（`page-users-keys.c`）を置いた。
  - 鍵の一覧と、鍵ごとの Remove（password を打つと押せる）。
  - field は現在の password、鍵の名前、鍵の PIN。「Add Security Key」と、touch の表示、答えの文。
- **試験の更新**:
  - `plan/ws131/tests/host-session.c`: TOUCH が伝わり要求は待ち続けること、鍵の一覧・追加・削除・CANCEL。
  - `plan/ws131/tests/host-system.c`: capabilities に HAS_KEYS、鍵の追加と touch、一覧、誤った password、削除。
  - `plan/ws089/tests/host-kl-system.c`: `HOST_KEYS` の stand-in。
  - `plan/ws172/tests/passkey-host-test.c`: field と参照。
  - 新しく run-host-settings-keys.sh（2026-10-10 削除: WS199 i01 で Users の頁から鍵の欄が消えたため、試験の整理の基準） を足した。

## 確認（段 B、2026-10-06 P2、host）

| 確認 | 結果 |
| --- | --- |
| `plan/ws131/tests/host-session.sh` | 63/63 |
| `plan/ws131/tests/host-system.sh` | PASS |
| run-host-settings-keys.sh（削除済み）（2 本の一覧の絵、password を打つと Remove が参照で頼む。host の renderer は field に 1 文字しか打てないので、追加（PIN 4 文字以上）は QEMU で確かめる） | PASS |
| `plan/ws172/tests/passkey-host-test.sh`・`fido2-host-test.sh`・`sessiond-auth-host-test.sh` | PASS・PASS・ok |
| build（zedBSD: libkeiland.so・wayland・settings・sessiond・passkey・passkey-fido2。Linux: `keiland-linux.mk all`） | 自分の code の warning 0（OpenSSL の package の build の警告は外部） |
| style-check（変えた C） | 新しい違反 0（passkey の main.c の既存の 27 は変わらない） |

未実施（T1 に頼む、鍵の無い所。2026-10-06 夜 Q1）:
- greeter の「Use a security key」の link と hint の見た目（鍵を登録した account が要るので、test の image の `/etc/passkey` に鍵の行を置く必要がある）。
- 鍵が無い時の振る舞い（`no-key` の文）。
- Settings の Users の「Security keys」の card の見た目。

T1 への依頼の手順（2026-10-06 P2）: `plan/ws172/tests/build-fido2-image.sh BUILD` で image を作り、`plan/ws172/tests/fido2-p003-guest.sh` を流す。
- 中身: passkey-fido2 の mode と `_passkey`、作り物の鍵の行、greeter の styles=5 と link（`KWL GREETER link` の log の位置をクリック）、3 文字の PIN は送らない、4 文字で no-key、password で login、Settings の Users の card。
- 撮る絵: greeter.png・key.png・no-key.png・settings.png。
- greeter の link の位置を試験が知るため、styles を受けた時に `KWL GREETER link x= y= width= height=` を出すようにした。


## T1-278 の直し（2026-10-07 q834 P2）

- `plan/ws172/tests/fido2-p003-guest.sh` の段 0 で kei の `~/.config/keiland/desktop.conf` に `welcome.done=1` を置く（他の行は保つ）。初回の session が Settings の Welcome を開くと、段 4 の `/bin/settings --page=users` がその窓へ行き `ZSETTINGS PAGE users` が出なかった。段 4 に `KWL WELCOME skip done=1 error=0` の確かめを足した。
- 確認: `sh -n` のみ。QEMU は T1 の再試験待ち。
