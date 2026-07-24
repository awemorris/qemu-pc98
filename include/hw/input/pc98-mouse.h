/*
 * QEMU NEC PC-98 bus mouse (uPD8255 PPI, IRQ13)
 *
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_INPUT_PC98_MOUSE_H
#define HW_INPUT_PC98_MOUSE_H

#include "hw/isa/isa.h"

#define TYPE_PC98_MOUSE "pc98-mouse"

/*
 * Create the PC-98 bus mouse (uPD8255 PPI at 0x7FD9/DB/DD/DF).  @irq is the
 * "bus mouse" interrupt line (IRQ13); the card pulses it at a fixed rate
 * while the driver has the interrupt enabled (Port C bit 0x10 low).
 */
ISADevice *pc98_mouse_init(ISABus *bus, qemu_irq irq);

#endif
