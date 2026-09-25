<!-- awesome-plan project=zedbsd record=ws035p009 -->

# ws035-p009: `/sbin/audiod`（mix、録音の配布）

Phase ID: `ws035-p009`
Parent: [WS035](../ws.md)
Status: **cleared**（q364-i01、2026-09-24）
Phase disposition: normal
Queue: q364（q364-i01）
実行: メインセッション

設計は [audiod-design.md](../audiod-design.md)（p050）。2026-09-24 のユーザーの決定（共有メモリ、streaming の interface なし、
`shm_open` 直後の `shm_unlink` で匿名にした fd を SCM_RIGHTS で渡す、`/dev/dsp0` の mmap に対応していれば使い非対応なら write）に従う。

## 作ったもの

`userland/base/audiod/`（base、既定 ON、`/sbin/audiod`、`/etc/service.d/audiod`、`rc.conf` に optional で有効）:

| file | 役割 |
| --- | --- |
| `protocol.h` | socket の message（要求 10 種、応答・事象 9 種）と共有メモリの header。**位置の読み書きの関数**（`audiod_position_load`・`_store`）: 8 byte の atomic が lock-free でない i386 では、位置ごとの sequence の語で上下 32 bit を 1 つの値として読む（単一の書き手の seqlock）。libpulse（p019）もこれを使う |
| `main.c` | `/run/audiod.sock`（0666）、1 thread の `poll` の loop、client（最大 64）、stream の作成（`shm_open(O_EXCL)`→即 `shm_unlink`→`ftruncate`→`mmap`→fd を SCM_RIGHTS）、要求の処理、SIGBUS の回復 |
| `mix.c` | 32 bit の整数で mix（S16 は 16 bit 上げる）。1 本の S16・同じ rate・等倍は bit 一致。rate は 32.32 の固定小数の線形補間、mono↔stereo、S16/S32/F32。stream の音量、underrun、REQUEST（period 分の空きごと）、drain の目標。録音は device の period を各 stream の形式と rate へ |
| `device.c` | `/dev/dsp0` を **O_RDWR** で開く（write-only の device は mmap できない、p049）。mmap があれば frontier の 2 fragment 先まで mix を直接書く。無ければ write で 2 period を保つ（`poll` の POLLOUT は空きがある間いつも立つので、足りない時だけ待ち、他は半 period の timeout）。device が無ければ時計で進め、録音は無音を渡す。録音は read を period にまとめる |

## 途中で直したもの

- **libc: `<sys/socket.h>` が `struct iovec` を定義していなかった**（POSIX は定義を求める）。`<sys/uio.h>` を含めた。
- 録音の非 blocking の `read` は来た分（512 byte など）を返す。period にまとめず捨てていたので録音が届かなかった。
- 補間の最後の 1 frame は次の frame が無いので読まれず、drain が終わらなかった（44.1 kHz の試験で client が止まった）。drain 中は残りを捨てる。
- 試験 client: 渡された fd の付いた data は `MSG_PEEK` できない（kernel の既存の規則）。header を `recvmsg` で読んでから残りを読む。
- i386（-march=i386）には lock-free の 8 byte atomic が無く build できなかった → 上の位置の関数。

## 検証（QEMU ICH9 HDA、`plan/ws035/tests/run-audiod-qemu.sh`、[evidence](evidence/)）

`plan/ws035/tests/audiod-client.c`（protocol を直接話す試験 client）で:

| 試験 | 結果 |
| --- | --- |
| 数列 96000 frame（S16 stereo 48 kHz） | **WAV で bit 一致**、2110 ms、underrun 0 |
| 2 つの client が同時に 3000 と 5000 | 出力の窓がすべて **8000**（mix） |
| 16000 を音量 1/2（32768） | **8000** |
| 44.1 kHz mono の数列 44100 frame | 補間で再生、drain が 1122 ms で完了 |
| duplex の録音 48000・144000 frame | 1015・2968 ms（実時間どおり）、overrun 0 |
| client が再生中の共有メモリを 0 に縮める | audiod は同じ PID で動き続け、別の接続の 48000 frame が bit 一致 |
| HDA の無い機械 | 再生 48000 frame が 1033 ms（時計で進む）、録音は無音が 982 ms で届く |

i386（pcat の rootfs）と pc98 の build も通る。

## 残り

- 音の再生の遅れは frontier の先 2 fragment ＋ mmap の lead 2 fragment（約 85 ms）。`played_position` は audiod が読んだ位置で、device の遅れを含まない（p019 の `pa_stream_get_latency` で補う）。
- 共有メモリの seal（`F_ADD_SEALS`）は未実装（設計 §4.3 の後段）。今は SIGBUS の回復で守る。
- `/dev/dsp` のゼロコピー（p056）、libpulse（p019）。
