/*
 * QEMU NEC PC-9821 Window Accelerator Board (WAB)
 *
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_PC98_WAB_H
#define HW_DISPLAY_PC98_WAB_H

#include "hw/isa/isa.h"

#define TYPE_PC98_WAB "pc98-wab"

/*
 * Create the Window Accelerator Board: a Cirrus CL-GD5426 behind the NEC LSI.
 * Attaches the remapped register ports and the display relay to the I/O space
 * and the linear framebuffer window to the system memory.
 */
void pc98_wab_init(ISABus *bus);

#endif
