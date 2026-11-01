# Cockpit

ユーザーが監視する状況の一枚（2026-10-30 ユーザーの指示、[master.md](master.md) の「Cockpit の運用」）。書き手は Q1 だけ。
最終更新: 2026-11-01 20:22（Q1。P1〜P4 を起動、WS074 の origin/browser を merge・push）

## 担当（P1〜P8）

| 担当 | WS | WS の説明 | 今している事 | 作業の予定 |
| --- | --- | --- | --- | --- |
| P1 | [WS291](ws291/ws.md) p026 | GPU の driver の描画の速さ（RTX 2070） | push buffer を記録の時に組む設計（q1838） | 設計 → 敵対的レビュー → 実装は別の Queue |
| P2 | [WS322](ws322/ws.md) p001 | VF2 の VPU を Vulkan Video に（H.264・H.265 decode、H.265 encode） | 調べと設計（q1845） | 設計 → レビュー → 実装の Phase の案。板は P4 の後 |
| P3 | [WS291](ws291/ws.md) p021 | GPU の driver の描画の速さ（Pi 5 の V3D） | dcache clean を省く直し（6d4ef0df9）を Pi 5 で計測（q1840-i02、20:45 再開） | 50 fps を安定して超えたら判定の材料 |
| P4 | [BUG-387](bugs/BUG-387.md) | VF2・Pi 5 で console から Wayland の desktop を起動できない | Pi 5 は PASS（video の member は wayland が 1920x1080 で起動、member でない人は新しい message で拒否）。account-admin・Settings の直しと VF2 の試験 | VF2 の試験 → merge |
| P5 | [WS334](ws334/ws.md) p025 | Web ブラウザ OmniGrammaton の Windows 版（.exe） | script の core の archive と link の確かめ（q1843） | p023（instance と API v4）・p026（libmedia の Windows）へ |
| P6 | [WS335](ws335/ws.md) p005 | libssl-compat・libcrypto-compat（OpenSSL と互換の自前の TLS と暗号） | AES-GCM（q1846）。p004 AES は cleared（28516 checks） | p007 整数の算術・p009 Ed25519 へ |
| P7 | — | — | — | — |
| P8 | [WS074](ws074/ws.md) | Web ブラウザ | 立てない。Claude のクラウドのセッションが担当 | 成果の origin/browser を Q1 が merge |

## WS ごとの進捗

（2026-11-01 10:29 クリア。これより後の進捗をここに書く。各 WS の状態は [master.md](master.md) と各 ws.md）

### WS291 GPU の描画の速さ

- 2026-11-01 20:38: p020（VF2 の PowerVR の model の 50 fps）cleared。c13 55.84・56.33・56.22 fps、c1 167 fps、GPU の ta 8.7 ms・3d 6.9 ms、回帰 PASS（main d5aded6bd）。

- 2026-11-01 20:42: p021（Pi 5）の基準の計測（main b2392803f、offscreen c74）は起動により 44.74 fps と 50.75 fps に割れる。GPU は front-end の詰まり。P3 が CPU の直列の分（pass ごとの約 16 MiB の dcache clean 2 回、約 0.75 ms）を省く直しを build（6d4ef0df9、未計測）。

### WS074 Web ブラウザ

- 2026-11-01 20:1x: origin/browser（クラウド、dev153 = 3,113 / 3,473）を main に merge（272e6c226、push 済み）。同じ基点から P8（local）も i17 で 3,125 / 3,472 まで進めていたので、縦書きの inline の装飾は P8 の版を採って衝突を解いた。host build は warning 0。merge の後の WPT の測り直しは未実施（クラウドのセッションの次の測定で分かる）。
- 2026-11-01 20:4x: origin/main（クラウドが push した 4 commit、iframe の box・画像・SVG ほか）を main に merge（502eb644a、衝突なし、host build warning 0、push はまだ）。

## open の Bug（今日見つかった物）

| Bug | 中身 | 状態 |
| --- | --- | --- |

## ユーザーの判断を待つ事

（無し。項目ごとに、背景・問い・選択肢の中身と得失・Q1 の推しと理由・用語の説明・決まらない間に起きる事を書く。master.md の「判断を待つ事の書き方」）

## ユーザーが拾える試験（頼まない、お手すきの時に）

（2026-11-01 10:29 クリア。手順は各 Bug の ticket にある）

## 決まった事

（2026-11-01 10:29 クリア。決定は各 ws.md の「ユーザーの決定」と [master-design-policy.md](master-design-policy.md)）
