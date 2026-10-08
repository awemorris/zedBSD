<!-- awesome-plan project=zedbsd record=ws156-p001 -->

# ws156-p001: app の通知の設計

Phase ID: `ws156-p001`
Parent: [WS156](../ws.md)
Status: planning → Q1 の判定待ち（2026-10-08 q902 P1 の照合: H1〜H7 は 2026-10-05 夜に決定、p002〜p004 がこの設計で実装され cleared（T1-269・T1-375b））（旧: planning（2026-10-05 P1 generation17。設計の第 1 版。code は §9 の判断の後。2026-10-05 夜: H1〜H7 決定済み））
Phase disposition: normal
Queue: ベータ2 の P1 の列の 3 番目（q は Q1 が振る）

## 範囲

- ws.md の仕様 1〜5（画面の下の中央を流れる headline の popup、× で消す、ring の形の log と hotkey、すべて消去、個別に消した物は log に残さない）と、
  Q1 の割り当ての「USB 媒体の icon の置き換え」（ws132-p004 の bar の媒体の icon は「WS156 ができるまでの代わり」、`wayland/media.h` 8〜15）。
- 決めること: app から通知を出す口、通知の中身、popup の大きさと動き、続けて来た時の並び、log と hotkey、保存、全画面・lock の時、system の通知、試験。
- 範囲外: 実装（p002〜）。Linux・FreeBSD の D-Bus の通知（§7）。lock の画面に通知を出す設定（WS148 の Privacy の頁）。

## 1. 今の形（2026-10-05 の main を読んだ）

| 項目 | 今 | 場所 |
| --- | --- | --- |
| Keiland の拡張の protocol | 手で書いた wire の仕様と opcode（XML も scanner も無い）。`kl_system_manager_v1`（版 7、get_* の opcode 0〜8）から network・audio・power・devices・account・sharing・monitor の object を作る。同じ uid の client にだけ見せ、greeter には見せない | `userland/desktop/libkeiland/system/kl-system-protocol.h`、`wayland/protocol.c` 48〜74、`wayland/system.c` 290・325・582〜695、`wayland/settings.c` 284 |
| libkeiland の口 | `kl_system_open`・`kl_system_dispatch`・`kl_system_take_result`、要求は番号つきで、答えは後で 1 回（`result(request, applied, saved)`） | `libkeiland/system/system.c`、`keiland.h` 1128〜1523 |
| overlay の描画 | `zwl_glass_draw` の最後の層（`zwl_volume_draw_popup`・`zwl_corner_draw` の辺り、画面 keyboard の下）。glass の形（`glass_shape`、`.opacity` で fade）、文字（`glass_draw_text`、UTF-8、日本語は fallback の font）、× の glyph（`GLASS_CLOSE_GLYPH`） | `wayland/shell.c` 417〜611、`glass.h`、`volume.c` 448〜540 |
| animation | main loop が 10 ms ごとに `zwl_glass_tick`、動いている間 `server->dirty = 1`。ease は file ごとの `1-(1-t)^3`（`corner_ease`・`home_ease`） | `shell.c` 2254、`corner.c` 440・882 |
| 入力 | click は `zwl_glass_button` の優先の鎖（各 module が rect で当たりを見る）、key は `zwl_glass_key`。Super+L・Super+Tab・Super+Alt+文字が使用中、Super+N は空き | `shell.c` 622・2181、`seat.c` 903〜1017、`edit.c` 357 |
| 全画面・lock | 全画面の window が全部を覆うと overlay を描かない（`zwl_glass_still`・`zwl_glass_overlay` に加われば描ける）。lock の間は greeter だけを描き、greeter が全部の入力を取る | `shell.c` 488〜497・1225・1539、`greeter.c` 247 |
| 通知に近い物 | 媒体の icon（新しい媒体で bar に USB の icon、3 回点滅、click で `files --devices`）、Wi-Fi の失敗の文（bar の network の menu の中だけ、BUG-187）、WS052 p007 の計画の toast（sleep の中止の理由） | `wayland/media.c`、`network.c` 220・713、`plan/ws052/phase007/phase.md` |
| app の身元 | toplevel の `app_id`（`xdg_toplevel.set_app_id`）、icon は `zwl_icon_for_app_id`。client には番号と uid だけ（window の無い client には app_id が無い） | `zwl.h` 391、`icons.c` 572 |
| D-Bus | Linux の backend に system bus だけの小さな client（logind 用）。session bus・`org.freedesktop.Notifications` は無い | `libkeiland-backend-linux/dbus-linux.c` |

## 2. 口（protocol と libkeiland）

`kl_system_manager_v1` に `get_notify`（opcode 9、manager の版 8）を足し、`kl_system_notify_v1` を作る（今の system の object と同じ作り、同じ uid の
client だけ、greeter には見せない）。

| 種類 | 名前（引数） | 意味 |
| --- | --- | --- |
| request | `post(uint request, uint replaces, string app, string title, string body, uint flags)` | 通知を出す。`replaces` が 0 でなければ、同じ client が前に出した同じ番号の通知を置き換える（表示中なら中身だけ替え、log なら log の中で替える）。`app` は表示の名前（空なら client の toplevel の app_id）。`flags`: `URGENT`（§4.4）・`ACTION`（本文の click で `activated` を送る） |
| request | `withdraw(uint request, uint id)` | 出した通知を引っ込める（表示中なら消し、log からも消す） |
| event | `posted(uint request, uint id)` | 受けた通知の番号（client の中で一意、0 は無し）。断った時は今の `result(request, …)` で `INVALID`（長すぎる文字列など）・`BUSY`（§4.3 の上限） |
| event | `activated(uint id)` | 利用者が本文を click した（`ACTION` の時だけ） |
| event | `closed(uint id, uint reason)` | `DISMISSED`（× で消した）・`EXPIRED`（流れ去って log に入った時ではなく、log からも消えた時）・`CLEARED`（すべて消去）・`WITHDRAWN` |

- 文字列の上限: `app` 64 byte、`title` 128 byte、`body` 512 byte（UTF-8。超えたら `INVALID`）。描く時は題 1 行・本文 2 行で切り、最後を「…」。
- 1 つの client が出せる数: 表示待ちと log を合わせて 32 まで（超えたら `BUSY`、迷惑な app が log を埋めないように）。
- libkeiland:

  ```c
  struct kl_notification {
  	const char *app;	/* 表示の名前、NULL なら app_id */
  	const char *title;
  	const char *body;
  	uint32_t replaces;	/* 置き換える番号、0 は新しい通知 */
  	unsigned flags;		/* KL_NOTIFY_URGENT・KL_NOTIFY_ACTION */
  };
  int kl_system_notify(struct kl_system *system, const struct kl_notification *notification, uint32_t *request);
  int kl_system_notify_withdraw(struct kl_system *system, uint32_t id, uint32_t *request);
  /* posted・activated・closed は kl_system_dispatch の KL_SYSTEM_CHANGED_NOTIFY と kl_system_take_notify_event で受ける */
  ```

  `kl_app` を使う app には `kl_app_notify(app, title, body)`（`kl_app` の app_id を `app` にする、答えを待たない）を足す。
- compositor の中の通知（§6）は同じ表に `zwl_notify_post(server, source, title, body, action)` で入れる（protocol を通らない）。

## 3. popup

### 3.1 大きさと位置

- 幅は画面の幅の 20%（ユーザーの「画面サイズの20%程度」）、ただし 320〜640 px に収める（1280 px の画面で 320 px、1920 px で 384 px）。高さは 76 px
  （icon 36 px、題 1 行、本文 2 行）。
- 位置は画面の下の中央、下の端から 48 px 上。
- 形は今の popup の作り（影、白い glass α .86、角の半径 12）。左に app の icon（`zwl_icon_for_app_id`、無ければ汎用の鈴の icon）、右上に ×。

### 3.2 動き（headline の news の板）

| 段 | 時間 | 位置（popup の中心の x） | 不透明度 |
| --- | --- | --- | --- |
| 入る | 280 ms、ease-out（`1-(1-t)^3`） | 画面の右端の外（右の端に popup の左端）→ 中央 | 0 → 1 |
| とどまる | 3000 ms | 中央 | 1 |
| 出る | 280 ms、ease-in（`t^3`） | 中央 → 画面の左端の外（左の端に popup の右端） | 1 → 0 |

- 出始めの右下: 入る段の y は最後の位置と同じ（下の中央の高さ）。「画面右下の右端から出て」は、下の中央の高さの右端の外から入ることにする。
- pointer が popup の上にある間はとどまる段の時間を止める（読んでいる途中で流れない）。
- 出終わった通知は log に入る（§5）。

### 3.3 続けて来た時

- 表示は 1 つずつ。来た順に待ち行列に入れる（最大 8。超えたら一番古い待ちを直接 log に入れる）。
- 待ちがある間は、とどまる段を 1500 ms に縮める（早送り）。今の通知が出る段に入ると、次の通知が同時に入る段を始める（板が流れていく見え方）。

### 3.4 × と click

- × の click: その通知を消す。**log に残さない**（ユーザーの要望）。app には `closed(DISMISSED)`。
- 本文の click: `ACTION` の通知なら app に `activated` を送り、通知を消す（log に残さない）。`ACTION` の無い通知の本文の click は何もしない。

### 3.5 全画面・lock・greeter

- 全画面の window がある時: popup を出さず、直接 log に入れる（`URGENT` だけは出す）。全画面の動画・game を邪魔しない。
- lock の間: popup を出さず、直接 log に入れる（中身を lock の画面に出さない。lock の画面に出すかは WS148 の Privacy の設定で後に）。
- greeter: 通知を受けない（system の object は greeter に見せない）。

## 4. 中身の規則

1. 同じ client の `replaces` の置き換え（§2）。
2. client が切れた時: 表示中・log の通知は残る（`ACTION` は無効になり、click しても何もしない）。
3. 上限（§2）。
4. `URGENT`: 全画面でも出す。とどまる段は pointer か key で触るまで続く（最大 30 秒）。電池が残り少ない、など system の通知に使う。

## 5. log

- **hotkey は Super+N**（空いている、Notifications の N）。押すと popup と同じ位置・大きさの板に、最新の通知を出す（板は動かず、fade-in 150 ms）。
- **ring の形**: ←・→ の key（と板の左右の端の矢印の click）で 1 つずつたどる。古い方へ → の端（一番古い）の次は最新に戻る（ring）。
- **すべて消去**: 最新の通知の「次」（→ で最新を越えた位置）に「Clear all notifications」の button の板がある。押すと log を全部消し、各 client に
  `closed(CLEARED)`。log が空なら、Super+N は「No notifications」の板だけを出す。
- log の板の ×: その通知を log から消す。
- 閉じる: Super+N・Esc・板の外の click、10 秒の無操作。
- 保存: **session の中の memory だけ**（logout・再起動で消える。通知の中身を disk に残さない）。最大 100 個、超えたら一番古い物から消す（`closed(EXPIRED)`）。

## 6. system の通知（compositor の中から）

| 源 | 今 | 通知 |
| --- | --- | --- |
| 新しい媒体（ws132-p004） | bar の USB の icon（3 回点滅、click で `files --devices`） | 「USB drive connected」と媒体の名前。本文の click で `files --devices`（`ACTION`）。**bar の媒体の icon は消す**（Q1 の割り当ての「USB 媒体の icon の置き換え」）。媒体の icon が担っていた「まだ mount していない媒体がある」の表示は、log に残る通知と Files の devices で足りる |
| Wi-Fi の接続の失敗（BUG-187） | bar の network の menu の中の文だけ | 「Could not join NAME」と理由（menu の文は今のまま） |
| sleep の中止（WS052 p007） | 計画の toast | 「Sleep was cancelled」と理由。p007 の toast の代わり |
| 電池が残り少ない（WS132） | bar の電池の icon だけ | 10% と 5% で 1 回ずつ、`URGENT` |

source の名前は「System」、icon は Kei の mark。

## 7. Linux・FreeBSD

- Keiland の app（libkeiland）は 3 つの OS で同じ `kl_system_notify` を使う（compositor の拡張なので OS に依らない）。
- 他の toolkit の app（GTK・Qt）が使う `org.freedesktop.Notifications`（D-Bus の session bus）は v1 では受けない。zedBSD に D-Bus が無く、Linux の backend の
  D-Bus の client は system bus だけで、session bus の名前の所有・method の受け付けが無い。Linux・FreeBSD で受けるなら、session bus の小さな service
  （`keiland-notify-dbus`、Notify・CloseNotification・GetCapabilities・GetServerInformation の 4 つ）を別の段に足す（§9 の H6）。

## 8. 試験

- host: 純粋な module `wayland/notify.c`（待ち行列・置き換え・上限・timeline・log の ring・すべて消去・個別に消した物を残さない）を、switcher と同じく
  host で compile する試験（`plan/ws142/tests/run-host-switcher.sh` の形）。protocol は `plan/ws131/tests/host-system.sh` の形（host の libwayland-client と
  compositor の `system.c`・`notify-shell` の一部、libkeiland の `system.c`）で `post`・`posted`・`withdraw`・`closed` を確かめる。
- QEMU（T1）: 試験の client `userland/tests/keiland-notify`（`KEILAND-NOTIFY …` の行を出す、`keiland-system` の probe の形）で通知を出し、
  `ZWL NOTIFY post/show/hide/log/dismiss/clear` の log の行と、入る段・とどまる段・出る段の screenshot、Super+N の log、←・→、すべて消去、× を QMP の key と
  pointer で確かめる（`plan/ws142/tests/p005-guest.sh` の形）。全画面の時に出ないこと、lock の時に出ないこと。媒体の挿入（QEMU の USB の disk の
  hotplug、ws132-p004 の試験の形）で system の通知。
- 実機（UAT）: 見た目と速さ（しゅっと動くか、120 Hz の 5330 で）。

## 9. 人間の判断が要る点

| ID | 問い | 案 |
| --- | --- | --- |
| H1 | popup の大きさ:「画面サイズの20%程度」を幅の 20%（320〜640 px）、高さ 76 px にしてよいか | よい |
| H2 | log の hotkey | Super+N |
| H3 | log の保存 | session の中の memory だけ（logout で消える）、100 個まで |
| H4 | 全画面・lock の時 | popup を出さず log に入れる（`URGENT` は全画面でも出す、lock では出さない） |
| H5 | 続けて来た時 | 1 つずつ流し、待ちがある時はとどまる段を 1.5 秒に早送り |
| H6 | Linux・FreeBSD の D-Bus の通知（他の toolkit の app） | **決定（2026-10-05 ユーザー）**: Linux は libkeiland-backend で受ける（§11）。実装は後回し（p006）。FreeBSD は後 |
| H7 | bar の媒体の icon を消して通知に置き換える（媒体が残っている間の icon は無くなる） | 消す |

### 決定（2026-10-05 夜、ユーザー、Q1 経由）

H1〜H5 と H7 を案のとおり承認（H6 は既に決定済み）。

## 10. 段（ws.md の案の確定）

| Phase | 内容 | 依存 |
| --- | --- | --- |
| p002 | 口: protocol（`get_notify`・`kl_system_notify_v1`）、compositor の受け取り（`notify.c` の純粋な部分と `notify-shell.c`）、libkeiland の `kl_system_notify`・`kl_app_notify`、試験の client、host 試験 | H1〜H5 |
| p003 | popup の描画と動き、× と click、全画面・lock、system の通知（媒体の icon の置き換え、Wi-Fi、sleep、電池） | p002、H7 |
| p004 | log（Super+N、ring、すべて消去、log の ×） | p003 |
| p005 | 全文規約、T1 の QEMU、実機の UAT | p002〜p004 |

## 11. Linux の D-Bus の通知（設計の記録、2026-10-05 夕のユーザーの決定、P1 generation19 q772）

ユーザー（2026-10-05、原文）:「Linuxではlibkeiland-backendにD-bus機能を入れて通知を取ればいいですね。実装はあと回しでいいです。設計だけ記録してください。」
→ §9 の H6 は「Linux は受ける。場所は libkeiland-backend（別の daemon の `keiland-notify-dbus` ではない）」に決まった。実装は後（段 p006、下）。

### 11.1 形

- Linux の backend（`userland/desktop/libkeiland-backend-linux/`）に `notify-linux.c`（新）を足す。compositor が session の始めに開き、session bus の
  `org.freedesktop.Notifications` の名前を持って、GTK・Qt・libnotify などの app の通知を受け、compositor の通知の表（§2 の `kl_system_notify_v1` と同じ表、
  §6 の `zwl_notify_post` の道）に入れる。Keiland の app は今の設計どおり `kl_system_notify`（Wayland）を使い、D-Bus は通らない。
- zedBSD には D-Bus が無いので zedBSD の backend は `ENOTSUP`。FreeBSD の backend も v1 は `ENOTSUP`（FreeBSD の desktop にも D-Bus の session bus はあるので、
  後で同じ file を共有できる。OS に依る所は bus の address と認証の uid だけ）。

### 11.2 D-Bus の口（Desktop Notifications Specification 1.2）

| 種類 | 名前（signature） | Keiland での扱い |
| --- | --- | --- |
| method | `GetCapabilities() → as` | `["body", "actions", "persistence"]`（`persistence` = log に残る。`body-markup`・`icon-static`・`sound` は言わない） |
| method | `Notify(s app_name, u replaces_id, s app_icon, s summary, s body, as actions, a{sv} hints, i expire_timeout) → u id` | `app_name` → app、`summary` → title、`body` → body（markup を言わないので平文。来た `<b>` などの簡単な tag は外す）。`replaces_id` が自分の出した番号なら置き換え。`actions` に `"default"` があれば `ACTION`（本文の click で `ActionInvoked(id, "default")`）、他の action は v1 では無視。`hints` の `urgency`（byte、2 = critical）→ `URGENT`。`app_icon`・`image-data` などの画像と `expire_timeout` は v1 では無視（popup の動きは §3.2 で決まっている） |
| method | `CloseNotification(u id)` | §2 の `withdraw`。無い番号は空の答え（spec どおり） |
| method | `GetServerInformation() → (s name, s vendor, s version, s spec_version)` | `("Keiland", "zedBSD", <Keiland の版>, "1.2")` |
| signal | `NotificationClosed(u id, u reason)` | §2 の `closed` から: 1 = 期限（log からも消えた）、2 = 利用者が消した（× と「すべて消去」）、3 = `CloseNotification` |
| signal | `ActionInvoked(u id, s action_key)` | §2 の `activated` から、`"default"` |

- 番号: D-Bus の通知の番号は backend が 1 から振り（0 は使わない、2^32 で回る）、compositor の通知の番号との対応の表を backend が持つ。
- 文字列の上限は §2 と同じ（app 64・title 128・body 512 byte）。D-Bus の app は断られることを想定していないので、超えた分は**切って受ける**（UTF-8 の
  文字の境で）。1 つの送り手（D-Bus の unique name）の数の上限 32 も §2 と同じで、超えたら一番古い物を log から落とす（`NotificationClosed` reason 1）。
- 名前の取り合い: `RequestName("org.freedesktop.Notifications", DO_NOT_QUEUE)` が `EXISTS`（他の通知の daemon が持っている、例: GNOME の session の中）なら、
  奪わずに何もしない（compositor の log に 1 行）。Keiland の session（`keiland-linux-install-session`）では他が居ないはず。

### 11.3 backend の API（案）

```c
struct kl_backend_notify_callbacks {
	/* 受けた通知。compositor の通知の番号を返す（0 は断った） */
	uint32_t (*posted)(void *data, const char *app, const char *title, const char *body, uint32_t replaces, unsigned flags);
	void (*withdrawn)(void *data, uint32_t id);
};
int kl_backend_notify_open(struct kl_backend *backend, const struct kl_backend_notify_callbacks *callbacks, void *data); /* ENOTSUP: zedBSD・FreeBSD */
int kl_backend_notify_fd(const struct kl_backend *backend);            /* poll する bus の fd、無ければ -1 */
void kl_backend_notify_dispatch(struct kl_backend *backend);           /* 読めた時に呼ぶ */
void kl_backend_notify_closed(struct kl_backend *backend, uint32_t id, unsigned reason);   /* compositor → NotificationClosed */
void kl_backend_notify_activated(struct kl_backend *backend, uint32_t id);                 /* compositor → ActionInvoked */
void kl_backend_notify_close(struct kl_backend *backend);
```

`keiland-backend.h` に足す（Keiland の内部の API、kernel の UAPI ではない）。compositor は §2 の表の送り手の種類に「D-Bus」を足し、`closed`・`activated` を
Wayland の client の代わりに backend へ返す。

### 11.4 今の D-Bus の client に足す物

`dbus-linux.c` は今 system bus の client（logind、method の呼び出しと signal の受け取り）だけ。足す物:
- session bus への接続（`DBUS_SESSION_BUS_ADDRESS` の `unix:path=`・`unix:abstract=`、無ければ `$XDG_RUNTIME_DIR/bus`）、EXTERNAL の認証（自分の uid）、`Hello`。
- `RequestName`・`ReleaseName`。
- METHOD_CALL を受けて METHOD_RETURN・ERROR を返す（今は返事を受けるだけ）。`org.freedesktop.DBus.Introspectable.Introspect` と `Peer.Ping` にも答える。
- signal を出す。
- 型: `as` の読み書き、`a{sv}` の読み（`urgency` の byte だけを拾い、他の variant は型を見て読み飛ばす）、`(ssss)` の書き。今の bound（frame 64 KiB、fd 16）の中で、
  長さ・深さ・残りの byte を必ず確かめる（相手は同じ uid の任意の app）。

### 11.5 試験（実装の段で）

- host: `notify-linux.c` を、socketpair の向こうの小さな fake の bus（frame を手で組む）に対して: Notify・置き換え・CloseNotification・GetCapabilities・
  GetServerInformation・切って受ける・数の上限・`NotificationClosed`・`ActionInvoked`、壊れた frame（長さ・型の誤り）で落ちないこと。
- Linux の QEMU+KVM の guest（WS131 の Debian、AGENTS.md の例外）: Keiland の session で `notify-send "title" "body"`（libnotify）と `gdbus call` で通知が popup に
  流れ、log に入ること。SSH と QMP の PNG で確かめる（T1）。

### 11.6 段

| Phase | 内容 | 依存 |
| --- | --- | --- |
| p006（新、後回し） | Linux: libkeiland-backend の D-Bus の通知（`notify-linux.c`、session bus、§11.2〜§11.4）、compositor の送り手の種類、host 試験、Linux の guest の T1 | p002（通知の表と口）、時期はユーザーの「実装はあと回し」に従い Q1 が決める |

## 結果

（設計の第 1 版。判断 H1〜H7 待ち）
