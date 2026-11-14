<!-- awesome-plan project=zedbsd record=ws343 -->

# WS343 base の TLS と ssh を自前に: 既定の image から OpenSSH・OpenSSL・curl・libcurl を外し、base に自前の ssh・sshd・libssl-compat.so・libcrypto-compat.so、fetch の https

- Master: [plan/master.md](../master.md)（2026-11-02 10:52 作成）。Primary Milestone: MG005。
- Status: completed（2026-11-15 Q1。全 Phase cleared。既定の config は自前の ssh・libssl-compat・libcrypto-compat・ca-certificates、fetch の https。回帰の試験は plan/tools/fetch/。CI の config/ci の作り直しは BUG-435）
- 目標（2026-11-14 の実態で書き直した）:
  - 既定の image（`mk/platform-*/defaults.mk`）に OpenSSH・OpenSSL・curl・libcurl を入れない。代わりに自前の ssh・sshd（[WS336](../ws336/ws.md)）と libssl-compat.so・libcrypto-compat.so（[WS335](../ws335/ws.md)）を入れる。— 済み（build system の作り直しで、2026-11-14 に p001 で確かめた）
  - library の名前は libssl-compat.so・libcrypto-compat.so のまま（libssl.so・libcrypto.so の名前は出さない。2026-11-14 ユーザーの決定）。— 済み
  - passkey-fido2・fidoctl を libcrypto-compat に移す。— 済み（p002）
  - base の fetch が libssl-compat で https を使えるようにする（`/etc/ssl/cert.pem` で鎖と名前を確かめる）。— p003（実装と試験は済み、mk の差分の案の承認待ち）
  - `mk/rootfs.mk` の行き先の無い libcurl.so の link を直す。— p003 の差分の案
  - ca-certificates は packages に残し、自前の実装（libssl-compat・ssh・fetch・ブラウザ）が `/etc/ssl/cert.pem` を参照する。— 済み
  - CI の `config/ci/*.mk` と `.github/workflows/ci.yml` の作り直しは [BUG-435](../bugs/BUG-435.md) で扱う（この WS の外）。

## ユーザーの決定

- （2026-11-02 10:52）ユーザーから、上の目標の趣旨の指示を受けた（WS335 の完了の後、互換の ssh・sshd の完成と合わせて）。

- （2026-11-02 20:46）ユーザーから、passkey-fido2・fidoctl（libpasskey の crypto-openssl.c）が使う OpenSSL の関数を libcrypto-compat に足し、link を差し替えるだけで OpenSSL から外せるようにする、という趣旨の指示を受けた（Q1 が出した「libpasskey に libcrypto-compat の内部を直接呼ぶ版を足す」案ではなく）。

- （2026-11-14）ユーザーから、library の名前は libssl-compat.so・libcrypto-compat.so のままでこの WS を締め、libssl.so・libcrypto.so の名前は出さない、という趣旨の決定を受けた（Q1 経由）。

## 関係する WS

- [WS335](../ws335/ws.md)（libssl-compat・libcrypto-compat）、[WS336](../ws336/ws.md)（自前の ssh・sshd）、[WS334](../ws334/ws.md)（Windows 版のブラウザの切り替え）、WS074（ブラウザの net/tls.c、WS335 p036）。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws343-p001 | 調べ（2026-11-14 に今の実態で書き直した）: 既定の config の package、OpenSSL・OpenSSH・curl を使う物、base に入れる名前、行き先の無い link、残りの仕事 | cleared（2026-11-14 Q1。今の実態で書き直した。menuconfig と MAC-T001 の試験は今の build と合わないので完了の条件から外した（Q1 の判断）） | — |
| ws343-p002 | libcrypto-compat に passkey が使う OpenSSL の関数 16 個を足し、passkey-fido2・fidoctl を link の差し替えだけで OpenSSL から外す | cleared（2026-11-02 22:20 Q1、b8c4adaf6 を main に merge 11846c70f、main で test-api-passkey PASS、passkey-fido2・fidoctl の amd64 の build が OpenSSL 無しで warning 0） | WS335 p039 |
| ws343-p003 | fetch の https（libssl-compat、`/etc/ssl/cert.pem`、名前の確かめ、http→https の redirect）と、mk の差分の案（fetch の link、`mk/userland.mk` の順、`mk/rootfs.mk` の libcurl.so の link） | cleared（2026-11-14 Q1、merge fdc6f069e1。ユーザーが fetch/Makefile・mk/userland.mk・mk/rootfs.mk の差分を承認（2026-11-14）、c8a075021f で当てた。host 18・3 機種の build warning 0・QEMU 4 PASS は同じ差分で P8 が確かめた） | p001 |
| ws343-p004 | 規約の全文の見直し（fetch の main.c） | cleared（2026-11-15 Q1、b5d6b3cf39 を main に merge eb3f3dcc28。style-check 21→1（残りは文字列の中の誤検出）、host 18・3 機種の build・QEMU 4 PASS） | p003 |

## Resume point

- 2026-11-15 Q1: completed。Phase の directory は消した（試験は plan/tools/fetch/ に移した、merge 39d511b770）。

- 2026-11-15 P8（q2103-i01）: 試験の始末。fetch の https の回帰（run-fetch-https.sh・https-server.py・run-fetch-qemu.sh・config-amd64-fetch.mk）を [plan/tools/fetch/](../tools/fetch/) に移し、tools.md に登録した。移した後に `sh plan/tools/fetch/run-fetch-https.sh` は `run-fetch-https: PASS (18 cases)`、`GUEST_RUNTIME=build/p8-fetch-guest sh plan/tools/fetch/run-fetch-qemu.sh`（QEMU x86_64、外の network あり）は `run-fetch-qemu: PASS (4 cases)`。今の main で動かない開発の時だけの試験（run-passkey-compare.sh・passkey-compare.c・config-amd64-compat.mk・ct-matrix.sh。ct-matrix.sh の範囲は `plan/ws335/tests/run-ct-matrix.sh api-passkey aes` と同じ）を消し、phase002・phase003・phase004・BUG-435 の inventory の参照に「消した」「移した」と書いた。`plan/ws343/tests/` は空になった。残りは Phase の directory の削除と completed への書き直し（Q1）。

- 2026-11-15 Q1: 全 Phase cleared（p004 merge eb3f3dcc28）。完了の手順: fetch の https の回帰の試験（run-fetch-https.sh・https-server.py・run-fetch-qemu.sh・config-amd64-fetch.mk）を plan/tools/fetch/ に移して tools.md に登録、今の main で動かない開発の時だけの試験（run-passkey-compare.sh・passkey-compare.c・config-amd64-compat.mk・ct-matrix.sh）は消す（Q1 の判断、試験の整理の基準）。その後 completed。

- 2026-11-14 Q1: p001・p003 cleared（merge fdc6f069e1）。ユーザーが mk・Makefile の差分 3 つを承認した（fetch の link、build の順、libcurl.so の link は curl を選んだ時だけ）。残りは p004（規約の全文の見直し）。範囲の外の気づき（libz.so の行き先の無い link、x86_64 の firmware の license の一覧が空、PACKAGE_LINKS が読まれない）は BUG-435 へ。

- 2026-11-14 23:20 P8（q2096-i01）: p001 を今の実態で書き直した。p003 で fetch の https を実装（main.c の書き直し）、host 試験 PASS、x86_64・rpi5・vf2 の build warning 0、QEMU x86_64 で example.com・rfc-editor.org（http→https を含む）の取得と自己署名の拒否 PASS（mk の差分の案を作業の copy に当てて）。次: mk の差分の案 1〜3 の承認と適用（main.c の merge と同時。案 1 が無いと fetch が link できない）、p004。

- 2026-11-14 Q1: 既定の config（`mk/platform-*/defaults.mk`）から openssh・openssl・curl・libcurl は外れ、自前の ssh・libssl-compat・libcrypto-compat・ca-certificates が入っている（作り直しで済んだ）。ユーザーの決定（2026-11-14）: library の名前は libssl-compat.so・libcrypto-compat.so のままで締める（libssl.so・libcrypto.so の名前は出さない）。残り: fetch の https（今は「https is not supported」）、`mk/rootfs.mk` の libcurl.so への link を消す mk の差分の案、CI の `config/ci/*.mk` と workflow の作り直しは BUG-435 で扱う。p001 は今の実態で書き直す。

- 2026-11-02 10:52 Q1: 作成。WS335 の completed と WS336 の ssh・sshd の完成の後に p001 を割り当てる。

## Q1 の判断（2026-11-02 22:20、p002 の merge）

- WS335 の check-skeleton の BASE を b8c4adaf6c（p002 の終わり）に移す。
- WS335 design.md §5 の公開の API の表と evp.h の範囲の note に passkey の部分集合を入れるのは WS335 p035（P1）で行う。
- libpasskey の crypto-openssl.c・crypto.h の comment（OpenSSL's libcrypto）は、OpenSSL 互換の API を呼ぶという意味で正しいので、そのままにする。
- passkey-fido2・fidoctl の実機（または WS172 の loopback の security key の image）での動作は、WS343 の後の Phase で確かめる。
