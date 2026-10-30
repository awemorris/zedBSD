<!-- awesome-plan project=zedbsd record=ws318-design-review -->

# WS318 p001 design-reviewer の記録（2026-10-31）

- 対象: [design.md](design.md)・[proposed/](proposed/)・[user-questions.md](user-questions.md)・[ws.md](ws.md) の初稿（commit 289588e92）。
- レビュー: design-reviewer の subagent を 1 本（読むだけ、build と試験は無し）。37 件（blocker 3・major 20・minor 14）。P5 が主な事実（Pi 4 の queue に compute が無い、libvulkan が 1.0 だけ、i915 の NV12 は image → buffer の copy だけ、全 driver が COINCIDE）を source で確かめた。
- 反映の後の版: この commit の design.md・proposed/・user-questions.md。proposed の header は host の cc と clang で構文を確かめた（`media.h` は `-std=c89 -Wall -Wextra -Werror -Wpedantic`、`media-vulkan.h` は tree の `include/libc/vulkan` で `-std=gnu99 -Wall -Wextra -Werror`。tree の Vulkan の header 自体が c89 の pedantic を通らないため）。

| # | 重さ | 指摘（要点） | 反映 |
| --- | --- | --- | --- |
| 1 | blocker | i915 では sample できる NV12 の image が作れず image → image の copy も通らない。1 つの VkImage の frame では渡せない | D7（frame を plane の並びに）、D9・§6.2（image → buffer → R8・R8G8 の image の transfer だけの道）、§6.3。driver の変更の置き場は Q318-10。p006b・p026 の受け入れを直した |
| 2 | blocker | Pi 4 は compute が無く、RTX・Pi 4 は decode も graphics も 1 本の queue。decode の queue を libmedia 専用にできない | D5（どの役も共有でき lock を通す、shared_hint）、D10（変換は描画の pass、compute を使わない）、`media-vulkan.h` の役を CONVERT に、Q318-4 に Pi 4 の制限 |
| 3 | blocker | SAMPLED の device の「変換無し」は DPB の参照の image を DPB の layout のまま渡してしまう | D11（DISTINCT・別名・copy の 3 つ）、§7.3、別名の約束を driver の WS に渡す（§11・§13.6） |
| 4 | major | libvulkan は 1.0 だけ。api_version を上げると instance が作れない。lavapipe が隠す | D4（1.0 固定、KHR の entry point）、§4.2、D21（vkfake を 1.0 と許可の表に絞る） |
| 5 | major | 「WS326 を待たずに始められる」の列が依存と矛盾。日程はユーザーの決定 | §14 の列を直し、Q318-14 に。p012 の依存から header を外した（#37） |
| 6 | major | p003 の受け入れ（今の runtime を vkfake で）は proc addr の差し込みでは届かない。PRIVATE も通らない | D21（vkfake を代わりの `libvulkan.so.1` に）、p003a・p003b |
| 7 | major | p006 で ops と frame を変えると media-probe と host-codec.c が build できなくなる | p006a の範囲に両方の追従を入れた（§12 の 2、plan/tools は Q1 の許可、§15 の 2） |
| 8 | major | p004 で表に libavcodec を足すと browser と media-probe が黙って libavcodec を使う | 旧い `media_decoder_open` は libavcodec 無し、Video Player・Music は一時の口（§12 の 1）。media-probe は HARDWARE|SOFTWARE。browser は Q318-11 |
| 9 | major | engine の既定の context を NONE にすると browser が hardware の decode を失う | engine の既定を PRIVATE に（`media.h`、§4.8）、libbrowser は明示に PRIVATE（§12 の 4） |
| 10 | major | camera の寿命・lock・slot の状態が足りない（自己の deadlock、close の後の unref、stop と start、mmap の寿命、DEQUEUE の間の lock） | §7.4（slot の状態の表と規則）、§7.2（落とす frame は lock の外で unref）、D18、DEVICE の道は毎回写して再投入 |
| 11 | major | app の queue の義務が submit だけでは足りない（present・wait idle・device wait idle） | `media-vulkan.h` の先頭の comment と D5、§4.2 の 4、§12 の 3（Video Player の直し） |
| 12 | major | fence の時間切れの切り替えの記述が今の code と WS326 の規則に合わない | §9 を誤りの写しの表に（timeout・失敗は EIO で終える、切り替えは ENOTSUP・ENODEV・EBUSY だけ、LOST は切り替えない）、D14 |
| 13 | major | frame の identity が再び使われると app の view の cache が古くなる | identity は増えるだけの数で再使用しない、app は小さな LRU（`media-vulkan.h`、§4.5） |
| 14 | major | `media_frame_create`・`media_frame_written` の契約が実装できない（NV12 の attachment、layout が無い、binary semaphore の二度待ち、submit の順） | `media_frame_create` は plane ごとの単一の plane の image と形式ごとの ENOTSUP、`media_frame_written` に layout、semaphore は 1 回だけ待ち以後は libmedia の fence の順、submit の順の規則（`media-vulkan.h`） |
| 15 | major | 毎 frame の CPU の複写（software の出力、camera の mmap）が 2026-10-12 の guardrail と食い違うのに問いが無い | §6.4 と Q318-12。libavcodec の 8 bit 以外の出力は ENOTSUP で次へ（CPU で変換しない） |
| 16 | major | OS の境界の checker の A1・A5 が camera の source に当たる | D17、p023b の範囲と受け入れに checker、許可の行は Q1（§15 の 2） |
| 17 | major | H.265 の decode は 1.3.269 の Khronos の入力に既に KHR で入っているはずで、版の更新は要らない（推定） | §2・§10.2 を直し、p011 を p011a（選び直し、照合から）と p011b（encode のための版の更新）に分けた。Q318-3 を直した |
| 18 | major | AAC の窓の切り替え・TNS は仕様が判定を決めないので標準廉価実装手順の条件 1 を満たさない | D20（p013 が informative な出典で数値まで固定）、packet の枠と色の係数を S に向く所として挙げた |
| 19 | major | p003・p006・p007・p012・p023 が 1 回の起動に大きすぎる。p021 の vkfake の拡張が無い、p023・p025 の受け入れが確かめられない | 各々を a・b に分けた。p020・p021 に vkfake の H.265・encode の模倣を入れた。p023b・p025 の受け入れを確かめられる物に |
| 20 | major | libavcodec の encode の add-in は AVCodecContext の field を書けない（版で配置が違う） | D16（`av_opt_set_*`）、p012b に `host-layout.c` の追従 |
| 21 | major | 他の WS の記録の食い違い（WS329 の依存、WS326 phase023 の p002、libbrowser は WS074、cam.c は WS282） | §11 の表に足し、§15 の 6 で Q1 に直しを頼む |
| 22 | major | readback を decoder の層に移すと Pi 4 の flight の重ねが崩れる | D13（hardware の PRIVATE の readback は runtime の中に残す） |
| 23 | major | notify・解放の callback の thread の契約が設計と食い違う（LOST は呼び手の thread で見つかる、encoder に thread が無い、「取るまで」が曖昧） | `media.h` の notify の comment（libmedia の thread からだけ、空にした時に再び呼べる、LOST の扱い）、wrap の release の comment（どの thread で呼ぶか） |
| 24 | minor | family が 1 つの時の CONCURRENT は誤り | `media-vulkan.h` の先頭と D8（1 つなら EXCLUSIVE） |
| 25 | minor | proposed の header に開発の段階の情報 | 移行の段落と「not produced yet」「0 only for now」を消した |
| 26 | minor | `exports.map` の glob が内部を漏らす | D1（名前を全部並べる）、p002・p010 |
| 27 | minor | ws.md がユーザーの発言を原文で載せている | 見送り: 起動の指示（ws.md の既にある原文の引用はそのまま残す）に従った。整理は Q1（§15 の 8） |
| 28 | minor | D11（旧）の MISSING の優先は app の文言を変える | §4.9 に意図した変更と記録、Q318-13 |
| 29 | minor | `media_planes.data` が書ける pointer | const にし、`media_frame_planes_write` を足した |
| 30 | minor | camera の config の encoder の寿命、dropped の意味 | open の間だけ読む、open からの累計（`media.h`） |
| 31 | minor | decoder の config の hold 0・超えた時・VULKAN を NONE の context で | hold 0 は 1、超えたら send が ENOBUFS、ENOTSUP を create の答えに（`media.h`） |
| 32 | minor | disk の空きの数、host の他の ICD、ASan の leak | §2 を 13 GB（2026-10-31 の `df`）に、vkfake は `VK_DRIVER_FILES` で lavapipe に固定し suppression（§13.2） |
| 33 | minor | Q318-3 の承認の主体と用語 | 「ユーザーの判断と Q1 の承認」に、sysroot・KHR・provisional を用語に |
| 34 | minor | 問いの漏れと中立さ（browser の libavcodec、CPU の複写、先に始めるか、i915 の driver、Pi 4 の compute、文言）、決定 6 を緩める事の明示、B・C の利点、Q318-2 の AAC | Q318-10〜14 を足し、Q318-1・6 に決定 6 を緩める事、各案に利点と欠点、Q318-2 に AAC、Q318-4 に Pi 4 |
| 35 | minor | `run-host-codec.sh` の `thread-posix.o` は `kl_once` のためだけ | §12 の 1（p004 で外す） |
| 36 | minor | arm64 の compile の時の分けは host で通らない | §13.2 の「限り」、p006a で runtime の値にするか Pi 4 の実機で |
| 37 | minor | p012 が p011 に依る | p012a の依存は p007b だけ、p021・p022 が p011b に依る |

## レビューが確かめて正しいとした所（要点）

decode の表の今の姿、media-app の切り替えの条件と `kl_once`、i915 の queue family と decode の usage、libvulkan に timeline が無いこと、libavcodec の package が LGPL で encoder 無しであること、camera の UAPI と import の条件と handle の retain、`DRV_GPU_BACKING_COHERENT` があり i915 の page が LLC の write-back であること（実機では未確認）、`pthread_sigmask` があり `pthread_setname_np` が無いこと、旧い口の使い手の一覧、HAL の変更が要らないこと、H.265 の解析を分ける理由。

## 確かめていない所（残り）

i915 の libvulkan の export が object 全体の allocation になるか（p025 の最初）、i915 の executor が video → graphics の submit の順を semaphore 無しで保つか（`vk-sync.c` の semaphore は identity、p006b で vkfake でなく 5330 で）、HEVC の適合の bitstream の数と大きさ（p016）、Khronos の tag の中身（p011a・p011b）。
