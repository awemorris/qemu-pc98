/*
 * PC-98 machine configuration tests
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define PC98_IDE_DATA     0x640
#define PC98_IDE_NSECTOR  0x644
#define PC98_IDE_SECTOR   0x646
#define PC98_IDE_LCYL     0x648
#define PC98_IDE_HCYL     0x64a
#define PC98_IDE_SELECT   0x64c
#define PC98_IDE_STATUS   0x64e
#define PC98_IDE_COMMAND  0x64e

#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_CMD_READ 0x20
#define ATA_CMD_WRITE 0x30
#define ATA_CMD_IDENTIFY 0xec

#define PC98_SCSI_ASR       0x0cc0
#define PC98_SCSI_INDIRECT  0x0cc2
#define PC98_SCSI_DMA       0x0cc4
#define PC98_SCSI_DATA      0x0cc6

#define PC98_SCSI_OWN_ID     0x00
#define PC98_SCSI_CONTROL    0x01
#define PC98_SCSI_CDB        0x03
#define PC98_SCSI_TARGET_LUN 0x0f
#define PC98_SCSI_CMD_PHASE  0x10
#define PC98_SCSI_COUNT      0x12
#define PC98_SCSI_DEST_ID    0x15
#define PC98_SCSI_STATUS     0x17
#define PC98_SCSI_COMMAND    0x18

#define PC98_SCSI_ASR_INT    0x80
#define PC98_SCSI_ASR_DBR    0x01
#define PC98_SCSI_RESET_DONE 0x01
#define PC98_SCSI_XFER_DONE  0x16
#define PC98_SCSI_DATA_OUT   0x88
#define PC98_SCSI_DATA_IN    0x89
#define PC98_SCSI_SELECTED   0x11
#define PC98_SCSI_COMMAND_OUT 0x8a
#define PC98_SCSI_STATUS_IN  0x8b
#define PC98_SCSI_MSG_OUT    0x8e
#define PC98_SCSI_MSG_IN     0x8f
#define PC98_SCSI_DISCONNECT 0x85
#define PC98_SCSI_RESET      0x00
#define PC98_SCSI_SELECT_ATN 0x06
#define PC98_SCSI_SELECT_ATN_XFER 0x08
#define PC98_SCSI_SELECT_XFER 0x09
#define PC98_SCSI_TRANSFER_INFO 0x20
#define PC98_SCSI_SINGLE_BYTE 0x80
#define PC98_SCSI_CONTROL_DMA 0x8c
#define PC98_SCSI_ROM_BASE    0x0d2000
#define PC98_BIOS_INT1B_COMPAT_ENTRY 0x000fefc0

#define PC98_DMA0_ADDR       0x01
#define PC98_DMA0_COUNT      0x03
#define PC98_DMA0_MASK       0x15
#define PC98_DMA_MODE        0x17
#define PC98_DMA_CLEAR_FF    0x19
#define PC98_DMA_RESET       0x1b
#define PC98_DMA0_PAGE       0x27

#define LGY98_IOBASE       0x00d0
#define LGY98_ISR          (LGY98_IOBASE + 7)
#define LGY98_RESET        0x03d0
#define LGY98_OLD_RESET    0x00e8
#define LGY98_BOARD_ID_A   0x03da
#define LGY98_BOARD_ID_B   0x03db
#define LGY98_BOARD_ID_C   0x03dc
#define LGY98_BOARD_ID_D   0x03dd

#define PC98_FDC_MSR       0x0090
#define PC98_FDC_FIFO      0x0092
#define PC98_FDC_CTRL      0x0094
#define PC98_KBD_STATUS    0x0043

#define PC98_ROM_BANK_PORT    0x043f
#define PC98_ROM_WINDOW       0xf8000
#define PC98_BASIC_BANK_SEL   0xe4
#define PC98_BASIC_MAGIC_OFF  0x0040

#define PC98_SYS16M_CTRL      0x043b
#define PC98_BIOS_MEMSIZE     0x0401
#define PC98_MODE_FF2         0x006a
#define PC98_MODE_STATUS      0x09a0
#define PC98_PEGC_CONTROL     0x000e0000
#define PC98_PEGC_APERTURE    0x0f00000
#define PC98_PEGC_HIGH_ALIAS  0xfff00000

#define PC98_PCM_CLOCK_PORT   0xa466
#define PC98_PCM_FIFO_PORT    0xa468
#define PC98_PCM_DACTRL_PORT  0xa46a
#define PC98_PCM_DATA_PORT    0xa46c
#define PC98_SLAVE_PIC_CMD    0x0008

#define PCI_CONFIG_ADDRESS    0x0cf8
#define PCI_CONFIG_DATA       0x0cfc
#define PCI_COMMAND           0x04
#define PCI_BASE_ADDRESS_0    0x10
#define PCI_BASE_ADDRESS_4    0x20
#define PCI_INTERRUPT_LINE    0x3c
#define PCI_COMMAND_IO        0x0001
#define PCI_COMMAND_MEMORY    0x0002
#define PCI_COMMAND_MASTER    0x0004

#define PC98_UHCI_DEVFN       (8 << 3)
#define PC98_EHCI_DEVFN       (13 << 3)
#define PC98_PCI_IRQ          14
#define PC98_UHCI_IO_BASE     0x6000
#define PC98_EHCI_MMIO_BASE   0xf1000000
#define EHCI_CAPLENGTH        0x20
#define EHCI_USBSTS           0x04
#define EHCI_USBINTR          0x08
#define EHCI_PORTSC1          0x44
#define EHCI_STS_PCD          0x04

#define FDC_MSR_DRV0_BUSY  0x01
#define FDC_MSR_CMD_BUSY   0x10
#define FDC_MSR_DIO        0x40
#define FDC_MSR_RQM        0x80

#define FDC_CMD_READ_ID    0x4a
#define FDC_CMD_READ_DATA  0x46
#define FDC_ST0_ABNTERM    0x40
#define FDC_ST1_NO_DATA    0x04

#define PC98_FDC_CTRL_MOTOR  0x08
#define PC98_FDC_CTRL_DMA_EN 0x10
#define PC98_FDC_CTRL_NRESET 0x80

#define PC98_DMA2_ADDR       0x09
#define PC98_DMA2_COUNT      0x0b
#define PC98_DMA2_MASK       0x15
#define PC98_DMA_MODE        0x17
#define PC98_DMA_CLEAR_FF    0x19
#define PC98_DMA_RESET       0x1b
#define PC98_DMA2_PAGE       0x23

#define E8390_STOP         0x01
#define E8390_START        0x02
#define E8390_NODMA        0x20

static char *pc98_qtree(const char *machine)
{
    QTestState *qts;
    char *qtree;

    qts = qtest_initf("-machine %s -nodefaults -display none", machine);
    qtree = qtest_hmp(qts, "info qtree");
    qtest_quit(qts);

    return qtree;
}

static void test_pc9801_has_no_pci(void)
{
    g_autofree char *qtree = pc98_qtree("pc9801");

    g_assert_nonnull(qtree);
    g_assert_null(strstr(qtree, "dev: pc98-pcihost"));
    g_assert_null(strstr(qtree, "dev: pc98-coregraph"));
}

static void test_pc9821_has_pci_coregraph(void)
{
    g_autofree char *qtree = pc98_qtree("pc9821");

    g_assert_nonnull(strstr(qtree, "dev: pc98-pcihost"));
    g_assert_nonnull(strstr(qtree, "dev: pc98-coregraph"));
}

static void test_pc9801_low_memory_workarea(void)
{
    QTestState *qts;

    /*
     * 5 MiB of physical address space represents the common 640 KiB
     * conventional plus 4 MiB extended configuration once the PC-98
     * 0xa0000-0xfffff hole is excluded.
     */
    qts = qtest_init(
        "-machine pc9801 -m 5M -nodefaults -display none");
    qtest_outb(qts, PC98_SYS16M_CTRL, 0x04);
    g_assert_cmphex(qtest_readb(qts, PC98_BIOS_MEMSIZE), ==, 0x20);
    qtest_outb(qts, PC98_SYS16M_CTRL, 0x00);
    g_assert_cmphex(qtest_readb(qts, PC98_BIOS_MEMSIZE), ==, 0x20);
    qtest_quit(qts);

    /* The 15-16 MiB system window exists only at the full 16 MiB size. */
    qts = qtest_init(
        "-machine pc9801 -m 16M -nodefaults -display none");
    qtest_outb(qts, PC98_SYS16M_CTRL, 0x04);
    g_assert_cmphex(qtest_readb(qts, PC98_BIOS_MEMSIZE), ==, 0x78);
    qtest_outb(qts, PC98_SYS16M_CTRL, 0x00);
    g_assert_cmphex(qtest_readb(qts, PC98_BIOS_MEMSIZE), ==, 0x70);
    qtest_quit(qts);
}

static void test_pc9821_pegc_selection(void)
{
    QTestState *qts;

    /* PEGC is opt-in: the default releases 15--16 MiB as ordinary RAM. */
    qts = qtest_init(
        "-machine pc9821 -m 16M -nodefaults -display none");
    qtest_outb(qts, PC98_SYS16M_CTRL, 0x04);
    g_assert_cmphex(qtest_inb(qts, PC98_SYS16M_CTRL), ==, 0x04);
    g_assert_cmphex(qtest_readb(qts, PC98_BIOS_MEMSIZE), ==, 0x78);
    qtest_writeb(qts, PC98_PEGC_APERTURE, 0x5a);
    g_assert_cmphex(qtest_readb(qts, PC98_PEGC_APERTURE), ==, 0x5a);
    qtest_quit(qts);

    /* Full PEGC owns the system-space hole and acknowledges its modes. */
    qts = qtest_init(
        "-machine pc9821,pegc=on -m 16M -nodefaults -display none");
    qtest_outb(qts, PC98_SYS16M_CTRL, 0x04);
    g_assert_cmphex(qtest_inb(qts, PC98_SYS16M_CTRL), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, PC98_BIOS_MEMSIZE), ==, 0x70);

    qtest_outb(qts, PC98_MODE_FF2, 0x07);
    qtest_outb(qts, PC98_MODE_FF2, 0x21);
    qtest_outb(qts, PC98_MODE_STATUS, 0x0a);
    g_assert_cmphex(qtest_inb(qts, PC98_MODE_STATUS) & 1, ==, 1);
    qtest_writeb(qts, PC98_PEGC_APERTURE, 0x96);
    g_assert_cmphex(qtest_readb(qts, PC98_PEGC_APERTURE), ==, 0xff);
    qtest_writew(qts, PC98_PEGC_CONTROL + 0x100, 0x80);
    g_assert_cmphex(qtest_readw(qts, PC98_PEGC_CONTROL + 0x100), ==, 0);
    qtest_writew(qts, PC98_PEGC_CONTROL + 0x102, 1);
    g_assert_cmphex(qtest_readw(qts, PC98_PEGC_CONTROL + 0x102), ==, 1);
    qtest_writeb(qts, PC98_PEGC_APERTURE, 0xa5);
    g_assert_cmphex(qtest_readb(qts, PC98_PEGC_APERTURE), ==, 0xa5);
    qtest_writeb(qts, PC98_PEGC_HIGH_ALIAS + 1, 0x3c);
    g_assert_cmphex(qtest_readb(qts, PC98_PEGC_APERTURE + 1), ==, 0x3c);

    qtest_outb(qts, PC98_MODE_FF2, 0x69);
    qtest_outb(qts, PC98_MODE_STATUS, 0x0d);
    g_assert_cmphex(qtest_inb(qts, PC98_MODE_STATUS) & 1, ==, 1);
    qtest_quit(qts);
}

static uint32_t pc98_pci_config_address(uint8_t devfn, uint8_t reg)
{
    return 0x80000000U | ((uint32_t)devfn << 8) | (reg & ~3);
}

static uint32_t pc98_pci_readl(QTestState *qts, uint8_t devfn, uint8_t reg)
{
    qtest_outl(qts, PCI_CONFIG_ADDRESS,
               pc98_pci_config_address(devfn, reg));
    return qtest_inl(qts, PCI_CONFIG_DATA);
}

static void pc98_pci_writel(QTestState *qts, uint8_t devfn, uint8_t reg,
                            uint32_t value)
{
    qtest_outl(qts, PCI_CONFIG_ADDRESS,
               pc98_pci_config_address(devfn, reg));
    qtest_outl(qts, PCI_CONFIG_DATA, value);
}

static void test_pc9821_usb_pci_io_mmio_irq(void)
{
    QTestState *qts;
    uint32_t status;

    qts = qtest_init(
        "-machine pc9821 -usb -nodefaults -display none "
        "-device usb-tablet,bus=usb11.0 "
        "-drive id=usbdrive,if=none,file=null-co://,format=raw");

    g_assert_cmphex(pc98_pci_readl(qts, PC98_UHCI_DEVFN, 0), ==,
                    0x70208086);
    g_assert_cmphex(pc98_pci_readl(qts, PC98_EHCI_DEVFN, 0), ==,
                    0x24cd8086);

    /* Replay the fixed assignments made by the compatibility ITF. */
    pc98_pci_writel(qts, PC98_UHCI_DEVFN, PCI_BASE_ADDRESS_4,
                    PC98_UHCI_IO_BASE | 1);
    pc98_pci_writel(qts, PC98_UHCI_DEVFN, PCI_COMMAND,
                    PCI_COMMAND_IO | PCI_COMMAND_MASTER);
    pc98_pci_writel(qts, PC98_UHCI_DEVFN, PCI_INTERRUPT_LINE,
                    PC98_PCI_IRQ);

    pc98_pci_writel(qts, PC98_EHCI_DEVFN, PCI_BASE_ADDRESS_0,
                    PC98_EHCI_MMIO_BASE);
    pc98_pci_writel(qts, PC98_EHCI_DEVFN, PCI_COMMAND,
                    PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    pc98_pci_writel(qts, PC98_EHCI_DEVFN, PCI_INTERRUPT_LINE,
                    PC98_PCI_IRQ);

    g_assert_cmphex(pc98_pci_readl(qts, PC98_UHCI_DEVFN,
                                   PCI_BASE_ADDRESS_4) & ~0x1f, ==,
                    PC98_UHCI_IO_BASE);
    g_assert_cmphex(pc98_pci_readl(qts, PC98_EHCI_DEVFN,
                                   PCI_BASE_ADDRESS_0) & ~0xfff, ==,
                    PC98_EHCI_MMIO_BASE);
    g_assert_cmphex(pc98_pci_readl(qts, PC98_UHCI_DEVFN,
                                   PCI_INTERRUPT_LINE) & 0xff, ==,
                    PC98_PCI_IRQ);
    g_assert_cmphex(pc98_pci_readl(qts, PC98_EHCI_DEVFN,
                                   PCI_INTERRUPT_LINE) & 0xff, ==,
                    PC98_PCI_IRQ);

    /* UHCI is exposed through PCI I/O and sees the full-speed tablet. */
    g_assert_cmphex(qtest_inw(qts, PC98_UHCI_IO_BASE + 0x10) & 1, ==, 1);

    /* EHCI is CPU-visible MMIO and sees the high-speed storage device. */
    g_assert_cmphex(qtest_readb(qts, PC98_EHCI_MMIO_BASE), ==,
                    EHCI_CAPLENGTH);
    g_assert_cmphex(qtest_readw(qts, PC98_EHCI_MMIO_BASE + 2), ==, 0x0100);

    /*
     * Enable port-change IRQ before hotplug.  Connecting high-speed storage
     * must then reach slave PIC IR6, the PC-98 IRQ14 input.  This also
     * exercises PCI INTx pin-D routing.
     */
    qtest_writel(qts, PC98_EHCI_MMIO_BASE + EHCI_CAPLENGTH + EHCI_USBINTR,
                 EHCI_STS_PCD);
    qtest_qmp_device_add(qts, "usb-storage", "usbdev0",
                         "{'bus':'usb20.0','drive':'usbdrive'}");
    g_assert_cmphex(qtest_readl(qts, PC98_EHCI_MMIO_BASE + EHCI_CAPLENGTH +
                                EHCI_PORTSC1) & 1, ==, 1);
    status = qtest_readl(qts, PC98_EHCI_MMIO_BASE + EHCI_CAPLENGTH +
                         EHCI_USBSTS);
    g_assert_cmphex(status & EHCI_STS_PCD, ==, EHCI_STS_PCD);
    qtest_outb(qts, PC98_SLAVE_PIC_CMD, 0x0a);
    g_assert_cmphex(qtest_inb(qts, PC98_SLAVE_PIC_CMD) & 0x40, ==, 0x40);

    qtest_quit(qts);
}

static void test_pc98_lgy98_port_map(void)
{
    QTestState *qts;

    qts = qtest_init(
        "-machine pc9821 -nodefaults -display none "
        "-netdev user,id=n0 -device pc98-lgy98,netdev=n0");

    g_assert_cmphex(qtest_inb(qts, LGY98_BOARD_ID_A), ==, 0x00);
    g_assert_cmphex(qtest_inb(qts, LGY98_BOARD_ID_B), ==, 0x40);
    g_assert_cmphex(qtest_inb(qts, LGY98_BOARD_ID_C), ==, 0x26);
    g_assert_cmphex(qtest_inb(qts, LGY98_BOARD_ID_D), ==, 0x0b);

    qtest_outb(qts, LGY98_IOBASE, E8390_NODMA | E8390_START);
    qtest_outb(qts, LGY98_ISR, 0xff);
    g_assert_cmphex(qtest_inb(qts, LGY98_IOBASE), ==,
                    E8390_NODMA | E8390_START);
    g_assert_cmphex(qtest_inb(qts, LGY98_ISR), ==, 0x00);

    /* The old base+0x18 mapping must not reset the controller. */
    g_assert_cmphex(qtest_inb(qts, LGY98_OLD_RESET), ==, 0xff);
    g_assert_cmphex(qtest_inb(qts, LGY98_ISR), ==, 0x00);

    g_assert_cmphex(qtest_inb(qts, LGY98_RESET), ==, 0x00);
    g_assert_cmphex(qtest_inb(qts, LGY98_ISR), ==, 0x80);

    qtest_quit(qts);
}

static void test_pc98_fdc_empty_read_id(void)
{
    QTestState *qts;
    uint8_t result[7];
    int i;

    qts = qtest_init(
        "-machine pc9821 -nodefaults -display none");

    /*
     * Windows 9x probes an empty PC-98 floppy drive with READ ID.  A uPD765A
     * reports an error result and keeps the selected unit's busy bit set
     * while those seven result bytes are waiting to be drained.
     */
    g_assert_cmphex(qtest_inb(qts, PC98_FDC_MSR), ==, FDC_MSR_RQM);
    qtest_outb(qts, PC98_FDC_FIFO, FDC_CMD_READ_ID);
    qtest_outb(qts, PC98_FDC_FIFO, 0x00);

    g_assert_cmphex(qtest_inb(qts, PC98_FDC_MSR), ==,
                    FDC_MSR_RQM | FDC_MSR_DIO | FDC_MSR_CMD_BUSY |
                    FDC_MSR_DRV0_BUSY);
    for (i = 0; i < G_N_ELEMENTS(result); i++) {
        result[i] = qtest_inb(qts, PC98_FDC_FIFO);
    }
    g_assert_cmphex(result[0], ==, FDC_ST0_ABNTERM);
    g_assert_cmphex(result[1], ==, FDC_ST1_NO_DATA);
    g_assert_cmphex(result[2], ==, 0x00);
    g_assert_cmphex(qtest_inb(qts, PC98_FDC_MSR), ==, FDC_MSR_RQM);

    qtest_quit(qts);
}

static void test_pc98_keyboard_status(void)
{
    QTestState *qts;

    qts = qtest_init("-machine pc9801 -nodefaults -display none");

    /* The PC-98 keyboard 8251 has TX ready/empty and DSR asserted. */
    g_assert_cmphex(qtest_inb(qts, PC98_KBD_STATUS) & 0x85, ==, 0x85);

    qtest_quit(qts);
}

static void test_pc98_fdc_2hd_1024_pio_read(void)
{
    g_autoptr(GError) err = NULL;
    g_autofree char *image = NULL;
    QTestState *qts;
    uint8_t sector[1024];
    uint8_t result[7];
    const unsigned int cylinder = 3;
    const unsigned int head = 1;
    const unsigned int sector_id = 5;
    const size_t image_size = 77 * 2 * 8 * sizeof(sector);
    const size_t sector_offset =
        ((cylinder * 2 + head) * 8 + sector_id - 1) * sizeof(sector);
    int fd;
    int i;

    fd = g_file_open_tmp("qemu-pc98-fdc-XXXXXX", &image, &err);
    g_assert_no_error(err);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, image_size), ==, 0);
    for (i = 0; i < sizeof(sector); i++) {
        sector[i] = (i * 37 + 0x5a) & 0xff;
    }
    g_assert_cmpint(lseek(fd, sector_offset, SEEK_SET), ==, sector_offset);
    g_assert_cmpint(qemu_write_full(fd, sector, sizeof(sector)), ==,
                    sizeof(sector));
    close(fd);

    qts = qtest_initf(
        "-machine pc9801 -nodefaults -display none "
        "-drive if=floppy,unit=0,format=raw,file=%s",
        image);

    /* PC-98 BIOS-style polled PIO: motor and reset, IRQ/DMA disabled. */
    qtest_outb(qts, PC98_FDC_CTRL,
               PC98_FDC_CTRL_MOTOR | PC98_FDC_CTRL_NRESET);
    qtest_outb(qts, PC98_FDC_FIFO, FDC_CMD_READ_DATA);
    qtest_outb(qts, PC98_FDC_FIFO, 0x00); /* unit 0, head 0 select */
    qtest_outb(qts, PC98_FDC_FIFO, cylinder);
    qtest_outb(qts, PC98_FDC_FIFO, head);
    qtest_outb(qts, PC98_FDC_FIFO, sector_id);
    qtest_outb(qts, PC98_FDC_FIFO, 3); /* 1024-byte physical sector */
    /* PC-98 polled PIO still completes one sector when EOT is track end. */
    qtest_outb(qts, PC98_FDC_FIFO, 8);
    qtest_outb(qts, PC98_FDC_FIFO, 0x2a);
    qtest_outb(qts, PC98_FDC_FIFO, 0xff);

    for (i = 0; i < sizeof(sector); i++) {
        g_assert_cmphex(qtest_inb(qts, PC98_FDC_MSR) &
                        (FDC_MSR_RQM | FDC_MSR_DIO | FDC_MSR_CMD_BUSY), ==,
                        FDC_MSR_RQM | FDC_MSR_DIO | FDC_MSR_CMD_BUSY);
        g_assert_cmphex(qtest_inb(qts, PC98_FDC_FIFO), ==, sector[i]);
    }
    for (i = 0; i < G_N_ELEMENTS(result); i++) {
        result[i] = qtest_inb(qts, PC98_FDC_FIFO);
    }
    g_assert_cmphex(result[0] & 0xc0, ==, 0x00);
    g_assert_cmphex(result[1], ==, 0x00);
    g_assert_cmphex(result[2], ==, 0x00);
    g_assert_cmphex(result[3], ==, cylinder);
    g_assert_cmphex(result[4], ==, head);
    g_assert_cmphex(result[5], ==, sector_id);
    g_assert_cmphex(result[6], ==, 3);
    g_assert_cmphex(qtest_inb(qts, PC98_FDC_MSR), ==, FDC_MSR_RQM);

    /* BIOS SENSE reissues SPECIFY and READ ID after data transfers. */
    qtest_outb(qts, PC98_FDC_FIFO, 0x03);
    qtest_outb(qts, PC98_FDC_FIFO, 0xdf);
    qtest_outb(qts, PC98_FDC_FIFO, 0x03);
    qtest_outb(qts, PC98_FDC_FIFO, FDC_CMD_READ_ID);
    qtest_outb(qts, PC98_FDC_FIFO, 0x00);
    for (i = 0; i < G_N_ELEMENTS(result); i++) {
        result[i] = qtest_inb(qts, PC98_FDC_FIFO);
    }
    g_assert_cmphex(result[0] & 0xc0, ==, 0x00);
    g_assert_cmphex(result[1], ==, 0x00);
    g_assert_cmphex(result[2], ==, 0x00);
    g_assert_cmphex(result[6], ==, 3);
    g_assert_cmphex(qtest_inb(qts, PC98_FDC_MSR), ==, FDC_MSR_RQM);

    qtest_quit(qts);
    g_assert_cmpint(g_remove(image), ==, 0);
}

static void test_pc98_fdc_2hd_1024_dma_read(void)
{
    g_autoptr(GError) err = NULL;
    g_autofree char *image = NULL;
    QTestState *qts;
    uint8_t expected[4 * 1024];
    uint8_t actual[4 * 1024];
    const unsigned int cylinder = 3;
    const unsigned int head = 1;
    const unsigned int sector_id = 5;
    const size_t sector_size = 1024;
    const size_t image_size = 77 * 2 * 8 * sector_size;
    const size_t sector_offset =
        ((cylinder * 2 + head) * 8 + sector_id - 1) * sector_size;
    const uint16_t dma_address = 0x8000;
    const uint16_t dma_count = sizeof(expected) - 1;
    int fd;
    int i;

    fd = g_file_open_tmp("qemu-pc98-fdc-dma-XXXXXX", &image, &err);
    g_assert_no_error(err);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, image_size), ==, 0);
    for (i = 0; i < sizeof(expected); i++) {
        expected[i] = (i * 53 + 0xc3) & 0xff;
    }
    g_assert_cmpint(lseek(fd, sector_offset, SEEK_SET), ==, sector_offset);
    g_assert_cmpint(qemu_write_full(fd, expected, sizeof(expected)), ==,
                    sizeof(expected));
    close(fd);

    qts = qtest_initf(
        "-machine pc9801 -nodefaults -display none "
        "-drive if=floppy,unit=0,format=raw,file=%s",
        image);

    qtest_outb(qts, PC98_DMA_RESET, 0);
    qtest_outb(qts, PC98_DMA_CLEAR_FF, 0);
    qtest_outb(qts, PC98_DMA2_ADDR, dma_address & 0xff);
    qtest_outb(qts, PC98_DMA2_ADDR, dma_address >> 8);
    qtest_outb(qts, PC98_DMA2_COUNT, dma_count & 0xff);
    qtest_outb(qts, PC98_DMA2_COUNT, dma_count >> 8);
    qtest_outb(qts, PC98_DMA2_PAGE, 0);
    qtest_outb(qts, PC98_DMA_MODE, 0x46); /* single, device-to-memory, ch2 */
    qtest_outb(qts, PC98_DMA2_MASK, 0x02); /* unmask channel 2 */

    qtest_outb(qts, PC98_FDC_CTRL,
               PC98_FDC_CTRL_MOTOR | PC98_FDC_CTRL_DMA_EN |
               PC98_FDC_CTRL_NRESET);
    qtest_outb(qts, PC98_FDC_FIFO, FDC_CMD_READ_DATA);
    qtest_outb(qts, PC98_FDC_FIFO, 0x00);
    qtest_outb(qts, PC98_FDC_FIFO, cylinder);
    qtest_outb(qts, PC98_FDC_FIFO, head);
    qtest_outb(qts, PC98_FDC_FIFO, sector_id);
    qtest_outb(qts, PC98_FDC_FIFO, 3);
    qtest_outb(qts, PC98_FDC_FIFO, 8); /* sectors 5 through 8 */
    qtest_outb(qts, PC98_FDC_FIFO, 0x2a);
    qtest_outb(qts, PC98_FDC_FIFO, 0xff);

    qtest_clock_step(qts, 1000000);
    qtest_memread(qts, dma_address, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));

    qtest_quit(qts);
    g_assert_cmpint(g_remove(image), ==, 0);
}

static void test_pc98_basic_rom_bank(void)
{
    static const uint8_t expected[] = "PC98BAS1";
    QTestState *qts;
    uint8_t magic[sizeof(expected) - 1];

    qts = qtest_init(
        "-machine pc9801 -nodefaults -display none");

    qtest_outb(qts, PC98_ROM_BANK_PORT, PC98_BASIC_BANK_SEL);
    qtest_memread(qts, PC98_ROM_WINDOW + PC98_BASIC_MAGIC_OFF,
                  magic, sizeof(magic));
    g_assert_cmpmem(magic, sizeof(magic), expected, sizeof(expected) - 1);

    qtest_quit(qts);
}

static void test_pc98_opna_pcm_fifo_irq(void)
{
    QTestState *qts;
    int i;

    qts = qtest_init(
        "-machine pc9821 -nodefaults -display none "
        "-audiodev none,id=snd0 -device pc98-opna,audiodev=snd0");

    /* Reset FIFO, select signed 16-bit stereo and a 128-byte watermark. */
    qtest_outb(qts, PC98_PCM_FIFO_PORT, 0x08);
    qtest_outb(qts, PC98_PCM_DACTRL_PORT, 0x32);
    qtest_outb(qts, PC98_PCM_FIFO_PORT, 0x26);
    qtest_outb(qts, PC98_PCM_DACTRL_PORT, 0x00);
    qtest_outb(qts, PC98_PCM_FIFO_PORT, 0xa6);

    for (i = 0; i < 256; i++) {
        qtest_outb(qts, PC98_PCM_DATA_PORT, i);
    }
    g_assert_cmphex(qtest_inb(qts, PC98_PCM_CLOCK_PORT) & 0xc0, ==, 0);

    /*
     * 0xA6 selects 5501.25 Hz.  Advancing 100 ms drains the test data,
     * crosses the watermark and latches the shared IRQ12 request.
     */
    qtest_clock_step(qts, 100 * 1000 * 1000);
    g_assert_cmphex(qtest_inb(qts, PC98_PCM_CLOCK_PORT) & 0x40, ==, 0x40);
    g_assert_cmphex(qtest_inb(qts, PC98_PCM_FIFO_PORT) & 0x10, ==, 0x10);
    qtest_outb(qts, PC98_SLAVE_PIC_CMD, 0x0a); /* OCW3: read IRR */
    g_assert_cmphex(qtest_inb(qts, PC98_SLAVE_PIC_CMD) & 0x10, ==, 0x10);

    qtest_outb(qts, PC98_PCM_FIFO_PORT, 0xa6);
    g_assert_cmphex(qtest_inb(qts, PC98_PCM_FIFO_PORT) & 0x10, ==, 0);

    qtest_quit(qts);
}

static void pc98_ide_wait_drq(QTestState *qts)
{
    int i;

    for (i = 0; i < 10000; i++) {
        uint8_t status = qtest_inb(qts, PC98_IDE_STATUS);

        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) {
            return;
        }
        qtest_clock_step(qts, 1000);
    }
    g_assert_not_reached();
}

static void pc98_ide_read_data(QTestState *qts, uint8_t *buf)
{
    int i;

    pc98_ide_wait_drq(qts);
    for (i = 0; i < 256; i++) {
        uint16_t value = qtest_inw(qts, PC98_IDE_DATA);

        stw_le_p(buf + i * 2, value);
    }
}

static void pc98_ide_read_sector(QTestState *qts, uint32_t lba, uint8_t *buf)
{
    qtest_outb(qts, PC98_IDE_NSECTOR, 1);
    qtest_outb(qts, PC98_IDE_SECTOR, lba);
    qtest_outb(qts, PC98_IDE_LCYL, lba >> 8);
    qtest_outb(qts, PC98_IDE_HCYL, lba >> 16);
    qtest_outb(qts, PC98_IDE_SELECT, 0xe0 | ((lba >> 24) & 0x0f));
    qtest_outb(qts, PC98_IDE_COMMAND, ATA_CMD_READ);
    pc98_ide_read_data(qts, buf);
}

static void pc98_ide_write_sector(QTestState *qts, uint32_t lba,
                                  const uint8_t *buf)
{
    int i;

    qtest_outb(qts, PC98_IDE_NSECTOR, 1);
    qtest_outb(qts, PC98_IDE_SECTOR, lba);
    qtest_outb(qts, PC98_IDE_LCYL, lba >> 8);
    qtest_outb(qts, PC98_IDE_HCYL, lba >> 16);
    qtest_outb(qts, PC98_IDE_SELECT, 0xe0 | ((lba >> 24) & 0x0f));
    qtest_outb(qts, PC98_IDE_COMMAND, ATA_CMD_WRITE);
    pc98_ide_wait_drq(qts);
    for (i = 0; i < 256; i++) {
        qtest_outw(qts, PC98_IDE_DATA, lduw_le_p(buf + i * 2));
    }
}

static void pc98_ide_identify(QTestState *qts, uint8_t *buf)
{
    qtest_outb(qts, PC98_IDE_SELECT, 0xa0);
    qtest_outb(qts, PC98_IDE_COMMAND, ATA_CMD_IDENTIFY);
    pc98_ide_read_data(qts, buf);
}

static void pc98_scsi_reg_select(QTestState *qts, uint8_t reg)
{
    qtest_outb(qts, PC98_SCSI_ASR, reg);
}

static void pc98_scsi_reg_write(QTestState *qts, uint8_t reg, uint8_t value)
{
    pc98_scsi_reg_select(qts, reg);
    qtest_outb(qts, PC98_SCSI_INDIRECT, value);
}

static uint8_t pc98_scsi_reg_read(QTestState *qts, uint8_t reg)
{
    pc98_scsi_reg_select(qts, reg);
    return qtest_inb(qts, PC98_SCSI_INDIRECT);
}

static void pc98_scsi_reg_write_buf(QTestState *qts, uint8_t reg,
                                    const uint8_t *buf, size_t len)
{
    size_t i;

    pc98_scsi_reg_select(qts, reg);
    for (i = 0; i < len; i++) {
        qtest_outb(qts, PC98_SCSI_INDIRECT, buf[i]);
    }
}

static void pc98_scsi_wait_asr(QTestState *qts, uint8_t mask,
                               uint8_t expected)
{
    int i;

    for (i = 0; i < 10000; i++) {
        if ((qtest_inb(qts, PC98_SCSI_ASR) & mask) == expected) {
            return;
        }
        qtest_clock_step(qts, 1000);
    }
    g_assert_not_reached();
}

static void pc98_scsi_setup_command(QTestState *qts, const uint8_t *cdb,
                                    size_t cdb_len, uint32_t byte_count)
{
    uint8_t count[3] = {
        byte_count >> 16,
        byte_count >> 8,
        byte_count,
    };

    pc98_scsi_reg_write(qts, PC98_SCSI_DEST_ID, 0);
    pc98_scsi_reg_write(qts, PC98_SCSI_TARGET_LUN, 0);
    pc98_scsi_reg_write_buf(qts, PC98_SCSI_COUNT, count, sizeof(count));
    pc98_scsi_reg_write_buf(qts, PC98_SCSI_CDB, cdb, cdb_len);
    pc98_scsi_reg_write(qts, PC98_SCSI_COMMAND, PC98_SCSI_SELECT_XFER);
}

/* Program the exact WD33C93 sequence used by Linux's PC-9801-55/92 driver. */
static void pc98_scsi_setup_linux_dma_command(QTestState *qts,
                                               const uint8_t *cdb,
                                               size_t cdb_len,
                                               uint32_t byte_count)
{
    uint8_t count[3] = {
        byte_count >> 16,
        byte_count >> 8,
        byte_count,
    };

    pc98_scsi_reg_write(qts, PC98_SCSI_DEST_ID, 0);
    pc98_scsi_reg_write(qts, PC98_SCSI_TARGET_LUN, 0);
    pc98_scsi_reg_write(qts, PC98_SCSI_CMD_PHASE, 0);
    pc98_scsi_reg_write_buf(qts, PC98_SCSI_CDB, cdb, cdb_len);
    pc98_scsi_reg_write(qts, PC98_SCSI_OWN_ID, cdb_len);
    qtest_outb(qts, PC98_SCSI_DMA, 0x01);
    pc98_scsi_reg_write_buf(qts, PC98_SCSI_COUNT, count, sizeof(count));
    pc98_scsi_reg_write(qts, PC98_SCSI_CONTROL, PC98_SCSI_CONTROL_DMA);
    pc98_scsi_reg_write(qts, PC98_SCSI_COMMAND,
                        PC98_SCSI_SELECT_ATN_XFER);
}

static void pc98_scsi_resume_linux_dma(QTestState *qts,
                                       uint32_t byte_count)
{
    uint8_t count[3] = {
        byte_count >> 16,
        byte_count >> 8,
        byte_count,
    };

    /* pc980155_dma_setup() opens the board gate before wd33c93.c resumes. */
    qtest_outb(qts, PC98_SCSI_DMA, 0x01);
    pc98_scsi_reg_write(qts, PC98_SCSI_CONTROL, PC98_SCSI_CONTROL_DMA);
    pc98_scsi_reg_write_buf(qts, PC98_SCSI_COUNT, count, sizeof(count));
    pc98_scsi_reg_write(qts, PC98_SCSI_CMD_PHASE, 0x45);
    pc98_scsi_reg_write(qts, PC98_SCSI_COMMAND,
                        PC98_SCSI_SELECT_ATN_XFER);
}

static void pc98_scsi_finish_command(QTestState *qts, uint8_t expected_status)
{
    uint8_t csr;
    uint8_t status;

    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_INT, PC98_SCSI_ASR_INT);
    csr = pc98_scsi_reg_read(qts, PC98_SCSI_STATUS);
    status = pc98_scsi_reg_read(qts, PC98_SCSI_TARGET_LUN);
    g_assert_cmphex(csr, ==, PC98_SCSI_XFER_DONE);
    g_assert_cmphex(status, ==, expected_status);
}

static uint8_t pc98_scsi_expect_irq(QTestState *qts, uint8_t expected_csr)
{
    uint8_t csr;

    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_INT, PC98_SCSI_ASR_INT);
    csr = pc98_scsi_reg_read(qts, PC98_SCSI_STATUS);
    g_assert_cmphex(csr, ==, expected_csr);
    return csr;
}

static void pc98_scsi_set_count(QTestState *qts, uint32_t byte_count)
{
    uint8_t count[3] = {
        byte_count >> 16,
        byte_count >> 8,
        byte_count,
    };

    pc98_scsi_reg_write_buf(qts, PC98_SCSI_COUNT, count, sizeof(count));
}

/* Exercise the non-combination, polled-PIO sequence used by Linux/98. */
static void pc98_scsi_linux_pio_start(QTestState *qts, const uint8_t *cdb,
                                      size_t cdb_len, uint8_t data_csr)
{
    size_t i;

    pc98_scsi_reg_write(qts, PC98_SCSI_DEST_ID, 0);
    pc98_scsi_reg_write(qts, PC98_SCSI_TARGET_LUN, 0);
    pc98_scsi_reg_write(qts, PC98_SCSI_COMMAND, PC98_SCSI_SELECT_ATN);
    g_test_message("  waiting for SELECTED");
    pc98_scsi_expect_irq(qts, PC98_SCSI_SELECTED);
    g_test_message("  waiting for MESSAGE OUT");
    pc98_scsi_expect_irq(qts, PC98_SCSI_MSG_OUT);

    pc98_scsi_set_count(qts, 1);
    pc98_scsi_reg_write(qts, PC98_SCSI_COMMAND,
                        PC98_SCSI_TRANSFER_INFO);
    g_test_message("  waiting for MESSAGE OUT DBR");
    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_DBR, PC98_SCSI_ASR_DBR);
    qtest_outb(qts, PC98_SCSI_DATA, 0x80); /* IDENTIFY, LUN 0 */
    g_test_message("  waiting for COMMAND OUT");
    pc98_scsi_expect_irq(qts, PC98_SCSI_COMMAND_OUT);

    pc98_scsi_set_count(qts, cdb_len);
    pc98_scsi_reg_write(qts, PC98_SCSI_COMMAND,
                        PC98_SCSI_TRANSFER_INFO);
    g_test_message("  waiting for COMMAND OUT DBR");
    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_DBR, PC98_SCSI_ASR_DBR);
    for (i = 0; i < cdb_len; i++) {
        qtest_outb(qts, PC98_SCSI_DATA, cdb[i]);
    }
    g_test_message("  waiting for DATA phase");
    pc98_scsi_expect_irq(qts, data_csr);
}

static void pc98_scsi_linux_pio_finish(QTestState *qts)
{
    uint8_t status;
    uint8_t message;

    pc98_scsi_expect_irq(qts, PC98_SCSI_STATUS_IN);
    pc98_scsi_reg_write(qts, PC98_SCSI_COMMAND,
                        PC98_SCSI_TRANSFER_INFO | PC98_SCSI_SINGLE_BYTE);
    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_DBR, PC98_SCSI_ASR_DBR);
    status = qtest_inb(qts, PC98_SCSI_DATA);
    g_assert_cmphex(status, ==, 0);

    pc98_scsi_expect_irq(qts, PC98_SCSI_MSG_IN);
    pc98_scsi_reg_write(qts, PC98_SCSI_COMMAND,
                        PC98_SCSI_TRANSFER_INFO | PC98_SCSI_SINGLE_BYTE);
    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_DBR, PC98_SCSI_ASR_DBR);
    message = qtest_inb(qts, PC98_SCSI_DATA);
    g_assert_cmphex(message, ==, 0); /* COMMAND COMPLETE */
    pc98_scsi_expect_irq(qts, PC98_SCSI_DISCONNECT);
}

static void pc98_scsi_setup_dma0(QTestState *qts, uint16_t address,
                                 uint16_t byte_count, uint8_t mode)
{
    uint16_t count = byte_count - 1;

    qtest_outb(qts, PC98_DMA_RESET, 0);
    qtest_outb(qts, PC98_DMA_CLEAR_FF, 0);
    qtest_outb(qts, PC98_DMA0_ADDR, address);
    qtest_outb(qts, PC98_DMA0_ADDR, address >> 8);
    qtest_outb(qts, PC98_DMA0_COUNT, count);
    qtest_outb(qts, PC98_DMA0_COUNT, count >> 8);
    qtest_outb(qts, PC98_DMA0_PAGE, 0);
    qtest_outb(qts, PC98_DMA_MODE, mode);
    qtest_outb(qts, PC98_DMA0_MASK, 0);
}

static void test_pc98_scsi_pio_dma_rw(void)
{
    g_autoptr(GError) err = NULL;
    g_autofree char *image = NULL;
    QTestState *qts;
    uint8_t write_cdb[10] = { 0x2a, 0, 0, 0, 0, 7, 0, 0, 1, 0 };
    uint8_t read_cdb[10] = { 0x28, 0, 0, 0, 0, 7, 0, 0, 1, 0 };
    uint8_t tur_cdb[6] = { 0 };
    uint8_t expected[512];
    uint8_t actual[512];
    uint8_t segmented_expected[1024];
    uint8_t segmented_actual[1024];
    const uint32_t dma_address = 0x8000;
    int fd;
    int i;

    fd = g_file_open_tmp("qemu-pc98-scsi-XXXXXX", &image, &err);
    g_assert_no_error(err);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 1024 * 1024), ==, 0);
    close(fd);

    qts = qtest_initf(
        "-machine pc9801 -nodefaults -display none "
        "-drive if=scsi,format=raw,file=%s",
        image);

    pc98_scsi_reg_write(qts, PC98_SCSI_COMMAND, PC98_SCSI_RESET);
    qtest_clock_step(qts, 100000);
    g_assert_cmphex(pc98_scsi_reg_read(qts, PC98_SCSI_STATUS), ==,
                    PC98_SCSI_RESET_DONE);

    /* Image-backed targets become ready after the ROM's initial TUR. */
    pc98_scsi_setup_command(qts, tur_cdb, sizeof(tur_cdb), 0);
    pc98_scsi_finish_command(qts, 0);

    for (i = 0; i < sizeof(expected); i++) {
        expected[i] = i * 37 + 0x5a;
    }
    pc98_scsi_setup_command(qts, write_cdb, sizeof(write_cdb),
                            sizeof(expected));
    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_DBR, PC98_SCSI_ASR_DBR);
    for (i = 0; i < sizeof(expected); i++) {
        qtest_outb(qts, PC98_SCSI_DATA, expected[i]);
    }
    pc98_scsi_finish_command(qts, 0);

    pc98_scsi_setup_command(qts, read_cdb, sizeof(read_cdb),
                            sizeof(actual));
    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_DBR, PC98_SCSI_ASR_DBR);
    for (i = 0; i < sizeof(actual); i++) {
        actual[i] = qtest_inb(qts, PC98_SCSI_DATA);
    }
    pc98_scsi_finish_command(qts, 0);
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));

    write_cdb[5] = 8;
    read_cdb[5] = 8;
    for (i = 0; i < sizeof(expected); i++) {
        expected[i] = i * 53 + 0xc3;
    }
    qtest_memwrite(qts, dma_address, expected, sizeof(expected));
    /*
     * Use the single-transfer modes programmed by Linux's PC-9801-55/92
     * driver, rather than relying on the otherwise equivalent demand-mode
     * values.  This keeps the qtest sequence identical to a physical-board
     * transfer through the PC-98 DMA controller.
     */
    pc98_scsi_setup_dma0(qts, dma_address, sizeof(expected), 0x48);
    pc98_scsi_setup_linux_dma_command(qts, write_cdb, sizeof(write_cdb),
                                      sizeof(expected));
    pc98_scsi_finish_command(qts, 0);

    memset(actual, 0, sizeof(actual));
    qtest_memwrite(qts, dma_address, actual, sizeof(actual));
    pc98_scsi_setup_dma0(qts, dma_address, sizeof(actual), 0x44);
    pc98_scsi_setup_linux_dma_command(qts, read_cdb, sizeof(read_cdb),
                                      sizeof(actual));
    pc98_scsi_finish_command(qts, 0);
    qtest_memread(qts, dma_address, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));

    /*
     * A block request may be larger than the current Linux scatter-gather
     * segment.  The WD count reaches zero after the first segment while the
     * SCSI request remains active, then command phase 45h resumes it.
     */
    write_cdb[5] = 9;
    write_cdb[8] = 2;
    read_cdb[5] = 9;
    read_cdb[8] = 2;
    for (i = 0; i < sizeof(segmented_expected); i++) {
        segmented_expected[i] = i * 29 + 0x71;
    }

    qtest_memwrite(qts, dma_address, segmented_expected, 512);
    pc98_scsi_setup_dma0(qts, dma_address, 512, 0x48);
    pc98_scsi_setup_linux_dma_command(qts, write_cdb, sizeof(write_cdb),
                                      512);
    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_INT, PC98_SCSI_ASR_INT);
    g_assert_cmphex(pc98_scsi_reg_read(qts, PC98_SCSI_STATUS), ==,
                    PC98_SCSI_DATA_OUT);
    qtest_memwrite(qts, dma_address + 512, segmented_expected + 512, 512);
    pc98_scsi_setup_dma0(qts, dma_address + 512, 512, 0x48);
    pc98_scsi_resume_linux_dma(qts, 512);
    pc98_scsi_finish_command(qts, 0);

    memset(segmented_actual, 0, sizeof(segmented_actual));
    qtest_memwrite(qts, dma_address, segmented_actual,
                   sizeof(segmented_actual));
    pc98_scsi_setup_dma0(qts, dma_address, 512, 0x44);
    pc98_scsi_setup_linux_dma_command(qts, read_cdb, sizeof(read_cdb), 512);
    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_INT, PC98_SCSI_ASR_INT);
    g_assert_cmphex(pc98_scsi_reg_read(qts, PC98_SCSI_STATUS), ==,
                    PC98_SCSI_DATA_IN);
    pc98_scsi_setup_dma0(qts, dma_address + 512, 512, 0x44);
    pc98_scsi_resume_linux_dma(qts, 512);
    pc98_scsi_finish_command(qts, 0);
    qtest_memread(qts, dma_address, segmented_actual,
                  sizeof(segmented_actual));
    g_assert_cmpmem(segmented_actual, sizeof(segmented_actual),
                    segmented_expected, sizeof(segmented_expected));

    /*
     * Repeat a two-segment transfer through the non-combination PIO path.
     * This catches both regressions seen with the Linux driver: clearing DBR
     * when TRANSFER INFO starts, and failing to interrupt when the WD count
     * reaches zero before the backend's larger buffer is exhausted.
     */
    write_cdb[5] = 11;
    read_cdb[5] = 11;
    qtest_outb(qts, PC98_SCSI_DMA, 0x02);
    for (i = 0; i < sizeof(segmented_expected); i++) {
        segmented_expected[i] = i * 43 + 0x19;
    }

    g_test_message("Linux-style segmented PIO WRITE: select and command");
    pc98_scsi_linux_pio_start(qts, write_cdb, sizeof(write_cdb),
                              PC98_SCSI_DATA_OUT);
    pc98_scsi_set_count(qts, 512);
    pc98_scsi_reg_write(qts, PC98_SCSI_COMMAND,
                        PC98_SCSI_TRANSFER_INFO);
    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_DBR, PC98_SCSI_ASR_DBR);
    for (i = 0; i < 512; i++) {
        qtest_outb(qts, PC98_SCSI_DATA, segmented_expected[i]);
    }
    g_test_message("Linux-style segmented PIO WRITE: first boundary");
    pc98_scsi_expect_irq(qts, PC98_SCSI_DATA_OUT);
    pc98_scsi_set_count(qts, 512);
    pc98_scsi_reg_write(qts, PC98_SCSI_COMMAND,
                        PC98_SCSI_TRANSFER_INFO);
    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_DBR, PC98_SCSI_ASR_DBR);
    for (i = 512; i < sizeof(segmented_expected); i++) {
        qtest_outb(qts, PC98_SCSI_DATA, segmented_expected[i]);
    }
    g_test_message("Linux-style segmented PIO WRITE: completion");
    pc98_scsi_linux_pio_finish(qts);

    memset(segmented_actual, 0, sizeof(segmented_actual));
    g_test_message("Linux-style segmented PIO READ: select and command");
    pc98_scsi_linux_pio_start(qts, read_cdb, sizeof(read_cdb),
                              PC98_SCSI_DATA_IN);
    pc98_scsi_set_count(qts, 512);
    pc98_scsi_reg_write(qts, PC98_SCSI_COMMAND,
                        PC98_SCSI_TRANSFER_INFO);
    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_DBR, PC98_SCSI_ASR_DBR);
    for (i = 0; i < 512; i++) {
        segmented_actual[i] = qtest_inb(qts, PC98_SCSI_DATA);
    }
    g_test_message("Linux-style segmented PIO READ: first boundary");
    pc98_scsi_expect_irq(qts, PC98_SCSI_DATA_IN);
    pc98_scsi_set_count(qts, 512);
    pc98_scsi_reg_write(qts, PC98_SCSI_COMMAND,
                        PC98_SCSI_TRANSFER_INFO);
    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_DBR, PC98_SCSI_ASR_DBR);
    for (i = 512; i < sizeof(segmented_actual); i++) {
        segmented_actual[i] = qtest_inb(qts, PC98_SCSI_DATA);
    }
    g_test_message("Linux-style segmented PIO READ: completion");
    pc98_scsi_linux_pio_finish(qts);
    g_assert_cmpmem(segmented_actual, sizeof(segmented_actual),
                    segmented_expected, sizeof(segmented_expected));

    qtest_quit(qts);
    g_assert_cmpint(g_remove(image), ==, 0);
}

static void test_pc98_scsi_free_bios_rom(void)
{
    static const uint8_t signature[] = { 0x55, 0xaa, 0x02 };
    QTestState *qts;
    uint8_t actual[sizeof(signature)];
    uint8_t opcode;

    qts = qtest_init(
        "-machine pc9801 -nodefaults -display none "
        "-drive if=scsi,format=raw,file=null-co://");
    qtest_memread(qts, PC98_SCSI_ROM_BASE + 9, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), signature, sizeof(signature));

    /* The SCSI ROM chains IDE/FDD requests through the fixed FD80:17C0 ABI. */
    qtest_outb(qts, PC98_ROM_BANK_PORT, 0xee); /* map firmware bank 7 */
    qtest_memread(qts, PC98_BIOS_INT1B_COMPAT_ENTRY, &opcode,
                  sizeof(opcode));
    g_assert_cmphex(opcode, ==, 0xe9); /* near JMP to the live dispatcher */
    qtest_quit(qts);
}

static void test_pc98_scsi_cd_read(void)
{
    g_autoptr(GError) err = NULL;
    g_autofree char *image = NULL;
    QTestState *qts;
    uint8_t inquiry_cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t capacity_cdb[10] = { 0x25 };
    uint8_t sense_cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    uint8_t read_cdb[10] = { 0x28, 0, 0, 0, 0, 16, 0, 0, 1, 0 };
    uint8_t pvd[2048] = { 0x01, 'C', 'D', '0', '0', '1', 0x01 };
    uint8_t buf[2048];
    int fd;
    int i;

    fd = g_file_open_tmp("qemu-pc98-scsi-cd-XXXXXX", &image, &err);
    g_assert_no_error(err);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 64 * 2048), ==, 0);
    g_assert_cmpint(lseek(fd, 16 * 2048, SEEK_SET), ==, 16 * 2048);
    g_assert_cmpint(qemu_write_full(fd, pvd, sizeof(pvd)), ==,
                    sizeof(pvd));
    close(fd);

    qts = qtest_initf(
        "-machine pc9801 -nodefaults -display none "
        "-drive if=scsi,media=cdrom,readonly=on,format=raw,file=%s",
        image);

    pc98_scsi_reg_write(qts, PC98_SCSI_COMMAND, PC98_SCSI_RESET);
    qtest_clock_step(qts, 100000);
    g_assert_cmphex(pc98_scsi_reg_read(qts, PC98_SCSI_STATUS), ==,
                    PC98_SCSI_RESET_DONE);

    pc98_scsi_setup_command(qts, inquiry_cdb, sizeof(inquiry_cdb), 36);
    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_DBR, PC98_SCSI_ASR_DBR);
    for (i = 0; i < 36; i++) {
        buf[i] = qtest_inb(qts, PC98_SCSI_DATA);
    }
    pc98_scsi_finish_command(qts, 0);
    g_assert_cmphex(buf[0] & 0x1f, ==, 0x05);
    g_assert_cmphex(buf[1] & 0x80, ==, 0x80);

    /* The first media command reports power-on Unit Attention. */
    pc98_scsi_setup_command(qts, capacity_cdb, sizeof(capacity_cdb), 8);
    pc98_scsi_finish_command(qts, 0x02);
    pc98_scsi_setup_command(qts, sense_cdb, sizeof(sense_cdb), 18);
    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_DBR, PC98_SCSI_ASR_DBR);
    for (i = 0; i < 18; i++) {
        buf[i] = qtest_inb(qts, PC98_SCSI_DATA);
    }
    pc98_scsi_finish_command(qts, 0);

    pc98_scsi_setup_command(qts, capacity_cdb, sizeof(capacity_cdb), 8);
    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_DBR, PC98_SCSI_ASR_DBR);
    for (i = 0; i < 8; i++) {
        buf[i] = qtest_inb(qts, PC98_SCSI_DATA);
    }
    pc98_scsi_finish_command(qts, 0);
    g_assert_cmphex(buf[3], ==, 63);
    g_assert_cmphex(buf[6], ==, 0x08);
    g_assert_cmphex(buf[7], ==, 0x00);

    pc98_scsi_setup_command(qts, read_cdb, sizeof(read_cdb), sizeof(buf));
    pc98_scsi_wait_asr(qts, PC98_SCSI_ASR_DBR, PC98_SCSI_ASR_DBR);
    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = qtest_inb(qts, PC98_SCSI_DATA);
    }
    pc98_scsi_finish_command(qts, 0);
    g_assert_cmpmem(buf, 7, pvd, 7);

    qtest_quit(qts);
    g_assert_cmpint(g_remove(image), ==, 0);
}

static void test_pc98_vvfat_boot_layout(void)
{
    g_autoptr(GError) err = NULL;
    g_autofree char *dirname = g_dir_make_tmp("qemu-vvfat98-XXXXXX", &err);
    g_autofree char *ordinary = NULL;
    g_autofree char *msdos = NULL;
    g_autofree char *io = NULL;
    g_autofree char *io_contents = g_malloc0(65536);
    QTestState *qts;
    uint8_t identify[512], ipl[512], pbr[512], root[512], data[512] = { 0 };
    uint32_t hidden, fat_start, root_lba, data_start;
    uint16_t bytes_per_sector, sectors_per_fat, root_entries;
    uint8_t sectors_per_cluster, number_of_fats;
    uint16_t io_cluster, msdos_cluster, ordinary_cluster = 0;
    g_autofree char *ordinary_contents = NULL;
    gsize ordinary_length;
    int offset;

    g_assert_no_error(err);
    g_assert_nonnull(dirname);

    /*
     * Create the ordinary file first.  vvfat98 must still allocate the two
     * DOS system files first rather than depending on host readdir() order.
     */
    ordinary = g_build_filename(dirname, "ordinary.txt", NULL);
    msdos = g_build_filename(dirname, "MSDOS.SYS", NULL);
    io = g_build_filename(dirname, "IO.SYS", NULL);
    g_assert_true(g_file_set_contents(ordinary, "x", 1, &err));
    g_assert_no_error(err);
    g_assert_true(g_file_set_contents(msdos, "M", 1, &err));
    g_assert_no_error(err);
    g_assert_true(g_file_set_contents(io, io_contents, 65536, &err));
    g_assert_no_error(err);

    qts = qtest_initf(
        "-machine pc9821 -nodefaults -display none "
        "-drive file=fat98:rw:%s,format=raw,if=none,id=d0 "
        "-device ide-hd,drive=d0,bus=ide.0,unit=0",
        dirname);

    pc98_ide_identify(qts, identify);
    g_assert_cmpuint(lduw_le_p(identify + 2), >, 0);
    g_assert_cmpuint(lduw_le_p(identify + 6), ==, 8);
    g_assert_cmpuint(lduw_le_p(identify + 12), ==, 17);

    pc98_ide_read_sector(qts, 0, ipl);
    g_assert_cmpmem(ipl + 4, 4, "IPL1", 4);
    g_assert_cmpuint(lduw_le_p(ipl + 0x1f6), ==, 0xaa55);
    g_assert_cmpuint(lduw_le_p(ipl + 0x1fe), ==, 0xaa55);

    pc98_ide_read_sector(qts, 136, pbr);
    g_assert_cmpuint(pbr[0], ==, 0xeb);
    g_assert_cmpuint(pbr[1], ==, 0x46);
    bytes_per_sector = lduw_le_p(pbr + 0x0b);
    sectors_per_cluster = pbr[0x0d];
    number_of_fats = pbr[0x10];
    root_entries = lduw_le_p(pbr + 0x11);
    sectors_per_fat = lduw_le_p(pbr + 0x16);
    hidden = ldl_le_p(pbr + 0x1c);

    g_assert_cmpuint(bytes_per_sector, ==, 1024);
    g_assert_cmpuint(hidden, ==, 136);
    g_assert_cmpuint(ldl_le_p(pbr + 0x3e), ==, hidden);
    g_assert_cmpuint(lduw_le_p(pbr + 0x44), ==, 512);

    fat_start = hidden + lduw_le_p(pbr + 0x0e) *
                bytes_per_sector / 512;
    root_lba = fat_start + number_of_fats * sectors_per_fat *
               bytes_per_sector / 512;
    data_start = root_lba + root_entries * 32 / 512;
    g_assert_cmpuint(lduw_le_p(pbr + 0x42), ==, data_start - hidden);

    pc98_ide_read_sector(qts, root_lba, root);
    g_assert_cmpuint(root[0x0b], ==, 0x28);
    g_assert_cmpmem(root + 32, 11, "IO      SYS", 11);
    g_assert_cmpmem(root + 64, 11, "MSDOS   SYS", 11);
    io_cluster = lduw_le_p(root + 32 + 0x1a);
    msdos_cluster = lduw_le_p(root + 64 + 0x1a);
    g_assert_cmpuint(io_cluster, ==, 2);
    g_assert_cmpuint(msdos_cluster, ==,
                     io_cluster + 65536 /
                     (bytes_per_sector * sectors_per_cluster));

    for (offset = 0; offset < sizeof(root); offset += 32) {
        if (!memcmp(root + offset, "ORDINARYTXT", 11)) {
            ordinary_cluster = lduw_le_p(root + offset + 0x1a);
            break;
        }
    }
    g_assert_cmpuint(ordinary_cluster, >=, 2);
    data[0] = 'Z';
    pc98_ide_write_sector(
        qts,
        data_start + (ordinary_cluster - 2) *
        sectors_per_cluster * bytes_per_sector / 512,
        data);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(ordinary, &ordinary_contents,
                                      &ordinary_length, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(ordinary_length, ==, 1);
    g_assert_cmpuint(ordinary_contents[0], ==, 'Z');
    g_clear_pointer(&io, g_free);
    g_clear_pointer(&msdos, g_free);
    io = g_build_filename(dirname, "io.sys", NULL);
    msdos = g_build_filename(dirname, "msdos.sys", NULL);

    /*
     * IO.SYS is optional for an ordinary data disk.  Removing both system
     * files must not prevent the directory-backed drive from being opened.
     */
    g_assert_cmpint(g_remove(io), ==, 0);
    g_assert_cmpint(g_remove(msdos), ==, 0);
    qts = qtest_initf(
        "-machine pc9821 -nodefaults -display none "
        "-drive file=fat98:rw:%s,format=raw,if=none,id=d0 "
        "-device ide-hd,drive=d0,bus=ide.0,unit=0",
        dirname);
    pc98_ide_read_sector(qts, 0, ipl);
    g_assert_cmpmem(ipl + 4, 4, "IPL1", 4);
    pc98_ide_read_sector(qts, 136, pbr);
    g_assert_cmpuint(lduw_le_p(pbr + 0x0b), ==, 1024);
    qtest_quit(qts);

    g_assert_cmpint(g_remove(ordinary), ==, 0);
    g_assert_cmpint(g_rmdir(dirname), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/pc98/pc9801/no-pci-coregraph",
                   test_pc9801_has_no_pci);
    qtest_add_func("/pc98/pc9821/pci-coregraph",
                   test_pc9821_has_pci_coregraph);
    qtest_add_func("/pc98/pc9801/low-memory-workarea",
                   test_pc9801_low_memory_workarea);
    qtest_add_func("/pc98/pc9821/pegc-selection",
                   test_pc9821_pegc_selection);
    qtest_add_func("/pc98/pc9821/usb-pci-io-mmio-irq",
                   test_pc9821_usb_pci_io_mmio_irq);
    qtest_add_func("/pc98/lgy98/port-map",
                   test_pc98_lgy98_port_map);
    qtest_add_func("/pc98/fdc/empty-read-id",
                   test_pc98_fdc_empty_read_id);
    qtest_add_func("/pc98/fdc/2hd-1024-pio-read",
                   test_pc98_fdc_2hd_1024_pio_read);
    qtest_add_func("/pc98/fdc/2hd-1024-dma-read",
                   test_pc98_fdc_2hd_1024_dma_read);
    qtest_add_func("/pc98/keyboard/status",
                   test_pc98_keyboard_status);
    qtest_add_func("/pc98/basic/rom-bank",
                   test_pc98_basic_rom_bank);
    qtest_add_func("/pc98/opna/pcm-fifo-irq",
                   test_pc98_opna_pcm_fifo_irq);
    qtest_add_func("/pc98/vvfat98/boot-layout",
                   test_pc98_vvfat_boot_layout);
    qtest_add_func("/pc98/scsi/pio-dma-rw",
                   test_pc98_scsi_pio_dma_rw);
    qtest_add_func("/pc98/scsi/cd-read",
                   test_pc98_scsi_cd_read);
    qtest_add_func("/pc98/scsi/free-bios-rom",
                   test_pc98_scsi_free_bios_rom);

    return g_test_run();
}
