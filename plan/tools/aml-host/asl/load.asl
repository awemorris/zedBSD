/*
 * WS049 AML test: Load from a buffer and from an operation region.
 * (Unload takes the handle LoadTable gives, see loadtable.asl.)
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.  The loaded tables are support/dynamic.asl and
 * support/dynamic2.asl, whose bytes the test runner writes to
 * build/aml-host/asl/support/*.inc.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "LOAD", 1)
{
    External (\DYNV, IntObj)
    External (\DYNM, MethodObj)
    External (\DYN2, IntObj)

    Name (FAIL, 0)
    Name (TBLB, Buffer ()
    {
#include "dynamic.inc"
    })
    Name (TBLC, Buffer ()
    {
#include "dynamic2.inc"
    })

    OperationRegion (TBLR, SystemMemory, 0x00400000, 0x1000)
    Field (TBLR, ByteAcc, NoLock, Preserve)
    {
        TBLF, 0x8000
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
        CHK (CondRefOf (\DYNV), 0, 1)

        /* Loads the table from the buffer; the outcome is true. */
        Load (TBLB, Local0)
        CHK (Local0, Ones, 2)
        CHK (CondRefOf (\DYNV), Ones, 3)
        CHK (\DYNV, 0x1234, 4)
        CHK (\DYNM (41), 42, 5)
        CHK (CondRefOf (\_SB.DYND), Ones, 6)

        /* Loads a second table from memory an operation region covers. */
        Store (TBLC, TBLF)
        Load (TBLR, Local1)
        CHK (Local1, Ones, 10)
        CHK (\DYN2, 0x5678, 11)

        Return (FAIL)
    }
}
