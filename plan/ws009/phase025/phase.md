<!-- awesome-plan project=zedbsd record=ws009-p025 -->

# ws009-p025: DOC-55 managed-wlan.md を今の `net` の口に合わせて書き直す（案 A）

Status: planned（2026-10-30 P1 の設計、q1705-i01。WS の受け入れの確認で見つけた満ちない条件の 3）
Parent: [WS009](../ws.md)
Approval: 2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」。2026-10-30 Q1: 案 A〜D を Phase にする、A と D は P1 がそのまま続けて実行してよい。
実行の担当: Opus（P1）。振る舞いを code から読み取る量が多いので Sonnet に渡さない。
依存: なし

## 1. 目的

`docs/reference/managed-wlan.md` は「Status: current for the managed WLAN v1 interface」と名乗るが、削除済みの
`net wifi set-key SSID PASSPHRASE [auto]` を唯一の文法として書く。今の `userland/base/net/main.c` の usage は
`net wifi add`・`modify`・`delete`・`enable`・`disable`・`list`・`connect`・`disconnect`・`session open|close [account]`。
文書を今の source と networkd の振る舞いに合わせ、header・source・試験へ辿れる形にする。
`net lan`・`net startup` がどの文書にも無ければ、[network-console.md](../../../docs/reference/network-console.md)（同じ DOC-55）に足す。

## 2. 読む物

- AGENTS.md §4・§7・§10、[docs/style.md](../../../docs/style.md)。
- 今の文書: `docs/reference/managed-wlan.md`・`docs/reference/network-console.md`。
- source（振る舞いの根拠）: `userland/base/net/main.c`（usage・dispatch・`wifi_store_command`・`wifi_session_command`・`wifi_command`・interactive の `net>`）、
  `userland/base/net/wifi-store.c`・`.h`（利用者ごとの store・system の store・file の mode・所有者・上限）、`wifi-conf.c`・`.h`、
  `userland/base/networkd/managed-wlan.c`・`managed-wlan-state.h`・`wifi-child.c`・`main.c`（状態・30 秒の retry・SO_PEERCRED・session）、
  `managed-lan.c`・`lan-configure.c`（`net lan`）。
- 試験の見当: `grep -rln 'wifi add\|wifi session\|wifi-store' plan/tools tests userland --include=*.c --include=*.sh --include=*.py`。

## 3. 作る物

- `docs/reference/managed-wlan.md` の書き直し（文法・store・session・状態・retry・権限・誤り・例・出典の表）。
- 必要なら `docs/reference/network-console.md` の `net lan`・`net startup` の節。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 4. 確かめの項目

- usage の各形が文書の文法の節と一字一句で合う（option の綴り、既定の値、`-` の意味）。
- store の path・mode・所有者・誰が書けるか、`HOME` を信用しない事、root と一般の利用者の違いを code から確かめて書く。
- 状態の名前（`disabled` など）と遷移、retry の間隔は networkd の定数から。
- 秘密を例に書かない（`REPLACE-WITH-SECRET` の類）。
- 主張ごとの出典の表（file と関数）。試験の無い主張は「試験 無し」と書く。

## 5. 変えてはいけない物

- source・header・試験の道具（文書が source と食い違えば source を正として文書を直し、source の不具合に見えれば結果に Bug の候補として書く）。
- 他の docs（link を直す時を除く）。HAL・toolchain・共有の build。

## 6. 確かめの command

```sh
timeout 120 build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/howto docs/architecture
grep -c 'set-key' docs/reference/managed-wlan.md   # 0
git diff --check
```

## 7. 終わりの条件

上の command が通る。usage の全ての wifi の形が文書にある。commit は
`git commit -m WIP -- docs/reference/managed-wlan.md docs/reference/network-console.md plan/ws009/phase025/phase.md plan/ws009/ws.md`。
結果の節（日付は host の `date`、command と最後の行、自分で決めた点、Bug の候補、未実施）。ws.md の行は「書いた・link PASS」（cleared は Q1）。

## 結果

（未着手）
