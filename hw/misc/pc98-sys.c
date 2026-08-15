/*
 * QEMU NEC PC-9821 system port
 *
 * uPD4990A calendar clock (bit-serial), system 8255 PPI (ports
 * 0x31-0x37), printer 8255 PPI (ports 0x40-0x46), free-running
 * time-stamp counter (0x5c/0x5e) and the software DIP switches
 * (SDIP).
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
#include "qemu/bcd.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/isa/isa.h"
#include "hw/misc/pc98-sys.h"
#include "system/address-spaces.h"
#include "system/rtc.h"
#include "migration/vmstate.h"
#include "trace.h"

/* Serial control lines on the calendar-clock command port (0x20). */
enum {
    CAL_STB  = 0x08,
    CAL_CLK  = 0x10,
    CAL_DATA = 0x20,
};

/* Calendar-clock command codes (low three bits of the command latch). */
enum {
    CAL_CMD_SHIFT    = 0x01,
    CAL_CMD_TIMESET  = 0x02,
    CAL_CMD_TIMEREAD = 0x03,
    CAL_CMD_EXTEND   = 0x07,
};

/* Mode bit selecting the uPD4993A-style extra output nibble. */
#define CAL_MODE_4993   0x20

#define TIMESTAMP_HZ    307200
#define TICK_PERIOD_NS  15625000LL   /* 1/64 second */
#define HRTIME_ADDR     0x04f1
#define HRTIME_DAY_TICKS (24 * 60 * 60 * 32)

struct Pc98SysState {
    ISADevice parent_obj;

    /* uPD4990A calendar clock (bit-serial interface) */
    uint8_t  cal_mode;
    uint8_t  cal_port;
    uint8_t  cal_cmd;
    uint64_t cal_out;
    uint64_t cal_shift;
    uint8_t  cal_shift_count;
    uint8_t  cal_cmd_shift;
    int64_t  rtc_offset;
    uint8_t  tick_mode;
    uint8_t  tick_count;
    uint8_t  hrtime_count;
    bool     hrtime_initialized;
    bool     tick_irq_level;

    /* system 8255 PPI (ports 0x31-0x37) */
    uint8_t sys_a;
    uint8_t sys_b;
    uint8_t sys_c;
    uint8_t sys_ctrlword;
    int     sys_c_probe;
    qemu_irq serial_irq_enable[3];

    /* printer 8255 PPI (ports 0x40-0x46) */
    uint8_t prn_a;
    uint8_t prn_b;
    uint8_t prn_c;
    uint8_t prn_ctrlword;

    /* software DIP switches */
    uint8_t sdip[24];
    uint8_t sdip_bank;

    bool sysclock_5mhz;

    PortioList portio_list;
    QEMUTimer *tick_timer;
    qemu_irq irq;
    qemu_irq speaker;
    int64_t timestamp_origin;
};

/* --- uPD4990A calendar clock --- */

/*
 * Periodic interrupt.  Port 0x128 selects the rate:
 *   0 -> 1/64 s, 1 -> 1/32 s, 2 -> disabled, 3 -> 1/16 s.
 * This handler runs every 1/64 s, so an interrupt is raised once every
 * (mode + 1) invocations unless the source is disabled.
 */
static void pc98_sys_hrtime_init(Pc98SysState *s)
{
    struct tm now;
    uint32_t ticks;
    int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    qemu_get_timedate(&now, s->rtc_offset);
    ticks = ((now.tm_hour * 60 + now.tm_min) * 60 + now.tm_sec) * 32;
    ticks += (ns % NANOSECONDS_PER_SECOND) * 32 / NANOSECONDS_PER_SECOND;

    /*
     * Bits 23:22 are a modulo-four elapsed-day field.  Real PC-9821
     * firmware initializes it to one; Windows 98 also depends on it not
     * starting at zero.
     */
    address_space_stl_le(&address_space_memory, HRTIME_ADDR,
                         ticks | 0x400000, MEMTXATTRS_UNSPECIFIED, NULL);
    s->hrtime_initialized = true;
}

static void pc98_sys_hrtime_advance(Pc98SysState *s)
{
    uint32_t ticks;

    if (!s->hrtime_initialized) {
        pc98_sys_hrtime_init(s);
        return;
    }

    ticks = address_space_ldl_le(&address_space_memory, HRTIME_ADDR,
                                 MEMTXATTRS_UNSPECIFIED, NULL);
    ticks++;
    if ((ticks & 0x3fffff) >= HRTIME_DAY_TICKS) {
        ticks = ((ticks & ~0x3fffff) + 0x400000) & 0xffffff;
    }
    address_space_stl_le(&address_space_memory, HRTIME_ADDR, ticks,
                         MEMTXATTRS_UNSPECIFIED, NULL);
}

static void pc98_sys_tick(void *opaque)
{
    Pc98SysState *s = opaque;
    static const uint8_t periods[4] = { 1, 2, 0, 4 };
    uint8_t period = periods[s->tick_mode & 0x03];

    if (period) {
        if (++s->tick_count >= period) {
            qemu_set_irq(s->irq, 1);
            s->tick_irq_level = true;
            s->tick_count = 0;
            trace_pc98_sys_hrtimer_irq(s->tick_mode);
        }
    }

    /* The BIOS work-area clock always advances at 32 Hz. */
    if (++s->hrtime_count >= 2) {
        pc98_sys_hrtime_advance(s);
        s->hrtime_count = 0;
    }

    timer_mod(s->tick_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TICK_PERIOD_NS);
}

/* Latch the host time-of-day into the serial output register as packed BCD. */
static void cal_latch_time(Pc98SysState *s)
{
    struct tm now;

    qemu_get_timedate(&now, s->rtc_offset);
    s->cal_out  = (uint64_t)to_bcd(now.tm_sec);
    s->cal_out |= (uint64_t)to_bcd(now.tm_min)  << 8;
    s->cal_out |= (uint64_t)to_bcd(now.tm_hour) << 16;
    s->cal_out |= (uint64_t)to_bcd(now.tm_mday) << 24;
    s->cal_out |= (uint64_t)now.tm_wday << 32;
    s->cal_out |= (uint64_t)(now.tm_mon + 1) << 36;
    s->cal_out |= (uint64_t)to_bcd(now.tm_year % 100) << 40;
    if (s->cal_mode & CAL_MODE_4993) {
        s->cal_out <<= 4;
    }
}

static void cal_set_time(Pc98SysState *s)
{
    struct tm now;
    uint64_t value = s->cal_shift;
    int year;

    if (s->cal_shift_count < 48) {
        return;
    }

    qemu_get_timedate(&now, s->rtc_offset);
    now.tm_sec = from_bcd(value & 0xff);
    now.tm_min = from_bcd((value >> 8) & 0xff);
    now.tm_hour = from_bcd((value >> 16) & 0xff);
    now.tm_mday = from_bcd((value >> 24) & 0xff);
    now.tm_mon = ((value >> 36) & 0x0f) - 1;
    year = from_bcd((value >> 40) & 0xff);
    now.tm_year = year + (year < 80 ? 100 : 0);
    now.tm_isdst = -1;
    s->rtc_offset = qemu_timedate_diff(&now);

    /* Keep the high-resolution work-area clock consistent with the RTC. */
    s->hrtime_initialized = false;
    pc98_sys_hrtime_init(s);
}

static void cal_serial_write(void *opaque, uint32_t addr, uint32_t value)
{
    Pc98SysState *s = opaque;

    /* A command is captured on the falling edge of the strobe line. */
    if ((s->cal_port & CAL_STB) && !(value & CAL_STB)) {
        s->cal_cmd = s->cal_port & 0x07;
        if (s->cal_cmd == CAL_CMD_TIMEREAD) {
            cal_latch_time(s);
        } else if (s->cal_cmd == CAL_CMD_SHIFT) {
            s->cal_shift = 0;
            s->cal_shift_count = 0;
        } else if (s->cal_cmd == CAL_CMD_TIMESET) {
            cal_set_time(s);
        } else if (s->cal_cmd == CAL_CMD_EXTEND) {
            /* extended form: the real opcode arrived over the serial line */
            s->cal_cmd = s->cal_cmd_shift & 0x0f;
            if (s->cal_cmd == CAL_CMD_TIMEREAD) {
                cal_latch_time(s);
            }
        }
    }
    /* Each falling clock edge shifts one bit in and one bit out. */
    if ((s->cal_port & CAL_CLK) && !(value & CAL_CLK)) {
        uint8_t data_in = (s->cal_port & CAL_DATA) != 0;

        if (s->cal_cmd == CAL_CMD_SHIFT && s->cal_shift_count < 64) {
            s->cal_shift |= (uint64_t)data_in << s->cal_shift_count;
            s->cal_shift_count++;
        } else {
            s->cal_cmd_shift = (s->cal_cmd_shift | (data_in << 4)) >> 1;
        }
        s->cal_out >>= 1;
    }
    s->cal_port = value;
}

static void cal_mode_write(void *opaque, uint32_t addr, uint32_t value)
{
    Pc98SysState *s = opaque;

    s->cal_mode = value;
}

static uint32_t cal_mode_read(void *opaque, uint32_t addr)
{
    Pc98SysState *s = opaque;

    return s->cal_mode;
}

static void cal_tick_ctl_write(void *opaque, uint32_t addr, uint32_t value)
{
    Pc98SysState *s = opaque;

    s->tick_mode = value & 0x03;
    s->tick_count = 0;
    qemu_set_irq(s->irq, 0);
    s->tick_irq_level = false;
    trace_pc98_sys_hrtimer_write(value, s->tick_mode);
}

static uint32_t cal_tick_ctl_read(void *opaque, uint32_t addr)
{
    Pc98SysState *s = opaque;
    uint8_t value = 0x80 | s->tick_mode;

    qemu_set_irq(s->irq, 0);        /* reading acknowledges the interrupt */
    s->tick_irq_level = false;
    trace_pc98_sys_hrtimer_read(value);
    return value;
}

/* --- system 8255 PPI --- */

static void pc98_sysppi_a_write(void *opaque, uint32_t addr, uint32_t value)
{
    Pc98SysState *s = opaque;

    s->sys_a = value;
}

static uint32_t pc98_sysppi_a_read(void *opaque, uint32_t addr)
{
    Pc98SysState *s = opaque;

    return (s->sys_ctrlword & 0x10) ? 0x73 : s->sys_a;
}

static void pc98_sysppi_b_write(void *opaque, uint32_t addr, uint32_t value)
{
    Pc98SysState *s = opaque;

    s->sys_b = value;
}

static uint32_t pc98_sysppi_b_read(void *opaque, uint32_t addr)
{
    Pc98SysState *s = opaque;

    /* In serial-readback mode bit 0 exposes the calendar output bit. */
    if (s->sys_ctrlword & 0x02) {
        return 0xf8 | (s->cal_out & 1);
    }
    return s->sys_a;
}

static void pc98_sysppi_c_write(void *opaque, uint32_t addr, uint32_t value)
{
    Pc98SysState *s = opaque;
    int i;

    s->sys_c = value;
    /* PC-98 uses active-low port-C bit 3 for the PIT channel-1 speaker. */
    qemu_set_irq(s->speaker, !(value & 0x08));
    for (i = 0; i < ARRAY_SIZE(s->serial_irq_enable); i++) {
        qemu_set_irq(s->serial_irq_enable[i], (value >> i) & 1);
    }
}

static uint32_t pc98_sysppi_c_read(void *opaque, uint32_t addr)
{
    Pc98SysState *s = opaque;

    /*
     * During ITF "protect mode" probing the firmware expects bit 5 to show up
     * a few reads into the sequence.  Count the probe down and assert the bit
     * at the two points the firmware samples it.
     */
    if (s->sys_c_probe) {
        s->sys_c_probe--;
        if (s->sys_c_probe == 0 || s->sys_c_probe == 4) {
            s->sys_c |= 0x20;
        }
    }
    return s->sys_c;
}

static void pc98_sysppi_ctrl_write(void *opaque, uint32_t addr, uint32_t value)
{
    Pc98SysState *s = opaque;
    uint32_t bit;

    if (value & 0x80) {
        s->sys_ctrlword = value;        /* mode-set control word */
        return;
    }
    /* 8255 bit set/reset: bits 3..1 pick the port-C bit, bit 0 the level */
    bit = 1u << ((value >> 1) & 7);
    pc98_sysppi_c_write(s, 0, (value & 1) ? (s->sys_c | bit)
                                          : (s->sys_c & ~bit));
}

bool pc98_sys_shutdown_armed(Pc98SysState *s)
{
    return (s->sys_c & 0xa0) == 0xa0;
}

uint8_t pc98_sys_get_portc(Pc98SysState *s)
{
    return s->sys_c;
}

/* --- printer 8255 PPI --- */

static void pc98_prnppi_a_write(void *opaque, uint32_t addr, uint32_t value)
{
    Pc98SysState *s = opaque;

    s->prn_a = value;
}

static uint32_t pc98_prnppi_a_read(void *opaque, uint32_t addr)
{
    Pc98SysState *s = opaque;

    return s->prn_a;
}

static void pc98_prnppi_b_write(void *opaque, uint32_t addr, uint32_t value)
{
    Pc98SysState *s = opaque;

    s->prn_b = value;
}

static uint32_t pc98_prnppi_b_read(void *opaque, uint32_t addr)
{
    Pc98SysState *s = opaque;

    /* System-clock strap: 5 MHz series reads 0x94, 8 MHz series 0xb4. */
    if (s->prn_ctrlword & 0x02) {
        return s->sysclock_5mhz ? 0x94 : 0xb4;
    }
    return s->prn_b;
}

static void pc98_prnppi_c_write(void *opaque, uint32_t addr, uint32_t value)
{
    Pc98SysState *s = opaque;

    s->prn_c = value;
}

static uint32_t pc98_prnppi_c_read(void *opaque, uint32_t addr)
{
    Pc98SysState *s = opaque;

    return s->prn_c;
}

static void pc98_prnppi_ctrl_write(void *opaque, uint32_t addr, uint32_t value)
{
    Pc98SysState *s = opaque;
    uint8_t bit;

    if (value & 0x80) {
        s->prn_ctrlword = value;
        return;
    }
    bit = 1u << ((value >> 1) & 7);
    pc98_prnppi_c_write(s, 0, (value & 1) ? (s->prn_c | bit)
                                          : (s->prn_c & ~bit));
}

/* --- free-running time-stamp counter --- */

static uint32_t pc98_timestamp_read(void *opaque, uint32_t addr)
{
    Pc98SysState *s = opaque;
    uint64_t ticks = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                              s->timestamp_origin,
                              TIMESTAMP_HZ, NANOSECONDS_PER_SECOND);

    switch (addr) {
    case 0x5c:
        return ticks & 0xffff;
    case 0x5d:
        return (ticks >> 8) & 0xff;
    case 0x5e:
        return (ticks >> 8) & 0xffff;
    case 0x5f:
        return (ticks >> 16) & 0xff;
    }
    return 0xffff;
}

static void pc98_wait_write(void *opaque, uint32_t addr, uint32_t value)
{
}

/* --- software DIP switches --- */

/* The port address nibble plus the bank flag select one of 24 SDIP bytes. */
static uint8_t sdip_index(Pc98SysState *s, uint32_t addr)
{
    uint8_t nibble = (addr >> 8) & 0x0f;

    return (s->sdip_bank & 0x40) ? nibble + 8 : nibble - 4;
}

static void pc98_sdip_write(void *opaque, uint32_t addr, uint32_t value)
{
    Pc98SysState *s = opaque;

    s->sdip[sdip_index(s, addr)] = value;
}

static uint32_t pc98_sdip_read(void *opaque, uint32_t addr)
{
    Pc98SysState *s = opaque;

    return s->sdip[sdip_index(s, addr)];
}

static void pc98_sdip_bank_write(void *opaque, uint32_t addr, uint32_t value)
{
    Pc98SysState *s = opaque;

    s->sdip_bank = value;
}

/* --- QOM device --- */

static const MemoryRegionPortio pc98_sys_portio[] = {
    { 0x20, 1, 1, .write = cal_serial_write },
    { 0x22, 1, 1, .read = cal_mode_read, .write = cal_mode_write },
    { 0x31, 1, 1, .read = pc98_sysppi_a_read, .write = pc98_sysppi_a_write },
    { 0x33, 1, 1, .read = pc98_sysppi_b_read, .write = pc98_sysppi_b_write },
    { 0x35, 1, 1, .read = pc98_sysppi_c_read, .write = pc98_sysppi_c_write },
    { 0x37, 1, 1, .write = pc98_sysppi_ctrl_write },
    { 0x40, 1, 1, .read = pc98_prnppi_a_read, .write = pc98_prnppi_a_write },
    { 0x42, 1, 1, .read = pc98_prnppi_b_read, .write = pc98_prnppi_b_write },
    { 0x44, 1, 1, .read = pc98_prnppi_c_read, .write = pc98_prnppi_c_write },
    { 0x46, 1, 1, .write = pc98_prnppi_ctrl_write },
    { 0x5c, 1, 1, .read = pc98_timestamp_read },
    { 0x5c, 2, 2, .read = pc98_timestamp_read },
    { 0x5d, 1, 1, .read = pc98_timestamp_read },
    { 0x5e, 1, 1, .read = pc98_timestamp_read },
    { 0x5e, 2, 2, .read = pc98_timestamp_read },
    { 0x5f, 1, 1, .read = pc98_timestamp_read, .write = pc98_wait_write },
    { 0x128, 1, 1, .read = cal_tick_ctl_read, .write = cal_tick_ctl_write },
    { 0x841e, 1, 1, .read = pc98_sdip_read, .write = pc98_sdip_write },
    { 0x851e, 1, 1, .read = pc98_sdip_read, .write = pc98_sdip_write },
    { 0x861e, 1, 1, .read = pc98_sdip_read, .write = pc98_sdip_write },
    { 0x871e, 1, 1, .read = pc98_sdip_read, .write = pc98_sdip_write },
    { 0x881e, 1, 1, .read = pc98_sdip_read, .write = pc98_sdip_write },
    { 0x891e, 1, 1, .read = pc98_sdip_read, .write = pc98_sdip_write },
    { 0x8a1e, 1, 1, .read = pc98_sdip_read, .write = pc98_sdip_write },
    { 0x8b1e, 1, 1, .read = pc98_sdip_read, .write = pc98_sdip_write },
    { 0x8c1e, 1, 1, .read = pc98_sdip_read, .write = pc98_sdip_write },
    { 0x8d1e, 1, 1, .read = pc98_sdip_read, .write = pc98_sdip_write },
    { 0x8e1e, 1, 1, .read = pc98_sdip_read, .write = pc98_sdip_write },
    { 0x8f1e, 1, 1, .read = pc98_sdip_read, .write = pc98_sdip_write },
    { 0x8f1f, 1, 1, .write = pc98_sdip_bank_write },
    { 0xf0f6, 1, 1, .write = pc98_sdip_bank_write },
    PORTIO_END_OF_LIST(),
};

static const uint8_t sdip_default[] = {
    0x7c, 0x73, 0x76, 0x3e, 0xdc, 0x7f, 0xff, 0xbf, 0x7f, 0x7f, 0x49, 0x98,
    0x8f, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
};

static void pc98_sys_reset(DeviceState *dev)
{
    Pc98SysState *s = PC98_SYS(dev);

    s->cal_mode = 0xff;
    s->cal_port = 0;
    s->cal_cmd = 0;
    /* PC-9821 starts the high-resolution timer at 32 Hz. */
    s->tick_mode = 1;
    s->tick_count = 0;
    s->hrtime_count = 0;
    s->hrtime_initialized = false;
    qemu_set_irq(s->irq, 0);
    s->tick_irq_level = false;

    s->sys_a = 0x00;
    s->sys_b = 0x00;
    s->sys_c = 0xff;
    s->sys_c_probe = 8;
    s->sys_ctrlword = 0x92;
    pc98_sysppi_c_write(s, 0, s->sys_c);

    s->prn_a = 0xff;
    s->prn_b = 0x00;
    s->prn_c = 0x81;
    s->prn_ctrlword = 0x82;

    s->sdip_bank = 0;
}

static int pc98_sys_post_load(void *opaque, int version_id)
{
    Pc98SysState *s = opaque;

    qemu_set_irq(s->irq, s->tick_irq_level);
    pc98_sysppi_c_write(s, 0, s->sys_c);
    return 0;
}

static const VMStateDescription vmstate_pc98_sys = {
    .name = "pc98-sys",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pc98_sys_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(cal_mode, Pc98SysState),
        VMSTATE_UINT8(cal_port, Pc98SysState),
        VMSTATE_UINT8(cal_cmd, Pc98SysState),
        VMSTATE_UINT64(cal_out, Pc98SysState),
        VMSTATE_UINT64(cal_shift, Pc98SysState),
        VMSTATE_UINT8(cal_shift_count, Pc98SysState),
        VMSTATE_UINT8(cal_cmd_shift, Pc98SysState),
        VMSTATE_INT64(rtc_offset, Pc98SysState),
        VMSTATE_UINT8(tick_mode, Pc98SysState),
        VMSTATE_UINT8(tick_count, Pc98SysState),
        VMSTATE_UINT8(hrtime_count, Pc98SysState),
        VMSTATE_BOOL(hrtime_initialized, Pc98SysState),
        VMSTATE_BOOL(tick_irq_level, Pc98SysState),
        VMSTATE_UINT8(sys_a, Pc98SysState),
        VMSTATE_UINT8(sys_b, Pc98SysState),
        VMSTATE_UINT8(sys_c, Pc98SysState),
        VMSTATE_UINT8(sys_ctrlword, Pc98SysState),
        VMSTATE_INT32(sys_c_probe, Pc98SysState),
        VMSTATE_UINT8(prn_a, Pc98SysState),
        VMSTATE_UINT8(prn_b, Pc98SysState),
        VMSTATE_UINT8(prn_c, Pc98SysState),
        VMSTATE_UINT8(prn_ctrlword, Pc98SysState),
        VMSTATE_UINT8_ARRAY(sdip, Pc98SysState, 24),
        VMSTATE_UINT8(sdip_bank, Pc98SysState),
        VMSTATE_TIMER_PTR(tick_timer, Pc98SysState),
        VMSTATE_INT64(timestamp_origin, Pc98SysState),
        VMSTATE_END_OF_LIST()
    }
};

static void pc98_sys_realize(DeviceState *dev, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(dev);
    Pc98SysState *s = PC98_SYS(dev);

    memcpy(s->sdip, sdip_default, sizeof(s->sdip));

    isa_register_portio_list(isadev, &s->portio_list, 0,
                             pc98_sys_portio, s, "pc98-sys");

    qdev_init_gpio_out(dev, &s->irq, 1);
    qdev_init_gpio_out_named(dev, &s->speaker, "speaker", 1);
    qdev_init_gpio_out_named(dev, s->serial_irq_enable,
                             "serial-irq-enable",
                             ARRAY_SIZE(s->serial_irq_enable));

    s->tick_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pc98_sys_tick, s);
    timer_mod(s->tick_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TICK_PERIOD_NS);
    s->timestamp_origin = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static const Property pc98_sys_properties[] = {
    DEFINE_PROP_BOOL("sysclock-5mhz", Pc98SysState, sysclock_5mhz, true),
};

static void pc98_sys_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pc98_sys_realize;
    dc->vmsd = &vmstate_pc98_sys;
    device_class_set_legacy_reset(dc, pc98_sys_reset);
    device_class_set_props(dc, pc98_sys_properties);
    /* Wired by board code (IRQ 15), not user creatable */
    dc->user_creatable = false;
}

static const TypeInfo pc98_sys_info = {
    .name          = TYPE_PC98_SYS,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(Pc98SysState),
    .class_init    = pc98_sys_class_init,
};

static void pc98_sys_register_types(void)
{
    type_register_static(&pc98_sys_info);
}

type_init(pc98_sys_register_types)
