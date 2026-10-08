# ベータ2 の RC に向けた優先の WS の記録の照合（2026-10-08 夜、P1 q902）

読むだけの報告。base は main 51d98cc0f（agent/p1 に merge）。対象はユーザーの優先順（master の Focus の block）の WS113 → WS051・WS050 → WS083 → WS090 → WS156 → WS183 → WS161 → WS157 → WS155 → WS143 → 残り（WS177・WS187・WS188・WS189・WS190・WS191）。
照らした物: 各 ws.md・phase.md、T1 の台帳（plan/agents/T1/requests.md、T1-446 まで）、T2 の台帳、Bug Board、BUG-256・266・267 の記録と 5330 の log、master の decisions-log。source は変えていない。
ベータ3・10/13 以降にユーザーが回した物は除いた: 各 WS の全文規約の Phase（2026-10-08）、WS172、WS052（Sleep・蓋）、BUG-255 と蓋の自動の切り替え、WS074 browser と q893、Linux・FreeBSD の作業（WS113 p010・WS143 p007・WS156 p006・WS191 p004・WS161 p006 の Linux・FreeBSD の build）、WS157・WS155 の続き。

## 1. 直した記録

この Queue の許可の範囲（列挙の WS の ws.md と phase.md の Status の行）だけ。旧い値は各 Status の行の「（旧: …）」に残した。

- ws.md の表・current を phase.md・台帳に合わせた: WS113、WS051（Status planning → incomplete）、WS050、WS083、WS090、WS156、WS183、WS161（planning → incomplete）、WS157、WS155、WS143、WS177（planning → incomplete）、WS187、WS188、WS189（planned → incomplete）、WS190、WS191。
- phase.md の Status: ws113 p001・p002・p004・p005・p011・p011a・p012・p013・p014、ws051 p001・p004a・p004b・p004c・p005a・p005b、ws050 p001・p005（5330 の log の引用の節を追加）、ws083 p001・p002・p004、ws090 p007・p009・p010・p017・p018・p021・p023・p026、ws156 p001、ws161 p001・p005（Q1 が ws.md で cleared にしていた物を phase.md に写した）、ws155 p000、ws143 p003・p004・p005・p006、ws177 p016〜p019、ws189 p002・p003、ws190 p001〜p003、ws191 p002・p003。
- cleared にした Phase は無い（ws161-p005 は Q1 の既存の判定の写し）。

## 2. Q1 が判定すべき Phase

| Phase | 証拠 | 推し |
| --- | --- | --- |
| ws113-p001 契約 | C1〜C4 は 2026-10-05 に決定（contracts-beta2.md）、後続が全部この契約で実装済み | cleared |
| ws113-p004（p004b）複数の同時出力 | T1-367 1) 新しい guest で `displays-p004b: PASS`。5330 のユーザーの UAT「HDMIに出力されました。extendもmirrorも動いています。」 | cleared（QEMU と実機の両方） |
| ws113-p005 kl_system_displays・明るさ | T1-366 `displays-p005` PASS。明るさの slider と Fn の key は 5330 で未 | cleared（実機の明るさは p008 へ）か実機まで保留 |
| ws113-p011 i915 の 2 つ目の出力 | 同じ 5330 の UAT（HDMI で拡張・mirror）。HDMI を抜いた時の head の release の log は未確認 | cleared（抜きは p008 で） |
| ws113-p002 inventory・HPD | T1-115 PASS（QEMU の回帰）。実機の (1)〜(4) は未だが UAT で外部 display が Settings に出た | 実機の (1)〜(4) を p008 にまとめて cleared |
| ws051-p001 設計 | 第 4 版、正解値は q848（p003 cleared）、p002〜p005b が実装済み | cleared |
| ws051-p004a・p004b・p005a | 5330 の UAT 2026-10-08 午後「USB-C DPでディスプレイ出力ができました！」「拡張モードで出力されました。」「ミラーもうまくいきました。」（TC2、BUG-256 resolved） | cleared（TC1 と抜き差しの繰り返しは p006 か UAT の残りに）。p004b は BUG-266 の付け替えの失敗を別に持つ |
| ws050-p001 設計 | §10 は 2026-10-04 ユーザーが決定 | cleared |
| ws050-p005 i915 との連携 | 5330 の log（BUG-256/kernel-5330-uat-20261008.log）: `typec: display port TC1: connector 0 ... bound (error 0)`、`TC2: connector 1 ... bound`、TC3・4 は not bound、`ucsi: 2 connectors, 4 Alternate Modes`、timeout 0。phase005 に引用 | cleared（master の merge の block の「typec: display port TC1・TC2 の確認待ち」はこれで答えが出ている） |
| ws083-p001 設計 | 第 3.1 版、人の判断は全部 2026-10-07 に決定、q897 の照合の review は blocking 無し | cleared |
| ws083-p002・p004 | T1-371 PASS（§8.2 の QEMU 回帰）。実機の hash は T1-435（未実行） | p004 は表で Q1 判定済みの QEMU の分。どちらも T1-435 の後に cleared |
| ws090-p007 Settings の kl_field | T1-263・260・338・373 (3)・376 (b)（PIN の crash の BUG-257 の直しの後）。Wi-Fi の鍵の欄だけ未（QEMU に Wi-Fi が無い） | cleared（Wi-Fi の鍵は 5330 の UAT へ） |
| ws090-p009 描画の層 | T1-261・T1-272 PASS。撮影は T1-373 (4)（t1 の build/t1-373/shots/） | ユーザーに PNG を見せて cleared |
| ws090-p010 改名の欄 | T1-261、T1-373 (2) で全項目 | cleared |
| ws090-p017 慣性 | T1-230b・T1-242・T1-246 PASS | cleared |
| ws090-p018 hover の部分の再描画 | T1-233、T1-389・T1-393（PNG は Q1 の目視） | 目視の後 cleared |
| ws090-p023 部品の置き換え | T1-266、T1-373 (1)。copy の取り消しの × と Wi-Fi の Disconnect は未（QEMU では見られない） | cleared（残りは 5330 の UAT へ） |
| ws090-p020・p021 | T1-252・T1-256 の PNG | ユーザーの外観の判断が要る（Q1 がユーザーに見せる） |
| ws156-p001 設計 | H1〜H7 は 2026-10-05 夜に決定、p002〜p004 cleared | cleared |
| ws161-p001 設計 | U1〜U5 は 2026-10-05 承認、p002〜p005 cleared | cleared |
| ws155-p000 mock | T1-179・179c、再指示は p001 に反映、app は p002〜p004 で cleared | cleared か置き換えの canceled |
| ws143-p003 i01 | T1-402（2 回目 PASS、1 回目 FAIL） | i01 は cleared、Phase は i02（intelbt・5330）まで in-progress |
| ws143-p004 | T1-405 FAIL → i03 の直しで T1-409 PASS、T1-426 でも bt-pair-p004 PASS | cleared |
| ws143-p005 i01a〜c | T1-419、T1-421 FAIL → T1-426・T1-432 PASS、T1-423 boot PASS | i01a〜c は cleared、Phase は i02〜i04 まで in-progress |
| ws177-p016 | T1-422 fail なし（PNG は Q1 の目視） | cleared |
| ws177-p017・p018 | T1-424（`one-time-code-field error=0`・`fill length=4 error=0`） | cleared |
| ws190-p001 設計 | 第 2 版、§7 は Q1 が回答、p002・p003 が実装済み | cleared |
| WS188（WS 全体） | p001〜p004 全部 cleared | WS の完了を判定。注意: 表に p004 が 2 つ（canceled の preview の spawn と cleared の Files の mount、phase004/ は後者）。番号は振り直していない |
| BUG-265（参考、範囲外） | T1-440 PASS だが Bug Board は scheduled のまま | Q1 が Bug Board を直す |

## 3. T1 の依頼の束（QEMU 1 回）

未実行の T1-443・T1-445（WS190）・T1-444（WS191）・T1-446（WS143 p006）と、WS189 の未実施の項目を 1 つの image・1 つの guest にまとめる案。全部が AAT の image（`plan/tools/aat/config-amd64-aat.mk` = current-uat ＋ 注入と撮影）に足す形なので 1 つにできる。T1-435（WS083 の 5330 の passthrough）は別の回（下の §4）。

### image（今の main の tree）

config（Q1 が置き場を決める。例 `plan/ws189/tests/config-amd64-aat-rc-bundle.mk`。この Queue では作っていない）:

```make
include plan/tools/aat/config-amd64-aat.mk
ZEDBSD_USER_PROGRAMS += kuidemo data-probe bluetoothd bt
CONFIG_BT_TEST_LOOPBACK := y
ZEDBSD_EXTRA_INPUTS += plan/ws122/tests/sample.mp4
ZEDBSD_EXTRA_FILES += --file /usr/share/ws191/sample.mp4=plan/ws122/tests/sample.mp4
```

build は T1-433・T1-438 と同じ形（`plan/tools/aat/build-image.sh` の手順で config をこれに替える）。結果に image の tree の SHA を書く（protocol 2026-10-08）。

### guest

- `VENUS_DISPLAY=dbus VENUS_OUTPUTS=2` の Venus の guest（`plan/ws035/tests/zdesktop-guest.sh start` の形）に HD Audio を足す: `-audiodev wav,id=w,path=OUT/ws191.wav,out.frequency=48000,out.channels=2,out.format=s16 -device intel-hda -device hda-duplex,audiodev=w`。zdesktop-guest.sh に追加の QEMU の引数の口は無いので、T1 の scratch の起動の写しに足す（T1-433 の scratch の build-dnd.sh と同じ扱い）か、Q1 が zdesktop-guest.sh に口を足す。
- head 1 は across-displays まで挿さない（`plan/tools/guest/venus-head.sh` で最後に挿す）。それまでは 1280x800 の 1 出力の geometry のまま。撮影は AAT の keiland-shot（vnc.sock は使わない）。
- 最初に `aat start` で 4 つ目の touch の device が `KWL TOUCH added` で見えることを確かめる（T1-443 の前提）。

### 順と合否

1. **T1-443（ws190-p002）**: desktop.widgets.touch-select 1〜6（6 は画面 keyboard が出なければ not-run）。合否は各手順の KUIDEMO の log と撮影（bar・handle の見た目は Q1）。
2. **T1-445（ws190-p003）**: apps.textedit.touch-select 1〜6。合否は `TEXTEDIT TOUCH` の log と撮影。
3. **T1-444（ws191-p003）**: 準備 `mkdir -p ~/Music ~/Videos; cp /usr/share/ws191/sample.mp4 ~/Music/sample.m4a; cp /usr/share/ws191/sample.mp4 ~/Videos/`。(a) Music の play.md、(b) `/bin/videoplayer /home/kei/Videos/sample.mp4`。合否: session の log に `KWL AUDIO streams offered=1`・`stream … create`・`ready`・`start … error=0`、seek で `flush … error=0`、閉じて `closed`。位置の表示が進む（PNG）、WAV が再生の区間で無音でない（`plan/ws035/tests/hda-wav-check.py`）、app の log に `AUDIO open error=` が無い。
4. **WS189 の残り（新しい依頼、手操作、drag は guest の `aat-input` の 1 run で段の間 40〜100 ms、最後に 1 s 止めて離す）**: desktop.dnd.content-to-desktop（Files の desktop を起こす）、apps.pdfviewer.drag-out（PDF を put）、desktop.dnd.photo-to-notes 3〜4、desktop.dnd.text-between-windows 5〜6。合否は各シナリオの log の行（drop・receive・`dropped=1`）と PNG。
5. **T1-446（ws143-p006）**: bluetoothd を起こす前に apps.settings.bluetooth-pair 1（`ZSETTINGS BLUETOOTH state reachable=0`）、その後 4（07 の Pair → consent か Just Works かを記録 → Esc → errno=13）と手順 2 の Other Devices の 07。時間があれば `plan/ws143/tests/bt-daemon-p003.sh` の scan（devices=5）。bluetooth-pair は draft なので手操作。
6. **desktop.dnd.across-displays（WS189 p002）**: 最後に `venus-head.sh 1 1280x800` で head 1 を挿し、シナリオどおり。終わったら抜く。2 出力が立たなければ not-run。

見込み: build 約 10〜20 分、試験 約 40〜50 分。長すぎれば 6 と 5 を次の回へ。

## 4. 実機が要る物

### T1 の 5330 の passthrough（ユーザーの立会いは不要）

- **T1-435**（WS083 p005・p006b、未実行）: VCS0 の bring-up、I frame・P・B の hash、HuC 無し、`i915: video:` の行。2026-10-08 午後 ユーザー「5330はつけっぱなしですので、Videoのテストで使ってよいです」。p008 の result status も probe が自動で見る。
- ws083-p007 の人工の hang からの回復は道具が未作成（P2 が作ってから）。

### 5330 の素の起動（ユーザーの立会いか SSH の log）

| 対象 | 見る物 |
| --- | --- |
| BUG-266（P2 の直しの後） | 拡張で eDP を off → TC2 だけで session が続く。続けて ws113-p014 の UAT（内蔵の off・HDMI の off・touch の試験に使えること） |
| ws113-p008 | M1 HDMI の抜き差し 10 回、M2 Settings の拡張 ⇔ mirror 5 回と配置の drag、M3 窓の画面の間の drag 10 回、異解像度、明るさの slider（p005・p013 の `backlight-probe`）、Fn の key、p012 の refresh 約 60 Hz と power off、p002 の (1)〜(4)、p011 の抜きの head の release、p015 の head の上の press |
| WS051 | TC1 の DP、抜き差しの繰り返し（p005a）、GOP が USB-C の起動（p004c）、IRQ_HPD の retrain（p005b） |
| WS050 p006 | /dev/typec、role・Alt Mode の操作 |
| WS177 p002・p003 | TC の legacy の PHY の待ち、UCSI の誤りの回復 |
| ws090 | Settings の Wi-Fi の鍵の欄と Disconnect（p007・p023）、大きい file の copy の取り消しの ×（p023）、p026 の `ZFILES DESKTOP band`・`KWL PERF` の測定（層 3 の判断） |
| ws156-p005 | popup の動き・×・hotkey の log の UAT |
| WS187 | lock の画面の touchpad のスワイプ・wheel、PIN、Hardware Key |
| WS161 p006 | YubiKey 5 の USB、ACR1252U と YubiKey 5 NFC（ユーザーの鍵と reader） |
| WS143 | p002 の passthrough（T1-378 は 5330 が Linux の時）、p003 i02 の firmware の load と scan、p005 i04 の実機の門（ユーザーの BR/EDR と LE の機器）、p008 の UAT |
| ws183-p002・BUG-261 | tap の click の反応・double click・tap-drag、GPIO の割り込みが止まらない |
| ws177 p006・p009・p011・p012・p015 | fidoctl の PIN の echo、手書き、Notes の画面、指、Mail の新しい UI |
| BUG-267（P1、Q1 の割り込み待ち） | USB の外付け touchscreen の 10 点 |

### 5320（電源オフ、ユーザーの時期）

- ws183-p001: `reads on its interrupt (irq 51 level=1 active_low=1)`、`samples its input` が無い、touchpad の動きと tap、BUG-247。ws183-p002 の tap も。

ベータ3 に回った物（実機の一覧から外した）: plan/agents/uat-5330-bare.md の B（蓋で HDMI へ、ws113-p011a・ws052-p012）と C（Sleep）。

## 5. ベータ2 の残りの作業（LW、1 LW ≈ 実時間 1〜2 時間、実機の待ちは含まない）

| WS | 残り | 実装 | 試験・判定 | 計 |
| --- | --- | --- | --- | --- |
| WS113 | BUG-266（P2、着手済み）、p008 の実機の手順と見つかる直し、p012・p013 の i915 の確認 | 1.0 | 0.8 | 1.8 |
| WS051 | p005b の範囲 2（案 (b)）の実装、p004c・p005b・TC1 の実機 | 0.5 | 0.3 | 0.8 |
| WS050 | p006 の実機（/dev/typec、操作） | 0 | 0.3 | 0.3 |
| WS083 | T1-435 と FAIL の直し（不確実）、p007 の hang の道具と実機、p008 の性能と門の既定化、R-S4 | 2.0 | 1.0 | 3.0 |
| WS090 | p026 の 5330 の測定（層 3 は測定次第で +1.5）、5330 の小さい確認、ユーザーの PNG の確認 | 0 | 0.5 | 0.5（+1.5） |
| WS156 | p005 の UAT | 0 | 0.2 | 0.2 |
| WS183 | p001・p002・BUG-261 の実機 | 0 | 0.4 | 0.4 |
| WS161 | p006 の UAT（直しが要れば +1） | 0 | 0.5 | 0.5（+1） |
| WS157 | 無し（続きはベータ3） | 0 | 0 | 0 |
| WS155 | p000 の判定だけ | 0 | 0 | 0 |
| WS143 | p005 i02（BR/EDR の HID host）2.5・i03（LE の HOGP）2.0・i04、p003 i02（intelbt の package と 5330）1.0、T1-446 の後の直し、p008 の UAT | 5.5 | 1.3 | 6.8 |
| WS177 | p002・p003・p006・p009・p011 の実機・UAT | 0 | 0.3 | 0.3 |
| WS187 | 実機の UAT | 0 | 0.2 | 0.2 |
| WS188 | 無し（完了の判定） | 0 | 0 | 0 |
| WS189 | §3 の 4・6 の結果と直し、シナリオの helper（今は by-agent）、p004 のシナリオの期待の直し | 0.6 | 0.2 | 0.8 |
| WS190 | T1-443・445 と直し | 0.3 | 0.2 | 0.5 |
| WS191 | T1-444 と直し | 0.3 | 0.2 | 0.5 |
| 計 | | 10.2 | 5.9 | **16.6 LW**（条件付き +2.5） |

範囲の外の参考: BUG-267（USB の touchscreen の multitouch、P1 の次）約 1 LW。WS143 が全体の 4 割で、i02・i03 を 10/13 までに入れるかが RC の最大の判断。

## 6. その他の見つけた事

- master の merge の block の「実機（5330）の確認待ち: dmesg の `typec: display port TC1:`・`TC2:`」は §2 の ws050-p005 の log で答えが出ている。
- plan/agents/sweep-20261008.md（前の q874）は 1 行の見出しだけで中身が無い。
- WS188 の表の p004 の ID の重複（§2）。
- ws189 の dnd のシナリオは今は active だが helper が無く run-aat.sh では by-agent（T1-439）。T1 は毎回手操作。
- 照合していない他のベータ2 の WS（WS089・099・127・128・131・132・159・169・170・175 など）は、この Queue の範囲の外。
