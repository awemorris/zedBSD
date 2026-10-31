<!-- awesome-plan project=zedbsd record=ws335-p033 -->

# ws335-p033: 統合（zedBSD の .so と exports、Windows の DLL、相互接続・適合の試験）

Status: planned（2026-11-01 p001 が作成。依存: p032・p023・p024）
Parent: [WS335](../ws.md)。担当: Opus（phase-runner-high を推す。推論が多い）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4・§7・§9、[design.md](../design.md) の §3・§4.3・§5・§8、[coding-style.md](../../coding-style.md)。
- `platform/common/userland-dynamic.mk:35-45`（libz-compat の .so の規則の前例）、`userland/base/libz-compat/Makefile`・`exports.map`。
- [plan/ws334/design.md](../../ws334/design.md) §6（Windows の build の約束、llvm-mingw、UCRT）。

## 2. 作る物

- zedBSD: `userland/base/libcrypto-compat/Makefile`・`exports.map`（公開の名前は node `LIBCRYPTO_COMPAT`、`cryc_*` は `LIBCRYPTO_COMPAT_PRIVATE`）、`userland/base/libssl-compat/Makefile`・`exports.map`、
  `platform/common/userland-dynamic.mk` の 2 つの .so の規則（`check-dynamic-elf.py` で SONAME と needed を照合）、package の登録（既定 n、image は `/lib/libcrypto-compat.so`・`/lib/libssl-compat.so`）、
  sysroot への header の install（`include/libc/compat/crypto`・`ssl` が sysroot の `/usr/include/compat/` に入るかを確かめ、要れば規則）。
- Windows: `userland/base/libcrypto-compat/sources.mk`・`userland/base/libssl-compat/sources.mk`（変数だけ）、build の script か Makefile（`make libssl-compat-win64`・`libssl-compat-winarm64`、出力 `build/libssl-compat-win64/` に DLL・`.dll.a`・`.a`・`.def`）。一番上の Makefile への target の追加は Q1 と相談（共有の file）。
- 試験: `plan/ws335/tests/tls-client.c`（host と zedBSD で動く小さな client: 引数の host・port・CA file、`SSL_*` の公開の API だけを使う）、`plan/ws335/tests/run-interop.sh`（host の `openssl s_server` を版・suite・group・鍵の種類の組で起動し tls-client で繋ぐ。1 本の script で要約だけを出す）、
  `plan/ws335/tests/run-limbo.sh`（x509-limbo の SERVER 全件を公開の API で流し、一致の数と違いの表）、`plan/ws335/tests/run-sites.sh`（実の site の一覧、host から、network の無い時は SKIP と書く）、
  net/tls.c の host の試験を compat で流す script（`plan/ws074/tests/host-loader.c` を変えず、試験の dir に `libssl.so.3 → libssl-compat.so`・`libcrypto.so.3 → libcrypto-compat.so` の symlink を置いて `LD_LIBRARY_PATH` で開かせる）。
- 結果の節、ws.md の行。

## 3. 試験と受け入れ

- `run-unit.sh all`・`run-ct.sh all`・`check-skeleton.sh` が PASS。
- run-interop.sh: TLS 1.3 × 3 suite × {x25519, P-256（HRR）} × {ECDSA P-256, ECDSA P-384, RSA 2048, RSA 4096} と TLS 1.2 × 6 suite × 同じ鍵、record の断片化（`-max_send_frag`）、非 blocking、server の close_notify の有無。全件 PASS。
- run-limbo.sh: 期待との不一致の全部に理由（design の決定に基づく）。理由の無い不一致は 0。
- net/tls.c の host の試験が compat で本家と同じ件数で PASS。
- zedBSD: `make $(BUILD)/dynamic/libcrypto-compat.so` などが warning 0。QEMU（Q1 の割当）の guest で tls-client が host の s_server に繋がる（SSH で操作し結果を読む。console の log で判定しない）。
- Windows: x86_64・aarch64 の DLL と tls-client.exe が warning 0 で link。実行は Windows の機械が揃うまで未実施と書く。

## 4. 注意

- 共有の `platform/common/userland-dynamic.mk` を変えるので、merge の衝突を Q1 に知らせる。toolchain は変えない。共有の build を消さない。
- 実の site の試験は外部に依るので、受け入れは interop と limbo で決め、site は記録に留める。

## 5. 結果

（未着手）
