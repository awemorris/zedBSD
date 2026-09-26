<!-- awesome-plan project=zedbsd record=ws035p053 -->

# ws035-p053: `wl_shm`（補助の経路）と cursor

Phase ID: `ws035-p053`
Parent: [WS035](../ws.md)
Status: cleared（q458-i01）
Phase disposition: normal
Queue: q458-i01
設計: [compositing-design.md](../compositing-design.md)（2026-09-25 承認）の D2（`wl_shm`）・D8（cursor）

## 目的

`wl_shm` 1（ARGB8888・XRGB8888）と `wl_shm_pool` を持つ（toolkit の software 描画、cursor の theme、小さな client のための補助の経路）。
cursor を zdesktop が描く（ウィンドウモードだけ）。

## 受け入れ

1. `wl_shm` の client の画像が窓として正しく出る（画面の読み取り）。pool は作成時と `resize` で 1 回だけ mmap し、commit ごとの CPU の copy は damage の範囲の行だけ。copy が終わった時点で `wl_buffer.release`。
2. cursor: zdesktop の既定の cursor（矢印）がウィンドウモードで pointer の位置に alpha で合成される。`wl_pointer.set_cursor` の `wl_shm` の surface と GPU の surface を受け、buffer の無い surface は cursor を隠す。全画面モードでは描かない（copy 0 のまま）。
3. GPU の画像の窓の frame の時間が、`wl_shm` の窓の有無で変わらない（測定）。
4. 新しい C は `plan/coding-style.md` の全文に従い、style-check の指摘 0。amd64 の build は warning 0。boot test。

## 設計（実装の決定）

- `wl_shm_pool`: client の fd（`shm_open` の後に `shm_unlink` した匿名のもの）を pool の作成時と `resize` で mmap（読み取りだけ）。`wl_buffer` は pool の中の offset・幅・高さ・stride・format。
- 窓ごとの staging buffer（HOST_VISIBLE・HOST_COHERENT、persistent map）と sampling 用の VkImage（optimal）を、shm の buffer の大きさで作る（大きさが変わるまで使い回す）。commit で damage の行を CPU で staging へ copy し、次の frame の command buffer の先頭で `vkCmdCopyBufferToImage`（damage の範囲だけ）。p053 では damage を行の範囲（最小の y から最大の y）にまとめる。
- XRGB8888 は alpha を 1 として描く（不透明の pipeline）、ARGB8888 は alpha の pipeline。
- cursor: zdesktop の既定の矢印（16×16 程度、白と黒の縁、ARGB）を起動時に作る。`set_cursor(serial, surface, hotspot)` の surface は role「cursor」になり、commit された画像を cursor に使う（shm と GPU のどちらも）。hotspot を引いた位置に最後に alpha の quad で描く。pointer が動くとウィンドウモードで描き直す（全画面の描き直し、damage は p055）。
- 試験の client: `wl_shm` で描く小さな client（`userland/base/tests/` か wltest の別の mode）と、cursor を動かす QMP の `input-send-event`（usb-tablet の絶対座標）。

## 実装（2026-09-26、q458-i01）

- `userland/base/libwayland/`: client の `wl_shm` 1（`create_pool`、`format` の event）と `wl_shm_pool` 1（`create_buffer`・`destroy`・`resize`）、`WL_SHM_FORMAT_ARGB8888`・`XRGB8888`。今まで client の library に無かった。
- `userland/base/zwl/shm.c`（新）: `wl_shm` の global（bind で 2 つの format）、pool（client の fd を作成時と `resize` で 1 回だけ読み取りで mmap、pool の object と buffer が参照を持つ）、buffer（pool の中の位置、format・範囲を検査）。窓の画像は **linear・host-visible・coherent の Vulkan の画像**（surface ごと、大きさが変わるまで使い回す）で、commit の後、frame が進行中でないときに CPU で **damage の行だけ** copy し、すぐ `wl_buffer.release`。設計の「staging を省いて直接」の案を選んだ（staging と GPU の copy が要らない。copy は 0.1〜0.8 ms、下の測定）。ARGB8888 は alpha の pipeline、XRGB8888 は不透明。zdesktop の矢印（12×19、白と黒の縁）も同じ種類の画像。
- `protocol.c`: `damage`・`damage_buffer` を外接矩形で保ち、commit で committed の damage へ（copy までの複数の commit は合わせる）。role の無い surface の commit を受ける（cursor の surface は `set_cursor` の前に commit される。前は protocol error で client を切っていた）。
- `seat.c`: `wl_pointer.set_cursor`（pointer の上の client だけ、surface と hotspot、surface 無しで非表示）、focus が変わると矢印に戻す。`input.c`: pointer の移動で描き直す。
- `compose.c`: 窓の画像は GPU の import か surface の `wl_shm` の画像。最後に cursor（client の surface を hotspot で、無ければ矢印、非表示なら無し）を alpha で描く。alpha の合成は Wayland の慣習の premultiplied（`srcColor = ONE`）に直した。cursor の buffer と frame callback も frame が持つ。
- **frame の予定**（範囲に追加、設計 D4 の「表示 1 回につき最大 1 回」の補い）: frame が完了したら、その frame で callback を送った窓の commit を、全部そろうか前の frame の時間の半分（4〜50 ms）が経つまで待ってから次の frame を描く。無いと、速く commit する client が次の frame を始め、遅い client の commit がさらに次の frame に回り、その client の frame が半分になった（下の測定）。
- 試験の client `userland/base/wlshm`（新）: `wl_shm` の窓（`--size`・`--color`・`--xrgb`・`--band`（動く帯、変わった行だけ damage）・`--cursor`（8×8 の自分の cursor）・`--hide-cursor`・`--hold`・`--delay-ms`）。`platform/amd64/vmunix.mk` に link の規則、zdesktop の試験の config に追加。
- 試験: `plan/ws035/tests/zdesktop-p053.sh`（画面の読み取り）、`zdesktop-p053-perf.sh`（測定）。

## 検証（2026-09-26、QEMU 8 GiB 4 vCPU NVMe、Venus（host は Lavapipe））

| 受け入れ | 結果 |
| --- | --- |
| 1. `wl_shm` の窓 | `zdesktop-p053.sh`: 赤の `wl_shm` の窓、半透明の緑（premultiplied 0x80008000）が赤に重なって (127,128,0)、GPU の青の窓が上、背景。帯の窓は最初の 2 回が全体（rows 0-300）、その後は帯の行だけ（rows 80-110 など）を copy、copy の後に release |
| 2. cursor | 矢印が pointer (100,100) に（先端の黒、中の白、左の背景）。client の 8×8 の黄色の cursor が hotspot (4,4) で pointer (640,400) に。`--hide-cursor` で非表示（pointer の下が窓の色）。全画面モード（wltest）で cursor は描かれない（pointer の下が模様の青） |
| 3. GPU の窓の frame が `wl_shm` の窓で変わらない | GPU の窓（wltest 200×150）の 30 秒の frame: 単独 273〜290、`wl_shm` の窓 2 つ（毎 frame 全体を描き直す）と 293〜299、帯（部分の copy）と 292〜315。copy は 1 回 0.1〜0.8 ms。frame の予定を入れる前は `wl_shm` の窓 2 つで 127〜130（GPU の窓 2 つでも 185〜192）で、静止した `wl_shm` の窓では 283〜295 だった（copy ではなく frame の開始の順序が原因と切り分けた） |
| 4. 規約・warning・boot | 新しい file（shm.c・wlshm/main.c）と変えた行の style-check 0、build warning 0。boot test PASS（`build/boot-test-amd64-ws035p053/login.png`） |
| 回帰 | p052 の `zdesktop-p052.sh` PASS（窓 2 つ・全画面・戻り、import 12） |

未確認: GPU の画像を cursor の surface にする client（`set_cursor` の GPU の surface）は試していない（経路は窓と同じ `surface_image`）。実機は未実施。

## 結果（2026-09-26、cleared）

`wl_shm` を補助の経路として持った: pool を 1 回 mmap、damage の行だけを host-visible の画像へ copy してすぐ release。cursor は zdesktop の矢印と client の surface（`wl_shm`・GPU）を alpha で合成し、全画面モードでは描かない。frame の予定で、速い client が遅い client の frame を減らさないようにした。

