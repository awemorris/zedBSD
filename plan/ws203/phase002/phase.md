<!-- awesome-plan project=zedbsd record=ws203-p002 -->
# ws203-p002: 最終規約・build確認

Parent: [WS203](../ws.md)
Status: cleared / normal
Queue: rpi4-genet-20261011-i02 (authorized by same user implementation scope)
Dependencies: p001 source output.
Acceptance: 全変更C/headerをC全文でreview、clang-format19 preview、style-check、git diff --check、ON/OFF warning0 arm64 vmunix build、FDT/MDIO/ring/packet所有権/close/reopenのbounded host model。独自Zlibで参照ソースの複製なし。実機未検証を明示しp003へhandoff。結果/commands/versions/source SHAを記録。

## Clearance / 2026-10-11

Final changed source reviewed against C全文/Guardrail/API boundaries; formatter preview followed by manual canonical formatting. ON/OFF arm64 vmunix builds exit 0/warning0 and image structure checks PASS; production-driver host model passes with ASan/UBSan/LeakSanitizer. Common-menu ON/OFF roundtrip passes for all 6 platforms. Style-check's 9 forward cleanup goto sites manually reviewed as permitted; diff clean. [Commands/results/versions/provenance/limits](../tests/results.md), [source SHA256](../tests/source.sha256). Scoped i02 cleared. No physical success inferred; p003 remains planned. No shared projection or remote closure performed.
