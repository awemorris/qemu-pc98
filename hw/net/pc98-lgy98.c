/*
 * QEMU NEC PC-98 LGY-98 C-bus Ethernet
 *
 * Copyright (c) 2026 Awe Morris
 *
 * The LGY-98 (Melco/Buffalo) is an NE2000-compatible (DP8390) C-bus
 * Ethernet card for the NEC PC-98 series.  Its register interface is the
 * same DP8390 + NE2000 ASIC found on PC/AT NE2000 clones, but the ports
 * are relocated into the PC-98 C-bus I/O map.  With base 0x10D0:
 *
 *   0x10D0-0x10DF (base+0x00..0x0f)  DP8390 register block (1-byte access)
 *   0x12D0        (base+0x200)       NE2000 ASIC data port (remote DMA,
 *                                    8- and 16-bit access)
 *   0x10E8        (base+0x18)        a read pulses the card reset
 *   0x13D0-0x13DF (base+0x300)       LGY-98 board-ID / "knock" ports
 *
 * This device reuses QEMU's shared NE2000 core (hw/net/ne2000.c)
 * unchanged: it builds the core's 0x20-byte I/O region with
 * ne2000_setup_io() -- whose internal decode is offset 0x00-0x0f =
 * DP8390 registers, 0x10 = ASIC data port, 0x1f = reset -- and then
 * aliases the relevant sub-ranges of that region into the PC-98 ports
 * above (the same "shared core + PC-98 port-mapping wrapper" scheme
 * used by hw/ide/pc98-ide.c and hw/display/pc98-wab.c), so the shared
 * NE2000 model stays untouched.  The board-ID ports at base+0x300 are
 * not part of the NE2000 core; they are stubbed here (reads return
 * 0xff, writes ignored), which is enough for the NE2000
 * register-level probe used by common drivers.
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
#include "hw/isa/isa.h"
#include "hw/net/pc98-lgy98.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "system/address-spaces.h"
#include "system/ioport.h"
#include "system/memory.h"
#include "qom/object.h"
#include "ne2000.h"

/* LGY-98 C-bus base address (fixed for the built-in card). */
#define LGY98_IOBASE        0x10d0
#define LGY98_REG_OFFSET    0x000   /* DP8390 register block   -> core 0x00 */
#define LGY98_ASIC_OFFSET   0x200   /* NE2000 ASIC data port   -> core 0x10 */
#define LGY98_RESET_OFFSET  0x018   /* reset pulse (read)      -> core 0x1f */
#define LGY98_BOARD_OFFSET  0x300   /* LGY-98 board-ID ports (stub)         */

OBJECT_DECLARE_SIMPLE_TYPE(Pc98Lgy98State, PC98_LGY98)

struct Pc98Lgy98State {
    ISADevice parent_obj;

    NE2000State ne2000;
    qemu_irq irq;

    MemoryRegion reg_alias;     /* 0x10D0-0x10DF -> core io 0x00 (16 bytes) */
    MemoryRegion asic_alias;    /* 0x12D0        -> core io 0x10 (2 bytes)  */
    MemoryRegion reset_alias;   /* 0x10E8        -> core io 0x1f (1 byte)   */
    PortioList board_portio;    /* 0x13D0-0x13DF board-ID stub              */
};

/*
 * LGY-98 board-ID / "knock" ports at base+0x300.  The real card
 * exposes a small identification sequence here; the NE2000
 * register-level probe does not need it, so reads float high (0xff)
 * and writes are ignored.  Implement the real sequence here only if a
 * guest's detection depends on it.
 */
static uint32_t lgy98_board_read(void *opaque, uint32_t addr)
{
    return 0xff;
}

static void lgy98_board_write(void *opaque, uint32_t addr, uint32_t val)
{
    /* nothing to do */
}

static const MemoryRegionPortio lgy98_board_portio[] = {
    { LGY98_IOBASE + LGY98_BOARD_OFFSET, 16, 1,
      .read = lgy98_board_read, .write = lgy98_board_write },
    PORTIO_END_OF_LIST(),
};

static NetClientInfo net_pc98_lgy98_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = ne2000_receive,
};

static const VMStateDescription vmstate_pc98_lgy98 = {
    .name = "pc98-lgy98",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(ne2000, Pc98Lgy98State, 0, vmstate_ne2000, NE2000State),
        VMSTATE_END_OF_LIST()
    }
};

static void pc98_lgy98_realize(DeviceState *dev, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(dev);
    Pc98Lgy98State *s = PC98_LGY98(dev);
    NE2000State *n = &s->ne2000;

    /* Build the shared NE2000 core I/O region (0x20 bytes, internal decode). */
    ne2000_setup_io(n, dev, 0x20);

    /*
     * Alias sub-ranges of the core region into the PC-98 port map instead of
     * re-decoding it, so the shared NE2000 model is used verbatim.
     */
    memory_region_init_alias(&s->reg_alias, OBJECT(dev), "pc98-lgy98-reg",
                             &n->io, 0x00, 0x10);
    memory_region_add_subregion(get_system_io(),
                                LGY98_IOBASE + LGY98_REG_OFFSET, &s->reg_alias);

    /* size 2 so both byte and word accesses reach the ASIC data port. */
    memory_region_init_alias(&s->asic_alias, OBJECT(dev), "pc98-lgy98-asic",
                             &n->io, 0x10, 2);
    memory_region_add_subregion(get_system_io(),
                                LGY98_IOBASE + LGY98_ASIC_OFFSET,
                                &s->asic_alias);

    /* a read at base+0x18 hits core offset 0x1f, which resets the chip. */
    memory_region_init_alias(&s->reset_alias, OBJECT(dev), "pc98-lgy98-reset",
                             &n->io, 0x1f, 1);
    memory_region_add_subregion(get_system_io(),
                                LGY98_IOBASE + LGY98_RESET_OFFSET,
                                &s->reset_alias);

    isa_register_portio_list(isadev, &s->board_portio, 0,
                             lgy98_board_portio, s, "pc98-lgy98-board");

    n->irq = s->irq;

    qemu_macaddr_default_if_unset(&n->c.macaddr);
    ne2000_reset(n);

    n->nic = qemu_new_nic(&net_pc98_lgy98_info, &n->c,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, n);
    qemu_format_nic_info_str(qemu_get_queue(n->nic), n->c.macaddr.a);
}

static void pc98_lgy98_reset(DeviceState *dev)
{
    Pc98Lgy98State *s = PC98_LGY98(dev);

    ne2000_reset(&s->ne2000);
}

static const Property pc98_lgy98_properties[] = {
    DEFINE_NIC_PROPERTIES(Pc98Lgy98State, ne2000.c),
};

static void pc98_lgy98_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pc98_lgy98_realize;
    device_class_set_legacy_reset(dc, pc98_lgy98_reset);
    device_class_set_props(dc, pc98_lgy98_properties);
    dc->vmsd = &vmstate_pc98_lgy98;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->user_creatable = false;
}

static const TypeInfo pc98_lgy98_info = {
    .name          = TYPE_PC98_LGY98,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(Pc98Lgy98State),
    .class_init    = pc98_lgy98_class_init,
};

static void pc98_lgy98_register_types(void)
{
    type_register_static(&pc98_lgy98_info);
}

type_init(pc98_lgy98_register_types)

ISADevice *pc98_lgy98_init(ISABus *bus, qemu_irq irq)
{
    ISADevice *isadev;
    Pc98Lgy98State *s;

    isadev = isa_new(TYPE_PC98_LGY98);
    s = PC98_LGY98(isadev);
    s->irq = irq;
    isa_realize_and_unref(isadev, bus, &error_fatal);

    return isadev;
}
