<!-- awesome-plan project=zedbsd record=ws121 -->

# WS121: Web ブラウザでのアクセラレーションつきのビデオ再生

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.3）: **ベータ3**（browser と一緒、WS074）。p001 は cleared。残りの p003（Range）はベータ3。2026-10-08 q910 P2 の照合: p002・p004〜p006 cleared（T1-315）、p001（設計、U0〜U7 は既定で進めた）は Q1 の判定、p003（Range）は未着手。browser はベータ3（2026-10-08 ユーザー）なので WS121 の残りもベータ3 か Q1 が決める）（2026-10-07 q831 P2: U0〜U7 は既定（推奨）で進めた。p002・p004〜p006 を実装、p003（Range）は後）
Primary Milestone: MG006
Objectives: O2
Parent: [Master](../master.md)
Focused goal: fg019（ベータ1。2026-10-05 Q1 records reconciliation: Master の工数の表で「ベータ2（2026-10-05 移動）」、段の表ではベータ3。どちらかは Q1 が Master で決める）
Queue: q831（P2）
Resume point: p002・p004〜p006 の T1（apps.browser.video）、p001 の U0〜U7 のユーザーの判断（既定で進めた）、p003（Range）。
2026-10-02 user: 動画関連（WS083・WS121・WS122）は**別セッション**でユーザーがノウハウを提供しステップバイステップで進める。このセッション（Q1/P1〜P8）は割り当てない。ベータ1 では最悪 drop してもリリース可能とする（努力目標）。VA-API（WS123）は canceled、アプリが Vulkan Video を直接使う。ブラウザへの組み込み（WS121）は Codex 側と調整。（2026-10-05 に下の行で置き換え: 別セッション・Codex との調整の記述は履歴）
2026-10-05 ユーザー: Codex の担当を外し、WS107・WS121 は Q1 が作業する（「WS107, WS121はあなたが作業します。」）。p001 の設計から。WS122 と同じく、まず dlopen の libavcodec の software decode で、GPU の decode（WS083 の Vulkan Video）は後で使う形を検討する。
<!-- awesome-plan-current:end -->

## 目標（2026-10-02 ユーザー（ベータ1、リリース目標 10/17））

「Webブラウザでアクセラレーションつきのビデオ再生を可能にする。」

- browser（~~WS074 は Codex が作業中~~ → 2026-10-05 から Q1 の担当）で `<video>` を Vulkan Video（WS083）を直接使う hardware decode で再生する。browser の source の所有と編集の順序は Codex の作業と調整が要る（ユーザーに確認）。2026-10-05 Q1 records reconciliation: Codex の担当は外れ、WS074・WS107・WS121 は Q1 が扱う（ユーザー）ので、この調整は不要になった。source は WS107 の配置（engine は `userland/desktop/libbrowser/`、shell は `userland/desktop/browser/`）で、`<video>` の対応は libbrowser の公開 API（`browser.h`）の変更を伴う見込み。WS074 のレンダリングの改善の停止は、WS121 の作業を止めない（ユーザーが WS121 を Q1 に割り当てた）。

## Phase

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 要件・設計（software decode の `<video>`、libmedia、Range、JS の API、音） | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: U0〜U7 は既定で進め、p002・p004〜p006 が実装・cleared。旧: in-progress（2026-10-05 夜、第 2 版、review 2 回目とユーザーの判断 U0〜U7 待ち）） | — |
| [p002](phase002/phase.md) | libmedia（mediafile の source、engine、共有の library） | cleared（2026-10-07、T1-315） | p001（既定で） |
| p003 | Range の loader と media-fetch（大きな remote の media） | planning（後に回した: 今は loader で全体を 64 MiB まで） | p002 |
| [p004](phase004/phase.md) | `<video>` の DOM・layout・描画、autoplay muted | cleared（2026-10-07、T1-315） | p002 |
| [p005](phase005/phase.md) | JS の HTMLMediaElement と event | cleared（2026-10-07、T1-315） | p004 |
| [p006](phase006/phase.md) | 音と controls の帯、AAT | cleared（2026-10-07、T1-315） | p005 |
| 最後 | 全文規約と回帰 | planning | 実装 Phase |

2026-10-05 Q1 records reconciliation: 「このセッションは割り当てない」「Codex 側と調整」は 2026-10-05 のユーザーの指示で置き換わった（上の Status の block の 2 行目）。Master の「動画（別セッション）」「別セッション（ユーザー） WS083 → WS122 → WS121」の行と段（ベータ1・2・3）の不一致は Q1 に直しを提案した。Status は planning のまま、Queue なし。
