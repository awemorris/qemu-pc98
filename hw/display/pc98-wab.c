/*
 * QEMU NEC PC-9821 Window Accelerator Board (WAB)
 *
 * Copyright (c) 2004 Fabrice Bellard
 * Copyright (c) 2004 Makoto Suzuki (suzu)
 * Copyright (c) 2026 Awe Morris
 *
 * The board is a Cirrus Logic CL-GD5428 sitting behind an NEC LSI on
 * the CPU local bus.  The CPU never addresses the Cirrus directly:
 * the LSI relocates the chip's VGA register block to a set of PC-98
 * I/O ports (the 0x0CA0 "IoVariant 1" scheme), exposes the video
 * memory through a linear framebuffer window, and provides a small
 * indexed control interface (ports 0xfaa/0xfab and 0xfa2/0xfa3) plus
 * a "display relay" (port 0xfac) that switches the monitor between
 * the native 98 graphics and the accelerator.  The exact software
 * interface is documented in the block comment above the "PC-98
 * Window Accelerator Board wiring" section below.
 *
 * This file is SELF-CONTAINED: it embeds a forked subset of QEMU's
 * Cirrus core (originally hw/display/cirrus_vga.c) with the chip
 * hardcoded to the GD5428, and with all PCI and MMIO support removed.
 * The base VGA core (hw/display/vga.c via vga_int.h) and the ROP
 * templates (cirrus_vga_rop.h / cirrus_vga_rop2.h) remain shared.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/reset.h"
#include "qapi/error.h"
#include "trace.h"
#include "migration/vmstate.h"
#include "ui/pixel_ops.h"
#include "ui/console.h"
#include "vga_regs.h"
#include "vga_int.h"
#include "qom/object.h"
#include "hw/isa/isa.h"
#include "hw/display/pc98-wab.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/ioport.h"

/***************************************
 *
 *  forked Cirrus GD5428 core: definitions
 *
 ***************************************/

//#define DEBUG_CIRRUS

/*
 * Cirrus chip ID (CR27).  The WAB LSI on the PC-9821 Mate B (Bp/Bs/Be)
 * sits in front of a CL-GD5428, so the emulated chip is hardcoded to it;
 * no other Cirrus variants are exposed by this device.
 */
#define CIRRUS_ID_CLGD5428  (0x26 << 2)

// sequencer 0x07
#define CIRRUS_SR7_BPP_VGA            0x00
#define CIRRUS_SR7_BPP_SVGA           0x01
#define CIRRUS_SR7_BPP_MASK           0x0e
#define CIRRUS_SR7_BPP_8              0x00
#define CIRRUS_SR7_BPP_16_DOUBLEVCLK  0x02
#define CIRRUS_SR7_BPP_24             0x04
#define CIRRUS_SR7_BPP_16             0x06
#define CIRRUS_SR7_BPP_32             0x08
#define CIRRUS_SR7_ISAADDR_MASK       0xe0

// sequencer 0x0f
#define CIRRUS_MEMSIZE_512k        0x08
#define CIRRUS_MEMSIZE_1M          0x10
#define CIRRUS_MEMSIZE_2M          0x18
#define CIRRUS_MEMFLAGS_BANKSWITCH 0x80 // bank switching is enabled.

// sequencer 0x12
#define CIRRUS_CURSOR_SHOW         0x01
#define CIRRUS_CURSOR_HIDDENPEL    0x02
#define CIRRUS_CURSOR_LARGE        0x04 // 64x64 if set, 32x32 if clear

// sequencer 0x17 (bus type; the WAB Cirrus is wired as ISA-style)
#define CIRRUS_BUSTYPE_ISA       0x38

// control 0x0b
#define CIRRUS_BANKING_DUAL             0x01
#define CIRRUS_BANKING_GRANULARITY_16K  0x20    // set:16k, clear:4k

// control 0x30
#define CIRRUS_BLTMODE_BACKWARDS        0x01
#define CIRRUS_BLTMODE_MEMSYSDEST       0x02
#define CIRRUS_BLTMODE_MEMSYSSRC        0x04
#define CIRRUS_BLTMODE_TRANSPARENTCOMP  0x08
#define CIRRUS_BLTMODE_PATTERNCOPY      0x40
#define CIRRUS_BLTMODE_COLOREXPAND      0x80
#define CIRRUS_BLTMODE_PIXELWIDTHMASK   0x30
#define CIRRUS_BLTMODE_PIXELWIDTH8      0x00
#define CIRRUS_BLTMODE_PIXELWIDTH16     0x10
#define CIRRUS_BLTMODE_PIXELWIDTH24     0x20
#define CIRRUS_BLTMODE_PIXELWIDTH32     0x30

// control 0x31
#define CIRRUS_BLT_BUSY                 0x01
#define CIRRUS_BLT_START                0x02
#define CIRRUS_BLT_RESET                0x04
#define CIRRUS_BLT_FIFOUSED             0x10
#define CIRRUS_BLT_AUTOSTART            0x80

// control 0x32
#define CIRRUS_ROP_0                    0x00
#define CIRRUS_ROP_SRC_AND_DST          0x05
#define CIRRUS_ROP_NOP                  0x06
#define CIRRUS_ROP_SRC_AND_NOTDST       0x09
#define CIRRUS_ROP_NOTDST               0x0b
#define CIRRUS_ROP_SRC                  0x0d
#define CIRRUS_ROP_1                    0x0e
#define CIRRUS_ROP_NOTSRC_AND_DST       0x50
#define CIRRUS_ROP_SRC_XOR_DST          0x59
#define CIRRUS_ROP_SRC_OR_DST           0x6d
#define CIRRUS_ROP_NOTSRC_OR_NOTDST     0x90
#define CIRRUS_ROP_SRC_NOTXOR_DST       0x95
#define CIRRUS_ROP_SRC_OR_NOTDST        0xad
#define CIRRUS_ROP_NOTSRC               0xd0
#define CIRRUS_ROP_NOTSRC_OR_DST        0xd6
#define CIRRUS_ROP_NOTSRC_AND_NOTDST    0xda

#define CIRRUS_ROP_NOP_INDEX 2
#define CIRRUS_ROP_SRC_INDEX 5

// control 0x33
#define CIRRUS_BLTMODEEXT_SOLIDFILL        0x04
#define CIRRUS_BLTMODEEXT_COLOREXPINV      0x02
#define CIRRUS_BLTMODEEXT_DWORDGRANULARITY 0x01

/*
 * The forked core keeps only the CPU-source FIFO BLT path; the MMIO
 * register aperture (cirrus_mmio_*) and the PCI device are
 * intentionally absent.
 */

/***************************************
 *
 *  forked Cirrus state (from cirrus_vga_internal.h, minus PCI/MMIO fields)
 *
 ***************************************/

struct CirrusVGAState;
typedef void (*cirrus_bitblt_rop_t)(struct CirrusVGAState *s,
                                    uint32_t dstaddr, uint32_t srcaddr,
                                    int dstpitch, int srcpitch,
                                    int bltwidth, int bltheight);

typedef struct CirrusVGAState {
    VGACommonState vga;

    MemoryRegion cirrus_vga_io;
    MemoryRegion cirrus_linear_io;
    MemoryRegion low_mem_container; /* container for 0xa0000-0xc0000 */
    MemoryRegion low_mem;           /* always mapped, overridden by: */
    MemoryRegion cirrus_bank[2];    /*   aliases at 0xa0000-0xb0000  */
    uint32_t cirrus_addr_mask;
    uint8_t cirrus_shadow_gr0;
    uint8_t cirrus_shadow_gr1;
    uint8_t cirrus_hidden_dac_lockindex;
    uint8_t cirrus_hidden_dac_data;
    uint32_t cirrus_bank_base[2];
    uint32_t cirrus_bank_limit[2];
    uint8_t cirrus_hidden_palette[48];
    bool enable_blitter;
    int cirrus_blt_pixelwidth;
    int cirrus_blt_width;
    int cirrus_blt_height;
    int cirrus_blt_dstpitch;
    int cirrus_blt_srcpitch;
    uint32_t cirrus_blt_fgcol;
    uint32_t cirrus_blt_bgcol;
    uint32_t cirrus_blt_dstaddr;
    uint32_t cirrus_blt_srcaddr;
    uint8_t cirrus_blt_mode;
    uint8_t cirrus_blt_modeext;
    cirrus_bitblt_rop_t cirrus_rop;
#define CIRRUS_BLTBUFSIZE (2048 * 4) /* one line width */
    uint8_t cirrus_bltbuf[CIRRUS_BLTBUFSIZE];
    uint8_t *cirrus_srcptr;
    uint8_t *cirrus_srcptr_end;
    uint32_t cirrus_srccounter;
    /* hwcursor display state */
    int last_hw_cursor_size;
    int last_hw_cursor_x;
    int last_hw_cursor_y;
    int last_hw_cursor_y_start;
    int last_hw_cursor_y_end;
    int real_vram_size; /* XXX: suppress that */
    int device_id;
} CirrusVGAState;

typedef void (*cirrus_fill_t)(struct CirrusVGAState *s,
                              uint32_t dstaddr, int dst_pitch,
                              int width, int height);

static uint8_t rop_to_index[256];

/***************************************
 *
 *  prototypes.
 *
 ***************************************/

static void cirrus_bitblt_reset(CirrusVGAState *s);
static void cirrus_update_memory_access(CirrusVGAState *s);

/***************************************
 *
 *  raster operations
 *
 ***************************************/

static bool blit_region_is_unsafe(struct CirrusVGAState *s,
                                  int32_t pitch, int32_t addr)
{
    if (!pitch) {
        return true;
    }
    if (pitch < 0) {
        int64_t min = addr
            + ((int64_t)s->cirrus_blt_height - 1) * pitch
            - s->cirrus_blt_width;
        if (min < -1 || addr >= s->vga.vram_size) {
            return true;
        }
    } else {
        int64_t max = addr
            + ((int64_t)s->cirrus_blt_height-1) * pitch
            + s->cirrus_blt_width;
        if (max > s->vga.vram_size) {
            return true;
        }
    }
    return false;
}

static bool blit_is_unsafe(struct CirrusVGAState *s, bool dst_only)
{
    /* should be the case, see cirrus_bitblt_start */
    assert(s->cirrus_blt_width > 0);
    assert(s->cirrus_blt_height > 0);

    if (s->cirrus_blt_width > CIRRUS_BLTBUFSIZE) {
        return true;
    }

    if (blit_region_is_unsafe(s, s->cirrus_blt_dstpitch,
                              s->cirrus_blt_dstaddr)) {
        return true;
    }
    if (dst_only) {
        return false;
    }
    if (blit_region_is_unsafe(s, s->cirrus_blt_srcpitch,
                              s->cirrus_blt_srcaddr)) {
        return true;
    }

    return false;
}

static void cirrus_bitblt_rop_nop(CirrusVGAState *s,
                                  uint32_t dstaddr, uint32_t srcaddr,
                                  int dstpitch,int srcpitch,
                                  int bltwidth,int bltheight)
{
}

static void cirrus_bitblt_fill_nop(CirrusVGAState *s,
                                   uint32_t dstaddr,
                                   int dstpitch, int bltwidth,int bltheight)
{
}

static inline uint8_t cirrus_src(CirrusVGAState *s, uint32_t srcaddr)
{
    if (s->cirrus_srccounter) {
        /* cputovideo */
        return s->cirrus_bltbuf[srcaddr & (CIRRUS_BLTBUFSIZE - 1)];
    } else {
        /* videotovideo */
        return s->vga.vram_ptr[srcaddr & s->cirrus_addr_mask];
    }
}

static inline uint16_t cirrus_src16(CirrusVGAState *s, uint32_t srcaddr)
{
    uint16_t *src;

    if (s->cirrus_srccounter) {
        /* cputovideo */
        src = (void *)&s->cirrus_bltbuf[srcaddr & (CIRRUS_BLTBUFSIZE - 1) & ~1];
    } else {
        /* videotovideo */
        src = (void *)&s->vga.vram_ptr[srcaddr & s->cirrus_addr_mask & ~1];
    }
    return *src;
}

static inline uint32_t cirrus_src32(CirrusVGAState *s, uint32_t srcaddr)
{
    uint32_t *src;

    if (s->cirrus_srccounter) {
        /* cputovideo */
        src = (void *)&s->cirrus_bltbuf[srcaddr & (CIRRUS_BLTBUFSIZE - 1) & ~3];
    } else {
        /* videotovideo */
        src = (void *)&s->vga.vram_ptr[srcaddr & s->cirrus_addr_mask & ~3];
    }
    return *src;
}

#define ROP_NAME 0
#define ROP_FN(d, s) 0
#include "cirrus_vga_rop.h"

#define ROP_NAME src_and_dst
#define ROP_FN(d, s) (s) & (d)
#include "cirrus_vga_rop.h"

#define ROP_NAME src_and_notdst
#define ROP_FN(d, s) (s) & (~(d))
#include "cirrus_vga_rop.h"

#define ROP_NAME notdst
#define ROP_FN(d, s) ~(d)
#include "cirrus_vga_rop.h"

#define ROP_NAME src
#define ROP_FN(d, s) s
#include "cirrus_vga_rop.h"

#define ROP_NAME 1
#define ROP_FN(d, s) ~0
#include "cirrus_vga_rop.h"

#define ROP_NAME notsrc_and_dst
#define ROP_FN(d, s) (~(s)) & (d)
#include "cirrus_vga_rop.h"

#define ROP_NAME src_xor_dst
#define ROP_FN(d, s) (s) ^ (d)
#include "cirrus_vga_rop.h"

#define ROP_NAME src_or_dst
#define ROP_FN(d, s) (s) | (d)
#include "cirrus_vga_rop.h"

#define ROP_NAME notsrc_or_notdst
#define ROP_FN(d, s) (~(s)) | (~(d))
#include "cirrus_vga_rop.h"

#define ROP_NAME src_notxor_dst
#define ROP_FN(d, s) ~((s) ^ (d))
#include "cirrus_vga_rop.h"

#define ROP_NAME src_or_notdst
#define ROP_FN(d, s) (s) | (~(d))
#include "cirrus_vga_rop.h"

#define ROP_NAME notsrc
#define ROP_FN(d, s) (~(s))
#include "cirrus_vga_rop.h"

#define ROP_NAME notsrc_or_dst
#define ROP_FN(d, s) (~(s)) | (d)
#include "cirrus_vga_rop.h"

#define ROP_NAME notsrc_and_notdst
#define ROP_FN(d, s) (~(s)) & (~(d))
#include "cirrus_vga_rop.h"

static const cirrus_bitblt_rop_t cirrus_fwd_rop[16] = {
    cirrus_bitblt_rop_fwd_0,
    cirrus_bitblt_rop_fwd_src_and_dst,
    cirrus_bitblt_rop_nop,
    cirrus_bitblt_rop_fwd_src_and_notdst,
    cirrus_bitblt_rop_fwd_notdst,
    cirrus_bitblt_rop_fwd_src,
    cirrus_bitblt_rop_fwd_1,
    cirrus_bitblt_rop_fwd_notsrc_and_dst,
    cirrus_bitblt_rop_fwd_src_xor_dst,
    cirrus_bitblt_rop_fwd_src_or_dst,
    cirrus_bitblt_rop_fwd_notsrc_or_notdst,
    cirrus_bitblt_rop_fwd_src_notxor_dst,
    cirrus_bitblt_rop_fwd_src_or_notdst,
    cirrus_bitblt_rop_fwd_notsrc,
    cirrus_bitblt_rop_fwd_notsrc_or_dst,
    cirrus_bitblt_rop_fwd_notsrc_and_notdst,
};

static const cirrus_bitblt_rop_t cirrus_bkwd_rop[16] = {
    cirrus_bitblt_rop_bkwd_0,
    cirrus_bitblt_rop_bkwd_src_and_dst,
    cirrus_bitblt_rop_nop,
    cirrus_bitblt_rop_bkwd_src_and_notdst,
    cirrus_bitblt_rop_bkwd_notdst,
    cirrus_bitblt_rop_bkwd_src,
    cirrus_bitblt_rop_bkwd_1,
    cirrus_bitblt_rop_bkwd_notsrc_and_dst,
    cirrus_bitblt_rop_bkwd_src_xor_dst,
    cirrus_bitblt_rop_bkwd_src_or_dst,
    cirrus_bitblt_rop_bkwd_notsrc_or_notdst,
    cirrus_bitblt_rop_bkwd_src_notxor_dst,
    cirrus_bitblt_rop_bkwd_src_or_notdst,
    cirrus_bitblt_rop_bkwd_notsrc,
    cirrus_bitblt_rop_bkwd_notsrc_or_dst,
    cirrus_bitblt_rop_bkwd_notsrc_and_notdst,
};

#define TRANSP_ROP(name) {\
    name ## _8,\
    name ## _16,\
        }
#define TRANSP_NOP(func) {\
    func,\
    func,\
        }

static const cirrus_bitblt_rop_t cirrus_fwd_transp_rop[16][2] = {
    TRANSP_ROP(cirrus_bitblt_rop_fwd_transp_0),
    TRANSP_ROP(cirrus_bitblt_rop_fwd_transp_src_and_dst),
    TRANSP_NOP(cirrus_bitblt_rop_nop),
    TRANSP_ROP(cirrus_bitblt_rop_fwd_transp_src_and_notdst),
    TRANSP_ROP(cirrus_bitblt_rop_fwd_transp_notdst),
    TRANSP_ROP(cirrus_bitblt_rop_fwd_transp_src),
    TRANSP_ROP(cirrus_bitblt_rop_fwd_transp_1),
    TRANSP_ROP(cirrus_bitblt_rop_fwd_transp_notsrc_and_dst),
    TRANSP_ROP(cirrus_bitblt_rop_fwd_transp_src_xor_dst),
    TRANSP_ROP(cirrus_bitblt_rop_fwd_transp_src_or_dst),
    TRANSP_ROP(cirrus_bitblt_rop_fwd_transp_notsrc_or_notdst),
    TRANSP_ROP(cirrus_bitblt_rop_fwd_transp_src_notxor_dst),
    TRANSP_ROP(cirrus_bitblt_rop_fwd_transp_src_or_notdst),
    TRANSP_ROP(cirrus_bitblt_rop_fwd_transp_notsrc),
    TRANSP_ROP(cirrus_bitblt_rop_fwd_transp_notsrc_or_dst),
    TRANSP_ROP(cirrus_bitblt_rop_fwd_transp_notsrc_and_notdst),
};

static const cirrus_bitblt_rop_t cirrus_bkwd_transp_rop[16][2] = {
    TRANSP_ROP(cirrus_bitblt_rop_bkwd_transp_0),
    TRANSP_ROP(cirrus_bitblt_rop_bkwd_transp_src_and_dst),
    TRANSP_NOP(cirrus_bitblt_rop_nop),
    TRANSP_ROP(cirrus_bitblt_rop_bkwd_transp_src_and_notdst),
    TRANSP_ROP(cirrus_bitblt_rop_bkwd_transp_notdst),
    TRANSP_ROP(cirrus_bitblt_rop_bkwd_transp_src),
    TRANSP_ROP(cirrus_bitblt_rop_bkwd_transp_1),
    TRANSP_ROP(cirrus_bitblt_rop_bkwd_transp_notsrc_and_dst),
    TRANSP_ROP(cirrus_bitblt_rop_bkwd_transp_src_xor_dst),
    TRANSP_ROP(cirrus_bitblt_rop_bkwd_transp_src_or_dst),
    TRANSP_ROP(cirrus_bitblt_rop_bkwd_transp_notsrc_or_notdst),
    TRANSP_ROP(cirrus_bitblt_rop_bkwd_transp_src_notxor_dst),
    TRANSP_ROP(cirrus_bitblt_rop_bkwd_transp_src_or_notdst),
    TRANSP_ROP(cirrus_bitblt_rop_bkwd_transp_notsrc),
    TRANSP_ROP(cirrus_bitblt_rop_bkwd_transp_notsrc_or_dst),
    TRANSP_ROP(cirrus_bitblt_rop_bkwd_transp_notsrc_and_notdst),
};

#define ROP2(name) {\
    name ## _8,\
    name ## _16,\
    name ## _24,\
    name ## _32,\
        }

#define ROP_NOP2(func) {\
    func,\
    func,\
    func,\
    func,\
        }

static const cirrus_bitblt_rop_t cirrus_patternfill[16][4] = {
    ROP2(cirrus_patternfill_0),
    ROP2(cirrus_patternfill_src_and_dst),
    ROP_NOP2(cirrus_bitblt_rop_nop),
    ROP2(cirrus_patternfill_src_and_notdst),
    ROP2(cirrus_patternfill_notdst),
    ROP2(cirrus_patternfill_src),
    ROP2(cirrus_patternfill_1),
    ROP2(cirrus_patternfill_notsrc_and_dst),
    ROP2(cirrus_patternfill_src_xor_dst),
    ROP2(cirrus_patternfill_src_or_dst),
    ROP2(cirrus_patternfill_notsrc_or_notdst),
    ROP2(cirrus_patternfill_src_notxor_dst),
    ROP2(cirrus_patternfill_src_or_notdst),
    ROP2(cirrus_patternfill_notsrc),
    ROP2(cirrus_patternfill_notsrc_or_dst),
    ROP2(cirrus_patternfill_notsrc_and_notdst),
};

static const cirrus_bitblt_rop_t cirrus_colorexpand_transp[16][4] = {
    ROP2(cirrus_colorexpand_transp_0),
    ROP2(cirrus_colorexpand_transp_src_and_dst),
    ROP_NOP2(cirrus_bitblt_rop_nop),
    ROP2(cirrus_colorexpand_transp_src_and_notdst),
    ROP2(cirrus_colorexpand_transp_notdst),
    ROP2(cirrus_colorexpand_transp_src),
    ROP2(cirrus_colorexpand_transp_1),
    ROP2(cirrus_colorexpand_transp_notsrc_and_dst),
    ROP2(cirrus_colorexpand_transp_src_xor_dst),
    ROP2(cirrus_colorexpand_transp_src_or_dst),
    ROP2(cirrus_colorexpand_transp_notsrc_or_notdst),
    ROP2(cirrus_colorexpand_transp_src_notxor_dst),
    ROP2(cirrus_colorexpand_transp_src_or_notdst),
    ROP2(cirrus_colorexpand_transp_notsrc),
    ROP2(cirrus_colorexpand_transp_notsrc_or_dst),
    ROP2(cirrus_colorexpand_transp_notsrc_and_notdst),
};

static const cirrus_bitblt_rop_t cirrus_colorexpand[16][4] = {
    ROP2(cirrus_colorexpand_0),
    ROP2(cirrus_colorexpand_src_and_dst),
    ROP_NOP2(cirrus_bitblt_rop_nop),
    ROP2(cirrus_colorexpand_src_and_notdst),
    ROP2(cirrus_colorexpand_notdst),
    ROP2(cirrus_colorexpand_src),
    ROP2(cirrus_colorexpand_1),
    ROP2(cirrus_colorexpand_notsrc_and_dst),
    ROP2(cirrus_colorexpand_src_xor_dst),
    ROP2(cirrus_colorexpand_src_or_dst),
    ROP2(cirrus_colorexpand_notsrc_or_notdst),
    ROP2(cirrus_colorexpand_src_notxor_dst),
    ROP2(cirrus_colorexpand_src_or_notdst),
    ROP2(cirrus_colorexpand_notsrc),
    ROP2(cirrus_colorexpand_notsrc_or_dst),
    ROP2(cirrus_colorexpand_notsrc_and_notdst),
};

static const cirrus_bitblt_rop_t cirrus_colorexpand_pattern_transp[16][4] = {
    ROP2(cirrus_colorexpand_pattern_transp_0),
    ROP2(cirrus_colorexpand_pattern_transp_src_and_dst),
    ROP_NOP2(cirrus_bitblt_rop_nop),
    ROP2(cirrus_colorexpand_pattern_transp_src_and_notdst),
    ROP2(cirrus_colorexpand_pattern_transp_notdst),
    ROP2(cirrus_colorexpand_pattern_transp_src),
    ROP2(cirrus_colorexpand_pattern_transp_1),
    ROP2(cirrus_colorexpand_pattern_transp_notsrc_and_dst),
    ROP2(cirrus_colorexpand_pattern_transp_src_xor_dst),
    ROP2(cirrus_colorexpand_pattern_transp_src_or_dst),
    ROP2(cirrus_colorexpand_pattern_transp_notsrc_or_notdst),
    ROP2(cirrus_colorexpand_pattern_transp_src_notxor_dst),
    ROP2(cirrus_colorexpand_pattern_transp_src_or_notdst),
    ROP2(cirrus_colorexpand_pattern_transp_notsrc),
    ROP2(cirrus_colorexpand_pattern_transp_notsrc_or_dst),
    ROP2(cirrus_colorexpand_pattern_transp_notsrc_and_notdst),
};

static const cirrus_bitblt_rop_t cirrus_colorexpand_pattern[16][4] = {
    ROP2(cirrus_colorexpand_pattern_0),
    ROP2(cirrus_colorexpand_pattern_src_and_dst),
    ROP_NOP2(cirrus_bitblt_rop_nop),
    ROP2(cirrus_colorexpand_pattern_src_and_notdst),
    ROP2(cirrus_colorexpand_pattern_notdst),
    ROP2(cirrus_colorexpand_pattern_src),
    ROP2(cirrus_colorexpand_pattern_1),
    ROP2(cirrus_colorexpand_pattern_notsrc_and_dst),
    ROP2(cirrus_colorexpand_pattern_src_xor_dst),
    ROP2(cirrus_colorexpand_pattern_src_or_dst),
    ROP2(cirrus_colorexpand_pattern_notsrc_or_notdst),
    ROP2(cirrus_colorexpand_pattern_src_notxor_dst),
    ROP2(cirrus_colorexpand_pattern_src_or_notdst),
    ROP2(cirrus_colorexpand_pattern_notsrc),
    ROP2(cirrus_colorexpand_pattern_notsrc_or_dst),
    ROP2(cirrus_colorexpand_pattern_notsrc_and_notdst),
};

static const cirrus_fill_t cirrus_fill[16][4] = {
    ROP2(cirrus_fill_0),
    ROP2(cirrus_fill_src_and_dst),
    ROP_NOP2(cirrus_bitblt_fill_nop),
    ROP2(cirrus_fill_src_and_notdst),
    ROP2(cirrus_fill_notdst),
    ROP2(cirrus_fill_src),
    ROP2(cirrus_fill_1),
    ROP2(cirrus_fill_notsrc_and_dst),
    ROP2(cirrus_fill_src_xor_dst),
    ROP2(cirrus_fill_src_or_dst),
    ROP2(cirrus_fill_notsrc_or_notdst),
    ROP2(cirrus_fill_src_notxor_dst),
    ROP2(cirrus_fill_src_or_notdst),
    ROP2(cirrus_fill_notsrc),
    ROP2(cirrus_fill_notsrc_or_dst),
    ROP2(cirrus_fill_notsrc_and_notdst),
};

static inline void cirrus_bitblt_fgcol(CirrusVGAState *s)
{
    unsigned int color;
    switch (s->cirrus_blt_pixelwidth) {
    case 1:
        s->cirrus_blt_fgcol = s->cirrus_shadow_gr1;
        break;
    case 2:
        color = s->cirrus_shadow_gr1 | (s->vga.gr[0x11] << 8);
        s->cirrus_blt_fgcol = le16_to_cpu(color);
        break;
    case 3:
        s->cirrus_blt_fgcol = s->cirrus_shadow_gr1 |
            (s->vga.gr[0x11] << 8) | (s->vga.gr[0x13] << 16);
        break;
    default:
    case 4:
        color = s->cirrus_shadow_gr1 | (s->vga.gr[0x11] << 8) |
            (s->vga.gr[0x13] << 16) | (s->vga.gr[0x15] << 24);
        s->cirrus_blt_fgcol = le32_to_cpu(color);
        break;
    }
}

static inline void cirrus_bitblt_bgcol(CirrusVGAState *s)
{
    unsigned int color;
    switch (s->cirrus_blt_pixelwidth) {
    case 1:
        s->cirrus_blt_bgcol = s->cirrus_shadow_gr0;
        break;
    case 2:
        color = s->cirrus_shadow_gr0 | (s->vga.gr[0x10] << 8);
        s->cirrus_blt_bgcol = le16_to_cpu(color);
        break;
    case 3:
        s->cirrus_blt_bgcol = s->cirrus_shadow_gr0 |
            (s->vga.gr[0x10] << 8) | (s->vga.gr[0x12] << 16);
        break;
    default:
    case 4:
        color = s->cirrus_shadow_gr0 | (s->vga.gr[0x10] << 8) |
            (s->vga.gr[0x12] << 16) | (s->vga.gr[0x14] << 24);
        s->cirrus_blt_bgcol = le32_to_cpu(color);
        break;
    }
}

static void cirrus_invalidate_region(CirrusVGAState * s, int off_begin,
                                     int off_pitch, int bytesperline,
                                     int lines)
{
    int y;
    int off_cur;
    int off_cur_end;

    if (off_pitch < 0) {
        off_begin -= bytesperline - 1;
    }

    for (y = 0; y < lines; y++) {
        off_cur = off_begin & s->cirrus_addr_mask;
        off_cur_end = ((off_cur + bytesperline - 1) & s->cirrus_addr_mask) + 1;
        if (off_cur_end >= off_cur) {
            memory_region_set_dirty(&s->vga.vram, off_cur, off_cur_end - off_cur);
        } else {
            /* wraparound */
            memory_region_set_dirty(&s->vga.vram, off_cur,
                                    s->cirrus_addr_mask + 1 - off_cur);
            memory_region_set_dirty(&s->vga.vram, 0, off_cur_end);
        }
        off_begin += off_pitch;
    }
}

static int cirrus_bitblt_common_patterncopy(CirrusVGAState *s)
{
    uint32_t patternsize;
    bool videosrc = !s->cirrus_srccounter;

    if (videosrc) {
        switch (s->vga.get_bpp(&s->vga)) {
        case 8:
            patternsize = 64;
            break;
        case 15:
        case 16:
            patternsize = 128;
            break;
        case 24:
        case 32:
        default:
            patternsize = 256;
            break;
        }
        s->cirrus_blt_srcaddr &= ~(patternsize - 1);
        if (s->cirrus_blt_srcaddr + patternsize > s->vga.vram_size) {
            return 0;
        }
    }

    if (blit_is_unsafe(s, true)) {
        return 0;
    }

    (*s->cirrus_rop) (s, s->cirrus_blt_dstaddr,
                      videosrc ? s->cirrus_blt_srcaddr : 0,
                      s->cirrus_blt_dstpitch, 0,
                      s->cirrus_blt_width, s->cirrus_blt_height);
    cirrus_invalidate_region(s, s->cirrus_blt_dstaddr,
                             s->cirrus_blt_dstpitch, s->cirrus_blt_width,
                             s->cirrus_blt_height);
    return 1;
}

/* fill */

static int cirrus_bitblt_solidfill(CirrusVGAState *s, int blt_rop)
{
    cirrus_fill_t rop_func;

    if (blit_is_unsafe(s, true)) {
        return 0;
    }
    rop_func = cirrus_fill[rop_to_index[blt_rop]][s->cirrus_blt_pixelwidth - 1];
    rop_func(s, s->cirrus_blt_dstaddr,
             s->cirrus_blt_dstpitch,
             s->cirrus_blt_width, s->cirrus_blt_height);
    cirrus_invalidate_region(s, s->cirrus_blt_dstaddr,
                             s->cirrus_blt_dstpitch, s->cirrus_blt_width,
                             s->cirrus_blt_height);
    cirrus_bitblt_reset(s);
    return 1;
}

/***************************************
 *
 *  bitblt (video-to-video)
 *
 ***************************************/

static int cirrus_bitblt_videotovideo_patterncopy(CirrusVGAState * s)
{
    return cirrus_bitblt_common_patterncopy(s);
}

static int cirrus_do_copy(CirrusVGAState *s, int dst, int src, int w, int h)
{
    int sx = 0, sy = 0;
    int dx = 0, dy = 0;
    int depth = 0;
    int notify = 0;

    /* make sure to only copy if it's a plain copy ROP */
    if (*s->cirrus_rop == cirrus_bitblt_rop_fwd_src ||
        *s->cirrus_rop == cirrus_bitblt_rop_bkwd_src) {

        int width, height;

        depth = s->vga.get_bpp(&s->vga) / 8;
        if (!depth) {
            return 0;
        }
        s->vga.get_resolution(&s->vga, &width, &height);

        /* extra x, y */
        sx = (src % ABS(s->cirrus_blt_srcpitch)) / depth;
        sy = (src / ABS(s->cirrus_blt_srcpitch));
        dx = (dst % ABS(s->cirrus_blt_dstpitch)) / depth;
        dy = (dst / ABS(s->cirrus_blt_dstpitch));

        /* normalize width */
        w /= depth;

        /* if we're doing a backward copy, we have to adjust
           our x/y to be the upper left corner (instead of the lower
           right corner) */
        if (s->cirrus_blt_dstpitch < 0) {
            sx -= (s->cirrus_blt_width / depth) - 1;
            dx -= (s->cirrus_blt_width / depth) - 1;
            sy -= s->cirrus_blt_height - 1;
            dy -= s->cirrus_blt_height - 1;
        }

        /* are we in the visible portion of memory? */
        if (sx >= 0 && sy >= 0 && dx >= 0 && dy >= 0 &&
            (sx + w) <= width && (sy + h) <= height &&
            (dx + w) <= width && (dy + h) <= height) {
            notify = 1;
        }
    }

    (*s->cirrus_rop) (s, s->cirrus_blt_dstaddr,
                      s->cirrus_blt_srcaddr,
                      s->cirrus_blt_dstpitch, s->cirrus_blt_srcpitch,
                      s->cirrus_blt_width, s->cirrus_blt_height);

    if (notify) {
        qemu_console_update(s->vga.con, dx, dy,
                            s->cirrus_blt_width / depth,
                            s->cirrus_blt_height);
    }

    /* we don't have to notify the display that this portion has
       changed since qemu_console_copy implies this */

    cirrus_invalidate_region(s, s->cirrus_blt_dstaddr,
                                s->cirrus_blt_dstpitch, s->cirrus_blt_width,
                                s->cirrus_blt_height);

    return 1;
}

static int cirrus_bitblt_videotovideo_copy(CirrusVGAState * s)
{
    if (blit_is_unsafe(s, false))
        return 0;

    return cirrus_do_copy(s, s->cirrus_blt_dstaddr - s->vga.params.start_addr,
                          s->cirrus_blt_srcaddr - s->vga.params.start_addr,
                          s->cirrus_blt_width, s->cirrus_blt_height);
}

/***************************************
 *
 *  bitblt (cpu-to-video)
 *
 ***************************************/

static void cirrus_bitblt_cputovideo_next(CirrusVGAState * s)
{
    int copy_count;
    uint8_t *end_ptr;

    if (s->cirrus_srccounter > 0) {
        if (s->cirrus_blt_mode & CIRRUS_BLTMODE_PATTERNCOPY) {
            cirrus_bitblt_common_patterncopy(s);
        the_end:
            s->cirrus_srccounter = 0;
            cirrus_bitblt_reset(s);
        } else {
            /* at least one scan line */
            do {
                (*s->cirrus_rop)(s, s->cirrus_blt_dstaddr,
                                 0, 0, 0, s->cirrus_blt_width, 1);
                cirrus_invalidate_region(s, s->cirrus_blt_dstaddr, 0,
                                         s->cirrus_blt_width, 1);
                s->cirrus_blt_dstaddr += s->cirrus_blt_dstpitch;
                s->cirrus_srccounter -= s->cirrus_blt_srcpitch;
                if (s->cirrus_srccounter <= 0)
                    goto the_end;
                /* more bytes than needed can be transferred because of
                   word alignment, so we keep them for the next line */
                /* XXX: keep alignment to speed up transfer */
                end_ptr = s->cirrus_bltbuf + s->cirrus_blt_srcpitch;
                copy_count = MIN(s->cirrus_srcptr_end - end_ptr, CIRRUS_BLTBUFSIZE);
                memmove(s->cirrus_bltbuf, end_ptr, copy_count);
                s->cirrus_srcptr = s->cirrus_bltbuf + copy_count;
                s->cirrus_srcptr_end = s->cirrus_bltbuf + s->cirrus_blt_srcpitch;
            } while (s->cirrus_srcptr >= s->cirrus_srcptr_end);
        }
    }
}

/***************************************
 *
 *  bitblt wrapper
 *
 ***************************************/

static void cirrus_bitblt_reset(CirrusVGAState * s)
{
    int need_update;

    s->vga.gr[0x31] &=
        ~(CIRRUS_BLT_START | CIRRUS_BLT_BUSY | CIRRUS_BLT_FIFOUSED);
    need_update = s->cirrus_srcptr != &s->cirrus_bltbuf[0]
        || s->cirrus_srcptr_end != &s->cirrus_bltbuf[0];
    s->cirrus_srcptr = &s->cirrus_bltbuf[0];
    s->cirrus_srcptr_end = &s->cirrus_bltbuf[0];
    s->cirrus_srccounter = 0;
    if (!need_update)
        return;
    cirrus_update_memory_access(s);
}

static int cirrus_bitblt_cputovideo(CirrusVGAState * s)
{
    int w;

    if (blit_is_unsafe(s, true)) {
        return 0;
    }

    s->cirrus_blt_mode &= ~CIRRUS_BLTMODE_MEMSYSSRC;
    s->cirrus_srcptr = &s->cirrus_bltbuf[0];
    s->cirrus_srcptr_end = &s->cirrus_bltbuf[0];

    if (s->cirrus_blt_mode & CIRRUS_BLTMODE_PATTERNCOPY) {
        if (s->cirrus_blt_mode & CIRRUS_BLTMODE_COLOREXPAND) {
            s->cirrus_blt_srcpitch = 8;
        } else {
            /* XXX: check for 24 bpp */
            s->cirrus_blt_srcpitch = 8 * 8 * s->cirrus_blt_pixelwidth;
        }
        s->cirrus_srccounter = s->cirrus_blt_srcpitch;
    } else {
        if (s->cirrus_blt_mode & CIRRUS_BLTMODE_COLOREXPAND) {
            w = s->cirrus_blt_width / s->cirrus_blt_pixelwidth;
            if (s->cirrus_blt_modeext & CIRRUS_BLTMODEEXT_DWORDGRANULARITY)
                s->cirrus_blt_srcpitch = ((w + 31) >> 5);
            else
                s->cirrus_blt_srcpitch = ((w + 7) >> 3);
        } else {
            /* always align input size to 32 bits */
            s->cirrus_blt_srcpitch = (s->cirrus_blt_width + 3) & ~3;
        }
        s->cirrus_srccounter = s->cirrus_blt_srcpitch * s->cirrus_blt_height;
    }

    /* the blit_is_unsafe call above should catch this */
    assert(s->cirrus_blt_srcpitch <= CIRRUS_BLTBUFSIZE);

    s->cirrus_srcptr = s->cirrus_bltbuf;
    s->cirrus_srcptr_end = s->cirrus_bltbuf + s->cirrus_blt_srcpitch;
    cirrus_update_memory_access(s);
    return 1;
}

static int cirrus_bitblt_videotocpu(CirrusVGAState * s)
{
    /* XXX */
    qemu_log_mask(LOG_UNIMP,
                  "cirrus: bitblt (video to cpu) is not implemented\n");
    return 0;
}

static int cirrus_bitblt_videotovideo(CirrusVGAState * s)
{
    int ret;

    if (s->cirrus_blt_mode & CIRRUS_BLTMODE_PATTERNCOPY) {
        ret = cirrus_bitblt_videotovideo_patterncopy(s);
    } else {
        ret = cirrus_bitblt_videotovideo_copy(s);
    }
    if (ret)
        cirrus_bitblt_reset(s);
    return ret;
}

static void cirrus_bitblt_start(CirrusVGAState * s)
{
    uint8_t blt_rop;

    if (!s->enable_blitter) {
        goto bitblt_ignore;
    }

    s->vga.gr[0x31] |= CIRRUS_BLT_BUSY;

    s->cirrus_blt_width = (s->vga.gr[0x20] | (s->vga.gr[0x21] << 8)) + 1;
    s->cirrus_blt_height = (s->vga.gr[0x22] | (s->vga.gr[0x23] << 8)) + 1;
    s->cirrus_blt_dstpitch = (s->vga.gr[0x24] | (s->vga.gr[0x25] << 8));
    s->cirrus_blt_srcpitch = (s->vga.gr[0x26] | (s->vga.gr[0x27] << 8));
    s->cirrus_blt_dstaddr =
        (s->vga.gr[0x28] | (s->vga.gr[0x29] << 8) | (s->vga.gr[0x2a] << 16));
    s->cirrus_blt_srcaddr =
        (s->vga.gr[0x2c] | (s->vga.gr[0x2d] << 8) | (s->vga.gr[0x2e] << 16));
    s->cirrus_blt_mode = s->vga.gr[0x30];
    s->cirrus_blt_modeext = s->vga.gr[0x33];
    blt_rop = s->vga.gr[0x32];

    s->cirrus_blt_dstaddr &= s->cirrus_addr_mask;
    s->cirrus_blt_srcaddr &= s->cirrus_addr_mask;

    trace_vga_cirrus_bitblt_start(blt_rop,
                                  s->cirrus_blt_mode,
                                  s->cirrus_blt_modeext,
                                  s->cirrus_blt_width,
                                  s->cirrus_blt_height,
                                  s->cirrus_blt_dstpitch,
                                  s->cirrus_blt_srcpitch,
                                  s->cirrus_blt_dstaddr,
                                  s->cirrus_blt_srcaddr,
                                  s->vga.gr[0x2f]);

    switch (s->cirrus_blt_mode & CIRRUS_BLTMODE_PIXELWIDTHMASK) {
    case CIRRUS_BLTMODE_PIXELWIDTH8:
        s->cirrus_blt_pixelwidth = 1;
        break;
    case CIRRUS_BLTMODE_PIXELWIDTH16:
        s->cirrus_blt_pixelwidth = 2;
        break;
    case CIRRUS_BLTMODE_PIXELWIDTH24:
        s->cirrus_blt_pixelwidth = 3;
        break;
    case CIRRUS_BLTMODE_PIXELWIDTH32:
        s->cirrus_blt_pixelwidth = 4;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cirrus: bitblt - pixel width is unknown\n");
        goto bitblt_ignore;
    }
    s->cirrus_blt_mode &= ~CIRRUS_BLTMODE_PIXELWIDTHMASK;

    if ((s->
         cirrus_blt_mode & (CIRRUS_BLTMODE_MEMSYSSRC |
                            CIRRUS_BLTMODE_MEMSYSDEST))
        == (CIRRUS_BLTMODE_MEMSYSSRC | CIRRUS_BLTMODE_MEMSYSDEST)) {
        qemu_log_mask(LOG_UNIMP,
                      "cirrus: bitblt - memory-to-memory copy requested\n");
        goto bitblt_ignore;
    }

    if ((s->cirrus_blt_modeext & CIRRUS_BLTMODEEXT_SOLIDFILL) &&
        (s->cirrus_blt_mode & (CIRRUS_BLTMODE_MEMSYSDEST |
                               CIRRUS_BLTMODE_TRANSPARENTCOMP |
                               CIRRUS_BLTMODE_PATTERNCOPY |
                               CIRRUS_BLTMODE_COLOREXPAND)) ==
         (CIRRUS_BLTMODE_PATTERNCOPY | CIRRUS_BLTMODE_COLOREXPAND)) {
        cirrus_bitblt_fgcol(s);
        cirrus_bitblt_solidfill(s, blt_rop);
    } else {
        if ((s->cirrus_blt_mode & (CIRRUS_BLTMODE_COLOREXPAND |
                                   CIRRUS_BLTMODE_PATTERNCOPY)) ==
            CIRRUS_BLTMODE_COLOREXPAND) {

            if (s->cirrus_blt_mode & CIRRUS_BLTMODE_TRANSPARENTCOMP) {
                if (s->cirrus_blt_modeext & CIRRUS_BLTMODEEXT_COLOREXPINV)
                    cirrus_bitblt_bgcol(s);
                else
                    cirrus_bitblt_fgcol(s);
                s->cirrus_rop = cirrus_colorexpand_transp[rop_to_index[blt_rop]][s->cirrus_blt_pixelwidth - 1];
            } else {
                cirrus_bitblt_fgcol(s);
                cirrus_bitblt_bgcol(s);
                s->cirrus_rop = cirrus_colorexpand[rop_to_index[blt_rop]][s->cirrus_blt_pixelwidth - 1];
            }
        } else if (s->cirrus_blt_mode & CIRRUS_BLTMODE_PATTERNCOPY) {
            if (s->cirrus_blt_mode & CIRRUS_BLTMODE_COLOREXPAND) {
                if (s->cirrus_blt_mode & CIRRUS_BLTMODE_TRANSPARENTCOMP) {
                    if (s->cirrus_blt_modeext & CIRRUS_BLTMODEEXT_COLOREXPINV)
                        cirrus_bitblt_bgcol(s);
                    else
                        cirrus_bitblt_fgcol(s);
                    s->cirrus_rop = cirrus_colorexpand_pattern_transp[rop_to_index[blt_rop]][s->cirrus_blt_pixelwidth - 1];
                } else {
                    cirrus_bitblt_fgcol(s);
                    cirrus_bitblt_bgcol(s);
                    s->cirrus_rop = cirrus_colorexpand_pattern[rop_to_index[blt_rop]][s->cirrus_blt_pixelwidth - 1];
                }
            } else {
                s->cirrus_rop = cirrus_patternfill[rop_to_index[blt_rop]][s->cirrus_blt_pixelwidth - 1];
            }
        } else {
            if (s->cirrus_blt_mode & CIRRUS_BLTMODE_TRANSPARENTCOMP) {
                if (s->cirrus_blt_pixelwidth > 2) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "cirrus: src transparent without colorexpand "
                                  "must be 8bpp or 16bpp\n");
                    goto bitblt_ignore;
                }
                if (s->cirrus_blt_mode & CIRRUS_BLTMODE_BACKWARDS) {
                    s->cirrus_blt_dstpitch = -s->cirrus_blt_dstpitch;
                    s->cirrus_blt_srcpitch = -s->cirrus_blt_srcpitch;
                    s->cirrus_rop = cirrus_bkwd_transp_rop[rop_to_index[blt_rop]][s->cirrus_blt_pixelwidth - 1];
                } else {
                    s->cirrus_rop = cirrus_fwd_transp_rop[rop_to_index[blt_rop]][s->cirrus_blt_pixelwidth - 1];
                }
            } else {
                if (s->cirrus_blt_mode & CIRRUS_BLTMODE_BACKWARDS) {
                    s->cirrus_blt_dstpitch = -s->cirrus_blt_dstpitch;
                    s->cirrus_blt_srcpitch = -s->cirrus_blt_srcpitch;
                    s->cirrus_rop = cirrus_bkwd_rop[rop_to_index[blt_rop]];
                } else {
                    s->cirrus_rop = cirrus_fwd_rop[rop_to_index[blt_rop]];
                }
            }
        }
        // setup bitblt engine.
        if (s->cirrus_blt_mode & CIRRUS_BLTMODE_MEMSYSSRC) {
            if (!cirrus_bitblt_cputovideo(s))
                goto bitblt_ignore;
        } else if (s->cirrus_blt_mode & CIRRUS_BLTMODE_MEMSYSDEST) {
            if (!cirrus_bitblt_videotocpu(s))
                goto bitblt_ignore;
        } else {
            if (!cirrus_bitblt_videotovideo(s))
                goto bitblt_ignore;
        }
    }
    return;
  bitblt_ignore:;
    cirrus_bitblt_reset(s);
}

static void cirrus_write_bitblt(CirrusVGAState * s, unsigned reg_value)
{
    unsigned old_value;

    old_value = s->vga.gr[0x31];
    s->vga.gr[0x31] = reg_value;

    if (((old_value & CIRRUS_BLT_RESET) != 0) &&
        ((reg_value & CIRRUS_BLT_RESET) == 0)) {
        cirrus_bitblt_reset(s);
    } else if (((old_value & CIRRUS_BLT_START) == 0) &&
               ((reg_value & CIRRUS_BLT_START) != 0)) {
        cirrus_bitblt_start(s);
    }
}


/***************************************
 *
 *  basic parameters
 *
 ***************************************/

static void cirrus_get_params(VGACommonState *s1,
                              VGADisplayParams *params)
{
    CirrusVGAState * s = container_of(s1, CirrusVGAState, vga);
    uint32_t line_offset;

    line_offset = s->vga.cr[0x13]
        | ((s->vga.cr[0x1b] & 0x10) << 4);
    line_offset <<= 3;
    params->line_offset = line_offset;

    params->start_addr = (s->vga.cr[0x0c] << 8)
        | s->vga.cr[0x0d]
        | ((s->vga.cr[0x1b] & 0x01) << 16)
        | ((s->vga.cr[0x1b] & 0x0c) << 15)
        | ((s->vga.cr[0x1d] & 0x80) << 12);

    params->line_compare = s->vga.cr[0x18] |
        ((s->vga.cr[0x07] & 0x10) << 4) |
        ((s->vga.cr[0x09] & 0x40) << 3);

    params->hpel = s->vga.ar[VGA_ATC_PEL];
    params->hpel_split = s->vga.ar[VGA_ATC_MODE] & 0x20;
}

static uint32_t cirrus_get_bpp16_depth(CirrusVGAState * s)
{
    uint32_t ret = 16;

    switch (s->cirrus_hidden_dac_data & 0xf) {
    case 0:
        ret = 15;
        break;                  /* Sierra HiColor */
    case 1:
        ret = 16;
        break;                  /* XGA HiColor */
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cirrus: invalid DAC value 0x%x in 16bpp\n",
                      (s->cirrus_hidden_dac_data & 0xf));
        ret = 15;               /* XXX */
        break;
    }
    return ret;
}

static int cirrus_get_bpp(VGACommonState *s1)
{
    CirrusVGAState * s = container_of(s1, CirrusVGAState, vga);
    uint32_t ret = 8;

    if ((s->vga.sr[0x07] & 0x01) != 0) {
        /* Cirrus SVGA */
        switch (s->vga.sr[0x07] & CIRRUS_SR7_BPP_MASK) {
        case CIRRUS_SR7_BPP_8:
            ret = 8;
            break;
        case CIRRUS_SR7_BPP_16_DOUBLEVCLK:
            ret = cirrus_get_bpp16_depth(s);
            break;
        case CIRRUS_SR7_BPP_24:
            ret = 24;
            break;
        case CIRRUS_SR7_BPP_16:
            ret = cirrus_get_bpp16_depth(s);
            break;
        case CIRRUS_SR7_BPP_32:
            ret = 32;
            break;
        default:
#ifdef DEBUG_CIRRUS
            printf("cirrus: unknown bpp - sr7=%x\n", s->vga.sr[0x7]);
#endif
            ret = 8;
            break;
        }
    } else {
        /* VGA */
        ret = 0;
    }

    return ret;
}

static void cirrus_get_resolution(VGACommonState *s, int *pwidth, int *pheight)
{
    int width, height;

    width = (s->cr[0x01] + 1) * 8;
    height = s->cr[0x12] |
        ((s->cr[0x07] & 0x02) << 7) |
        ((s->cr[0x07] & 0x40) << 3);
    height = (height + 1);
    /* interlace support */
    if (s->cr[0x1a] & 0x01)
        height = height * 2;
    *pwidth = width;
    *pheight = height;
}

/***************************************
 *
 * bank memory
 *
 ***************************************/

static void cirrus_update_bank_ptr(CirrusVGAState * s, unsigned bank_index)
{
    unsigned offset;
    unsigned limit;

    if ((s->vga.gr[0x0b] & 0x01) != 0)  /* dual bank */
        offset = s->vga.gr[0x09 + bank_index];
    else                        /* single bank */
        offset = s->vga.gr[0x09];

    if ((s->vga.gr[0x0b] & 0x20) != 0)
        offset <<= 14;
    else
        offset <<= 12;

    if (s->real_vram_size <= offset)
        limit = 0;
    else
        limit = s->real_vram_size - offset;

    if (((s->vga.gr[0x0b] & 0x01) == 0) && (bank_index != 0)) {
        if (limit > 0x8000) {
            offset += 0x8000;
            limit -= 0x8000;
        } else {
            limit = 0;
        }
    }

    if (limit > 0) {
        s->cirrus_bank_base[bank_index] = offset;
        s->cirrus_bank_limit[bank_index] = limit;
    } else {
        s->cirrus_bank_base[bank_index] = 0;
        s->cirrus_bank_limit[bank_index] = 0;
    }
}

/***************************************
 *
 *  I/O access between 0x3c4-0x3c5
 *
 ***************************************/

static int cirrus_vga_read_sr(CirrusVGAState * s)
{
    switch (s->vga.sr_index) {
    case 0x00:                  // Standard VGA
    case 0x01:                  // Standard VGA
    case 0x02:                  // Standard VGA
    case 0x03:                  // Standard VGA
    case 0x04:                  // Standard VGA
        return s->vga.sr[s->vga.sr_index];
    case 0x06:                  // Unlock Cirrus extensions
        return s->vga.sr[s->vga.sr_index];
    case 0x10:
    case 0x30:
    case 0x50:
    case 0x70:                  // Graphics Cursor X
    case 0x90:
    case 0xb0:
    case 0xd0:
    case 0xf0:                  // Graphics Cursor X
        return s->vga.sr[0x10];
    case 0x11:
    case 0x31:
    case 0x51:
    case 0x71:                  // Graphics Cursor Y
    case 0x91:
    case 0xb1:
    case 0xd1:
    case 0xf1:                  // Graphics Cursor Y
        return s->vga.sr[0x11];
    case 0x05:                  // ???
    case 0x07:                  // Extended Sequencer Mode
    case 0x08:                  // EEPROM Control
    case 0x09:                  // Scratch Register 0
    case 0x0a:                  // Scratch Register 1
    case 0x0b:                  // VCLK 0
    case 0x0c:                  // VCLK 1
    case 0x0d:                  // VCLK 2
    case 0x0e:                  // VCLK 3
    case 0x0f:                  // DRAM Control
    case 0x12:                  // Graphics Cursor Attribute
    case 0x13:                  // Graphics Cursor Pattern Address
    case 0x14:                  // Scratch Register 2
    case 0x15:                  // Scratch Register 3
    case 0x16:                  // Performance Tuning Register
    case 0x17:                  // Configuration Readback and Extended Control
    case 0x18:                  // Signature Generator Control
    case 0x19:                  // Signal Generator Result
    case 0x1a:                  // Signal Generator Result
    case 0x1b:                  // VCLK 0 Denominator & Post
    case 0x1c:                  // VCLK 1 Denominator & Post
    case 0x1d:                  // VCLK 2 Denominator & Post
    case 0x1e:                  // VCLK 3 Denominator & Post
    case 0x1f:                  // BIOS Write Enable and MCLK select
#ifdef DEBUG_CIRRUS
        printf("cirrus: handled inport sr_index %02x\n", s->vga.sr_index);
#endif
        return s->vga.sr[s->vga.sr_index];
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cirrus: inport sr_index 0x%02x\n", s->vga.sr_index);
        return 0xff;
    }
}

static void cirrus_vga_write_sr(CirrusVGAState * s, uint32_t val)
{
    switch (s->vga.sr_index) {
    case 0x00:                  // Standard VGA
    case 0x01:                  // Standard VGA
    case 0x02:                  // Standard VGA
    case 0x03:                  // Standard VGA
    case 0x04:                  // Standard VGA
        s->vga.sr[s->vga.sr_index] = val & sr_mask[s->vga.sr_index];
        if (s->vga.sr_index == 1)
            s->vga.update_retrace_info(&s->vga);
        break;
    case 0x06:                  // Unlock Cirrus extensions
        val &= 0x17;
        if (val == 0x12) {
            s->vga.sr[s->vga.sr_index] = 0x12;
        } else {
            s->vga.sr[s->vga.sr_index] = 0x0f;
        }
        break;
    case 0x10:
    case 0x30:
    case 0x50:
    case 0x70:                  // Graphics Cursor X
    case 0x90:
    case 0xb0:
    case 0xd0:
    case 0xf0:                  // Graphics Cursor X
        s->vga.sr[0x10] = val;
        s->vga.hw_cursor_x = (val << 3) | (s->vga.sr_index >> 5);
        break;
    case 0x11:
    case 0x31:
    case 0x51:
    case 0x71:                  // Graphics Cursor Y
    case 0x91:
    case 0xb1:
    case 0xd1:
    case 0xf1:                  // Graphics Cursor Y
        s->vga.sr[0x11] = val;
        s->vga.hw_cursor_y = (val << 3) | (s->vga.sr_index >> 5);
        break;
    case 0x07:                  // Extended Sequencer Mode
        cirrus_update_memory_access(s);
        /* fall through */
    case 0x08:                  // EEPROM Control
    case 0x09:                  // Scratch Register 0
    case 0x0a:                  // Scratch Register 1
    case 0x0b:                  // VCLK 0
    case 0x0c:                  // VCLK 1
    case 0x0d:                  // VCLK 2
    case 0x0e:                  // VCLK 3
    case 0x0f:                  // DRAM Control
    case 0x13:                  // Graphics Cursor Pattern Address
    case 0x14:                  // Scratch Register 2
    case 0x15:                  // Scratch Register 3
    case 0x16:                  // Performance Tuning Register
    case 0x18:                  // Signature Generator Control
    case 0x19:                  // Signature Generator Result
    case 0x1a:                  // Signature Generator Result
    case 0x1b:                  // VCLK 0 Denominator & Post
    case 0x1c:                  // VCLK 1 Denominator & Post
    case 0x1d:                  // VCLK 2 Denominator & Post
    case 0x1e:                  // VCLK 3 Denominator & Post
    case 0x1f:                  // BIOS Write Enable and MCLK select
        s->vga.sr[s->vga.sr_index] = val;
#ifdef DEBUG_CIRRUS
        printf("cirrus: handled outport sr_index %02x, sr_value %02x\n",
               s->vga.sr_index, val);
#endif
        break;
    case 0x12:                  // Graphics Cursor Attribute
        s->vga.sr[0x12] = val;
        s->vga.force_shadow = !!(val & CIRRUS_CURSOR_SHOW);
#ifdef DEBUG_CIRRUS
        printf("cirrus: cursor ctl SR12=%02x (force shadow: %d)\n",
               val, s->vga.force_shadow);
#endif
        break;
    case 0x17:                  // Configuration Readback and Extended Control
        s->vga.sr[s->vga.sr_index] = (s->vga.sr[s->vga.sr_index] & 0x38)
                                   | (val & 0xc7);
        cirrus_update_memory_access(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cirrus: outport sr_index 0x%02x, sr_value 0x%02x\n",
                      s->vga.sr_index, val);
        break;
    }
}

/***************************************
 *
 *  I/O access at 0x3c6
 *
 ***************************************/

static int cirrus_read_hidden_dac(CirrusVGAState * s)
{
    if (++s->cirrus_hidden_dac_lockindex == 5) {
        s->cirrus_hidden_dac_lockindex = 0;
        return s->cirrus_hidden_dac_data;
    }
    return 0xff;
}

static void cirrus_write_hidden_dac(CirrusVGAState * s, int reg_value)
{
    if (s->cirrus_hidden_dac_lockindex == 4) {
        s->cirrus_hidden_dac_data = reg_value;
#if defined(DEBUG_CIRRUS)
        printf("cirrus: outport hidden DAC, value %02x\n", reg_value);
#endif
    }
    s->cirrus_hidden_dac_lockindex = 0;
}

/***************************************
 *
 *  I/O access at 0x3c9
 *
 ***************************************/

static int cirrus_vga_read_palette(CirrusVGAState * s)
{
    int val;

    if ((s->vga.sr[0x12] & CIRRUS_CURSOR_HIDDENPEL)) {
        val = s->cirrus_hidden_palette[(s->vga.dac_read_index & 0x0f) * 3 +
                                       s->vga.dac_sub_index];
    } else {
        val = s->vga.palette[s->vga.dac_read_index * 3 + s->vga.dac_sub_index];
    }
    if (++s->vga.dac_sub_index == 3) {
        s->vga.dac_sub_index = 0;
        s->vga.dac_read_index++;
    }
    return val;
}

static void cirrus_vga_write_palette(CirrusVGAState * s, int reg_value)
{
    s->vga.dac_cache[s->vga.dac_sub_index] = reg_value;
    if (++s->vga.dac_sub_index == 3) {
        if ((s->vga.sr[0x12] & CIRRUS_CURSOR_HIDDENPEL)) {
            memcpy(&s->cirrus_hidden_palette[(s->vga.dac_write_index & 0x0f) * 3],
                   s->vga.dac_cache, 3);
        } else {
            memcpy(&s->vga.palette[s->vga.dac_write_index * 3], s->vga.dac_cache, 3);
        }
        /* XXX update cursor */
        s->vga.dac_sub_index = 0;
        s->vga.dac_write_index++;
    }
}

/***************************************
 *
 *  I/O access between 0x3ce-0x3cf
 *
 ***************************************/

static int cirrus_vga_read_gr(CirrusVGAState * s, unsigned reg_index)
{
    switch (reg_index) {
    case 0x00: // Standard VGA, BGCOLOR 0x000000ff
        return s->cirrus_shadow_gr0;
    case 0x01: // Standard VGA, FGCOLOR 0x000000ff
        return s->cirrus_shadow_gr1;
    case 0x02:                  // Standard VGA
    case 0x03:                  // Standard VGA
    case 0x04:                  // Standard VGA
    case 0x06:                  // Standard VGA
    case 0x07:                  // Standard VGA
    case 0x08:                  // Standard VGA
        return s->vga.gr[s->vga.gr_index];
    case 0x05:                  // Standard VGA, Cirrus extended mode
    default:
        break;
    }

    if (reg_index < 0x3a) {
        return s->vga.gr[reg_index];
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cirrus: inport gr_index 0x%02x\n", reg_index);
        return 0xff;
    }
}

static void
cirrus_vga_write_gr(CirrusVGAState * s, unsigned reg_index, int reg_value)
{
    trace_vga_cirrus_write_gr(reg_index, reg_value);
    switch (reg_index) {
    case 0x00:                  // Standard VGA, BGCOLOR 0x000000ff
        s->vga.gr[reg_index] = reg_value & gr_mask[reg_index];
        s->cirrus_shadow_gr0 = reg_value;
        break;
    case 0x01:                  // Standard VGA, FGCOLOR 0x000000ff
        s->vga.gr[reg_index] = reg_value & gr_mask[reg_index];
        s->cirrus_shadow_gr1 = reg_value;
        break;
    case 0x02:                  // Standard VGA
    case 0x03:                  // Standard VGA
    case 0x04:                  // Standard VGA
    case 0x06:                  // Standard VGA
    case 0x07:                  // Standard VGA
    case 0x08:                  // Standard VGA
        s->vga.gr[reg_index] = reg_value & gr_mask[reg_index];
        break;
    case 0x05:                  // Standard VGA, Cirrus extended mode
        s->vga.gr[reg_index] = reg_value & 0x7f;
        cirrus_update_memory_access(s);
        break;
    case 0x09:                  // bank offset #0
    case 0x0A:                  // bank offset #1
        s->vga.gr[reg_index] = reg_value;
        cirrus_update_bank_ptr(s, 0);
        cirrus_update_bank_ptr(s, 1);
        cirrus_update_memory_access(s);
        break;
    case 0x0B:
        s->vga.gr[reg_index] = reg_value;
        cirrus_update_bank_ptr(s, 0);
        cirrus_update_bank_ptr(s, 1);
        cirrus_update_memory_access(s);
        break;
    case 0x10:                  // BGCOLOR 0x0000ff00
    case 0x11:                  // FGCOLOR 0x0000ff00
    case 0x12:                  // BGCOLOR 0x00ff0000
    case 0x13:                  // FGCOLOR 0x00ff0000
    case 0x14:                  // BGCOLOR 0xff000000
    case 0x15:                  // FGCOLOR 0xff000000
    case 0x20:                  // BLT WIDTH 0x0000ff
    case 0x22:                  // BLT HEIGHT 0x0000ff
    case 0x24:                  // BLT DEST PITCH 0x0000ff
    case 0x26:                  // BLT SRC PITCH 0x0000ff
    case 0x28:                  // BLT DEST ADDR 0x0000ff
    case 0x29:                  // BLT DEST ADDR 0x00ff00
    case 0x2c:                  // BLT SRC ADDR 0x0000ff
    case 0x2d:                  // BLT SRC ADDR 0x00ff00
    case 0x2f:                  // BLT WRITEMASK
    case 0x30:                  // BLT MODE
    case 0x32:                  // RASTER OP
    case 0x33:                  // BLT MODEEXT
    case 0x34:                  // BLT TRANSPARENT COLOR 0x00ff
    case 0x35:                  // BLT TRANSPARENT COLOR 0xff00
    case 0x38:                  // BLT TRANSPARENT COLOR MASK 0x00ff
    case 0x39:                  // BLT TRANSPARENT COLOR MASK 0xff00
        s->vga.gr[reg_index] = reg_value;
        break;
    case 0x21:                  // BLT WIDTH 0x001f00
    case 0x23:                  // BLT HEIGHT 0x001f00
    case 0x25:                  // BLT DEST PITCH 0x001f00
    case 0x27:                  // BLT SRC PITCH 0x001f00
        s->vga.gr[reg_index] = reg_value & 0x1f;
        break;
    case 0x2a:                  // BLT DEST ADDR 0x3f0000
        s->vga.gr[reg_index] = reg_value & 0x3f;
        /* if auto start mode, starts bit blt now */
        if (s->vga.gr[0x31] & CIRRUS_BLT_AUTOSTART) {
            cirrus_bitblt_start(s);
        }
        break;
    case 0x2e:                  // BLT SRC ADDR 0x3f0000
        s->vga.gr[reg_index] = reg_value & 0x3f;
        break;
    case 0x31:                  // BLT STATUS/START
        cirrus_write_bitblt(s, reg_value);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cirrus: outport gr_index 0x%02x, gr_value 0x%02x\n",
                      reg_index, reg_value);
        break;
    }
}

/***************************************
 *
 *  I/O access between 0x3d4-0x3d5
 *
 ***************************************/

static int cirrus_vga_read_cr(CirrusVGAState * s, unsigned reg_index)
{
    switch (reg_index) {
    case 0x00:                  // Standard VGA
    case 0x01:                  // Standard VGA
    case 0x02:                  // Standard VGA
    case 0x03:                  // Standard VGA
    case 0x04:                  // Standard VGA
    case 0x05:                  // Standard VGA
    case 0x06:                  // Standard VGA
    case 0x07:                  // Standard VGA
    case 0x08:                  // Standard VGA
    case 0x09:                  // Standard VGA
    case 0x0a:                  // Standard VGA
    case 0x0b:                  // Standard VGA
    case 0x0c:                  // Standard VGA
    case 0x0d:                  // Standard VGA
    case 0x0e:                  // Standard VGA
    case 0x0f:                  // Standard VGA
    case 0x10:                  // Standard VGA
    case 0x11:                  // Standard VGA
    case 0x12:                  // Standard VGA
    case 0x13:                  // Standard VGA
    case 0x14:                  // Standard VGA
    case 0x15:                  // Standard VGA
    case 0x16:                  // Standard VGA
    case 0x17:                  // Standard VGA
    case 0x18:                  // Standard VGA
        return s->vga.cr[s->vga.cr_index];
    case 0x24:                  // Attribute Controller Toggle Readback (R)
        return (s->vga.ar_flip_flop << 7);
    case 0x19:                  // Interlace End
    case 0x1a:                  // Miscellaneous Control
    case 0x1b:                  // Extended Display Control
    case 0x1c:                  // Sync Adjust and Genlock
    case 0x1d:                  // Overlay Extended Control
    case 0x22:                  // Graphics Data Latches Readback (R)
    case 0x25:                  // Part Status
    case 0x27:                  // Part ID (R)
        return s->vga.cr[s->vga.cr_index];
    case 0x26:                  // Attribute Controller Index Readback (R)
        return s->vga.ar_index & 0x3f;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cirrus: inport cr_index 0x%02x\n", reg_index);
        return 0xff;
    }
}

static void cirrus_vga_write_cr(CirrusVGAState * s, int reg_value)
{
    switch (s->vga.cr_index) {
    case 0x00:                  // Standard VGA
    case 0x01:                  // Standard VGA
    case 0x02:                  // Standard VGA
    case 0x03:                  // Standard VGA
    case 0x04:                  // Standard VGA
    case 0x05:                  // Standard VGA
    case 0x06:                  // Standard VGA
    case 0x07:                  // Standard VGA
    case 0x08:                  // Standard VGA
    case 0x09:                  // Standard VGA
    case 0x0a:                  // Standard VGA
    case 0x0b:                  // Standard VGA
    case 0x0c:                  // Standard VGA
    case 0x0d:                  // Standard VGA
    case 0x0e:                  // Standard VGA
    case 0x0f:                  // Standard VGA
    case 0x10:                  // Standard VGA
    case 0x11:                  // Standard VGA
    case 0x12:                  // Standard VGA
    case 0x13:                  // Standard VGA
    case 0x14:                  // Standard VGA
    case 0x15:                  // Standard VGA
    case 0x16:                  // Standard VGA
    case 0x17:                  // Standard VGA
    case 0x18:                  // Standard VGA
        /* handle CR0-7 protection */
        if ((s->vga.cr[0x11] & 0x80) && s->vga.cr_index <= 7) {
            /* can always write bit 4 of CR7 */
            if (s->vga.cr_index == 7)
                s->vga.cr[7] = (s->vga.cr[7] & ~0x10) | (reg_value & 0x10);
            return;
        }
        s->vga.cr[s->vga.cr_index] = reg_value;
        switch(s->vga.cr_index) {
        case 0x00:
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07:
        case 0x11:
        case 0x17:
            s->vga.update_retrace_info(&s->vga);
            break;
        }
        break;
    case 0x19:                  // Interlace End
    case 0x1a:                  // Miscellaneous Control
    case 0x1b:                  // Extended Display Control
    case 0x1c:                  // Sync Adjust and Genlock
    case 0x1d:                  // Overlay Extended Control
        s->vga.cr[s->vga.cr_index] = reg_value;
#ifdef DEBUG_CIRRUS
        printf("cirrus: handled outport cr_index %02x, cr_value %02x\n",
               s->vga.cr_index, reg_value);
#endif
        break;
    case 0x22:                  // Graphics Data Latches Readback (R)
    case 0x24:                  // Attribute Controller Toggle Readback (R)
    case 0x26:                  // Attribute Controller Index Readback (R)
    case 0x27:                  // Part ID (R)
        break;
    case 0x25:                  // Part Status
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cirrus: outport cr_index 0x%02x, cr_value 0x%02x\n",
                      s->vga.cr_index, reg_value);
        break;
    }
}

/***************************************
 *
 *  write mode 4/5
 *
 ***************************************/

static void cirrus_mem_writeb_mode4and5_8bpp(CirrusVGAState * s,
                                             unsigned mode,
                                             unsigned offset,
                                             uint32_t mem_value)
{
    int x;
    unsigned val = mem_value;
    uint8_t *dst;

    for (x = 0; x < 8; x++) {
        dst = s->vga.vram_ptr + ((offset + x) & s->cirrus_addr_mask);
        if (val & 0x80) {
            *dst = s->cirrus_shadow_gr1;
        } else if (mode == 5) {
            *dst = s->cirrus_shadow_gr0;
        }
        val <<= 1;
    }
    memory_region_set_dirty(&s->vga.vram, offset, 8);
}

static void cirrus_mem_writeb_mode4and5_16bpp(CirrusVGAState * s,
                                              unsigned mode,
                                              unsigned offset,
                                              uint32_t mem_value)
{
    int x;
    unsigned val = mem_value;
    uint8_t *dst;

    for (x = 0; x < 8; x++) {
        dst = s->vga.vram_ptr + ((offset + 2 * x) & s->cirrus_addr_mask & ~1);
        if (val & 0x80) {
            *dst = s->cirrus_shadow_gr1;
            *(dst + 1) = s->vga.gr[0x11];
        } else if (mode == 5) {
            *dst = s->cirrus_shadow_gr0;
            *(dst + 1) = s->vga.gr[0x10];
        }
        val <<= 1;
    }
    memory_region_set_dirty(&s->vga.vram, offset, 16);
}

/***************************************
 *
 *  memory access between 0xa0000-0xbffff
 *
 *  (MMIO branch removed: the WAB never exposes the Cirrus MMIO aperture.)
 *
 ***************************************/

static uint64_t cirrus_vga_mem_read(void *opaque,
                                    hwaddr addr,
                                    uint32_t size)
{
    CirrusVGAState *s = opaque;
    unsigned bank_index;
    unsigned bank_offset;
    uint32_t val;

    if ((s->vga.sr[0x07] & 0x01) == 0) {
        return vga_mem_readb(&s->vga, addr);
    }

    if (addr < 0x10000) {
        /* video memory */
        bank_index = addr >> 15;
        bank_offset = addr & 0x7fff;
        if (bank_offset < s->cirrus_bank_limit[bank_index]) {
            bank_offset += s->cirrus_bank_base[bank_index];
            if ((s->vga.gr[0x0B] & 0x14) == 0x14) {
                bank_offset <<= 4;
            } else if (s->vga.gr[0x0B] & 0x02) {
                bank_offset <<= 3;
            }
            bank_offset &= s->cirrus_addr_mask;
            val = *(s->vga.vram_ptr + bank_offset);
        } else
            val = 0xff;
    } else {
        val = 0xff;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cirrus: mem_readb 0x" HWADDR_FMT_plx "\n", addr);
    }
    return val;
}

static void cirrus_vga_mem_write(void *opaque,
                                 hwaddr addr,
                                 uint64_t mem_value,
                                 uint32_t size)
{
    CirrusVGAState *s = opaque;
    unsigned bank_index;
    unsigned bank_offset;
    unsigned mode;

    if ((s->vga.sr[0x07] & 0x01) == 0) {
        vga_mem_writeb(&s->vga, addr, mem_value);
        return;
    }

    if (addr < 0x10000) {
        if (s->cirrus_srcptr != s->cirrus_srcptr_end) {
            /* bitblt */
            *s->cirrus_srcptr++ = (uint8_t) mem_value;
            if (s->cirrus_srcptr >= s->cirrus_srcptr_end) {
                cirrus_bitblt_cputovideo_next(s);
            }
        } else {
            /* video memory */
            bank_index = addr >> 15;
            bank_offset = addr & 0x7fff;
            if (bank_offset < s->cirrus_bank_limit[bank_index]) {
                bank_offset += s->cirrus_bank_base[bank_index];
                if ((s->vga.gr[0x0B] & 0x14) == 0x14) {
                    bank_offset <<= 4;
                } else if (s->vga.gr[0x0B] & 0x02) {
                    bank_offset <<= 3;
                }
                bank_offset &= s->cirrus_addr_mask;
                mode = s->vga.gr[0x05] & 0x7;
                if (mode < 4 || mode > 5 || ((s->vga.gr[0x0B] & 0x4) == 0)) {
                    *(s->vga.vram_ptr + bank_offset) = mem_value;
                    memory_region_set_dirty(&s->vga.vram, bank_offset,
                                            sizeof(mem_value));
                } else {
                    if ((s->vga.gr[0x0B] & 0x14) != 0x14) {
                        cirrus_mem_writeb_mode4and5_8bpp(s, mode,
                                                         bank_offset,
                                                         mem_value);
                    } else {
                        cirrus_mem_writeb_mode4and5_16bpp(s, mode,
                                                          bank_offset,
                                                          mem_value);
                    }
                }
            }
        }
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cirrus: mem_writeb 0x" HWADDR_FMT_plx " "
                      "value 0x%02" PRIx64 "\n", addr, mem_value);
    }
}

static const MemoryRegionOps cirrus_vga_mem_ops = {
    .read = cirrus_vga_mem_read,
    .write = cirrus_vga_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/***************************************
 *
 *  hardware cursor
 *
 ***************************************/

static inline void invalidate_cursor1(CirrusVGAState *s)
{
    if (s->last_hw_cursor_size) {
        vga_invalidate_scanlines(&s->vga,
                                 s->last_hw_cursor_y + s->last_hw_cursor_y_start,
                                 s->last_hw_cursor_y + s->last_hw_cursor_y_end);
    }
}

static inline void cirrus_cursor_compute_yrange(CirrusVGAState *s)
{
    const uint8_t *src;
    uint32_t content;
    int y, y_min, y_max;

    src = s->vga.vram_ptr + s->real_vram_size - 16 * KiB;
    if (s->vga.sr[0x12] & CIRRUS_CURSOR_LARGE) {
        src += (s->vga.sr[0x13] & 0x3c) * 256;
        y_min = 64;
        y_max = -1;
        for(y = 0; y < 64; y++) {
            content = ((uint32_t *)src)[0] |
                ((uint32_t *)src)[1] |
                ((uint32_t *)src)[2] |
                ((uint32_t *)src)[3];
            if (content) {
                if (y < y_min)
                    y_min = y;
                if (y > y_max)
                    y_max = y;
            }
            src += 16;
        }
    } else {
        src += (s->vga.sr[0x13] & 0x3f) * 256;
        y_min = 32;
        y_max = -1;
        for(y = 0; y < 32; y++) {
            content = ((uint32_t *)src)[0] |
                ((uint32_t *)(src + 128))[0];
            if (content) {
                if (y < y_min)
                    y_min = y;
                if (y > y_max)
                    y_max = y;
            }
            src += 4;
        }
    }
    if (y_min > y_max) {
        s->last_hw_cursor_y_start = 0;
        s->last_hw_cursor_y_end = 0;
    } else {
        s->last_hw_cursor_y_start = y_min;
        s->last_hw_cursor_y_end = y_max + 1;
    }
}

/* NOTE: we do not currently handle the cursor bitmap change, so we
   update the cursor only if it moves. */
static void cirrus_cursor_invalidate(VGACommonState *s1)
{
    CirrusVGAState *s = container_of(s1, CirrusVGAState, vga);
    int size;

    if (!(s->vga.sr[0x12] & CIRRUS_CURSOR_SHOW)) {
        size = 0;
    } else {
        if (s->vga.sr[0x12] & CIRRUS_CURSOR_LARGE)
            size = 64;
        else
            size = 32;
    }
    /* invalidate last cursor and new cursor if any change */
    if (s->last_hw_cursor_size != size ||
        s->last_hw_cursor_x != s->vga.hw_cursor_x ||
        s->last_hw_cursor_y != s->vga.hw_cursor_y) {

        invalidate_cursor1(s);

        s->last_hw_cursor_size = size;
        s->last_hw_cursor_x = s->vga.hw_cursor_x;
        s->last_hw_cursor_y = s->vga.hw_cursor_y;
        /* compute the real cursor min and max y */
        cirrus_cursor_compute_yrange(s);
        invalidate_cursor1(s);
    }
}

static void vga_draw_cursor_line(uint8_t *d1,
                                 const uint8_t *src1,
                                 int poffset, int w,
                                 unsigned int color0,
                                 unsigned int color1,
                                 unsigned int color_xor)
{
    const uint8_t *plane0, *plane1;
    int x, b0, b1;
    uint8_t *d;

    d = d1;
    plane0 = src1;
    plane1 = src1 + poffset;
    for (x = 0; x < w; x++) {
        b0 = (plane0[x >> 3] >> (7 - (x & 7))) & 1;
        b1 = (plane1[x >> 3] >> (7 - (x & 7))) & 1;
        switch (b0 | (b1 << 1)) {
        case 0:
            break;
        case 1:
            ((uint32_t *)d)[0] ^= color_xor;
            break;
        case 2:
            ((uint32_t *)d)[0] = color0;
            break;
        case 3:
            ((uint32_t *)d)[0] = color1;
            break;
        }
        d += 4;
    }
}

static void cirrus_cursor_draw_line(VGACommonState *s1, uint8_t *d1, int scr_y)
{
    CirrusVGAState *s = container_of(s1, CirrusVGAState, vga);
    int w, h, x1, x2, poffset;
    unsigned int color0, color1;
    const uint8_t *palette, *src;
    uint32_t content;

    if (!(s->vga.sr[0x12] & CIRRUS_CURSOR_SHOW))
        return;
    /* fast test to see if the cursor intersects with the scan line */
    if (s->vga.sr[0x12] & CIRRUS_CURSOR_LARGE) {
        h = 64;
    } else {
        h = 32;
    }
    if (scr_y < s->vga.hw_cursor_y ||
        scr_y >= (s->vga.hw_cursor_y + h)) {
        return;
    }

    src = s->vga.vram_ptr + s->real_vram_size - 16 * KiB;
    if (s->vga.sr[0x12] & CIRRUS_CURSOR_LARGE) {
        src += (s->vga.sr[0x13] & 0x3c) * 256;
        src += (scr_y - s->vga.hw_cursor_y) * 16;
        poffset = 8;
        content = ((uint32_t *)src)[0] |
            ((uint32_t *)src)[1] |
            ((uint32_t *)src)[2] |
            ((uint32_t *)src)[3];
    } else {
        src += (s->vga.sr[0x13] & 0x3f) * 256;
        src += (scr_y - s->vga.hw_cursor_y) * 4;


        poffset = 128;
        content = ((uint32_t *)src)[0] |
            ((uint32_t *)(src + 128))[0];
    }
    /* if nothing to draw, no need to continue */
    if (!content)
        return;
    w = h;

    x1 = s->vga.hw_cursor_x;
    if (x1 >= s->vga.last_scr_width)
        return;
    x2 = s->vga.hw_cursor_x + w;
    if (x2 > s->vga.last_scr_width)
        x2 = s->vga.last_scr_width;
    w = x2 - x1;
    palette = s->cirrus_hidden_palette;
    color0 = rgb_to_pixel32(c6_to_8(palette[0x0 * 3]),
                            c6_to_8(palette[0x0 * 3 + 1]),
                            c6_to_8(palette[0x0 * 3 + 2]));
    color1 = rgb_to_pixel32(c6_to_8(palette[0xf * 3]),
                            c6_to_8(palette[0xf * 3 + 1]),
                            c6_to_8(palette[0xf * 3 + 2]));
    d1 += x1 * 4;
    vga_draw_cursor_line(d1, src, poffset, w, color0, color1, 0xffffff);
}

/***************************************
 *
 *  LFB memory access
 *
 *  (MMIO branch removed: the WAB never exposes the Cirrus MMIO aperture.)
 *
 ***************************************/

static uint64_t cirrus_linear_read(void *opaque, hwaddr addr,
                                   unsigned size)
{
    CirrusVGAState *s = opaque;
    uint32_t ret;

    addr &= s->cirrus_addr_mask;

    /* video memory */
    if ((s->vga.gr[0x0B] & 0x14) == 0x14) {
        addr <<= 4;
    } else if (s->vga.gr[0x0B] & 0x02) {
        addr <<= 3;
    }
    addr &= s->cirrus_addr_mask;
    ret = *(s->vga.vram_ptr + addr);

    return ret;
}

static void cirrus_linear_write(void *opaque, hwaddr addr,
                                uint64_t val, unsigned size)
{
    CirrusVGAState *s = opaque;
    unsigned mode;

    addr &= s->cirrus_addr_mask;

    if (s->cirrus_srcptr != s->cirrus_srcptr_end) {
        /* bitblt */
        *s->cirrus_srcptr++ = (uint8_t) val;
        if (s->cirrus_srcptr >= s->cirrus_srcptr_end) {
            cirrus_bitblt_cputovideo_next(s);
        }
    } else {
        /* video memory */
        if ((s->vga.gr[0x0B] & 0x14) == 0x14) {
            addr <<= 4;
        } else if (s->vga.gr[0x0B] & 0x02) {
            addr <<= 3;
        }
        addr &= s->cirrus_addr_mask;

        mode = s->vga.gr[0x05] & 0x7;
        if (mode < 4 || mode > 5 || ((s->vga.gr[0x0B] & 0x4) == 0)) {
            *(s->vga.vram_ptr + addr) = (uint8_t) val;
            memory_region_set_dirty(&s->vga.vram, addr, 1);
        } else {
            if ((s->vga.gr[0x0B] & 0x14) != 0x14) {
                cirrus_mem_writeb_mode4and5_8bpp(s, mode, addr, val);
            } else {
                cirrus_mem_writeb_mode4and5_16bpp(s, mode, addr, val);
            }
        }
    }
}

/***************************************
 *
 *  system-to-screen (CPU-source) BitBLT
 *
 ***************************************/

static void map_linear_vram_bank(CirrusVGAState *s, unsigned bank)
{
    MemoryRegion *mr = &s->cirrus_bank[bank];
    bool enabled = !(s->cirrus_srcptr != s->cirrus_srcptr_end)
        && !((s->vga.sr[0x07] & 0x01) == 0)
        && !((s->vga.gr[0x0B] & 0x14) == 0x14)
        && !(s->vga.gr[0x0B] & 0x02);

    memory_region_set_enabled(mr, enabled);
    memory_region_set_alias_offset(mr, s->cirrus_bank_base[bank]);
}

static void map_linear_vram(CirrusVGAState *s)
{
    map_linear_vram_bank(s, 0);
    map_linear_vram_bank(s, 1);
}

static void unmap_linear_vram(CirrusVGAState *s)
{
    memory_region_set_enabled(&s->cirrus_bank[0], false);
    memory_region_set_enabled(&s->cirrus_bank[1], false);
}

/* Compute the memory access functions */
static void cirrus_update_memory_access(CirrusVGAState *s)
{
    unsigned mode;

    memory_region_transaction_begin();
    if ((s->vga.sr[0x17] & 0x44) == 0x44) {
        goto generic_io;
    } else if (s->cirrus_srcptr != s->cirrus_srcptr_end) {
        goto generic_io;
    } else {
        if ((s->vga.gr[0x0B] & 0x14) == 0x14) {
            goto generic_io;
        } else if (s->vga.gr[0x0B] & 0x02) {
            goto generic_io;
        }

        mode = s->vga.gr[0x05] & 0x7;
        if (mode < 4 || mode > 5 || ((s->vga.gr[0x0B] & 0x4) == 0)) {
            map_linear_vram(s);
        } else {
        generic_io:
            unmap_linear_vram(s);
        }
    }
    memory_region_transaction_commit();
}


/* I/O ports */

static uint64_t cirrus_vga_ioport_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    CirrusVGAState *c = opaque;
    VGACommonState *s = &c->vga;
    int val, index;

    addr += 0x3b0;

    if (vga_ioport_invalid(s, addr)) {
        val = 0xff;
    } else {
        switch (addr) {
        case 0x3c0:
            if (s->ar_flip_flop == 0) {
                val = s->ar_index;
            } else {
                val = 0;
            }
            break;
        case 0x3c1:
            index = s->ar_index & 0x1f;
            if (index < 21)
                val = s->ar[index];
            else
                val = 0;
            break;
        case 0x3c2:
            val = s->st00;
            break;
        case 0x3c4:
            val = s->sr_index;
            break;
        case 0x3c5:
            val = cirrus_vga_read_sr(c);
            break;
            break;
        case 0x3c6:
            val = cirrus_read_hidden_dac(c);
            break;
        case 0x3c7:
            val = s->dac_state;
            break;
        case 0x3c8:
            val = s->dac_write_index;
            c->cirrus_hidden_dac_lockindex = 0;
            break;
        case 0x3c9:
            val = cirrus_vga_read_palette(c);
            break;
        case 0x3ca:
            val = s->fcr;
            break;
        case 0x3cc:
            val = s->msr;
            break;
        case 0x3ce:
            val = s->gr_index;
            break;
        case 0x3cf:
            val = cirrus_vga_read_gr(c, s->gr_index);
            break;
        case 0x3b4:
        case 0x3d4:
            val = s->cr_index;
            break;
        case 0x3b5:
        case 0x3d5:
            val = cirrus_vga_read_cr(c, s->cr_index);
            break;
        case 0x3ba:
        case 0x3da:
            /* just toggle to fool polling */
            val = s->st01 = s->retrace(s);
            s->ar_flip_flop = 0;
            break;
        default:
            val = 0x00;
            break;
        }
    }
    trace_vga_cirrus_read_io(addr, val);
    return val;
}

static void cirrus_vga_ioport_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    CirrusVGAState *c = opaque;
    VGACommonState *s = &c->vga;
    int index;

    addr += 0x3b0;

    /* check port range access depending on color/monochrome mode */
    if (vga_ioport_invalid(s, addr)) {
        return;
    }
    trace_vga_cirrus_write_io(addr, val);

    switch (addr) {
    case 0x3c0:
        if (s->ar_flip_flop == 0) {
            val &= 0x3f;
            s->ar_index = val;
        } else {
            index = s->ar_index & 0x1f;
            switch (index) {
            case 0x00 ... 0x0f:
                s->ar[index] = val & 0x3f;
                break;
            case 0x10:
                s->ar[index] = val & ~0x10;
                break;
            case 0x11:
                s->ar[index] = val;
                break;
            case 0x12:
                s->ar[index] = val & ~0xc0;
                break;
            case 0x13:
                s->ar[index] = val & ~0xf0;
                break;
            case 0x14:
                s->ar[index] = val & ~0xf0;
                break;
            default:
                break;
            }
        }
        s->ar_flip_flop ^= 1;
        break;
    case 0x3c2:
        s->msr = val & ~0x10;
        s->update_retrace_info(s);
        break;
    case 0x3c4:
        s->sr_index = val;
        break;
    case 0x3c5:
        cirrus_vga_write_sr(c, val);
        break;
    case 0x3c6:
        cirrus_write_hidden_dac(c, val);
        break;
    case 0x3c7:
        s->dac_read_index = val;
        s->dac_sub_index = 0;
        s->dac_state = 3;
        break;
    case 0x3c8:
        s->dac_write_index = val;
        s->dac_sub_index = 0;
        s->dac_state = 0;
        break;
    case 0x3c9:
        cirrus_vga_write_palette(c, val);
        break;
    case 0x3ce:
        s->gr_index = val;
        break;
    case 0x3cf:
        cirrus_vga_write_gr(c, s->gr_index, val);
        break;
    case 0x3b4:
    case 0x3d4:
        s->cr_index = val;
        break;
    case 0x3b5:
    case 0x3d5:
        cirrus_vga_write_cr(c, val);
        break;
    case 0x3ba:
    case 0x3da:
        s->fcr = val & 0x10;
        break;
    }
}

/*
 * Migration state is intentionally omitted: the WAB device has no vmstate
 * (see pc98_wab_class_init), so the Cirrus core vmstate is not needed here.
 */

/***************************************
 *
 *  initialize
 *
 ***************************************/

static void cirrus_reset(void *opaque)
{
    CirrusVGAState *s = opaque;

    vga_common_reset(&s->vga);
    unmap_linear_vram(s);
    s->vga.sr[0x06] = 0x0f;
    s->vga.sr[0x1F] = 0x22;         // MemClock
    s->vga.sr[0x0F] = CIRRUS_MEMSIZE_2M;
    s->vga.sr[0x17] = CIRRUS_BUSTYPE_ISA;
    s->vga.sr[0x15] = 0x03; /* memory size, 3=2MB, 4=4MB */
    s->vga.cr[0x27] = s->device_id;

    s->cirrus_hidden_dac_lockindex = 5;
    s->cirrus_hidden_dac_data = 0;
}

static const MemoryRegionOps cirrus_linear_io_ops = {
    .read = cirrus_linear_read,
    .write = cirrus_linear_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static const MemoryRegionOps cirrus_vga_io_ops = {
    .read = cirrus_vga_ioport_read,
    .write = cirrus_vga_ioport_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * Forked from cirrus_init_common(), hardcoded to the GD5428 and with the
 * MMIO aperture removed.  The 0x3b0 register block and the 0xa0000 low-memory
 * window are mapped into the caller-supplied containers (the WAB's private
 * io/mem regions), never into the real system spaces.
 */
static void wab_cirrus_init(CirrusVGAState *s, Object *owner,
                            MemoryRegion *system_memory,
                            MemoryRegion *system_io)
{
    int i;
    static int inited;

    if (!inited) {
        inited = 1;
        for(i = 0;i < 256; i++)
            rop_to_index[i] = CIRRUS_ROP_NOP_INDEX; /* nop rop */
        rop_to_index[CIRRUS_ROP_0] = 0;
        rop_to_index[CIRRUS_ROP_SRC_AND_DST] = 1;
        rop_to_index[CIRRUS_ROP_NOP] = 2;
        rop_to_index[CIRRUS_ROP_SRC_AND_NOTDST] = 3;
        rop_to_index[CIRRUS_ROP_NOTDST] = 4;
        rop_to_index[CIRRUS_ROP_SRC] = 5;
        rop_to_index[CIRRUS_ROP_1] = 6;
        rop_to_index[CIRRUS_ROP_NOTSRC_AND_DST] = 7;
        rop_to_index[CIRRUS_ROP_SRC_XOR_DST] = 8;
        rop_to_index[CIRRUS_ROP_SRC_OR_DST] = 9;
        rop_to_index[CIRRUS_ROP_NOTSRC_OR_NOTDST] = 10;
        rop_to_index[CIRRUS_ROP_SRC_NOTXOR_DST] = 11;
        rop_to_index[CIRRUS_ROP_SRC_OR_NOTDST] = 12;
        rop_to_index[CIRRUS_ROP_NOTSRC] = 13;
        rop_to_index[CIRRUS_ROP_NOTSRC_OR_DST] = 14;
        rop_to_index[CIRRUS_ROP_NOTSRC_AND_NOTDST] = 15;
    }
    s->device_id = CIRRUS_ID_CLGD5428;

    /* Register ioport 0x3b0 - 0x3df */
    memory_region_init_io(&s->cirrus_vga_io, owner, &cirrus_vga_io_ops, s,
                          "cirrus-io", 0x30);
    memory_region_set_flush_coalesced(&s->cirrus_vga_io);
    memory_region_add_subregion(system_io, 0x3b0, &s->cirrus_vga_io);

    memory_region_init(&s->low_mem_container, owner,
                       "cirrus-lowmem-container",
                       0x20000);

    memory_region_init_io(&s->low_mem, owner, &cirrus_vga_mem_ops, s,
                          "cirrus-low-memory", 0x20000);
    memory_region_add_subregion(&s->low_mem_container, 0, &s->low_mem);
    for (i = 0; i < 2; ++i) {
        static const char *names[] = { "vga.bank0", "vga.bank1" };
        MemoryRegion *bank = &s->cirrus_bank[i];
        memory_region_init_alias(bank, owner, names[i], &s->vga.vram,
                                 0, 0x8000);
        memory_region_set_enabled(bank, false);
        memory_region_add_subregion_overlap(&s->low_mem_container, i * 0x8000,
                                            bank, 1);
    }
    memory_region_add_subregion_overlap(system_memory,
                                        0x000a0000,
                                        &s->low_mem_container,
                                        1);
    memory_region_set_coalescing(&s->low_mem);

    /*
     * I/O handler for the linear framebuffer window.  This is the only host
     * aperture the WAB exposes; it doubles as the CPU-source BLT FIFO sink
     * (see the system-to-screen BitBLT note above), so no separate bitblt
     * aperture is created.
     */
    memory_region_init_io(&s->cirrus_linear_io, owner, &cirrus_linear_io_ops, s,
                          "cirrus-linear-io", s->vga.vram_size_mb * MiB);
    memory_region_set_flush_coalesced(&s->cirrus_linear_io);

    s->real_vram_size = 2 * MiB; /* GD5428: keep 2MB so the masks work */

    /* XXX: s->vga.vram_size must be a power of two */
    s->cirrus_addr_mask = s->real_vram_size - 1;

    s->vga.get_bpp = cirrus_get_bpp;
    s->vga.get_params = cirrus_get_params;
    s->vga.get_resolution = cirrus_get_resolution;
    s->vga.cursor_invalidate = cirrus_cursor_invalidate;
    s->vga.cursor_draw_line = cirrus_cursor_draw_line;

    s->vga.big_endian_fb = false;

    qemu_register_reset(cirrus_reset, s);
}

/*
 * ============================================================================
 *  PC-98 Window Accelerator Board (WAB) software interface
 * ============================================================================
 *
 * On a real PC-9821 the CPU cannot see the Cirrus GD54xx chip's
 * native VGA register block, its DAC, or its framebuffer at the usual
 * VGA addresses.  An NEC LSI on the CPU local bus interposes and
 * re-exposes the chip through a PC-98-specific I/O map.  Two
 * software-visible layouts exist; this device implements "IoVariant 1",
 * the layout used by the desktop Mate B (Bp/Bs/Be, CL-GD5428) and
 * by the NEC display miniport's internal "path 08h".
 *
 *  --- Two-stage indexed control interface ---------------------------------
 *
 *  Ports 0xfaa (index) / 0xfab (data) -- mirrored at 0xfa2 / 0xfa3 -- form an
 *  index/data register file (see wab_ctl_read()/wab_ctl_write()):
 *
 *    reg 0x00  machine / interface ID (read only).  Cirrus WAB / Core-Graph
 *              boards answer 0x50-0x5d or 0x70; 0x00/0xff means "no two-stage
 *              accelerator here" (e.g. a PCI-attached chip).  The NEC path-08h
 *              range is 0x58-0x5d.  This device returns 0x5b.
 *    reg 0x01  banked 32KB VRAM window placement (0x80->0xf20000,
 *              0xa0->0xf00000, 0xc0->0xf60000, 0xe0->0xf40000).  Used with the
 *              standard Cirrus GR09/GR0A bank registers; the linear-framebuffer
 *              clients do not need it, so it is not modelled.
 *    reg 0x02  linear-aperture base selector: the host window is placed at
 *              physical (data << 24).  Clients write 0xf0 to expose the
 *              linear framebuffer at 0xf0000000.
 *    reg 0x03  bit1 = display relay (1: accelerator drives the monitor,
 *              0: native 98 GDC); bit0 = register / host-access enable.
 *
 *  --- Relocated Cirrus VGA register block ---------------------------------
 *
 *  The gate array moves the chip's 0x3b0-0x3df VGA registers to (see
 *  pc98_wab_realize() / wab_map_reg()):
 *
 *    0x3c0-0x3cf -> 0x0ca0-0x0caf  (incl. the 0x3c3 "sleep" latch at 0x0ca3)
 *    0x3b4/0x3b5 -> 0x0ba4/0x0ba5  (mono CRTC)
 *    0x3ba       -> 0x0baa         (mono status)
 *    0x3d4/0x3d5 -> 0x0da4/0x0da5  (colour CRTC)
 *    0x3da       -> 0x0daa         (colour status / attribute flip-flop reset)
 *
 *  (IoVariant 2, used by some B-MATE variants, instead relocates to
 *  0x0c50/0x0d54/... and keys off 0xfa2/0xfa3 + 0x0902 + 0x0c53.  It is not
 *  implemented here.)
 *
 *  --- Linear framebuffer and CPU-source BitBLT ----------------------------
 *
 *  reg 0x02 aims the 1MB host aperture; real-mode clients also find a fixed
 *  window at 0x00f00000 (WAB_LFB_ADDR).  The same aperture is the source path
 *  for the Cirrus system-to-screen (MEMSYSSRC) BitBLT.
 *
 *  --- Wake / relay ports --------------------------------------------------
 *
 *    0xfac   secondary display-relay latch (PCI-era WAB-emulation models)
 *    0xff82  video-subsystem enable (POS102 bit0)
 *    0x0ca3  "sleep" latch bit0 (part of the NEC path-08h gate sequence)
 *
 *  The full NEC path-08h scanout-enable gate sequence (ports 0x68/0x6a/0x5f
 *  bracketing reg 0x03 and the 0x0ca3 sleep latch) stabilises real hardware,
 *  but the emulated chip has no external scanout arbitration, so those ports
 *  need no side effects here.
 * ============================================================================
 */

/***************************************
 *
 *  PC-98 Window Accelerator Board wiring
 *
 ***************************************/

OBJECT_DECLARE_SIMPLE_TYPE(Pc98WabState, PC98_WAB)

#define WAB_LFB_ADDR    0x00f00000     /* fixed real-mode linear window */
#define WAB_LFB_SIZE    0x00100000     /* 1 MiB */

#define WAB_NUM_REG_PORTS  6

struct Pc98WabState {
    ISADevice parent_obj;

    CirrusVGAState cirrus;

    /* private containers hiding the Cirrus's default 0x3b0/0xa0000 mappings */
    MemoryRegion priv_io;
    MemoryRegion priv_mem;

    MemoryRegion reg_alias[WAB_NUM_REG_PORTS];
    MemoryRegion lfb;             /* fixed window at 0xf00000 */
    MemoryRegion lfb_movable;     /* window relocated via control reg 0x02 */
    uint32_t lfb_movable_base;    /* 0 = not mapped */

    PortioList ctl_portio;
    uint8_t reg_index;            /* 0xfaa index latch */
    uint8_t reg_index_a2;         /* 0xfa2 index latch */
    uint8_t video_enable;         /* 0xff82 */
    uint8_t relay;                /* 0xfac / control reg 0x03 bit1 */
    uint8_t mmio_enable;          /* control reg 0x03 bit0 */
};

/*
 * Indexed control interface behind ports 0xfaa (index) / 0xfab (data), also
 * mirrored at 0xfa2 / 0xfa3.  See the interface block comment above for the
 * full register description; the subset the DOS clients actually drive is:
 *   0x00  machine/interface ID (read only) -- 0x5b identifies the path-08h WAB
 *   0x02  linear-framebuffer base: window is placed at (data << 24)
 *   0x03  bit1 = display relay, bit0 = register/host-access enable
 */
static void wab_ctl_write(Pc98WabState *s, uint8_t index, uint8_t dat)
{
    switch (index) {
    case 0x02:
        if (dat != 0x00 && dat != 0xff) {
            uint32_t base = (uint32_t)dat << 24;

            if (s->lfb_movable_base != base) {
                if (s->lfb_movable_base) {
                    memory_region_del_subregion(get_system_memory(),
                                                &s->lfb_movable);
                }
                s->lfb_movable_base = base;
                memory_region_add_subregion_overlap(get_system_memory(),
                                                    base, &s->lfb_movable, 2);
            }
        }
        break;
    case 0x03:
        s->relay = (dat & 0x02) ? 1 : 0;
        s->mmio_enable = dat & 0x01;
        break;
    default:
        break;
    }
}

static uint8_t wab_ctl_read(Pc98WabState *s, uint8_t index)
{
    switch (index) {
    case 0x00:
        return 0x5b;   /* NEC "path 08h" WAB/Core-Graph id (range 0x50-0x5d) */
    case 0x02:
        return s->lfb_movable_base >> 24;
    case 0x03:
        return (s->relay ? 0x02 : 0x00) | (s->mmio_enable ? 0x01 : 0x00);
    default:
        return 0xff;
    }
}

static void wab_faa_write(void *opaque, uint32_t addr, uint32_t val)
{
    ((Pc98WabState *)opaque)->reg_index = val;
}
static uint32_t wab_faa_read(void *opaque, uint32_t addr)
{
    return ((Pc98WabState *)opaque)->reg_index;
}
static void wab_fab_write(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98WabState *s = opaque;
    wab_ctl_write(s, s->reg_index, val);
}
static uint32_t wab_fab_read(void *opaque, uint32_t addr)
{
    Pc98WabState *s = opaque;
    return wab_ctl_read(s, s->reg_index);
}

static void wab_fa2_write(void *opaque, uint32_t addr, uint32_t val)
{
    ((Pc98WabState *)opaque)->reg_index_a2 = val;
}
static uint32_t wab_fa2_read(void *opaque, uint32_t addr)
{
    return ((Pc98WabState *)opaque)->reg_index_a2;
}
static void wab_fa3_write(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98WabState *s = opaque;
    wab_ctl_write(s, s->reg_index_a2, val);
}
static uint32_t wab_fa3_read(void *opaque, uint32_t addr)
{
    Pc98WabState *s = opaque;
    return wab_ctl_read(s, s->reg_index_a2);
}

/*
 * Display relay (port 0xfac): bit0/bit1 non-zero -> the accelerator output is
 * live on the monitor.  Only switches the display; the framebuffer windows
 * stay mapped.  Display compositing based on the relay is a follow-up (the
 * Cirrus currently renders to its own console).
 */
static void wab_fac_write(void *opaque, uint32_t addr, uint32_t val)
{
    ((Pc98WabState *)opaque)->relay = val & 0x03;
}
static uint32_t wab_fac_read(void *opaque, uint32_t addr)
{
    return 0xfc | (((Pc98WabState *)opaque)->relay & 0x03);
}

static void wab_ff82_write(void *opaque, uint32_t addr, uint32_t val)
{
    ((Pc98WabState *)opaque)->video_enable = val & 0x01;
}
static uint32_t wab_ff82_read(void *opaque, uint32_t addr)
{
    return ((Pc98WabState *)opaque)->video_enable;
}

static const MemoryRegionPortio wab_ctl_portio[] = {
    { 0x0fa2, 1, 1, .read = wab_fa2_read,  .write = wab_fa2_write },
    { 0x0fa3, 1, 1, .read = wab_fa3_read,  .write = wab_fa3_write },
    { 0x0faa, 1, 1, .read = wab_faa_read,  .write = wab_faa_write },
    { 0x0fab, 1, 1, .read = wab_fab_read,  .write = wab_fab_write },
    { 0x0fac, 1, 1, .read = wab_fac_read,  .write = wab_fac_write },
    { 0xff82, 1, 1, .read = wab_ff82_read, .write = wab_ff82_write },
    PORTIO_END_OF_LIST(),
};

static void wab_map_reg(Pc98WabState *s, int idx, uint16_t port, uint8_t off,
                        uint64_t size, const char *name)
{
    memory_region_init_alias(&s->reg_alias[idx], OBJECT(s), name,
                             &s->cirrus.cirrus_vga_io, off, size);
    memory_region_add_subregion(get_system_io(), port, &s->reg_alias[idx]);
}

static void pc98_wab_realize(DeviceState *dev, Error **errp)
{
    Pc98WabState *s = PC98_WAB(dev);
    VGACommonState *vga = &s->cirrus.vga;
    int idx = 0;

    vga->vram_size_mb = 4;
    s->cirrus.enable_blitter = true;
    if (!vga_common_init(vga, OBJECT(dev), errp)) {
        return;
    }

    memory_region_init(&s->priv_io, OBJECT(dev), "pc98-wab-priv-io", 0x10000);
    memory_region_init(&s->priv_mem, OBJECT(dev), "pc98-wab-priv-mem", 0x100000);

    wab_cirrus_init(&s->cirrus, OBJECT(dev), &s->priv_mem, &s->priv_io);

    /*
     * Remap the Cirrus VGA register block to the PC-98 WAB "IoVariant 1"
     * (0xCA0) scheme used by the NEC path-08h driver:
     *   0x3c0..0x3cf -> 0x0ca0..0x0caf (incl. the 0x0ca3 "sleep" register)
     *   0x3b4/0x3b5  -> 0x0ba4/0x0ba5
     *   0x3ba        -> 0x0baa
     *   0x3d4/0x3d5  -> 0x0da4/0x0da5
     *   0x3da        -> 0x0daa
     */
    wab_map_reg(s, idx++, 0x0ca0, 0x3c0 - 0x3b0, 16, "pc98-wab-reg-3c0");
    wab_map_reg(s, idx++, 0x0ba4, 0x3b4 - 0x3b0, 2,  "pc98-wab-reg-3b4");
    wab_map_reg(s, idx++, 0x0baa, 0x3ba - 0x3b0, 1,  "pc98-wab-reg-3ba");
    wab_map_reg(s, idx++, 0x0da4, 0x3d4 - 0x3b0, 2,  "pc98-wab-reg-3d4");
    wab_map_reg(s, idx++, 0x0daa, 0x3da - 0x3b0, 1,  "pc98-wab-reg-3da");

    /* fixed linear framebuffer window at 0xf00000 */
    memory_region_init_alias(&s->lfb, OBJECT(dev), "pc98-wab-lfb",
                             &s->cirrus.cirrus_linear_io, 0, WAB_LFB_SIZE);
    memory_region_add_subregion_overlap(get_system_memory(), WAB_LFB_ADDR,
                                        &s->lfb, 2);

    /* movable linear window, placed by control register 0x02 (data << 24) */
    memory_region_init_alias(&s->lfb_movable, OBJECT(dev), "pc98-wab-lfb-hi",
                             &s->cirrus.cirrus_linear_io, 0, WAB_LFB_SIZE);

    portio_list_init(&s->ctl_portio, OBJECT(dev), wab_ctl_portio, s,
                     "pc98-wab-ctl");
    portio_list_add(&s->ctl_portio, get_system_io(), 0);

    /* The Cirrus renders to its own console; shown when the relay is on. */
    vga->con = qemu_graphic_console_create(dev, 0, vga->hw_ops, vga);
}

static void pc98_wab_reset(DeviceState *dev)
{
    Pc98WabState *s = PC98_WAB(dev);

    s->reg_index = 0;
    s->reg_index_a2 = 0;
    s->video_enable = 0;
    s->relay = 0;
    s->mmio_enable = 0;
}

static void pc98_wab_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pc98_wab_realize;
    device_class_set_legacy_reset(dc, pc98_wab_reset);
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    /* TODO: migration (reuse the Cirrus vmstate once the layout is settled) */
}

static const TypeInfo pc98_wab_info = {
    .name          = TYPE_PC98_WAB,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(Pc98WabState),
    .class_init    = pc98_wab_class_init,
};

static void pc98_wab_register_types(void)
{
    type_register_static(&pc98_wab_info);
}

type_init(pc98_wab_register_types)

void pc98_wab_init(ISABus *bus)
{
    ISADevice *isadev = isa_new(TYPE_PC98_WAB);

    /* give it an id so its console can be targeted (e.g. "screendump f wab") */
    DEVICE(isadev)->id = g_strdup("wab");
    isa_realize_and_unref(isadev, bus, &error_fatal);
}
