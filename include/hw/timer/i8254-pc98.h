/*
 * QEMU NEC PC-9821 8253/8254 interval timer
 *
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_TIMER_I8254_PC98_H
#define HW_TIMER_I8254_PC98_H

#include "hw/isa/isa.h"

/*
 * Create a PC-98 PIT: counters at 0x71/0x73/0x75/0x77 (mirror 0x3fd9..),
 * counter 0 output connected to @alt_irq (the board's IRQ0).
 */
ISADevice *pc98_pit_init(ISABus *bus, qemu_irq alt_irq);

#endif
