<!-- awesome-plan project=zedbsd record=ws121-p001 -->

# ws121-p001: browser の `<video>` の再生の要件・設計

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: U0〜U7 は既定で進め、p002・p004〜p006 が実装・cleared）（旧: in-progress（2026-10-05 夜、P2 g16、q775。設計の第 3 版（review 2 回目の条件 a・b・c・e を反映）。ユーザーの判断 U0〜U7 待ち））
Disposition: normal
Parent: [WS121](../ws.md)
Queue: q775（Q1、2026-10-05）
依存: [WS122](../../ws122/ws.md) の p003（mediafile）・p004（libavcodec の dlopen の add-in、cleared）、[WS107](../../ws107/ws.md)（libbrowser の分離、completed）

## 範囲と方針（2026-10-05 ユーザー、Q1 の伝達）

- ユーザー（2026-10-05）「WS107, WS121はあなたが作業します。」 WS122 と同じく、**まず dlopen の libavcodec の software decode**。GPU の decode（[WS083](../../ws083/ws.md) の Vulkan Video）は後で同じ境界の後ろに足す。
- Q1 の方向: WS122 の video player の add-in（`userland/desktop/videoplayer` の `codec.c`・`bitstream.c`・`codec-layout.h`、FFmpeg の major の確かめ）と自前の demux（`userland/desktop/mediafile`）を使い回す。mediafile の I/O は pread の helper 1 か所（`mf_read_at`）なので、network の data のための reader の callback の source を足し、HTTP の Range で読む。
- この Phase は設計だけ（code を書かない）。実装は p002 以降。

## 調べた今の形（2026-10-05、p2 の worktree、main 274c5304）

| 部分 | 今の形 | `<video>` に要る物 |
| --- | --- | --- |
| DOM | `DOM_TAG_VIDEO`・`AUDIO`・`SOURCE`・`TRACK` は tag の名前だけ（`dom/dom.h`、`dom/names.c`。source・track は void）。要素の固有の状態は `struct dom_element` の field か pointer（subclass は無い。GC の印は `element_trace`、malloc の物は `element_finalize`） | `dom_element` に media の状態への pointer |
| CSS・layout | UA の規則に video が無く `display: inline` の普通の箱、width・height の属性は無視。replaced は IMG・OBJECT・form control だけ（`layout/box.c`、`layout/replaced.c`）。`object-fit` は未対応 | replaced の箱、UA の規則、固有の寸法（300×150 の既定） |
| 画像 | `page/images.c` が取り、`image/decode.c` が `struct img_bitmap`（0xAARRGGBB、straight alpha、`serial`）に。GIF は最初の frame だけ（timer で描き直す物は今は無い）。`<img>` の load・error の event は無い | poster は同じ道 |
| 描画 | display list（`paint/paint.h`: RECT・TEXT・CLIP・UNCLIP・IMAGE）。CPU は `software.c`（最近傍）、GPU は `vulkan.c` の 2048² の atlas に `serial` を key にして 1 回だけ写す。atlas が溢れたら次の prepare で reset | 毎 frame 替わる絵の専用の texture の道 |
| 再描画 | `browser_view_process` は `page_needs_layout` の時だけ redraw。paint だけの道は `page_needs_paint`（`page/input.c`）。`page_paint` は display list を全部作り直す | paint だけの世代（frame ごと） |
| JS | interface の表（`bind/internal.h` の `bind_interface`、`window_interfaces[]`、`node_prototype_index`）。event は `bind_fire_event`、handler の名前は `bind/handler.c` の `handler_types[]`（media の event は無い）。Promise を返す native は `fetch` の形（root して後で settle、`bind_checkpoint`） | HTMLMediaElement・HTMLVideoElement・MediaError・TimeRanges、media の event |
| event loop | engine は単一の thread（loader の DNS の resolver だけ別、pipe で起こす）。埋め込み側が `browser_view_poll_fds`・`_timeout`・`_process` を回す。poll の fd は loader の物だけ。rAF は 16 ms の timer。GC は main の thread の stack を走査する | media の thread からの wake の fd、frame の期限の timeout |
| network | loader は GET だけ、追加の header は If-None-Match だけ、応答は全部を受けてから 1 回の callback、上限 256 MiB、cache は 200 だけ。Range・206 は無い。`file:` は同期で全部を読む | Range の要求・206・Content-Range |
| 音 | shell にも engine にも音は無い。audiod の client は videoplayer の `audio.c` だけ（protocol を直接、`AUDIOD_STREAM_VOLUME` あり、`send` に MSG_NOSIGNAL が無く応答を timeout 無しで待つ）。audiod は client あたり複数の stream を持てる（`struct audiod_client` の `streams` の列）、client は最大 64 | 音の出口 |
| 公開の API | `<browser.h>`（`BROWSER_API_VERSION` 2）。platform の service の hook は無い。ABI の変更は [browser-component の規則](../../standards/browser-component.md) §7 で計画に記録 | 下の D5 のとおり変えない |
| player の部品 | mediafile（MP4・MOV・MKV・WebM、fragment の MP4 は読まない、size が要る、Vorbis・MP3 の container・Ogg・WAV は無い）、`codec.c`（FFmpeg 9 と 7 の major、h264・hevc・vp9・vp8・mpeg4・aac・opus・mp3、AV1 は image の build では無い）、`bitstream.c`、`media.c`（media の thread、8 枚の AVFrame の ring、音の位置で時計、video の track が無いと開けない）。mediafile は library でなく player に直接 compile | 共有する形 |

## 設計（第 2 版、2026-10-05 夜。review 1 回目の反映は末尾の表）

### D0. 原則

- **main の thread（view を使う thread）は待たない**: mediafile・decoder・audiod の I/O は全部 media の thread。main は要求を積み、知らせ（wake の fd）を受けて状態と event に変えるだけ。main が `read_at` を呼ぶ経路は 0（p002 の受け入れ条件）。
- **VM・DOM の cell に触るのは main だけ**: media の thread は C の構造体だけを持つ（GC は main の stack だけを走査する）。
- 止める時は世代（generation）で: seek・load・close のたびに要素の世代を進め、古い世代の待ち・結果は捨てる。

### D1. 部品の共有: 新しい library `libmedia`（要判断 U1）

`userland/desktop/libmedia/`（`/lib/libmedia.so`、header `userland/desktop/libmedia/media.h`。base の desktop の中だけの内部の API、外部の SDK にしない）に、mediafile（demux）・`codec.c`・`codec-layout.h`・`bitstream.c`（libavcodec の dlopen の add-in）・`audio.c`（audiod の client）・`media.c`（再生の engine）を移し、名前の接頭辞を `media_` に揃える。NEEDED は libc だけ（FFmpeg は dlopen）。

- log: `vp_log`（videoplayer の main.c にある）を `media_set_log(void (*)(void *, const char *), void *)` の hook にする（`-z defs` の link を通す）。videoplayer は今の行（OPEN・SEEK・END・CODEC）を同じ文で出し、WS122 の試験の照合を保つ。
- decoder の thread の数は引数（videoplayer は今の 0 = FFmpeg が決める、browser は 2）。
- 変える build: `platform/amd64/vmunix.mk`（libmedia の rule、videoplayer と libbrowser の link と `check-dynamic-elf.py --needed`）、`libbrowser/Makefile` の package の依存、videoplayer の Makefile、WS122 の host 試験（`plan/ws122/tests/run-host-mediafile.sh`・`run-host-codec.sh` の source の path）、`plan/tools/browser-component/run.sh`（host の libbrowser の build）、Linux・FreeBSD の keiland の build（videoplayer がそこにあれば）。
- WS122 の所有の file を動かすので、p002 は WS122 p003 が cleared になってから（WS122 と同じ担当で順に）。
- 代案: libbrowser に直接 compile（library を増やさない。直しは二重）。

### D2. mediafile の source と error

```c
struct mf_source {
	int (*read_at)(void *context, uint64_t offset, void *data, size_t size);	/* 0、ECANCELED（止めた）、EIO（読めない）、EINVAL（範囲の外） */
	uint64_t size;									/* 全体の大きさ（必須） */
	void *context;
};
int mf_open_source(const struct mf_source *source, struct mf_file **file);
```

- `mf_open(path)` は pread の source の wrapper として残す。`struct mf_file` の `fd` を source に、`mf_read_at` は `read_at` を呼ぶ。
- **error の伝え方を直す**（review H2）: `ENODATA` は本当の終わりだけ。`ECANCELED`・`EIO` はそのまま上へ、`EINVAL` は壊れた file。直す所: `mkv.c` の `element_at` の失敗を ENODATA にしない、cues の読みの失敗は ECANCELED・EIO なら「cues 未読」として次の seek で読み直す（EINVAL の時だけ cues 無し）、cues 無しの seek の走査の error を返す。`mp4.c` も同じ点検。media の engine は `mf_read`・`mf_seek` の結果を全部見る（今の `media.c` の「0 以外は EOF」「`(void)mf_seek`」を直す）。
- engine の error の種類: 終わり（ended）・中断（何もしない）・network（D3 の再試行の後 `MEDIA_ERR_NETWORK`）・decode（`MEDIA_ERR_DECODE`）・形式（`MEDIA_ERR_SRC_NOT_SUPPORTED`）。
- MKV の cues 無しの seek（review M6）: 読んだ cluster の位置と時刻を覚え、seek は覚えた所から走査する。cues が無い間の `seekable` は覚えた範囲。
- 大きさの分からない応答（chunked で長さ無し、生放送）、fragment の MP4（MSE・DASH）は `MEDIA_ERR_SRC_NOT_SUPPORTED`。

### D3. network の読み込み（libbrowser の `page/media-fetch.c` と loader）

- **block の cache**（要素ごと、main が持つ）: 256 KiB の block の疎な表、予算 32 MiB、捨てる順は LRU、`read_at` が待っている block と次の 2 block は pin。`read_at`（media の thread）は lock の中で caller の buffer に**複写してから**返す（main が捨てても壊れない）。無い block は要求の queue に積み、wake を書いて condition で待つ。待ちは「世代が変わった・quit」で ECANCELED に解ける。
- **Range の要求**（review H3）: `net_request` に range（first, last）と `no_cache` を持たせ、`loader_prepare`（最初・redirect・keep-alive の再試行のどれでも）が毎回 `Range: bytes=A-B` を付ける。Range の要求は cache を引かず、`If-None-Match` を付けず、cache に入れない。1 本目は 256 KiB、以後 2 MiB、先読みは同時に 2 本まで。
- **206 の検証**: `Content-Range: bytes A-B/T` を解く（`*/T`、`bytes */T` の 416 も）。要求より短い 206 は返った分だけ使い、残りを要求し直す。A が要求と違えば EIO。T が 1 本目と違う、または 1 本目の `ETag`（無ければ `Last-Modified`）と違えば資源が替わったので `MEDIA_ERR_NETWORK`（続きの要求に `If-Range` を付ける）。416 は大きさの外（`read_at` は EINVAL）。
- **Range を無視する server（200）**（review H4）: loader に「header が揃った」callback を足す（`net_http_framing_update` の `headers_done` の時）。200 で `Content-Length` が 64 MiB を超える・長さが無い時は直ちに止めて `MEDIA_ERR_NETWORK`（「この server は Range が無いので再生できない」）。64 MiB 以下は全体を受け、`net_response` の body の buffer を**複写せずに**移して memory の source にする（cache に入れない）。一時の memory は raw と body の 2 つで 128 MiB まで（D11）。要判断 U7（上限の 64 MiB）。
- **失敗と停滞**（review M7）: block の要求の失敗は 3 回まで再試行（1・2・4 秒）、尽きたら `MEDIA_ERR_NETWORK`。要る block が 3 秒来ない間は `stalled` を 1 回、ring が空になったら `waiting`、戻ったら `playing`。
- **`file:`** は file: の文書からだけ（review M14。http(s) の page の file: の media は `MEDIA_ERR_SRC_NOT_SUPPORTED`）。pread の source（`mf_open`）。`data:` は解いた bytes の memory の source。`blob:` と MSE は無い。
- **fetch の mode**（review H6）: view は media が http(s) を要る時に loader を**遅れて作る**（`BROWSER_FETCH_AT_ONCE` の view でも media の分だけ）。`browser_view_poll_fds` は wake の fd を**先頭**に、loader の fd をその後に出す（capacity が尽きても wake は落ちない）。`browser_view_settle` は media の要求の queue を回し、`BROWSER_SETTLE_LAYOUT` の時は D4 の「絵を待つ」段を通る。

### D4. 再生の engine と thread（review H1・M1・M2・M3・M5）

- **open は media の thread で**: main は `media_engine_open(engine, source)` で要求を積んで直ちに返す。mf_open・decoder の open・音の stream の作成は media の thread。結果（metadata か error）は wake で返る。`canPlayType` の codec の load（`pthread_once` の dlopen）は main でよい。
- **wake**: view ごとの pipe 1 本。write の端は `O_NONBLOCK`、engine は「wake 済み」の flag で 1 回にまとめる（pipe が溢れても engine は止まらない）。main は読んで flag を下ろし、各 engine の知らせ（metadata・最初の絵・waiting と playing・終わり・error・seek の完了・寸法の変化）を読む。
- **media の thread の待ち**は全部（ring の空き、BGRA の受け渡し、`read_at`）condition で、quit と世代を見る。p002 の試験: 待ちの各点で close・seek を入れて join が終わる。
- **絵**: AVFrame の参照の ring は 4 枚。BGRA の buffer は 2 枚で、状態（free・writing・ready・shown）と seek の世代を持ち、lock の中で替える。media の thread は次の絵を**表示の寸法**（main が layout の後に知らせる箱の device pixel の大きさ、上限 1920×1080）で BGRA に変換する（sws、bilinear）。描画は 1:1 で写すだけ（review L3: 縮めて aliasing を出さない、変換の無駄も無い）。engine は**表示中の絵の AVFrame の参照を持ち続け**、箱の大きさが変わったら（一時停止中・終わった後・seek の後でも）同じ絵を新しい大きさで変換し直す（review 2 b）。変換し直すまでの間は古い buffer を最近傍で伸ばす。上限を超える箱（4K の窓など）は上限で変換して最近傍で拡大する（制限）。contain の矩形は CPU と GPU で同じ規則で整数の pixel に丸める（左上は floor、大きさは round）。
- **seek**: 世代を進め、ring・buffer・音を捨て、key frame へ戻って目標の時刻まで decode して捨てる。完了は「目標以降の最初の絵が ready」（一時停止中でもその絵を出す）。
- **時計**（review 2 a）: 時計の元は「audiod の stream が動いている（再生中・muted でない・音が尽きていない・audiod が答える）間だけ audiod の位置（`played_position` と `played_time_ns` が埋まっていればそれで device の遅延を除く、無ければ `read_position`）、それ以外は monotonic」。元を替える瞬間（mute・unmute・音が尽きた・audiod を諦めた・stream を作った）に anchor（clock_time と frames・us）を今の時刻で取り直し、時計が跳ばない・止まらないようにする。mediafile は 1 本の cursor で全 track を読むので音だけの seek はできない: **muted の間も音を decode して捨て**、unmute の時は今の時計の位置の標本から stream に書き始める。
- **headless**（`browser_view_settle` の仮想の時計、review 2 c）: engine に外部の時計（`media_engine_set_clock(engine, seconds)`）を与え、settle は仮想の時刻 T へ進む前に「T の絵が ready」（再生していない要素は HAVE_CURRENT_DATA）まで待つ。待ちの上限は **settle の呼び出し全体で 1 つ**（settle の budget、尽きたら `ETIMEDOUT`）。settle の中では engine の知らせを仮想の時刻 T に結びつけた task にし、timer と同じ順（時刻、同じ時刻は timer が先）で送る。settle の最後は layout の後に「表示中の絵が今の箱の大きさで変換し直された」まで待つ段を通る。音は出さない。`BROWSER_DUMP_PAINT` には絵の世代を出さない（dump を決定的に）。
- **再描画**: 期限で絵を替えたら `page->media_generation` を進め、`page_needs_paint` がそれを見る。`browser_view_process` に「layout は要らないが paint が要る」時の redraw を足す。`browser_view_timeout` は次の絵の期限と timeupdate（250 ms）を含め、ms へは**切り上げ**。描かれない video（箱が無い・viewport の外・`display:none`）では世代を進めない（絵は取って時計は進める）。
- 上限: view ごとに再生する要素は 4。5 つ目の `play()` は `NotAllowedError`（制限として記録、review L2 の代案は後）。

### D5. 音（engine が直接 audiod へ）

- libmedia の audiod の client は **engine ごとに 1 本の接続と 1 つの stream**（16-bit stereo 48 kHz）で、「音の track があり、muted でなく、再生中」の間だけ持つ（view あたり最大 4、muted の preview が並ぶ page では接続も stream も使わない）。1 本の接続を要素で共有すると、ある要素の 500 ms の待ちが他の要素の書き込みを止めるので共有しない（review 2 e）。stream の番号は接続ごとに 1。
- audiod とのやりとりは全部 media の thread（main は flag を立てるだけ）。`send` は `MSG_NOSIGNAL`、要求の答えは 500 ms で諦め、**諦めたら接続ごと閉じる**（stream も共有 memory も捨て、遅れて届く答えを受けない）。受け取る予定の無い SCM_RIGHTS の fd は受けたら閉じる（今の `audio.c` の受信は閉じずに漏らす）。audiod が落ちても詰まっても browser は止まらず、音無しで続ける（時計は monotonic へ、anchor を取り直す）。
- `volume` は `AUDIOD_STREAM_VOLUME`。`muted` は接続と stream を閉じる（時計は上のとおり monotonic へ、音の decode は続けて捨てる）。pause で stop、再生で start、seek で flush、要素の破棄・page を離れる・view の破棄で destroy。
- **ABI は変えない**（v2 のまま、struct・export・SONAME 同じ）。ただし `<browser.h>` の説明を更新する（browser-component の規則 §7、review M11）: library が media の thread を作り audiod へ音を出すこと、`browser_view_poll_fds` の先頭の fd は view の wake で loader の物ではないこと（revents をそのまま `browser_view_process` に渡す）、`browser_view_settle` が絵を待つこと。header の説明の変更は実装の Phase（p005）で、docs/ の変更は無い（公開の API の文書は header）。
- 埋め込み側が media を止める口（Settings の窓など）: 要判断 U4。案は export を 1 つ足す `browser_view_set_media(view, flags)`（`BROWSER_MEDIA_NO_AUDIO`・`BROWSER_MEDIA_NO_AUTOPLAY`。v2 のまま追加の export）。

### D6. DOM・layout・描画（review H5・M4・M8）

- `dom_element` に `struct media_element *media`（malloc。review L4 の側の表は後の最適化）。
- **GC で止まらないように**: 要素が「再生中（potentially playing）・取得中・送る event が残る」間は、page の media の一覧が wrapper を root する。止まったら外す。finalize は engine の要素への参照を切り、engine を page の「片付け」の列に移すだけで、join は次の `browser_view_process`・`browser_view_settle`（または page の破棄）で行う（GC の中で join しない）。「送る event が残る」は engine の知らせの queue が空でないこと。
- 文書から外れた時: microtask の checkpoint の後もまだ外れていれば pause（同じ task の中の付け替えは止めない）。
- UA の規則: `video` は object-fit が無いので描画で contain を固定。`audio:not([controls]) { display: none }`、`audio[controls]` は D9 の U5 まで 300×54 の空の箱。
- layout: VIDEO を replaced。固有の寸法は videoWidth×videoHeight（metadata の後）、無ければ poster、無ければ 300×150。寸法が変わるたび（H.264 の SPS の変化も）`resize` と layout の世代。回転・pasp・BT.709・10 bit・HDR は未対応（review L1: 制限として記録。sws の既定は BT.601、縦の動画の回転は無視）。
- poster: `page/images.c` の walk に `<video poster>`。絵が出るまで poster（無ければ透明）。
- display list に `PAINT_VIDEO`（engine の process 内で一意の serial、表示中の buffer の世代、箱、contain の矩形）。
  - CPU（`software.c`）: buffer を 1:1 で写す（D4 の表示の寸法）。
  - GPU（`vulkan.c`）: atlas を使わず video ごとの `VkImage`（B8G8R8A8、optimal）と descriptor set（pool の `maxSets` を増やす）。今の shader（`texelFetch`、最近傍）を 1:1 で使い、shader は変えない。描く順を保つため video の item の前後で draw を分け、set を付け替える。staging の buffer からの copy と layout の遷移は render pass の前に記録。破棄は record の中の `paint_gpu_prepare`（契約上、前の work が終わった後）でだけ、`paint_gpu_close`・`browser_view_set_gpu(NULL)`・device lost でも解放。key は serial（page・文書をまたいで衝突しない）。

### D7. JS の API と状態（review M8・M9）

- **HTMLMediaElement**: `src`・`currentSrc`・`crossOrigin`（反映）・`networkState`・`preload`（`none` は play・load まで取らない、他は metadata まで）・`buffered`・`load()`・`canPlayType()`・`readyState`・`seeking`・`currentTime`・`duration`（metadata まで NaN、長さの無い MKV は `+Infinity`）・`paused`・`defaultPlaybackRate`・`playbackRate`（1.0 以外は値だけ、速さは 1.0、制限）・`played`・`seekable`・`ended`・`autoplay`・`loop`・`play()`・`pause()`・`controls`・`volume`・`muted`・`defaultMuted`、定数。**HTMLVideoElement**: `width`・`height`・`videoWidth`・`videoHeight`・`poster`・`playsInline`（反映）。**MediaError**、**TimeRanges**（`buffered` は block の cache の byte の範囲を、MP4 は sample の表・MKV は覚えた cluster で時刻に換算。換算できない時は metadata の後の 0..currentTime）。
- **資源の選択の起動**: `src` の設定、`src` の無い要素への `<source>` の挿入、文書への挿入（`networkState` が EMPTY の時）、`load()`。属性の変化の hook を `dom/attribute.c` に足す（media の要素の `src` だけ）。
- **候補**: `src` があればそれだけ。無ければ子の `<source>` を順に、`type` が `canPlayType` で空なら飛ばし、開くのに失敗したら `<source>` に `error` を送って次へ。尽きたら `networkState` NO_SOURCE で待つ（後の `<source>` の挿入で続ける）。
- **状態と event の順**（p005 の host の JS 試験の期待にする）:

| 段 | readyState・networkState | event（順） |
| --- | --- | --- |
| 選択の始まり | HAVE_NOTHING・LOADING | `loadstart` |
| metadata | HAVE_METADATA | `durationchange` → `resize`（video）→ `loadedmetadata` |
| 最初の絵 | HAVE_CURRENT_DATA | `loadeddata` |
| 先がある | HAVE_FUTURE_DATA → HAVE_ENOUGH_DATA | `canplay` →（再生中なら `playing`）→ `canplaythrough` |
| 取得の進み・休み | — | `progress`（350 ms ごとまで）・`suspend`（preload の分を取り終えた） |
| `play()` | paused=false | `play` →（HAVE_FUTURE_DATA 以上なら）`playing`、未満なら `waiting` |
| `pause()` | paused=true | `timeupdate` → `pause` |
| seek | seeking=true | `seeking` →（完了で）`timeupdate` → `seeked` |
| 終わり（loop） | — | 先頭へ seek（`seeking`・`seeked`） |
| 終わり（loop で無い） | ended=true、paused=true | `timeupdate` → `pause` → `ended` |
| error | NETWORK_IDLE か NO_SOURCE | `error`（`<source>` の失敗は `<source>` に） |
| `load()`（途中） | HAVE_NOTHING・EMPTY | 未決の play の Promise を `AbortError`、`abort`・`emptied` |

- **`play()` の Promise**: HAVE_FUTURE_DATA 以上で再生が始まった時に resolve（既に再生中なら次の task で resolve）。`pause()`・`load()`・終わり（loop で無い）で未決の物を `AbortError`、error で `NotSupportedError`、自動再生の規則（D8）・上限（D4）で `NotAllowedError`。
- **`canPlayType`**: mediafile の container の表（video/mp4・audio/mp4・video/webm・audio/webm・video/x-matroska）と codec の表（mediafile が知り、かつ libavcodec に decoder がある物）の両方に合う時だけ `"maybe"`（`codecs=` まで合えば `"probably"`）。`audio/mpeg`・`audio/ogg`・`audio/wav`・Vorbis は `""`（mediafile に無い）。libavcodec が無ければ常に `""`。
- 範囲の外（後）: MSE、EME、WebVTT（`<track>`）、`requestVideoFrameCallback`、Picture-in-Picture、Fullscreen API、`captureStream`、canvas の `drawImage(video)`、HLS・DASH、速さの変更。

### D8. 自動再生（要判断 U2）

- 推奨: `autoplay` と user の操作の無い `play()` は muted の時だけ許す（音つきは `NotAllowedError`）。user の操作（click・key・touch の event の中、またはその後 5 秒）の中の `play()` は音つきで許す。

### D9. 範囲（要判断 U3・U5）

- U3 `<audio>`: engine は video の track が無くても再生する（今の player の engine は video の track が要るので直す）。**対象は MP4（M4A、AAC）と WebM・Matroska（Opus）の音だけ**（review M9）。Web の `<audio>` に多い MP3・Ogg（Vorbis・Opus）・WAV・FLAC は mediafile に demux が無く、足すのは別の量（各 container の reader、Vorbis の codec の表）なので Future Work の候補にする。
- U5 `controls`: 最小の操作の帯（再生・一時停止、seek の bar、時刻、mute）を engine が描くか。

### D10. GPU の decode（WS083）を後で足す境界

- libmedia の decoder は ops の表（open・send・receive・flush・close）にし、software と vulkan-video を同じ形で選ぶ。絵の型 `struct media_picture { kind; ... }` は今は `MEDIA_PICTURE_BGRA` だけ、後で `MEDIA_PICTURE_VK_IMAGE`（NV12、YCbCr の変換を shader で）。
- Vulkan Video は decode の queue family が要る。browser の device は埋め込み側が貸す（`struct browser_gpu`）ので、その時に `browser_gpu` の拡張（API の版を上げる）が要る。WS083 の後に設計する。

### D11. 安全と資源

- demux・decode は page の data を同じ process で読む（WS074 の D1、sandbox は後）。
- memory の上限: block の cache 32 MiB、Range の無い server の全体 64 MiB（受ける間の一時は 128 MiB）、mediafile の packet 64 MiB・moov 64 MiB、AVFrame の ring 4 枚、BGRA 2 枚（1080p で 16 MiB）。固有の寸法の上限 4096×2304（超えたら `MEDIA_ERR_SRC_NOT_SUPPORTED`）。
- 片付けの順: page を離れる・view の破棄で、全 engine の世代を進めて quit（`read_at` の待ちは ECANCELED、audiod の待ちは 500 ms で解ける）、join、それから DOM と cache を解放。iframe の子の文書の media も同じ一覧で（子の文書の破棄で止める）。

## Phase の分割（案、ws.md に投影）

| Phase | 内容 | 依存 | 確かめ |
| --- | --- | --- | --- |
| p002 | libmedia（D1・D2・D4 の engine: source、error の伝え方、async の open、audio だけの再生、世代と待ち、BGRA の 2 枚と表示の寸法、外部の時計、wake、decoder の ops、log の hook、音の改善 D5）、videoplayer をそれに移す | p001、WS122 p003・p004 cleared | host（WS122 の mediafile・codec の試験を libmedia で、reader の source、error の種類、待ちの各点での close・seek と join、外部の時計）。T1: videoplayer の回帰（WS122 p004 の試験） |
| p003 | loader の Range（D3: range・no_cache・206・Content-Range・416・If-Range・header の callback・Range の無い 200）と `page/media-fetch.c`（block の cache、file・data の source、遅れて作る loader、wake の fd を先頭に） | p002 | host（python の server: Range、Range を無視、短い 206、redirect、keep-alive の切断と再試行、途中の資源の差し替え、中断） |
| p004 | DOM・layout・描画（replaced、poster、`PAINT_VIDEO`、CPU と GPU の 1:1、paint だけの世代、GC の root、片付け）と最小の再生の glue（`autoplay muted` だけ、settle が絵を待つ） | p002・p003 | host の headless（`file:` の試験の素材、仮想の時刻ごとの絵を許容つきで比べる）、`plan/tools/browser-component`（lavapipe）で GPU と CPU の一致 |
| p005 | JS の API と状態の表（D7）、event loop（wake・timeout・timeupdate）、自動再生（D8）、`<audio>`（U3）、`<browser.h>` の説明の更新（U4 なら export） | p004 | host の JS の試験（event の順、play の Promise、seek、`load()` の中断、`<source>` の fallback、文書から外す） |
| p006 | 音（D5）の結線と（U5 なら）操作の帯 | p005 | T1（QEMU）: guest の browser で host の test server（QEMU の user network の 10.0.2.2）の page の video を再生、時刻ごとの QMP の PNG、audiod の stream の状態は SSH で問う（serial・console の log は使わない）。素材は小さい（320×180、数秒） |
| 最後 | 全文規約と回帰 | 全部 | — |

試験の素材: host の ffmpeg の `lavfi`（testsrc・sine）で作る自作の小さな file（H.264+AAC の MP4、moov が後ろの MP4、VP9+Opus の WebM、cues の無い WebM、AAC だけの M4A。各 200 KiB 以下）を `plan/ws121/tests/` に commit する（WS122 の `sample.mp4` と同じ扱い。自作なので license の問題は無い）。image は `plan/ws121/tests/` の config.mk と個別の複写だけで作る（2026-10-04 ユーザーの規則）。headless の比較は単色・縞の pattern で、channel の差の許容つき（host の FFmpeg 7 と image の FFmpeg 9、sws の SIMD の差）。

## 要判断（ユーザーへ）

- U0: WS121 の目標は「Vulkan Video の hardware decode で再生」（ws.md）。2026-10-05 の方針（まず software decode、GPU は後で同じ境界の後ろ）に合わせ、**WS121 の完了の条件を「libavcodec の software decode で `<video>` が再生できる」にし、GPU の decode は WS083 の後の Phase（または別の WS）にする**でよいか。
- U1: 部品の共有を新しい library `libmedia.so` にするか（推奨）、libbrowser に直接 compile するか。
- U2: 自動再生の規則（推奨: muted だけ自動、音つきは user の操作の後）。
- U3: `<audio>` を入れるか、入れるなら対象を MP4（AAC）・WebM（Opus）に限ってよいか（MP3・Ogg・WAV・FLAC は後）。
- U4: 埋め込み側が media（音・自動再生）を止める export を足すか（v2 のまま追加）。足さなければ今の利用者（browser・browser-probe）には要らない。
- U5: `controls` の標準の操作の帯を作るか（推奨: 最小の帯を p006 で）。
- U7: Range に応えない server の動画を全体で受ける上限（推奨 64 MiB、超えたら再生できないと出す）。
- 参考（判断ではない）: MSE が無いので、YouTube など MSE で配る site は再生できない。対象は普通の `<video src=".mp4">`・`.webm` の page。

## 確かめ

- 設計だけ。code の変更・build・試験は無い。
- design-reviewer の review: 1 回目（2026-10-05 夜）は高 6・中 14・低 8、第 2 版で下の表のとおり反映。
- 2 回目（2026-10-05 夜、第 2 版 3df53896）: **条件付きで p002 へ進める**。p002 の前の条件は a（時計の元と mute・unmute の anchor）・b（表示中の AVFrame を持って変換し直す、settle の最後の段）・c（settle 全体の待ちの上限と event の順）・e（接続を engine ごとに、timeout の後の扱い、fd の漏れ）で、**第 3 版で反映した**（D4・D5・D6）。p003・p005 の設計の中で直す残り:
  - p003: header の callback が自分を cancel した時は直後に `cancelled` を見て `EAGAIN` で戻る、3xx の header では呼ばない、body を移す API（`net_request_take_body`）、raw の buffer の realloc で一時に約 192 MiB になりうる（D11 の 128 MiB は下限）。遅れて作る loader を page の media-fetch に渡す道、`browser.h` の AT_ONCE の説明、settle の loop の終わりの条件（media の要求と engine の知らせが空）と wake の fd の poll。wake の fd の契約の説明は p005 でなく p003 に同梱。If-Range は弱い ETag（W/）なら Last-Modified。
  - p004: descriptor の pool が足りない時の作り直し（または描く video の数の上限）。
  - p005: 状態と event の表の抜け（自動再生の行、再生中に data が尽きた行、`volumechange`・`ratechange`・後からの `durationchange`、HAVE_ENOUGH_DATA の基準、`play()` の `NotSupportedError` は source を使えなかった時だけか、metadata の行の `resize` の条件を仕様の原文で確かめる）。
  - p002: `mediafile.c` の先頭の pread も source を通すこと。
  - p006: audiod の stream の状態を SSH で問う guest の道具が無ければ範囲に含める。

## 未実施

- 実装（p002 以降）、QEMU・実機。

## review 1 回目の反映（第 2 版）

| 指摘 | 反映 |
| --- | --- |
| H1 open が main で deadlock | D0・D4: open は media の thread、main は `read_at` を呼ばない（p002 の受け入れ条件） |
| H2 error が ended に | D2: ENODATA は終わりだけ、mkv・mp4・media.c の直し、engine の error の種類 |
| H3 loader で Range が落ちる | D3: range と no_cache を request に、毎回 Range、cache を引かない・入れない、206・416・If-Range |
| H4 Range の無い 200 | D3: header の callback で大きい物を打ち切る、64 MiB 以下は body を移す、U7 |
| H5 GC で止まる | D6: 再生中・取得中・event の残る間 root、finalize は片付けの列へ |
| H6 AT_ONCE に loader が無い | D3: loader を遅れて作る、wake を poll の先頭に、settle に media の段 |
| M1 thread の間の共有 | D3（複写して返す・pin・LRU）、D4（世代・O_NONBLOCK・待ちの規則と試験） |
| M2 headless の決定性 | D4（外部の時計、dump に世代を出さない）、試験の素材と許容 |
| M3 paint だけの redraw | D4（`browser_view_process` に paint の判定、切り上げ、描かれない video） |
| M4 GPU の texture | D6（1:1 で shader を変えない、set の付け替え、copy の位置、破棄の場所、serial の key） |
| M5 BGRA の受け渡し・一時停止中の seek | D4（buffer の状態と世代、seek の完了の定義） |
| M6 network の費用 | D2（覚えた cluster）、D3（LRU と pin） |
| M7 network の失敗・停滞 | D3（再試行・stalled・waiting） |
| M8 仕様からの外れ | D6（文書から外れた時）、D7（選択の起動・候補の fallback・状態と event の表・終わり・寸法の変化） |
| M9 `<audio>` と canPlayType | D7（mediafile の表と突き合わせ）、D9・U3（MP4・WebM の音だけ） |
| M10 audiod | D4（音が尽きたら monotonic、played_position）、D5（media の thread だけ、MSG_NOSIGNAL、500 ms、遅れて作る stream、view に 1 接続） |
| M11 browser.h | D5（ABI 同じ、header の説明を p005 で更新、wake を先頭）、U4 |
| M12 Phase の依存・目標 | Phase の表（p002 ← WS122 p003、p003 ← p002、p004 ← p002・p003 と最小の glue）、U0、試験の素材の置き場所 |
| M13 libmedia の build | D1（log の hook、threads の引数、変える build の一覧） |
| M14 file: の media | D3（file: の文書からだけ） |
| L1〜L8 | L1 は D6 の制限、L2 は D4 の制限、L3 は D4 の表示の寸法、L4 は D6 の後の最適化、L5 は D7 の換算、L6 は D11 の iframe、L7 は p006 の確かめ、L8 は 1:1 の写しで filtering の差が無い |

## design-reviewer の 1 回目の指摘（2026-10-05 夜、第 2 版で反映）

高（改訂で必ず直す）:
- H1: 今の `vp_media_open` は呼び出し側の thread で `mf_open` する（`videoplayer/media.c` の open の後に thread を作る）。D3 の `read_at` は main の thread が Range を出すまで待つので、main で open すると deadlock。→ open は要求を積んで返し、mf_open と decoder の open は media の thread で、結果は wake で。
- H2: mediafile と media.c が error を終端にまとめる（`mkv.c` の element_at の失敗を ENODATA に、cues の読みの失敗で黙って cues 無し、`media.c` は mf_read の 0 以外を全部 EOF、`mf_seek` の結果を捨てる）。network の失敗・ECANCELED で `ended` が出る。→ 終端・中断・network・decode を分けて伝える（mediafile は WS122 の部品なので調整）。
- H3: loader の cache 引き・redirect・keep-alive の再試行・304 の経路で Range が落ちる（`loader_begin`・`loader_prepare`）。→ request に range と「cache を使わない」を持たせ毎回 Range を付ける、Content-Range の検証、短い 206、416、If-Range（ETag）で途中の差し替えを検出。
- H4: Range を無視する server の 200 は raw・body・cache の複写で数百 MiB、全部を受けるまで始まらない。→ header が揃った時の callback を足し、大きい 200 は打ち切る（または streaming の受信）、上限と見積もりを D11 に。
- H5: 再生中の要素が GC で回収されて止まる（`new Audio(url).play()`）。→ 再生中・取得中・event の残る間は page の media の一覧から root、finalize は解放だけ、join は page の破棄の順で。
- H6: headless の既定（`BROWSER_FETCH_AT_ONCE`）は loader を作らず、settle は media の要求を回さない（`view/view.c`）。→ fetch の mode ごとの扱い、wake の fd を loader と無関係に出す、settle に media の段。

中（M1〜M14）: thread の間の block の pin と中断の世代・wake の pipe の non-blocking（M1）、headless の決定性の外部時計・試験の素材の許容（M2）、`browser_view_process` に paint だけの redraw の判定と期限の切り上げ・描かれない video は世代を進めない（M3）、GPU の専用 texture の descriptor・draw の分割・copy の位置・破棄は record の中の prepare・key は process で一意・shader の作り直し（M4）、BGRA の buffer の状態と一時停止中の seek の絵（M5）、cues の無い MKV と interleave の悪い MP4 の network の費用・LRU（M6）、再生中の network の失敗・stalled・waiting（M7）、資源の選択の起動の時機・source の fallback・文書から外れた時の stable state・状態と event の表・終端の手順・寸法の変化（M8）、`<audio>` は mp3・ogg・wav を demux できず Vorbis も無い、canPlayType を mediafile の表と突き合わせる（M9）、audiod の client の MSG_NOSIGNAL・timeout・遅延の stream の作成・音が先に尽きた時の時計（M10）、`<browser.h>` の ABI は変えなくても header の説明（poll・timer・音・thread）を更新する・wake の fd を先頭に（M11）、Phase の依存（p002 ← WS122 p003・p004、p003 ← p002）・WS121 の目標「Vulkan Video の hardware decode」との差の判断・試験の素材の置き場所（M12）、libmedia の `vp_log` の未定義・threads の引数・package と host の build（M13）、http(s) の page からの file: の media の禁止（M14）。

低（L1〜L8）: 回転・pasp・BT.709・10 bit の制限の記録、5 つ目の play の扱い、表示の寸法での変換、`dom_element` の pointer、buffered・seekable の換算と Duration の無い MKV、iframe の media、p006 の試験の判定（serial の log を使わない、guest から host の server、小さい素材）、GPU と CPU の一致の許容。

