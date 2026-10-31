<!-- awesome-plan project=zedbsd record=ws336-design-review -->

# WS336 p001 design-reviewer の記録（2026-11-01）

- 対象: worktree `/home/awe/zedBSD-worktrees/p5-ws336` の ca4805d47（design.md・header 16 本・骨格・phase002〜021・試験の道具・user-questions.md）。
- reviewer: design-reviewer（読むだけ。scratchpad で crypto-contract.c を host の OpenSSL 3.5.6 で build して 108 checks PASS、全 .c を cc と clang で警告 0 を確かめた）。
- 結果: 32 件（高 3・中 14・低 15）。全部を反映した（見送り 0）。反映の後に `check-skeleton.sh`・`run-unit.sh all`（test-tools 13 checks）・`crypto-contract.sh`（108 checks）・`git diff --check` が PASS。

| # | 重さ | 指摘（要点） | 扱い |
| --- | --- | --- | --- |
| 1 | 高 | pre-auth・session の process が fork で host key の seed と listener の descriptor を受け継ぐ | 反映。子は fork と execve（sshd 自身を `SSHD_EXEC_PREAUTH`・`SSHD_EXEC_SESSION`）で始め、設定は monitor の `SSH_MON_CONFIG` で受ける。`struct sshd_context` から seed を外し公開の blob だけにした。descriptor は全部 close-on-exec、子は `closefrom`（sshd.h、ssh-privsep.h、design D8・§6.2） |
| 2 | 高 | 上の layer の送りを溜めるのが「自分が始めた交換」だけで、相手の re-key の間に送ってしまう（RFC 4253 §7.1） | 反映。自分の KEXINIT から自分の NEWKEYS まで、誰が始めても溜める（ssh-transport.h、D15、p008 の試験） |
| 3 | 高 | 未知・拒む global request と channel request の末尾の data、長い名前を EINVAL にしそう（OpenSSH の hostkeys-00、-X・-A） | 反映。名前と want_reply だけを読み残りを見ない、長い名前・値は SSH_CREQ_OTHER（ssh-channel.h、D16、p010 の試験） |
| 4 | 中 | 1 時間に 64 回の署名の上限が長い高速の転送を切る | 反映。session は間隔（`SSHD_SIGN_INTERVAL_MS` 1 秒、早い要求は待つ）、pre-auth は合計 `SSHD_PREAUTH_SIGN_MAX` 4（sshd.h） |
| 5 | 中 | global request を送る関数と、known_hosts の行を消す関数が無い | 反映。`ssh_channel_put_global_request`、`ssh_known_hosts_remove` |
| 6 | 中 | CLOSING の channel への request で接続ごと落ちる（RFC 4254 §5.3） | 反映。CLOSING でも data・EOF・request を読み、program が捨て FAILURE（ssh-channel.h） |
| 7 | 中 | 固定の遅れでは存在しない user の早い返りを隠せない | 反映。応答を要求から `SSHD_FAIL_DELAY_MS` 以上後に揃え、存在しない・locked の user にも dummy の hash で crypt（sshd.h「Timing」、design §8、p016・p020 の試験） |
| 8 | 中 | cipher の試験が header を読んだ同じ人の組み立てと比べるだけで循環 | 反映。draft-ietf-sshm-chacha20-poly1305-04 の Appendix A を既知の答えの試験に（p004、design §1） |
| 9 | 中 | 道具と Phase の許可の食い違い（program の内部の header が BASE の照合で FAIL、test-monitor が link できない） | 反映。BASE の照合は library の header（`ssh-*.h`、`ssh-client-*.h` を除く、と `sshd.h`）だけ。run-unit.sh は test の ` * run-unit-sources:` の行で program の file と `userland/base/login/verify.c` を足せる。`ssh-client-*.c` は既定の link から外した |
| 10 | 中 | root なしの mode が 1 process で、privsep が p019（QEMU）まで試されない | 反映。root なしでも 3 process・socketpair・message は同じで chroot と ids の変更だけを省く（sshd.h、design §6.4、p017 の `sshd-pair-host.sh`） |
| 11 | 中 | EMSGSIZE が「大きすぎ（致命）」と「出力が満ちた（待つ）」の 2 つの意味、held の上限が無い | 反映。満ちた時は ENOBUFS（ssh-common.h に意味を足した）、大きすぎは EINVAL、`SSH_TRANSPORT_HELD_MAX` 4 MiB（ssh-transport.h・ssh-packet.h） |
| 12 | 中 | X25519 の全 0 の確かめが library 任せ | 反映。derive が通っても K_CL を `ssh_ct_equal` で 0 と比べて EINVAL（ssh-crypto.h、p003） |
| 13 | 中 | p019 まで zedBSD の header で compile しない（glibc だけの口に流れる） | 反映。`check-skeleton.sh` の 7 番目: `build/llvm/bin/clang --target=x86_64-unknown-zedbsd` と main の checkout の sysroot（読むだけ）で全 .c を compile（今は全部 PASS） |
| 14 | 中 | p016・p019 が 1 回の起動には大きい | 反映。p016 を monitor と login（p016）・pre-auth と session（p017）・listener と相互運用 B（p018）に、p019 を package と amd64 の build（p021）・QEMU（p022）・他の arch（p023）に分けた。以後の番号を振り直した（p002〜p025） |
| 15 | 中 | MaxStartups の仕組みが無い | 反映。monitor ごとの pipe（login を許したら 1 byte）と SIGCHLD（sshd.h） |
| 16 | 中 | authorized_keys を root が読む（FIFO・device で止まる、確かめと open の競争） | 反映。monitor が fork した user の ids の子が `O_NONBLOCK|O_NOFOLLOW` で開き、開いた descriptor を fstat で確かめ pipe で渡す（sshd.h） |
| 17 | 中 | session の descriptor が shell に漏れる（署名の窓口・記録の書き込み） | 反映。close-on-exec を全部に、子に渡すのは pty か pipe だけ（sshd.h、p017・p020 の試験） |
| 18 | 低 | design §2.3 の verify.c の行番号の誤り | 反映（:34〜71・:75〜111・:105） |
| 19 | 低 | SFTP の SYMLINK の引数の順（OpenSSH は draft と逆） | 反映（ssh-sftp.h、OpenSSH PROTOCOL §4.1、p019） |
| 20 | 低 | p007 が OpenSSH の source（myproposal.h）を試験の出典にしている | 反映。`openssh-10.5p1-ssh_config.5` の既定の一覧に替えた |
| 21 | 低 | p004 の「最小 16 bytes」が受けで強いる誤りを招く | 反映。送り手の規則と書き、鍵ありの NEWKEYS が packet_length 8 になる事を書いた |
| 22 | 低 | EXT_INFO（7）と交換の外の 30〜49 の扱いが無い | 反映。EXT_INFO は捨てる、段に合わない 21・30〜49 は EPROTO（ssh-transport.h） |
| 23 | 低 | monitor の frame の秘密の型が名指されていない | 反映。読み手は常に secret の buffer、PASSWORD_LOGIN・AUTH_DONE・SESSION_START が秘密（ssh-privsep.h）。型の番号を詰め直した |
| 24 | 低 | `ssh_sftp_next_packet` の comment に引数に無い名前 | 反映 |
| 25 | 低 | `ssh_buf_put_space` の pointer の寿命 | 反映（次の書きまで有効） |
| 26 | 低 | env・exec・subsystem の上限超えが EINVAL で接続を切る | 反映（#3 と同じく SSH_CREQ_OTHER） |
| 27 | 低 | pre-auth の「交換ごとに 1 回」の署名は monitor が強制できない、session id の中継の危険 | 反映。受け入れた危険として ssh-privsep.h と design §6.2 に書き、pre-auth の署名を合計 4 回に |
| 28 | 低 | USERAUTH_SUCCESS を誰が送るか | 反映。pre-auth が送り、出力を書き切ってから AUTH_DONE（sshd.h） |
| 29 | 低 | known_hosts を引く名前（OpenSSH は HostName） | 反映（ssh-hostfile.h、p015 の試験） |
| 30 | 低 | wire.h に uint64・boolean が無く、64 KiB では 256 KiB の境界の packet を組めない | 反映。`wire_u64`・`wire_bool`、`WIRE_MAX` 260 KiB（static で置く）。道具の自己試験 `test-tools.c` を足した |
| 31 | 低 | 記録の古さ（ws.md の目標の行、design-review.md が未作成、Q3 が C の時の依存） | 反映（ws.md の目標の行、この file、p020 の依存に注記） |
| 32 | 低（推測） | chroot の中で crypto の library の遅い初期化が失敗しうる | 反映。pre-auth は chroot の前に crypto を一度ずつ使う（sshd.h） |

reviewer が正しいと確かめた点（要約）: chacha20-poly1305 の鍵の前後・nonce・counter・tag の範囲と確かめの順、AEAD の padding が長さを除く事（Appendix A の packet_length 0x48）、hybrid の C_INIT 1216・S_REPLY 1120・K と H の符号、RFC 4253 §7.2 の 'C'・'D' と連結、strict KEX の名前・組・初回だけ・seq の戻し・最初が KEXINIT・wrap、空の MAC の一覧（draft §5。OpenSSH 10.5p1 kex.c:1020〜1022 でも確かめた）、USERAUTH の署名 data と PK_OK、known_hosts の `[host]:port`、openssh-key-v1 の Ed25519 の 64 bytes、blob・署名・指紋の大きさ、message 番号・理由の code・SFTP の番号、design §2 の file:line の抜き取り。
