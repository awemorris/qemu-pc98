/*
 * QEMU NEC PC-98 LGY-98 C-bus Ethernet
 *
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_NET_PC98_LGY98_H
#define HW_NET_PC98_LGY98_H

#include "hw/isa/isa.h"

#define TYPE_PC98_LGY98 "pc98-lgy98"

/*
 * Create the LGY-98 C-bus Ethernet card (NE2000/DP8390-compatible), a
 * wrapper around the shared NE2000 core mapped onto the PC-98 port block
 * at 0x10D0.  @irq is the C-bus interrupt line (LGY-98 default INT is IRQ6).
 */
ISADevice *pc98_lgy98_init(ISABus *bus, qemu_irq irq);

#endif
