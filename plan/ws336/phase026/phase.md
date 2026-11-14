<!-- awesome-plan project=zedbsd record=ws336-p026 -->

# ws336-p026: 試験の C の規約の全文の見直し

Status: 実装済み・試験 PASS（2026-11-15 P2 q2094。cleared は Q1 が決める。p025 の残りを 2026-11-14 に Q1 が分けた）
Parent: [WS336](../ws.md)
担当（推し）: conformance-reviewer
依存: p025

## 目的

`plan/ws336/tests/` の C（unit・fuzz・driver・shim）を `plan/coding-style.md` の全文と §14 の checklist に合わせる。書き方だけを直し、試験の中身（期待の値・場面）は変えない。

## 範囲

p025 の機械の検査で残った物（[p025 の phase.md](../phase025/phase.md) の「残り」に file ごとの件数と再開の手順）: paragraph comment 464、条件の中の呼び出し 141、forward declaration 61、conditional 51、入れ子の宣言 13、comment の形 1。purpose comment は 1 つずつ中身を読んで書く。

## 確かめ

- 見直しの前後で `sh plan/ws336/tests/run-unit.sh all`・`sshd-pair-host.sh`・`interop-host.sh`・`BASE=4ef0ba4b28 SSH_ZEDBSD_SYSROOT=<自分の build の sysroot> sh plan/ws336/tests/check-skeleton.sh`・`crypto-contract.sh` が PASS、`git diff --check` が空。
- 機械の検査の残りが 0、または規約が許す形（前向きの 1 つの cleanup の goto など）だけである事を数で示す。

## 結果（P2 q2094、2026-11-15 00:0x）

状態: 実装済み・試験 PASS（cleared は Q1 が決める）。直したのは `plan/ws336/tests/` の C の書き方だけ（37 file、+2188 -334 行）。期待の値・場面・試験の数は変えていない。`userland/base/ssh/` は触っていない（header の基点は `4ef0ba4b28` のまま）。

### 機械の検査（`python3 plan/tools/style-check.py plan/ws336/tests/*.c plan/ws336/tests/unit/*.[ch] plan/ws336/tests/fuzz/*.c`）

| 規則 | 前 | 後 |
| --- | --- | --- |
| paragraph-comment（段落の前の purpose comment） | 464 | 0 |
| call-in-condition（条件の中の呼び出し） | 141 | 0 |
| forward-declaration（static 関数の前向き宣言） | 61 | 0 |
| conditional（`?:`） | 51 | 0 |
| nested-declaration（入れ子の宣言） | 13 | 0 |
| comment-form | 1 | 0 |
| 合計 | 731 | 0 |

- 前の数は着手時に同じ command で数えた（p025 の phase.md の件数と一致）。後は出力が空（`wc -l` が 0）。
- paragraph comment は 1 つずつ前後の code を読んで書いた。直前の comment が無い段落に足した（関数の先頭の初期化、`if` の判断、後始末、`return`）。自動で入れたのは、`ssh_buf_free(&out)` などの後始末と `ssh_channels_init(&channels)` など定型の行だけ（同じ語の定型の comment）。
- 条件の中の呼び出し（`strcmp`・`memcmp`・`strlen`・`memchr`・`poll`・`connect`・parser の呼び出し・`next_random()`・`fuzz_error_ok` など）は、名前の付いた変数に出して `if` で見た。短絡の順は保った（前の clause が偽なら呼ばない所は、前の `if` の中に入れた）。`sshd-pair-client.c` の case の振り分けの `strcmp` の連なりは、名前の表（`case_names`）と `switch` にした。
- `?:` は `if`・`else` にした。`malloc(size > 0 ? size : 1)` は `allocation` を 1 に直す形にした。
- 入れ子の宣言（`{ ... }` だけの scope と loop の中の変数）は、関数の先頭の宣言にまとめて中括弧を外した。`for` の本体に `if` と `else` を入れた所は中括弧を付けた。
- static 関数 61 に 1 行の前向き宣言を足した（test-auth・test-client・test-config・test-kex・test-transport）。
- `main` の最後の `return check_finish(...)` を `status = check_finish(...); return status;` に直した（18 file）。他の `return f(...)` も 5 か所を直した（`parse_request_of`・`parse_request_wire`・`banner_of`・`parse_spec`・`client_read` ほか）。
- 検査の誤検出: `test-config.c` の `"Include /etc/ssh/sshd_config.d/*.conf\n"` の `/*` を comment の始まりと読むので、`"…/" "*.conf\n"` と文字列を 2 つに分けた（値は同じ）。
- `fuzz-common.c` の `case 0: ...; break;` の 1 行の形を、label と文を別の行にした。

### 確かめた command と最後の行（見直しの前後で同じ。前は p025 の結果と同じ状態から、後は 38722e1bfc）

- `sh plan/ws336/tests/run-unit.sh all` → 前後とも `run-unit: PASS`（全 18 本 PASS）。各 test の check の数は前後で同じ（test-ttymodes 20682、test-channel 1603654、test-hostfile 736329、test-kex 402989 など）。`test-config` は前 1321691・後 1321693 で、差の 2 は前の側を c231c505e8 から取り出した temp directory で走らせた時に `userland/packages/network/openssh/files/ssh_config` が無かったため（`test-config.c` の同じ file を両方で走らせると取り出した側が 1321691、worktree が 1321693）。`test-transport` は走らせるたびに ±1 動く（前 128387〜128389、後 128389）。
- `sh plan/ws336/tests/sshd-pair-host.sh` → `sshd-pair-host: PASS`（前後）。
- `sh plan/ws336/tests/interop-host.sh` → `interop-host: 161 checks, 0 failed` / `interop-host: PASS`（前後）。
- `sh plan/ws336/tests/crypto-contract.sh` → `crypto-contract: PASS`（108 checks。前後）。
- `BASE=4ef0ba4b28 SSH_ZEDBSD_SYSROOT=$PWD/build/x86_64/sysroot sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`（zedBSD の sysroot への compile の 7 番を含む。sysroot は `make CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached` と `... sysroot` で自分の build/x86_64/sysroot に作った。`userland/base/ssh` は触っていないので前後で同じ）。
- `git diff --check` → 出力なし。
- `FUZZ_RUNS=1000000 sh plan/ws336/tests/fuzz-host.sh` → `fuzz-host: PASS`（必須の command ではないが、fuzz の 11 target・sftp-fs-stub・timing-host・sshd-host-shim の build を兼ねて後だけ流した。前は流していない）。

### 残り・未実施

- `CHECK(f(...))`・`CHECK_EQ(f(...), n)` の macro の引数の中の呼び出しは直していない（unit の C に約 1700 か所、機械の検査は検出しない。試験の assert の形で、「`if` の条件の中」ではなく値の引数）。直すなら名前の付いた変数に出す機械的な作業で、次の機会に Q1 が要るかを決める。
- 関数に purpose comment が無い static 関数（短い stub など）と file-scope の変数の comment は機械で検出できず、見た所だけ足した（`sftp-fs-stub.c`・`sshd-pair-client.c` の表）。
- clang-format は host に無く、流していない。
- 消す物（Q1 に頼む）: worktree の `build/`（`build/tmp/ssh-*`・`build/tmp/fuzz-host`・`build/llvm`・`build/x86_64`）。
- 実装が `userland/base/ssh/` にあるので、前回の p025 の「壊す確かめ」は繰り返していない（この Phase は試験の C だけを直した。試験が壊れる事の確かめは、変えた file の期待の値を触っていないので不要と判断）。
