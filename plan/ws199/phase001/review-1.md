# ws199-p001 第 2 版の敵対的レビュー（review-1）

対象: [phase.md](phase.md) 第 2 版（branch agent/p1、commit 63db6b7bc）。前提: [ws.md](../ws.md)「仕様の変更」とユーザーの決定は変えない。
照合した code: `userland/desktop/sessiond/`（auth.c・auth-policy.c・auth.h・seat.c・session.c）、`userland/base/passkey/`（main.c・request.c・record.c・passkey.h）、
`userland/base/passkey-fido2/`（main.c・device.c・helper.c・fido2.h）、`userland/base/libpasskey/`（ctap2.h・ctap2.c・verify.h・os-zedbsd.c）、
`userland/desktop/wayland/`（greeter.c・keyboard.c）、`userland/desktop/libkeiland-backend-zedbsd/`（session-zedbsd.c・events-zedbsd.c）、
`userland/desktop/settings/`（page-users-keys.c・page-users-pin.c・Makefile*）、`include/uapi/system.h`、`src/drivers/usb/usb.c`、
`src/drivers/generic/hidraw.c`・`smartcard.c`、`src/drivers/pci/pci-xhci.c`、`userland/desktop/include/keiland/keiland.h`、`plan/ws172/`・`plan/ws200/ws.md`。
CTAP2 は手元に仕様が無く、CTAP2.0/2.1 の知識による（その旨を各所に書く）。build・試験は走らせていない（設計のレビューだけ）。

判定の要約: **blocker 2、major 12、minor 15**。i01 は軽い直しの後に GO、i02〜i05 は直してから。

---

## Blocker

### B1. lock の「lock の後に挿した鍵」の判定に根拠が無く、sleep の復帰で必ず偽られる（§3.7、§4.3、§8）

- **誤り 1: sessiond は lock の時刻を知らない。** lock は compositor だけの状態で（`wayland/greeter.c:388` `kwl_lock` が `greeter_lock_at` を compositor の中に持つ）、
  sessiond の session socket の request は `UNLOCK`・`STYLES`・`ENROLL` などだけで lock の通知は無い（`sessiond/session.c:30-70`、`auth.c:9-28`）。
  §4.3「sessiond が lock の時刻より後に USB の ADD を受けていた時だけ optional」は、新しい通知（例 `LOCKED`／`UNLOCKED`）無しには実装できない。§4.5 にも無い。
- **誤り 2: sleep からの復帰で USB の機器は数え直され、挿しっぱなしの鍵が「lock の後に挿した」と見える。** compositor は sleep の前に lock する
  （`wayland/sleep.c:533`、lid は `backend-host.c:376`）。xHCI の resume は状態が戻らない時に controller を reset して全部の機器を数え直す
  （`src/drivers/pci/pci-xhci.c:6107-6131`、注記「QEMU's xHCI always reports」）。数え直しは `usb.c:5593` の REMOVE と `usb.c:5983` の ADD を post する。
  起こる状況: touch=0 の user が鍵を挿したまま蓋を閉じる → lock → sleep → 復帰 → ADD（同じ vendor・product）→ sessiond は optional → lock の画面は
  「挿した」と見て（§3.7「lock の後に鍵を挿すと card が出て（swipe 無しで）鍵のモードに入る」）タッチ無しで解ける。誰も触れずに lock が外れる。
  QEMU では毎回、実機（5330）でも Save/Restore Error の時に起こる。ユーザーの決定「挿しっぱなしならタッチを促す」に反する。
- **誤り 3: vendor・product の照合と「後から問い直す」流れが壊れている。** §4.3 は passkey-fido2 が使った鍵の vendor・product を答えに添え、合わなければ required で
  「問い直す」とするが、その時には一度目の up=false の試みは passkey-fido2 で検証に通り、`main_auth` は大きくなった署名の数を /etc/passkey に書き戻している
  （`passkey-fido2/main.c:501-506`）。sessiond が後から ok を捨てると、数えは attempt のまま（`auth.c:479-483`）で、成功を失敗に替える経路は今の
  `auth_finish`（`auth.c:779-801`）に無い。さらに helper はどの node の鍵が答えたかを返さない（`helper.c:200-245`、message に node が無い）。
- **誤り 4（推測を含む）: USB の ADD は class driver の probe の前に出る**（`usb.c:5983` の直後に `interface_probe_internal`）。hidraw の node が出来た時は別に
  `KERN_SYSTEM_EVENT_INPUT` の ADD（subject `hidrawN`、detail `usage=f1d0:0001`）が出る（`src/drivers/generic/hidraw.c:716-734`）。鍵の出入りには
  INPUT の hidraw の事象の方が正確で、vendor・product の近似（§8）も要らない。
- **直し方:**
  1. compositor → sessiond に `LOCKED`（lock した時）と `UNLOCKED` を足す（session socket、user の権限で十分: 自分の session の事だけ）。§4.5 と keiland.md に書く。
  2. sessiond は `seat.c` の既存の購読（`SEAT_EVENT_CLASSES` は既に INPUT と USB、`seat.c:69-70`、`:193`）の中で、FIDO の hidraw の INPUT ADD
     （usage page 0xF1D0）を、`LOCKED` の後で、**かつ sessiond が知る最後の復帰（sessiond の sleep.c が自分で sleep させる）から数秒の猶予の後**のものだけ記録する。
     OVERFLOW を受けたら記録を捨てる（安全側: required）。
  3. passkey の request の presence は「optional の node の一覧」（例 `presence=optional:hidraw3`）として渡し、passkey-fido2 は**その node の鍵にだけ** up=false を
     使う。後から問い直す流れは消す。
  4. §6 の host 試験に「復帰の後の ADD は数えない」「OVERFLOW で optional を捨てる」「LOCKED の前の ADD は数えない」を足し、UAT に「挿したまま lock → sleep → 復帰で
     タッチを促す」を足す。

### B2. タッチ不要の login で、logout・session の異常終了のたびに自動で login し直す（§3.6）

- **誤り:** §3.6 は「画面が出た時」に `KEYOWNER` を問い、touch=0 なら「すぐ `AUTH user fido2`」。greeter は boot の時だけでなく、Log Out の後
  （`session-zedbsd.c:226` `kl_backend_session_logout`、sessiond が新しい greeter を出す）と session の異常終了の後にも出る。
- **起こる状況:** 鍵を挿したまま Log Out → greeter → KEYOWNER → 自動の AUTH → desktop に戻る。挿したままでは logout も、別の user での login も、greeter の
  Restart・Shut Down も選べない。session が起動直後に落ちる不具合があると、成功で数えが消える（`auth-policy.c:310-319`）ので greeter と session の往復が
  止まらない。
- **直し方（ユーザーの判断が要る）:** login も lock と同じく「greeter が出た後に挿した鍵」だけ自動にし、greeter が出た時から挿さっている鍵は鍵のモードの画面
  （タッチ・Enter を待つ）に留める案を推す。少なくとも Log Out・session の終了の後の greeter では自動の AUTH をしない。boot の直後の扱いもユーザーに確かめる。
  greeter の状態機械の host 試験に「logout の後の greeter は自動で送らない」を足す。

---

## Major

### M1. KEYOWNER が署名の無い silent の答えを信じる: 偽の機器で持ち主と設定が分かり、他人の失敗の数えを増やせる（§4.2）

- **根拠:** 今の silent の問いは `presence=0` で、答えに署名が無くても受ける（`libpasskey/ctap2.c:760-762`）。helper は答えた最初の鍵を選ぶだけで、
  署名を確かめるのは login の本番の答えだけ（`helper.c:200-216`、`main.c:473-499`）。§4.2 の `key-owner` は全部の account の credential ID を、挿さった
  どの機器にも送る（allowList）。
- **起こる状況:** greeter の前で、CTAP2 を真似る USB 機器（プログラムできる機器）を挿す → 機器は allowList で全部の credential ID を知り、好きな ID を
  「持っている」と答える → `KEYOWNER user=<誰でも> pin=0|1 touch=0|1` が画面に出る（どの account が「PIN もタッチも不要」かが分かる: 盗む鍵の的を選べる）。
  touch=0・pin=0 の account なら greeter が自動で AUTH を送り、本番の検証で落ちて**その account の失敗が数えられ、password の login まで最大 16 秒遅れる**
  （`auth-policy.c:298-341`、「every style of the account together」）。挿し直しで繰り返せる（KEYOWNER の 1 秒の上限は AUTH を止めない）。
- **直し方:** key-owner でも passkey-fido2 が challenge を作り、silent の答えの**署名を account の公開鍵で確かめてから**持ち主と答える（flag は求めない。
  CTAP2 の getAssertion は up=false でも署名を返す、仕様の知識による）。署名の無い答え・合わない答えは `KEYOWNER none`。答えに `pin=` `touch=` を出すのは確かめた後だけ。
  security.md に「KEYOWNER は全 account の credential ID を挿さった鍵に送る」ことと開示の範囲を書く。

### M2. KEYOWNER の持ち主が 2 人以上の時と、allowList の上限が決まっていない。request の形も今の code に合わない（§4.2）

- **持ち主が複数:** 1 本の鍵を 2 つの account（例 管理者用と普段用）に登録できる（登録の excludeList は同じ account の中だけ、`main.c:567-573`）。全部の
  ID を 1 つの allowList で問うと、鍵は最初に合った 1 つだけを返す（CTAP2 の知識による）。どちらの user を選ぶかは鍵の内部の順で決まり、touch=0 なら
  意図しない account に自動で login する。→ account ごとに問い、2 人以上なら `KEYOWNER users=a,b`（自動で選ばず一覧で選ばせる）か `KEYOWNER ambiguous`。
- **allowList の上限:** getInfo の `maxCredentialCountInList`・`maxCredentialIdLength`・`maxMsgSize`（`ctap2.h:101-110` に読んである）を越える allowList は
  鍵が `CTAP2_ERR_LIMIT_EXCEEDED`（0x15）等で断る（CTAP2.1 の知識による）。account 数×5 本を一度に送る §8 の見積もりは成り立たない。上限で分けて問う。
- **request の形:** passkey の request は field 1 の名前が空でなく 32 byte 以内の時だけ受ける（`passkey/request.c:85-86`）。名前の無い `key-owner` は
  `bad-request` になる。sessiond は答えの uid が問うた account と合わなければ失敗にする（`auth.c:779-787`、名前が account でなければ uid は -1）。
  KEYOWNER は sessiond の側で別の答えの経路（`ok uid=` ではなく `owner=`）と、greeter・session の許す表（`auth.c:341-351`）を設計に書く。
  greeter の一覧に居ない user（shell が nologin、`greeter.c:942-971`、`GREETER_USERS` の上限）は自動の AUTH もしないと書く。

### M3. 一度に 1 つの request の通り道を、1 秒ごとの KEYINFO と KEYOWNER が塞ぐ（§3.3、§3.5、§3.6、§4.1）

- **根拠:** backend は descriptor ごとに 1 つの request しか待たない（`session-zedbsd.c:744-770` `session_send` の EBUSY）。sessiond も socket ごとに 1 つ
  （`auth.c:184-189` `ERROR busy`）。Settings と lock の画面は同じ session socket を使う（同じ compositor の `--control-fd`）。KEYINFO は毎回 passkey →
  passkey-fido2 → helper の 3 つの process と鍵の claim（`HIDRAW_GRAB`、`os-zedbsd.c:128-136`、`device.c:53-58`）を伴う。
- **起こる状況:**
  - Settings の Add・Reset の popup を開いたまま idle の lock がかかる → lock の画面の `UNLOCK`・`STYLES` が KEYINFO と重なって EBUSY。`POWER`・`SERVICE` も同じ。
  - popup が開いている間、毎秒 helper が鍵を claim し、同じ session の browser の WebAuthn が鍵を開けられない瞬間ができる。
  - greeter で、KEYOWNER（全 account の silent の問い）が走っている間に user が password を入れて Enter → AUTH が EBUSY。
- **直し方:** 1 秒ごとの poll をやめ、compositor が既に受けている INPUT の hidraw の事象（`events-zedbsd.c:43-44` は INPUT を購読済み、`:216` で今は
  まとめて `input_changed`）を Settings にも event で渡し、変わった時だけ KEYINFO を問う。lock 中は Settings の鍵の request を compositor で止める。
  greeter は KEYOWNER と AUTH・STYLES の順を状態機械で決め（AUTH を待つ間は KEYOWNER を問わない、KEYOWNER の答えの前に Enter されたら答えを待って送る）、
  host 試験に入れる。

### M4. USB の事象・NFC の前提が今の kernel と passkey-fido2 に合わない（§1、§3.3、§3.5、§3.6、§6）

- §1「smartcard の card の出入りも同じ class」は誤り。smartcard は**読み取り機の slot の登録・抜き**の時だけ post する（`smartcard.c:209`、`:292`）。
  NFC の鍵をかざしても事象は無い。→ greeter・lock は NFC の鍵で KEYOWNER を問えず、鍵のモードに自動で入らない。
- passkey-fido2 は hidraw の鍵しか開けない（`device.c:49-58` は `pk_os_list` だけ。`pk_os_list_cards` を使わない）。ws172-p003 の残りにも
  「WS161 p005（NFC）が入ったら helper が /dev/smartcard* も使う」とある（`plan/ws172/phase003/phase.md:56`）。§3.3「hold it to the reader」、§3.5 の NFC の
  Replug、§6 の UAT「YubiKey 5、NFC」は今の経路では動かない。KEYINFO の名前も、NFC では読み取り機の名前になる（`os-zedbsd.c:338-341`）。
- USB の ADD は node より先（B1 の誤り 4）。§3.6「0.5 秒置いて、出来なければ 1 秒後にもう 1 回」は推測の待ちで、遅い機器では取りこぼす。
- **直し方:** 鍵の出入りは INPUT の hidraw の事象（`usage=f1d0`）で見る。NFC は WS199 の範囲から外す（文言と UAT から消し、Future Work か WS161 へ）か、
  passkey-fido2 の NFC の対応を i02 の作業として明記し、「かざした」ことを知る手段（poll か新しい事象。後者は kernel の UAPI の追加で、HAL ではないが Q1 の判断）
  を設計する。

### M5. 失敗の語が足りず、画面が誤った案内をする。特に「鍵の初期化」へ誤って導く（§3.3、§3.6、§4.4）

- **key-locked:** helper は `PIN_BLOCKED`（0x32: 初期化しか戻せない）と `PIN_AUTH_BLOCKED`（0x34: 抜き差しで戻る、3 回続けて誤った時）を同じ `key-locked` にする
  （`helper.c:356-359`）。sessiond はそれを `locked` にする（`auth-policy.c:172,176`）。§3.3「`key-locked`（Reset を案内）」は、抜き差しで済む時に
  **鍵の全部の credential（他の機械・web の分も）を消す初期化**へ導く。→ `key-blocked`（0x32、Reset へ）と `key-replug`（0x34、抜き差しを案内）に分ける
  （CTAP2 の値は ctap2.h:43-44 と仕様の知識による）。
- **bad-secret:** 登録の password の誤り（`passkey-fido2/main.c:141-146`）と鍵の PIN の誤り（`helper.c:353-354`）が同じ `bad-secret`。§3.3「`bad-secret`
  （1 か 5 に戻る）」はどちらに戻るか決められない。→ `bad-password` と `bad-pin` に分けるか、wizard の最初の step で password だけを確かめる。
- **pin-required:** helper は 0x36（`PIN_REQUIRED`、alwaysUv の鍵）を `device` にする（`helper.c:362-372` の default）。sessiond は知らない語を
  `bad-secret` にする（`auth-policy.c:357-359`）。§3.6・§8 の「`pin-required` で PIN の欄へ」は今の写像では届かない。→ helper・passkey-fido2・
  `policy_reasons`・security.md の語の表に足す。pin-required は数えない（下の M6）ことも決める。
- **PIN の規則違反:** `setPIN`・`changePIN` で鍵の最小長（getInfo の minPINLength、`ctap2.h:107`）より短いと 0x37（`PIN_POLICY_VIOLATION`）で、今は `device`。
  §3.3 の「4〜63 文字」は鍵ごとに違う。→ KEYINFO の答えに `min=` を足し、0x37 を `weak-pin` に写す。

### M6. 自動で始めた鍵の試みの数え方と、新しい操作の数え・許可・時間が決まっていない（§3.6、§3.7、§4.3、§4.4）

- **根拠:** 試みは passkey を走らせる前に数え、成功でだけ消える（`auth.c:479-483`、`auth-policy.c:298-319`）。CANCEL の答えは `fail timeout` で、それも
  数えて遅れる（`auth.c:171-177`、`:757-758`、`:917-930`）。
- **起こる状況:** タッチだけの account で鍵を挿すと自動で AUTH が始まり 30 秒待つ。user が「Use your password」を押す（CANCEL）・触れない（timeout）・鍵を抜く
  （device）たびに失敗が数えられ、2 秒→最大 16 秒の遅れが password にもかかる。lock で挿しっぱなしの鍵に自動で試みを始める実装なら、席を外している間に数えが積もる。
- **直し方:** 自動の試みは「鍵が答えを出す前に止めた」（CANCEL・抜け・触れない）時は数えないか、少なくとも遅れを付けない、とユーザーの判断で決める
  （security.md の「数えてから走らせる」規則の例外になるので、安全側の根拠を書く）。lock・greeter とも、自動の試みは挿した時に 1 回だけで、失敗の後に自動で
  繰り返さないと書く。新しい request ごとに表で決める: greeter・session のどちらが送れるか（`auth.c:341-351` の表に当たる物）、秘密の行の数、数え
  （`set-fido2-options`・`key-reset` は password の試みとして数える、`KEYINFO`・`KEYOWNER`・`key-set-pin`・`key-change-pin` は数えない）、時間（`auth.c:497-499`
  は style で決めるので、触れる `key-reset` は `SESSIOND_PASSKEY_KEY_MS`、他は 10 秒）。

### M7. Reset の 10 秒の窓に、poll・password の確かめ・silent の問いが全部入る（§3.5、§4.4）

- **根拠:** authenticatorReset は電源が入ってから一定の時間（多くの鍵で 10 秒）の内だけ受け、触れることを求める（CTAP2 の知識による）。§3.5 は KEYINFO の 1 秒の poll で
  0→1 を見てから button を有効にし、押してから sessiond → passkey → password の確かめ（`ACCOUNT_HASH_ROUNDS` 65536 の SHA-512 crypt）→ passkey-fido2 →
  helper → silent の問い → reset、と進む。password が誤っていれば 2 秒以上の遅れ（`auth.c:917-930`）で窓は確実に切れる。
- **起こる状況:** 押すのが遅れる・password を誤ると `not-allowed` で、もう一度抜き差しからやり直し。NFC は M4 のとおり経路が無い。
- **直し方:** `key-reset` を「password を確かめ、それから passkey-fido2 が鍵の 1→0→1 を自分で待ち（最大 30 秒）、出来た直後に silent の問いと reset を送る」1 回の
  実行にする。画面は「抜いて挿し直してください → 触れてください」だけになり、10 秒を数える表示も要らない。password の誤りは抜き差しの前に分かる。
  消す行: その鍵が持っていた**この account** の行に加え、同じ鍵が持っていた他の account の行をどうするか（消す・残して記録する）を決める（今の案では他の
  account の行が死んだまま残り、その account の 5 本の枠を占める）。

### M8. passkey の request の field の数の規則と §4.3 の新しい field が合わない（§4.3、§5）

- **根拠:** 各操作の field の数は一つに決まっている（`passkey/request.c:27-35`、`auth` は 4、security.md「each operation has an exact number of fields」）。
  §4.3「新しい field `presence=required|optional`」は auth の形を 5 に変え、password・PIN の auth も変わるのか、fido2 だけかが書かれていない。
  `auth` の shape は style に依らないので、fido2 だけ 5 にするには解析の規則を変える。sessiond と passkey が違う版の組（片方だけ更新された image）だと全部の
  fido2 の試みが `bad-request` になる。
- **直し方:** fido2 の login を新しい操作 `auth-fido2`（name・PIN（空可）・presence）にして `auth` の形は変えない。B1 の node の一覧もここに入れる。
  空の PIN の扱い（pin=0 で空でない PIN が来た時は PIN を使う、pin=1 で空なら `bad-request`）も書く。

### M9. WS200（認証方式の選択）との重なりが設計に無い（§3.2、§2、範囲）

- **根拠:** WS200 は Users の頁に「Sign-in Methods」（Password・PIN・Security Key の checkbox）を作り、外した方式は greeter・lock で出さず受けない。設定は
  sessiond・passkey の側（/etc/passkey 等）に置く（`plan/ws200/ws.md` の目標）。
- **食い違い:**
  1. WS200 で Security Key を外した account でも、§3.6 の KEYOWNER・自動の鍵のモードは鍵を挿すと動く。KEYOWNER は styles の許しを見ると書く必要がある。
  2. 両方が /etc/passkey に account ごとの設定の行を足す。`fido2-options` と WS200 の行が別の種類で、別の password の操作（`set-fido2-options` と WS200 の物）に
     なる。→ 1 つの行 `<name>:<uid>:options:styles=…:fido2-pin=0|1:fido2-touch=0|1`（知らない key は残す）と 1 つの操作 `set-options` に揃える。
  3. `dialog.c` を WS200 も使う（§3.2）が、WS200 は WS199 の後の Queue（q922）。WS200 の ws.md に依存（WS199 の i01）を書き、部品の API を i01 で固める。
  4. 両方が Users の頁（`page-users*.c`）を変える。i01 の「Users の頁の鍵と PIN の card を消す」と WS200 の変更の順を Q1 に確かめる。

### M10. 試験の計画が実行できず、捕まえるべき誤りを捕まえない（§6）

- **QEMU:** QEMU の `u2f-emulated` は U2F（CTAP1）だけ（知識による、設計も「見込み」と書く）。ws172-p003 の段 C（QEMU で鍵の流れを確かめる手段）は
  **ユーザーの決定で作らない**ことになっている（`plan/ws172/phase003/phase.md:16`）。§6「passkey-fido2 の host 試験の偽の鍵」は存在しない（host 試験は
  `plan/ws172/tests/passkey-host-test.c`・`sessiond-auth-host-test.c` と、CTAP2 の台本の transport を持つ `plan/ws161/tests/libpasskey-ctap2-host-test.c` だけで、
  どれも guest の中で鍵を出せない）。→ 「QEMU で greeter・lock の鍵のモード 3 通りの PNG」は今の手段では撮れない。選択肢: (a) QEMU の `-device canokey`
  （CTAP2・PIN を持つ。QEMU が libcanokey-qemu 付きで build されている時だけ、**未確認**）、(b) T1 の host に挿した鍵の `usb-host` の passthrough、
  (c) 試験の build だけの passkey-fido2 の偽の helper。どれにするかと、段 C の決定を変えることをユーザーに確かめる。
- **実物の鍵の経路が一度も確かめられていない:** 鍵の login の実機の UAT（WS161 p006）は未実施。WS199 は PIN 不要・タッチ不要の上乗せを、基本の経路の確認の前に
  積む。→ i03 の前に、今の経路の 5330＋YubiKey の確認（WS161 p006 か WS199 の小さな smoke）を依存に入れる。
- **捕まえない誤り（host の純粋な関数では出ない）:** 復帰の後の数え直し（B1）、logout の後の自動の login（B2）、EBUSY の重なり（M3）、0x34 と 0x32 の区別（M5）、
  2 人の持ち主（M2）、YubiKey が PIN 設定済みで UV 無し・up=false の assertion を本当に出すか、alwaysUv、Reset の窓（M7）。これらを T1 の AAT か 5330 の UAT の
  項目に書く。

### M11. security.md を最後（i05）に書く順が、docs を先に書く方針と逆（§5、§7）

- **根拠:** security.md の冒頭「the document comes first and the implementation follows it」（`docs/architecture/security.md:3-5`）。§7 は i05 で書く。
  presence の決め方・KEYOWNER の開示・数えの例外という**特権の境界の規則**が、実装（i03・i04）の merge の後に文書になる。
- **漏れ:** greeter・session の行の protocol は `docs/architecture/keiland.md` にもある（`auth.c:11` が参照）。鍵の操作は `docs/reference/security-keys.md` にもある。
  §5 は security.md だけ。
- **直し方:** security.md・keiland.md の差分を p001 の設計の一部として先に書き（i03 の前）、design-reviewer に通す。

### M12. `fido2-options` の行の寿命・読み出し・重複が決まっていない（§2、§4.4、§4.5）

- **読み出し:** §4.5 の `kl_system_account_key_options_get` に当たる sessiond・passkey の操作が無い（§4.4 は `set-fido2-options` だけ。今の `enrolled` の答えは
  `pin=0|1 fido2=N key=…`、`passkey/main.c:390-423`）。→ `enrolled` の答えに options を足すと書く。
- **寿命:** 最後の鍵を Remove・Reset しても options の行は残る（`passkey_record_edit` は鍵の行だけを消す、`record.c:144-173`）。radio は灰色になるが、次に Add した鍵は
  警告も password も無しで「PIN もタッチも不要」で始まる。account の削除と password の reset は名前の全部の行を消すので問題無い（`account-admin/main.c:826-898`、
  `passkey_record_replace` の kind NULL）。→ 最後の鍵が消えたら options の行も消す（既定に戻る）と決めるか、Add の完了の画面で今の mode を見せる。
- **重複・誤り:** 同じ account に options の行が 2 本、`touch=0 pin=1` の行、値の誤り、の時にどれを使うかを書く。**最も強い方（pin=1 touch=1）**に倒すこと。

---

## Minor

| # | 節 | 何が誤りか | 根拠 | 直し方 |
| --- | --- | --- | --- | --- |
| m1 | §3.6 | 「OK が来ても 0.5 秒経つまで desktop へ移らない」を greeter は守れない。sessiond は OK で session を始め、compositor の READY で greeter に display を返させる | `session.c:30-40`、`session-zedbsd.c:36-45` | greeter が「Checking…」を出した時刻から 0.5 秒経つまで display を返す（RELEASED）のを遅らせる、と仕組みを書く。sessiond の待ちの上限と合わせる |
| m2 | §3.2、§4.5 | Settings からの CANCEL の口が無い（今の Settings の鍵の card は取り消せない）。KL_SYSTEM_HAS_*・KL_SYSTEM_CHANGED_*（KEYINFO の答え）も無い | `keiland.h:1251-1281`、`page-users-keys.c:8-22` | `kl_system_account_key_cancel`、`KL_SYSTEM_HAS_KEY_OPS`、`KL_SYSTEM_CHANGED_KEYINFO` を §4.5 に足す |
| m3 | §3.8 | ユーザーは「オンスクリーンキーボードを入力欄のすぐ下」。設計は独自の keypad（数字 3×4＋ABC）。形が違う。鍵の PIN は UTF-8 の任意の文字で、keypad では ASCII しか打てない | ws.md「仕様の変更」、`keyboard.c:1011-1014` | 独自の keypad にする理由と見た目をユーザーに確かめる。非 ASCII は物の keyboard だけと書く |
| m4 | §3.3、§3.4 | PIN の長さ「4〜63 文字」は CTAP2 では 4 code point 以上・63 byte 以下（UTF-8）、最小は鍵ごと（M5） | `ctap2.h:107`、CTAP2 の知識 | 文字と byte を分けて書く。最小は KEYINFO の `min=` |
| m5 | §0、Event | 第 1 版の D1〜D4 が「全部推し」の範囲か未確認のまま i01 に入る（Add の最初の Password の step は D4） | phase.md:171 | i01 の前に Q1 経由でユーザーに確かめる |
| m6 | §3.1 | ユーザーの「システムに登録済みのキーの名前一覧」は全 account の鍵とも読める。設計はその user の鍵だけ（ENROLLED は session の user） | ws.md「由来」、`auth.c:19` | どちらかをユーザーに確かめる |
| m7 | §8 | 「up=false を断る鍵は『タッチだけ』に落とす」は成り立たない。今の silent の問い自体が up=false で、断る鍵は見つからず `no-key` で、login が全く出来ない | `helper.c:200-216` | 危険の表を直す（その鍵は今も使えない、と書く） |
| m8 | §4.2 | 1 秒の上限を越えた KEYOWNER を断るのか、遅らせるのかが無い。greeter の 0.5 秒・1.5 秒の問いと重なると 2 回目が断られる | §3.6 と §4.2 | sessiond で遅らせて 1 つにまとめる（最後の 1 つを後で走らせる） |
| m9 | §3.6 | 「USB の REMOVE で password の欄に戻る」はどの機器の REMOVE でも戻る。AUTH の途中で鍵が抜けた時の CANCEL も無い | §3.6 | 鍵の node（hidraw）の REMOVE だけを見る。AUTH の途中なら CANCEL を送る（数えは M6） |
| m10 | §3.6 | KEYOWNER の答えで user を選び直すと、別の user の欄に打っていた password をどうするかが無い | §3.6 | 打ちかけの password は消して選び直す、か、打っている間は選び直さない、を決める |
| m11 | §4.5 | Linux・FreeBSD: Settings は Makefile.linux・Makefile.freebsd でも build する。新しい頁・`dialog.c` を 3 つの Makefile に足すこと、backend の新しい host の callback（hidraw の事象）の stub、頁の「not supported here」の表示が書かれていない | `settings/Makefile.linux:17`、`Makefile.freebsd:17` | §4.5 に書く |
| m12 | §4.3 | sessiond が「自分で購読する」とあるが、既に seat.c が INPUT・USB を購読している。2 つ目の open か既存の流れに足すかが無い | `seat.c:69-70,177-205` | 既存の `sessiond_seat_events_collect` で鍵の事象も記録する（1 つの流れ） |
| m13 | §2 | PIN もタッチも不要の login も sessiond の `signed_in` を立て、6 桁の Software Security Key の PIN を許す | `auth-policy.c:310-319` | 意図どおりか決め、security.md の「The PIN is offered only after…」に書く |
| m14 | Phase | p001 が設計・5 回の merge・host 試験を 1 Phase（約 19 LW）に持つ。WS に規約の全文の見直しの Phase が無い（i05 の style-check は代わりにならない） | ws.md の Phase の表、AGENTS.md（Awesome Plan §6） | i01〜i04 を Phase に分け、近い最後に conformance の Phase を足す（Q1 に依頼） |
| m15 | 依存 | 依存に書く ws172-p002 は in-progress（T1 待ち）、ws172-p007（greeter の方式の選択）は test-wait で、§3.6・§3.7 はその上に乗る。ws172 の ws.md の表は p003 を uncleared と書き phase.md は cleared（表が古い） | `plan/ws172/ws.md:36-46`、`plan/ws172/phase003/phase.md:4` | 依存に ws172-p007 と ws187-p003 を足し、p002・p007 が cleared になるまで i04 を始めないと書く。ws172 の表の食い違いは Q1 に知らせる |

---

## 今の code と設計の記述の照合（正しかった所）

- §1 の経路・passkey の操作の一覧・libpasskey の `presence` と PIN token・passkey-fido2 の UP と UV の必須（`main.c:486`）・greeter と lock で画面の keyboard を閉じる
  （`keyboard.c:1011-1014`）・Settings に modal が無い、は code と合う。
- `fido2-options` の行は今の passkey が残す（`record.c` の `record_line_is` は kind を完全一致で比べ、知らない kind の行は書き直しで残る）。古い passkey-fido2 は
  UP と UV を求め続けるので、古い版と組んでも緩む方向には壊れない。account の削除・password の reset は名前の全部の行を消す（options も消える）。
- 鍵の PIN は 1 本の鍵にだけ送る今の規則（`helper.c:200-219`、登録は `many-keys`、`helper.c:259-267`）は §4.4 と合う。
- HAL（`include/hal/hal.h`・`src/hal/`）の変更は要らない。B1・M4 の直し方は既存の `/dev/system` の INPUT の事象で足り、kernel の UAPI も変えない
  （NFC の「かざした」事象を足すなら UAPI の追加になり、Q1 の判断）。

## i01〜i05 の判定

| i | 判定 | 条件 |
| --- | --- | --- |
| i01 | **軽い直しの後に GO** | m5（D1〜D4 の確認）、M9-3・4（`dialog.c` の API と Users の頁の順を WS200 と合わせる）、M5 の bad-secret（今の ENROLL の wizard で 1 と 5 のどちらへ戻るかを決める。分けられるまでは「password か鍵の PIN が違う」と出して 1 から）、m2（Cancel は i01 では出さない、と明記）、m6 |
| i02 | **直してから** | M5（語を分ける）、M7（reset を 1 回の実行に）、M3（poll をやめ事象で問う、lock 中は止める）、M6（新しい操作の表）、M4（NFC を範囲から外すか作業に入れる）、M12 の読み出し |
| i03 | **直してから（始めない）** | B1（LOCKED の通知、復帰の猶予、node の一覧の presence）、M8（`auth-fido2` の形）、M6、M9-1・2（WS200 と 1 つの options の行）、M12、M11（security.md を先に）、M10 の実物の鍵の確認を前に |
| i04 | **直してから（始めない）** | B2（ユーザーの判断）、M1（署名を確かめる KEYOWNER）、M2（複数の持ち主・allowList の上限・request の形）、M3（greeter の順）、M4（hidraw の事象）、M6、m1・m8・m9・m10、m15 の依存 |
| i05 | **直してから** | M11（docs は i03 の前へ移す）、M10（QEMU の手段をユーザーに確かめる、UAT の項目を足す）、m14（Phase の分け方と conformance の Phase） |

## 確かめていないこと（推測）

- CTAP2 の挙動（up=false でも署名を返す、PIN 設定済みの鍵が pinUvAuthParam 無しの getAssertion に UV=0 で答える、0x15・0x30・0x34・0x36・0x37 の意味、reset の
  10 秒）は仕様の知識による。YubiKey 5 の実物では確かめていない。
- 5330 の実機で S0idle の復帰の後に xHCI が数え直すか（Save/Restore Error が出るか）は未確認。QEMU では code の注記どおり毎回数え直す。
- QEMU の `canokey` の有無（T1 の host の QEMU の build）は未確認。
- `/dev/system` の購読に権限の確かめが無いこと（`system-device.c:486`）は分岐だけを見た。`system_event_subscribe` の中は読んでいない。
