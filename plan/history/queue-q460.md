<!-- awesome-plan project=zedbsd record=queue-history q460 -->

# Queue q460: look and feel の疎通確認（ws035-p059）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-26）
Active Queue: q460
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザー「…ハリボテでもいいので、一回このlook and feelが実現できることを、疎通確認的に実装してみませんか？」「スクリーン上部のバーはハリボテでいいです。ウィンドウタイトルバーは実装しましょう。…続けてください。フォントはあとで置き換えるので、Google Fontsからアルファベットを持ってきましょう。コミットしなければOKです。」

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q460-i01 | [ws035-p059](../ws035/phase059/phase.md) | cleared（浮いたタイトルバー・すりガラス・上部のバー・壁紙。drag・最大化・閉じるが動く） |

依存: ws035-p052〜p054（cleared）。

結果: `zwl --glass` で参考画像の look and feel を描けた。背後の窓のぼかしは p057。

Upcoming Work Outlook: ws035-p011（窓管理）、p025（題名・枠の描画。p059 の浮いたタイトルバーを正式に）、p055（damage）、p057（効果: 背後のぼかし）。
