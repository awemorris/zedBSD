# WS009-p007: current architecture and control-device contracts

Date: 2026-09-09
Status: completed (q150); [results](results.md)
Parent: [WS009](../ws.md)
Timebox: 60 active minutes

Publish the current independent implementation/HAL/kernel ownership overview
(DOC-10/11), bounded compatibility profile (DOC-12), and console/graphics/system
control-device references (DOC-50/51/52). Reconcile networking references with
the current net/networkd/dhcpc boundaries (DOC-55). Inventory GPU declarations
and clearly retain the producer's manual hold rather than invent an installed
interface (DOC-54).

Every operation and ownership claim must point to current source/header/tests.
Do not claim POSIX certification, BSD on-disk compatibility, user-page direct
I/O, UAS transport, native GPU acceleration or a complete installer without
evidence. Fixed ABI widths, permissions, capability/error discovery and cleanup
are part of each control-device reference. Do not use historical split source
paths after the refactoring.

Run the maintained relative-link checker for all product Markdown and the
modified planning records. Repair stale product anchors against current source;
retain historical plan material. Build/runtime evidence belongs to producer
phases; documentation-only changes do not require repeating those campaigns.
Remaining producer-dependent claims stay scoped and uncleared.
