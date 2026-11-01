<!-- awesome-plan project=zedbsd record=ws322-p007b -->

# ws322-p007b JVT の残り 26 本

- 親: [WS322](../ws.md)。2026-11-02 03:45 Q1 が p007 の結果から作成。
- Status: cleared（2026-11-02 06:09 Q1、main で p004/run.sh PASS。VF2 で JVT 100/101 一致・1 本は Extended profile を理由付きで断る、libmedia も 101 本一致）。前: cleared（2026-11-02 05:39 P2 q1907-i01。Q1 の受け入れ待ち）
- 依存: p007。

## 目的

p007 の JVT（H.264 の適合の stream 101 本）で全部の絵が一致しなかった 26 本の原因を突き止めて直す。

- vkvideo-probe の parser・DPB の planner で止まった 13 本（Extended profile、JVC_C、TANDBERG ほか）: Extended profile のように VPU が扱わない物は、誤りとして正しく断る事を確かめる（受け入れ）。扱える物は probe の側を直す。
- 絵が違う 13 本（重み付き予測の CAWP1・CVWP1〜3_TOSHIBA_E、CACQP3_Sony_D、CABA3_TOSHIBA_E、FRExt1/3_Panasonic、Sony の "_3" 5 本（300 枚中 5 枚）、CVSE3_Sony_H、MV1_BRCM_D）: 原因（parameter set の書き戻し、重み付き予測の表、DPB、表示順、参照の作り）を切り分け、driver・libvulkan・probe の側で直す。
- （2026-11-02 Q1 の追加）製品の道は libmedia と Video Player なので、libmedia の H.264 の parser・DPB に同じ限りが無いかを確かめ、あれば直す。JVT の stream を media-probe・Video Player の確かめにも入れる。

## 受け入れ

- 26 本それぞれが、全部の絵で一致するか、扱わない理由が明示されて誤りとして断られる。
- p007 の回帰（run-vf2.sh・run-vf2-play.sh）と host 試験（p004/run.sh）が前と同じ。
- 板を元に戻し cksum が一致する。

## 結果（2026-11-02 P2、q1907-i01）

### 切り分け（host）

- host の道具 `plan/ws322/tests/p007b/`: `jvt-host.c`（vkvideo-probe の h264.c・dpb.c で読み、engine の put_picture と同じに vpu-h264-ps.c で SPS・PPS を書き戻した bytes を出す）、`host-check.py`（その bytes を ffmpeg で decode して参照と照合し、probe の表示順を真似て板の結果を予想する）、`libmedia-host.c`・`libmedia-check.sh`（libmedia の h264.c・h264-dpb.c で同じ事）、`convert.c`（StdVideo から vpu-codec.h の形へ、vk-video.c の読みと同じ）。
- 直す前の host の予想は p007 の板の不一致の数と 13 本とも同じ数だった。firmware・engine・libvulkan・書き戻しの誤りは無く、全部 vkvideo-probe の側だった:

| 原因 | stream |
| --- | --- |
| 表示順: 待ちの 256 枚が満ちると全部を出していた（境目の前後が入れ替わる） | CABA3_Sony_C・CABACI3_Sony_B・CANL3_Sony_C・CAPM3_Sony_D・CVBS3_Sony_C（各 5 枚）、CVSE3_Sony_H（9 枚）、MV1_BRCM_D（2 枚） |
| 同じ id の PPS を stream の途中で別の中身で送り直す（probe は始めに全部の set を読み、id ごとに最後の物だけを持っていた） | CAWP1・CVWP2・CVWP3_TOSHIBA_E（重み付き予測の有無）、CACQP3_Sony_D（chroma の QP の offset が絵ごとに変わる）、CABA3_TOSHIBA_E、FRExt1・FRExt3_Panasonic。CVWP1_TOSHIBA_E は違う PPS で slice header を読み違えて止まった |
| 順の数の type 1 を読まなかった | BAMQ1・BAMQ2・NLMQ1・NLMQ2_JVC_C、LS_SVA_D、MR1_BT_A、MR5_TANDBERG_C |
| memory management operation 5 を断っていた | MR2_TANDBERG_E、MR4_TANDBERG_C |
| frame_num の gap を断っていた（SPS は gap を許す） | MR3_TANDBERG_B |
| Extended profile（profile_idc 88、constraint_set1 = 1 で Main にも従う stream） | BA3_SVA_C。VF2 の libvulkan と kernel は profile 66・77・100 だけを受ける（Wave511 は Extended を持たない） |

### 直し

- vkvideo-probe（`userland/tests/vkvideo-probe/`）:
  - parameter set を stream の順に読む。h264_open は stream 全体の要る物（最初の SPS の profile、最大の大きさ、最多の参照）だけを集めて set を空に戻し、h264_next_picture が絵の前の SPS・PPS をその場で持つ。unit の長さと FNV-1a の hash が変わったら generation を上げ、probe は次の絵の前に VkVideoSessionParameters を作り直す（Vulkan は既にある id の更新を許さない）。
  - 表示順: 待ちが満ちたら小さい順の半分だけを出す（DPB の bumping と同じ）。IDR と operation 5 の絵の前に全部を出す。並べる鍵は top と bottom の小さい方。
  - 順の数の type 1（8.2.1.2）、operation 5（後の絵から見た順の数と frame_num 0、DPB の全部の参照を外す）、frame_num の gap（8.2.5.2: 番号ごとに sliding window の後に絵の無い short-term の frame を marking だけに置く。decoder に渡す参照には入れない）。
  - decoder が受けない profile は `refused: the decoder does not take the stream's H.264 profile (profile_idc N)` を出して exit 6（PROBE_REFUSED）で断る。
- libmedia（`userland/desktop/libmedia/`、Q1 の追加の範囲）: 始めから set を順に読み、generation で parameters を作り直し、bumping・type 1・operation 5・gap の推定を持っていた。板の media-probe で見つかった足りない 3 つを直した:
  - `h264-dpb.c` の h264_lists_admit: slice の参照の list の有効な範囲に gap で推定した frame があると絵を落としていた。推定の frame（8.2.5.2 で予測に使われない）と list の外の index は比べず、他の index だけ device の list と同じ絵を選ぶ事を確かめる。直す前は MR3・MR4・MR5_TANDBERG で 300 枚中約 264 枚を落とし、落ちた絵を参照する絵が続けて落ちた。
  - `h264.c` の h264_reorder_depth: VUI の無い Baseline を並べ替え 0 としていた。Baseline でも順の数の type 0・1 は並べ替えられる（HCBP1・HCBP2_HHI_A、MR4・MR5_TANDBERG_C は P だけで表示順が復号順と違う）。E.2.1 のとおり level の DPB の枚数にし、type 2（表示順が復号順）だけを 0 にした。
  - `vkvideo.c`: 表示の順を待つ絵が、新しい submission から 32 以上遅れたら、その場で完了させる（`VKVIDEO_SETTLE_BEHIND`）。runtime は最後の 64 の ticket の結果しか持たず、表示順の待ちは並べ替えの深さの枚数より長くなりうる（その絵より前の絵が全部その間に追い越す）。上の直しで MR4・MR5 が深さを持つと、約 60 の decode を待つ絵の結果が上書きされ、130 枚で ECANCELED で止まった。

### 確かめた事（host・build）

| 項目 | 結果 |
| --- | --- |
| probe の host（101 本、`host-check.py`） | 101 本とも ring の bytes の decode が参照と全部一致、probe の表示順の予想も 101 本とも全部一致 |
| libmedia の host（101 本、`libmedia-check.sh`） | 直した後: BA3_SVA_C 以外の 100 本が全部の絵を decode し参照と一致（BA3_SVA_C は libmedia の h264_open が断る）。直す前: MR3・MR4・MR5 が 36〜37 枚 |
| host 試験 | `sh plan/ws322/tests/p004/run.sh build/tmp/p007b-host-p004`: ps-slots 4094・engine 56・wire 33・nv12 state 69・executor 505・nv12 752、全部 0 failed（p007 と同じ数） |
| build | `config-vf2-video-test.mk`（build/ws322-p007t、vmunix・wayland・files・terminal・keiland-shot・vkmatrix・vkcoherency・vkcompute・media-probe・vkvideo-probe・videoplayer）warning 0 |

### 確かめた事（VF2 実機 10.0.30.2）

`VF2_HOST=kei@10.0.30.2 VF2_PLUG=AC276E432AE6 sh plan/ws322/tests/p007b/run-vf2-jvt.sh OUT build/ws322-p007t build/tmp/p007-jvt build/tmp/vpu-fw`（kernel は p007 と同じ cksum 2968315243）。

| 回 | 内容 | 結果 |
| --- | --- | --- |
| 1（04:20〜04:27、`build/tmp/p007b-vf2-r1`） | probe を直した後の J 101 本、media-probe 7 本、Video Player 1 本 | J: 100 本が全部の絵で一致、BA3_SVA_C は exit 6 で断った（理由の行あり）。engine の誤り 0・立て直し 0。M: CAPM3・CAWP1・CACQP3・BAMQ1・MR2・CVSE3 が全部一致、MR3_TANDBERG_B は 300 枚中 37 枚（libmedia の gap の落とし） |
| 2（04:32〜04:44、`build/tmp/p007b-vf2-r2`） | libmedia の list の直しの後、J 101 本、media-probe 101 本 | J は 1 回目と同じ。M: 97 本が全部一致（BA3_SVA_C は libmedia が software の h264sw に移して 33 枚一致）。HCBP1・HCBP2_HHI_A・MR4・MR5_TANDBERG_C は絵は全部参照の集合にあり順だけ違った（VUI の無い Baseline の並べ替え 0） |
| 3（04:46〜04:58、`build/tmp/p007b-vf2-r3`） | 並べ替えの直しの後、J 101 本、media-probe 101 本、Video Player HCBP1_HHI_A | J は同じ。M: HCBP1・HCBP2 が全部一致、99 本が一致。MR4・MR5_TANDBERG_C は 130 枚で ECANCELED（上の 3 つ目） |
| 4（05:00〜05:11、`build/tmp/p007b-vf2-r4`） | vkvideo.c の直しの後、J 101 本、media-probe 101 本、Video Player MR4_TANDBERG_C | J: 100 本が全部の絵で一致、BA3_SVA_C は exit 6 で断る。M: 101 本とも全部の絵が参照と順に一致（100 本は vulkan-video、BA3_SVA_C は h264-software）。VP: 誤り 0・compositor の誤り 0、読み戻しの道で 12 s に 147 枚（下の未実施） |

- p007 の回帰（同じ build、kernel 2968315243）:
  - `run-vf2.sh build/tmp/p007b-reg …`（05:12〜05:35）: vf2-regress（vkmatrix 109/109、`-p 3` PASS、`-r 3` 327/327、vkcoherency PASS、vkcompute 4096/4096、desktop の compositor error 0、keiland-shot の PNG cksum 518389155 は p007 と同じ）。S1 1080p30 平均 13.2 ms・1080p60 12.4 ms・2160p30 42.4 ms、1080p60 120 枚・2160p30 30 枚が一致。F1 240 枚一致（flush 4）・600 枚で flush 10。F2 は 1080p60 120/120、1080p30 119/120（p007 と同じ frame 119 の切れ目）。F3 は立て直し 1、誤り 51・111 の後に次の IDR から一致、直後の 60 枚一致。F4 ×1.0 は誤り 269・立て直し 5、×1.05〜1.25 は誤り 0。F6 の SIGKILL の後 15 枚一致。V media-probe 1080p30 600/600 一致。VP 1080p30 16 s に 481 枚・late 1、1080p60 930 枚・late 32、compositor の誤り 0。J は 100/101（BA3_SVA_C だけ、理由を出して断る）。F5 は 3 回で止まり、以後 session は「opened no stream (5)」、vkcompute PASS。p007 と同じ形（J だけ 75 → 100）。
  - `run-vf2-play.sh build/tmp/p007b-play …`（05:35〜05:38）: Video Player 1080p60 の 20 s を 2 回、1179 枚・late 21、1181 枚・late 19（late は始めの約 3 s だけ）、engine の誤り 0。p007（1153・late 47、1182・late 18）と同じ形。
- 板の後始末（全部の回）: `/boot/boot0/vmunix` 3758577736、`/etc/group` 4260090434、`zedbsd.cfg` 632416842、`/lib/firmware/wave511` は無い、/tmp は空（p007 と同じ）。

### 26 本ごとの結果

| stream | p007 | p007b（vkvideo-probe、板） | 原因と直し |
| --- | --- | --- | --- |
| CAWP1_TOSHIBA_E | 90 中 9 | 90/90 | PPS の送り直し、probe |
| CVWP1_TOSHIBA_E | 止まった（3 枚） | 90/90 | 同上 |
| CVWP2_TOSHIBA_E | 90 中 6 | 90/90 | 同上 |
| CVWP3_TOSHIBA_E | 90 中 6 | 90/90 | 同上 |
| CACQP3_Sony_D | 50 中 0 | 50/50 | 同上 |
| CABA3_TOSHIBA_E | 300 中 0 | 300/300 | 同上 |
| FRExt1_Panasonic_D | 8 中 3 | 8/8 | 同上 |
| FRExt3_Panasonic_E | 11 中 0 | 11/11 | 同上 |
| CABA3_Sony_C・CABACI3_Sony_B・CANL3_Sony_C・CAPM3_Sony_D・CVBS3_Sony_C | 各 300 中 295 | 各 300/300 | 表示順の 256 の境目、probe |
| CVSE3_Sony_H | 278 中 269 | 278/278 | 同上 |
| MV1_BRCM_D | 257 中 255 | 257/257 | 同上 |
| BAMQ1・BAMQ2・NLMQ1・NLMQ2_JVC_C | 止まった（0 枚） | 各 30/30 | 順の数の type 1、probe |
| LS_SVA_D | 止まった | 1700/1700 | 同上 |
| MR1_BT_A | 止まった | 62/62 | 同上 |
| MR5_TANDBERG_C | 止まった | 300/300 | 同上 |
| MR2_TANDBERG_E | 止まった（26 枚） | 300/300 | operation 5、probe |
| MR4_TANDBERG_C | 止まった（14 枚） | 300/300 | 同上 |
| MR3_TANDBERG_B | 止まった（9 枚） | 300/300 | frame_num の gap、probe（libmedia の list の比べも直した） |
| BA3_SVA_C | 止まった（理由の行なし） | 断る（exit 6、`refused: … profile_idc 88`） | Extended profile は VF2 の decoder が受けない |

- libmedia（media-probe、板の 4 回目）: 101 本とも全部の絵が参照と順に一致。直す前（1・2 回目）は MR3・MR4・MR5_TANDBERG（gap の list の比べ）、HCBP1・HCBP2_HHI_A・MR4・MR5（Baseline の並べ替え 0）が違った。

## 判断（範囲の中の技術）

- BA3_SVA_C は constraint_set1_flag = 1 で Main にも従うので、Main の session で decode できる見込みはあるが、probe と libmedia は SPS の profile_idc のまま session を作り、VF2 の decoder（libvulkan・kernel・Wave511）は Extended を断る。受け入れのとおり、理由を出して断る形にした。libmedia はこの時 h264sw（software）に移り、参照と一致した。
- operation 5 の絵の kernel の行 `order count differs (0)`: firmware は operation 5 の後の順の数（0）を返し、Vulkan の PicOrderCnt は前の値なので engine の比べが行を出す。絵は一致し、行だけの事なので直していない（MR2・MR3・MR4 で 7 行、行の上限は 32）。
- gap で推定した frame は Vulkan の参照（`is_non_existing`）として渡さず、marking の中だけに置く。Vulkan では参照の slot は以前の decode で有効にされた物でなければならず、絵の無い frame の slot を作れないため。Wave5 は firmware が bitstream から gap を自分で扱う。

## 未実施・残り

- Video Player の小さい stream（QCIF・CIF、VUI なし）: decode は誤り 0 だが「29 slots for pictures the GPU holds, more than 17; pictures are read back」で読み戻しの道に移り、1 枚約 74 ms の描画で約 12 fps、全部の絵が late（CAPM3_Sony_D・MR3_TANDBERG_B・HCBP1_HHI_A・MR4_TANDBERG_C、12 s に 122〜150 枚）。Video Player の読み戻しの道（CPU の拡大）の速さで、decode の誤りではない。直していない（Video Player の側）。並べ替えの直しで VUI の無い Baseline も level の DPB の枚数を持つので、同じ道に移る stream が増える。
- media-probe の MP4 は ffmpeg の `-c copy` で作り、生の H.264 の時刻は復号順のまま（並べ替えの有る stream で）。libmedia は自分で順の数で並べるので照合は表示順で行えた。
- libmedia の他の backend（RTX・Intel・RPi4）での JVT の照合は未実施（list の比べの直しは device の list に gap の frame を渡さない前提で、どの driver でも同じ）。
