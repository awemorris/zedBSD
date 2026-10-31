/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * A thermal zone that overheats, for the QEMU test of the thermal
 * emergency: added to the machine with QEMU's -acpitable.  Each read of
 * _TMP counts one; the zone reads 40 degrees for the first 24 reads (two
 * minutes at the driver's five-second period, enough for the login), then
 * 10 degrees more at each read up to 100.  _CRT is 90 degrees, so the
 * fifth and sixth rising reads are the two in a row at the critical level.
 * Temperatures are in tenths of a kelvin.
 */
DefinitionBlock ("", "SSDT", 2, "ZEDBSD", "HOTZONE", 1)
{
    Scope (\_TZ)
    {
        Name (HZCN, Zero)

        ThermalZone (HOTZ)
        {
            Method (_TMP, 0, Serialized)
            {
                HZCN++
                Local0 = 3132
                If (HZCN > 24)
                {
                    Local1 = (HZCN - 24) * 100
                    If (Local1 > 600)
                    {
                        Local1 = 600
                    }

                    Local0 += Local1
                }

                Return (Local0)
            }

            Method (_CRT, 0, NotSerialized)
            {
                Return (3632)
            }
        }
    }
}
