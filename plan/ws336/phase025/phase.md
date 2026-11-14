<!-- awesome-plan project=zedbsd record=ws336-p025 -->

# ws336-p025: 規約の全文の見直し

Status: cleared（2026-11-14 23:1x Q1、04c73da979 を main に merge 6637a37b99。userland/base/ssh の見直し、header の comment の直し（基点 4ef0ba4b28）、試験と 3 機種の build PASS。plan/ws336/tests/ の C の規約の残りは p026 に分けた）
Parent: [WS336](../ws.md)
担当（推し）: conformance-reviewer
依存: p024
WS335 の待ち: 無し

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `plan/coding-style.md` の全文
- この WS が足した全 source（`userland/base/ssh/`、`plan/ws336/tests/` の C）
- 各 Phase の結果の「header の持ち越しの直し」
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- 書き方だけの修正（動作は変えない）。実装の間に見つかった header の comment の誤りの直し（この Phase でだけ header を変えてよい。BASE は新しい SHA になる）
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- `run-unit.sh all`・`crypto-contract.sh`・`interop-host.sh`・`sshd-pair-host.sh`・`check-skeleton.sh`（BASE なし）・p022 の `qemu-interop.sh` が見直しの前後で PASS

## 4. 注意

- 無し
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

HAL・toolchain・共有の `build/`、他の WS の file。動作を変える修正は別の Phase に回す。

## 6. 確かめの command

- `sh plan/ws336/tests/interop-host.sh`
- `sh plan/ws336/tests/run-unit.sh all`
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果（P2 q2089、2026-11-14 23:0x）

状態: 実装済み・試験 PASS（cleared は Q1 が決める）。BASE（header の基点）は `4ef0ba4b28`（この Phase が header の comment を直した commit）に変わる。

### 直した物（書き方だけ。動作は変えない）

`userland/base/ssh/` の 27 file:

- 閉じ括弧の後の空行（coding-style §5）: 241 か所に空行を足した。
- 段落の前の comment（§5）: 約 310 か所に purpose comment を足した（関数の先頭の初期化、`if`・代入の段落、loop の後始末など）。
- 条件の中の関数: `ssh-config.c` の `config_is_blank`・`config_lower`・`memchr`・`memcmp`・`strcmp`（16 か所）、`ssh-hostfile.c` と `ssh-key.c` の `memcmp`（4）、`ssh-packet.c` の `memchr`（1）を名前の付いた変数に出した。blank を飛ばす loop は `config_skip_blanks`・`config_skip_word`・`config_trim_blanks`（`ssh-config.c` の static 関数。forward declaration と comment 付き）にまとめた。結果は同じ（unit・interop で確かめた）。
- header の comment の誤り（この Phase だけ許される）:
  - `ssh-channel.h`: 全 global request を断ると書いた所が、want_reply の no-more-sessions に REQUEST_SUCCESS を返す実装と食い違っていた（2 か所を揃えた）。長さの上限を越えた既知の要求を SSH_CREQ_OTHER と読む事は、header の冒頭に既に書いてある（持ち越しの 1 件目はこれで済み）。
  - `ssh-privsep.h`: `ssh_mon_put_frame` の EINVAL（列挙外の型・NULL の body）と EMSGSIZE の区別、out に誤りを記録しない事を足した。`ssh_mon_call` は呼ぶ process が SIGPIPE を無視する前提を足した（sshd-main.c・sshd-child.c・sshd-monitor.c・ssh-main.c は既に無視している）。
  - `phase012/phase.md` の試験の一覧: put_frame の上限越えは EMSGSIZE。
- `plan/ws336/tests/` の C 全 file: 閉じ括弧の後の空行（機械の変更。動作なし）。
- 試験を 1 つ足した: `test-hostfile.c` に「最後の byte だけ違う key の @revoked は revoke しない」（`hostfile_same_key` の比較を 1 byte 短くする壊しが見逃された。足した後は FAIL する）。

### 確かめた command と最後の行（見直しの前後で同じ）

- `sh plan/ws336/tests/run-unit.sh all` → `run-unit: PASS`（前後とも。test-transport 128389 checks、test-ttymodes 20682 checks）
- `sh plan/ws336/tests/sshd-pair-host.sh` → `sshd-pair-host: PASS`
- `sh plan/ws336/tests/interop-host.sh` → `interop-host: 161 checks, 0 failed` / `interop-host: PASS`（p024 の log の語の変更の後で初めて流した）
- `SSH_ZEDBSD_SYSROOT=$PWD/build/x86_64/sysroot BASE=4ef0ba4b28 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`（zedBSD の sysroot の compile の 7 番を含む。script の既定の sysroot `/home/awe/zedBSD-claude1/build/amd64/sysroot` は作り直しの後に無く、スキップされる: 古い path。`BASE=0e9b68f948` では header を直したので FAIL する）
- `sh plan/ws336/tests/crypto-contract.sh` → `crypto-contract: PASS`（108 checks）
- `git diff --check` → 出力なし
- `make -j4 CONFIGMK=config/config.{x86_64,rpi5,vf2}.mk build/<機種>/packages/ssh/.stamp`（x86_64・aarch64・riscv64）→ 3 つとも rc 0、log の warning 0（ssh・sshd・ssh-keygen・sftp-server の 4 つが LD まで）。

### 壊す確かめ（実装を git の外で壊し、元に戻して `cmp` で一致）

10 通りを流した（2026-11-14 Q1 が件数を直した。前の記述は 12 通り・11 FAIL）。9 通りは FAIL（見逃し 0）、1 通りは PASS で試験を足した: no-more-sessions の判定を反転、`ssh_mon_put_frame` の上限の比較を 1 ずらす、`ssh_packet_take_line` の NUL の確かめを反転、private key の magic の確かめを反転、同じく magic の比較を 1 byte 短く、`config_skip_blanks` の停止の条件を反転、`config_trim_blanks` の添字を 1 ずらす、subsystem の名前の比較を反転、base64 の右 shift を 1 ずらす、`hostfile_same_key` の比較を 1 byte 短く（最初は PASS: 試験を足した後は FAIL）。全部 `RESTORED-OK`（`cmp` と `git diff --quiet`）。

### 残り（次の担当へ）

- `plan/ws336/tests/` の C の規約の違反（機械の検査 `plan/tools/style-check.py`）が残る: paragraph-comment 464・call-in-condition 141・forward-declaration 61・conditional 51・nested-declaration 13・comment-form 1（試験の code。動作に関わらない書き方）。試験は暗い bytes を組む短い列が多く、purpose comment は 1 つずつ内容を読んで書く必要があるため、この Phase では直していない。次の担当が file ごとに直せる（`python3 plan/tools/style-check.py --summary plan/ws336/tests/*.c plan/ws336/tests/unit/*.[ch] plan/ws336/tests/fuzz/*.c`）。
- `userland/base/ssh/` の機械の検査の残り: goto 109（`cleanup`・`done` への前向きの 1 つの label だけ。規約が許す形）、call-in-condition 8（`S_ISDIR`・`S_ISREG`・`S_ISCHR`・`WIFEXITED`・`WIFSIGNALED` の macro。関数でない）、conditional 5（`sshd-monitor.c` の `config->x ? "yes" : "no"` の対称な 2 択と `1U : 0U`）、name 3（`ssh-transport.h` の `version_local`・`kexinit_local` と `ssh-transport.c` の `version_local`: 「自分の側」の意味の名前で、struct の member の改名は header の変更になるため直さない）。
- clang-format はこの host に無く、流していない（未実施）。
- p022 の `qemu-interop.sh`・`replace-check.sh`・`qemu-*.sh` は古い build の前提（`ZEDBSD_CONFIG`・`config/ci/*`・`platform/`）で、大きな書き直しになるため流していない（未実施、p024 の Q1 の結果と同じ理由。Pi 5 の実機の相互運用は p024 で確かめ済み）。QEMU・実機は使っていない。
- `fuzz-host.sh` は monitor の log だけの変更なので必須でなく、背景で流している間に結果をここに書けなかった場合は未実施として扱う。
- 消す物（Q1 に頼む）: worktree の `build/`（`build/tmp/ssh-*.log`・`sysroot-*.log`・`ssh-unit`・`ssh-skeleton`・`sshd-pair` の run directory、`build/llvm`・`build/x86_64`・`build/rpi5`・`build/vf2` など）。

## 持ち越し（Q1）

- 2026-11-01 p010: `ssh-channel.h` の comment の食い違い 2 件を直す（長さの上限を越えた既知の要求は EINVAL でなく SSH_CREQ_OTHER と読む事を phase.md と揃える。ssh_channel_global_request の「no-more-sessions を守る」と「全部の global request を REQUEST_FAILURE で断る」の矛盾: 実装は want_reply の no-more-sessions に REQUEST_SUCCESS）。
- 2026-11-01 p012: phase.md §3 の put_frame の上限越え（EINVAL）を header（EMSGSIZE）に揃える。mon_call を呼ぶ process が SIGPIPE を無視する前提を header の comment に書く（p016・p017 も守る）。
