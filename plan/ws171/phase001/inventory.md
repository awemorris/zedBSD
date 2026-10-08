| # | function | hal.h line | comment | words | defined in |
| --- | --- | --- | --- | --- | --- |
| 1 | `hal_strlen` | 49 | none | 0 | amd64, i386, m68k |
| 2 | `hal_memset` | 53 | none | 0 | amd64, i386, m68k |
| 3 | `hal_memset16` | 59 | none | 0 | amd64, i386, m68k |
| 4 | `hal_memset32` | 65 | none | 0 | amd64, i386, m68k |
| 5 | `hal_memcpy` | 71 | none | 0 | amd64, i386, m68k |
| 6 | `hal_putchar` | 77 | none | 0 | amd64, i386 |
| 7 | `hal_putc` | 88 | short | 38 | amd64, i386, arm64 |
| 8 | `hal_puts` | 92 | none | 0 | amd64, i386 |
| 9 | `hal_printf` | 96 | none | 0 | amd64, i386, m68k |
| 10 | `hal_assert` | 101 | none | 0 | amd64, i386, m68k |
| 11 | `hal_fatal` | 107 | none | 0 | amd64, i386, m68k |
| 12 | `hal_cpu_start_others` | 133 | placeholder | 3 | amd64, i386, shared:cpu-up.c |
| 13 | `hal_cpu_count` | 139 | short | 6 | amd64, i386, shared:cpu-up.c |
| 14 | `hal_cpu_current` | 145 | short | 11 | amd64, i386, shared:cpu-up.c |
| 15 | `hal_cpu_ready_mask` | 151 | placeholder | 3 | amd64, i386, shared:cpu-up.c |
| 16 | `hal_cpu_notify` | 170 | contract | 102 | amd64, i386, shared:cpu-up.c |
| 17 | `hal_cpu_notify_mask` | 178 | short | 23 | amd64, i386, shared:cpu-up.c |
| 18 | `hal_cpu_park` | 185 | placeholder | 3 | amd64, i386, shared:cpu-up.c |
| 19 | `hal_cpu_panic_all` | 191 | placeholder | 3 | amd64, i386, shared:cpu-up.c |
| 20 | `hal_cpu_idle` | 197 | short | 10 | amd64, i386, sparcv9 |
| 21 | `hal_cpu_idle_suspend_supported` | 211 | contract | 70 | amd64, shared:idle-suspend-unsupported.c |
| 22 | `hal_cpu_idle_suspend` | 232 | contract | 129 | amd64, shared:idle-suspend-unsupported.c |
| 23 | `hal_halt` | 241 | short | 15 | amd64, i386, m68k |
| 24 | `hal_cpu_mask_zero` | 247 | short | 7 | (macro or none) |
| 25 | `hal_cpu_mask_fill` | 260 | short | 7 | (macro or none) |
| 26 | `hal_cpu_mask_set` | 273 | short | 7 | (macro or none) |
| 27 | `hal_cpu_mask_clear` | 285 | short | 7 | (macro or none) |
| 28 | `hal_cpu_mask_test` | 297 | short | 7 | (macro or none) |
| 29 | `hal_irq_disable` | 334 | short | 8 | amd64, i386, arm64, sparcv9 |
| 30 | `hal_irq_enable` | 340 | short | 3 | amd64, i386, arm64, sparcv9 |
| 31 | `hal_irq_set_affinity` | 346 | short | 3 | amd64, i386, arm64, sparcv9, m68k |
| 32 | `hal_irq_get_affinity` | 354 | short | 3 | amd64, i386, arm64, sparcv9, m68k |
| 33 | `hal_irq_set_mode` | 372 | contract | 45 | amd64, i386, arm64, sparcv9, m68k |
| 34 | `hal_irq_set_wake` | 396 | contract | 129 | amd64, shared:idle-suspend-unsupported.c |
| 35 | `hal_irq_suspend` | 424 | contract | 185 | amd64, shared:idle-suspend-unsupported.c |
| 36 | `hal_irq_resume` | 433 | contract | 30 | amd64, shared:idle-suspend-unsupported.c |
| 37 | `hal_irq_mask` | 439 | short | 4 | amd64, i386, arm64, m68k |
| 38 | `hal_irq_unmask` | 446 | short | 4 | amd64, i386, arm64, m68k |
| 39 | `hal_irq_register` | 453 | short | 7 | amd64, i386, arm64 |
| 40 | `hal_irq_unregister` | 462 | short | 7 | amd64, i386, arm64 |
| 41 | `hal_irq_register_msi` | 473 | short | 17 | amd64, i386, arm64, sparcv9, m68k |
| 42 | `hal_irq_unregister_msi` | 485 | short | 8 | amd64, i386, arm64, sparcv9, m68k |
| 43 | `hal_irq_alloc_msi` | 498 | contract | 84 | amd64, i386, arm64, sparcv9, m68k |
| 44 | `hal_irq_attach_msi` | 509 | short | 23 | amd64, i386, arm64, sparcv9, m68k |
| 45 | `hal_irq_detach_msi_sync` | 522 | contract | 63 | amd64, i386, arm64, sparcv9, m68k |
| 46 | `hal_irq_free_msi` | 534 | contract | 42 | amd64, i386, arm64, sparcv9, m68k |
| 47 | `hal_irq_send_eoi` | 541 | short | 6 | amd64, i386, arm64, sparcv9, m68k |
| 48 | `hal_rtc_read_epoch_time` | 566 | short | 10 | amd64, i386, arm64, sparcv9, m68k |
| 49 | `hal_rtc_read_counter` | 585 | contract | 104 | amd64, i386, arm64, sparcv9, m68k |
| 50 | `hal_pmem_get_total_size` | 604 | short | 5 | amd64, i386, arm64, m68k |
| 51 | `hal_pmem_alloc` | 610 | short | 5 | amd64, i386, arm64, sparcv9, m68k |
| 52 | `hal_pmem_to_kernel` | 624 | contract | 50 | amd64, i386, arm64 |
| 53 | `hal_pmem_alloc_limited` | 636 | contract | 47 | amd64, i386, arm64 |
| 54 | `hal_pmem_free` | 648 | short | 21 | amd64, i386, arm64, sparcv9, m68k |
| 55 | `hal_pmem_map_uncached` | 665 | contract | 111 | arm64 |
| 56 | `hal_pmem_unmap_uncached` | 677 | short | 32 | arm64 |
| 57 | `hal_space_create` | 729 | short | 9 | amd64, i386, m68k |
| 58 | `hal_space_destroy` | 737 | short | 33 | amd64, i386, m68k |
| 59 | `hal_space_switch` | 744 | short | 12 | amd64, i386, m68k |
| 60 | `hal_space_map` | 757 | contract | 58 | amd64, i386, m68k |
| 61 | `hal_space_unmap` | 768 | short | 9 | amd64, i386, m68k |
| 62 | `hal_space_map_device` | 781 | contract | 44 | amd64, i386, arm64 |
| 63 | `hal_space_unmap_device` | 791 | short | 6 | amd64, i386, arm64 |
| 64 | `hal_space_prot` | 799 | short | 8 | amd64, i386, m68k |
| 65 | `hal_space_prot_query` | 817 | short | 76 | amd64, i386, m68k |
| 66 | `hal_space_query` | 829 | short | 13 | amd64, i386, m68k |
| 67 | `hal_space_clear_flags` | 838 | short | 9 | amd64, i386, m68k |
| 68 | `hal_space_flush_tlb` | 853 | short | 50 | amd64, i386, m68k |
| 69 | `hal_space_flush_tlb_range` | 860 | short | 7 | amd64, i386, m68k |
| 70 | `hal_space_get_page_size` | 871 | short | 13 | amd64, i386, m68k |
| 71 | `hal_space_get_user_range` | 878 | short | 8 | amd64, i386, m68k |
| 72 | `hal_task_create_for_init_context` | 904 | contract | 44 | amd64, i386, arm64 |
| 73 | `hal_task_create` | 910 | short | 3 | amd64, i386, m68k |
| 74 | `hal_task_destroy` | 920 | short | 3 | amd64, i386, m68k |
| 75 | `hal_task_context_switch` | 927 | short | 4 | amd64, i386, m68k |
| 76 | `hal_task_fork_current` | 938 | contract | 29 | amd64, i386, m68k |
| 77 | `hal_task_exec_current` | 946 | short | 9 | amd64, i386, m68k |
| 78 | `hal_task_exec_validate` | 955 | short | 9 | amd64, i386, m68k |
| 79 | `hal_task_get_user_stack` | 964 | short | 6 | amd64, i386 |
| 80 | `hal_task_get_user_context` | 971 | short | 11 | amd64, i386, arm64 |
| 81 | `hal_task_signal_enter` | 980 | placeholder | 3 | amd64, i386, m68k |
| 82 | `hal_task_signal_return` | 993 | placeholder | 3 | amd64, i386, m68k |
| 83 | `hal_task_get_current` | 1001 | short | 4 | amd64, i386 |
| 84 | `hal_task_set_tls` | 1007 | short | 3 | amd64, i386 |
| 85 | `hal_task_get_tls` | 1015 | short | 3 | amd64, i386 |
| 86 | `hal_task_set_private` | 1022 | short | 10 | amd64, i386 |
| 87 | `hal_task_get_private` | 1027 | none | 0 | amd64, i386 |
| 88 | `hal_task_get_space` | 1031 | none | 0 | amd64, i386 |
| 89 | `hal_task_transfer` | 1035 | none | 0 | amd64, i386, m68k |
| 90 | `hal_task_get_user_gpregs` | 1059 | contract | 61 | amd64, arm64 |
| 91 | `hal_task_set_user_gpregs` | 1064 | none | 0 | amd64, arm64 |
| 92 | `hal_task_get_user_fpregs` | 1069 | none | 0 | amd64, arm64 |
| 93 | `hal_task_set_user_fpregs` | 1074 | none | 0 | amd64, arm64 |
| 94 | `hal_task_get_user_vregs` | 1079 | none | 0 | amd64, arm64 |
| 95 | `hal_task_set_user_vregs` | 1084 | none | 0 | amd64, arm64 |
| 96 | `hal_task_set_single_step` | 1097 | short | 66 | amd64, arm64 |
| 97 | `hal_task_get_single_step` | 1102 | none | 0 | amd64, arm64 |
| 98 | `hal_task_set_debug_points` | 1143 | contract | 133 | amd64, arm64 |
| 99 | `hal_task_get_debug_points` | 1149 | none | 0 | amd64, arm64 |
| 100 | `hal_mb` | 1166 | short | 2 | amd64, i386, m68k |
| 101 | `hal_rmb` | 1169 | none | 0 | amd64, i386 |
| 102 | `hal_wmb` | 1172 | none | 0 | amd64, i386 |
| 103 | `hal_io_mb` | 1175 | none | 0 | amd64, i386 |
| 104 | `hal_io_rmb` | 1178 | none | 0 | amd64, i386 |
| 105 | `hal_io_wmb` | 1181 | none | 0 | amd64, i386 |
| 106 | `hal_icache_invalidate_range` | 1187 | short | 2 | amd64, m68k |
| 107 | `hal_dcache_clean_range` | 1192 | none | 0 | m68k |
| 108 | `hal_dcache_invalidate_range` | 1197 | none | 0 | m68k |
| 109 | `hal_dcache_clean_invalidate_range` | 1202 | none | 0 | m68k |
| 110 | `hal_sync_instruction_stream` | 1207 | none | 0 | m68k |
| 111 | `hal_io_inp8` | 1217 | short | 1 | x86(common) |
| 112 | `hal_io_inp16` | 1221 | none | 0 | x86(common) |
| 113 | `hal_io_inp32` | 1225 | none | 0 | x86(common) |
| 114 | `hal_io_outp8` | 1229 | none | 0 | x86(common) |
| 115 | `hal_io_outp16` | 1234 | none | 0 | x86(common) |
| 116 | `hal_io_outp32` | 1239 | none | 0 | x86(common) |
| 117 | `hal_mmio_read8` | 1244 | none | 0 | amd64, i386, m68k |
| 118 | `hal_mmio_read16` | 1248 | none | 0 | amd64, m68k |
| 119 | `hal_mmio_read32` | 1252 | none | 0 | amd64, m68k |
| 120 | `hal_mmio_read64` | 1256 | none | 0 | amd64, m68k |
| 121 | `hal_mmio_write8` | 1260 | none | 0 | amd64, i386 |
| 122 | `hal_mmio_write16` | 1265 | none | 0 | amd64 |
| 123 | `hal_mmio_write32` | 1270 | none | 0 | amd64 |
| 124 | `hal_mmio_write64` | 1275 | none | 0 | amd64 |
| 125 | `hal_get_arch_handoff` | 1290 | short | 21 | amd64, i386, arm64, sparcv9, m68k |
| 126 | `hal_reset` | 1297 | short | 6 | m68k |
| 127 | `hal_poweroff` | 1303 | short | 4 | m68k |
| 128 | `hal_panic` | 1309 | short | 5 | (macro or none) |
| 129 | `hal_entropy_fill` | 1318 | short | 26 | amd64, i386, m68k |
| 130 | `hal_get_memstat` | 1361 | short | 5 | amd64, i386, arm64 |

Totals: contract 20, none 42, placeholder 6, short 62, all 130
