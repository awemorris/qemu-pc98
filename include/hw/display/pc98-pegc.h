/*
 * NEC PC-9821 PEGC planar accelerator
 *
 * Copyright (c) 1999-2025, NP2 developer team
 * Original PEGC implementation: Neko Project 21/W rev.52 and later,
 * developed by SimK.
 *
 * QEMU port and integration:
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HW_DISPLAY_PC98_PEGC_H
#define HW_DISPLAY_PC98_PEGC_H

#define PC98_PEGC_SOURCE_FIFO_SIZE 96

typedef struct Pc98PegcPlaneState {
    uint8_t plane_access;
    uint16_t rop;
    uint8_t data_select;
    uint32_t mask;
    uint16_t length;
    uint16_t shift;
    uint8_t palette1;
    uint8_t palette2;
    uint32_t pattern[8];

    uint8_t source_fifo[PC98_PEGC_SOURCE_FIFO_SIZE];
    uint8_t source_len;
    uint16_t remaining;
    uint8_t transfer_active;
    uint8_t first_write;
} Pc98PegcPlaneState;

void pc98_pegc_plane_reset(Pc98PegcPlaneState *s);
void pc98_pegc_plane_restart(Pc98PegcPlaneState *s);
uint32_t pc98_pegc_plane_reg_read(Pc98PegcPlaneState *s,
                                  uint32_t offset, unsigned size);
void pc98_pegc_plane_reg_write(Pc98PegcPlaneState *s, uint32_t offset,
                               uint32_t value, unsigned size);
uint32_t pc98_pegc_plane_vram_read(Pc98PegcPlaneState *s,
                                   const uint8_t *vram, uint32_t address,
                                   unsigned size);
uint8_t pc98_pegc_plane_vram_write(Pc98PegcPlaneState *s, uint8_t *vram,
                                   uint32_t address, uint32_t value,
                                   unsigned size);

#endif
