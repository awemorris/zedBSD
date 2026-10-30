/*
 * WS049 AML test: mutexes, events, serialized methods, Sleep, Stall and
 * Timer.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.  sync.evals checks the methods that must fail (a mutex
 * acquired below the thread's sync level, a release of a mutex not held)
 * and that a mutex an evaluation left held is free after it ends.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "SYNC", 1)
{
    Name (FAIL, 0)
    Mutex (MTX0, 0)
    Mutex (MTX3, 3)
    Mutex (MTX5, 5)
    Event (EVT0)
    Name (DPTH, 0)

    Method (CHK, 3)
    {
        If (LNotEqual (Arg0, Arg1))
        {
            If (LEqual (FAIL, 0))
            {
                Store (Arg2, FAIL)
            }
        }
    }

    /* A serialized method that calls itself. */
    Method (SRCR, 1, Serialized, 4)
    {
        Increment (DPTH)
        If (LGreater (Arg0, 0))
        {
            SRCR (Subtract (Arg0, 1))
        }
        Return (DPTH)
    }

    /* Acquires a lower sync level while holding a higher one: must fail. */
    Method (BADO)
    {
        Acquire (MTX5, 0xFFFF)
        Acquire (MTX3, 0xFFFF)
        Release (MTX3)
        Release (MTX5)
        Return (0)
    }

    /* Releases a mutex it does not hold: must fail. */
    Method (BADR)
    {
        Release (MTX0)
        Return (0)
    }

    /* Holds a mutex and returns without releasing it. */
    Method (LEAK)
    {
        Acquire (MTX0, 0xFFFF)
        Return (0)
    }

    /* Reports whether MTX0 is free: zero when it could be acquired at once. */
    Method (FREE)
    {
        Store (Acquire (MTX0, 0), Local0)
        If (LEqual (Local0, 0))
        {
            Release (MTX0)
        }
        Return (Local0)
    }

    Method (MAIN)
    {
        /* Acquire returns zero when it got the mutex. */
        CHK (Acquire (MTX0, 0xFFFF), 0, 1)
        CHK (Acquire (MTX0, 0), 0, 2)
        Release (MTX0)
        Release (MTX0)

        /* Levels may rise and are released in reverse. */
        CHK (Acquire (MTX3, 0xFFFF), 0, 3)
        CHK (Acquire (MTX5, 0xFFFF), 0, 4)
        Release (MTX5)
        Release (MTX3)

        /* The global lock is a mutex too. */
        CHK (Acquire (\_GL, 0xFFFF), 0, 5)
        Release (\_GL)

        /* An event counts its signals; Wait returns Ones when it times out. */
        Reset (EVT0)
        CHK (Wait (EVT0, 10), Ones, 10)
        Signal (EVT0)
        Signal (EVT0)
        CHK (Wait (EVT0, 10), 0, 11)
        CHK (Wait (EVT0, 0xFFFF), 0, 12)
        CHK (Wait (EVT0, 0), Ones, 13)
        Signal (EVT0)
        Reset (EVT0)
        CHK (Wait (EVT0, 0), Ones, 14)

        /* A serialized method may call itself. */
        CHK (SRCR (3), 4, 20)

        /* Timer goes forward across Sleep and Stall. */
        Store (Timer, Local0)
        Sleep (5)
        Stall (10)
        Store (Timer, Local1)
        CHK (LGreaterEqual (Subtract (Local1, Local0), 50000), Ones, 30)

        Return (FAIL)
    }
}
