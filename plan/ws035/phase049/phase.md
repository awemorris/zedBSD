<!-- awesome-plan project=zedbsd record=ws035p049 -->

# ws035-p049: `/dev/dspN` の OSS mmap interface（コピーあり）

Phase ID: `ws035-p049`
Parent: [WS035](../ws.md)
Status: **cleared**（q356-i01、2026-09-24）
Phase disposition: normal
Queue: q356（q356-i01）
実行: メインセッション

## 経緯

2026-09-24 ユーザー決定: `/dev/dsp0` に OSS の mmap 対応 interface を持たせ、audiod は対応なら mmap、非対応なら write を使う。
今はゼロコピーの DMA buffer は要らず、概念上の interface とコピーありでも動く実装を最低限とし、ゼロコピーはその後に別に目指す
（p056）。設計は [audiod-design.md §6](../audiod-design.md)。

## 変更

### UAPI（`include/uapi/audio.h`）

| 追加 | 内容 |
| --- | --- |
| `KERN_AUDIO_GET_CAPS`（9） | `struct audio_caps { caps, mmap_bytes, mmap_lead_bytes, reserved }`。`KERN_AUDIO_CAP_MMAP`・`_TRIGGER`・`_MMAP_COPY` |
| `KERN_AUDIO_GET_OPTR`（10）・`GET_IPTR`（11） | `struct audio_mmap_position { uint64 bytes; uint32 fragments; uint32 offset }`（OSS の `count_info` 相当）。両 ABI で 16 byte |
| `KERN_AUDIO_SET_TRIGGER`（12） | `KERN_AUDIO_TRIGGER_OUTPUT`・`_INPUT` の bit で mmap した方向を開始・停止 |
| `mmap(fd, 0, mmap_bytes, prot, MAP_SHARED)` | **OSS と同じく `PROT_WRITE` を含めば再生、`PROT_READ` だけなら録音**の ring |

**frontier**（位置の報告の意味）: 再生では「device がそこまでの byte を mapping から取った位置」（書き手はその先を埋める。取った fragment は
mapping の中で無音にされる）、録音では「mapping にそこまでの録音がある位置」。コピーありでもゼロコピーでも同じ意味なので、
p056 で中身を変えても利用者は変わらない。

### framework（`src/drivers/audio/audio.c`）

- mmap は `kern_pmem_alloc` の物理連続の **shadow ring**（32 KiB）を `vm_device_create` で user に出す（GPU と同じ仕組み、HAL の変更なし）。
- 再生: fragment の割込みごとに、hardware の 2 fragment 先（`AUDIO_MMAP_LEAD_FRAGMENTS`。DMA の fetch は音より先に進むため）の
  fragment を shadow から DMA の ring へ copy し、shadow のその fragment を無音にする。書き手が遅れると、1 周前の音ではなく無音が鳴る。
  trigger の時点で先頭の 2 fragment を取る。
- 録音: 完了した fragment を DMA の ring から shadow へ copy する。1 周より遅れた分は最後の 1 周だけを写す。
- mmap した方向は close まで read・write・DRAIN が `EBUSY`。write・read で動き始めた方向は mmap できない（`EBUSY`）。
  mmap した再生の close は drain しない。poll は、前回の位置の問い合わせから frontier が 1 fragment 以上進んだら ready。
- **`O_RDWR` の open は hardware にある方向だけを取る**ようにした。system（`src/kern/syscall.c`）は write-only で開いた device の
  mmap を拒む（POSIX どおり）ので、再生だけの device（QEMU の `hda-output` など）を mmap するには `O_RDWR` で開く必要がある。
  以前は `O_RDWR` が録音の無い device で `ENODEV` だった。

## 検証

| 試験 | 結果 |
| --- | --- |
| host fixture（`plan/ws035/tests/run-audio-framework-test.sh`、通常と ASan/UBSan） | 既存 12 項目と新しい mmap 2 項目が PASS。UAPI の ILP32/LP64 の大きさ |
| fixture の mmap 再生 | caps、範囲・権限の拒否、trigger で先頭 2 fragment、位置と fragment の数、4 fragment 先を埋め続けて 20 fragment が連続した数列で鳴る、止めると無音、trigger で停止、close で漏れ無し、write 後の mmap は EBUSY、`PROT_WRITE` の方向の無い open は拒否、`O_RDWR` で両方を mmap して 1 回の trigger で両方が動く |
| fixture の mmap 録音 | 読みだけの mapping、read は EBUSY、3 fragment が順に届く、1 周を超えると wrap して数列が続く |
| QEMU ICH9 HDA（`plan/ws035/tests/run-hda-mmap-qemu.sh`、[evidence](evidence/)） | **mmap 再生 96000 frame が bit 一致**（2004 ms、poll 97 回）。write の再生も bit 一致（退行なし）。同じ起動で mmap 24000 → write 48000 の 2 回とも bit 一致。duplex の mmap 録音 48000・144000 frame が 1012・3007 ms（実時間どおり）、その後の read の録音も overrun 0 |
| CI の kernel（amd64・pcat・pc98）と audio を入れた pcat・pc98 | build できる。warning 0（clang driver の `-no-pie` 未使用を除く） |

`audiotest` に `mmapplay`・`mmapcapture` を足し、`hda-wav-check.py` に続けて鳴った複数の数列を確かめる `patterns` を足した。

## 残り

- ゼロコピー（DMA の ring を直接 mmap）は p056。
- mmap した再生の underrun（書き手が遅れて無音になった回数）は数えていない。
