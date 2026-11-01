# Cockpit

ユーザーが監視する状況の一枚（2026-10-30 ユーザーの指示、[master.md](master.md) の「Cockpit の運用」）。書き手は Q1 だけ。
最終更新: 2026-11-01 20:22（Q1。P1〜P4 を起動、WS074 の origin/browser を merge・push）

## 担当（P1〜P8）

| 担当 | WS | WS の説明 | 今している事 | 作業の予定 |
| --- | --- | --- | --- | --- |
| P1 | [WS324](ws324/ws.md) p001 | RTX 2070 の Vulkan Video（H.264 encode、H.265 decode・encode） | 調べと設計（q1854） | 設計 → レビュー → 実装の Phase |
| P2 | [WS322](ws322/ws.md) p003 | VF2 の VPU を Vulkan Video に（H.264・H.265 decode、H.265 encode） | PowerVR の executor の NV12・plane の view・export（q1863）。p002: VF2 で Wave511 が起動し 6 本 68 frame が ffmpeg と一致 | p004 H.264 decode の道 |
| P3 | [WS291](ws291/ws.md) p027 | GPU の描画の速さ（RTX の submit の hand-off） | 回帰: RTX の desktop・game mode・bench と QEMU の Venus（q1859） | PASS なら merge |
| P4 | [WS291](ws291/ws.md) p027 | 同上 | 回帰: Pi 5（arm64）と VF2（riscv64）の __thread の libvulkan（q1860） | PASS なら merge。その後 Bug |
| P5 | [WS334](ws334/ws.md) p028 | Web ブラウザ OmniGrammaton の Windows 版（.exe） | libkeiland の Win32 その 1（窓・loop・入力・Vulkan の surface、q1862）。p026 libmedia の Windows は cleared | p029（題の帯・IME・clipboard・DPI） |
| P6 | [WS335](ws335/ws.md) p010 | libssl-compat・libcrypto-compat（OpenSSL と互換の自前の TLS と暗号） | P-256・P-384 の群（q1861）。p004 AES・p005 GCM・p007 整数は cleared | p011 ECC の鍵・ECDH・ECDSA へ |
| P7 | — | — | 返却（22:26）。WS334 p027 の QEMU の guest の確かめ PASS（画面: build/review/p027-guest/） | 未割当 |
| P8 | [WS074](ws074/ws.md) | Web ブラウザ | 立てない。Claude のクラウドのセッションが担当 | 成果の origin/browser を Q1 が merge |

- Sonnet ×3（WS334、2026-11-01 21:48〜）: p024 永続の HTTP の cache（q1855）、p027 browser の UI の準備（q1856、22:07 返却、host・build PASS、guest は P7）、p030 OpenSSL の Windows の build（q1857）。

## WS ごとの進捗

（2026-11-01 10:29 クリア。これより後の進捗をここに書く。各 WS の状態は [master.md](master.md) と各 ws.md）

### WS291 GPU の描画の速さ

- 2026-11-01 20:38: p020（VF2 の PowerVR の model の 50 fps）cleared。c13 55.84・56.33・56.22 fps、c1 167 fps、GPU の ta 8.7 ms・3d 6.9 ms、回帰 PASS（main d5aded6bd）。

- 2026-11-01 20:42: p021（Pi 5）の基準の計測（main b2392803f、offscreen c74）は起動により 44.74 fps と 50.75 fps に割れる。GPU は front-end の詰まり。P3 が CPU の直列の分（pass ごとの約 16 MiB の dcache clean 2 回、約 0.75 ms）を省く直しを build（6d4ef0df9、未計測）。

- 2026-11-01 21:07: p026（RTX の push buffer の設計）cleared。計測で GPU の 1 回の仕事は 9〜10 µs（NVK 13.3 µs より短い）。B-U で subs-1000 は 75〜90 fps の見込み（今 56〜58）、B-K は fps より CPU の時間に効く。Q1 の判断: B-U は待たずに返す、p027（B-U）→ p028（B-K）、P-C は後。

- 2026-11-01 21:17: p021（Pi 5 の V3D の model の 50 fps）cleared。GPU だけが触る作業用の memory（c74 で約 16 MiB）の cache の書き戻しを pass ごとに 2 回していたのを省き、c74 画面 51.58〜52.93 fps・offscreen 53.5〜55.5 fps（4 起動）。desktop は前と同じ（launch 60・drag 60・wiseview 62 ほか）。

- 2026-11-01 22:10: WS291 をラップアップ（ユーザーの指示）。p027（B-U）は実装と実機の段 A まで: 小さな submit 1000 回で 55.5/59.0 → 86.4/82.9 fps（NVK 75.4 を超えた）、100 回で +32 %、画は一致。回帰は未実施なので branch は未 merge。

### WS074 Web ブラウザ

- 2026-11-01 20:1x: origin/browser（クラウド、dev153 = 3,113 / 3,473）を main に merge（272e6c226、push 済み）。同じ基点から P8（local）も i17 で 3,125 / 3,472 まで進めていたので、縦書きの inline の装飾は P8 の版を採って衝突を解いた。host build は warning 0。merge の後の WPT の測り直しは未実施（クラウドのセッションの次の測定で分かる）。
- 2026-11-01 20:4x: origin/main（クラウドが push した 4 commit、iframe の box・画像・SVG ほか）を main に merge（502eb644a、衝突なし、host build warning 0、push はまだ）。

## open の Bug（今日見つかった物）

| Bug | 中身 | 状態 |
| --- | --- | --- |
| [BUG-387](bugs/BUG-387.md) | VF2・Pi 5 で console から Wayland の desktop を起動できない | fixed（21:05）。GPU の node を root:video 0660 に、既定の利用者と Settings で作る人を video に、CI の Pi 5 は GPU on・graphical login |

## ユーザーの判断を待つ事

（無し）

## ユーザーが拾える試験（頼まない、お手すきの時に）

（2026-11-01 10:29 クリア。手順は各 Bug の ticket にある）

- Pi 4 の GPU の回帰（WS291 p021 の直し 6d4ef0df9 は Pi 4 と Pi 5 で共有の code。Pi 4 は今 SwitchBot に繋がっておらず担当は遠隔で試せない）: Pi 4 に main の image を入れ、desktop と GPU の app（mview など）が前と同じに動くか。
- VF2 の root（UFS 256 MB）の空き: 2026-11-01 の計測で 95%〜満杯になり、P4 の mview の texture の写しが ENOSPC で切れた。試験は /tmp（tmpfs）に置く形で回避した。板の古い物（~kei の p7・p15 など他の担当の置き物を含む）の整理か、SD の root の拡げをするか。
- Pi 5 の en0 の link: P3 の計測で、起動 3・4 の途中に en0 が 1000 Mbps から 100 Mbps に落ちて SSH が切れた。cable・switch の側か driver かは未調査。

## 決まった事

（2026-11-01 10:29 クリア。決定は各 ws.md の「ユーザーの決定」と [master-design-policy.md](master-design-policy.md)）

- 2026-11-01 21:41 [WS322](ws322/ws.md) Q322-2: Wave420L の参考の source は BSD-3-Clause を採って値と手順だけ使う。
- 2026-11-01 21:41 [WS322](ws322/ws.md) Q322-3: GPU で直に表示する道（WS318 の p006b・p007a・p009）を先に実装する。
- 2026-11-01 21:42 [WS322](ws322/ws.md): GPU で直に表示する道は WS243 で既にあったので、WS318 を始めず WS322 の p003・p004 で VF2 を既存の道（Video Player の VP_GPU_PRESENT）に載せる。P7 に WS318 は割り当てない。
