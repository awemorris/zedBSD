<!-- awesome-plan project=zedbsd record=ws333-p004-uapi -->

# WS333 p004: `/dev/thermal` の UAPI の案（ユーザーの review に出す物）

- 版: 第 1 版（2026-11-12 P1、q2012-i01、基点 main 3a073a7e62）。design-reviewer の指摘の反映は [uapi-review.md](uapi-review.md)。
- 全文: [../proposed/include/uapi/thermal.h](../proposed/include/uapi/thermal.h)（実装の時に `include/uapi/thermal.h` に置く。この Phase では置かない）。
- layout の確かめ: [layout-test.c](layout-test.c)（host の gcc・clang の -m64・-m32 で `_Static_assert` が通り、kernel の `include/kern/thermal.h`・`cpuperf.h` と一緒に include して同じ名前の macro が同じ値、CPU の flag の値が kernel の物と同じ）。

## 1. 何の口か（背景）

WS333 は、CPU の休み方（C-state）・CPU の速さ（P-state、HWP）・P-core と E-core の相対の速さ・温度・fan・電力の読み・「strategy」（性能を取るか静かさ・省電力を取るかの選び）・熱の非常の停止を、1 つの device `/dev/thermal` とその道具 `/sbin/thermal` の下にまとめる。kernel の中の口（`include/kern/thermal.h` など）は main にあり、5330 の実機で HWP・電力の読み・Dell の BIOS の熱の mode が動いた（試験だけの patch で）。user から読む・選ぶ口（UAPI）がまだ無いので、ここでその形を決める。UAPI は一度出すと変えにくいので、実装の前にユーザーの review に出す（design.md §4.10）。

用語:

- **ioctl**: device の file に対する命令。番号と、受け渡す struct の形で決まる。
- **受け手（receiver）**: strategy を受け取って自分の物に写す driver。今は「cpu」（CPU の速さの要求）と「dell-wmi」（Dell の BIOS の熱の mode）。Pi 5 の fan も後で受け手になる。
- **domain**: 1 つの速さの要求を一緒に受ける CPU の組（5330 は P-core の 2 組と E-core 8 つで 10）。
- **HFI**: Intel の CPU が「今どの core が速い・効率が良いか」を書く表。
- **滞在（residency）**: CPU が各 C-state に居た時間。電力の受け入れで「深く休めているか」を見るのに使う。

## 2. 既に決まった事（2026-11-01 ユーザーの決定、Q333-1〜7 を推しで）と、この案での形

| 問い | 決定 | この案 |
| --- | --- | --- |
| Q333-1 起動の時の strategy | B: rc が保存した strategy を渡す（既定 balanced） | `/sbin/thermal strategy …` が成功の後に `/etc/thermal.conf` に書き、rc が `/sbin/thermal restore` を走らせる（§5）。kernel は起動の時 FIRMWARE（何も書かない） |
| Q333-2 gating の軸 | C: 保存と表示だけ | `struct thermal_strategy` の `gating` は受け渡すが、今の受け手は使わない |
| Q333-3 root の直の設定 | A: 直の設定が優先し CUSTOM、次の strategy で消える | `SET_CPU_DOMAIN` の後の `GET_STRATEGY` は flag `CUSTOM` |
| Q333-4 3 軸の向き | A: どれも 100 = その物が多い | speed 100 = 一番速く、noise 100 = 一番うるさくてよい、gating 100 = 一番細かく電源を切る |
| Q333-5 Dell の Quiet | A: manual で noise ≤ 33 の時だけ | 受け手の中の写し（UAPI には出ない）。`firmware_name` に BIOS の mode の名前 |
| Q333-6 電力と滞在 | A: 入れる（読みだけ） | `GET_METER`・`GET_CPU_IDLE`・`GET_PACKAGE_IDLE`。電力の上限は書かない |
| Q333-7 MSR の道の既定 | A: 5330 の受け入れ（p014）の後に有効 | UAPI は既定の有無に依らない。MSR の道の無い kernel では CPU の欄の多くが UNKNOWN、SET は EOPNOTSUPP |

## 3. node・番号・権限

| 項目 | 案 |
| --- | --- |
| node | `/dev/thermal`（cdev 0x00010008、mode 0666）、`/dev/thermal-inject`（0x00010009、mode 0600、試験の build だけ）。2026-11-12 の grep で両方とも空き |
| ioctl の group | `'T'`（空き） |
| 番号の範囲 | 1〜31 熱の core（`src/kern/thermal.c`）、32〜63 CPU（`src/kern/cpuperf.c` が `kern_thermal_ioctl_range_register` で登録）、64〜 試験の注入。登録の無い番号は ENOTTY |
| 権限 | GET・SUBSCRIBE・read は誰でも。SET_* は root（ioctl の中で判定、他は EPERM）。sessiond は root か wheel の利用者に代わって strategy と boost だけを SET する（p019）。EMERGENCY_ACK は pid 1 だけ、非常の状態が SIGNALED の時だけ（他は EPERM・EINVAL） |

## 4. ioctl の一覧

| 番号 | 名前 | 向きと struct（byte） | 中身・戻り |
| --- | --- | --- | --- |
| 1 | `KERN_THERMAL_GET_STATUS` | 出 `thermal_status`（80） | sensor・fan・meter・受け手の数、strategy の値・flag・世代、passive の上限、非常の状態（状態・始めた sensor・その温度・時刻）、flag（`EMERGENCY_OFF` は今は常に 0） |
| 2 | `KERN_THERMAL_GET_SENSOR` | 入出 `thermal_sensor_info`（96） | index → 名前・種類（ZONE・DEVICE）・CPU の印・温度（m°C）・passive・critical・読んだ時刻・VALID/STALE。index ≥ count は EINVAL（count は返す） |
| 3 | `KERN_THERMAL_GET_FAN` | 入出 `thermal_fan_info`（96） | index → 名前・rpm・rpm の最大・自動の段・boost の段・段の数・状態（KNOWN・STALE・STALLED・CAN_BOOST） |
| 4 | `KERN_THERMAL_GET_METER` | 入出 `thermal_meter_info`（80） | index → 名前・種類（PACKAGE・CORES・GRAPHICS・PLATFORM・DRAM）・積算の energy（nJ、64 bit）・時刻 |
| 5 | `KERN_THERMAL_GET_STRATEGY` | 出 `thermal_strategy`（48） | 値・3 軸（MANUAL の時だけ非 0）・flag（CUSTOM・PARTIAL・PENDING）・世代・BIOS の mode の名前・受け手の数 |
| 6 | `KERN_THERMAL_SET_STRATEGY` | 入出 `thermal_strategy` | root。値と 3 軸を入れ、flag と世代を返す。戻り: 受け手が無い・全部が対応外 → EOPNOTSUPP（strategy は記録）、対応する受け手が全部失敗 → 最初の errno、他は 0 で flag が PARTIAL・PENDING（5 秒）・SUPERSEDED。FIRMWARE・未知の値・MANUAL でない時の非 0 の軸・101 以上の軸・reserved の非 0 → EINVAL |
| 7 | `KERN_THERMAL_GET_RECEIVER` | 入出 `thermal_receiver_info`（64） | index → 受け手の名前・最後の結果（errno・UNCONFIRMED・適用した世代・かかった ms・時刻） |
| 8 | `KERN_THERMAL_SET_BOOST` | 入出 `thermal_boost`（16） | root。fan（index か ALL）と段。fan の driver を 5 秒待つ（PENDING）。段が boost_count を超える → EINVAL、boost の無い fan → EOPNOTSUPP、fan が無い → ENOENT |
| 9 | `KERN_THERMAL_SUBSCRIBE` | 入 `thermal_subscription`（16） | 事象の種類の mask（前の mask を置き換える、0 は購読を止める） |
| 10 | `KERN_THERMAL_EMERGENCY_ACK` | なし | pid 1 だけ。init が非常の停止を始めた印 |
| 32 | `KERN_THERMAL_GET_CPU` | 入出 `thermal_cpu_info`（96） | cpu → topology（package・die・module・core・thread）・core の種類（P・E）・domain・相対の速さ（0〜100）・HFI の値（0〜255）・HWP の 4 つの anchor・Highest と今の MHz の推定・flag。request に READ_SAMPLE を立てると CPU を起こして今の MHz を測る（約 100 ms 眠る） |
| 33 | `KERN_THERMAL_GET_CPU_DOMAIN` | 入出 `thermal_cpu_domain`（248） | domain → 所属の CPU の bit・段の形（RANGE・STEPS）・共有の単位・今の要求（min・max・desired・EPP）・熱の上限・flag |
| 34 | `KERN_THERMAL_SET_CPU_DOMAIN` | 入 `thermal_cpu_setting`（32） | root、診断。1 つの domain か全部（全部の時は各 domain の範囲の ‰）に直の値。次の strategy で消える。戻り: EINVAL・EOPNOTSUPP・ETIMEDOUT（要求は残り、後で効く） |
| 35 | `KERN_THERMAL_GET_CPU_RELATIVE` | 出 `thermal_cpu_relative`（16） | 相対の速さの測り方の候補（FREQUENCY・FEEDBACK・PROVIDER）と今の物 |
| 36 | `KERN_THERMAL_SET_CPU_RELATIVE` | 入 `thermal_cpu_relative` | root。測り方を選ぶ（次の起動まで）。測れない物は EOPNOTSUPP |
| 37 | `KERN_THERMAL_GET_CPU_IDLE` | 入出 `thermal_cpu_idle`（128） | cpu → その core（と module）の C-state の滞在（状態の番号・数える所・ns）。その CPU の上で読むので最大 1 秒眠る |
| 38 | `KERN_THERMAL_GET_PACKAGE_IDLE` | 入出 `thermal_cpu_idle` | package → package の C-state の滞在（PC2・PC3・PC6・PC7） |
| 64 | `KERN_THERMAL_INJECT_SENSOR` | 入 `thermal_inject_sensor`（64） | 試験の sensor を足す・値を変える（同じ名前は同じ sensor） |
| 65 | `KERN_THERMAL_INJECT_RECEIVER` | 入 `thermal_inject_receiver`（48） | 試験の受け手（決まった errno を決まった ms の後に返す） |
| 66 | `KERN_THERMAL_INJECT_PATH` | 入 `thermal_inject_path`（16） | 次の非常で init に送るか（実機と同じ）、kernel が直に切るか |

事象（`read`）: 1 件 128 byte の `struct thermal_event`（`struct system_event` と同じ大きさで、頭の size・種類・値・通し番号・時刻の並びが近い）。種類は STRATEGY・BOOST・PASSIVE・CRITICAL・EMERGENCY・FAN_STALLED・CPU・OVERFLOW。open ごとに 64 件の ring、溢れたら古い物を捨て、次の read の最初に OVERFLOW（値は捨てた数）。`poll` は 1 件以上ある時に読める。

## 5. `/sbin/thermal` の使い方の案

誰でも読める。`*` は root（sessiond 経由では strategy と boost を wheel の利用者も）。

```
thermal                          要約（strategy、温度の一覧、fan、電力、非常の状態）
thermal sensors | fans | meters | receivers
thermal strategy                 今の strategy、BIOS の mode、受け手ごとの最後の結果
thermal strategy performance | balanced | powersave            *  選んで /etc/thermal.conf に保存
thermal strategy manual <speed> <noise> <gating>               *  各 0〜100、100 = その物が多い
thermal restore                                                *  /etc/thermal.conf の strategy を渡す（無ければ balanced。rc が起動の時に走らせる）
thermal boost <fan|all> <level>                                *  fan の段の下限（0 で戻す。保存しない）
thermal cpu                      CPU ごとの 1 行（種類 P/E、domain、相対の速さ、今の MHz）
thermal cpu <n> [sample]         1 つの CPU の全部の欄（sample は CPU を起こして MHz を測る）
thermal cpu domains              domain ごとの段の範囲と今の要求・上限・CUSTOM
thermal cpu set <domain|all> [min=N] [max=N] [desired=N] [epp=N] *  診断の直の設定（次の strategy で消える）
thermal cpu relative [frequency|feedback|provider]             *  相対の速さの測り方（引数なしは表示だけ）
thermal idle [cpu <n> | package <n>]    C-state の滞在（2 回読み、間の割合も出す option -i <秒>）
thermal power [-i <秒>]          meter ごとの W（2 回読みの差）
thermal watch [<種類>...]        事象を 1 行ずつ（strategy・boost・passive・critical・emergency・fan・cpu）
```

- 出力は 1 行 1 項目の「key value」の形を基本にし、表示の道具（Keiland の monitor、p022）と試験の script が読める形にする（詳細は p005 後半・p013 で決める）。
- 終了の値: 0 成功、1 kernel が拒んだ（errno を 1 行で出す）、2 使い方の誤り。strategy の PARTIAL・PENDING は 0 で、受け手ごとの結果を出す。
- `/etc/thermal.conf`: 1 行 `strategy <名前> [<speed> <noise> <gating>]`。書けない時は strategy の SET 自体は成功のまま警告を出す。

## 6. 設計（design.md §4.10）からの差と、kernel の側に要る事

| 所 | 差 | 理由 |
| --- | --- | --- |
| meter の単位 | design §4.6 は µJ、案は nJ（`energy_nj`） | kernel の写しが nJ（p005）。64 bit で 1 W なら 584 年、50 W でも 11 年で溢れない。換算の丸めを UAPI に持ち込まない |
| `thermal_cpu_domain` | WS301 §5.2 の 240 byte → 248 byte（`cpu_count`・`share` を足した） | kernel の `kern_cpuperf_domain` が持ち、共有の単位（E-core の 4 つ組の確かめ、p014）を見るのに要る |
| `thermal_cpu_info` | WS301 の `core_type` を `core_class`（P・E）と `core_type`（processor の code）に分け、`firmware_id`・`request` を足した。96 byte のまま | kernel の `kern_cpuperf_topology` の形。READ_SAMPLE は CPU を起こすので読み手が選ぶ |
| `thermal_cpu_idle` | 状態ごとに `scope`（CORE・MODULE・PACKAGE）を足した | 5330 の MC6（module の C6）を CC6 とぶつけずに出せるようにする（p011 の持ち越し）。今の kernel は CORE と PACKAGE だけを出す |
| `thermal_strategy` | 受け手ごとの結果を中に持たず、`GET_RECEIVER` に分けた | design §4.10 の表のとおり。struct が 48 byte で済む |
| 名前と値 | `KERN_THERMAL_*` の値は `include/kern/thermal.h`・`thermal_private.h` の同じ名前の値と同じにした | 実装（p005 後半）で kernel の header がこの UAPI の header を include し、二重の定義を外す（design §4.2） |

kernel の側に足す事（実装の Phase、p005 後半・p013）: 非常の始まりの時刻（`emergency_time_ns`）、事象の通し番号と名前の写し、`thermal_fan_info` の flag への写し（kernel は 2 つの欄に分けて持つ）、試験の注入の 3 つの口。

## 7. ユーザーに見てほしい点

1. ioctl の一覧と struct の欄（§4、全文は header の案）。足りない物・要らない物。
2. `/sbin/thermal` の命令の形（§5）。特に「strategy を選ぶと保存される」（Q333-1 B）と「boost は保存しない」。
3. 誰でも読める（0666）事。温度・電力・CPU の速さは他の利用者にも見える（Linux の sysfs と同じ扱い）。

（技術の判断は担当と Q1 が決めた: 番号の並び、nJ、struct の大きさ、事象の形。）
