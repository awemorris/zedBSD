<!-- awesome-plan project=zedbsd record=ws197 -->

# WS197: Bluetooth のスマホ連携（SMS の MAP、通話の HFP、連絡先の PBAP）

<!-- awesome-plan-current:start -->
Status: incomplete（p005 cleared、HFP・実機PBAP UAT・WS最終規約は未完）
Primary Milestone: MG006
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Queue: [Codex承認済み実行記録](codex-queue.md)、media-uat-20261010 finished。共有QueueはQ1が反映。
Target: **ベータ2**（2026-10-10 ユーザー「WS197はbeta2.mdで必須に入れておいてください。」）
Resume point: p005/p011/p012 cleared。媒体UAT修正をmain e654733f1へ統合、Phone起動SIGSEGVは実機修正版exit0。添付操作GUI UATは次回起動。p010受信のMIME相互運用修正はhost/buildとSSHでの写真保存を確認、ユーザーがテキスト/写真MMSの受信・表示を確認。受信部分Queue finished、source 89d487814。送信は未完。HFP・実機PBAP UAT・WS最終規約は未完。
<!-- awesome-plan-current:end -->

## 由来（2026-10-09 ユーザー）

「Bluetoothは、SMSのMAP, CallsのHFP, PBAP, を実装したいですが、見積もり規模はどうでしょうか。今はANCSは実装しなくていいですが、見積もりだけでも。」
「OBEX, MAP, Integration, PBAP, HFPの順で実装しますか。beta2.mdの必須が終わってからです。」

参考（Q1 の説明、2026-10-09）: Microsoft の Phone Link は iPhone では Bluetooth の HFP・MAP・PBAP・ANCS の標準だけを使い、Android では独自の app と IP（通話の音は HFP）。

## 以前の再開の手順（2026-10-10 Q1、履歴。現状は上のResume point）

- **code と詳細の記録は保留の branch `agent/p1-ws197`（head 2bf274a38）にある。** main には無い（10/17 の公開まで release の bluetoothd を変えないため、Q1 の判断）。p002・p003 の phase.md と review は 2026-10-10 に branch の f06bf4dbb の物を main にも写した（ユーザーの依頼）。branch で更新したら main へも写す（branch の main の merge で揃う）。試験（plan/ws197/tests/）は branch の code が要るので branch にだけある。
- 再開の時: (1) branch に main を merge する（`git switch agent/p1-ws197 && git merge main`。WS143・WS199 の bluetoothd・passkey の変更と衝突しうるので WS143 の host 試験 `plan/ws143/tests/bt-daemon-host-test.sh` と `plan/ws197/tests/bt-phone-host-test.sh` を流す）。(2) branch の plan/ws197/phase003/phase.md の「進み」の最後の行の**再開点**から続ける。(3) 区切りごとに main へ merge（ベータ2 に入れる、2026-10-10 ユーザー）。merge の前の T1 の HID の回帰はやめた（2026-10-10 ユーザー「流しすぎです。もう不要」）、host 試験と build で merge し実機の UAT で確かめる。
- 各 commit で: WS143 と WS197 の host 試験、target の bluetoothd の build warning 0。QEMU の HID の回帰（WS143 の 4 本）は流さない（2026-10-10 ユーザー）。

## 目標

- zedBSD の bluetoothd（WS143）にスマホ連携の profile を足す: MAP の MCE（SMS の一覧・受信の通知・送信）、PBAP の PCE（連絡先）、HFP の Hands-Free（通話の制御と通話の音）。
- [WS170](../ws170/ws.md) Phone の app の backend（compositor の API）につなぎ、Settings でスマホの pairing と許可を扱う。
- 制約: iPhone の MAP は読む・通知だけで送信できない。Android の MAP の送信は機種による。IP の経路（WS170）は別。ANCS は今は作らない（見積もり +8 LW）。

## Phase（2026-10-10 Q1、計 約 129 LW）

| Phase | 内容 | LW | Status |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計（profile の役割、bluetoothd の構造、WS170 の API、試験）、review 3 回、ユーザーの決定 Q1〜Q16 | 4 | cleared |
| p002 | RFCOMM・OBEX・SDP の server と client・phone.c・phone の pairing・linkmgr・drop の回復・WS143 の変更（i01〜i08）。詳細は branch の phase002/phase.md | 22 | **cleared**（host 約 2,500 checks、T1-518 HID の回帰 PASS、2026-10-10 Q1） |
| p003 | MAP（MCE の MAS と MNS）。2026-10-10: i01〜i07 実装済み、c56043c2b（i06）まで main に merge（T1-526 PASS）、i07＋owner の純粋な関数 5c6219e7a も T1-527 PASS（p004 の 1 回目の Just Works の CONSENT は前からの flake、T1-502 と同じ）で main に merge。**p003 は cleared**（実機の MAP は p008）。、phone link の持ち主・記録・再接続、socket の PHONE。詳細設計 第 2.1 版（review 2 回）、i01〜i08。詳細は branch の phase003/phase.md | 24 | cleared（main統合済み、実機UATはp008） |
| [p004](phase004/phase.md) | SMS の層の interface の設計（Phone app・libkeiland・compositor・libkeiland-backend・bluetoothd、v3.1、review 3 回）。ユーザーの決定 P1〜P8 | — | cleared（設計、2026-10-10） |
| p004a | libkeiland-backend の phone-zedbsd.c（bluetoothd の socket）・compositor の phone-shell.c の bluetooth の backend・libkeiland の追加（phase004 §3〜§5・§11.2〜§11.4） | 6.5 | cleared 候補（2026-10-10、main に merge 4faf17473、KL_VERSION 79・manager 27、host 試験 PASS。実機は下の UAT。詳細は [phase004](phase004/phase.md) の「実装の進み」） |
| p004b | Phone の app の保存と同期（目印・merge・E.164 の key・送信の状態） | 3.5 | cleared 候補（2026-10-10、main に merge 778c1e377、host 試験 PASS。region の設定が無いので国番号は既定 81（P8）） |
| p004c | Settings の「Use as phone」と通知（WS156 の lock_text の変更は P1 が同じ Phase で行う、Q1） | 1.5 | cleared 候補（2026-10-10 P1、e7a478d33、host 試験 PASS、merge 待ち。実機は下の UAT） |
| [p005](phase005/phase.md) | PBAP・Phoneの電話帳/名前/通話履歴・直列同期・Settings。第3.1版i01〜i07 | 11.5 | cleared（2026-10-10 Codex、host・全文規約・named build warning0。実機PBAPはp008） |
| p006 | HFP の制御（AT の SLC、indicator、応答・終話・発信、発信者、割り込み、codec の交渉） | 12 | planned |
| p007 | HFP の音: p007a xHCI の isochronous・usb-bt の interface 1・SCO の口、p007b SCO・audiod・CVSD の後に mSBC（Q9 の SCO の UAPI は p007a の設計の後にユーザーに聞く） | 25 | planned |
| p008 | 実機（Android が先、iPhone は HFP の後、Q13）。PHONE PROBE は p003 で消すので MAP の操作で確かめる | 20 | planned |
| p009 | 規約の全文の見直し | 2 | planned |
| [p010](phase010/phase.md) | MMS写真/動画送受信（受信統合済み、送信は未完。p012がviewer起動を補完） | — | in-progress |
| [ws197-p011](phase011/phase.md) | Phoneの＋によるメディア選択とDnD・仮添付（ws157-p006 API出力に依存） | — | cleared |
| [ws197-p012](phase012/phase.md) | 添付のdouble-click起動・draftサムネイル・captionなし写真の起動SIGSEGV修正 | — | cleared |

関連の Bug: [BUG-282](../bugs/BUG-282.md)（WS143 の hid.c の page の途中の Connection Request の取り違え、p003 i03 と同じ形で直す）。Future Work: fw-bt-goep2（ERTM・GOEP 2.0、MAP 1.4・PBAP 1.2）、F-086（SUBSCRIBE の phone の分は p003）。

## 2026-10-10 Q1: p002 の判定

p002（RFCOMM・OBEX・SDP・phone.c と WS143 の変更、i01〜i08）は保留の branch agent/p1-ws197 の 92157604f で実装済み。host 試験は全部 PASS、T1-518（WS143 の HID の回帰 4 本、bt-loopback-p002・bt-daemon-p003・bt-pair-p004・bt-hid-p005）が全部 PASS。**p002 は cleared**（code は 10/17 の後に main へ merge）。branch の phase002/phase.md への反映は P1。

## 2026-10-10 T1-524（HID の回帰、883abd4b8）

bt-loopback-p002・bt-daemon-p003（SHOW・BONDS）・bt-pair-p004 は PASS。**bt-hid-p005 が 2 回とも FAIL**: 自動の接続の後に HOG の mouse が open にならない（state=waiting）、keyboard の EVDEV の node が出ない、controller が去った時の KEY_B の release、controller が戻った時の HOG の mouse。→ WS197 i01〜i03 の WS143 の変更（linkmgr の page の枠・session・router）による回帰の見込み。**main への merge は止める**。P1 が直す。

## 5330 の UAT の手順（p004a〜c、スマホの SMS、2026-10-10 P1）

ユーザー「実機でテストするので詳細なQEMUテストは不要です」（2026-10-10）。p004a〜c は host 試験と build だけで、動きは Latitude 5330 の実機で確かめる。Android のスマホ 1 台（Q13: Android が先）。iPhone の MAP は送信ができない（読む・通知だけ）。

前提: main に merge した後の image（WS197 の bluetoothd と desktop を含む）を 5330 に入れ、ユーザーで sign in。スマホの Bluetooth を on、SMS を送ってくれる別の電話を用意。

1. **pairing と Use as phone**: Settings → Bluetooth を開く（Bluetooth を on）。
   - まだ pair していないスマホ: Other Devices にスマホ（種類 Phone）が出る。**Use as phone** を押す → PAIR phone=1 の pairing（両方に同じ数字が出たら両方で確認）→ スマホが「メッセージへのアクセス」を聞いたら**許可**。
   - pair 済みのスマホ: My Devices のスマホの行の **Use as phone** を押す。
   - 期待: 行の下の文が「Messages connecting...」→「Messages connected」（1 分ほど）、page の下に「The phone is used for messages.」。desktop の設定 phone.backend が 2。許可しなかった時は「Allow access to messages on the phone」。
2. **最初の同期**: Phone の app を開く → 過去 30 日（folder ごとに最大 500 通）の SMS が番号（または相手の名前）の会話で出る。500 通を越えた時は「Some older messages were not brought in.」が 1 回。同期の item では通知が出ない。
3. **受信（app が開いている時）**: 別の電話から SMS を送る → 数秒で会話に入り、app の通知「Message from <名前>」。その会話を開いていれば既読になり、スマホの側でも既読になる。
4. **受信（app が閉じている時）**: Phone の app を閉じて SMS を送る → desktop の通知（Phone、題は相手の名前か番号、本文は 1 行目）。通知の本文を押すと Phone の app がその会話で開く（`phone --peer <番号>`）。開いた時の同期でその SMS が入る（重複しない）。
5. **lock の画面**: 画面を lock して SMS を送る → lock の画面に「New message from <名前>」だけの板が出る（本文は出ない）。app が開いている時と閉じている時の両方。
6. **送信**: 会話で文を書いて送る → 「Sending」→「Sent」→（スマホが報告すれば）「Delivered」。相手の電話に届く。app を閉じて開き直しても 1 通のまま（スマホの送信済みの写しと重なる、`s<key>.txt` に名前が変わる）。スマホが送信を受け付けない機種では送信の button が灰色で「The phone does not take texts to send.」。
7. **離れて戻る**: スマホの Bluetooth を off にする（またはスマホを離す）→ Settings の行が「Used as phone (not connected)」などに。その間に届いた SMS は、on に戻した後の同期で入る。
8. **送信の途中で切れる**: 送信の直後にスマホの Bluetooth を off → その 1 通は「Sending」のまま（unknown）。1 時間後に同期を 1 回してもスマホの写しと重ならなければ「Not delivered」。
9. **やめる**: Settings の **Stop using as phone** → 「The phone is no longer used.」、phone.backend が 0。Phone の app には新しい SMS が来ない。

集める物: 失敗した時は、compositor の log の `KWL PHONE` の行、Phone の app の stderr の `PHONE` の行（番号・本文は出ない、長さだけ）、bluetoothd の log、`~/Documents/Phone/messages/` と `~/Documents/Phone/sync/bt-<address>.state` の中身、Settings の画面の写真。

限界（記録）: 番号の国番号の既定は 81（Settings に地域の設定が無い、P8）。app が閉じている間の SMS は保存されず、次の起動の同期で入る（P1、Q2）。

## 2026-10-10 Codex 引き継ぎ

ユーザーが p005 完了と新規受信SMSの修正を指定。p005はin-progress、Pc1〜Pc6は推奨どおり確定。main `c5af76952` の i01〜i05 を前提として [実行記録](codex-queue.md) の i06/i07 と診断を実施。master は Q1 が反映。

## 2026-10-10 新規受信の診断とMMSテキスト承認

実機 beta2+ge8adcdf の17:41 JSTのMNS通知は `type=1 message-type=3 handle-present=1`（NewMessage / MMS）。通知自体は到達し、MAPのSMS限定条件とlisting filterで除外されていた。本文・番号・handleは記録しない。ユーザー回答「MMSのテキスト受信も含める」により、SMS受信修正へMMSのテキスト本文の抽出・履歴取り込み・通知・保存・表示を追加する。添付画像・動画は対象外、MIME生データを本文として表示しない。MIME parserは独自Zlib実装、RFC2045/2046のtransfer encodingとmultipartを参照。共有master/queueへの投影はQ1に保留。

## 2026-10-10 Codex 完了報告

p005 i06/i07を実装し、host回帰・最終変更source全文規約・zedBSD/Linux named buildを検証、p005をclearedとする（詳細/コマンド/限界は[p005結果](phase005/phase.md)）。新規受信はスマホがMMSとして通知していたためSMSフィルタで落ちていた。ユーザー承認のMMSテキスト抽出・履歴/通知/保存/表示を追加し、さらにCRLFを正規化、Phoneの本文と一覧previewでは改行コードを描画しない。修正版bluetoothdを実機へ更新後、ユーザーが「ゴミは消え、日本語も受信できました」と確認。p004のMMSテキスト追加と受信修正はcleared、添付画像/動画は対象外。WS全体はHFP・実機PBAP UAT・最終WS conformanceが残るためincomplete。master/共有Queueの投影とWS199のcompleted整理はQ1に引き継ぐ。

## main統合確認（2026-10-10）

実装/検証/完了記録のsource commit `6bca6f2a2`をmainへmerge `bee41dded`で統合した。merge前のmain `5e178dd17`のbeta2.md更新を保持。master/共有Queueは未変更、push/公開は行わない。変更sourceは検証したprivate treeと同一。Q1向けの残り投影/整理は既述どおり。

## 2026-10-10 写真がファイル名になる現象の解析（実装範囲の追加は未承認）

- ユーザー報告: 写真を受信すると画像ではなくファイル名になる。今回の依頼は解析。前回のMMSテキスト受信の受け入れを取り消す条件ではなく、画像添付は既存承認の対象外。
- codeで確認: `map.c:map_run_get`はGetMessageで`Attachment=0`を送るため画像本体を要求しない。`mms.c:mms_part`は明示的attachmentとimage/*を読み飛ばし、text/plainだけを取り出す。MIMEヘッダーのfilename/nameを本文に写すコードは無い。従ってファイル名表示はスマホが返した代替text/plainである可能性が高いが、当該bMessageの構造は未取得なので断定しない。
- `view.c:view_photo`は現状、図形でサンプル画像を描く仮表示。MMS画像の取得・保存・画像decode/表示へつながっていない。既存MAP/bMessageの全体上限65536byteも写真対応時には見直しが要る。
- 必要な拡張: 添付を含むMAP取得、MIMEの画像partとテキストの対応づけ、bluetoothd→compositor→libkeiland→Phoneの添付中継、Phone所有の画像保存と既存decoderによる描画、写真を扱う受信上限/メモリの設計。新しいQueueの範囲として画像対応を承認する前に、許容形式/上限を具体化する。今回sourceと実機daemonは変更していない。

## 2026-10-10 メディア管理の先行実装

ユーザーの新規承認により、PhotosのDB処理をCLI所有にしcompositor APIから使う。Phoneの＋とDnDは同じAPIを使う。既存Photos p004/p005のcleared履歴は維持、新p006/p007とws197-p011で変更後を検証する。MMS写真/動画送受信は基盤完成後に接続。旧途中treeは未統合。共有master/Queueへの投影はQ1に保留。

2026-10-10構造更新: ws157-p006はメディアCLI/API/Photos、ws157-p007はその最終全文規約、ws197-p011はPhone選択/DnDと自身の全文規約。p011はp006の検証済みAPI出力に依存する。全scopeはユーザーのメディア管理先行承認を維持。

## 2026-10-10 保存形式の変更承認

ユーザー指定を優先し、従来の `~/Pictures/Library` 月別TSV保存を現scopeで置き換える。`~/Pictures/Media/metadata.db` はversion付きJSON、原本copyは `Media/Files/YYYY/MM/dd/名前`。JPEG EXIF撮影日時がなければPNG/JPEG/動画等は取り込み日で整理する（元ファイルmtimeは使わない）。日付・バイト数・画像寸法・hash・原名・favorite・rotation・albumを保持し、未知のJSON fieldを更新時にも保存することで撮影地等へ拡張可能にする。既存Libraryの自動移動/削除はしない。旧ファイルはそのまま、必要な原本はmediastorage addで再取り込みできる。p006設計・p007検証・p011の選択元へ同じ承認を反映。以前のcleared履歴は旧形式の履歴として維持する。

## 2026-10-10 scoped clearance

ws197-p011 cleared、前提のws157-p006/p007もcleared。main `874e12d3b`、[最終検証](../ws157/tests/verification-20261010.md)。写真/動画の未送信draftとtext appendを実装、MMS送受信接続の完了とはしない。p010はuncleared、旧試作treeを維持。HFP/PBAP UAT/最終WS conformanceが残るためWSはincomplete。共有記録の投影はQ1に保留。

## 2026-10-10 受信メディアのcheckpoint

p010受信部分でAttachment=1とMIME/FD→mediastorage保存→Phone実画像表示/再openを実装し、host/build/変更source全文規約/OS境界を確認。[証拠](tests/media-receive-verification-20261010.md)。承認済みの実機4ファイル交換・desktop restartは新sessionの起動まで確認したが、Bluetooth再接続が停止し、その再restart後はSSH timeout。ユーザーへ画面応答と必要なら実機restartを依頼。接続回復後に最終Phone更新と新写真受信を確認する。WSはincomplete、p010全体の送信/動画player起動、HFP/PBAP UAT/p009を保持。共有master/Queue投影はQ1担当に保留。

## 2026-10-10 再作成後のMIME回帰の修正

p010の[部分Queue i02](codex-queue.md)で、スマホがleaf/WAP Content-Typeにboundaryを付けるMMSを正しくpart解析するよう修正。本文header露出、画像のENODATA/EOPNOTSUPPは同じroot分類の問題。host codec/実Phone→CLI原本保存/decode/再open、3target build warning0、変更source全文規約を通過し、実機の3実行ファイル更新・Bluetooth/desktop restart後に履歴画像保存を確認。新規テキスト/写真MMSの通知・画面表示はユーザー確認待ち。WSはincomplete、p010全体未完とQ1への共有投影保留を保持。詳細は[検証記録](tests/media-receive-verification-20261010.md)。

その後ユーザーがテキスト/写真MMSの「受信し、表示されました。」を確認。SSHでも通知・原本保存を確認し、受信部分check cleared/Queue finished、source `89d487814`。p010全体は未完、WS incomplete、共有投影Q1保留を保持。

main統合checkpoint: `035d1d25b` (WIP) fast-forward済み、c43a01797の既存変更を保持。hostで確認した受信経路をmainへ反映、pushなし。SSH/UAT待ちを解消した記録とはしない。

2026-10-10再開条件更新: ユーザーがPhone起動の遅れを報告し、実機イメージを再作成する。トップレベルconfig.mkとMakeの依存展開/AMD64 image file listを確認、必要5componentと画像libraryは含まれる。mediastorageの明示選択もgitignore対象config.mkに追加。他設定は保持。実機再作成後の起動時間・MAP再接続・写真保存/表示を確認する（現時点で原因や実機成功を断定しない）。

## 2026-10-10 メディアUAT修正の追加

ユーザーの5項目とPhone起動待ちの追加報告を有限[Queue](codex-queue.md)へ記録。p012を追加し、写真/動画を各viewerへ開く操作、draft写真のサムネイル、再読込したcaptionなしmediaのNULL参照を修正する。関連の[ws127-p013](../ws127/phase013/phase.md)、[ws157-p008/p009](../ws157/ws.md)と並行して本セッションが実装・検証・main統合する。p010の送信、HFP/PBAP、WS全体p009の義務は維持。共有master/Queue/cacheへの投影はQ1。

## 2026-10-10 UAT修正の統合とscoped clearance

ws197-p012 cleared。source `e654733f1eb6e1296eacf34b5dff6002a5dd17e1` をmainへ統合しclean HEADを確認。[証拠](../ws157/tests/media-uat-verification-20261010.md)。Phone起動NULL参照修正、提供JPEGの表示、短いhost/全文規約/buildが今回の有限条件を満たす。今回全変更の最終確認はws157-p009もcleared。実機GUI UATと各WS全体の既存未完条件は維持、WS statusはincompleteのまま。共有master/Queue/cacheの投影はQ1、GitHub未公開。
