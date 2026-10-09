<!-- awesome-plan project=zedbsd record=ws001-p047 -->

# ws001-p047: scheduling の関数と時計

Status: planning（2026-10-09 Q1、ベータ3 以降、未割当。ユーザーの WS001 の「上限 4 LW で止める」があるので Q1 の割当まで着手しない）
Parent: [WS001](../ws.md)
Queue: なし

## 経緯

[ws001-p045](../phase045/phase.md)（POSIX.1-2024 の header の全数の照合）で、宣言も実装も無いと分かった関数のうち、大きい物を分けた（2026-10-09 Q1 承認）。照合の表は `plan/ws001/tests/posix-headers/`（`check.py`）。

## 範囲

sched_get_priority_max/min・sched_getparam/setparam・sched_getscheduler/setscheduler・sched_rr_get_interval（sched.h）、pthread の sched 属性（inheritsched・schedpolicy・scope・getschedparam/setschedparam・setschedprio・prioceiling・protocol、pthread.h の 20）、posix_spawnattr の sched（4）、pthread_getcpuclockid・clock_getcpuclockid・CLOCK_PROCESS_CPUTIME_ID・CLOCK_THREAD_CPUTIME_ID、SCHED_SPORADIC と sched_param の sched_ss_* member。

## 進め方の案

kernel の scheduler が持つ物（SCHED_OTHER だけか、優先度の範囲、CPU 時間の時計）を調べ、無い物は規格の許す形（ENOTSUP・EPERM・固定の値）で。kernel の変更が要れば別に提案。足した後に `check.py` で該当の名前が無い物の列から消えることを確かめる。

## 受け入れ

- 該当の名前が x86_64・i386・aarch64 で宣言され、実装がある（または規格の許す option の宣言で持たないと明示）。
- build warning 0、host 試験、必要なら guest の試験は T1。
