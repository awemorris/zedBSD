<!-- awesome-plan project=zedbsd record=ws335-p037 -->

# ws335-p037: 試験の相手（scripted な TLS server）と試験の側の署名器（試験の道具）

Status: planned（2026-11-01 p001 が design-review の指摘 1・12 を受けて足した。依存: p025・p026・p027・p011・p008）
Parent: [WS335](../ws.md)。担当: Opus（試験の道具の設計を含む。phase-runner を推す）。p029・p030・p031・p032 の試験が使う。

## 1. 読む物

- [design.md](../design.md) §8・§9、[design-review.md](../design-review.md) の 1・12。
- `userland/base/libssl-compat/sslc-tls.h`・`sslc-buf.h`・`sslc-record.h`・`sslc-keysched.h`・`sslc-msg.h`（定数と構造体）。
- 仕様（`/home/awe/zedBSD-claude1/build/sources/specs/tls/`）: rfc8446.txt（4・5・7、4.4.3 の署名の内容）、rfc5246.txt（7.4）、rfc8422.txt（5.4）、nist-fips-186-5.pdf（6.4.1 ECDSA の署名、印刷の頁 N は PDF の N+9）。

## 2. 作る物

- `plan/ws335/tests/unit/tlspeer.h`（試験の道具。p029 以降の Phase は変えない）:
  - 試験の側の ECDSA P-256 の署名器（FIPS 186-5 6.4.1、試験の固定の k、cryc_ecp と cryc_bn の検証済みの部品を使う。署名は p011 の確かめに通ること）。RSA の秘密鍵は持たない（RSA の server の試験は p033 の OpenSSL の s_server で）。
  - TLS 1.3 の server の flight の組み立て（ServerHello・HelloRetryRequest・EncryptedExtensions・Certificate・CertificateVerify・Finished、NewSessionTicket・KeyUpdate）と、
    TLS 1.2 の ECDHE の flight（ServerHello・Certificate・ServerKeyExchange・ServerHelloDone・CCS・Finished）。message は RFC の構文から試験の側で書く（sslc-msg の writer を使わない）。
    鍵の schedule と record の保護は p026・p027 の検証済みの関数を使ってよい（RFC 8448 で照合済み）。
  - client の ClientHello を読んで中身（random・session id・share・拡張）を返す最小の parser（試験の側で、sslc-msg を使わない）。
  - 壊し方の印（tag の 1 bit、Finished の 1 byte、unsolicited な拡張、downgrade の印、2 回目の HRR、ServerHello と EE の同じ record）。
  - 試験の証明書: derwriter.h で作る ECDSA P-256 の root と leaf（SAN に host 名、固定の有効期限）。
- `plan/ws335/tests/unit/test-tlspeer.c`: 道具そのものの試験（作った CertificateVerify が cryc_signature_verify で通る、作った record が sslc_record_open で開く、1.3 の Finished が sslc_tls13_finished と一致）。
- 結果の節、ws.md の行。

## 3. 終わりの条件

- `sh plan/ws335/tests/run-unit.sh tlspeer all` が PASS。check-skeleton.sh（BASE なし）PASS。commit。
- p029〜p032 の phase.md が使い方を読めるよう、tlspeer.h の冒頭に API の説明（関数ごとの comment）。

## 結果

（未着手）
