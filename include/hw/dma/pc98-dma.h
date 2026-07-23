/*
 * QEMU NEC PC-9821 DMA controller
 *
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DMA_PC98_DMA_H
#define HW_DMA_PC98_DMA_H

#include "hw/isa/isa.h"

void pc98_dma_init(ISABus *bus);

#endif
