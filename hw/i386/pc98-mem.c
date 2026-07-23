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
 *   +-- pc98-vram-f00000 @ 0xf00000  PEGC linear VRAM (16MB space on) prio 1
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
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/i386/pc98.h"
#include "hw/core/loader.h"
#include "system/ioport.h"
#include "system/memory.h"
#include "system/reset.h"

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
    BANK_IDE      = 3,
    BANK_ITF      = 4,
    BANK_BIOS     = 5,     /* main BIOS spans banks 5..7 (96 KiB) */
    BANK_BIOS_TOP = 7,     /* bank paged into the 0xf8000 window */
};

#define ROM_PCI_FILE    "pc98pci.bin"
#define ROM_PCI_BYTES   0x8000
#define ROM_IDE_FILE    "pc98ide.bin"
#define ROM_IDE_BYTES   0x2000
#define ROM_ITF_FILE    "pc98itf.bin"
#define ROM_ITF_BYTES   0x8000
#define ROM_BIOS_FILE   "pc98bios.bin"
#define ROM_BIOS_BYTES  0x18000
#define ROM_BANK_FILE   "pc98bank%d.bin"

#define OFF_PCI         (ROM_BANK_BYTES * BANK_PCI)
#define OFF_IDE         (ROM_BANK_BYTES * BANK_IDE)
#define OFF_ITF         (ROM_BANK_BYTES * BANK_ITF)
#define OFF_BIOS        (ROM_BANK_BYTES * BANK_BIOS)

#define REQUIRED_BANKS  ((1 << BANK_ITF) | (7 << BANK_BIOS))

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
    MemoryRegion ide_ram;     /* IDE work RAM window at 0xda000 */
    MemoryRegion bios;
    MemoryRegion e8000_ram;
    MemoryRegion f8000_rom;
    MemoryRegion f8000_ram;

    MemoryRegion ram_mid;
    MemoryRegion ram_f00000;
    MemoryRegion *pegc_window;
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
    uint8_t bios_ram_gate;    /* 0x53d bit 1: writable BIOS RAM copy */
    uint8_t sys16m;           /* 0x43b: 16 MiB system space enabled */
    uint8_t ide_rom_present;  /* pc98ide.bin was found */
    uint8_t hd_mask;          /* attached IDE disks, bit per drive */

    void (*ems_cb)(void *opaque, uint32_t value);
    void *ems_cb_arg;
};

/* apply the state of one movable RAM window */
static void mem_apply_window(Pc98MemState *s, int idx)
{
    Pc98MemWindow *w = &s->win[idx];
    uint8_t val = s->win_map[idx];

    memory_region_transaction_begin();

    memory_region_set_alias_offset(&w->ram, val * 0x10000);

    /* val 0x0a: text+planar VRAM window; val 0x0e: 0xe0000 VRAM window */
    memory_region_set_enabled(&w->tvram, val == 0x0a);
    memory_region_set_enabled(&w->vram_a8000, val == 0x0a);
    memory_region_set_enabled(&w->vram_b0000, val == 0x0a);
    memory_region_set_enabled(&w->vram_e0000, val == 0x0e);

    memory_region_transaction_commit();
}

/* apply the state of the 0xf8000 bank window */
static void mem_apply_top_bank(Pc98MemState *s, uint32_t ram_src)
{
    memory_region_transaction_begin();

    memory_region_set_alias_offset(&s->f8000_rom,
                                   s->top_bank * ROM_BANK_BYTES);

    if (s->top_bank == BANK_BIOS_TOP && s->bios_ram_gate) {
        memory_region_set_alias_offset(&s->f8000_ram, ram_src);
        memory_region_set_enabled(&s->f8000_ram, true);
    } else {
        memory_region_set_enabled(&s->f8000_ram, false);
    }

    memory_region_transaction_commit();
}

static void mem_apply_sys16m(Pc98MemState *s)
{
    memory_region_transaction_begin();
    memory_region_set_enabled(&s->sys16m_mirror, s->sys16m);
    memory_region_set_enabled(&s->ram_f00000, !s->sys16m);
    /* the PEGC linear VRAM window lives in the 16MB system space */
    memory_region_set_enabled(s->pegc_window, s->sys16m);
    memory_region_transaction_commit();
}

/*
 * 0xd8000 window.  When the IDE selection is active and enabled, the IDE BIOS
 * option ROM appears at 0xd8000 (0x2000); its work RAM is paged in at 0xda000
 * when ide_ram_gate is set (the ITF memory test writes it).
 */
static void mem_apply_dwin(Pc98MemState *s)
{
    bool ide_rom_on = s->dwin_sel == DWIN_IDE &&
                      s->ide_rom_gate && s->ide_rom_present;
    bool ide_ram_on = ide_rom_on && s->ide_ram_gate;

    memory_region_transaction_begin();
    memory_region_set_enabled(&s->ide_rom, ide_rom_on);
    memory_region_set_enabled(&s->ide_ram, ide_ram_on);
    memory_region_transaction_commit();
}

/* fill in the BIOS work area when the writable BIOS RAM copy is paged in */
static void mem_patch_bios_workarea(Pc98MemState *s)
{
    uint8_t *ram = memory_region_get_ram_ptr(s->ram);
    uint16_t ext_mb;

    /* memory size */
    ram[0x401] = s->sys16m ? 0x70 : 0x78;
    ext_mb = (s->ram_size - 0x1000000) >> 20;
    ram[0x594] = ext_mb & 0xff;
    ram[0x595] = ext_mb >> 8;
    /* printer interface */
    ram[0x458] &= ~0x06;
    ram[0x5b3] &= ~0xe0;
    /* system clock: 5MHz -> 0x24, 8MHz -> 0xa4 */
    ram[0x501] = 0x24;

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
            ram[0x55d] |= 0x01;  /* ide drive connected */
            ram[0x5b0] = 0x00;   /* ide drive size */
        } else {
            ram[0x457] = 0x38;   /* no drive0 */
            ram[0x5b0] = 0x38;
        }
        if (s->hd_mask & 2) {
            ram[0x457] |= 0x42;  /* drive1: class 2 */
            ram[0x45d] |= 0x10;
            ram[0x55d] |= 0x02;
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
        ram[0xf8e90] |= (s->hd_mask & 0x0f);
    }
}

/* --- I/O ports --- */

static void mem_sys16m_write(void *opaque, uint32_t addr, uint32_t data)
{
    Pc98MemState *s = opaque;
    uint8_t enable = !(data & 0x04);

    if (s->sys16m != enable) {
        s->sys16m = enable;
        mem_apply_sys16m(s);
    }
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

    s->ide_rom_gate = !!(data & 0x10);
    mem_apply_dwin(s);

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
    if (read_rom_image(ROM_PCI_FILE, buf + OFF_PCI, ROM_PCI_BYTES)) {
        found |= (1 << BANK_PCI);
    }
    if (read_rom_image(ROM_IDE_FILE, buf + OFF_IDE, ROM_IDE_BYTES)) {
        found |= (1 << BANK_IDE);
    }
    if (read_rom_image(ROM_ITF_FILE, buf + OFF_ITF, ROM_ITF_BYTES)) {
        found |= (1 << BANK_ITF);
    }
    if (read_rom_image(ROM_BIOS_FILE, buf + OFF_BIOS, ROM_BIOS_BYTES)) {
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

    /* BIOS: hide the PnP BIOS signature so the guest skips PnP enumeration */
    for (i = 0x8000; i < 0x18000; i += 0x10) {
        uint8_t *p = buf + OFF_BIOS + i;
        if (p[0] == 0x24 && p[1] == 'P' && p[2] == 'n' && p[3] == 'P') {
            p[0] = 'n';
            p[2] = 0x24;
            break;
        }
    }

    s->ide_rom_present = ((found & (1 << BANK_IDE)) != 0);
    return true;
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

    s->dwin_sel = DWIN_IDE;
    s->ide_rom_gate = 1;
    s->ide_ram_gate = 1;
    mem_apply_dwin(s);

    s->bios_ram_gate = 0;
    memory_region_set_enabled(&s->e8000_ram, false);

    s->top_bank = BANK_ITF;
    mem_apply_top_bank(s, 0xf8000);
}

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
    memory_region_add_subregion_overlap(&s->lowmem, base, &w->vram_e0000, 1);
    memory_region_set_enabled(&w->vram_e0000, false);
}

Pc98MemState *pc98_mem_init(MemoryRegion *system_memory,
                            MemoryRegion *system_io,
                            MemoryRegion *ram,
                            uint64_t ram_size,
                            const Pc98VgaRegions *vga,
                            uint8_t hd_connect,
                            void (*ems_select)(void *opaque, uint32_t value),
                            void *ems_opaque)
{
    Pc98MemState *s = g_new0(Pc98MemState, 1);
    uint8_t *buf;

    s->ram = ram;
    s->ram_size = ram_size;
    s->hd_mask = hd_connect;
    s->sys16m = 1;
    s->ems_cb = ems_select;
    s->ems_cb_arg = ems_opaque;

    /* ROM blob */
    buf = g_malloc(ROM_IMAGE_BYTES);
    memset(buf, 0xff, ROM_IMAGE_BYTES);
    if (!mem_load_firmware(s, buf)) {
        error_report("could not load PC-9821 BIOS "
                     "(pc98bank*.bin or pc98itf.bin+pc98bios.bin; use -L)");
        exit(1);
    }
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

    memory_region_init_alias(&s->cbus_rom, NULL, "pc98.cbus-rom",
                             &s->rom_empty, 0, 0x18000);
    memory_region_add_subregion(&s->lowmem, 0xc0000, &s->cbus_rom);

    memory_region_init_alias(&s->d8000_rom, NULL, "pc98.d8000-rom",
                             &s->rom_empty, 0, 0x8000);
    memory_region_add_subregion(&s->lowmem, 0xd8000, &s->d8000_rom);

    /* IDE BIOS option ROM at 0xd8000 (overlays the empty window) */
    memory_region_init_alias(&s->ide_rom, NULL, "pc98.ide-rom",
                             &s->rom, OFF_IDE, 0x2000);
    memory_region_add_subregion_overlap(&s->lowmem, 0xd8000, &s->ide_rom, 1);
    memory_region_set_enabled(&s->ide_rom, false);

    /* IDE BIOS work RAM window at 0xda000 (tested by the ITF POST) */
    memory_region_init_alias(&s->ide_ram, NULL, "pc98.ide-ram",
                             ram, 0xda000, 0x2000);
    memory_region_add_subregion_overlap(&s->lowmem, 0xda000, &s->ide_ram, 2);
    memory_region_set_enabled(&s->ide_ram, false);

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

    memory_region_add_subregion(system_memory, 0, &s->lowmem);

    /* 1 MiB .. 15 MiB RAM */
    memory_region_init_alias(&s->ram_mid, NULL, "pc98.ram-mid",
                             ram, 0x100000, 0xf00000 - 0x100000);
    memory_region_add_subregion(system_memory, 0x100000, &s->ram_mid);

    /* 15 MiB .. 16 MiB: RAM when the 16MB system space is disabled */
    memory_region_init_alias(&s->ram_f00000, NULL, "pc98.ram-f00000",
                             ram, 0xf00000, 0x100000);
    memory_region_add_subregion(system_memory, 0xf00000, &s->ram_f00000);
    memory_region_set_enabled(&s->ram_f00000, false);

    /* PEGC 256-colour linear VRAM window (16MB system space only) */
    s->pegc_window = vga->vram_f00000;
    memory_region_add_subregion_overlap(system_memory, 0xf00000,
                                        s->pegc_window, 1);
    memory_region_set_enabled(s->pegc_window, s->sys16m);

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

    return s;
}
