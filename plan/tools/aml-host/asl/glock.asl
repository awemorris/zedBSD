/*
 * WS049 AML test: the FACS Global Lock shared with firmware.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.  aml-host --global-lock gives \_GL_ a simulated FACS lock
 * whose firmware, at each Sleep, asks for a lock the system owns or lets
 * go of a lock it owns (glock.args).  The steps: the system takes the
 * lock; firmware asks for it; the release signals GBL_RLS and firmware
 * takes it; an Acquire with no wait times out; an Acquire that waits gets
 * the lock once firmware lets it go, which raises GBL_STS; a Lock field
 * nests inside it.  acpiexec has no such firmware (glock.harness-only).
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "GLOCK", 1)
{
    Name (FAIL, 0)

    OperationRegion (MEM0, SystemMemory, 0x100000, 0x10)
    Field (MEM0, ByteAcc, Lock, Preserve)
    {
        BYT0, 8
    }

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

    Method (MAIN)
    {
        /* A Lock field takes and lets go of the lock by itself. */
        Store (0x5A, BYT0)
        CHK (BYT0, 0x5A, 1)

        /* The system takes the free lock; firmware asks for it meanwhile. */
        CHK (Acquire (\_GL_, 0xFFFF), 0, 2)
        Sleep (1)

        /* The release hands the lock to firmware (GBL_RLS). */
        Release (\_GL_)

        /* Firmware owns it: an Acquire that does not wait times out. */
        CHK (Acquire (\_GL_, 0), Ones, 3)

        /* An Acquire that waits gets it once firmware lets it go. */
        CHK (Acquire (\_GL_, 0xFFFF), 0, 4)

        /* A Lock field inside nests in the held lock. */
        Store (0xA5, BYT0)
        CHK (BYT0, 0xA5, 5)
        Release (\_GL_)
        Return (FAIL)
    }
}
