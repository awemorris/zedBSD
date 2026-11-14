# zedBSD の ssh・sshd の試験

zedBSD の自前の ssh・sshd・ssh-keygen・sftp-server（`userland/base/ssh`、libcrypto-compat の上）の回帰の試験。
WS336 から移した（2026-11-15）。各 script は tree の root に移ってから動き、出力は `build/tmp/` の下（fresh-out.sh の
新しい run の directory）。判定は最後の行。

## host の試験（ASan・UBSan、host の OpenSSL 3.5 を `host-crypto/` の shim で）

| script | 確かめる事 | 最後の行 |
| --- | --- | --- |
| `run-unit.sh all`（`unit/test-*.c`） | 各 module の単体（buf・cipher・kex・key・auth・channel・packet・transport・config・hostfile・monitor・privsep・sftp・client・ttymodes・tools） | `run-unit: PASS` |
| `crypto-contract.sh`（`crypto-contract.c`） | ssh が使う crypto の関数の約束（OpenSSL 3.5 の interface） | `crypto-contract: PASS` |
| `sshd-pair-host.sh`（`sshd-pair-host.c`・`sshd-pair-client.c`） | 自前の sshd の 1 接続を host の OpenSSH の ssh・sftp と | `sshd-pair-host: PASS` |
| `interop-host.sh`（`sshd-host-shim.c`・`sftp-host.py`・`sftp-fs-stub.c`） | 自前の ssh・sshd・sftp-server と host の OpenSSH の相互の接続、SFTP、fuzz の短い回 | `interop-host: PASS` |
| `keygen-host.sh` | ssh-keygen の鍵の生成・読み・形式 | `keygen-host: PASS` |
| `check-skeleton.sh` | header の単独の compile、crypto の library を呼ぶのが ssh-crypto.c だけ、source に開発の記録が無い、`BASE=<sha>` で library の header が不変、zedBSD の sysroot の header での compile（`build/llvm/bin/clang` と `SSH_ZEDBSD_SYSROOT`、既定 `build/x86_64/sysroot`。無ければその確かめだけ skip） | `check-skeleton: PASS` |
| `fuzz-host.sh`（`fuzz/`・`timing-host.c`） | libFuzzer の 11 target（既定 各 300 万回）、sftp-server の乱数の入力、monitor の時間の差、子の descriptor | `fuzz-host: PASS` |
| `compat-host.sh` | host 用に build した libcrypto-compat の上で contract・unit・pair・interop と package の files | `compat-host: PASS` |
| `build-host.sh OUT [program...]` | 4 つの program の host の build（他の script が使う） | `build-host: PASS` |

## QEMU の試験

`qemu-interop.sh`（`qemu-helper.py`、image の config は `config-amd64-ssh.mk`）: `plan/tools/guest/test-image.sh` で
x86_64 の image を作り（自前の source の warning 0）、boot-test.sh で login prompt、guest の sshd に host の OpenSSH の ssh・scp・sftp
から（key・password・拒否・ControlMaster・-tt・RekeyLimit）、privilege separation（uid 22・/var/empty）、guest の ssh
から host の OpenSSH sshd と自分へ、新しい image で sshd-start が host key を作る事。`QEMU_INTEROP_BUILD`（既定
`build/ssh-interop`、新しい image は別の `<BUILD>-fresh`）、`QEMU_INTEROP_PARTS`。host に sshpass が要る。最後の行 `qemu-interop: PASS`。
