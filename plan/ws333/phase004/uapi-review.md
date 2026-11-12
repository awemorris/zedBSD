<!-- awesome-plan project=zedbsd record=ws333-p004-review -->

# WS333 p004: UAPI の案の design-reviewer の指摘と反映

- review: 2026-11-12、design-reviewer 1 回、対象は第 1 版（4b7f85a9d6）の [../proposed/include/uapi/thermal.h](../proposed/include/uapi/thermal.h)・[uapi-proposal.md](uapi-proposal.md)・[layout-test.c](layout-test.c)。blocker 0・major 7・minor 15。
- reviewer が確かめて問題が無かった所: 暗黙の padding が無い（clang の `-Wpadded -Werror` で -m64・-m32・armv7・m68k・powerpc・riscv32・mips・sparcv9）、ioctl の番号の重なりが無い、group `'T'` と cdev 0x00010008・0x00010009 が空き、`hw.thermal` の名前・kernel の中の struct の tag とぶつからない、`thermal_private.h` の同じ名前の macro が同じ値。
- 反映した第 2 版で: host の gcc・clang の -m64・-m32 で layout-test PASS、zedBSD の clang の `-Wpadded -Werror` で armv7・riscv32・m68k・powerpc・mips・sparcv9・i386 が通る（2026-11-12 P1）。

| 指摘 | 要点 | 反映 |
| --- | --- | --- |
| M1 | 事象の index・value の意味が kernel の実装（thermal.c・thermal-guard.c）と食い違う（STRATEGY・EMERGENCY・FAN_STALLED・OVERFLOW） | 反映: UAPI を今の kernel の実装に合わせた（STRATEGY は index = strategy・value = 世代、EMERGENCY は value = m°C で始まりの 1 回、FAN_STALLED は止まった時の 1 回で value 0、OVERFLOW は index 0）。proposal §4 に表 |
| M2 | 事象の頭が `struct system_event` と同じ offset でない | 反映: value を int32 にして size 0・value 12・sequence 16・time 24 に揃えた。`_Static_assert` で offset を固めた |
| M3 | strategy の最後の結果（全部の失敗・EOPNOTSUPP）が読めない | 反映: `thermal_strategy.error` と `thermal_status.strategy_error` を足し、「strategy は受け手の答えに依らず記録」を明記した。事象 STRATEGY の後は GET で読む |
| M4 | 失敗の時の copyout と範囲外の errno が未定・食い違い | 反映: 頭の注に共通の規則（失敗は何も写さない、範囲外は ENOENT、要る結果は 0 と flag）。READ_SAMPLE の時間切れは 0 と `KERN_THERMAL_CPU_SAMPLE_TIMEOUT`。SET_STRATEGY の EOPNOTSUPP の後は GET_STRATEGY で世代と結果 |
| M5 | 振り分けが番号の下 8 bit だけを見る | 反映: 「request の値の全体で照合し違えば ENOTTY、struct を大きくする時は新しい番号」を UAPI の約束にした（kernel の handler に要る事として proposal §6） |
| M6 | sessiond の strategy が保存されない（Q333-1 B） | 反映: 保存の書き手を `/sbin/thermal` 1 つにし、sessiond は `/sbin/thermal strategy …` を実行する（p019 への持ち越し）。書きは一時 file と rename |
| M7 | 「Linux の sysfs と同じ」は電力の読みについて誤り（RAPL の energy は PLATYPUS の後 root だけ） | 反映: proposal §7 の 3 を、背景（担当の記憶、未確認と明記）と案 A（1 秒の粒度の写しを誰でも、推し）・B（root だけ）の問いに直した。ユーザーの判断 |
| m1 | FAN_STALLED の「15 s」は誤りで約 3 秒 | 反映しない: fan の読みは poll の 5 周ごと（`THERMAL_FAN_POLLS` 5、thermal.c:60・2102）で、3 回は約 15 秒。説明を「3 readings (15 s)」にした |
| m2 | passive・critical の説明が不正確（働くのは CPU の sensor・検査を通った ZONE だけ） | 反映: 説明を直し、`KERN_THERMAL_READING_ARMED` を足した（kernel に要る事） |
| m3 | GET_STATUS に CPU の世代と数が無い | 反映: `cpu_count`・`cpu_domain_count`・`cpu_package_count`・`cpu_generation` |
| m4 | GET_PACKAGE_IDLE の target と count の意味 | 反映: target は GET_CPU の package の番号（疎でありうる、無い番号は ENOENT）、count は CPU の数 |
| m5 | domain ALL の permille は preference に当たらない、desired 0 は 0 のまま | 反映: 注を直した |
| m6 | SUBSCRIBE の規則の穴 | 反映: `KERN_THERMAL_EVENT_MASK`（外の bit は EINVAL）、mask 0 の後の read は EINVAL、OVERFLOW は購読に依らず来て GET を読み直す、通し番号は飛ぶ。受け手の名前の事象は無いので説明から外した |
| m7 | SET の out の欄の扱い | 反映: 「out の欄は入力では無視」を頭の注に。SET_STRATEGY が埋める欄を注に |
| m8 | 注入の安全（node の区別・build の switch・消えない sensor・水準の検査） | 反映: `/dev/thermal` は 1〜63 だけ、注入の node は自分の入口で 64〜。switch の名前 `CONFIG_THERMAL_TEST_INJECT`（既定 n、mk の追加は実装の時にユーザーの確認）。sensor は消えず 32 を共有（ENOSPC）、注入の critical も同じ検査 |
| m9 | 誰でも購読の上限なし、CPU を起こす読みを誰でも | 反映: 購読の open は 16 まで（`KERN_THERMAL_READER_MAX`、ENOSPC）。READ_SAMPLE と GET_CPU_IDLE は root |
| m10 | `/sbin/thermal` の穴（restore の EOPNOTSUPP、conf の書き直し、壊れた conf、5 秒の block、wheel の shell、EOPNOTSUPP の保存） | 反映: proposal §5 に全部の扱いを書いた（EOPNOTSUPP は 0 と知らせ、restore は書き直さない、壊れた conf は balanced と警告、wheel は shell では sudo、EOPNOTSUPP でも保存） |
| m11 | `EMERGENCY_OFF` は未決の機能の印、device 番号を UAPI に出す前例が無い | 反映: 両方を header から外した |
| m12 | 512 を UAPI の定数に、`thermal_cpu_info` の伸びる余地 | 反映: `KERN_THERMAL_CPU_MAX` 512（layout-test が `KERN_CPU_MAX` と照らす）、`thermal_cpu_info` を 128 byte（reserved 9 語）に |
| m13 | SET_BOOST の未記載の振る舞い | 反映: ALL は飛ばす、最初の読みの前は boost_count 0、PENDING の後の結果は GET_FAN と事象で |
| m14 | SIGNALED の説明の誤り、init が `/dev/thermal` を開いておく事 | 反映: 説明を直し、ACK の注に「init は起動の時に開く」 |
| m15 | 記録の穴（review の file が無い、design §16.1 の µJ、layout-test が thermal_private.h を見ない、著作権の注の形） | 反映: この file を作った、design §16.1 を nJ に、layout-test の注に「thermal_private.h は手で照らす」と書き著作権の注を coding-style §13 の形に |
