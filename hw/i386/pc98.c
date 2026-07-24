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
#include "hw/audio/pc98-wss.h"
#include "hw/block/pc98-fdc.h"
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
#include "hw/net/pc98-lgy98.h"
#include "hw/timer/i8254-pc98.h"
#include "system/address-spaces.h"
#include "system/ioport.h"
#include "system/memory.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/tcg.h"
#include "target/i386/cpu.h"

/*
 * PC-98 interrupt lines (they do not match the PC/AT layout):
 *   IRQ 0  PIT
 *   IRQ 1  keyboard
 *   IRQ 2  display vsync
 *   IRQ 3  FM sound
 *   IRQ 4  RS-232C
 *   IRQ 5  network #2
 *   IRQ 6  network #1
 *   IRQ 7  cascade from the slave PIC  (the PC/AT wires this on IRQ2)
 *   IRQ 8  x87 error (FERR)
 *   IRQ 9  built-in IDE
 *   IRQ10  floppy, 640 KB interface
 *   IRQ11  floppy, 1 MB interface
 *   IRQ12  Mate-X PCM (CS4231A)
 *   IRQ13  bus mouse
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

    PortioList portio_list;
};

struct Pc98MachineClass {
    X86MachineClass parent;
};

#define TYPE_PC98_MACHINE MACHINE_TYPE_NAME("pc98")
OBJECT_DECLARE_TYPE(Pc98MachineState, Pc98MachineClass, PC98_MACHINE)

/*
 * Board-level I/O ports serviced directly by the machine object: the A20
 * gate, the software reset/shutdown latch, and a handful of read-only strap
 * registers the firmware samples during power-on self test.
 */

static bool pc98_a20_enabled(void)
{
    return (X86_CPU(first_cpu)->env.a20_mask >> 20) & 1;
}

static void pc98_a20_drive(bool enabled)
{
    x86_cpu_set_a20(X86_CPU(first_cpu), enabled);
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
    pc98_a20_drive(true);
}

static uint32_t pc98_a20_cmd_read(void *opaque, uint32_t addr)
{
    return 0x5e | (pc98_a20_enabled() ? 0 : 1);
}

static void pc98_a20_cmd_write(void *opaque, uint32_t addr, uint32_t data)
{
    if (data == 0x02) {
        pc98_a20_drive(true);
    } else if (data == 0x03) {
        pc98_a20_drive(false);
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

/* Read-only straps sampled by the firmware: CPU mode, wait state, sound id. */
static uint32_t pc98_cpu_mode_read(void *opaque, uint32_t addr)
{
    return 0xec;
}

static uint32_t pc98_wait_strap_read(void *opaque, uint32_t addr)
{
    return 0x90;
}

static uint32_t pc98_sound_id_read(void *opaque, uint32_t addr)
{
    return 0x7f; /* no FM/PCM sound board attached yet */
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
    { 0x9894, 1, 1, .read = pc98_wait_strap_read },
    { 0xa460, 1, 1, .read = pc98_sound_id_read },
    PORTIO_END_OF_LIST(),
};

static void pc98_devices_init(Pc98MachineState *pms)
{
    MachineState *machine = MACHINE(pms);
    X86MachineState *x86ms = X86_MACHINE(pms);
    GSIState *gsi_state;
    ISABus *isa_bus;
    ISADevice *sysdev;
    qemu_irq *i8259;
    Pc98VgaRegions vga_regions;
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

    pc98_pit_init(isa_bus, x86ms->gsi[0]);

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
            hd[j] = drive_get(IF_IDE98, j / 2, j % 2);
        }
        idedev = pc98_ide_init(isa_bus, hd, x86ms->gsi[9]);
        pms->ide = PC98_IDE(idedev);
    }

    /* system port (RTC IRQ15) */
    sysdev = isa_new(TYPE_PC98_SYS);
    isa_realize_and_unref(sysdev, isa_bus, &error_fatal);
    qdev_connect_gpio_out(DEVICE(sysdev), 0, x86ms->gsi[15]);
    pms->sys = PC98_SYS(sysdev);

    /* display (vsync IRQ2); must precede pc98_mem_init */
    pms->vga = pc98_vga_init(get_system_io(), x86ms->gsi[2], &vga_regions);

    /*
     * memory controller: ROM banks, RAM windows, mirrors.  hd_connect
     * feeds the BIOS work-area IDE geometry patch.
     */
    pms->mem = pc98_mem_init(get_system_memory(), get_system_io(),
                             machine->ram, machine->ram_size, &vga_regions,
                             pms->ide ? pc98_ide_connected(pms->ide) : 0,
                             pc98_vga_select_ems, pms->vga);

    /* Window Accelerator Board (Cirrus GD5426 behind the NEC LSI) */
    pc98_wab_init(isa_bus);

    /* LGY-98 C-bus Ethernet (NE2000-compatible, IRQ6) */
    pc98_lgy98_init(isa_bus, x86ms->gsi[6]);

    /*
     * Built-in WSS / Mate-X PCM (CS4231A codec, IRQ12/DMA1).  Both resolve
     * through the ISA bus: isa_bus_register_input_irqs() above wires the
     * i8259 inputs (so IRQ12 -> gsi[12]) and pc98_dma_init() registered the
     * PC-98 DMA controller.
     */
    pc98_wss_init(isa_bus);

    /* board ports: A20 gate, software reset, and firmware straps */
    portio_list_init(&pms->portio_list, OBJECT(pms), pc98_board_ports,
                     pms, "pc98-machine");
    portio_list_add(&pms->portio_list, get_system_io(), 0);
}

static void pc98_machine_state_init(MachineState *machine)
{
    Pc98MachineState *pms = PC98_MACHINE(machine);
    X86MachineState *x86ms = X86_MACHINE(machine);

    if (machine->ram_size < 16 * MiB) {
        error_report("pc98 machine requires at least 16 MiB of RAM");
        exit(1);
    }
    if (machine->ram_size & (8 * MiB - 1)) {
        error_report("pc98 machine requires the RAM size to be a multiple "
                     "of 8 MiB");
        exit(1);
    }

    x86ms->above_4g_mem_size = 0;
    x86ms->below_4g_mem_size = machine->ram_size;

    x86_cpus_init(x86ms, CPU_VERSION_LATEST);

    pc98_devices_init(pms);
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

static void pc98_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = pc98_machine_state_init;
    mc->reset = pc98_machine_reset;

    mc->family = "pc98";
    mc->desc = "NEC PC-9821";
    mc->max_cpus = 1;
    mc->default_cpu_type = X86_CPU_TYPE_NAME("486");
    mc->default_ram_size = 16 * MiB;
    mc->default_ram_id = "pc98.ram";
    mc->no_parallel = 1;
    mc->no_cdrom = 1;

    compat_props_add(mc->compat_props, pc98_compat_props,
                     G_N_ELEMENTS(pc98_compat_props));
}

static const TypeInfo pc98_machine_info = {
    .name          = TYPE_PC98_MACHINE,
    .parent        = TYPE_X86_MACHINE,
    .instance_size = sizeof(Pc98MachineState),
    .class_size    = sizeof(Pc98MachineClass),
    .class_init    = pc98_class_init,
};

static void pc98_machine_init_types(void)
{
    type_register_static(&pc98_machine_info);
}
type_init(pc98_machine_init_types);
