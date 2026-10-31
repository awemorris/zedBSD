<!-- awesome-plan project=zedbsd record=ws318-questions -->

# WS318 ユーザーの判断を待つ点（p001、2026-10-31 P5）

- WS: [WS318](ws.md)「libmedia の作り直し: Vulkan の device の共有・codec の問い合わせと指定・encoder・camera・libavcodec の内部の fallback、Keiland の app を libmedia だけに」。
- 状態: planning。p001（設計）を P5 が書いた（[design.md](design.md)）。実装は WS326（H.264 の software の decoder）の completed の後にすぐ始める予定（WS326 に触れない Phase は先に始められる、design §14）。
- 決まっていない事: 下の Q318-1〜14。どれも推しの案があり、推しのままなら設計はそのまま実装に入れる。Q318-3 は Q1 の承認（toolchain の扱い）も要る。design-reviewer の指摘（[design-review.md](design-review.md)）で 10〜14 を足した。
- 用語:
  - **Vulkan Video**: GPU の動画の decode・encode の hardware を Vulkan の命令で使う標準の口。zedBSD は hardware の codec を全部これで見せる（ユーザーの決定 5）。
  - **driver の WS**: 各 GPU（i915・Pi 4・Pi 5・VF2・ODROID-C5・RTX・RDNA）で Vulkan Video の足りない codec を作る WS319〜WS325。WS318 の completed の後に始める（ユーザーの決定 6）。
  - **vkfake**: この WS で作る試験の道具。host の CPU で動く Vulkan（lavapipe）の上に、偽の「動画の queue」を差し込み、libmedia が正しい順序と値で Vulkan Video を呼ぶかを記録して確かめる。本物の hardware の画質や正しい bitstream は確かめられない。
  - **libavcodec**: FFmpeg の codec の library。zedBSD では別の package（LGPL）で、libmedia が有れば dlopen して使う。
  - **sysroot**: zedBSD の program を build する時に使う header と library の一式。全部の build が共有する。
  - **KHR・provisional**: Vulkan の拡張の名前の印。KHR は正式に決まった物、provisional（EXT の暫定）は後で変わりうる試しの物。
  - **compute**: GPU の汎用の計算の命令。Pi 4 の GPU の driver は今これを持たず、描画（graphics）だけを持つ。

## Q318-1 driver が無い機能の確かめ方

背景: libmedia に作る Vulkan Video の encode（H.264・H.265）と H.265 の decode は、今どの GPU の driver も持っていない（WS319〜WS325 で作る）。WS318 を先に完了させる決定（6）なので、WS318 の中では実の hardware で動かせない。

- **A（推し）**: WS318 では vkfake で呼び方と値を確かめて完了にする。実の hardware での確かめ（bitstream の正しさ・画質）は WS319（i915）の最初の受け入れにし、そこで見つかった libmedia の誤りは WS319 の範囲で直す。決定 6 の「libmedia を作りきってから driver」を、実機の確かめだけ driver の WS に回す形に緩める。利点: 予定どおり driver の WS に進める。欠点: libmedia の誤りが driver の WS で見つかる。
- B: これらの backend を WS318 から外し、最初の driver の WS（WS319）で作る。利点: 実機と一緒に作れる。欠点: libmedia の口が WS318 の完了の時点で揃わない。
- C: WS318 を driver が 1 つできるまで完了にしない。利点: 決定 6 のまま全部を確かめられる。欠点: driver の WS と WS318 が循環して待つ（driver の WS は WS318 の後）。

## Q318-2 libavcodec の encode の fallback と zedBSD の package

背景: 決定 4 で「hardware の encoder が無い時は libmedia の中で libavcodec に fallback」とした。しかし zedBSD の libavcodec の package は LGPL の build で encoder を全部無効にしている。また FFmpeg 自身の LGPL の encoder に H.264・H.265 は無い（使えるのは GPL の x264・x265 か、別の library の openh264）。

- **A（推し）**: package は今のまま（LGPL、encoder 無し。AAC の encoder も無いが、AAC は独自の encoder がある）。libmedia の encode の fallback は作り、encoder を持つ libavcodec が入っている system（利用者が自分で入れた物、host の Debian など）で働く。zedBSD の標準の image では、hardware の無い機械の H.264 の encode は WS329（独自）の完了までは無い。利点: license が今のまま。欠点: 当面 zedBSD の標準の image では効かない。
- B: GPL の build の libavcodec（x264・x265 入り）を別の package として作る（package の license は GPL、libmedia は dlopen で使うだけ）。利点: すぐ encode できる。欠点: GPL の package が増え、license の監査が要る。
- C: openh264（BSD）を package にして libavcodec から使う。利点: BSD。欠点: Cisco の特許の扱い（配布の形の条件）を確かめる必要がある。

## Q318-3 Vulkan の header の更新（ユーザーの判断と Q1 の承認）

背景: zedBSD の Vulkan の header（`include/libc/vulkan/`）は Khronos の 1.3.269 から選んで作った物で、H.265 の decode の構造体（1.3.269 の元には KHR として入っているはず、照合は p011a）が選ばれておらず、encode の正式の拡張（KHR）は 1.3.269 には provisional（EXT）しか無い。header は全 build が使う sysroot に入るので、作り直しは Q1 が行う（toolchain の扱い、Q1 の承認）。HAL は変えない。

- **A（推し）**: 2 段で行う。(1) 版はそのままで H.265 の decode を選び直す（p011a）。(2) KHR の encode のある 1.3.274 以降の固定の tag で作り直す（p011b、具体の tag は一覧を見て決め `API-PROVENANCE.md` に記録）。どちらも今の使い手（libvulkan・libmedia・GPU の driver）を build で確かめる。
- B: (1) だけ。encode の hardware の backend は WS318 から外す（driver の WS で header と一緒に）。
- C: どちらもしない（H.265 の decode と hardware の encode を WS318 から外す）。

## Q318-4 MJPEG の camera の decode

背景: 多くの webcam は大きな画面・高い frame rate を MJPEG（JPEG の連続）でしか出さない。JPEG の decode は Vulkan Video に無いので libmedia が行う。2026-10-12 の決定（画像の処理は GPU で、毎 frame の経路で CPU で画素を触らない）との兼ね合いがある。

- **A（推し）**: 符号の解読（Huffman、順番に読むしかない部分）だけを CPU、逆変換（IDCT）と色の変換を GPU の描画の pass で（Pi 4 は compute が無いので描画で行う）。camera が同じ大きさ・rate で YUYV・NV12 を出せる時はそちらを先に選ぶ。利点: 画素の重い処理が GPU。欠点: 作る量が多い（設計と実装の 2 Phase）。
- B: まず全部を CPU（既存の独自の libjpeg-compat）で decode し、GPU 化は後の Phase。利点: 早く動く。欠点: Pi 4 などで重く、2026-10-12 の決定（毎 frame の画素を CPU で触らない）の例外になる。
- C: MJPEG は今は扱わない（YUYV・NV12 の mode だけ）。利点: 作らずに済む。欠点: 多くの webcam で大きな画面・高い rate が使えない。

## Q318-5 AAC-LC の encoder に足す物

背景: 決定 6 で AAC-LC の encode を独自に（profile は decoder と同じ）。既存の encoder（WS205、Bluetooth 用）は長い窓だけ・TNS 無し・PNS 無し・1〜2 channel・32/44.1/48 kHz。用語: **短い窓（窓の切り替え）**は打楽器などの急な音の前に出る「前のにじみ」を防ぐ仕組み、**TNS** は時間方向の雑音の形を整える道具、**PNS** は雑音らしい帯域を「雑音」とだけ送って bit を節約する道具（低い bitrate で効く）。

- **A（推し）**: 窓の切り替え（短い窓）と TNS を足す。PNS は足さない。rate は decoder が受ける 8〜96 kHz、channel は 1〜2（5.1 などは後）。
- B: 窓の切り替えだけ。
- C: 今のまま（encoder の口に載せるだけ）。

## Q318-6 camera の実機の確かめと WS282

背景: camera の libmedia の口は作れるが、実の camera から frame を取る UVC の driver（WS282）は実装済み・実機の試験が未実施で保留中（webcam の購入も未決）。WS318 の completed を driver の WS（WS319〜）の前提にしているので、camera の実機を待つと driver の WS も遅れる。

- **A（推し）**: camera の実機の Phase（p027）だけを WS282 の実機の確かめの後に残し、それ以外の WS318 の Phase が終われば driver の WS（WS319〜）を始めてよいことにする（決定 6 の「completed の後」を、この 1 Phase だけ緩める）。利点: driver の WS が待たない。欠点: WS318 が長く incomplete のまま残る。
- B: WS318 の受け入れから camera の実機を外し、WS282（か WS285）の受け入れに移す。利点: WS318 を早く completed にできる。欠点: libmedia の camera の実機の確かめが別の WS に移る。
- C: WS282 の実機の確かめを先に行い、WS318 を全部終えてから driver の WS。利点: 決定 6 のまま。欠点: webcam の購入と WS282 の再開を待つ。

## Q318-7 file への書き出し（録画）

背景: camera → encoder の流れは作るが、encode した結果を MP4 などの file にする部分（muxer）は今の libmedia に無い（読む側だけ）。

- **A（推し）**: この WS の範囲の外。録画の app を作る時に新しい WS で。
- B: MP4 の書き出しをこの WS に足す。

## Q318-8 H.265 の範囲

背景: H.265 の decode・encode を Vulkan Video で扱う。10 bit（Main 10）は出力の形式（P010）と表示・変換の道が別に要る。

- **A（推し）**: Main（8 bit 4:2:0）だけ。Main 10 は後の WS。
- B: Main 10 も入れる。

## Q318-9 hardware の encode の B-frame

背景: B-frame は前後の frame を参照して圧縮を上げるが、並べ替えと遅れが増え、camera の生の配信には向かない。

- **A（推し）**: 最初は IDR と P だけ（B-frame 無し）。問い合わせには device の B-frame の数を出し、後で足せる形にする。
- B: B-frame も入れる。

## Q318-10 i915 の driver の小さな変更をどこで行うか

背景: i915（5330、基準の機械）の Vulkan Video の decode の出力は、今の driver ではそのまま描画に使えない（sample できない）。libmedia は driver を変えずに GPU の中の copy 2 回で渡せる（追加の CPU の処理は無い）。driver が decode の出力を sample できる形で出せば copy が無くなる。

- **A（推し）**: WS318 では driver を変えず copy の道で作り、driver の変更は WS319（i915 の Vulkan Video）で行う。
- B: WS318 に i915 の driver の小さな変更（decode の出力を sample できる形に）の Phase を足す。利点: 基準の機械で最初から copy が無い。欠点: driver の作業が WS318 に入る（決定 6 の分け方と違う）。

## Q318-11 browser の `<video>` に libavcodec を使うか

背景: 決定 4 は「Keiland の app は libmedia だけ、無ければ libavcodec」。browser の `<video>`・`<audio>` も libmedia を使うが、今は libavcodec を使っていない。browser は network から来る信用できない file を同じ process で decode するので、FFmpeg の不具合がそのまま browser の危険になる（libbrowser に process の隔離は無い）。

- **A（推し）**: browser の engine は libavcodec を使わない（hardware と独自の software だけ）。他の app（Video Player・Music）は libavcodec まで使う。
- B: browser も libavcodec まで使う（見られる形式が増える）。

## Q318-12 毎 frame の CPU の複写（2026-10-12 の決定との関係）

背景: 2026-10-12 の決定は「描画の毎 frame の経路で画素を CPU で触らない」。次の 2 つは毎 frame CPU が画素を 1 回写す。(1) software の decoder（独自の H.264、libavcodec）の出力を GPU の memory へ（software の decoder は CPU で動くので、出力は CPU の memory にある）。(2) camera が GPU の memory に直に書けない機械での camera の frame（直に書けるのは今 bcm2711 の GPU の driver（Pi 5 の V3D）だけ。i915 は p025 で足す）。どちらも Vulkan に「CPU の memory をそのまま GPU に見せる」拡張が zedBSD に無いため避けられない。

- **A（推し）**: この 2 つを例外として認める（独自の software の decoder は GPU の memory に直に書いて追加の複写を 0 にする。camera は直に書ける機械を増やす）。
- B: 認めない。software の decode の表示と、直に書けない camera を使えないことにする。

## Q318-13 libavcodec が無い時の文言

背景: 今の Video Player・Music は、hardware の decoder が無く libavcodec も無い時に「hardware が無い」の類の文言を出す。新しい libmedia は「libavcodec が無い（入れれば見られる）」を先に返す。利用者が取れる行動を示すための変更で、文言が変わる。

- **A（推し）**: 「libavcodec が無い」を先に出す（入れれば見られることが分かる）。
- B: 今のまま hardware の理由を先に出す。

## Q318-14 WS326 の完了を待たずに一部を始めるか

背景: 2026-10-30 の日程の決定は「WS318 は WS326（H.264 の software の decoder）の後にすぐ」。WS318 の一部（公開の header と骨格、試験の道具、Vulkan の header、H.265 の解析）は WS326 の file に触れないので、WS326 の完了を待たずに始められる（design §14）。

- **A（推し）**: WS326 に触れない Phase（p002・p003a・p003b・p011a・p011b・p016〜p019）は空いた担当で先に始め、WS326 の completed の後に残りへ。
- B: 予定どおり WS326 の completed の後に全部を始める。
