<!-- awesome-plan project=zedbsd record=queue -->

# Queue

今の Queue と直近の Queue（全担当）。承認された Queue の範囲だけが実行の境界（AGENTS.md §2）。
担当ごとの詳細は `plan/agents/P<n>/queue.md`、担当の台帳は [registry](agents/registry.md)、次の番号は [master.md](master.md) の「次の番号」。

- 1 行は 1 attempt: `| qNNN / qNNN-iNN | 担当（定義・worktree・branch） | Phase | 範囲と承認（ユーザーの原文） | 結果 |`。
- 結果の語は `pending`・`in-progress`・`cleared`・`uncleared`（取り下げは理由を）。終わった節は Q1 がまとめて
  [history](history/index.md) へ移す。q1593 までの Board は [history/queue-board-through-q1593.md](history/queue-board-through-q1593.md)
  （2026-10-27 に P3 の精査 q1608 が古い未決の行を閉じた後に移した）。

## 2026-10-26 22:57: WS290（q1594〜）

承認: 2026-10-26 22:57 ユーザー「…新しいwsを作り、現状のghci.mdを読んで、zedBSDのGHCI driverを書き直してください。…」と、同日の決定（GHCI 2.0 だけ、host 試験だけ）。

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1594 / q1594-i01 | P1（kernel-and-driver-designer、worktree `/home/awe/zedBSD-worktrees/p1-ws290`・branch `agent/p1-ws290`、基点 main） | [ws290-p001](ws290/phase001/phase.md) | 設計（ghci.md 2.0 と今の driver の照合、構成、host 試験の model、Phase の分割、ghci.md の疑問の一覧）、design-reviewer。code は書かない | 担当の作業は終了（design.md 版 2、review 28 件反映、9050ae0fd、merge 593c6e5be、P1 は返却）。p001 の clearance は G01〜G39・U1〜U7 のユーザーへの提示の後 |
| q1595 / q1595-i01 | P2（phase-runner、worktree `/home/awe/zedBSD-worktrees/p2-ws291`・branch `agent/p2-ws291`、基点 main） | [ws291-p001](ws291/phase001/phase.md) | desktop の計測と 60 fps、実機 10.0.10.3（BootNext で USB の zedBSD）。結果: cleared（全段 60 fps、d29a59f6b、merge bad29b52f）。承認: 2026-10-26 23:05 ユーザー「P2も立てます。…amdrdnaドライバの改善を行ってほしいです。…」 | cleared（d29a59f6b、merge bad29b52f。範囲の欄の結果のとおり） |
| q1596 / q1596-i01 | P2（同じ worktree p2-ws291） | ws291-p002 | Linux で約 50 fps の 3D の scene を決め、zedBSD との差を測る（予約、p001 の後） | cleared（2026-10-27 00:43: scene-6500 zedBSD 20.1 fps・Linux 50.1 fps、merge 4cdbf1b7a） |

## 2026-10-26 23:17: i915 の残り（q1597〜）

承認: 2026-10-26 23:17 ユーザー「P4を立てて、WS244, WS245, WS252の実装を終わらせます。今から5330を起動するので、それにアクセスしてテストしてほしいです。」

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1597 / q1597-i01 | P4（phase-runner、worktree `/home/awe/zedBSD-worktrees/p4-i915`・branch `agent/p4-i915`、基点 main） | [WS245](ws245/ws.md) の残り（p041〜p045・p050d4c ほか） | main の tip で 5330 の実機の回帰、FAIL は二分探索で原因の段を特定して直す、残りの実装、p044・p045 の受け入れの読み替えはユーザーに示す | cleared（2026-10-26 23:40: 5330 の回帰 PASS（main 684a5243f）、5dd23a62e を merge 1c7c926d3。p041〜p045・p050d4c cleared） |
| q1598 / q1598-i01 | P4 | [WS244](ws244/ws.md) の残り | Gen9 の display・GT の出典の直し（ws.md の再開点）、5330 で回帰 | cleared（5330 の分 PASS、7dc936a23、merge 1c7c926d3。SV7 は試さない、2026-10-26 23:45 ユーザー） |
| q1599 / q1599-i01 | P4 | [WS252](ws252/ws.md) p002〜p008 | RKL・ADL-S/RPL-S・ICL の GT。実機が無い物は host 試験まで（D5「無いまま」）、5330 で退行が無いこと | finished（2026-10-27 P3 の精査 q1608: WS252 の p002〜p006 は host 試験・5330 の回帰まで実装済み（ws252/ws.md）、WS252 は completed）。旧: pending |

| q1600 / q1600-i01 | P2（同じ worktree p2-ws291） | ws291-p002a | amdrdna の executor に depth・index draw・mip（mview が動く 3D）。承認: ユーザー「機能を足してから比較（推し）」 | cleared（2026-10-27 00:43: 1 体で Linux と pixel 一致、b20e5545e、merge 5b654104f） |
| q1601 / q1601-i01 | P4 | WS252（firmware） | driver が名指す DMC の全部（rkl・adls・icl を足す）を zedBSD-firmware の i915/（SHA256SUMS・LICENSE）と userland/firmware/i915（Makefile の照合・manifest・WS233 の firmware-inventory）に。zedBSD-firmware は local の commit まで、push はユーザーの指示の時。承認: 2026-10-26 23:58 ユーザー「zedBSD-firmware/ツリーに、i915の依存としてすべて入れられませんか？」 | 済み（2026-10-27 01:01: 5ebc72e07、merge 7f9e423b6。zedBSD-firmware は local の fcdfccf、push はユーザーの指示待ち） |
| q1602 / q1602-i01 | P4 | [ws293-p001](ws293/ws.md) | i915 の render/ を vk/ に改名（動作は不変）、build・host 試験・5330 の回帰。承認: 2026-10-27 00:06 ユーザー「vk/に揃えたいです。」 | cleared（2026-10-27 01:39: c766c36ee、merge 3eef78c3e、5330 PASS） |
| q1603 / q1603-i01 | P2（phase-runner、worktree p2-ws291） | ws291-p003 | 投入の経路の改善（非同期の submit、record・submit の費用）、measure.sh bench で Linux と比べる。結果: cleared（2026-10-27 04:31、big-100 6.06→62.0 fps、subs-100 4.35→32.5、scene-6500 20.1→21.9）。承認: 2026-10-27 00:44 ユーザー「投入の経路から（推し）」 | cleared（2026-10-27 04:31、範囲の欄の結果のとおり） |
| q1604 / q1604-i01 | P4 | WS233 の試験の追従 | plan/ws031/tests/run-i915-firmware-package-test.sh を DMC 6 file・新しい cache の名前に直し、host の rm -rf を fresh_out に替え、完了した WS031 から plan/ws233/tests/ へ移す（WS233 p001 が参照、試験の整理の基準で残す物）。q1602 の後 | cleared（2026-10-27 01:44: 2f56e7a66、PASS） |
| q1605 / q1605-i01 | P4 | ws031 の試験（WS075 が使う） | plan/ws031/tests の lower・compile の期待値を今の mview の shader に合わせる（試験の側だけ）。q1604 の後 | cleared（2026-10-27 01:51: 2c8ae752c、run-vk-host-tests.sh 全部 PASS） |
| q1606 / q1606-i01 | P4 | WS245・WS244 の完了の後片付け | ws.md を完了の形に（a1d8b7613、merge 0121eb6dc）、Phase の dir 等は Q1 が削除（2a6fa1306）。回帰に使う道具を plan/tools/i915-display/・plan/tools/hw5330/ へ移す（2026-10-27 01:54） | cleared（2026-10-27 02:01: 2d4d09c63・456e6ff46、merge 7ba7f784a、run-xd-host-tests all passed、5330-run PASS） |
| q1607 / q1607-i01 | P1（bug-fixer、worktree `/home/awe/zedBSD-worktrees/p1-bug346`・branch `agent/p1-bug346`） | [BUG-346](bugs/BUG-346.md) | VF2（10.0.30.2）で sshd の子の回収漏れを再現・原因・修正・確認。承認: 2026-10-27 04:10 ユーザー「VF2を使ってSSHの問題を修正してください。P1が空いていますか？AMD RDNAマシンでは、SSHの問題を回避しつつ、元の作業を継続してください。」 | cleared（2026-10-27 05:10: 7bdee4894・530c5b522、merge ae266e6fe） |
| q1608 / q1608-i01 | P3（phase-runner、worktree `/home/awe/zedBSD-worktrees/p3-wsaudit`・branch `agent/p3-wsaudit`） | WS の精査 | Master の registry の未完の WS（incomplete・planning・planned）を ws.md・decisions-log・他の WS と照らし、completed・canceled・保留（blocked）に直す。証拠と出典を一覧に。承認: 2026-10-27 04:24 ユーザー「すみません、残っているとされたWSの大半が、すでに別なWSで完了していたり、私がブロックしたりキャンセルしたりしたものでした。サブエージェントP3を立てて、残っているWSを精査してください。masterとWSを修正して、completedやcanceledにしてください。」 | cleared（P3 の 655604714 を main に merge。結果は agents/P3/ws-audit.md、ユーザーへの問い 9 件は Q1） |
| q1609 / q1609-i01 | P2（phase-runner、worktree p2-ws291） | ws291-p004 | GPU 側の最初の安い試し: PA の small primitive filter・back-face cull・VGT の vertex reuse を RADV の値に（Mesa は値の参照、block ごとに出典）、scene-6500 の GPU の ms を測る。足りなければ NGG の設計の案を Q1 に。承認: 2026-10-26 ユーザーの WS291 の指示（Linux よりよくなるまでプロファイルベースで改善） | cleared（2026-10-27 04:43: 設定は RADV と同じ、差は NGG の culling、bfb05bb24） |
| q1610 / q1610-i01 | P4（phase-runner、worktree `/home/awe/zedBSD-worktrees/p4-plan`・branch `agent/p4-plan`） | plan の形式の最適化 | master.md を中心に plan/ 一式を Opus 5.5 が読みやすい形へ。P3 の ws-audit.md の結果を取り込む。承認: 2026-10-27 04:34 ユーザー「master.mdの内容が整理されていないことが、あなたの混乱につながっているように感じます。そこで、Opus 5.5であるあなたにとって理解しやすい形式に、master.mdなどのplan/以下一式を最適化してほしいです。これはP4に今すぐやってほしいです。」 | cleared（P4 の 9a6c18aab → main 0a3f17258、registry の記録。2026-10-28 Q1 が行を直した） |
| q1611 / q1611-i01 | P2（phase-runner、worktree p2-ws291） | ws291-p005 → p006 → p007 | NGG の A（passthrough）→ B（culling）→ C（詰め直し・属性の後回し）。承認: 2026-10-27 04:44 ユーザー「A〜C を今進める（推し）」 | paused（2026-10-27 P2 第 6 世代の返却 d025cbecd → main 9a444389c。p005 cleared、p006（p007 を統合）は culling + compaction が実機で止まらず 6500 体 30.68 ms、未達は約 20 ms。次は属性の後回し、ユーザーの判断待ち）。→ uncleared のまま閉じ、続きは q1635（2026-10-28） |
| q1612 / q1612-i01 | P1 → P3（2026-10-27 06:59 ユーザー「では、5330のリサイズを実施してください。」「P3で実施してOKです」、worktree `/home/awe/zedBSD-worktrees/p3-5330resize`） | 5330 の NVMe に zedBSD | live OS を USB に → SSH で ext4 を 16 GB 縮める → NVMe に zedBSD の区画 → BootNext で NVMe から起動、5330-run.sh を NVMe 向けに。承認: 2026-10-27 06:51 ユーザー「P1を立てて以下の作業をしてほしいです。5330で、Linuxのext4をリサイズして領域を空けて、nvmeにパーティションを作って、zedBSDを起動できるようにしたいです。USBメモリの書き換えが頻繁になって、故障が出ているのですが、半導体メモリの供給に問題があって、USBメモリの価格が非常に上がっており、消耗品と割り切って買えなくなっているからです。…USBメモリにgpartedの使える軽量live OSをインストールするなどして、スクリプト経由かSSH経由でext4をリサイズするのがいいと思います。」（Q1 の問いへのクリック: zedBSD に 16 GB） | cleared（2026-10-27 07:09: ext4 を 16 GiB 縮め p4〜p6 を作った、b2a22bd2f。2026-10-27 08:28: NVMe から zedBSD が起動し 5330-run PASS、9c4735ae1） |
| q1613 / q1613-i01 | P1（同じ worktree p1-5330nvme） | [ws298](ws298/ws.md) p001→p002 | Intel VMD の driver の設計と実装、5330 で NVMe が見える。承認: 2026-10-27 06:57 ユーザー「zedBSD に VMD の driver を書く」 | cleared（2026-10-27 08:33: 133176679） |
| q1614 / q1614-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-pm`・branch `agent/p1-pm`） | 5330 の起動の道具 | grub に zedBSD の項目（既定は Debian）、~/boot-zedbsd.sh（次の 1 回だけ zedBSD）。承認: 2026-10-27 08:44 ユーザーの指示（すぐ） | cleared（2026-10-27 08:59: e272ef967、実機 2 回 PASS） |
| q1615 / q1615-i01 | P1 | WS299・WS300・WS052・WS301・WS302 の計画 | 新しい WS の計画を広く（調査・設計の Phase の案と全体の依存関係）。実装は後で順に。承認: 2026-10-27 08:44 ユーザー「新規WSはまず計画を広く進めて、全体の依存関係を見ましょう。」 | cleared（2026-10-27: WS299・WS300・WS052・WS301・WS302・WS303 の ws.md と [pm-plan.md](agents/P1/pm-plan.md)。2026-10-28 Q1 が行を直した） |
| q1616 / q1616-i01 | P1（phase-runner、worktree p1-pm） | [ws299](ws299/ws.md) p001 → p002 | NVMe の速さの測定と設計、buffer cache の直し。承認: 2026-10-27 09:39 ユーザー「P1は、しばらくはさっきお願いした内容で進めていきます」「質問に答えたらP1を開始してよいです」 | cleared（p001・p002 は済み、続きは q1617。p001 は 2026-10-28 Q1 が cleared と判定（受け入れの数はユーザー「Debianの95%以上をゴールにします。」）） |
| q1617 / q1617-i01 | P1（phase-runner、worktree p1-pm） | [ws299](ws299/ws.md) p002 の回帰 → root の広げ → p003〜p006 | p002 の QEMU の回帰、5330-nvme-write.sh で root を p5 いっぱいに、p003 MDTS・PRP list（design-reviewer）、p004 非同期・queue・vector、p005 readahead・UFS、p006 HMB。受け入れは Debian の 95% 以上。承認: 2026-10-27 10:28 ユーザー「Debianの95%以上をゴールにします。」「やる」（HMB） | paused（2026-10-27 ラップアップ、使用量の残り 6%。p002・p003 cleared、p004 は測る口と設計の案まで、再開点は ws299 の ws.md と phase004/phase.md。p009 は Q1 の判定待ち）。→ uncleared のまま閉じ、続きは q1634（2026-10-28。p001・p009 は Q1 が cleared と判定、p004 の範囲は Q1 が決定） |
| q1618 / q1618-i01 | P3（phase-runner、worktree `/home/awe/zedBSD-worktrees/p3-ws304`・branch `agent/p3-ws304`） | [ws304](ws304/ws.md) p001 → p002 | Pi 5 の pcie1 と NVMe の設計（design-reviewer）と実装・host 試験・build。実機は使わない。承認: 2026-10-27 10:34 ユーザー「…コードだけ書いておくのがいいと思います。」 | cleared（2026-10-27: ws304 p001・p002 cleared、e616b4128。2026-10-28 Q1 が行を直した） |
| q1619 / q1619-i01 | P4（phase-runner、worktree `/home/awe/zedBSD-worktrees/p4-vfio`・branch `agent/p4-vfio`） | [ws305](ws305/ws.md) p001 → p002 | passthrough の環境の設計と構築。承認: 2026-10-27 10:55 ユーザー「はい、パススルーの環境を立てていいと思います。…使い分けていきましょう。」 | uncleared（ラップアップ、2953c2fa2、再開は ws305/ws.md） |
| q1620 / q1620-i01 | P3（phase-runner、worktree `/home/awe/zedBSD-worktrees/p3-ws304`） | [ws304](ws304/ws.md) p003 | Pi 5 の実機で pcie1 と NVMe（X1001）を確かめる。SwitchBot 無し（止まったらユーザー）。承認: 2026-10-27 11:26 ユーザー「では、Pi5をSwitchbotなしで起動しますので、テストをお願いできますか。VF2は電源を落とします。」 | uncleared（q1620-i03 まで: pcie1 の link が Polling で落ちる。次はハード側の切り分け（Raspberry Pi OS の lspci、ユーザーの判断）、[ws304](ws304/ws.md)。P3 は返却） |
| q1621 / q1621-i01 | P3（phase-runner、worktree `/home/awe/zedBSD-worktrees/p3-ws290`） | [ws290](ws290/ws.md) design の改訂 → p002 → p003 → p004 … | amaterrace-gpu の ghci.md の 3af62ca → f718918（と include/）の更新を読み、design.md を改訂（G の回答の反映、design-reviewer）、p002 以降の phase.md を立てて順に実装（host 試験だけ）。承認: 2026-10-27 13:55 ユーザー「P3で、~/amaterrace-gpu/docs/ghci.md の更新を読んで、GHCIドライバを更新してください。」 | paused（2026-10-27 P3 ラップアップ cd7cfbc16。ユーザー「そのほかは仕様を調整中です。いったん作業を終了していいです。」「このあと資料全般を修正するので、それまで待ってください。」。再開は資料の修正の後、ws290 の ws.md の手順）。2026-10-28: 仕様の疑問は q1622〜q1633 で全部解決、p001 の仕上げは新しい Queue で。→ 続きは q1636（2026-10-28） |
| q1622 / q1622-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ghci3`） | [ws290](ws290/ws.md) の ghci.md の三次回答のレビュー | amaterrace-gpu 7fc707d の docs/ghci.md・include と三次回答（plan/ws290/ghci-answers-hw-round3.md）を読み、H01〜H23・G02〜G49・V07 が解決したか、新しい食い違いを plan/ws290/ghci-review-round3.md に。source は変えない。承認: 2026-10-27 ユーザー「下記の通りだそうです。疑問が解決するかレビューしてください。P1でやりましょう。」 | cleared（2026-10-27 P1 dfc52306f → main 23136aacb。解決 5・一部 17・未解決 3・食い違い 24、次の疑問 J01〜J27 は plan/ws290/ghci-review-round3.md） |
| q1623 / q1623-i01 | P2（phase-runner、worktree p2-ws291） | [ws291](ws291/ws.md) RDNA の改善点の調査 | 計測と改善点の発見だけ（実装しない）。結果は plan/ws291/phase006/phase.md の追記か plan/ws291/rdna-next.md。承認: 2026-10-27 ユーザー「RDNAでの高速化を進めます。さらに改善できそうなポイントについて調べてください。計測と改善ポイントの発見まででいいです。」 | cleared（2026-10-27 P2 a303c3891、plan/ws291/rdna-next.md: scene-6500 zedBSD frame 34.14・GPU 30.25 ms 対 Linux 20.31・19.92 ms、候補 1 属性の後回しと LDS、2 BAR の WC の map、3 acquire、4 画素の小物） |
| q1624 / q1624-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ghci3`） | [ws290](ws290/ws.md) 正本の ghci.md の洗い出し | amaterrace-gpu の docs/ghci.md（8035b4a、正本）を読み、driver の実装に足りない情報と疑問を plan/ws290/ghci-review-8035b4a.md に。source・design は変えない。承認: 2026-10-27 ユーザー「GHCIドライバの件に戻ります。ghci.mdを読んで、ドライバ実装に足りない情報や疑問を洗い出してください。」 | cleared（2026-10-27 P1 865efa9b8、M01〜M58: C 17・H 17・M 19・L 5） |
| q1625 / q1625-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ghci3`） | [ws290](ws290/ws.md) eb296d1 の ghci.md で M01〜M58 を照合 | 正本の ghci.md（eb296d1）で M01〜M58 が解決したか、新しい食い違いを plan/ws290/ghci-review-eb296d1.md に。source・design は変えない。承認: 2026-10-27 ユーザー「照合してOKです。」 | cleared（2026-10-27 P1 ab2233daa: 解決 12・一部 31・未解決 15、次の疑問 N01〜N29（C 5）） |
| q1626 / q1626-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ghci3`） | [ws290](ws290/ws.md) GHCI の回答 第 1 版のレビュー → ghci-review.md 第 2 版 | ~/amaterrace-gpu/docs/ghci-review-reply.md（1 行目「GHCI Review Reply - Version 1.0 … eb296d1 (HEAD: 8b3ca3c)」）を ghci.md の正本と照らし、残る疑問を plan/ws290/ghci-review.md に第 2 版として上書き（1 行目を変え、末尾 DONE）。source・design は変えない。承認: 2026-10-27 ユーザー「以後、納得できる回答が得られるまで、 plan/ws290/ghci-review.md にレビューを置きます。」 | cleared（2026-10-27 P1 916d2db9a、ghci-review.md 第 2 版: 回答 第 1 版は M への回答、N は未解決 23・食い違い 6） |
| q1627 / q1627-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ghci3`） | [ws290](ws290/ws.md) GHCI の回答 第 2 版のレビュー → ghci-review.md 第 3 版 | 回答 Version 2.0（N01〜N29、ghci.md 382dd77）を ghci.md と照らし、残る疑問を第 3 版に上書き（1 行目を変え、末尾 DONE）。承認: 2026-10-27 ユーザー「以後、納得できる回答が得られるまで、 plan/ws290/ghci-review.md にレビューを置きます。」 | cleared（2026-10-27 P1 87d563d5d、第 3 版: 解決 4・一部 15・未解決 10・新 N30、残り 26（C 3: N01〜N03）。KMD の土台は書ける） |
| q1628 / q1628-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ghci3`） | [ws290](ws290/ws.md) GHCI の回答 第 3 版のレビュー → ghci-review.md 第 4 版 | 回答 Version 3.0（N01〜N30、ghci.md 0fc7927）を ghci.md と照らし、残る疑問を第 4 版に上書き。承認: 2026-10-27 ユーザー「以後、納得できる回答が得られるまで、 plan/ws290/ghci-review.md にレビューを置きます。」「疑問の解消のみを行います。」 | cleared（2026-10-27 P1 c40311ee7、第 4 版: 解決 10、残り 22（C 1: N02）、新 N31・N32） |
| q1629 / q1629-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ghci3`） | [ws290](ws290/ws.md) GHCI の回答 第 4 版のレビュー → ghci-review.md 第 5 版 | 回答 Version 4.0（N01〜N32、ghci.md 7153329）を ghci.md と照らし、残る疑問を第 5 版に上書き。承認: 2026-10-27 ユーザー「以後、納得できる回答が得られるまで、 plan/ws290/ghci-review.md にレビューを置きます。」「疑問の解消のみを行います。」 | cleared（2026-10-28 P1 8344227f9、第 5 版: 解決 12、残り 11（C 0・H 3・M 6・L 2）、新 N33） |
| q1630 / q1630-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ghci3`） | [ws290](ws290/ws.md) GHCI の回答 第 5 版のレビュー → ghci-review.md 第 6 版 | 回答 Version 5.0（N01〜N33、ghci.md fcd97e1）を ghci.md と照らし、残る疑問を第 6 版に上書き。承認: 2026-10-27 ユーザー「以後、納得できる回答が得られるまで、 plan/ws290/ghci-review.md にレビューを置きます。」「疑問の解消のみを行います。」 | cleared（2026-10-28 P1 fc871aab9、第 6 版: 解決 4、残り 7（M 4・L 3、C・H 0）） |
| q1631 / q1631-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ghci3`） | [ws290](ws290/ws.md) GHCI の回答 第 6 版のレビュー → ghci-review.md 第 7 版 | 回答 Version 6.0（N01〜N33、ghci.md bc3b4e6）を ghci.md と照らし、残る疑問を第 7 版に上書き。承認: 2026-10-27 ユーザー「以後、納得できる回答が得られるまで、 plan/ws290/ghci-review.md にレビューを置きます。」「疑問の解消のみを行います。」 | cleared（2026-10-28 P1 8bb4899c1、第 7 版: 解決 4、残り 4（M 1: N34 新、L 3）） |
| q1632 / q1632-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ghci3`） | [ws290](ws290/ws.md) GHCI の回答 第 7 版のレビュー → ghci-review.md 第 8 版 | 回答 Version 7.0（N01〜N34、ghci.md 35b89af）を ghci.md と照らし、残る疑問を第 8 版に上書き。承認: 2026-10-27 ユーザー「以後、納得できる回答が得られるまで、 plan/ws290/ghci-review.md にレビューを置きます。」「疑問の解消のみを行います。」 | cleared（2026-10-28 P1 39224ba16、第 8 版: 解決 3、残り 3（L: N12・N35・N36）） |
| q1633 / q1633-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ghci3`） | [ws290](ws290/ws.md) GHCI の回答 第 8 版のレビュー → ghci-review.md 第 9 版 | 回答 Version 8.0 を ghci.md と照らし、残る疑問（N12・N35・N36）を第 9 版に上書き。承認: 2026-10-27 ユーザー「以後、納得できる回答が得られるまで、 plan/ws290/ghci-review.md にレビューを置きます。」 | cleared（2026-10-28 P1 c7482e938、第 9 版: 疑問は残っていない（N01〜N36 全部解決、ghci.md 89529de）） |

## 2026-10-28 03:15: N=3（q1634〜）

承認: 2026-10-28 ユーザー「N=3で作業します。」と担当ごとの指示（各行の範囲の欄）。

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1634 / q1634-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ws299`・branch `agent/p1-ws299`、基点 main） | [ws299](ws299/ws.md) p004 → p005 → p006 → p007 → p008 | p004（raw の先読み、範囲は 2026-10-28 Q1 が決定）の design-reviewer・実装・host・QEMU・5330、続けて p005（UFS の file の先読み・書きの合流）、p006 HMB、p007 受け入れ、p008 規約。実機は 5330（遠隔の電源は無い）。承認: 2026-10-28 ユーザー「P1: P1は5330で作業します。NVMeの高速化の続きをお願いします。」 | uncleared（2026-10-28 P1 が context の残りで返却、cc1869b7c → main bce0883be。p004 は実装・review 2 回・QEMU・5330 の読み（raw 4 GiB 1.68〜2.24 GB/s で平ら、Debian 1.1 GB/s を越える）まで。乱読み・書きの再測定（review の後に後半 440〜560 MB/s、原因は未確認）は残り。p005 は調査と計画だけ、p006〜p008 は未着手。32 MiB の cache で 200 MiB の書きが止まる件は未切り分け） |
| q1635 / q1635-i01 | P2（phase-runner、worktree `/home/awe/zedBSD-worktrees/p2-ws291b`・branch `agent/p2-ws291b`、基点 main） | [ws291](ws291/ws.md) p006 の続き → p008〜p009 | NGG（culling・compaction・属性の後回し）などで scene-6500 を Linux 以上に（[rdna-next.md](ws291/rdna-next.md) の順）。実機は RDNA の機械 10.0.10.3（電源は Q1 の SwitchBot）。BAR の窓の WC の map（P2 の 779020ee6・cfcd07066、amdrdna の外: gpu.h・gpu.c・vm-device.h・vm/device.c・hal/amd64/space.c）は、HAL_SPACE_WC の契約（hal.h「conflicting requests must not be silently accepted」、i915 の ktest は WC|DEVICE を INVALID と試す）の意味の変更にあたるため一旦保留し、2026-10-28 ユーザー「P2 の WC の変更は承認します。」で承認（[Guardrail](guardrail.md) の承認済みの HAL 差分）。ゲーム最適化の option への分け直しは WS306（この Queue の外）。承認: 2026-10-28 ユーザー「P2: P2はRDNAマシンで作業します。RDNAの高速化をお願いします。まずNGGなどの高速化を取り入れてLinuxと同等以上の描画速度を達成して、そのあとで、ゲーム用最適化をオプションに分けていく段取りになっているので、確認してください。」 | uncleared（2026-10-28 P2 の返却 cd763014d → main。属性の後回し・WC の map（record 2.17 → 0.39 ms）・guard band・late alloc。scene-6500 は 30.7 fps・GPU 30.6 ms で目標の約 20 ms は未達。hang の試験の 5 回目で session が落ちた件は未切り分け。再開点は ws291 の ws.md と phase006） |
| q1636 / q1636-i01 | P3（phase-runner、worktree `/home/awe/zedBSD-worktrees/p3-ws290b`・branch `agent/p3-ws290b`、基点 main） | [ws290](ws290/ws.md) p001 の仕上げ（design 版 4）→ p002〜p012 | ghci.md 89529de で design 版 4、design-reviewer（Fable のレビューはしない）、phase.md を立てて p002 から実装（host 試験）。VF2＋KC705 の実機はユーザーの用意の後に Q1 が割り当てる。承認: 2026-10-28 ユーザー「P3: P3はGHCIドライバを作成します。まずはホストで開発しますが、私が今からVF2ボードを準備して、新しいSwitchbotで制御できるようにします。VF2はKC705 FPGAボードに接続されており、プログラミングは私が行いますので、そのあと実機検証が可能です。Fableのレビューは不要です。」 | paused（2026-10-28 ユーザー「P3はいったん待機してください。GHCI仕様をアップデートします。」。P3 は 3aa608b61 で返却（design 版 4 は本文まで、design-reviewer は未実施、仕様の欠け S01・S02 と判断 U5・U8〜U11 は design §19・§20）。再開は仕様の更新の後、新しい起動で） |
| q1637 / q1637-i01 | P3（phase-runner、worktree `/home/awe/zedBSD-worktrees/p3-ws290c`・branch `agent/p3-ws290c`、基点 main） | [ws290](ws290/ws.md) GHCI の仕様担当とのレビューのやり取り（第 10 版〜）と design 版 4 の追従 | main の checkout の plan/ws290/ghci-review.md に上書き（1 行目を変え、末尾 DONE）、~/amaterrace-gpu/docs/ghci-review-reply.md を 60 秒ごとに見て（1 行目が変わり末尾 DONE で受信）、更新された ghci.md を読んでレビュー、納得まで繰り返し、納得したら polling を止めて返る。design 版 4 は worktree で追従（実装はしない）。承認: 2026-10-28 03:33 ユーザー「GHCI仕様担当とのメッセージングについてです。P3は、下記のプロトコルでGHCIドライバ担当とやりとりします。…受信は60秒おきにファイルをチェックします。以後、仕様担当とP3がお互いに納得できる回答が得られるまで、 このプロトコルで通信します。お互いに納得が得られた後は、一次停止しますが、指示があれば、送信や受信ポーリングを再開します。」 | cleared（2026-10-28 03:47 第 12 版「疑問は残っていない」、仕様担当 Final Signoff（reply 0018f88）。仕様は ghci.md cf8be08・ghci_cmd.h 1a1fa85・gsi1_isa.h bea1ef9。design 版 4 は追従済み（P3 4c108791c）、design-reviewer は未実施。polling は一時停止） |
| q1638 / q1638-i01 | P3（phase-runner、worktree `/home/awe/zedBSD-worktrees/p3-ws290d`・branch `agent/p3-ws290d`、基点 main） | [ws290](ws290/ws.md) p001 の残り（design-reviewer を Opus 5.5 で）→ Phase の確定 → p002〜 | design 版 4（ghci.md cf8be08）を design-reviewer で review・反映、仕様の不備が無く design を approve できたら Phase を確定して p002 から実装（host 試験）。仕様担当への疑問は ghci-review.md を取り決めどおり用意して Q1 → ユーザーに知らせる（polling はユーザーの指示の時。2026-10-28 ユーザー「GHCI仕様担当が、レビューを受け取って、仕様をアップデートしています。プロトコルに従って相互アップデートを開始します。P3は自律的に仕様アップデートを受け取ってレビューを返し、相互に解決するまで継続してください。」で第 13 版から相互の更新を開始。2026-10-28 ユーザー「はい、P3は仮の扱いで実装を行ってよいです。」→ 仕様の回答を待つ点は仮の扱いで、Phase の確定と p002 からの実装に進む）。VF2 の電源（プラグミニ 2）は自分で入れ直してよい。承認: 2026-10-28 ユーザー「P3はOpus 5.5で設計レビューして、先に進めて下さい。GHCI仕様担当との通信が必要なときは、プロトコル通りファイルを用意して、私に言ってください。レビューでGHCI仕様に不備が見つからず、ドライバ設計をapproveできた場合、Phaseを確定し、実装に進んでOKです。」 | cleared の分: p001・p002（2026-10-28 P3 が context の残りで返却 b289a76dc → main 5efe7b8d9）。続きは q1641 |
| q1639 / q1639-i01 | P2（phase-runner、worktree `/home/awe/zedBSD-worktrees/p2-ws291c`・branch `agent/p2-ws291c`、基点 main） | [ws291](ws291/ws.md) p006 の続き → p008〜p009 | scene-6500 などで Linux と同等以上になるまで自律で測定と改善を繰り返す。hang が繰り返すなら調べて直す。30 分で分からなければ Fable 5.1（effort high）のサブエージェントに事情を説明して進め方の助言を得て続ける。どうしても解けなければ止めてユーザーに判断を求める。2026-10-28 ユーザー「P2は何度でもFableに聞いてください。」→ Fable の助言は回数の制限なし。実機 10.0.10.3、電源はプラグミニ 1 を自分で入れ直してよい。承認: 2026-10-28 ユーザー「P2は自律的にループして、Linuxと同等以上の性能をゴールに、継続してください。hangが繰り返し起きるなら調査して修正してください。30分調査してわからなかった場合は、Fable 5.1 Highのサブエージェントを作成して、事情を説明した上で解決までどういう進め方をすればいいかのアドバイスを得て、さらに継続してください。どうしても解決できない場合、停止してユーザに判断を求めてください。これらはP2が自律的に行います。」 | 続きは q1645（2026-10-28 P2 返却 dc0e46e09 → main。fragment で scene-6500 30.7→44.7 fps・GPU 20.5 ms（Linux 50.2・19.5）、ring の空き（症状 A）、SMU の保護と guard。未達: fence の固定の約 1.3 ms、vertex 0.8 ms、big-N・subs-N の CPU） |
| q1640 / q1640-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ws299b`・branch `agent/p1-ws299b`、基点 main） | [ws299](ws299/ws.md) p004 の残り → p005 → p006 → p007 → p008 | 5330 の Debian で fstrim（と書きの試験に使う p6 の swap の区画だけ blkdiscard）をしてから測り、SSD の消去の影響をなるべく除いて書きを測り直す。Debian の基準も同じ条件で。続けて受け入れ（Debian の 95% 以上）まで。2026-10-28 ユーザー「P1は自律的にゴールに向かわせてください。」→ 64 KiB の上限の変更も WS299 の中で P1 が判断して進める（全 platform の build と回帰で確かめる）。承認: 2026-10-28 ユーザー「P1はfstrimを使った上で、SSDの消去の影響をなるべく排除して、再度目標達成まで継続してください。」 | 続きは q1646（2026-10-28 P1 返却 0500994dd → main ce305bfd3。達: raw 書き 119%・raw 読み 150%・UFS 読み 126%・乱読み QD1 98%。未達: UFS 書き 55%・乱読み QD32 44%・O_DIRECT（p010 は設計のみ）） |

| q1641 / q1641-i01 | P3（phase-runner、worktree `/home/awe/zedBSD-worktrees/p3-ws290e`・branch `agent/p3-ws290e`、基点 main） | [ws290](ws290/ws.md) 仕様担当とのやり取り（第 14 版の回答の見張り・第 15 版〜）と p003 → p013 | q1638 の続き（同じ承認: 2026-10-28 ユーザー「P3はOpus 5.5で設計レビューして、先に進めて下さい。…」「…P3は自律的に仕様アップデートを受け取ってレビューを返し、相互に解決するまで継続してください。」「はい、P3は仮の扱いで実装を行ってよいです。」）。p014 の実機は KC705 の書き込みの知らせの後。2026-10-28 Q1 が所有に追加: platform/amd64/vmunix.mk・platform/riscv64/vmunix.mk の GHCI の行、Makefile の CONFIG_GHCI_NATIVE の行（U1）。WS288 の 1.1 の試験 12 file の git rm も OK | cleared の分: 仕様のレビューの Signoff（d9252e4、第 16 版）、p003、design 4.8（P3 が context の残りで返却 18cbe1b44 → main）。続きは q1642 |

| q1642 / q1642-i01 | P3（phase-runner、worktree `/home/awe/zedBSD-worktrees/p3-ws290f`・branch `agent/p3-ws290f`、基点 main） | [ws290](ws290/ws.md) p004 → p013 | q1641 の続き（同じ承認と Q1 の所有の追加: vmunix.mk の GHCI の行、Makefile の CONFIG_GHCI_NATIVE の行、WS288 の 1.1 の試験 12 file の git rm）。仕様は d9252e4 で確定、見張りは止めたまま | cleared の分: p004・p013（P3 が返却 bb020cf80 → main 2869d0c77）。続きは q1644 |
| q1643 / q1643-i01 | P4（phase-runner、worktree `/home/awe/zedBSD-worktrees/p4-ws307`・branch `agent/p4-ws307`、基点 main） | [ws307-p001](ws307/ws.md) | インストーラの仕様の確定（調査・spec.md・ユーザーの確認事項は Q1 経由・design-reviewer・実装の Phase の案）。code は書かない。承認: 2026-10-28 ユーザー「P4を立てて、WS307の仕様検討を行ってもらってください。ユーザ確認事項があれば回答させていただきます。仕様を確定させたいです。そのためのphaseをQ1が作り、P4に実行させてください。」 | 受け入れの残り 1 点（2 回目の review の後の変更 §5.10・installer-image・U26〜U29・§13a は review 未）を q1647 で。P4 返却 0640c4b9e → main 175d8add1（spec.md 683 行、U1〜U29 回答済み、実装 p002〜p014 134 LW） |

| q1644 / q1644-i01 | P3（phase-runner、worktree `/home/awe/zedBSD-worktrees/p3-ws290g`・branch `agent/p3-ws290g`、基点 main） | [ws290](ws290/ws.md) p005 → p012 | q1642 の続き（同じ承認と所有）。hcr.h・packets.h は書き直す Phase で消す | in-progress（2026-10-28 ユーザーの訂正「P3はhelperが完了してから待機でいいのですが、…」で p009 の helper を続行、完了の後に返却。前の記録: 11:27 の待機の区切り、P3 返却 50e8b8d2a → main fa4f5da8a。p005〜p008 cleared、p009 は最初から） |

| q1645 / q1645-i01 | P2（phase-runner、worktree `/home/awe/zedBSD-worktrees/p2-ws291d`・branch `agent/p2-ws291d`、基点 main） | [ws291](ws291/ws.md) p006 の続き → p008〜p009 | q1639 の続き（同じ承認: 自律で Linux と同等以上まで、hang は直す、Fable は何度でも、解けなければユーザーへ）。2026-10-28 Q1 が所有に追加: fence の遅れの原因が kernel の scheduler・起床の道にあればその直し、big-N・subs-N の libvulkan ↔ gpu core の往復（共有の code）の直し。どちらも先に案を Q1 に送り、他の GPU の driver（i915・nvrtx・PowerVR）の build と回帰を確かめる。HAL の API は変えない。2026-10-28 Q1 の OK: 案 A（Keiland の game mode の present を thread に）、B1（mview --images=4）、案 C（libvulkan の Wayland の acquire を fd で待つ）。2026-10-28 ユーザー「P2は、USB NICが通信できない（DHCPが取得できないとか、ue0が異常状態）のとき、自分で再起動するように、serivceを登録すれば、とりあえず問題を回避して継続できると思いますので、P2の判断でこれを行ってよいです。」「P2は、5330と同じように、NVMeにzedBSDをインストールしてから作業を継続する方がいいような気がします。USBメモリが半導体供給不足で手に入りづらいからです。」 | 続きは q1651（2026-10-28 P2 返却 89cb2a92a・e2c14e68a → main。UCLK 固定・NVMe 移行・案 A/C/B1・初期化の表示の無効化。scene-6500 49.2 fps（Linux 50.0）、GPU は Linux 以下） |

| q1646 / q1646-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ws299c`・branch `agent/p1-ws299c`、基点 main） | [ws299](ws299/ws.md) p010 O_DIRECT → QD32 → UFS の書き → p007 → p008 | q1640 の続き（同じ承認: fstrim の条件で Debian の 95% 以上まで自律、O_DIRECT を UAPI に足す（ユーザー））。2026-10-28 Q1 が所有に追加: src/kern/file.c・syscall.c・include/kern/file.h（O_DIRECT の open の検査・F_SETFL・bounce の振り分け・pdirect）。2026-10-28 ユーザー「さきほどの「mkfs で作った native の UFS が write-through になり、4 GiB の書きが 7 MB/s しか出ない件」は、P1に依頼します。」「UFS読み書きの高速化のあとに取り組んでもらいます。」「もしUFS読み書きの高速化にwrite-throughに修正が必要なら、先に取り組んだり同時に取り組んだりしてもOKです」→ 範囲に追加（原則 UFS の読み書きの高速化の後、要れば先・同時も可）、所有に userland/base/mkfs/ | paused（2026-10-28 WS308 を先に、ユーザー。O_DIRECT b809c70d1。5330 の固まりは再現した時に考え直す） |

| q1647 / q1647-i01 | P4（phase-runner、worktree `/home/awe/zedBSD-worktrees/p4-ws307b`・branch `agent/p4-ws307b`、基点 main） | (1) [ws307-p001](ws307/ws.md) の残り: 2 回目の review の後の変更の design-reviewer と反映 → (2) [BUG-347](bugs/BUG-347.md) の直し | (1) は q1643 と同じ承認。(2) 承認: 2026-10-28 ユーザー「BUG-347は推しで解決します。担当はP4で、現在の仕様検討が終わった後にスケジュールします。」（推し = root の tree を作る所で umask 022 を固定） | (2) BUG-347 resolved（merge a79ce5edb）。(1) は 3 回目の review の結果が P4 の返却の後に届いた（plan/ws307/design-review3.md）、反映は q1648 |

| q1648 / q1648-i01 | P4（phase-runner、worktree `/home/awe/zedBSD-worktrees/p4-ws307c`・branch `agent/p4-ws307c`、基点 main） | [ws307-p001](ws307/ws.md) の残り | 3 回目の review（design-review3.md）と J1〜J4 の回答を spec に反映（q1643 と同じ承認）。code は書かない | 済み（P4 e7e87b0f4 → main。review3 17 件と J1〜J4 を反映）。J5 の反映は q1649 |

| q1649 / q1649-i01 | P4（phase-runner、worktree `/home/awe/zedBSD-worktrees/p4-ws307d`・branch `agent/p4-ws307d`、基点 main） | [ws307-p001](ws307/ws.md) の最後 | J5 の回答を spec に反映（J1 の非公開の取り消し、運用禁止の規則、root 起動相当の扱い）。code は書かない。承認は q1643 と同じ | cleared（P4 23c76dd28 → main 18b6d7a11。ws307-p001 は Q1 が cleared） |

| q1650 / q1650-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ws308`・branch `agent/p1-ws308`、基点 main） | [ws308](ws308/ws.md) p001 → p004 | VM の metadata の大域の lock の取り合いを無くす。Fable 5.1 High の助言 OK。2026-10-28 ユーザー「P1は、現在原因がわかっていないパフォーマンスの問題については、Fable 5.1 Highに何度でも助言を求めてOKです。」（WS299 に戻った後も）。所有: src/kern/vm/（metadata-lock.c・vmspace-copy.c ほか lock に関わる所）、src/kern/syscall.c の uaccess の道、plan/ws308/。2026-10-28 ユーザー「P1の触る範囲が広がるのはOKです。」→ 所有に src/kern/entry.c・process.c・vm の object registry・src/drivers/pci/pci-nvme.c を追加。Q1 が src/hal/amd64/bsp-pcat/timecounter-policy.c の実装の直し（API 不変）を追加。承認: 2026-10-28 ユーザー「P1にお伝えください。フリーズは再現したときに対策を考え直します。仮想メモリのロックの変更は、P1の担当にして、一時的にそれに対応する別なWSを処理して、そのあとでNVMeのWSに戻りましょう。P1は、Fable 5.1 Highのサブエージェントに助言を求めてOKです。状況を共有して、検討の進め方を助言してもらってください。」 | paused（2026-10-28 11:07 ユーザーの 13 時までの待機。P1 返却 f43be9280 → main f2e9370eb。p002・p003 と command_lock の削減、5330 の QD32 direct 214.5k IOPS（Debian の 91%）・cached 204.8k（87%）。再開点は ws308/ws.md）。続きは q1652 |

| q1651 / q1651-i01 | P2（phase-runner、worktree `/home/awe/zedBSD-worktrees/p2-ws291e`・branch `agent/p2-ws291e`、基点 main） | [ws291](ws291/ws.md) | (1) mview の試験中に起動の logo（GOP の buffer）が見える件を直す（ユーザーの指示: GPU の使用中は console を止め、GOP の buffer は黒に）→ (2) Linux 以上まで（案 E・big-N・subs-N）。同じ承認（自律、Fable は何度でも、共有の code は案を Q1 に） | paused（2026-10-28 11:07 ユーザーの 13 時までの待機。P2 返却 07abbf57d → main。起動 logo の件を直した（GOP を黒に塗る、lease の終わりは黒へ flip、console は止めたまま）、実機で確かめ。Fable の CPU 時間の助言は記録のみ）。続きは q1653 |

| q1652 / q1652-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ws308b`・branch `agent/p1-ws308b`、基点 main） | [ws308](ws308/ws.md) の続き | q1650 と同じ承認と所有（13 時の再開）。Q1 が src/kern/buf.c・disk.c を所有に追加 | 済み（P1 返却 c9266aa62 → main 394ab9393。乱読み QD32 direct 99.3%・QD1 105%（同じ時間帯の Debian 比）。続きは q1654） |
| q1653 / q1653-i01 | P2（phase-runner、worktree `/home/awe/zedBSD-worktrees/p2-ws291f`・branch `agent/p2-ws291f`、基点 main） | [ws291](ws291/ws.md) の続き | q1651 と同じ承認（13 時の再開）: Fable の助言の M2 → 案 E の提案、executor の改善。2026-10-28 Q1 の OK（条件付き）: 共有の code の案 1（WSI の MAILBOX の画像 +1）・2（vkCmd* を recording に直に encode）・3（Reset・Begin・End を Submit に畳む）・4（小さい transaction を同期で）・5（gpu.c の wake・poll_notify を待ち手がいる時だけ、design-reviewer の後） | 済み（P2 返却 bcacd6219 → main 2cdf6a152。案 1・2・3a、UCLK 固定は既定 off（idle で SMU が止まるため）。続きは q1655） |

| q1654 / q1654-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ws308c`・branch `agent/p1-ws308c`、基点 main） | [ws308](ws308/ws.md) p004（不具合 1 の直し）・p006 の不具合 2・3・p007 規約 → WS299 | q1652 と同じ承認と所有。2026-10-28 ユーザー（クリック）「WS308 の中で P1 が直す (推し)」「後回し（Future Work） (推し)」（p005） | 済み（WS308 completed、merge c4172fbd4・a3ee18c0f。WS299 の再開は 5330 の停止で q1657 へ） |

| q1655 / q1655-i01 | P2（phase-runner、worktree `/home/awe/zedBSD-worktrees/p2-ws291g`・branch `agent/p2-ws291g`、基点 main） | [ws291](ws291/ws.md) の続き | 案 3b・4・5（Q1 の OK 済み、条件どおり）。2026-10-28 ユーザー「P2は、負荷に応じて上げ下げさせる形、を取ってください。今夜のsoakは不要です。最短ではないものの、よりよい形に仕上げたいです。」 → UCLK を負荷に応じた DPM で（夜間の soak は取り消し）。承認: q1653 と同じ。2026-10-28 ユーザー（クリック）「夜間に流す (推し)」 | 済み（P2 返却 e1314f4ce → main f0fd9b02b。3b・UCLK の設計と診断。案 4 は後回し。続きは q1658） |

| q1656 / q1656-i01 | P3（phase-runner、worktree `/home/awe/zedBSD-worktrees/p3-ws290g`（helper の未 commit の差分を引き継ぐため同じ worktree）・branch `agent/p3-ws290g`） | [ws290](ws290/ws.md) p016 の直しの確かめと commit → p017（全 backend・全 GPU driver の console の使用終了） | q1644 と同じ承認。p017 のユーザーの答え（Q1〜Q13、hal.h は変えない）と 2026-10-28 の全体の決定（GPU の最初の flip で console の framebuffer の backend は使用終了、serial は続く、全 arch・全 GPU driver）。Q1 が src/drivers/gpu/gpu.c（flip の道の kern_text_end の 1 か所）を所有に追加 | p016 cleared。p017 は段 1〜3・3a・5・11 まで（P3 返却 23f23a5c5、merge は QEMU の確かめの後）。続きは q1659 |

| q1657 / q1657-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ws299d`・branch `agent/p1-ws299d`、基点 main） | [ws299](ws299/ws.md)（q1646 の続き） | direct の書き 59% → UFS の書き 55% → mkfs native の write-through → p007 → p008。q1640・q1646 と同じ承認（自律、Fable は何度でも） | cleared（2026-10-28 21:50、merge f88836330。memcpy の改善と全体の測り直しの後、ユーザーの継続の判断待ちで停止） |

| q1658 / q1658-i01 | P2（phase-runner、worktree `/home/awe/zedBSD-worktrees/p2-ws291h`・branch `agent/p2-ws291h`、基点 main） | [ws291](ws291/ws.md) UCLK の負荷に応じた DPM（段 B〜D）→ 受け入れ → 案 4・5 | q1655 と同じ承認 | cleared（2026-10-28 23:4x、i02 で完了、merge ae5c52a21。UCLK の DPM 解決、underflow 0、2 s の止まりは Keiland の input の scan（b574fbf6a）、venus-reg PASS、評価済み。案 4・5 はユーザーの判断待ち） |
| q1659 / q1659-i01 | P3（phase-runner、worktree `/home/awe/zedBSD-worktrees/p3-ws290h`・branch `agent/p3-ws290h`、agent/p3-ws290g の先から） | [ws290](ws290/ws.md) p017 の残り（段 4・6・8・9・10・7）と QEMU の確かめ | q1656 と同じ承認。Q14「止める (推し)」、M8 は stub を足して試験を直す（Q1） | cleared（2026-10-28、merge b99e96139。host 試験・16 build・QEMU 2 本 PASS。p017 は実機（p014・M9）が残り in-progress） |
| q1660 / q1660-i01 | P4（phase-runner、worktree `/home/awe/zedBSD-worktrees/p4-closeout`・branch `agent/p4-closeout`、基点 main fb1f6afee） | 記録の監査（status.py）の COMPLETED_WITH_PHASES 89・COMPLETED_WITH_TESTS 79 の解消: 完了の WS の ws.md を完了の形に、Phase の directory と WS だけの試験の削除の一覧、今後も使う試験の plan/tools/ への移動 | 2026-10-28 ユーザー「計画ドキュメントの矛盾は解決してアップデートしておいてください。」 | cleared（2026-10-28、merge 582ff2906 と Q1 の削除。status.py・links.py の指摘 0） |
| q1661 / q1661-i01 | P2（phase-runner、worktree `/home/awe/zedBSD-worktrees/p2-ws291i`・branch `agent/p2-ws291i`、基点 main） | [ws291](ws291/ws.md) Keiland の input の device の検出を thread へ、main loop の他の blocking の点検と直し | 2026-10-28 ユーザー「Keilandの直し方ですが、デバイスの検出はスレッドに追い出してください。他にもメインループにそういったブロッキング要素がないかチェックしてください。」 | uncleared（2026-10-29: 入力の検出の thread 化と点検の表は済んだが、ユーザー「アドホックにはしません。」で merge 保留、WS309 の設計で置き換える） |

## 2026-10-29 01:37: P1〜P6（q1662〜q1667）

承認: 2026-10-29 01:3x ユーザー「P1にWS309 / P2にWS291の続き / P3にWS221と、ついでに同じWS内で、1GbEの性能を使い切れるまでTCPスタックの最適化、5330を利用。 / P4にWS307 / P5は詳細が決まっていないWSの設計のみを、ユーザの判断を聞きながら全般的に進める / P6は設計が決まっているWSについて、テストなしでひたすら実装だけ進めてテスト未完了でincompleteだけど実装はできていてマージされていない状態のWSを増やす。」（同時に 6）。

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1662 / q1662-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ws309`・branch `agent/p1-ws309`、基点 main） | [ws309](ws309/ws.md) | 設計の確定（pool の N は CPU 数で固定、ユーザーの回答「CPU 数で固定 (推し)」）→ design-reviewer → Phase の分割（Q1 に表を送る）→ 実装と試験。原文「P1にWS309」。q1661 の branch は置き換え（読むだけ） | 返却（p001〜p003 cleared、ef99d4812 → main b9f358a5a） |
| q1662 / q1662-i02 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ws309b`・branch `agent/p1-ws309b`、基点 main b9f358a5a） | [ws309](ws309/ws.md) p005 から | i01 と同じ承認 | 返却（p005 の displays・desktop・printers、56702e0fb → main 9402d4c74） |
| q1662 / q1662-i03 | P1（worktree p1-ws309c、基点 main 9402d4c74） | ws309 p005 の残りから | i01 と同じ承認 | 返却（p004・p005・p019 cleared、30650bb3b → main 2bf956f01） |
| q1662 / q1662-i04 | P1（worktree p1-ws309d、基点 main 5b32eb44c） | ws309 p006 から | i01 と同じ承認 | 返却（p006・p007 cleared、p008 の queue lock、608d063d1 → main 11781c470） |
| q1662 / q1662-i05 | P1（worktree p1-ws309e、基点 main 11781c470） | ws309 p008 の残りから | i01 と同じ承認 | 返却・ラップアップ（記録だけ、p008 の残り 1 から、c58ed8126 → main） |
| q1662 / q1662-i06 | P1（worktree `/home/awe/zedBSD-worktrees/p1-ws309f`、基点 main e0456e216） | ws309 p008 の残り 1 から | i01 と同じ承認 | 返却（p008 cleared、p009 の 1 歩目、a436f63ae → main 2b5979b98） |
| q1662 / q1662-i07 | P1（worktree `/home/awe/zedBSD-worktrees/p1-ws309g`、基点 main 2b5979b98） | ws309 p009 の残り 1〜5 から | i01 と同じ承認 | 返却（p009 cleared、p010 は回転と大きさの変更の試験が残り、abf15895b → main d441dcafb） |
| q1662 / q1662-i08 | P1（worktree `/home/awe/zedBSD-worktrees/p1-ws309h`、基点 main d441dcafb） | ws309 p010 の残り 2（出力の大きさの変更の QEMU の道と試験）→ p011〜（回転は Q1 の判断で p012 に） | i01 と同じ承認 | 返却（p010・p011 cleared、p012 は調べと手順だけ、8429b0efa → main） |
| q1662 / q1662-i09 | P1（worktree `/home/awe/zedBSD-worktrees/p1-ws309i`、基点 main） | ws309 p012（display の thread、park と drain の非同期、回転、BUG-353）の手順 1 から | i01 と同じ承認 | cleared（2026-10-29、98e3eae2a → main a9d07b850。p012 cleared、BUG-353 は RTX 2070 で PASS（799e52d4c）） |
| q1662 / q1662-i10 | P1（worktree `/home/awe/zedBSD-worktrees/p1-ws309i` に main を merge して続ける） | ws309 p013（present の常駐の thread）→ p014 → p016 | i01 と同じ承認。2026-10-29 Q1: p014 の spawn のため libc に `posix_spawn_file_actions_addclosefrom_np`（glibc 2.34・FreeBSD 13.1 と同じ名前）を足す（P1 の案 (a)）、所有に include/libc/spawn.h・userland/base/libc/posix.c を足す | 返却（2026-10-30、13c603132 → main b9c381154。p013・p014・p016・p017・p018 cleared。p015 はユーザーの判断待ちで WS309 は未完） |
| q1663 / q1663-i01 | P2（phase-runner、worktree `/home/awe/zedBSD-worktrees/p2-ws291j`・branch `agent/p2-ws291j`、基点 main） | [ws291](ws291/ws.md) p006 の続き | 案 5 → 案 4（ユーザーの回答「両方 (推し)」）、実機 10.0.10.3。原文「P2にWS291の続き」。q1658 と同じ自律の承認 | cleared（2026-10-29、919a05b78 → main。案 5・案 4、subs-1000 5.97→18 fps、venus-reg PASS） |
| q1664 / q1664-i01 | P3（phase-runner、worktree `/home/awe/zedBSD-worktrees/p3-ws221`・branch `agent/p3-ws221`、基点 main） | [ws221](ws221/ws.md) p002〜p009 | §9 は全部推し（ユーザーの回答「全部 推しで (推し)」）。kernel・driver・設定・TCP、5330 で 1GbE を使い切るまで TCP を最適化。原文「P3にWS221と、ついでに同じWS内で、1GbEの性能を使い切れるまでTCPスタックの最適化、5330を利用。」。5330 は P3 に割当 | 返却（1GbE の TCP の受け入れ達成: VF2 send 949.1・recv 949.0、MTU 9000、BUG-350、e7f69889a → main c42014745。WS221 の jumbo の残り p002〜p007 は未着手） |
| q1665 / q1665-i01 | P4（phase-runner、worktree `/home/awe/zedBSD-worktrees/p4-ws307e`・branch `agent/p4-ws307e`、基点 main） | [ws307](ws307/ws.md) p002〜p014 | spec.md どおりに実装。toolchain（sysroot.mk・libcxx）・asset の upload・HAL に触れる所は始める前に Q1 へ。QEMU。原文「P4にWS307」 | 返却（p005・p008 の 512・p002 の一部・p007 の HAL の案、f049d689d → main 10bc3185e） |
| q1665 / q1665-i02 | P4（worktree p4-ws307f、基点 main 35d2a373c） | i01 の続き | i01 と同じ承認 | 返却（p002 の大部分・splash・p003 段 1、246d63690 → main 92e91e61b） |
| q1665 / q1665-i03 | P4（worktree p4-ws307g、基点 main 92e91e61b） | i01 の続き（p003 段 2 から） | i01 と同じ承認 | 返却（p003 cleared、p006 段 1〜4、1f487021a → main 241c03f1d） |
| q1665 / q1665-i04 | worktree p4-ws307h、基点 main d94ceac00 | 続き | 同じ承認 | 返却・ラップアップ（p005・p006・p008 の 4096 の段、c0974cb41 → main 377efd966。UFS 上の swap file と p008-4k の起動は未判定） |
| q1666 / q1666-i01 | P5（kernel-and-driver-designer、worktree `/home/awe/zedBSD-worktrees/p5-design`・branch `agent/p5-design`、基点 main） | 詳細の決まっていない WS の設計 | code は書かない。判断は Q1 経由でユーザーへ。P1〜P4・P6 の WS は除く。原文「P5は詳細が決まっていないWSの設計のみを、ユーザの判断を聞きながら全般的に進める」 | 返却（全部 merge 63386d8a4） |
| q1666 / q1666-i02 | P5（worktree p5-design-b） | i01 の続き | i01 と同じ承認 | 返却（WS303 第 4 版・WS301 第 2 版・WS300 の調べ、6bddb1eda → main d94ceac00） |
| q1666 / q1666-i03 | worktree p5-design-c、基点 main d94ceac00 | 続き | 同じ承認 | 返却（WS303 第 5 版・WS301 第 3 版の改訂 2・WS300 の読み、7f5ab2b73 → main fb47c73a8） |
| q1666 / q1666-i04 | P5（worktree p5-design-d、基点 main 68ee65381） | 続き | 同じ承認、web の取得は禁止 | 停止（ユーザーが止めた、先頭 8d9ea93ce まで main。WS303 第 5 版の review は未反映、WS301 改訂 3 の review は未返却、WS300 は下書き） |
| q1667 / q1667-i01 | P6（phase-runner、worktree `/home/awe/zedBSD-worktrees/` の `p6-ws223`・`p6-ws282`・`p6-ws243b`・`p6-ws296`・branch は `agent/` に同じ名前、基点 main） | WS223・WS282〜WS284・WS243・WS296 の実装 | 試験なし、main に merge しない、incomplete のまま。調べが要る物は飛ばす。WS ごとにサブのサブエージェント可。原文「P6は設計が決まっているWSについて、テストなしでひたすら実装だけ進めて…マージされていない状態のWSを増やす。」・「調査しなくてよい、設計ができているか自明なWSについて、ひたすら書いてください。…WSごとにサブのサブエージェントを作ってよいです。」 | 返却（WS296 p001〜p003・p004 途中、WS282 p002 途中、merge しない） |
| q1667 / q1667-i02 | P6（worktree p6-ws296b・p6-ws282b） | i01 の続き | i01 と同じ承認 | 返却（WS296 p004 の usb.c 済み・rtl8822bu 途中、f31a4954d、merge しない） |
| q1667 / q1667-i03 | P6（worktree p6-ws296c・p6-ws282b） | i01 の続き | i01 と同じ承認 | 返却（WS296 全 Phase 実装済み 1809873bd、WS282 p002 途中 c60cdbd44、merge しない） |
| q1667 / q1667-i04 | worktree p6-ws282c（c60cdbd44 から）、基点 main d94ceac00 | 続き | 同じ承認 | 返却・ラップアップ（WS282 p002〜p004b 実装済み・試験未実施、WS284 p002 script・p003 CPU の参照、agent/p6-ws282c 677277d8f、merge しない） |
| q1668 / q1668-i01 | P2（phase-runner、worktree `/home/awe/zedBSD-worktrees/p2-ws291k`・branch `agent/p2-ws291k`、基点 main 9402d4c74） | [ws291](ws291/ws.md) p006 の続き | RDNA で shot の直しの確かめ → big-N・subs-N。ユーザーの回答（クリック、2026-10-29）「WS291 の続き (推し)」、q1663 と同じ自律の承認 | 返却（big-1000 35.5→57〜59、subs 48→39 µs、f5c01e255 → main 239e8ed7f） |
| q1668 / q1668-i02 | worktree p2-ws291l、基点 main d94ceac00 | 続き | 同じ承認 | 停止（ユーザーの指示、lib の直し subs-1000 submit 37.6〜39.3→34.3〜35.1 ms、kernel big-1000 +2.5 %、e6283eb70 → main 3ca557953。案 A・B は設計の review 済み・未実装） |

## 2026-10-29 06:15: 再開（P1〜P4）

承認: 2026-10-29 ユーザー「訂正します、タイマーでP1, P2, P3, P4だけの再開です。」と、WS235（「P1は、RTL8159というPCIeの10GbEアダプタのドライバを書いてほしいです。」→ 訂正「RTL8127です。」）・WS291（「WS291 の NVIDIA の段 (推し)」）の決定。

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1669 / q1669-i01 | P1（phase-runner、worktree `/home/awe/zedBSD-worktrees/p1-ws235`・branch `agent/p1-ws235`、基点 main 42da8d889） | [ws235](ws235/ws.md) p001 → 実装 | RTL81xx の共通の driver（rge を参考）→ RTL8127 の拡張、10.0.10.3、受け入れは約 7 Gbit/s と Linux | 返却（p001 判断待ちを除き済み、p002 骨格・p003 link の code、617b6a380 → main dc091f374） |
| q1670 / q1670-i01 | P2（phase-runner、worktree `/home/awe/zedBSD-worktrees/p2-ws291m`・branch `agent/p2-ws291m`、基点 main 42da8d889） | [ws291](ws291/ws.md) の NVIDIA の段 | RTX 2070 で desktop 60 fps・model 50 fps、RDNA の案 A・B は host・QEMU まで | 返却（BAR1 の遅延 map・VA 3.5 GiB・NVK の基準、0a3aa842a → main。p010a と VA の実機の確かめは移設の後） |
| q1664 / q1664-i02 | P3（worktree `/home/awe/zedBSD-worktrees/p3-ws221b`・branch `agent/p3-ws221b`、基点 main 42da8d889） | [ws221](ws221/ws.md) p004 から | i01 と同じ承認 | ラップアップ（ユーザーが VF2 を使うため、p004 途中 f2827fdb3、未 merge: net の backend に MTU が無く merge の前提を満たさない） |
| q1665 / q1665-i05 | P4（worktree `/home/awe/zedBSD-worktrees/p4-ws307i`・branch `agent/p4-ws307i`、基点 main 42da8d889） | [ws307](ws307/ws.md) p008 の 4096 の起動の直しから | i01 と同じ承認 | 返却（p008-4k の FAT の identify の直し、p005・p006 cleared、p009 段 2 の一部、90cfb10ce → main cd04f2a59） |
| q1664 / q1664-i03 | P3（worktree `/home/awe/zedBSD-worktrees/p3-ws221c`・branch `agent/p3-ws221c`、基点 92ec21301 = f2827fdb3＋main） | [ws221](ws221/ws.md) p004 の残りから | i01 と同じ承認、VF2 はユーザーが開放（「VF2はしばらく使わなくなりました。開放します。」） | 返却（p004・p005 cleared、p009 の一部（受けの窓を byte、window scale shift 5）、c08bf2b5d → main cf3b1ba72） |
| q1669 / q1669-i02 | P1（worktree `/home/awe/zedBSD-worktrees/p1-ws235b`、基点 main a69e7147e） | ws235 p001 の判断の記録 → link の実機の試験 → p004 | i01 と同じ承認、p001 の回答 ①②③⑤ | 返却（p001 cleared、p002〜p004 は 10G の link と TCP が残り、a62fed7bb → main 7d39bb655） |
| q1665 / q1665-i06 | P4（worktree `/home/awe/zedBSD-worktrees/p4-ws307j`、基点 main a69e7147e） | ws307 p009 段 2 の残りから | i01 と同じ承認 | 返却（p009 段 2〜4、dc4dd958e → main 23d2741ea） |
| q1665 / q1665-i07 | P4（worktree `/home/awe/zedBSD-worktrees/p4-ws307k`、基点 main 7d2f68e67） | ws307 p009 段 5 から | i01 と同じ承認 | 返却（p009 の QEMU の受け入れ PASS、Q1 の判定で cleared、diskpart batch の delete の不具合を直した、d6139052b → main 6b115e9fd） |
| q1665 / q1665-i08 | P4（worktree `/home/awe/zedBSD-worktrees/p4-ws307l`、基点 main 6b115e9fd） | ws307 p009 を cleared に記録 → p010（installer の app、画面の PNG を Q1 へ）→ p011 | i01 と同じ承認 | 返却（p009・p010 cleared、p011 は BUG-356 で未達、640063c48 → main 6c3076b5c） |
| q1665 / q1665-i09 | P4（worktree `/home/awe/zedBSD-worktrees/p4-ws307m`、基点 main） | ws307 p013（文書）→ p014（規約の全文の見直し、WS307 が変えた C）、p011 は BUG-356 の後 | i01 と同じ承認 | 返却（p013 cleared、p014 は今までの C の見直しが済み、p011・p012・p004・p007 の C は入った後に見直す、580a117e3 → main ee83a6eda） |
| q1684 / q1684-i01 | P4（worktree `/home/awe/zedBSD-worktrees/p4-ws283`、基点 main） | [ws283](ws283/ws.md) p002〜p014（bcm2711 の Vulkan の compute、設計 済み、実装だけ、merge は試験の後） | 2026-10-29 ユーザー「作業の手が空いてしまったら、テストなしで実装だけ進める戦略で、残っているWSのphaseをまんべんなく進めましょう。」（WS283 は保留・設計だけ、実装は後日だったのを、この指示で実装だけ進める） | 返却（p002〜p005・p007・p009 実装済み・試験未実施、branch agent/p4-ws283 b7fd8d23e は Pi の実機の試験の後に merge） |
| q1665 / q1665-i10 | P4（worktree `/home/awe/zedBSD-worktrees/p4-ws307n`、基点 main） | ws307 p011（通しの試験、BUG-356・357 が直ったので流し直す: 4096・USB・MBR と zedbsd.cfg の disk を含む）→ 日本語の画面の撮影 | i01 と同じ承認 | cleared（2026-10-30、f82bd280f → main adeab690a。p011 cleared: 通し 25 STEP・日本語の画面 21 枚 PASS） |
| q1664 / q1664-i04 | P3（worktree `/home/awe/zedBSD-worktrees/p3-ws221d`、基点 main 539fd96e7） | ws221 p008・p009 の残り → p006 → p007 → WS311 p001 | i01 と同じ承認 | 返却（p006〜p009 cleared、e2ce3ff0e → main d0bdf9b87。p011・WS311 p001 は i05 へ） |
| q1664 / q1664-i05 | P3（worktree `/home/awe/zedBSD-worktrees/p3-ws221e`、基点 main d0bdf9b87） | ws221 p011（IPv6 の TCP の試験と直し）→ WS311 p001 | i01 と同じ承認、p011 はユーザーの指示「IPv6はほとんどテストされていない現実がありますので、TCP改善で一緒にテストして直してください。」 | 返却（p011 cleared → main 08b9a1ca2、WS311 p001 の設計の第 2 版 642d812f3 → main 1fd28c4de） |
| q1670 / q1670-i02 | P2（worktree `/home/awe/zedBSD-worktrees/p2-ws291n`、基点 main 7b297b46a） | ws291 の NVIDIA の段: VA の実機の確かめ → p010a | i01 と同じ承認 | 返却（VA の直しは実機で OK、p010a は host で実装済み、5bc20b638 → main 6c063c971） |
| q1670 / q1670-i03 | P2（worktree `/home/awe/zedBSD-worktrees/p2-ws291o`、基点 main） | ws291 p010a の実機（rtx-mview-check.sh）→ p010a の残り → p011 | i01 と同じ承認 | 返却（ZETA_REQUIRES_BLOCK_LINEAR を影の surface で直し device lost は解消、depth test で画が消える件が残り、89108832d → main e30f1b78e） |
| q1670 / q1670-i04 | P2（worktree `/home/awe/zedBSD-worktrees/p2-ws291p`、基点 main） | ws291 p010a の depth test で画が消える件（rtx-depth-probe.sh から）→ p010a の受け入れ → p011 | i01 と同じ承認 | 返却（p010a cleared、depth は動く、消えた件は BUG-353、scene-5000 42.2 fps、p011 は設計だけ、0b35eee90 → main 5aa6b291a） |
| q1670 / q1670-i05 | P2（worktree `/home/awe/zedBSD-worktrees/p2-ws291q`、基点 main 5aa6b291a） | ws291 p011（非同期の submit で scene-5000 を 50 fps）、設計は phase011/phase.md | i01 と同じ承認、GPU の待ちを lock の外へ出す件は 2026-10-29 Q1 の判断で p011 | 返却（p011 cleared、scene-5000 42.2 → 49.1〜49.5 fps、6846a495c → main 73d78bb16） |
| q1670 / q1670-i06 | P2（worktree `/home/awe/zedBSD-worktrees/p2-ws291r`、基点 main） | ws291 p010 の残り: RTX 2070 の desktop の操作（scroll 20・hover 23・drag 36 fps）を 60 fps に | i01 と同じ承認 | 返却（drag 31 → 61・launch 51 → 64 fps、受け入れは WS309 の後、fdca948fe → main ae2e1283c） |
| q1678 / q1678-i01 | P2（worktree `/home/awe/zedBSD-worktrees/p2-ws242`、基点 main ae2e1283c） | [ws242](ws242/ws.md) の design.md（p003〜p007）の仕上げ → 実装（試験なし、merge は試験の後） | 2026-10-27 ユーザー「すぐ取り組むものの一つです。」、2026-10-29 ユーザー「作業の手が空いてしまったら、テストなしで実装だけ進める戦略で、残っているWSのphaseをまんべんなく進めましょう。」 | 返却（p003〜p007 実装済み・試験未実施、branch agent/p2-ws242 fda52e406 は試験の後に merge） |
| q1680 / q1680-i01 | P2（worktree `/home/awe/zedBSD-worktrees/p2-ws217`、基点 main） | [ws217](ws217/ws.md) p009a（UAPI・核）→ p009b（i915）→ p009c（libvulkan）、design-v5.md の順。compositor の段は WS309 の後なので触らない（試験なし、merge は試験の後） | 2026-10-28 ユーザー「WS217 を開き直す」、2026-10-29 ユーザー「作業の手が空いてしまったら、テストなしで実装だけ進める戦略で、残っているWSのphaseをまんべんなく進めましょう。」 | 返却（p009a〜p009c 実装済み・試験未実施、branch agent/p2-ws217 fb1b270d2 は 5330・SV7 と venus-reg の後に merge） |
| q1682 / q1682-i01 | P2（worktree `/home/awe/zedBSD-worktrees/p2-ws068`、基点 main） | [ws068](ws068/ws.md) p037・p034〜p036（desktop GL 3.3〜4.6）・p039 ETC2（実装だけ、merge は試験の後） | 2026-10-27 ユーザーの回答「継続」（空き時間だけ）、2026-10-29 ユーザー「作業の手が空いてしまったら、テストなしで実装だけ進める戦略で、残っているWSのphaseをまんべんなく進めましょう。」 | 返却（p037・p034・p039 と p035・p036 の一部、実装済み・試験未実施、branch agent/p2-ws068 cb093865b） |
| q1682 / q1682-i02 | P2（同じ worktree `/home/awe/zedBSD-worktrees/p2-ws068` に main を merge して続ける） | ws068 の Venus の QEMU の試験（glx-p037・回帰の glx-p013・p031・p033・egl-p019〜p030）と見つけた不具合の直し、通れば merge の候補 | 同上 | cleared（2026-10-29、45aa87120 → main fe49f1d60。Venus で glx-p037・回帰の glx-p013・p031・p033・egl-p019〜p030・egl-p039 PASS（build/p2-v6・v7）。i915 実機は未実施。p037・p039 cleared） |
| q1686 / q1686-i01 | P2（同じ worktree `/home/awe/zedBSD-worktrees/p2-ws068` に main を merge して続ける） | [ws068](ws068/ws.md) p034〜p036 の残りの実装と、各 Phase の Venus（QEMU）の試験。通れば merge の候補 | 2026-10-29 ユーザー「作業の手が空いてしまったら、テストなしで実装だけ進める戦略で、残っているWSのphaseをまんべんなく進めましょう。」（Venus の QEMU は手元で試せるので試験まで） | 返却（2026-10-29、a0cd3fa5d → main 782237d73。Venus で 16 本 PASS。p034・p040〜p044・p046 cleared、残りを p045・p047〜p068 に分けた。受け入れは Venus のみ（ユーザー）） |
| q1671 / q1671-i01 | P5（phase-runner、worktree `/home/awe/zedBSD-worktrees/p5-ws307p002`、基点 main） | [ws307](ws307/ws.md) p002 の残り（compositor の 5 file、H の段 2 の準備（merge・upload はユーザーの指示まで止める）、試験と残りの棚卸し）。asset の名前を変える f24bd97bf は branch `agent/p5-nightly-upload` に置き、作業の branch では 9715c5167 で取り消した。upload の後は f24bd97bf を merge でなく cherry-pick（または 9715c5167 の revert）で入れる（取り消しが履歴にあるので merge では入らない） | 2026-10-29 ユーザー「Keiという名前をzedBSDに変えるWSを実行してほしいです。P5を立てて実行していただけますか。」、p002 の出典と U26〜U29・J3 は ws307 の原文 | 返却（compositor・nightly の base・docs、試験は QEMU で名前 OK、24a0ea956 → main 96ee703c6。upload と f24bd97bf の cherry-pick が残り、p002 は in-progress） |
| q1672 / q1672-i01 | P6（phase-runner、worktree `/home/awe/zedBSD-worktrees/p6-ws312`、基点 main） | [ws312](ws312/ws.md) p001〜p004 | 2026-10-29 ユーザー「IPv6のfragment対応は、今P6を立ててやっていいです。」。この WS は試験まで行い merge する（Q1 の解釈） | 返却（WS312 completed、930169a83 → main 45c65539d） |
| q1673 / q1673-i01 | P3（phase-runner、worktree `/home/awe/zedBSD-worktrees/p3-ws311`、基点 main） | [ws311](ws311/ws.md) p001 の review-2 と cleared → p002（門と模型）→ p003（CUBIC と UAPI）→ p004（実機の測りと既定の判定）→ p005 | 2026-10-29 ユーザー「CUBICを実装して、性能がよくなったらCUBICに切り替えるWSを作って、取り組んでほしいです。jumbo frameの続きでやってほしいです。」、p001 の問いは Q1 の判断（ws311/ws.md） | 返却（WS311 completed、既定を CUBIC に、cc2ad2075 → main 3edf82c60） |
| q1679 / q1679-i01 | P3（worktree `/home/awe/zedBSD-worktrees/p3-ws221f`、基点 main） | [ws221](ws221/ws.md) p010（RTL8127 の 10GbE で TCP の詰め、10.0.10.3）、WS235 の引き継ぎ（stack が律速、1 本の worker・MSI が BSP 宛て） | i01 と同じ承認 | 返却（send 6280・6275 Mbit/s＝Linux の 92 %、recv 3401・3320 は未達、checksum の offload、bffc05d7d → main 4b39403c5） |
| q1679 / q1679-i02 | P3（worktree `/home/awe/zedBSD-worktrees/p3-ws221g`、基点 main） | ws221 p010 の続き: VF2・5330 の非退行（worker の起こしの直しは全 NIC に効く）→ recv の詰め（centris で zedBSD の ACK を tcpdump、偽の重複 ACK の元） | i01 と同じ承認 | cleared（2026-10-30、main fd94b023e: p010 cleared（10GbE recv は ATR 有効で Linux の 90〜97%、send 92%）、p012 SACK cleared） |
| q1674 / q1674-i01 | P5（phase-runner、worktree `/home/awe/zedBSD-worktrees/p5-ws307lock`、基点 main） | [ws307](ws307/ws.md) p015（画面の lock の既定を never、never で lock される不具合の直し） | 2026-10-29 ユーザー「WS307でついでに行ってほしいのですが、デスクトップがロックされるデフォルトの時間の設定を、neverをデフォルトにしてほしいです。また、neverにしても、現在は実際にはロックされてしまうみたいなので、直しておいてほしいです。」 | 返却（p015 cleared、8755132b3 → main 478c88fdf） |
| q1675 / q1675-i01 | P7（phase-runner、worktree `/home/awe/zedBSD-worktrees/p7-ws313`、基点 main） | [ws313](ws313/ws.md) p001（調べと設計）→ 実装の Phase | 2026-10-29 ユーザー「WSを立てます。P7の作業として開始します。…起動してデスクトップが表示できるところまでをゴールにして、WSを作ります。」 | ラップアップ（2026-10-29 ユーザー「P7は再起動します。」「なんらかの理由で、P7のコンテキスト状態がよくなかったんだと思います。」、BSP・媒体の道具・GPU の骨格、1f4acb4ee → main 52256eecc） |
| q1675 / q1675-i02 | P7（worktree `/home/awe/zedBSD-worktrees/p7-ws313b`、基点 main 52256eecc） | ws313 の再開点（phase001/phase.md）から: DTS の取得と host 試験 → arm64 の sysroot と world → 周辺の driver（SD・USB・Ethernet）→ Mali と Vulkan → disk image と夜の試験の用意 | i01 と同じ、§8 のユーザーの判断（ws.md） | 返却（全 block の code、実装済み・実機未試験、f998cfd82 → main 27e77a200） |
| q1675 / q1675-i03 | P7（worktree `/home/awe/zedBSD-worktrees/p7-ws313c`、基点 main） | ws313 の Mali の desktop の残り（render pass の LOAD、gpu1 と compositor の起動の順、display の import_image）と compiler の未対応、Amlogic の blob の notice（実装だけ） | 同上 | 返却（LOAD・import_image・gpu1 の順・compiler の cube など、5228815e6 → main 36de1abf3、実装済み・実機未試験） |
| q1675 / q1675-i04 | P7（worktree `/home/awe/zedBSD-worktrees/p7-ws313d`、基点 main） | ws313 の Mali を他の GPU と同じ範囲へ（WLS の compute、clear attachments、拡大の blit、FragDepth・SampleMask・UBO の vector load、TLS の FBD の確かめの案）、実装だけ | 同上（2026-10-29 ユーザー「余裕があればきちんと実装の対応範囲を他のGPUと同様に広げてください。」） | 返却（WLS・blit・clear attachments・FragDepth・SampleMask・UBO の vector load（既定無効）、c5a53aef1 → main a667bce34、実装済み・実機未試験） |
| q1675 / q1675-i05 | P7（worktree `/home/awe/zedBSD-worktrees/p7-ws313e`、基点 main） | ws313 の Mali の残り（depth attachment・mip level・textureGrad・共通の front end の OpFunctionCall の inline・SampleId・stencil の書き込み）、実装だけ | 同上 | 返却（2026-10-29、8e1dd87a1 → main 0b8486f08。6 件を実装（depth attachment・mip level・textureGrad・inline・SampleId・stencil の書き込み）と FBD を 128 byte に（78a87b871）。build と host の試験まで、実機・QEMU 未実施。p010・p011 in-progress） |
| q1687 / q1687-i01 | P7（worktree `/home/awe/zedBSD-worktrees/p7-ws313e` に main を merge して続ける） | [ws313](ws313/ws.md) Mali の未対応の残り（pass の中の clear attachments の depth・stencil と vkCmdClearDepthStencilImage、D24S8 の stencil aspect の copy と depth aspect の copy の stencil の保ち、kill/update の memory 書きの判定、textureGradOffset の試験）と phase010・phase011 の残り、実装だけ | 2026-10-29 ユーザー「作業の手が空いてしまったら、テストなしで実装だけ進める戦略で、残っているWSのphaseをまんべんなく進めましょう。」、「WS313は調査、コード作成などで、2つまでサブエージェントを作って並列化し、効率化してください。」 | 返却（2026-10-29、b35b6240c → main b6649f09f。依頼の 5 項目と Mesa との照合からの desktop の経路の直し（c08176e6c・db080e110）。build と host の試験まで、実機未実施。残りは C5 の実機の結果の後） |
| q1688 / q1688-i01 | P4（worktree `/home/awe/zedBSD-worktrees/p4-ws307n` に main を merge して続ける） | [BUG-355](bugs/BUG-355.md)・[BUG-358](bugs/BUG-358.md): native UFS の書き込みの遅さ（512 で約 1 MB/s、4096 の sector で 30〜40 KB/s）の原因を調べて直す（kernel の UFS・buffer cache・journal・block の driver の書き方）。直した後に WS307 p011 の通しを流し直す | 2026-10-29 ユーザー「P4は、先にUFSが遅いバグを修正してください。現在の流しテストは中止して、UFSを直してから、再度流しのテストを行ってください。そうでないと、インストールが遅すぎて、テストが非効率だと思います。」 | cleared（2026-10-30、6387b82a0・92e3362f8 → main ed93695ac。mkfs の native が tail journal を書かない、kernel の j3 の 4096 の sector→block の直し。QEMU で flush 5702→6・14453→3（512）、25816→8・18187→3（4096）、crash 試験 PASS、14 platform の vmunix warning 0。実機は未実施） |
| q1689 / q1689-i01 | P3（worktree `/home/awe/zedBSD-worktrees/p3-ws221g`） | [ws221](ws221/ws.md) p013: 5330 の USB の 2.5G（RTL8156、zedBSD の CDC NCM の driver）の送受信の遅さ（送信 265〜275、受信 555〜696 Mbit/s、Linux の r8152 は 2353）を driver で直す。5330 で計測 | 2026-10-29 ユーザー「P3で、2.5GのUSBが遅いことは、ドライバの問題と思いますので、5330を使って修正してほしいです。」 | cleared（2026-10-30、a317e1a55 → main 68d7c0caf。p013・p014 cleared: 5330 の NCM send 272→2097・recv 1225→1449 Mbit/s、Linux r8152 は 2353/2354。recv は xHCI の要求の数が天井でなかった） |
| q1690 / q1690-i01 | P2（worktree `/home/awe/zedBSD-worktrees/p2-ws314`、基点 main） | [ws314](ws314/ws.md) p001 → p002 → p003 → 以後の Phase（設計の後の実装、試験は build・host・QEMU） | 2026-10-30 ユーザー（ws314/ws.md の原文 1〜9） | in-progress |
| q1691 / q1691-i01 | P6（worktree `/home/awe/zedBSD-worktrees/p6-ws314doc`、基点 main） | [ws314](ws314/ws.md) p011（Opus、構成の設計と comment）→ p012 は Sonnet の別の起動 | 2026-10-30 ユーザー（ws314/ws.md の原文 11） | cleared（2026-10-30、p011・p012 cleared。Sonnet の書き直し 933c667bc と Q1 の see-through の直し 64f09ca82 → main d4ba04b96） |
| q1692 / q1692-i01 | P5（WS243 の後に続けて、worktree は main から新しく） | NVIDIA RTX 2070（10.0.10.3、nvrtx）で Notes の app が起動するかの確かめ、起動しなければ原因を調べて直す（Bug を立てる） | 2026-10-30 ユーザー「NVIDIA RTXを使っているときにNotesアプリが起動できるか確認し、できなければ対応しておいてください。」 | cleared（2026-10-30、BUG-361 直した: nvrtx に D24S8 の stencil と S8Z24 の page の kind、実機で Notes が起動し pen と文字、3d37902ca → main 95807962e） |
| q1693 / q1693-i01 | P5（worktree `/home/awe/zedBSD-worktrees/p5-ws291`、基点 main） | [ws291](ws291/ws.md) の NVIDIA の段: p011（RTX 2070 の重い 3D の model 50 fps、Linux NVK の基準との差）→ p010 の受け入れの測り直し（WS309 の後） | 2026-10-30 ユーザー「…そのあと、デスクトップ機では、RTXドライバの最適化に進んでください。エージェントは再利用しても新規で立ててもOKです。」、ws291 の承認（i01） | in-progress |
| q1694 / q1694-i01 | P3（worktree `/home/awe/zedBSD-worktrees/p3-ws315`、基点 main） | [ws315](ws315/ws.md) p001 設計 → 実装（RTL8156 の vendor mode の driver、5330 で試験。RTL8157 は資料だけで実装） | 2026-10-30 ユーザー「Realtek方式については、NCMでどこまで速度が上がるか観察してから、ぜひ実装しましょう。…」「5GbEのNICは持っていません。今後購入してテストしたいです。資料だけで実装しておいてほしいです。」 | in-progress |
| q1695 / q1695-i01 | P1（worktree `/home/awe/zedBSD-worktrees/p1-ws326`、基点 main） | [ws326](ws326/ws.md) p001 module の設計（header と仕様の comment と骨格と試験の仕様） | 2026-10-30 ユーザー（ws326/ws.md の原文 2） | in-progress |
| q1696 / q1696-i01 | P3（worktree `/home/awe/zedBSD-worktrees/p3-ws315`、main を merge して続ける） | [ws315](ws315/ws.md) p006: F-WS315-rcvbuf（TCP の受けの buffer の自動調整か既定の拡大、5330 の同時の送受信で Linux 同等へ） | 2026-10-30 ユーザー「F-WS315-rcvbufを解決してほしいです。WS315のphaseにして、今取り組んでください。」 | uncleared（2026-10-30 ラップアップ、同時の send の退行が未解決、main cbd98d6f4 に merge（ユーザー OK）） |
| q1697 / q1697-i01 | P2（worktree `/home/awe/zedBSD-worktrees/p2-ws327`、基点 main） | [ws327](ws327/ws.md) p001 設計 → p002〜p005 | 2026-10-30 ユーザー「WS327は今取り組みます。」と ws327/ws.md の原文 | cleared（p001・p002、p003 の host まで。ラップアップ b73eb1fa7） |
| q1697 / q1697-i02 | P2（同じ worktree、branch `agent/p2-ws327` fb2d34dc0 から） | [ws327](ws327/ws.md) p003 の guest の実行 → p007 委譲の設計（標準廉価実装手順） | 2026-10-30 ユーザー「P2をいったんラップアップして、標準廉価実装手順を適用するようにWS237のphaseを整理しなおして、再起動してほしいです。」 | cleared（2026-10-30 Q1、p003・p007、main 5de56c3e1） |
| q1698 / q1698-i01 | P4（worktree `/home/awe/zedBSD-worktrees/p4-ws307o`、基点 main） | [ws307](ws307/ws.md) の残り: p002 の nightly の asset の upload と f24bd97bf の cherry-pick、p015 の sleep の既定を Never、p004（clang・libcxx の package の build と zedBSD の上の GTK4 の app の build）、p008 の残り（512 の j3 の sync）、p014 の規約の見直しの残り。p007（kernel の firmware の変数の driver、名前は文字列・値は byte の列、HAL の API は変えない）も | 2026-10-30 ユーザー（ws307/ws.md の原文） | uncleared（2026-10-30 ラップアップ、p002 cleared、残りは ws307 の Resume point。520360247） |
| q1690 / q1690-i02 | P2（同じ worktree） | [ws314](ws314/ws.md) p013（kl_launch）→ p014（Preview）→ p015（言語の event）→ p007（libbrowser）→ p008 → p009 → p010 | ws314 の原文 | in-progress |
| q1676 / q1676-i01 | P5（phase-runner、worktree `/home/awe/zedBSD-worktrees/p5-ws250`、基点 main） | [ws250](ws250/ws.md) p001 の design.md の仕上げ → 実装（試験なし、merge は試験の後） | 2026-10-27 ユーザー「すぐ取り組むものの一つです。」、2026-10-29 ユーザー「作業の手が空いてしまったら、テストなしで実装だけ進める戦略で、残っているWSのphaseをまんべんなく進めましょう。」 | 返却（p001 cleared、p002〜p005 実装済み・試験未実施、branch agent/p5-ws250 51da3274b は実機の試験の後に merge） |
| q1676 / q1676-i02 | P5（同じ worktree `/home/awe/zedBSD-worktrees/p5-ws250`・branch `agent/p5-ws250` を続ける） | ws250 の実機の試験（10.0.10.3 の RTX 2070、rtx-hang-check.sh の S0〜S5 と --probe S1）と見つけた不具合の直し、p007 | i01 と同じ | 返却（実機 S0〜S5 通り、p002〜p005・p007 cleared、b929c6a75 → main 67b8aa8d2。RESET を先にする案はユーザーの判断待ち） |
| q1678 / q1678-i02 | P5（worktree `/home/awe/zedBSD-worktrees/p5-ws242`、branch `agent/p5-ws242`＝agent/p2-ws242 fda52e406 に main を merge した物） | ws242 p008（RTX 2070 の回帰: 起動・desktop・mview・vkhang、p006 の止めの道）と見つけた不具合の直し、p009 | i01 と同じ | 返却（p003〜p009 cleared、RTX 2070 の実機の回帰が通った、95c384832 → main 7a620a671。RTX 30・40・GTX 16 は実機が無く host の試験だけ） |
| q1677 / q1677-i02 | P5（worktree `/home/awe/zedBSD-worktrees/p5-ws243`、branch `agent/p5-ws243`＝agent/p6-ws243c 9dbf7fda5 に main を merge する） | ws243 を今の main（WS250 の session ごとの channel・WS242）に合わせ、host の試験、10.0.10.3 の RTX 2070 で p004（NVDEC の H.264 の decode の hash、query の status、readback の速さ） | 同上 | cleared（2026-10-30、a99b9213d → main 65d8082c1。WS243 completed） |
| q1677 / q1677-i01 | P6（phase-runner、worktree `/home/awe/zedBSD-worktrees/p6-ws243c`、基点 main） | [ws243](ws243/ws.md) p001 の設計 → 実装（試験なし、merge は試験の後） | 同上 | 返却（p001 設計・review 済み、p002・p003 実装済み・試験未実施、branch agent/p6-ws243c 9dbf7fda5 は試験の後に merge） |
| q1681 / q1681-i01 | P6（worktree `/home/awe/zedBSD-worktrees/p6-ws299`、基点 main） | [ws299](ws299/ws.md) の残り（p004 の判定の材料、p005・p006（HMB）・p010 の実装、試験なし、merge は試験の後） | 2026-10-27 ユーザー「5330でNVMeの最適化（速度向上）をWSを立ててやってほしいです。」、2026-10-29 ユーザー「作業の手が空いてしまったら、テストなしで実装だけ進める戦略で、残っているWSのphaseをまんべんなく進めましょう。」 | 返却（mkfs native の tail journal の件はユーザーの A/B/C 待ち、buf の束の書き、実装済み・試験未実施、branch agent/p6-ws299 520f92799 は試験の後に merge） |
| q1683 / q1683-i01 | P6（worktree `/home/awe/zedBSD-worktrees/p6-ws300`、基点 main） | [ws300](ws300/ws.md) p001・p002（設計 → 実装、試験なし、merge は試験の後） | 2026-10-27 ユーザー（ws300 の原文）、2026-10-29 ユーザー「作業の手が空いてしまったら、テストなしで実装だけ進める戦略で、残っているWSのphaseをまんべんなく進めましょう。」 | 返却（p001 cleared、p002〜p005 実装済み・試験未実施、branch agent/p6-ws300 b46559d53 は 5330 の試験の後に merge） |
| q1685 / q1685-i01 | P6（worktree `/home/awe/zedBSD-worktrees/p6-ws052`、branch `agent/p6-ws052`＝agent/p6-ws300 b46559d53 から） | [ws052](ws052/ws.md) p014〜p016（S0ix を 5330 で、WS300 の VMD の suspend の上に、実装だけ） | 2026-10-29 ユーザー「作業の手が空いてしまったら、テストなしで実装だけ進める戦略で、残っているWSのphaseをまんべんなく進めましょう。」 | 返却（p014〜p016 実装済み・試験未実施、branch agent/p6-ws052 acbbbdba4（WS300 を含む）は 5330 の試験の後に merge） |
| q1667 / q1667-i05 | P6（worktree `/home/awe/zedBSD-worktrees/p6-ws282d`、branch `agent/p6-ws282d`＝agent/p6-ws282c 677277d8f に main を merge） | [ws282](ws282/ws.md) p005〜p011（camera の枠組みと UVC、実装だけ）→ [ws284](ws284/ws.md) の残り | 2026-10-29 ユーザー「作業の手が空いてしまったら、テストなしで実装だけ進める戦略で、残っているWSのphaseをまんべんなく進めましょう。」 | in-progress |
| q1669 / q1669-i03 | P1（worktree `/home/awe/zedBSD-worktrees/p1-ws235c`、基点 main 7b297b46a） | ws235 の 10G の link と rge0 だけの TCP → p005 | i01 と同じ承認 | 返却（p002〜p005 cleared、send 3539・recv 3373 Mbit/s、Linux 6808・5458、d3248bf1a → main 0bc806084） |
| q1669 / q1669-i04 | P1（worktree `/home/awe/zedBSD-worktrees/p1-ws235d`、基点 main） | ws235 p006（RTL8127 の拡張: profile・多 vector の MSI-X・RSS・割り込みの抑え・zero copy）→ p007 → p008 | i01 と同じ承認、p001 の回答 ①②③⑤ | 返却（p006〜p008 cleared、driver は律速でない（send 4935・recv 3360）、2dfb45018 → main 1a0f63b26） |
| q1669 / q1669-i05 | P1（worktree `/home/awe/zedBSD-worktrees/p1-ws235e`、基点 main 1a0f63b26） | ws235 の受け入れの最後: p008 の後の tree と server の config の kernel で 10.0.10.3 の非退行（link・ping・ssh・tcp-perf 両向き）→ WS235 を完了の形に（Q1 が判定） | i01 と同じ承認 | 返却（最後の確かめ PASS、429c6b2dc → main ffad67106、WS235 completed e0456e216） |
| q1700 / q1700-i01 | P5（worktree `/home/awe/zedBSD-worktrees/p5-ws291n`、基点 main ef35deaf9、実機 10.0.10.3） | [ws291](ws291/ws.md) NVIDIA の段: p010 の desktop の測り直し、p012 以降で scene-5000 を 50 fps 超えへ | 2026-10-30 ユーザー「WS291はNVIDIAですか？それなら今実行してOKです。」 | uncleared（2026-10-30 ラップアップ、計測なし。c59e5a60e） |
| q1701 / q1701-i01 | P1（worktree `/home/awe/zedBSD-worktrees/p1-ws009`、基点 main ef35deaf9） | [ws009](ws009/ws.md) p009（DOC-54）を標準廉価実装手順の Phase に組み直す設計（Sonnet の Phase は Q1 が起動） | 2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」（順は WS009 → WS296 → WS297、Q1） | cleared（2026-10-30 Q1、main 05d32e667） |
| q1702 / q1702-i01 | P3（worktree `/home/awe/zedBSD-worktrees/p3-ws315`、main 5130445fb、5330・10.0.10.3） | [ws315](ws315/ws.md) p006 の同時の send の退行を解決、回帰・10GbE、F-WS315-rcvbuf | 2026-10-30 ユーザー「WS315は評価が長くて、あまりトークンを使わないため、今起動しなおします。途中でセッションが利用制限で強制終了になってもよいようにすすめてください。」 | cleared（2026-10-30 Q1、WS315 completed、main 834d10d88） |
| q1703 / q1703-i01 | P2（worktree `/home/awe/zedBSD-worktrees/p2-ws327`、main b901b1968、QEMU） | [ws327](ws327/ws.md) p016 受け入れ（境界・QEMU の回帰・FreeBSD・壊れた閉じた WS の試験・Win32 の揺れの一覧） | 2026-10-30 ユーザー「P2は手順書を書いたとのことですので、Sonnetで実施してOKです。」（p008〜p015 の後の受け入れ） | uncleared（2026-10-30 ラップアップ、p016 途中、e9f8da415） |
| q1704 / q1704-i01 | P1（worktree `/home/awe/zedBSD-worktrees/p1-ws009r`、main d73c023ef） | [ws009](ws009/ws.md) p024 DOC-54 の敵対的な正確さの review（Opus） | 2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。」 | cleared（2026-10-30 Q1、p024、main d2c8a5969） |
| q1705 / q1705-i01 | P1（worktree `/home/awe/zedBSD-worktrees/p1-ws009r`、main aaf2bc280） | [ws009](ws009/ws.md) の受け入れと完了の形 → [ws296](ws296/ws.md) を標準廉価実装手順に組み直す設計 | 2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」 | uncleared（2026-10-30 ラップアップ。p025〜p027 cleared、p028 途中、36c56acb1） |
| q1706 / q1706-i01 | P3（worktree `/home/awe/zedBSD-worktrees/p3-ws315`、main b776fd468、QEMU） | [BUG-367](bugs/BUG-367.md) loopback の TCP の close が戻らない件の解析と修正 | 2026-10-30 Q1（WS327 p016 で P2 が発見、AGENTS.md の Bug の扱い） | cleared（2026-10-30 Q1、BUG-367 closed、main 701b21b19） |
| q1707 / q1707-i01 | P3（worktree `/home/awe/zedBSD-worktrees/p3-ws315`、main 36359a937、QEMU） | [BUG-369](bugs/BUG-369.md) QEMU の guest への SSH の往復の伸びの解析と修正（TIME_WAIT が原因なら F-BUG367-time-wait も） | 2026-10-30 Q1（AGENTS.md の Bug の扱い） | cleared（2026-10-30 Q1、BUG-369 closed、main e1c714fb5） |

## 2026-10-30 13:50: WS074 を P8 で（q1708〜）

承認: 2026-10-30 13:4x ユーザー「P8でWS074（libbrowserの改善）を実行してください。」（同日「WS074はphase-runner-lowで実行することにします。」）。同日の「WS328に関しては、WS074を止めてWS328を先に実装する」より新しい指示なので、WS074 を今再開する（WS328 は未着手のまま）。

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1708 / q1708-i01 | P8（phase-runner-low、worktree `/home/awe/zedBSD-worktrees/p8-ws074`・branch `agent/p8-ws074`、基点 main 2ba50ae52、host だけ） | [ws074-p260](ws074/phase260/phase.md)（と p261・p262） | local の host で Chromium との比較の環境を作り直し（WPT の固定 commit を自分の build へ、Debian の chromium 153.0.8010.52）、Chromium の基準と dev20 の全件の測定、その後 gap の大きい原因から libbrowser を直す。区切りごとに全件の測定で後退 0 | uncleared（2026-10-30 返却。Debian の比較の環境 5e20c8da9、測定は背景で継続、修正 0。q1708-i02 で続ける） |

## 2026-10-30: WS313・WS327・WS307・WS291 を P1・P5・P6・P7 で（q1709〜）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1709 / q1709-i01 | P1（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p1-ws313`・branch `agent/p1-ws313`、基点 main 5aca73738、host・image の build） | [ws313](ws313/ws.md) ODROID-C5 の BSP と Mali | ユーザー「WS313をP1で実行します。ODROIDのテスト用イメージをください。」→ まず実機の試験の image（text の console と Keiland）を作って渡す（bootloader の区画を含む）。その後 ws.md の Resume point から | cleared（2026-10-30 Q1。試験の image 2 つ（text・Keiland）、bootloader は mdrjr/meta-odroid-aml 548e2f037e、試験だけに使う。Phase は実装済み・実機未試験のまま。merge 0384466f5） |
| q1710 / q1710-i01 | P5（phase-runner、worktree `/home/awe/zedBSD-worktrees/p5-ws327`・branch `agent/p5-ws327`、基点 main 5aca73738、QEMU） | [ws327](ws327/ws.md) p016 受け入れの残り → p017 → WS の完了 | ユーザー「WS327をP5で実行します。P5はこのWSが終わったら返却してください。」 | cleared（2026-10-30 Q1。WS327 の全 Phase、cf2963051、merge a17e9386d。完了の片付けは Q1） |
| q1711 / q1711-i01 | P6（phase-runner、worktree `/home/awe/zedBSD-worktrees/p6-ws307`・branch `agent/p6-ws307`、基点 main 5aca73738、QEMU） | [ws307](ws307/ws.md) graphical installer の残り（Resume point から） | ユーザー「WS307をP6で実行します。P6はこのWSが終わったら返却してください。」 | uncleared（2026-10-30 返却。p015・p004・p008 cleared、p007 はユーザーの判断待ち（G2）、p014 は p007 の後。93f7ec9d3、merge） |
| q1712 / q1712-i01 | P7（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p7-ws291`・branch `agent/p7-ws291`、基点 main 5aca73738、実機 10.0.10.3） | [ws291](ws291/ws.md) GPU の描画の速さ（NVIDIA の段、p010 の q1700-i01 節から → p012 以降） | ユーザー「WS291をP7で実行します。P7はこのWSが終わったら返却してください。」 | uncleared（2026-10-30 返却。p010 cleared、p012 uncleared（hardware の限界）、RDNA は実機なしで止め、p020 は VF2 の画面なしと基準未決で止め。6c49a6095、merge 28cf45cc8） |

## 2026-10-30 15:0x: WS331 を P2 で（q1713）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1713 / q1713-i01 | P2（phase-runner、worktree `/home/awe/zedBSD-worktrees/p2-ws331`・branch `agent/p2-ws331`、基点 main、host だけ） | [ws331-p001](ws331/ws.md) | 一覧の作成だけ（code は変えない）。ユーザー「了解です。ガードレールに記載してください。これを修正するWSを立ててください。P2で実行してください。HALのマップ関数には、カーネル側に相当する関数があるはずです。まずは違反利用された関数が、どの関数に置き換えられるか、一覧を作成してください。そして、ラッパーがない場合、カーネルに関数追加しますので、その一覧をください。」 | cleared（2026-10-30 Q1。p001 の一覧 6af47c4a4、merge f4fcb6ff2。Phase は in-progress のまま、ユーザーの決定待ち） |

## 2026-10-30: WS313 の firmware を P1 で（q1714）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1714 / q1714-i01 | P1（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p1-ws313`・branch `agent/p1-ws313`（main に追従）と zedBSD-firmware の worktree `/home/awe/zedBSD-worktrees/p1-c5-firmware`・branch `agent/p1-c5-firmware`、host だけ） | [ws313](ws313/ws.md) p003 媒体（firmware の package 化） | ユーザー「P1: ODROIDで使うamlogicブートローダとMaliファームウェアは、zedBSD-firmwareリポジトリに追加してください。make menuconfigにファームウェアとして入れてください。当然他のファームウェアと同じでライセンス合意も必要です。ODROID-C5のBSPを選ぶと、Amlogicのこのチップのファームウェアが自動で依存関係で有効になるようにしてください。」 zedBSD-firmware の push はユーザーの指示の時だけ（Q1） | uncleared（2026-10-30 返却。package・menuconfig・firmware.list は 9c09f68f4、zedBSD-firmware は local の 227a76db8。package 経由の image の build は license の自動の同意が権限の判定で止まり未実施。zedBSD-firmware の push はユーザーの判断待ち（Amlogic の license）） |
| q1708 / q1708-i02 | P8（phase-runner-low、同じ worktree p8-ws074） | [ws074-p260](ws074/phase260/phase.md) | i01 の続き: 背景の測定の compare、gap の大きい原因から libbrowser の修正 | uncleared（2026-10-30 返却。Chromium 156 の PASS 3,472 のうち 2,105 → 2,215（63.80%）、後退 0、Acid3 100/100。9df11eec9、merge 11888c436。p260 は in-progress） |

## 2026-10-30 16:0x: WS291 PowerVR を P3 で（q1715）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1715 / q1715-i01 | P3（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p3-ws291`・branch `agent/p3-ws291`、基点 main、実機 VF2） | [ws291](ws291/ws.md) p020 PowerVR の model 50 fps | ユーザー「WS291のPowerVRの最適化ですが、VF2が空いているので、P3で実行してOKです。」 基準はユーザーの未回答の間 Q1 の既定: zedBSD の今の fps で 40 fps 前後の scene を決め、50 fps 以上へ | uncleared（2026-10-30 返却。VF2 の HDMI に画面が無く loader の framebuffer が無い（元の kernel でも同じ、10-28 から）。fps は未計測。a7e9379f5） |

## 2026-10-30: WS313 の bootloader を公式の image から（q1716）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1716 / q1716-i01 | P1（phase-runner-high、worktree p1-ws313 と p1-c5-firmware） | [ws313](ws313/ws.md) p003 媒体 | ユーザー「このファイルは、このボードで利用する目的であれば、再配布可能です。zedBSD-firmwareに入れてください。ライセンスは下記です。 prior written permissionはHardkernelに与えられており、このボードでしか使用できないイメージであれば、再配布が可能なんだそうです。」（下に Amlogic の license の本文、2018 Amlogic の「Redistribution and use … strictly prohibited without prior written permission from Amlogic.」） bootloader を公式の image の物に替え、zedBSD-firmware に license の本文と一緒に入れる。試験の image を作り直す。push は Q1 | cleared（2026-10-30 Q1。zedBSD-firmware 43662bd（未 push）、zedBSD 1bf02b7f・40e11db1（revision）・966a7eac・68683b74。試験の image 2 つは公式の bootloader。main への merge は zedBSD-firmware の push の後） |
| q1715 / q1715-i02 | P3（phase-runner-high、同じ worktree p3-ws291、実機 VF2） | [ws291](ws291/ws.md) p020 PowerVR の model 50 fps | ユーザー「VF2はHDMIを接続しました。P3を再始動し、必要ならVF2をSwitchbotで再起動してください。」 i01 の続き（範囲の 2: scene の決定・計測・改善） | uncleared（2026-10-30 返却。driver の欠け 7 つを直し qs40 × 13 は direct で 26.57 fps、目標 50 に未達、profile の途中。cb03c9983、merge 7e77db340） |

## 2026-10-30: WS331 p002〜p004 を P2 で（q1717）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1717 / q1717-i01 | P2（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p2-ws331`・branch `agent/p2-ws331`、main に追従、host・QEMU） | [ws331-p002](ws331/ws.md)・[p003](ws331/ws.md)・[p004](ws331/ws.md) | ユーザー「表A,Bを許可します」 | cleared（2026-10-30 Q1。p002・p003・p004 cleared） |

## 2026-10-30: WS307 p007 を P6 で（q1718）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1718 / q1718-i01 | P6（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p6-ws307`・branch `agent/p6-ws307`、main に追従、QEMU（OVMF）） | [ws307-p007](ws307/ws.md) → p014 → WS307 の完了 | ユーザー「WS307の判断事項は、HAL_SPACE_SYS で 実行可能フラグが使えない実装になっているから、使えるようにしよう、という指摘ですか？もしそうなら、それは実装の問題であって、HALのAPIの変更ではないです。」（改訂 5 の G1〜G3 で実装） | uncleared（2026-10-30 返却。p007 の efiboot の OVMF の試験 PASS、p014 cleared。残り: install-admin の導入の QEMU、古い entry の掃除。ccd0c8693、merge 061230951） |

## 2026-10-30: WS331 p005 を P2 で（q1719）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1719 / q1719-i01 | P2（phase-runner-high、worktree p2-ws331、QEMU） | [ws331-p005](ws331/ws.md) | 2026-10-30 ユーザーから、Raspberry Pi の console を kernel_putc の形に今作り直す、という趣旨の指示を受けた。pc98 の 3 か所も | uncleared（2026-10-30 返却。p005 は実装と QEMU・host・VF2 の起動まで。残り: VF2 の画面の目視、amd64 の Venus、Pi で GPU の最初の flip の EBUSY（HAL が framebuffer の view を持つ）） |

## 2026-10-30: WS331 p007 HAL の early console の framebuffer の窓を他の platform へ（q1720）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1720 / q1720-i01 | P2（phase-runner-high、worktree p2-ws331、QEMU、実機 VF2） | [ws331-p007](ws331/ws.md) | 2026-10-30 ユーザーから、HAL が unmap device できるよう early console の GOP の領域を map の list に入れておく形（GHCI の時に PC で入った物）を確かめ、他の platform でも同じにする、という趣旨の指示を受けた | cleared（2026-10-30 Q1。arm64・riscv64 の窓、boot-fb の end。QEMU raspi4b で窓が外れる事、VF2 の実機で窓が外れる事（dmesg）、20 port の build。52ccb91b3） |

## 2026-10-30: WS331 p008・p009 を P2、WS332 を P3 で（q1721・q1722）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1721 / q1721-i01 | P2（phase-runner-high、worktree p2-ws331、QEMU） | [ws331-p008](ws331/ws.md) → [p009](ws331/ws.md) | 2026-10-30 ユーザーから、Pi の共通化と ODROID-C5 の分離、kernel の header の _private.h の分離という趣旨の指示を受けた | uncleared（2026-10-30 返却。p008 済み（watch.c の HVS を bsp-rpi4 へ）、p009 は atomic.h を除いて済み。atomic.h はユーザーの判断待ち。570e03603、merge 2aa9aeabd） |
| q1722 / q1722-i01 | P3（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p3-ws332`・branch `agent/p3-ws332`、host・QEMU） | [ws332-p001](ws332/ws.md) → [p002](ws332/ws.md) | 2026-10-30 ユーザーから、KERN_CLOCK_HZ を今回削除し、tick を API で扱い可変にする WS を Q1 の提案どおり立てる、という趣旨の指示を受けた | cleared（2026-10-30 Q1。p001 設計、p002 KERN_CLOCK_HZ の削除、wlan の UAPI の deadline_ns、x68k の FPU の検査。19 port の build、QEMU 5 port、host 試験。24a5cdb0d、merge 9d6ed7005） |

## 2026-10-30: WS331 p010 を P2 で（q1723）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1723 / q1723-i01 | P2（phase-runner-high、worktree p2-ws331、QEMU） | [ws331-p010](ws331/ws.md) | 2026-10-30 ユーザーから、HAL の atomic.h を macro だけの契約にして kernel の atomic.h で undef する形と、pc98・pcat の legacy の窓の番地を driver の基底で書く形の趣旨の指示を受けた | cleared（2026-10-30 Q1。atomic.h の分離（案 C）、pc98・pcat の窓の番地、20 port の build、QEMU pcat・amd64・rpi4・m68k、host 試験、gpu の build の選択の試験は Q1 が流して PASS。7396b8440、merge 94e9b6e88） |

## 2026-10-30: WS331 p011 を P2 で（q1724）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1724 / q1724-i01 | P2（phase-runner-high、worktree p2-ws331、QEMU） | [ws331-p011](ws331/ws.md) | 2026-10-30 ユーザーから、arch の spinlock の取得を include/hal/arch/atomic/<arch>.h に分けて hal/atomic.h の macro の契約に入れる実装の指示を受けた | cleared（2026-10-30 Q1。arch の spinlock の取得・待ちの合図を include/hal/arch/atomic/<arch>.h の macro に、lock.c の out-of-line をやめた。20 port の build、QEMU pcat・amd64・rpi4・m68k、host 試験、gpu の build の選択の試験（Q1）。fa204ae82、merge 9e9b4a9c8） |

## 2026-10-30: WS009 を P7 で（q1725）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1725 / q1725-i01 | P7（phase-runner、worktree `/home/awe/zedBSD-worktrees/p7-ws009`・branch `agent/p7-ws009`、QEMU） | [ws009](ws009/ws.md) p028（clean build と QEMU の起動の再現）→ WS の受け入れの確認 → 完了の形 | 2026-10-30 ユーザーから、WS009 を P7 で実行する、という趣旨の指示を受けた | cleared（2026-10-30 Q1。p028 cleared、WS009 completed。89e9ecbb1、merge 94f1dec75） |

## 2026-10-30: WS291 p020 を P2 で（q1726）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1726 / q1726-i01 | P2（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p2-ws291v`・branch `agent/p2-ws291v`、実機 VF2） | [ws291-p020](ws291/phase020/phase.md) PowerVR の qs40 × 13 で 50 fps | 2026-10-30 ユーザーから、WS291 の PowerVR の最適化を P2 で実行する、という趣旨の指示を受けた | uncleared（2026-10-31 返却。direct で c13 42.35 fps（目標 50）。板で計測の command が権限の判定で拒否され止まった。板は元に戻した。branch agent/p2-ws291v dc49bd786 は d983c4ee5（scanout の直し）を含むので BUG-375 が直るまで main に merge しない） |

## 2026-10-30: WS074 を P8 で自走（q1727）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1727 / q1727-i01 | P8（phase-runner-low、worktree p8-ws074） | [ws074-p260](ws074/phase260/phase.md) | 2026-10-30 ユーザーから、Interop 2025 で Chromium が PASS する項目の 100% をゴールに P8 が自走する、という趣旨の指示を受けた。返却のたびに Q1 が次の attempt で起動し直す | uncleared（2026-10-31 返却（自走の 1 回目）。2,386 / 3,472 = 68.72%、後退 0、Acid3 100/100。fccca00dc、merge） |

## 2026-10-30: WS291 p013 を P3 で（q1728）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1728 / q1728-i01 | P3（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p3-ws291r`・branch `agent/p3-ws291r`、実機 10.0.10.3） | [ws291-p013](ws291/phase013/phase.md) | 2026-10-30 ユーザーから、WS291 の RTX の改善を P3 で行う、という趣旨の指示を受けた | uncleared（2026-10-31 返却。big-1000 48 → 69.7 fps、big-100 401〜420 → 567〜604、subs-1000 11（NVK 75.4）。差の内訳は phase013。BUG-374 は解析だけ。823ce7bde、merge ccd7bf2d2） |

## 2026-10-30: WS291 p021 を P4 で（q1729）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1729 / q1729-i01 | P4（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p4-ws291pi`・branch `agent/p4-ws291pi`、image の build、実機 Pi 5 はユーザーが起動した後 SSH） | [ws291-p021](ws291/phase021/phase.md) | 2026-10-30 ユーザーから、P4 で Pi 5 の GPU の改善を行い、PowerVR と同じくスペックの比から目標を決め、Pi 5 の image を渡す、という趣旨の指示を受けた | uncleared（2026-10-30 ラップアップ。image を作った（rpi5-p021.img）、driver の欠けの設計。権限の判定で Mesa の照合の試験が止まり、ユーザーの許可の後に q1730 で起動し直す。5291c05b1、merge 5a329d692） |

## 2026-10-30: WS291 p021 の続き（P4）と p022（P1）（q1730・q1731）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1730 / q1730-i01 | P4（phase-runner-high、worktree p4-ws291pi） | [ws291-p021](ws291/phase021/phase.md) | 2026-10-30 ユーザーから、P4 に plan/ws141/temp/ の利用を許可して起動し直す、という趣旨の指示を受けた。driver の欠け（indexed・instance・mip・直の fetch）の実装と V3D の host 試験 | uncleared（2026-10-31 返却。host: indexed・instance・直の fetch・mip の第 1 段、glass の試験の直し、Pi 4・Pi 5 の build warning 0、host 試験 PASS。実機は未。branch agent/p4-ws291pi e4f77b9e5 は Pi 4 の desktop の回帰を実機で確かめるまで main に merge しない） |
| q1731 / q1731-i01 | P1（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p1-ws291i`・branch `agent/p1-ws291i`、実機 5330） | [ws291-p022](ws291/phase022/phase.md) | 2026-10-30 ユーザーから、i915 も WS291 に Phase を作り 5330 で最適化を行い P1 を割り当てる、という趣旨の指示を受けた | in-progress・返却（2026-10-31 P1: c600 23.0 → 32.6 fps、Linux 50.6 に未達。主因は電力の上限 PL1 13 W で WS333 待ち。最後 6e6c26b8c、merge d26c657c7） |

## 2026-10-31: BUG-371・372・373 を P7 で（q1732）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1732 / q1732-i01 | P7（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p7-bug373`・branch `agent/p7-bug373`、QEMU） | [BUG-373](bugs/BUG-373.md)・[BUG-371](bugs/BUG-371.md)・[BUG-372](bugs/BUG-372.md) | 今日の変更による network の退行（Q1 の判断、AGENTS.md の Bug の扱い） | cleared（30fd47a4f・157a69cc0・c5a5e14b8・cb2f410a4、main 52aa284ba。BUG-371・372・373・375 closed、VF2 の実機で確かめた） |

## 2026-10-31: WS291 p020 の続き（P2、q1733）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1733 / q1733-i01 | P2（phase-runner-high、worktree p2-ws291v、実機 VF2） | [ws291-p020](ws291/phase020/phase.md) | 2026-10-31 ユーザーから、P2 が VF2 で SSH と sudo を使って kernel の入れ替えと計測の program の実行を続けてよい、という趣旨の指示を受けた | in-progress・返却（2026-10-31 P2: c13 42.30 → 45.78 fps、50 に未達。c8f69f7fa・806ff2ca2 は agent/p2-ws291v、BUG-375 の直しの後に merge） |
| q1727 / q1727-i02 | P8（phase-runner-low、worktree p8-ws074、host と QEMU） | [ws074-p260](ws074/phase260/phase.md) | i01 の続き（100% まで自走）。最初に zedBSD の image で libbrowser の build と QEMU の起動を確かめる | in-progress・返却（2,386 → 2,427、main 9175ad146。i03 へ） |

## 2026-10-31: WS333 p001（P5）・p002（P1）（q1734・q1735）

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1734 / q1734-i01 | P5（kernel-and-driver-designer、worktree `/home/awe/zedBSD-worktrees/p5-ws333`・branch `agent/p5-ws333`） | [ws333-p001](ws333/phase001/phase.md) | 2026-10-31 ユーザーから、WS301・WS303 を統合した WS333 で CPU の省電力を先に直す、という趣旨の指示を受けた | uncleared（2026-10-31: 設計の第 2 版 4b80eb69c、merge 9946e22c0。理由: ユーザーの問い Q333-1〜7 の答え待ち、推しで進めてよい） |
| q1735 / q1735-i01 | P1（phase-runner-high、worktree p1-ws291i、実機 5330） | [ws333-p002](ws333/phase002/phase.md) | 同上（amd64 の idle の MWAIT を先に） | in-progress（MWAIT C6 e44a18dd7・review の追い 09f8fd4ea、main に merge d26c657c7。idle 13.00 → 11.70 W。残りは WS333 の後の Phase） |

## 2026-10-31: 担当の無い仕事の割り当て（q1736〜q1742）

2026-10-31 ユーザーから、WS307・WS331・WS332・WS326 を空いている P3 に、BUG-370 と WS291 を P6 に、BUG-374 を P4 に割り当てる、という趣旨の指示を受けた。

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1736 / q1736-i01 | P3（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p3-seq`・branch `agent/p3-seq`、QEMU（OVMF）） | [ws307](ws307/ws.md) p007 の残り → WS307 の完了 | 上のユーザーの指示 | cleared（515766eaa・c8c98ef5d、merge db69ae87e。p007-install-entry PASS、WS307 completed） |
| q1737 / q1737-i01 | P3（同じ worktree、q1736 の後） | [ws331](ws331/ws.md) p006 規約の全文の見直し → WS331 の完了 | 上のユーザーの指示 | cleared（db1fd7749〜8aca507b9、main 1035ec0a0。20 port の build・QEMU 4 つ PASS、WS331 completed） |
| q1738 / q1738-i01 | P6（2026-10-31 Q1 が P3 から移した、worktree `/home/awe/zedBSD-worktrees/p6-ws332`・branch `agent/p6-ws332`） | [ws332](ws332/ws.md) p003（率を仮定する残り）→ p004 規約 → WS332 の完了 | 上のユーザーの指示 | cleared（ca16e864b〜4c6c4de88、main 39fc879c5。WS332 completed） |
| q1739 / q1739-i01 | P3（同じ worktree、q1738 の後、実装は phase-runner-low を標準廉価実装手順で） | [ws326](ws326/ws.md) p015・p017 以降 | 上のユーザーの指示 | canceled（2026-10-31 WS326 を昼までに終える指示で Q1 が直に回す、q1748 以降） |
| q1740 / q1740-i01 | P6（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p6-ws291`・branch `agent/p6-ws291`、QEMU） | [BUG-370](bugs/BUG-370.md)（Photos で保存すると全面表示が閉じる） | 上のユーザーの指示 | cleared（acf7ff71a・3dc13b2e5、BUG-370 closed） |
| q1741 / q1741-i01 | P6（同じ worktree、q1740 の後、実機 10.0.10.3） | [ws291-p014](ws291/phase014/phase.md) | 上のユーザーの指示 | in-progress・返却（4b7d0551c: big-1000 67〜69 → 85〜91 fps、NVK 136 に未達。残りは kernel の decode の短縮と WSI の block-linear） |
| q1742 / q1742-i01 | P4（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p4-bug374`・branch `agent/p4-bug374`、実機 10.0.10.3 は P6 と lock で交代） | [BUG-374](bugs/BUG-374.md)（RTX の cursor の色が逆） | 上のユーザーの指示 | uncleared（2026-10-31 P4 b2e2993de、merge 54a134140。理由: ユーザーの目視待ち。診断の kernel と手順は ticket） |
| q1743 / q1743-i01 | P6（同じ worktree p6-ws291、q1740 の後、q1741 の前、QEMU） | [BUG-376](bugs/BUG-376.md) | 2026-10-31 ユーザーから、PS/2 の aux を起票してどこでもいいので割り当てる、という趣旨の指示を受けた | cleared（933563341、BUG-376 closed、QEMU と実機） |
| q1744 / q1744-i01 | 5330 が空いた担当（P1 の q1735・q1731 の後） | [ws299](ws299/ws.md) 5330 の NVMe の速さの続き | 2026-10-31 ユーザーから、WS299 は 5330 が空いたらどの担当でもよいのでスケジュールする、という趣旨の指示を受けた | in-progress・返却（3a4e64353〜5857d62a4、main f31bbac2e。QEMU の UFS 新 file 490→851 MB/s・上書き 410→852、crash 試験 PASS。5330 の実測は電源の入れ直しの後、plan/ws299/temp/run-5330-q1744.sh） |
| q1745 / q1745-i01 | P4（Pi 5 の WS291 p021 の後、実機 Pi 5） | [ws304](ws304/ws.md) Pi 5 の外の PCIe の続き（ハードの切り分けを含む） | 2026-10-31 ユーザーから、WS304 は空いている所にスケジュールする、という趣旨の指示を受けた | pending |
| q1746 / q1746-i01 | P5（WS333 p001 の後） | [ws306](ws306/ws.md) p001 設計 | 2026-10-31 ユーザーから、WS306 は検討済みなので進め、決定事項が不明なら cockpit に書けば答える、という趣旨の指示を受けた | uncleared（2026-10-31: 設計の第 3 版 3f1e3c655、merge 843847185。理由: ユーザーの問い Q306-7・Q306-9 の答え待ち。実装は Q306-9 の後） |
| q1747 / q1747-i01 | P4（q1742 BUG-374 の後、実機 10.0.10.3 の ue0、lock で P6 と交代） | [BUG-222](bugs/BUG-222.md) の試験 | 2026-10-31 ユーザーから、BUG-222 は別のチケットで解決済みの見込みなので試験だけ流して完了にする、という趣旨の指示を受けた | cleared（5d51b15b6、merge dd85f56ee。BUG-222 closed） |

## 2026-10-31: WS326 を昼までに（q1748・q1749）

2026-10-31 ユーザーから、libmedia の実装の WS を昼までに終わるよう優先度を上げてスケジュールする、という趣旨の指示を受けた。Q1 が Sonnet の実装の担当を直に起動し、依存の順に回す。

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1748 / q1748-i01 | S-p015（phase-runner-low、worktree `/home/awe/zedBSD-worktrees/s-ws326-p015`・branch `agent/s-ws326-p015`） | [ws326-p015](ws326/ws.md) | phase.md のとおり（header を変えない） | cleared（34119fa06、test-interp 23460・test-weight 159 checks） |
| q1749 / q1749-i01 | S-p017（phase-runner-low、worktree `/home/awe/zedBSD-worktrees/s-ws326-p017`・branch `agent/s-ws326-p017`） | [ws326-p017](ws326/ws.md) | phase.md のとおり（header を変えない） | cleared（2134a8efe、test-direct 259 checks） |

## 2026-10-31: Bug の自動の割り当て（q1750）

2026-10-31 ユーザーから、Bug も自動でスケジュールして直し、原因が分からなければ bug-analyzer を使う（BUG-375 は特に）、という趣旨の指示を受けた。

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1750 / q1750-i01 | P2（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p2-bugs`・branch `agent/p2-bugs`、QEMU と host） | [BUG-363](bugs/BUG-363.md)・[BUG-344](bugs/BUG-344.md)・[BUG-366](bugs/BUG-366.md)・[BUG-354](bugs/BUG-354.md)・[BUG-328](bugs/BUG-328.md)・[BUG-352](bugs/BUG-352.md) | ticket ごとに直し・確かめ・ticket と Bug Board の行の更新 | cleared（8c8c556b5〜ad1c430c1、main f81fef099。6 件とも QEMU で PASS、closed） |
| q1751 / q1751-i01 | S-p019（phase-runner-low、worktree `/home/awe/zedBSD-worktrees/s-ws326-p019`・branch `agent/s-ws326-p019`） | [ws326-p019](ws326/ws.md) | phase.md のとおり（header を変えない） | cleared（053bafc0b、test-recon 33584 checks） |

## 2026-10-31: WS318 の設計（q1752）

2026-10-31 ユーザーから、不在の間は設計だけ進められる WS は設計を進め、libmedia の WS の優先度を上げる、という趣旨の指示を受けた。

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1752 / q1752-i01 | P5（kernel-and-driver-designer、worktree `/home/awe/zedBSD-worktrees/p5-ws318`・branch `agent/p5-ws318`） | [ws318](ws318/ws.md) p001 設計 | plan/ws318/ だけ、design-reviewer と問いの一覧まで | uncleared（2026-10-31: 設計 e4ce407a1、merge 85fbeb091。理由: ユーザーの問い Q318-1〜14 の答え待ち） |

## 2026-10-31: Bug の 2 つ目の束（q1753）

上の q1750 と同じユーザーの指示（Bug を自動でスケジュールして直す）。

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1753 / q1753-i01 | P4（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p4-bugs`・branch `agent/p4-bugs`、QEMU と host、image は /tmp） | [BUG-351](bugs/BUG-351.md)・[BUG-349](bugs/BUG-349.md)・[BUG-362](bugs/BUG-362.md)・[BUG-368](bugs/BUG-368.md)・[BUG-359](bugs/BUG-359.md) | ticket ごとに直し・確かめ・ticket と Bug Board の行の更新 | cleared（d3219b01a〜5b56cbaef、main b0b4577c7。BUG-351・359 closed（QEMU）、349・362・368 fixed（確かめの残り）） |

## 2026-10-31: WS333 の実装の始め（q1754）

2026-10-31 ユーザーから、不在の間は実装できる WS は実装だけでも進め、試験できるなら試験もする、という趣旨の指示を受けた。Q333-1〜7 は推しで進める（CPU の道は問いを待たない）。

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1754 / q1754-i01 | P7（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p7-ws333`・branch `agent/p7-ws333`、host と build） | [ws333](ws333/ws.md) p003 → p006 | kernel の口の header の案と review、cpuperf の枠組みの kernel の実装と host 試験。UAPI（p004）は作らない | in-progress・返却（p003 cleared main f197c8a3f。p006 は 44678bac6・c33842bc9、host 試験 196 checks PASS・20 port の build、QEMU の起動は q1759 で） |
| q1755 / q1755-i01 | O-p021（phase-runner-high、Opus、worktree `/home/awe/zedBSD-worktrees/o-ws326-p021`・branch `agent/o-ws326-p021`） | [ws326-p021](ws326/ws.md) | phase.md のとおり。h264sw-reflist.h の非存在の frame の印だけ header を直してよい（新しい BASE を記録） | cleared（8ddb130a9・86539318e。22 本 408 frame が ffmpeg と一致、新しい BASE 8ddb130a9） |
| q1756 / q1756-i01 | S-p023（phase-runner-low、worktree `/home/awe/zedBSD-worktrees/s-ws326-p023`・branch `agent/s-ws326-p023`） | [ws326-p023](ws326/ws.md) | phase.md のとおり。p021 と並べ、実 stream の試験は p021 の merge の後 | cleared（ac1109d25・37789c4d6。run-host-decode PASS） |

## 2026-10-31: Bug の 3 つ目の束（q1757）

上の q1750 と同じユーザーの指示（Bug を自動でスケジュールして直す）。

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1757 / q1757-i01 | P2（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p2-bugs3`・branch `agent/p2-bugs3`、QEMU と host、image は /tmp） | [BUG-360](bugs/BUG-360.md)・[BUG-306](bugs/BUG-306.md)・[BUG-292](bugs/BUG-292.md)・[BUG-289](bugs/BUG-289.md)・[BUG-365](bugs/BUG-365.md) | ticket ごとに直し・確かめ・ticket と Bug Board の行の更新 | cleared（e3c3eb4db〜b3aa69fa2、main 612a2ea04。BUG-289・292 closed、360・306 fixed（確かめの残り）、365 一部） |
| q1758 / q1758-i01 | P5（kernel-and-driver-designer、worktree `/home/awe/zedBSD-worktrees/p5-ws329`・branch `agent/p5-ws329`） | [ws329](ws329/ws.md) p001 設計 | plan/ws329/ だけ、design-reviewer と問いの一覧まで。実装は WS326 の完了の後 | uncleared（設計 c56fb91dc、merge 455a7d5b4。理由: ユーザーの問い Q329-1〜5 の答え待ち） |
| q1759 / q1759-i01 | P7（phase-runner-high、同じ worktree p7-ws333） | [ws333](ws333/ws.md) p006 の QEMU の起動の回帰 → p007 x86 の decode の module の設計 | boot-test.sh で amd64・armv7-virt・riscv64-virt・raspi4b。p007 は design.md §9 のとおり、5330 の読みは未実施で可 | cleared（p006 の起動の回帰 d76310ea4、p007 39f4a5597・f45480c58） |
| q1760 / q1760-i01 | O-p022（phase-runner-high、Opus、worktree `/home/awe/zedBSD-worktrees/o-ws326-p022`・branch `agent/o-ws326-p022`） | [ws326-p022](ws326/ws.md) | phase.md のとおり。h264sw-*.c の修正は可、header は不可。check-skeleton の BASE は 8ddb130a9 | cleared（4568d794b〜c80e33c37、in 101 本が全 frame 一致） |

## 2026-10-31: Bug の 4 つ目の束（q1761）

上の q1750 と同じユーザーの指示（Bug を自動でスケジュールして直す）。

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1761 / q1761-i01 | P2（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p2-bugs4`・branch `agent/p2-bugs4`、QEMU と host、出力は全部 /tmp） | [BUG-312](bugs/BUG-312.md)・[BUG-173](bugs/BUG-173.md)・[BUG-238](bugs/BUG-238.md)・[BUG-199](bugs/BUG-199.md)・[BUG-281](bugs/BUG-281.md)・[BUG-308](bugs/BUG-308.md) | ticket ごとに直し・確かめ・ticket と Bug Board の行の更新 | cleared（4f19b40a4〜b35939531、main 9154dc6db。312・173・238・199・281 closed、308 fixed（review と crash 試験の残り）） |
| q1762 / q1762-i01 | P6（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p6-ws333`・branch `agent/p6-ws333`、host と QEMU、出力は /tmp） | [ws333](ws333/ws.md) p005 の前半（熱の core） | UAPI（p004）・/dev/thermal・/sbin/thermal は作らない | in-progress・返却（前半の熱の core b122563a8〜259e45cd7、main b7ac4f576。後半は p004 の後） |

## 2026-10-31: 空いた P3・P5 の割り当て（q1763・q1764）

2026-10-31 ユーザーから、不在の間は設計だけ進められる WS は設計を、実装できる WS は実装を、試験できるなら試験も進める、という趣旨の指示を受けた。

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1763 / q1763-i01 | P5（kernel-and-driver-designer、worktree `/home/awe/zedBSD-worktrees/p5-ws330`・branch `agent/p5-ws330`） | [ws330](ws330/ws.md) p001 設計 | plan/ws330/ だけ、design-reviewer と問いまで。実装は WS329 の完了の後 | uncleared（設計 84d01a58a、merge 299a2f4b3。理由: ユーザーの問い Q330-1〜4 の答え待ち） |
| q1764 / q1764-i01 | P3（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p3-ws291r2`・branch `agent/p3-ws291r2`、実機 10.0.10.3） | [ws291-p014](ws291/phase014/phase.md) の続き | kernel の record の decode の短縮など、phase014 の再開点の候補 | in-progress・返却（d74ebaff5〜1f66a9e89、main 7dd9f1538。big-1000 116 fps、subs は判断待ち） |
| q1765 / q1765-i01 | S-333p008（phase-runner-low、worktree `/home/awe/zedBSD-worktrees/s-ws333-p008`） | [ws333-p008](ws333/phase008/phase.md) | phase.md のとおり、header を変えない（BASE 39f4a5597） | cleared（fae4a0d41） |
| q1766 / q1766-i01 | S-333p009（phase-runner-low、worktree `/home/awe/zedBSD-worktrees/s-ws333-p009`） | [ws333-p009](ws333/phase009/phase.md) | 同上 | cleared（9fb61c61e） |
| q1767 / q1767-i01 | S-333p010（phase-runner-low、worktree `/home/awe/zedBSD-worktrees/s-ws333-p010`） | [ws333-p010](ws333/phase010/phase.md) | 同上。legacy の 0xCE の比の道は p011 で使わない（Q1 の判断） | cleared（35d5d4500） |
| q1768 / q1768-i01 | P7（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p7-ws333b`・branch `agent/p7-ws333b`、QEMU、出力は /tmp） | [ws333](ws333/ws.md) p012（tick の位相を揃える）→ [BUG-377](bugs/BUG-377.md) | p012 は amd64 の HAL の実装と QEMU の起動まで、5330 の計測は後。BUG-377 は config の依存の宣言だけ（noct の規則は変えない） | in-progress・返却（p012 9b86c8c4a は main 22c629663、5330 の電力の数待ち。BUG-377 87a6d4ff7 fixed） |
| q1727 / q1727-i03 | P8（phase-runner-low、同じ worktree p8-ws074） | [ws074](ws074/ws.md) phase260 の自走の 3 回目 | 100% までの自走（ユーザーの指示）、計測の出力は /tmp | in-progress・返却（2,427 → 2,507（72.21%）、main bdbf90154。i04 へ） |

## 2026-10-31: 空いた P4・P6・P7 の割り当て（q1769〜q1771）

2026-10-31 ユーザーから、不在の間は実装できる WS は実装を、試験できるなら試験も進め、libmedia の WS は昼までに終えるよう優先度を上げる、という趣旨の指示を受けた。

| Queue | 担当 | Phase | 範囲 | 結果 |
| --- | --- | --- | --- | --- |
| q1769 / q1769-i01 | P4（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p4-ws326p026`・branch `agent/p4-ws326p026`） | ws326-p026 | 設計を先に、移しは p022 の merge の後（Q1 が知らせる） | cleared（acb27716b〜ac38fd1d9、新しい BASE 92ca2d0e8） |
| q1770 / q1770-i01 | P7（phase-runner-high、worktree p7-ws333b） | [ws333](ws333/ws.md) p011 x86 の CPU の driver の統合 | HWP の道だけ（legacy の 0xCE の比の道は作らない）。5330 は p014 で | in-progress・返却（a904b540d〜dc2d8a647、main a058c08d7。5330 は p014） |
| q1771 / q1771-i01 | P6（phase-runner-high、worktree p6-ws333） | [ws333](ws333/ws.md) p017 ACPI WMI の層 | SMI を起こさない。AML の harness を plan/tools/aml-host/ へ | cleared（caff3950b〜93fb7eadd、main 809dfe329） |
| q1772 / q1772-i01 | P2（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p2-vf2bugs`・branch `agent/p2-vf2bugs`、実機 VF2） | [BUG-338](bugs/BUG-338.md)・[BUG-342](bugs/BUG-342.md)・[BUG-340](bugs/BUG-340.md)・[BUG-343](bugs/BUG-343.md)・[BUG-339](bugs/BUG-339.md) | 今の main の kernel で実機で確かめ、残りは直す。出力は /tmp | cleared（912e43c1f・f12ef81f2、main f52088f45。338・340・342・343 closed、339 は判断待ち） |
| q1773 / q1773-i01 | P1（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p1-ws326p024`・branch `agent/p1-ws326p024`） | [ws326-p024](ws326/ws.md) | 計測・profile・目標・案を先に、h264sw-*.c の変更は p026 の merge の後 | cleared（b667c71b5〜544298816、main 13a9ae886） |
| q1774 / q1774-i01 | P6（phase-runner-high、worktree p6-ws333） | [ws333](ws333/ws.md) p016 見張りと非常の停止 → p021 passive | `thermal.emergency=off` は作らない（ユーザーに聞くまで保留） | cleared（af8aaa5f6〜f857f4647、main a57216e81） |
| q1775 / q1775-i01 | P5（kernel-and-driver-designer、worktree `/home/awe/zedBSD-worktrees/p5-ws302`・branch `agent/p5-ws302`） | [ws302](ws302/ws.md) p001 設計 | plan/ws302/ だけ、design-reviewer と問いまで | uncleared（設計 e6d67143f、merge 896d3e3db。理由: ユーザーの問い Q302-1〜7 の答え待ち） |
| q1776 / q1776-i01 | P7（phase-runner-high、worktree p7-ws333b） | [ws333](ws333/ws.md) p018 Dell の provider | host の harness まで、5330 の実機は電源の入れ直しの後。SMM の失敗の印は値だけを出典つきで（Q1 の判断） | in-progress・返却（2923e980e・cf6ed87fb、main 4d656bf2e。host の harness 30 項目 PASS、5330 の実機は電源の入れ直しの後） |
| q1777 / q1777-i01 | P4（phase-runner、worktree p4-bugs） | [ws333](ws333/ws.md) p019 sessiond の THERMAL の要求 | UAPI（p004）待ちの kernel への呼びは仮の口 | in-progress・返却（370fccb1e を main に。rules の host 試験 113 checks、kernel への呼びは UAPI まで stub） |
| q1778 / q1778-i01 | P1（conformance-reviewer、worktree `/home/awe/zedBSD-worktrees/p1-ws326p025`・branch `agent/p1-ws326p025`） | [ws326-p025](ws326/ws.md) | 書き方だけ、全試験が前と同じ | cleared（181a071ac） |
| q1779 / q1779-i01 | P6（kernel-and-driver-designer、worktree `/home/awe/zedBSD-worktrees/p6-ws310`・branch `agent/p6-ws310`） | [ws310](ws310/ws.md) p001 設計 | plan/ws310/ だけ、design-reviewer と問いまで | uncleared（設計 38f8c85eb、merge a2aa89c4f。理由: ユーザーの問い Q310-1〜6 の答え待ち） |
| q1727 / q1727-i04 | P8（phase-runner-low、同じ worktree p8-ws074） | [ws074](ws074/ws.md) phase260 の自走の 4 回目 | 100% までの自走（ユーザーの指示）、計測は /tmp | in-progress・返却（2,507 → 2,680（77.19%）、main 335892b7b、Acid3 100/100。i05 へ） |
| q1780 / q1780-i01 | P4（phase-runner、worktree `/home/awe/zedBSD-worktrees/p4-ws326close`・branch `agent/p4-ws326close`） | [ws326](ws326/ws.md) の完了の片付け | 試験を plan/tools/h264sw/ へ、参照の書き換え、ws.md を完了の形、Phase の directory の git rm（一覧を Q1 に先に） | cleared（684d0512b、main 8bf8f0280。WS326 completed） |
| q1727 / q1727-i05 | P8（phase-runner-low、同じ worktree p8-ws074） | [ws074](ws074/ws.md) phase260 の自走の 5 回目 | 100% までの自走（ユーザーの指示）。SVG のベクトルの拡大は WS328 の範囲なので触らない | in-progress・返却（2,680 → 2,709（78.02%）、main 89bb4c7c3、Acid3 100/100、後退 1（偶然通っていた subgrid）。i06 へ） |
| q1727 / q1727-i06 | P8（phase-runner-low、同じ worktree p8-ws074） | [ws074](ws074/ws.md) phase260 の自走の 6 回目 | 100% までの自走（ユーザーの指示）。grid の track の大きさの仕様どおりの書き直し → subgrid → 残りの baseline。SVG のベクトルの拡大は WS328 の範囲 | in-progress・返却（2,709 → 2,787（80.27%）、main eaf70b23b、Acid3 100/100、Acid2 99.27%、後退 0。i07 へ） |
| q1727 / q1727-i07 | P8（phase-runner-low、同じ worktree p8-ws074） | [ws074](ws074/ws.md) phase260 の自走の 7 回目 | 100% までの自走（ユーザーの指示）。通る ID の多い順に、inline-block の高さ・縦書きの grid・::first-letter など。SVG のベクトルの拡大は WS328 の範囲 | in-progress・返却（2,787 → 2,848（82.03%）、main 3bf27ad00、Acid3 100/100、Acid2 100%、後退 1（backdrop-filter の境目の 1 画素）。i08 へ） |
| q1727 / q1727-i08 | P8（phase-runner-low、同じ worktree p8-ws074） | [ws074](ws074/ws.md) phase260 の自走の 8 回目 | 100% までの自走（ユーザーの指示）。CSSOM の rule の object・anchor() の直列化・縦書きの container・animation など。SVG のベクトルの拡大は WS328 の範囲 | in-progress・返却（2026-10-31 16:40 host の再起動のためラップアップ。inline.c の 1 件 b2a9ef175、main fc2d32861。後退の確かめは未実施。i09 へ） |
| q1781 / q1781-i01 | P3（kernel-and-driver-designer、worktree `/home/awe/zedBSD-worktrees/p3-ws291wsi`・branch `agent/p3-ws291wsi`） | [ws291-p023](ws291/phase023/phase.md) 設計 | plan/ws291/phase023/ だけ、design-reviewer と問いまで | cleared（設計の第 2 版 54c1b7ca6、merge ffd702bdb。問い 2 つは実装を止めない） |
| q1781 / q1781-i02 | P3（同じ worktree p3-ws291wsi、phase-runner の役で続ける） | [ws291-p023](ws291/phase023/phase.md) 実装 | I1 共通の部分（include/uapi/gpu.h・gpu-share.h・gpu.h・gpu.c、HAL は変えない）→ nvrtx の block-linear の置き方と NVC57E の scanout（既定は確かめまで切る）。2026-10-31 ユーザーの RTX の block-linear の実装の指示 | in-progress・返却（2026-10-31 16:42 再起動のラップアップ。0f1cce8fd、main b30707d0a。subs-1000 10.9 → 21 fps、画素の差 0、desktop の後退なし。scenes の new・bl の腕は未実施） |
| q1782 / q1782-i01 | P2（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p2-ws309v2`・branch `agent/p2-ws309v2`、QEMU と VF2、後で 10.0.10.3） | [ws309-p015](ws309/phase015/phase.md) game mode の V2（zero copy の direct scanout、全 GPU） | phase.md の手順 1〜4、HAL は変えない | in-progress・返却（2026-10-31 16:40 host の再起動のためラップアップ。89d86ef95、main に未 merge。QEMU で 6 往復の zero copy を確かめた、最後の commit は未 build、VF2 は password 待ち） |
| q1782 / q1782-i02 | P2（phase-runner-high、同じ worktree p2-ws309v2、QEMU・10.0.10.3・VF2、後で Pi 4・Pi 5） | [ws309-p015](ws309/phase015/phase.md) の続き | 2026-10-31 ユーザーから、RTX と VF2 で zero copy を実装する、VF2 の後に Pi 4、次に Pi 5 に入れ替えて確かめる、という趣旨の指示。RTX は block-linear の貸し出し（nvrtx.disp.bl は試験の時だけ）、VF2 で通ったら main へ | in-progress・返却（2026-10-31 20:23、7da994932、main d8e39d789。RTX（block-linear、既定で有効）・VF2・Pi 4 の動画で zero copy を確かめた。残り: Pi 4 の 3D（p025 の後）・Pi 5・libvulkan の README・規約の見直し） |
| q1781 / q1781-i03 | P3（phase-runner-high、同じ worktree p3-ws291wsi、10.0.10.3） | [ws291-p023](ws291/phase023/phase.md) block-linear の scanout の直し | 2026-10-31 ユーザーの目視で、block-linear の画面が block 単位で乱れた（写真 build/review/ws291/p023-bl-visual-1.jpg）。原因を調べて直し、自分の方法で確かめてから次の目視を用意する。既定は 0 のまま | cleared（effb3d149、main。ユーザーの 2 回目の目視 PASS） |
| q1727 / q1727-i09 | P8（phase-runner-low、同じ worktree p8-ws074） | [ws074](ws074/ws.md) phase260 の自走の 9 回目 | 100% までの自走（ユーザーの指示、2026-10-31 再開の指示）。i08 の inline.c の確かめ → ::part() → CSSOM の rule の object など。SVG のベクトルの拡大は WS328 の範囲 | in-progress・返却（2026-10-31 20:40、2,848 → 2,922（84.16%）、main b827c1b43、Acid3 100/100・Acid2 100%、後退 0。i10 へ） |
| q1783 / q1783-i01 | P4（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p4-bug378`・branch `agent/p4-bug378`、Pi 4 の実機と SwitchBot 14C19FA80F86） | [BUG-378](bugs/BUG-378.md) arm64 の text が 2 MiB を越えると止まる | src/hal/arm64/ の実装の直し（HAL の API は変えない）、Pi 4 の実機で起動まで | in-progress・返却（2026-10-31 19:16、eff578caf・3ad1a7d8a、main e8f43510d。QEMU raspi4b の起動 PASS、arm64 の 5 config の build warning 0。実機の Pi 4 は SD を挿すのを待つ。P2 が Pi の試験の始めに確かめる） |
| q1784 / q1784-i01 | P4（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p4-ws291pi`・branch `agent/p4-ws291pi`、Pi 4 の実機） | [ws291-p025](ws291/phase025/phase.md) Pi 4 で mview を動かし最適化 | 2026-10-31 ユーザーの指示。driver と Vulkan の backend の中だけ、HAL は変えない。1920x1080 で起動、direct scanout で計測。BUG-380 | in-progress |
| q1727 / q1727-i10 | P8（phase-runner-low、同じ worktree p8-ws074） | [ws074](ws074/ws.md) phase260 の自走の 10 回目 | 100% までの自走（ユーザーの指示）。shadow DOM の段 1 → 縦書きの grid の直交の項目の一般化 → animation など。SVG のベクトルの拡大は WS328 の範囲 | in-progress・返却（2026-10-31 22:51、2,922 → 2,975（85.69%）、main 11159cbbe、Acid3 100/100・Acid2 100%、後退 0。i11 へ） |
| q1785 / q1785-i01 | P3（phase-runner-high、worktree p3-ws291wsi、10.0.10.3） | [ws291-p024](ws291/phase024/phase.md) RTX の描画の最適化を目標まで | 2026-10-31 ユーザーの指示。subs-1000 の 1 run の固定の費用 → big-1000 の残り → zero copy の big-100 の遅れ。目標は NVK 並み（big-1000 136.2・subs-1000 75.4 fps）。HAL は変えない | in-progress・返却（2026-10-31 23:41、main 7055ea569。subs-1000 20 → 34.5、subs-100 182 → 300、big-1000 は揺れの内。submit の MMIO・texture の語の cache・job-submit A+B・BUG-381・monitor の wake。目標には未達。i02 へ） |
| q1786 / q1786-i01 | P7（phase-runner-high、worktree `/home/awe/zedBSD-worktrees/p7-bug379`・branch `agent/p7-bug379`、QEMU、Pi 4 は P4 と調整） | [BUG-379](bugs/BUG-379.md) fork の livelock | A（src/kern/vm の fork）・B（libc の aarch64 の vfork の posix_spawn）・C（libvulkan の present_worker_get）。HAL は変えない | cleared（2026-10-31 23:23、7300658b2 main。A・B・C、QEMU の amd64・raspi4b・riscv64・i386・armv7 で forkrace PASS、Pi 4 の session で描く（P4）。BUG-379 closed） |
| q1727 / q1727-i11 | P8（phase-runner-low、同じ worktree p8-ws074） | [ws074](ws074/ws.md) phase260 の自走の 11 回目 | 100% までの自走（ユーザーの指示）。transitions・animation の補間 → multicol → dynamic import() など。SVG のベクトルの拡大は WS328 の範囲 | in-progress・返却（2026-11-01 00:16、2,975 → 3,005（86.55%）、main fd4449c07、Acid3 100/100・Acid2 100%、後退 0。i12 へ） |
| q1785 / q1785-i02 | P3（phase-runner-high、同じ worktree p3-ws291wsi、10.0.10.3） | [ws291-p024](ws291/phase024/phase.md) RTX の最適化の続き | 目標まで（ユーザーの指示）。submit の kernel の frame・monitor の wake・libvulkan の submit の時間・big-1000 の record・lending の big-100。fence の無い submit の job は変えない（ユーザーに聞いている） | in-progress |
| q1784 / q1784-i03 | P4（phase-runner-high、同じ worktree p4-ws291pi、Pi 4） | [ws291-p025](ws291/phase025/phase.md) の続き | desktop 60 fps と c21 50 fps。Q1 の判断: Terminal の文字の atlas を device-local・staging に（userland/desktop/terminal を範囲に足す、Venus でも確かめ）、c21 は 40.5 で受け入れず bin と render の重ね・CPU の削減を続ける | in-progress |
| q1727 / q1727-i12 | P8（phase-runner-low、同じ worktree p8-ws074） | [ws074](ws074/ws.md) phase260 の自走の 12 回目 | 100% までの自走（ユーザーの指示）。host-css-rule-model の確かめ → subgrid の残り・multicol・grid の baseline など。SVG のベクトルの拡大は WS328 の範囲 | in-progress |
| q1787 / q1787-i01 | P7（kernel-and-driver-designer、worktree `/home/awe/zedBSD-worktrees/p7-ws334`・branch `agent/p7-ws334`） | [ws334-p001](ws334/phase001/phase.md) 設計 | plan/ws334/ だけ、design-reviewer と問いまで。libbrowser の file は P8（WS074）と共有なので、変える file の一覧を設計に書く | in-progress |
