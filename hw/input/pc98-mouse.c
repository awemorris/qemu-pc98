/*
 * QEMU NEC PC-98 bus mouse (uPD8255 PPI at 0x7FD9-0x7FDF, IRQ13)
 *
 * Copyright (c) 2026 Awe Morris
 *
 * The NEC PC-98 "bus mouse" is a uPD8255 programmable peripheral interface
 * (PPI) wired to a quadrature mouse.  It occupies four odd I/O ports:
 *
 *   0x7FD9  Port A  read : the selected 4-bit nibble of the latched X or Y
 *                          motion counter in the low nibble, with the mouse
 *                          button state in the upper nibble
 *   0x7FDB  Port B  read : status (reads 0x40 while configured as input)
 *   0x7FDD  Port C  r/w  : the control/latch bits (see below)
 *   0x7FDF          write: the 8255 control word -- a mode set when bit 7 is
 *                          1, otherwise a bit set/reset (BSR) of one Port C
 *                          bit: index = (val >> 1) & 7, value = val & 1
 *
 * The driver reads a motion counter one nibble at a time.  The upper half of
 * Port C is an output latch that steers the read and captures the counters:
 *
 *   bit 0x80 (HC)  a 0->1 edge latches the running X/Y counters (each clamped
 *                  to signed 8 bits) and clears them, so the driver captures a
 *                  stable snapshot before reading it out
 *   bit 0x40 (SXY) selects the counter read through Port A: 0 = X, 1 = Y
 *   bit 0x20 (SHL) selects the nibble read through Port A: 0 = low, 1 = high
 *   bit 0x10 (INT) interrupt enable, active low: 0 enables the periodic IRQ13,
 *                  1 masks it
 *
 * While the interrupt is enabled the card raises IRQ13 at a fixed rate
 * (~120 Hz here; a real card scales this by a programmable timing field --
 * left as a TODO).  The button bits are active low: Port A bit 0x80 is the
 * left button and bit 0x20 the right button (both 1 = released); bit 0x40 is
 * held high.
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
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/input/pc98-mouse.h"
#include "hw/isa/isa.h"
#include "migration/vmstate.h"
#include "system/ioport.h"
#include "qom/object.h"
#include "ui/input.h"

/* uPD8255 mode-word bits (as latched from a 0x7FDF control write). */
#define PPI_MODE_PORTCL   0x01
#define PPI_MODE_PORTB    0x02
#define PPI_MODE_PORTCH   0x08
#define PPI_MODE_PORTA    0x10
#define PPI_MODE_SET      0x80

/* Port C control bits (upper nibble, driven by the driver via BSR writes). */
#define PORTC_HC          0x80   /* 0->1 edge latches the motion counters   */
#define PORTC_SXY         0x40   /* counter select: 0 = X, 1 = Y            */
#define PORTC_SHL         0x20   /* nibble select: 0 = low, 1 = high        */
#define PORTC_INT_MASK    0x10   /* active low: 0 = IRQ13 enabled           */

/* Port A button bits (active low: 0 = pressed). */
#define MOUSE_BTN_LEFT    0x80
#define MOUSE_BTN_RIGHT   0x20

/* Periodic mouse interrupt rate.  TODO: honour the selectable timing field. */
#define PC98_MOUSE_RATE_HZ      120
#define PC98_MOUSE_INTERVAL_NS  (NANOSECONDS_PER_SECOND / PC98_MOUSE_RATE_HZ)

/* Bound the running counters so they cannot overflow before the next latch. */
#define PC98_MOUSE_COUNTER_LIMIT  2048

OBJECT_DECLARE_SIMPLE_TYPE(Pc98MouseState, PC98_MOUSE)

struct Pc98MouseState {
    ISADevice parent_obj;

    /* uPD8255 PPI registers */
    uint8_t porta;
    uint8_t portb;
    uint8_t portc;
    uint8_t mode;

    /* running motion counters accumulated since the last HC latch */
    int32_t counter_x;
    int32_t counter_y;
    /* latched counters (clamped to signed 8 bits), read out nibble by nibble */
    int8_t latch_x;
    int8_t latch_y;
    /* button state, active low (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT bits) */
    uint8_t buttons;

    QEMUTimer *irq_timer;
    QemuInputHandlerState *input_handler;
    PortioList portio_list;
    qemu_irq irq;
};

/* --- host input --- */

static int pc98_mouse_clamp(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/*
 * Accumulate relative motion and track button state.  Latching is *not* done
 * here -- it happens on the Port C HC 0->1 edge (pc98_mouse_set_portc).  The
 * PC-98 counters use the same axis sense as QEMU relative events (X right
 * positive, Y down positive).
 */
static void pc98_mouse_event(DeviceState *dev, QemuConsole *src,
                             QemuInputEvent *evt)
{
    Pc98MouseState *s = PC98_MOUSE(dev);

    switch (evt->type) {
    case INPUT_EVENT_KIND_REL:
        if (evt->rel.axis == INPUT_AXIS_X) {
            s->counter_x = pc98_mouse_clamp(s->counter_x + evt->rel.value,
                                            -PC98_MOUSE_COUNTER_LIMIT,
                                            PC98_MOUSE_COUNTER_LIMIT);
        } else if (evt->rel.axis == INPUT_AXIS_Y) {
            s->counter_y = pc98_mouse_clamp(s->counter_y + evt->rel.value,
                                            -PC98_MOUSE_COUNTER_LIMIT,
                                            PC98_MOUSE_COUNTER_LIMIT);
        }
        break;

    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            if (evt->btn.down) {
                s->buttons &= ~MOUSE_BTN_LEFT;
            } else {
                s->buttons |= MOUSE_BTN_LEFT;
            }
        } else if (evt->btn.button == INPUT_BUTTON_RIGHT) {
            if (evt->btn.down) {
                s->buttons &= ~MOUSE_BTN_RIGHT;
            } else {
                s->buttons |= MOUSE_BTN_RIGHT;
            }
        }
        break;

    default:
        break;
    }
}

static const QemuInputHandler pc98_mouse_handler = {
    .name  = "PC-98 bus mouse",
    .mask  = INPUT_EVENT_MASK_REL | INPUT_EVENT_MASK_BTN,
    .event = pc98_mouse_event,
};

/* --- periodic IRQ13 --- */

static void pc98_mouse_tick(void *opaque)
{
    Pc98MouseState *s = opaque;

    if (s->portc & PORTC_INT_MASK) {
        return;                         /* masked while we were scheduled */
    }
    /* Pulse the edge-triggered IRQ13 line and re-arm for the next tick. */
    qemu_irq_raise(s->irq);
    qemu_irq_lower(s->irq);
    timer_mod(s->irq_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PC98_MOUSE_INTERVAL_NS);
}

/* --- Port C control latch --- */

static void pc98_mouse_set_portc(Pc98MouseState *s, uint8_t value)
{
    /* HC 0->1 edge: snapshot and clear the running counters. */
    if ((value & PORTC_HC) && !(s->portc & PORTC_HC)) {
        s->latch_x = pc98_mouse_clamp(s->counter_x, -128, 127);
        s->latch_y = pc98_mouse_clamp(s->counter_y, -128, 127);
        s->counter_x = 0;
        s->counter_y = 0;
    }
    /* INT enable transition: (re)arm or stop the periodic IRQ13 timer. */
    if ((value ^ s->portc) & PORTC_INT_MASK) {
        if (!(value & PORTC_INT_MASK)) {
            timer_mod(s->irq_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                    PC98_MOUSE_INTERVAL_NS);
        } else {
            timer_del(s->irq_timer);
            qemu_irq_lower(s->irq);
        }
    }
    s->portc = value;
}

/* --- I/O ports --- */

/*
 * Port A read: when configured as an input, return the button state in the
 * upper nibble (active low, bit 0x40 held high) and the selected nibble of the
 * selected motion counter in the low nibble.  HC selects the latched snapshot
 * versus the live counter, SXY selects X versus Y, SHL selects low versus high
 * nibble.
 */
static uint32_t pc98_mouse_porta_read(void *opaque, uint32_t addr)
{
    Pc98MouseState *s = opaque;
    int value;
    uint8_t ret;

    if (!(s->mode & PPI_MODE_PORTA)) {
        return s->porta;
    }

    ret = (s->buttons & 0xf0) | 0x40;

    if (s->portc & PORTC_HC) {
        value = (s->portc & PORTC_SXY) ? s->latch_y : s->latch_x;
    } else {
        value = (s->portc & PORTC_SXY) ? s->counter_y : s->counter_x;
    }

    if (s->portc & PORTC_SHL) {
        ret |= (value >> 4) & 0x0f;
    } else {
        ret |= value & 0x0f;
    }
    return ret;
}

static void pc98_mouse_porta_write(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98MouseState *s = opaque;

    s->porta = val;
}

static uint32_t pc98_mouse_portb_read(void *opaque, uint32_t addr)
{
    Pc98MouseState *s = opaque;

    if (s->mode & PPI_MODE_PORTB) {
        return 0x40;
    }
    return s->portb;
}

static void pc98_mouse_portb_write(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98MouseState *s = opaque;

    s->portb = val;
}

/*
 * Port C read: the upper nibble reflects the driver's own control latch; the
 * lower nibble, when configured as an input, reports DIP-switch straps (fixed
 * here) plus a constant 0x08 the BIOS expects.
 */
static uint32_t pc98_mouse_portc_read(void *opaque, uint32_t addr)
{
    Pc98MouseState *s = opaque;
    uint8_t ret = s->portc;

    if (s->mode & PPI_MODE_PORTCH) {
        ret &= 0x1f;
    }
    if (s->mode & PPI_MODE_PORTCL) {
        ret &= 0xf0;
        ret |= 0x08;
        /* TODO: expose the real DIP-switch straps in the low bits. */
    }
    return ret;
}

static void pc98_mouse_portc_write(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98MouseState *s = opaque;

    pc98_mouse_set_portc(s, val);
}

/*
 * 0x7FDF control-word write: bit 7 set is a mode write (which, per the 8255,
 * also resets the Port C output latch to 0); bit 7 clear is a bit set/reset of
 * a single Port C bit.
 */
static void pc98_mouse_ctrl_write(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98MouseState *s = opaque;

    if (val & PPI_MODE_SET) {
        s->mode = val;
        pc98_mouse_set_portc(s, 0);
    } else {
        unsigned bit = (val >> 1) & 7;
        uint8_t portc = s->portc;

        portc &= ~(1u << bit);
        portc |= (val & 1) << bit;
        pc98_mouse_set_portc(s, portc);
    }
}

/* The 8255 control word is write-only; a read floats high. */
static uint32_t pc98_mouse_ctrl_read(void *opaque, uint32_t addr)
{
    return 0xff;
}

static const MemoryRegionPortio pc98_mouse_portio[] = {
    { 0x7fd9, 1, 1,
      .read = pc98_mouse_porta_read, .write = pc98_mouse_porta_write },
    { 0x7fdb, 1, 1,
      .read = pc98_mouse_portb_read, .write = pc98_mouse_portb_write },
    { 0x7fdd, 1, 1,
      .read = pc98_mouse_portc_read, .write = pc98_mouse_portc_write },
    { 0x7fdf, 1, 1,
      .read = pc98_mouse_ctrl_read, .write = pc98_mouse_ctrl_write },
    PORTIO_END_OF_LIST(),
};

/* --- QOM device --- */

static void pc98_mouse_reset(DeviceState *dev)
{
    Pc98MouseState *s = PC98_MOUSE(dev);

    /* Power-on state matches NP21W: all control bits high (INT masked). */
    s->porta = 0x00;
    s->portb = 0x00;
    s->portc = 0xf0;
    s->mode = 0x93;
    s->counter_x = 0;
    s->counter_y = 0;
    s->latch_x = -1;
    s->latch_y = -1;
    s->buttons = MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT;

    timer_del(s->irq_timer);
    qemu_irq_lower(s->irq);
}

static const VMStateDescription vmstate_pc98_mouse = {
    .name = "pc98-mouse",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(porta, Pc98MouseState),
        VMSTATE_UINT8(portb, Pc98MouseState),
        VMSTATE_UINT8(portc, Pc98MouseState),
        VMSTATE_UINT8(mode, Pc98MouseState),
        VMSTATE_INT32(counter_x, Pc98MouseState),
        VMSTATE_INT32(counter_y, Pc98MouseState),
        VMSTATE_INT8(latch_x, Pc98MouseState),
        VMSTATE_INT8(latch_y, Pc98MouseState),
        VMSTATE_UINT8(buttons, Pc98MouseState),
        VMSTATE_TIMER_PTR(irq_timer, Pc98MouseState),
        VMSTATE_END_OF_LIST()
    }
};

static void pc98_mouse_realize(DeviceState *dev, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(dev);
    Pc98MouseState *s = PC98_MOUSE(dev);

    isa_register_portio_list(isadev, &s->portio_list, 0,
                             pc98_mouse_portio, s, "pc98-mouse");
    qdev_init_gpio_out(dev, &s->irq, 1);
    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pc98_mouse_tick, s);
    s->input_handler = qemu_input_handler_register(dev, &pc98_mouse_handler);
}

static void pc98_mouse_unrealize(DeviceState *dev)
{
    Pc98MouseState *s = PC98_MOUSE(dev);

    g_clear_pointer(&s->input_handler, qemu_input_handler_unregister);
    timer_free(s->irq_timer);
    s->irq_timer = NULL;
}

static void pc98_mouse_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pc98_mouse_realize;
    dc->unrealize = pc98_mouse_unrealize;
    device_class_set_legacy_reset(dc, pc98_mouse_reset);
    dc->vmsd = &vmstate_pc98_mouse;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    /* Wired by board code (IRQ13), not user creatable. */
    dc->user_creatable = false;
}

static const TypeInfo pc98_mouse_info = {
    .name          = TYPE_PC98_MOUSE,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(Pc98MouseState),
    .class_init    = pc98_mouse_class_init,
};

static void pc98_mouse_register_types(void)
{
    type_register_static(&pc98_mouse_info);
}

type_init(pc98_mouse_register_types)

ISADevice *pc98_mouse_init(ISABus *bus, qemu_irq irq)
{
    ISADevice *isadev;

    isadev = isa_new(TYPE_PC98_MOUSE);
    isa_realize_and_unref(isadev, bus, &error_fatal);
    qdev_connect_gpio_out(DEVICE(isadev), 0, irq);

    return isadev;
}
