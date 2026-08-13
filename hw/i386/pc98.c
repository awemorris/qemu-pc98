/*
 * QEMU NEC PC-9821 machine
 *
 * PC-98 support
 *   Copyright (c) 2009 TAKEDA, toshiya
 *
 * PC-98 support modernisation
 *   Copyright (c) 2026 Awe Morris
 *
 * This device is derived from the PC-98 model in the qemu/9821 fork
 * (GPL, by TAKEDA toshiya) and has been reimplemented and
 * restructured for modern QEMU.  Its register-level behaviour was
 * cross-checked against the Neko Project II and NP21W emulators.
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
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/audio/pcspk.h"
#include "hw/block/pc98-fdc.h"
#include "hw/char/pc98-serial.h"
#include "hw/display/pc98-coregraph.h"
#include "hw/display/pc98-vga.h"
#include "hw/display/pc98-wab.h"
#include "hw/dma/pc98-dma.h"
#include "hw/i386/pc98.h"
#include "hw/ide/pc98-ide.h"
#include "hw/ide/ide-bus.h"
#include "hw/input/pc98-kbd.h"
#include "hw/input/pc98-mouse.h"
#include "hw/i386/x86.h"
#include "hw/intc/i8259-pc98.h"
#include "hw/isa/isa.h"
#include "hw/misc/pc98-sys.h"
#include "hw/scsi/pc98-scsi.h"
#include "hw/pci/pci.h"
#include "hw/core/sysbus.h"
#include "hw/timer/i8254-pc98.h"
#include "system/address-spaces.h"
#include "system/ioport.h"
#include "system/memory.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "migration/vmstate.h"
#include "system/tcg.h"
#include "target/i386/cpu.h"

/*
 * PC-98 interrupt lines (they do not match the PC/AT layout):
 *   IRQ 0  PIT
 *   IRQ 1  keyboard
 *   IRQ 2  display vsync
 *   IRQ 3  WSS / Mate-X PCM (compatibility BIOS assignment)
 *   IRQ 4  RS-232C
 *   IRQ 5  PC-9801-92 SCSI
 *   IRQ 6  network #1
 *   IRQ 7  cascade from the slave PIC  (the PC/AT wires this on IRQ2)
 *   IRQ 8  x87 error (FERR)
 *   IRQ 9  built-in IDE
 *   IRQ10  floppy, 640 KB interface
 *   IRQ11  floppy, 1 MB interface
 *   IRQ12  PC-9801-86 FM sound (alternate setting)
 *   IRQ13  bus mouse
 *   IRQ14  PCI INTx on pc9821
 *   IRQ15  calendar clock (uPD4990A)
 *
 * DMA:
 *   channel 1 CS4231A
 *   channel 2 floppy (1 MB I/F)
 *   channel 3 floppy (640 KB I/F)
 *
 * The PIT counter clock is 2457600 Hz; see hw/timer/i8254-pc98.c.
 */

struct Pc98MachineState {
    X86MachineState parent;

    Pc98MemState *mem;
    Pc98SysState *sys;
    Pc98VgaState *vga;
    Pc98IdeState *ide;
    uint8_t shutdown_index;
    bool pegc_enabled;

    PortioList portio_list;
};

struct Pc98MachineClass {
    X86MachineClass parent;

    bool has_pci;   /* instantiate the PCI host bridge */
    bool has_wab;   /* instantiate the legacy NEC-LSI/Cirrus WAB */
    bool has_coregraph; /* PCI Core-Graph with a non-PnP Cirrus child */
    bool pegc_post_compat; /* stock Xa7 ROM 640x400 packed-pixel POST path */
    bool supports_pegc; /* full PEGC can be selected with the free BIOS */
};

#define TYPE_PC98_MACHINE   MACHINE_TYPE_NAME("pc98")
#define TYPE_PC9801_MACHINE MACHINE_TYPE_NAME("pc9801")
#define TYPE_PC9821_MACHINE MACHINE_TYPE_NAME("pc9821")
#define PC98_PCI_IRQ 14
OBJECT_DECLARE_TYPE(Pc98MachineState, Pc98MachineClass, PC98_MACHINE)

static bool pc98_machine_get_pegc(Object *obj, Error **errp)
{
    return PC98_MACHINE(obj)->pegc_enabled;
}

static void pc98_machine_set_pegc(Object *obj, bool value, Error **errp)
{
    PC98_MACHINE(obj)->pegc_enabled = value;
}

/*
 * Board-level I/O ports serviced directly by the machine object: the A20
 * gate, the software reset/shutdown latch, and a handful of read-only strap
 * registers the firmware samples during power-on self test.
 */

static bool pc98_a20_enabled(void)
{
    return (X86_CPU(first_cpu)->env.a20_mask >> 20) & 1;
}

static void pc98_a20_drive(Pc98MachineState *pms, bool enabled)
{
    x86_cpu_set_a20(X86_CPU(first_cpu), enabled);
    if (pms->mem) {
        pc98_mem_set_a20_wrap(pms->mem, !enabled);
    }
}

/*
 * The A20 status ports expose a fixed strap pattern; bit 0 additionally
 * carries the complement of the gate state, reading 0 while A20 is open.
 * Ports 0xF2 and 0xF6 differ only in which strap bits they advertise.
 */
static uint32_t pc98_a20_latch_read(void *opaque, uint32_t addr)
{
    return 0x2e | (pc98_a20_enabled() ? 0 : 1);
}

static void pc98_a20_latch_write(void *opaque, uint32_t addr, uint32_t data)
{
    pc98_a20_drive(opaque, true);
}

static uint32_t pc98_a20_cmd_read(void *opaque, uint32_t addr)
{
    return 0x5e | (pc98_a20_enabled() ? 0 : 1);
}

static void pc98_a20_cmd_write(void *opaque, uint32_t addr, uint32_t data)
{
    if (data == 0x02) {
        pc98_a20_drive(opaque, true);
    } else if (data == 0x03) {
        pc98_a20_drive(opaque, false);
    }
}

/*
 * Software reset.  If the firmware has armed the shutdown flag kept in the
 * system-port device, the guest wants a full machine reset; otherwise it is
 * leaving protected mode the 80286 way and only the CPU needs to be pulsed.
 */
static void pc98_soft_reset(Pc98MachineState *pms)
{
    if (pc98_sys_shutdown_armed(pms->sys)) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    } else {
        cpu_interrupt(first_cpu, CPU_INTERRUPT_INIT);
        /* the INIT re-masks A20 in the CPU; keep the wrap window in step */
        if (pms->mem) {
            pc98_mem_set_a20_wrap(pms->mem, true);
        }
    }
}

static void pc98_reset_pulse_write(void *opaque, uint32_t addr, uint32_t data)
{
    pc98_soft_reset(opaque);
}

static void pc98_reset_latch_write(void *opaque, uint32_t addr, uint32_t data)
{
    if (data & 0x01) {
        pc98_soft_reset(opaque);
    }
}

/*
 * The IDE option ROM samples this port and declines to install itself when a
 * channel's "interface absent" bit is set, so those bits have to be clear
 * whenever a disk is attached.  Bit 0 always reads back as one.
 */
static uint32_t pc98_ide_presence_read(void *opaque, uint32_t addr)
{
    Pc98MachineState *pms = opaque;
    uint8_t present = pms->ide ? pc98_ide_connected(pms->ide) : 0;
    uint32_t value = 0x01;

    if (!(present & 0x01)) {
        value |= 0x20;
    }
    if (!(present & 0x02)) {
        value |= 0x40;
    }
    return value;
}

/* Read-only straps sampled by the firmware: CPU mode, wait state. */
static uint32_t pc98_cpu_mode_read(void *opaque, uint32_t addr)
{
    return 0xec;
}

static uint32_t pc98_wait_strap_read(void *opaque, uint32_t addr)
{
    return 0x90;
}

/*
 * Firmware-initiated power off.
 *
 * qemu/9821 inherited the Bochs BIOS convention at port 0x8900.  Keep the
 * eight-byte signature rather than making any single accidental access
 * destructive.  The free BIOS uses this as the hardware back end for the
 * PC-98 APM BIOS interface (INT 1fh, AX=9a07h).
 */
static void pc98_bios_shutdown_write(void *opaque, uint32_t addr,
                                     uint32_t value)
{
    static const char signature[] = "Shutdown";
    Pc98MachineState *pms = opaque;

    if (value == signature[pms->shutdown_index]) {
        pms->shutdown_index++;
        if (pms->shutdown_index == sizeof(signature) - 1) {
            pms->shutdown_index = 0;
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        }
    } else {
        pms->shutdown_index = 0;
    }
}

/*
 * 0xf070-0xf07f: optional chipset feature-detection registers probed by the
 * Xa7-class firmware (e.g. 0xf074 bit 3 -> BIOS work-area 0x480 bit 3).  We
 * model none of these devices; return 0 ("feature absent") rather than the
 * open-bus 0xff, which would advertise phantom hardware.
 */
static uint32_t pc98_f07x_read(void *opaque, uint32_t addr)
{
    return 0x00;
}

static const MemoryRegionPortio pc98_board_ports[] = {
    { 0xf0,   1, 1, .read = pc98_ide_presence_read,
                    .write = pc98_reset_pulse_write },
    { 0xf2,   1, 1, .read = pc98_a20_latch_read,
                    .write = pc98_a20_latch_write },
    { 0xf6,   1, 1, .read = pc98_a20_cmd_read,
                    .write = pc98_a20_cmd_write },
    { 0x534,  1, 1, .read = pc98_cpu_mode_read,
                    .write = pc98_reset_latch_write },
    { 0x8900, 1, 1, .write = pc98_bios_shutdown_write },
    { 0x9894, 1, 1, .read = pc98_wait_strap_read },
    { 0xf070, 16, 1, .read = pc98_f07x_read },
    PORTIO_END_OF_LIST(),
};

static const VMStateDescription vmstate_pc98_machine = {
    .name = "pc98-machine",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(shutdown_index, Pc98MachineState),
        VMSTATE_END_OF_LIST()
    }
};

static void pc98_devices_init(Pc98MachineState *pms)
{
    MachineState *machine = MACHINE(pms);
    X86MachineState *x86ms = X86_MACHINE(pms);
    GSIState *gsi_state;
    ISABus *isa_bus;
    ISADevice *sysdev;
    ISADevice *pitdev;
    qemu_irq *i8259;
    Pc98VgaRegions vga_regions;
    Pc98MachineClass *pmc = PC98_MACHINE_GET_CLASS(pms);
    int i;

    gsi_state = g_malloc0(sizeof(*gsi_state));
    x86ms->gsi = qemu_allocate_irqs(gsi_handler, gsi_state, ISA_NUM_IRQS);

    isa_bus = isa_bus_new(NULL, get_system_memory(), get_system_io(),
                          &error_abort);
    isa_bus_register_input_irqs(isa_bus, x86ms->gsi);

    /* PICs: master at 0x00, slave at 0x08, cascade on IRQ7 */
    i8259 = pc98_pic_setup(isa_bus, x86_allocate_cpu_irq());
    for (i = 0; i < ISA_NUM_IRQS; i++) {
        gsi_state->i8259_irq[i] = i8259[i];
    }
    g_free(i8259);

    if (tcg_enabled()) {
        x86_register_ferr_irq(x86ms->gsi[8]);
    }

    pitdev = pc98_pit_init(isa_bus, x86ms->gsi[0]);

    pc98_dma_init(isa_bus);

    /* keyboard (IRQ1) */
    {
        ISADevice *kbd = isa_new(TYPE_PC98_KBD);
        isa_realize_and_unref(kbd, isa_bus, &error_fatal);
        qdev_connect_gpio_out(DEVICE(kbd), 0, x86ms->gsi[1]);
    }

    /* bus mouse (uPD8255 PPI at 0x7fd9-0x7fdf, IRQ13) */
    pc98_mouse_init(isa_bus, x86ms->gsi[13]);

    /* floppy controller (1MB I/F IRQ11/DMA2, 640KB I/F IRQ10/DMA3) */
    {
        DriveInfo *fd[MAX_FD];
        ISADevice *fdc = isa_new(TYPE_PC98_FDC);
        int j;

        for (j = 0; j < MAX_FD; j++) {
            fd[j] = drive_get(IF_FLOPPY, 0, j);
        }
        isa_realize_and_unref(fdc, isa_bus, &error_fatal);
        pc98_fdc_init_drives(fdc, fd);
    }

    /* built-in IDE interface (IRQ9) */
    {
        DriveInfo *hd[4];
        ISADevice *idedev;
        int j;

        for (j = 0; j < 4; j++) {
            hd[j] = drive_get(IF_IDE, j / 2, j % 2);
        }
        idedev = pc98_ide_init(isa_bus, hd, x86ms->gsi[9]);
        pms->ide = PC98_IDE(idedev);
    }

    /* system port (RTC IRQ15) */
    sysdev = isa_new(TYPE_PC98_SYS);
    isa_realize_and_unref(sysdev, isa_bus, &error_fatal);
    qdev_connect_gpio_out(DEVICE(sysdev), 0, x86ms->gsi[15]);
    pms->sys = PC98_SYS(sysdev);

    /* Standard -serial selects the PC-98 built-in uPD8251 interface. */
    if (serial_hd(0)) {
        ISADevice *serial = isa_new(TYPE_PC98_SERIAL);

        qdev_prop_set_chr(DEVICE(serial), "chardev", serial_hd(0));
        isa_realize_and_unref(serial, isa_bus, &error_fatal);
    }

    /* PIT channel 1, gated by active-low system-PPI port-C bit 3. */
    {
        ISADevice *speaker = isa_new(TYPE_PC_SPEAKER);

        qdev_prop_set_uint32(DEVICE(speaker), "iobase", 0);
        qdev_prop_set_uint32(DEVICE(speaker), "pit-frequency", 2457600);
        qdev_prop_set_uint8(DEVICE(speaker), "pit-channel", 1);
        object_property_set_link(OBJECT(speaker), "pit", OBJECT(pitdev),
                                 &error_fatal);
        isa_realize_and_unref(speaker, isa_bus, &error_fatal);
        qdev_connect_gpio_out_named(
            DEVICE(sysdev), "speaker", 0,
            qdev_get_gpio_in_named(DEVICE(speaker), "gate", 0));
    }

    /* display (vsync IRQ2); must precede pc98_mem_init */
    pms->vga = pc98_vga_init(get_system_io(), x86ms->gsi[2],
                             pmc->pegc_post_compat,
                             pms->pegc_enabled,
                             &vga_regions);

    /*
     * memory controller: ROM banks, RAM windows, mirrors.  hd_connect
     * feeds the BIOS work-area IDE geometry patch.
     */
    pms->mem = pc98_mem_init(get_system_memory(), get_system_io(),
                             machine->ram, machine->ram_size, &vga_regions,
                              pms->ide ? pc98_ide_connected(pms->ide) : 0,
                              pmc->has_pci, pmc->pegc_post_compat,
                              pms->pegc_enabled,
                              pc98_vga_select_ems, pms->vga);

    /*
     * PC-9801-92 compatible C-Bus SCSI interface.  It is created only when
     * an if=scsi drive is present, so its option ROM cannot perturb the
     * ordinary IDE-only firmware path.  Create it after the memory
     * controller so the option ROM can join the Xa POST shadow-RAM gate.
    */
    if (drive_get_max_bus(IF_SCSI) >= 0) {
        pc98_scsi_init(isa_bus, pms->mem);
    }

    /*
     * Window Accelerator Board (Cirrus GD5426 behind the NEC LSI).
     *
     * The legacy WAB's fixed 15 MiB aperture conflicts with RAM after the
     * Xa7 firmware disables the 16 MiB system-space hole.  The PCI model
     * therefore exposes only the relocatable high aperture; the non-PCI
     * model retains the fixed DOS-facing window.
     */
    if (pmc->has_wab) {
        pc98_wab_init(isa_bus, !pmc->has_pci);
    }

    /*
     * PCI host bridge.  PC-98 uses Configuration Mechanism #1 at the PC/AT
     * ports 0xCF8/0xCFC.  All four PCI interrupt pins are level-ORed onto
     * the fixed IRQ14 route advertised by the compatibility PCI BIOS.
     */
    if (pmc->has_pci) {
        DeviceState *host = qdev_new(TYPE_PC98_PCI_HOST);
        SysBusDevice *sbd = SYS_BUS_DEVICE(host);
        PCIBus *pci_bus;

        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_connect_irq(sbd, 0, x86ms->gsi[PC98_PCI_IRQ]);
        pci_bus = pc98_pci_get_bus(host);
        /* wire dev0 config reg 0x64 (D000 window shadow) to the mem controller */
        pc98_pci_set_d000_mem(pms->mem);
        if (pmc->has_coregraph) {
            PCIDevice *coregraph =
                pci_new(PCI_DEVFN(7, 0), TYPE_PC98_COREGRAPH);

            /* Keep the shared GDC/Core-Graph console addressable by device. */
            DEVICE(coregraph)->id = g_strdup("coregraph");
            pc98_coregraph_set_primary_vga(coregraph, pms->vga);
            pci_realize_and_unref(coregraph, pci_bus, &error_fatal);
        }

        /*
         * -usb provides a Windows 2000-compatible USB 1.1/2.0 pair.  They
         * remain optional so an ordinary PC-9821 does not gain hardware
         * unless requested.  The compatibility firmware assigns fixed BARs
         * before an OS starts and may later reallocate them.
         */
        if (machine_usb(machine)) {
            PCIDevice *uhci = pci_new(PCI_DEVFN(8, 0), "piix3-usb-uhci");
            PCIDevice *ehci = pci_new(PCI_DEVFN(13, 0), "usb-ehci");

            DEVICE(uhci)->id = g_strdup("usb11");
            pci_realize_and_unref(uhci, pci_bus, &error_fatal);

            DEVICE(ehci)->id = g_strdup("usb20");
            pci_realize_and_unref(ehci, pci_bus, &error_fatal);
        }
    } else if (machine_usb(machine)) {
        error_report("pc9801 has no PCI bus for a USB host controller");
        exit(1);
    }

    /* board ports: A20 gate, software reset, and firmware straps */
    portio_list_init(&pms->portio_list, OBJECT(pms), pc98_board_ports,
                     pms, "pc98-machine");
    portio_list_add(&pms->portio_list, get_system_io(), 0);
}

static void pc98_machine_state_init(MachineState *machine)
{
    Pc98MachineState *pms = PC98_MACHINE(machine);
    Pc98MachineClass *pmc = PC98_MACHINE_GET_CLASS(pms);
    X86MachineState *x86ms = X86_MACHINE(machine);

    if (machine->ram_size < 2 * MiB) {
        error_report("pc98 machine requires at least 2 MiB of RAM");
        exit(1);
    }
    if (pms->pegc_enabled && !pmc->supports_pegc) {
        error_report("PEGC is only available on the pc9821 machine");
        exit(1);
    }
    /*
     * The BIOS work area reports memory below 16 MiB in 128 KiB units.
     * Requiring 8 MiB multiples excluded the non-power-of-two memory
     * configurations common on 386-era PC-98 systems.
     */
    if (machine->ram_size & (128 * KiB - 1)) {
        error_report("pc98 machine requires the RAM size to be a multiple "
                     "of 128 KiB");
        exit(1);
    }

    x86ms->above_4g_mem_size = 0;
    x86ms->below_4g_mem_size = machine->ram_size;

    x86_cpus_init(x86ms, CPU_VERSION_LATEST);

    pc98_devices_init(pms);
    vmstate_register(NULL, 0, &vmstate_pc98_machine, pms);
}

static void pc98_machine_reset(MachineState *machine, ResetType type)
{
    CPUState *cs;

    qemu_devices_reset(type);

    CPU_FOREACH(cs) {
        x86_cpu_after_reset(X86_CPU(cs));
    }
}

static GlobalProperty pc98_compat_props[] = {
    /* PC-98 A20 semantics: wrap the whole address space at 1 MiB */
    { TYPE_X86_CPU, "pc98-a20-mask", "on" },
};

/*
 * The PC-98 interrupt controller pair lives at I/O 0x00/0x08 with the
 * cascade on IR7, none of which the in-kernel PC/AT model can express, so
 * hardware accelerators must keep the irqchip in userspace.
 */
static bool pc98_get_kernel_irqchip_default(const MachineState *ms)
{
    return false;
}

static void pc98_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    Pc98MachineClass *pmc = PC98_MACHINE_CLASS(oc);

    mc->init = pc98_machine_state_init;
    mc->reset = pc98_machine_reset;
    mc->get_kernel_irqchip_default = pc98_get_kernel_irqchip_default;
    mc->family = "pc98";
    mc->desc = "NEC PC-9821";
    mc->max_cpus = 1;
    mc->default_cpu_type = X86_CPU_TYPE_NAME("486");
    mc->default_ram_size = 16 * MiB;
    mc->default_ram_id = "pc98.ram";
    mc->block_default_type = IF_IDE;
    mc->no_serial = 1;
    mc->no_parallel = 1;
    mc->no_cdrom = 1;

    object_class_property_add_bool(oc, "pegc",
                                   pc98_machine_get_pegc,
                                   pc98_machine_set_pegc);
    object_class_property_set_description(
        oc, "pegc",
        "Enable PEGC (requires pc9821 and the QEMU compatibility BIOS)");

    pmc->has_pci = false;
    pmc->has_wab = true;
    pmc->has_coregraph = false;
    pmc->pegc_post_compat = false;
    pmc->supports_pegc = false;

}

/*
 * pc9801: the pre-PC-9821 configuration.  It retains the base GDC/GRCG/EGC
 * display but has no local-bus Window Accelerator Board.
 */
static void pc9801_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    Pc98MachineClass *pmc = PC98_MACHINE_CLASS(oc);

    mc->desc = "NEC PC-9801";
    pmc->has_pci = false;
    pmc->has_wab = false;
    pmc->has_coregraph = false;
    pmc->pegc_post_compat = false;
    pmc->supports_pegc = false;

    compat_props_add(mc->compat_props, pc98_compat_props,
                     G_N_ELEMENTS(pc98_compat_props));
}

/*
 * pc9821: PCI-equipped PC-9821 with the on-board Core-Graph bridge.  The
 * Cirrus chip below Core-Graph is not itself visible as a PCI function.
 */
static void pc9821_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    Pc98MachineClass *pmc = PC98_MACHINE_CLASS(oc);

    mc->desc = "NEC PC-9821";
    pmc->has_pci = true;
    pmc->has_wab = false;
    pmc->has_coregraph = true;
    pmc->pegc_post_compat = true;
    pmc->supports_pegc = true;

    compat_props_add(mc->compat_props, pc98_compat_props,
                     G_N_ELEMENTS(pc98_compat_props));
}

static const TypeInfo pc98_machine_types[] = {
    {
        .name          = TYPE_PC98_MACHINE,
        .parent        = TYPE_X86_MACHINE,
        .instance_size = sizeof(Pc98MachineState),
        .class_size    = sizeof(Pc98MachineClass),
        .class_init    = pc98_class_init,
        .abstract      = true,
    },
    {
        .name          = TYPE_PC9801_MACHINE,
        .parent        = TYPE_PC98_MACHINE,
        .class_init    = pc9801_class_init,
    },
    {
        .name          = TYPE_PC9821_MACHINE,
        .parent        = TYPE_PC98_MACHINE,
        .class_init    = pc9821_class_init,
    },
};

DEFINE_TYPES(pc98_machine_types)
