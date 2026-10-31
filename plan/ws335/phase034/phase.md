<!-- awesome-plan project=zedbsd record=ws335-p034 -->

# ws335-p034: 安全の review と hardening

Status: planned（2026-11-01 p001 が作成。依存: p033）
Parent: [WS335](../ws.md)。担当: Opus（敵対的な review。実装の担当とは別の起動）。

## 1. 読む物

- [design.md](../design.md) §6・§8、全部の header（約束）、全部の .c。RFC 9325（BCP 195）、RFC 8446 の付録 C・E（実装の注意と安全の考察）、FIPS 203 の 3.3（実装の要件）。

## 2. 行う事

- 定数時間: run-ct.sh を全部の秘密の道（AES・GCM・ChaCha20/Poly1305・HMAC/HKDF・X25519・P-256 の scalar 倍と ECDH・Ed25519 の署名・ML-KEM の decaps・record の open・Finished の比較・鍵の schedule）に。足りない ct-*.c を足す。-O2 と -O3、cc と clang の両方。
- fuzz: libFuzzer（clang `-fsanitize=fuzzer,address,undefined`）で、DER・X.509・PEM・ServerHello・EncryptedExtensions・Certificate・ServerKeyExchange・record の open・conn の受信の全体・ML-KEM の decaps と ek の import。各 1 時間（夜の長い fuzz はユーザーに確かめてから）。corpus は `build/tmp/` に。
- review の checklist: 秘密の消去の漏れ（全部の return の道）、整数の overflow（size_t の加算・乗算）、長さの照合の漏れ、例外の点（無限遠・全零）、乱数の失敗の道、alert の選び方、thread の安全（SSL_CTX・store・誤りの queue）、Windows の道（UTF-8 と UTF-16、store の handle の解放）。
- 見つけた欠陥は直す（header の変更は Q1 に返す）。直した物ごとに試験を足す。

## 3. 受け入れ

- 全部の試験と fuzz が PASS（crash 0）、ct の違反 0、review の指摘の全部が直されたか理由付きで見送り（結果の表）。

## 4. 結果

（未着手）
