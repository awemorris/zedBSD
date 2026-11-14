<!-- awesome-plan project=zedbsd record=ws343-p001 -->

# ws343-p001 調べ: 今の build の既定の package、OpenSSL・OpenSSH・curl を使う物、base に入れる名前、残りの仕事

- 親: [ws.md](../ws.md)。Queue: q2096-i01（P8、worktree `/home/awe/zedBSD-worktrees/p8-ws343`、branch `agent/p8-ws343`、基点 main c231c505e8）。
- Status: cleared（2026-11-14 Q1）

## 目的

2026-11-02 に立てた p001（CI の config ごとの package の依存、soname、fetch の https、ca-certificates、menuconfig と MAC-T001）は、
2026-11-06 の build system の作り直しより前の前提で書かれていた。今の build と code で何が済み、何が残るかを確かめ、ws.md の目標と Phase の表を今の形に直す。

## 確かめた事（2026-11-14、main c231c505e8）

| 項目 | 今の実態 | 確かめ方 |
| --- | --- | --- |
| 既定の config | `mk/platform-x86_64・rpi5・vf2/defaults.mk` に ssh（自前、ssh・sshd・ssh-keygen・sftp-server）・libcrypto-compat・libssl-compat・ca-certificates・fetch が入り、openssh・openssl・curl・libcurl は入っていない | `grep` で defaults.mk を見た |
| library の名前 | libssl-compat.so・libcrypto-compat.so（`/lib`）。libssl.so・libcrypto.so の名前は出さない（2026-11-14 ユーザーの決定、ws.md） | `userland/base/libssl-compat/Makefile` |
| 自前の program の TLS | libbrowser・mailer・browser-win32 は compat を直に呼ぶ（WS335 completed）。passkey-fido2・fidoctl は libcrypto-compat に link（p002）。ssh は libcrypto-compat | 各 Makefile の require |
| fetch | `userland/base/fetch/main.c` は https を断っていた（「https is not supported」） | source |
| ca-certificates | external の package、`/etc/ssl/cert.pem` に置く。libcrypto-compat の既定の CA file も `/etc/ssl/cert.pem`（`cryc-os.h`） | Makefile と header |
| 行き先の無い link | `mk/rootfs.mk` の `ROOTFS_PACKAGE_LINKS` が無条件に `/usr/lib/libcurl.so=libcurl.so.4` を作る（curl が無いので行き先が無い）。同じ形で `/usr/lib/libz.so=libz.so.1` も zlib を選ばない既定の image では行き先が無い（この Queue の外、Q1 に報告） | `mk/rootfs.mk` 76〜79 行 |
| package の PACKAGE_LINKS | curl・zlib の package の Makefile の `PACKAGE_LINKS` は今の `mk/rootfs.mk` が読まない（作り直しの前の名の残り。curl を選んでも libcurl.so の link は package からは来ない） | `grep PACKAGE_LINKS` |
| OpenSSL を要る外部の package | openssh・curl・python3（`userland/packages/lang/python3`）。どれも既定に入らない | `grep security/openssl` |
| CI の config | `config/ci/*.mk` の 9 個が openssl・openssh・curl を名指すが、`config/ci/` の全部が作り直しの前の形（BUG-435 の inventory）。BUG-435 で扱う | `grep` |

## 決まった事・残り

- 残りは fetch の https と、mk の差分の案（libcurl.so の link、fetch の link）の 2 つ。p003 で行う。
- CI の config と `.github/workflows/ci.yml` の作り直しは BUG-435（この WS の外）。
- 旧 p001 の「menuconfig と MAC-T001 の試験」は今の build の形と合わない（menuconfig は作り直しで変わった）。この WS の完了の条件から外し、要るなら新しい WS で扱う（Q1 の判断を待つ）。
