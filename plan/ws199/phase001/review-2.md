# ws199-p001 第 3 版の再確認（review-2）

対象: [phase.md](phase.md) 第 3 版（branch agent/p1、commit 2dc99e171）の §9（§2〜§8 より優先）。前回: [review-1.md](review-1.md)。
照合した code（2dc99e171 の tree）: `userland/desktop/sessiond/`（auth.c・auth.h・auth-policy.c・seat.c）、`userland/base/passkey/`（main.c・request.c・passkey.h）、
`userland/base/passkey-fido2/`（main.c・device.c・helper.c・fido2.h）、`userland/base/libpasskey/`（ctap2.h・ctap2.c・os-zedbsd.c）、
`userland/desktop/libkeiland-backend-zedbsd/`（session-zedbsd.c・events-zedbsd.c）、`userland/desktop/libkeiland-backend/keiland-backend.h`、
`userland/desktop/wayland/`（system.c・handoff.c・greeter.c・sleep.c・sleep-rules.c・settings.c）、`userland/desktop/include/keiland/keiland.h`、
`userland/desktop/settings/page-users-keys.c`、`src/drivers/generic/hidraw.c`、`plan/ws200/ws.md`。
CTAP2 の挙動は手元に仕様が無く知識による（「推測」と書いた所）。build・試験は走らせていない（設計の読み合わせだけ）。

判定の要約: **i01 は GO（J4・J5 の答えを待つ、または推しで進めて答えで直す）。i02 は直してから**（新しい major 5 件。どれも設計の文の直しで足り、
HAL・UAPI の変更は要らない）。i03・i04 の方針（§9.4）は概ね妥当、注意 3 点。

---

## 1. review-1 の指摘の閉じ方

| # | 状態 | 一言 |
| --- | --- | --- |
| B1 | 方針で閉じた（i03） | LOCKED/UNLOCKED、hidraw の ADD の node、node ごとの up=false。復帰の猶予 5 秒は §4 の注 1 |
| B2 | 判断待ち（J1） | 推しは妥当。i01・i02 に関係しない |
| M1 | 方針で閉じた（i04） | silent の答えの署名を確かめる |
| M2 | 方針で閉じた（i04） | many-owners、allowList の分割、`key-owner` の name、uid の引き直し |
| M3 | **一部だけ閉じた** | poll をやめ hidraw の事象（`hidraw.c:731` の detail に `usage=f1d0:0001`、ADD・REMOVE とも）で問うのは code と合う。lock 中に止めるのは KEYINFO だけで、長い KEYRESET・KEYPIN が UNLOCK・sleep を塞ぐ所が残る（N5）。greeter の KEYOWNER と AUTH の順（i04）は §9.4 に再掲が無い |
| M4 | 閉じた | NFC を範囲の外へ（§9.2）、鍵の出入りは hidraw の事象 |
| M5 | ほぼ閉じた | 語は分けた。残りは語の表を request の種類ごとにする事と、helper の手元の検査の語（N8） |
| M6 | ほぼ閉じた | 表は出来た。ただし「数えない」request の成功が今の `auth_granted` で数えを消し `signed_in` を立てる（N1）。KEYRESET の password の後の失敗の数え（N6） |
| M7 | 形は閉じた | 1 回の実行・replug は passkey-fido2 が待つ。ただし鍵の確かめが replug の**前**に移り（N4）、deadline の和（N2）・取り消し（N3）・他の account の行（N10）が残る |
| M8 | 方針で閉じた（i03） | `auth-fido2` |
| M9 | 判断待ち（J4） | `plan/ws200/ws.md` に WS199 i01 の部品への依存と 1 行の options がまだ無い（Q1 の記録、N15） |
| M10 | 閉じた（i01・i02 の範囲） | QEMU は鍵の無い画面だけ、鍵の操作は 5330。sessiond の host 試験の項目が足りない（N14） |
| M11 | 閉じた | docs は各 i の code の前 |
| M12 | 方針で閉じた（i03） | `enrolled` に options、最後の鍵で消す、重複は強い方 |
| m1 | 方針で閉じた（i04） | RELEASED を遅らせる |
| m2 | 閉じた（API） | `kl_system_account_key_cancel`、`KL_SYSTEM_HAS_KEY_OPS`。取り消しの意味は N3 |
| m3 | 判断待ち（J3） | |
| m4 | 閉じた | `min=`、code point と byte（`ctap2.c:972-1000` の `ctap2_check_pin` と同じ規則） |
| m5・m6 | 判断待ち（J5） | |
| m7・m8・m9・m10・m12・m13 | 方針で閉じた（i03・i04） | |
| m11 | 閉じた | Makefile.linux・.freebsd、ENOTSUP、「Not available on this system」 |
| m14 | Q1 待ち | ws.md の Phase の表はまだ p001・p002 の 2 行（N15） |
| m15 | 閉じた | i04 の依存に ws172-p002・p007・ws187-p003 |

確かめて問題の無かった点: KEYPIN を password 無し・数え無しにする事（D1）は、session の user が鍵の node を既に 0600 で持つので
（`sessiond/seat.c:130`、user の process は hidraw に直に setPIN・changePIN を送れる）新しい攻撃面を作らない。hidraw の REMOVE で node は
fd が残っていても directory から消える（`hidraw.c:283-321` `drv_hidraw_unregister` の `cdev_unregister`）ので、replug を `pk_os_list` の poll で見る作りは成り立つ。
`KL_SYSTEM_HAS_*`・`KL_SYSTEM_CHANGED_*` は bit の並び（`keiland.h:1246-1281`）で、新しい bit を足す形は今の規則どおり。

---

## 2. 閉じていない・新しい指摘（i01・i02）

### N1（major、security）。「数えない」request の成功が、数えを消し Software Security Key の PIN を開ける（§9.3 の操作の表）

- **根拠:** `auth_granted` は AUTH・UNLOCK でない request の成功を全部「password で確かめた変更」と見て
  `sessiond_policy_success(count, SESSIOND_STYLE_PASSWORD)` を呼び、`wrong`・`pin_wrong` を 0 にし `signed_in = 1` にする
  （`sessiond/auth.c:847-859`、BUG-285 の直し）。STYLES・ENROLLED だけは `auth_listing` に分かれる（`auth.c:790-793`）。
  §9.3 は KEYINFO・KEYPIN を「数えない」とするが、成功の扱いを書いていないので、今の分かれ方に足すと `auth_granted` に落ちる。
- **起こる状況:** (1) 自動の login の session（password を一度も入れていない、`signed_in = 0`）で Settings の Security Keys の popup を開く →
  KEYINFO の成功 → `signed_in = 1` → lock の画面に 6 桁の PIN が出る（security.md「The PIN is offered only after…」と `auth-policy.c:12-17` の規則に反する）。
  (2) lock の画面で password・PIN を誤って遅れが育った後、何かの経路で KEYINFO が走ると `wrong`・`pin_wrong` が 0 に戻り、5 回で止まった PIN が再び試せる。
  §9.3 は「lock の間は問わない」とするが、守る場所（compositor か Settings か）が書かれておらず、sessiond は lock を知らない（review-1 B1）。
- **直し方:** §9.3 の表に列「成功の時」を足す: KEYINFO・KEYPIN は**数えず、数えを消さず、`signed_in` を立てない**（`auth_listing` と同じく policy に触れない経路）。
  KEYRESET は password を確かめたので今の ENROLL と同じく消してよい。KEYINFO・KEYPIN・KEYRESET の失敗も `auth_refused` の遅れと syslog の
  「failed change」を通さない（KEYRESET の password の誤りだけ通す）。lock 中の拒否は compositor（`server->locked` の時は Settings の鍵の request を EBUSY）で守ると書く。
  sessiond の host 試験（`plan/ws172/tests/sessiond-auth-host-test.c`）に「KEYINFO・KEYPIN の成功で wrong・pin_wrong・signed_in が変わらない」を足す。

### N2（major）。KEYRESET の 60 秒が中の時間の和より短く、鍵を初期化した後に殺されうる（§9.3 の表、Reset を 1 回の実行に）

- **根拠:** 中の時間は password の確かめ（65536 回の SHA-512 crypt）＋ silent の問いの helper（`HELPER_OPEN_MS` 2 s/鍵、`helper.c:35`）＋
  replug の待ち 30 s ＋ reset の helper（`device.c:134` の deadline は `FIDO2_TOUCH_MS` 30 s ＋ `DEVICE_SLACK_MS` 3 s、helper の alarm も 33 s、`fido2.h:50-51`）で、
  最悪 66 s を超える。sessiond は deadline で passkey の group に TERM、2 秒後に KILL する（`auth.c:231-235`）。group には passkey-fido2 と helper も入る。
  今の `SESSIOND_PASSKEY_KEY_MS` は 35 s（`auth.h:39`）で、60 s の定数はまだ無い。
- **起こる状況:** 挿し直しが遅く（25 s）触れるのも遅い（30 s 近く）と、鍵は reset を終えた直後に passkey-fido2 が TERM で死に、/etc/passkey の行は消えず、
  画面には `timeout`。鍵の全部の credential は消えているのに、画面は「失敗」と言い、この機械には死んだ行が残る。
- **直し方:** sessiond の KEYRESET の deadline を中の和＋余裕（例 75 s）にし、passkey-fido2 が自分の全体の deadline（replug 30 s、reset の helper 33 s）を持つと書く。
  reset を送った後は passkey-fido2 が TERM を保留し（`main_change` と同じく signal を止める）、答えと行の書き換えまで済ませる。それでも答えが無かった時は
  Settings が「鍵が初期化されたか分からない」と出し、ENROLLED を問い直すと書く。

### N3（major）。取り消し・時間切れで helper が殺されても、鍵は reset の触れる待ちを続ける（§9.3 の取り消し）

- **根拠:** CANCEL は passkey の group に SIGTERM を送るだけ（`auth.c:171-177`、`auth_kill`）。helper は既定の signal のまま（`helper.c:96-142`）で、
  CTAPHID の CANCEL を送らずに死ぬ。libpasskey には `pk_hid_cancel`（`hid.c:138`、transport の `cancel`、`ctap2.h:85`）があるが、helper はどこでも呼ばない。
  device.c も答えの無い helper を SIGKILL するだけ（`device.c:199-200`）。
- **起こる状況（CTAP の知識による推測）:** Reset の Touch の step で Cancel を押す → helper は死ぬが、鍵は authenticatorReset を受け取ったまま点滅して触れるのを待つ →
  利用者が（戸惑って、または押すのと同時に）触れる → 鍵は初期化される。画面は「取り消した」、/etc/passkey の行は残る。取り消しが効かない破壊的な操作になる。
- **直し方:** helper は SIGTERM を受けたら（または passkey-fido2 が helper を止める前に）開いている channel に CTAPHID_CANCEL を送ってから終わる、と §9.3 に書く。
  KEYRESET の取り消しは「reset を送る前」（replug の待ち）だけ確実で、Touch の step の取り消しは鍵の答え（`KEEPALIVE_CANCEL` 0x2d）を待って結果を出す。
  libpasskey の host 試験の software authenticator に「reset の触れる待ちで CANCEL を受けると 0x2d で終わり、消さない」を足す。

### N4（major）。鍵の確かめが replug の前に移り、別の鍵を初期化し、別の鍵の行を消しうる（§9.3 の Reset）

- **根拠:** §9.3 は「挿さった鍵 1 本に account の credential の有無を silent に問い（覚える）→ `status replug` → 現れたらすぐ reset」。挿し直された鍵が同じ鍵かを確かめない。
  review-1 M7 の直し方は「出来た直後に silent の問いと reset」だった。
- **起こる状況:** 鍵 A で問い（A の行を覚える）→ 抜く → 間違えて鍵 B を挿す → B が初期化され、/etc/passkey からは **A の**行が消える（A はまだ使えるのに login できなくなる）。
  B がこの account に登録されていれば B の行は死んだまま残る。2 本を同時に挿すと何が起こるかも書かれていない。
- **直し方:** replug の後、reset の前に silent の問いをする（CTAPHID の INIT・getInfo・getAssertion は 1 秒未満で、窓に収まる。推測）。消す行は**その答え**で決める。
  前の問い（または getInfo の AAGUID・vendor・product）と違えば reset せず `other-key`。待ちは「0 本を見てから 1 本」で、2 本以上は `many-keys` で止める。
  鍵が account の credential を 1 つも持たない時も reset はする（他の用途の鍵の初期化）と明記する。

### N5（major、M3 の残り）。長い鍵の操作が session の 1 本の通り道を塞ぎ、lock の解除と sleep を止める（§9.3、§4.5）

- **根拠:** backend は request を 1 つだけ待つ（`session-zedbsd.c:752-755`）。lock の画面の UNLOCK は EBUSY なら打った秘密を残して後の tick で送り直す
  （`greeter.c:1703-1708`）。sleep の POWER suspend は EBUSY が 3 秒（`KWL_SLEEP_SEND_MS`、`sleep-rules.h:32`）続くと失敗にする（`sleep-rules.c:159-165`）。
  lock・sleep は Settings の request を止めない（`handoff.c:179-191` は lock 中も system の答えを受ける）。
- **起こる状況:** Reset のウィザードの replug の待ち（最大 60 s 超、N2）の間に席を外す → idle の lock → idle の sleep は EBUSY で 3 秒後に失敗（蓋を閉じても眠らない。
  失敗は次の sleep も止める、`sleep.c:576-582`）→ 戻って password を打っても、解除は鍵の操作が終わるまで黙って待つ。今の ENROLL fido2（35 s）にも同じ穴があるが、
  i02 は 60 s 超の操作と、鍵の抜き差しのたびの KEYINFO を足す。
- **直し方:** compositor は lock（`kwl_lock`）と sleep の前に、待っている Settings の鍵の操作に CANCEL を送り（N3 の CTAPHID_CANCEL 付き）、答えを待ってから
  UNLOCK・POWER を送ると書く。lock 中は Settings の鍵の request を EBUSY（N1）。unlock の後に `KL_SYSTEM_CHANGED_KEYS` を 1 回出し、ウィザードが今の鍵を問い直す。
  greeter の KEYOWNER と AUTH の順（review-1 M3 の後半）は i04 の §9.4 に再掲する。

### N6（minor）。KEYRESET で password が正しかった後の失敗も password の誤りとして数える（§9.3 の表）

- **根拠:** 数えは passkey を走らせる前（`auth.c:479-483`）、消すのは成功だけ。CANCEL・時間切れは `timeout`（`auth.c:757-758`）で、遅れが付く（`auth.c:917-930`）。
- **起こる状況:** 窓に間に合わず `not-allowed`、触れずに `timeout`、Cancel、を 3 回 → 正しい password なのに 4 秒の遅れ。数えは account の全部の方式の共有なので lock の解除も遅れる。
- **直し方:** passkey-fido2 が password を確かめた直後に `status verified` を出し、sessiond はそこで数えを消す（password の成功と同じ）。その後の失敗は数えない。

### N7（minor）。`status replug` の通り道の 3 か所の変更が書かれていない（§9.3）

- sessiond の `auth_read` は `status touch` 以外の行を答えと見なす（`auth.c:684-692`）ので、`status replug` は今のままでは答え（`internal`）になる。
- backend は知らない行を EPROTO の答えにして request を終える（`session-zedbsd.c:803-858`、`TOUCH` だけ先に抜く `:790-795`）。新しい行（例 `REPLUG`）を TOUCH と同じく抜く。
- compositor の TOUCH は `pin_waiting && pin_key` の時だけ Settings へ（`system.c:986-1003`）。新しい操作も `pin_key` を立てる、REPLUG も同じ道、と書く。
  新しい answer の番号（`KL_BACKEND_SESSION_*`）か、ENROLL を使い回すかも決める（`handoff.c:186-191`）。

### N8（minor）。語の表は request の種類ごとに要り、helper の手元の検査は `device` のまま（§9.3 の失敗の語）

- sessiond の表は 1 本で、知らない語は `bad-secret`（`auth-policy.c:32-47`、`:222-223`）。「greeter の AUTH は今の語、Settings には そのまま」は表を 2 列にする事。
  UNLOCK も session の socket なので、「Settings の request」ではなく「AUTH・UNLOCK は写す、ENROLL・REMOVE・KEY* はそのまま」と request で分けると書く。
  0x34 を `key-replug` にすると、写さなければ AUTH・UNLOCK では今の `locked` から `bad-secret` に変わる。
- helper は鍵に PIN が無いと CTAP の 0x35 を待たずに `device` を出す（`helper.c:323-327`）。新しい PIN が規則に合わない時も libpasskey は手元で EINVAL を返し
  （`ctap2.c:986-993`）、helper はそれを `device` にする（`helper.c:351-352`）。→ それぞれ `no-pin`・`pin-policy` に写すと書く。
- 今の不具合（i01 で直す）: Settings の表は `key-locked`・`locked-account` を待つが（`page-users-keys.c:518-523`）、sessiond は両方を `locked` にして送る（`auth-policy.c:36,40`）。
  i01 のウィザードは `locked` を受ける行を持つ。

### N9（minor）。`KL_SYSTEM_CHANGED_KEYS` の取りこぼし（§9.3 の鍵の情報の問い）

- backend の INPUT は今は detail を見ずに `input_changed` にまとめる（`events-zedbsd.c:216-217`）。新しい host の callback（`keiland-backend.h:62-76` の `kl_backend_host` に 1 つ）が要る。
- OVERFLOW（`events-zedbsd.c:221-223`）でも CHANGED_KEYS を出す。事象の無い kernel・`events_fail` の後（`events-zedbsd.c:298-309`）は二度と来ないので、Insert の step に「Check again」か遅い poll の代わりを置く。
- KEYINFO は compositor の ENROLLED（account の object を作るたび、`system.c:861-864`）と重なると EBUSY。Settings は EBUSY を次の CHANGED_RESULT で問い直す、と書く。

### N10（minor、M7 の残り）。同じ鍵を登録した他の account の行は reset の後も残る（§9.3）

- 決めればよい: 残して security.md に書く（その account の Remove で消せる）か、passkey-fido2 が replug の後の問いで全 account の行を照らして消す（開示は無い: 結果は行の削除だけ）。

### N11（minor）。i01 のウィザードの手順が書かれていない（§7 の i01、§9.3）

- §3.3 の Insert（KEYINFO）・Name の既定（鍵の名前）・Set PIN は i02 の物。i01 は「Password → Name（既定「Security Key」）→ 鍵の PIN → Touch → Done」になり、
  `bad-secret` は password と鍵の PIN のどちらか分からない（今の helper、`helper.c:355-356`）ので「The password or the key's PIN is wrong.」と出して Password の step に戻す、と書く。

### N12（minor）。ウィザードの間 password が Settings の memory に残る（§3.3、§3.5）

- Add の Password は step 1、送るのは step 6。Insert の待ちは何分でも続く。秘密の欄を送った時・閉じた時に消すだけでなく、「ウィザードが時間（例 2 分）を越えたら Password からやり直す」
  か、password を最後の step で聞く（D4 の判断に関わる、J5）を書く。

### N13（minor）。reset の窓の言葉

- YubiKey は「挿してから 5 秒以内」に reset を送る必要があると記憶している（推測、未確認）。§3.5・§4.4 の「10 秒」は「鍵による（CTAP2 は 10 秒以内、YubiKey は 5 秒の見込み）」に直す。
  §9.3 の「現れたらすぐ」の作りはどちらでも成り立つ。

### N14（minor）。試験の計画に sessiond の項目が無い（§9.3 の試験）

- `plan/ws172/tests/sessiond-auth-host-test.c` に: KEYINFO・KEYPIN が数えず消さない（N1）、`status replug` が答えにならず REPLUG で伝わる（N7）、KEYRESET の deadline（N2）、
  `status verified` の後の失敗を数えない（N6）、語の表が request で分かれる（N8）。5330 の UAT に「Touch の step で Cancel して触れても初期化されない」（N3）と
  「replug で別の鍵を挿すと止まる」（N4）、「reset 中に lid を閉じると取り消して眠る」（N5）を足す。

### N15（minor、記録）。Phase の表と WS200 の記録

- `plan/ws199/ws.md` の Phase の表は p001・p002 のまま（§9.2 の分け方は Q1 待ち）。`plan/ws200/ws.md` に WS199 i01 の popup の部品への依存と 1 行の options（J4）が無い。どちらも Q1 の記録。

---

## 3. i03・i04（§9.4）の方針の確かめ

方針は review-1 の B1・M1・M2・M6・M8・M12・m1・m7〜m13 に答えていて、向きは妥当。次の 3 点を i03・i04 の詳しい設計で扱う。

1. **復帰の猶予 5 秒は時間の当て推量。** 数え直しは hub・dock の後ろの鍵や遅い controller で 5 秒を越えうる（5330 は未確認）。時間ではなく「復帰の後、lock の画面に
   利用者の入力が来るまでの ADD は数えない」か「自動では送らず 1 押し」に寄せると、時間に依らない。
2. **「lock の後に挿した鍵」は物理的に居る人に対しては守りにならない。** 挿しっぱなしの鍵は、抜いて挿し直せば「後に挿した鍵」になる。触れるのと同じく手が要るだけで、
   守るのは「人の居ない契機（暗い復帰・再列挙）での解除」だけ。security.md にそう書き、J2 でユーザーに示す（タッチ不要は「鍵を持つ人は誰でも」の警告の範囲）。
3. `auth-fido2` の node の一覧は、helper がどの node の鍵が答えたかを返す必要がある（今の message に node は無い、`fido2.h:23-28`）。passkey-fido2 は node ごとの handle と
   up=false の可否を helper の job に渡す形にする。greeter の KEYOWNER と AUTH の順（review-1 M3）も再掲する。

HAL（`include/hal/hal.h`・`src/hal/`）・kernel の UAPI の変更は i01〜i04 のどれにも要らない（hidraw の事象は今の `/dev/system` の INPUT で足りる）。

---

## 4. i01・i02 の判定

| i | 判定 | 条件 |
| --- | --- | --- |
| i01 | **GO**（J4・J5 の答え待ち。推しで始めて答えで直してもよい） | N11（i01 の手順と `bad-secret` の戻り先）、N8 の今の不具合（`locked` の行）、N12 の秘密の寿命を phase.md に 1〜2 行。部品の API は WS200 も使う前提で固める（J4） |
| i02 | **直してから** | N1・N2・N3・N4・N5 を §9.3 に書く（どれも文の直し）。N6〜N9・N14 も同時に。直した版は全体の再レビューでなく、この 5 点の短い確認で足りる |

## 5. 確かめていないこと（推測）

- CTAP2: 触れる待ちの reset が host の消滅で止まらない事、CTAPHID_CANCEL で 0x2d になる事、reset の前の getAssertion が窓を閉じない事、YubiKey の 5 秒。
- 5330 で復帰の後に xHCI が数え直すか、その時間。
- reset の helper の deadline の和は code の定数から出した。passkey の password の確かめの実時間は測っていない。
