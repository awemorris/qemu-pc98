/*
 * QEMU NEC PC-9821 8259A interrupt controller wiring
 *
 * PC-98 support
 *   Copyright (c) 2009 TAKEDA, toshiya
 *
 * PC-98 support modernisation
 *   Copyright (c) 2026 Awe Morris
 *
 * The PC-98 uses two 8259A chips like the AT, but with a different
 * I/O layout (0x00/0x02 and 0x08/0x0a, 2-byte register stride), the
 * slave cascaded into master input 7 instead of 2, and no edge/level
 * control register.  Only the wiring differs, so this reuses the
 * shared TYPE_I8259 device (with its "cascade-irq" property) rather
 * than duplicating the chip model; the INTA cascade decode itself
 * lives in the shared i8259.c because the CPU acknowledge path
 * (cpu_get_pic_interrupt -> pic_read_irq(isa_pic)) is common to all
 * x86 machines.
 *
 * This wiring is derived from the PC-98 model in the qemu/9821 fork
 * (GPL, by TAKEDA toshiya) and has been reimplemented for modern
 * QEMU.  Its register-level behaviour was cross-checked against the
 * Neko Project II and NP21W emulators.
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
#include "hw/core/qdev-properties.h"
#include "hw/intc/i8259.h"
#include "hw/intc/i8259-pc98.h"
#include "hw/isa/i8259_internal.h"
#include "hw/isa/isa.h"
#include "system/memory.h"

/* Map the 2-byte command/data register block at a 2-byte stride. */
static void pc98_pic_map(ISADevice *isadev, int base)
{
    PICCommonState *s = PIC_COMMON(isadev);
    MemoryRegion *alias = g_new(MemoryRegion, 2);

    memory_region_init_alias(&alias[0], OBJECT(s), "pc98-pic-cmd",
                             &s->base_io, 0, 1);
    memory_region_init_alias(&alias[1], OBJECT(s), "pc98-pic-data",
                             &s->base_io, 1, 1);
    isa_register_ioport(isadev, &alias[0], base);
    isa_register_ioport(isadev, &alias[1], base + 2);
}

qemu_irq *pc98_pic_setup(ISABus *bus, qemu_irq parent_irq_in)
{
    qemu_irq *irq_set;
    DeviceState *dev;
    ISADevice *isadev;
    int i;

    irq_set = g_new0(qemu_irq, ISA_NUM_IRQS);

    /* master: cascade on IRQ7, ports mapped by hand (iobase = -1) */
    isadev = isa_new(TYPE_I8259);
    dev = DEVICE(isadev);
    qdev_prop_set_uint8(dev, "cascade-irq", 7);
    qdev_prop_set_bit(dev, "master", true);
    isa_realize_and_unref(isadev, bus, &error_fatal);
    pc98_pic_map(isadev, 0x00);

    qdev_connect_gpio_out(dev, 0, parent_irq_in);
    for (i = 0; i < 8; i++) {
        irq_set[i] = qdev_get_gpio_in(dev, i);
    }
    isa_pic = PIC_COMMON(dev);

    /* slave: output feeds master input 7 */
    isadev = isa_new(TYPE_I8259);
    dev = DEVICE(isadev);
    isa_realize_and_unref(isadev, bus, &error_fatal);
    pc98_pic_map(isadev, 0x08);

    qdev_connect_gpio_out(dev, 0, irq_set[7]);
    for (i = 0; i < 8; i++) {
        irq_set[i + 8] = qdev_get_gpio_in(dev, i);
    }
    slave_pic = PIC_COMMON(dev);

    return irq_set;
}
