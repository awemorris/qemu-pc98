/*
 * NEC PC-9821 PEGC planar accelerator
 *
 * Copyright (c) 1999-2025, NP2 developer team
 * Original PEGC implementation: Neko Project 21/W rev.52 and later,
 * developed by SimK.
 *
 * QEMU port integration:
 * Copyright (c) 2026 Awe Morris
 *
 * The register semantics and the shifter/ROP corrections in this port are
 * based on the SL9821 PEGC hardware analysis by Tomomi Sakai:
 * https://www.satotomi.com/sl9821/sl9821_tec5.html
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "qemu/osdep.h"
#include "hw/display/pc98-pegc.h"

enum {
    PEGC_PLANE_ACCESS = 0x04,
    PEGC_ROP          = 0x08,
    PEGC_DATA_SELECT  = 0x0a,
    PEGC_MASK         = 0x0c,
    PEGC_LENGTH       = 0x10,
    PEGC_SHIFT        = 0x12,
    PEGC_PALETTE1     = 0x14,
    PEGC_PALETTE2     = 0x18,
    PEGC_PATTERN      = 0x20,
    PEGC_PATTERN_END  = 0xa0,
};

#define PEGC_VRAM_MASK 0x7ffff

static unsigned pegc_cpu_bit(unsigned pixel)
{
    return (pixel & ~7U) | (7U - (pixel & 7U));
}

void pc98_pegc_plane_restart(Pc98PegcPlaneState *s)
{
    s->source_len = 0;
    s->remaining = (s->length & 0x0fff) + 1;
    s->transfer_active = 1;
    s->first_write = 1;
}

void pc98_pegc_plane_reset(Pc98PegcPlaneState *s)
{
    memset(s, 0, sizeof(*s));
    pc98_pegc_plane_restart(s);
}

static uint8_t pegc_reg_read_byte(Pc98PegcPlaneState *s, uint32_t offset)
{
    unsigned plane;

    switch (offset) {
    case PEGC_PLANE_ACCESS:
        return s->plane_access;
    case PEGC_ROP:
        return s->rop;
    case PEGC_ROP + 1:
        return s->rop >> 8;
    case PEGC_DATA_SELECT:
        return s->data_select;
    case PEGC_MASK ... PEGC_MASK + 3:
        return s->mask >> ((offset - PEGC_MASK) * 8);
    case PEGC_LENGTH:
        return s->length;
    case PEGC_LENGTH + 1:
        return s->length >> 8;
    case PEGC_SHIFT:
        return s->shift;
    case PEGC_SHIFT + 1:
        return s->shift >> 8;
    case PEGC_PALETTE1:
        return s->palette1;
    case PEGC_PALETTE2:
        return s->palette2;
    default:
        break;
    }

    if (offset >= PEGC_PATTERN && offset < PEGC_PATTERN + 8 * 4) {
        plane = (offset - PEGC_PATTERN) / 4;
        return s->pattern[plane] >> (((offset - PEGC_PATTERN) & 3) * 8);
    }
    return 0;
}

uint32_t pc98_pegc_plane_reg_read(Pc98PegcPlaneState *s,
                                  uint32_t offset, unsigned size)
{
    uint32_t value = 0;
    unsigned pixel;
    unsigned plane;
    unsigned i;

    if ((s->rop & 0x8000) && offset >= PEGC_PATTERN &&
        offset < PEGC_PATTERN_END && !(offset & 3)) {
        /*
         * Modified by Awe Morris according to the SL9821 hardware analysis:
         * pixel-oriented pattern registers are four-byte-spaced palette
         * entries, rather than a packed array whose layout changes with the
         * host access size.
         */
        pixel = (offset - PEGC_PATTERN) / 4;
        for (plane = 0; plane < 8; plane++) {
            value |= ((s->pattern[plane] >> pixel) & 1) << plane;
        }
        return value;
    }

    for (i = 0; i < size; i++) {
        value |= (uint32_t)pegc_reg_read_byte(s, offset + i) << (i * 8);
    }
    return value;
}

static void pegc_reg_write_byte(Pc98PegcPlaneState *s, uint32_t offset,
                                uint8_t value)
{
    uint32_t shift;
    unsigned plane;

    switch (offset) {
    case PEGC_PLANE_ACCESS:
        s->plane_access = value;
        return;
    case PEGC_ROP:
        s->rop = (s->rop & 0xff00) | value;
        return;
    case PEGC_ROP + 1:
        s->rop = (s->rop & 0x00ff) | ((uint16_t)value << 8);
        return;
    case PEGC_DATA_SELECT:
        s->data_select = value & 1;
        return;
    case PEGC_MASK ... PEGC_MASK + 3:
        shift = (offset - PEGC_MASK) * 8;
        s->mask = (s->mask & ~(0xffU << shift)) | ((uint32_t)value << shift);
        return;
    case PEGC_LENGTH:
        s->length = (s->length & 0x0f00) | value;
        return;
    case PEGC_LENGTH + 1:
        s->length = (s->length & 0x00ff) | ((value & 0x0f) << 8);
        return;
    case PEGC_SHIFT:
        s->shift = (s->shift & 0x1f00) | (value & 0x1f);
        return;
    case PEGC_SHIFT + 1:
        s->shift = (s->shift & 0x001f) | ((value & 0x1f) << 8);
        return;
    case PEGC_PALETTE1:
        s->palette1 = value;
        return;
    case PEGC_PALETTE2:
        s->palette2 = value;
        return;
    default:
        break;
    }

    if (offset >= PEGC_PATTERN && offset < PEGC_PATTERN + 8 * 4) {
        plane = (offset - PEGC_PATTERN) / 4;
        shift = ((offset - PEGC_PATTERN) & 3) * 8;
        s->pattern[plane] = (s->pattern[plane] & ~(0xffU << shift)) |
                            ((uint32_t)value << shift);
    }
}

void pc98_pegc_plane_reg_write(Pc98PegcPlaneState *s, uint32_t offset,
                               uint32_t value, unsigned size)
{
    unsigned pixel;
    unsigned plane;
    unsigned i;

    if ((s->rop & 0x8000) && offset >= PEGC_PATTERN &&
        offset < PEGC_PATTERN_END && !(offset & 3)) {
        pixel = (offset - PEGC_PATTERN) / 4;
        for (plane = 0; plane < 8; plane++) {
            s->pattern[plane] &= ~(1U << pixel);
            s->pattern[plane] |= ((value >> plane) & 1) << pixel;
        }
        return;
    }

    for (i = 0; i < size; i++) {
        pegc_reg_write_byte(s, offset + i, value >> (i * 8));
    }
    if (offset < PEGC_PATTERN) {
        pc98_pegc_plane_restart(s);
    }
}

static void pegc_fifo_append(Pc98PegcPlaneState *s, uint8_t pixel)
{
    if (s->source_len == PC98_PEGC_SOURCE_FIFO_SIZE) {
        memmove(s->source_fifo, s->source_fifo + 1,
                PC98_PEGC_SOURCE_FIFO_SIZE - 1);
        s->source_len--;
    }
    s->source_fifo[s->source_len++] = pixel;
}

static void pegc_pattern_from_read(Pc98PegcPlaneState *s, uint8_t pixel,
                                   unsigned position)
{
    unsigned plane;

    for (plane = 0; plane < 8; plane++) {
        s->pattern[plane] &= ~(1U << position);
        s->pattern[plane] |= ((pixel >> plane) & 1) << position;
    }
}

uint32_t pc98_pegc_plane_vram_read(Pc98PegcPlaneState *s,
                                   const uint8_t *vram, uint32_t address,
                                   unsigned size)
{
    uint32_t value = 0;
    unsigned width;
    unsigned i;
    int direction;

    /* NP21/W models byte accesses as open data with no shifter side effect. */
    if (size != 2 && size != 4) {
        return 0;
    }

    width = size * 8;
    direction = (s->rop & 0x0200) ? -1 : 1;
    for (i = 0; i < width; i++) {
        uint32_t pixel_addr = (address * 8 + direction * (int)i) &
                              PEGC_VRAM_MASK;
        uint8_t pixel = vram[pixel_addr];
        bool match;

        if (!(s->rop & 0x0100)) {
            pegc_fifo_append(s, pixel);
        }
        if ((s->rop & 0x2000) && !(s->rop & 0x0100)) {
            pegc_pattern_from_read(s, pixel, i);
        }

        if (s->data_select & 1) {
            /*
             * Modified by Awe Morris according to the SL9821 hardware
             * analysis: compare returns one when every enabled plane matches
             * palette register 1.  The old NP21/W path inverted this result.
             */
            match = !((pixel ^ s->palette1) & ~s->plane_access);
            value |= (uint32_t)match << pegc_cpu_bit(i);
        }
    }
    return value;
}

static uint8_t pegc_pattern_pixel(Pc98PegcPlaneState *s, unsigned position)
{
    unsigned method = (s->rop >> 10) & 3;
    uint8_t pattern = 0;
    unsigned plane;

    switch (method) {
    case 1:
        return s->palette2;
    case 2:
        return s->palette1;
    case 3:
        /* Palette selection is performed independently for each plane. */
        return 0;
    default:
        for (plane = 0; plane < 8; plane++) {
            pattern |= ((s->pattern[plane] >> (position & 31)) & 1) << plane;
        }
        return pattern;
    }
}

static uint8_t pegc_rop_pixel(Pc98PegcPlaneState *s, uint8_t source,
                              uint8_t destination, uint8_t pattern)
{
    uint8_t result = destination;
    uint8_t rop = s->rop;
    unsigned plane;

    if (!(s->rop & 0x1000)) {
        return (destination & s->plane_access) |
               (source & ~s->plane_access);
    }

    /*
     * PEGC ROP truth-table bit numbering, as documented by SL9821:
     *   bit 7 S&D&P, 6 S&D&~P, 5 S&~D&P, 4 S&~D&~P,
     *   bit 3 ~S&D&P, 2 ~S&D&~P, 1 ~S&~D&P, 0 ~S&~D&~P.
     * This table is a factual hardware interface description quoted in
     * compact form from https://www.satotomi.com/sl9821/sl9821_tec5.html.
     */
    for (plane = 0; plane < 8; plane++) {
        unsigned index;
        bool bit;

        if (s->plane_access & (1U << plane)) {
            continue;
        }
        if (((s->rop >> 10) & 3) == 3) {
            pattern = (source & (1U << plane)) ? s->palette1 : s->palette2;
        }
        index = (((source >> plane) & 1) << 2) |
                (((destination >> plane) & 1) << 1) |
                ((pattern >> plane) & 1);
        bit = rop & (1U << index);
        result &= ~(1U << plane);
        result |= bit << plane;
    }
    return result;
}

uint8_t pc98_pegc_plane_vram_write(Pc98PegcPlaneState *s, uint8_t *vram,
                                   uint32_t address, uint32_t value,
                                   unsigned size)
{
    unsigned width;
    unsigned source_shift;
    unsigned destination_shift;
    unsigned leading;
    unsigned processed = 0;
    unsigned i;
    int direction;
    uint8_t dirty = 0;

    if (size != 2 && size != 4) {
        return 0;
    }
    if (!s->transfer_active) {
        pc98_pegc_plane_restart(s);
    }

    width = size * 8;
    source_shift = s->shift & (size == 4 ? 0x1f : 0x0f);
    destination_shift = (s->shift >> 8) & (size == 4 ? 0x1f : 0x0f);
    leading = s->first_write ? destination_shift : 0;
    direction = (s->rop & 0x0200) ? -1 : 1;

    for (i = leading; i < width && s->remaining; i++) {
        unsigned transfer_pos = i - leading;
        unsigned source_pos = (s->rop & 0x0100) ? transfer_pos :
                              source_shift + transfer_pos;
        unsigned mask_bit = pegc_cpu_bit(transfer_pos);
        uint32_t pixel_addr = (address * 8 + direction * (int)i) &
                              PEGC_VRAM_MASK;
        uint8_t source;
        uint8_t destination;
        uint8_t pattern;

        processed++;
        if (!(s->mask & (1U << mask_bit))) {
            s->remaining--;
            continue;
        }
        if (s->rop & 0x0100) {
            source = source_pos < width &&
                     (value & (1U << pegc_cpu_bit(source_pos))) ? 0xff : 0;
        } else if (source_pos < s->source_len) {
            source = s->source_fifo[source_pos];
        } else {
            source = 0;
        }

        destination = vram[pixel_addr];
        pattern = pegc_pattern_pixel(s, transfer_pos);
        vram[pixel_addr] = pegc_rop_pixel(s, source, destination, pattern);
        dirty |= pixel_addr < 0x40000 ? 1 : 2;
        s->remaining--;
    }

    if (!(s->rop & 0x0100)) {
        unsigned consumed = MIN(processed, s->source_len);

        memmove(s->source_fifo, s->source_fifo + consumed,
                s->source_len - consumed);
        s->source_len -= consumed;
    }
    s->first_write = 0;
    if (!s->remaining) {
        s->transfer_active = 0;
    }
    return dirty;
}
