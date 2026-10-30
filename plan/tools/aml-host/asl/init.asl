/*
 * WS049 AML test: device initialization by _STA and _INI (ACPI 6.5
 * section 6.5.1).
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.  aml-host runs it with --init (init.args), as the kernel
 * initializes the devices after loading; acpiexec does so during its own
 * initialization.  Each _INI appends a letter to ORDR.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "INIT", 1)
{
    Name (FAIL, 0)
    Name (ORDR, "")

    Method (MARK, 1)
    {
        Concatenate (ORDR, Arg0, ORDR)
    }

    Scope (\_SB)
    {
        Method (_INI)
        {
            MARK ("S")
        }

        /* Present: its _INI runs, then its children's. */
        Device (DEV1)
        {
            Name (_HID, "ZED0011")
            Method (_INI)
            {
                MARK ("1")
            }
            Device (DV11)
            {
                Name (_ADR, 1)
                Method (_INI)
                {
                    MARK ("a")
                }
            }
        }

        /* Neither present nor functioning: skipped with its children. */
        Device (DEV2)
        {
            Name (_HID, "ZED0012")
            Name (_STA, 0)
            Method (_INI)
            {
                MARK ("2")
            }
            Device (DV21)
            {
                Name (_ADR, 1)
                Method (_INI)
                {
                    MARK ("b")
                }
            }
        }

        /* Functioning but not present: only its children run. */
        Device (DEV3)
        {
            Name (_HID, "ZED0013")
            Name (_STA, 0x08)
            Method (_INI)
            {
                MARK ("3")
            }
            Device (DV31)
            {
                Name (_ADR, 1)
                Method (_INI)
                {
                    MARK ("c")
                }
            }
        }

        /* A _STA method that says present. */
        Device (DEV4)
        {
            Name (_HID, "ZED0014")
            Method (_STA)
            {
                Return (0x0F)
            }
            Method (_INI)
            {
                MARK ("4")
            }
        }
    }

    Method (MAIN)
    {
        If (LNotEqual (ORDR, "S1ac4"))
        {
            Store (1, FAIL)
        }
        Return (FAIL)
    }
}
