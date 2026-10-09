<!-- awesome-plan project=zedbsd record=beta2-triage -->

# ベータ2 の残り作業とトリアージ（2026-10-09 Q1、2026-10-10 未明 更新）

公開は 10/17（OSC 当日、朝から会場）なので、**10/16 中にリリースの準備を終える**。作業日は 10/09〜10/16。
体制: P1（実装・debug）＋T1（QEMU の試験）＋ユーザー（5330 の UAT・判断）。この表は計画で、Queue の承認ではない。
見積もりは LW（Logical Week、このプロジェクトの単位で 1 LW ≈ エージェントの実時間 20 分。master の 2026-10-06 の見直しと同じ）。完了した項目は消す。

## 日程

| 日 | やること |
| --- | --- |
| 10/09〜10/12 | 試験待ちの物を T1 で流して直す。5330 の UAT（下の「実機」をまとめて） |
| **10/13** | **機能の凍結の目標**（2026-10-09 ユーザー）。release notes・既知の問題をまとめる。ベータなので UAT の Bug を直し切れなくてよく、code freeze はぎりぎりまで行わないこともある |
| 10/14〜10/15 | 最終回帰（QEMU）と 5330 の確認、出た Bug を「直す／既知の問題に書く」で仕分け |
| **10/16** | 最終の image・配布物・license の一覧・release notes を確定。WS143・WS083 は必須（2026-10-09 ユーザー）。間に合わなければここで OFF。公開の手順の確認（公開はユーザーの指示で） |

## 今の状況（2026-10-10 未明）

- **2026-10-10 の UAT で 6 件の Bug（BUG-275〜280）が出たので、P1 は WS197 を止めてこちらを先に直す。**
- P1 の実装・直しの必須は済んでいた。 残りの必須は全部「待ち」: T1 の再試験、ユーザーの review と UAT、5330 の復帰、日程の決まった作業（10/14 の最終回帰、10/16 の公開の準備）。
- その間の P1 は WS197（Bluetooth のスマホ連携、ベータ3）を 10/17 まで main に入れない別の branch で進める（ユーザー「beta2.mdの必須が終わってから」）。T1 の FAIL・UAT の Bug が来たら P1 はすぐそちらへ戻る。
- **ユーザーに頼みたい事**: 5330 の電源か network（10/09 夜から ping も SSH も届かない、T1-435 が待っている）、release の文書の review、UAT、PNG の確認、試験の機器の情報。

## UAT の結果（2026-10-10、5330、ユーザー）

| 項目 | 結果 | 次 |
| --- | --- | --- |
| 窓の dock の解除のダブルタップ | ✔ 遅れなし | — |
| ダブルタップからの title bar のドラッグ | ✔ | — |
| Settings の Wi-Fi の on/off | ✔ | — |
| 状態の島のパネル（WS192） | ✔ 開く。✘ Mute の文字が右にずれる | [BUG-278](bugs/BUG-278.md) 直した。T1-514 の PNG で Mute と「No notifications」が中央（build/review/t1-514/）、ユーザーの目視で close |
| BUG-253（蓋） | ✔ close | — |
| Settings の Bluetooth | ✘ device が無いと出る | [BUG-275](bugs/BUG-275.md) **直った（5330 で確認）**: 原因は USB の driver が Bluetooth の voice の endpoint（大きさ 0）を拒んでいた。新しい kernel で 8087:0033・/dev/bluetooth0・firmware の load・`STATE ready`・`POWER on`。ユーザーが Settings と HID の pairing を確かめて close |
| Terminal の選んだ文字のドラッグ | ✘ 選択の上でダブルタップ（tap からの drag）を始めると最初の tap で選択が外れる | [BUG-276](bugs/BUG-276.md) 直した（範囲の上の tap は 350 ms 待って消す、その間の press と移動で範囲の drag）。QEMU で tap を注入できないので次の UAT で確認。Text Editor 等は範囲の drag が無く同じ不具合は無い |
| Settings の Ethernet | ✘ 接続中に No Cable | [BUG-277](bugs/BUG-277.md) 直した（USB LAN の stop が carrier を 0 にしたまま）、T1-513 の QEMU で戻りに address が付く、次の UAT で実機 |
| Settings の YubiKey | ✔ 2026-10-10 FIDO2 の鍵の登録と login（BUG-279 close）。要望: 独立の頁とウィザード → WS199、Users の頁のパスワード変更と認証方式 → WS200（ベータ2）、ロック画面の button の高さ → BUG-283 | — |
| (旧) Settings の YubiKey | ✘ No security key registered で操作できない | [BUG-279](bugs/BUG-279.md) 見込み: 買ったままの鍵に PIN が無く Add が押せなかった。足りない物を表示（PIN が無ければ「run fidoctl set-pin in Terminal」）。次の UAT で確認。Settings の中で PIN を付けるのはベータ3 の候補 |
| menuconfig（WS193） | ✔ | WS193 p002 cleared |
| WS177 準正常系（USB-C・PIN・手書き・Notes） | ✔ | p002・p003・p006・p011 と U の p033〜p038 を cleared。残りは p019（Browser の IME・form、T1-425 の残り） |
| WS194 keiland-linux の package の確認 | ✔ | p002 cleared |
| BUG-189・BUG-212（有線と Wi-Fi） | ✔ close | — |
| USB LAN の速さ（BUG-222） | ✘ 遅いまま。ifconfig は 2500Mbps（link は正しい） | [BUG-222](bugs/BUG-222.md) ifconfig に `media: 2500Mbps` 等を出す直しと、CDC の通知の読みの直し（短い endpoint の device で link と速度の通知を落としていた）を merge、T1-515。Settings の Link speed は前から有る。5330 の値は次の UAT（`ifconfig ue0`・`dmesg | grep link`・Settings）。速さの調べは 5330 の復帰の後、直らなければ既知の問題 |
| App Home への遷移の滑らかさ | ✘ Linux の driver より fps が低く見える | [BUG-280](bugs/BUG-280.md) 調べた: compositor は Linux と同じ code、差は driver（1 frame が 16.7 ms を越えて 30 fps、GPU の周波数が上がりきらない、present の待ち）。**決定（ユーザー）: ベータ2 は既知の問題**、5330 の計測の後に WS139（ベータ3） |

## 必須

| 項目 | 状態 | 待っている物 | LW | 担当 |
| --- | --- | --- | --- | --- |
| [WS129](ws129/ws.md) p005 release notes と既知の問題 | 下書き済み（[notes](../docs/release/zedbsd-1.0.0-beta2.md)・[known issues](../docs/release/zedbsd-1.0.0-beta2-known-issues.md)） | ユーザーの review。RC で review の comment を消す | 0.5 | ユーザー・P1 |
| [WS129](ws129/ws.md) p013 利用の手引き | 下書き済み（[guide](../docs/release/zedbsd-1.0.0-beta2-guide.md)） | ユーザーの review | 0.5 | ユーザー・P1 |
| [WS129](ws129/ws.md) p006 最終回帰（release の image） | 未着手 | 10/14（RC の後） | 3 | T1 |
| [WS129](ws129/ws.md) p008 公開の準備（tag・CI・配布物） | 手順は用意済み（host の確かめ PASS） | 10/16、公開はユーザーの指示 | 0.5 | Q1・P1 |
| T1 の再試験の結果（2026-10-10）: 509 C7 PASS、510 画面 keyboard PASS、513 有線の戻り PASS、514 パネルの PNG。511 tcp-loss-speed FAIL（fetch が繋がらない）、512 Python（ベータ3）は 17/45 files 失敗、513 の tcp-receive-speed で fetch が 2 回に 1 回返らない | P1 の見立て: 試験の host 側は正しく、guest の USB LAN で SYN か SYN-ACK が落ちる・ue0 の TCP が約 2 分止まる（packet の pool の枯渇の見込み、未確認）。T1-516: 接続の失敗は再現せず転送は完走、ただし損失 2% で 0.58 MB/s（損失 0% の 6.19 の 1/4 未満）、受信の 6 回は全部 ok（約 10 MB/s、止まらず）。T1-515: ifconfig の media・Settings の Link speed（QEMU は Unknown）PASS、lan-hotplug が 2 回に 1 回 FAIL → P1: 回帰ではなく SSH の呼び出しが約 60 s 止まる前からの flake（[BUG-281](bugs/BUG-281.md)、試験は取り直す形に）。損失の回復: 並び替えの持ち数を 16 → 44（window の全部、64-bit）に直した（1 定数）。**T1-517 PASS**（損失 2% で 0.58 → 2.59 MB/s、0% も 6.19 → 8.42、hot-plug・受信も ok）。受信の SACK はベータ3 の候補（約 3 LW）。実機は 5330 の UAT | 2 | P1・T1 |
| 上の再試験で出る FAIL の直し | — | T1 の結果 | 2 | P1 |
| [WS083](ws083/ws.md) Vulkan Video（H.264） | host の作業は済み。release の config は OFF、T1-435 が PASS したら ON の 1 行 | **5330 の復帰**（T1-435） | 2 | T1・P1 |
| [WS143](ws143/ws.md) Bluetooth の HID | QEMU の回帰 PASS（T1-502）。UAT の確認表 B1〜B14 は [phase008](ws143/phase008/phase.md) | 5330 の UAT と機器の情報 | 3 | ユーザー・P1 |
| WS192（状態の島のパネル）の UAT の指摘 | WS193・WS194 は UAT OK、WS192 は BUG-278（Mute の文字） | P1 | 1 | P1 |
| 5330 の UAT（下の「UAT の確認項目」） | — | ユーザー | — | ユーザー |
| [WS199](ws199/ws.md) セキュリティキーの独立の頁とウィザード（2026-10-10 UAT の要望） | P1、BUG-283 の後 | — | 8 | P1・T1・ユーザー |
| [WS200](ws200/ws.md) Users の頁のパスワード変更のウィザードと認証方式の選択（同） | P1、WS199 の後 | — | 6 | P1・T1・ユーザー |
| [BUG-283](bugs/BUG-283.md) ロック画面の button の高さ | P1 | — | 1 | P1 |
| UAT で出る Bug の debug の枠（2026-10-10 の 6 件: BUG-275〜280） | P1 が着手 | — | 10 | P1 |
| T1-481 の needs-person の PNG 11 枚（build/review/bugsweep/）、WS192 のパネルの PNG（build/review/t1-496/） | — | ユーザー | —（15 分） | ユーザー |

## UAT の確認項目（ユーザー、5330 の release の image）

迷ったら PNG か一言を Q1 へ。✔ は動けば OK、✘ はその場の様子（何をしたら何が起きたか）を教えてください。

| # | 項目 | 手順（自明でない物だけ） | 期待 |
| --- | --- | --- | --- |
| 1 | [WS192](ws192/ws.md) 状態の島の操作パネル | 右上の icon の島を指で tap（mouse の click でも） | 右上に glass のパネル。Wi-Fi の switch、音量の slider と mute、Input の行の tap で言語の切り替え、Wi-Fi の「›」で AP の一覧。外の tap・Esc・島の再 tap で閉じる |
| 5 | [WS183](ws183/ws.md) touchpad の tap | 1 本指の tap、素早い 2 回の tap、tap の直後に指を置いて動かす | tap はすぐ click（遅れを感じない）、2 回は double click、最後は drag |
| 6 | [WS187](ws187/ws.md) ロック画面 | (a) 手動で Lock、(b) 蓋を閉じて開ける・放置で自動 lock（5 分以内） | 時計が中央より上に大きい。画面の下の方から上へ swipe（touchpad・touchscreen）か mouse の wheel を上で解除の画面。(a) は必ず認証、(b) は 5 分以内なら swipe だけで解除 |
| 7 | [WS161](ws161/ws.md)・WS172 YubiKey | YubiKey 5（USB）を挿し、ロック画面か login で Hardware Key を選び鍵に触れる。NFC は ACR1252U に YubiKey 5 NFC を置く | 解除・login できる。PIN・Password の選択も出る |
| 8 | [WS143](ws143/ws.md) Bluetooth のキーボード・マウス | Settings → Bluetooth で BR/EDR（従来型）と LE の機器をそれぞれ pairing、文字を打つ・pointer を動かす。その間 Wi-Fi も使う | 入力が効く。Wi-Fi が切れない。✘ なら 10/16 に OFF |
| 9 | [WS083](ws083/ws.md) Vulkan Video | ユーザーの UAT は無し（release の image の Video Player は FFmpeg の CPU の decode で、Vulkan Video を使う program は入っていない。2026-10-09 P1 の調べ）。実機の確かめは T1-435 と人工の hang F1・F2 | — |
| 13 | [WS090](ws090/ws.md) 描画の速さ | desktop で範囲選択の枠を drag、Text Editor・Files で scroll | もたつかない（体感で、遅い所を教えてください） |
| 17 | 写真の判定 | build/review/bugsweep/ の PNG 11 枚（T1-481 の needs-person） | 見た目が正しいかを OK／NG で |

## 合計

| 区分 | LW |
| --- | --- |
| 必須（WS143・WS083 を含む、残り） | 26 |
| **計** | **26 LW**（約 9 時間、大半は待ちの後の作業。P1 と T1 が並行するので 7 日の中に余裕がある。UAT の待ちは含まない） |

## 既知の問題に書いて出す（ベータ3 以降）

| 項目 | 理由 |
| --- | --- |
| BUG-280（App Home への遷移の fps）、BUG-217（最大化の session の状態）、BUG-223（動画の全画面）、BUG-205（太字の font） | 設計の変更が要る |
| BUG-255（蓋を閉じた間の HDMI）、BUG-159（電池で 5 fps）、BUG-145（AX211 の DHCP）、BUG-165（5330 の DSDT） | 調査が長い・実機の時間が要る |
| [WS001 p045](ws001/phase045/phase.md) POSIX の header の残り・p046〜p051、[WS126](ws126/ws.md) Python | ベータ3 の列（合間に P1） |
| 規約の全文の見直しの Phase（各 WS） | ユーザーの決定でベータ3 |

## 運用

- 凍結の目標の 10/13 の後も、ベータなので UAT の Bug の直しは 10/16 の準備に間に合う範囲で続ける。新しい仕様の変更は「ベータ3 に回すか」を Q1 がユーザーに聞く。
- Q1 は進むたびにこの表を更新し、完了した項目を消す（2026-10-09 ユーザー「都度、beta2.mdを更新していただけると、進捗がわかって助かります」）。
