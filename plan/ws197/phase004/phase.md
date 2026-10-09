<!-- awesome-plan project=zedbsd record=ws197-p004 -->

# ws197-p004: SMS の層の流れと境界の interface（Phone の app・libkeiland・compositor・libkeiland-backend・bluetoothd）の詳細設計

Phase ID: `ws197-p004`
Parent: [WS197](../ws.md)
Related: [WS170](../../ws170/ws.md)（Phone の app、保存の持ち主）
Status: planning（2026-10-10 P1: 詳細設計の第 3.1 版。[review-1.md](review-1.md)（blocker 3・major 13）、[review-2.md](review-2.md)（blocker 0・major 6・minor 14）、review-3（major 1・minor 6、Event の行）を本文に入れた（各節の `[B1]`・`[M1]`・`[MA]`・`[MG]`・`[m]`・`[n]` の印）。残りの判断は §9 で Q1 経由でユーザーへ。p004a〜c の実装は p003 の後）
Phase disposition: normal
Queue: Q1 の投入（2026-10-10「WS197 p004 の詳細設計として、SMS 利用における Phone app・libkeiland・libkeiland-backend・bluetoothd の流れと interface を確定し、別の session で判断の違いが出ないようにする」）

由来（2026-10-10 ユーザー）:「P1はWS197に戻る前に、WS170のlibkeiland-backendにおけるMAPの実装について、bluetoothdをどう叩くのか、それから、bluetoothdに何が実装されるべきか、このあたりを明確にしておいて、別なセッションで判断の違いが生じないようにしてください。全般的に、SMS利用におけるPhone app, libkeiland, libkeiland-backend, bluetoothdの流れを明確にして、どのようなインタフェースになるか、関連WSに記載してください。」

**この文書が正**: WS197 p001 §8（第 1 案）と p003 §9（bluetoothd の socket）と食い違う所は、この文書が後の決定で、p001 §8 の該当の行を置き換える（§10 の表）。p003 の i07 はこの文書の §5 の文法と §5.4 の権限で実装する。

版: 2026-10-10 第 1 版（274cc2c20）→ 同日 第 2 版（review-1 の指摘、e9a737e2f）→ 第 3 版（review-2 の指摘）。

前提のユーザーの決定（p001 §11、2026-10-09「全部推しどおり」）: Q1 スマホは 1 台、**Q2 持ち主だけが見る・bluetoothd は中継だけ・保存は Phone の app の `~/Documents/Phone/`**、Q3 スマホの連絡先は別の組、Q4 最初の同期は過去 30 日・folder ごとに最大 500 通、Q5 MMS は範囲外、Q8 banner と lock の画面、**Q10 Linux・FreeBSD の Keiland は作らない（「無い」の backend だけ）**、Q14 logout で切る、Q16 (a) MAP 1.1。

範囲: **SMS だけ**。連絡先（PBAP、p005）と通話（HFP、p006）は同じ形で足せるように名前と場所を空けるが、interface は p005・p006 の詳細設計で決める（ws.md の p004 の行の「通話・連絡先」はこの文書では扱わない。行の直しは Q1 に頼む）[m]。

## 0. 用語

- **handle**: bluetoothd が socket に出す 1 通の名前 `<session 8 桁の 16 進>.<スマホの handle 16 桁の 16 進>`。MAP の session（MAS の接続）の間だけ有効。保存しない（app の memory の表だけ、§6.5）。
- **key**: 1 通の重複の鍵（16 桁の 16 進、p003 §8.3）。MAP の session を越えて同じ。保存の `Source` と file の名前に使う。日時の無い 1 通は `-`（partial）。
- **cursor**: 1 回の同期の中だけで使う続きの目印（bluetoothd が作り、他の層は中身を読まない）。
- **目印（sync mark）**: Phone の app が保存する「どこまで取り込んだか」（§6.2）。
- **持ち主**: bluetoothd の有効な `.phone` の記録の uid（p003 §3）。
- **番号の鍵** [MD]: 相手を名指す正規化した文字列。会話と重ねの比べに使う（§2）。規則（P8）:
  1. 英字を含む送り手（"Amazon" など）、または数字が 6 桁未満（短い番号）: 前後の空白を除いて小文字にした文字列そのもの。
  2. 他は区切り（空白・`-`・`.`・括弧）を除く。`+` で始まれば `+` と数字のまま。`00` で始まれば `+` に置き換える。`0` 1 つで始まれば、その `0` を除いて `+<国番号>` を前に付ける（国番号は Settings の地域、無ければ記録の国番号の既定 `81`）。他（国内の番号を `0` 無しで書く国、NANP の 10 桁など）は `+<国番号>` を前に付ける。
  - 例（国番号 81）: `090-1234-5678` → `+819012345678`、`+81 90 1234 5678` → `+819012345678`、`080-1234-5678` → `+818012345678`（別の相手）。
  - 限界（記録）[n3]: `+` も `0` も無く国番号から書いた番号（`819012345678`）には `+81` がもう一度付き、別の鍵になる。スマホの MAP は普通 `+` 付きか国内の形で出すので、出れば p008 で直す。

## 1. 層の流れ

```
Phone の app（WS170、userland/desktop/phone/、app_id "phone"）
   保存の唯一の持ち主: ~/Documents/Phone/（messages・contacts・sync/）
   │  libkeiland（kl_system_phone_*、KL_VERSION N は merge の時に Q1）
   │    同じ uid の compositor への system manager の拡張 kl_system_phone_v1（版 M）
compositor（userland/desktop/wayland/phone-shell.c）
   backend の表: 0 none・1 loopback・2 bluetooth（desktop の設定 phone.backend）
   1 通も disk に書かない。app_id "phone" の client へ中継し、app が無ければ通知
   │  libkeiland-backend の kl_backend_phone（新）
   │    zedBSD: libkeiland-backend-zedbsd/phone-zedbsd.c（bluetoothd の socket）
   │    Linux・FreeBSD: libkeiland-backend/unsupported/phone-unsupported.c（ENOTSUP）
bluetoothd（userland/base/bluetoothd/、_bluetooth）
   /run/bluetoothd.sock の PHONE の request と SUBSCRIBE の event
   phone.c（link・持ち主）・map.c（MAP の MCE）・phoneio.c（行）
   1 通も disk に書かない（log は件数と code だけ）
   │  RFCOMM・OBEX（MAS の client、MNS の server）・SDP・L2CAP
スマホ（Android が先、iPhone は p006 の後）
```

### 1.1 SMS の 3 つの流れ（正常系）

**(A) 同期（pull、Phone の app が主）**

1. app が §6.1 の時に `kl_system_phone_sync(MESSAGES, since, limit, "", 32, &request)`。`since`・`limit` は目印から（§6.2）。
2. compositor は app の request を backend の `kl_backend_phone_page` へ。backend は同期の接続で bluetoothd に `PHONE PAGE messages since=… limit=… cursor= count=32`。
3. bluetoothd の map.c が MAS で COUNT・LIST・GET・UNREAD を行い、1 通ごとに `PHONE MESSAGE …` の行と本文、最後に `PHONE PAGE-END cursor=… more=… count=… skipped=… capped=…`、`DONE`。
4. backend は 1 通ごとに item、最後に page の結果を compositor へ。compositor は app の object へ `item`（その request の番号付き）を順に、`page_end`、`done(request, 0)` を送る。libkeiland は item を app の item の queue に積み、`page_end` を request ごとに記録し、`done` を result の ring（`kl_system_take_result`）に入れる。
5. app は item を保存し（§6.3）、result（error 0）を取った時、`page_end` の `count` が受けた item の数と一致すれば、`more=1` なら同じ同期の続きを `cursor` で頼む。`more=0` で目印を進める（§6.2）。

**(B) 受信の live（push、bluetoothd が主）**

1. スマホが MNS に NewMessage を Put。map.c が LOCATE・GET・UNREAD を行い、SUBSCRIBE の接続へ `PHONE MESSAGE …` と本文。
2. backend の SUBSCRIBE の接続がそれを読み、compositor へ live の item（request 0）。
3. compositor は listen した app_id "phone" の phone object へ `item`（request 0）。listen した object が 1 つも無い時（app が動いていない）だけ通知を出す（§7）。**誰も保存しない**: app が閉じていれば item は捨てられ、次の同期（A）で取り込まれる（Q2 の「中継だけ」。取り込みの保証の範囲は §6.4）。
4. app は live の item を (A) と同じ規則で保存する（§6.3）。

**(C) 送信**

1. app が `State: sending` の item を保存し、`kl_system_phone_send_text(SMS, to, text, length, &request)`。
2. compositor は backend の `kl_backend_phone_send` へ。backend は操作の接続で `PHONE SEND to="…" length=N` と N byte。
3. bluetoothd は bMessage を組み立て PushMessage（outbox）。答え `PHONE SENT request=<n> handle=<h> state=pushed`、`DONE`。backend は result（id、0、n）を compositor へ。compositor は app へ `done(request, 0)`（スマホの outbox に入った）を送り、(n → app の object と request) を対応表に入れる。
4. スマホが送り、MNS に SendingSuccess（と DeliverySuccess）。bluetoothd は SUBSCRIBE へ `PHONE SENT request=<n> handle=<h> state=sent|delivered|failed`（state ごとに 1 回）。compositor は対応表から app へ `status(request, SENT|DELIVERED|FAILED)`（v1 の status）。app は item の State を書き直す。
5. 送った 1 通は後の同期（sent の folder）や live の MessageShift でも key 付きで来る。app は §6.3 の規則で自分の item と重ね、2 つ目の file を作らない。

### 1.2 request の一生 [B1]

新しい request の結果は全部 libkeiland の result の ring（今の `kl_system_take_result(system, &request, &error)`。Phone の app は既に読んでいる）で終わる。wire の `done` の event がそれを運ぶ。

| request | 来る event（この順） | 終わり |
| --- | --- | --- |
| sync | `item`×0〜count（その request の番号）→ `page_end` → `done(request, OK)`。失敗は `done(request, code)` だけ（`page_end` は来ない。来ていた item は保存してよい（key で重複を除く）。目印を進めないだけ）[m1] | `done` |
| send_text | `done(request, code)`。OK の後に `status(request, SENT)`・`DELIVERED`・`FAILED`（0〜2 個） | 結果は `done`、送りの状態は `status` |
| mark_read | `done(request, code)` | `done` |
| link_set | `done(request, code)` | `done` |
| listen・watch_link | 無し（on の後に `link` が 1 回。listen が gate に掛かった時、その `link` は why `not-phone`）[m13] | — |

- `done` の code は `KL_SYSTEM_RESULT_*`（§4 の表）。libkeiland は今の `system_view_result` と `system_view_error_of` で errno に直し、app は `kl_system_take_result` で errno を見る [MA]。

- compositor は `done`・`page_end`・`link`・`dropped` を落とさない（§4.2）。
- libkeiland の「この app の同期が 1 つ走っている（EBUSY）」は、`done`、object の喪失、180 秒の期限のどれかで解ける [B2]。期限は層ごとに段にする: backend の 1 page の見張り 120 秒 < compositor の同期の待ち 150 秒（待たせた同期は 150 秒で `done(TIMEOUT)`）< libkeiland 180 秒 [m2]。app は `done` を待ち、180 秒で来なければその同期を失敗として目印を進めない。

## 2. 保存（Phone の app、WS170 の store の変更）

誰が何を保存するか（Q2）:

| 層 | 保存する物 |
| --- | --- |
| Phone の app | 全部（1 通、連絡先、同期の目印）。`~/Documents/Phone/` |
| libkeiland | 何も（memory の queue だけ） |
| compositor | 何も（送信の対応表、memory） |
| libkeiland-backend | 何も（読みの buffer だけ） |
| bluetoothd | 1 通は何も。`.phone` の記録（持ち主・profile・enabled）と bond だけ（p003 §3） |

WS170 の store（`userland/desktop/phone/store.c`）の変更（p001 §8.6 を確定）:

- 1 通の file: `messages/<会話の key>/<file の名前>.txt`。header に `Source: bt:<スマホの address>:map:<key>`（partial は `Source` を書かず `Partial: yes`）、`Truncated: yes`（bluetoothd が 16 KB で切った時）。`Channel: sms`、`Direction: in|out`、`Date: <UNIX 秒>`（item の `time`）、`State: unread|read|sending|sent|delivered|failed|unknown`（`unknown` は新しい状態: 送れたか分からない、§8）。
- **知らない header の行を書き直しで保つ** [m]: store は file を書き直す時、読んで分からなかった header の行もそのまま書き戻す（この版の app から。古い app が別の機械で `Source` を消す危険は、`s<key>.txt` の名前でも重複を除くので残らない）。
- file の名前: `Source` のある 1 通は `s<key>.txt`。無い 1 通（自分の送信・partial）は今の `<日時>-<通し番号>.txt`。後で `Source` を足したら `s<key>.txt` に rename する（§6.3）[M3]。
- **会話の key** [M3, MD]: 番号の鍵（§0）が連絡先の番号の鍵と一致すれば連絡先の id、しなければ番号の鍵から作る folder: 数字の鍵は `n<+ を除いた数字>`、英字の鍵は `a<鍵の UTF-8 の 16 進>`（file の名前に使えない字を避ける）。連絡先を作らない（一覧には番号か送り手の名で出す）。後で連絡先が作られたら、その番号の鍵の folder を連絡先の folder として**読む**（移し替えない。表示で重ねる）。限界: 国番号の既定が違う機械（P8）で国内の書き方の同じ番号は別の鍵になる（記録）。
- 同期の目印: `sync/bt-<address>.state`（§6.2）。
- 上限: store は開く時に全部を読む（`store.c`）。開く時間は p004b の実装で測る（10000 通で 1 秒を目安、越えれば会話ごとの遅延の読みを Future Work に）。
- 限界（記録）[m]: key は方向・日時（秒）・番号・本文から作るので、同じ相手から同じ秒に同じ本文の 2 通は 1 通になる。`s<key>.txt` はスマホの address を含まないので、別のスマホ（Q1 で 1 台）の同じ 1 通も 1 つになる。

## 3. libkeiland の interface（app が使う）

既存（KL_VERSION 55、変えない）: `kl_system_phone_send`（1023 byte まで）、`kl_system_phone_call`、`kl_system_take_phone_event`（`struct kl_phone_event`、v1 の `received`・`status`）。`struct kl_phone_event` の大きさは変えない（公開の ABI）。

新しく足す（KL_VERSION N、manager の版 M。番号は merge の時に Q1）。

### 3.1 定数

```c
#define KL_SYSTEM_HAS_PHONE_SYNC	(新しい bit)	/* kl_system_phone_listen, _link, _watch_link, _link_set, _sync, _page_end, _send_text, _mark_read, kl_system_take_phone_item（KL_VERSION N） */

#define KL_PHONE_MESSAGES	0U	/* sync の what（p005 で KL_PHONE_CONTACTS 1・KL_PHONE_CALLS 2。今は EINVAL） */

#define KL_PHONE_ITEMS		10U	/* kl_phone_event の kind: item の queue に物が来た（何個でも 1 つの印） */
#define KL_PHONE_LINK_CHANGED	11U	/* kl_phone_event の kind: link の状態が変わった */
#define KL_PHONE_DROPPED	12U	/* kl_phone_event の kind: live の item か phone の event を落とした（同期をやり直す） */

#define KL_PHONE_SEND_MAX	8192U	/* send_text の本文の上限（byte、NUL 無し） */
#define KL_PHONE_ITEM_TEXT_MAX	16384U	/* item の本文の上限（bluetoothd が切る） */
#define KL_PHONE_HANDLE_MAX	32U	/* "<8>.<16>" と NUL に余裕 */
#define KL_PHONE_KEY_MAX	20U	/* 16 と NUL に余裕 */
#define KL_PHONE_CURSOR_MAX	64U
#define KL_PHONE_PEER_MAX	132U	/* 128 byte と NUL に余裕 */
#define KL_PHONE_DATETIME_MAX	24U
#define KL_PHONE_ADDRESS_MAX	18U	/* "AA:BB:CC:DD:EE:FF" と NUL */
#define KL_PHONE_WHY_MAX	32U
```

- kind の値は v1 の `KL_PHONE_RECEIVED`（1）・`KL_PHONE_STATUS`（2）と重ならない 10 番台にする [m]。送信の状態は v1 の `KL_PHONE_SENT`・`_DELIVERED`・`_FAILED` だけを使う（QUEUED は作らない: 「outbox に入った」は send_text の result 0）[B1, M13]。

### 3.2 構造体（大きさを呼び手が渡す）[M6]

```c
/* 1 通（同期の page の item、または live の item）。text は libkeiland の buffer を指し、次の kl_system_take_phone_item まで有効。 */
struct kl_phone_item {
	uint32_t request;			/* 同期の request の番号、live は 0 */
	unsigned what;				/* KL_PHONE_MESSAGES */
	char handle[KL_PHONE_HANDLE_MAX];	/* mark_read に使う（保存しない） */
	char key[KL_PHONE_KEY_MAX];		/* 16 桁の 16 進、または "-" */
	unsigned folder;			/* 0 inbox、1 sent */
	unsigned direction;			/* 0 in、1 out */
	int64_t time;				/* UNIX 秒 */
	unsigned zone;				/* 0 phone、1 mse、2 local、3 received（時刻の出どころ） */
	char datetime[KL_PHONE_DATETIME_MAX];	/* スマホの文字列のまま */
	char peer[KL_PHONE_PEER_MAX];		/* 相手の番号 */
	char name[KL_PHONE_PEER_MAX];		/* 相手の名前（スマホの連絡先の名前、無ければ空） */
	unsigned read;				/* 1 既読 */
	unsigned partial;			/* 1: key が無い（日時が分からない） */
	unsigned truncated;			/* 1: 本文を 16 KB で切った */
	const char *text;			/* UTF-8、NUL で終わる */
	size_t length;				/* text の byte 数 */
};

/* phone link の状態。 */
struct kl_phone_link {
	unsigned backend;			/* 0 none、1 loopback、2 bluetooth */
	unsigned linked;			/* 1: スマホの link が ready */
	unsigned messages;			/* 0 off、1 connecting、2 ready、3 failed */
	unsigned can_send;			/* 1: 送信できる */
	unsigned notify;			/* 1: 受信の通知が来る（0 なら同期だけで受ける、§6.1） */
	unsigned owner;				/* 1: この user がスマホの持ち主 */
	unsigned enabled;			/* 1: 記録の「スマホとして使う」が on（Settings の switch）[ME] */
	unsigned profiles;			/* bit 0 messages、1 contacts、2 calls（Settings の switch） */
	unsigned present;			/* 1: 持ち主が seat に居る（bluetoothd が link を持とうとする） */
	char address[KL_PHONE_ADDRESS_MAX];	/* スマホの address（同期の目印の名前）、無ければ空 */
	char why[KL_PHONE_WHY_MAX];		/* §3.5 の why の語 */
};
```

- 関数は `size_t size`（呼び手の `sizeof`）を取り、libkeiland は知っている大きさまでを書き、残りを 0 にする。後の版（p005・p006）は struct の最後に欄を足し、古い app は短い size を渡す。size が最初の版の大きさより小さければ EINVAL [m12]。`kl_system_phone_page_end` は引数の並びで伸ばさない（p005 で欄が要れば新しい関数を足す、記録）。

### 3.3 関数

| 関数 | 意味 | 返り値 |
| --- | --- | --- |
| `int kl_system_phone_listen(struct kl_system *system, unsigned on)` | この app が新しい event（live の item・link・dropped）を受けるか。on で link の状態が 1 回来る（gate に掛かれば why `not-phone`） | 0、ENOTSUP（`KL_SYSTEM_HAS_PHONE_SYNC` 無し） |
| `int kl_system_phone_link(const struct kl_system *system, struct kl_phone_link *link, size_t size)` | 最後に聞いた link の状態 | 0、ENOTSUP、ENOENT（まだ聞いていない） |
| `int kl_system_phone_sync(struct kl_system *system, unsigned what, int64_t since, unsigned limit, const char *cursor, unsigned count, uint32_t *request)` | 1 page を頼む（count 1〜32、limit 0〜500（0 は上限なし）、cursor は空か前の `page_end` の物。1 回の同期の全部の page で since と limit は同じ）[m] | 0、ENOTSUP、EINVAL、EBUSY（この app の同期が 1 つ走っている） |
| `int kl_system_take_phone_item(struct kl_system *system, struct kl_phone_item *item, size_t size)` | 届いた item を 1 つ取る。同期の item と live の item は届いた順の 1 本の queue | 1 取った、0 無い |
| `int kl_system_phone_page_end(const struct kl_system *system, uint32_t request, char *cursor, size_t cursor_size, unsigned *more, unsigned *count, unsigned *skipped, unsigned *capped)` | sync の request の page の終わり（`done` の前に届く。最後の 8 個の request の分を持つ） | 0、ENOENT |
| `int kl_system_phone_send_text(struct kl_system *system, unsigned channel, const char *to, const char *text, size_t length, uint32_t *request)` | 送信。channel は KL_PHONE_SMS だけ（他は ENOTSUP）。to は区切り（`-`・空白・括弧・`.`）を libkeiland が除いた後に `[0-9+*#]` の 1〜32 文字。text は 1〜8192 byte の UTF-8 で NUL を含まない [m] | 0、ENOTSUP、EINVAL |
| `int kl_system_phone_mark_read(struct kl_system *system, const char *handle, uint32_t *request)` | スマホの側を既読に（§6.5） | 0、ENOTSUP、EINVAL |
| `int kl_system_phone_watch_link(struct kl_system *system, unsigned on)` | link の状態（`KL_PHONE_LINK_CHANGED`）だけを受ける（Settings 用、app_id の gate 無し）[ME] | 0、ENOTSUP |
| `int kl_system_phone_link_set(struct kl_system *system, const char *address, unsigned on, unsigned profiles, uint32_t *request)` | 「スマホとして使う」と profile の switch（bluetoothd の `PHONE LINK`、持ち主の確かめは bluetoothd、app_id の gate 無し）[ME] | 0、ENOTSUP、EINVAL |

- 結果: `kl_system_take_result` の (request, error)。error は §3.5。app_id の gate（§4.1）に掛かった request は `done(request, REFUSED)`（EACCES）、listen は `link` の why `not-phone` で答える [m13]。
- libkeiland は item を queue に積むたびに `KL_SYSTEM_CHANGED_PHONE` を立てる [MC]。
- result の ring（32）が満ちると一番古い result を黙って捨てる（今の作り）。同期の `done` が落ちた app は 180 秒の期限で失敗とする（記録）[n6]。
- `kl_system_take_phone_event` には v1 の `KL_PHONE_STATUS`（送信の状態）と、新しい `KL_PHONE_ITEMS`・`_LINK_CHANGED`・`_DROPPED` が来る。`KL_PHONE_ITEMS` は item の queue が空から物ありになった時の 1 つの印で、item ごとには積まない [B2]。**印は目安**: app は `KL_SYSTEM_CHANGED_PHONE` のたびと `KL_PHONE_DROPPED` を受けた時に、`kl_system_take_phone_item` を 0 が返るまで呼ぶ（印が ring から落ちても queue が止まらない）[MC]。
- libkeiland の phone の event の ring（16）が満ちて古い物を捨てた時は、次の take で `KL_PHONE_DROPPED` を先に返す（`KL_NOTIFY_LOST` と同じ作り）[B2]。item の queue（最大 64 個、本文は item ごとに malloc で 16 KB まで、take で前の物を free）が満ちて捨てた時も `KL_PHONE_DROPPED`。
- app は `KL_PHONE_ITEMS` の後に item を全部取ってから、result を見る（同期の request の result はその item の後に来る）。
- bluetooth の backend の時、v1 の `received` は**どの app にも送らない**（listen した Phone の app は item で受ける）[M1, m6]。loopback の時は今どおり（試験の偽の data なので gate も掛けない）[n5]。

### 3.4 app の側の規則（Phone の app）

- 起動の時に `kl_system_phone_listen(1)`。同期は §6.1。
- `can_send` が 0 の時は送信の button を灰色に（「スマホが送信を受け付けません」。iPhone の MAP、Android の一部）。
- app の通知 [MF]: `kl_app_notify` は **live の item（request 0）で、dir=in・read=0 で、§6.3 で新しい file を作った物**だけ（同期の item・重ねた物・重複には出さない）。初回の同期や起動の時の同期で通知を出さない。

### 3.5 errno と why の語（app が見る物）[m]

result の errno:

| errno | 意味（bluetoothd の語、または作る層） |
| --- | --- |
| ENODEV | backend が無い（phone.backend 0、compositor） |
| ENOTSUP | 機能が無い（Linux・FreeBSD の backend、古い compositor、bluetoothd の `no-send`、SMS 以外の channel） |
| ENOTCONN | スマホの MAP が ready でない（`not-ready`）、bluetoothd が居ない（backend） |
| EACCES | 持ち主でない・app_id が違う（`permission`、`not-phone`、compositor） |
| EINVAL | 番号・本文・引数の誤り（`number`・`argument`・`length`） |
| EBUSY | 混んでいる（`busy`、backend の queue） |
| ESTALE | cursor か handle が古い MAP の session の物（`stale-cursor`・`stale`） |
| ECONNRESET | 途中で link か MAP か bluetoothd との接続が切れた（`lost`、backend） |
| ETIMEDOUT | 読むのが遅すぎた・スマホが答えない・backend の見張り（`slow`・`timeout`、backend） |
| EMSGSIZE | 1 通の listing が大きすぎる（`too-large`） |
| ENOENT | スマホに無い（`not-found`） |
| ENOBUFS | compositor が item を app に送れなかった（client の出力が 1 MB を越えた） |
| EIO | スマホが断った（`refused`・`unavailable`）、他の失敗 |

errno と送信の扱い（§8）[m10]: 送信の `done` が ECONNRESET・ETIMEDOUT・ENOBUFS なら `unknown`（送れたか分からない）。他の errno（ENODEV・ENOTSUP・ENOTCONN・EACCES・EINVAL・EBUSY・ESTALE・EMSGSIZE・ENOENT・EIO）は確かな拒否で `failed`。

link の `why` の語（作る層）: bluetoothd の STATE の why（`absent`・`peer-closed`・`key-missing`・`key-size`・`security`・`busy-links`・`permission`・`no-mas`・`sdp`・`rfcomm`・`obex`・`closed`・`timeout`・`refused`）、backend の `not-owner`（SUBSCRIBE が permission）・`no-record`（SHOW に記録が無い）・`unreachable`（bluetoothd が居ない）、compositor の `unsupported`（ENOTSUP の backend）・`no-backend`。

## 4. compositor の protocol（kl_system_phone_v1 の拡張）

manager の版 M（今 26 の次、merge の時に Q1）。v1 の request 0〜2・event 0〜2 は今のまま。新しい event は object の版が M 以上の時だけ送る [M13]。

| 向き | opcode | 名前 | 引数 |
| --- | --- | --- | --- |
| request | 3 | listen | uint on |
| request | 4 | sync | uint request, uint what, int since_high, uint since_low, uint limit, string cursor, uint count |
| request | 5 | send_text | uint request, uint channel, string to, array text |
| request | 6 | mark_read | uint request, string handle |
| request | 7 | link_set | uint request, string address, uint on, uint profiles |
| request | 8 | watch_link | uint on |
| event | 3 | item | uint request, uint what, string handle, string key, uint folder, uint direction, int time_high, uint time_low, uint zone, string datetime, string peer, string name, uint flags（bit 0 read、1 partial、2 truncated）, array text |
| event | 4 | page_end | uint request, string cursor, uint more, uint count, uint skipped, uint capped |
| event | 5 | link | uint backend, uint linked, uint messages, uint can_send, uint notify, uint owner, uint enabled, uint profiles, uint present, string address, string why |
| event | 6 | dropped | — |
| event | 7 | done | uint request, uint code（`KL_SYSTEM_RESULT_*`） |

- `done` の code [MA]: 今の `KL_SYSTEM_RESULT_*` に足す: `LOST` 12（ECONNRESET）、`TIMEOUT` 13（ETIMEDOUT）、`TOO_LARGE` 14（EMSGSIZE）、`NO_ROOM` 15（ENOBUFS）、`NOT_CONNECTED` 16（ENOTCONN）。スマホに無い（`not-found`、ENOENT）は今の `NO_KEY` 8 を使う [n1]。`system_view_error_of` に対応を足す。既存の対応: OK 0、REFUSED（EACCES）、UNSUPPORTED（ENOTSUP）、BUSY（EBUSY）、INVALID（EINVAL）、UNAVAILABLE（ENODEV）、STALE（ESTALE）、FAILED（EIO）。wire に OS の errno の数を出さない。libkeiland は `done` を result の ring に入れる（`system_view_result`、app は `kl_system_take_result`）[B1]。
- v1 の `send`（1023 byte）は backend 2 でも受ける: channel が SMS でなければ `result(UNSUPPORTED)`、app_id の gate（§4.1）に掛かれば `result(REFUSED)`、他は `result(OK)` の後に `send_text` と同じ道で送り、v1 の `status` を返す（`done` は送らない）[M13, m6]。
- 1 つの item の wire は最大 約 17 KB（本文 16 KB）で、wire の上限 65532 byte に収まる。

### 4.1 権限 [M1, P7]

- phone object は compositor と同じ uid の client だけ（今のまま）。
- `listen`・`sync`・`send_text`・`mark_read`・v1 の `send` は、client の window の app_id が "phone" の時だけ（Mail の "mailer" と同じ強さ、`mail-shell.c`）。他は `done(request, REFUSED)`、listen は `link` の why `not-phone` で答える。gate は request の時と、live の item・`received` を送る時の両方で見る（window の app_id が後で付いた app は listen をやり直す）[m13]。`watch_link`・`link_set` は gate しない（Settings が使う。持ち主の確かめは bluetoothd）[ME]。§9 の P7 の推しで、ユーザーの判断を待つ。
- compositor は bluetoothd の持ち主の判定（§5.4）に従う: 持ち主でない session の compositor では link の `owner` が 0、sync・send は EACCES。

### 4.2 落とさない event [B2]

- compositor の client の出力が 1 MB を越えると `kwl_emit` は event を ENOBUFS で捨てる（`wire.c`）。
- `done`・`page_end`・`link`・`dropped` は捨てない: 送れなかった物を object ごとの「借り」（届く順の列。最後の `link` と `dropped` の印、`page_end` と `done` を 8 組まで。溢れたら一番古い request の `page_end` を消し、その `done` を `NO_ROOM` に置き換えて残す）に置き、compositor の tick で出力が減ってから順に送り直す。
- **借りがある間は**、その object への保証の event（`done`・`page_end`・`link`・`dropped`）を全部借りの後ろに順に積み（後の小さい `done` が前の `page_end` を追い越さない）、item は送らずに捨てる（同期の item なら、その request の `done` を `NO_ROOM` に、live なら `dropped` を借りに）[MB]。
- 同期の item を 1 つでも捨てたら、その request の `done` を `NO_ROOM` にする。live の item を捨てたら `dropped` を借りに置く。
- p001 §8.4（S7）の live の event の通し番号は作らない: compositor が `dropped` を確実に届け、libkeiland が自分の ring と queue の溢れを `KL_PHONE_DROPPED` にするので要らない（§10）。

### 4.3 compositor の中（phone-shell.c）

- backend の表に `bluetooth`（値 2、`KL_SYSTEM_PHONE_BACKEND_BLUETOOTH`）。表の形を `struct phone_backend { name; open; close; update; send; call; sync; mark_read; }` に広げる。loopback は sync に固定の偽の page（2 通: 受信 1・送信 1、key 付き、`more=0`）を返し、link は messages=ready・can_send=1・notify=1（T1 の AAT 用）[m]。
- **backend の開け閉め** [MG]: compositor は起動の時に `kl_backend_phone_open()` を**いつも**開く（`phone.backend` に関わらない。greeter の compositor は開かない、bluetooth-shell.c と同じ。Linux・FreeBSD は全部 ENOTSUP の物）。`watch_link`・`link_set` と link の状態は `phone.backend` に関わらずこの backend を使う（Settings は switch が off でも記録を読み書きできる）。`phone.backend` が 2 の時だけ、SMS の中継（sync・send_text・mark_read・live の item・v1 の send）をこの backend で行う。`phone.backend` が 2 でなくなった時、走っている SMS の request は `done(UNAVAILABLE)`（ENODEV）にし、`link` を送る（backend は開いたまま）[M11]。`settings-keys.c` の `phone.backend` の範囲を 0〜2 に。
- **Settings の「スマホとして使う」の順** [MG]: on は (1) 記録が無ければ `PAIR … phone=1`（Bluetooth の拡張）、(2) `link_set on`（`done` を待つ）、(3) `phone.backend = 2`。off は (1) `link_set off`（`done` を待つ）、(2) `phone.backend = 0`。途中の失敗はそこで止め、前の段を戻さない（記録の enabled は bluetoothd が正、Settings は `link` の event で今の状態を表示し直す）。
- main loop の毎回 `kl_backend_phone_update()` で読んだ物を配る。**同じ update の中では result を SENT より先に処理**し、対応表に無い n の SENT は 10 秒保留して照らす（bluetoothd の map.c の保留と同じ）。backend が繋ぎ直した（state の reachable が 0→1）時は対応表を空にする [M4]。
- 同期: 1 つの app の同期は 1 本（EBUSY）。backend の同期の接続は 1 本なので、全体で同時に 1 つの同期を流し、他の object の同期は compositor の中で待たせる（app_id の gate で実際は Phone の app だけ）。待たせた同期は 150 秒で `done(TIMEOUT)` [M7, m2]。
- `link` の event は listen か watch_link をした object へ、backend の state が変わった時と `phone.backend` が変わった時に送る。`link_set` は backend の `kl_backend_phone_link_set` へ、結果は `done`。compositor の Bluetooth の `PAIR … phone=1`・`FORGET` の `DONE` の後に `kl_backend_phone_refresh()` を呼ぶ（§5.3、[n4]）。
- 版 M 以上の object への新しい request（3〜8）の結果は `done` だけ（v1 の `result` は v1 の request（send・call）だけ）[n2]。v1 の `status` と `result` は §4.2 の落とさない event に入れない（`status` が落ちても §6.3 の 1 時間の規則で決まる）。
- 送信の対応表: `bluetoothd の request の番号 n → (client, object, app の request)`、32 個（古い物から捨てる）。client が去ったら対応を消す（後の `PHONE SENT` は捨てる。app は次の同期で知る、§6.3）。
- 通知（§7）。
- log は件数・長さ・errno だけ（番号・名前・本文を出さない）。

## 5. bluetoothd の socket の PHONE の文法（正）

p003 §9 を正にし、i07 の実装で次を確定する。行は改行で終わる。値は `key=value`（value は空白の無い語か `"…"`、`"` と `\` は `\` で、0x20 未満と 0x7F は `\xHH`）。名前と番号は escape の前に 128 byte で切る。daemon の 1 行は改行を含め 2047 byte 以下。長さ付きの値は行の最後の `length=N` の後の N byte（改行を付けない）。

### 5.1 request（1 つの接続で 1 度に 1 つ。答えは `DONE` で終わり、失敗は `ERROR <語>` の後に `DONE`。答えを待つ間に書かれた行は捨てられる）

| request | 誰が | 答え |
| --- | --- | --- |
| `PHONE SHOW` | 持ち主・root には全部、D8 の人（seat の人と wheel）には address と enabled だけ、他には行無し [m] | `PHONE address=… owner=<uid>|invalid mine=0|1 enabled=0|1 profiles=m,c,h present=0|1 link=none|paging|securing|ready|closing messages=off|connecting|ready|failed send=0|1 notify=0|1[ why=…]`、`DONE`。記録が無ければ行無しで `DONE` |
| `PHONE LINK ADDRESS on|off [profiles=m,c,h]` | 持ち主・root | `DONE` |
| `PHONE SUBSCRIBE` | 持ち主・root、同時に 2 本まで（記録が無い・持ち主でない: `ERROR permission`、3 本目: `ERROR busy`） | `DONE` の直後に今の `PHONE STATE …` を 1 行、以後 event の行だけ（§5.2）。この接続の request の行は読み捨てる |
| `PHONE PAGE messages since=<UNIX 秒> limit=<0..500> cursor=<cursor か空> count=<1..32>` | 持ち主・root | `PHONE MESSAGE …`＋本文 が 0〜count 個、`PHONE PAGE-END cursor=<次> more=0|1 count=<出した数> skipped=<飛ばした数> capped=0|1`、`DONE` |
| `PHONE READ handle=<handle>` | 持ち主・root | `DONE` |
| `PHONE SEND to="<番号>" length=<1..8192>` ＋ N byte | 持ち主・root | `PHONE SENT request=<n> handle=<handle>|- state=pushed`、`DONE` |
| `PHONE DROP ADDRESS` | root（試験の道具） | `DONE` |
| `PAIR ADDRESS bredr phone=1` | seat の人（p003 §3.2） | 今の PAIR と同じ |
| `FORGET ADDRESS bredr` | 持ち主・root（`.phone` が有効な時） | 今と同じ |

ERROR の語: `not-ready`・`no-send`・`permission`・`owned`・`phone-seat`・`not-phone`・`argument`・`number`・`length`・`busy`・`stale-cursor`・`stale`・`lost`・`slow`・`too-large`・`not-found`・`unavailable`・`refused`・`timeout`。

- **PAGE の `limit`**（p003 からの変更 [M2]）: folder ごとの上限。0 は上限なし（folder を最後まで）。省くと 500（Q4 の最初の同期の値）。`capped=1` は、この同期のどこかの folder で上限に達して止めた事を表す（`more=0` の page で 1 になる。cursor に「この同期で止めた」を持たせる）。i07 で `map.c` の `BTD_MAP_FOLDER_LIMIT` を page ごとの値にする。
- PAGE の cursor は `<session 8 桁の 16 進>.<since の 16 進>.<folder>.<offset>.<capped>`（中身は bluetoothd だけが読む。i07 で実装、c56043c2b の後の d1a47470d）。1 回の同期の全部の page で since は同じでなければならない（違えば `stale-cursor`）。limit は page ごとの request の値を使い、cursor は持たない（app は同じ値を渡す）[m5]。
- `capped` は、folder の大きさ（COUNT の ListingSize）が limit より大きい時に 1。大きさが分からず、ちょうど limit 通の folder では 1 になる（記録）。500 通が「新しい 500 通」になるのは、スマホが新しい順に並べる時だけ（Android は新しい順の見込み、iPhone は未確認、p008）[m5]。
- PAGE の途中の失敗は item を出した後でも `ERROR <語>`、`DONE`（`PAGE-END` は来ない）。
- SEND の `length=0` と文法の誤りは `ERROR length` の後に接続を閉じる（p003 §4.3）。backend は送る前に §3.3 の検査をし、そういう行を bluetoothd に出さない [m]。
- **送信の request の番号** [M4]: map.c の `next_request` は daemon の起動時の乱数から始める（session と同じく `btd_random`）。i07 で直す。

### 5.2 event（SUBSCRIBE の接続へ）

| 行 | 意味 |
| --- | --- |
| `PHONE STATE …`（SHOW の行と同じ中身） | 状態が変わった時と SUBSCRIBE の直後 |
| `PHONE MESSAGE handle=… key=…|- folder=inbox|sent dir=in|out time=<UNIX 秒> zone=phone|mse|local|received datetime="…" peer="…" name="…" read=0|1 partial=0|1 truncated=0|1 length=<n>` ＋ n byte | live の受信（PAGE の答えの item も同じ形） |
| `PHONE SENT request=<n> handle=<handle> state=sent|delivered|failed` | 自分の送信の状態（state ごとに 1 回） |
| `PHONE MESSAGE-GONE handle=<handle>` | スマホで消された（p004 では backend が捨てる） |
| `PHONE DROPPED` | bluetoothd の queue が満ちて live の 1 通を落とした、または SUBSCRIBE の出力の queue が 192 KB を越えて event を落とした |

### 5.3 libkeiland-backend（`kl_backend_phone`、新）[M5, M7]

```c
struct kl_backend_phone;

#define KL_BACKEND_PHONE_CHANGED_STATE		1U	/* state が変わった */
#define KL_BACKEND_PHONE_CHANGED_ITEM		2U	/* item が来た */
#define KL_BACKEND_PHONE_CHANGED_RESULT		4U	/* request の結果が来た */
#define KL_BACKEND_PHONE_CHANGED_SENT		8U	/* live の送信の状態が来た */
#define KL_BACKEND_PHONE_CHANGED_DROPPED	16U	/* bluetoothd の PHONE DROPPED */

#define KL_BACKEND_PHONE_SENT		1U	/* 送信の状態（keiland.h の KL_PHONE_SENT・_DELIVERED・_FAILED と同じ値） */
#define KL_BACKEND_PHONE_DELIVERED	2U
#define KL_BACKEND_PHONE_FAILED		3U

struct kl_backend_phone_state {
	unsigned reachable;			/* bluetoothd に繋がっている */
	unsigned subscribed;			/* SUBSCRIBE できた（持ち主） */
	unsigned have_record;			/* SHOW に記録がある */
	unsigned linked;			/* link=ready */
	unsigned messages;			/* 0 off、1 connecting、2 ready、3 failed */
	unsigned can_send;
	unsigned notify;
	unsigned present;
	unsigned enabled;
	unsigned profiles;			/* bit 0 messages、1 contacts、2 calls */
	char address[KL_BACKEND_BT_ADDRESS_MAX];
	char why[KL_BACKEND_BT_REASON_MAX];
};

#define KL_BACKEND_PHONE_HANDLE_MAX	32U
#define KL_BACKEND_PHONE_KEY_MAX	20U
#define KL_BACKEND_PHONE_DATETIME_MAX	24U
#define KL_BACKEND_PHONE_PEER_MAX	132U
#define KL_BACKEND_PHONE_CURSOR_MAX	64U

struct kl_backend_phone_item {			/* text は backend の buffer、次の take まで有効 */
	uint32_t id;				/* page の id（0 にならない）、live は 0 */
	char handle[KL_BACKEND_PHONE_HANDLE_MAX];
	char key[KL_BACKEND_PHONE_KEY_MAX];
	unsigned folder;
	unsigned direction;
	int64_t time;
	unsigned zone;
	char datetime[KL_BACKEND_PHONE_DATETIME_MAX];
	char peer[KL_BACKEND_PHONE_PEER_MAX];
	char name[KL_BACKEND_PHONE_PEER_MAX];
	unsigned read;
	unsigned partial;
	unsigned truncated;
	const char *text;
	size_t length;
};

struct kl_backend_phone_result {
	uint32_t id;
	int error;				/* §3.5 の errno */
	char cursor[KL_BACKEND_PHONE_CURSOR_MAX];	/* PAGE の結果 */
	unsigned more;
	unsigned count;
	unsigned skipped;
	unsigned capped;
	uint32_t sent_request;			/* SEND の結果の n */
};

struct kl_backend_phone *kl_backend_phone_open(void);	/* NULL は memory 無しだけ。Linux・FreeBSD は全部 ENOTSUP の物 */
void kl_backend_phone_close(struct kl_backend_phone *phone);	/* 走っている request は捨てる（compositor が ENODEV にする） */
int kl_backend_phone_update(struct kl_backend_phone *phone, unsigned *changed);	/* 待たずに読み書きする。0 か EINVAL */
void kl_backend_phone_get_state(const struct kl_backend_phone *phone, struct kl_backend_phone_state *state);
int kl_backend_phone_page(struct kl_backend_phone *phone, unsigned what, int64_t since, unsigned limit, const char *cursor, unsigned count, uint32_t *id);
int kl_backend_phone_read(struct kl_backend_phone *phone, const char *handle, uint32_t *id);
int kl_backend_phone_send(struct kl_backend_phone *phone, const char *to, const uint8_t *text, size_t length, uint32_t *id);
int kl_backend_phone_link_set(struct kl_backend_phone *phone, const char *address, unsigned on, unsigned profiles, uint32_t *id);
int kl_backend_phone_take_item(struct kl_backend_phone *phone, struct kl_backend_phone_item *item);	/* 1 か 0 */
int kl_backend_phone_take_result(struct kl_backend_phone *phone, struct kl_backend_phone_result *result);	/* 1 か 0 */
int kl_backend_phone_take_sent(struct kl_backend_phone *phone, uint32_t *sent_request, unsigned *state);	/* 1 か 0 */
void kl_backend_phone_refresh(struct kl_backend_phone *phone);	/* SHOW と SUBSCRIBE をすぐやり直す（PAIR phone=1・FORGET の後）[n4] */
```

- 返り値: page・read・send・link_set は 0、ENOTCONN（bluetoothd が居ない）、ENOTSUP、EINVAL（§3.3 の検査）、EBUSY（その接続の queue が満ち）。
- **接続は 3 本**: SUBSCRIBE、同期（PAGE だけ、queue 4）、操作（SEND・READ・LINK・SHOW、queue 32）。1 本の接続は 1 度に 1 つの request（bluetoothd が待ちの間の行を捨てるので、backend は答えの `DONE` まで次を書かない）。SEND は同期の page の後ろに並ばない [M7]。
- 状態: SUBSCRIBE の `PHONE STATE` が正。SUBSCRIBE が `ERROR permission`（持ち主でない・記録が無い）の時は、操作の接続で `PHONE SHOW` を 30 秒ごとに読み（`mine=1` になったらすぐ SUBSCRIBE をやり直す）、`ERROR busy` の時は 10 秒後にやり直す [m]。自分の `link_set` と、compositor の Bluetooth の `PAIR … phone=1`・`FORGET` の `DONE` の直後にも SHOW と SUBSCRIBE をやり直す（30 秒待たない）[m14]。
- 読みの buffer: 行は 2048 byte、本文は `length` の分を malloc（16 KB まで、越える `length` は接続を閉じて繋ぎ直す: bluetoothd の誤り）。SEND の本文は non-blocking の書きの buffer（8 KB と行）で、書ける時に続ける [m]。
- 見張り: request ごとに 120 秒（PAGE は 1 page ごと）で答えが無ければ ETIMEDOUT にし、その接続を閉じて繋ぎ直す [m]。
- 切断（bluetoothd の再起動）: 走っている request は全部 ECONNRESET、reachable 0 → 1 秒ごとに繋ぎ直して SUBSCRIBE。
- **Settings の道**: `PHONE LINK`・SHOW は `kl_backend_phone` の操作の接続（`link_set`、state）。`PAIR … phone=1` は今の `kl_backend_bluetooth` に `KL_BACKEND_BT_PAIR_PHONE`（新しい request、bluetooth-zedbsd.c が `PAIR ADDRESS bredr phone=1` を送る）を足す（WS143 の file を WS197 p004c で変える）[M5]。

### 5.4 bluetoothd の中の持ち主と権限 [B3, M7]

- PHONE の request（SHOW を除く）と SUBSCRIBE は、有効な `.phone` の記録の uid（持ち主）と root だけ（p003 §3、§9.2）。持ち主の uid の process は compositor に限らない（SSH の process も同じ uid なら受けられる。§9 の P7 は compositor の側の gate）。
- **event を送る時にもう一度確かめる**: SUBSCRIBE の接続へ `PHONE MESSAGE`・`SENT`・`STATE`・`DROPPED` を出す時、接続の uid が今の有効な記録の uid か root でなければ送らない。記録が変わった時（load・link_set・forget・handoff）、uid が持ち主でなくなった SUBSCRIBE の接続は閉じる。PAGE・READ・SEND の答えも、答えを書く時に同じ確かめをする（`ERROR permission`）。p003 i07 で実装し、host 試験（持ち主の変更の間の SUBSCRIBE）を足す。
- 持ち主が seat に居ない間は MAP を閉じている（Q14）。その間の SUBSCRIBE は受ける（STATE だけが来る）。
- **client の枠**: 今の予約（空きが 4 以下の時は root と seat の人だけ）では、seat の人でない uid が 12 本を持てる。i07 で、root でも seat の人でもない uid の接続を uid ごとに 4 本までにした（accept の時に、その uid が今持つ全部の接続を数える。compositor は Bluetooth の 4 本と phone の 3 本で 7 本、seat の人なので制限を受けない）。限界（記録）[m3]: VT を切り替えた背面の session の compositor は seat の人でないので、その間に bluetoothd が再起動すると 4 本で止まる（前面に戻れば 1 秒ごとのやり直しで直る。背面の持ち主は Q14 で MAP を使わない）。
- 持ち主の変化で、持ち主でない subscriber と、phone の request の答えを待つ client は閉じる（拒否の `ERROR permission` にしない: SEND が push 済みなら backend は ECONNRESET → app は `unknown`）[m4]。i07 で実装済み。main.c は host で build しないので、この判定を host 試験できる pure な関数に切り出して試験を足す（Q1 の指示、p003 の残り）。

## 6. 同期の規則（app が主）

### 6.1 いつ [M9]

- app の起動の時、link の `messages` が ready になった時（off・connecting・failed から ready への変化だけ。起動の時に既に ready なら起動の同期の 1 回）[m]、`KL_PHONE_DROPPED` と sync の ESTALE の後、利用者の「今すぐ同期」。
- `notify` が 0 の間（MNS が無い、live が来ない）は、app が動いている間 5 分ごと。この同期の since は「前の同期を始めた時刻 − 10 分」（24 時間の重なりは ready の時・起動の時・1 日 1 回だけ。毎回の 24 時間の GET を避ける）[m7]。
- 深い同期: 過去 7 日（`since = 今 − 7 日`、limit 0）。目印は動かさない（取りこぼしの拾い直し、§6.4）。state の file の `deep_at`（最後の深い同期の時刻）が 24 時間より古ければ、起動の時と動いている間に 1 回 [m7]。
- 1 回の同期は messages の inbox と sent を cursor でたどり、`more=0` まで。同期の最中に `dropped` が来たら、終わった後にもう 1 回だけ同期する。

### 6.2 目印 [M2]

`sync/bt-<address>.state`（text、`messages_since <UNIX 秒>` と `deep_at <UNIX 秒>` の行。p005 で contacts・calls の行を足す）。

- 初回（目印が無い）: `since = 今 − 30 日`、`limit = 500`（Q4）。
- 2 回目から: `since = messages_since`、`limit = 0`（上限なし）。
- 同期が `more=0` で終わったら `messages_since = (この同期を始めた時刻) − 24 時間`（遅れて届く SMS・時刻の推定の誤り・page の間の 1 件のずれを重ねて拾う。重複は key で除く）。途中の失敗では書き換えない。
- 初回で `capped=1`: 30 日のうち 500 通を越えた分は取らない（Q4 の決定）。目印は上のとおり進め、app は「古いメッセージの一部は取り込んでいません」を 1 回出す。
- cursor は目印に書かない（MAP の session ごとに無効）。

### 6.3 重複と重ね [M3]

app は開いた時に、全部の会話の folder の `Source` と `s<key>.txt` の名前の索引（key → file）を memory に作る。1 つの item を保存する時の手順:

1. **key のある item で索引にある**: その file の read の変化だけを反映（スマホで既読になれば `State: read`。手元で既読にした物はそのまま）して終わり。
2. **key のある item で索引に無い**: 重ねの候補（下）を探す。見つかれば、その file に `Source` を足し、`Date` をスマホの物に、`Partial` を消し、方向が out なら State を `sent` に（`delivered` はそのまま。`sending`・`unknown`・1 時間の規則で `failed` にした物も、sent の folder にあるのは送れた証拠なので `sent`）[m8]、`s<key>.txt` に rename し、索引に入れて終わり。無ければ新しく `s<key>.txt` を作る。
3. **partial の item（key `-`）**: 候補を探す。ここでは `Source` のある file も候補に入れる（key のある同じ 1 通が先に保存されていれば重複を作らない）[m9]。見つかれば何もしない（既にある）。無ければ新しく `<日時>-<通し番号>.txt`（`Partial: yes`）を作る。partial に key は付けない（後で key のある同じ 1 通が来れば手順 2 で重なる）。
- 手順 1 の既読の反映は dir=in の 1 通だけ [m9]。

**候補**: `Source` の無い file のうち、次を全部満たす物の、時刻の差の一番小さい 1 つ。1 つの file は 1 度しか重ならない（重ねたら `Source` が付き、候補から外れる）。

- 番号の鍵（§0）が同じ（folder ではなく番号の鍵で比べる）、
- 方向が同じ、
- 本文が同じ（byte ごと。改行の CRLF と LF は同じと見る）、
- 時刻の差が ±10 分（zone が `local` の item は zedBSD の時間帯で推した時刻なので、±10 分に「スマホと zedBSD の時間帯の差」が加わりうる。外れた時は重ならず 2 つの file になる。記録）[m8]。

送信の State: `sending` → `status` の SENT で `sent`、DELIVERED で `delivered`、FAILED で `failed`。`done` の error が §8 の「分からない」の物なら `unknown`（`sending` と同じに扱い、上の重ねで `sent` になる）。`sending`・`unknown` が 1 時間たっても重ならない時は、まず同期を 1 回し、それでも重ならなければ `failed`（「送れたか分かりません」）[M8, M9]。

### 6.4 取りこぼしの検出と保証の範囲 [M2]

| 何で | 誰が気づく | app の動き |
| --- | --- | --- |
| bluetoothd の queue が満ち live を落とした | bluetoothd → `PHONE DROPPED` → compositor の `dropped`（落とさない、§4.2） | 同期 |
| SUBSCRIBE の出力が溢れた | bluetoothd → `PHONE DROPPED` | 同期 |
| compositor の client の出力が 1 MB を越えた | compositor → `done(ENOBUFS)`、live は `dropped` | 同期（page はやり直し） |
| libkeiland の ring・item の queue が満ちた | libkeiland → `KL_PHONE_DROPPED` | 同期 |
| page の item の数が `count` と違う | app | 目印を進めず、同期をやり直す（続けて 3 回まで。3 回とも違えば 10 分後） |
| app が閉じていた | — | 起動の時の同期で拾う |
| page の間の受信・削除で 1 件ずれる（offset で進むため） | — | 24 時間の重なりと live と 1 日 1 回の深い同期で補う |

**保証の範囲**（P1 の帰結）: スマホが受けた時刻（MAP の datetime）が目印 − 24 時間より新しい 1 通は、同期で取り込む。それより古い datetime で後から届いた物（スマホの時計の誤り、遅い配達）は、7 日以内なら 1 日 1 回の深い同期で取り込む。それより古い物と、初回の 30 日の 500 通を越えた物は取り込まない。

### 6.5 既読（mark_read）[M10]

- app は memory に key → handle の表を持つ（最新の同期と live の item から。保存しない）。
- 利用者が会話を開いて未読を読んだ時、その会話の未読の 1 通ごとに、表に handle があれば `mark_read` を 1 つずつ（前の `done` を待ってから次）。handle が無い 1 通は、次の同期でスマホの側が `read=0` のまま来た時、その handle で出す。
- `mark_read` の ESTALE・ENOTCONN は同期を起こさない（次の同期で handle が新しくなってから出す）。

## 7. 通知（Q8）[M12]

- **app が動いている時**: Phone の app が自分で通知を出す（§3.4 の規則: live の新しい受信だけ）。compositor は出さない（二重にしない）。app が閉じている間に compositor が通知した 1 通は、起動の時の同期の item なので app は通知しない [MF]。
- **app が動いていない時**（listen した app_id "phone" の object が無い）: compositor が live の item（dir=in）で WS156 の通知（client 0）を出す: 題は相手の名前（item の `name`、無ければ番号）、本文は 1 行目、押すと Phone の app をその相手のタイムラインで開く（`/bin/phone --peer <番号>`、p004b で Phone の app に引数を足す）。
- **lock の画面**: 今の WS156 の通知は lock の画面で何も出さない。Q8 の「lock の画面では『新しいメッセージ』と相手の名前だけ（本文は出さない）」は、WS156 の通知（compositor の client 0 の通知と、app の `kl_app_notify` の両方）に「lock の画面の文」（`lock_text`）を足す変更が要る [m11]。持ち主は WS156（Q1 が割り当て）、p004c はその interface を使う側（`lock_text` = "New message from <名前>"）。WS156 の通知の click で program を起動できるか（`/bin/phone --peer`）は確かめていない（p004c の始めに WS156 の code で確かめる）。着信の 2 つの button と lock の画面の応答は p006。

## 8. 切断・再接続・suspend・失敗

| 出来事 | bluetoothd | compositor・app |
| --- | --- | --- |
| スマホが離れた（link が切れた） | MAP の queue の request は全部 `ERROR lost`、`PHONE STATE … link=none messages=off`、自分で page し直す（p003 §5） | link の event。走っていた同期は ECONNRESET（目印は進めない）。ready に戻ったら同期 |
| スマホが MAP を許可していない | `messages=failed why=permission`、600 秒ごとと `PHONE LINK on` でやり直し | 「スマホでメッセージへのアクセスを許可してください」 |
| MAS が無い | `why=no-mas`（やり直さない） | 「スマホがメッセージの共有に対応していません」 |
| suspend | 何もしない。resume で backoff を戻す（p003 §5.6）。死んだ link は supervision の timeout で切れる | 同上 |
| 持ち主の logout | MAP と ACL を切る（Q14） | 持ち主の compositor は終わる |
| bluetoothd の再起動 | — | backend が繋ぎ直す（§5.3）、走っていた request は ECONNRESET、対応表は空に |
| phone.backend が 2 でなくなった | — | 走っている request は ENODEV、link の event |
| **送信の途中で切れた** [M8] | PUSH の答えの前なら `ERROR lost`（スマホが Put を受けたかは分からない） | send の `done` が ECONNRESET・ETIMEDOUT・ENOBUFS なら app は `unknown`（失敗と見せず、§6.3 の重ねか 1 時間の規則で決まる）。`failed` にするのは §3.5 の確かな拒否だけ。利用者に「再送」を勧めない。持ち主が変わって bluetoothd が接続を閉じた時も ECONNRESET（§5.4） |

## 9. 判断の残り（推しは太字、Q1 経由でユーザーへ）

| # | 判断 | 選択肢 | 推し |
| --- | --- | --- | --- |
| P1 | app が閉じている間に届いた SMS | **保存しない（通知だけ。次の起動の同期で取り込む。Q2 の「中継だけ」）。取り込みの保証は §6.4 の範囲（目印 − 24 時間より新しい物は同期で、7 日以内の遅い物は 1 日 1 回の深い同期で）** / compositor が app の代わりに `~/Documents/Phone/` に書く（Q2 の変更） | **保存しない** |
| P2 | libkeiland の item の本文 | **libkeiland の buffer を指す（次の take まで有効、struct は小さい）** / struct に 16 KB の配列 | **buffer を指す** |
| P3 | 長い送信の関数 | **新しい `kl_system_phone_send_text`（長さ付き、8192 byte）。今の `kl_system_phone_send`（1023 byte）は残す** / 今の関数の上限を上げる | **新しい関数** |
| P4 | `phone.backend` を 2 にするのは誰か | **Settings の「スマホとして使う」を on にした時に Settings が設定。手で 0 に戻せる** / compositor が bluetoothd の記録を見て自動 | **Settings** |
| P5 | 送った 1 通の重ね | **app が「同じ番号の鍵・方向・本文・±10 分」で重ねる（§6.3）** / bluetoothd が送った 1 通の key を答えに付ける（MSE の listing を待つ、遅い） | **app が重ねる** |
| P6 | 同期の 1 page の数 | **32（bluetoothd の上限）固定** / app が選ぶ | **32 固定** |
| P7 | SMS を受けられる app | **app_id "phone" の app だけ（Mail の "mailer" と同じ強さ。同じ uid の他の app には SMS の本文を渡さない。同じ uid の process が bluetoothd の socket を直に使うのは防がない）** / 同じ uid の全部の app（今の v1 の `received` と同じ） | **"phone" だけ** |

| P8 | 番号の鍵の国番号の既定（§0） | **Settings の地域から（無ければ 81）。技術の判断として Q1 の裁量** / 利用者に選ばせる | **地域から** |

KL_VERSION と manager の版の番号は merge の時に Q1 が割り当てる（判断ではない）。

## 10. p001 §8 と第 1 版からの変更（この文書が正）

| 元 | この文書 | 理由 |
| --- | --- | --- |
| p001 `PHONE PAGE … after=` | `cursor=`、`limit=`、PAGE-END に `capped` | p003 §1.1、review-1 M2 |
| p001 event の `source=` | `handle=` と `key=` | p003 §8.3: handle は session ごと |
| p001 `PHONE GET`・`fetch(source)` | 作らない（本文は item で 16 KB まで来る） | p003 M9 |
| p001 本文 64 KB | 16 KB（受け）、8192 byte（送り） | p003 §1.1 |
| p001 `page_item`・`message` の 2 つの event | 1 つの `item`（request 0 が live） | app の保存の道を 1 つに |
| p001 suspend で profile を閉じる | 何もしない | p003 §1.1 |
| p001 `Source: bt:<address>:map:<id>` | `Source: bt:<address>:map:<key>`、file の名前は `s<key>.txt` | key が session を越える名前 |
| p001 §8.4 live の event の通し番号（S7） | 作らない。compositor が `dropped` を落とさず、libkeiland が自分の溢れを `KL_PHONE_DROPPED` にする（§4.2、§3.3） | review-1 B2 |
| p001 §8.5 banner を app が前面に無い時 | app が動いていれば app が通知、動いていなければ compositor | review-1 M12 |
| 第 1 版 `KL_PHONE_QUEUED`・`failed` の event | 作らない。結果は `done` → result の ring | review-1 B1 |
| 第 1 版 1 本の request の接続 | 同期と操作の 2 本（と SUBSCRIBE） | review-1 M7 |
| 第 1 版 v1 の `received` を古い app に | bluetooth の backend では送らない | review-1 M1 |
| 第 2 版 `done(request, int error)` | `done(request, uint code)`（`KL_SYSTEM_RESULT_*` に 6 個を足す） | review-2 MA |
| 第 2 版 番号の鍵 = 末尾 9 桁 | 正規化した E.164（英字・短い番号は文字列） | review-2 MD（090 と 080 が衝突） |
| 第 2 版 Settings の道は p004c で | `watch_link`・`link_set` を今の版で（gate 無し）、`kl_phone_link` に enabled・profiles・present | review-2 ME |

## 11. 作る物の一覧と持ち場 [m]

p004 を 3 つの Phase に分ける案（Q1 に ws.md の表の更新を頼む）: **p004a** backend・compositor・libkeiland（WS197）、**p004b** Phone の app の store と同期（WS170 の code を WS197 で）、**p004c** Settings・通知（WS156 の lock の変更は依頼）。

### 11.1 bluetoothd（WS197 p003）

| 物 | どこで |
| --- | --- |
| `.phone` の記録・PAIR phone=1・LINK・SHOW・FORGET・HID の上限 | p003 i01（済み） |
| 出力の queue・client の枠 16・長さ付きの入力 | p003 i02（済み） |
| phone link の一生 | p003 i03（済み、T1-524 の直しは ws197 branch の e3ab488d4） |
| listing と event の XML・datetime | p003 i04（済み） |
| bMessage | p003 i05（済み） |
| OBEX の response の hook と timeout、map.c、phone の配線（opened の ours、MNS の SDP record、MAP の ready で backoff を戻す、SHOW の present） | p003 i06（済み、ws197 branch の c56043c2b） |
| main の PHONE の request（PAGE の `limit`・`capped` を含む）・SUBSCRIBE（直後の STATE）・DROPPED、token の cancel、answer・emit・room の hook（client を同期で閉じない）、**§5.4 の送る時の持ち主の確かめと記録の変化での SUBSCRIBE と待つ client の閉じ**、**uid ごとの接続の上限 4**、**`next_request` を乱数から**、SHOW の MAP の部分 | p003 i07（済み、ws197 branch の d1a47470d） |
| 持ち主の変化・SUBSCRIBE の閉じ・uid の上限の判定を pure な関数に切り出して host 試験（Q1 2026-10-10） | p003 の残り（小さい単位） |

### 11.2 libkeiland-backend（WS197 p004a）

- `keiland-backend.h` に §5.3 の API。
- `libkeiland-backend-zedbsd/phone-zedbsd.c`（新）と host の試験（偽の bluetoothd の socket pair、`plan/ws131/tests/host-session.c` と同じ作り）。
- `libkeiland-backend/unsupported/phone-unsupported.c`（Linux・FreeBSD）。`plan/tools/keiland-os-boundary` の表に足す。
- `bluetooth-zedbsd.c` に `KL_BACKEND_BT_PAIR_PHONE`（WS143 の file、p004c）。

### 11.3 compositor（WS197 p004a、WS170 の code を変える）

- `phone-shell.c`: backend の表、`bluetooth`、§4 の request と event、§4.2 の借り、送信の対応表、同期の待ち、app_id の gate、通知（p004c）。
- `kl-system-protocol.h`: §4 の opcode、manager の版、`KL_SYSTEM_PHONE_BACKEND_BLUETOOTH` 2。
- `settings-keys.c`: `phone.backend` の範囲 0〜2。
- 試験: compositor の host 試験（偽の backend）と T1 の AAT（loopback の偽の page）。

### 11.4 libkeiland（WS197 p004a、WS170 の API の拡張）

- `keiland.h`: §3 の定数・struct・関数、KL_VERSION N の行。`system.c`・`system-protocol.c`・`system-view.c`（item の queue、page_end の記録、ring の溢れの印、同期の EBUSY と 180 秒、`system_view_error_of` の新しい code）・`system-private.h`・`exports.map`。
- 試験: `plan/ws089/tests/host-kl-system.c` の作りで item・page_end・done・dropped・ring の溢れ。

### 11.5 Phone の app（WS170 の code、WS197 p004b。WS170 の ws.md に記録）

- store: §2 の Source・Partial・Truncated・`unknown`・`s<key>.txt`・番号の鍵の会話・知らない header の保持・目印、§6.3 の索引と重ね。
- main: listen、link の表示、同期（§6.1・§6.2）、送信の `send_text`、`can_send` の灰色、§6.5 の既読、`--peer` の引数。
- 試験: store の host 試験（重複・重ね・rename・番号の鍵・目印・capped）と T1 の AAT（loopback の偽の page）。

### 11.6 Settings（WS197 p004c）

- Bluetooth の頁の「スマホとして使う」（`KL_BACKEND_BT_PAIR_PHONE` を compositor の bluetooth の拡張と `kl_system_bluetooth_*` で）、profile の switch（§3.3 の `kl_system_phone_link_set` → compositor の request 7 → `kl_backend_phone_link_set`）、状態（§3.3 の `kl_system_phone_watch_link` と `kl_phone_link` の enabled・profiles・present・messages・why）、「スマホで許可してください」、`phone.backend` を 2 に（P4）。libkeiland と compositor の部分（request 7・8、link の欄）は p004a で作る（版を 1 度で決める）[ME]。

### 11.7 規約の見直しと merge の危険 [m]

- WS197 が変える WS170 の file（phone-shell.c・store.c・main.c・libkeiland）の規約の全文の見直しは WS197 p009 で行う（WS170 p005 は WS170 自身の分）。
- p004 の code は main に随時 merge する（ユーザー「保留中のコードは随時mainに入れてOK」）。WS170 の file の衝突は merge ごとに Q1 が見る。

### 11.8 見積もり

p004 の 12 LW の内訳の案: p004a 6.5（backend 3、compositor 2、libkeiland 1.5）、p004b 3.5、p004c 1.5、試験と T1 0.5。

## 12. 試験

- host:
  - backend（偽の bluetoothd）: PAGE の item と PAGE-END（capped）、ERROR、SUBSCRIBE の STATE・MESSAGE・SENT・DROPPED、長さ付きの本文が 2 回の read に割れる、**SENT が pushed の結果より先に来る**、bluetoothd の切断と繋ぎ直し（**request の番号の衝突**）、SEND の `length=0` を出さない、見張りの 120 秒、SUBSCRIBE の permission と busy のやり直し。
  - libkeiland: item の queue と `KL_PHONE_ITEMS` の 1 つの印、page_end、done → result、**phone の ring の溢れで DROPPED**、同期の EBUSY が done・180 秒で解ける、大きさの引数。
  - compositor（phone-shell）: 送信の対応表と 10 秒の保留、同期の待ち、**`page_end`・`done` が ENOBUFS で借りになり、出力が減ってから届く**、app_id の gate、backend の開け閉め。
  - Phone の store: §6.3 の手順の全部（key の重複、partial、送った 1 通の重ね、rename、1 対 1、`failed` の後の重ねで `sent`、partial と Source の file）、番号の鍵（`090…` と `+8190…` が同じ、`090…` と `080…` が別、英字の送り手）、知らない header の保持、目印と capped と `deep_at`、1 時間の規則、**同期の item で通知しない**。
  - libkeiland（足し）: **`KL_PHONE_ITEMS` の印が ring から落ちても item が取れる**、`done` の code と errno の対応。
  - compositor（足し）: **`page_end` だけが ENOBUFS の時、後の `done` が追い越さない**、listen の gate の答え（why `not-phone`）、watch_link と link_set が gate 無し。
  - bluetoothd（p003 i07）: **持ち主の変更の間の SUBSCRIBE**（前の持ち主に event が行かない、接続が閉じる）、**SEND の答えを待つ間の持ち主の変更で接続が閉じる**、uid ごとの上限 4（main の判定を pure な関数に切り出して host 試験、Q1 の指示）、PAGE の limit と capped（bt-map-host-test で済み）。
- QEMU（T1）: 本物のスマホは無い。loopback の backend の偽の page で、Phone の app の同期と表示と送信の状態を AAT で。
- 実機（p008）: Android で 30 日の同期、受信の通知、送信の sent・delivered、離れて戻る、許可の拒否、送信の途中で離れる（`unknown`）。

## Event

- 2026-10-10: 第 1 版（P1、274cc2c20）。
- 2026-10-10: design-reviewer の review → [review-1.md](review-1.md)（blocker 3・major 13・minor 約 20。§0・§5.1〜§5.2（B3 の後）・§6.1・§10・§11.1・§11.2 の配置は GO）。
- 2026-10-10: 第 2 版（P1、e9a737e2f）。全部に答えた（各節の印）。
- 2026-10-10: design-reviewer の確認 → [review-2.md](review-2.md)（blocker 0・major 6（MA〜MF）・minor 14。§5 は GO で p003 i07 に進めてよい、p004a〜c は第 3 版の後）。
- 2026-10-10: 第 3 版（P1、2c30374b7）。MA〜MF と m1〜m14 を入れた（各節の印）。p003 i07（d1a47470d）は §5 の第 3 版の形（cursor の capped、閉じる時の扱い）で実装済み。
- 2026-10-10: design-reviewer の確認（review-3、短い確認、file にせずこの行に要約）: MA〜MF・m1〜m14 は閉じた（m2・m13 は古い文が残っていた）。新しい major 1: **MG** 「スマホとして使う」の switch が phone.backend・記録の enabled・PAIR の 3 つの意味を持ち、backend の開け閉めと順が決まっていない（Settings が backend 0 の間に記録を読めない、off の時に LINK off を送れない）。minor: n1 NOT_FOUND と NO_KEY の重なり、n2 v1 の result と done の二重、n3 `+` 無しの国番号付きの番号、n4 backend に refresh が無い、n5 loopback の gate、n6 result の ring の溢れ（記録）。判定: p004a は §4.3 に MG の段落を入れれば GO、p004b は GO、p004c は MG の Settings の順を書けば GO。
- 2026-10-10: 第 3.1 版（P1）。MG（backend はいつも開き、SMS の中継だけ phone.backend 2 で。Settings の on・off の順）、m2・m13 の古い文、n1〜n6 を入れた。review-3 の判定の条件を満たしたので p004a〜c は GO（短い差分、再 review は要らない、review-3 の判定のとおり）。

## ユーザーの決定（2026-10-10）

P1〜P7 はクリック「全部推しどおり」: P1 app が閉じている間の SMS は保存せず通知だけ（次の同期で取り込む）、P2 本文は次の取得まで libkeiland の buffer を指す、P3 新しい kl_system_phone_send_text（8192 byte）で旧い関数は残す、P4 Settings の「Use as phone」で phone.backend=2、P5 自分の送信とスマホの写しの照合は app（番号・向き・本文・±10 分）、P6 1 頁 32 件固定、P7 SMS の本文は app_id「phone」だけ。P8（技術、Q1）: 番号の正規化の既定の国番号は Settings の region、無ければ 81（推しどおり）。

## 実装の進み（ws197 branch、p004a〜c）

| 部分 | commit | 内容 | 確認 |
| --- | --- | --- | --- |
| p004a backend | 5e431704b | `keiland-backend.h` の §5.3 の API、`libkeiland-backend-zedbsd/phone-zedbsd.c`（3 本の接続、SUBSCRIBE の STATE が正、permission の 30 秒・busy の 10 秒のやり直し、見張り 120 秒、繋ぎ直し）、`libkeiland-backend/unsupported/phone-unsupported.c`（Linux・FreeBSD は ENOTSUP）、試験 `plan/ws197/tests/phone-backend-host-test.sh`（偽の bluetoothd） | host PASS |
| p004a libkeiland・compositor（2026-10-10 P1 の新しい世代が前の世代の未 commit の差分を続けた） | この commit | libkeiland: KL_VERSION 79・`KL_SYSTEM_HAS_PHONE_SYNC`・§3 の定数・struct・9 つの関数、`system-view.c` の item の queue（64、本文は item ごとに malloc、take で前の物を free、空からの 1 つ目で `KL_PHONE_ITEMS`）・page_end の 8 個・link・ring の溢れの `KL_PHONE_DROPPED`・同期の EBUSY（`done` か 180 秒で解ける）・`system_view_error_of` の 5 つの code。protocol: manager の版 27、opcode 3〜8・event 3〜7、`KL_SYSTEM_RESULT_LOST` 12〜`NOT_CONNECTED` 16、`KL_SYSTEM_PHONE_BACKEND_BLUETOOTH` 2。compositor `phone-shell.c`: backend の表に bluetooth、backend はいつも開く（greeter は開かない）、SMS の中継は phone.backend 2 の時だけ、app_id "phone" の gate（listen は why `not-phone`、watch_link・link_set は gate 無し）、§4.2 の借り（object ごと 20 個、page_end を消す時は done を NO_ROOM に）、送信の対応表と SENT の 10 秒の保留、同期は全体で 1 本・待ちは 150 秒で TIMEOUT、setting が 2 でなくなれば UNAVAILABLE、bluetoothd に繋ぎ直したら対応表を空に、v1 の send は bluetooth でも gate して送る。`settings-keys.c` の phone.backend 0〜2。**直した**: 1 回の update で page の item と答えが一緒に来た時、答えを先に処理して request を消し item を捨てていた → item を先に処理する。試験: 新 `plan/ws197/tests/phone-shell-host-test.sh`（偽の backend と capture の client、54 checks: gate、同じ update の item・page_end・done の順、同期の待ちと 150 秒、SENT の保留と 10 秒、借りの順と NO_ROOM、live、dropped、link_set、UNAVAILABLE、繋ぎ直し、v1 の gate、object の喪失、libkeiland の queue・印・溢れ・page_end・EBUSY と 180 秒・code・link） | zedBSD の wayland・libkeiland.so warning 0、keiland-linux.mk の all warning 0、phone-shell・phone-backend・ws170 の phone の host 試験 PASS、keiland-os-boundary PASS、style-check 0 |
| p004b Phone の app の保存と同期（2026-10-10 P1） | この commit | store（`store.c`・`phone.h`）: 番号の鍵 `ph_number_key`（§0、国番号の既定 81、`ph_store_set_country`）、番号の会話（`messages/n<数字>`・`a<16 進>`、連絡先の file 無し、`conversation` 1、名前は item の `Name:`）、後で作った連絡先が会話を引き継ぎ、開き直すと番号の folder を連絡先の物として読む、`ph_store_phone_message`（§6.3 の手順 1〜3: key の索引は `Source` と `s<key>.txt` の名前、既読の反映、候補（同じ向き・同じ本文（CRLF=LF、末尾の改行を除く）・±10 分・一番近い・1 対 1）の重ねで `Source`・時刻・`sent`（delivered はそのまま）・`s<key>.txt` に rename、partial の保存と後の key での重ね）、`Source`・`Partial`・`Truncated`・`Name` の header と知らない header の行の保持、状態 `unknown`、item の `serial`（run の中の名前、並べ替えで index が動くため）と `ph_store_find_serial`、同期の目印 `sync/bt-<address>.state`（`ph_store_sync_load`・`_save`）。main（`main.c`）: `kl_system_phone_listen`、link（`can_send` 0 で送信の button を灰色・notice、`why` の permission・no-mas の notice、messages が ready になったら同期）、同期（§6.1・§6.2: 初回 30 日・500、以後は目印から上限なし、notify 0 の間 5 分ごと（前の始め − 10 分）、1 日 1 回の 7 日の深い同期、page の item の数が count と違えば続けて 3 回まで・後は 10 分後、ESTALE はやり直し、失敗は目印を動かさない、成功で目印 = 始め − 24 時間、初回の capped で notice、途中の dropped でもう 1 回、180 秒の見張り）、item の保存と通知（live の新しい未読の受信だけ `kl_app_notify`）、送信は paired の時 `kl_system_phone_send_text`（SMS）、done の ECONNRESET・ETIMEDOUT・ENOBUFS は `unknown`・他は `failed`、status で sent・delivered、1 時間の規則（同期を 1 回してから `failed`）、既読（§6.5: key → handle の表、会話を開いた時と、手元で既読・スマホで未読の item が来た時に `mark_read` を 1 つずつ）、`--peer NUMBER`（その会話を開く）。試験: 新 `plan/ws197/tests/phone-store-host-test.sh`（40 checks） | zedBSD の phone warning 0、keiland-linux.mk の all warning 0、phone-store・WS170 の host-phone・host-phone-store PASS、style-check 0。**限界**: Settings に地域の設定が無いので国番号は既定の 81（P8 の「地域から」は地域の設定ができた時に `ph_store_set_country` を呼ぶ）。main の同期・通知・既読の流れは host 試験が無い（実機の UAT で確かめる）。WS170 の ws.md への記録は Q1 に依頼 |
