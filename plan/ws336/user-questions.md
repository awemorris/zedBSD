<!-- awesome-plan project=zedbsd record=ws336-questions -->

# WS336 ユーザーへの問い（p001 の設計から、2026-11-01）

## どの WS か

- **WS336**: zedBSD の自前の sshd と ssh（OpenSSH と同じ protocol を話す、鍵は ed25519、鍵の交換は最新の定番の 1 つ）。今 image に入っている OpenSSH（外部の package）を、zedBSD の base の自前の実装に置き換える WS。
- 暗号の部品は WS335（libssl-compat と libcrypto-compat、P6 が設計中）の OpenSSL 互換の口を使い、ssh の側では暗号を作らない。
- **今の状態**: p001（設計）を P5 が終えたところ。設計は [design.md](design.md)、実装の Phase は p002〜p021（[ws.md](ws.md)）。実装はまだ始まっていない。
- **決まっていない事**: 下の Q1〜Q9。どれも「推し」の案で設計してあり、推しのままなら設計を変えずに進められる。推しと違う答えの時は、その問いの「違う答えの時」の Phase を直す。

## 用語

- **sshd・ssh**: sshd は接続を受ける側（server）、ssh は接続する側（client）の program。
- **鍵の交換（KEX）**: 接続の最初に、盗聴されても分からない共通の秘密を両側で作る手順。今の OpenSSH の第一は `mlkem768x25519-sha256`（量子計算機でも破りにくい ML-KEM と、従来の X25519 を組み合わせた物）。
- **cipher**: 通信を暗号化し、改ざんを見つける方式。`chacha20-poly1305@openssh.com` は OpenSSH の第一。
- **権限の分離（privilege separation）**: sshd が、まだ誰か分からない相手からの data を、root の権限を持たない別の process で読む仕組み。そこに不具合があっても root を取られにくい。
- **sftp・scp**: file を送る仕組み。今の OpenSSH の scp は既定で sftp の protocol を使う（相手に sftp-server が要る）。`scp -O` は古い protocol（相手に scp の program が要る）。
- **port の転送**: ssh の接続の中を通して別の TCP の接続をつなぐ機能（`ssh -L`・`-R`）。
- **agent**: 秘密鍵を持って署名だけをする常駐の program（ssh-agent）と、それを接続先に貸す機能。
- **passphrase**: 秘密鍵の file を暗号化する合言葉。

## 問い

### Q1. password での login を入れるか

- **推し: A 入れる**（sshd は password も受ける。今の image の設定と同じ）
- B 入れない（鍵だけ）
- 理由: `sshpass` で password を渡す試験の道具が 14 本あり、そのうち VF2 と ZYBO の zedBSD を相手にする物（`plan/tools/vf2/run.sh`・`boot-cycle.sh`、`plan/tools/zybo/ssh-check.sh` など）は確かめた（残りの相手が zedBSD か Linux かは未確認）。既定の利用者は user/user に変える決定がある。入れない時はそれらの道具を鍵に直す作業が要る。password は総当りの的になるので、試行の回数・時間の制限と失敗の記録を入れる（design.md §8）。

### Q2. port の転送（`ssh -L`・`-R`）を入れるか

- **推し: A 今は入れない**（要る時に別の WS）
- B 入れる
- 理由: 今の道具で `-L` を使うのは相手が Linux の host（5330 の Debian）の時だけで、zedBSD の sshd には要らない。入れると sshd の攻撃の面が広がる。

### Q3. scp・sftp をどこまで入れるか

- **推し: A sftp-server（受ける側）だけ入れる。`scp -O` を使う道具 4 本は `-O` を外す**
- B A に加えて、古い scp の受け手（相手の `scp -O` を受ける `scp` の program）も入れる
- C どちらも入れない（file は `ssh host 'cat > file'` で送る）
- 理由: host の scp（既定の sftp mode）で guest に file を送る道具（`plan/tools/guest/guest.py` の put・get、zybo の 8 MiB の試験）が多く、sftp-server が無いとそれらが全部止まる。`scp -O` を使うのは 4 本（`plan/tools/gpu-perf/measure.sh`、`plan/tools/rtx-video/rtx-*-check.sh` の 3 本）で、`-O` を外せば sftp-server で動く。zedBSD から外へ送る scp・sftp の client（zedBSD 側の `scp` コマンド）はどの案でも今は入れない。
- 違う答えの時: B は p017 に scp の受け手を足す。C は p013・p017 を取り下げ、guest.py などを直す Phase を足す。

### Q4. agent（ssh-agent と agent の転送）を入れるか

- **推し: A 入れない**
- B 入れる
- 理由: 今の道具は使っていない。秘密鍵を常駐させる仕組みで、範囲と攻撃の面が広い。

### Q5. 権限の分離の深さ

- **推し: B OpenSSH と同じ深さ**: 接続ごとに root の monitor、認証の前の通信は `/var/empty` に閉じ込めた uid `sshd` の process、login の後は利用者の uid の process。host key と password の確かめは monitor だけ。子の process は新しく program を起こし直して（execve）始めるので、host key などの秘密の写しを持たない。
- A 分離しない（1 つの root の process。Dropbear の形）
- C B に加えて、認証の前の process を zedBSD の `sandbox_spawn`（使える system call を絞る仕組み、WS168）で動かす
- 理由: A は認証の前の不具合がそのまま root の乗っ取りになる。C は一段強いが、暗号の library を static に link する build が要り、WS335 の側の作業が増える。B で始めて、C は後の Phase の候補にする。

### Q6. image を OpenSSH から自前に替える時期

- **推し: A QEMU の相互運用の試験（p019）が通ったら、既定の config を全部まとめて替える。OpenSSH の package は tree に残し、選べるままにする。実機の確かめはユーザーが拾える試験の一覧に置く**
- B 実機（5330・VF2・ZYBO のどれか）で確かめてから替える
- C 替えずに両方を選べるままにする（既定は OpenSSH）
- 理由: 自前と OpenSSH は同じ path（`/usr/sbin/sshd` など）を使うので、1 つの image にはどちらか一方。A なら替えた後は OpenSSL の package を ssh のためには要らなくなる。`config/ci/config-amd64-server.mk` はユーザーの物なので、替えるかは別に聞く。

### Q7. 鍵の交換を `mlkem768x25519-sha256` の 1 つだけにする事の確かめ

- **推し: A 1 つだけ**（P6 も同意。WS335 が ML-KEM と SHA-3 を作る）
- B `curve25519-sha256` も持つ
- 理由: 「最新の定番を 1 つ」の決定どおり。OpenSSH 10.1 以降の ssh は量子に強くない鍵の交換に警告を出すので、curve25519 だけでは host の ssh から毎回警告が出る。その代わり、**OpenSSH 9.9 より古い相手（例: Debian 12 の OpenSSH 9.2）には自前の ssh からつながらない**。B なら古い相手にもつながるが、名前が 2 つになる（部品は WS335 に既にあるので作業は小さい）。

### Q8. cipher を `chacha20-poly1305@openssh.com` の 1 つだけにする

- **推し: A chacha20-poly1305 だけ**
- B `aes256-gcm@openssh.com` も持つ（WS335 に AES-GCM の口を足す）
- 理由: OpenSSH の第一で、software で速く安全に作りやすい。AES-GCM は CPU の命令が無いと遅い。

### Q9. 秘密鍵の file の passphrase

- **推し: A 今は passphrase の無い鍵だけ**（ssh-keygen は `-N ''` だけ。passphrase 付きの鍵の file は「対応していない」と言って断る）
- B passphrase 付きも読み書きする
- 理由: passphrase の鍵の file は bcrypt と AES-CTR が要り、どちらも WS335 の範囲の外（bcrypt は OpenSSL の口にも無い）。今の道具の鍵は全部 passphrase 無し。B は WS335 に部品を足す別の判断が要る。
