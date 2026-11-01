# Cockpit

ユーザーが監視する状況の一枚（2026-10-30 ユーザーの指示、[master.md](master.md) の「Cockpit の運用」）。書き手は Q1 だけ。
最終更新: 2026-11-01 09:49（Q1。週の使用量の残りが少ないため全担当をラップアップ。再開は master.md の引き継ぎ）

## 担当（P1〜P8）

**進み（2026-11-01）**: WS334 p021 cleared（main 2c7e666e2）。WS336 p004（ssh の暗号の組と packet）cleared（main a7cb17936）、WS336 の担当は p005（鍵）へ。VF2 の BUG-386 は en1 が attach されるまで確かめた（link up は線の挿し替えが要る、拾える試験へ）。
**2026-11-01 08:24 の割り振り（ユーザーの指示）**: Bug を先に直す。P1 = VF2 の Bug（[BUG-386](bugs/BUG-386.md) en1 の link → [BUG-385](bugs/BUG-385.md) SD の誤りで root が ro）、P2 = RTX の [BUG-374](bugs/BUG-374.md)（cursor の色が逆）、Sonnet 1 人ずつ WS335 p003（SHA-3）・WS336 p004（暗号の組と packet）、P7 = WS334、P8 = WS074。
**2026-11-01 08:21 再開**: P7（q1808-i02）・P8（q1727-i16）・Sonnet 5 本（q1820〜q1824 の i02）を起動した。07:22 の再起動で壊れた git は 08:05 に修理済み（master.md の「動いている担当と Queue」）。

| 担当 | WS | WS の説明 | 今している事 | 作業の予定 |
| --- | --- | --- | --- | --- |
| P1 | — | — | 返却。BUG-384 を直した（main 98179a6a2、Pi 5 の実機で起動）。空き | — |
| P2 | — | — | ラップアップ（2026-11-01 06:53）。BUG-382 を直した（main ecad7fc74、5330 で確かめ） | — |
| P3 | — | — | ラップアップ（2026-11-01 07:00）。RTX subs-1000 42.7 → 58.7 fps（目標 75.4）、BUG-381・383 を直した。残りの G1 は (b)（記録の時に push buffer を組む）の設計とユーザーの判断が要る | — |
| P4 | — | — | 返却（2026-11-01 06:53）。main の計測: Pi 5 は前後で差なし（c74 約 50 fps、desktop 59〜67）、VF2 は c1 168・c13 56.0 fps（記録 141・52.5）、gpu-selftest 13/13 | — |
| P5 | — | — | 返却（2026-11-01 04:53）。WS336 の設計を仕上げた（推しの答え、TODO の一覧 T1〜T31、基点 d4ab182d2 のまま）。実装は Sonnet が p002 から | — |
| P6 | [WS335](ws335/ws.md) | 自前の libssl-compat・libcrypto-compat | 設計の仕上げ（q1789-i02）: 推しの答え、Windows は 1 つの .exe に static link、新しい基点 | 終われば Sonnet が p002 から実装 |
| P7 | [WS334](ws334/ws.md) | OmniGrammaton（libbrowser のブラウザ）の Windows 移植 | p021（統合: OS に触れる所を OS の層へ）cleared（2026-11-01 08:28、run-unit all PASS: posix 16・win32 18、main 2c7e666e2） | 同じ Queue の残り: 単一の .exe（DLL を作らない）→ 新しい Phase の行 → libkeiland の Windows を pthread 無しに |
| P8 | [WS074](ws074/ws.md) | Web ブラウザ（libbrowser）: Interop 2025 で Chromium が通る試験を全部通す | 自走の 15 回目（q1727-i15）。i14 で 3,107（89.49%）、Acid2・Acid3 100% | CSS_TRACKS の上限 → col の背景 → 縦の inline の背景 |

実装の担当（Sonnet・Opus の単発）: WS335 の p003（SHA-3）・p006（ChaCha20・Poly1305）・p008（X25519）、WS336 の p004（暗号の組と packet）・p005（鍵）（Sonnet、worktree s-wsNNN-pNNN）。

## WS ごとの進捗

（2026-11-01 05:07 ユーザーの指示でクリア。これより後の進捗をここに書く。各 WS の状態は [master.md](master.md) と各 ws.md）

## open の Bug（今日見つかった物）

| Bug | 中身 | 状態 |
| --- | --- | --- |
| [BUG-373](bugs/BUG-373.md) | main の kernel の Wi-Fi の ioctl の検査の順の誤りで、古い networkd が EINVAL を受け network が上がらない | closed（VF2 の実機で確かめた） |
| [BUG-371](bugs/BUG-371.md) | Wi-Fi の初期化の失敗だけで init が有線を含む networking を飛ばす | closed（VF2 の実機で確かめた） |
| [BUG-372](bugs/BUG-372.md) | VF2 の serial の console が入力を受けない（元から未実装） | closed（VF2 の実機で確かめた） |
| [BUG-375](bugs/BUG-375.md) | direct scanout の後に window mode へ戻る時、compositor が EBUSY で終わる（VF2） | closed（P7: 原因は GPU の kernel の命令の buffer が満杯の時に待たずに失敗する事。待つ形に直し、VF2 で 8 回とも落ちず。main 2c5897f26） |
| [BUG-383](bugs/BUG-383.md) | libbrowser が http の page からも file: の副資源を読む（local の file の読み出し、安全） | open（WS074 の領域） |
| [BUG-382](bugs/BUG-382.md) | 5330 の NVMe の p7 を discard して起動した直後に mkfs が失敗（間欠） | open |
| [BUG-379](bugs/BUG-379.md) | Pi 4 の session の中で compositor が描かない。原因は kernel の fork の livelock | closed（Pi 4 の session で描く） |
| [BUG-380](bugs/BUG-380.md) | Pi 4 で mview（3D）の pipeline が作れない（V3D の executor） | open |
| [BUG-378](bugs/BUG-378.md) | arm64 の kernel の text が 2 MiB を越えると起動の途中で止まる（Pi 4、全 arm64 の board に効く） | closed（実機の Pi 4 で起動） |
| [BUG-374](bugs/BUG-374.md) | RTX の mouse の cursor の色が他の GPU と逆 | fixed（2026-11-01 09:08、P2。原因は cursor の色の倍率の既定の値、main に merge。画面の目視は拾える試験） |
| [BUG-370](bugs/BUG-370.md) | Photos で保存すると全面表示が閉じる | closed（QEMU で確かめた） |
| [BUG-376](bugs/BUG-376.md) | PS/2 の mouse が無くても aux の node を作り、open のたびに約 80 ms 待つ | closed（QEMUと実機 で確かめた） |
| [BUG-222](bugs/BUG-222.md) | ue0 の SCP が約 10 Mbps（10-06 の UAT） | closed（10.0.10.3 の ue0 で 2500 Mbps、scp 62〜86 MB/s） |

## ユーザーの判断を待つ事

disk の空きの件（下の 00）だけが作業を遅らせている。

### 00. host の disk の一括の片付け（2026-10-31 02:3x、急ぎ）

- **済み（2026-10-31 14:39）**: 同日 ユーザーから、古い worktree を消してよい、という趣旨の指示を受けた。今動いている担当・午後の試験・VF2 と RTX の続き・git に無い記録を持つ 16 個を残し、211 個を `git worktree remove` で消した（branch は全部残す）。host の / の空きは 4.7 GB → 499 GB。

- 背景: host の / が満杯になり（938G 中 891G、空き 0）、P1・P4・P8・P3・P6 の build が「No space left on device」で落ちた。担当が頼んだ一時物を Q1 が消したが、新しい担当の sysroot の写し（1 つ約 1.7 GB）などで 04:3x に空きは約 3.8 GB（危ない）。担当には出力を全部 /tmp に置かせ、消せる物を挙げさせている。大きいのは古い担当の worktree（`/home/awe/zedBSD-worktrees/` に約 250 個、合わせて約 559 GB）の `build/`（試験の image・kernel・sysroot の写し）。
- Q1 がしようとした事: 今使っていない worktree（返却済み・merge 済みの担当の物）の `build/` の中身を消す（`build/review` の PNG は残す、今動いている担当の worktree と WS313 の ODROID の image は残す、git の中身と branch は消さない）。**権限の判定（取り返せない削除）で止められた**ので、回避せずユーザーに聞く。
- 決めてほしい事: この片付けをしてよいか。よければ settings に許可を足すか、ユーザーが `! ` で直に走らせる。それまで担当は大きな image を /tmp（tmpfs、空き約 69G）に作る。

### 01. AMD の CPU の仕様書の取得（2026-10-31 04:5x、権限の判定で止められた）

- **済み（2026-10-31 14:50）**: ユーザーが 2 つの PDF を置いた。Q1 が `/home/awe/zedBSD-specs/24594_APM_v3-r3.02.pdf`・`ACPI_Spec_6_5_Aug29.pdf` に写した（git の外）。「仕様未取得」で外した部分（WS333 p008・p010 の AMD と _PSS など、WS310 の _OSC の確かめ）は、次にその WS を進める時に入れ直す。

- 2026-10-31 ユーザーから URL を問われ、Q1 が答えた（AMD 24594 の PDF、ACPI 6.5 の PDF と第 8 章の HTML。URL は Q1 の知識からで記録に無かった）。置き場所は git の外の `/home/awe/zedBSD-specs/`。ユーザーが置くのを待つ。

- 背景: WS333 p007（x86 の CPU の register を読む module の設計）で、P7 が AMD の公開の仕様書 APM Vol 3（24594、CPUID の HwPstate・CPB の bit の位置の出典）を amd.com から取ろうとした。1 回目は bot の確かめの page が返り、2 回目に browser の header を付けた取得が**権限の判定（bot の確かめの回避）で止められた**。P7 は回避せず止め、AMD の取得はしない。
- 影響: AMD の CPU の P-state の bit を仕様で裏付けられないので、p008 の範囲から外し「仕様未取得」と記録して進める。5330（Intel）には影響しない。Intel の SDM は取得済み。
- 同じ形でもう 1 件: ACPI 6.5 の第 8 章（_PSS・_PSD・_PPC、firmware が示す P-state の表の形の出典）を uefi.org から取ったら Cloudflare の challenge の page が返り、P7 は再試行せず止めた。p010 から _PSS などの解析を「仕様未取得」として外す。06:3x に WS310（_OSC の設計、P6）でも同じ ACPI 6.5 の PDF の取得が bot の確かめで 403 になり、回避せず「仕様で未確認」の印で進めている。
- 決めてほしい事: ユーザーが AMD APM Vol 3 と ACPI 6.5 の PDF を手で置くか（置き場所は Q1 が決める、git の外）、その 2 つの部分は後回しにするか。

### 02. 共有の /tmp の試験の作業 directory の片付け（2026-10-31 05:1x、権限の判定で止められた）

- **済み（2026-10-31 14:39）**: 同日 ユーザーから、消してよい、という趣旨の指示を受けた。`/tmp/boot-test.*` 300 個と担当が挙げた出力 10 個を消した。返却済みの P1 の WS299 の QEMU 2 つ（9 時間余り動いていた）も、ユーザーの許可を受けて止めた。

- 背景: plan/tools/boot-test.sh は /tmp/boot-test.* に作業の directory を作る。夜の間に QEMU の起動の試験を多く流したので溜まっている。Q1 が QEMU の使っていない物だけを消そうとしたら、権限の判定（共有の scratch の一括の削除）で止められた。回避していない。
- 影響: /tmp（RAM の上）の空きは今約 56 GB あり、急ぎではない。
- 06:5x にも、担当が消してよいと挙げた /tmp の試験の出力（/tmp/ws326-speed.run.*・/tmp/p1-speed-*.run.*・/tmp/p1-stage・/tmp/p1-video-check、scratchpad の interp）を wildcard で消そうとして、同じ判定で止められた。
- 決めてほしい事: 00 の worktree の片付けと合わせて、許可を足すか、ユーザーが `! sh plan/tools/q1-clean.sh --tmp` を走らせるか。

### 0h. WS302（デスクトップのスレッドバランサ DTB）の設計の問い Q302-1〜7（2026-10-31 新しい）

- 背景: WS302 は、kernel が thread の振る舞い（GPU を使う・入力に応える・長く計算する）と nice から、速い P-core と省電力の E-core への割り付けを scheduler に提案する仕組み（CONFIG_DTB、既定 off、ユーザーの決定）。WS333 の CPU の口が入ったので、P5 が設計（約 22 LW、Phase p002〜p007）を書き、敵対的レビュー（blocker 2・major 11）を反映した（main 896d3e3db）。HAL は変えない。全文は [ws302/user-questions.md](ws302/user-questions.md)。
- 用語: P-core は速い core、E-core は省電力の core（5330 は P 2・E 8）。SMT は 1 つの core を 2 つの CPU に見せる仕組み。starvation guard は優先度の低い thread が長く走れない事を防ぐ見張り。
- Q302-1 規則の順: **推し A** nice → 長い計算（E-core）→ 画面を出す process（compositor）→ GPU の app → terminal → その他。（E は terminal を長い計算より先にする案。A は compiler を terminal の規則に含めない読み）
- Q302-2 nice 0 の長い計算が空いた P-core にあふれてよいか: **推し A** よい（nice > 0 はあふれない）。
- Q302-3 優先度の段（user の既定 10、compositor 8、GPU の app 9、nice を 8/9/11/12 に）と 100 ms の見張りを DTB が有効な時だけ: **推し A**。（B は上げない）
- Q302-4 compositor から focus の hint の口: **推し B** v1 には入れない（5330 の数で要れば条件つきの p005）。
- Q302-5 統計は CONFIG_DTB=y の build だけで集める: **推し A**。
- Q302-6 観察の口（読みだけの sysctl `kern.dtb`）: **推し A**。
- Q302-7 受け入れの数: **推し** design.md §6.4 の表（A1〜A7）。
- 止めている物: WS302 の実装（p002 は Q302-5、p003 は Q302-2・3、p004 は Q302-1・6 の答えの後）。

### 0j. WS291 p014（RTX の描画の速さ）の subs-1000 をどう進めるか（2026-10-31 新しい）

- **決まった（2026-10-31 14:39）**: ユーザーから、(b) で進める、という趣旨の指示を受けた。あわせて、image の並びは普通は client が求めるが、そこをどう扱うかを問われた（Q1 の答え: swapchain の image の並びは WSI と driver が決め、読む相手の都合で compositor が読める並びを知らせる形にする。設計で確かめる）。[ws291-p023](ws291/phase023/phase.md) を新設し、P3 が設計（q1781）。

- 背景: RTX 2070 の big-1000 は今夜 67 → 116 fps（NVK 136）まで上がった。subs-1000（1 frame に submit が 1000 回）は 11 fps のまま（NVK 75）。P3 の診断で、zedBSD は GPU の 1 回の実行に約 60 µs（うち pass の終わりの全画面の copy 約 29 µs）かかり、NVK は約 5〜7 µs と分かった。
- copy を省くには、次のどちらかが要る（どちらも範囲が広い）:
  - (a) libvulkan が「誰も見ない私用の fence」を kernel に伝える形に、libvulkan と全 GPU の driver の protocol を変える。
  - (b) 画面に出す image を block-linear（GPU の得意な並び）にする（p013 の (B) の案、WSI の変更）。
- 決めてほしい事: (a)・(b) のどちらかで進めるか、subs-1000 は今の値で止めるか。Q1 の推しは (b)（copy 自体が要らなくなり、desktop の他の GPU にも効く見込み）。

### 0k. WS291 p023（共有の image の画素の並び、RTX の block-linear）の問い Q1・Q2（2026-10-31 15:24 新しい）

- **2 回目の目視（2026-10-31 18:39）**: 正しい（ユーザー: desktop が正常）。P3 の直し effb3d149（block-linear の kind の context DMA）の後。既定を 1 にして計測し直す（P2）。

- **目視の結果（2026-10-31 18:14）**: 誤り。表示はされるが block 単位で乱れる（ユーザーの写真 build/review/ws291/p023-bl-visual-1.jpg）。既定は 0 のまま、P3 が直す（q1781-i03）。直るまで RTX は image を貸さず window mode で合成（P2 の規則）。

- **変わった（2026-10-31 17:42）**: ユーザーの指示で、block-linear の画面出しは目視の前に既定で有効にする。目視は拾える試験として残す。

- **決まった（2026-10-31 15:39）**: Q1 は (b) 目視（CRC は作らない。block-linear の画面出しはユーザーが見るまで既定で切る、確かめの手順は P3 が phase.md に書き「拾える試験」へ）。Q2 は (b) 続けて内訳を測る（[p024](ws291/phase024/phase.md) を新設）。

- 問いの本文: [user-questions.md](ws291/phase023/user-questions.md)。どちらも実装を止めない。
- Q1: RTX の display が block-linear を正しく出したかの確かめ（(a) 推し: display の CRC を読む仕組みを足す、約 3 LW／(b) 目視を拾える試験に、確かめまで既定で切る）。
- Q2: subs-1000 を NVK の 75 fps に近づける続き（(a) 推し: 実装の結果を見てから別の Phase で決める／(b) 続けて内訳を測る Phase を足す／(c) 約 16 fps で止める）。

### 0l. WS334（OmniGrammaton の Windows 移植）の設計の問い U1〜U15（2026-11-01 01:12 新しい）

- **全部決まった（2026-11-01 01:25）**: U3・U10・U12 は決定 17・16・20、残りは推しの案（決定 21）。p001 は cleared。実装は p002 から（担当の割当はユーザーの指示で）。

- 問いの本文: [ws334/user-questions.md](ws334/user-questions.md)（前置き・用語の表つき）。設計は [ws334/design.md](ws334/design.md)。どれも推しの案があり、推しのままなら「推しで」。
- U3（toolchain の llvm-mingw と Debian の gcc-mingw-w64-ucrt64 の導入）は Q1 の許可も要る（ユーザーが推しでよければ Q1 が許可する）。

### 0n. WS336（自前の sshd・ssh）の設計の問い Q1〜Q9（2026-11-01 03:03、2026-11-01 04:45 答え: 推し、TODO の一覧を WS に）

- 問いの本文: [ws336/user-questions.md](ws336/user-questions.md)（前置き・用語つき）。推し: Q1 password の login を入れる、Q2 port の転送は入れない、Q3 sftp-server だけ（scp -O を使う道具 4 本は -O を外す）、Q4 agent は入れない、Q5 OpenSSH と同じ深さの権限の分離、Q6 QEMU の相互運用の試験が通ったら既定を替える（OpenSSH の package は残す）、Q7 鍵の交換は mlkem768x25519-sha256 だけ、Q8 chacha20-poly1305 だけ、Q9 passphrase の無い鍵だけ。

### 0o. WS335（libssl-compat・libcrypto-compat）の設計の問い U1〜U6（2026-11-01 03:04、2026-11-01 04:45 答え: 推し、Windows は static link）

- 問いの本文: [ws335/user-questions.md](ws335/user-questions.md)。推し（どれも A）: U1 範囲の外の一覧、U2 net/tls.c の切り替えは -DWB_TLS_COMPAT（dlopen の名前だけ、p036 の案）、U3 ssh の鍵の交換 mlkem768x25519-sha256、U4 名前と置き場所、U5 Windows の ROOT の store から Disallowed を除き、DLL の横の cert.pem を足す、U6 証明書の CN への fallback をしない。推しでない答えは header の変更（新しい基点）になる。

### 1. WS299 で見つかった、5330 の NVMe 以外の課題

- 背景: 2026-10-28 の P1 の報告（[ws299/ws.md](ws299/ws.md) の「結果（2026-10-28 21:50）」）。idle と HWP は WS333 に入れた。WS299 そのものは、5330 が空いたら担当を付ける（2026-10-31 の指示、q1744）。
- 残り 3 つ:
  - 時刻の読み（clock_gettime）が 1 ms 刻み。vDSO（kernel に入らずに時刻を読む仕組み）が無いため。
  - program が main から return した時に、stdout の buffer が書き出されない（libc の不具合）。
  - libc の memcpy・memset の速さの続き（大きな copy、build の flag -fno-builtin -Os）。
- 決めてほしい事: この 3 つを Bug にするか、WS にするか。

### 2. P4 が 10-27 の記録の監査（q1660）の時に拾った候補（詳しくした物）

- 背景: 10-27 に P4 が、完了した WS の記録を片付ける作業（記録の監査）の途中で、判断が要りそうな物を一覧にした。記録が短く、Q1 が調べ直した結果を書く。
- (a) WS162（FIDO2 の login）を canceled にするか → 調べ直すと、WS162 は 2026-10-27 に「WS172・WS199 で達成」として completed になっていた。**判断は要らない（解決済み）**。
- (b) WS279 p009（Pi 5 の GPU の license と類似性の監査）の担当 → WS279 は完了したが、p009（Linux・Mesa の code を参考にした所が、license の上で問題無いかと、写しになっていないかの監査）は未実施のまま閉じた。誰がいつ行うかが未決。
- (c) T1-691（試験の担当 T1 の 691 番の試験）で見た「P4m の時々の失敗」を Bug にするか → 記録に中身が残っておらず、何の失敗かが今は分からない。Q1 が history を調べるか、取り下げるか。
- (d) `nvrtx.disp.own` の既定 → RTX の driver の起動の parameter で、1 にすると driver が画面を受け取った時に console ではなく driver 自身の画面をすぐに出す。既定を 0 のままにするか 1 にするか。
- (e) `plan/ws004/tests/pci-msi-qemu.c` を残すか → QEMU の EDU という試験用の PCI の device で MSI の割り込みが届くかを見る kernel の試験の code。これを有効にする唯一の config（CONFIG_KERNEL_TEST_CHECKPOINTS）が消えたので、今は build されない。残す（config を作り直す）か、消すか。
- (f) master-design-policy.md へ移す候補 約 25 件 → 各 WS の ws.md に書かれたユーザーの決定のうち、WS をまたいで効く物（設計の方針）を、全体の方針の文書に写すかどうか。一覧は Q1 が作り直して改めて聞く。

### 4. WS153（Settings の Apps の頁と third-party の app の repository）の U2〜U15

- 背景: [WS153](ws153/ws.md) は、第三者の app を repository から入れる仕組み（package の形、配り方、署名、Settings の Apps の頁）の WS。2026-10-05 に U1（導入の範囲は system 全体の /apps/）はユーザーが決めた。残りの U2〜U15 は「ユーザーが検討中（質問しない）」として止めていて、WS153 は p001 の後で保留。設計の文書は [ws153/design.md](ws153/design.md)（各項目に Q1 の推しがある）。
- 項目（推しは design.md の表）:
  - U2 package の形: 自己完結の bundle（推し、依存の衝突が無い）か、共有の依存の package か。
  - U3 配り方（HTTP と署名か HTTPS か）と U4 署名の検証の実装（Ed25519 などを自前で書くか）。
  - U5 公式の repository: 運営するか、誰が鍵を持つか、image に既定で入れるか、鍵が漏れた時。
  - U6 sandbox が無い事（段 1 は無し、入れた app は利用者の全ての権限で動くと画面で示す）。
  - U7 package の名前と拡張子（`.kapp`）。U8 開発者向けの SDK は別の WS にするか。
  - U9 第三者の binary に、どの library のどんな変更までを互換と約束するか（platform の ABI の安定の方針）。
  - U10 手元の署名の無い `.kapp` の導入を許すか（段 1 は許さない）。U11 更新の確認の自動の問い合わせ（1 日 1 回、既定 on）。
  - U12 展開の上限（package 256 MiB・展開 1 GiB など）。U13 導入の確認の dialog を誰が描くか（compositor）。U14 既定の app の UI を段 1 に入れるか（段 2）。U15 system 全体に入れる特権の helper（setuid root の小さな program）の口。
- 決めてほしい事: 今は進めないなら、このまま保留（聞かない）。進めるなら、design.md の推しで決めてよい項目と、話し合いたい項目を教えてほしい。

### 5. 以前からの物（変わっていない）

- WS282〜WS285（camera など）: 実装を始める時期、WS282 の試験の画面を出す PC と買う webcam。
- BUG-345（Pi 5 の click が画面に出ない窓、再発したら知らせてもらう）。

## ユーザーが拾える試験（頼まない、お手すきの時に）

- **VF2 の 2 つ目の LAN の口（[BUG-386](bugs/BUG-386.md)、2026-11-01 08:51 に追加）**: 直した kernel（main 03db37f54）を板に置いた後に、LAN の線を en1 の口（2 つ目）に挿し替えて電源を入れ直す。network に出て SSH が通れば良い（板に直した kernel を置くのは Q1・担当。試す時に言ってもらえれば置く）。済んだら線を en0 に戻す。
- ODROID-C5 の実機で、BUG-384 の直しの後の kernel が U-Boot の booti で起動するか（image_size が BSS を含む大きさになった、main 98179a6a2）。
- VF2: 今の main の kernel で HDMI に console の画面が出るか（WS331 p005 の目視の残り、BUG-372 は直った）。
- installer（BUG-360）: 大きさの欄を全部消して打ち直せるか（QEMU は disk の空きの都合で未実施）。Settings（BUG-306）: 回転を 2 回目に 90° にしても窓が残るか。
- **5330 の電源の入れ直し（済み、2026-11-01 上がっているのを確かめた。P1 が q1788 で試験中）**: 入れ直した後、担当が WS299 の image で NVMe の速さを測り、WS333 p012 の前後の暇な時の電力・p011 の HWP・RAPL・p018 の Dell の mode と fan を実機で確かめる（p014）。 2026-10-31 02:2x、P1 が main の kernel で起動した後に応答しない（遠隔の電源が無い）。入れ直すと Debian に戻る。古い userland と新しい kernel の組み合わせで network が上がらないだけの見込み（BUG-373・371）。P1 はそれまで QEMU で WS299 を進める。
- **Pi 5（2026-10-31 の午後にユーザーが行う予定）**: image は出来ている（P4、main d26c657c7＋Pi 5 の GPU の branch、戻しの kernel 入り）: `/home/awe/zedBSD-worktrees/p4-ws291pi/build/ws291-p021c/rpi5-p021.img`（sha256 dcadd719…）。Q1 が SD に書く（`sudo dd … bs=4M conv=fsync`）か、ユーザーが書く。起動したら Q1 に知らせると担当が SSH で GPU の clock と fps を測る。起動しなければ config.txt の `kernel=vmunix-fallback`。
- **ODROID-C5（2026-10-31 の午後、VF2 の最適化の後）**: 試験の image で HDMI に画面が出なかった（ユーザー）。serial（今は VF2 につないでいる）を C5 につないで、起動のどこで止まるかを見る。Q1 が担当（WS313）を起動して声をかける。
- **10.0.10.3（[BUG-374](bugs/BUG-374.md)、RTX の cursor の色、2026-11-01 09:08 に更新）**: 直した（cursor の色の倍率の register、display の CRC で全色一致を確かめた）。目視の確かめ: 10.0.10.3 の lock の中で `sh plan/ws291/tests/rtx-cursor-view.sh build build/b374/u` → `RESTORE_CKSUM=1463007375 sh plan/ws291/tests/rtx-cursor-view.sh up OUT build/b374/u` で入れ（Q1・担当が行う）、desktop の矢印が内側 白・外枠 黒なら直った。戻しは `rtx-cursor-view.sh back OUT build/b374/u`。

## 決まった事

- 2026-11-01 09:31 WS310（ACPI の _OSC）: ACPI compliant のために必ず通す、system を壊すつもりで進める、最初の試験はデスクトップ（10.0.10.3）、受け入れが通れば既定 on（[ws310/ws.md](ws310/ws.md)）。担当の割当は未。

- 2026-11-01 09:30 WS329（H.264 の encoder）: 画質より速さ、SIMD は自動の vector 化の Phase と short vector 型の intrinsics。残りは Q1 が推しで（[ws329/ws.md](ws329/ws.md)）。WS338 の SIMD も同じ形。
- 2026-11-01 09:30 WS330（H.265）: Main（8 bit）だけ、10 bit は後の WS（[ws330/ws.md](ws330/ws.md)）。

- 2026-11-01 09:24 WS318（libmedia）: hardware が無い所は独自の software に fallback、独自の software が揃えば libavcodec を使わず依存を切る、header は 2 段で更新、MJPEG は扱わない。残りの問いは Q1 が方針に沿って決めた。設計の改訂が要る（[ws318/ws.md](ws318/ws.md)）。

- 2026-11-01 09:16 BUG-339（VF2 の U-Boot の DTB）: 案 (4) 今のまま、案内の log を出して止める（[BUG-339](bugs/BUG-339.md)）。
- 2026-11-01 09:16 x86 の PC の UEFI の起動の画面は monitor（EDID）に合わせ、video=640x480 を外す: [WS339](ws339/ws.md)（planning）。
- 2026-11-01 09:16 Pi 5 の SSH の資格はユーザーから受けた（担当に伝えた）。Pi 5 と x86 の keyboard は VF2 の Bolt の受信機と違う物。

- 2026-11-01 09:05 WS291 p024（RTX の速さ）: G1 は (b) push buffer を記録の時に組む形で、設計から始める（[ws291/ws.md](ws291/ws.md)）。
- 2026-11-01 09:05 WS333: Q333-1〜7 は全部推し（[ws333/ws.md](ws333/ws.md)）。
- 2026-11-01 09:05 WS306: Q306-9 (a) 枠組みを先に進める・Q306-7 (a) amdrdna.vk.ngg を診断用に残す（[ws306/ws.md](ws306/ws.md)）。
- 2026-11-01 09:05 WS326 の速さの続きは新しい WS: [WS338](ws338/ws.md)（planning）。

（2026-11-01 05:07 ユーザーの指示でクリア。決定は各 ws.md の「ユーザーの決定」と [master-design-policy.md](master-design-policy.md) にある）

