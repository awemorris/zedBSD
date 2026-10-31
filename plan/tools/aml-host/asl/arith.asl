/*
 * WS049 AML test: integer arithmetic, logic and comparison.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.  Operands come from locals and names so that the compiler
 * cannot fold them (and the tests are built with iasl -oa).
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "ARITH", 1)
{
    Name (FAIL, 0)
    Name (NINT, 0x10)
    Name (NSTR, "1A")
    Name (NBUF, Buffer () { 0x34, 0x12 })

    /* Records the first failing check. */
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
        Store (7, Local0)
        Store (3, Local1)

        /* Add, Subtract, Multiply, Divide, Mod. */
        CHK (Add (Local0, Local1), 10, 1)
        CHK (Subtract (Local0, Local1), 4, 2)
        CHK (Multiply (Local0, Local1), 21, 3)
        Divide (Local0, Local1, Local2, Local3)
        CHK (Local3, 2, 4)
        CHK (Local2, 1, 5)
        CHK (Mod (Local0, Local1), 1, 6)
        CHK (Subtract (Local1, Local0), 0xFFFFFFFFFFFFFFFC, 7)
        Add (Local0, Local1, Local4)
        CHK (Local4, 10, 8)
        CHK (Divide (100, Local1), 33, 9)

        /* Shifts, including by the width or more. */
        CHK (ShiftLeft (Local0, 4), 0x70, 10)
        CHK (ShiftRight (0x70, 4), 7, 11)
        Store (64, Local5)
        CHK (ShiftLeft (Local0, Local5), 0, 12)
        CHK (ShiftRight (Local0, Local5), 0, 13)
        CHK (ShiftLeft (1, 63), 0x8000000000000000, 14)

        /* Bitwise operators. */
        CHK (And (0xF0F0, 0xFF00), 0xF000, 20)
        CHK (Or (0xF0F0, 0x0F00), 0xFFF0, 21)
        CHK (XOr (0xF0F0, 0xFF00), 0x0FF0, 22)
        CHK (NAnd (0xF0F0, 0xFF00), 0xFFFFFFFFFFFF0FFF, 23)
        CHK (NOr (0xF0F0, 0x0F00), 0xFFFFFFFFFFFF000F, 24)
        CHK (Not (Local0), 0xFFFFFFFFFFFFFFF8, 25)
        CHK (FindSetLeftBit (0x80), 8, 26)
        CHK (FindSetRightBit (0x80), 8, 27)
        CHK (FindSetLeftBit (0), 0, 28)
        CHK (FindSetRightBit (0x8000000000000000), 64, 29)
        CHK (ToBCD (1234), 0x1234, 30)
        CHK (FromBCD (0x9876), 9876, 31)

        /* Increment and Decrement on a local, a name and a package element. */
        Store (5, Local6)
        Increment (Local6)
        CHK (Local6, 6, 40)
        Decrement (Local6)
        Decrement (Local6)
        CHK (Local6, 4, 41)
        Increment (NINT)
        CHK (NINT, 0x11, 42)
        Store (Package () { 1, 2 }, Local7)
        Increment (Index (Local7, 1))
        CHK (DerefOf (Index (Local7, 1)), 3, 43)
        Store (0xFFFFFFFFFFFFFFFF, Local6)
        Increment (Local6)
        CHK (Local6, 0, 44)

        /* Logical operators produce Ones or Zero. */
        CHK (LAnd (Local0, Local1), Ones, 50)
        CHK (LAnd (Local0, 0), 0, 51)
        CHK (LOr (0, Local1), Ones, 52)
        CHK (LOr (0, 0), 0, 53)
        CHK (LNot (0), Ones, 54)
        CHK (LNot (Local0), 0, 55)
        CHK (LEqual (Local0, 7), Ones, 56)
        CHK (LGreater (Local0, Local1), Ones, 57)
        CHK (LLess (Local0, Local1), 0, 58)
        CHK (LNotEqual (Local0, Local1), Ones, 59)
        CHK (LGreaterEqual (Local0, 7), Ones, 60)
        CHK (LLessEqual (Local0, 6), 0, 61)

        /* Comparison by the first operand's type. */
        CHK (LEqual ("abc", "abc"), Ones, 70)
        CHK (LLess ("abc", "abd"), Ones, 71)
        CHK (LLess ("ab", "abc"), Ones, 72)
        CHK (LGreater ("b", "abc"), Ones, 73)
        CHK (LEqual (Buffer () { 1, 2 }, Buffer () { 1, 2 }), Ones, 74)
        CHK (LLess (Buffer () { 1, 2 }, Buffer () { 1, 3 }), Ones, 75)
        CHK (LEqual (0x1A, NSTR), Ones, 76)
        CHK (LEqual (0x1234, NBUF), Ones, 77)

        /* Implicit conversion of operands to integers. */
        CHK (Add (NSTR, 1), 0x1B, 80)
        CHK (Add (NBUF, 1), 0x1235, 81)
        CHK (Add ("FFz", 1), 0x100, 82)

        Return (FAIL)
    }
}
