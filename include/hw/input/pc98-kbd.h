/*
 * QEMU NEC PC-9821 keyboard
 *
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_INPUT_PC98_KBD_H
#define HW_INPUT_PC98_KBD_H

#include "hw/isa/isa.h"
#include "qom/object.h"

#define TYPE_PC98_KBD "pc98-kbd"
OBJECT_DECLARE_SIMPLE_TYPE(Pc98KbdState, PC98_KBD)

#endif
