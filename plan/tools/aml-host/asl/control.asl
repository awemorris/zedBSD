/*
 * WS049 AML test: control flow and method invocation.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "CONTROL", 1)
{
    Name (FAIL, 0)
    Name (CNT, 0)

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

    /* Returns the larger of two integers through If/Else. */
    Method (MAX2, 2)
    {
        If (LGreater (Arg0, Arg1))
        {
            Return (Arg0)
        }
        Else
        {
            Return (Arg1)
        }
    }

    /* Classifies a number with ElseIf. */
    Method (CLS, 1)
    {
        If (LLess (Arg0, 10))
        {
            Return (1)
        }
        ElseIf (LLess (Arg0, 100))
        {
            Return (2)
        }
        Else
        {
            Return (3)
        }
    }

    /* Sums 1..Arg0 with a While loop. */
    Method (SUMN, 1)
    {
        Store (0, Local0)
        Store (1, Local1)
        While (LLessEqual (Local1, Arg0))
        {
            Add (Local0, Local1, Local0)
            Increment (Local1)
        }
        Return (Local0)
    }

    /* Sums the odd numbers below Arg0, stopping at 50, with Break and Continue. */
    Method (ODDS, 1)
    {
        Store (0, Local0)
        Store (0, Local1)
        While (One)
        {
            Increment (Local1)
            If (LGreaterEqual (Local1, Arg0))
            {
                Break
            }
            If (LEqual (And (Local1, 1), 0))
            {
                Continue
            }
            Add (Local0, Local1, Local0)
            If (LGreater (Local0, 50))
            {
                Break
            }
        }
        Return (Local0)
    }

    /* Returns from inside nested loops. */
    Method (FIND, 1)
    {
        Store (0, Local0)
        While (LLess (Local0, 10))
        {
            Store (0, Local1)
            While (LLess (Local1, 10))
            {
                If (LEqual (Multiply (Local0, Local1), Arg0))
                {
                    Return (Add (Multiply (Local0, 100), Local1))
                }
                Increment (Local1)
            }
            Increment (Local0)
        }
        Return (Ones)
    }

    /* Recursion: factorial. */
    Method (FACT, 1)
    {
        If (LLessEqual (Arg0, 1))
        {
            Return (1)
        }
        Return (Multiply (Arg0, FACT (Subtract (Arg0, 1))))
    }

    /* Seven arguments and eight locals. */
    Method (SEVN, 7)
    {
        Store (Arg0, Local0)
        Store (Arg1, Local1)
        Store (Arg2, Local2)
        Store (Arg3, Local3)
        Store (Arg4, Local4)
        Store (Arg5, Local5)
        Store (Arg6, Local6)
        Store (Add (Local0, Local1), Local7)
        Return (Add (Add (Add (Local7, Local2), Add (Local3, Local4)), Add (Local5, Local6)))
    }

    /* A method that returns nothing, called for its side effect. */
    Method (BUMP)
    {
        Increment (CNT)
    }

    /* Switch, which the compiler turns into While and If with a temporary name. */
    Method (SWT, 1, Serialized)
    {
        Switch (ToInteger (Arg0))
        {
            Case (1)
            {
                Return ("one")
            }
            Case (Package () { 2, 3 })
            {
                Return ("two or three")
            }
            Default
            {
                Return ("other")
            }
        }
        Return ("none")
    }

    /* A name created inside a method exists only while it runs. */
    Method (TMPN, 1, Serialized)
    {
        Name (TVAL, 5)
        Add (TVAL, Arg0, TVAL)
        Return (TVAL)
    }

    Method (MAIN)
    {
        CHK (MAX2 (3, 9), 9, 1)
        CHK (MAX2 (12, 9), 12, 2)
        CHK (CLS (5), 1, 3)
        CHK (CLS (50), 2, 4)
        CHK (CLS (500), 3, 5)
        CHK (SUMN (10), 55, 6)
        CHK (SUMN (0), 0, 7)
        CHK (ODDS (8), 16, 8)
        CHK (ODDS (100), 64, 9)
        CHK (FIND (42), 607, 10)
        CHK (FIND (97), Ones, 11)
        CHK (FACT (10), 3628800, 12)
        CHK (SEVN (1, 2, 3, 4, 5, 6, 7), 28, 13)
        BUMP ()
        BUMP ()
        CHK (CNT, 2, 14)
        CHK (SWT (1), "one", 15)
        CHK (SWT (3), "two or three", 16)
        CHK (SWT (7), "other", 17)
        CHK (TMPN (1), 6, 18)
        CHK (TMPN (2), 7, 19)

        /* An If whose predicate is a method call. */
        If (MAX2 (0, 0))
        {
            CHK (1, 0, 21)
        }

        /* Nested Else chains. */
        Store (0, Local0)
        If (LEqual (Local0, 1))
        {
            Store (10, Local1)
        }
        Else
        {
            If (LEqual (Local0, 0))
            {
                Store (20, Local1)
            }
            Else
            {
                Store (30, Local1)
            }
        }
        CHK (Local1, 20, 22)

        Return (FAIL)
    }
}
