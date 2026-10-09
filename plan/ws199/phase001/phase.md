<!-- awesome-plan project=zedbsd record=ws199-p001 -->

# ws199-p001: セキュリティキーの頁・ウィザード・ログインの鍵のモード（設計）

Phase ID: `ws199-p001`
Parent: [WS199](../ws.md)
Status: planning（2026-10-10 P1: 第 3 版。[review-1.md](review-1.md)（blocker 2・major 12・minor 15）に答えた（末尾の §9）。ユーザーの判断 J1〜J6 を Q1 経由で尋ね中。code は書かない）
Phase disposition: normal
Queue: q921（P1、2026-10-10）
依存: ws172-p002・p003（PIN と鍵の今の経路）、docs/architecture/security.md「Login authentication」、ws187（lock の画面）
所有 path: `userland/desktop/settings/`、`userland/desktop/libkeiland/system/`、`userland/desktop/include/keiland/keiland.h`、`userland/desktop/wayland/`（greeter.c・system.c）、`userland/desktop/libkeiland-backend*/`、`userland/desktop/sessiond/`、`userland/base/passkey/`、`userland/base/passkey-fido2/`、`userland/base/libpasskey/`、`docs/architecture/security.md`、`plan/ws199/`

版: 2026-10-10 第 1 版（P1、5903f7548: 頁とウィザードだけ）→ 第 2 版（63db6b7bc、ログインの鍵のモード、PIN 不要・タッチ不要、Software Security Key、user の自動の選択）→ 第 3 版（review-1 の直しを §9 に。§2〜§8 の本文で §9 と食い違う所は §9 が優先）。

## 0. ユーザーの決定（ws.md、2026-10-10）

- ログイン画面で鍵が挿さっていれば自動で鍵のログインのモード。鍵を挿すと、その鍵の credential の持ち主の user を自動で選ぶ。
- Settings に「ログインに鍵の PIN を要らない」「ログインに鍵のタッチを要らない」。組み合わせは 3 通り: **PIN＋タッチ（既定）・タッチだけ・どちらも不要**（タッチ不要は PIN 不要が前提）。入れる時は password と警告「鍵を持つ人は誰でも login できる」。
- タッチ不要の時は「鍵を確かめています」を最低 0.5 秒出してから desktop へ。PIN 不要ならタッチを促す。PIN が要れば PIN とタッチ。PIN の入力は画面の keyboard を欄のすぐ下に出す。
- **lock の画面のタッチ不要は、lock の後に挿した鍵だけ**（挿しっぱなしならタッチを促す）。
- Settings に「Security Keys」の頁。物理の鍵の無い今の PIN（/etc/passkey の pin）も残し、同じ頁で「Software Security Key」として管理。
- passkey・passkey-fido2・fidoctl の分け方は今のまま。console・su・sudo・SSH は password だけ。

## 1. 今（調べ）

- 経路（変えない）: Settings・greeter（特権なし）→ compositor → backend → sessiond（root、常駐、失敗の数え・遅れ）→ `/sbin/passkey`（root、短命、password と PIN、/etc/passkey）→ `/usr/libexec/passkey-fido2`（鍵の challenge と検査）→ device helper（`_passkey`、chroot、CTAP）。
- passkey の操作: `styles`、`enrolled`、`auth`、`enroll-pin`・`remove-pin`、`enroll-fido2`、`remove-fido2`。鍵の情報・鍵の PIN の設定と変更・初期化・鍵の持ち主の問い合わせ・login の選択（PIN・タッチ）は**無い**。
- libpasskey: `pk_ctap2_get_info`（clientPin の有無）、`pk_ctap2_pin_retries`、`pk_ctap2_set_pin`・`change_pin`、assertion の要求に `presence`（0 で up=false）と PIN token の有無がある。authenticatorReset は定数だけ。`pk_os_list` は鍵の node（`/dev/input/hidrawN`）・vendor・product・名前。NFC は `/dev/smartcardN`。
- passkey-fido2 の検査（security.md「Logging in」の 2）は UP と UV の flag を必須にしている。
- kernel は USB の付け外しを `/dev/system` の `KERN_SYSTEM_EVENT_USB`（subject `usbB.A`、detail に vendor・product、smartcard の card の出入りも同じ class）で知らせる。
- greeter・lock は今の画面の keyboard（keyboard.c）を出さない（`server->greeter`・`locked` で閉じる）。
- Settings に modal の popup の部品は無い。

## 2. 鍵の設定（account ごと）と /etc/passkey

- 新しい行の種類 `<name>:<uid>:fido2-options:pin=<0|1>:touch=<0|1>`（account に 1 行。無ければ pin=1・touch=1）。`touch=0` は `pin=0` の時だけ正しい（3 通り）。知らない種類の行は今の規則で残るので、古い passkey は読み捨てる（security.md の /etc/passkey の規則）。
- 変更は新しい操作 `set-fido2-options`（password と pin・touch）。account の password で確かめる。鍵の登録が無い account は `not-enrolled`。
- 鍵の PIN を要らない時: assertion を PIN token 無しで求め、UV の flag を求めない。鍵の credential は今 credProtect を付けずに作るので UV 無しの assertion を出せる（CTAP2.1 の `alwaysUv` の鍵は出さない: その時は `pin-required` で PIN の欄に戻る）。
- タッチを要らない時: assertion を `up=false`（`presence` 0）で求め、UP の flag を求めない。**sessiond が「今はタッチ不要を許す」と passkey に言った時だけ**（§4.3）。

## 3. 画面

### 3.1 Settings の頁「Security Keys」（sidebar で Users の次）

- **Software Security Key**: 今の 6 桁の PIN（Users の頁の PIN の card を移す）。状態（Set／Not set）と Set・Change・Remove（今の `kl_system_account_set_pin`）。
- **Security keys**: 登録済みの鍵の名前の一覧、各行に Remove。button **Add Key**、**Change PIN**、**Reset Key**。
- **Sign in with a security key**: 3 つの radio「PIN and touch（既定）」「Touch only」「No PIN, no touch」。鍵の登録が無い時は灰色。今より弱い方へ変える時は popup（警告「Anyone who has your security key can sign in to this computer.」と password）。強い方へは password だけ。
- Users の頁の鍵と PIN の card は消し、「Security Keys」への link。

### 3.2 popup（Settings の窓の中の modal、新 `dialog.c`）

- 窓の中央の card、下の頁は暗くして入力を受けない。題、本文、欄（0〜2）、button（Back・Cancel・主の button）、下の小さな link（任意）、step n/m。
- **busy**: 処理中は spinner と言葉、欄と button は灰色で押せない。触れる待ちの間だけ Cancel は押せる（→ passkey の `fail timeout`）。Esc は busy でない時だけ Cancel。秘密の欄は閉じた時・送った時に消す。
- WS200 の 2 つのウィザードも同じ部品。

### 3.3 Add Key のウィザード

1. **Password**（登録は password で確かめる、security.md）。
2. **Insert**: 「Insert your security key (or hold it to the reader).」sessiond の `KEYINFO`（§4.1）を 1 秒ごと（popup が開いている間だけ）。0 個は待つ。2 個以上は「Remove the other keys: only one at a time.」。1 個: 名前と「PIN set／No PIN」。
3. **Name**: 既定は鍵の名前（無ければ「Security Key」、同じ名前が登録済みなら「… 2」）、編集可（1〜32 byte、`:` と制御文字は不可）。
4. 鍵に PIN が無い時だけ **Set PIN**（4〜63 文字を 2 回）→ busy → 成功で 6 へ（今入れた PIN を使う）。
5. **PIN**: 下に link「Forgot the PIN?」→ Reset Key へ。残り回数が 3 以下なら出す。
6. **Touch**（busy「Touch your security key.」、Cancel 可）→ 7 **Done**。
- 失敗の言葉: `bad-secret`（1 か 5 に戻る）、`key-locked`（Reset を案内）、`many-keys`、`no-key`、`timeout`（6 からやり直し）、他は語を添えて。

### 3.4 Change PIN のウィザード

1. **Insert**（PIN の無い鍵は Set PIN へ）。2. **PIN**: 今の PIN、新しい PIN を 2 回 → busy → Done。password は要らない（鍵の PIN は鍵が自分で確かめる。第 1 版の D1）。

### 3.5 Reset Key のウィザード

1. **Warning**（鍵の全部の credential と PIN が消える、他の機械・web の分も）。2. **Password**（この機械の login の鍵を消すため）。3. **Replug**: 「Unplug the key, plug it back in, then press Reset within 10 seconds.」（CTAP2 は電源の入りから 10 秒以内だけ受ける）。KEYINFO で 0 個 → 1 個を見て Reset を有効にし 10 秒を数える。NFC は「Take the key away and hold it again」。4. **Touch** → 5. **Done**「Add it again to sign in with it.」この機械の、その鍵が持っていた登録は消える（第 1 版の D2: reset の前に鍵に問い、成功の後に /etc/passkey の行を消す）。

### 3.6 ログイン画面（greeter）の鍵のモード

- greeter は `/dev/system` の USB の事象を受ける（compositor の既存の system の購読に `KERN_SYSTEM_EVENT_USB` を足す。特権は要らない）。画面が出た時と USB の ADD の事象の時に sessiond に `KEYOWNER`（§4.2）を問う（鍵の node が出来るまで 0.5 秒置いて、出来なければ 1 秒後にもう 1 回）。
- 答えが user なら: その user を選び（一覧に居れば）、鍵のモードに入る。誰の credential も無い鍵は「This security key is not registered here.」を 1 行（password の欄はそのまま）。鍵が抜けたら（USB の REMOVE）password の欄に戻る。
- 鍵のモードの画面は選んだ user の設定（§2、sessiond が `KEYOWNER` の答えに `pin=` `touch=` を添える）で:
  - **どちらも不要**: 「Checking your security key…」を出し、すぐ `AUTH user fido2`（秘密無し）。OK が来ても 0.5 秒経つまで desktop へ移らない（最低の表示時間）。
  - **タッチだけ**: 「Touch your security key.」と `AUTH user fido2`（秘密無し）。
  - **PIN とタッチ**: PIN の欄と、そのすぐ下の keypad（§3.8）。Enter で `AUTH user fido2` と PIN → TOUCH → 「Touch your security key.」
  - どれも下に link「Use your password」（password の欄に戻る。鍵のモードは鍵を挿し直すか link「Use a security key」で戻る）。
- 失敗: `bad-secret`（PIN の誤り）・`timeout`・`cloned`・`pin-required`（`alwaysUv` の鍵: PIN の欄に移る）を 1 行で。sessiond の遅れ（2 秒〜）は今どおり。

### 3.7 lock の画面

- 今の方式の pill（ws187-p003）に加え、§3.6 と同じ鍵のモード。user は session の user で決まっているので `KEYOWNER` は「この鍵がこの user の物か」を確かめるだけ。
- **タッチ不要は lock の後に挿した鍵だけ**: sessiond が決める（§4.3）。lock の時から挿しっぱなしの鍵は「タッチだけ」と同じ画面（タッチを促す）。
- lock の後に鍵を挿すと card が出て（swipe 無しで）鍵のモードに入る。

### 3.8 PIN の keypad（greeter・lock）

- 画面の keyboard（keyboard.c の panel）は端に付く panel で、greeter・lock では閉じる作りなので使わない。greeter が自分で描く小さな keypad を PIN の欄の**すぐ下**に出す: 数字 3×4（1〜9、0、Backspace、Enter）と「ABC」で英字の 3 段（鍵の PIN は 4〜63 文字の任意の文字。英字の段は小文字・大文字の切り替え）。物の keyboard も今どおり効く。
- Software Security Key（6 桁の PIN）の欄にも同じ keypad（数字だけ）を出す。

## 4. sessiond と passkey の操作

### 4.1 KEYINFO（Settings のウィザード）

- session の request `KEYINFO` → passkey `key-info`（秘密無し）→ passkey-fido2 が鍵を数え、1 個なら名前・PIN の有無・PIN の残り回数（PIN を送らない）。答え `KEYINFO count=N [name=<hex> pin=0|1 retries=N]`。失敗を数えない。

### 4.2 KEYOWNER（greeter・lock）

- greeter の request `KEYOWNER` → passkey `key-owner`（秘密無し）→ passkey-fido2 が /etc/passkey の全部の account の fido2 の credential（account ごと、`zedbsd.login` の rp）で、鍵に up=false・UV 無しの assertion を問う（今の「どの鍵がこの account の credential を持つか」の問いと同じ silent の問い、security.md「Choosing the key」）。持ち主の account（uid 1000 以上、password が locked・expired でない）の名前と、その account の `fido2-options` を答える: `KEYOWNER user=<name> pin=0|1 touch=0|1`、無ければ `KEYOWNER none`、鍵が 2 個以上は `KEYOWNER many`。
- 開示: login 画面で鍵を挿すと持ち主の名前が分かる（その鍵を持つ人に）。ログイン画面は既に user の一覧を出しているので新しい開示は「どの user の鍵か」だけ。lock の画面の `KEYOWNER` は session の user の credential だけで問う。
- 失敗を数えない。鍵の挿し抜きを繰り返す攻撃で passkey が走り続けないよう、sessiond は `KEYOWNER` を 1 秒に 1 回までにする。

### 4.3 AUTH・UNLOCK の fido2（秘密無し）

- 今の `AUTH user fido2` の後の秘密（鍵の PIN）の行を空で受ける（PIN 不要の account）。passkey の request に新しい field `presence=required|optional`（sessiond が決める）。
- sessiond の決め方:
  - account の `fido2-options` が touch=1 → required。
  - greeter（login）: touch=0 → optional。
  - lock（UNLOCK）: touch=0 で、**sessiond が lock の時刻より後に USB の ADD の事象（`/dev/system`、sessiond が自分で購読する）を受けていた**時だけ optional、他は required。事象は鍵か他の機器か分からない（vendor・product は鍵の物と照らす: passkey-fido2 が使った鍵の vendor・product を答えに添え、sessiond が lock の後の ADD の事象と照らす。合わなければ「挿しっぱなし」と同じ扱いで、その試みは required で問い直す）。
- passkey-fido2: 秘密が空なら PIN token 無し（UV を求めない、account の pin=0 の時だけ。pin=1 なのに空なら `bad-request`）。`presence=optional` なら up=false（account の touch=0 の時だけ）。検査の flag も合わせて緩める（UP・UV を account の設定と request の両方が許す時だけ外す）。
- 失敗の数えと遅れは今どおり（鍵の試みとして数える）。

### 4.4 鍵の操作（Settings）

| 操作 | 秘密 | 中身 |
| --- | --- | --- |
| `key-set-pin` | 鍵の新しい PIN | PIN の無い鍵 1 個に setPIN |
| `key-change-pin` | 鍵の今の PIN と新しい PIN | changePIN |
| `key-reset` | account の password | 鍵が account の credential を持つかを問い、authenticatorReset（touch）、成功で持っていた行を /etc/passkey から消す |
| `set-fido2-options` | account の password | §2 の行を書く |

- libpasskey に `pk_ctap2_reset`（0x07、CTAP2_ERR_NOT_ALLOWED 0x30・OPERATION_DENIED 0x27・USER_ACTION_TIMEOUT 0x2F を語に: `not-allowed`・`denied`・`timeout`）。
- どれも鍵 1 個の時だけ（`many-keys`）。PIN は鍵 1 個にだけ送る（今の規則）。

### 4.5 compositor・backend・libkeiland

- backend: `kl_backend_session_key_info`・`_key_owner`・`_key_set_pin`・`_key_change_pin`・`_key_reset`・`_set_key_options`、`kl_backend_session_authenticate`・`_unlock` は fido2 で空の秘密を受ける。
- compositor: Settings の account の object に request と event（KEYINFO の答え）を足し、protocol の版を上げる。greeter は USB の事象と `KEYOWNER`。
- libkeiland: `kl_system_account_key_info`・`_key_set_pin`・`_key_change_pin`・`_key_reset`・`_key_options_get`・`_set_key_options`。KL_VERSION を 1 つ上げる（番号は merge の時に Q1）。
- Linux・FreeBSD の backend は新しい関数を ENOTSUP。

## 5. docs/architecture/security.md に書く事

- 新しい操作（§4.1〜§4.4）と field、`fido2-options` の行、PIN 不要・タッチ不要の規則と警告（鍵を持つ人は誰でも login できる。console・su・sudo・SSH は password だけのまま）、lock のタッチ不要は lock の後の USB の ADD を sessiond が見た時だけ、`KEYOWNER` の開示と 1 秒の上限、Reset で消える登録。

## 6. 試験

- host: libpasskey の reset（台本の鍵）、passkey の request の解析（新しい操作の field の数、空の秘密、presence）、passkey-fido2 の検査の flag の緩め方（account の設定と request の組み合わせ 3×2、許さない組で UP・UV 無しの assertion を拒む）、sessiond の presence の決め方（lock の時刻と USB の事象の前後、vendor・product の照合）、`KEYOWNER` の 1 秒の上限、Settings のウィザードの状態機械（純粋な関数: step の遷移・既定の名前・PIN の 2 回の照合・busy の入力の拒否）、greeter の鍵のモードの状態機械（0.5 秒の最低の表示、鍵の抜けで戻る）。
- QEMU（T1、AAT）: QEMU の `u2f-emulated` は U2F だけで CTAP2 の PIN が無い見込み（未確認）。無ければ画面は host の偽の鍵の台本（passkey-fido2 の host 試験の偽の鍵）で層ごと、PNG（頁、各 step、busy、greeter と lock の鍵のモード 3 通り）をユーザーに。
- 5330 の UAT（YubiKey 5、NFC）: 3 通りの login、lock の挿しっぱなしと挿し直し、Add（PIN の有る鍵・無い鍵）、Change PIN、Reset、Remove、Software Security Key。

## 7. 実装の順（小さく merge）

| i | 内容 |
| --- | --- |
| i01 | Settings の popup の部品と頁「Security Keys」（今の操作だけ: 一覧・Remove・Add（今の 3 欄をウィザードに）・Software Security Key）、Users の頁の link |
| i02 | libpasskey の reset、passkey・passkey-fido2 の `key-info`・`key-set-pin`・`key-change-pin`・`key-reset`、sessiond の KEYINFO と操作、backend・compositor・libkeiland、ウィザードの Insert・Set PIN・Change PIN・Reset |
| i03 | `fido2-options` と `set-fido2-options`、Settings の 3 つの radio と警告、passkey-fido2 の UV・UP の緩め（presence の field）、sessiond の presence の決め方と USB の事象の購読 |
| i04 | `KEYOWNER`、greeter の USB の事象、鍵のモード（3 通り、0.5 秒）、keypad、lock の画面の鍵のモード |
| i05 | security.md、host 試験の残り、T1 の AAT の依頼、style-check |

## 8. 危険と未確認

| 項目 | 内容 |
| --- | --- |
| up=false の assertion | 鍵によって up=false を断る（CTAP2.0 の一部）。断られたら「タッチだけ」と同じ画面に落とす。5330 の YubiKey で確かめる |
| UV 無しの assertion | `alwaysUv` の鍵、credProtect の鍵は UV を求める。`pin-required` で PIN の欄へ |
| lock の後の鍵の判定 | USB の事象の vendor・product の照合は近似（同じ型の別の機器の抜き差しでも「挿し直し」と見る）。同じ型の鍵を 2 本持つ人が 1 本を抜き差しし、もう 1 本が挿しっぱなしの時は 2 本で `many` なので試みない |
| KEYOWNER の負荷 | 鍵 1 本・account 数 × credential 5 本の silent の問い。1 秒の上限 |
| QEMU の CTAP2 の模擬 | 無い見込み。画面の確かめは偽の鍵の台本で |
| 見積もり | i01 3、i02 5、i03 4、i04 5、i05 2、計 **約 19 LW** |

## 9. 第 3 版: review-1 への答え（§2〜§8 より優先）

### 9.1 ユーザーの判断（Q1 経由で尋ね中、推しは太字）

| # | 判断 | 推し |
| --- | --- | --- |
| J1（B2） | タッチ不要の自動の login: greeter が出るたびに挿しっぱなしの鍵で自動で送ると、Log Out・別の user・Restart/Shut Down が使えず、起動で落ちる session は繰り返す | **自動で送るのは greeter が出た後に挿した（挿し直した）鍵だけ。挿しっぱなしの鍵は鍵のモードの画面を出し「Sign in」を 1 押し** |
| J2（B1） | lock の「lock の後に挿した鍵」 | **§9.4 の作りで作る**（compositor が lock を sessiond に知らせ、FIDO の hidraw の追加の事象で判定、復帰の直後は数えない、その node だけに up=false）。無理なら lock ではタッチ不要を使わない |
| J3（m3） | PIN の keyboard | **greeter が欄のすぐ下に描く小さな keypad**（今の画面の keyboard は端の panel で greeter・lock では閉じる作り）。非 ASCII の PIN は物の keyboard だけ |
| J4（M9） | WS200 の Sign-in Methods との重なり | **/etc/passkey の 1 行の options（methods と鍵の pin・touch）にまとめ、両方の頁が同じ設定を読む。popup の部品は WS199 i01 で作り WS200 が使う** |
| J5（m5・m6） | 第 1 版の D1〜D4、頁の一覧は自分の鍵だけ | **推しどおり**、一覧は session の user の鍵だけ |
| J6 | ベータ2 の範囲（10/13 の凍結） | **i01・i02 まで**（頁、Software Security Key、Add・Change PIN・Reset のウィザード、鍵の情報）。i03・i04（PIN 不要・タッチ不要、鍵の自動のモード、user の自動の選択）は 5330 の YubiKey で up=false・UV 無し・復帰の再列挙を先に確かめ、ベータ3 か凍結の後 |

### 9.2 範囲と Phase の分け方（m14）

- NFC（reader に当てる鍵）は鍵のモード・ウィザードの範囲の外（M4: passkey-fido2 は hidraw だけを開き、kernel は card を当てた事象を出さない）。画面の言葉から NFC を消す。
- Phase の分け方を Q1 に頼む: p001 設計（この文書）、p002 = i01、p003 = i02、p004 = i03・i04（J6 の答えで時期）、p005 規約の全文の見直し、p006 T1 と 5330 の UAT。
- 依存（m15）: i04 は ws172-p002（PIN の login、T1 待ち）・ws172-p007（greeter の方式の選択、test-wait）・ws187-p003 が cleared になってから。ws172 の ws.md の表の p003 の食い違いは Q1 へ。
- security.md（と keiland.md・security-keys.md）は各 i の code の前に書く（M11）。

### 9.3 i01・i02 の直し（ベータ2 の範囲）

- **鍵の情報の問い（M3）**: 1 秒ごとの poll をやめる。popup が開いた時に 1 回 `KEYINFO`、その後は compositor が FIDO の hidraw（`/dev/system` の INPUT、detail の `usage=f1d0:0001`、backend の events-zedbsd.c が既に INPUT を購読している）の ADD・REMOVE を見て `KL_SYSTEM_CHANGED_KEYS`（新）を Settings に出し、Settings はその時だけ `KEYINFO` を問う。lock の間（session の socket を UNLOCK が使う）は問わない。
- **失敗の語（M5）**: helper の CTAP の status を語に分ける: 0x31 → `bad-key-pin`（鍵の PIN の誤り、password の `bad-secret` と別）、0x32 → `key-locked`（Reset だけが戻す）、0x34 → `key-replug`（抜いて挿し直せば戻る。Reset に導かない）、0x35 → `no-pin`、0x36 → `pin-required`、0x37 → `pin-policy`（その鍵の PIN の規則に合わない）、0x30 → `not-allowed`。sessiond の語の表（`auth-policy.c`）に足し、Settings の request にはそのまま返す（greeter の AUTH は今の語のまま: 鍵の PIN の誤りは今どおり `bad-secret` と見せる）。ウィザードは `bad-secret` で Password の step、`bad-key-pin` で PIN の step へ戻る。
- **新しい操作の表（M6）**:

| request（Settings の session だけ） | passkey の操作 | 秘密 | 数え | 時間 |
| --- | --- | --- | --- | --- |
| `KEYINFO` | `key-info` | 無し | 数えない | 5 s |
| `KEYPIN set` | `key-set-pin` | 鍵の新しい PIN | 数えない（鍵が数える） | 10 s |
| `KEYPIN change` | `key-change-pin` | 鍵の今の PIN、新しい PIN | 数えない（鍵が数える） | 10 s |
| `KEYRESET` | `key-reset` | account の password | password の試みとして数える | 60 s（replug 30 s、touch 30 s） |

  greeter からは送れない（今の ENROLL と同じ規則）。どれも一度に 1 つ（今の socket の規則）。
- **Reset を 1 回の実行に（M7）**: ウィザードは Warning → Password → `KEYRESET` を送る。passkey が password を確かめ（誤りは今の遅れの後に `bad-secret`）、passkey-fido2 が挿さった鍵 1 本に account の credential の有無を silent に問い（持っていた credential の ID を覚える）、`status replug` を出して鍵が抜けて挿し直されるのを 30 s まで待ち（`pk_os_list` を 100 ms ごと）、現れたらすぐ authenticatorReset を送り（10 秒の窓の中）、`status touch`、成功で覚えた credential の行を /etc/passkey から消して ok。`status replug` は sessiond → backend → compositor → libkeiland で `KL_SYSTEM_CHANGED_REPLUG`（新）として Settings に届き、画面は「Unplug the key, then plug it back in.」。
- **取り消し（m2）**: `kl_system_account_key_cancel`（今の `kl_backend_session_cancel` → sessiond の CANCEL）。i01 の Add は今の API のままで Cancel を出さない（touch の待ちの 30 s で終わる）。i02 で Touch と Replug の step に Cancel。`KL_SYSTEM_HAS_KEY_OPS`（新）で古い compositor は i02 の button を出さない。
- **PIN の長さ（m4）**: 鍵の PIN は鍵の minPINLength（`KEYINFO` の `min=`、無ければ 4）以上の code point、63 byte 以下（UTF-8）。Software Security Key は今の 6 桁。
- **options の読み出し（M12）**: i02 では options を作らない（i03）。i03 で作る時は: `enrolled` の答えに `options=` を足して読む、最後の鍵を消したら options の行も消す、重複・不正な行は一番強い設定（PIN とタッチ）として読む。
- **Linux・FreeBSD（m11）**: Settings の Makefile.linux・Makefile.freebsd に新しい file を足し、鍵の操作は `KL_SYSTEM_HAS_KEY_OPS` が無いので「Not available on this system」を出す。backend の新しい関数は ENOTSUP。
- **試験（M10）**: libpasskey の host 試験（`plan/ws161/tests/libpasskey-ctap2-host-test.c`）の software authenticator に reset（10 秒の窓、touch）と新しい status を足す。passkey の request の解析（新しい 4 つの操作の field の数）、helper の語の表、passkey-fido2 の wire の試験（`plan/ws172/tests/fido2-wire-host-test.c`）に replug と touch の status、Settings のウィザードの状態機械（純粋な関数）。QEMU に CTAP2 の鍵は無い（ws172-p003 のユーザーの決定で QEMU の鍵の経路は作らない）ので、画面は鍵の無い状態（Insert の待ち、Software Security Key、一覧）を T1 の AAT で PNG に、鍵の操作は 5330 の UAT（YubiKey 5: Add の PIN 有り・無し、Change PIN、Reset、`key-replug` の再現、Remove）。

### 9.4 i03・i04 の直し（J6 の答えの後の範囲、ここでは方針だけ）

- **lock の判定（B1、J2）**: compositor が lock・unlock の時に sessiond へ `LOCKED`・`UNLOCKED`（新しい session の行）。sessiond は既存の `/dev/system` の購読（`seat.c`）で FIDO の hidraw の ADD（`usage=f1d0:0001`）の node 名と時刻を記録し、sleep からの復帰（sessiond が受ける `sleep.end`）の後 5 秒の ADD は数えない（再列挙）。UNLOCK の fido2 の request に「lock の後に ADD した node」の一覧を付け、passkey-fido2 はその node の鍵にだけ up=false を許す（他の node は up=true）。
- **request の形（M8）**: `auth` の field の数を変えず、新しい操作 `auth-fido2`（name、鍵の PIN（空可）、presence、node の一覧）。
- **KEYOWNER（M1・M2）**: silent の答えの署名を確かめる（credential の公開鍵で）。署名の無い・合わない答えは持ち主と見ない。同じ鍵が 2 つ以上の account に登録されていれば `KEYOWNER many-owners`（選ばない）。allowList は鍵の `maxCredentialCountInList` ごとに分けて問う。name の無い request は passkey の request の規則に新しい形で（`key-owner` は name の field を `-`）。sessiond の uid の照合は `KEYOWNER` では行わない（答えの名前から sessiond が passwd で引き直す）。
- **自動の試みの数え（M6）**: 自動で始めた鍵の試みが取り消し・時間切れ・鍵の抜けで終わった時は数えない（その鍵が答えて署名が合わなかった時だけ数える）。
- **WS200 との 1 行（J4、M9）**: `<name>:<uid>:options:methods=password,pin,fido2:key-pin=<0|1>:key-touch=<0|1>`。Security Key の方式が外れていれば鍵のモードに入らない。
- **0.5 秒（m1）**: greeter が「Checking…」を出した時刻から 0.5 秒経つまで display を返さない（session の READY の後の RELEASED を遅らせる）。
- **m7〜m10、m12、m13**: up=false を断る鍵は今も使えない（危険の表を直す）、KEYOWNER は sessiond で最後の 1 つにまとめて遅らせる、鍵の node の REMOVE だけで戻り AUTH の途中なら CANCEL、user を選び直す時は打ちかけの password を消す、鍵の事象は seat.c の流れ 1 つで、PIN もタッチも不要の login も `signed_in` を立てる（security.md に書く）。

## Event

- 2026-10-10: 第 1 版（P1、5903f7548）。
- 2026-10-10: 第 2 版（P1、63db6b7bc）。ユーザーの仕様の変更（ログインの鍵のモード、PIN 不要・タッチ不要、Software Security Key、user の自動の選択）と決定を入れた。第 1 版の D1〜D4 は推しどおり（D1 Change PIN は鍵の PIN だけ、D2 Reset で消えた登録は消す、D3 独立の頁、D4 password は最初）として扱う（ユーザーの「全部推し」の範囲かは Q1 に確かめる）。
- 2026-10-10: design-reviewer（agent ad183bc29369c7a4e）→ [review-1.md](review-1.md)（blocker 2・major 12・minor 15。i01 は軽い直しで GO、i02〜i05 は直してから）。
- 2026-10-10: 第 3 版（P1）。§9 に全部の答え。ユーザーの判断 J1〜J6 を Q1 へ。
