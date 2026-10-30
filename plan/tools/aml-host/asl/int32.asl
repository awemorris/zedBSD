/*
 * WS049 AML test: 32-bit integers, which a DSDT of revision 1 selects.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.
 */
DefinitionBlock ("", "DSDT", 1, "ZEDBSD", "INT32", 1)
{
    Name (FAIL, 0)
    Name (BIG, 0xFFFFFFFF)

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
        Store (BIG, Local0)

        /* Ones and wrap-around are 32 bits wide. */
        CHK (Ones, 0xFFFFFFFF, 1)
        CHK (Add (Local0, 1), 0, 2)
        CHK (Not (0), 0xFFFFFFFF, 3)
        CHK (ShiftLeft (1, 32), 0, 4)
        CHK (LNot (0), 0xFFFFFFFF, 5)
        Store (Local0, Local1)
        Increment (Local1)
        CHK (Local1, 0, 6)

        /* Conversions use four bytes and eight digits. */
        ToBuffer (Local0, Local3)
        CHK (SizeOf (Local3), 4, 10)
        CHK (Concatenate ("x", 0x1F), "x0000001F", 11)
        Store (0x0102, Local2)
        Concatenate (Local2, Local2, Local4)
        CHK (SizeOf (Local4), 8, 12)
        CHK (ToInteger (Buffer () { 1, 2, 3, 4, 5, 6 }), 0x04030201, 13)

        Return (FAIL)
    }
}
