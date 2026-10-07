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
| [ws177-p001](phase001/phase.md) | 案 E: compositor の wl_surface.enter・leave（backlog-p1 42） | in-progress（2026-10-08 P1 q882 実装・host PASS、T1 の QEMU 待ち） | ws113-p007・p015 の出力の所属 |
