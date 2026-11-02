<!-- awesome-plan project=zedbsd record=ws345 -->

# WS345 ブラウザの thread の分け方: JavaScript を窓の event の処理と別の thread に、通信（connect を含む）を独立した thread に

- Master: [plan/master.md](../master.md)（2026-11-02 14:03 作成）。Primary Milestone: MG006。
- Status: incomplete（P8 が p001 の設計）
- 目標: ブラウザ（libbrowser と browser の shell、zedBSD・Windows・Linux の Keiland）で、JavaScript の実行と通信がどれだけ長くかかっても、窓の event の処理（入力・再描画・resize・title bar の操作）が止まらない形にする。JavaScript は窓の event の thread と別の名前付きの thread で動かす。通信は同期の呼び出し（connect・TLS の handshake・read・write）のままでよく、それを独立した名前付きの thread で行う。
- 置き場所: userland/desktop/libbrowser/、userland/desktop/browser/（shell）、Windows の OmniGrammaton の build（WS334）。

## ユーザーの決定

- （2026-11-02 14:03）ユーザーから、ブラウザで JavaScript の処理の間に GUI の event の処理が止まるので、JavaScript を別の thread で動かして窓の event の処理を止めないようにする、通信も connect() が同期なら同期のままでよいが、通信は独立した別の thread にする、という趣旨の指示を受けた。
- 全体の方針（Keiland、2026-10-28・29）: 待つ処理は数を明示した名前付きの thread で同期に行う。数の無い pool や場当たりの直しにしない。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws345-p001 | 調べと設計: 今の thread と event loop（zedBSD・Windows・Linux の shell、libbrowser の page・view・script・fetch・loader・net/tls）、どこで止まるか（JS、connect・DNS・TLS・read の同期の呼び出し）の計測、thread の表（名前・数・持つ物・渡し方）、DOM・layout と JS の排他、event の受け渡し、取り消し、試験の道 | in-progress（2026-11-02 14:03 P8 q1959-i01） | — |
| ws345-p002〜 | 実装（p001 が Phase に分ける） | planning | p001 |
| ws345-pNNN | 規約の全文の見直し（AGENTS §2） | planning | 実装 |

## Resume point

- 2026-11-02 14:03 Q1: 作成。P8 が p001（設計）に着手。設計の後に実装の Phase を立てる。
