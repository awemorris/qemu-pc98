/*
 * QEMU NEC PC-9821 8253/8254 interval timer
 *
 * Self-contained variant of hw/timer/i8254.c for the PC-98: the
 * counter input clock is 2.4576 MHz (5 MHz system-clock series)
 * instead of the AT's 1.193182 MHz, and the three counters are wired
 * to odd I/O ports with a 2-byte stride (0x71/0x73/0x75/0x77,
 * mirrored at 0x3fd9..0x3fdf).
 *
 * Kept as a full copy rather than adding switches to the shared AT
 * model, so the PC/AT timer stays untouched and this file is easy to
 * rebase.
 *
 * Based on hw/timer/i8254.c:
 *   Copyright (c) 2003-2004 Fabrice Bellard
 *
 * PC-98 support
 *   Copyright (c) 2009 TAKEDA, toshiya
 *
 * PC-98 support modernisation
 *   Copyright (c) 2026 Awe Morris
 *
 * The PC-98 register-level behaviour is derived from the QEMU/9821 fork
 * (GPL, by TAKEDA toshiya) and was cross-checked against the Neko Project
 * II and NP21W emulators.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/isa/isa.h"
#include "hw/timer/i8254.h"
#include "hw/timer/i8254-pc98.h"
#include "hw/timer/i8254_internal.h"
#include "qom/object.h"

/*
 * PC-98 counter input clock.  The 5 MHz system-clock series divides the
 * 2.4576 MHz oscillator; the 8 MHz series would use 1.9968 MHz.  We model
 * the common 5 MHz timing.
 */
#define PC98_PIT_FREQ 2457600

#define RW_STATE_LSB 1
#define RW_STATE_MSB 2
#define RW_STATE_WORD0 3
#define RW_STATE_WORD1 4

struct Pc98PitState {
    PITCommonState parent_obj;
};

#define TYPE_PC98_PIT "pc98-pit"
OBJECT_DECLARE_SIMPLE_TYPE(Pc98PitState, PC98_PIT)

typedef struct Pc98PitClass {
    PITCommonClass parent_class;
    DeviceRealize parent_realize;
} Pc98PitClass;

DECLARE_CLASS_CHECKERS(Pc98PitClass, PC98_PIT, TYPE_PC98_PIT)

static void pit_irq_timer_update(PITChannelState *s, int64_t current_time);

static int pit_get_count(PITChannelState *s)
{
    uint64_t d;
    int counter;

    d = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->count_load_time,
                 PC98_PIT_FREQ, NANOSECONDS_PER_SECOND);
    switch (s->mode) {
    case 0:
    case 1:
    case 4:
    case 5:
        counter = (s->count - d) & 0xffff;
        break;
    case 3:
        /* XXX: may be incorrect for odd counts */
        counter = s->count - ((2 * d) % s->count);
        break;
    default:
        counter = s->count - (d % s->count);
        break;
    }
    return counter;
}

/* get pit output bit (PC-98 clock) */
static int pc98_pit_get_out(PITChannelState *s, int64_t current_time)
{
    uint64_t d;
    int out;

    d = muldiv64(current_time - s->count_load_time, PC98_PIT_FREQ,
                 NANOSECONDS_PER_SECOND);
    switch (s->mode) {
    default:
    case 0:
    case 1:
        out = (d >= s->count);
        break;
    case 2:
        out = ((d % s->count) == 0 && d != 0) ? 1 : 0;
        break;
    case 3:
        out = (d % s->count) < ((s->count + 1) >> 1);
        break;
    case 4:
    case 5:
        out = (d == s->count);
        break;
    }
    return out;
}

/* return -1 if no transition will occur (PC-98 clock).  */
static int64_t pc98_pit_get_next_transition_time(PITChannelState *s,
                                                 int64_t current_time)
{
    uint64_t d, next_time, base;
    int period2;

    d = muldiv64(current_time - s->count_load_time, PC98_PIT_FREQ,
                 NANOSECONDS_PER_SECOND);
    switch (s->mode) {
    default:
    case 0:
    case 1:
        if (d < s->count) {
            next_time = s->count;
        } else {
            return -1;
        }
        break;
    case 2:
        base = QEMU_ALIGN_DOWN(d, s->count);
        if ((d - base) == 0 && d != 0) {
            next_time = base + s->count;
        } else {
            next_time = base + s->count + 1;
        }
        break;
    case 3:
        base = QEMU_ALIGN_DOWN(d, s->count);
        period2 = ((s->count + 1) >> 1);
        if ((d - base) < period2) {
            next_time = base + period2;
        } else {
            next_time = base + s->count;
        }
        break;
    case 4:
    case 5:
        if (d < s->count) {
            next_time = s->count;
        } else if (d == s->count) {
            next_time = s->count + 1;
        } else {
            return -1;
        }
        break;
    }
    next_time = s->count_load_time + muldiv64(next_time, NANOSECONDS_PER_SECOND,
                                              PC98_PIT_FREQ);
    if (next_time <= current_time) {
        next_time = current_time + 1;
    }
    return next_time;
}

static void pit_set_channel_gate(PITCommonState *s, PITChannelState *sc,
                                 int val)
{
    switch (sc->mode) {
    default:
    case 0:
    case 4:
        break;
    case 1:
    case 5:
    case 2:
    case 3:
        if (sc->gate < val) {
            /* restart counting on rising edge */
            sc->count_load_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            pit_irq_timer_update(sc, sc->count_load_time);
        }
        break;
    }
    sc->gate = val;
}

static void pc98_pit_get_channel_info(PITCommonState *s, PITChannelState *sc,
                                 PITChannelInfo *info)
{
    info->gate = sc->gate;
    info->mode = sc->mode;
    info->initial_count = sc->count;
    info->out = pc98_pit_get_out(sc, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static inline void pit_load_count(PITChannelState *s, int val)
{
    if (val == 0) {
        val = 0x10000;
    }
    s->count_load_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->count = val;
    pit_irq_timer_update(s, s->count_load_time);
}

static void pit_latch_count(PITChannelState *s)
{
    if (!s->count_latched) {
        s->latched_count = pit_get_count(s);
        s->count_latched = s->rw_mode;
    }
}

static void pit_ioport_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    PITCommonState *pit = opaque;
    int channel, access;
    PITChannelState *s;

    addr &= 3;
    if (addr == 3) {
        channel = val >> 6;
        if (channel == 3) {
            /* read back command */
            for (channel = 0; channel < 3; channel++) {
                s = &pit->channels[channel];
                if (val & (2 << channel)) {
                    if (!(val & 0x20)) {
                        pit_latch_count(s);
                    }
                    if (!(val & 0x10) && !s->status_latched) {
                        s->status =
                            (pc98_pit_get_out(s,
                                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) << 7) |
                            (s->rw_mode << 4) | (s->mode << 1) | s->bcd;
                        s->status_latched = 1;
                    }
                }
            }
        } else {
            s = &pit->channels[channel];
            access = (val >> 4) & 3;
            if (access == 0) {
                pit_latch_count(s);
            } else {
                s->rw_mode = access;
                s->read_state = access;
                s->write_state = access;
                s->mode = (val >> 1) & 7;
                s->bcd = val & 1;
            }
        }
    } else {
        s = &pit->channels[addr];
        switch (s->write_state) {
        default:
        case RW_STATE_LSB:
            pit_load_count(s, val);
            break;
        case RW_STATE_MSB:
            pit_load_count(s, val << 8);
            break;
        case RW_STATE_WORD0:
            s->write_latch = val;
            s->write_state = RW_STATE_WORD1;
            break;
        case RW_STATE_WORD1:
            pit_load_count(s, s->write_latch | (val << 8));
            s->write_state = RW_STATE_WORD0;
            break;
        }
    }
}

static uint64_t pit_ioport_read(void *opaque, hwaddr addr, unsigned size)
{
    PITCommonState *pit = opaque;
    int ret, count;
    PITChannelState *s;

    addr &= 3;
    if (addr == 3) {
        /* Mode/Command register is write only */
        return 0;
    }

    s = &pit->channels[addr];
    if (s->status_latched) {
        s->status_latched = 0;
        ret = s->status;
    } else if (s->count_latched) {
        switch (s->count_latched) {
        default:
        case RW_STATE_LSB:
            ret = s->latched_count & 0xff;
            s->count_latched = 0;
            break;
        case RW_STATE_MSB:
            ret = s->latched_count >> 8;
            s->count_latched = 0;
            break;
        case RW_STATE_WORD0:
            ret = s->latched_count & 0xff;
            s->count_latched = RW_STATE_MSB;
            break;
        }
    } else {
        switch (s->read_state) {
        default:
        case RW_STATE_LSB:
            count = pit_get_count(s);
            ret = count & 0xff;
            break;
        case RW_STATE_MSB:
            count = pit_get_count(s);
            ret = (count >> 8) & 0xff;
            break;
        case RW_STATE_WORD0:
            count = pit_get_count(s);
            ret = count & 0xff;
            s->read_state = RW_STATE_WORD1;
            break;
        case RW_STATE_WORD1:
            count = pit_get_count(s);
            ret = (count >> 8) & 0xff;
            s->read_state = RW_STATE_WORD0;
            break;
        }
    }
    return ret;
}

static void pit_irq_timer_update(PITChannelState *s, int64_t current_time)
{
    int64_t expire_time;
    int irq_level;

    if (!s->irq_timer || s->irq_disabled) {
        return;
    }
    expire_time = pc98_pit_get_next_transition_time(s, current_time);
    irq_level = pc98_pit_get_out(s, current_time);
    qemu_set_irq(s->irq, irq_level);
    s->next_transition_time = expire_time;
    if (expire_time != -1) {
        timer_mod(s->irq_timer, expire_time);
    } else {
        timer_del(s->irq_timer);
    }
}

static void pit_irq_timer(void *opaque)
{
    PITChannelState *s = opaque;

    pit_irq_timer_update(s, s->next_transition_time);
}

static void pit_reset(DeviceState *dev)
{
    PITCommonState *pit = PIT_COMMON(dev);
    PITChannelState *s;

    pit_reset_common(pit);

    s = &pit->channels[0];
    if (!s->irq_disabled) {
        timer_mod(s->irq_timer, s->next_transition_time);
    }
}

static void pit_post_load(PITCommonState *s)
{
    PITChannelState *sc = &s->channels[0];

    if (sc->next_transition_time != -1 && !sc->irq_disabled) {
        timer_mod(sc->irq_timer, sc->next_transition_time);
    } else {
        timer_del(sc->irq_timer);
    }
}

static const MemoryRegionOps pit_ioport_ops = {
    .read = pit_ioport_read,
    .write = pit_ioport_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * PC-98 realize: build the counters and map the four registers with a
 * 2-byte stride at 0x71.. and again at 0x3fd9.. (the board passes
 * iobase = -1 so the AT contiguous mapping never happens).
 */
static void pit_realizefn(DeviceState *dev, Error **errp)
{
    PITCommonState *pit = PIT_COMMON(dev);
    ISADevice *isadev = ISA_DEVICE(dev);
    PITChannelState *s;
    MemoryRegion *alias;
    int i;

    s = &pit->channels[0];
    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pit_irq_timer, s);
    qdev_init_gpio_out(dev, &s->irq, 1);

    memory_region_init_io(&pit->ioports, OBJECT(pit), &pit_ioport_ops,
                          pit, "pc98-pit", 4);

    alias = g_new(MemoryRegion, 8);
    for (i = 0; i < 4; i++) {
        memory_region_init_alias(&alias[i], OBJECT(pit), "pc98-pit-io",
                                 &pit->ioports, i, 1);
        isa_register_ioport(isadev, &alias[i], 0x71 + i * 2);
        memory_region_init_alias(&alias[i + 4], OBJECT(pit), "pc98-pit-io",
                                 &pit->ioports, i, 1);
        isa_register_ioport(isadev, &alias[i + 4], 0x3fd9 + i * 2);
    }

    qdev_set_legacy_instance_id(dev, 0x71, 2);
}

static void pc98_pit_class_init(ObjectClass *klass, const void *data)
{
    PITCommonClass *k = PIT_COMMON_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    /* Replace (do not chain) the common realize: no AT ioport mapping. */
    dc->realize = pit_realizefn;
    k->set_channel_gate = pit_set_channel_gate;
    k->get_channel_info = pc98_pit_get_channel_info;
    k->post_load = pit_post_load;
    device_class_set_legacy_reset(dc, pit_reset);
}

static const TypeInfo pc98_pit_info = {
    .name          = TYPE_PC98_PIT,
    .parent        = TYPE_PIT_COMMON,
    .instance_size = sizeof(Pc98PitState),
    .class_init    = pc98_pit_class_init,
    .class_size    = sizeof(Pc98PitClass),
};

static void pc98_pit_register_types(void)
{
    type_register_static(&pc98_pit_info);
}

type_init(pc98_pit_register_types)

ISADevice *pc98_pit_init(ISABus *bus, qemu_irq alt_irq)
{
    DeviceState *dev;
    ISADevice *d;

    d = isa_new(TYPE_PC98_PIT);
    dev = DEVICE(d);
    qdev_prop_set_uint32(dev, "iobase", -1);
    isa_realize_and_unref(d, bus, &error_fatal);
    qdev_connect_gpio_out(dev, 0, alt_irq);

    return d;
}
