<!-- awesome-plan project=zedbsd record=ws177 -->

# WS177: ベータ2 積み残し（準正常系・異常系）

Status: planning（2026-10-06 Q1 が作成）
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
| [ws177-p002](phase002/phase.md) | 案 F: i915 の TC の legacy の PHY の待ちを sleep に、停止で PHY を返す（backlog-p2 153 の一部） | in-progress（2026-10-08 P1 q882 実装・host PASS・build、実機は 5330 の後） | ws051-p002b |
| [ws177-p003](phase003/phase.md) | 案 G の host の分: UCSI の誤りの理由・CANCEL・PPM_RESET の回復・通知の無い時の poll・記録と再生・停止の道・取り消し・forget（backlog-p2 148〜150・155 の一部） | in-progress（2026-10-08 P1 q882 実装・host PASS・build、実機は 5330 の後） | ws050-p002〜p005 |
| [ws177-p004](phase004/phase.md) | 案 D: desktop の UI の小物（kl_field の limit・KL_VERSION 64、accent の keyboard、network の行の as_is、IME の popup の accent、Files の Help の折り返し） | test-wait（T1-407）（2026-10-08 P1 q884 実装・host PASS・build） | — |
| [ws177-p005](phase005/phase.md) | 案 A: 通知とメールの通知の口の堅さ（client の去り・UTF-8・速さ・ring の溢れ・listen の掃除・allowed の event・arrived の門、KL_VERSION 65・manager 20） | test-wait（T1-408）（2026-10-08 P1 q884 実装・host PASS・build） | — |
| [ws177-p006](phase006/phase.md) | 案 I の 45 行: fidoctl の PIN の入力（端末で echo を切る、CTAP2 の長さの規則を先に） | in-progress（2026-10-08 P1 q884 実装・host PASS・build、main に統合、端末は UAT） | — |
| [ws177-p007](phase007/phase.md) | 案 B: Welcome の準正常系（印の失敗・Files の失敗・Network の段の言葉・折り返し・keyboard） | test-wait（T1-410）（2026-10-08 P1 q884 実装・host PASS・build） | — |
| [ws177-p008](phase008/phase.md) | 案 C: Files の Recents の仕上げ（止めた一覧の表示、Clear Recents の問い、一覧の stamp と Text Editor の読み直し、KL_VERSION 66） | test-wait（T1-411）（2026-10-08 P1 q884 実装・host PASS・build） | — |
