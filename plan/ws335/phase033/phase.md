<!-- awesome-plan project=zedbsd record=ws335-p033 -->

# ws335-p033: 統合（zedBSD の libssl-compat.so と exports、Windows の static library の source の一覧、相互接続・適合の試験）

Status: planned（2026-11-01 p001 が作成、同日 q1789-i02 で Windows を static link に、libcrypto-compat.so を p039 に分けた。依存: p032・p023・p024・p039）
Parent: [WS335](../ws.md)。担当: Opus（phase-runner-high を推す。推論が多い）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4・§7・§9、[design.md](../design.md) の §3（D23〜D27）・§4.2〜§4.4・§5・§8、[coding-style.md](../../coding-style.md)。
- p039 の結果（libcrypto-compat.so の規則・`sources.mk`・`exports.map`・`check-exports.sh`）。この Phase はそれを変えず、同じ形で libssl-compat を足す。
- `platform/common/userland-dynamic.mk:35-56`（libz-compat と、libz-compat に依る libpng-compat の .so の規則の前例）。
- Windows の build の約束: [plan/ws334/design.md](../../ws334/design.md) §6（llvm-mingw・UCRT）と `apps/browser-win32/Makefile`（`sources.mk` を include して `lib<名前>.a` を作る `WIN32_STATIC_LIBRARY` の形）。WS334 の file は読むだけ。

## 2. 作る物

- zedBSD: `userland/base/libssl-compat/Makefile`・`exports.map`（node `LIBSSL_COMPAT`、`include/libc/compat/ssl/ssl.h` の関数の全部を名前で）、`platform/common/userland-dynamic.mk` の `libssl-compat.so` の規則（`-l:libcrypto-compat.so -l:libc.so`、`check-dynamic-elf.py --needed libcrypto-compat.so --needed libc.so --soname libssl-compat.so`）、package の登録（既定 n、image は `/lib/libssl-compat.so`）、
  `include/libc/compat/ssl/` の sysroot への install（p039 の crypto と同じ道）。`check-exports.sh` で libssl-compat.so も照合する（p039 の script が 2 つの .so を扱えなければ、引数の形を保って足す）。
- `userland/base/libssl-compat/sources.mk`（変数だけ）: `LIBSSL_COMPAT_SOURCES`（全部の .c。OS に依る file は無い）。libcrypto-compat の `sources.mk` は p039 の物を使う。
- Windows の成果物は **static library だけ**（`libcrypto-compat.a`・`libssl-compat.a`）。DLL・import library・`.def` は作らない（D23）。この WS の中に Windows の make の target は作らない（D26: 使う側の build が `sources.mk` を読んで作る）。試験のための Windows の build は試験の script の中で行う（下）。
- 試験:
  - `plan/ws335/tests/tls-client.c`（host・zedBSD・Windows で動く小さな client: 引数の host・port・CA file、`SSL_*` の公開の API だけを使う。Windows では `WSAStartup` と SOCKET を試験の側で）。
  - `plan/ws335/tests/run-interop.sh`（host の `openssl s_server` を版・suite・group・鍵の種類の組で起動し tls-client で繋ぐ。1 本の script で要約だけを出す）。`--win32` で、tls-client.exe を `run-unit.sh --win32` と同じ flag で 2 つの .a に static link し、Wine の上で同じ host の s_server に繋ぐ（組は 1.3 と 1.2 の各 1 suite × ECDSA・RSA。Wine の prefix は `build/tmp/ws335-wine`）。
  - `plan/ws335/tests/run-limbo.sh`（x509-limbo の SERVER 全件を公開の API で流し、一致の数と違いの表。p038 の結果を使う）、`plan/ws335/tests/run-sites.sh`（実の site の一覧、host から、network の無い時は SKIP と書く）。
  - net/tls.c の host の試験を compat で流す script（`plan/ws074/tests/host-loader.c` を変えず、試験の dir に `libssl.so.3 → libssl-compat.so`・`libcrypto.so.3 → libcrypto-compat.so` の symlink を置いて `LD_LIBRARY_PATH` で開かせる）。
- 結果の節、ws.md の行。

## 3. 試験と受け入れ

- `run-unit.sh all`・`run-unit.sh --win32 all`・`run-ct.sh all`・`BASE=NEWBASE check-skeleton.sh` が PASS。
- run-interop.sh: TLS 1.3 × 3 suite × {x25519, P-256（HRR）} × {ECDSA P-256, ECDSA P-384, RSA 2048, RSA 4096} と TLS 1.2 × 6 suite × 同じ鍵、record の断片化（`-max_send_frag`）、非 blocking、server の close_notify の有無。全件 PASS。
- run-interop.sh --win32: 上の部分集合が Wine で PASS。Wine の prefix の current user の "Disallowed" に試験の中間の証明書を入れると、その中間を通る接続が `X509_V_ERR_CERT_REJECTED` で失敗し、`SSL_CTX_set_default_verify_paths` の後に `cert.pem`（試験の CA）を tls-client.exe の隣に置くと `SSL_CTX_load_verify_locations` 無しで繋がる（D14・D24）。
- run-limbo.sh: 期待との不一致の全部に理由（design の決定に基づく）。理由の無い不一致は 0。
- net/tls.c の host の試験が compat で本家と同じ件数で PASS。
- zedBSD: `make $(BUILD)/dynamic/libssl-compat.so` が warning 0、`check-exports.sh` PASS。QEMU（Q1 の割当）の guest で tls-client が host の s_server に繋がる（SSH で操作し結果を読む。console の log で判定しない）。
- Windows: x86_64・aarch64 で 2 つの .a と tls-client.exe が warning 0 で link（aarch64 は link まで）。実の Windows の機械での実行は未実施と書く（WS334 の試験の機械が揃ってから）。

## 4. 注意

- 共有の `platform/common/userland-dynamic.mk` を変えるので、merge の衝突を Q1 に知らせる。toolchain は変えない。共有の build を消さない。
- 実の site の試験は外部に依るので、受け入れは interop と limbo で決め、site は記録に留める。
- p039 の libcrypto-compat.so の規則・export を変えない（変える必要があれば止めて Q1 に）。

## 5. 結果

（未着手）
