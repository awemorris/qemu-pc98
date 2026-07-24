/*
 * QEMU NEC PC-98 built-in WSS (Windows Sound System / Mate-X PCM)
 *
 * Copyright (c) 2026 Awe Morris
 *
 * The later PC-9821 desktops (e.g. the V13) carry a built-in "Windows
 * Sound System" compatible PCM unit, marketed by NEC as Mate-X PCM.  At
 * its heart is a Crystal CS4231A codec, the very same codec QEMU already
 * models in hw/audio/cs4231a.c; only the surrounding I/O map and the
 * IRQ/DMA strapping are PC-98 specific.  The port block lives at 0x0F40:
 *
 *   0x0F40  R/W  IRQ/DMA configuration latch (PC-98 specific)
 *   0x0F43  RO   Sound ID (PC-98 specific; low 6 bits read 0x04)
 *   0x0F44  R/W  CS4231 Index/Address register  (codec offset 0)
 *   0x0F45  R/W  CS4231 Index Data              (codec offset 1)
 *   0x0F46  RO   CS4231 Status                  (codec offset 2)
 *   0x0F47  R/W  CS4231 PIO Data                (codec offset 3)
 *
 * The routing is fixed to IRQ12 (PC-98 INT5) and DMA channel 1.
 *
 * Rather than re-implement the codec, this device instantiates the shared
 * CS4231A with iobase=0x0F44/irq=12/dma=1, which lines its four contiguous
 * registers up exactly with 0x0F44-0x0F47 (the same "shared core + PC-98
 * port-mapping wrapper" scheme used by hw/ide/pc98-ide.c and
 * hw/net/pc98-lgy98.c), and adds only the two PC-98-only ports (0x0F40 and
 * 0x0F43).  The codec's IRQ and DMA resolve through the ISA bus: the pc98
 * machine registers the i8259 input IRQs (so IRQ12 -> gsi[12]) and the
 * PC-98 DMA controller on that bus.  The register usage was cross-checked
 * against the StratoHAL/Suika3 WSS driver (wss_detect / wss_init_chip):
 * everything past 0x0F43 is stock CS4231 handled by cs4231a.c.
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
#include "qemu/error-report.h"
#include "qemu/audio.h"
#include "hw/audio/pc98-wss.h"
#include "hw/core/qdev-properties.h"
#include "hw/isa/isa.h"
#include "system/ioport.h"
#include "qom/object.h"

/*
 * The shared CS4231A codec (hw/audio/cs4231a.c) does not export a public
 * header, but its QOM type name and its "iobase"/"irq"/"dma" properties are
 * stable; wrap it by name.
 */
#define TYPE_CS4231A            "cs4231a"

/* PC-98 WSS port block. */
#define PC98_WSS_IOBASE         0x0f40
#define PC98_WSS_CFG            (PC98_WSS_IOBASE + 0)   /* 0x0F40 R/W */
#define PC98_WSS_SOUND_ID       (PC98_WSS_IOBASE + 3)   /* 0x0F43 RO  */
#define PC98_WSS_CODEC_BASE     (PC98_WSS_IOBASE + 4)   /* 0x0F44 codec */

/* Fixed board wiring: IRQ12 (INT5), DMA channel 1. */
#define PC98_WSS_CODEC_IRQ      12
#define PC98_WSS_CODEC_DMA      1

/*
 * IRQ/DMA config latch default.  The driver decodes IRQ from (cfg>>3)&7 via
 * {-1,3,5,10,12,...} and DMA from cfg&7 via {-1,0,1,3,...}; field 4 -> IRQ12
 * and field 2 -> DMA1, so (4<<3)|2 = 0x22 reads back as IRQ12/DMA1.
 */
#define PC98_WSS_CFG_DEFAULT    0x22

/* Sound ID: the driver requires (id & 0x3f) == 0x04 for the internal WSS. */
#define PC98_WSS_SOUND_ID_VALUE 0x04

OBJECT_DECLARE_SIMPLE_TYPE(Pc98WssState, PC98_WSS)

struct Pc98WssState {
    ISADevice parent_obj;

    AudioBackend *audio_be;     /* optional; forwarded to the CS4231A child */
    ISADevice *codec;           /* the wrapped shared CS4231A, or NULL       */
    PortioList portio;          /* PC-98-only ports 0x0F40 / 0x0F43          */
    uint8_t cfg;                /* 0x0F40 IRQ/DMA configuration latch        */
};

/*
 * 0x0F40 IRQ/DMA configuration register.  The board routing is hardwired to
 * IRQ12/DMA1 in this model, so we simply latch whatever the guest writes and
 * report it back: the driver's auto-detect writes the IRQ12/DMA1 field values
 * and re-reads them, and the fixed routing is left untouched.
 */
static uint32_t pc98_wss_cfg_read(void *opaque, uint32_t addr)
{
    Pc98WssState *s = opaque;

    return s->cfg;
}

static void pc98_wss_cfg_write(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98WssState *s = opaque;

    s->cfg = val & 0xff;
}

/* 0x0F43 Sound ID (read only); writes are ignored. */
static uint32_t pc98_wss_id_read(void *opaque, uint32_t addr)
{
    return PC98_WSS_SOUND_ID_VALUE;
}

static const MemoryRegionPortio pc98_wss_portio[] = {
    { PC98_WSS_CFG, 1, 1,
      .read = pc98_wss_cfg_read, .write = pc98_wss_cfg_write },
    { PC98_WSS_SOUND_ID, 1, 1,
      .read = pc98_wss_id_read },
    PORTIO_END_OF_LIST(),
};

static void pc98_wss_realize(DeviceState *dev, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(dev);
    Pc98WssState *s = PC98_WSS(dev);
    ISABus *bus = isa_bus_from_device(isadev);
    ISADevice *codec;
    Error *local_err = NULL;

    s->cfg = PC98_WSS_CFG_DEFAULT;

    /*
     * Instantiate the shared CS4231A codec on the same ISA bus.  Its four
     * registers are contiguous from iobase, so iobase=0x0F44 places them at
     * 0x0F44-0x0F47.  IRQ12/DMA1 resolve through the ISA bus.
     */
    codec = isa_new(TYPE_CS4231A);
    object_property_set_uint(OBJECT(codec), "iobase", PC98_WSS_CODEC_BASE,
                             &error_abort);
    object_property_set_uint(OBJECT(codec), "irq", PC98_WSS_CODEC_IRQ,
                             &error_abort);
    object_property_set_uint(OBJECT(codec), "dma", PC98_WSS_CODEC_DMA,
                             &error_abort);
    if (s->audio_be) {
        object_property_set_str(OBJECT(codec), "audiodev",
                                audio_be_get_id(s->audio_be), &error_abort);
    }

    /*
     * The codec's realize calls audio_be_check(); its only expected failure
     * is "no audio backend available".  The PC-98 firmware and the DOS/HDD
     * boot path do not need the codec, so degrade gracefully rather than
     * abort the machine: warn, drop the codec, but still register the
     * PC-98-only ports so the board identity stays consistent.  The machine
     * therefore boots even with no -audiodev on the command line.
     */
    if (!isa_realize_and_unref(codec, bus, &local_err)) {
        warn_report_err(local_err);
        warn_report("pc98-wss: CS4231A codec disabled (no audio backend)");
        s->codec = NULL;
    } else {
        s->codec = codec;
    }

    isa_register_portio_list(isadev, &s->portio, 0, pc98_wss_portio, s,
                             "pc98-wss");
}

static const Property pc98_wss_properties[] = {
    DEFINE_AUDIO_PROPERTIES(Pc98WssState, audio_be),
};

static void pc98_wss_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pc98_wss_realize;
    device_class_set_props(dc, pc98_wss_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "NEC PC-98 built-in WSS (Mate-X PCM, CS4231A)";
    /* Board-embedded device: created by the pc98 machine, not by the user. */
    dc->user_creatable = false;
    /*
     * No vmstate: the wrapped CS4231A migrates itself, and the 0x0F40 latch
     * is a fixed strap re-read/rewritten by the driver's auto-detect.
     */
}

static const TypeInfo pc98_wss_info = {
    .name          = TYPE_PC98_WSS,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(Pc98WssState),
    .class_init    = pc98_wss_class_init,
};

static void pc98_wss_register_types(void)
{
    type_register_static(&pc98_wss_info);
}

type_init(pc98_wss_register_types)

void pc98_wss_init(ISABus *bus)
{
    ISADevice *isadev = isa_new(TYPE_PC98_WSS);

    isa_realize_and_unref(isadev, bus, &error_fatal);
}
