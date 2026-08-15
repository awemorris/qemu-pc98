/*
 * QEMU NEC PC-9821 memory controller
 *
 * ROM bank switching (ITF/BIOS/IDE/PCI), movable RAM windows at
 * 0x80000/0xa0000, the 16MB system space and top-of-4G mirrors.
 *
 * PC-98 support
 *   Copyright (c) 2009 TAKEDA, toshiya
 *
 * PC-98 support modernisation
 *   Copyright (c) 2026 Awe Morris
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This device is derived from the PC-98 model in the qemu/9821 fork
 * (GPL, by TAKEDA toshiya) and has been reimplemented and
 * restructured for modern QEMU.  Its register-level behaviour was
 * cross-checked against the Neko Project II and NP21W emulators.
 *
 * Memory topology:
 *
 *   system_memory
 *   +-- pc98.lowmem (container, 1 MiB) @ 0x00000
 *   |   +-- pc98.ram-base            alias ram[0..0x80000)        prio 0
 *   |   +-- pc98.win1-ram @ 0x80000  alias ram[map1*64K..+128K)   prio 0
 *   |   +-- pc98.win1-{tvram,a8000,b0000,e0000} overlays          prio 1
 *   |   +-- pc98.win2-ram @ 0xa0000  alias ram[map2*64K..+128K)   prio 0
 *   |   +-- pc98.win2-{tvram,a8000,b0000,e0000} overlays          prio 1
 *   |   +-- pc98.cbus-rom @ 0xc0000  0xff-filled ROM              prio 0
 *   |   +-- pc98.d8000-rom @ 0xd8000 0xff-filled ROM (TODO IDE)   prio 0
 *   |   +-- pc98.bios @ 0xe8000      alias rom[BIOS 96K]          prio 0
 *   |   +-- pc98.e8000-ram           alias ram[0xe8000..+64K)     prio 1
 *   |   +-- pc98.f8000-rom @ 0xf8000 alias rom[bank*32K]          prio 2
 *   |   +-- pc98.f8000-ram           alias ram[..]                prio 3
 *   +-- pc98.ram-mid @ 0x100000      alias ram[1M..15M)
 *   +-- pc98.ram-f00000 @ 0xf00000   alias ram[15M..16M)  (16MB space off)
 *   +-- pc98.pegc-post @ 0xf00000     Xa7 POST backing      (16MB space on)
 *   +-- pc98.pegc-high @ 0xfff00000   full PEGC high alias  (PEGC enabled)
 *   +-- pc98.sys16m-mirror @ 0xfa0000 alias lowmem[0xa0000..1M)
 *   |                                 (16MB space on)
 *   +-- pc98.ram-high @ 0x1000000    alias ram[16M..) (if ram > 16M)
 *   +-- pc98.top-mirror @ 0xfffa0000 alias lowmem[0xa0000..1M), always
 *
 * The top mirror covers the reset vector: 0xfffffff0 -> lowmem 0xffff0,
 * which is the ITF window after reset.  (With the PC-98 A20  mask active
 * the CPU wraps to 0xffff0 directly as well.)
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"
#include "hw/i386/pc98.h"
#include "hw/isa/isa.h"
#include "target/i386/cpu.h"
#include "hw/core/loader.h"
#include "system/ioport.h"
#include "system/memory.h"
#include "system/reset.h"
#include "migration/vmstate.h"

/*
 * The firmware ROM is eight 32 KiB banks.  They can be provided either as one
 * file per bank (pc98bank0.bin .. pc98bank7.bin) or as the split option-ROM
 * dumps below.  Only the ITF bank and the three main-BIOS banks are mandatory.
 */
#define ROM_BANK_BYTES   0x8000
#define ROM_BANK_COUNT   8
#define ROM_IMAGE_BYTES  (ROM_BANK_BYTES * ROM_BANK_COUNT)

enum {
    BANK_PCI      = 0,
    BANK_APIC     = 1,     /* not mapped yet */
    BANK_BASIC    = 2,     /* free ROM BASIC compatibility stub */
    BANK_IDE      = 3,
    BANK_ITF      = 4,
    BANK_BIOS     = 5,     /* main BIOS spans banks 5..7 (96 KiB) */
    BANK_BIOS_TOP = 7,     /* bank paged into the 0xf8000 window */
};

#define ROM_PCI_FILE    "pc98pci.bin"
#define ROM_PCI_BYTES   0x8000
#define ROM_BASIC_FILE  "pc98basic.bin"
#define ROM_BASIC_BYTES 0x8000
#define ROM_IDE_FILE    "pc98ide.bin"
#define ROM_IDE_BYTES   0x2000
#define ROM_ITF_FILE    "pc98itf.bin"
#define ROM_ITF_BYTES   0x8000
#define ROM_BIOS_FILE   "pc98bios.bin"
#define ROM_BIOS_BYTES  0x18000
#define ROM_BANK_FILE   "pc98bank%d.bin"

#define OFF_PCI         (ROM_BANK_BYTES * BANK_PCI)
#define OFF_BASIC       (ROM_BANK_BYTES * BANK_BASIC)
#define OFF_IDE         (ROM_BANK_BYTES * BANK_IDE)
#define OFF_ITF         (ROM_BANK_BYTES * BANK_ITF)
#define OFF_BIOS        (ROM_BANK_BYTES * BANK_BIOS)

#define REQUIRED_BANKS  ((1 << BANK_ITF) | (7 << BANK_BIOS))

#define PC98_MP_RESERVE_SIZE 0x200
#define PC98_MP_BIOS_OFFSET  (PC98_MP_FLOAT_ADDR - 0x000e8000)

typedef struct QEMU_PACKED Pc98MpFloating {
    uint8_t signature[4];
    uint32_t config_table;
    uint8_t length;
    uint8_t spec_revision;
    uint8_t checksum;
    uint8_t feature[5];
} Pc98MpFloating;

typedef struct QEMU_PACKED Pc98MpConfigHeader {
    uint8_t signature[4];
    uint16_t length;
    uint8_t spec_revision;
    uint8_t checksum;
    uint8_t oem_id[8];
    uint8_t product_id[12];
    uint32_t oem_table;
    uint16_t oem_table_size;
    uint16_t entry_count;
    uint32_t local_apic_address;
    uint16_t extended_length;
    uint8_t extended_checksum;
    uint8_t reserved;
} Pc98MpConfigHeader;

typedef struct QEMU_PACKED Pc98MpProcessor {
    uint8_t type;
    uint8_t apic_id;
    uint8_t apic_version;
    uint8_t flags;
    uint32_t signature;
    uint32_t feature_flags;
    uint32_t reserved[2];
} Pc98MpProcessor;

typedef struct QEMU_PACKED Pc98MpBus {
    uint8_t type;
    uint8_t id;
    uint8_t bus_type[6];
} Pc98MpBus;

typedef struct QEMU_PACKED Pc98MpIoApic {
    uint8_t type;
    uint8_t id;
    uint8_t version;
    uint8_t flags;
    uint32_t address;
} Pc98MpIoApic;

typedef struct QEMU_PACKED Pc98MpInterrupt {
    uint8_t type;
    uint8_t interrupt_type;
    uint16_t flags;
    uint8_t source_bus;
    uint8_t source_irq;
    uint8_t destination_apic;
    uint8_t destination_irq;
} Pc98MpInterrupt;

/* selections for the 0xd8000 option-ROM window (port 0x63c) */
enum {
    DWIN_IDE = 1,
    DWIN_PCI = 2,
    DWIN_PNP = 3,
};

typedef struct Pc98MemWindow {
    MemoryRegion ram;         /* movable RAM alias, 128 KiB */
    MemoryRegion tvram;       /* overlays: aliases of the VGA regions */
    MemoryRegion vram_a8000;
    MemoryRegion vram_b0000;
    MemoryRegion vram_e0000;
} Pc98MemWindow;

struct Pc98MemState {
    MemoryRegion *ram;
    uint64_t ram_size;

    MemoryRegion rom;         /* 8 x 32 KiB ROM bank blob */
    MemoryRegion rom_empty;   /* 0xff-filled "no ROM" content */

    MemoryRegion lowmem;      /* 1 MiB container */
    MemoryRegion ram_base;
    Pc98MemWindow win[2];     /* 0x80000 / 0xa0000 windows */
    MemoryRegion cbus_rom;
    MemoryRegion d8000_rom;
    MemoryRegion ide_rom;     /* IDE BIOS option ROM at 0xd8000 */
    MemoryRegion pci_rom;     /* PCI: $PCI BIOS (BANK0) swapped in at 0xd8000 */
    MemoryRegion ide_ram;     /* IDE work RAM window at 0xda000 */
    MemoryRegion d000_ram_b;  /* PCI: reg 0x64 shadow RAM at 0xdb000 (0x1000) */
    MemoryRegion d000_ram_hi; /* PCI: reg 0x64 shadow RAM at 0xdc000 (0x4000) */
    MemoryRegion bios;
    MemoryRegion e8000_ram;
    MemoryRegion f8000_rom;
    MemoryRegion f8000_ram;
    MemoryRegion probe_page;  /* PCI: page-sized window over 0xf8000 */
    MemoryRegion a20_wrap;    /* 1 MiB wrap alias for hardware accelerators */

    MemoryRegion ram_mid;
    MemoryRegion ram_f00000;
    MemoryRegion *pegc_post;
    MemoryRegion pegc_high;
    MemoryRegion sys16m_mirror;
    MemoryRegion ram_high;
    MemoryRegion top_mirror;

    PortioList portio_list;

    /* register/latch state */
    uint8_t win_map[2];       /* 0x461 / 0x463 window base selectors */
    uint8_t dwin_sel;         /* 0x63c: 0xd8000 window content */
    uint8_t ide_rom_gate;     /* 0x53d bit 4: IDE option ROM visible */
    uint8_t ide_ram_gate;     /* 0x1e8e: IDE work RAM visible */
    uint8_t top_bank;         /* bank currently paged at 0xf8000 */
    uint32_t top_bank_ram_src; /* RAM alias source selected with top_bank */
    uint8_t bios_ram_gate;    /* 0x53d bit 1: writable BIOS RAM copy */
    uint8_t sys16m;           /* 0x43b: 16 MiB system space enabled */
    bool has_ram_f00000;
    uint8_t ide_rom_present;  /* pc98ide.bin was found */
    uint8_t hd_mask;          /* attached IDE disks, bit per drive */
    bool has_pci;             /* PCI machine: 0xc0000 window shadowed as RAM */
    bool compatibility_bios;
    bool pegc_post_compat;     /* NEC Xa7 ROM packed-pixel POST path */
    bool pegc_enabled;         /* full PEGC selected by the machine property */
    uint8_t d000_shadow;      /* PCI reg 0x64: D000 shadow-RAM enable bits */
    uint8_t bios_probe_write; /* PCI config 0x69 bit 4 */
    uint8_t f8e90_reset;
    uint8_t f8e90_value;      /* current IDE probe bitmap (the latch) */
    GPtrArray *cbus_option_roms;
    bool cbus_rom_gate;
    bool a20_wrap_enabled;

    void (*ems_cb)(void *opaque, uint32_t value);
    void *ems_cb_arg;
    void (*pegc_post_cb)(void *opaque, bool active);
    void *pegc_post_cb_arg;
};

static void mem_apply_cbus_rom_gate(Pc98MemState *s, bool enable)
{
    unsigned i;

    s->cbus_rom_gate = enable;
    memory_region_transaction_begin();
    for (i = 0; i < s->cbus_option_roms->len; i++) {
        MemoryRegion *rom = g_ptr_array_index(s->cbus_option_roms, i);

        memory_region_set_enabled(rom, enable);
    }
    memory_region_transaction_commit();
}

/*
 * Register an option ROM installed on the C-Bus.  Xa-class chipsets keep
 * C0000h-DFFFFh shadow RAM in front of expansion ROMs during POST, then
 * expose the ROMs together with the late option-ROM gate.  Older machines
 * expose C-Bus ROMs from reset.
 */
void pc98_mem_register_cbus_rom(Pc98MemState *s, MemoryRegion *rom,
                                hwaddr address)
{
    g_ptr_array_add(s->cbus_option_roms, rom);
    memory_region_add_subregion_overlap(&s->lowmem, address, rom, 1);
    memory_region_set_enabled(rom, s->cbus_rom_gate);
}

/* apply the state of one movable RAM window */
static void mem_apply_window(Pc98MemState *s, int idx)
{
    Pc98MemWindow *w = &s->win[idx];
    uint8_t val = s->win_map[idx];

    memory_region_transaction_begin();

    memory_region_set_alias_offset(&w->ram, val * 0x10000);

    /*
     * val 0x0a selects the text + B/R/G planar VRAM window at the window
     * base.  The 4th (intensity) plane lives at the fixed address 0xe0000
     * and is accessible whenever the planar graphics VRAM is (a real PC-98
     * game just writes 0xe0000 directly after enabling 16-colour mode; it
     * does not switch the RAM-window map to reach it).  The historical
     * val==0x0e "0xe0000 window" is kept as well.
     */
    memory_region_set_enabled(&w->tvram, val == 0x0a);
    memory_region_set_enabled(&w->vram_a8000, val == 0x0a);
    memory_region_set_enabled(&w->vram_b0000, val == 0x0a);
    memory_region_set_enabled(&w->vram_e0000, val == 0x0a || val == 0x0e);

    memory_region_transaction_commit();
}

#define PROBE_PAGE_SIZE 4096

/*
 * Writes to the probe-page window.  Only the latch byte at 0xe90 is live
 * (and only while config byte 0x69 bit 4 holds the window open); when the
 * BIOS RAM shadow is paged in underneath, everything else is forwarded to
 * it so the BIOS work bytes in this page keep working.  Plain ROM writes
 * are discarded, as on hardware.
 */
static void probe_page_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Pc98MemState *s = opaque;
    uint8_t *page = memory_region_get_ram_ptr(&s->probe_page);
    bool shadow = s->top_bank == BANK_BIOS_TOP && s->bios_ram_gate;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr off = addr + i;
        uint8_t b = val >> (i * 8);

        if (off == 0xe90) {
            if (s->bios_probe_write) {
                page[off] = b;
            }
        } else if (shadow) {
            page[off] = b;
            ((uint8_t *)memory_region_get_ram_ptr(s->ram))[0xf8000 + off] = b;
        }
    }
    memory_region_flush_rom_device(&s->probe_page, addr, size);
}

static uint64_t probe_page_read(void *opaque, hwaddr addr, unsigned size)
{
    Pc98MemState *s = opaque;
    uint8_t *page = memory_region_get_ram_ptr(&s->probe_page);
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        val |= (uint64_t)page[addr + i] << (i * 8);
    }
    return val;
}

static const MemoryRegionOps probe_page_ops = {
    .read = probe_page_read,
    .write = probe_page_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/* apply the state of the 0xf8000 bank window */
static void mem_apply_top_bank(Pc98MemState *s, uint32_t ram_src)
{
    s->top_bank_ram_src = ram_src;
    memory_region_transaction_begin();

    memory_region_set_alias_offset(&s->f8000_rom,
                                   s->top_bank * ROM_BANK_BYTES);

    if (s->top_bank == BANK_BIOS_TOP && s->bios_ram_gate) {
        memory_region_set_alias_offset(&s->f8000_ram, ram_src);
        memory_region_set_enabled(&s->f8000_ram, true);
    } else {
        memory_region_set_enabled(&s->f8000_ram, false);
    }

    if (s->has_pci) {
        /*
         * Xa7 keeps the IDE probe bitmap in a one-byte latch at 0xf8e90.
         * The ITF writes it while BANK4 is visible by briefly opening config
         * byte 0x69 bit 4.  The IDE BIOS reads it later with BANK7 visible.
         *
         * The latch is modelled as a page-sized rom-device copy of the
         * window: anything smaller would fragment the 0xf8000 page below
         * the granularity a hardware accelerator can map, demoting every
         * instruction fetch from this page (the BIOS keeps a timing loop
         * at 0xf8e11) to per-instruction emulation.
         */
        bool on = s->bios_probe_write ||
                  (s->top_bank == BANK_BIOS_TOP && s->ide_rom_gate);

        if (on) {
            uint8_t *page = memory_region_get_ram_ptr(&s->probe_page);

            if (s->top_bank == BANK_BIOS_TOP && s->bios_ram_gate) {
                memcpy(page,
                       (uint8_t *)memory_region_get_ram_ptr(s->ram) + ram_src,
                       PROBE_PAGE_SIZE);
            } else {
                memcpy(page,
                       (uint8_t *)memory_region_get_ram_ptr(&s->rom) +
                       s->top_bank * ROM_BANK_BYTES,
                       PROBE_PAGE_SIZE);
            }
            page[0xe90] = s->f8e90_value;
            memory_region_flush_rom_device(&s->probe_page, 0,
                                           PROBE_PAGE_SIZE);
        }
        memory_region_set_enabled(&s->probe_page, on);
    }

    memory_region_transaction_commit();
}

static void mem_apply_sys16m(Pc98MemState *s)
{
    memory_region_transaction_begin();
    memory_region_set_enabled(&s->sys16m_mirror, s->sys16m);
    if (s->has_ram_f00000) {
        memory_region_set_enabled(&s->ram_f00000, !s->sys16m);
    }
    if (s->pegc_post_compat || s->pegc_enabled) {
        memory_region_set_enabled(s->pegc_post, s->sys16m);
    }

    memory_region_transaction_commit();
    if (s->pegc_post_cb) {
        s->pegc_post_cb(s->pegc_post_cb_arg,
                        s->pegc_post_compat && s->sys16m);
    }
}

static void mem_sync_sys16m_workarea(Pc98MemState *s)
{
    uint8_t *ram = memory_region_get_ram_ptr(s->ram);
    uint64_t low16_size;

    /* 1-16 MiB extended RAM, in 128 KiB units. */
    low16_size = MIN(s->ram_size, 16 * MiB);
    ram[0x401] = (low16_size - 1 * MiB) / (128 * KiB);
    if (s->sys16m && low16_size == 16 * MiB) {
        ram[0x401] -= 8; /* the 15-16 MiB system-space hole */
    }
}

/*
 * 0xd8000 window.  When the IDE selection is active and enabled, the IDE BIOS
 * option ROM appears at 0xd8000 (0x2000); its work RAM is paged in at 0xda000
 * when ide_ram_gate is set (the ITF memory test writes it).
 */
static void mem_apply_dwin(Pc98MemState *s)
{
    bool pci_rom_on = false;
    bool ide_rom_on;
    bool ide_ram_on;
    bool d000_b_on = false;
    bool d000_hi_on = false;

    if (s->has_pci) {
        /*
         * Port 0x63c (dwin_sel) selects the 0xd8000 window content:
         * value 1 swaps in the $PCI BIOS (BANK0); any other
         * value shows the base option ROM (the internal IDE BIOS).
         *
         * Config register 0x64 then gates the option ROM's shadow RAM back
         * over it one 4 KiB page at a time: bit 0x10 -> 0xda000,
         * bit 0x20 -> 0xdb000, bit 0x80 -> 0xdc000-0xdffff.
         */
        pci_rom_on = s->dwin_sel == 1;
        ide_rom_on = s->dwin_sel != 1 &&
                     s->ide_rom_gate && s->ide_rom_present;
        ide_ram_on = (s->d000_shadow & 0x10) != 0;
        d000_b_on  = (s->d000_shadow & 0x20) != 0;
        d000_hi_on = (s->d000_shadow & 0x80) != 0;
    } else {
        ide_rom_on = s->dwin_sel == DWIN_IDE &&
                     s->ide_rom_gate && s->ide_rom_present;
        ide_ram_on = ide_rom_on && s->ide_ram_gate;
    }

    memory_region_transaction_begin();
    memory_region_set_enabled(&s->ide_rom, ide_rom_on);
    memory_region_set_enabled(&s->ide_ram, ide_ram_on);
    if (s->has_pci) {
        memory_region_set_enabled(&s->pci_rom, pci_rom_on);
        memory_region_set_enabled(&s->d000_ram_b, d000_b_on);
        memory_region_set_enabled(&s->d000_ram_hi, d000_hi_on);
    }
    memory_region_transaction_commit();
}

/*
 * Host bridge (dev0) config register 0x64 write: the D000-segment shadow
 * control byte (0x67 of the dword).  Called from the PCI host bridge.
 */
void pc98_mem_set_d000_shadow(void *opaque, uint8_t bits)
{
    Pc98MemState *s = opaque;

    if (s->d000_shadow != bits) {
        s->d000_shadow = bits;
        mem_apply_dwin(s);
    }
}

void pc98_mem_set_bios_probe_write(void *opaque, bool enable)
{
    Pc98MemState *s = opaque;

    if (s->bios_probe_write == enable) {
        return;
    }
    if (s->bios_probe_write && !enable && s->has_pci) {
        const uint8_t *page = memory_region_get_ram_ptr(&s->probe_page);

        /*
         * The ITF wrote its candidate-drive mask through the window.  The
         * chipset returns the actually populated IDE slots in the low
         * nibble; otherwise Xa7 probes empty channels indefinitely.  This
         * is the PCI-firmware equivalent of the hd_connect value that the
         * older PC-98 memory model exposed at the same physical address.
         */
        s->f8e90_value = (page[0xe90] & 0xf0) | (s->hd_mask & 0x0f);
    }
    s->bios_probe_write = enable;
    mem_apply_top_bank(s, 0xf8000);
}

/*
 * PC-98 masks A20 by wrapping the whole address space at 1 MiB.  TCG models
 * that exactly in the CPU (the pc98-a20-mask property), but a hardware
 * accelerator never sees the CPU's address mask, so mirror the only window
 * real-mode software can actually reach (segment arithmetic tops out at
 * 0x10ffef) with an alias of the first megabyte.
 */
void pc98_mem_set_a20_wrap(void *opaque, bool wrap)
{
    Pc98MemState *s = opaque;

    s->a20_wrap_enabled = wrap;
    memory_region_set_enabled(&s->a20_wrap, wrap);
}

/* fill in the BIOS work area when the writable BIOS RAM copy is paged in */
static void mem_patch_bios_workarea(Pc98MemState *s)
{
    uint8_t *ram;
    uint8_t physical_hd_mask;
    uint8_t logical_hd_mask;
    uint16_t ext_mb;

    mem_sync_sys16m_workarea(s);
    ram = memory_region_get_ram_ptr(s->ram);
    ext_mb = s->ram_size > 16 * MiB ?
             (s->ram_size - 16 * MiB) >> 20 : 0;
    ram[0x594] = ext_mb & 0xff;
    ram[0x595] = ext_mb >> 8;
    /* printer interface */
    ram[0x458] &= ~0x06;
    ram[0x5b3] &= ~0xe0;
    /* system clock: 5MHz -> 0x24, 8MHz -> 0xa4 */
    ram[0x501] = 0x24;
    /*
     * The emulated keyboard implements the PC-9801-119/PC-9821 extended
     * command set.  NEC NTDETECT uses this flag to select PC98_106KEY.
     */
    ram[0x481] |= 0x40;

    /*
     * 055Dh describes the dense BIOS drive-number space; 05BAh describes
     * physical ATA slots.  Keep the probe latch in the physical form too.
     * Overwrite rather than OR the low nibble because the ITF work-area
     * seed contains a provisional drive-0 bit before probing hardware.
     */
    physical_hd_mask = s->hd_mask & 0x0f;
    logical_hd_mask = (1U << ctpop8(physical_hd_mask)) - 1;
    ram[0x55d] = (ram[0x55d] & 0xf0) | logical_hd_mask;
    ram[0x5ba] = (ram[0x5ba] & 0xf0) | physical_hd_mask;
    ram[0xf8e90] = (ram[0xf8e90] & 0xf0) | physical_hd_mask;

    /*
     * ram[0x457] selects the IDE geometry *class* the IDE BIOS uses for INIT
     * DEVICE PARAMETERS and SENSE: drive-0 class is bits 3-5, drive-1 class
     * bits 0-2, and the SENSE geometry comes from a fixed table in the IDE
     * BIOS ROM indexed by that class (not from IDENTIFY).  Class 2 (bits 0x10)
     * is the "variable" profile -- 8 heads, 17 sectors, cylinders derived from
     * the IDENTIFY capacity -- the only class that adapts to an arbitrary disk
     * image.  The ITF's own probe picks a fixed 614/4 class out of
     * uninitialised NVRAM bits, so it has to be overridden here after the bank
     * flip.  Disk images must be partitioned for 8-head geometry to boot.
     *
     * This is programmed whenever disks are attached, so the internal disk
     * BIOS finds them even without the pc98ide.bin option ROM present.
     */
    if (s->hd_mask) {
        if (s->hd_mask & 1) {
            ram[0x457] = 0x90;   /* drive0: class 2, variable 8-head */
            ram[0x45d] |= 0x08;  /* fast ide */
            ram[0x5b0] = 0x00;   /* ide drive size */
        } else {
            ram[0x457] = 0x38;   /* no drive0 */
            ram[0x5b0] = 0x38;
        }
        if (s->hd_mask & 2) {
            ram[0x457] |= 0x42;  /* drive1: class 2 */
            ram[0x45d] |= 0x10;
        } else {
            ram[0x457] |= 0x07;  /* no drive1 */
            ram[0x5b0] |= 0x07;
        }
        if (s->hd_mask & 3) {
            ram[0x480] |= 0x80;  /* support new sense command */
            /*
             * bit 6: an IDE fixed disk is bootable -- the BIOS boot
             * dispatcher (0xfff08) tests this to run the fixed-disk boot
             * scan (which far-calls the installed IDE option ROM) instead
             * of falling through to ROM BASIC.
             */
            ram[0x45d] |= 0x40;
        }
    }
}

/* --- I/O ports --- */

static void mem_sys16m_write(void *opaque, uint32_t addr, uint32_t data)
{
    Pc98MemState *s = opaque;
    uint8_t enable = s->pegc_enabled || !(data & 0x04);

    if (s->sys16m != enable) {
        s->sys16m = enable;
        mem_apply_sys16m(s);
    }
    mem_sync_sys16m_workarea(s);
}

static uint32_t mem_sys16m_read(void *opaque, uint32_t addr)
{
    Pc98MemState *s = opaque;

    return s->sys16m ? 0x00 : 0x04;
}

/* 0x43d: flip the 0xf8000 window between the ITF and the top BIOS bank */
static void mem_bankflip_write(void *opaque, uint32_t addr, uint32_t data)
{
    Pc98MemState *s = opaque;

    switch (data) {
    case 0x00:
    case 0x10:
    case 0x18:
        if (s->top_bank != BANK_ITF) {
            s->top_bank = BANK_ITF;
            mem_apply_top_bank(s, 0xf8000);
        }
        break;
    case 0x02:
    case 0x12:
        if (s->top_bank != BANK_BIOS_TOP) {
            s->top_bank = BANK_BIOS_TOP;
            if (s->bios_ram_gate) {
                mem_patch_bios_workarea(s);
            }
            mem_apply_top_bank(s, 0xf8000);
        }
        break;
    }
}

static uint32_t mem_bankflip_read(void *opaque, uint32_t addr)
{
    return 0x00; /* don't hit the cache */
}

/* 0x43f: EMS page selection and direct 0xf8000 bank selection */
static void mem_bankctl_write(void *opaque, uint32_t addr, uint32_t data)
{
    Pc98MemState *s = opaque;

    switch (data & 0xf0) {
    case 0x20:
        if (s->ems_cb) {
            s->ems_cb(s->ems_cb_arg, data);
        }
        break;
    case 0xe0:
        if (s->top_bank != ((data >> 1) & 0x07)) {
            s->top_bank = (data >> 1) & 0x07;
            /* this path pages the writable RAM copy in from 0xe8000 */
            mem_apply_top_bank(s, 0xe8000);
        }
        break;
    }
}

static void mem_win0_write(void *opaque, uint32_t addr, uint32_t data)
{
    Pc98MemState *s = opaque;

    if (s->win_map[0] != (data & 0xfe)) {
        s->win_map[0] = data & 0xfe;
        mem_apply_window(s, 0);
    }
}

static uint32_t mem_win0_read(void *opaque, uint32_t addr)
{
    Pc98MemState *s = opaque;

    return s->win_map[0];
}

static void mem_win1_write(void *opaque, uint32_t addr, uint32_t data)
{
    Pc98MemState *s = opaque;

    if (s->win_map[1] != (data & 0xfe)) {
        s->win_map[1] = data & 0xfe;
        mem_apply_window(s, 1);
    }
}

static uint32_t mem_win1_read(void *opaque, uint32_t addr)
{
    Pc98MemState *s = opaque;

    return s->win_map[1];
}

/* 0x53d: gate the IDE option ROM and the writable BIOS RAM copy */
static void mem_romgate_write(void *opaque, uint32_t addr, uint32_t data)
{
    Pc98MemState *s = opaque;
    bool old_ide_rom_gate = s->ide_rom_gate;

    s->ide_rom_gate = !!(data & 0x10);
    /*
     * Xa7 firmware does not necessarily enable the writable BIOS-copy gate.
     * Its late IDE-ROM gate-in is the last common point before boot, after
     * POST has finished initialising (and clearing) the BIOS work area.
     */
    if (s->has_pci && !old_ide_rom_gate && s->ide_rom_gate) {
        mem_patch_bios_workarea(s);
        mem_apply_cbus_rom_gate(s, true);
    }
    mem_apply_dwin(s);
    if (s->has_pci) {
        mem_apply_top_bank(s, 0xf8000);
    }

    if (s->bios_ram_gate != !!(data & 0x02)) {
        s->bios_ram_gate = !!(data & 0x02);
        memory_region_set_enabled(&s->e8000_ram, s->bios_ram_gate);
        mem_apply_top_bank(s, 0xf8000);
    }
}

static void mem_dwin_write(void *opaque, uint32_t addr, uint32_t data)
{
    Pc98MemState *s = opaque;

    /* TODO: IDE BIOS/PCI/PnP ROM content banking at 0xd8000 */
    s->dwin_sel = data & 0x03;
    mem_apply_dwin(s);
}

static uint32_t mem_dwin_read(void *opaque, uint32_t addr)
{
    Pc98MemState *s = opaque;

    return s->dwin_sel;
}

static uint32_t mem_63d_read(void *opaque, uint32_t addr)
{
    return 0x04;
}

static void mem_ide_ram_write(void *opaque, uint32_t addr, uint32_t data)
{
    Pc98MemState *s = opaque;

    switch (data) {
    case 0x80:
        s->ide_ram_gate = 0;
        break;
    case 0x81:
        s->ide_ram_gate = 1;
        break;
    }
    mem_apply_dwin(s);
}

static uint32_t mem_ide_ram_read(void *opaque, uint32_t addr)
{
    Pc98MemState *s = opaque;

    return s->ide_ram_gate ? 0x81 : 0x80;
}

static const MemoryRegionPortio pc98_mem_portio[] = {
    { 0x43b, 1, 1, .read = mem_sys16m_read, .write = mem_sys16m_write },
    { 0x43d, 1, 1, .read = mem_bankflip_read, .write = mem_bankflip_write },
    { 0x43f, 1, 1, .write = mem_bankctl_write },
    { 0x461, 1, 1, .read = mem_win0_read, .write = mem_win0_write },
    { 0x463, 1, 1, .read = mem_win1_read, .write = mem_win1_write },
    { 0x53d, 1, 1, .write = mem_romgate_write },
    { 0x63c, 1, 1, .read = mem_dwin_read, .write = mem_dwin_write },
    { 0x63d, 1, 1, .read = mem_63d_read },
    { 0x1e8e, 1, 1, .read = mem_ide_ram_read, .write = mem_ide_ram_write },
    PORTIO_END_OF_LIST(),
};

/* --- firmware image loading and fix-ups --- */

/*
 * Replace a matched code site with NOPs.  A template byte of 0x00 is treated
 * as a wildcard (it stands for an immediate operand we do not constrain).  The
 * ITF variant that ends in "hlt; jmp $-2" is accepted either at its full
 * length or truncated just before the halt, hence the two acceptable lengths.
 */
static bool nop_code_site(uint8_t *rom, int at, const uint8_t *tmpl,
                          int full_len, int short_len)
{
    int i;

    for (i = 0; i < full_len; i++) {
        if (tmpl[i] != 0x00 && tmpl[i] != rom[at + i]) {
            break;
        }
    }
    if (i == short_len || i == full_len) {
        memset(rom + at, 0x90, i);
        return true;
    }
    return false;
}

/*
 * The ITF power-on self test halts the machine when it "detects" a timer or
 * cache fault that only occurs under emulation.  Each abort site loads the
 * address of an error string, saves a resume pointer, and jumps to the
 * reporting routine.  We first find the string, then locate the abort site by
 * its fixed instruction signature (keyed on the string's address) and NOP it
 * so the POST proceeds.
 */
static bool disable_itf_selftest(uint8_t *rom, const char *msg)
{
    int msg_len = strlen(msg);
    bool patched = false;
    int base;

    for (base = 1; base < ROM_BANK_BYTES - msg_len; base++) {
        uint16_t ptr;
        int at, i;

        for (i = 0; i < msg_len; i++) {
            if (rom[base + i] != (uint8_t)msg[i]) {
                break;
            }
        }
        if (i != msg_len) {
            continue;
        }

        /* the code loads the string pointer as (base - 1) */
        ptr = base - 1;

        {
            uint8_t tmpl[12] = {
                0xbe, ptr & 0xff, ptr >> 8,   /* mov si, msg      */
                0xbd, 0x00, 0x00,             /* mov bp, resume   */
                0xe9, 0x00, 0x00,             /* jmp report       */
                0xf4,                         /* hlt   (resume:)  */
                0xeb, 0xfe,                   /* jmp $-2          */
            };
            for (at = 0; at < ROM_BANK_BYTES - 12; at++) {
                tmpl[4] = (at + 9) & 0xff;
                tmpl[5] = (at + 9) >> 8;
                patched |= nop_code_site(rom, at, tmpl, 12, 9);
            }
        }
        {
            uint8_t tmpl[13] = {
                0xbe, ptr & 0xff, ptr >> 8,   /* mov si, msg      */
                0x8b, 0xdd,                   /* mov bx, bp       */
                0xbd, 0x00, 0x00,             /* mov bp, resume   */
                0xe9, 0x00, 0x00,             /* jmp report       */
                0x8b, 0xeb,                   /* mov bp, bx (res:)*/
            };
            for (at = 0; at < ROM_BANK_BYTES - 13; at++) {
                tmpl[6] = (at + 11) & 0xff;
                tmpl[7] = (at + 11) >> 8;
                patched |= nop_code_site(rom, at, tmpl, 13, 13);
            }
        }
    }
    return patched;
}

/* recompute the two ITF-bank checksum bytes after patching */
static void fix_rom_checksum(uint8_t *rom)
{
    uint8_t lo = 0, hi = 0;
    int i;

    for (i = 0; i < ROM_BANK_BYTES; i += 2) {
        lo += rom[i + 0];
        hi += rom[i + 1];
    }
    rom[0x7ffe] -= lo;
    rom[0x7fff] -= hi;
}

static bool read_rom_image(const char *name, uint8_t *dest, int size)
{
    char *path = qemu_find_file(QEMU_FILE_TYPE_BIOS, name);
    bool ok = false;

    if (path) {
        ok = (load_image_size(path, dest, size) == size);
        g_free(path);
    }
    return ok;
}

static bool mem_load_firmware(Pc98MemState *s, uint8_t *buf)
{
    char name[32];
    uint32_t found = 0;
    int i;

    for (i = 0; i < ROM_BANK_COUNT; i++) {
        snprintf(name, sizeof(name), ROM_BANK_FILE, i);
        if (read_rom_image(name, buf + ROM_BANK_BYTES * i, ROM_BANK_BYTES)) {
            found |= (1 << i);
        }
    }
    /*
     * A complete bank dump is one coherent firmware set.  Do not overwrite
     * parts of it with split ROMs found later on another QEMU data path (for
     * example the bundled free BIOS after -L points at a machine dump).
     * Split images only fill regions for which no bank image was found.
     */
    if (!(found & (1 << BANK_PCI)) &&
        read_rom_image(ROM_PCI_FILE, buf + OFF_PCI, ROM_PCI_BYTES)) {
        found |= (1 << BANK_PCI);
    }
    if (!(found & (1 << BANK_BASIC)) &&
        read_rom_image(ROM_BASIC_FILE, buf + OFF_BASIC, ROM_BASIC_BYTES)) {
        found |= (1 << BANK_BASIC);
    }
    if (!(found & (1 << BANK_IDE)) &&
        read_rom_image(ROM_IDE_FILE, buf + OFF_IDE, ROM_IDE_BYTES)) {
        found |= (1 << BANK_IDE);
    }
    if (!(found & (1 << BANK_ITF)) &&
        read_rom_image(ROM_ITF_FILE, buf + OFF_ITF, ROM_ITF_BYTES)) {
        found |= (1 << BANK_ITF);
    }
    if ((found & (7 << BANK_BIOS)) != (7 << BANK_BIOS) &&
        read_rom_image(ROM_BIOS_FILE, buf + OFF_BIOS, ROM_BIOS_BYTES)) {
        found |= (7 << BANK_BIOS);
    }

    if ((found & REQUIRED_BANKS) != REQUIRED_BANKS) {
        return false;
    }

    /* ITF: neutralise the emulation-hostile self tests */
    {
        static const char *const faults[] = {
            "TIMER ERROR",
            "TIMER INTERRUPT ERROR",
            "CACHE RAM ERROR",
            "CACHE ERROR",
            "2ND CACHE RAM ERROR",
            "2ND CACHE ERROR",
        };
        bool patched = false;

        for (i = 0; i < (int)ARRAY_SIZE(faults); i++) {
            patched |= disable_itf_selftest(buf + OFF_ITF, faults[i]);
        }
        if (patched) {
            fix_rom_checksum(buf + OFF_ITF);
        }
    }

    /*
     * The Xa7 dump publishes a PnP entry in the pageable D8000 ROM bank,
     * which is not implemented yet.  Keep that structure hidden rather than
     * letting a guest call into the IDE ROM currently visible at D8000.
     *
     * The free BIOS entry is permanently mapped at FD80:1800 and is safe to
     * expose.  It can be distinguished by the real-mode segment in the
     * installation-check structure, so do not hide every $PnP signature as
     * the original workaround did.
     */
    for (i = 0x8000; i < 0x18000; i += 0x10) {
        uint8_t *p = buf + OFF_BIOS + i;
        if (p[0] == 0x24 && p[1] == 'P' && p[2] == 'n' && p[3] == 'P' &&
            lduw_le_p(p + 15) == 0xd800) {
            p[0] = 'n';
            p[2] = 0x24;
            break;
        }
    }

    s->ide_rom_present = ((found & (1 << BANK_IDE)) != 0);
    return true;
}

static bool mem_is_compatibility_bios(const uint8_t *buf)
{
    static const char signature[] = "PC-98 COMPATIBILITY BIOS SETUP";
    const uint8_t *bios = buf + OFF_BIOS;
    size_t i;

    for (i = 0; i + sizeof(signature) - 1 <= ROM_BIOS_BYTES; i++) {
        if (!memcmp(bios + i, signature, sizeof(signature) - 1)) {
            return true;
        }
    }
    return false;
}

static uint8_t pc98_mp_checksum(const void *data, size_t length)
{
    const uint8_t *p = data;
    uint8_t sum = 0;

    while (length--) {
        sum += *p++;
    }
    return -sum;
}

static Pc98MpInterrupt *pc98_mp_add_interrupt(uint8_t **entry,
                                              uint8_t entry_type,
                                              uint8_t interrupt_type,
                                              uint16_t flags,
                                              uint8_t source_irq,
                                              uint8_t destination_apic,
                                              uint8_t destination_irq)
{
    Pc98MpInterrupt *interrupt = (Pc98MpInterrupt *)*entry;

    *entry += sizeof(*interrupt);
    interrupt->type = entry_type;
    interrupt->interrupt_type = interrupt_type;
    interrupt->flags = cpu_to_le16(flags);
    interrupt->source_bus = 0;
    interrupt->source_irq = source_irq;
    interrupt->destination_apic = destination_apic;
    interrupt->destination_irq = destination_irq;
    return interrupt;
}

void pc98_mem_install_mptable(Pc98MemState *s, Error **errp)
{
    uint8_t image[PC98_MP_RESERVE_SIZE] = { 0 };
    Pc98MpFloating *floating = (Pc98MpFloating *)image;
    Pc98MpConfigHeader *header = (Pc98MpConfigHeader *)(image + 16);
    uint8_t *entry = image + 16 + sizeof(*header);
    uint8_t *rom = memory_region_get_ram_ptr(&s->rom) + OFF_BIOS;
    CPUState *cs;
    unsigned entry_count = 0;
    unsigned processor_count = 0;
    size_t i;
    int irq;

    QEMU_BUILD_BUG_ON(sizeof(*floating) != 16);
    QEMU_BUILD_BUG_ON(sizeof(*header) != 44);
    QEMU_BUILD_BUG_ON(sizeof(Pc98MpProcessor) != 20);
    QEMU_BUILD_BUG_ON(sizeof(Pc98MpBus) != 8);
    QEMU_BUILD_BUG_ON(sizeof(Pc98MpIoApic) != 8);
    QEMU_BUILD_BUG_ON(sizeof(Pc98MpInterrupt) != 8);

    if (!s->compatibility_bios) {
        error_setg(errp, "pc9821 SMP requires the QEMU PC-98 compatibility "
                   "BIOS");
        return;
    }
    for (i = 0; i < PC98_MP_RESERVE_SIZE; i++) {
        if (rom[PC98_MP_BIOS_OFFSET + i] != 0xff) {
            error_setg(errp, "PC-98 compatibility BIOS has no empty MPS "
                       "table reserve at 0x%x", PC98_MP_FLOAT_ADDR);
            return;
        }
    }

    memcpy(floating->signature, "_MP_", 4);
    floating->config_table = cpu_to_le32(PC98_MP_CONFIG_ADDR);
    floating->length = 1;
    floating->spec_revision = 4;

    memcpy(header->signature, "PCMP", 4);
    header->spec_revision = 4;
    memcpy(header->oem_id, "QEMU    ", 8);
    memcpy(header->product_id, "PC-9821 SMP ", 12);
    header->local_apic_address = cpu_to_le32(0xfee00000);

    CPU_FOREACH(cs) {
        X86CPU *cpu = X86_CPU(cs);
        CPUX86State *env = &cpu->env;
        Pc98MpProcessor *processor = (Pc98MpProcessor *)entry;

        entry += sizeof(*processor);
        processor->type = 0;
        processor->apic_id = cpu->apic_id;
        processor->apic_version = 0x14;
        processor->flags = 1 | (cs == first_cpu ? 2 : 0);
        processor->signature = cpu_to_le32(env->cpuid_version);
        processor->feature_flags =
            cpu_to_le32(env->features[FEAT_1_EDX]);
        processor_count++;
        entry_count++;
    }
    if (processor_count < 2 || processor_count > 4) {
        error_setg(errp, "PC-98 MPS table requires 2 to 4 processors");
        return;
    }

    for (irq = 0; irq < 2; irq++) {
        Pc98MpBus *bus = (Pc98MpBus *)entry;

        entry += sizeof(*bus);
        bus->type = 1;
        bus->id = irq;
        memcpy(bus->bus_type, irq ? "PCI   " : "ISA   ", 6);
        entry_count++;
    }
    {
        Pc98MpIoApic *ioapic = (Pc98MpIoApic *)entry;

        entry += sizeof(*ioapic);
        ioapic->type = 2;
        ioapic->id = PC98_IOAPIC_ID;
        ioapic->version = 0x11;
        ioapic->flags = 1;
        ioapic->address = cpu_to_le32(0xfec00000);
        entry_count++;
    }

    /*
     * PC-98 slave PIC cascades on IRQ7; IRQ8..15 consequently use
     * I/O APIC pins 7..14.
     */
    for (irq = 0; irq < ISA_NUM_IRQS; irq++) {
        unsigned pin;
        uint16_t flags;

        if (irq == 7) {
            continue;
        }
        pin = irq < 7 ? irq : irq - 1;
        flags = irq == 14 ? 0x000d : 0x0005;
        pc98_mp_add_interrupt(&entry, 3, 0, flags, irq,
                              PC98_IOAPIC_ID, pin);
        entry_count++;
    }
    pc98_mp_add_interrupt(&entry, 3, 3, 0x0005, 0,
                          PC98_IOAPIC_ID, PC98_IOAPIC_EXTINT_PIN);
    entry_count++;
    pc98_mp_add_interrupt(&entry, 4, 3, 0x0005, 0,
                          X86_CPU(first_cpu)->apic_id, 0);
    entry_count++;

    g_assert(entry <= image + sizeof(image));
    header->length = cpu_to_le16(entry - (uint8_t *)header);
    header->entry_count = cpu_to_le16(entry_count);
    header->checksum = pc98_mp_checksum(header,
                                        entry - (uint8_t *)header);
    floating->checksum = pc98_mp_checksum(floating, sizeof(*floating));
    memcpy(rom + PC98_MP_BIOS_OFFSET, image, sizeof(image));
}

/*
 * Reset.  sys16m is deliberately left untouched: the 16 MiB-space selection is
 * a latch that survives a soft reset on real hardware, so re-seeding it here
 * would diverge from that.
 */
static void pc98_mem_reset(void *opaque)
{
    Pc98MemState *s = opaque;

    s->win_map[0] = 0x08;
    s->win_map[1] = 0x0a;
    mem_apply_window(s, 0);
    mem_apply_window(s, 1);
    mem_apply_sys16m(s);

    /*
     * PCI (Xa7) firmware shadows the 0xd8000 window as RAM for the sizing
     * POST and gates the option ROM in only afterwards, so start with both
     * the window content selector (dwin_sel != 1) and the IDE gate off there.
     * The BX2 firmware expects the IDE ROM visible from reset (dwin=DWIN_IDE).
     */
    s->dwin_sel = s->has_pci ? 0 : DWIN_IDE;
    s->ide_rom_gate = s->has_pci ? 0 : 1;
    s->ide_ram_gate = 1;
    mem_apply_dwin(s);
    mem_apply_cbus_rom_gate(s, !s->has_pci);

    s->bios_ram_gate = 0;
    memory_region_set_enabled(&s->e8000_ram, false);

    s->bios_probe_write = 0;
    s->f8e90_value = s->f8e90_reset;
    /* every CPU reset re-masks A20 on PC-98 (see x86_cpu_reset_hold) */
    s->a20_wrap_enabled = true;
    memory_region_set_enabled(&s->a20_wrap, s->a20_wrap_enabled);

    s->top_bank = BANK_ITF;
    mem_apply_top_bank(s, 0xf8000);
}

static int pc98_mem_post_load(void *opaque, int version_id)
{
    Pc98MemState *s = opaque;

    mem_apply_window(s, 0);
    mem_apply_window(s, 1);
    mem_apply_sys16m(s);
    mem_sync_sys16m_workarea(s);
    mem_apply_dwin(s);
    mem_apply_cbus_rom_gate(s, s->cbus_rom_gate);
    memory_region_set_enabled(&s->e8000_ram, s->bios_ram_gate);
    mem_apply_top_bank(s, s->top_bank_ram_src);
    memory_region_set_enabled(&s->a20_wrap, s->a20_wrap_enabled);
    return 0;
}

static const VMStateDescription vmstate_pc98_mem = {
    .name = "pc98-mem",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pc98_mem_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(win_map, Pc98MemState, 2),
        VMSTATE_UINT8(dwin_sel, Pc98MemState),
        VMSTATE_UINT8(ide_rom_gate, Pc98MemState),
        VMSTATE_UINT8(ide_ram_gate, Pc98MemState),
        VMSTATE_UINT8(top_bank, Pc98MemState),
        VMSTATE_UINT32(top_bank_ram_src, Pc98MemState),
        VMSTATE_UINT8(bios_ram_gate, Pc98MemState),
        VMSTATE_UINT8(sys16m, Pc98MemState),
        VMSTATE_UINT8(d000_shadow, Pc98MemState),
        VMSTATE_UINT8(bios_probe_write, Pc98MemState),
        VMSTATE_UINT8(f8e90_value, Pc98MemState),
        VMSTATE_BOOL(cbus_rom_gate, Pc98MemState),
        VMSTATE_BOOL(a20_wrap_enabled, Pc98MemState),
        VMSTATE_END_OF_LIST()
    }
};

static void mem_build_window(Pc98MemState *s, int idx, hwaddr base,
                             const Pc98VgaRegions *vga)
{
    Pc98MemWindow *w = &s->win[idx];
    g_autofree char *name = g_strdup_printf("pc98.win%d-ram", idx + 1);

    memory_region_init_alias(&w->ram, NULL, name, s->ram, 0, 0x20000);
    memory_region_add_subregion(&s->lowmem, base, &w->ram);

    memory_region_init_alias(&w->tvram, NULL, "pc98.win-tvram",
                             vga->tvram, 0, 0x8000);
    memory_region_add_subregion_overlap(&s->lowmem, base, &w->tvram, 1);
    memory_region_set_enabled(&w->tvram, false);

    memory_region_init_alias(&w->vram_a8000, NULL, "pc98.win-vram-a8000",
                             vga->vram_a8000, 0, 0x8000);
    memory_region_add_subregion_overlap(&s->lowmem, base + 0x8000,
                                        &w->vram_a8000, 1);
    memory_region_set_enabled(&w->vram_a8000, false);

    memory_region_init_alias(&w->vram_b0000, NULL, "pc98.win-vram-b0000",
                             vga->vram_b0000, 0, 0x10000);
    memory_region_add_subregion_overlap(&s->lowmem, base + 0x10000,
                                        &w->vram_b0000, 1);
    memory_region_set_enabled(&w->vram_b0000, false);

    memory_region_init_alias(&w->vram_e0000, NULL, "pc98.win-vram-e0000",
                             vga->vram_e0000, 0, 0x8000);
    memory_region_add_subregion_overlap(&s->lowmem, base + 0x40000,
                                        &w->vram_e0000, 1);
    memory_region_set_enabled(&w->vram_e0000, false);
}

Pc98MemState *pc98_mem_init(MemoryRegion *system_memory,
                            MemoryRegion *system_io,
                            MemoryRegion *ram,
                            uint64_t ram_size,
                            const Pc98VgaRegions *vga,
                            uint8_t hd_connect,
                            bool has_pci,
                            bool pegc_post_compat,
                            bool pegc_enabled,
                            void (*ems_select)(void *opaque, uint32_t value),
                            void *ems_opaque)
{
    Pc98MemState *s = g_new0(Pc98MemState, 1);
    uint8_t *buf;

    s->ram = ram;
    s->ram_size = ram_size;
    s->hd_mask = hd_connect;
    s->has_pci = has_pci;
    s->pegc_post_compat = pegc_post_compat && !pegc_enabled;
    s->pegc_enabled = pegc_enabled;
    s->cbus_option_roms = g_ptr_array_new();
    s->cbus_rom_gate = !has_pci;
    s->sys16m = 1;
    s->ems_cb = ems_select;
    s->ems_cb_arg = ems_opaque;
    s->pegc_post_cb = vga->set_pegc_post_active;
    s->pegc_post_cb_arg = vga->pegc_opaque;

    /* ROM blob */
    buf = g_malloc(ROM_IMAGE_BYTES);
    memset(buf, 0xff, ROM_IMAGE_BYTES);
    if (!mem_load_firmware(s, buf)) {
        error_report("could not load PC-9821 BIOS "
                     "(pc98bank*.bin or pc98itf.bin+pc98bios.bin; use -L)");
        exit(1);
    }
    s->compatibility_bios = mem_is_compatibility_bios(buf);
    if (s->pegc_enabled && !s->compatibility_bios) {
        error_report("PEGC requires the QEMU PC-98 compatibility BIOS");
        exit(1);
    }
    s->f8e90_reset = buf[BANK_BIOS_TOP * ROM_BANK_BYTES + 0xe90];
    memory_region_init_rom(&s->rom, NULL, "pc98.rom", ROM_IMAGE_BYTES,
                           &error_fatal);
    memcpy(memory_region_get_ram_ptr(&s->rom), buf, ROM_IMAGE_BYTES);
    g_free(buf);

    /*
     * Unpopulated C-bus extension-ROM windows.  The BIOS probes these by
     * far-calling each window's init entry based on a fixed presence mask,
     * so a bare 0xff/0x00 fill makes it execute garbage and hang.  Fill the
     * first byte of every 2 KiB slot with RETF (0xcb) so a spurious probe
     * call returns immediately; the rest stays 0xff (open-bus like).
     */
    memory_region_init_rom(&s->rom_empty, NULL, "pc98.rom-empty", 0x18000,
                           &error_fatal);
    {
        uint8_t *p = memory_region_get_ram_ptr(&s->rom_empty);
        unsigned off;
        memset(p, 0xff, 0x18000);
        for (off = 0; off < 0x18000; off += 0x800) {
            p[off] = 0xcb; /* RETF */
        }
    }

    /* low 1 MiB container */
    memory_region_init(&s->lowmem, NULL, "pc98.lowmem", 0x100000);

    memory_region_init_alias(&s->ram_base, NULL, "pc98.ram-base",
                             ram, 0, 0x80000);
    memory_region_add_subregion(&s->lowmem, 0, &s->ram_base);

    mem_build_window(s, 0, 0x80000, vga);
    mem_build_window(s, 1, 0xa0000, vga);

    /*
     * C-bus / option-ROM region at 0xc0000-0xdffff.
     *
     * On the PCI machines (Xa7-class firmware) the chipset shadows this whole
     * region as RAM: the memory-sizing POST writes and reads back 0xc0000
     * through 0xdffff (including the 0xd8000 option-ROM window), and only
     * *after* the test does the firmware gate the IDE/PCI option ROM in.  So
     * back the full 0x20000 with the underlying DRAM and skip the open-bus
     * ROM window; the IDE ROM still overlays at 0xd8000 once gated on.
     *
     * On the older (BX2) firmware the 0xc0000 window stays an open-bus ROM
     * whose 2 KiB RETF fill keeps the BIOS's far-call probe from executing
     * garbage, and the 0xd8000 window is a separate open-bus ROM.
     */
    if (has_pci) {
        memory_region_init_alias(&s->cbus_rom, NULL, "pc98.cbus-ram",
                                 ram, 0xc0000, 0x20000);
        memory_region_add_subregion(&s->lowmem, 0xc0000, &s->cbus_rom);
    } else {
        memory_region_init_alias(&s->cbus_rom, NULL, "pc98.cbus-rom",
                                 &s->rom_empty, 0, 0x18000);
        memory_region_add_subregion(&s->lowmem, 0xc0000, &s->cbus_rom);

        memory_region_init_alias(&s->d8000_rom, NULL, "pc98.d8000-rom",
                                 &s->rom_empty, 0, 0x8000);
        memory_region_add_subregion(&s->lowmem, 0xd8000, &s->d8000_rom);
    }

    /*
     * IDE / option-ROM at 0xd8000 (overlays the window).  The BX2 firmware
     * uses only the 8 KiB IDE BIOS; the Xa7 firmware banks in a larger option
     * ROM (0x8000) whose upper pages it shadows to RAM via reg 0x64.
     */
    memory_region_init_alias(&s->ide_rom, NULL, "pc98.ide-rom",
                             &s->rom, OFF_IDE, has_pci ? 0x8000 : 0x2000);
    memory_region_add_subregion_overlap(&s->lowmem, 0xd8000, &s->ide_rom, 1);
    memory_region_set_enabled(&s->ide_rom, false);

    /* PCI: $PCI BIOS (BANK0) swapped into 0xd8000 when dwin_sel == 1 */
    if (has_pci) {
        memory_region_init_alias(&s->pci_rom, NULL, "pc98.pci-rom",
                                 &s->rom, OFF_PCI, 0x8000);
        memory_region_add_subregion_overlap(&s->lowmem, 0xd8000,
                                            &s->pci_rom, 1);
        memory_region_set_enabled(&s->pci_rom, false);
    }

    /*
     * IDE BIOS work RAM window at 0xda000 (tested by the ITF POST).  The BX2
     * firmware pages the whole 0x2000 in via port 0x1e8e; the Xa7 firmware
     * gates it one page at a time via reg 0x64, so restrict it to page 10.
     */
    memory_region_init_alias(&s->ide_ram, NULL, "pc98.ide-ram",
                             ram, 0xda000, has_pci ? 0x1000 : 0x2000);
    memory_region_add_subregion_overlap(&s->lowmem, 0xda000, &s->ide_ram, 2);
    memory_region_set_enabled(&s->ide_ram, false);

    /* PCI: reg 0x64 per-page shadow RAM over the option ROM (0xdb000, 0xdc000) */
    if (has_pci) {
        memory_region_init_alias(&s->d000_ram_b, NULL, "pc98.d000-ram-b",
                                 ram, 0xdb000, 0x1000);
        memory_region_add_subregion_overlap(&s->lowmem, 0xdb000,
                                            &s->d000_ram_b, 2);
        memory_region_set_enabled(&s->d000_ram_b, false);

        memory_region_init_alias(&s->d000_ram_hi, NULL, "pc98.d000-ram-hi",
                                 ram, 0xdc000, 0x4000);
        memory_region_add_subregion_overlap(&s->lowmem, 0xdc000,
                                            &s->d000_ram_hi, 2);
        memory_region_set_enabled(&s->d000_ram_hi, false);
    }

    memory_region_init_alias(&s->bios, NULL, "pc98.bios",
                             &s->rom, OFF_BIOS, 0x18000);
    memory_region_add_subregion(&s->lowmem, 0xe8000, &s->bios);

    memory_region_init_alias(&s->e8000_ram, NULL, "pc98.e8000-ram",
                             ram, 0xe8000, 0x10000);
    memory_region_add_subregion_overlap(&s->lowmem, 0xe8000,
                                        &s->e8000_ram, 1);
    memory_region_set_enabled(&s->e8000_ram, false);

    memory_region_init_alias(&s->f8000_rom, NULL, "pc98.f8000-rom",
                             &s->rom, OFF_ITF, 0x8000);
    memory_region_add_subregion_overlap(&s->lowmem, 0xf8000,
                                        &s->f8000_rom, 2);

    memory_region_init_alias(&s->f8000_ram, NULL, "pc98.f8000-ram",
                             ram, 0xf8000, 0x8000);
    memory_region_add_subregion_overlap(&s->lowmem, 0xf8000,
                                        &s->f8000_ram, 3);
    memory_region_set_enabled(&s->f8000_ram, false);

    if (has_pci) {
        /* see the comment in mem_apply_top_bank() */
        memory_region_init_rom_device(&s->probe_page, NULL,
                                      &probe_page_ops, s,
                                      "pc98.f8000-probe", PROBE_PAGE_SIZE,
                                      &error_fatal);
        memory_region_add_subregion_overlap(&s->lowmem, 0xf8000,
                                            &s->probe_page, 4);
        memory_region_set_enabled(&s->probe_page, false);
        s->f8e90_value = s->f8e90_reset;
    }

    memory_region_add_subregion(system_memory, 0, &s->lowmem);

    /* A20 wrap window for hardware accelerators; see pc98_mem_set_a20_wrap */
    memory_region_init_alias(&s->a20_wrap, NULL, "pc98.a20-wrap",
                             &s->lowmem, 0, 0x100000);
    memory_region_add_subregion_overlap(system_memory, 0x100000,
                                        &s->a20_wrap, 1);
    /* the CPU comes out of reset with A20 masked; match it */
    memory_region_set_enabled(&s->a20_wrap, true);

    /* 1 MiB .. the smaller of installed RAM and 15 MiB */
    memory_region_init_alias(&s->ram_mid, NULL, "pc98.ram-mid",
                             ram, 0x100000,
                             MIN(ram_size, 15 * MiB) - 1 * MiB);
    memory_region_add_subregion(system_memory, 0x100000, &s->ram_mid);

    /* 15 MiB .. 16 MiB: RAM when the 16MB system space is disabled */
    if (ram_size > 15 * MiB) {
        memory_region_init_alias(&s->ram_f00000, NULL, "pc98.ram-f00000",
                                 ram, 0xf00000,
                                 MIN(ram_size - 15 * MiB, 1 * MiB));
        memory_region_add_subregion(system_memory, 0xf00000,
                                    &s->ram_f00000);
        memory_region_set_enabled(&s->ram_f00000, false);
        s->has_ram_f00000 = true;
    }

    /*
     * NEC Xa7 firmware tests and displays its POST through the PEGC linear
     * aperture without first branching on a presence result.  This backing
     * store is mutually exclusive with the 15--16 MiB RAM alias through
     * mem_apply_sys16m().
     */
    s->pegc_post = vga->pegc_post;
    if (s->pegc_post_compat || s->pegc_enabled) {
        memory_region_add_subregion_overlap(system_memory, 0xf00000,
                                            s->pegc_post, 1);
        memory_region_set_enabled(s->pegc_post, s->sys16m);
    }
    if (s->pegc_enabled) {
        /*
         * 32-bit PC-9821 software, including Windows NT setup, uses the
         * always-present top-of-address-space alias of the linear PEGC VRAM.
         */
        memory_region_init_alias(&s->pegc_high, NULL, "pc98.pegc-high",
                                 s->pegc_post, 0, 0x80000);
        memory_region_add_subregion(system_memory, 0xfff00000,
                                    &s->pegc_high);
    }

    /*
     * 16MB system space mirror of the low-1MiB layout (0xfa0000..0xffffff
     * mirrors 0xa0000..0xfffff).
     */
    memory_region_init_alias(&s->sys16m_mirror, NULL, "pc98.sys16m-mirror",
                             &s->lowmem, 0xa0000, 0x60000);
    memory_region_add_subregion_overlap(system_memory, 0xfa0000,
                                        &s->sys16m_mirror, 1);

    /* RAM above 16 MiB */
    if (ram_size > 0x1000000) {
        memory_region_init_alias(&s->ram_high, NULL, "pc98.ram-high",
                                 ram, 0x1000000, ram_size - 0x1000000);
        memory_region_add_subregion(system_memory, 0x1000000, &s->ram_high);
    }

    /* top-of-4G mirror (covers the reset vector) */
    memory_region_init_alias(&s->top_mirror, NULL, "pc98.top-mirror",
                             &s->lowmem, 0xa0000, 0x60000);
    memory_region_add_subregion(system_memory, 0xfffa0000, &s->top_mirror);

    /* bank switch ports */
    portio_list_init(&s->portio_list, NULL, pc98_mem_portio, s, "pc98-mem");
    portio_list_add(&s->portio_list, system_io, 0);

    qemu_register_reset(pc98_mem_reset, s);
    pc98_mem_reset(s);
    vmstate_register(NULL, 0, &vmstate_pc98_mem, s);

    return s;
}
