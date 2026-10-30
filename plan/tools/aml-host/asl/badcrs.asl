/*
 * WS049 AML test (ws049-p017): drv_acpi_resources_walk() refuses resource
 * templates whose descriptors run past their end, without reading past it.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * \RES1's _CRS is a fixed 32-bit memory descriptor cut after its length
 * bytes; aml-host walks it (badcrs.args) and must print the refusal (EIO,
 * badcrs.output).  MAIN only returns 0.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "BADCRS", 1)
{
    Device (RES1)
    {
        Name (_HID, "ZED0032")
        Name (_CRS, Buffer () {0x86, 0x09, 0x00, 0x01, 0x00, 0x00})
    }

    Method (MAIN, 0)
    {
        Return (0)
    }
}
