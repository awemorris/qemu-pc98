/*
 * QEMU NEC PC-9821 8259A interrupt controller wiring
 *
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_INTC_I8259_PC98_H
#define HW_INTC_I8259_PC98_H

#include "hw/isa/isa.h"

/*
 * Create the PC-98 master/slave 8259A pair:
 *   master at I/O 0x00/0x02, slave at 0x08/0x0a (2-byte register stride),
 *   the slave cascades into master input 7 (not 2 as on the AT), no ELCR.
 * Master output is connected to @parent_irq_in.  Returns an allocated
 * array of 16 input IRQs.
 */
qemu_irq *pc98_pic_setup(ISABus *bus, qemu_irq parent_irq_in);

#endif
