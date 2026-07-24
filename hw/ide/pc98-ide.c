/*
 * QEMU NEC PC-9821 IDE interface
 *
 * PC-98 support
 *   Copyright (c) 2009 TAKEDA, toshiya
 *
 * PC-98 support modernisation
 *   Copyright (c) 2026 Awe Morris
 *
 * This device is derived from the PC-98 model in the QEMU/9821 fork
 * (GPL, by TAKEDA toshiya) and has been reimplemented and
 * restructured for modern QEMU.  Its register-level behaviour was
 * cross-checked against the Neko Project II and NP21W emulators.
 *
 * The PC-98 built-in IDE has two "banks" (each an ATA channel with a
 * master/slave pair) multiplexed onto one register block: port
 * 0x430/0x432 selects the active bank, and the command block lives at
 * 0x640-0x64e with a 2-byte stride, control/status at 0x74c/0x74e.
 * It reuses the shared IDE core (register handlers take an IDEBus
 * opaque) but wires the PC-98 port map itself, so the PC/AT ISA IDE
 * model stays untouched.  Both ATA hard disks and ATAPI CD-ROMs are
 * supported (the shared core provides the ATAPI PACKET machinery);
 * a CD-ROM is attached with, e.g., -drive if=ide98,media=cdrom.
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

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/ide/ide-bus.h"
#include "hw/ide/pc98-ide.h"
#include "hw/isa/isa.h"
#include "migration/vmstate.h"
#include "system/ioport.h"
#include "qom/object.h"
#include "ide-internal.h"

#define PC98_IDE_NBUS 2

struct Pc98IdeState {
    ISADevice parent_obj;

    IDEBus bus[PC98_IDE_NBUS];
    IDEBus *cur_bus;
    qemu_irq irq;
    PortioList portio_list;
};

/*
 * Port 0x430/0x432 chooses which of the two ATA channels is mapped into the
 * shared command block.  A write with bit 7 set is a no-op guard; otherwise
 * bit 0 picks the channel.  The read reports the channel currently selected.
 */
static void pc98_ide_chsel_write(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98IdeState *s = opaque;

    if (val & 0x80) {
        return;
    }
    s->cur_bus = &s->bus[val & 1];
}

static uint32_t pc98_ide_chsel_read(void *opaque, uint32_t addr)
{
    Pc98IdeState *s = opaque;

    return s->cur_bus == &s->bus[1];
}

/*
 * The PC-98 built-in IDE BIOS polls the status register for exactly
 * READY_STAT | SEEK_STAT (0x50) after commands such as IDLE IMMEDIATE
 * (0xe1).  A real IDE drive keeps DSC (SEEK_STAT) asserted whenever it is
 * not mid-seek, but the shared IDE core leaves it clear for several
 * "no-op" commands and returns 0x40, which makes the BIOS spin until it
 * times out.  Re-assert DSC on reads whenever the drive is ready and idle;
 * this turns 0x40 into 0x50 while leaving DRQ/BSY/error states untouched.
 * Kept here so the shared IDE core is not modified.
 *
 * This is only correct for ATA hard disks.  On an ATAPI (CD-ROM) device the
 * DSC bit is command-specific, so the fixup is restricted to a real IDE_HD on
 * the active unit; ATAPI status is passed through unchanged.
 */
static inline uint32_t pc98_ide_fixup_status(IDEBus *bus, uint32_t st)
{
    IDEState *active = &bus->ifs[bus->unit];

    if (active->blk && active->drive_kind == IDE_HD &&
        (st & (BUSY_STAT | READY_STAT)) == READY_STAT) {
        st |= SEEK_STAT;
    }
    return st;
}

/*
 * A bank with no drives must read as a floating bus (0xff), not 0x00 as
 * the shared IDE core returns.  The ITF probes each bank by watching the
 * DRQ bit: 0xff (DRQ stuck high) marks the bank as absent (drive class 7
 * in work-area 0x457), while 0x00 (DRQ clear) would make it register a
 * phantom second drive, and the IDE BIOS then hangs at POST waiting for
 * an interrupt from it.
 */
static inline bool pc98_ide_bus_empty(IDEBus *bus)
{
    return !bus->ifs[0].blk && !bus->ifs[1].blk;
}

/* command block: 0x640 + reg*2 -> IDE register 'reg' on the current bank */
static void pc98_ide_cmd_write(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98IdeState *s = opaque;

    ide_ioport_write(s->cur_bus, (addr - 0x640) >> 1, val);
}

static uint32_t pc98_ide_cmd_read(void *opaque, uint32_t addr)
{
    Pc98IdeState *s = opaque;
    unsigned reg = (addr - 0x640) >> 1;
    uint32_t ret;

    if (pc98_ide_bus_empty(s->cur_bus)) {
        return 0xff;
    }
    ret = ide_ioport_read(s->cur_bus, reg);
    if (reg == 7) {
        ret = pc98_ide_fixup_status(s->cur_bus, ret);
    }
    return ret;
}

static void pc98_ide_data_writew(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98IdeState *s = opaque;

    ide_data_writew(s->cur_bus, 0, val);
}

static uint32_t pc98_ide_data_readw(void *opaque, uint32_t addr)
{
    Pc98IdeState *s = opaque;

    return ide_data_readw(s->cur_bus, 0);
}

static void pc98_ide_data_writel(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98IdeState *s = opaque;

    ide_data_writel(s->cur_bus, 0, val);
}

static uint32_t pc98_ide_data_readl(void *opaque, uint32_t addr)
{
    Pc98IdeState *s = opaque;

    return ide_data_readl(s->cur_bus, 0);
}

/* control/alt-status at 0x74c, drive-address register at 0x74e */
static void pc98_ide_ctrl_write(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98IdeState *s = opaque;

    ide_ctrl_write(s->cur_bus, 0, val);
}

static uint32_t pc98_ide_status_read(void *opaque, uint32_t addr)
{
    Pc98IdeState *s = opaque;

    if (pc98_ide_bus_empty(s->cur_bus)) {
        return 0xff;
    }
    return pc98_ide_fixup_status(s->cur_bus, ide_status_read(s->cur_bus, 0));
}

/*
 * Drive-address register: the two high bits float high, the head-select
 * nibble appears inverted in bits 5..2, and the low two bits flag which
 * device of the pair is active (bit 1 master, bit 0 slave).
 */
static uint32_t pc98_ide_drive_addr_read(void *opaque, uint32_t addr)
{
    Pc98IdeState *s = opaque;
    IDEBus *bus = s->cur_bus;
    IDEState *active = &bus->ifs[bus->unit];
    uint32_t value = 0xc0;

    value |= (~active->select & 0x0f) << 2;
    value |= bus->unit ? 0x01 : 0x02;
    return value;
}

static const MemoryRegionPortio pc98_ide_portio[] = {
    { 0x430, 1, 1, .read = pc98_ide_chsel_read, .write = pc98_ide_chsel_write },
    { 0x432, 1, 1, .read = pc98_ide_chsel_read, .write = pc98_ide_chsel_write },
    { 0x640, 8, 2, .read = pc98_ide_data_readw, .write = pc98_ide_data_writew },
    { 0x640, 1, 4, .read = pc98_ide_data_readl, .write = pc98_ide_data_writel },
    { 0x640, 1, 1, .read = pc98_ide_cmd_read, .write = pc98_ide_cmd_write },
    { 0x642, 1, 1, .read = pc98_ide_cmd_read, .write = pc98_ide_cmd_write },
    { 0x644, 1, 1, .read = pc98_ide_cmd_read, .write = pc98_ide_cmd_write },
    { 0x646, 1, 1, .read = pc98_ide_cmd_read, .write = pc98_ide_cmd_write },
    { 0x648, 1, 1, .read = pc98_ide_cmd_read, .write = pc98_ide_cmd_write },
    { 0x64a, 1, 1, .read = pc98_ide_cmd_read, .write = pc98_ide_cmd_write },
    { 0x64c, 1, 1, .read = pc98_ide_cmd_read, .write = pc98_ide_cmd_write },
    { 0x64e, 1, 1, .read = pc98_ide_cmd_read, .write = pc98_ide_cmd_write },
    { 0x74c, 1, 1, .read = pc98_ide_status_read, .write = pc98_ide_ctrl_write },
    { 0x74e, 1, 1, .read = pc98_ide_drive_addr_read },
    PORTIO_END_OF_LIST(),
};

uint8_t pc98_ide_connected(Pc98IdeState *s)
{
    uint8_t ret = 0;
    int b, u;

    for (b = 0; b < PC98_IDE_NBUS; b++) {
        for (u = 0; u < 2; u++) {
            IDEState *ide = &s->bus[b].ifs[u];
            if (ide->blk && ide->drive_kind == IDE_HD) {
                ret |= 1 << (b * 2 + u);
            }
        }
    }
    return ret;
}

static void pc98_ide_reset(DeviceState *dev)
{
    Pc98IdeState *s = PC98_IDE(dev);
    int b;

    for (b = 0; b < PC98_IDE_NBUS; b++) {
        ide_bus_reset(&s->bus[b]);
    }
    s->cur_bus = &s->bus[0];
}

static const VMStateDescription vmstate_pc98_ide = {
    .name = "pc98-ide",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_IDE_BUS(bus[0], Pc98IdeState),
        VMSTATE_IDE_DRIVES(bus[0].ifs, Pc98IdeState),
        VMSTATE_IDE_BUS(bus[1], Pc98IdeState),
        VMSTATE_IDE_DRIVES(bus[1].ifs, Pc98IdeState),
        VMSTATE_END_OF_LIST()
    }
};

static void pc98_ide_realize(DeviceState *dev, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(dev);
    Pc98IdeState *s = PC98_IDE(dev);
    int b;

    for (b = 0; b < PC98_IDE_NBUS; b++) {
        ide_bus_init(&s->bus[b], sizeof(s->bus[b]), dev, b, 2);
        ide_bus_init_output_irq(&s->bus[b], s->irq);
        ide_bus_register_restart_cb(&s->bus[b]);
    }
    s->cur_bus = &s->bus[0];

    isa_register_portio_list(isadev, &s->portio_list, 0,
                             pc98_ide_portio, s, "pc98-ide");
}

static void pc98_ide_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pc98_ide_realize;
    device_class_set_legacy_reset(dc, pc98_ide_reset);
    dc->vmsd = &vmstate_pc98_ide;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    dc->user_creatable = false;
}

static const TypeInfo pc98_ide_info = {
    .name          = TYPE_PC98_IDE,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(Pc98IdeState),
    .class_init    = pc98_ide_class_init,
};

static void pc98_ide_register_types(void)
{
    type_register_static(&pc98_ide_info);
}

type_init(pc98_ide_register_types)

/*
 * Impose the PC-98 IDE logical geometry on a drive.  The PC-98 IDE BIOS
 * addresses the disk with a fixed "class 2" profile (see ram[0x457] in
 * pc98-mem.c): 8 heads, 17 sectors per track, and a cylinder count it
 * derives from the IDENTIFY capacity as total / (8 * 17).  Program the same
 * shape into the drive so IDENTIFY reports it and C/H/S reads land on the
 * LBAs the BIOS intends.  QEMU's generic geometry auto-guess must not be
 * used -- it picks a different layout and every partition access misses.
 * Disk images therefore must be partitioned for 8-head geometry (as the
 * real BIOS's own FORMAT would).
 */
#define PC98_IDE_HEADS    8
#define PC98_IDE_SECTORS  17

static void pc98_ide_set_geometry(IDEState *ide)
{
    int cyls;

    /* ATAPI (CD-ROM) devices have no fixed C/H/S geometry; leave them alone. */
    if (!ide->blk || ide->drive_kind != IDE_HD || ide->nb_sectors == 0) {
        return;
    }

    cyls = ide->nb_sectors / (PC98_IDE_HEADS * PC98_IDE_SECTORS);
    if (cyls < 1) {
        cyls = 1;
    } else if (cyls > 65535) {
        cyls = 65535;
    }

    ide->cylinders = cyls;
    ide->heads = ide->drive_heads = PC98_IDE_HEADS;
    ide->sectors = ide->drive_sectors = PC98_IDE_SECTORS;
    ide->identify_set = 0;   /* rebuild IDENTIFY data with the new geometry */
}

ISADevice *pc98_ide_init(ISABus *bus, DriveInfo **hd_table, qemu_irq irq)
{
    DeviceState *dev;
    ISADevice *isadev;
    Pc98IdeState *s;
    int i;

    isadev = isa_new(TYPE_PC98_IDE);
    dev = DEVICE(isadev);
    s = PC98_IDE(dev);
    s->irq = irq;
    isa_realize_and_unref(isadev, bus, &error_fatal);

    /* bank1 -> drives 0,1 ; bank2 -> drives 2,3 */
    for (i = 0; i < 4; i++) {
        if (hd_table[i]) {
            ide_bus_create_drive(&s->bus[i / 2], i % 2, hd_table[i]);
        }
    }
    for (i = 0; i < PC98_IDE_NBUS; i++) {
        ide_bus_reset(&s->bus[i]);
    }
    /* Override geometry after reset so ide_reset() does not clobber it. */
    for (i = 0; i < 4; i++) {
        if (hd_table[i]) {
            pc98_ide_set_geometry(&s->bus[i / 2].ifs[i % 2]);
        }
    }

    return isadev;
}
