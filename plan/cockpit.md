# Cockpit

ユーザーが監視する状況の一枚（2026-10-30 ユーザーの指示、[master.md](master.md) の「Cockpit の運用」）。書き手は Q1 だけ。
最終更新: 2026-11-01 20:22（Q1。P1〜P4 を起動、WS074 の origin/browser を merge・push）

## 担当（P1〜P8）

| 担当 | WS | WS の説明 | 今している事 | 作業の予定 |
| --- | --- | --- | --- | --- |
| P1 | [WS291](ws291/ws.md) p027 | GPU の driver の描画の速さ（RTX 2070） | libvulkan の submit を返事を待たずに返す形（B-U）の実装と計測（q1849） | p028 kernel の組み置き（B-K） |
| P2 | [WS322](ws322/ws.md) p001 | VF2 の VPU を Vulkan Video に（H.264・H.265 decode、H.265 encode） | 調べと設計（q1845） | 設計 → レビュー → 実装の Phase の案。板は P4 の後 |
| P3 | — | — | 返却（21:17）。WS291 p021（Pi 5 の model の 50 fps）は cleared: c74 画面 51.6〜52.9 fps（前 49.2〜50.7） | 次の Pi 5 の作業は未割当 |
| P4 | — | Bug の修正の担当 | 返却（21:28）。BUG-387 は fixed、CI の Pi 5 の image で graphical login の desktop まで起動（画面: build/review/q1850-pi5-ci-desk1.png） | 次の Bug の割当を待つ |
| P5 | [WS334](ws334/ws.md) p023 | Web ブラウザ OmniGrammaton の Windows 版（.exe） | instance と API v4（q1847）。p025 は cleared（script の core が単体の archive で link でき、Wine で host と bytes 一致） | p026（libmedia の Windows）へ |
| P6 | [WS335](ws335/ws.md) p007 | libssl-compat・libcrypto-compat（OpenSSL と互換の自前の TLS と暗号） | 固定幅の整数と Montgomery の算術（q1851）。p004 AES・p005 AES-GCM は cleared | p009 Ed25519・p010 P-256 へ |
| P7 | — | VF2 の desktop の描画の速さ | 返却（21:35）。type 60・61 fps（10/22 は 41〜44）、output 43〜47（32〜41）、打鍵から表示 17 ms（30）、launch・drag・home・wiseview は 60 前後 | 未割当 |
| P8 | [WS074](ws074/ws.md) | Web ブラウザ | 立てない。Claude のクラウドのセッションが担当 | 成果の origin/browser を Q1 が merge |

## WS ごとの進捗

（2026-11-01 10:29 クリア。これより後の進捗をここに書く。各 WS の状態は [master.md](master.md) と各 ws.md）

### WS291 GPU の描画の速さ

- 2026-11-01 20:38: p020（VF2 の PowerVR の model の 50 fps）cleared。c13 55.84・56.33・56.22 fps、c1 167 fps、GPU の ta 8.7 ms・3d 6.9 ms、回帰 PASS（main d5aded6bd）。

- 2026-11-01 20:42: p021（Pi 5）の基準の計測（main b2392803f、offscreen c74）は起動により 44.74 fps と 50.75 fps に割れる。GPU は front-end の詰まり。P3 が CPU の直列の分（pass ごとの約 16 MiB の dcache clean 2 回、約 0.75 ms）を省く直しを build（6d4ef0df9、未計測）。

- 2026-11-01 21:07: p026（RTX の push buffer の設計）cleared。計測で GPU の 1 回の仕事は 9〜10 µs（NVK 13.3 µs より短い）。B-U で subs-1000 は 75〜90 fps の見込み（今 56〜58）、B-K は fps より CPU の時間に効く。Q1 の判断: B-U は待たずに返す、p027（B-U）→ p028（B-K）、P-C は後。

- 2026-11-01 21:17: p021（Pi 5 の V3D の model の 50 fps）cleared。GPU だけが触る作業用の memory（c74 で約 16 MiB）の cache の書き戻しを pass ごとに 2 回していたのを省き、c74 画面 51.58〜52.93 fps・offscreen 53.5〜55.5 fps（4 起動）。desktop は前と同じ（launch 60・drag 60・wiseview 62 ほか）。

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
- Pi 5 の en0 の link: P3 の計測で、起動 3・4 の途中に en0 が 1000 Mbps から 100 Mbps に落ちて SSH が切れた。cable・switch の側か driver かは未調査。

## 決まった事

（2026-11-01 10:29 クリア。決定は各 ws.md の「ユーザーの決定」と [master-design-policy.md](master-design-policy.md)）

- 2026-11-01 21:41 [WS322](ws322/ws.md) Q322-2: Wave420L の参考の source は BSD-3-Clause を採って値と手順だけ使う。
- 2026-11-01 21:41 [WS322](ws322/ws.md) Q322-3: GPU で直に表示する道（WS318 の p006b・p007a・p009）を先に実装する。
