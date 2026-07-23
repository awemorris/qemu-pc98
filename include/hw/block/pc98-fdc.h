/*
 * QEMU NEC PC-9821 floppy disk controller
 *
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_BLOCK_PC98_FDC_H
#define HW_BLOCK_PC98_FDC_H

#include "hw/block/fdc.h"
#include "hw/isa/isa.h"
#include "system/blockdev.h"
#include "qom/object.h"

#define TYPE_PC98_FDC "pc98-fdc"
OBJECT_DECLARE_SIMPLE_TYPE(Pc98FdcState, PC98_FDC)

void pc98_fdc_init_drives(ISADevice *fdc, DriveInfo **fds);

#endif
