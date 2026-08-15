/*
 * NEC PC-9821 Core-Graph bridge with an internal Cirrus GD5440
 *
 * Copyright (c) 2026 Awe Morris
 *
 * Core-Graph is the PCI-visible function (1033:0009).  The Cirrus chip sits
 * on the bridge's private bus: it has no PCI configuration function or BAR.
 * NEC exposes it through the fixed 0xFAA/0xFAB control interface, relocated
 * VGA ports, and a host aperture selected by control register 2.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/display/pc98-coregraph.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_ids.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "ui/console.h"
#include "cirrus_vga_internal.h"
#include "qom/object.h"
#include "trace.h"
#include "vga_regs.h"

#define COREGRAPH_ID              0x5b
#define COREGRAPH_LFB_SIZE        (1 * MiB)
#define COREGRAPH_LEGACY_SIZE     0x8000
#define COREGRAPH_MMIO_OFFSET     ((4 * MiB) - 256)
#define COREGRAPH_MMIO_INTERNAL   (COREGRAPH_LFB_SIZE - 256)

#define COREGRAPH_BLT_RESET       0x04
#define COREGRAPH_BLT_START       0x02
#define COREGRAPH_BLT_SOLID_BRUSH 0xc0

typedef struct Pc98CoreGraphState {
    PCIDevice parent_obj;

    CirrusVGAState cirrus;

    MemoryRegion control_io;
    MemoryRegion video_enable_io;
    MemoryRegion io_ca0;
    MemoryRegion io_ba4;
    MemoryRegion io_baa;
    MemoryRegion io_da4;
    MemoryRegion io_daa;
    MemoryRegion linear_alias;
    MemoryRegion mmio_alias;
    MemoryRegion legacy_alias;
    MemoryRegion isa_alias;
    MemoryRegion isa_cursor;

    uint8_t index;
    uint8_t regs[5];
    uint8_t video_enable;
    bool io_enabled;
    bool linear_mapped;
    bool mmio_mapped;
    bool legacy_mapped;
    bool isa_mapped;
    hwaddr cur_linear_base;
    hwaddr cur_legacy_base;
    hwaddr isa_base;
    uint64_t isa_size;
    Pc98VgaState *primary_vga;
    bool display_active;
} Pc98CoreGraphState;

OBJECT_DECLARE_SIMPLE_TYPE(Pc98CoreGraphState, PC98_COREGRAPH)

static void coregraph_update_display(Pc98CoreGraphState *s);

static hwaddr coregraph_legacy_base(uint8_t value)
{
    switch (value) {
    case 0x10:
        return 0x000b0000;
    case 0xa0:
        return 0x00f00000;
    case 0x80:
        return 0x00f20000;
    case 0xc0:
        return 0x00f40000;
    case 0xe0:
        return 0x00f60000;
    default:
        return 0;
    }
}

static void coregraph_apply_mappings(Pc98CoreGraphState *s)
{
    /*
     * Control register 3 does NOT gate the host memory apertures.
     * Its bits are the BitBLT MMIO enable (bit 0) and the display
     * relay (bit 1).  The linear window appears as soon as register 2
     * holds a valid page: the NEC display driver writes reg 2 and
     * immediately probes VRAM through that window; if nothing decodes
     * there it concludes the accelerator is broken and falls back to
     * the 16-colour GDC driver without ever touching reg 3 or 0xFF82.
     *
     * Before a linear page is selected, activity on reg 3 or the 0xFF82
     * video-enable exposes the banked probe window.  Core-Graph path 08h
     * switches to the reg02 linear aperture; keeping the classic-WAB window
     * mapped at the same time would shadow guest RAM in the 15-16 MiB range.
     */
    MemoryRegion *system_memory = get_system_memory();
    bool linear_valid = s->regs[2] != 0x00 && s->regs[2] != 0xff;
    bool legacy_enabled = !linear_valid && ((s->regs[3] & 0x03) != 0 || s->video_enable);
    hwaddr legacy_base = coregraph_legacy_base(s->regs[1]);
    hwaddr linear_base = (hwaddr)s->regs[2] << 24;
    CirrusVGAState *c = &s->cirrus;
    uint8_t isa_page;
    hwaddr isa_base = 0;
    uint64_t isa_size = 0;

    /*
     * Core-Graph exposes the relocated colour CRTC block at 0xDA4/0xDA5.
     * The reusable VGA core resets MISC output to monochrome addressing,
     * which would reject those accesses and make CR27 read as 0xff before
     * the guest has had a chance to program MISC itself.
     */
    c->vga.msr |= VGA_MIS_COLOR;

    /*
     * GD5430 ISA addressing: a non-zero SR07[7:4] makes the chip decode its
     * VRAM aperture at that megabyte of the 24-bit bus (address bits 23-20);
     * GR0B bit 5 widens the aperture to 2 MiB by ignoring address bit 20.
     * The NEC Windows 95 mini-VDD (vacl.vxd) relies on this on Xe10-class
     * boards: its mode tables program SR07 = 0xA1/0xA3/0xA5, placing the
     * framebuffer at 0x00A00000 - the PC-9821 window-accelerator region.
     */
    isa_page = c->vga.sr[0x07] & 0xf0 & ~((c->vga.gr[0x0b] >> 1) & 0x10);
    /*
     * Only the PC-9821 window-accelerator page is wired through Core-Graph.
     * Generic Cirrus also uses SR07 values such as 11h merely to enable
     * extended modes.  Treating every non-zero high nibble as an ISA decode
     * maps VRAM over ordinary RAM at 1 MiB; a DOS extender then loses the
     * code it is executing as soon as it writes SR07=11h.
     */
    if (isa_page == 0xa0) {
        isa_base = (hwaddr)isa_page << 16;
        isa_size = (c->vga.gr[0x0b] & 0x20) ? 2 * MiB : 1 * MiB;
    } else {
        isa_page = 0;
    }

    /*
     * Called from the relocated VGA port wrapper on every register write;
     * skip the memory transaction when nothing changes.
     */
    if (s->io_enabled &&
        s->linear_mapped == linear_valid &&
        (!linear_valid || s->cur_linear_base == linear_base) &&
        s->mmio_mapped == linear_valid &&
        s->legacy_mapped == (legacy_enabled && legacy_base != 0) &&
        (!s->legacy_mapped || s->cur_legacy_base == legacy_base) &&
        s->isa_mapped == (isa_page != 0) &&
        (!s->isa_mapped ||
         (s->isa_base == isa_base && s->isa_size == isa_size))) {
        return;
    }

    memory_region_transaction_begin();

    /*
     * The relocated register block is always decoded.  In particular,
     * ACLMM.VXD calibrates itself from the vertical-retrace bit at 0xDAA
     * before it sets control register 3.  Hiding the I/O block until reg03
     * bit 0 was set made that calibration execute a 2^32-iteration LOOPNE
     * for every sample.  The access bit gates the host memory apertures, not
     * the register decode needed to turn the accelerator on.
     */
    memory_region_set_enabled(&s->io_ca0, true);
    memory_region_set_enabled(&s->io_ba4, true);
    memory_region_set_enabled(&s->io_baa, true);
    memory_region_set_enabled(&s->io_da4, true);
    memory_region_set_enabled(&s->io_daa, true);
    s->io_enabled = true;

    if (s->linear_mapped) {
        memory_region_del_subregion(system_memory, &s->linear_alias);
        s->linear_mapped = false;
    }
    if (linear_valid) {
        memory_region_add_subregion_overlap(system_memory, linear_base,
                                            &s->linear_alias, 2);
        s->linear_mapped = true;
        s->cur_linear_base = linear_base;
    }

    /*
     * NEC's display driver maps the BitBLT register page separately at
     * linear_base + 4 MiB - 256.  The reusable GD5440 core recognizes MMIO
     * at the final 256 bytes of its 1 MiB VRAM aperture, so expose that
     * internal page at the board-level address without widening the visible
     * framebuffer window.
     */
    if (s->mmio_mapped) {
        memory_region_del_subregion(system_memory, &s->mmio_alias);
        s->mmio_mapped = false;
    }
    if (linear_valid) {
        memory_region_add_subregion_overlap(system_memory,
                                            linear_base +
                                            COREGRAPH_MMIO_OFFSET,
                                            &s->mmio_alias, 2);
        s->mmio_mapped = true;
    }

    if (s->legacy_mapped) {
        memory_region_del_subregion(system_memory, &s->legacy_alias);
        s->legacy_mapped = false;
    }
    if (legacy_enabled && legacy_base) {
        memory_region_add_subregion_overlap(system_memory, legacy_base,
                                            &s->legacy_alias, 2);
        s->legacy_mapped = true;
        s->cur_legacy_base = legacy_base;
    }

    if (s->isa_mapped) {
        memory_region_del_subregion(system_memory, &s->isa_cursor);
        memory_region_del_subregion(system_memory, &s->isa_alias);
        s->isa_mapped = false;
    }
    if (isa_page) {
        memory_region_set_size(&s->isa_alias, isa_size);
        memory_region_add_subregion_overlap(system_memory, isa_base,
                                            &s->isa_alias, 2);
        memory_region_add_subregion_overlap(system_memory,
                                            isa_base +
                                            COREGRAPH_MMIO_INTERNAL,
                                            &s->isa_cursor, 3);
        s->isa_mapped = true;
        if (s->isa_base != isa_base || s->isa_size != isa_size) {
            trace_pc98_coregraph_isa_window(isa_base, isa_size);
        }
    }
    s->isa_base = isa_base;
    s->isa_size = isa_size;

    memory_region_transaction_commit();
}

static uint64_t coregraph_control_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    Pc98CoreGraphState *s = opaque;

    if ((addr & 1) == 0) {
        trace_pc98_coregraph_control_read(s->index, s->index);
        return s->index;
    }
    if (s->index == 0) {
        trace_pc98_coregraph_control_read(s->index, COREGRAPH_ID);
        return COREGRAPH_ID;
    }
    if (s->index == 4) {
        /*
         * Register 4 is read-only: it reports the banked-window index that
         * corresponds to the window base selected through register 1.
         * The NEC mini-VDD reads it back to confirm the window it just
         * programmed.
         */
        uint8_t idx;
        switch (s->regs[1]) {
        case 0xa0:      /* 0xf00000 */
            idx = 0x00;
            break;
        case 0x80:      /* 0xf20000 */
            idx = 0x01;
            break;
        case 0xc0:      /* 0xf40000 */
            idx = 0x02;
            break;
        case 0xe0:      /* 0xf60000 */
            idx = 0x03;
            break;
        default:
            idx = 0x00;
            break;
        }
        trace_pc98_coregraph_control_read(s->index, idx);
        return idx;
    }
    if (s->index < ARRAY_SIZE(s->regs)) {
        trace_pc98_coregraph_control_read(s->index, s->regs[s->index]);
        return s->regs[s->index];
    }
    trace_pc98_coregraph_control_read(s->index, 0xff);
    return 0xff;
}

static void coregraph_control_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    Pc98CoreGraphState *s = opaque;

    if ((addr & 1) == 0) {
        s->index = value;
        trace_pc98_coregraph_index_write(s->index);
        return;
    }
    if (s->index > 0 && s->index < ARRAY_SIZE(s->regs)) {
        /*
         * Register 2 (linear window page): 0x00 and 0xFF are probe values,
         * not valid pages.
         */
        if (s->index == 2 && (value == 0x00 || value == 0xff)) {
            trace_pc98_coregraph_control_write(s->index, value);
            return;
        }
        s->regs[s->index] = value;
        trace_pc98_coregraph_control_write(s->index, value);
        coregraph_apply_mappings(s);
        coregraph_update_display(s);
    }
}

static const MemoryRegionOps coregraph_control_ops = {
    .read = coregraph_control_read,
    .write = coregraph_control_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * 0xFF82 video-subsystem enable.  The NEC Core-Graph display driver
 * (acl3u8.drv / vacl.vxd) writes bit 0 here to switch the monitor from the
 * native 98 GDC output to the accelerator, and reads it back to confirm.
 * Without this latch the driver cannot enable the Cirrus and hangs while
 * setting a packed-pixel (>= 8 bpp) mode.
 */
static uint64_t coregraph_video_enable_read(void *opaque, hwaddr addr,
                                            unsigned size)
{
    Pc98CoreGraphState *s = opaque;

    return s->video_enable;
}

static void coregraph_video_enable_write(void *opaque, hwaddr addr,
                                         uint64_t value, unsigned size)
{
    Pc98CoreGraphState *s = opaque;

    s->video_enable = value & 0x01;
    trace_pc98_coregraph_video_enable(s->video_enable);
    coregraph_apply_mappings(s);
    coregraph_update_display(s);
}

/*
 * NEC's PC-98 display drivers use an all-zero 8x8 monochrome pattern
 * as a solid foreground brush.  Standard Alpine semantics select the
 * background colour for zero bits, but PC98 Core-Graph pattern as all
 * ones.  Substitute the pattern only while the synchronous
 * video-to-video BLT executes, leaving guest VRAM and the reusable
 * Cirrus implementation unchanged.
 */
static bool coregraph_begin_solid_brush(Pc98CoreGraphState *s,
                                        uint32_t *pattern_addr,
                                        uint8_t saved[8])
{
    CirrusVGAState *c = &s->cirrus;
    uint32_t addr;
    int i;

    if (c->vga.gr[0x30] != COREGRAPH_BLT_SOLID_BRUSH) {
        return false;
    }

    addr = c->vga.gr[0x2c] |
           (c->vga.gr[0x2d] << 8) |
           (c->vga.gr[0x2e] << 16);
    addr &= c->cirrus_addr_mask;

    /*
     * The reusable core aligns an 8-bpp video-memory pattern to 64 bytes
     * before reading its eight monochrome rows.
     */
    addr &= ~63U;
    if (addr + 8 > c->real_vram_size) {
        return false;
    }

    for (i = 0; i < 8; i++) {
        if (c->vga.vram_ptr[addr + i] != 0x00) {
            return false;
        }
    }

    memcpy(saved, c->vga.vram_ptr + addr, 8);
    memset(c->vga.vram_ptr + addr, 0xff, 8);
    *pattern_addr = addr;
    trace_pc98_coregraph_solid_brush(addr, c->cirrus_shadow_gr1);
    return true;
}

static void coregraph_end_solid_brush(Pc98CoreGraphState *s,
                                      uint32_t pattern_addr,
                                      const uint8_t saved[8],
                                      bool active)
{
    if (active) {
        memcpy(s->cirrus.vga.vram_ptr + pattern_addr, saved, 8);
    }
}

/*
 * A start value can also clear BLT reset in the same GR31 write.  The
 * reusable core handles reset and start as mutually exclusive edges, while
 * NEC's Windows 95 driver relies on both taking effect.  Split that combined
 * transition into a reset-clear write followed by the requested start.
 */
static void coregraph_blt_status_write(Pc98CoreGraphState *s,
                                       MemoryRegion *target,
                                       hwaddr target_addr,
                                       uint8_t value)
{
    CirrusVGAState *c = &s->cirrus;
    uint8_t old = c->vga.gr[0x31];
    uint8_t modeext = c->vga.gr[0x33];
    uint8_t saved[8];
    uint32_t pattern_addr = 0;
    bool brush;

    if ((old & COREGRAPH_BLT_RESET) &&
        !(value & COREGRAPH_BLT_RESET) &&
        (value & COREGRAPH_BLT_START)) {
        memory_region_dispatch_write(target, target_addr,
                                     value & ~COREGRAPH_BLT_START,
                                     MO_8, MEMTXATTRS_UNSPECIFIED);
        trace_pc98_coregraph_blt_reset_start(old, value);
    }

    brush = (value & COREGRAPH_BLT_START) &&
            coregraph_begin_solid_brush(s, &pattern_addr, saved);
    /*
     * GR33 is a GD5446 extension.  The GD5440 stores the register value for
     * readback but does not apply it to BitBLT; NEC's Windows 95 driver uses
     * 0xff as its "no extension" value.  Keep the guest-visible register,
     * while presenting the GD5440 execution semantics to the reusable core.
     */
    if (value & COREGRAPH_BLT_START) {
        c->vga.gr[0x33] = 0;
    }
    memory_region_dispatch_write(target, target_addr, value, MO_8,
                                 MEMTXATTRS_UNSPECIFIED);
    c->vga.gr[0x33] = modeext;
    coregraph_end_solid_brush(s, pattern_addr, saved, brush);
}

/*
 * Relocated VGA registers 0xCA0-0xCAF (VGA 0x3C0-0x3CF).  A plain alias
 * would do for the data path, but SR07 (0xCA4/0xCA5) and GR0B (0xCAE/0xCAF)
 * writes move the ISA VRAM aperture, so forward through a wrapper and
 * re-evaluate the host mappings after every write.
 */
static uint64_t coregraph_vga_io_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    Pc98CoreGraphState *s = opaque;
    uint64_t value = 0xff;

    memory_region_dispatch_read(&s->cirrus.cirrus_vga_io, addr + 0x10,
                                &value, size_memop(size) | MO_LE,
                                MEMTXATTRS_UNSPECIFIED);
    return value;
}

static void coregraph_vga_io_write(void *opaque, hwaddr addr,
                                   uint64_t value, unsigned size)
{
    Pc98CoreGraphState *s = opaque;

    if (size == 1 && addr == 0x0f &&
        s->cirrus.vga.gr_index == 0x31) {
        coregraph_blt_status_write(s, &s->cirrus.cirrus_vga_io,
                                   addr + 0x10, value);
    } else {
        memory_region_dispatch_write(&s->cirrus.cirrus_vga_io, addr + 0x10,
                                     value, size_memop(size) | MO_LE,
                                     MEMTXATTRS_UNSPECIFIED);
    }
    coregraph_apply_mappings(s);
}

static const MemoryRegionOps coregraph_vga_io_ops = {
    .read = coregraph_vga_io_read,
    .write = coregraph_vga_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * NEC's miniport uses word I/O to write an index/data pair in one
     * VideoPortWritePortUshort() call.  The underlying Cirrus region is
     * byte-wide, so ask the memory core to split wider accesses instead of
     * rejecting them as invalid.
     */
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * Core-Graph keeps the top 256 bytes of its one-MiB framebuffer as VRAM for
 * the hardware cursor.  BitBLT MMIO is a separate board-level window at
 * linear_base + 4 MiB - 256.  The generic Cirrus linear region overlays MMIO
 * on its own final 256 bytes, so it cannot be exposed directly as the board
 * LFB.  Preserve the other important Core-Graph behavior: while a
 * system-source BLT is active, writes anywhere in this aperture feed the
 * Cirrus source FIFO instead of VRAM.
 */
static uint64_t coregraph_linear_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    Pc98CoreGraphState *s = opaque;
    CirrusVGAState *c = &s->cirrus;
    uint8_t sr17 = c->vga.sr[0x17];
    uint64_t value = 0xff;

    /*
     * Preserve the reusable core's GR0B address transforms and extended
     * write modes, but suppress its final-256-byte MMIO overlay.  Core-Graph
     * exposes those registers only in its separate 4 MiB - 256 window.
     */
    addr &= c->cirrus_addr_mask;
    c->vga.sr[0x17] &= ~0x04;
    memory_region_dispatch_read(&c->cirrus_linear_io, addr, &value,
                                MO_8, MEMTXATTRS_UNSPECIFIED);
    c->vga.sr[0x17] = sr17;
    return value;
}

static void coregraph_linear_write(void *opaque, hwaddr addr,
                                   uint64_t value, unsigned size)
{
    Pc98CoreGraphState *s = opaque;
    CirrusVGAState *c = &s->cirrus;
    uint8_t sr17 = c->vga.sr[0x17];

    addr &= c->cirrus_addr_mask;
    c->vga.sr[0x17] &= ~0x04;
    memory_region_dispatch_write(&c->cirrus_linear_io, addr, value,
                                 MO_8, MEMTXATTRS_UNSPECIFIED);
    c->vga.sr[0x17] = sr17;
}

static const MemoryRegionOps coregraph_linear_ops = {
    .read = coregraph_linear_read,
    .write = coregraph_linear_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static uint64_t coregraph_cursor_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    Pc98CoreGraphState *s = opaque;

    return s->cirrus.vga.vram_ptr[COREGRAPH_MMIO_INTERNAL + addr];
}

static void coregraph_cursor_write(void *opaque, hwaddr addr,
                                   uint64_t value, unsigned size)
{
    Pc98CoreGraphState *s = opaque;
    uint32_t vram_addr = COREGRAPH_MMIO_INTERNAL + addr;

    s->cirrus.vga.vram_ptr[vram_addr] = value;
    memory_region_set_dirty(&s->cirrus.vga.vram, vram_addr, 1);
}

static const MemoryRegionOps coregraph_cursor_ops = {
    .read = coregraph_cursor_read,
    .write = coregraph_cursor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static uint64_t coregraph_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    Pc98CoreGraphState *s = opaque;
    uint64_t value = 0xff;

    memory_region_dispatch_read(&s->cirrus.cirrus_linear_io,
                                COREGRAPH_MMIO_INTERNAL + addr,
                                &value, size_memop(size) | MO_LE,
                                MEMTXATTRS_UNSPECIFIED);
    return value;
}

static void coregraph_mmio_write(void *opaque, hwaddr addr,
                                 uint64_t value, unsigned size)
{
    Pc98CoreGraphState *s = opaque;

    if (size == 1 && addr == 0x40) {
        coregraph_blt_status_write(s, &s->cirrus.cirrus_linear_io,
                                   COREGRAPH_MMIO_INTERNAL + addr, value);
    } else {
        memory_region_dispatch_write(&s->cirrus.cirrus_linear_io,
                                     COREGRAPH_MMIO_INTERNAL + addr,
                                     value, size_memop(size) | MO_LE,
                                     MEMTXATTRS_UNSPECIFIED);
    }
}

static const MemoryRegionOps coregraph_mmio_ops = {
    .read = coregraph_mmio_read,
    .write = coregraph_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static const MemoryRegionOps coregraph_video_enable_ops = {
    .read = coregraph_video_enable_read,
    .write = coregraph_video_enable_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void coregraph_reset(DeviceState *dev)
{
    Pc98CoreGraphState *s = PC98_COREGRAPH(dev);

    s->index = 0;
    s->video_enable = 0;
    memset(s->regs, 0, sizeof(s->regs));

    /*
     * The motherboard firmware leaves a valid banked window selected.
     * ACLMM reads the value while classifying the fixed-interface path.
     * 0xe0 selects the 0xf60000 window used as the generic built-in GD54xx
     * reset value.
     */
    s->regs[1] = 0xe0;
    coregraph_apply_mappings(s);
    coregraph_update_display(s);
}

static int coregraph_post_load(void *opaque, int version_id)
{
    Pc98CoreGraphState *s = opaque;

    coregraph_apply_mappings(s);
    coregraph_update_display(s);
    return 0;
}

static int coregraph_pre_save(void *opaque)
{
    Pc98CoreGraphState *s = opaque;

    /* The shared Cirrus VMState likewise snapshots only between BLTs. */
    return s->cirrus.cirrus_srccounter ? -EBUSY : 0;
}

static const VMStateDescription vmstate_pc98_coregraph = {
    .name = "pc98-coregraph",
    .version_id = 3,
    .minimum_version_id = 1,
    .pre_save = coregraph_pre_save,
    .post_load = coregraph_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, Pc98CoreGraphState),
        VMSTATE_STRUCT(cirrus, Pc98CoreGraphState, 0,
                       vmstate_cirrus_vga, CirrusVGAState),
        VMSTATE_UINT8(index, Pc98CoreGraphState),
        VMSTATE_UINT8_V(video_enable, Pc98CoreGraphState, 2),
        VMSTATE_UINT8_ARRAY(regs, Pc98CoreGraphState, 5),
        VMSTATE_UINT8_ARRAY_V(cirrus.cirrus_hidden_palette,
                              Pc98CoreGraphState, 48, 3),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * The Cirrus RAM block is private to Core-Graph and is reached through an
 * I/O aperture rather than a direct RAM alias.  Force a full scanout update:
 * dirty-log listeners otherwise do not see every write to that private RAM
 * block when it is not directly present in the system address space.
 */
static bool coregraph_gfx_update(void *opaque)
{
    Pc98CoreGraphState *s = opaque;
    VGACommonState *vga = &s->cirrus.vga;
    uint8_t gr5;
    bool result;

    /*
     * A Windows full-screen DOS box keeps the Core-Graph function enabled
     * while re-enabling the native GDC scanout.  Windows leaves several GDC
     * registers, including the cursor enable, programmed behind its normal
     * Cirrus desktop; MODE1_DISP is the signal that distinguishes live GDC
     * output from that preserved state.  Follow the active scanout rather
     * than transient Cirrus blanking during a mode set.
     */
    if (s->primary_vga &&
        (!s->display_active ||
         pc98_vga_display_enabled(s->primary_vga))) {
        return pc98_vga_update_console(s->primary_vga);
    }

    /*
     * Switching to the native GDC leaves its 640-pixel-wide surface attached
     * to the shared console.  If Cirrus is blank when the relay switches
     * back, generic vga_draw_blank() uses the saved Cirrus width before its
     * normal mode path has a chance to resize.  Resize here, on the UI refresh
     * thread, so the first blanking pass cannot overrun each scanline.
     */
    if (vga->last_scr_width > 0 && vga->last_scr_height > 0 &&
        (qemu_console_get_width(vga->con, -1) != vga->last_scr_width ||
         qemu_console_get_height(vga->con, -1) != vga->last_scr_height)) {
        qemu_console_resize(vga->con, vga->last_scr_width,
                            vga->last_scr_height);
    }
    /*
     * NEC's Windows 95 Core-Graph driver leaves GR05's VGA shift field
     * cleared after selecting an SR07 packed-pixel mode.  The board still
     * scans the aperture linearly; QEMU's generic VGA renderer otherwise
     * interprets it as planar.  Apply the board-side scanout semantics
     * without changing the guest-visible Cirrus register.
     */
    gr5 = vga->gr[VGA_GFX_MODE];
    if ((s->regs[3] & 0x02) && (vga->sr[0x07] & 0x01)) {
        vga->gr[VGA_GFX_MODE] = (gr5 & ~0x60) | 0x40;
    }
    vga->hw_ops->invalidate(vga);
    result = vga->hw_ops->gfx_update(vga);
    vga->gr[VGA_GFX_MODE] = gr5;
    return result;
}

static void coregraph_invalidate(void *opaque)
{
    Pc98CoreGraphState *s = opaque;
    VGACommonState *vga = &s->cirrus.vga;

    vga->hw_ops->invalidate(vga);
    if (s->primary_vga) {
        pc98_vga_invalidate_console(s->primary_vga);
    }
}

static const GraphicHwOps coregraph_hw_ops = {
    .invalidate = coregraph_invalidate,
    .gfx_update = coregraph_gfx_update,
};

static void coregraph_update_display(Pc98CoreGraphState *s)
{
    bool active;

    if (!s->primary_vga) {
        return;
    }

    /*
     * Real PC-9821 systems feed the GDC and Core-Graph outputs through one
     * monitor relay.  ACLMM controls it with fixed-interface register 3 bit
     * 1, while older WAB-compatible drivers use the video-enable latch at
     * 0xFF82.  Keep one stable QEMU console callback and let it dispatch to
     * the selected producer.  Replacing the callback during a Windows mode
     * set can strand an in-flight SDL refresh on the old producer.
     */
    active = (s->regs[3] & 0x02) || s->video_enable;
    if (active == s->display_active) {
        return;
    }

    s->display_active = active;
    qemu_console_hw_invalidate(s->cirrus.vga.con);
}

static void coregraph_init_io_alias(MemoryRegion *alias, Object *owner,
                                    const char *name, MemoryRegion *source,
                                    hwaddr offset, hwaddr size,
                                    hwaddr target)
{
    memory_region_init_alias(alias, owner, name, source, offset, size);
    memory_region_set_enabled(alias, false);
    memory_region_add_subregion(get_system_io(), target, alias);
}

static void coregraph_realize(PCIDevice *dev, Error **errp)
{
    Pc98CoreGraphState *s = PC98_COREGRAPH(dev);
    CirrusVGAState *c = &s->cirrus;
    Object *owner = OBJECT(dev);

    c->vga.vram_size_mb = 1;
    c->enable_blitter = true;
    if (!vga_common_init(&c->vga, owner, errp)) {
        return;
    }

    /*
     * CR27=A0 identifies the GD5440 on verified Core-Graph machines.  QEMU's
     * GD5430 core uses that same chip ID and supplies the required Alpine
     * register set and BitBLT engine.
     */
    cirrus_init_common(c, owner, CIRRUS_ID_CLGD5430, 0,
                       get_system_memory(), get_system_io());

    /*
     * The GD5440 core supports more memory, but the verified Core-Graph
     * board and NEC miniport expose 1 MiB.  Besides aperture wrapping, this
     * determines the hardware-cursor storage block at VRAM + 0xFC000.
     */
    c->real_vram_size = COREGRAPH_LFB_SIZE;
    c->cirrus_addr_mask = COREGRAPH_LFB_SIZE - 1;
    c->linear_mmio_mask = COREGRAPH_LFB_SIZE - 256;

    /*
     * Undo the PC/AT-facing mappings made by the reusable core.  Core-Graph
     * exposes neither standard VGA I/O nor A0000 VGA memory directly.
     */
    memory_region_del_subregion(get_system_io(), &c->cirrus_vga_io);
    memory_region_del_subregion(get_system_memory(), &c->low_mem_container);

    /*
     * The linear window exposes the board's 1 MiB of Cirrus VRAM.  SR17 turns
     * the final 256 bytes into the BitBLT MMIO aperture; that is the Cirrus
     * core default (real_vram_size - 256), so leave linear_mmio_mask alone.
     */

    memory_region_init_io(&s->io_ca0, owner, &coregraph_vga_io_ops, s,
                          "coregraph-io-ca0", 0x10);
    s->io_ca0.disable_reentrancy_guard = true;
    memory_region_set_enabled(&s->io_ca0, false);
    memory_region_add_subregion(get_system_io(), 0x0ca0, &s->io_ca0);
    coregraph_init_io_alias(&s->io_ba4, owner, "coregraph-io-ba4",
                            &c->cirrus_vga_io, 0x04, 2, 0x0ba4);
    coregraph_init_io_alias(&s->io_baa, owner, "coregraph-io-baa",
                            &c->cirrus_vga_io, 0x0a, 1, 0x0baa);
    coregraph_init_io_alias(&s->io_da4, owner, "coregraph-io-da4",
                            &c->cirrus_vga_io, 0x24, 2, 0x0da4);
    coregraph_init_io_alias(&s->io_daa, owner, "coregraph-io-daa",
                            &c->cirrus_vga_io, 0x2a, 1, 0x0daa);

    memory_region_init_io(&s->control_io, owner, &coregraph_control_ops, s,
                          "coregraph-control", 2);
    memory_region_add_subregion(get_system_io(), 0x0faa, &s->control_io);
    memory_region_init_io(&s->video_enable_io, owner,
                          &coregraph_video_enable_ops, s,
                          "coregraph-video-enable", 1);
    memory_region_add_subregion(get_system_io(), 0xff82,
                                &s->video_enable_io);

    memory_region_init_io(&s->linear_alias, owner, &coregraph_linear_ops, s,
                          "coregraph-linear", COREGRAPH_LFB_SIZE);
    s->linear_alias.disable_reentrancy_guard = true;
    memory_region_init_io(&s->mmio_alias, owner, &coregraph_mmio_ops, s,
                          "coregraph-mmio", 256);
    s->mmio_alias.disable_reentrancy_guard = true;
    memory_region_init_alias(&s->legacy_alias, owner, "coregraph-legacy",
                             &c->low_mem_container, 0,
                             COREGRAPH_LEGACY_SIZE);
    memory_region_init_alias(&s->isa_alias, owner, "coregraph-isa-window",
                             &c->cirrus_linear_io, 0, 2 * MiB);
    /*
     * Keep the aperture itself as a flat alias, but override its hardware
     * cursor storage.  Otherwise generic Cirrus interprets these bytes as
     * its in-aperture MMIO register page.
     */
    memory_region_init_io(&s->isa_cursor, owner, &coregraph_cursor_ops, s,
                          "coregraph-isa-cursor", 256);

    if (s->primary_vga) {
        c->vga.con = pc98_vga_get_console(s->primary_vga);
        object_property_set_link(OBJECT(c->vga.con), "device", OBJECT(dev),
                                 &error_abort);
        qemu_graphic_console_set_hwops(c->vga.con, &coregraph_hw_ops, s);
    } else {
        c->vga.con = qemu_graphic_console_create(DEVICE(dev), 0,
                                                  &coregraph_hw_ops, s);
    }
    coregraph_reset(DEVICE(dev));
}

void pc98_coregraph_set_primary_vga(PCIDevice *dev, Pc98VgaState *vga)
{
    Pc98CoreGraphState *s = PC98_COREGRAPH(dev);

    assert(!DEVICE(dev)->realized);
    s->primary_vga = vga;
}

static void coregraph_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    pc->realize = coregraph_realize;
    pc->vendor_id = PCI_VENDOR_ID_NEC;
    pc->device_id = 0x0009;
    pc->revision = 0x01;
    pc->class_id = PCI_CLASS_DISPLAY_OTHER;

    dc->desc = "NEC Core-Graph bridge with internal Cirrus GD5440";
    dc->vmsd = &vmstate_pc98_coregraph;
    device_class_set_legacy_reset(dc, coregraph_reset);
    dc->user_creatable = false;
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo coregraph_info = {
    .name = TYPE_PC98_COREGRAPH,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(Pc98CoreGraphState),
    .class_init = coregraph_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void coregraph_register_types(void)
{
    type_register_static(&coregraph_info);
}

type_init(coregraph_register_types)
