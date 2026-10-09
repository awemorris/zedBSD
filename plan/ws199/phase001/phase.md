<!-- awesome-plan project=zedbsd record=ws199-p001 -->

# ws199-p001: Settings のセキュリティキーの頁とウィザード（設計・実装・host 試験）

Phase ID: `ws199-p001`
Parent: [WS199](../ws.md)
Status: planning（2026-10-10 P1: 画面の流れと層の設計の第 1 版。Q1 へ要点を送り、ユーザーの判断 D1〜D4 の答えの後に実装）
Phase disposition: normal
Queue: q921（P1、2026-10-10）
依存: ws172-p003（今の鍵の登録の経路）、docs/architecture/security.md「Login authentication」
所有 path: `userland/desktop/settings/`（新 page-keys.c・dialog.c、page-users-keys.c を移す）、`userland/desktop/libkeiland/system/`（account の API）、`userland/desktop/include/keiland/keiland.h`、`userland/desktop/wayland/system.c`、`userland/desktop/libkeiland-backend*/`（session の鍵の API）、`userland/desktop/sessiond/auth.c`、`userland/base/passkey/`、`userland/base/passkey-fido2/`、`userland/base/libpasskey/ctap2.c`、`docs/architecture/security.md`、`plan/ws199/`

## 1. 今（調べ）

- Settings の Users の頁の中の「Security keys」の card（`page-users-keys.c`）: 登録済みの鍵の一覧と Remove、3 つの欄（今の password、鍵の名前、鍵の PIN）と Add Security Key。
- 経路（変えない、security.md）: Settings（特権なし）→ libkeiland `kl_system_account_add_key`・`remove_key`・`keys`・`touched` → compositor（`wayland/system.c` の account の object）→ backend `kl_backend_session_add_key`・`remove_key`・`enrolled`・`cancel` → sessiond（`auth.c`、request を passkey へ）→ `/sbin/passkey`（root、request.c）→ `/usr/libexec/passkey-fido2`（鍵の操作）→ device helper（`_passkey`、chroot、CTAP）。
- passkey の操作: `styles`、`enrolled`、`auth`、`enroll-pin`・`remove-pin`、`enroll-fido2`（label、鍵の PIN、password）、`remove-fido2`。鍵の情報（挿さっている鍵、PIN の有無）、鍵の PIN の設定・変更、鍵の初期化は**無い**（fidoctl に set-pin・change-pin はあるが端末の root の道具）。
- libpasskey: `pk_ctap2_get_info`（clientPin の option で PIN の有無）、`pk_ctap2_pin_retries`、`pk_ctap2_set_pin`、`pk_ctap2_change_pin` はある。authenticatorReset（0x07）は定数だけで関数が無い。`pk_os_list` は鍵の node・vendor・product・**名前**（USB の product の文字列）を返す。
- Settings に modal の popup の部品は無い（WS200 の 2 つのウィザードも同じ部品を使う）。

## 2. 画面

### 2.1 頁「Security Keys」（sidebar で Users の次、`SE_PAGE_KEYS`）

- 登録済みの鍵の一覧（名前。今の `kl_system_account_keys`）、各行に Remove。
- button: **Add Key**、**Change PIN**、**Reset Key**。
- 鍵の機能の無い desktop（`KL_SYSTEM_HAS_KEYS` が無い）は今の言葉。
- Users の頁の card は消し、「Security Keys」への link を置く（PIN の card は Users に残す）。

### 2.2 popup（Settings の窓の中の modal、新 `dialog.c`）

- 窓の中央の card、下の頁は暗くして入力を受けない。題、本文、欄（0〜2）、button（Back・Cancel・Next/主の button）、下の小さな link（任意）、進み（step n/m）。
- **busy**: 処理中（鍵への問い合わせ、触れる待ち、passkey の答え待ち）は spinner と言葉（「Checking…」「Touch your security key.」）、欄と button は灰色で押せない。触れる待ちの間だけ Cancel は押せる（`kl_system_account_cancel` → passkey の `fail timeout`）。
- Esc は busy でない時だけ Cancel。秘密の欄は閉じた時と送った時に消す（今の規則）。
- WS200 の Change Password と Sign-in Methods も同じ部品。

### 2.3 Add Key のウィザード

1. **Password**: 「Enter your password to add a security key.」今の password（登録は password で確かめる、security.md）。
2. **Insert**: 「Insert your security key (or hold it to the reader).」1 秒ごとに鍵の情報を問う（§3 の `key-info`）。0 個: 待つ。2 個以上: 「Remove the other keys: only one at a time.」（`many-keys`、security.md の「Registering」）。1 個: 名前（USB の product の文字列）と「PIN set／No PIN」を出し Next。
3. **Name**: 既定の名前（鍵の名前、無ければ「Security Key」、同じ名前が登録済みなら「… 2」）を入れた欄、編集可（1〜32 byte、`:` と制御文字は不可、今の規則）。
4. 鍵に PIN が**無い**時だけ **Set PIN**: 「This key has no PIN. Choose one (4 to 63 characters).」新しい PIN を 2 回 → busy → 成功で 5 へ（PIN は今入れた物を使う。5 を飛ばす）。
5. **PIN**: 「Enter the key's PIN.」下の link「Forgot the PIN?」→ Reset のウィザード（§2.5）。残りの試行回数が 3 以下なら言葉で出す（`retries`）。
6. **Touch**: busy「Touch your security key.」（今の TOUCH の合図）。Cancel 可。
7. **Done**: 「<名前> was added.」Close。一覧を読み直す。
- 失敗の言葉（今の reason の語から）: `bad-secret`（1 で: Password is wrong → 1 に戻る、5 で: The PIN is wrong（残り n 回）→ 5 に戻る）、`key-locked`（The key is locked after too many wrong PINs. Reset it to use it again.）、`many-keys`、`no-key`、`timeout`（Not touched in time → 6 からやり直せる）、他は「The key could not be added (<語>).」。

### 2.4 Change PIN のウィザード

1. **Insert**（§2.3 の 2 と同じ。PIN の無い鍵は「This key has no PIN.」で Set PIN へ）。
2. **PIN**: 今の PIN と新しい PIN を 2 回 → busy → Done。password は要らない（鍵の PIN は鍵の物で、鍵自身が今の PIN を確かめる。D1）。

### 2.5 Reset Key のウィザード

1. **Warning**: 「Resetting erases everything on the key: its PIN and every account it signs in to, on this computer and elsewhere (web sites, other computers). This cannot be undone.」button「Continue」（赤）。
2. **Password**（D1: 初期化はこの機械の login の鍵を消すので password で確かめる）。
3. **Replug**: 「Unplug the key, plug it back in, then press Reset within 10 seconds.」（CTAP2 の authenticatorReset は電源の入りから 10 秒以内だけ受ける）。鍵の情報を問い続け、鍵が抜けて挿し直されたことを見て（0 個 → 1 個）Reset の button を有効にし、10 秒を数える。NFC は「Take the key away from the reader and hold it again」。
4. **Touch**: busy「Touch your security key to confirm.」
5. **Done**: 「The key was reset. Add it again to sign in with it.」この機械の登録のうち、その鍵が持っていた物は消える（D2）。
- 失敗: `not-allowed`（10 秒を過ぎた: 3 に戻る）、`timeout`、`many-keys`、他。

## 3. 層ごとの変更

| 層 | 変更 |
| --- | --- |
| libpasskey | `pk_ctap2_reset`（authenticatorReset 0x07、touch を待つ。CTAP2_ERR_NOT_ALLOWED 0x30・OPERATION_DENIED 0x27・USER_ACTION_TIMEOUT 0x2F） |
| passkey-fido2・helper | 新しい操作: `key-info`（鍵の一覧: 数、1 個なら名前・PIN の有無・PIN の残り回数。PIN を送らない）、`key-set-pin`（新しい PIN）、`key-change-pin`（今の PIN、新しい PIN）、`key-reset`（D2: 先に鍵が account の credential を持つかを問い、reset の成功の後にその行を /etc/passkey から消す）。どれも鍵が 1 個の時だけ（`many-keys`）。PIN は鍵 1 個にだけ送る（security.md の規則のまま） |
| passkey（request.c） | 4 つの操作の field の数と secret（key-info は無し、key-set-pin・key-change-pin は鍵の PIN だけで account の password 無し、key-reset は account の password）。answer に `key name=<hex> pin=0|1 retries=N` と `keys N` |
| sessiond（auth.c） | 4 つの command を passkey へ。失敗の数え（password の誤りは今どおり数える。鍵の PIN の誤りは鍵が数える）。`status touch` を TOUCH で返す |
| backend | `kl_backend_session_key_info`・`_key_info_get`、`_key_set_pin`、`_key_change_pin`、`_key_reset`（答えは session_answer の新しい種類 `KL_BACKEND_SESSION_KEY`） |
| compositor（wayland/system.c） | account の object に request 4 つと event（key info）。protocol の版を上げる |
| libkeiland | `kl_system_account_key_info`（鍵の数・名前・PIN の有無・残り回数）、`_key_set_pin`、`_key_change_pin`、`_key_reset`、answer は今の account の answer と同じ形。**KL_VERSION を 1 つ上げる**（番号は merge の時に Q1） |
| Settings | 新 `page-keys.c`（頁）、`dialog.c`（popup の部品）、3 つのウィザードの状態機械、Users の頁の card を消して link |
| Linux・FreeBSD の backend | 新しい関数は ENOTSUP（今の鍵の API と同じ扱い） |
| docs/architecture/security.md | 新しい操作と規則（D1・D2）を書く |

## 4. 試験

- host: libpasskey の reset（CTAP の台本の鍵、今の libpasskey の host 試験に足す）、passkey の request の解析（新しい 4 つの field の数）、Settings のウィザードの状態機械（純粋な関数に分け、step の遷移・既定の名前・PIN の 2 回の照合・busy の間の入力の拒否を試す）。
- QEMU（T1、AAT）: QEMU に CTAP2 の模擬の鍵（`-device u2f-emulated` は U2F だけで CTAP2 の PIN が無い見込み、未確認）。無ければ passkey の試験の道具（passkey-fido2 の host 試験の偽の鍵）で層ごとに、画面は PNG（頁、各 step、busy）をユーザーに。
- 5330 の UAT（YubiKey 5）: Add（PIN の無い鍵と有る鍵）、Change PIN、Reset、Remove、lock の画面で使えること。

## 5. ユーザーの判断（推しは太字）

| # | 判断 | 推し |
| --- | --- | --- |
| D1 | 確かめの password: Add・Remove・Reset は account の password、Change PIN と Set PIN は鍵の PIN だけ（鍵が自分で確かめる） | **推しどおり** |
| D2 | Reset で消えた鍵の、この機械の登録（/etc/passkey の行）: reset の前に鍵が持つ credential を問い、成功の後に消す／残す（login に使えないまま一覧に残る） | **消す** |
| D3 | 頁の場所: sidebar の独立の頁「Security Keys」（Users の次）、Users の頁の鍵の card は link に置き換える | **推しどおり** |
| D4 | Add Key の最初の password の step: ウィザードの最初（推し）／最後の確定の前 | **最初** |

## 6. 見積もり

libpasskey と passkey・passkey-fido2 の 4 つの操作 4 LW、sessiond・backend・compositor・libkeiland の経路 3 LW、Settings の popup の部品 2 LW、頁と 3 つのウィザード 4 LW、試験 2 LW、計 **約 15 LW**（10/13 の凍結の目標には全部は厳しい。Add Key のウィザード（今の経路に key-info と Set PIN を足す）を先に、Change PIN・Reset を後の commit に分ける）。

## Event

- 2026-10-10: 第 1 版（P1）。
