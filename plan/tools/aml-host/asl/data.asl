/*
 * WS049 AML test: strings, buffers, packages and the conversions between
 * them.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "DATA", 1)
{
    Name (FAIL, 0)
    Name (STR1, "Hello")
    Name (BUF1, Buffer (8) { 0x01, 0x02, 0x03 })
    Name (PKG1, Package (4) { 0x10, "text", Buffer () { 0xAA, 0xBB }, Package () { 1, 2 } })
    Name (INT1, 0)
    Name (STRT, "")
    Name (BUFT, Buffer (4) {})
    Name (VPKG, Package () {})

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
        /* SizeOf. */
        CHK (SizeOf (STR1), 5, 1)
        CHK (SizeOf (BUF1), 8, 2)
        CHK (SizeOf (PKG1), 4, 3)
        Store ("abc", Local0)
        CHK (SizeOf (Local0), 3, 4)

        /* Index and DerefOf on each kind of container. */
        CHK (DerefOf (Index (STR1, 1)), 0x65, 10)
        CHK (DerefOf (Index (BUF1, 2)), 3, 11)
        CHK (DerefOf (Index (BUF1, 5)), 0, 12)
        CHK (DerefOf (Index (PKG1, 0)), 0x10, 13)
        CHK (DerefOf (Index (PKG1, 1)), "text", 14)
        CHK (DerefOf (Index (DerefOf (Index (PKG1, 3)), 1)), 2, 15)
        Store (0x7F, Index (BUF1, 7))
        CHK (DerefOf (Index (BUF1, 7)), 0x7F, 16)
        Store ("new", Index (PKG1, 1))
        CHK (DerefOf (Index (PKG1, 1)), "new", 17)
        Index (PKG1, 0, Local1)
        CHK (DerefOf (Local1), 0x10, 18)

        /* Concatenate by the first operand's type. */
        CHK (Concatenate ("ab", "cd"), "abcd", 20)
        CHK (Concatenate ("x", 0x1F), "x000000000000001F", 21)
        CHK (Concatenate (Buffer () { 1 }, Buffer () { 2, 3 }), Buffer () { 1, 2, 3 }, 22)
        Store (0x0102, Local2)
        CHK (Concatenate (Local2, 0x0304), Buffer () { 2, 1, 0, 0, 0, 0, 0, 0, 4, 3, 0, 0, 0, 0, 0, 0 }, 23)
        CHK (Concatenate (Buffer () { 9 }, 0x0102), Buffer () { 9, 2, 1, 0, 0, 0, 0, 0, 0 }, 24)
        CHK (Concatenate ("s", Buffer () { 0xAB, 0x01 }), "s0xAB 0x01", 25)

        /* Mid on strings and buffers. */
        CHK (Mid ("abcdef", 1, 3), "bcd", 30)
        CHK (Mid ("abcdef", 4, 10), "ef", 31)
        CHK (Mid ("abcdef", 10, 2), "", 32)
        CHK (Mid (Buffer () { 1, 2, 3, 4 }, 2, 2), Buffer () { 3, 4 }, 33)

        /* Explicit conversions. */
        CHK (ToInteger ("0x1F"), 0x1F, 40)
        CHK (ToInteger ("123"), 123, 41)
        CHK (ToInteger (Buffer () { 0x78, 0x56, 0x34, 0x12 }), 0x12345678, 42)
        CHK (ToBuffer (0x0102), Buffer () { 2, 1, 0, 0, 0, 0, 0, 0 }, 43)
        CHK (ToBuffer ("AB"), Buffer () { 0x41, 0x42, 0 }, 44)
        CHK (ToString (Buffer () { 0x41, 0x42, 0x00, 0x43 }, Ones), "AB", 45)
        CHK (ToString (Buffer () { 0x41, 0x42, 0x43 }, 2), "AB", 46)
        CHK (ToDecimalString (1234), "1234", 47)
        CHK (ToDecimalString (Buffer () { 1, 20, 255 }), "1,20,255", 48)
        CHK (ToHexString (Buffer () { 1, 0xAB }), "0x01,0xAB", 49)

        /* Store converts to the type of a named target. */
        Store ("1A", INT1)
        CHK (INT1, 0x1A, 50)
        CHK (ObjectType (INT1), 1, 51)
        Store (0x41, STRT)
        CHK (STRT, "0000000000000041", 52)
        Store (Buffer () { 1, 2, 3, 4, 5, 6 }, BUFT)
        CHK (BUFT, Buffer () { 1, 2, 3, 4 }, 53)
        Store (Buffer () { 9 }, BUFT)
        CHK (BUFT, Buffer () { 9, 0, 0, 0 }, 54)
        Store (0x0807, BUFT)
        CHK (BUFT, Buffer () { 7, 8, 0, 0 }, 55)

        /* CopyObject changes the type of a named object. */
        CopyObject ("now a string", INT1)
        CHK (ObjectType (INT1), 2, 56)
        CHK (INT1, "now a string", 57)

        /* A package stored into a local is a copy. */
        Store (PKG1, Local3)
        Store (0x99, Index (Local3, 0))
        CHK (DerefOf (Index (PKG1, 0)), 0x10, 60)
        CHK (DerefOf (Index (Local3, 0)), 0x99, 61)

        /* VarPackage with a computed size. */
        Store (3, Local4)
        Store (Package (Local4) { 1 }, VPKG)
        CHK (SizeOf (VPKG), 3, 62)

        /* Match. */
        Store (Package () { 5, 10, 15, 20 }, Local5)
        CHK (Match (Local5, MEQ, 15, MTR, 0, 0), 2, 70)
        CHK (Match (Local5, MGT, 7, MLT, 20, 0), 1, 71)
        CHK (Match (Local5, MGE, 12, MTR, 0, 3), 3, 72)
        CHK (Match (Local5, MEQ, 99, MTR, 0, 0), Ones, 73)

        /* ObjectType of each kind. */
        CHK (ObjectType (STR1), 2, 80)
        CHK (ObjectType (BUF1), 3, 81)
        CHK (ObjectType (PKG1), 4, 82)
        CHK (ObjectType (MAIN), 8, 83)
        CHK (ObjectType (Local5), 4, 84)

        Return (FAIL)
    }
}
