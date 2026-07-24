/*
 * QEMU NEC PC-98 built-in WSS (Windows Sound System / Mate-X PCM)
 *
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_AUDIO_PC98_WSS_H
#define HW_AUDIO_PC98_WSS_H

#include "hw/isa/isa.h"

#define TYPE_PC98_WSS "pc98-wss"

/*
 * Create the PC-98 built-in WSS (Windows Sound System / Mate-X PCM), a
 * wrapper around the shared CS4231A codec mapped onto the PC-98 port block
 * at 0x0F40.  The routing is hardwired to IRQ12 (INT5) and DMA channel 1.
 */
void pc98_wss_init(ISABus *bus);

#endif
