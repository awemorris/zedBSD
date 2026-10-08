<!-- awesome-plan project=zedbsd record=ws177 -->

# WS177: ベータ2 積み残し（準正常系・異常系）

Status: incomplete（2026-10-08 q902 P1 の照合: 19 Phase のうち 12 cleared、p016〜p018 は Q1 の判定待ち、p002・p003・p006・p011 は実機・UAT、p019 の残りはベータ3。旧: planning（2026-10-06 Q1 が作成））
Master: [master](../master.md)
Primary Milestone: MG006（関係: MG002・MG003・MG005）
段: ベータ2

## 目的

2026-10-06 夜 ユーザー:「ベータ1のWS＋ベータ2の一部実装をまずすべて実装してください。…正常系が通ればいいです。準正常系と異常系で未実装な部分は、そういった積み残しを管理するWSを作って、そこにPhaseとして積みましょう。それはすべての実装が1パス通って疎通確認できてからでいいです。」「積み残しWSはベータ2積み残しという形でWSを作りましょう。」

第 1〜3 段の実装で、正常系だけを実装して残した準正常系・異常系を、ここに Phase として集めて実装する。

## 集め方

2026-10-06 夜 ユーザー:「各WSに入れるとWSがCompleteにできないので、専用の1つのベータ2積み残しというWSに入れてください。」
- 積み残しは元の WS・Phase には置かず、この WS にだけ書く（元の WS は正常系で完了にできる）。
- 担当は実装した時に、準正常系・異常系で実装しなかった物（元の WS・Phase、条件、期待の動き、source の所在）を、自分の一覧の file に 1 行ずつ足す: P1 は [backlog-p1.md](backlog-p1.md)、P2 は [backlog-p2.md](backlog-p2.md)、その他は Q1 に送る（file を分けて merge の衝突を避ける）。担当はこの 2 つの file を書いてよい（Q1 の委任）。
- 全ての実装を 1 回通して疎通を確かめた後（第 1〜3 段の後）、Q1 が一覧を読み、この WS の Phase に分ける（機能ごと）。
- Bug（症状が出た物）は Bug Board に、ここには「まだ実装していない分岐」だけを置く。

## 完了の条件

集めた Phase が全て cleared（host の試験と T1 の試験）。

## Phase

| Phase | 内容 | 状態 | 依存 |
| --- | --- | --- | --- |
| [ws177-p001](phase001/phase.md) | 案 E: compositor の wl_surface.enter・leave（backlog-p1 42） | cleared（2026-10-08 T1-404 PASS） | ws113-p007・p015 の出力の所属 |
| [ws177-p002](phase002/phase.md) | 案 F: i915 の TC の legacy の PHY の待ちを sleep に、停止で PHY を返す（backlog-p2 153 の一部） | in-progress（実装・host・build。QEMU は i915 を通らない、5330 の実機が残り） | | ws051-p002b |
| [ws177-p003](phase003/phase.md) | 案 G の host の分: UCSI の誤りの理由・CANCEL・PPM_RESET の回復・通知の無い時の poll・記録と再生・停止の道・取り消し・forget（backlog-p2 148〜150・155 の一部） | in-progress（host の分を実装・host PASS・build。5330 の実機が残り） | | ws050-p002〜p005 |
| [ws177-p004](phase004/phase.md) | 案 D: desktop の UI の小物（kl_field の limit・KL_VERSION 64、accent の keyboard、network の行の as_is、IME の popup の accent、Files の Help の折り返し） | cleared（2026-10-08 Q1、T1-406・T1-413） | | — |
| [ws177-p005](phase005/phase.md) | 案 A: 通知とメールの通知の口の堅さ（client の去り・UTF-8・速さ・ring の溢れ・listen の掃除・allowed の event・arrived の門、KL_VERSION 65・manager 20） | cleared（2026-10-08 Q1、T1-408） | | — |
| [ws177-p006](phase006/phase.md) | 案 I の 45 行: fidoctl の PIN の入力（端末で echo を切る、CTAP2 の長さの規則を先に） | in-progress（実装・host・build、main に統合。端末で echo が出ないことは実機か serial の UAT） | | — |
| [ws177-p007](phase007/phase.md) | 案 B: Welcome の準正常系（印の失敗・Files の失敗・Network の段の言葉・折り返し・keyboard） | cleared（2026-10-08 Q1、T1-410） | | — |
| [ws177-p008](phase008/phase.md) | 案 C: Files の Recents の仕上げ（止めた一覧の表示、Clear Recents の問い、一覧の stamp と Text Editor の読み直し、KL_VERSION 66） | cleared（2026-10-08 Q1、T1-412） | | — |
| [ws177-p009](phase009/phase.md) | 案 H: 手書きの頑健さ（tap の note、templates の壊れ・空・大きすぎ、Hershey の重ね線と番号の衝突、印の位置、templates を thread で、8,192 点を越える ink） | cleared（2026-10-08 Q1、host の範囲。実機の UAT は未） | | — |
| [ws177-p010](phase010/phase.md) | 案 J: keiland-preview の仕上げ（libpdf の memory の font と Mahora の埋め込み、爆弾・fuzz の試験、縮小画像の子を 2 つ同時、失敗の印の cache、Quick Look・Today を待たずに） | cleared（2026-10-08 Q1、T1-414 と host） | | — |
| [ws177-p011](phase011/phase.md) | 案 K3: Notes の Save Clean Copy の仕上げ（form・Type 3・pattern の resource の刈り込み、name tree の /Limits と直接の filespec、名の木の根を頁の木と取り違える不具合、copy を開く menu と一度だけの注意） | in-progress（実装・host・build。Notes の画面（案内・menu）は UAT） | | — |
| [ws177-p012](phase012/phase.md) | 案 K（Notes の側）: 文字の box が zoom・scroll・窓に付いて動く、指、Ctrl+S・W、長い行、font の無い時（回転は未実装） | cleared（2026-10-08 Q1、T1-415。指は実機の UAT、回転は backlog） | | — |
| [ws177-p013](phase013/phase.md) | 案 K の 9: libkeiland の field・text area の undo・redo（打鍵ごと）、clipboard、語の移動（KL_VERSION 67、全ての app） | cleared（2026-10-08 Q1、T1-416） | | — |
| [ws177-p014](phase014/phase.md) | 案 N4: Browser の sign-in code（欄が無い時は clipboard、別の窓は通知に page の名、英字の code、語の境、2 分で起きて取り下げ、通知の click の AAT。one-time-code の欄は libbrowser の判断待ち） | cleared（2026-10-08 Q1、T1-418） | | ws169-p005・ws156-p003 |
| [ws177-p015](phase015/phase.md) | 案 N2: Mail の基本の操作（TLS の失敗の理由と自己署名の信頼、Trash の完全な削除・Gmail の Sent、account の編集・削除・16 個、日付の語、512 通の上限） | cleared（2026-10-08 Q1、host と T1-420。新しい UI は実機の UAT） | | ws169-p003・p004 |
| [ws177-p016](phase016/phase.md) | 案 N: Mail の IMAP・SMTP の互換（ISO-2022-JP・Shift_JIS・EUC-JP、1 MiB 超を BODYSTRUCTURE で、modified UTF-7・literal の folder 名、UID MOVE・UID EXPUNGE、宛先の名の encoded word と折り返し、AUTH LOGIN。76 は p014 で済み） | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2）。T1-422） | | | ws169-p003・ws177-p015 |
| [ws177-p017](phase017/phase.md) | libbrowser の `browser_view_focus_field`（autocomplete の token で欄を探して focus、public の口の追加） | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2）。T1-424） | | | ws177-p014 の判断（2026-10-08 ユーザー許可） |
| [ws177-p018](phase018/phase.md) | sign-in code を `autocomplete="one-time-code"` の欄に直に入れる（p014 の後半、無ければ clipboard） | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2）。T1-424） | | | ws177-p017 |
| [ws177-p019](phase019/phase.md) | 案 O: Browser の IME・form と shell の fd（surrounding text・purpose（KL_VERSION 68）、選んだ文節、click で合成を値に、script の value、textarea の value、64 を超える fd を shell が poll。13 は不要と判断） | test-wait（T1-425: (a)(c)(e) は確認、(b)(d) は期待どおりでない。Browser の合成の確定・OSK の content type の残り（q893）はベータ3、2026-10-08 ユーザー「ブラウザはベータ3に移します」） | | ws177-p017 |
| [ws177-p020](phase020/phase.md) | 案 M の 1: 音楽の collection と cover（絵のある .mp4 の音、4096 曲・深さ 4 の上限を外す、tags の cache と folder の変化、album の同じさと Unicode の大小、cover の切り抜きと知らせ） | in-progress（2026-10-08 P2 q903 実装・host PASS・build c0e39af49、T1 は p021 と一緒に） | ws120-p008・p009 |
| [ws177-p021](phase021/phase.md) | 案 M の 2: 音楽の再生の失敗と key（decode・読み・file の消失で次へ、音の service の lost で開き直し（WS191）、Files の理由、検索の field の Escape、連打のまとめ） | in-progress（2026-10-08 P2 q903） | ws120-p009・ws191-p003 |
