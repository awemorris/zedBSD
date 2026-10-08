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
| [ws177-p021](phase021/phase.md) | 案 M の 2: 音楽の再生の失敗と key（decode・読み・file の消失で次へ、音の service の lost で開き直し（WS191）、Files の理由、検索の field の Escape、連打のまとめ） | in-progress（2026-10-08 P2 q903 実装・host PASS・build・AAT の helper、T1 の依頼を Q1 へ（p020 と一緒に）） | ws120-p009・ws191-p003 |
| [ws177-p022](phase022/phase.md) | 案 Q の 1: printd の同時の送信（printer ごと 1・全体 4）、名前解決の補助の thread と 10 秒、応答の上限（header 16 KiB・値 1024 B・名前 256）、待ちの取り消し、LPD の abort、protocol の異常で終わる（backlog-p2 124・125 の printd 側） | in-progress（2026-10-08 P2 q905 実装・host PASS・build） | ws145-p002 |
| [ws177-p023](phase023/phase.md) | 案 Q の 2: backend の printd の寿命の表（ACCEPTED 前の EOF で 2 回まで送り直し、10 秒に 3 回の起動の制限と 60 秒、FATAL、fd・CTRUNC・長い行で shut、取り消しの細部、fd の所有）（backlog-p2 125 の backend 側） | in-progress（2026-10-08 P2 q905 実装・host PASS・build） | ws145-p003・ws177-p022 |
| [ws177-p024](phase024/phase.md) | 案 Q の 3: 設定の file の writer の thread、compositor の job の title の厳密な UTF-8、protocol.c の dispatch の層の試験と printers の object の wire の試験（backlog-p2 126・143） | in-progress（2026-10-08 P2 q905 実装・host PASS・build） | ws145-p003・ws177-p023 |
| [ws177-p025](phase025/phase.md) | 案 Q の 4: Settings で printer の名前・IPP の path・LPD の queue を変える（kl_system_printers_edit、KL_VERSION 75、manager 24）、頁の host の描画の試験（backlog-p2 127） | in-progress（2026-10-08 P2 q905 実装・host PASS・build） | ws145-p004・ws177-p024 |
| [ws177-p026](phase026/phase.md) | 案 A2: Settings の Notifications の頁（Mail・Calendar・Phone・Browser の通知の switch と compositor の門、Browser の sign-in の code の switch）（backlog-p2 67） | in-progress（2026-10-08 P2 実装・host PASS・build） | ws156-p002・ws169-p002 |
| [ws177-p027](phase027/phase.md) | 案 T の 1: mediafile の fragmented MP4（mvex/trex、moof の tfhd・tfdt・trun、tkhd の track_ID）と壊れた index（file の外の sample を落として数える、dropped_count）（backlog-p2 116） | in-progress（2026-10-08 P2 q906 実装・host PASS・build） | ws122-p003 |
| [ws177-p028](phase028/phase.md) | 案 T の 2: MPEG-TS の reader（PAT/PMT、PES、H.264・AAC の ADTS、90 kHz、PTS で seek） | in-progress（2026-10-08 P2 q906 実装・host PASS・build） | ws177-p027 |
| [ws177-p029](phase029/phase.md) | 案 T の 3: Ogg の reader（Opus・Vorbis・Theora、granule、二分の seek） | in-progress（2026-10-08 P2 q906 実装・host PASS・build） | ws177-p028 |
| [ws177-p030](phase030/phase.md) | 案 T の 4: AVI の reader（RIFF・hdrl/strl・movi・idx1） | in-progress（2026-10-08 P2 q906 実装・host PASS・build） | ws177-p029 |
| [ws177-p031](phase031/phase.md) | 案 T の 5: videoplayer・Music を libmedia へ（media_ の接頭辞、decoder の ops の表、落とした packet の知らせ）（backlog-p2 119） | in-progress（2026-10-08 P2 q906 実装・host PASS・build） | ws177-p030 |
| [ws177-p032](phase032/phase.md) | printd の IPP の Print-Job の本体を chunked で、job-state-reasons の log、busy でも job-id があれば送り直さない（BUG-271、実機は未実施） | in-progress（2026-10-08 P2 実装・host（mock）PASS・build） | ws177-p022 |
<<<<<<< HEAD
| [ws177-p033](phase033/phase.md) | 案 U の 1: 上端の帯の長押し（500 ms で press を流し直す、apps bar は preview）、Home が開ききった後の desktop の層を描かない（backlog-p2 151） | in-progress（2026-10-08 P2 q908 実装・host・build） | ws181-p003 |
| [ws177-p034](phase034/phase.md) | 案 U の 2: 全画面の app へ上端の帯の press を流し直す（touch.c の `kwl_touch_shell_handback`）（backlog-p2 151） | in-progress（2026-10-08 P2 q908 実装・build） | ws177-p033 |
| [ws177-p035](phase035/phase.md) | 案 U の 3: 整列の枠より大きい最小の大きさの窓は外して詰め直す、大きさの固定の窓は枠の中に置く（letterbox）、枠の左右を desktop の swipe の帯の外へ（backlog-p2 152） | planned | ws181-p004 |
| [ws177-p036](phase036/phase.md) | 案 U の 4: 出力の大きさの変更で整列の枠を計算し直す、メニューの間の network・volume の press でメニューを先に閉じる、swap の途中の desktop の切り替え・lock で swap を取り消す（backlog-p2 152） | planned | ws177-p035 |
| [ws177-p037](phase037/phase.md) | 案 U の 5: 整列の記憶（同じ desktop で同じ形を選び直すと前の窓を前の枠へ、compositor の中だけ。2026-10-08 ユーザーの決定 (b)）、Super+矢印で鍵盤だけの入れ替え（backlog-p2 152） | planned | ws177-p036 |
| [ws177-p038](phase038/phase.md) | 案 U の 6: Home が開く・閉じる途中の bar の fade、Home の上の status の pill の隙間の press を Home へ、明るい壁紙の Home の白の文字（backlog-p2 154） | planned | ws181-p005 |
| [ws177-p040](phase040/phase.md) | 案 L の 1: libpdf の頁の文字に form XObject の中の文字（注釈は描かないので範囲の外）（backlog-p2 17） | in-progress（2026-10-08 夜 P1 q907） | ws128-p004 |
| [ws177-p041](phase041/phase.md) | 案 L の 2: PDF Viewer の検索の一致の規則（行・ハイフン・空白、Unicode の大小・全角半角・濁点・合字）、一致の数と背景の読み、U+FFFD を知らせる（backlog-p2 18・19・22・25） | in-progress（2026-10-08 夜 P1 q907） | ws177-p040 |
| [ws177-p042](phase042/phase.md) | 案 L の 3: 選択（頁をまたぐ、語・行・全て、指の長押しと handle、回転した字の塗り）（backlog-p2 20・21・24） | in-progress（2026-10-08 夜 P1 q907） | ws177-p041 |
| [ws177-p043](phase043/phase.md) | 案 L の 4: titlebar の無い時の窓の中の検索の欄、Enter の後の caret を末尾に（backlog-p2 23・41） | in-progress（2026-10-08 夜 P1 q907） | ws177-p042 |
=======
| [ws177-p040](phase040/phase.md) | 案 L の 1: libpdf の頁の文字に form XObject の中の文字（注釈は描かないので範囲の外）（backlog-p2 17） | 実装・host PASS・build（2026-10-08 夜 P1 q907）、T1 待ち | ws128-p004 |
| [ws177-p041](phase041/phase.md) | 案 L の 2: PDF Viewer の検索の一致の規則（行・ハイフン・空白、Unicode の大小・全角半角・濁点・合字）、一致の数と背景の読み、U+FFFD を知らせる（backlog-p2 18・19・22・25） | 実装・host PASS・build（2026-10-08 夜 P1 q907）、T1 待ち | ws177-p040 |
| [ws177-p042](phase042/phase.md) | 案 L の 3: 選択（頁をまたぐ、語・行・全て、指の長押しと handle、回転した字の塗り）（backlog-p2 20・21・24） | 実装・host PASS・build（2026-10-08 夜 P1 q907）、T1 待ち | ws177-p041 |
| [ws177-p043](phase043/phase.md) | 案 L の 4: titlebar の無い時の窓の中の検索の欄、Enter の後の caret を末尾に（backlog-p2 23・41） | 実装・host PASS・build（2026-10-08 夜 P1 q907）、T1 待ち | ws177-p042 |
>>>>>>> 9b9a5c0a2
