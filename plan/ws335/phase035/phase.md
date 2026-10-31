<!-- awesome-plan project=zedbsd record=ws335-p035 -->

# ws335-p035: 規約の全文の見直しと header の持ち越しの直し

Status: planned（2026-11-01 p001 が作成。依存: p034）
Parent: [WS335](../ws.md)。担当: conformance-reviewer（他に作業が無い時、Q1 が割り当てる）。

- WS335 が足した C の全部（`userland/base/libcrypto-compat/`・`userland/base/libssl-compat/`・`include/libc/compat/crypto/`・`include/libc/compat/ssl/`・`plan/ws335/tests/`）を [coding-style.md](../../coding-style.md) の全文と照らし、書き方だけを直す（動作は変えない）。
- 実装の間に見つかった header の comment の誤り（各 phase.md の結果と design-review.md に記録された物）をここで直す。直した後は基点の SHA が変わるので、Q1 が新しい基点を ws.md に書く。
- 受け入れ: style-check の違反 0、run-unit.sh all・run-ct.sh all・check-skeleton.sh・統合の試験の件数が前と同じ。

## 結果

（未着手）

## 持ち越し（Q1）

- 2026-11-01 p002: `cryc-hash.h` の HKDF（extract・expand）と HMAC の init の comment に、NULL の pointer と 0 でない長さの組が EINVAL である事を書く（今は ENOTSUP だけを約束）。
- 2026-11-01 p002: `plan/ws335/tests/run-unit.sh --win32` が新しい Wine の prefix を作る時、この host では `wineboot -i` が kernel32.dll を読めず止まる（担当は ws334-p019 の prefix を写して回避）。prefix の作り方を直すか、出来た prefix を写す手順を script に入れる。

