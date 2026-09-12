# QEMU + Venus のリモート検証

`run-venus-remote.py` はローカルビルド、一時イメージの準備、SSH 転送、リモート QEMU の起動、画面とログの回収を一回のコマンドで行う。転送先の既定値は、ユーザーがテストイメージ全体の転送を許可した `awe@10.0.10.25`。ローカルには Python 3、make、通常の amd64 ビルド環境、mtools、SSH/SCP が必要。

リポジトリのルートで実行する。

```sh
python3 plan/ws014/tests/run-venus-remote.py \
  --attempt q306-venus-001 --phase venus --frame 1 \
  --left 255,0,0 --right 0,255,0
```

既定のビルドは `make -j16 ... disk-image`。`BUILD`、`ARCH_IMAGE_DIR`、`DATA_IMAGE`、`SWAP_IMAGE` はすべて `build/venus-amd64/` 配下を指定し、`ZEDBSD_CONFIG` は `plan/ws014/tests/config-venus-amd64.mk` を指定する。通常の `config.mk` は変更しない。

ビルド後の GPT を読み、`vmunix` が存在する FAT パーティションを選択する。元イメージを複製し、その複製内の `zedbsd.cfg` だけを `init=/bin/sh` に変更する。`--init` で別の実行ファイルを指定できる。イメージ内のカーネルとビルドしたカーネルの SHA-256 を照合し、ELF の `vt_history` から求めた物理アドレスを QMP のコンソール取得に使う。

各 `--attempt` は未使用の名前にする。ローカルの保存先は `plan/ws014/temp/remote/<attempt>/`、リモートは `/home/awe/zedbsd-q306-venus/<attempt>/`。リモートのハーネスもイメージと OVMF 変数を複製してから専用 QEMU を起動する。既存 VM は操作しない。

描画後の画面は、`egl-headless` が読み戻した QEMU の framebuffer を専用 Unix socket の VNC 経由で取得する。起動引数は `-display egl-headless,rendernode=/dev/dri/renderD128` と `-vnc unix:<capture>/vnc.sock`。TCP の VNC ポートは開かない。`venus_rfb.py` が RFB 3.8 の nonincremental RAW update を要求し、32bpp little-endian RGB888を受け取り、全画素が揃った時だけ `frame.ppm` を保存する。QMP は引き続き制御、PCI情報、コンソールの物理メモリ取得に使う。

QEMU 10.0.11 のGL scanoutは `SCANOUT_TEXTURE` となるため、`screendump` が参照する `qemu_console_surface()` はNULLを返す。この状態では待機しても `no surface` は解消しない。一方、`egl-headless` は内部surfaceへ画素を読み戻し、VNCの表示listenerへ更新を通知する。この既存経路を使い、QEMUやゲストドライバは画面取得のために改変しない。根拠はQEMU公式ソースの [console.c](https://github.com/qemu/qemu/blob/v10.0.11/ui/console.c)、[egl-headless.c](https://github.com/qemu/qemu/blob/v10.0.11/ui/egl-headless.c)、[vnc.c](https://github.com/qemu/qemu/blob/v10.0.11/ui/vnc.c)。

リモートの追加依存は QEMU のVNC対応と Python 3 標準ライブラリだけで、VNC viewerや画像ライブラリは不要。ハーネスと同じディレクトリへ `venus_rfb.py` も転送し、source manifest・artifact・実行結果でそのSHA-256を照合する。リモートの `result.json` には `capture_method` と、geometry・取得byte数・PPM hashを含む `rfb_capture` を保存する。


明示的な再利用には `--skip-build` を付ける。再利用したことと各ファイルのハッシュも記録される。異なるフレームを確認する例:

```sh
python3 plan/ws014/tests/run-venus-remote.py \
  --attempt q306-venus-002 --skip-build --phase venus --frame 2 \
  --left 0,0,255 --right 255,255,0
```

`--phase 2d` は表示経路だけの対照試験、`--boot-only` はシェル到達の確認。これらの成功は Vulkan 実行の成功を意味しない。フレームの期待値は必ず呼び出し側が指定し、奇数フレームは左が赤・右が緑、偶数フレームは左が青・右が黄。

`result.json` にビルドコマンド、Git commit、関連ソース・カーネル・アプリ・元イメージ・複製イメージ・ハーネスのハッシュ、リモートの結果を保存する。`evidence/` には取得できた JSON、QMP、コンソール、レンダラのログと PPM 画像を失敗時も回収する。ディスクイメージ、OVMF 変数、ソケットは回収しない。成功にはフレームの一致、実行対象のハッシュ一致、QEMU の正常終了、必要な証拠ファイルの回収が必要。

ビルド、転送、ゲスト待機、QMP、RFB、リモート全体に時間制限がある。失敗時も試行ディレクトリを残し、再実行や計画の完了処理は自動では行わない。

実測ホストでは `libvirglrenderer1 1.1.0-2` は導入済みだったが、`virgl-server` は未導入だった。Debian公式の同版パッケージを専用ディレクトリに展開し、既定で `/home/awe/zedbsd-q306-venus/dependencies/virgl-server-1.1.0-2/usr/libexec/virgl_render_server` を指定する。システムに導入済みなら `--render-server /usr/libexec/virgl_render_server` を指定できる。QEMUへ渡す `RENDER_SERVER_EXEC_PATH` と実行ファイルのSHA-256も記録する。環境準備の詳細とパッケージのハッシュはPhaseの実行証跡を参照する。

GPUのresetでファームウェアのsurfaceが消えることがある。その場合は `boot_surface_available=false` を記録し、描画後の `frame.ppm` を必須として検証する。描画前の `boot.ppm` がないことだけでは描画試験を中断しない。


RFB parserだけを再検証する場合は、次をローカルで実行する。実QEMUやリモートホストは使わない。

```sh
python3 -B plan/ws014/tests/test-venus-rfb.py
```

有限の模擬peerで8ケースを確認する。分割された受信、RGB変換と完全な画素coverage、DesktopSize/LastRect、geometry・文字列・矩形の上限、未知encoding、途中EOF、全体deadlineを検証し、不完全な画像を成功として保存しないことを確認する。この試験の成功を、実ホストのVulkan描画成功へ読み替えない。
