<!-- awesome-plan project=zedbsd record=ws199-p001 -->

# ws199-p001: セキュリティキーの頁・ウィザード・ログインの鍵のモード・NFC（設計）

Phase ID: `ws199-p001`
Parent: [WS199](../ws.md)
Status: planning（2026-10-10 P1: 第 4 版。ユーザーの答え（ws.md「設計の review の後の決定」）、[review-2.md](review-2.md) の N1〜N15、BUG-286（NFC）を入れた。短い再確認の後に i01 から実装）
Phase disposition: normal
Queue: q921（P1、2026-10-10）
依存: ws172-p002・p003（PIN と鍵の今の経路）、ws161-p005（libpasskey の transport-nfc、usb-ccid、smartcard）、ws187-p002・p003（lock の画面の swipe と方式の pill）、docs/architecture/security.md「Login authentication」
所有 path: `userland/desktop/settings/`、`userland/desktop/libkeiland/system/`、`userland/desktop/include/keiland/keiland.h`、`userland/desktop/wayland/`（greeter.c・system.c・handoff.c・sleep.c）、`userland/desktop/libkeiland-backend*/`、`userland/desktop/sessiond/`、`userland/base/passkey/`、`userland/base/passkey-fido2/`、`userland/base/libpasskey/`、`userland/base/fidoctl/`、`docs/architecture/security.md`、`docs/reference/security-keys.md`、`plan/ws199/`。**Q1 の許しが要る**: `src/drivers/generic/smartcard.c`（card の出入りの事象、§5.3、WS161 の file）

版: 第 1 版（5903f7548）→ 第 2 版（63db6b7bc）→ 第 3 版（2dc99e171、review-1 への答え）→ **第 4 版（この文書。前の版の本文を置き換える。前の版は git の履歴）**。

## 0. ユーザーの決定（ws.md、2026-10-10）

- Settings に独立の頁「Security Keys」。鍵の一覧、Add Key（ウィザード）、Change PIN、Reset Key。今の 6 桁の PIN は同じ頁で「Software Security Key」。処理中は popup を操作できない（busy）。
- ログイン画面（greeter）: 鍵が挿さっていれば（NFC は reader に当てると）自動で鍵のモードに入り、その鍵の持ち主の user を選ぶ。**login は常にタッチが要る**。PIN が要る設定なら PIN（欄のすぐ下の keypad）とタッチ。
- Settings に「ログインに鍵の PIN を要らない」「unlock に鍵のタッチを要らない」。組み合わせは 3 通り: **PIN＋タッチ（既定）・タッチだけ（PIN 不要）・PIN もタッチも不要（タッチ不要は lock の解除だけ）**。入れる時は password と警告。
- **タッチ不要は lock の解除だけ**。lock の画面は swipe しないと認証の card が出ないので、挿したままの鍵でも自動の解除にならない。「lock の後に挿した鍵」の判定は作らない。swipe（または key）で card が出た後、鍵があればタッチ無しで解除し、「鍵を確かめています」を最低 0.5 秒出す。警告に「鍵を挿したままだと、機械の前の人は誰でもスワイプで解除できる」。
- /etc/passkey の 1 行の options に WS200 の Sign-in Methods と鍵の PIN・タッチをまとめ、両方の頁が読む。popup の部品は WS199 i01 で作り WS200 が使う。
- 鍵の PIN の変更は鍵の PIN だけ（password 無し）。Reset で消えた登録はこの機械からも消す。Add の password は最初。範囲は「全部ベータ2」。
- passkey・passkey-fido2・fidoctl の分け方は今のまま。console・su・sudo・SSH は password だけ。
- BUG-286（2026-10-10 UAT）: NFC の YubiKey で登録と login が失敗（USB は動く）→ passkey-fido2 に NFC の card を足す（WS199 の中、§5）。

## 1. 今（調べ）

- 経路（変えない）: Settings・greeter（特権なし）→ compositor → backend → sessiond（root、常駐、失敗の数え・遅れ）→ `/sbin/passkey`（root、短命、password と PIN、/etc/passkey）→ `/usr/libexec/passkey-fido2`（challenge と検査）→ device helper（`_passkey`、chroot、CTAP）。
- passkey の操作: `auth`・`styles`・`enrolled`・`enroll-pin`・`remove-pin`・`enroll-fido2`・`remove-fido2`（request.c、field の数は固定）。
- passkey-fido2 は `pk_os_list` の USB の hidraw だけを開く（device.c）。helper は始めに開いた鍵だけに問う。NFC（`/dev/smartcardN`、ws161-p005）は fidoctl でしか使えない（BUG-286 の原因）。
- 鍵の出入り: kernel は hidraw の node の ADD・REMOVE を `/dev/system` の INPUT で出す（detail に `usage=f1d0:0001`）。smartcard は **slot**（reader）の ADD・REMOVE だけを USB の class で出し、**card の出入りは slot の node の read() だけ**（`ccid_event`）。
- sessiond の seat.c は INPUT・USB を購読し、session の間は hidraw と smartcard の node を session の user に 0600 で渡す。
- sessiond の `auth_granted` は AUTH・UNLOCK 以外の成功を password の成功として数えを消し `signed_in` を立てる（BUG-285 の B）。STYLES・ENROLLED は `auth_listing` で policy に触れない。
- greeter の lock: swipe・key で card が出る、30 s 何も無ければ card が消える（`GREETER_CARD_IDLE_MS`）。

## 2. /etc/passkey の options の行（WS200 と共有）

- `<name>:<uid>:options:methods=<password,pin,fido2>:key-pin=<0|1>:key-touch=<0|1>`。account に 1 行。無い時は methods=全部、key-pin=1、key-touch=1。
- `key-touch=0` は `key-pin=0` の時だけ正しい。重複・不正な行は既定（一番強い）として読む。知らない種類の行は今の規則で残る。
- 書くのは passkey の `set-options`（password で確かめる、§4.4）。WS199 は key-pin・key-touch を、WS200 は methods を変え、他の field は読んだまま書き戻す。
- 最後の fido2 の鍵を消したら key-pin=1・key-touch=1 に戻す（methods が既定なら行ごと消す）。
- 読むのは passkey（`enrolled` の答えに `options=` を足す）と passkey-fido2（auth の時）。

## 3. 画面

### 3.1 Settings の頁「Security Keys」（sidebar で Users の次）

- **Software Security Key**: 今の 6 桁の PIN（Users の頁の PIN の card を移す）。Set・Change・Remove（今の `kl_system_account_set_pin`）。
- **Security keys**: session の user の登録済みの鍵の名前の一覧、各行に Remove。button **Add Key**、**Change PIN**、**Reset Key**。
- **Sign in with a security key**（i04）: radio 3 つ「PIN and touch」「Touch only」「No PIN, no touch to unlock」。鍵の登録が無い時は灰色。弱い方へは popup（警告と password）、強い方へは password だけ。警告の文:
  - Touch only: 「Anyone who has your security key can sign in to this computer with a touch.」
  - No PIN, no touch to unlock: 上に加え「While your key stays plugged in (or lies on the reader), anyone at this computer can unlock it with a swipe.」
- Users の頁の鍵と PIN の card は消し、「Security Keys」への link。
- Linux・FreeBSD は鍵の操作の button の代わりに「Not available on this system」（`KL_SYSTEM_HAS_KEY_OPS` が無い）。

### 3.2 popup（`dialog.c`、Settings の窓の中の modal）

- 窓の中央の card、下の頁は暗くして入力を受けない。題、本文、欄（0〜2、秘密の欄は伏せ字）、button（Back・Cancel・主）、下の小さな link（任意）、「Step n of m」。
- **busy**: spinner と言葉、欄と button は灰色。Cancel は取り消せる step（Touch・Replug）だけ押せる。Esc は busy でない時だけ Cancel。
- 秘密の欄（password・PIN）は送った時・閉じた時に消す。ウィザードが 2 分何も無ければ秘密を消して最初の秘密の step に戻す（N12）。
- API（i01 で実装、WS200 も使う）: `se_dialog_open(app, act, ready)`・`se_dialog_step(app, title, step, steps, body, primary, can_back)`・`se_dialog_field(app, label, placeholder, kind, limit)`・`se_dialog_digits`・`se_dialog_set_text`・`se_dialog_text`・`se_dialog_length`・`se_dialog_focus`・`se_dialog_link`・`se_dialog_error`・`se_dialog_busy(app, text, cancellable)`・`se_dialog_close`・`se_dialog_dismiss`。owner は `act(app, SE_DIALOG_PRIMARY|BACK|CANCEL|LINK|IDLE)` と `ready(app)` を渡す。ui.c は popup が開いている間、click を popup の control だけに、key を全部 popup に渡し、scroll・drag を止め、別の頁へ移ると Cancel する。

### 3.3 Add Key のウィザード

- **i01（今の API）**: 1 Password → 2 Name（既定「Security Key」、同じ名前が有れば「Security Key 2」…、1〜32 byte、`:` と制御文字は不可）→ 3 Key PIN → 4 Touch（busy「Touch your security key, or hold it to the reader.」）→ 5 Done。今の ENROLL fido2 の 1 回の request（password・名前・鍵の PIN）。Cancel は出さない（今の 35 s の時間で終わる）。`bad-secret` は「The password or the key's PIN is wrong.」で 1 へ（名前は残す。今の helper は両者を分けない、N11）。`locked`（今の sessiond の語）は「Too many wrong attempts. Wait, then try again.」、`many-keys`・`no-key`・`timeout`・他は語を添えて。
- **i03（鍵の情報の後）**: 1 Password → 2 Insert（KEYINFO、§4.1。0 個は「Insert your security key, or hold it to the reader.」と button「Check again」、2 個以上は「Remove the other keys: only one at a time.」、1 個で名前と PIN の有無）→ 3 Name（既定は鍵の名前）→ 鍵に PIN が無ければ 4 Set PIN（2 回、§4.4 の長さ）→ 5 Key PIN（link「Forgot the PIN?」→ Reset Key。残り 3 回以下なら出す）→ 6 Touch（Cancel 可）→ 7 Done。`bad-secret` は 1、`bad-key-pin` は 5、`key-locked` は Reset を案内、`key-replug` は「Unplug the key and plug it in again.」。

### 3.4 Change PIN のウィザード（i03）

- 1 Insert（PIN の無い鍵は Set PIN へ）→ 2 今の PIN と新しい PIN を 2 回 → busy → Done。password は要らない（鍵が自分で確かめる）。

### 3.5 Reset Key のウィザード（i03）

- 1 Warning（鍵の全部の credential と PIN が消える、他の機械・web の分も）→ 2 Password → `KEYRESET` を送る → 3 Replug（`status replug` を受けて「Unplug the key, then plug it back in.」NFC は「Take the key away from the reader, then hold it there again.」。挿し直したらすぐ鍵に送るので、利用者は急がなくてよい。窓は鍵による: CTAP2 は電源の入りから 10 秒、YubiKey は 5 秒の見込み）→ 4 Touch（Cancel 可）→ 5 Done「This key's sign-in registrations on this computer were removed. Add it again to sign in with it.」。
- 答えが無いまま終わった時（§4.5 の時間切れ）は「The key may or may not have been reset.」と出し、ENROLLED を問い直して一覧を直す。

### 3.6 ログイン画面（greeter）の鍵のモード（i05）

- compositor が鍵の出入り（§5.3 の `keys_changed`）を greeter に伝える。greeter は画面が出た時と鍵が来た時に `KEYOWNER`（§4.2）を問う（sessiond が 1 秒に 1 回まで、最後の 1 つにまとめる）。
- 答え `user=<name>`: その user を選び（一覧に居れば。打ちかけの password は消す）、鍵のモードに入る。`none` は「This security key is not registered here.」を 1 行。`many`・`many-owners` は何もしない。鍵が抜けたら（AUTH の途中なら CANCEL して）password の欄に戻る。
- 鍵のモードの画面（login は常にタッチ）:
  - **key-pin=1**: PIN の欄と、そのすぐ下の keypad（§3.8）。Enter で `AUTH user fido2` と PIN → TOUCH →「Touch your security key, or hold it to the reader.」
  - **key-pin=0**: 「Touch your security key, or hold it to the reader.」とすぐ `AUTH user fido2`（PIN 無し）。時間切れ（30 s）は自動で繰り返さず button「Try again」。
  - 下に link「Use your password」（鍵を挿し直すか link「Use a security key」で戻る）。
- 自動で始めた試みが取り消し・時間切れ・鍵の抜けで終わった時は数えない（鍵が答えて署名が合わなかった時だけ数える）。
- 失敗: `bad-secret`（PIN の誤り）・`timeout`・`cloned`・`pin-required`（`alwaysUv` の鍵: PIN の欄に移る）を 1 行。遅れは今どおり。

### 3.7 lock の画面（i05）

- swipe・key で card が出た時（今の規則）、sessiond に `KEYOWNER`（session の user の credential だけで問う）。鍵が user の物なら鍵のモード:
  - key-pin=0・key-touch=0: 「Checking your security key…」と `UNLOCK fido2`（PIN 無し）。OK でも 0.5 秒経つまで解かない。
  - 他は §3.6 と同じ（PIN とタッチ、またはタッチだけ）。
- card が出ている間に鍵を挿す・当てると、同じく鍵のモードに入る。card が出ていない時（hint だけ）の鍵の出入りは何もしない（sleep の復帰の数え直しで解けない）。
- sleep に入る時に card を閉じる（復帰は必ず hint から）。
- 今の pill（Password・PIN・Security Key）はそのまま。Security Key の pill は鍵のモードと同じ画面。

### 3.8 PIN の keypad（greeter・lock、i05）

- greeter が PIN の欄の**すぐ下**に描く: 数字 3×4（1〜9、0、Backspace、Enter）と「ABC」で英字 3 段（小文字・大文字の切り替え）。物の keyboard も効く。非 ASCII の PIN は物の keyboard だけ。
- Software Security Key（6 桁）の欄には数字だけの keypad。

## 4. sessiond と passkey の操作

### 4.1 KEYINFO（i03）

- session の request `KEYINFO` → passkey `key-info` → passkey-fido2 が鍵（USB と答える NFC の card）を数え、1 個なら名前・PIN の有無・残り回数・minPINLength。答え `KEYINFO count=N [name=<hex> pin=0|1 retries=N min=N]`。

### 4.2 KEYOWNER（i05）

- greeter の request `KEYOWNER`（lock は session の request で、その user の分だけ）→ passkey `key-owner`（name の field は `-`、lock は user の名前）→ passkey-fido2 が /etc/passkey の fido2 の credential で鍵に up=false・UV 無しの assertion を問い、**答えの署名を credential の公開鍵で確かめる**（合わない答えは持ち主と見ない）。allowList は鍵の `maxCredentialCountInList` ごとに分ける。
- 答え `KEYOWNER user=<name> key-pin=0|1 key-touch=0|1`、`none`、`many`（鍵 2 個以上）、`many-owners`（同じ鍵が 2 つ以上の account に）。持ち主は uid 1000 以上、password が locked・expired でない account。sessiond は答えの名前を passwd で引き直す。
- 開示: login の画面で鍵を挿すと持ち主の名前が分かる（その鍵を持つ人に）。

### 4.3 AUTH・UNLOCK の fido2（i04）

- passkey の新しい操作 `auth-fido2`（word、name、`login|unlock`、鍵の PIN（空可））。sessiond は AUTH の fido2 を `login`、UNLOCK の fido2 を `unlock` で送る（greeter は決められない）。
- passkey-fido2 は options の行で決める: PIN が空 → key-pin=0 の時だけ PIN token 無し・UV を求めない（key-pin=1 なのに空なら `bad-request`）。up=false は `unlock` かつ key-touch=0 の時だけ。検査の flag も同じ条件の時だけ外す。
- 失敗の数え・遅れは今どおり。PIN もタッチも無い解除でも成功は `signed_in` を立てる（security.md に書く）。

### 4.4 Settings の鍵の操作（i03・i04）

| request（Settings の session だけ） | passkey の操作 | 秘密 | 失敗の数え | 成功の時 | 時間（sessiond） |
| --- | --- | --- | --- | --- | --- |
| `KEYINFO` | `key-info` | 無し | 数えない・遅れ無し | policy に触れない（`auth_listing` と同じ） | 5 s |
| `KEYPIN set` | `key-set-pin` | 鍵の新しい PIN | 数えない（鍵が数える） | policy に触れない | 10 s |
| `KEYPIN change` | `key-change-pin` | 鍵の今の PIN、新しい PIN | 数えない（鍵が数える） | policy に触れない | 10 s |
| `KEYRESET` | `key-reset` | account の password | password の誤りだけ数える（`status verified` の後の失敗は数えない） | `status verified` で数えを消し `signed_in`（password の成功） | 75 s |
| `SETOPTIONS` | `set-options` | account の password、key-pin、key-touch | password の試み | password の成功 | 10 s |

- KEYINFO・KEYPIN の失敗は `auth_refused` の遅れと syslog の「failed change」を通さない（N1）。greeter からは送れない。どれも一度に 1 つ。
- 鍵の PIN の長さ: 鍵の minPINLength（無ければ 4）以上の code point、63 byte 以下（libpasskey の `ctap2_check_pin` と同じ規則）。合わない時は `pin-policy`。
- libpasskey に `pk_ctap2_reset`（0x07）。

### 4.5 Reset の 1 回の実行（i03、N2・N3・N4・N10）

1. passkey が password を確かめる（誤りは遅れの後 `bad-secret`）→ `status verified`（sessiond が数えを消す）。
2. passkey-fido2 が `status replug` を出し、鍵が「0 本になってから 1 本」になるのを 30 s まで待つ（USB は `pk_os_list` を 100 ms ごと、NFC は slot の `ccid_event` の REMOVED→INSERTED）。2 本以上は `many-keys`。
3. 現れたらすぐ: helper が silent の問いで**この鍵が持つ /etc/passkey の credential（全部の account）**を調べ（覚える）、authenticatorReset を送る → `status touch`。
4. 成功で、覚えた credential の行を /etc/passkey から消して ok（他の account の行も: どれも鍵の中では消えている）。鍵が account の credential を持たなくても reset はする。
- 時間: passkey-fido2 は replug 30 s、reset の helper 33 s を自分で持つ。sessiond の `KEYRESET` は 75 s。
- reset を送った後、passkey-fido2 は SIGTERM を止め（`main_change` と同じ）、helper の答えと行の書き換えまで済ませる。
- 取り消し（N3）: helper は SIGTERM で、開いている USB の channel に CTAPHID_CANCEL を送り（`pk_hid_cancel`、signal の handler は write だけ）、鍵の答え（`KEEPALIVE_CANCEL` 0x2d か成功）を待って結果を出す。passkey-fido2 は helper の答えを待ってから終わる（sessiond の KILL まで 2 s）。NFC は card を離せば止まる。
- この CANCEL は assert・make の触れる待ちにも同じく効く。

### 4.6 status と語の通り道（N7・N8）

- 新しい status 行: `status replug`、`status verified`。sessiond の `auth_read` は `status touch` と同じく答えにしない。backend は `REPLUG`・`VERIFIED` を `TOUCH` と同じく先に抜き、compositor が Settings に `KL_SYSTEM_CHANGED_REPLUG` を出す（`pin_key` を新しい操作でも立てる）。
- helper の CTAP の status を語に: 0x31 `bad-key-pin`、0x32 `key-locked`、0x34 `key-replug`、0x35 `no-pin`、0x36 `pin-required`、0x37 `pin-policy`、0x30 `not-allowed`、0x27 `denied`、0x2F `timeout`、0x2D `canceled`。helper の手元の検査も: PIN の無い鍵は `no-pin`、libpasskey の PIN の規則の EINVAL は `pin-policy`。
- sessiond の語の表は request で分ける: **AUTH・UNLOCK は今の語に写す**（`bad-key-pin`→`bad-secret`、`key-locked`・`key-replug`→`locked`、他の新しい語→`bad-secret`）、**ENROLL・REMOVE・KEY*・SETOPTIONS はそのまま**。
- 今の不具合（i01 で直す）: Settings は `key-locked`・`locked-account` を待つが sessiond は `locked` を送る → ウィザードは `locked` の行を持つ。

### 4.7 lock・sleep と Settings の鍵の操作（N5）

- compositor は lock（`kwl_lock`）と sleep の前に、待っている Settings の鍵の操作に CANCEL を送り（§4.5 の CTAPHID_CANCEL）、答え（最大 3 s）を待ってから lock・sleep を進める。
- lock の間、compositor は Settings の鍵の request を EBUSY で返す。unlock の後に `KL_SYSTEM_CHANGED_KEYS` を 1 回出し、ウィザードは今の鍵を問い直す。

### 4.8 compositor・backend・libkeiland

- backend: `kl_backend_session_key_info`・`_key_set_pin`・`_key_change_pin`・`_key_reset`・`_set_options`・`_key_owner`、AUTH・UNLOCK の fido2 は空の秘密を受ける。host の callback `keys_changed`（§5.3）。Linux・FreeBSD は ENOTSUP。
- compositor: Settings の account の object に request と event（KEYINFO・REPLUG・CHANGED_KEYS）、greeter に鍵の出入りと KEYOWNER。protocol の版を上げる。
- libkeiland: `kl_system_account_key_info`・`_key_set_pin`・`_key_change_pin`・`_key_reset`・`_key_cancel`・`_options_get`・`_set_options`、`KL_SYSTEM_HAS_KEY_OPS`・`KL_SYSTEM_CHANGED_KEYS`・`KL_SYSTEM_CHANGED_REPLUG`。KL_VERSION を上げる（番号は merge の時に Q1）。

## 5. NFC（BUG-286、i02）

### 5.1 libpasskey（os-zedbsd.c）

- `pk_os_list_slots`: `/dev/smartcard*` の slot を card の有無に依らず全部（slot の出入りは USB の事象）。
- `pk_os_card_attach(card, path)`: slot を O_RDWR で開くだけ（電源は入れない、claim しない）。`pk_os_card_power(card, io)`: CCID_POWER_ON（claim）と io。今の `pk_os_card_open` はこの 2 つ。`pk_os_card_present(card)`・`pk_os_card_event(card, &inserted)`（`ccid_event` を 1 つ読む）。
- Linux は今どおり card 無し（ENOTSUP・0 個）。

### 5.2 passkey-fido2 と helper

- `fido2_devices` に `cards[PK_OS_DEVICES_MAX]`・`card_count`。root が全部の slot を attach して helper に渡す（helper は開かない。claim は helper の電源の入りで取る）。
- 鍵の数え方: USB の handle ＋「card が有り、電源が入り、FIDO の applet の SELECT が通った slot」。電源・SELECT が通らない slot（ACR1552 の SAM の slot の EIO、FIDO でない card、他の program が claim 中の EBUSY）は飛ばす。
- assert: USB の鍵と今ある card に silent の問い。どれも持たず slot が有れば `status touch`（「Touch your security key, or hold it to the reader.」）を出し、slot の INSERTED を触れる時間まで待って、来た card に問う（security.md の「tapped during the attempt」）。NFC の鍵は場に入った事で user presence を満たす。
- make（登録）: 鍵 1 個（USB ＋答える card）。0 個で slot が有れば同じく当てるのを待つ。2 個以上は `many-keys`。
- KEYINFO・KEYPIN・KEYRESET・KEYOWNER も同じ数え方。

### 5.3 card の出入りの事象（Q1 の許しが要る）

- kernel の smartcard.c の `smartcard_card_changed` が、card が来た・去った時に `KERN_SYSTEM_EVENT_USB` の `KERN_SYSTEM_EVENT_CHANGE`（subject `smartcardN`、detail `vendor=.. product=.. slot=.. card=1|0 name=..`）を出す。今ある action と class だけで、UAPI（include/uapi）は変えない。`kern_system_event_post` を呼べる文脈かは実装で確かめる（呼べなければ event の worker に移す）。
- backend の events-zedbsd.c が USB の class も購読し、INPUT の hidraw の ADD・REMOVE（`usage=f1d0:0001`）と smartcard の CHANGE で `keys_changed` を呼ぶ。OVERFLOW でも呼ぶ。事象の無い kernel では来ないので、Insert の step に「Check again」。
- 代わり（許しが出ない時）: sessiond が slot を開いて `ccid_event` を読み、session の socket で伝える（root の daemon の code が増えるので推しは kernel）。

### 5.4 fidoctl（済み、aead021aa）

- `fidoctl list` は答える card だけを `card` と数え、他は `slot PATH ...: <理由>`。`-s`（up=false）と `-s verify`。

## 6. docs

- security.md と security-keys.md は各 i の code の前に書く: NFC の card の数え方と待ち（i02）、新しい操作と表（§4.4）・status・語の表・Reset の手順と消える行・CANCEL（i03）、options の行と規則・警告・`auth-fido2` の決め方・タッチ不要は unlock だけ（i04）、KEYOWNER の開示と 1 秒・鍵のモード・0.5 秒・sleep で card を閉じる（i05）。

## 7. 試験

- host:
  - libpasskey（`plan/ws161/tests/libpasskey-ctap2-host-test.c`）: reset（窓、touch、CANCEL で 0x2d・消さない）、新しい status の語。
  - passkey の request（新しい操作の field の数、空の PIN、`login|unlock`）、options の行の読み書き（既定、重複、key-touch=0 で key-pin=1 の不正、methods を書き戻す）。
  - passkey-fido2 の wire（`plan/ws172/tests/fido2-wire-host-test.c`）: replug・verified の status、検査の flag の緩め方（key-pin・key-touch・login|unlock の組で、許さない組の UP・UV 無しを拒む）、KEYOWNER の署名の照合。
  - sessiond（`plan/ws172/tests/sessiond-auth-host-test.c`）: KEYINFO・KEYPIN の成功・失敗で wrong・pin_wrong・signed_in が変わらない、`status replug`・`verified` が答えにならない、verified の後の失敗を数えない、KEYRESET の 75 s、語の表が request で分かれる、KEYOWNER の 1 秒。
  - Settings のウィザード（純粋な関数: step の遷移・既定の名前・2 回の照合・busy の拒否・2 分の秘密の消去）、greeter の鍵のモード（0.5 秒、鍵の抜けで戻る、card の無い時は何もしない）。
- QEMU（T1、AAT）: CTAP2 の鍵は無い（ws172-p003 のユーザーの決定）。頁・ウィザードの鍵の無い step（Insert の待ち、Software Security Key、一覧、radio と警告）を PNG に。
- 5330 の UAT（YubiKey 5 NFC、USB と ACR1552）: Add（USB・NFC、PIN 有り・無し）、Change PIN、Reset（USB・NFC、Touch で Cancel して触れても消えない、replug で別の鍵を挿すと止まる）、Remove、Software Security Key、login の 2 通り（USB・NFC）、unlock の 3 通り、lock 中の reset で lid を閉じると取り消して眠る、鍵の自動のモードと user の選択。
- 先に（Q1、fidoctl、§11）: up=false・UV 無しの assertion とその署名。

## 8. 実装の順（全部ベータ2、小さく merge）

| i | 内容 | 主な file |
| --- | --- | --- |
| i01 | popup の部品と頁「Security Keys」（今の操作だけ: 一覧・Remove・Add の 5 step・Software Security Key）、Users の頁の link、`locked` の行 | settings/（dialog.c（新）、page-users-keys.c（頁と鍵の card・Add/Remove）、page-users-pin.c（Software Security Key）、page-users.c（link の card）、settings.h（SE_PAGE_SECURITY_KEYS・se_dialog・se_keys）、pages.c、ui.c、system.c、main.c、Makefile 3 つ） |
| i02 | NFC（§5）: libpasskey の slot、passkey-fido2 と helper、kernel の card の事象、backend の `keys_changed`、security.md | libpasskey・passkey-fido2・smartcard.c・events-zedbsd.c |
| i03 | KEYINFO・KEYPIN・KEYRESET（§4.1・§4.4〜§4.7）、reset と CANCEL、Insert・Set PIN・Change PIN・Reset のウィザード | libpasskey・passkey・passkey-fido2・sessiond・backend・wayland・libkeiland・settings |
| i04 | options の行と `set-options`・`auth-fido2`、検査の緩め、radio と警告 | passkey・passkey-fido2・sessiond・settings |
| i05 | KEYOWNER、greeter と lock の鍵のモード、keypad、0.5 秒、sleep で card を閉じる | sessiond・passkey・wayland（greeter.c・sleep.c） |
| i06 | host 試験の残り、style-check、T1 の AAT の依頼、5330 の UAT の一覧 | plan/ws199/tests |

## 9. review-2 への答え

| # | 答え |
| --- | --- |
| N1 | §4.4 の「成功の時」の列。KEYINFO・KEYPIN は policy に触れない。lock 中は compositor が EBUSY（§4.7） |
| N2 | KEYRESET 75 s、passkey-fido2 の自分の deadline、reset の後は TERM を止める、答えの無い時の言葉（§3.5・§4.5） |
| N3 | helper の SIGTERM で CTAPHID_CANCEL、鍵の答えを待つ（§4.5） |
| N4 | 前の問いをやめ、replug の後・reset の前に問う。0→1 本、2 本は `many-keys`（§4.5） |
| N5 | lock・sleep の前に CANCEL、lock 中は EBUSY、unlock の後に CHANGED_KEYS（§4.7） |
| N6 | `status verified`（§4.4・§4.5） |
| N7 | §4.6 |
| N8 | §4.6（request で分ける、helper の手元の語、`locked` の行） |
| N9 | `keys_changed` の callback、OVERFLOW、「Check again」、EBUSY は次の CHANGED_RESULT で問い直す（§5.3・§3.3） |
| N10 | 全部の account の行を消す（§4.5） |
| N11 | i01 の 5 step と `bad-secret` の戻り先（§3.3） |
| N12 | 2 分で秘密を消す（§3.2） |
| N13 | 窓は鍵による、すぐ送る（§3.5） |
| N14 | §7 |
| N15 | Q1 の記録（ws.md の Phase の表、ws200/ws.md の依存と options の行） |
| §3 の 1・2 | ユーザーの決定で「lock の後に挿した鍵」の判定が無くなり、消えた。card が出ていない時の鍵の出入りは何もせず、sleep で card を閉じる（§3.7） |
| §3 の 3 | node の一覧は要らなくなった（タッチ不要は unlock の全部の鍵）。KEYOWNER と AUTH の順は §3.6（KEYOWNER の答えの後に AUTH） |

## 10. 危険と未確認

| 項目 | 内容 |
| --- | --- |
| up=false の assertion の署名 | 今の login の silent の問いで up=false は 5330 の USB で既に使っている（答えの有無だけ）。署名と flag は fidoctl -s で Q1 が確かめる（§7） |
| UV 無しの assertion | `alwaysUv`・credProtect の鍵は UV を求める → `pin-required` で PIN の欄へ |
| NFC の SAM の slot | 電源・SELECT の失敗で飛ばす。失敗の時間が長い reader は鍵の問いを遅らせる（5330 で測る） |
| card の事象の kernel の文脈 | smartcard の card の変化の呼び出し元が event の post を許すか（§5.3） |
| CTAPHID_CANCEL | reset の触れる待ちで鍵が 0x2d を返す事は CTAP の記述による（推測）。5330 で確かめる |
| QEMU | CTAP2 の鍵の模擬は無い。鍵の操作は 5330 |
| 見積もり | i01 3、i02 4、i03 6、i04 3、i05 5、i06 2、計 **約 23 LW** |

## 11. 第 4.1 版: review-3 への答え（§3〜§8 より優先）

[review-3.md](review-3.md): i01 GO、i02 は R8・R9 の後、i03〜i05 は R1〜R5 の後。

| # | 答え（i） |
| --- | --- |
| R1 | /sbin/passkey は passkey-fido2 に渡す操作（ENROLL・REMOVE・AUTH/UNLOCK の fido2、KEY*、KEYOWNER）の間 SIGTERM・SIGHUP・SIGPIPE を無視して passkey-fido2 の終わりまで待つ。passkey-fido2 は assert・make の間も TERM を flag で受け、helper の答えを最大 1.5 s 待ってから helper を KILL して終わる（reset を送った後は答えまで止める）。helper は TERM で flag を立て、KEEPALIVE の callback で `pk_hid_cancel`。sessiond の猶予 2 s で KILL された時だけ「may or may not」。ENROLLED の問い直しは答えの後（i03） |
| R2 | KEYOWNER は policy に触れない（数えない・消さない・`signed_in` を立てない・遅れ無し・failed の syslog 無し）。`auth_parse` は KEYOWNER を greeter と session に許し、session は name を owner に固定（i05） |
| R3 | **ユーザーの決定（2026-10-10、クリック「置きっ放しもタッチ」）**: reader に載せたままの NFC の card もタッチと見なす。assert は USB の鍵の後に reader に在る card に問い、どの鍵も答えない時だけ `status touch` を出して当てるのを待つ。PIN 不要の人は誰も触らずに login・解除できる → Settings の警告（PIN 不要・タッチ不要を入れる時）、security.md、release notes に書く（i02 で assert、i04 で警告、i05 で greeter は今の設計のまま） |
| R4 | AUTH・UNLOCK の fido2 は前もって数えず、答えの語で数える: `bad-secret`（鍵の PIN の誤り）・`cloned`・署名の不一致は数える、`timeout`・`canceled`・`no-key`・鍵の抜けは数えない。鍵の PIN は鍵が数える（8 回、電源ごと 3 回）ので総当たりは開かない、と security.md に（i04） |
| R5 | sleep の前に、待っている fido2 の request（Settings・greeter・lock のどれでも）に CANCEL。sessiond は CANCEL・deadline で終わり答えを読んでいない exchange の失敗を遅れ無しで返す。lock は答えを待たずに画面を出し、POWER だけが待つ。card を閉じる処理は sleep の側（lock 済みの sleep は `kwl_lock` を通らない）（i03・i05） |
| R6 | AUTH・UNLOCK の写しで `pin-required`・`cloned` はそのまま通す（greeter・lock が行を持つ）（i04） |
| R7 | `status verified` は passkey-fido2 が password を確かめた直後に出す。verified の後の失敗は数えず、遅れと failed の syslog も通さない（i03） |
| R8 | kernel の関数は `drv_smartcard_card`（`usb_ccid_worker` の thread から呼ばれ、`smartcard_post` を spinlock の外で呼べる）。slot の ADD・REMOVE でも `keys_changed`。`include/uapi/system.h` の USB の注釈 1 行の追記（UAPI の layout は不変）も Q1 の許しの範囲に。volumed が NFC のたびに scan する件は Q1 へ（i02） |
| R9 | 電源・SELECT の失敗の slot は `pk_os_card_power_off` で claim を放す（attach は保つ）。当てるのを待つのは USB の鍵が 1 本も答えない時だけ。KEYOWNER・KEYINFO は待たない。reader の電源の入り切りが card の出入りに見えるかは 5330 で確かめ、要れば backend が短い間の CHANGE を無視する（i02） |
| R10 | KEYOWNER の 1 秒の間隔と「最後の 1 つ」は greeter（compositor）が持ち、sessiond は 1 秒に 1 回を越えたら `ERROR busy`（i05） |
| R11 | i01 で実装と記録（§3.2 の API、§8 の file、`KL_SYSTEM_HAS_KEYS`・`_PIN`、5 本で Add を灰色、名前の前後の空白、2 分の idle は main loop の timeout に `se_dialog_wait`） |
| R12 | Q1 の記録 |

## 進み

| 日 | i | 内容 | 検証 |
| --- | --- | --- | --- |
| 2026-10-10 | i01 | dialog.c（popup）、Security Keys の頁（Software Security Key の Set/Change/Remove を password → PIN 2 回の popup に、鍵の一覧と Remove（password の popup）、Add Key の 4 step: Password → Name（「Security Key」か空いた「Security Key N」、前後の空白を除く）→ Key PIN → Touch → Done）、鍵が 5 本なら Add を灰色、Users の頁は Security Keys への link の card、`locked` の語の行、2 分の idle で password を消して step 1 へ。i01 では Change PIN・Reset Key の button は出さない（i03）。「Not available」は `KL_SYSTEM_HAS_KEYS`・`KL_SYSTEM_HAS_PIN` で判定 | zedBSD の build（settings、warning 0）、host の renderer（plan/ws089/tests/host-build.sh の objects と、scratch の stub で add・remove・PIN の成功・失敗の答えを流し、PNG を目で確認: 頁、各 step、busy、bad-secret・locked の 2 行の誤り、Esc で閉じる）、style-check（変えた所） |
| 2026-10-10 | i02（userland の分） | BUG-286: libpasskey に slot の API（`pk_os_list_slots`・`pk_os_card_attach`・`_present`・`_event`・`_select`（電源と FIDO の applet、通らなければ電源を切って claim を放す）・`_power_off`、Linux は stub）。passkey-fido2 は全部の slot を root で attach して helper に渡す。helper: login は USB の鍵が 1 本も答えない時だけ `touch` を出して試みの間に来た card を待つ（reader に在る card は使わない、R3 の推し）、登録は在る card を 1 本と数え（USB と合わせ 2 本以上は `many-keys`）、無ければ当てるのを待つ。security.md に NFC の段落。kernel の card の事象（§5.3）と backend の `keys_changed` は Q1 の許し待ち | zedBSD の build（passkey-fido2・fidoctl、warning 0）、host 試験 plan/ws172/tests/fido2-host-test.sh・plan/ws161/tests/fidoctl-host-test.sh・libpasskey-host-test.sh PASS、style-check。5330 の NFC の UAT は未実施 |
| 2026-10-10 | i02（kernel・R3） | R3 の決定（置きっ放しもタッチ）で login は reader に在る card にも問う。src/drivers/generic/smartcard.c の `drv_smartcard_card` が card の出入りで `KERN_SYSTEM_EVENT_USB` の CHANGE（detail に `card=0|1`）を出す（Q1 の許し、UAPI の layout 不変、system.h の注釈 1 行）。security.md と release notes に置きっ放しの鍵の注意。plan/ws089/tests/host-kl-system.c に kl_system_printers_edit の stub、fido2-p003-guest.sh の段 4 を security-keys の頁に（Q1 の指示） | vmunix の build warning 0、ccid-proto・fido2 の host 試験 PASS。backend の `keys_changed` は i03 で |
| 2026-10-10 | i03 | libpasskey `pk_ctap2_reset`。passkey の `key-info`・`key-set-pin`・`key-change-pin`・`key-reset`（fido2 の操作の間 TERM・HUP・PIPE を無視、R1）。passkey-fido2: TERM を flag で受けて helper に渡し 1.5 s 待つ、helper は TERM で KEEPALIVE の時に CTAPHID_CANCEL、`info`・`done`・`reset MASK` の message、reset は password → `status verified` → `status replug`（USB は 0→1 本、NFC は card の INSERTED）→ 1.5 s の間に全 account の credential を silent に問う → authenticatorReset → 持っていた行を消す、語の分け（bad-key-pin・key-locked・key-replug・no-pin・pin-required・pin-policy・not-allowed・canceled）。sessiond: KEYINFO・KEYPIN・KEYRESET（policy に触れない・verified で数えを消す・その後の失敗はすぐ）、REPLUG、AUTH/UNLOCK だけ語を写す、CANCEL・deadline の timeout は遅れ無し（R5）。backend: KEYINFO・KEYOP・REPLUG、events-zedbsd の USB 購読と `keys_changed`（FIDO の hidraw・smartcard）。compositor: account の key_info・key_pin・key_reset・key_cancel（版 25）、lock 中は EBUSY、lock・sleep の前に CANCEL、unlock と鍵の出入りで keys_changed。libkeiland KL_VERSION 77。Settings: Add（Insert で KEYINFO、鍵の名前を既定に、PIN の無い鍵は Set PIN → そのまま登録、鍵の PIN の誤りは password を保って PIN の step へ）、Change PIN、Reset Key（Warning → Password → Replug → Touch → Done、Cancel は鍵に）、Done は Cancel 無し。security.md に段落。plan/ws132/tests/p003-guest.sh の購読の class を 0xaf に | zedBSD の build（passkey・passkey-fido2・fidoctl・sessiond・wayland・settings）warning 0。host 試験: plan/ws199/tests/sessiond-keys-host-test.sh（新）・plan/ws172/tests/sessiond-auth-host-test.sh・fido2-host-test.sh・passkey-host-test.sh・plan/ws161/tests/fidoctl-host-test.sh PASS。Settings の host の renderer で各 wizard の PNG（build/p1-ws199/ops-*.png）。5330 の実機・QEMU は未実施 |

## Event

- 2026-10-10: 第 1 版（5903f7548）。第 2 版（63db6b7bc）。design-reviewer → [review-1.md](review-1.md)。第 3 版（2dc99e171）。
- 2026-10-10: design-reviewer の再確認 → [review-2.md](review-2.md)（i01 GO、i02 は N1〜N5 を直してから）。
- 2026-10-10: ユーザーの答え（タッチ不要は unlock だけ、login は常にタッチ、keypad、全部ベータ2、他は推し）と BUG-286（NFC）。fidoctl の `-s` と list の直し（aead021aa）。
- 2026-10-10: 第 4 版（P1、この文書）。
