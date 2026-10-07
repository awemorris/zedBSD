<!-- awesome-plan project=zedbsd record=ws179 -->

# WS179: UI のアクセントカラーを選べるように

Status: incomplete（2026-10-07 追加、ベータ2。p001 を P1 が実行中）
Master: [master](../master.md)
Primary Milestone: MG006

## 由来

ユーザー（2026-10-07）:「UIのボタンなどのアクセントカラーは、何色かから選べるようにしましょう。現状のUIが非常に完成度が高く、ちょっとよくばりな意見を持ってしまいました。満足している裏返しと思ってください。」
クリック: 範囲は「ベータ2（今の作業の後）」、色は「8 色の固定」。

## 目標

- Settings の Appearance で 8 色の固定の accent（今の青を既定、ほかに紫・ピンク・赤・橙・黄・緑・graphite の案）から選ぶ。
- libkeiland の部品（button の主・選択・switch・slider・link・focus の輪・進みの bar など）と compositor の自前の UI（bar・App Home・titlebar の control・OSK の強調）が設定の accent を使う。
- 各色はライト・ダークのそれぞれで文字との contrast を確かめた値（明るい色の上の文字は黒など）を持つ。
- 設定は desktop の設定の key（例 `appearance.accent`）で、変えると開いている app にも伝わる（ライト・ダークの切り替えと同じ道）。

## Phase

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計（[design.md](design.md)、色の表、accent を使う所の一覧、伝え方、KL の API）と実装: libkeiland・compositor の UI・Settings（Appearance の選択）・Files、host の試験と T1 の撮影 | in-progress（P1、実装と host 試験済み、T1-335 待ち） | — |
| [p002](phase002/phase.md) | Calendar・PDF Viewer・Phone・Mailer・Notes・Image Viewer の独自の accent を theme に従わせる（2026-10-07 Q1: p001 の直後、ベータ2 の中） | in-progress（P1、実装と host 試験済み、T1 待ち） | p001 |
| [p003](phase003/phase.md) | 規約の全文の見直し（WS の変えた C の全部、ベータ3）、AAT のシナリオ（`tests/scenarios/desktop/appearance/`）に accent | in-progress（2026-10-08 P1 q875: AAT のシナリオと helper、QEMU は T1。規約はベータ3） | p001、p002 |
