<!-- awesome-plan project=zedbsd record=ws197-p004 -->

# ws197-p004: SMS の層の流れと境界の interface（Phone の app・libkeiland・compositor・libkeiland-backend・bluetoothd）の詳細設計

Phase ID: `ws197-p004`
Parent: [WS197](../ws.md)
Related: [WS170](../../ws170/ws.md)（Phone の app、保存の持ち主）
Status: planning（2026-10-10 P1: 詳細設計の第 1 版。design-reviewer を通し、残りの判断（§9）を Q1 経由でユーザーに尋ねる。実装は p003 の後）
Phase disposition: normal
Queue: Q1 の投入（2026-10-10「WS197 p004 の詳細設計として、SMS 利用における Phone app・libkeiland・libkeiland-backend・bluetoothd の流れと interface を確定し、別の session で判断の違いが出ないようにする」）

由来（2026-10-10 ユーザー）:「P1はWS197に戻る前に、WS170のlibkeiland-backendにおけるMAPの実装について、bluetoothdをどう叩くのか、それから、bluetoothdに何が実装されるべきか、このあたりを明確にしておいて、別なセッションで判断の違いが生じないようにしてください。全般的に、SMS利用におけるPhone app, libkeiland, libkeiland-backend, bluetoothdの流れを明確にして、どのようなインタフェースになるか、関連WSに記載してください。」

**この文書が正**: WS197 p001 §8（第 1 案）と p003 §9（bluetoothd の socket）と食い違う所は、この文書が後の決定で、p001 §8 の該当の行を置き換える（§10 の表）。p003 の実装（保留の branch `agent/p1-ws197`）はこの文書の §5 の文法に合わせる。

前提のユーザーの決定（p001 §11、2026-10-09「全部推しどおり」）: Q1 スマホは 1 台、**Q2 持ち主だけが見る・bluetoothd は中継だけ・保存は Phone の app の `~/Documents/Phone/`**、Q3 スマホの連絡先は別の組、Q4 最初の同期は過去 30 日・folder ごとに最大 500 通、Q5 MMS は範囲外、Q8 banner と lock の画面、**Q10 Linux・FreeBSD の Keiland は作らない（「無い」の backend だけ）**、Q14 logout で切る、Q16 (a) MAP 1.1。

## 0. 用語

- **handle**: bluetoothd が socket に出す 1 通の名前 `<session 8 桁の 16 進>.<スマホの handle 16 桁の 16 進>`。MAP の session（MAS の接続）の間だけ有効。保存に使わない。
- **key**: 1 通の重複の鍵（16 桁の 16 進、p003 §8.3）。MAP の session を越えて同じ。保存の `Source` に使う。日時の無い 1 通は `-`（partial）。
- **cursor**: 1 回の同期の中だけで使う続きの目印（bluetoothd が作る。中身を他の層は読まない）。
- **目印（sync mark）**: Phone の app が保存する「どこまで取り込んだか」（§6.2）。

## 1. 層の流れ

```
Phone の app（WS170、userland/desktop/phone/）
   保存の唯一の持ち主: ~/Documents/Phone/（messages・contacts・sync/）
   │  libkeiland（kl_system_phone_*、KL_VERSION は merge の時に Q1）
   │    同じ uid の compositor への Wayland の system manager の拡張 kl_system_phone_v1
compositor（userland/desktop/wayland/phone-shell.c）
   backend の表: 0 none・1 loopback・2 bluetooth（desktop の設定 phone.backend）
   1 通も disk に書かない。持ち主の app へ中継し、banner を出す
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

1. app が起動・link の ready（§3.4 の `KL_PHONE_LINK`）・「今すぐ同期」で `kl_system_phone_sync(MESSAGES, since, "", 32)` を出す。`since` は目印から（§6.2）。
2. compositor は app の request を backend の `kl_backend_phone_page` に渡す。backend は bluetoothd に `PHONE PAGE messages since=… cursor= count=32`。
3. bluetoothd の map.c が MAS で COUNT・LIST・GET・UNREAD を行い、1 通ごとに `PHONE MESSAGE …` の行と本文を返し、最後に `PHONE PAGE-END cursor=… more=… count=… skipped=…`、`DONE`。
4. backend は 1 通ごとに item、最後に page の終わりを compositor へ。compositor は app の request の id を付けて `item` と `page_end` の event を送る。libkeiland は item を app の queue に積む。
5. app は item を保存し（key で重複を除く）、`page_end` の `count` が受けた item の数と一致した時だけ、`more=1` なら同じ同期の続きを `cursor` で頼む。`more=0` で目印を進める。

**(B) 受信の live（push、bluetoothd が主）**

1. スマホが MNS に NewMessage を Put。map.c が LOCATE・GET・UNREAD を行い、SUBSCRIBE の接続へ `PHONE MESSAGE …` と本文。
2. backend の SUBSCRIBE の接続がそれを読み、compositor へ live の item（request 0）。
3. compositor は (a) live を聞く（`kl_system_phone_listen`）持ち主の全部の phone object へ `item`（request 0）、(b) Phone の app が前面に無ければ banner（§7）。**誰も保存しない**: app が閉じていれば item は捨てられ、次の同期（A）で取り込まれる（Q2 の「中継だけ」の帰結）。
4. app は live の item を (A) と同じ規則で保存する（key で重複を除く）。

**(C) 送信**

1. app が `State: sending` の item を保存し、`kl_system_phone_send_text(SMS, to, text, length, &request)`。
2. compositor は backend の `kl_backend_phone_send` へ。backend は `PHONE SEND to="…" length=N` と N byte。
3. bluetoothd は bMessage を組み立て PushMessage（outbox）。答え `PHONE SENT request=<n> handle=<h> state=pushed`、`DONE`。backend は result（id、0、bluetoothd の request の番号 n）を compositor へ。compositor は app へ `status(request, QUEUED)` を送り、(n → app の object と request) の対応を覚える。
4. スマホが送り、MNS に SendingSuccess（と DeliverySuccess）。bluetoothd は SUBSCRIBE へ `PHONE SENT request=<n> handle=<h> state=sent|delivered|failed`（state ごとに 1 回）。compositor は対応から app へ `status(request, SENT|DELIVERED|FAILED)`。app は item の State を書き直す。
5. 送った 1 通は後の同期（sent の folder）で key 付きでも来る。app は §6.3 の規則で自分の `sending`・`sent` の item と重ね、Source を足す（2 つ目の item を作らない）。

## 2. 保存（Phone の app、WS170 の store の変更）

誰が何を保存するか（Q2）:

| 層 | 保存する物 |
| --- | --- |
| Phone の app | 全部（1 通、連絡先、同期の目印）。`~/Documents/Phone/` |
| libkeiland | 何も（memory の queue だけ） |
| compositor | 何も（app の request と bluetoothd の request の番号の対応だけ、memory） |
| libkeiland-backend | 何も（読みの buffer だけ） |
| bluetoothd | 1 通は何も。`.phone` の記録（持ち主・profile・enabled）と bond だけ（p003 §3） |

WS170 の store（`userland/desktop/phone/store.c`）の変更（p001 §8.6 を確定）:

- 1 通の file: `messages/<会話の key>/<file の名前>.txt`。header に `Source: bt:<スマホの address>:map:<key>`（key が `-` の partial は `Source` を書かず `Partial: yes`）、`Truncated: yes`（bluetoothd が 16 KB で切った時）。`Channel: sms`、`Direction: in|out`、`Date: <UNIX 秒>`（item の `time`）、`State: unread|read|sending|sent|delivered|failed`。
- file の名前: Source のある 1 通は `s<key>.txt`（2 台の機械が同じスマホを同期しても cloud の上で 1 つの file）。Source の無い 1 通（手で送った・partial）は今の `<日時>-<通し番号>.txt`。
- 会話の key: 連絡先の番号と一致すれば連絡先の id、しなければ番号の key `n<正規化した番号>`（連絡先を作らない。一覧には番号で出す）。正規化: `[0-9+]` 以外を除き、先頭の `00` を `+` に。国の文脈（`090…` と `+8190…`）の重ね合わせは表示の時に Settings の地域で行う（保存の名前は変えない）。後で連絡先が作られたら、その番号の `n…` の folder を連絡先の folder として**読む**（移し替えない。表示で 2 つを重ねる）。
- 重複の判定（保存の前）: 同じ `Source` の file があれば、`State` の read の変化だけを書き直し、新しい file を作らない。
- 同期の目印: `sync/bt-<address>.state`（§6.2）。
- 上限: store は開く時に全部を読む（`store.c`）。スマホの SMS は 30 日・2 folder × 500 = 最大 1000 通 × 同期の回数。今の `STORE_CONTACTS_MAX` 1024 は連絡先の数で、1 通の数の上限は無い。開く時間の見積もりは p004 の実装で測る（10000 通で 1 秒を目安、越えれば会話ごとの遅延の読みを Future Work に）。

## 3. libkeiland の interface（app が使う）

既存（KL_VERSION 55、変えない）: `kl_system_phone_send`（1023 byte まで）、`kl_system_phone_call`、`kl_system_take_phone_event`（`struct kl_phone_event`、v1 の `received`・`status`）。`struct kl_phone_event` の大きさは変えない（公開の ABI）。

新しく足す（KL_VERSION は merge の時に Q1 が番号を割り当てる。以下では `KL_VERSION N`）:

### 3.1 定数

```c
#define KL_SYSTEM_HAS_PHONE_SYNC	(新しい bit)	/* kl_system_phone_sync, _listen, _send_text, _mark_read, _link (KL_VERSION N) */

#define KL_PHONE_MESSAGES	0U	/* kl_system_phone_sync の what */
#define KL_PHONE_CONTACTS	1U	/* p005（PBAP）。それまでは ENOTSUP */
#define KL_PHONE_CALLS		2U	/* p005（PBAP の履歴）。それまでは ENOTSUP */

#define KL_PHONE_QUEUED		6U	/* status の新しい状態: スマホの outbox に入った（まだ送っていない） */
#define KL_PHONE_ITEM		3U	/* kl_phone_event の kind: item が kl_system_take_phone_item で取れる */
#define KL_PHONE_PAGE_END	4U	/* kl_phone_event の kind: 1 つの page の終わり（request・state に error） */
#define KL_PHONE_LINK		5U	/* kl_phone_event の kind: link の状態が変わった（kl_system_phone_link で読む） */
#define KL_PHONE_DROPPED	6U	/* kl_phone_event の kind: live の item か event を落とした（同期をやり直す） */

#define KL_PHONE_SEND_MAX	8192U	/* kl_system_phone_send_text の本文の上限（byte、NUL 無し） */
#define KL_PHONE_ITEM_TEXT_MAX	16384U	/* item の本文の上限（bluetoothd が切る） */
#define KL_PHONE_HANDLE_MAX	32U
#define KL_PHONE_KEY_MAX	20U
#define KL_PHONE_CURSOR_MAX	64U
#define KL_PHONE_NAME_MAX	132U	/* 128 byte と NUL に余裕 */
#define KL_PHONE_DATETIME_MAX	24U
```

### 3.2 構造体

```c
/* 1 通（同期の page の item、または live の item）。text は libkeiland の buffer を指し、次の kl_system_take_phone_item まで有効。 */
struct kl_phone_item {
	uint32_t request;		/* 同期の request の番号、live は 0 */
	unsigned what;			/* KL_PHONE_MESSAGES（p005 で CONTACTS・CALLS） */
	char handle[KL_PHONE_HANDLE_MAX];	/* "<session>.<handle>"、mark_read に使う */
	char key[KL_PHONE_KEY_MAX];		/* 16 桁の 16 進、または "-" */
	unsigned folder;		/* 0 inbox、1 sent */
	unsigned direction;		/* 0 in、1 out */
	int64_t time;			/* UNIX 秒 */
	unsigned zone;			/* 0 phone、1 mse、2 local、3 received（時刻の出どころ） */
	char datetime[KL_PHONE_DATETIME_MAX];	/* スマホの文字列のまま（表示しない、重複の判定の補い） */
	char peer[KL_PHONE_NAME_MAX];		/* 相手の番号 */
	char name[KL_PHONE_NAME_MAX];		/* 相手の名前（スマホの連絡先の名前、無ければ空） */
	unsigned read;			/* 1 既読 */
	unsigned partial;		/* 1: key が無い（日時が分からない） */
	unsigned truncated;		/* 1: 本文を 16 KB で切った */
	const char *text;		/* UTF-8、NUL で終わる */
	size_t length;			/* text の byte 数 */
};

/* phone link の状態（KL_PHONE_LINK の後に読む）。 */
struct kl_phone_link {
	unsigned backend;		/* 0 none、1 loopback、2 bluetooth */
	unsigned linked;		/* 1: スマホの link が ready */
	unsigned messages;		/* 0 off、1 connecting、2 ready、3 failed */
	unsigned can_send;		/* 1: 送信できる */
	unsigned notify;		/* 1: 受信の通知が来る（0 なら同期だけで受ける） */
	unsigned owner;			/* 1: この user がスマホの持ち主 */
	char address[18];		/* スマホの address（同期の目印の名前に使う）、無ければ空 */
	char why[32];			/* 失敗の語（permission・no-mas・timeout・key-missing・peer-closed・not-owner …） */
};
```

### 3.3 関数

| 関数 | 意味 | 返り値 |
| --- | --- | --- |
| `int kl_system_phone_listen(struct kl_system *system, unsigned on)` | この app が新しい event（`KL_PHONE_ITEM` の live・`_LINK`・`_DROPPED`）を受けるか。on で link の状態が 1 回来る。Phone の app だけが呼ぶ | 0、ENOTSUP（`KL_SYSTEM_HAS_PHONE_SYNC` 無し） |
| `int kl_system_phone_link(const struct kl_system *system, struct kl_phone_link *link)` | 最後に聞いた link の状態 | 0、ENOTSUP、ENOENT（まだ聞いていない） |
| `int kl_system_phone_sync(struct kl_system *system, unsigned what, int64_t since, const char *cursor, unsigned count, uint32_t *request)` | 1 page を頼む（count 1〜32、cursor は空か前の `page_end` の物） | 0、ENOTSUP、EINVAL、EBUSY（この app の同期が 1 つ走っている） |
| `int kl_system_take_phone_item(struct kl_system *system, struct kl_phone_item *item)` | 届いた item を 1 つ取る（古い順） | 1 取った、0 無い |
| `int kl_system_phone_page_end(const struct kl_system *system, uint32_t request, char *cursor, size_t size, unsigned *more, unsigned *count, unsigned *skipped, int *error)` | `KL_PHONE_PAGE_END` の event の request の結果 | 0、ENOENT |
| `int kl_system_phone_send_text(struct kl_system *system, unsigned channel, const char *to, const char *text, size_t length, uint32_t *request)` | 送信（`KL_PHONE_SEND_MAX` まで、UTF-8、NUL 無し） | 0、ENOTSUP、EINVAL |
| `int kl_system_phone_mark_read(struct kl_system *system, const char *handle, uint32_t *request)` | スマホの側を既読に | 0、ENOTSUP、EINVAL |

- 結果と状態は今の `kl_system_take_phone_event` で来る: `KL_PHONE_STATUS`（request、state: QUEUED・SENT・DELIVERED・FAILED、送信の失敗の errno は §3.5）、`KL_PHONE_ITEM`（item が queue にある合図、request は item の物）、`KL_PHONE_PAGE_END`（request、state に error、詳しくは `kl_system_phone_page_end`）、`KL_PHONE_LINK`、`KL_PHONE_DROPPED`。`mark_read` の結果は `KL_PHONE_STATUS`（state 0、error だけ）。
- libkeiland の queue: 同期の item は request ごとに 32 個（1 page）、live の item は 16 個の ring。本文は item ごとに malloc（16 KB まで）、`take` で前の物を free。live の ring が満ちて古い物を捨てた時、libkeiland は `KL_PHONE_DROPPED` を立てる（compositor には分からない、p001 S7）。
- 今の v1 の `received`（1023 byte で切った本文）は、listen していない app（古い app）にだけ従来どおり来る。listen した app には live は `KL_PHONE_ITEM` だけで来る（二重にしない）。

### 3.4 app の側の規則（Phone の app）

- 起動の時に `kl_system_phone_listen(1)`。`KL_PHONE_LINK` で `messages` が ready になった時と、`KL_PHONE_DROPPED` の時と、起動の時に同期（§6）。
- 送信の欄の上限は `can_send` が 0 の時は送信の button を灰色に（「スマホが送信を受け付けません」。iPhone の MAP、Android の一部）。

### 3.5 errno（app が見る物）

| errno | 意味（bluetoothd の語） |
| --- | --- |
| ENODEV | backend が無い（phone.backend 0） |
| ENOTSUP | 機能が無い（Linux・FreeBSD、古い compositor、`no-send`） |
| ENOTCONN | スマホの MAP が ready でない（`not-ready`） |
| EACCES | 持ち主でない、スマホが許可していない（`permission`） |
| EINVAL | 番号・本文・引数の誤り（`number`・`argument`） |
| EBUSY | 混んでいる（`busy`） |
| ESTALE | cursor か handle が古い MAP の session の物（`stale-cursor`・`stale`）: app は目印から同期をやり直す |
| ECONNRESET | 途中で link か MAP が切れた（`lost`） |
| ETIMEDOUT | 読むのが遅すぎた・スマホが答えない（`slow`・`timeout`） |
| EMSGSIZE | 1 通の listing が大きすぎる（`too-large`） |
| ENOENT | スマホに無い（`not-found`） |
| EIO | スマホが断った（`refused`・`unavailable`） |

## 4. compositor の protocol（kl_system_phone_v1 の拡張）

manager の版を 1 上げる（番号は merge の時に Q1）。v1 の request 0〜2・event 0〜2 は今のまま。

| 向き | opcode | 名前 | 引数 |
| --- | --- | --- | --- |
| request | 3 | listen | uint on |
| request | 4 | sync | uint request, uint what, int since_high, uint since_low, string cursor, uint count |
| request | 5 | send_text | uint request, uint channel, string to, array text |
| request | 6 | mark_read | uint request, string handle |
| event | 3 | item | uint request, uint what, string handle, string key, uint folder, uint direction, int time_high, uint time_low, uint zone, string datetime, string peer, string name, uint flags（bit 0 read、1 partial、2 truncated）, array text |
| event | 4 | page_end | uint request, int error, string cursor, uint more, uint count, uint skipped |
| event | 5 | link | uint backend, uint linked, uint messages, uint can_send, uint notify, uint owner, string address, string why |
| event | 6 | dropped | — |

- `status` の state に QUEUED（6）を足す。`result(request, applied, saved)` は v1 のまま（applied は OK・UNAVAILABLE、新しい request の失敗は `status(request, FAILED)` の後に error を運ぶため、`status` に第 3 の引数を足さず、新しい event `failed(uint request, int error)`（opcode 7）を足す）。
- 1 つの item の wire は最大 約 17 KB（本文 16 KB）で、wire の上限 65532 byte に収まる。client の出力が 1 MB を越えると compositor は event を捨てる（`wire.c`）: page の item を 1 つでも捨てたら `page_end` の error を ENOBUFS にする（app は数の不一致でも分かる）。live の item を捨てたら `dropped` を送る。
- 権限: phone object は compositor と同じ uid の client だけ（今のまま）。compositor は bluetoothd の持ち主の判定（§5.6）に従う: 持ち主でない session の compositor では link の `owner` が 0、sync・send は EACCES。

### 4.1 compositor の中（phone-shell.c）

- backend の表に `bluetooth`（値 2）。表の形を `struct phone_backend { name; open; close; update; send; call; sync; mark_read; }` に広げ、loopback は sync を ENOTSUP。
- `phone.backend` が 2 の時、compositor は起動時に `kl_backend_phone_open()` し、main loop の毎回 `kl_backend_phone_update()` で読んだ物を配る。backend が ENOTSUP（Linux・FreeBSD）なら link の `messages` は off、why `unsupported`。
- 同期の request: 1 つの app の同期は 1 本（EBUSY）。全体で同時の同期は bluetoothd の PAGE の枠（2）まで、それを越える app の同期は compositor の中で待たせる（順番に backend へ）。
- 送信の対応表: `bluetoothd の request の番号 n → (client, object, app の request)`、32 個（古い物から捨てる）。client が去ったら対応を消す（後の `PHONE SENT` は捨てる。app は次の同期で sent を知る、§6.3）。
- banner（§7）。
- log は件数・長さ・errno だけ（番号・名前・本文を出さない）。

## 5. bluetoothd の socket の PHONE の文法（正）

p003 §9 を正にし、i06・i07 の実装で次を確定する。行は改行で終わる。値は `key=value`（value は空白の無い語か `"…"`、`"` と `\` は `\` で、0x20 未満と 0x7F は `\xHH`）。名前と番号は escape の前に 128 byte で切る。daemon の 1 行は改行を含め 2047 byte 以下。長さ付きの値は行の最後の `length=N` の後の N byte（改行を付けない）。

### 5.1 request（1 つの接続で 1 度に 1 つ。答えは `DONE` で終わり、失敗は `ERROR <語>` の後に `DONE`）

| request | 誰が | 答え |
| --- | --- | --- |
| `PHONE SHOW` | 誰でも（持ち主でない人には address と enabled だけ） | `PHONE address=… owner=<uid>|invalid mine=0|1 enabled=0|1 profiles=m,c,h present=0|1 link=none|paging|securing|ready|closing messages=off|connecting|ready|failed send=0|1 notify=0|1[ why=…]`、`DONE`。記録が無ければ行無しで `DONE` |
| `PHONE LINK ADDRESS on|off [profiles=m,c,h]` | 持ち主・root | `DONE` |
| `PHONE SUBSCRIBE` | 持ち主・root、同時に 2 本まで | `DONE` の直後に今の `PHONE STATE …` を 1 行、以後 event の行だけ（§5.2）。この接続の request の行は読み捨てる |
| `PHONE PAGE messages since=<UNIX 秒> cursor=<cursor か空> count=<1..32>` | 持ち主・root | `PHONE MESSAGE …`＋本文 が 0〜count 個、`PHONE PAGE-END cursor=<次> more=0|1 count=<出した数> skipped=<飛ばした数>`、`DONE` |
| `PHONE READ handle=<handle>` | 持ち主・root | `DONE` |
| `PHONE SEND to="<番号>" length=<1..8192>` ＋ N byte | 持ち主・root | `PHONE SENT request=<n> handle=<handle>|- state=pushed`、`DONE` |
| `PHONE DROP ADDRESS` | root（試験の道具） | `DONE` |
| `PAIR ADDRESS bredr phone=1` | seat の人（p003 §3.2） | 今の PAIR と同じ |
| `FORGET ADDRESS bredr` | 持ち主・root（`.phone` が有効な時） | 今と同じ |

ERROR の語: `not-ready`・`no-send`・`permission`・`owned`・`phone-seat`・`not-phone`・`argument`・`number`・`length`・`busy`・`stale-cursor`・`stale`・`lost`・`slow`・`too-large`・`not-found`・`unavailable`・`refused`・`timeout`。

- PAGE の cursor は `<session 8 桁の 16 進>.<since の 16 進>.<folder>.<offset>`（中身は bluetoothd だけが読む）。`more=0` の時の cursor は使わない。
- PAGE の途中の失敗は item を出した後でも `ERROR <語>`、`DONE`（`PAGE-END` は来ない。app は目印を進めない）。
- 1 つの接続の request の待ちは 1 つ。backend は request の接続を 1 本、SUBSCRIBE の接続を 1 本使う（bluetoothd の 16 の枠のうち、compositor の Bluetooth の 4 本と合わせて 6 本、seat の人の予約 4 の中で足りる: 予約は「空きが 4 以下の時に root と seat の人だけ」）。

### 5.2 event（SUBSCRIBE の接続へ）

| 行 | 意味 |
| --- | --- |
| `PHONE STATE …`（SHOW の行と同じ中身） | 状態が変わった時と SUBSCRIBE の直後 |
| `PHONE MESSAGE handle=… key=…|- folder=inbox|sent dir=in|out time=<UNIX 秒> zone=phone|mse|local|received datetime="…" peer="…" name="…" read=0|1 partial=0|1 truncated=0|1 length=<n>` ＋ n byte | live の受信（PAGE の答えの item も同じ形） |
| `PHONE SENT request=<n> handle=<handle> state=sent|delivered|failed` | 自分の送信の状態（state ごとに 1 回） |
| `PHONE MESSAGE-GONE handle=<handle>` | スマホで消された（app は使わない、p004 では捨てる） |
| `PHONE DROPPED` | bluetoothd の queue が満ちて live の 1 通を落とした、または SUBSCRIBE の出力の queue が 192 KB を越えて event を落とした |

### 5.3 libkeiland-backend の関数との対応（`kl_backend_phone`、新）

```c
struct kl_backend_phone;
struct kl_backend_phone *kl_backend_phone_open(void);		/* NULL は memory 無しだけ。Linux・FreeBSD は ENOTSUP の物を返す */
void kl_backend_phone_close(struct kl_backend_phone *phone);
int kl_backend_phone_update(struct kl_backend_phone *phone, unsigned *changed);	/* 待たずに読む。changed: STATE・ITEM・RESULT・SENT・DROPPED の bit */
void kl_backend_phone_get_state(const struct kl_backend_phone *phone, struct kl_backend_phone_state *state);
int kl_backend_phone_page(struct kl_backend_phone *phone, unsigned what, int64_t since, const char *cursor, unsigned count, uint32_t *id);
int kl_backend_phone_read(struct kl_backend_phone *phone, const char *handle, uint32_t *id);
int kl_backend_phone_send(struct kl_backend_phone *phone, const char *to, const uint8_t *text, size_t length, uint32_t *id);
int kl_backend_phone_take_item(struct kl_backend_phone *phone, struct kl_backend_phone_item *item);	/* id は page の id、live は 0 */
int kl_backend_phone_take_result(struct kl_backend_phone *phone, struct kl_backend_phone_result *result);	/* id、errno、PAGE-END の cursor・more・count・skipped、SEND の n */
int kl_backend_phone_take_sent(struct kl_backend_phone *phone, uint32_t *bluetoothd_request, unsigned *state);	/* live の PHONE SENT */
```

| backend の関数 | bluetoothd の行 | backend が返す物 |
| --- | --- | --- |
| open | 接続 2 本（request、SUBSCRIBE）。無ければ 1 秒ごとに繋ぎ直す（今の bluetooth-zedbsd と同じ） | state の reachable |
| （SUBSCRIBE の接続） | `PHONE SUBSCRIBE` → `DONE` → `PHONE STATE`… | STATE の変化、live の item（id 0）、SENT、DROPPED |
| page | `PHONE PAGE messages …` | item（page の id）を順に、最後に result（PAGE-END か ERROR） |
| read | `PHONE READ handle=…` | result |
| send | `PHONE SEND to="…" length=N`＋N byte | result（`PHONE SENT … request=n`、n を result に） |
| 状態の読み直し | SUBSCRIBE の `PHONE STATE`（SHOW は使わない。SUBSCRIBE が EACCES なら SHOW を 30 秒ごと、持ち主でない時の表示用） | state |

- backend の request は queue で 1 本ずつ（最大 8 個、越えると EBUSY）。page の答えの item は来たまま compositor へ渡す（backend は 1 page 分を抱えない）。
- 読みの buffer: 行は 2048 byte、本文は `length` の分を malloc（16 KB まで、越える `length` は接続を閉じて繋ぎ直す: bluetoothd の誤り）。
- 切断（bluetoothd の再起動）: 待ちの request は全部 ECONNRESET、state は reachable 0 → 繋ぎ直して SUBSCRIBE、STATE が来たら compositor が link の event を送る。

### 5.4 bluetoothd の中の持ち主と権限

- PHONE の request（SHOW を除く）と SUBSCRIBE は、有効な `.phone` の記録の uid（持ち主）と root だけ（p003 §3、§9.2）。compositor は session の user の uid で接続するので、持ち主の session の compositor だけが SMS を受ける。
- 持ち主が seat に居ない間は MAP を閉じている（Q14）。その間の SUBSCRIBE は受ける（STATE だけが来る）。

## 6. 同期の規則（app が主）

### 6.1 いつ

- app の起動の時、link の `messages` が ready になった時、`KL_PHONE_DROPPED`・`ESTALE` の後、利用者の「今すぐ同期」。
- 1 回の同期は messages の inbox と sent を cursor でたどり、`more=0` まで。

### 6.2 目印

`sync/bt-<address>.state`（text、`messages_since <UNIX 秒>` の行。p005 で contacts・calls の行を足す）。

- 初回（目印が無い）: `since = 今 − 30 日`（Q4）。bluetoothd が folder ごとに最大 500 通で止める。
- 同期が `more=0` で終わったら `messages_since = (この同期を始めた時刻) − 24 時間`（遅れて届く SMS・時刻の推定の誤り・page の間の 1 件のずれを重ねて拾う。重複は key で除く）。途中の失敗では書き換えない。
- cursor は目印に書かない（MAP の session ごとに無効）。

### 6.3 重複と送った 1 通の重ね

- key のある item: 同じ `Source` の file があれば新しく作らない（read の変化だけ反映: スマホで既読になれば `State: read`、手元で既読にした物はそのまま）。
- partial（key `-`）の item と、自分の送信（`Source` の無い `Direction: out` の item）: 同じ会話・同じ方向・同じ本文・時刻の差が ±10 分の file があれば重ね（`Source` を足す・時刻はスマホの物に）、無ければ新しく作る。
- 送信の State: `sending` →（`status` QUEUED は State を変えない）→ SENT で `sent`、DELIVERED で `delivered`、FAILED で `failed`。app が閉じていて status を逃した `sending` は、同期で sent の folder の同じ 1 通と重なった時に `sent` に。1 時間たっても `sending` のままなら `failed`（「送れたか分かりません」）。

### 6.4 取りこぼしの検出

| 何で | 誰が気づく | app の動き |
| --- | --- | --- |
| bluetoothd の queue が満ち live を落とした | bluetoothd → `PHONE DROPPED` → compositor の `dropped` | 同期 |
| SUBSCRIBE の出力が溢れた | bluetoothd → `PHONE DROPPED` | 同期 |
| compositor の client の出力が 1 MB を越えた | compositor → `page_end` の ENOBUFS、live は `dropped` | 同期（page はやり直し） |
| libkeiland の ring が満ちた | libkeiland → `KL_PHONE_DROPPED` | 同期 |
| page の item の数が `count` と違う | app | 目印を進めず、同期をやり直す |
| app が閉じていた | — | 起動の時の同期で拾う |
| page の間の受信・削除で 1 件ずれる（offset で進むため） | — | 24 時間の重なりと live で補う（p003 §8.5） |

## 7. 通知の banner（compositor、Q8）

- live の item で `direction` in、Phone の app が前面に無い時: banner（相手の名前（item の `name`、無ければ番号）と本文の 1 行目）。押すと Phone の app をその相手のタイムラインで開く。
- lock の画面では「新しいメッセージ」と相手の名前だけ（本文を出さない）。
- 2 つの button（着信の応答・拒否、p006）と lock の画面の表示は WS156・lock の持ち主への依頼（p001 §8.5、Q1 が割り当て）。SMS の banner は 1 つの button（本体の click）で今の WS156 の通知で足りる。

## 8. 切断・再接続・suspend・失敗

| 出来事 | bluetoothd | compositor・app |
| --- | --- | --- |
| スマホが離れた（link が切れた） | MAP の queue の request は全部 `ERROR lost`、`PHONE STATE … link=none messages=off`、自分で page し直す（p003 §5） | link の event。走っていた同期は ECONNRESET（目印は進めない）。ready に戻ったら同期 |
| スマホが MAP を許可していない | `messages=failed why=permission`、600 秒ごとと `PHONE LINK on` でやり直し | link の why `permission`: app は「スマホでメッセージへのアクセスを許可してください」 |
| MAS が無い | `why=no-mas`（やり直さない） | 「スマホがメッセージの共有に対応していません」 |
| suspend | 何もしない。resume で backoff を戻す（p003 §5.6）。死んだ link は supervision の timeout で切れる | 同上 |
| 持ち主の logout | MAP と ACL を切る（Q14） | 持ち主の compositor は終わる |
| bluetoothd の再起動 | — | backend が繋ぎ直す（§5.3）、走っていた request は ECONNRESET |
| 送信の途中で切れた | PUSH が答えの前なら `ERROR lost` | app は `failed`。答えの後なら `pushed` 済みで、スマホが送る（status は来ないかもしれない: §6.3 の 1 時間） |

## 9. 判断の残り（推しは太字、Q1 経由でユーザーへ）

| # | 判断 | 選択肢 | 推し |
| --- | --- | --- | --- |
| P1 | app が閉じている間に届いた SMS | **保存しない（banner だけ。次の起動の同期で取り込む。Q2 の「中継だけ」）** / compositor が app の代わりに `~/Documents/Phone/` に書く（Q2 の変更） | **保存しない** |
| P2 | libkeiland の item の本文 | **libkeiland の buffer を指す（次の take まで有効、struct は小さい）** / struct に 16 KB の配列（呼び手の stack が大きい） | **buffer を指す** |
| P3 | 長い送信の関数 | **新しい `kl_system_phone_send_text`（長さ付き、8192 byte）。今の `kl_system_phone_send`（1023 byte）は残す** / 今の関数の上限を上げる（古い compositor との組で黙って切れる） | **新しい関数** |
| P4 | `phone.backend` を 2 にするのは誰か | **Settings の「スマホとして使う」を on にした時に Settings が設定（p001 §8.4）。手で 0 に戻せる** / compositor が bluetoothd の記録を見て自動 | **Settings** |
| P5 | 送った 1 通の重ね | **app が「同じ会話・本文・±10 分」で重ねる（bluetoothd の handle は session ごとに変わるので使えない）** / bluetoothd が送った 1 通の key を答えに付ける（MSE の listing を待つ必要、遅い） | **app が重ねる** |
| P6 | 同期の 1 page の数 | **32（bluetoothd の上限）固定** / app が選ぶ | **32 固定** |

KL_VERSION と manager の版の番号は merge の時に Q1 が割り当てる（判断ではない）。

## 10. p001 §8 からの変更（この文書が正）

| p001 §8 | この文書 | 理由 |
| --- | --- | --- |
| `PHONE PAGE … after=` | `cursor=` | p003 §1.1 |
| event の `source=` | `handle=` と `key=` | p003 §8.3: handle は session ごと |
| `PHONE GET`・`fetch(source)` | 作らない（本文は item で 16 KB まで来る） | p003 M9 |
| 本文 64 KB | 16 KB（受け）、8192 byte（送り） | p003 §1.1 |
| `page_item`・`message` の 2 つの event | 1 つの `item`（request 0 が live） | 同じ形で、app の保存の道を 1 つに |
| suspend で profile を閉じる | 何もしない | p003 §1.1 |
| `Source: bt:<address>:map:<id>` | `Source: bt:<address>:map:<key>`、file の名前は `s<key>.txt` | key が session を越える名前 |

## 11. 作る物の一覧と持ち場

### 11.1 bluetoothd（WS197）

| 物 | どこで |
| --- | --- |
| `.phone` の記録・PAIR phone=1・LINK・SHOW・FORGET・HID の上限 | p003 i01（済み） |
| 出力の queue・client の枠 16・長さ付きの入力 | p003 i02（済み） |
| phone link の一生（page・受け・交差・SECURING・切断の理由・sleep.end） | p003 i03（済み） |
| listing と event の XML・datetime | p003 i04（済み） |
| bMessage | p003 i05（済み） |
| OBEX の response の hook と timeout、map.c（MAS の client・MNS の server・queue・PAGE・live・PUSH・SENT）、phone の配線（opened の ours、MNS の SDP record、MAP の ready で backoff を戻す、SHOW の present） | p003 i06（途中、f06bf4dbb） |
| phoneio の item の行、main の PHONE の request（PAGE・READ・SEND・SUBSCRIBE と直後の STATE・DROPPED）、token の cancel、answer・emit・room の hook（client を同期で閉じない） | p003 i07 |
| SHOW と STATE にスマホの名前（`name="…"`、Settings の表示）を足すか | p004（必要なら、§9 に無い小さい足し） |

### 11.2 libkeiland-backend（WS197 p004）

- `keiland-backend.h` に §5.3 の API。
- `libkeiland-backend-zedbsd/phone-zedbsd.c`（新、bluetoothd の socket）と host の試験（偽の bluetoothd の socket pair、`plan/ws131/tests/host-session.c` と同じ作り）。
- `libkeiland-backend/unsupported/phone-unsupported.c`（Linux・FreeBSD、ENOTSUP）。`plan/tools/keiland-os-boundary` の表に足す。

### 11.3 compositor（WS197 p004、WS170 の code を変える）

- `phone-shell.c`: backend の表の拡張、`bluetooth`、§4 の request と event、送信の対応表、同期の待ち、banner。
- `kl-system-protocol.h`: §4 の opcode、manager の版。
- 試験: compositor の host 試験（偽の backend）と T1 の AAT（偽の bluetoothd は無いので、backend を loopback の拡張で試す: loopback に sync の偽の page を足す）。

### 11.4 libkeiland（WS197 p004、WS170 の API の拡張）

- `keiland.h`: §3 の定数・struct・関数、KL_VERSION N の行。`system.c`・`system-protocol.c`: request と event、queue、ring、DROPPED。`exports.map`。
- 試験: `plan/ws089/tests/host-kl-system.c` の作りで item・page_end・dropped。

### 11.5 Phone の app（WS170 の code、WS197 p004 で行う。WS170 の ws.md に記録）

- store: §2 の Source・Partial・Truncated・`s<key>.txt`・番号の key の会話・目印。
- main: listen、link の表示、同期（§6）、重複と重ね（§6.3）、送信の `send_text`、`can_send` の灰色、mark_read（会話を開いた時、スマホの側も既読に）。
- 試験: store の host 試験（重複・重ね・目印）と T1 の AAT（loopback の偽の page）。

### 11.6 Settings（WS197 p004）

- Bluetooth の頁の「スマホとして使う」（`PAIR … phone=1`、`kl_system_bluetooth_*` の拡張と `kl_backend_bluetooth` の PAIR に phone の flag）、profile の switch（`PHONE LINK`）、状態（SHOW の行）、「スマホで許可してください」、`phone.backend` を 2 に（P4）。

### 11.7 見積もり

p004 の 12 LW（ws.md）の内訳の案: backend 3、compositor 2.5、libkeiland 2、Phone の app 3、Settings 1、試験と T1 0.5。

## 12. 試験

- host: backend（偽の bluetoothd: PAGE の item と PAGE-END、ERROR、SUBSCRIBE の STATE・MESSAGE・SENT・DROPPED、長さ付きの本文が 2 回の read に割れる、bluetoothd の切断と繋ぎ直し）、libkeiland（queue・ring・dropped）、compositor の phone-shell（送信の対応表、同期の待ち、ENOBUFS）、Phone の store（Source の重複、partial の重ね、送った 1 通の重ね、目印の 24 時間）。
- QEMU（T1）: 本物のスマホは無い。loopback の backend に偽の同期を足し、Phone の app の同期と表示と送信の状態を AAT で。
- 実機（p008）: Android で 30 日の同期、受信の banner、送信の sent・delivered、離れて戻る、許可の拒否。

## Event

- 2026-10-10: 第 1 版（P1、agent/p1）。design-reviewer に回す。
