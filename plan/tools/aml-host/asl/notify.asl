/*
 * WS049 AML test: Notify reaches the handlers drivers install, and _OSI
 * answers.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.  aml-host installs a printing handler on \DEV0 and \_SB.PWRB
 * (notify.args) and must print their notifications (notify.output); a
 * notification of a node without a handler is dropped.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "NOTIFY", 1)
{
    Name (FAIL, 0)

    Device (DEV0)
    {
        Name (_HID, "ZED0020")
    }

    Device (DEV1)
    {
        Name (_HID, "ZED0021")
    }

    Scope (\_SB)
    {
        Device (PWRB)
        {
            Name (_HID, EisaId ("PNP0C0C"))
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
        Notify (DEV0, 0x80)
        Notify (\_SB.PWRB, 0x02)
        Notify (DEV1, 0x81)

        /* _OSI knows the Windows releases and nothing else of the operating system. */
        CHK (\_OSI ("Windows 2022"), Ones, 1)
        CHK (\_OSI ("Windows 2009"), Ones, 2)
        CHK (\_OSI ("Linux"), 0, 3)
        CHK (\_OSI ("Darwin"), 0, 4)
        CHK (\_OSI ("Windows 2099"), 0, 5)

        Return (FAIL)
    }
}
