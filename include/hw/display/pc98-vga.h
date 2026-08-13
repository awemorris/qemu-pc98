/*
 * QEMU NEC PC-9821 display (GDC/GRCG)
 *
 * Copyright (c) 1998 Yui (Neko Project II)
 * Copyright (c) 2009 TAKEDA, toshiya (QEMU/9821)
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_PC98_VGA_H
#define HW_DISPLAY_PC98_VGA_H

#include "qemu/typedefs.h"
#include "hw/i386/pc98.h"
#include "system/memory.h"

typedef struct VGAState Pc98VgaState;

/*
 * Create the PC-98 display subsystem: registers the I/O ports on
 * @system_io, creates the graphic console and the vsync timer (@irq is
 * the IRQ2 line).  Fills @regions with the VRAM MemoryRegions for the
 * board (pc98-mem) to map.
 */
Pc98VgaState *pc98_vga_init(MemoryRegion *system_io, qemu_irq irq,
                            bool pegc_post_compat,
                            bool pegc_enabled,
                            Pc98VgaRegions *regions);

/* Let a board-level monitor relay reuse and render the GDC console. */
QemuConsole *pc98_vga_get_console(Pc98VgaState *s);
bool pc98_vga_update_console(Pc98VgaState *s);
void pc98_vga_invalidate_console(Pc98VgaState *s);
bool pc98_vga_display_enabled(Pc98VgaState *s);

void pc98_vga_select_ems(void *opaque, uint32_t value);

#endif
