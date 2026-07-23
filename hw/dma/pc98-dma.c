/*
 * QEMU NEC PC-9821 DMA controller (uPD71037 / 8237 with PC-98 wiring)
 *
 * Self-contained variant of hw/dma/i8257.c implementing the PC-98 wiring:
 *  - single controller, registers at odd ports 0x01-0x1f (stride 2)
 *  - page registers at 0x21/0x23/0x25/0x27 (mapping ch1,2,3,0)
 *  - bank boundary register at 0x29
 *  - access control at 0x439 (bit 2: wrap DMA addresses at 1 MiB)
 *  - high page registers at 0xe05-0xe0b (write only)
 *
 * Based on hw/dma/i8257.c:
 *   Copyright (c) 2003-2004 Vassili Karpov (malc)
 *
 * PC-98 support
 *   Copyright (c) 2009 TAKEDA, toshiya
 *
 * PC-98 support modernisation
 *   Copyright (c) 2026 Awe Morris
 *
 * The PC-98 register-level behaviour is derived from the qemu/9821
 * fork (GPL, by TAKEDA toshiya) and was cross-checked against the
 * Neko Project II and NP21W emulators.
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
#include "hw/isa/isa.h"
#include "hw/dma/pc98-dma.h"
#include "system/ioport.h"
#include "system/physmem.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/log.h"
#include "qom/object.h"

#define ADDR 0
#define COUNT 1

enum {
    CMD_MEMORY_TO_MEMORY = 0x01,
    CMD_FIXED_ADDRESS    = 0x02,
    CMD_BLOCK_CONTROLLER = 0x04,
    CMD_COMPRESSED_TIME  = 0x08,
    CMD_CYCLIC_PRIORITY  = 0x10,
    CMD_EXTENDED_WRITE   = 0x20,
    CMD_LOW_DREQ         = 0x40,
    CMD_LOW_DACK         = 0x80,
    CMD_NOT_SUPPORTED    = CMD_MEMORY_TO_MEMORY | CMD_FIXED_ADDRESS
    | CMD_COMPRESSED_TIME | CMD_CYCLIC_PRIORITY | CMD_EXTENDED_WRITE
    | CMD_LOW_DREQ | CMD_LOW_DACK
};

enum {
    MODE_DIR = 0x20,
};

enum {
    ACCESS_WRAP_1MB = 0x04,   /* 0x439 bit 2: keep DMA within the low 1 MiB */
};

typedef struct Pc98DmaRegs {
    int now[2];
    uint16_t base[2];
    uint8_t mode;
    uint8_t page;
    uint8_t pageh;
    uint8_t dack;
    uint8_t eop;
    uint8_t bank_wrap;
    IsaDmaTransferHandler transfer_handler;
    void *opaque;
} Pc98DmaRegs;

#define TYPE_PC98_DMA "pc98-dma"
OBJECT_DECLARE_SIMPLE_TYPE(Pc98DmaState, PC98_DMA)

struct Pc98DmaState {
    ISADevice parent_obj;

    uint8_t status;
    uint8_t command;
    uint8_t mask;
    uint8_t flip_flop;
    uint8_t access_reg;
    Pc98DmaRegs regs[4];
    PortioList portio_list;

    QEMUBH *dma_bh;
    bool dma_bh_scheduled;
    int running;
};

static void pc98_dma_run(void *opaque);

static inline void pc98_dma_init_chan(Pc98DmaState *d, int ichan)
{
    Pc98DmaRegs *r;

    r = d->regs + ichan;
    r->now[ADDR] = r->base[ADDR];
    r->now[COUNT] = 0;
}

static inline int pc98_dma_getff(Pc98DmaState *d)
{
    int ff;

    ff = d->flip_flop;
    d->flip_flop = !ff;
    return ff;
}

/* iport = 0..7: channel address/count registers */
static uint32_t pc98_dma_read_chan(Pc98DmaState *d, int iport)
{
    int ichan, nreg, ff, val, dir;
    Pc98DmaRegs *r;

    ichan = iport >> 1;
    nreg = iport & 1;
    r = d->regs + ichan;

    dir = ((r->mode >> 5) & 1) ? -1 : 1;
    ff = pc98_dma_getff(d);
    if (nreg) {
        val = r->base[COUNT] - r->now[COUNT];
    } else {
        val = r->now[ADDR] + r->now[COUNT] * dir;
    }

    return (val >> (ff << 3)) & 0xff;
}

static void pc98_dma_write_chan(Pc98DmaState *d, int iport, uint32_t data)
{
    int ichan, nreg;
    Pc98DmaRegs *r;

    ichan = iport >> 1;
    nreg = iport & 1;
    r = d->regs + ichan;
    if (pc98_dma_getff(d)) {
        r->base[nreg] = (r->base[nreg] & 0xff) | ((data << 8) & 0xff00);
        pc98_dma_init_chan(d, ichan);
    } else {
        r->base[nreg] = (r->base[nreg] & 0xff00) | (data & 0xff);
    }
}

/* iport = 0..7: controller registers */
static void pc98_dma_write_cont(Pc98DmaState *d, int iport, uint32_t data)
{
    int ichan;

    switch (iport) {
    case 0x00:                  /* command */
        if ((data != 0) && (data & CMD_NOT_SUPPORTED)) {
            qemu_log_mask(LOG_UNIMP, "pc98-dma: cmd 0x%02x not supported\n",
                          data);
            return;
        }
        d->command = data;
        break;

    case 0x01:                  /* request */
        ichan = data & 3;
        if (data & 4) {
            d->status |= 1 << (ichan + 4);
        } else {
            d->status &= ~(1 << (ichan + 4));
        }
        d->status &= ~(1 << ichan);
        pc98_dma_run(d);
        break;

    case 0x02:                  /* single mask */
        if (data & 4) {
            d->mask |= 1 << (data & 3);
        } else {
            d->mask &= ~(1 << (data & 3));
        }
        pc98_dma_run(d);
        break;

    case 0x03:                  /* mode */
        ichan = data & 3;
        d->regs[ichan].mode = data;
        break;

    case 0x04:                  /* clear flip flop */
        d->flip_flop = 0;
        break;

    case 0x05:                  /* reset */
        d->flip_flop = 0;
        d->mask = ~0;
        d->status = 0;
        d->command = 0;
        break;

    case 0x06:                  /* clear mask for all channels */
        d->mask = 0;
        pc98_dma_run(d);
        break;

    case 0x07:                  /* write mask for all channels */
        d->mask = data;
        pc98_dma_run(d);
        break;
    }
}

static uint32_t pc98_dma_read_cont(Pc98DmaState *d, int iport)
{
    int val;

    switch (iport) {
    case 0x00:                  /* status */
        val = d->status;
        d->status &= 0xf0;
        break;
    case 0x07:                  /* mask */
        val = d->mask;
        break;
    default:
        val = 0;
        break;
    }

    return val;
}

/* PC-98 port layout */

static void dma_port_write(void *opaque, uint32_t nport, uint32_t data)
{
    static const uint8_t wrap_masks[4] = { 0, 0x0f, 0, 0xff };
    Pc98DmaState *d = opaque;
    int ichan;

    switch (nport) {
    case 0x01: case 0x03: case 0x05: case 0x07:
    case 0x09: case 0x0b: case 0x0d: case 0x0f:
        pc98_dma_write_chan(d, (nport >> 1) & 7, data);
        break;
    case 0x11: case 0x13: case 0x15: case 0x17:
    case 0x19: case 0x1b: case 0x1d: case 0x1f:
        pc98_dma_write_cont(d, (nport >> 1) & 7, data);
        break;
    case 0x21: case 0x23: case 0x25: case 0x27:
        ichan = ((nport >> 1) + 1) & 3;
        d->regs[ichan].page = data;
        break;
    case 0x29:
        ichan = data & 3;
        d->regs[ichan].bank_wrap = wrap_masks[(data >> 2) & 3];
        break;
    case 0x439:
        d->access_reg = data;
        break;
    case 0xe05: case 0xe07: case 0xe09: case 0xe0b:
        ichan = ((nport >> 1) - 2) & 3;
        d->regs[ichan].pageh = data;
        break;
    }
}

static uint32_t dma_port_read(void *opaque, uint32_t nport)
{
    Pc98DmaState *d = opaque;
    int ichan;

    switch (nport) {
    case 0x01: case 0x03: case 0x05: case 0x07:
    case 0x09: case 0x0b: case 0x0d: case 0x0f:
        return pc98_dma_read_chan(d, (nport >> 1) & 7);
    case 0x11: case 0x13: case 0x15: case 0x17:
    case 0x19: case 0x1b: case 0x1d: case 0x1f:
        return pc98_dma_read_cont(d, (nport >> 1) & 7);
    case 0x21: case 0x23: case 0x25: case 0x27:
        ichan = ((nport >> 1) + 1) & 3;
        return d->regs[ichan].page;
    case 0x439:
        return d->access_reg;
    }
    return 0;
}

static const MemoryRegionPortio pc98_dma_portio[] = {
    { 0x01, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x03, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x05, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x07, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x09, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x0b, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x0d, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x0f, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x11, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x13, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x15, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x17, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x19, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x1b, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x1d, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x1f, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x21, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x23, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x25, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x27, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0x29, 1, 1, .write = dma_port_write },
    { 0x439, 1, 1, .read = dma_port_read, .write = dma_port_write },
    { 0xe05, 1, 1, .write = dma_port_write },
    { 0xe07, 1, 1, .write = dma_port_write },
    { 0xe09, 1, 1, .write = dma_port_write },
    { 0xe0b, 1, 1, .write = dma_port_write },
    PORTIO_END_OF_LIST(),
};

/* IsaDma interface */

static bool pc98_dma_has_autoinitialization(IsaDma *obj, int nchan)
{
    Pc98DmaState *d = PC98_DMA(obj);

    return (d->regs[nchan & 3].mode >> 4) & 1;
}

static void pc98_dma_hold_DREQ(IsaDma *obj, int nchan)
{
    Pc98DmaState *d = PC98_DMA(obj);
    int ichan = nchan & 3;

    d->status |= 1 << (ichan + 4);
    pc98_dma_run(d);
}

static void pc98_dma_release_DREQ(IsaDma *obj, int nchan)
{
    Pc98DmaState *d = PC98_DMA(obj);
    int ichan = nchan & 3;

    d->status &= ~(1 << (ichan + 4));
    pc98_dma_run(d);
}

static void pc98_dma_channel_run(Pc98DmaState *d, int ichan)
{
    Pc98DmaRegs *r = &d->regs[ichan];
    int n;

    n = r->transfer_handler(r->opaque, ichan,
                            r->now[COUNT], r->base[COUNT] + 1);
    r->now[COUNT] = n;

    /* increment the page register within the bank boundary */
    if (r->bank_wrap) {
        uint32_t last_addr = r->base[ADDR];
        if (r->mode & MODE_DIR) {
            last_addr -= n;
            if (last_addr & 0xffff0000) {
                r->page = ((r->page - 1) & r->bank_wrap) |
                          (r->page & ~r->bank_wrap);
            }
        } else {
            last_addr += n;
            if (last_addr & 0xffff0000) {
                r->page = ((r->page + 1) & r->bank_wrap) |
                          (r->page & ~r->bank_wrap);
            }
        }
    }

    if (n == r->base[COUNT] + 1) {
        d->status |= (1 << ichan);
    }
}

static void pc98_dma_run(void *opaque)
{
    Pc98DmaState *d = opaque;
    int ichan;
    int rearm = 0;

    if (d->running) {
        rearm = 1;
        goto out;
    } else {
        d->running = 1;
    }

    for (ichan = 0; ichan < 4; ichan++) {
        int mask = 1 << ichan;

        if ((0 == (d->mask & mask)) && (0 != (d->status & (mask << 4)))) {
            pc98_dma_channel_run(d, ichan);
            rearm = 1;
        }
    }

    d->running = 0;
out:
    if (rearm) {
        qemu_bh_schedule_idle(d->dma_bh);
        d->dma_bh_scheduled = true;
    }
}

static void pc98_dma_register_channel(IsaDma *obj, int nchan,
                                      IsaDmaTransferHandler transfer_handler,
                                      void *opaque)
{
    Pc98DmaState *d = PC98_DMA(obj);
    Pc98DmaRegs *r = d->regs + (nchan & 3);

    r->transfer_handler = transfer_handler;
    r->opaque = opaque;
}

static bool pc98_dma_is_verify_transfer(Pc98DmaRegs *r)
{
    return (r->mode & 0x0c) == 0;
}

static hwaddr pc98_dma_addr(Pc98DmaState *d, Pc98DmaRegs *r)
{
    hwaddr addr = ((r->pageh & 0x7f) << 24) | (r->page << 16) | r->now[ADDR];

    if (d->access_reg & ACCESS_WRAP_1MB) {
        addr &= 0xfffff;
    }
    return addr;
}

static int pc98_dma_read_memory(IsaDma *obj, int nchan, void *buf, int pos,
                                int len)
{
    Pc98DmaState *d = PC98_DMA(obj);
    Pc98DmaRegs *r = &d->regs[nchan & 3];
    hwaddr addr = pc98_dma_addr(d, r);

    if (pc98_dma_is_verify_transfer(r)) {
        memset(buf, 0, len);
        return len;
    }

    if (r->mode & MODE_DIR) {
        int i;
        uint8_t *p = buf;

        physical_memory_read(addr - pos - len, buf, len);
        for (i = 0; i < len >> 1; i++) {
            uint8_t b = p[len - i - 1];
            p[i] = b;
        }
    } else {
        physical_memory_read(addr + pos, buf, len);
    }

    return len;
}

static int pc98_dma_write_memory(IsaDma *obj, int nchan, void *buf, int pos,
                                 int len)
{
    Pc98DmaState *d = PC98_DMA(obj);
    Pc98DmaRegs *r = &d->regs[nchan & 3];
    hwaddr addr = pc98_dma_addr(d, r);

    if (pc98_dma_is_verify_transfer(r)) {
        return len;
    }

    if (r->mode & MODE_DIR) {
        int i;
        uint8_t *p = buf;

        physical_memory_write(addr - pos - len, buf, len);
        for (i = 0; i < len; i++) {
            uint8_t b = p[len - i - 1];
            p[i] = b;
        }
    } else {
        physical_memory_write(addr + pos, buf, len);
    }

    return len;
}

static void pc98_dma_schedule(IsaDma *obj)
{
    Pc98DmaState *d = PC98_DMA(obj);

    if (d->dma_bh_scheduled) {
        qemu_notify_event();
    }
}

static void pc98_dma_reset(DeviceState *dev)
{
    Pc98DmaState *d = PC98_DMA(dev);
    int i;

    pc98_dma_write_cont(d, 0x05, 0);
    for (i = 0; i < ARRAY_SIZE(d->regs); i++) {
        d->regs[i].bank_wrap = 0;
    }
    d->access_reg = 0;
}

static int pc98_dma_phony_handler(void *opaque, int nchan, int dma_pos,
                                  int dma_len)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "pc98-dma: unregistered DMA channel used nchan=%d\n", nchan);
    return dma_pos;
}

static void pc98_dma_realize(DeviceState *dev, Error **errp)
{
    ISADevice *isa = ISA_DEVICE(dev);
    Pc98DmaState *d = PC98_DMA(dev);
    int i;

    isa_register_portio_list(isa, &d->portio_list, 0,
                             pc98_dma_portio, d, "pc98-dma");

    for (i = 0; i < ARRAY_SIZE(d->regs); ++i) {
        d->regs[i].transfer_handler = pc98_dma_phony_handler;
    }

    d->dma_bh = qemu_bh_new(pc98_dma_run, d);
}

static void pc98_dma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    IsaDmaClass *idc = ISADMA_CLASS(klass);

    dc->realize = pc98_dma_realize;
    device_class_set_legacy_reset(dc, pc98_dma_reset);

    idc->has_autoinitialization = pc98_dma_has_autoinitialization;
    idc->read_memory = pc98_dma_read_memory;
    idc->write_memory = pc98_dma_write_memory;
    idc->hold_DREQ = pc98_dma_hold_DREQ;
    idc->release_DREQ = pc98_dma_release_DREQ;
    idc->schedule = pc98_dma_schedule;
    idc->register_channel = pc98_dma_register_channel;
    /* Reason: needs to be wired up by isa_bus_dma() to work */
    dc->user_creatable = false;
    /* TODO: migration support (vmstate) */
}

static const TypeInfo pc98_dma_info = {
    .name = TYPE_PC98_DMA,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(Pc98DmaState),
    .class_init = pc98_dma_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_ISADMA },
        { }
    }
};

static void pc98_dma_register_types(void)
{
    type_register_static(&pc98_dma_info);
}

type_init(pc98_dma_register_types)

void pc98_dma_init(ISABus *bus)
{
    ISADevice *isa;

    isa = isa_new(TYPE_PC98_DMA);
    isa_realize_and_unref(isa, bus, &error_fatal);

    /* one controller serves all four (8-bit) channels */
    isa_bus_dma(bus, ISADMA(isa), ISADMA(isa));
}
