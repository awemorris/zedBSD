<!-- awesome-plan project=zedbsd record=beta2-triage -->

# ベータ2 の残り作業（2026-10-10 Q1 更新）

公開は 10/17（OSC 当日、朝から会場）なので、**10/16 中にリリースの準備を終える**。
体制: P1（実装・debug）＋T1（QEMU と 5330 の試験）＋ユーザー（UAT・判断）。この表は計画で、Queue の承認ではない。
見積もりは LW（1 LW ≈ エージェントの実時間 20 分）。完了した項目は消す。Q1 は進むたびに更新する。

## 日程

| 日 | やること |
| --- | --- |
| 10/10〜10/12 | 走っている Bug の直しと試験を終える → WS199（セキュリティキー）→ WS200 |
| **10/13** | 機能の凍結の目標。ベータなので UAT の Bug は直し切れなくてよく、code freeze はぎりぎりまで行わないこともある |
| 10/14〜10/15 | 最終回帰（QEMU）と 5330 の UAT、出た Bug を「直す／既知の問題に書く」で仕分け |
| **10/16** | 最終の image・配布物・license の一覧・release notes を確定。Vulkan Video は既定で ON（2026-10-11 ユーザー）。公開の手順の確認（公開はユーザーの指示で） |

## 今の状況

- 2026-10-11: [BUG-287](bugs/BUG-287.md)（スマホ連携が「connecting」のまま）の原因は USB の Transaction Error の後に Bluetooth の受けが止まること。P1 の直し（usb-bt の回復、同じ人の bond での phone=1、settings の store 24→64）を main に merge（b91d5675c、kernel の build warning 0）。Settings の Connect を phone に出さない・失敗の案内・Phone app の案内・phone link の log も merge（7bfc667e7）。BUG-287 の直しは全部 main に入った。**image を作り直してよい**。
- WS199・WS200 は実装と QEMU の試験が済み、残りは次の UAT（#4〜#8）。
- WS083（Vulkan Video）は既定で ON にした（2026-10-11 ユーザー）。残りは p007 の hang の実機の確かめ。libavcodec なしで libmedia だけで mp4（H.264・AAC）と .m4a を再生する [WS202](ws202/ws.md) は設計だけを書き、実装は別のセッション。
- T1 の WS143 の HID の回帰はもう流さない（ユーザー）。P2 は WS083 を終えて退いた。
- 残る既知の Bug は USB LAN の遅さ（BUG-222、4.9 MB/s）。

## 必須

| 項目 | 状態 | LW | 担当 |
| --- | --- | --- | --- |
| [WS083](ws083/ws.md) Vulkan Video（H.264） | p001〜p006・p008 cleared（実機で全 stream 一致、1080p 相当 1 frame 約 5 ms）。2026-10-11 から既定で ON（ユーザー）。残り: p007 の hang の回復の実機の確かめ（hang の kernel を 1 回置く、手順は phase007、ユーザーの判断） | 0.5 | ユーザー・Q1 |
| [WS202](ws202/ws.md) 動画再生） | libmediaにH.264, AACを実装、動画app, 音楽appで利用。） | 4.0 | ユーザー |
| [WS197](ws197/ws.md) Bluetooth のスマホ連携（SMS の MAP・通話の HFP・連絡先の PBAP）（2026-10-10 ユーザー「WS197はbeta2.mdで必須に入れておいてください。」） | p001・p002 cleared、p003 MAP は i01〜i07 実装（i06 まで main に merge、i07 は T1-527）、p004 の SMS の interface の設計は cleared（判断 P1〜P8 は推しどおり）。p004a〜c（SMS を Phone の app で）を 2026-10-10 夜に main に merge（bc4c7f9f6、host 試験 PASS、実機は UAT）。p005 PBAP は設計の第 1 版と review-1（blocker 1）、ユーザーの判断 Pc1〜Pc6 待ち、i01（vCard の読み）は先に始めてよい（55e471604、backend の欄の読みの欠陥の直しも merge） → p006・p007 HFP → p008 実機 | 約 64 | P1 |
| [WS129](ws129/ws.md) p005・p013 release notes・既知の問題・利用の手引き | 下書き済み（[notes](../docs/release/zedbsd-1.0.0-beta2.md)・[known issues](../docs/release/zedbsd-1.0.0-beta2-known-issues.md)・[guide](../docs/release/zedbsd-1.0.0-beta2-guide.md)）。**ユーザーの review 待ち**。WS199・WS200 の機能を足し、RC で review の comment を消す | 1.5 | ユーザー・P1 |
| [WS129](ws129/ws.md) p006 最終回帰（release の image） | 10/14 | 3 | T1 |
| [WS129](ws129/ws.md) p008 公開の準備（tag・CI・配布物） | 手順は用意済み。10/16、公開はユーザーの指示 | 0.5 | Q1・P1 |
| 次の UAT で出る Bug の枠 | — | 5 | P1 |
| **計** | | **約 88 LW**（約 29 時間、うち WS197 が約 75） | |

## 次の UAT で確認してほしい事項（5330、WS199・WS200 の後の image）

| # | 項目 | 手順 | 期待 |
| --- | --- | --- | --- |
| 11 | [WS197](ws197/ws.md) スマホの SMS（Android） | 手順は ws197/ws.md の「5330 の UAT の手順」: Settings → Bluetooth でスマホを pairing して「Use as phone」→ Phone の app で受信・送信・同期、app を閉じている間の通知 | SMS が送受信でき、履歴が同期される |

## 既知の問題に書いて出す（ベータ3 以降）

| 項目 | 理由 |
| --- | --- |
| BUG-255（蓋を閉じた間の HDMI）、BUG-159（電池で 5 fps）、BUG-145（AX211 の DHCP）、BUG-165（5330 の DSDT） | 調査が長い・実機の時間が要る |
| WS201（/home の暗号化）、WS195（/opt/keiland）、WS196（useradd 等）、WS198（self-build）、[WS001 p045](ws001/phase045/phase.md) POSIX の header、[WS126](ws126/ws.md) Python | ベータ3 の列 |
| 規約の全文の見直しの Phase（各 WS） | ユーザーの決定でベータ3 |

## 運用

- 凍結の目標の 10/13 の後も、ベータなので UAT の Bug の直しは 10/16 の準備に間に合う範囲で続ける。新しい仕様の変更は「ベータ3 に回すか」を Q1 がユーザーに聞く。
- Q1 は進むたびにこの表を更新し、完了した項目を消す（2026-10-09 ユーザー「都度、beta2.mdを更新していただけると、進捗がわかって助かります」）。
