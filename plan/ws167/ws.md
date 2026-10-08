<!-- awesome-plan project=zedbsd record=ws167 -->
# WS167: GPU の command の protocol の独自化

Status: incomplete（2026-10-08 q910 P2 の照合: p002 cleared（T1-313）、p001（設計）は Q1 の判定、p003（規約）はベータ3。WS の完了を Q1 が判定）（2026-10-07 p002 実装、T1 待ち。2026-10-05 追加、**ベータ2**（2026-10-05 ユーザー）、見積もり 2 LW。p001 の設計の第 1 版あり）
Master: [master](../master.md)
Primary Milestone: MG006

## 由来

ユーザー（2026-10-05）「GPUのコマンドが、Venusプロトコル番号を流用しているので、独自の名前と番号にする。ただしVenusと一致している内容からスタートする。Venusの番号を再利用したことをヘッダに書いて、Googleの著作権表示を外せるようにする。」

## 範囲（案、p001 の設計で確定する）

1. 今 Venus の protocol の番号を流用している GPU の command を、独自の名前と番号の header にする。最初の内容は Venus と一致させる（互換）。
2. header に「Venus の番号を再利用した」ことを書き、Google の著作権の表示を外せる形にする（license の確かめ）。
3. 影響する kernel・libvulkan・試験の範囲を調べる。

## Phase

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws167-p001](phase001/phase.md) | 要件と設計 | planning（設計の第 1 版、2026-10-05 P1 q730。ユーザーの判断 H1〜H3（license・UAPI の置き場所・名前）待ち） | — |
| [ws167-p002](phase002/phase.md) | header・3 か所の置き換え・license の記録・値の照合・T1 | cleared（2026-10-07、T1-313） | p001、H1〜H3 |
| ws167-p003 | 全文規約の見直し | planned | p002 |
