<!-- awesome-plan project=zedbsd record=ws336-p023 -->

# ws336-p023: 他の arch の build

Status: cleared（2026-11-03 Q1、ws.md の表のとおり）。前: in-progress（2026-11-02 23:27 P6 q1998。2026-11-02 23:35 実装済み・試験 PASS、cleared は Q1）。前: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner-low（Sonnet 5.5 high）
依存: p021
WS335 の待ち: **WS335 の実装**（libcrypto-compat が各 arch で build できる事）

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `plan/ws336/design.md` §10
- `plan/guardrail.md` の「userland/base の program の有無」（arch で有無を分けない）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- aarch64（config-rpi4.mk）・riscv64（config-visionfive2-keiland.mk）・armv7（config-zybo.mk）の試験の config（`plan/ws336/tests/config-<arch>-ssh.mk`、openssh を自前に替える）と、その build の結果
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- 各 arch の `$(BUILD)/bin/ssh`・`sshd`・`ssh-keygen`・`sftp-server` の build（警告 0）と ELF の検査

## 4. 注意

- link が通らない時は runtime・libc の側で直す（Guardrail）。libcrypto-compat の側の問題は Q1 に返す。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての library の header、HAL・toolchain・共有の `build/`、既定の config、他の WS の file。

## 6. 確かめの command

- 各 arch の個別の target の make（警告 0）
- `sh plan/ws336/tests/run-unit.sh all`
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

2026-11-02 23:35（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。P6、worktree `p6-ws336`（branch `agent/p6-ws336`、基点 main a212b29ea9）。ssh と libcrypto-compat の source、header・HAL・toolchain・既定の config・共有の `build/` は変えていない（共有の sysroot は読み取り専用の symlink で使った。armv7 の sysroot は古く `compat/crypto` の header が無かったので、Q1 が `make sysroot-armv7` で作り直した）。

### 作った物

- 試験の config 3 つ: `plan/ws336/tests/config-arm64-ssh.mk`（`config/ci/config-rpi4.mk`）・`config-riscv64-ssh.mk`（`config-visionfive2-keiland.mk`）・`config-armv7-ssh.mk`（`config-zybo.mk`）。各々 openssh・openssl を外し、`libcrypto-compat ssh sshd ssh-keygen sftp-server` を足す。
- `platform/armv7/userland.mk`: armv7 には `libcrypto-compat.so` の規則も ssh の規則も無かった（platform/common/userland-dynamic.mk は LP64 の 3 つだけが include する）ので足した。
  - `$(DYNAMIC_DIR)/libcrypto-compat.so`: userland-dynamic.mk と同じ形（`-soname libcrypto-compat.so --hash-style=both -Bsymbolic-functions -z defs -z now -z relro -z separate-code -z nodelete`、`--version-script=exports.map`、`-l:libc.so`、`check-dynamic-elf.py --role shared-library --needed libc.so --soname libcrypto-compat.so`）。加えて、run-time ABI の helper（`__aeabi_*`）を armv7 の `libbuiltins-pic.a` から取り `--exclude-libs ALL` で外に出さない（ld.so と同じ）。
  - `ARMV7_SSH_PROGRAM`: 4 つの program（crt1・自分の object・`-l:libcrypto-compat.so -l:libc.so`、`check-dynamic-elf --needed libcrypto-compat.so --needed libc.so`）。
- arm64・riscv64 は p021 の共通の規則（userland-dynamic.mk の `DYNAMIC_SSH_PROGRAM`）で link され、規則の変更は要らなかった。

### 実行した command と最後の行

- `make -j8 ZEDBSD_CONFIG=plan/ws336/tests/config-<arch>-ssh.mk BUILD=build/p023-<arch> build/p023-<arch>/dynamic/libcrypto-compat.so build/p023-<arch>/bin/{ssh,sshd,ssh-keygen,sftp-server}`:
  - arm64（aarch64）: rc 0、compiler の warning 0、OpenSSL の行 0。
  - riscv64: rc 0、warning 0、OpenSSL 0。
  - armv7（ELF32 ARM）: rc 0、warning 0、OpenSSL 0。
- 各 arch の 4 つの program: `check-dynamic-elf.py --machine <aarch64|riscv64|armv7> --role application --needed libcrypto-compat.so --needed libc.so` を通り、`llvm-readelf -d` の NEEDED は 2 つ（libcrypto-compat.so と libc.so）だけ。
- 各 arch の `libcrypto-compat.so`: `check-dynamic-elf --role shared-library --needed libc.so --soname libcrypto-compat.so` を通り、`sh plan/ws335/tests/check-exports.sh` → `check-exports: libcrypto-compat.so public 78, cryc_ 190, defined 268` / `check-exports: PASS`（3 つとも）。
- `sh plan/ws336/tests/run-unit.sh all` → `run-unit: PASS`
- `BASE=0e9b68f948 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`
- `git diff --check` → 出力なし

### わざと壊す確かめ

`build/tmp/p023-mut/mut.py`: platform の makefile の 1 か所を一時に変えて `make -W tools/build/check-dynamic-elf.py`（全ての link の規則の前提）で link し直し、写しから戻して `cmp`（新しい時刻）、もう一度 link し直す（6 回とも rc 0）。

- FAIL（5）: armv7 の program の link から libcrypto-compat を外す、armv7 の program の検査の needed を別の名前にする、armv7 の libcrypto-compat の soname を変える、共通の規則（arm64）の link から libcrypto-compat を外す、共通の規則の needed を libcrypto.so にする。
- 生き残り（1、同値）: armv7 の libcrypto-compat の link から builtins の archive を外す。libcrypto-compat が呼ぶ 6 つの `__aeabi_*` は今は libc.so も export している（libc が自分で使う 19 個）ので link は通る。archive は libc の export の一覧に依らないための物で残す。

### 自分で決めた点

- armv7 の libcrypto-compat は ABI の helper を自分の中に持つ（libc の export に依らない）。
- 試験の config は既定の config を include して program の一覧だけを替える（既定の config は変えない）。

### 未実施・残り

- QEMU（arm64・riscv64 の virt）での実行と ZYBO の実機は、この Phase の項目に無いので流していない。ZYBO の root（`platform/armv7/rootfs.mk` の `ARMV7_ROOT_COMMANDS`）に自前の ssh を入れる事（OpenSSH の時の `chmod mkdir mv` の追加と同じく sshd-start が要る command も）は p024 の入れ替えで。
- rpi5・visionfive2（keiland でない物）・riscv64-virt の config での build は、同じ arch の規則なので流していない。
- 規約の全文の見直しは p025（C の変更は無い）。
- 消す物（Q1 に頼む）: `build/p023-arm64/`・`build/p023-riscv64/`・`build/p023-armv7/`・`build/p023-rpi4`（無ければ無し）、`build/tmp/p023-*`、worktree の `build/{amd64,arm64,riscv64,armv7}/sysroot`（共有への symlink、要らなければ）。
