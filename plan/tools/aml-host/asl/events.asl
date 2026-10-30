/*
 * WS049 AML test: GPEs and their methods, the fixed power button, and the
 * Embedded Controller (its address space, _REG and _Qxx).
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.  aml-host simulates the hardware at q35's ports and plays
 * the events of events.args before MAIN runs: GPEs 5, 7 and 9, an EC
 * query 0x42 on the EC's GPE 0x16, and a press of the power button.
 * acpiexec has no such hardware (events.harness-only).
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "EVENTS", 1)
{
    Name (FAIL, 0)
    Name (L05C, 0)
    Name (E07C, 0)
    Name (L09C, 0)

    Scope (\_GPE)
    {
        /* A level GPE whose method notifies the power button device. */
        Method (_L05)
        {
            Increment (L05C)
            Notify (\_SB.PWRB, 0x80)
        }

        /* An edge GPE. */
        Method (_E07)
        {
            Increment (E07C)
        }

        /* A GPE only a device's _PRW names: it wakes, and is not enabled at run time. */
        Method (_L09)
        {
            Increment (L09C)
        }
    }

    Scope (\_SB)
    {
        Device (PWRB)
        {
            Name (_HID, EisaId ("PNP0C0C"))
        }

        Device (WDEV)
        {
            Name (_HID, "ZED0030")
            Name (_PRW, Package () { 0x09, 0x03 })
        }

        Device (EC0)
        {
            Name (_HID, EisaId ("PNP0C09"))
            Name (_CRS, ResourceTemplate ()
            {
                IO (Decode16, 0x0062, 0x0062, 0x00, 0x01)
                IO (Decode16, 0x0066, 0x0066, 0x00, 0x01)
            })
            Name (_GPE, 0x16)
            Name (ECOK, 0)
            Name (Q42C, 0)

            /* Records that the EmbeddedControl space is there. */
            Method (_REG, 2)
            {
                If (LEqual (Arg0, 3))
                {
                    Store (Arg1, ECOK)
                }
            }

            OperationRegion (ECOR, EmbeddedControl, 0, 0x100)
            Field (ECOR, ByteAcc, Lock, Preserve)
            {
                Offset (0x10),
                TEMP, 8,
                Offset (0x20),
                LIDS, 1,
                , 7,
                Offset (0x30),
                CNT0, 16
            }

            /* The lid event. */
            Method (_Q42)
            {
                Increment (Q42C)
                Notify (\_SB.LID0, 0x80)
            }
        }

        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Method (_LID)
            {
                Return (\_SB.EC0.LIDS)
            }
        }
    }

    /* The GPE block's status and enable registers, as firmware may read them. */
    OperationRegion (GPEB, SystemIO, 0x0620, 0x10)
    Field (GPEB, ByteAcc, NoLock, Preserve)
    {
        GS0, 8,
        GS1, 8,
        GS2, 8,
        Offset (0x08),
        GE0, 8,
        GE1, 8,
        GE2, 8
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
        /* Each runtime GPE's method ran once; the wake-only one did not. */
        CHK (L05C, 1, 1)
        CHK (E07C, 1, 2)
        CHK (L09C, 0, 3)

        /* The handled GPEs are cleared and enabled again; GPE 9 stays masked and set. */
        CHK (GS0, 0, 4)
        CHK (GE0, 0xA0, 5)
        CHK (GS1, 0x02, 6)
        CHK (GE1, 0, 7)
        CHK (GE2, 0x40, 8)

        /* The EC's space is connected and reads the EC's bytes. */
        CHK (\_SB.EC0.ECOK, 1, 10)
        CHK (\_SB.EC0.TEMP, 0x3C, 11)
        CHK (\_SB.LID0._LID (), 1, 12)
        Store (0x1234, \_SB.EC0.CNT0)
        CHK (\_SB.EC0.CNT0, 0x1234, 13)

        /* The EC's query ran its _Q42. */
        CHK (\_SB.EC0.Q42C, 1, 20)

        Return (FAIL)
    }
}
