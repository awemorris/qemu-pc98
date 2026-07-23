/*
 * QEMU NEC PC-9821 board definitions
 *
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_I386_PC98_H
#define HW_I386_PC98_H

#include "system/memory.h"

/*
 * Display regions provided by the VGA device (or RAM placeholders until
 * the VGA model is wired up).  pc98-mem maps aliases of these into the
 * movable RAM windows and the fixed VRAM slots of the low 1 MiB.
 */
typedef struct Pc98VgaRegions {
    MemoryRegion *tvram;       /* text VRAM,        0x08000 bytes (0xa0000) */
    MemoryRegion *vram_a8000;  /* planar VRAM,      0x08000 bytes (0xa8000) */
    MemoryRegion *vram_b0000;  /* planar VRAM,      0x10000 bytes (0xb0000) */
    MemoryRegion *vram_e0000;  /* planar VRAM,      0x08000 bytes (0xe0000) */
    MemoryRegion *vram_f00000; /* PEGC linear VRAM, 0x80000 bytes (0xf00000) */
} Pc98VgaRegions;

typedef struct Pc98MemState Pc98MemState;

/*
 * Set up the PC-98 memory controller: loads the ITF/BIOS ROM images,
 * builds the low-1MiB bank topology, the 16MB-space and top-of-4G
 * mirrors, and registers the bank-switch I/O ports.
 */
Pc98MemState *pc98_mem_init(MemoryRegion *system_memory,
                            MemoryRegion *system_io,
                            MemoryRegion *ram,
                            uint64_t ram_size,
                            const Pc98VgaRegions *vga,
                            uint8_t hd_connect,
                            void (*ems_select)(void *opaque, uint32_t value),
                            void *ems_opaque);

#endif
