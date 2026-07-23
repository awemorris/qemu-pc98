/*
 * QEMU NEC PC-9821 system port (uPD4990A RTC, system/printer 8255, SDIP)
 *
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_PC98_SYS_H
#define HW_MISC_PC98_SYS_H

#include "hw/isa/isa.h"
#include "qom/object.h"

#define TYPE_PC98_SYS "pc98-sys"
OBJECT_DECLARE_SIMPLE_TYPE(Pc98SysState, PC98_SYS)

/*
 * Returns true when the firmware has armed the shutdown flag (system-port
 * port-C bits 7 and 5 both set).  The board's software-reset port (0x534)
 * uses it to choose between a full system reset and a CPU-only reset.
 */
bool pc98_sys_shutdown_armed(Pc98SysState *s);

#endif
