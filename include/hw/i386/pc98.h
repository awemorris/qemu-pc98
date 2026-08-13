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

typedef struct PCIBus PCIBus;

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
    MemoryRegion *pegc_post;    /* Xa7 POST PEGC backing, 0x80000 bytes */
    void (*set_pegc_post_active)(void *opaque, bool active);
    void *pegc_opaque;
} Pc98VgaRegions;

typedef struct Pc98MemState Pc98MemState;

/* Install a C-Bus option ROM behind the machine's POST shadow-RAM gate. */
void pc98_mem_register_cbus_rom(Pc98MemState *s, MemoryRegion *rom,
                                hwaddr address);

/* PCI host bridge for the PCI-equipped PC-9821 machines (pc98-pci). */
#define TYPE_PC98_PCI_HOST "pc98-pcihost"

/*
 * Host bridge (dev0) config register 0x64: the D000-segment shadow control.
 * Called by the PCI host bridge; opaque is the Pc98MemState.
 */
void pc98_mem_set_d000_shadow(void *opaque, uint8_t bits);

/*
 * Host bridge (dev0) config byte 0x69 bit 4: allow the Xa7 ITF to update
 * the IDE probe bitmap latch at 0xf8e90.
 */
void pc98_mem_set_bios_probe_write(void *opaque, bool enable);
void pc98_mem_set_a20_wrap(void *opaque, bool wrap);

/* Give the PCI host bridge the memory-controller state for reg 0x64. */
void pc98_pci_set_d000_mem(void *mem);

/* Return the root bus created by a realized PC-98 PCI host bridge. */
PCIBus *pc98_pci_get_bus(DeviceState *host);

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
                            bool has_pci,
                            bool pegc_post_compat,
                            bool pegc_enabled,
                            void (*ems_select)(void *opaque, uint32_t value),
                            void *ems_opaque);

#endif
