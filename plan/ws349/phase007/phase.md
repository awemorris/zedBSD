<!-- awesome-plan project=zedbsd record=ws349-p007 -->

# ws349-p007 規約の全文の見直し

- WS: [ws.md](../ws.md)。依存: p006。
- 状態: planned
- 担当の定義の推し: conformance-reviewer。

## 目的

この WS が書いた C（`src/drivers/platform/rpi5/rpi5-hdmi-audio.c`・`.h`・`rpi5-hdmi-audio-wire.c`・`.h`、`src/kern/platform/rpi5.c` の変えた所、`plan/ws349/tests/` の C）を [coding-style](../../coding-style.md) の全文と照らし、書き方だけを直す（動作は変えない）。

## 項目

- coding-style の全節（§14 の review checklist）。
- [guardrail](../../guardrail.md) の出典の規則: 値と手順の出典が block ごと、複数行の comment、file の先頭に出典を書かない、Linux・alsa-lib の名前・構造・comment を写していない（register・bit の名前が独自か）。
- source の comment・log に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照）が無い。

## 受け入れ条件

- 直した後に kernel の build が warning 0、host 試験が PASS。
- 直した所の一覧を phase.md に。
