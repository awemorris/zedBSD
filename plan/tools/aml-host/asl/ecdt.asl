/*
 * WS049 AML test: an Embedded Controller the ECDT describes, reachable
 * from _INI.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.  aml-host starts the EC from support/ecdt.asl's ECDT before
 * _REG and _INI (ecdt.args), so the EC's _REG runs once with the other
 * spaces and DEV1's _INI reads a byte of the simulated EC.  Attaching the
 * device later adds its GPE: the query 0x42 of ecdt.args runs _Q42.
 * Without the ECDT, _INI would find the space not connected yet.  acpiexec
 * has no EC hardware (ecdt.harness-only).
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "ECDT", 1)
{
    Name (FAIL, 0)
    Name (INIV, 0)
    Name (REGC, 0)

    Scope (\_SB)
    {
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

            /* Records that the EmbeddedControl space is there, and how often it was told. */
            Method (_REG, 2)
            {
                If (LEqual (Arg0, 3))
                {
                    Store (Arg1, ECOK)
                    Increment (REGC)
                }
            }

            OperationRegion (ECOR, EmbeddedControl, 0, 0x100)
            Field (ECOR, ByteAcc, Lock, Preserve)
            {
                Offset (0x10),
                TEMP, 8
            }

            Method (_Q42)
            {
                Increment (Q42C)
            }
        }

        /* A device whose _INI needs the EC, as laptop firmware's often do. */
        Device (DEV1)
        {
            Name (_HID, "ZED0040")
            Method (_INI)
            {
                If (\_SB.EC0.ECOK)
                {
                    Store (\_SB.EC0.TEMP, INIV)
                }
            }
        }
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
        /* _REG ran once, before _INI, which read the EC. */
        CHK (\_SB.EC0.ECOK, 1, 1)
        CHK (REGC, 1, 2)
        CHK (INIV, 0x3C, 3)

        /* The device's GPE was added: the query ran its method. */
        CHK (\_SB.EC0.Q42C, 1, 4)

        /* The EC still answers. */
        CHK (\_SB.EC0.TEMP, 0x3C, 5)
        Return (FAIL)
    }
}
