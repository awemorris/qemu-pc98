/*
 * QEMU NEC PC-9821 IDE interface
 *
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_IDE_PC98_IDE_H
#define HW_IDE_PC98_IDE_H

#include "hw/isa/isa.h"
#include "system/blockdev.h"
#include "qom/object.h"

#define TYPE_PC98_IDE "pc98-ide"
OBJECT_DECLARE_SIMPLE_TYPE(Pc98IdeState, PC98_IDE)

/*
 * Create the PC-98 built-in IDE interface (2 banks x 2 drives), IRQ 9.
 * @hd_table holds up to 4 drives (bank1 master/slave, bank2 master/slave).
 */
ISADevice *pc98_ide_init(ISABus *bus, DriveInfo **hd_table, qemu_irq irq);

/*
 * Connection bitmap for the BIOS: bit i set if drive i (0..3) is an
 * attached hard disk.  Used by the machine's port 0xf0 read and to build
 * the BIOS work-area "hd_connect" value.
 */
uint8_t pc98_ide_connected(Pc98IdeState *s);

#endif
