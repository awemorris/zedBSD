<!-- awesome-plan project=zedbsd record=ws034p019 -->

# ws034-p019: ca-certificates（`packages/security/ca-certificates`）

Phase ID: `ws034-p019`
Parent: [WS034](../ws.md)
Status: **cleared**（q330-i01、2026-09-23）
Phase disposition: normal
Queue: q330（q330-i01）
実行: メインセッション

## 範囲

TLS の検証に使う信頼 root を入れる。inventory §7 の既定案 (a) に従い、curl が配布する PEM
（`cacert-2026-08-13.pem`、121件）を**そのまま** `/etc/ssl/cert.pem` に置く。
WS032 の OpenSSL は `--openssldir=/etc/ssl` なので、OpenSSL の既定の CA file がこの名前になる。

## 入れたもの

| 場所 | 内容 |
| --- | --- |
| `userland/packages/tools/archive.sh` | root に `-` を渡すと「archive でない1ファイル」として扱う。size と SHA-256 だけを確かめ、tar の member 検査はしない。`extract` は拒否する |
| `userland/packages/external.mk` | `ZEDBSD_EXTERNAL_FILE`（1ファイルの配布物）。取得・検証・`<name>-download` を作り、展開と patch は無い |
| `userland/packages/security/ca-certificates/` | `Makefile`（版・URL・size・SHA-256、更新の手順）と `MPL-2.0.txt`（証明書データのライセンス本文）。image には `/etc/ssl/cert.pem`（0644）と `/usr/share/licenses/ca-certificates/MPL-2.0.txt` |
| `plan/ws034/tests/config-amd64-ca.mk` | 試験用 config（userland の試験 config に openssl・ca-certificates を足す） |

## 検証

| 検証 | 結果 |
| --- | --- |
| 配布元の SHA-256 | curl が公開する `.sha256` と一致（p001） |
| **別系統との突き合わせ** | Debian `ca-certificates_20260816`（署名付き `.dsc` で検証済み）の `mozilla/certdata.txt` から、`CKA_TRUST_SERVER_AUTH = CKT_NSS_TRUSTED_DELEGATOR` の root を取り出し、証明書の DER の SHA-1 で比べた。**121件すべて一致、片方にしか無いものは0件** |
| 改竄の検出 | 1 byte 足したもの: size 不一致で拒否。同じ大きさで1 bit 変えたもの: SHA-256 不一致で拒否。`extract` に `-` を渡すと拒否 |
| 権限 | 取得したファイルは 0600 のまま（fetch の一時ファイルの権限）なので、image では `--mode` で 0644 にした。これが無いと root 以外は CA を読めない |
| ゲスト（QEMU、KVM）で OpenSSL | `openssl version -d` は `/etc/ssl`。host で取った curl.se の実際の chain（leaf → Let's Encrypt YR2 → ISRG Root YR → ISRG Root X1）を、CA を指定せずに `openssl verify` して **OK**。CA を空にすると失敗（exit 1）。bundle の121件すべてを OpenSSL が読める |

証拠: `evidence/`（ゲストの出力、chain の subject・期限・fingerprint）。

## 残したこと

- `/etc/ssl/certs`（hash 名の directory）は作っていない。OpenSSL・curl・wget は CA file で足りる。
- curl と wget からの利用（`--with-ca-bundle=/etc/ssl/cert.pem`）は ws034-p017・p020 で確かめる。
- 更新は Makefile の冒頭に書いた手順で行う。自動の更新は無い。
