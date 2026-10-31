<!-- awesome-plan project=zedbsd record=ws335-p038 -->

# ws335-p038: x509-limbo の全件の照合（path validation の適合）

Status: planned（2026-11-01 p001 が design-review の指摘 23・27 を受けて p020 から分けた。依存: p020）
Parent: [WS335](../ws.md)。担当: Opus（違いの理由の判断が要る）。

- `plan/ws335/tests/unit/test-limbo.c`: `WS335_VECTORS/rec/limbo.vec` の SERVER の 9,792 件を cryc_chain_verify（store に trusted、peer に leaf と untrusted、params に time・peer の名前・max_depth）で流し、期待（SUCCESS・FAILURE）との一致の数を数える。
- 結果の節: 一致の数と、違った件を id の前置き（rfc5280・webpki・bettertls・pathlen・pathological・cve・rfc9881・crl・online・invalid）ごとに数え、理由で分類した表。理由は design.md の決定（D11 の policy の処理なし、D12 の CN なし、trust anchor の basicConstraints と pathLen を見ない、CRL を見ない）か、cryc の欠陥か。
- 欠陥は直す（cryc-chain.c・cryc-name.c・cryc-x509.c。header の変更が要れば Q1 に返す）。trust anchor の制約（limbo の `rfc5280::root-non-critical-basic-constraints` ほか）を見るかは、件数を見てから Q1 に案を返す。
- 終わりの条件: 理由の無い違いが 0。`run-unit.sh limbo all` と `run-unit.sh --win32 limbo`（Windows の build でも同じ件数）が PASS（test-limbo は理由付きの違いを数えて PASS にする: 違いの id の表を試験の中の data として持つ）。

## 結果

（未着手）
