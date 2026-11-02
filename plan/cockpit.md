# Cockpit

ユーザーが監視する状況の一枚（2026-10-30 ユーザーの指示、[master.md](master.md) の「Cockpit の運用」）。書き手は Q1 だけ。
最終更新: 2026-11-02 06:50（Q1。origin/main を merge・push、wave511 の firmware を push）。前の更新: 2026-11-02 02:23（Q1。BUG-394 を merge）

## 担当（P1〜P8）

| 担当 | WS | WS の説明 | 今している事 | 作業の予定 |
| --- | --- | --- | --- | --- |
| P1 | [WS324](ws324/ws.md) | RTX 2070 の Vulkan Video | 返却（11:17）。p007 cleared: H.265 decode が HEVC_v1 の範囲内 131 本中 130 本で正しい、Video Player の GPU の道で H.264・H.265 を再生。今日の作業は終わり | 次の指示で encode（p008a）から再開 |
| P2 | [WS344](ws344/ws.md) | 自前の WebP の decoder | p017（VP8 の decoder の統合、非可逆の画像を libwebp と照合、q1966）に 14:41 着手 | — |
| P3 | — | 待機 | BUG-412: 実の mouse の session（14:30）で、動かさない click 5/5 は効いた。効かなかった 4 回は押した行と離した行が違う（設計どおり）。usb-hid の直しは main に。Pi 5 は診断の状態 | 元の kernel での同じ session（任意）、板の復元 |
| P4 | — | 待機 | WS319 の H.265 decode と Video Player の GPU の道を 12:03 に merge（q1926 cleared）。5330 は Debian で空き | — |
| P5 | — | 待機 | WS336 p015（自前の ssh の client）を 13:46 に merge（host の OpenSSH の sshd と 93 項目 PASS） | p018（sshd の listener、p017 の後） |
| P6 | [WS336](ws336/ws.md) | 互換の ssh・sshd | p017 cleared。p018（sshd の listener と自前の ssh との相互運用、q1965）に 14:36 着手 | — |
| P7 | [WS335](ws335/ws.md) p038 | libssl-compat・libcrypto-compat | limbo の全件の照合と違いの理由（q1929） | — |
| P8 | [WS345](ws345/ws.md) | ブラウザ | JavaScript を窓の event と別の thread に、通信を独立した thread に（p001 の設計、q1959、14:03 着手） | — |

- Sonnet ×3（WS334、2026-11-01）: p024 HTTP の cache・p027 UI の準備・p030 OpenSSL の Windows は全部 cleared。

- Sonnet ×3（WS335、2026-11-02 00:24〜）: p009 Ed25519（q1876）、p016 DER と PEM（q1877）、p025 TLS の定数と読み書き（q1878）。

## WS ごとの進捗

（2026-11-01 10:29 クリア。これより後の進捗をここに書く。各 WS の状態は [master.md](master.md) と各 ws.md）

### WS291 GPU の描画の速さ

- 2026-11-01 20:38: p020（VF2 の PowerVR の model の 50 fps）cleared。c13 55.84・56.33・56.22 fps、c1 167 fps、GPU の ta 8.7 ms・3d 6.9 ms、回帰 PASS（main d5aded6bd）。

- 2026-11-01 20:42: p021（Pi 5）の基準の計測（main b2392803f、offscreen c74）は起動により 44.74 fps と 50.75 fps に割れる。GPU は front-end の詰まり。P3 が CPU の直列の分（pass ごとの約 16 MiB の dcache clean 2 回、約 0.75 ms）を省く直しを build（6d4ef0df9、未計測）。

- 2026-11-01 21:07: p026（RTX の push buffer の設計）cleared。計測で GPU の 1 回の仕事は 9〜10 µs（NVK 13.3 µs より短い）。B-U で subs-1000 は 75〜90 fps の見込み（今 56〜58）、B-K は fps より CPU の時間に効く。Q1 の判断: B-U は待たずに返す、p027（B-U）→ p028（B-K）、P-C は後。

- 2026-11-01 21:17: p021（Pi 5 の V3D の model の 50 fps）cleared。GPU だけが触る作業用の memory（c74 で約 16 MiB）の cache の書き戻しを pass ごとに 2 回していたのを省き、c74 画面 51.58〜52.93 fps・offscreen 53.5〜55.5 fps（4 起動）。desktop は前と同じ（launch 60・drag 60・wiseview 62 ほか）。

- 2026-11-01 22:10: WS291 をラップアップ（ユーザーの指示）。p027（B-U）は実装と実機の段 A まで: 小さな submit 1000 回で 55.5/59.0 → 86.4/82.9 fps（NVK 75.4 を超えた）、100 回で +32 %、画は一致。回帰は未実施なので branch は未 merge。

- 2026-11-01 23:19: p027（RTX の submit の hand-off）を全 architecture の回帰の後に main に merge（70b8b4521）。amd64 RTX: subs-1000 52.9 → 81.2〜83.8 fps、subs-100 466 → 622 fps、他は main と同じ、画の比較 29 一致。Venus・Pi 5・VF2 も main と同じ。

### WS335 libssl-compat・libcrypto-compat

- 2026-11-02 02:2x: p026（TLS の record の保護）cleared。RFC 8448 の record 41 件が一致、9 suite、定数時間の試験 PASS。

- 2026-11-02 02:34: p028（handshake の message の encode と読み）cleared。test-msg 936784 checks、RFC 8448 の ClientHello と送る拡張は byte 一致。

### WS334 Windows 版ブラウザ

- 2026-11-02 02:44: p033 cleared。system の暗い mode を頁に渡す（prefers-color-scheme）、accent の追従、Mica、cookie を DPAPI で暗号化、touch と pen、頁の画像の drag out。Wine で 18 PASS。TSF・DirectManipulation・modal の間の描き直し・題の帯の透過は本物の Windows が要るので p037 に分けた。

### WS322 VF2 の VPU

- 2026-11-02 03:45: p007 cleared。VPU の 1 枚は 1080p で 7 ms・2160p で 20 ms。止まった時の立て直し、再生中の RESET、2 本同時、kill を VF2 で確かめた。Video Player は 1080p30・1080p60 とも再生（1080p60 は始めの数秒だけ遅れ）。JVT は 101 本のうち 75 本が全部の絵で一致、残り 26 本は p007b。VPU の driver の既定の on は p007b の後。

### WS074 Web ブラウザ

- 2026-11-02 06:50: クラウドのセッションが終了（ユーザー）。その最後の origin/main を main に merge し、main を push した（5725993599）。host build warning 0、Windows の cross-build PASS。WS074 は Q1 の管理に戻る。

- 2026-11-01 20:1x: origin/browser（クラウド、dev153 = 3,113 / 3,473）を main に merge（272e6c226、push 済み）。同じ基点から P8（local）も i17 で 3,125 / 3,472 まで進めていたので、縦書きの inline の装飾は P8 の版を採って衝突を解いた。host build は warning 0。merge の後の WPT の測り直しは未実施（クラウドのセッションの次の測定で分かる）。
- 2026-11-01 20:4x: origin/main（クラウドが push した 4 commit、iframe の box・画像・SVG ほか）を main に merge（502eb644a、衝突なし、host build warning 0、push はまだ）。

## open の Bug（今日見つかった物）

| Bug | 中身 | 状態 |
| --- | --- | --- |
| [BUG-402](bugs/BUG-402.md)〜[BUG-407](bugs/BUG-407.md) | Windows 版ブラウザの UAT の 2 回目（検索の button が反応しない、app の icon、遷移中の応答、取得中の表示と URL、about.google の画像、戻るの cache） | open（P8 の待ち、BUG-401 の後） |
| [BUG-395](bugs/BUG-395.md)〜[BUG-399](bugs/BUG-399.md) | Windows 版ブラウザの UAT の 5 件（題の帯の drag、Reload の icon、link で固まる、app の一覧、検索の欄の幅） | open（P8・P4・P7） |
| [BUG-393](bugs/BUG-393.md) | Pi 5 の GPU の起動時の自己試験の描画 | fixed（02:20。既定で off、`rpi4gpu.selftest=1` で on。storage まで 6.3 s → 0.9〜1.2 s） |
| [BUG-394](bugs/BUG-394.md) | 起動が長い、init が desktop を優先して順番を組めるように | fixed（02:23。init の 2 段の起動を既定に。Pi 5 の大きな待ちは GPU の自己試験 5.9 s で BUG-393 で解消。Pi 5 での変更後の計測は未実施） |
| [BUG-390](bugs/BUG-390.md) | App Home の icon が出ない（全機械） | fixed（01:42、画面: build/review/q1889-pi5-home-after.png） |
| [BUG-391](bugs/BUG-391.md) | compositor が落ちると login の画面から出直す（tablet で login できない） | fixed（02:09、QEMU の amd64 Venus。crash の後は同じ利用者の desktop か lock 画面、2 分に 3 回目で login 画面） |
| [BUG-392](bugs/BUG-392.md) | 初期の PIN 0000、login・lock の画面で PIN を既定に | fixed（02:09。PIN 4〜6 桁、起動直後から PIN、誤りの回数は root の file に残り再起動で戻らない。PIN 0000 は弱くなる点を security.md に記した） |
| [BUG-389](bugs/BUG-389.md) | RTX の desktop で App Home から browser を開いても窓が出ない（main でも同じ） | open（23:19、WS291 の回帰で発見） |
| [BUG-387](bugs/BUG-387.md) | VF2・Pi 5 で console から Wayland の desktop を起動できない | fixed（21:05）。GPU の node を root:video 0660 に、既定の利用者と Settings で作る人を video に、CI の Pi 5 は GPU on・graphical login |

## ユーザーの判断を待つ事

（無し）

- 2026-11-02 08:15 Latitude 5330 の NVMe の zedBSD を main d00e07108 の image（plan/tools/hw5330/config-5330.mk）に更新（ユーザーの指示）。5330-run --nvme-image PASS、画面 build/review/5330/q1-5330-main-d00e071.png。Debian に戻した。

## ユーザーが拾える試験（頼まない、お手すきの時に）

（2026-11-01 10:29 クリア。手順は各 Bug の ticket にある）

- Pi 4 の GPU の回帰（WS291 p021 の直し 6d4ef0df9 は Pi 4 と Pi 5 で共有の code。Pi 4 は今 SwitchBot に繋がっておらず担当は遠隔で試せない）: Pi 4 に main の image を入れ、desktop と GPU の app（mview など）が前と同じに動くか。
- VF2 の root（UFS 256 MB）の空き: 2026-11-01 の計測で 95%〜満杯になり、P4 の mview の texture の写しが ENOSPC で切れた。試験は /tmp（tmpfs）に置く形で回避した。板の古い物（~kei の p7・p15 など他の担当の置き物を含む）の整理か、SD の root の拡げをするか。
- OmniGrammaton.exe（Windows 版のブラウザ）を本物の Windows で: main の build の zip（`make browser-win64-zip`）を展開して起動し、WS334 p032 の M1〜M8（描画・--no-gpu・https・location の欄・IME での日本語入力・cookie と localStorage の保持・暗い mode・snap の配置）。Wine では IME・snap・公開の site が確かめられない。
- Pi 5 で BUG-391・392: desktop で compositor を kill -9 して desktop から戻るか、Super+L の後の kill で lock 画面から戻り PIN 0000 で開くか、login 画面で PIN 0000 が既定で出るか（QEMU では確認済み、Pi 5 で 1 s 後に GPU が使えるかは未確認）。
- Pi 5 で BUG-412（click）: 元の kernel に戻した板で、同じように動かさない短い click を数回（今の直しの候補の kernel では、動かさない click 5 回とも効いた。元の kernel でも効けば直しの効果は分からないまま）。
- Pi 5 の HDMI: 今の capture の板は、host で開いている間だけ hotplug・EDID を出す。Pi の電源を入れる前に capture を開けば 1920x1080 60 Hz で desktop が出る（P3 が確かめた）。
- Pi 5 の en0 の link: P3 の計測で、起動 3・4 の途中に en0 が 1000 Mbps から 100 Mbps に落ちて SSH が切れた。cable・switch の側か driver かは未調査。

## 決まった事

- 2026-11-02 12:45 WS344（自前の WebP の decoder）: ユーザーが VP8 の特許の危険を受け入れて、自前の VP8 の decoder を進める（許諾の条件は後でユーザーが確かめる）。

- 2026-11-02 10:52 WS335 の完了の後: Windows 版のブラウザを libssl-compat・libcrypto-compat に切り替え（WS334）、互換の ssh・sshd を完成（WS336）、CI の config から OpenSSH・OpenSSL・curl・libcurl を外し base に自前の ssh・sshd・libssl.so・libcrypto.so と fetch の https（[WS343](ws343/ws.md)）。ca-certificates は packages に残す。

- 2026-11-02 08:28 [WS324](ws324/ws.md)・[WS322](ws322/ws.md): 今日の作業は decode（WS324 は p007、WS322 は p006）と Video Player の GPU の道での再生までで終え、担当を終了する。encode は保留でなく、次の指示ですぐ再開する。両 WS は incomplete。

- 2026-11-02 06:30 [WS324](ws324/ws.md): p009b（NVENC の関門）を止めて、H.265 の decode（p005〜p007）を先に。

- 2026-11-02 06:09 [WS324](ws324/ws.md) Q-p009b-1: NVIDIA の driver の package（.run）の GSP の firmware の log の書式の区画を、GSP の log を文に直す目的だけで host で読んでよい（repo には入れない）。

（2026-11-01 10:29 クリア。決定は各 ws.md の「ユーザーの決定」と [master-design-policy.md](master-design-policy.md)）

- 2026-11-01 21:41 [WS322](ws322/ws.md) Q322-2: Wave420L の参考の source は BSD-3-Clause を採って値と手順だけ使う。
- 2026-11-01 21:41 [WS322](ws322/ws.md) Q322-3: GPU で直に表示する道（WS318 の p006b・p007a・p009）を先に実装する。
- 2026-11-01 21:42 [WS322](ws322/ws.md): GPU で直に表示する道は WS243 で既にあったので、WS318 を始めず WS322 の p003・p004 で VF2 を既存の道（Video Player の VP_GPU_PRESENT）に載せる。P7 に WS318 は割り当てない。
