<!-- awesome-plan project=zedbsd record=ws154 -->

# WS154: Settings の Languages の頁で IME を選ぶ（日本語・SKK・なし=英語）と、SKK の IME の新しい実装

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: p001〜p004 は cleared（表を直した）。残りは 5330 の UAT（SKK の操作感）と範囲の判断（`>`・`/`・`#`・Tab・注釈）、規約（ベータ3））
Primary Milestone: MG006
Related Milestones: —
Parent: [Master](../master.md)
Queue: Q1（ベータ2 の割り当て、P2、2026-10-05）
Resume point: p002〜p004 は実装済み（T1-173 と languages-p004 の T1 待ち）。p001 の 3（範囲）はユーザーに確認中。残りは T1 の結果、実機の UAT、全文の規約の見直し。
<!-- awesome-plan-current:end -->

## 単一目標

Settings に Languages の頁を足して、使う IME を「日本語」「SKK」「なし（英語の mode）」から選べるようにし、SKK の入力方式の IME を新しく実装する。

## ユーザーの指示（2026-10-04 夜、原文）

「SettingsにLanguages画面を追加して、IMEを選択できるようにします。また、選べるIMEは、日本語、SKKです。何もIMEを選ばないときは英語モードです。SKK IMEは新規実装します。辞書はEmacsのものを使ってください。」

## 用語（Q1 の整理）

- **日本語**: 今の Kei の IME（[WS095](../ws095/ws.md)、ローマ字かな変換で文節を変換する方式。辞書は SKK の形式の `SKK-JISYO.X`・`SKK-JISYO.kei` を使う）。
- **SKK**: SKK の入力方式（Emacs の SKK・ddskk の操作: 大文字で変換の開始、送り仮名の区切り、▽・▼ の mode、`l`・`q`・`C-j` などの mode の切替）の IME を新しく実装する。
- **なし**: IME を使わない英語の mode（key がそのまま入る）。

## 範囲（p001 で設計して確定）

0. Languages の頁には UI の言語の選択も置く（[WS158](../ws158/ws.md) の翻訳、2026-10-05 ユーザー「SettingsのLanguagesタブで選択」）。

1. **Settings の Languages の頁**（新しい頁。pages.c の表・glyph・検索の語）: IME の選択（日本語・SKK・なし）。設定は kl_settings_* → compositor の desktop.conf（Guardrail の「app と設定」）。選択を変えると login の session の中ですぐ切り替わる（再 login 不要かは設計で決める）。
2. **IME の切り替えの仕組み**: compositor の input method（zwp_input_method_v2 の側、keiland-ime の process）が選んだ IME を起動・切り替える。今の IME の on・off の key・右上の IME の status（A／あ、ws095-p005）との関係、SKK の mode の表示（▽・▼・かな・カナ・英数）。
3. **SKK の IME の新しい実装**: SKK の操作の状態機械、送り仮名、変換の候補の選択（space・x・候補の窓）、辞書の登録（再帰の登録の mode）、利用者の辞書の保存（今の日本語の IME の辞書の保存（BUG-143、入力が無い 3 分の後）と同じ考え）。
4. **辞書**: Emacs（`userland/base/emacs/dict/` の REmacs の SKK の辞書、`SKK-JISYO.X`・`SKK-JISYO.remacs`）を使う（ユーザー）。install の場所は今の IME の辞書の path の整理（`/usr/share/kei/ime` → `/usr/share/keiland/ime` の案、ユーザーの判断待ち）と合わせる。**辞書は重複して持ち、別々に管理する**（2026-10-04 夜 ユーザー「Emacsの辞書は重複して持ってください。別々に管理します。」）: SKK の IME 用に Emacs の辞書の複写を SKK の IME の側（例: `userland/desktop/ime/skk/dict/`）に置き、日本語の IME の辞書・`userland/base/emacs/dict/` とは独立に更新する。
5. IME の状態は app ごとに記憶する（[ws095-p016](../ws095/phase016/phase.md)、2026-10-04 ユーザー）。SKK の mode も同じ単位で記憶する。
6. Linux・FreeBSD の Keiland でも同じ（IME は Keiland の process）。

## Phase（案）

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws154-p001](phase001/phase.md) | 設計（IME の切り替えの仕組み、Languages の頁、SKK の状態機械と操作の範囲、辞書の共有と path、試験の方法） | cleared（2026-10-05 Q1: 設計は Q1 が確認（1・2 は技術の裁量で承認、3 の範囲はユーザーの判断待ちで Future の候補）。cleared）（2026-10-08 q910 P2 の照合で phase.md に合わせた。旧: in-progress（設計済み、Q1 の確認待ち）） | WS095 の今の構成 |
| [ws154-p002](phase002/phase.md) | IME の選択の仕組みと Languages の頁（日本語・なし） | cleared（2026-10-05 Q1: T1-173b で languages-p002 PASS（ja・none・SKK の変換と確定・頁の switch）。cleared）（2026-10-08 q910 P2 の照合で phase.md に合わせた。旧: in-progress（実装済み、T1 待ち）） | p001 |
| [ws154-p003](phase003/phase.md) | SKK の IME の実装（host の試験で状態機械と変換） | cleared（2026-10-05 Q1: host の試験に加え、T1-173b・T1-174b の QEMU で engine が動く（変換・確定・mode）。cleared）（2026-10-08 q910 P2 の照合で phase.md に合わせた。旧: in-progress（実装・host の試験済み）） | p001 |
| [ws154-p004](phase004/phase.md) | SKK を選択に加え、QEMU（T1）と実機の UAT、全文の規約 | cleared（2026-10-05 Q1: T1-174b で languages-p004 PASS（skk-katakana・app ごとの記憶・skk-latin）。実機の操作感は UAT、全文規約は WS の最後の Phase。cleared）（2026-10-08 q910 P2 の照合で phase.md に合わせた。旧: in-progress（実装済み、T1 待ち）） | p002、p003 |

- 2026-10-05 Q1: p001〜p004 を cleared（T1-173b・T1-174b PASS、T1-173・174 の FAIL は試験の期待の誤りだった）。残り: 5330 の UAT の操作感、範囲の判断（`>`・`/`・`#`・Tab・注釈）、全文規約の見直し。
- 2026-10-06 P2: `tools/release/license-components.json` に `ime-dict-skk`（Zlib、project の license の下）を登録した（license-inventory の「unlisted external package: ime-dict-skk」の解消、Q1 の依頼）。
