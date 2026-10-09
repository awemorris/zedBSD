# ws199-p001 第 4 版の短い再確認（review-3）

対象: [phase.md](phase.md) 第 4 版（branch agent/p1、commit 63e430c09）。前回: [review-2.md](review-2.md)。範囲は依頼の 4 点だけ（全体の再レビューではない）:
review-2 の N1〜N5 の閉じ方、ユーザーの決定（ws.md「設計の review の後の決定」）の反映、§5（NFC、BUG-286）、i01 の具体さ。
照合した code（63e430c09 の tree）: `userland/desktop/sessiond/`（auth.c・auth.h・auth-policy.c・seat.c・main.c・greeter.c・session.c）、
`userland/base/passkey/main.c`・`request.c`、`userland/base/passkey-fido2/`（main.c・device.c・helper.c・wire.c・fido2.h）、
`userland/base/libpasskey/`（os-zedbsd.c・os.h・hid.c・os-posix.c）、`userland/desktop/libkeiland-backend-zedbsd/session-zedbsd.c`、
`userland/desktop/wayland/`（greeter.c・sleep.c・sleep-rules.c・system.c）、`userland/desktop/settings/`（pages.c・settings.h・page-users*.c・Makefile*）、
`userland/desktop/include/keiland/keiland.h`、`src/drivers/generic/smartcard.c`、`src/drivers/usb/usb-ccid.c`、`src/kern/system-event.c`、
`include/uapi/system.h`、`userland/base/volumed/main.c`、`plan/ws200/ws.md`。
CTAP2・NFC の挙動は手元に仕様が無く知識による（「推測」と書いた所）。build・試験は走らせていない（設計の読み合わせだけ）。
注: review の間、この worktree には i01 の未 commit の変更（`settings/dialog.c` が untracked、`settings.h`・`page-users-keys.c`・`page-users-pin.c` が変更中）が
あった。設計の照合は tracked の code で行い、未 commit の code は §3.2 の API の名前を見ただけ。

判定の要約: **i01 は GO**（R11 の細部を phase.md に足すか、実装で決めて記録する）。**i02 は R8・R9 の文の直しで GO**。
**i03 は直してから**（R1: /sbin/passkey が SIGTERM で先に死ぬため、§4.5 の「TERM を止める・helper の答えを待つ」が成り立たず、N2・N3・N5 が閉じていない）。
**i04・i05 は直してから**（R2: KEYOWNER の成功が policy に触れる道、R3: NFC の置いたままの鍵での自動の login、R4: 数えの規則の矛盾、R5: greeter・lock の自身の fido2 の待ちが sleep を止める）。
HAL（`include/hal/hal.h`・`src/hal/`）の変更は要らない。UAPI の layout も変わらない（R8 に注釈の 1 行だけ）。

---

## 1. review-2 の N1〜N5 の閉じ方

| # | 状態 | 一言 |
| --- | --- | --- |
| N1 | **Settings の操作では閉じた**。KEYOWNER が残る（R2） | §4.4 の「成功の時」の列、失敗の遅れ・syslog を通さない、lock 中の EBUSY は code と合う（`auth.c:789-800` の分かれに `auth_listing` 相当の道を足す形）。§4.2 の KEYOWNER の成功・失敗の扱いが書かれていない |
| N2 | **閉じていない**（R1） | 75 s ≥ 中の和（password ＋ replug 30 s ＋ helper 33 s、約 64 s）と passkey-fido2 の自分の deadline は良い。だが「reset を送った後 TERM を止める」は passkey-fido2 だけで、親の /sbin/passkey が TERM で死ぬので sessiond は待たずに `timeout` を答える |
| N3 | **一部**（R1） | helper の SIGTERM → CTAPHID_CANCEL の方針は良い（poll は EINTR で続く、`os-posix.c:90-91`）。だが鍵の答えを待つ間に sessiond は既に答えを返し、passkey-fido2・helper は孤児になる。「sessiond の KILL まで 2 s」は起きない |
| N4 | **閉じた** | replug の後・reset の前に問い、その答えで消す行を決める。0→1 本、2 本は `many-keys`、credential の無い鍵も reset |
| N5 | **一部**（R1・R5） | Settings の鍵の操作は lock・sleep の前に CANCEL、lock 中は EBUSY、unlock の後に CHANGED_KEYS。だが (1) CANCEL の答えは R1 で操作の終わりより先に来る、(2) CANCEL された答えも policy の遅れ（2〜16 s）で保留され「最大 3 s」を越えうる、(3) greeter・lock の自身の AUTH・UNLOCK の fido2 の触れる待ち（i05 で自動に始まる）も sleep を止めるのに §4.7 は Settings だけ |

---

## 2. 指摘

### R1（major、N2・N3・N5 の残り）。/sbin/passkey が SIGTERM で先に死に、sessiond は鍵の操作の終わりを待たずに答え、passkey-fido2・helper が孤児になる（§4.5・§4.7）

- **根拠:**
  - sessiond は passkey を新しい process group で起こし（`sessiond/auth.c:616`・`:636`）、CANCEL と deadline は group 全体に SIGTERM（`auth.c:171-177`、`:231-233`、`auth_kill` `:704-713`）。
  - /sbin/passkey は signal を既定にし（`passkey/main.c:189-193`）、passkey-fido2 を待つ間も TERM を止めない（`passkey/main.c:577-582`）。→ TERM で即座に死ぬ。
  - sessiond は passkey の終わりを `waitpid` で見て（`auth.c:238-244`）、答えが無く `term_ms` があれば `timeout`（`auth.c:757-758`）、出力を閉じ（`:739`）、`pid = 0` にする（`:738`）。
    KILL は `pid > 0` の間しか送られない（`auth.c:225-235`）ので、**孫（passkey-fido2・helper）には KILL が来ない**。
  - §4.5 は「reset を送った後、passkey-fido2 は SIGTERM を止め」「helper は SIGTERM で CTAPHID_CANCEL を送り鍵の答えを待つ」「passkey-fido2 は helper の答えを待ってから終わる（sessiond の KILL まで 2 s）」。
- **起こる状況:**
  1. Reset の Touch の step で Cancel → sessiond はすぐ `FAIL timeout`（遅れ 2 s 以上の後）。その時、鍵はまだ CANCEL の答えを返しておらず、利用者が同時に触れていれば
     reset は成功し、孤児の passkey-fido2 が後で /etc/passkey の行を消す。画面は「may or may not」と言い §3.5 の ENROLLED の問い直しを**行の書き換えの前に**走らせうる（一覧が古いまま）。
  2. 答えの後すぐの次の request（CHANGED_KEYS の後の KEYINFO、ENROLLED、lock の KEYOWNER・UNLOCK）が、孤児の helper が握る hidraw の GRAB（`os-zedbsd.c:128-137`）や
     smartcard の claim（`smartcard.c` の `smartcard_power_on`、他の file は EBUSY）に当たり `no-key`・`device`・`many-keys` 等になる。
  3. §4.5 の最後の行（CANCEL を assert・make の触れる待ちにも）: assert・make では passkey-fido2 は TERM を止めないので死に、helper だけが TERM を捕まえて生き残る。
     鍵が CANCEL に答えなければ helper は alarm（`helper.c:119`、33 s）まで GRAB を握る。
  4. §4.7 の lock・sleep の CANCEL の「答え」は操作の終わりを意味しない（sleep に入る時、孤児はまだ鍵と話している）。
- **直し方:**
  - /sbin/passkey は passkey-fido2 に渡す操作（ENROLL・REMOVE・AUTH の fido2、KEY*、KEYOWNER）の間、SIGTERM・SIGHUP・SIGPIPE を無視（SIG_IGN）して passkey-fido2 の終わりまで待つ、と §4.5 に書く。
    group の TERM は passkey-fido2 と helper に届き、sessiond の exchange は passkey-fido2 が終わるまで busy のままになる。
  - passkey-fido2 は assert・make の間も TERM を flag で受け、helper の答え（CANCEL の結果）を最大 1.5 s 待ってから helper を KILL して終わる。reset を送った後は今の設計どおり止める。
  - sessiond の猶予 2 s（`SESSIOND_PASSKEY_GRACE_MS`、`auth.h:40`）を上限とし、それで KILL された時だけ「may or may not」にする。ENROLLED の問い直しは exchange の終わり（答え）の後。
  - helper の TERM の handler は「write だけ」より、flag を立てて KEEPALIVE の callback（`hid.c` の KEEPALIVE は触れる待ちの間 100 ms ごとに来る、推測）で `pk_hid_cancel` を呼ぶ方が、
    channel ID を handler に渡さずに済み簡単（handler は async-signal-safe のまま）。
  - host 試験（`plan/ws172/tests/sessiond-auth-host-test.c`）: TERM を無視して 1 s 後に `ok` を答える偽の passkey で、答えが `timeout` でなく `ok` になる事。

### R2（major、security、i05）。KEYOWNER の成功・失敗の扱いが無く、今の分かれでは greeter で数えを消し PIN を開けうる（§4.2・§4.4）

- **根拠:** auth_finish は STYLES・ENROLLED 以外の成功を `auth_granted` に送り、AUTH・UNLOCK でない成功は `sessiond_policy_success(count, SESSIOND_STYLE_PASSWORD)`
  （`auth.c:789-800`、`:854-859`）で `wrong`・`pin_wrong` を 0、`signed_in = 1` にする（`auth-policy.c:175-183`）。PIN は `signed_in` の後だけ出る（`auth-policy.c:149-160`）。
  name が `-` の KEYOWNER は `auth_lookup` が外れて uid が -1 なので、今の code のままなら `ok` も `bad-secret` に変わり（`auth.c:780-782`）、`auth_refused` の遅れと
  「failed change」の syslog を通る（`auth.c:901-931`）。§4.2・§4.4 はどちらの道かを書いていない。
- **起こる状況:** 実装が KEYOWNER の答えの user で count を引き `auth_granted` に流すと、**鍵を挿しただけ**（秘密無し、up=false）で、その user の
  `signed_in` が立ち数えが消える。greeter は PIN を login に出し（STYLES の filter）、挿すたびに `pin_wrong` が 0 に戻るので、鍵を持つ人は 6 桁の PIN を無制限に試せる。
  逆に `auth_refused` に流すと、鍵を挿すたびに共有の unknown の count に遅れが付き、他の request が `ERROR busy` になる。
- **直し方:** §4.4 の表に KEYOWNER の行を足す: **数えない・数えを消さない・`signed_in` を立てない・遅れ無し・syslog の failed を通さない**（`auth_listing` と同じ道）、
  答えの uid の照合（`auth.c:780-787`）の例外。`auth_parse` の許し（`auth.c:341-351`）を「KEYOWNER は greeter と session、session の時は name を owner に固定」に。
  host 試験に「KEYOWNER の成功で wrong・pin_wrong・signed_in が変わらない、STYLES に pin が増えない」。

### R3（major、i05、ユーザーの決定「login は常にタッチ」）。NFC の reader に置いたままの鍵で、greeter が人の操作無しに login する（§3.6・§5.2）

- **根拠:** §3.6「greeter は画面が出た時と鍵が来た時に KEYOWNER」「key-pin=0: …とすぐ `AUTH user fido2`」、§5.2「NFC の鍵は場に入った事で user presence を満たす」。
  CCID の slot は card を置いたままなら PRESENT のまま（`smartcard.c:217-257`、事象は出入りの時だけ）。
- **起こる状況:** key-pin=0（Touch only）の user が鍵を ACR1552 に置いたまま log out する（または置いたまま起動する）→ greeter が出る → KEYOWNER → `user=` → AUTH →
  helper が card に電源を入れて assertion（up=true、NFC は場に居る事で満たす）→ **誰も触らずに desktop**。「Try again」も card が置かれたままなら同じ。
  lock の key-touch=1・key-pin=0 でも、置いたままの NFC の鍵では「タッチ」がswipe だけで満たされ、§3.1 の「Touch only」の警告（"with a touch"）が実際と合わない。
- **直し方（設計の判断。必要ならユーザーに確かめる）:** 推し:「NFC の card は、試みの**始まりの後に** INSERTED が来た時だけタッチと見なす」。
  greeter は画面が出た時に既にある card では自動で AUTH を始めず（KEYOWNER で user を選ぶだけ）、card の REMOVED→INSERTED（当て直し）で始める。
  passkey-fido2 の `login`（と `unlock` の key-touch=1）では、helper は始めに PRESENT の card に問わず、INSERTED を待つ（`tapped during the attempt` と同じ待ち）。
  §3.1 の Touch only の警告にも「NFC の鍵を reader に置いたままにすると、swipe だけで解除される」を足す。5330 の UAT に「置いたまま log out → greeter で自動で入らない」。

### R4（major、i05）。自動の試みの数えの規則が §3.6 と §4.3、code で食い違う

- **根拠:** §3.6「自動で始めた試みが取り消し・時間切れ・鍵の抜けで終わった時は数えない（鍵が答えて署名が合わなかった時だけ数える）」、§4.3「失敗の数え・遅れは今どおり」。
  今の sessiond は passkey を走らせる**前に**数え（`auth.c:475-483`、`auth-policy.c:12-13`「an attempt cut short counts as a failure」）、成功だけが消す。
- **起こる状況:** §4.3 のとおりに作ると、greeter で鍵を挿して 30 s 触れない・抜く・Cancel のたびに持ち主の `wrong` が増える。`wrong` は password・PIN と共有なので、
  鍵を何度か挿し抜きしただけで password の login にも 4〜16 s の遅れが付く。§3.6 のとおりに作るには sessiond が「鍵が答えた」を知る手段が要るが、書かれていない。
- **直し方:** §4.3 に規則を 1 つにして書く。例: AUTH・UNLOCK の fido2 は前もって数えるのをやめ、答えの語で数える（`bad-secret`（鍵の PIN の誤り）・`cloned`・署名の不一致は数える、
  `timeout`・`canceled`・`no-key`・鍵の抜けは数えない）。鍵の PIN は鍵が自分で数える（8 回、電源ごと 3 回）ので、sessiond で数えない失敗が PIN の総当たりを開けない事を security.md に書く。
  host 試験に「fido2 の timeout・CANCEL で wrong が変わらない、bad-secret で増える」。

### R5（major、N5 の残り、i03・i05）。greeter・lock の自身の fido2 の待ちと、CANCEL された答えの遅れが sleep を止める（§4.7・§3.6・§3.7）

- **根拠:** backend は request を 1 つだけ待つ（`session-zedbsd.c:752-755`）。sleep の POWER は EBUSY が `KWL_SLEEP_SEND_MS`（3 s）続くと失敗し（`sleep-rules.c:160-163`）、
  失敗は次の sleep を止める（`sleep.c:575-584`）。sessiond は失敗の答えを policy の遅れ（2 s、3 回ごとに倍、最大 16 s、`auth-policy.c:186-205`）だけ保留し、
  その間も busy（`auth.c:119-121`、`:928-930`）。§4.7 の CANCEL は Settings の操作だけ。
- **起こる状況:**
  1. i05 の greeter: 鍵を挿す → 自動の AUTH が触れるのを 30 s 待つ → 蓋を閉じる → POWER は EBUSY のまま 3 s → sleep 失敗（鞄の中で眠らない）。lock の key-touch=1 の待ちも同じ。
  2. Settings の ENROLL・KEYRESET（verified の前）・SETOPTIONS は前もって数えるので、CANCEL の答え（`timeout`）は `wrong` が 4 以上なら 4 s 以上保留され、§4.7 の「最大 3 s」が切れる。
- **直し方:** §4.7 を「sleep の前に、待っている fido2 の request（Settings・greeter・lock のどれでも）に CANCEL」に広げる。§3.7 の「sleep で card を閉じる」と同じ所で行う。
  sessiond は CANCEL・deadline で終わり passkey の答えを読んでいない exchange の失敗を**遅れ無しで**答える（判定を読んでいないので推測の oracle にならない。数えは R4 の規則のまま）。
  lock は答えを待たずにすぐ画面を出し（lock を 3 s 遅らせない）、POWER だけが答えを待つ。

### R6（minor、i04・i05）。§4.6 の語の写しでは `pin-required`・`cloned` が greeter・lock に届かない（§3.6）

- §4.6 は AUTH・UNLOCK の「他の新しい語→`bad-secret`」、今の表は `cloned`→`bad-secret`（`auth-policy.c:43`）。§3.6 は `cloned` と `pin-required`（alwaysUv の鍵で PIN の欄に移る）を出すとする。
- このままでは key-pin=0 の user の alwaysUv・credProtect の鍵は「wrong」とだけ出て PIN の欄に移らない。→ AUTH・UNLOCK の表に `pin-required` を通す（秘密の当否でなく鍵の性質なので oracle にならない）。
  `cloned` は今どおり `bad-secret` に写すなら §3.6 から消す。

### R7（minor、i03）。`status verified` の出し手と後始末（§4.5・§4.4）

- §4.5 の 1 は「passkey が password を確かめる」だが、fido2 の操作は passkey が request を丸ごと passkey-fido2 に渡し（`passkey/main.c:103-111`）、password は passkey-fido2 が確かめる
  （`passkey-fido2/main.c:135-142`）。KEYRESET も同じなら、`status verified` は passkey-fido2 が `login_verify` の直後に出す、と書く。
- sessiond は `status verified` を KEYRESET（と、使うなら SETOPTIONS）の時だけ受け、他の request では無視する。
- verified の後の失敗も `auth_refused` を通ると、`wrong = 0` でも遅れは 2 s（`auth-policy.c:194-196`）で「failed change」が syslog に出る（`auth.c:918`）。表の「数えない」に「遅れ無し、syslog は reset の失敗として」を足す。

### R8（minor、i02）。§5.3 の kernel の事象: 関数の名前、文脈の答え、slot の ADD、UAPI の注釈

- `smartcard_card_changed` は無い。card の出入りは `drv_smartcard_card`（`src/drivers/generic/smartcard.c:217-257`）で、呼び出し元は `usb_ccid_worker`（kernel thread、
  `sched_sleep` する、`src/drivers/usb/usb-ccid.c:457-488`）と loopback（`smartcard-loopback.c:220-221`）。`kern_system_event_post`（`src/kern/system-event.c:73-113`）は
  spinlock と waitq と poll_notify だけで、`smartcard_post`（`smartcard.c:994-1011`）は既に register・unregister（thread の文脈）から呼ばれている。
  → **呼べる**（spinlock を外した後、`smartcard_event` の隣で `smartcard_post(.., KERN_SYSTEM_EVENT_CHANGE)`、card の有無を引数に足す）。§5.3・§10 の「確かめる」「worker に移す」は消してよい。
  detail は 64 byte（`system.h` の `KERN_SYSTEM_EVENT_DETAIL_MAX`）で、`card=` を name の前に置く設計は name が切れても壊れない。
- card を載せたまま reader を挿すと slot の ADD だけで CHANGE は来ない（初めの状態は `description->present`、`smartcard.c:177-179`）。backend は smartcard の ADD・REMOVE でも `keys_changed` を呼ぶ。
- UAPI の layout は変わらないが、`include/uapi/system.h:327` の USB の注釈は「attached or detached」で、CHANGE は新しい意味。注釈の 1 行と security-keys.md に detail の形を書く（Q1 の承認の範囲に含める）。
  既存の購読者: volumed は USB の事象のたびに disk を数え直す（`volumed/main.c:298-305`）→ NFC を当てるたびに scan が走る（害は小さい。WS199 の外なので Q1 に送る）。seat.c は USB で node を渡し直す（安い）。

### R9（minor、i02）。§5.1・§5.2 の slot の扱いの細部

- 電源・SELECT が通らなかった slot（SAM、FIDO でない card）は、claim を握ったまま（`smartcard_power_on` は成功した時だけ claim を残す。SELECT の失敗では残る）にせず、
  `CCID_POWER_OFF` で放す（session の user の program を塞がない）。libpasskey に `pk_os_card_power_off`（attach は保つ）を足す。
- assert の「どれも持たず slot が有れば当てるのを待つ」は、USB の鍵が挿さっているが登録が無い時にも 30 s 待つ事になる（5330 は内蔵の reader で slot が常にある）。
  「USB の鍵が 1 本も無い時だけ当てるのを待つ」にする。KEYOWNER・KEYINFO は待たない、と書く。
- 推測（未確認）: 非接触の reader では `CCID_POWER_OFF`（close）で RF が切れ、card が REMOVED→INSERTED と見えるものがある。KEYOWNER・AUTH の電源の入り切りが CHANGE を生むと
  `keys_changed` → KEYOWNER の 1 秒ごとの輪になる。5330 の ACR1552 で確かめ、要れば passkey-fido2 の電源の切りの後の短い時間の CHANGE を backend が無視する（`changes` の数で見分ける）。
- 確かめて問題の無かった点: root が開いた slot の fd を `_passkey`・chroot の helper が使う作りは成り立つ（ioctl に資格の検査は無い、`smartcard.c:456-499`。claim は file ごとで、
  fork で同じ file を共有する）。seat.c が session の間 node を user の 0600 にしても（`seat.c:129-134`）root の open と開いた後の fd には効かない。
  card の事象は open ごとの queue（`CCID_EVENT_QUEUE` 16）なので、root が attach した後に来た INSERTED は helper の read で取りこぼさない。

### R10（minor、i05）。KEYOWNER の「1 秒に 1 回、最後の 1 つにまとめる」の置き場所

- sessiond の exchange は 1 つずつで、重なれば `ERROR busy`（`auth.c:184-189`）。sessiond にまとめの slot を作るより、greeter（compositor）が 1 秒の間隔と「最後の 1 つ」を持ち、
  sessiond は KEYOWNER を 1 秒に 1 回を越えたら `ERROR busy` で断るだけ、と書く方が簡単。KEYOWNER の間の password の Enter は今の `greeter_submit_pending` で後に送る（`greeter.c:903-904`）と書く。

### R11（minor、i01）。i01 をそのまま実装するための細部

- **API の名前:** §3.2 は `settings_dialog_open(spec)`…だが、Settings の名前は `se_`（`settings.h`）で、作りかけの code は `se_dialog_open(app, act, ready)`・`se_dialog_step`・
  `se_dialog_busy(app, text, cancellable)`…。WS200 が使うので §3.2 を実際の名前と引数に直す。
- **file の一覧:** §8 の i01 は「dialog.c・page-keys.c・Makefile 3 つ」だが、頁を足すには `settings.h` の `enum se_page_id`（Users の次、`settings.h:256`）と `se_app` の popup、
  `pages.c` の行（`pages.c:44` の次）、`search.c` の項目、`ui.c` の押下・key・tick を popup が開いている時は popup へ、`page-users.c` の鍵・PIN の card を消して link、glyph（今ある
  `SE_GLYPH_LOCK` か新しい鍵の絵）、Makefile 3 つ（`Makefile:13`、`Makefile.linux:17`、`Makefile.freebsd:17`）が要る。作りかけは新しい page-keys.c でなく
  page-users-keys.c・page-users-pin.c を変えている。どちらかに揃えて §8 に書く。
- **能力の bit:** `KL_SYSTEM_HAS_KEY_OPS` は i03 まで無い。i01 の「Not available on this system」は今の `KL_SYSTEM_HAS_KEYS`・`KL_SYSTEM_HAS_PIN`（`keiland.h:1254`・`:1256`）で決める。
- **数の上限:** 鍵は 5 本まで（`KL_SYSTEM_KEYS_MAX`、`keiland.h:2105`。6 本目は passkey-fido2 が `bad-request`）。5 本の時 Add Key を灰色に。
- **名前:** sessiond は label の頭の空白を落とす（`sessiond_policy_word` が区切りを飛ばし、label は word[2] から、`auth.c:396`）。Settings で前後の空白を除いてから検査し、
  既定の名前の重複も除いた形で比べる。許す文字は `fido2_label_valid`（`passkey-fido2/wire.c:371-392`: 1〜32 byte、制御文字・DEL・`:` 不可、UTF-8 は可）。
- **鍵の PIN:** 空は送らない（今の make は UV を求める、`passkey-fido2/main.c` の `PK_FLAG_UV` の検査）。PIN の無い鍵は今 `device`（`helper.c:326`）になるので、i01 の `device` の行に
  「If your key has no PIN yet, set one first, then try again.」を足す（i03 の Set PIN まで）。
- **Remove と他の button:** Remove の popup（1 Password → busy → 閉じて一覧を直す）と、Change PIN・Reset Key の button を i01 で隠すか灰色にするかを 1 行。
- **2 分:** 入力の無い間も popup の tick（`se_dialog_tick`）が回るよう、Settings の main loop が時間で起きるかを確かめる（起きなければ popup が開いている間だけ timeout を付ける）。

### R12（minor、記録、N15 の残り）

- `plan/ws200/ws.md` にはまだ WS199 i01 の popup への依存と /etc/passkey の 1 行の options（§2）が無い（Queue の行だけ）。Q1 の記録。

---

## 3. ユーザーの決定の反映（依頼の 2）

| 決定 | 反映 | 穴 |
| --- | --- | --- |
| login は常にタッチ | §3.6・§4.3。up=false は `unlock` かつ key-touch=0 の時だけで、sessiond が AUTH→`login`、UNLOCK→`unlock` を決める。greeter は UNLOCK を送れない（`auth.c:341-351`）ので greeter からは外せない | NFC の置いたままの鍵（R3）。KEYOWNER の成功の道（R2） |
| タッチ不要は unlock だけ、swipe で card が出た後だけ | §3.7。card は swipe（`greeter.c:1547`）か修飾でない key（`greeter.c:673-682`）で出て、30 s で消える（`greeter.c:906-918`）。hint の間の鍵の出入りは何もしない、sleep で card を閉じる | sleep で card を閉じる所で、待っている UNLOCK の fido2 も CANCEL する（R5）。lock が既に掛かっている時の sleep は `kwl_lock` を通らない（`sleep.c:530-536`）ので、card を閉じる処理は sleep の側に置く |
| 置いたままの鍵でも swipe の後は解除される（警告） | §3.1 の警告の文 | Touch only の警告も NFC では swipe だけになる（R3） |
| PIN 不要・タッチ不要に password と警告 | §3.1・§4.4 SETOPTIONS | 無し |

## 4. 判定

| i | 判定 | 条件 |
| --- | --- | --- |
| i01 | **GO** | R11 を phase.md に足す（または実装で決めて記録）。N5 の穴は今の ENROLL fido2（35 s）にも残る（i03 で直す既知の事） |
| i02 | **GO（R8・R9 の文の直しの後）** | R3 の判断（NFC の card を「始まりの後の INSERTED」だけタッチと見るか）は i02 の helper の待ちの作りに関わるので、i02 の前に決めておくのが安い |
| i03 | **直してから** | R1（passkey の TERM の無視、passkey-fido2 の assert・make の TERM、猶予、問い直しの順）、R5 の 2、R7 |
| i04 | **直してから** | R6、R4 の規則（auth-fido2 の数え） |
| i05 | **直してから** | R2、R3、R4、R5 の 1、R10 |

直した版は全体の再レビューでなく、R1〜R5 の短い確認で足りる。

## 5. 確かめていないこと（推測）

- NFC の FIDO の鍵が、置いたままの card への新しい電源の入り（CCID_POWER_ON）のたびに user presence を満たすか（R3 の前提。§5.2 の記述に従った）。
- 非接触の reader の POWER_OFF で card が REMOVED→INSERTED に見えるか（R9）。
- 鍵が reset の触れる待ちで CTAPHID_CANCEL に 0x2d で答えるか、その速さ（R1 の 1.5 s の見込み）。
- Settings の main loop が入力の無い時に tick するか（R11）。
- 作りかけの i01 の code（untracked の dialog.c など）の中身は読んでいない（API の名前だけ）。
