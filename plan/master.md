<!-- awesome-plan project=zedbsd record=master -->

# zedBSD Master

[AGENTS.md](../AGENTS.md)（規則）· [Queue](queue.md) · [担当の台帳](agents/registry.md) · [運用](agents/protocol.md) · [完了した WS](workstreams-closed.md) · [Guardrail](guardrail.md) · [設計方針と全体の決定](master-design-policy.md) · [道具と実機](tools.md) · [Bug Board](known-bugs.md) · [Future Work](future-work.md) · [Past Log](history/index.md) · [設定](config.md) · [plan の地図](README.md)

<!--
  「今の状態」の各 block は「master:<名前>:start」〜「master:<名前>:end」で丸ごと置き換えてよい（今の事だけを書く。終わった事・経緯は書かない）。
  置き換え: sed -i '/^<!-- master:agents:start/,/^<!-- master:agents:end/{//!d}' plan/master.md の後に sed -i '/^<!-- master:agents:start/r new.md' plan/master.md。
  ユーザーの決定の日付の log は置かない（AGENTS.md §8）。
-->

## 今の状態

### 動いている担当と Queue

**Cockpit の運用（2026-10-30 ユーザー）**: [cockpit.md](cockpit.md) を Q1 が保つ。ユーザーの指示（原文）「plan/cockpit.md を作成します。P1-P8がそれぞれ今何をしているかを先頭に表で書きます。表には、取り組んでいるWS、その説明、作業予定が書かれています。その下、取り組んでいるWSごとの表があり、WSの説明とphaseの一覧、今の進捗が書かれます。進捗はセッションをまたぐので、クリアが指示されるまでは保持されます。長くなったらユーザに確認してクリアするか切り詰めてOKです。その下に、ユーザに判断してほしい事項が表にまとめられます。ユーザはこのファイルを監視して、タイムラインを遡らなくても状況を把握できるようにします。タイムラインへの出力も残し、これはユーザがスマートフォンで確認するときに使われます。」「この仕組みをmaster.mdに記録して、運用開始してください。」
→ 解釈（Q1）: (1) 担当の起動・返却・Phase の cleared・merge・ユーザーの判断の発生と解決のたびに cockpit.md を直し、同じ時に commit する（強制終了に備える）。(2) WS ごとの進捗はセッションをまたいで残し、消すのはユーザーの指示か確認の後。(3) タイムライン（チャット）への報告も今までどおり続ける。(4) cockpit.md は写し: 状態の本体は各 ws.md・queue.md・registry.md で、食い違えば本体に合わせる。
**判断を待つ事の書き方（2026-10-30 ユーザー）**: ユーザー（原文）「cockpit.mdのユーザの判断事項は、ユーザが判断できるように、背景から説明して、提案があるなら丁寧に説明して、専門用語があれば説明するように、master.mdを改善してください。つまり、cockpit.mdだけを見て、判断できるようになりたいです。…V2がどんなものが書いてないし、判断1～4も書かれていないです。…別のGPUというのがどういうシチュエーションなのかの背景が描かれておらず、companionというここでの用語の意味が示されていなくて、やはり理解できないです。…背景（WS074はCodexが実施している）が書かれていないと、思い出す労力や認知負荷が高いです。」
→ cockpit.md の「ユーザーの判断を待つ事」は、一覧の表（番号・WS の番号と短い説明・一行の問い）の下に、項目ごとに次を全部書く（他の file を開かずに判断できること）: (1) 背景（どの WS か、何を作っていて、なぜ今この問いが出たか、関係する担当・セッション）、(2) 問いそのもの（選択肢が複数なら全部）、(3) 各選択肢の中身と得失（何が変わるか、費用、危険、後戻りのしやすさ）、(4) Q1 の推しと理由、(5) 用語の説明（その項目で使う専門語・このプロジェクトだけの語・略語の全部）、(6) 決まらない間に起きる事。参照の link は補いにとどめ、link 先を読まないと分からない書き方をしない。

<!-- master:agents:start -->
**2026-10-30 引き継ぎ（週間の利用の上限でセッションが終わる。次のセッションの Q1 はここから自動で再開する）**: ユーザー「P3,P5,P4をラップアップして、進捗を報告してください。次のセッションで自動再開できるように、ハンドオーバーしてください。」
再開の手順: (1) 下の表の各担当の branch の最後の SHA を `git cat-file -t` で確かめ、未 merge の物を registry で見る。(2) ラップアップした担当（P3・P4・P5）は同じ名前・同じ worktree で起動し、ws.md の Resume point から続けさせる。(3) P1・P2 と Sonnet の p007 は、終わっていなければ同じ worktree で再起動（未 commit の差分は担当が読んで続ける）。(4) /tmp/zedbsd-rdna.lock の中身で 10.0.10.3 の状態を確かめる。

| 担当 | Queue | WS と次の手 | 実機 |
| --- | --- | --- | --- |
| P1 | q1705-i01 返却（12:3x、ラップアップ） | [ws009](ws009/ws.md): p025〜p027 cleared、p028 は DOC-T60 済み・build/boot の再現の途中（main 6bfc2c1e3）。次の 1 手は phase028/phase.md（`P028_BOOT_ONLY=… p028-reproduce.sh native` → hybrid uefi bios → 完了の形と移す物・消す物）。その後 WS296（P6 の 1809873bd を元に組み直す案は phase028.md の末尾）→ WS297。Q1 の残り: p1-ws009r の build/tmp/p028-20261030-110805（4.5 GB）・build/tmp/p028-run.out の削除 | — |
| P2 | q1703-i01 返却（ラップアップ） | [ws327](ws327/ws.md): p016 in-progress（main 52ce25740）。済み: 境界・build・host 試験・FreeBSD・閉じた WS の試験・Win32 の一覧・network_finish。残り（phase016/phase.md の引き継ぎ）: image を作り直して音の device 無しで QEMU の回帰、Video Player の PAUSE と Photos の import の FAIL の解析、未到達の回帰、BUG-367 の後に 08-network の流し直しと 08-network.png → p017 | QEMU |
| P3 | q1706-i01 in-progress | [BUG-367](bugs/BUG-367.md) TCP の close が戻らない件（WS315 の後の WS221・WS299 はその後） | QEMU |
| P4 | q1698-i01 返却（09:13） | [ws307](ws307/ws.md)（main fbdd143a8 に merge、branch agent/p4-ws307o 520360247）: p002 cleared（upload）。p015 sleep Never 実装・host 試験 PASS、QEMU は host の負荷で 2 回未達（load の低い時に 3 回目）。p004 pkgconf と各 package の開発の file、clang・libcxx は build 済み、installer-image を 1 本だけ build → p004-gtk.sh（並行の make は不可）。p008 は p004-gtk.sh の j3 で確かめ直し。p014 は p007 の C を後で。p007 は driver の設計の下書き（改訂 3、SMEP・SMAP の下の 1:1 の map を HAL を変えずに作れるか）。Q1 の残り: proposed/hal-uefi-variables.md・hal-fw-variables.md の削除（link を外してから）。CI の別件 2 つは phase002 | QEMU |
| P5 | q1700-i01 返却（09:13、計測は未実施） | [ws291](ws291/ws.md) NVIDIA: kernel の build だけ（vmunix-m0）、引き継ぎ c59e5a60e（main 12a2cc467）。次の 1 手は [phase010](ws291/phase010/phase.md) の「q1700-i01」節（main の userland を NVMe に入れ、rtx-desk-profile.sh を 2 回、p012 を立てる） | 10.0.10.3 は free |
| Sonnet | — | [ws326](ws326/ws.md): cleared 20 Phase（p007 参照の list も main 5d43ad00d）。次: p015（補間と重み）・p017（B の direct）を Sonnet で並べて起動 → p019 → p021 の前に reflist の header の直し（[phase021](ws326/phase021/phase.md) の p007 の引き継ぎ）→ p021（Opus）→ p022 → p026・p023〜p025 | — |

予定（ユーザーの決定、各 ws.md）: WS326 の後に WS318（すぐ）と WS329（H.264 encoder）、WS329 の後に WS330（H.265）。WS315 の後に WS221 と WS299。WS328 は WS074 と調整してから p001。
**次のセッションの始めにする事（2026-10-30 ユーザー）**:
1. [BUG-363](bugs/BUG-363.md)（image に /etc/hosts が無い）に取り組む（ユーザー「BUG-363は、次回のセッション開始時に取り組むようにスケジュールしておいてください。」）。
2. （済み）WS315 p006 の同時の send の退行は解決し WS315 は completed（2026-10-30 834d10d88）。
3. [WS250](ws250/ws.md) の QEMU の起動の回帰を流して completed に（ユーザー「WS250は流してよいです。」。fault の app・isolate の道は Future Work へ）。
4. （済み 2026-10-30: WS309 p015 の判断 1〜4 はユーザーが決めた。p015 の実装を担当に割り当てる）旧: ユーザーに [WS309 p015](ws309/phase015/phase.md) の「要る判断」1〜4 を聞く（ユーザー「WS309の判断は、次回セッション開始時に聞いてくれるように記録してください。」。聞く前に AGENTS.md §3 の前置き: WS の番号と説明・状態・決まっていない事・用語の説明）。
5. 上の表の担当（P1・P2・P4・P5）と WS326 の Sonnet（p015・p017）を再開。

止めたまま: [WS305](ws305/ws.md) VFIO、[WS304](ws304/ws.md) Pi 5 の NVMe、[WS290](ws290/ws.md) GHCI（保留）、[WS299](ws299/ws.md)（継続の判断待ち）。
main に未 merge の branch は [registry](agents/registry.md) の試験待ちの欄。web の GET は「統合の注意」の条件で可。HAL の差分の承認は無い。
**終わりの条件と機械の割当（2026-10-29 23:2x ユーザー）**: 「P6の結果はマージしてOKです。別なツリーのセッションで再開したときに試験から継続できるように、P6からハンドオーバーしてください。」「P5は、NVDECが実機video appで動いて、WS completedになったら、P5は終了して返却してください。」「P4は、先にUFSが遅いバグを修正してください。…」（q1688）「P3は、お願いしてあるネットワークの改善が終わったら、終了して返却してください。」「P2は、WS068が完了したら、終了して返却してください。」「P1は、WS309が完了したら、終了して返却してください。」「P3で、2.5GのUSBが遅いことは、ドライバの問題と思いますので、5330を使って修正してほしいです。」（q1689）「GPUドライバの改善は、ゴールがまだ見えないので、先にP3のネットワークを優先して、デスクトップ機に割り当てて、P3の作業を完了させるようにスケジューリングしてください。そのあとに、P5のNVDECを完了させます。そのあと、デスクトップ機では、RTXドライバの最適化に進んでください。エージェントは再利用しても新規で立ててもOKです。」「VF2を他のエージェントが使わなくなるのを待って、実機でP7のODROID-C5のテストを行います。」「centrisのNICの設定は、変更してOKです。」
- 解釈（Q1）: 10.0.10.3 は P3 の WS221 p010 が終わるまで P3 が専有、次に P5（WS243 p006 の実機）、その後 RTX の driver の最適化（WS291 の NVIDIA の段）。VF2 は P3 が手放した後に C5 の試験へ。
- WS068（2026-10-29 ユーザー「WS068ですが、i915の試験を受け入れ条件から外して、VenusのみでOKとします。また、残りが大きい件は了解しましたので、phaseに分解して、目標を管理しながら、空いた時間に細々と取り組んで、WS完了に近づいていけるように、準備してください。」）: 受け入れは Venus のみ。P2 が残りを Phase に分けて返却し、以後は手の空いた担当が Phase ごとに進める（前の「P2 は WS068 の完了で返却」を置き換える、Q1 の解釈）。
- WS221（2026-10-29 ユーザー「P3は、SACKの実装を優先してください。ATRを有効にした評価で進めます。」）: centris の ixgbe の ATR は有効に戻し、評価は ATR 有効で。P3 は zedBSD の TCP の SACK を先に実装し、その後 q1689（5330 の NCM）。
- WS221 p014・WS315（2026-10-30 ユーザー「xHCIの改修は行いましょう。たとえ今回効果がなくても、価値がある実装だと考えます。Realtek方式については、NCMでどこまで速度が上がるか観察してから、ぜひ実装しましょう。…」原文は ws315/ws.md）: xHCI の 1 endpoint に複数の要求は効果が無くても入れる。その後 Realtek の vendor mode の driver（WS315）。
<!-- master:agents:end -->

**ユーザー不在の間の自律実行（2026-10-29 12:4x ユーザー「夜まで寝ます。しばらく質問に回答できないので、疑問で止まったphaseがあれば、質問はメッセージ表示だけしてクリック回答にはせず、別なphaseに移って作業を進めてください。」）**: 判断が要る Phase は uncleared・止めにして理由と要る判断を記録し、Q1 は問いをメッセージで書くだけ（クリックの問いにしない）。担当は別の Phase に移る。

**手が空いた担当の進め方（2026-10-29 ユーザー「作業の手が空いてしまったら、テストなしで実装だけ進める戦略で、残っているWSのphaseをまんべんなく進めましょう。」）**: 手の空いた担当は、残っている WS の Phase を試験なしで実装だけ進める（build warning 0 と host の試験まで、実機・QEMU は無し）。Phase は「実装済み・試験未実施」と記録し cleared にしない。branch は試験の後に merge する（P6 の 2026-10-29 の運用と同じ、Q1 の解釈）。

### 統合の注意

<!-- master:merge:start -->
- WS074（2026-10-30 ユーザー「Codexは解約しました。WS074はClaudeの管理下に移ります。」）: Codex のセッションは終わった。WS074 は Q1 が管理し、Claude の担当（P1〜P8・B1・Sonnet）に割り当ててよい。main の checkout の WS074 の未 commit の変更は無い（2026-10-30 git status で確かめた）。
- H.265（2026-10-30 ユーザー「H.265のパテントプールへの支払いは行う方針です。H.265の実装を行っていいです。」）: HEVC の decode・encode を実装してよい（WS236 の「HEVCはライセンスがないのでやらないです」を置き換え）。
- 5330 への SSH（2026-10-30 ユーザー「5330へのSSHは許可します。」）: 権限の判定に止められた 5330（ssh の別名 solaris10-man、10.0.30.3）への SSH を許可。担当は試験の前に uname で 5330 の Debian か zedBSD かを確かめる。身元の分からない host には login しない。
- firmware の license（2026-10-30 ユーザー「試験に関しては私はすべてのライセンスに同意しておりますので、自動でいいです」）: 試験の image の build で firmware の license の確認が出たら、担当は `yes | make firmware-licenses`（自分の worktree）で同意して続けてよい。
- web の取得（2026-10-29）: 一度禁止（Fable の配下が bot の確認の回避で権限の判定に 3 回止められた件）、その後ユーザー「Webから取得してOKです。サブエージェントにWebの取得(GET)を許可して下さい。GETはパラメータなしなら安全と見なしていいです。」→ 担当と配下に web の GET を許す。条件: WebFetch・WebSearch の tool と、パラメータの無い GET の取得（BSD の source の tarball など）。bot の確認・拒否に当たったら即やめて Q1 に報告、User-Agent の変更・別の取り方の再試行はしない。
- サブエージェントの起動は番号の小さい担当を優先（2026-10-29 ユーザー「P1,P2,..というふうに、数字の小さなエージェントに優先してサブエージェントを起動させてください。必要なら優先度の低いものを停止してください。」）。上限に当たったら Q1 が番号の大きい担当の配下を止める。今の枠: P5 の配下は同時 2 本、P6 は 1 本、P7 は 2 本（2026-10-29 ユーザー「WS313は調査、コード作成などで、2つまでサブエージェントを作って並列化し、効率化してください。」）。
- 全担当の成果は main に merge 済み（上の Queue の作業中の物を除く）。`agent/p2-ws288-p006` は WS288 の終了で使わない。
- 既定の config: Pi の BT は config-rpi4.mk・config-rpi5.mk で n（ユーザー）、Pi 5 の GPU（CONFIG_DRIVER_BCM2712_GPU）は CI で n、
  fan（CONFIG_DRIVER_RPI5_FAN）は y、CI の VF2 は CONFIG_POWERVR_DEBUG n。
- firmware は全部 zedBSD-firmware（rev fcdfccf、i915 の DMC 6 file を含む、push 済み）から取る。
<!-- master:merge:end -->

### Focus と優先順位

<!-- master:focus:start -->
- 公開は rev.5 まで（実質のベータ2）。次に何をやるか（以前の「ベータ3・ベータ4」の段は取り消し）は Q1 とユーザーが整理中。下の「保留の WS」はその整理の対象。
- **Keiland の thread の整理**（[WS309](ws309/ws.md)、incomplete）: 設計は確定（design.md、J1 は V2 swapchain 共有）、p001〜p010・p019 cleared（2026-10-29）、P1 が p011 から実装中。構成の表とユーザーの決定の原文は ws309/ws.md（手書きの照合の pool の N は 2026-10-29 ユーザーの回答「CPU 数で固定 (推し)」で決定済み）。ゲームモードは Vulkan display（VK_KHR_display）の仕様の中で実装する（ユーザー「工数が多くてもかまいません。」）。
- **GPU の描画の速さ**（[WS291](ws291/ws.md)）: RDNA は重い scene で Linux と同等（scene-1500 201.8 対 200.4 fps、scene-6500 49.2 対 50.1）、小さな submit の多い run（big・subs）は Linux の 1/3〜1/10。次は案 4・5（ユーザーの判断）、その後 NVIDIA → PowerVR、Linux 以上の後に WS306。
- **5330 の電源と性能**（[WS299](ws299/ws.md)）: direct の書き 88%、UFS の file の書き 52%、CPU の速さは Debian の 70〜90%（idle が C1 だけ・HWP 無し・hybrid を知らない、別の WS が要る）。継続はユーザーの判断。その後は WS300 → WS052 → WS301 → WS302 → 熱と fan（[pm-plan.md](agents/P1/pm-plan.md)）。
- **GHCI 1.0 の driver**（[WS290](ws290/ws.md)）: 保留（FPGA 側の設計の完了と、ユーザーの明示の再開の指示を待つ）。p017 は実装・host 試験・QEMU 済み、残りは実機（p014・M9）と p015。
- **インストーラ**（[WS307](ws307/ws.md)）: 仕様は確定（spec.md）、実装 p002〜p014 はユーザーの指示の後。
- 達成: fg021 ZYBO（2026-10-23）、fg020 VisionFive 2（2026-10-22）、fg019 ベータ2（2026-10-17 OSC）、fg018 Linux 標準 GTK4（WS114）、fg010 OSC のデモ、fg012〜fg016。Pi 5（WS278・WS279）も completed。取り下げ: fg022 KC705 の GPU の board（WS288 の終了）。
- 実機の使い分け（2026-10-27 10:49 ユーザー）: デスクトップは GPU の driver の開発（今後は Linux＋VFIO の passthrough）、Keiland の app は VM で開発して VF2 で速さを詰める、5330 は Intel GPU・電源管理・Alder Lake。
<!-- master:focus:end -->

<!-- master:priority:start -->
優先順位（依存による順とは別。Queue の権限は変えない）:
1. WS309（Keiland の thread の整理）: P1（q1662-i02）が p005 から実装。
2. WS291: P2（q1663）が案 5 → 案 4。並べて WS221（P3）・WS307（P4）・設計の全般（P5）・実装だけ（P6）。
3. WS299: ユーザーの継続の判断の後（UFS の書き・mkfs の write-through・p007・p008、または区切る）。
4. すぐ取り組む物（2026-10-27 ユーザー）: WS221（Jumbo frame、p008・p009 は WS220 から移した物）、WS242（RTX 30/40）・WS243（NVDEC）・WS250（nvrtx の取り消しと復帰）、WS223（全画面は合成、推しで実装）、WS009 の DOC-54（/dev/gpu の UAPI の文書）。
5. WS307 の実装、WS217（画面の回転、開き直した）: ユーザーの指示の時。
6. WS290・WS292・WS226: WS290 の再開の後。WS295・WS296・WS068: ユーザーの指示の時。保留の WS（WS297 を含む）: Q1 とユーザーの整理の後。
<!-- master:priority:end -->

### ユーザーの未決

<!-- master:open-decisions:start -->
- WS299 の継続（P1 の report: ws299/ws.md の「結果（2026-10-28 21:50）」）。担当の外で見つけた物の扱い: amd64 の idle（C1 だけ）・HWP・P/E core の置き方（CPU が Debian の 70〜90%）、clock_gettime に vDSO が無く 1 ms 刻み、main から return した時に stdout が flush されない、libc の memcpy の続き（大きな copy・memset・build の flag -fno-builtin -Os）。
- kernel の PS/2 の aux: mouse が無いのに node を作り、open のたびに 約 80 ms 待って EIO（src/kern/platform/pcat.c の drv_pcat_ps2_8042_init の辺り）。Bug にするか、いつ直すか。
- P4（q1660）が拾った候補: WS162 を canceled にするか（WS172 に吸収）、WS279 p009（ライセンスと類似性の監査）の担当、P4m の時々の失敗（T1-691）の Bug、nvrtx.disp.own の既定、pci-msi-qemu.c（CONFIG_KERNEL_TEST_CHECKPOINTS の唯一の config が消えた）を残すか、master-design-policy.md へ移す候補 約 25 件（各 WS の ws.md に原文）。retained の 41 WS は source の comment の掃除の Queue の後に片付ける。
- 名前の変更（2026-10-28 ユーザー「…KeiというOS名を削除して、zedBSDに統一します。インストーラだけでなくすべてに影響します。…」、WS307 の Phase）: Keiland・libkeiland は残す、OS 名は zedBSD に仮決め。
- 保留の WS の整理（以前のベータ3・4 の中身を含む）。Q1 とユーザー。
- WS304: Raspberry Pi OS で同じ HAT・FFC・NVMe の link を見るか（ハードの切り分け）。
- WS306: ゲーム最適化の環境変数の名前と書式、知らない option の扱い（p001 の設計で推しを示す）。
- WS282〜WS285: 実装を始める時期（ユーザー「設計だけ残してラップアップして、後日実装することにします。」）。再開の時に WS282 の試験の画面を出す PC（D11）と買う webcam（D12）。
- WS286 の後の物: openXC7 の host への導入。LLVM の cache の asset を GitHub の release に upload してよいか（toolchain の変更の時に確かめる）。
- BUG-345（Pi 5 の click が画面に出ない窓）: 再発したら再起動の前に Q1 に知らせてもらう。
- 以前からの物: UEFI の loader の GOP の mode の選び方、BUG-222（ue0 の SCP が約 10 Mbps、tracking）、WS153 U2〜U15（ユーザーが検討中、聞かない）。
<!-- master:open-decisions:end -->

### 止まっている物

<!-- master:blocked:start -->
- WS290 の実機の試験: VF2＋KC705（GHCI の bitstream はユーザーが書く）、VF2 の電源は SwitchBot のプラグミニ 2（14C19FA80F86）。WS290 自体が保留。
- ユーザーが拾える試験（頼まない）: 5330 の USB 2.5G（RTL8156）を vendor mode の driver（WS315）で使っている間に、adapter の抜き差しと、switch 側の cable の抜き差し（link の上げ下げ）をして、ue0 が戻って ping が通るか（P3 は遠隔でできない）、RDNA の desktop で画面の途切れが無くなったか目で見る（WS291 b574fbf6a・7650291df、機械は今 q1661 の /bin/wayland）、5330 の物理の keyboard・mouse。
- `plan/records.json`（GitHub の同期の対応）に消した Phase の path が 118 か所残る（同期は今使っていない）。
- VF2 の zedbsd.cfg は login=graphical を外したまま（元は zedbsd.cfg.p044orig、戻さない）。
- GitHub への公開は push の指示の時だけ。
<!-- master:blocked:end -->

### 次の番号

<!-- master:next:start -->
BUG-369 · q1707 · WS331 · KL_VERSION 105。使う前に衝突を確かめる（`ls plan/bugs`・`ls -d plan/ws*`・`grep q16 plan/queue.md`）。
<!-- master:next:end -->

## 目的

- **目的**: 寛容なライセンスで企業が自由に使える UNIX 互換 OS を、GPL の Linux kernel に依存せずに作る。
- **利用者**: OS を組み込んで独自のディストリビューションを作る開発者・企業と、デスクトップ・ラップトップ・SBC で使う個人。
- **最終成果**: Linux/Android を置き換えられる水準の kernel と userland、最小の HAL による移植契約、用途別に構成・配布できる仕組み。
- **範囲**: kernel、HAL、driver、libc、base の userland、Keiland（desktop）、外部 package のクロスビルド、インストーラ、文書。
- **範囲外**: Linux の kernel ABI・DRM の互換、Mesa 流の user mode driver、正式な UNIX 認証・Vulkan CTS 認証の取得（主張しない）。
- **制約**: HAL の API の変更は差分ごとの事前承認（[Guardrail](guardrail.md)）。独立実装とライセンスの境界（[設計方針](master-design-policy.md)）。

### Objectives

- **O1**: 寛容なライセンスで企業が自由に使いやすい UNIX 互換システムを、GPL の Linux kernel に依存せず、Linux/Android を置き換え可能な水準で提供する。
- **O2**: デスクトップ、ラップトップ、SBC、タブレット、モバイルなど様々な規模で動く kernel と userland を提供し、開発者が独自ディストリビューションを自由にカスタマイズ・リブランディング・配布できるようにする。
- **O3**: UNIX/BSD/Linux の遺産から現代のシステムに必要なエッセンスを抽出し、networkd、netconf、service などをシンプルで一貫した仕組みとして再実装する。
- **O4**: ページベース MMU を備える 32bit/64bit コンピュータへ UNIX 互換 OS を確実に移植できる、明確で最小限の HAL を定義し、人類の共有知とする。
- **O5**: AI 時代の OSS のあり方を、大規模な AI 活用開発を通じて探索し、成果・失敗・人間の判断を再利用可能な知見として共有する。

### Milestone

達成は所属の WS の完了の数ではなく、到達点の証拠で判定する。completed の Milestone はまだ無い。WS の所属は下の表と [完了した WS](workstreams-closed.md) の Primary の列。

| Milestone | Objective | 受け入れの核 |
| --- | --- | --- |
| **MG001** 継続開発できる基盤 | O4, O5 | 文書化した環境で build でき、設計境界・規約・試験・制限を追跡できる |
| **MG002** UNIX アプリケーションの実行基盤 | O1 | process・memory・libc・loader/TLS の対応範囲を互換性台帳と代表アプリで確認できる |
| **MG003** 対象機へ導入して単独起動 | O2, O4 | 合意した機種・媒体でインストール後の単独起動と login を確認できる。実機と QEMU の証拠を分ける |
| **MG004** データの保持とメモリ/ストレージの実用 | O1, O2 | 永続化、低メモリ時の進行、媒体世代、既定構成の性能を確認できる |
| **MG005** 一貫したネットワーク/サービス管理 | O1, O2, O3 | networkd・netconf・service の責務・設定・操作が一貫し、永続化と失敗後の復旧を確認できる |
| **MG006** グラフィカルな操作環境 | O2 | 入力・描画・ウィンドウ・端末・GUI ツールの一連の操作を確認できる |
| **MG007** 用途別の独自ディストリビューション | O1, O2 | 第三者が用途別に構成し、独自ブランドで build・配布できる |
| **MG008** 最小 HAL の移植契約と異種機での実証 | O4 | HAL 契約・移植手順と異種/レトロ機での実証を公開する |
| **MG009** AI 活用 OSS 開発の知見の公開 | O5 | 設計権限・レビュー・変更追跡・失敗からの回復の事例と根拠を公開する |

## 未完の WS

状態の本体は各 ws.md、ここは写し。完了・取り下げは [workstreams-closed.md](workstreams-closed.md) へ移す。2026-10-27 に P3（q1608）が精査した（[ws-audit.md](agents/P3/ws-audit.md)）。

### 進めている・次の WS

| WS | Primary | 内容 | 状態 | 再開点 |
| --- | --- | --- | --- | --- |
| [WS299](ws299/ws.md) | MG004 | 5330 の NVMe の読み書きを速くする（512 MiB の崖、転送・queue・readahead）（2026-10-27 ユーザー） | incomplete | p001〜p003・p009 cleared（raw の読み 583 MB/s ≒ Debian の 50%）。受け入れは Debian の 95% 以上、HMB はやる（ユーザー）。q1634（P1）で p004 raw の先読み → p005〜p008 2026-10-30 ユーザー「WS299はRealtek 2.5Gのあとにスケジュールします。」→ WS315 の後 |
| [WS300](ws300/ws.md) | MG003 | NVMe と VMD の低電力（S0 idle の VMD の suspend・NVMe の状態、APST・ASPM・LTR）（2026-10-27 ユーザー） | incomplete（p001 cleared、p002〜p005 実装済み・試験未実施、branch agent/p6-ws300 は 5330 の試験の後に merge） | p006（5330、WS052 p014 と一緒）。Q300-6（_OSC 無しの ASPM）はユーザーの判断待ち |
| [WS301](ws301/ws.md) | MG004 | CPU の DVFS と P/E core の相対性能の枠組み（UAPI は /dev/thermal、HAL は変えない、HWP の無い CPU も P-state を driver で）（2026-10-27・29 ユーザー） | planning | 設計 第 3 版の改訂 2（P5）、review の minor の反映と再 review、約 25 LW |
| [WS302](ws302/ws.md) | MG006 | デスクトップスレッドバランサ DTB（CONFIG_DTB、sched への hint）（2026-10-27 ユーザー） | planning | p001 検討（WS301 p001 の後）、約 32 LW |
| [WS303](ws303/ws.md) | MG004 | 熱と fan（/dev/thermal・/sbin/thermal、strategy performance・balanced・powersave・manual、fan は上げるだけ、非常の停止）（2026-10-27・29 ユーザー） | planning | 設計 第 5 版（P5）、design-reviewer の後に Q303-15〜19、約 35 LW、passive は WS301 に依存 |
| [WS304](ws304/ws.md) | MG003 | Raspberry Pi 5 の外の PCIe（pcie1、M.2）と NVMe（2026-10-27 ユーザー、実機の試験はまだ） | incomplete | p001・p002 cleared（e616b4128、host 試験・build のみ）、p003 の実機は新しい SwitchBot の後、HAT の型番をユーザーに確認 |
| [WS305](ws305/ws.md) | MG006 | デスクトップの GPU の passthrough の試験環境（Linux＋VFIO＋QEMU、素の起動と使い分け）（2026-10-27 ユーザー） | incomplete（再開待ち） | P4 はラップアップ（2026-10-27）。script と guest の image は用意済み、host（10.0.10.3）では未着手。再開は ws.md の「状態と再開点」 |
| [WS306](ws306/ws.md) | MG006 | GPU の driver の枠組みの「ゲーム最適化」の UAPI（ON/OFF と option の文字列、GPU の名前と driver の版の取得）（2026-10-27 ユーザー） | planning | p001 の設計（使用量が戻ってから） |
| [WS052](ws052/ws.md) | MG003 | 電源管理（S0ix を 5330 で初めて動かす、modern standby、/dev/system） | incomplete | 再開点 p014（WS300 p002 の後）、並べて p015・p016、約 31 LW |
| [WS009](ws009/ws.md) | MG001 | 文書 | incomplete（DOC-54 を今書く、2026-10-27 ユーザーの回答「今書く」） | DOC-54（/dev/gpu の UAPI の文書） |
| [WS068](ws068/ws.md) | MG006 | EGL と OpenGL ES（と desktop GL 3.0〜4.6）を Vulkan と display 拡張の上に実装する（Wayland とディスプレイ直接の両方）（2026-09-26・27 ユーザー指示） | incomplete・継続（空き時間だけ。受け入れは Venus のみ、2026-10-29 ユーザー） | 次は p045（sample shading、Venus の試験だけ）→ p047〜p068（GL 4.x を分けた物）→ p007 規約 |
| [WS074](ws074/ws.md) | MG006 | zedBSD の Web ブラウザ `userland/base/zdesktop-browser`（HTML5 の layout engine → 最適化にこだわらない JavaScript engine の接続 → CSS の準拠と Chrome との比較で目標値を段階的に上げる。JS と Wasm の実行 engine を共通化。画像は libpng-compat・新しい libjpeg-compat、TLS は当面 OpenSSL）（2026-09-27 ユーザー指示） | incomplete・継続（Codex のセッション） | WPT Interop 2025 の 100%（ws074-q20261019-55 active） |
| [WS217](ws217/ws.md) | MG006 | 画面の回転（Settings の Display、縦長で Notes を使う）（2026-10-13 ユーザーの UAT） | incomplete（2026-10-28 ユーザー「WS217 を開き直す」） | p009-1（Intel の PRM）・p009〜p011（scanout の 180°）・p005（複数 head）・p006（pen・touch と display の結び付け）・p012（game の session）→ p007 確かめ・p008 規約。p002a・p002b・p003・p004 は済み |
| [WS221](ws221/ws.md) | MG005 | Jumbo frame（Ethernet の L2 の大きな MTU、device ごとの上限、IP・TCP の MSS）（2026-10-13 ユーザー） | incomplete（すぐ取り組む物、2026-10-27 ユーザー「Jumbo Frameはすぐやるものの一つにします。」） | p001 の判断から（p008・p009 は WS220 の旧 p006 tcp_device_mss・p007 を移した物、2026-10-28 ユーザー「WS221 へ移す (推し)」） 2026-10-30 ユーザー「WS221はRealtek 2.5Gのあとにスケジュールします。」→ WS315 の後 |
| [WS223](ws223/ws.md) | MG006 | 全画面は compositor の合成、game mode は Wayland の拡張で求められた時だけ（Video Player は game mode をやめる）（2026-10-13 ユーザー） | planning（すぐ進める、2026-10-27 ユーザーの回答「推しで決めて実装」、D1〜D6 は推し） | p001 の D1〜D6 を推しで確定 → p002 |
| [WS226](ws226/ws.md) | MG006 | Khronos の Vulkan 1.3 の header をそのまま使い、libvulkan で Vulkan 1.3 の API を実装しきる（不整合の整理）（2026-10-13 ユーザー） | planning（WS290 の後、全 GPU driver で Vulkan 1.3 相当を capability の flag とともに、2026-10-27 ユーザー） | WS290 の後に p001 |
| [WS242](ws242/ws.md) | MG006 | nvrtx の RTX 20/30/40 の全モデル（2026-10-15 ユーザー） | incomplete（すぐ取り組む物、2026-10-27 ユーザー「すぐ取り組むものの一つです。」） | p001 の再開点 1〜4、GTX 16・RTX 30・40 の既定の有効（ユーザーの決定、未実装） |
| [WS250](ws250/ws.md) | MG006 | nvrtx の終わらない GPU の仕事の取り消しと復帰、system は動き続ける（2026-10-15 ユーザー） | incomplete（すぐ取り組む物、2026-10-27 ユーザー「すぐ取り組むものの一つです。」） | p001 の設計（草稿は main 64f05b2c7） |
| [WS290](ws290/ws.md) | MG006 | zedBSD の GHCI の driver を GHCI 1.0（amaterrace-gpu の docs/ghci.md 89529de）だけに書き直す。試験は host だけ | incomplete・保留（2026-10-28 ユーザー: FPGA 側の設計が終わるまで P3 終了、再開は明示の指示。p017 は実機待ち） | q1636（P3）で p001 の design 版 4 → p002〜（host 試験、実機は VF2＋KC705 の用意の後） |
| [WS291](ws291/ws.md) | MG006 | GPU の driver の描画の速さ（desktop 60 fps・重い 3D の model 50 fps）: AMD RDNA（P2）→ NVIDIA（P2、GPU を入れ替え）→ PowerVR（VF2、P3） | incomplete（p001〜p005 cleared、p006 in-progress） | q1635（P2）で p006 の続き → p008〜p009、Linux 以上の後に WS306 |
| [WS309](ws309/ws.md) | MG006 | Keiland の compositor の thread の構成を整理し main loop が待たないようにする。ゲームモードは Vulkan display の仕様の中で | incomplete（p001〜p007・p019 cleared、P1 q1662-i05） | p008 の残り（fence 付きの task）から |
| [WS310](ws310/ws.md) | MG004 | ACPI の _OSC を呼び、システム全体の挙動を厳重に確かめる（WS301 の Q301-3、2026-10-29 ユーザー「最終的には呼ぶが、別なWSにして…今は呼ばない。」） | planning | p001（WS301 の後） |
| [WS313](ws313/ws.md) | MG008 | ODROID-C5（Amlogic S905X5M）の BSP（bsp-s905x5m）と Mali の GPU で、起動して desktop を表示する（2026-10-29 ユーザー） | incomplete（全 block の code を揃えた、実装済み・実機未試験。2026-10-29 夜にユーザーが実機で試験） | 夜の試験（plan/ws313/tests/README.md）→ 直し、Mali の desktop（render pass の LOAD など） |
| [WS316](ws316/ws.md) | MG003 | 5330 の USB4 と AQC の 10GbE（2026-10-30 ユーザー） | planning | p001 の調べ（5330 の Debian で CM・AQC の見え方・IOMMU） |
| [WS317](ws317/ws.md) | MG005 | hotplugd: hot-plug の device（PCIe・USB・ディスク）の承認と driver の開始・終了、volumed を置き換え（2026-10-30 ユーザー） | planning（今は取り組まない、ユーザー） | p001 の設計から |
| [WS318](ws318/ws.md) | MG006 | libmedia の作り直し: Vulkan の device の共有・codec の問い合わせと指定・encoder（H.264・H.265・AAC-LC）・カメラ・libavcodec の内部の fallback、app を libmedia だけに（2026-10-30 ユーザー） | planning | p001 設計から。2026-10-30 ユーザー「WS318はH.264デコーダのあとにすぐスケジュールします。」→ WS326 の completed の直後 |
| [WS319](ws319/ws.md) | MG006 | i915（5330、Alder Lake） の Vulkan Video: H.264 encode（VDEnc、HuC の firmware）、H.265 decode・encode | planning | WS318 の completed の後 |
| [WS320](ws320/ws.md) | MG006 | Pi 4（BCM2711） の Vulkan Video: H.264 encode（VideoCore の MMAL video_encode を Vulkan Video の encode として、WS236 と同じ道）、H.265 decode（HEVC の block） | planning | WS318 の completed の後 |
| [WS321](ws321/ws.md) | MG006 | Pi 5（BCM2712） の Vulkan Video: H.265 decode（HEVC の block を Vulkan Video として）。H.264 は hardware に無く libavcodec | planning | WS318 の completed の後 |
| [WS322](ws322/ws.md) | MG006 | VisionFive 2（JH7110） の Vulkan Video: H.264・H.265 decode（Wave511）、H.265 encode（Wave420L）を PowerVR の Vulkan の executor の Vulkan Video として。H.264 encode は hardware に無く libavcodec | planning | WS318 の completed の後 |
| [WS323](ws323/ws.md) | MG006 | ODROID-C5（Amlogic S905X5M） の Vulkan Video: H.264・H.265 の decode と encode（Amlogic の VPU、encode は hardware の有無の確認から）を Mali の Vulkan の executor の Vulkan Video として | planning | WS318 の completed の後 |
| [WS324](ws324/ws.md) | MG006 | RTX 2070（nvrtx） の Vulkan Video: H.264 encode（NVENC）、H.265 decode（NVDEC）・encode（NVENC） | planning | WS318 の completed の後 |
| [WS325](ws325/ws.md) | MG006 | RDNA（RX 5600 XT、VCN 2.0） の Vulkan Video: H.264・H.265 の decode と encode（VCN） | planning | WS318 の completed の後 |
| [WS326](ws326/ws.md) | MG006 | libmedia の H.264 の software の decoder（独自、module の設計は Opus、実装は Sonnet 5.5 high）（2026-10-30 ユーザー） | incomplete（q1695 P1 が p001） | p001 module の設計 |
| [WS327](ws327/ws.md) | MG006 | libkeiland の thread と network の薄い wrapper（POSIX と Win32 で同じ interface）と app の移行、libbrowser は使わない（2026-10-30 ユーザー） | incomplete（q1697 P2） | p001 設計から |
| [WS328](ws328/ws.md) | MG006 | libbrowser のベクトル描画（path・CSS の角丸・gradient・影）と SVG の描画（2026-10-30 ユーザー）。WebP・AVIF は WS326 の結果の後 | planning（WS074 と調整してから） | p001 設計から |
| [WS329](ws329/ws.md) | MG006 | libmedia の H.264 の software の encoder（標準廉価実装手順、decoder と共通化）（2026-10-30 ユーザー） | planning | WS326 の completed の後に p001 設計（Opus） |
| [WS330](ws330/ws.md) | MG006 | libmedia の H.265 の software の decoder と encoder（標準廉価実装手順、H.264 と共通化）（2026-10-30 ユーザー） | planning | WS329 の completed の後に p001 設計（Opus） |
| [WS292](ws292/ws.md) | MG006 | GHCI の session の保護（PROT）を zedBSD の driver に（WS290 の後） | planning | p001 設計（WS290 と G29 の回答の後） |
| [WS295](ws295/ws.md) | MG006 | Intel の xe の driver（Meteor Lake 以降の iGPU と discrete の Arc、GuC の submission、i915 の使える所は写して重複） | planning | p001 調査（担当の起動はユーザーの指示の時） |
| [WS296](ws296/ws.md) | MG001 | src/drivers（gpu/ を除く）の comment から開発の管理情報を消し、XXX の規約の当て直し（usb など 11 file） | planned | 18 LW、担当の起動はユーザーの指示の時 |
| [WS307](ws307/ws.md) | MG003 | Keiland のアプリとしての zedBSD のグラフィカルなインストーラ（UEFI＋ACPI の PC、ウィザード、disk 全体／partition の選択、base・desktop・firmware・packages の tarball）（2026-10-28 ユーザー） | incomplete（p001 cleared、仕様は spec.md） | 実装 p002〜p014（134 LW、指示の後） |

### 保留の WS（何をやるかは Q1 とユーザーが整理中）

「元:」は以前の段（ベータ3・4 などは取り消し）。

| WS | Primary | 内容 | 状態 | 再開点 |
| --- | --- | --- | --- | --- |
| [WS001](ws001/ws.md) | MG002 | POSIX.1-2024 準拠 | incomplete・保留（元: ベータ3、段は取り消し・整理中） | p041〜p044 の guest 試験、p045 POSIX の header、p046〜p051、p040 mesg（BUG-067） |
| [WS013](ws013/ws.md) | MG007 | CPAR（container 分割） | incomplete・保留（Future Work F-002） | F-002（runtime の隔離・CLI・service container） |
| [WS026](ws026/ws.md) | MG001 | 試験資産の整理 | planning・保留（元: ベータ3、段は取り消し・整理中） | Phase 未定義 |
| [WS080](ws080/ws.md) | MG002 | `ld.coff`: Win64 PE32+ の動的ローダ（PE/COFF の mapping・relocation・DLL・import/export・Microsoft x64 ABI・最小の TEB/PEB・GS base）。NT の loader は再現せず `AddressOfEntryPoint` へ直接。互換の DLL は上に積む（2026-09-28 ユーザーの仕様 [spec.md](ws080/spec.md)） | incomplete・保留 | p002 は agent/p1 の a06d5d75d・6c7c991b1（未 merge）、p003〜p012 |
| [WS082](ws082/ws.md) | MG002 | Linux の `/dev/kvm` の移植の検討（eventfd 等の非 POSIX の fd の代わりに unix socket の message で MMIO・IRQ の通知。ioctl を直接の移植・別の仕組みでの代替・実装不能に分類）（2026-09-28 ユーザー） | planning・保留（要検討） | ユーザーの判断（study.md §10） |
| [WS098](ws098/ws.md) | MG006 | IME の変換のニューラル化: 辞書で候補を作り、小型のモデル（15 MB 未満）で同音異義語の選択（語の番号の並び）とひらがな列の形態素解析（語の境界と品詞、BiLSTM か小型の Attention）を評価する（2026-09-29 夜 ユーザー、IME の最後の仕上げ） | planning・保留（要検討・ブロック） | p001 設計（WS095 の後、corpus と license はユーザー） |
| [WS115](ws115/ws.md) | MG002 | upstream GTK4をzedBSD `packages/desktop/gtk4`へ移植し知見を記録 | incomplete・保留（元: ベータ3、段は取り消し・整理中） | p010（GTK の起動、ld.so の上限は WS140 で解決済み）、p003 |
| [WS116](ws116/ws.md) | MG002 | upstream Qt6の範囲をGTK4移植後に検討し `packages/desktop/qt6`へ移植 | planning・保留（元: ベータ3、段は取り消し・整理中） | p001〜p005（WS117 の後） |
| [WS117](ws117/ws.md) | MG006 | Linux の本物の Qt6 を調査し、素の Qt6 アプリが動くよう compositor を改良（WS115/116 の前） | planned・保留（元: ベータ3 以降、段は取り消し・整理中） | p001〜p006 |
| [WS126](ws126/ws.md) | MG002 | Python 3 の package（fg019） | incomplete・保留（元: ベータ3、段は取り消し・整理中） | p005 の guest の受け入れ（test-wait）、p006 規約 |
| [WS144](ws144/ws.md) | MG005 | VPN（bridge・tunnel などの汎用の network の基盤と、選んだ VPN の protocol、Settings の stub の VPN の頁の実体）。ベータ2 の実装の項目、時期は未定（2026-10-04 ユーザー） | planning・保留（元: ベータ4 以降、段は取り消し・整理中） | p001 から |
| [WS146](ws146/ws.md) | MG006 | SSH を使う独自のオンラインストレージ（server に agent を転送、file の木と metadata の database、差分の同期、手元に本体の無い on-the-fly のアクセス、OneDrive のような機能）。「あとで」の要望（2026-10-04 ユーザー） | planning・保留（元: ベータ4 以降、段は取り消し・整理中） | p001 から（WS150 の後） |
| [WS147](ws147/ws.md) | MG006 | Microsoft OneDrive の client（Settings の Sharing の頁から設定、OAuth・Graph の API・同期、on-the-fly は WS146 と共有）（2026-10-04 ユーザー「独立WSにします」） | planning・保留（元: ベータ4 以降、段は取り消し・整理中） | p001 から（WS150 の後） |
| [WS150](ws150/ws.md) | MG004 | userland の file system の kernel の枠組み（FUSE に当たる物）。クラウドストレージ（WS146・WS147）の前提（2026-10-04 ユーザー） | planning・保留（要検討・ブロック） | p001 から |
| [WS152](ws152/ws.md) | MG007 | system の更新（Settings の Updates の頁の実体）。ベータ3 の実装の項目、p001 は方式の検討（2026-10-04 ユーザー） | planning・保留（元: ベータ3、段は取り消し・整理中） | p001 の U1〜U12 の判断 |
| [WS153](ws153/ws.md) | MG007 | Settings の Apps の頁と third-party の app の repository（userland/packages とは別）。p001 は package の仕組みの検討（2026-10-04 ユーザー） | incomplete・保留（ユーザーが検討中） | U2〜U15 の決定の後に p002 以降 |
| [WS158](ws158/ws.md) | MG006 | Keiland 本体と Keiland の app の翻訳（英語が基準、日本語はベータ2、Settings の Languages の頁で選ぶ。F-068 を昇格）（2026-10-05 ユーザー） | incomplete・保留（元: ベータ3、段は取り消し・整理中） | p002〜p005 の T1・ユーザーの日本語の review、p006 |
| [WS171](ws171/ws.md) | MG008 | hal.h の全ての関数に HAL v2 と同じ水準の契約の comment を書く（comment の差分もユーザーの review の後に当てる）（2026-10-05 ユーザー「このコメント、すべての関数につけてほしいです。今でなくていいので」） | incomplete・保留（優先度低め、ユーザーの review 待ち） | p002 のユーザーの review、p003 の適用 |
| [WS172](ws172/ws.md) | MG006 | passkey の認証の枠組み: sessiond は外部の `/sbin/passkey`（base）で password・PIN・FIDO2（将来はセキュリティチップ）を確かめる、root だけの `/etc/passkey`、PIN の失敗の回数は sessiond の memory（2026-10-05 ユーザー） | incomplete・保留（元: 実装はベータ3、段は取り消し・整理中） | p004b の TPM の設計、p005 OpenSSL の置き換え、p007・p011 の試験、p006 規約 |
| [WS176](ws176/ws.md) | MG006 | Canvas: pen と touch のイラスト制作・画像編集（layer・brush・素材集・CLIP STUDIO と PSD の互換）（2026-10-06 ユーザー、ベータ3） | planning・保留（元: ベータ3、段は取り消し・整理中） | p001 から |
| [WS180](ws180/ws.md) | MG006 | Emacs の拡張 — Emacs をベースにしたグラフィカルなエディタ（エージェント開発のための次世代のエディタ） | planning・保留（元: ベータ3 以降、段は取り消し・整理中） | Phase 未定義 |
| [WS184](ws184/ws.md) | MG006 | 左手デバイスの OSK（クリエイターモード: ダイヤル・ホイール・ボタン 2×5、左上の swipe で出す、ベータ2） | planning・保留（元: ベータ3、段は取り消し・整理中） | p001 設計から |
| [WS185](ws185/ws.md) | MG006 | ゲームパッドの OSK とゲームコンソールモード（両上隅の同時 swipe、Xbox の pad を模す、段 1 は mview、ベータ2） | planning・保留（元: ベータ3、段は取り消し・整理中） | p001 設計から |
| [WS186](ws186/ws.md) | MG003 | Realtek RTL8822CE（5320 の PCIe の WiFi、ベータ3、ベータ2 が早く終われば前倒し） | planning・保留（元: ベータ3、段は取り消し・整理中） | p001 調査と設計から |
| [WS197](ws197/ws.md) | MG006 | Bluetooth のスマホ連携: MAP（SMS・MMS）・Integration（WS170）・PBAP・HFP（通話、SCO）。ベータ2 の必須（2026-10-10 ユーザー） | incomplete・保留（元: HFP はベータ3、段は取り消し・整理中） | p006・p007 HFP、p010 の MMS の送信、p014、p008 実機、p009 規約 |
| [WS198](ws198/ws.md) | MG002 | zedBSD の上で zedBSD を self-build（host の clang、2026-10-09 ユーザー、優先度低、ベータ2 の見込み） | incomplete・保留（元: ベータ3、段は取り消し・整理中） | p001 の判定から |
| [WS201](ws201/ws.md) | MG002 | /home の暗号化（UFS の先頭の key slot、FIDO2 の hmac-secret／PRF と回復のパスワード、master key は初回に /dev/random）（2026-10-10 ユーザー、ベータ3、今は検討しない） | planning・保留（元: ベータ3、段は取り消し・整理中） | p001 設計 |
| [WS204](ws204/ws.md) | MG006 | HD Audio の録音（内蔵マイク）と HFP 通話の音（2026-10-11 ユーザー） | incomplete・保留（元: HFP はベータ3、段は取り消し・整理中） | p001 の U1・U2、p002〜p005 |
| [WS205](ws205/ws.md) | MG006 | Bluetooth audio（A2DP の SBC・AAC、headset で HFP 通話、libmedia の encoder）（2026-10-11 ユーザー） | incomplete・保留（元: HFP はベータ3、段は取り消し・整理中） | p001 の D1〜D6、p004〜p011 |
| [WS209](ws209/ws.md) | MG006 | audiod の音の endpoint と route（通話・Bluetooth・modem）（2026-10-11 ユーザー Q12） | incomplete・保留（元: HFP はベータ3、段は取り消し・整理中） | p001 の再 review、p004〜p006 |
| [WS212](ws212/ws.md) | MG006 | Wiseview の作り直し: 仮想デスクトップ 3 枚の矩形と窓の一覧、drag で仮想デスクトップ・display をまたいで移動、multi display の縦の積み重ね、display のローテーション（タグ・Shift+Super+←/→、比率の保持）（2026-10-12 ユーザー） | planning・保留（着手はユーザーが決める） | p001〜p005 |
| [WS222](ws222/ws.md) | MG006 | 画像・PDF・字形の library を menuconfig の Desktop へ戻す（PNG・PDF・JPEG・TrueType）（2026-10-13 ユーザー、今の作業が全部終わってから） | planned・保留（後日） | p001 |
| [WS225](ws225/ws.md) | MG006 | Vulkan の header を仕様から導ける内容だけで書き直し、Khronos の著作権表示を外す（2026-10-13 ユーザー） | planning・保留（ブロック） | p001 |
| [WS234](ws234/ws.md) | MG006 | libkeiland の DLL 版（zedBSD 用と Windows 用）で、同じ .exe が zedBSD と Windows の両方で動く | planning・保留 | p001 |
| [WS240](ws240/ws.md) | MG006 | nvrtx の RTX 50（Blackwell）（2026-10-15 ユーザー） | planning・保留（開始しない） | — |
| [WS251](ws251/ws.md) | MG001 | source の tree 全体から開発の管理情報を削除する（2026-10-15 ユーザー） | incomplete・保留（作業対象になった時） | p001 の棚卸し、drivers の外の群 |
| [WS257](ws257/ws.md) | MG006 | i915 を Haswell・Broadwell（Gen7.5・Gen8）に対応（2026-10-18 ユーザー） | planning・保留（今は取り組まない） | — |
| [WS273](ws273/ws.md) | MG008 | Anbernic RG350（JZ4770、MIPS32）の実機への移植（2026-10-19 ユーザー） | incomplete・保留 | 再開は ws273/phase003/phase.md、実機 U26 |
| [WS275](ws275/ws.md) | MG008 | sun4u に /dev/graphics を実装し Xzed を動かす（2026-10-19 ユーザー） | incomplete・保留 | p006（BUG-337）、p007 |
| [WS281](ws281/ws.md) | MG002 | libc の malloc を速くする（first fit・1 本の list・1 つの lock の作り直し）（2026-10-22 ユーザー） | planning・保留（後日） | — |
| [WS282](ws282/ws.md) | MG006 | camera の driver の枠組み（/dev/camN）と USB Video Class の driver（CSI の rpicam は今回やらない） | incomplete・保留（設計だけ、実装は後日） | p002〜p011（設計 版 3 は済み） |
| [WS283](ws283/ws.md) | MG006 | bcm2711 の Vulkan の compute（Pi 4・Pi 5） | incomplete・保留（設計だけ、実装は後日） | p002〜p014 |
| [WS284](ws284/ws.md) | MG006 | YOLOX-Nano の推論（C と Vulkan compute、Apache-2.0） | incomplete・保留（設計だけ、実装は後日） | p002〜p008 |
| [WS285](ws285/ws.md) | MG006 | camera の object detection の demo の app（WS282〜WS284 の上） | planning・保留（後日） | p001 設計 |
| [WS287](ws287/ws.md) | MG008 | EHCI の full-speed・low-speed の device（split transaction、ZYBO の ChipIdea を含む） | planning・保留（後日） | — |
| [WS297](ws297/ws.md) | MG001 | completed の WS に残った「規約の全文の見直し」を一括で（WS296 と同じ形、2026-10-27 ユーザーの回答「1 つの WS にまとめる」） | planned（時期は整理で） | p001 対象の確定（候補 101 Phase・89 WS） |
