<!-- awesome-plan project=zedbsd record=ws170-p000 -->

# ws170-p000: Phone の app の UI の mock

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-177b の撮影の後、p002〜p004 で実装・cleared）（旧: in-progress（実装・host の PNG・build は済み。QEMU は T1 待ち、夕方の UAT の image に入れるかは Q1））
Disposition: normal
Parent: [WS170](../ws.md)
Queue: q743（2026-10-05、P2）
依存: なし

## 範囲（Q1、ユーザーの優先）

Phone の app の外側だけ。連絡先の一覧 → 連絡先ごとの iOS のメッセージ風のタイムライン（吹き出し・通話の記録・添付）、送信の欄と電話の button（押すと「backend が無い」）。data は app の中の固定の試験 data。保存・API・backend は作らない。Keiland の既存の app と同じ作りで、image の config に入れて App Home から起動できるように。

## 実装（2026-10-05、P2）

- `userland/desktop/phone/`（新規、package `phone`、既定では image に入らない `n`）:
  - `phone.h`: 試験 data の型（`ph_contact`・`ph_item`、種類 text・call・photo・file、channel SMS・MMS・RCS・回線・VoIP）と view の API。
  - `data.c`: 8 人の連絡先（日本語の名前の 1 人、短い番号の 1 件、何も無い 1 人）と、各種の item（RCS・SMS・MMS の送受信、回線と VoIP の通話の着信・発信・不在・応答なし、写真・PDF）。
  - `view.c`: libkeiland の canvas と widget で描く。左に「Phone」の題・検索の欄（名前と番号を ASCII の大小を区別せずに絞る）・連絡先の行（頭文字の丸・名前・最新の item の時刻と 1 行・未読の点）。右に header（名前・番号・最新のメッセージの channel・電話の button）、タイムライン（日の変わり目に「channel · 日 時刻」、相手の灰色の吹き出しは左、自分の吹き出しは右で RCS は青・SMS/MMS は緑、自分の吹き出しの下に「Read 09:33」などの状態、通話の card（着信・発信は緑、不在は赤、応答なしは灰）、写真は描いた風景で代用、file は file の絵の card）、下に添付の「+」・メッセージの欄（placeholder は「RCS Message」か「Text Message」、Enter で送信）・送信の button。幅 680 未満では一覧かタイムラインの一方を出し、タイムラインに戻る button。キー: 上下で連絡先、Enter で開く、Esc で一覧へ。
  - 送信（文字があるとき）・電話・添付は、下に「No phone backend: … cannot be … yet.」の chip を 4 秒出し、`PHONE NOBACKEND action=send|call|attach` を log に書く。
  - `main.c`: videoplayer と同じ kl_app の loop（menu: File の Quit Phone（Ctrl+Q）、Conversation の Call・Send Message・Attach File...）。
- `platform/amd64/vmunix.mk`: `bin/phone` の link の規則（libvulkan・libwayland-client・libkeiland・libtruetype・libc）。
- `userland/desktop/wayland/apps.conf`: App Home に「Phone」（`/bin/phone`、無い image では出ない）。
- 試験の image の config: `plan/ws170/tests/config-amd64-phone.mk`（CI の image ＋ phone）。release・CI の config に足すかは Q1。

## glass（2026-10-05、ユーザーの再指示「ちょっとAppleそのままという感じもするので、デスクトップ透過を有効にして、スクリーン全体のスクショが見てみたいです。」、Q1 経由）

- `main.c`: 最初の frame で `kl_window_see_through` を見て、透ける swapchain なら glass にする（`PHONE GLASS see_through=1`）。毎 frame `ph_view_panels` の panel を `kl_window_set_glass` で送る（glass の無い compositor では不透明に戻す）。
- `view.c`: glass では地を透明にし、連絡先と timeline を Settings と同じ浮いた card（余白 12・間 10・角 16、`glass_sidebar`・`glass_content`）にする。header と入力欄の地は無し（区切りは内側の細い線）。
- Apple そのままに見えないように: 選んだ行は Files と同じ selection の色（青の塗り＋白の字をやめた）、相手の吹き出しは glass では半透明の白、SMS の自分の吹き出しは緑をやめて teal（`0x0f9d8a`）、吹き出しの角を 14 にして送り手の側の下の角をほぼ直角（4）に、通話の card も glass では半透明。
- host: `run-host-phone.sh` に glass の 2 枚（`host-phone-glass.png`・`host-phone-glass-ben.png`。壁紙をぼかして明るくした地に、frame を alpha で重ねた近似）を足した。PASS 10。

## 確かめ

- host: `sh plan/ws170/tests/run-host-phone.sh` → PASS 10（glass-ben を含む。start・select-kenji・select-ben・send（`NOBACKEND action=send contact=1 length=5`）・call・search・narrow-open・narrow-back・key-down）。PNG は `build/ws170/host-phone-{start,kenji,ben,send,call,search,narrow-list,narrow-timeline,narrow-back}.png`（目で見て確かめた）。
- build: `make ZEDBSD_CONFIG=plan/ws170/tests/config-amd64-phone.mk BUILD=build/ws170-zed build/ws170-zed/bin/phone` が warning 0（`-Os -Wall -Wextra -Werror`）。host の gcc も `-Wall -Wextra -Werror` で 0。
- style-check: 新しい file は違反 0。
- QEMU（T1 に依頼する）: App Home から Phone を起動し、一覧とタイムラインの PNG、送信と電話の chip の PNG。
- 未実施: QEMU、実機、Linux・FreeBSD の Keiland の build（mock の範囲では足していない）、日本語の入力（kl_field は ASCII の key だけ）。
