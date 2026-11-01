# Cockpit

ユーザーが監視する状況の一枚（2026-10-30 ユーザーの指示、[master.md](master.md) の「Cockpit の運用」）。書き手は Q1 だけ。
最終更新: 2026-11-01 20:22（Q1。P1〜P4 を起動、WS074 の origin/browser を merge・push）

## 担当（P1〜P8）

| 担当 | WS | WS の説明 | 今している事 | 作業の予定 |
| --- | --- | --- | --- | --- |
| P1 | [WS291](ws291/ws.md) p026 | GPU の driver の描画の速さ（RTX 2070） | push buffer を記録の時に組む設計（q1838） | 設計 → 敵対的レビュー → 実装は別の Queue |
| P2 | [WS291](ws291/ws.md) p020 | GPU の driver の描画の速さ（VF2 の PowerVR） | model の 50 fps の受け入れの測り直し（q1839） | 満たせば判定、満たさなければ改善の続き |
| P3 | [WS291](ws291/ws.md) p021 | GPU の driver の描画の速さ（Pi 5 の V3D） | model の 50 fps（49.2〜49.9 の境）（q1840） | coordinate shader の attribute を絞るなど |
| P4 | [BUG-387](bugs/BUG-387.md) | VF2・Pi 5 で console から Wayland の desktop を起動できない | 直しは 9616367f5（build だけ）。Pi 5・VF2 の板の窓を待つ（q1842） | 板の試験 → merge。以後の Bug の修正も P4 |
| P5 | — | — | — | — |
| P6 | — | — | — | — |
| P7 | — | — | — | — |
| P8 | [WS074](ws074/ws.md) | Web ブラウザ | 立てない。Claude のクラウドのセッションが担当 | 成果の origin/browser を Q1 が merge |

## WS ごとの進捗

（2026-11-01 10:29 クリア。これより後の進捗をここに書く。各 WS の状態は [master.md](master.md) と各 ws.md）

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
