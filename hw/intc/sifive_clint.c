/*
 * SiFive CLINT (Core Local Interruptor)
 * FZI: This is more or less a copy of the RISC-V ACLINT, but in one device
 *
 * Copyright (c) 2016-2017 Sagar Karandikar, sagark@eecs.berkeley.edu
 * Copyright (c) 2017 SiFive, Inc.
 * Copyright (c) 2021 Western Digital Corporation or its affiliates.
 *
 * This provides real-time clock, timer and interprocessor interrupts.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "target/riscv/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/intc/sifive_clint.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

// #define CLINT_LOG_VERBOSE

#ifdef CLINT_LOG_VERBOSE
#define LOG_DEBUG(fmt, ...) qemu_log(fmt,  ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ;
#endif

// #define CLINT_WARN

#ifdef CLINT_WARN
#define LOG_WARN(fmt, ...) qemu_log(fmt,  ##__VA_ARGS__)
#else
#define LOG_WARN(fmt, ...) ;
#endif

typedef struct sifive_clint_mtimer_callback {
    SifiveClintState *s;
    int num;
} sifive_clint_mtimer_callback;

static void riscv_cpu_set_rdtime_fn(CPURISCVState *env,
                                    uint64_t (*fn)(void *),
                                    void *arg)
{
    env->rdtime_fn = fn;
    env->rdtime_fn_arg = arg;
}

static uint64_t cpu_riscv_read_rtc_raw(uint32_t timebase_freq)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
        timebase_freq, NANOSECONDS_PER_SECOND);
}

static uint64_t cpu_riscv_read_rtc(void *opaque)
{
    SifiveClintState *mtimer = opaque;
    return cpu_riscv_read_rtc_raw(mtimer->timebase_freq) + mtimer->time_delta;
}

/*
 * Called when timecmp is written to update the QEMU timer or immediately
 * trigger timer interrupt if mtimecmp <= current timer value.
 */
static void sifive_clint_mtimer_write_timecmp(SifiveClintState *mtimer,
                                              RISCVCPU *cpu,
                                              int hartid,
                                              uint64_t value)
{
    uint32_t timebase_freq = mtimer->timebase_freq;
    uint64_t next;
    uint64_t diff;

    uint64_t rtc_r = cpu_riscv_read_rtc(mtimer);

    /* Compute the relative hartid w.r.t the socket */
    hartid = hartid - mtimer->hartid_base;

    mtimer->timecmp[hartid] = value;
    if (mtimer->timecmp[hartid] <= rtc_r) {
        /*
         * If we're setting an MTIMECMP value in the "past",
         * immediately raise the timer interrupt
         */
        qemu_irq_raise(mtimer->irqs[hartid]);
        LOG_DEBUG("clint%d@0x%016" PRIx64 ": setting mcmp: 0x%016" PRIx64 " - MCMP IS IN THE PAST -> TRIGGER IMMEDIATE TIMER INTR\n", hartid, rtc_r, mtimer->timecmp[hartid]);
        return;
    }

    if (value & (1UL << 63)) {
        LOG_WARN("QEMU WARN -- clint%d@0x%016" PRIx64 ": setting mcmp: 0x%016" PRIx64 " - MCMP IS IMPOSSIBLY HIGH -> STOPING QEMU TIMER\n", hartid, rtc_r, mtimer->timecmp[hartid]);
        qemu_irq_lower(mtimer->irqs[hartid]);
        timer_del(mtimer->timers[hartid]);
        return;
    }

    /* otherwise, set up the future timer interrupt */
    qemu_irq_lower(mtimer->irqs[hartid]);
    diff = mtimer->timecmp[hartid] - rtc_r;
    /* back to ns (note args switched in muldiv64) */
    uint64_t ns_diff = muldiv64(diff, NANOSECONDS_PER_SECOND, timebase_freq);

    /*
     * check if ns_diff overflowed and check if the addition would potentially
     * overflow
     */
    if ((NANOSECONDS_PER_SECOND > timebase_freq && ns_diff < diff) ||
        ns_diff > INT64_MAX) {
        next = INT64_MAX;
    } else {
        /*
         * as it is very unlikely qemu_clock_get_ns will return a value
         * greater than INT64_MAX, no additional check is needed for an
         * unsigned integer overflow.
         */
        next = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns_diff;
        /*
         * if ns_diff is INT64_MAX next may still be outside the range
         * of a signed integer.
         */
        next = MIN(next, INT64_MAX);
    }

    LOG_DEBUG("clint" TARGET_FMT_ld "@0x%016" PRIx64 ": setting mcmp: 0x%016" PRIx64 "\tnext: %016" PRId64 " ns\n", cpu->env.mhartid, rtc_r, mtimer->timecmp[hartid], next);
    timer_mod(mtimer->timers[hartid], next);
}

/*
 * Callback used when the timer set using timer_mod expires.
 * Should raise the timer interrupt line
 */
static void sifive_clint_mtimer_cb(void *opaque)
{
    sifive_clint_mtimer_callback *state = opaque;

#ifdef CLINT_WARN
    SifiveClintState *mtimer = state->s;
    size_t hartid = state->num;
    uint64_t jitter = cpu_riscv_read_rtc(state->s) - mtimer->timecmp[hartid];
   if (jitter > 1000000) { // JITTER > 1ms
        LOG_WARN("QEMU WARN -- clint%d@0x%016" PRIx64 ": tigger timer intr - Jitter: %" PRIx64 "\tnow:  %016" PRId64 " ns\n", state->num, cpu_riscv_read_rtc(state->s),
            jitter, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
#endif

    LOG_DEBUG("clint%d@0x%016" PRIx64 ": tigger timer intr - Jitter: %" PRIx64 "\tnow:  %016" PRId64 " ns\n", state->num, cpu_riscv_read_rtc(state->s),
       cpu_riscv_read_rtc(state->s) - mtimer->timecmp[hartid], qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    qemu_irq_raise(state->s->irqs[state->num]);
}

/* CPU read MTIMER register */
static uint64_t sifive_clint_read(void *opaque, hwaddr addr,
    unsigned size)
{
    SifiveClintState *clint = opaque;

    /* SWI */
    if (addr < (clint->num_harts << 2)) {
        size_t hartid = clint->hartid_base + (addr >> 2);
        CPUState *cpu = cpu_by_arch_id(hartid);
        CPURISCVState *env = cpu ? cpu_env(cpu) : NULL;
        if (!env) {
            error_report("clint-swi: invalid hartid: %zu", hartid);
        } else if ((addr & 0x3) == 0) {
            return (clint->sswi) ? 0 : ((env->mip & MIP_MSIP) > 0);
        }
    /* MTIMER */
    } else if (addr >= clint->timecmp_base &&
        addr < (clint->timecmp_base + (clint->num_harts << 3))) {
        size_t hartid = clint->hartid_base +
                        ((addr - clint->timecmp_base) >> 3);
        CPUState *cpu = cpu_by_arch_id(hartid);
        CPURISCVState *env = cpu ? cpu_env(cpu) : NULL;
        if (!env) {
            error_report("clint-mtimer: invalid hartid: %zu", hartid);
        } else if ((addr & 0x7) == 0) {
            /* timecmp_lo for RV32/RV64 or timecmp for RV64 */
            uint64_t timecmp = clint->timecmp[hartid];
            return (size == 4) ? (timecmp & 0xFFFFFFFF) : timecmp;
        } else if ((addr & 0x7) == 4) {
            /* timecmp_hi */
            uint64_t timecmp = clint->timecmp[hartid];
            return (timecmp >> 32) & 0xFFFFFFFF;
        } else {
            error_report("clint-mtimer: invalid read: %08x", (uint32_t)addr);
            return 0;
        }
    } else if (addr == clint->time_base) {
        /* time_lo for RV32/RV64 or timecmp for RV64 */
        uint64_t rtc = cpu_riscv_read_rtc(clint);
        return (size == 4) ? (rtc & 0xFFFFFFFF) : rtc;
    } else if (addr == clint->time_base + 4) {
        /* time_hi */
        return (cpu_riscv_read_rtc(clint) >> 32) & 0xFFFFFFFF;
    }

    error_report("clint-mtimer: invalid read: %08x", (uint32_t)addr);
    return 0;
}

/* CPU write MTIMER register */
static void sifive_clint_write(void *opaque, hwaddr addr,
    uint64_t value, unsigned size)
{
    SifiveClintState *clint = opaque;
    int i;

    /* SWI */
    if (addr < (clint->num_harts << 2)) {
        size_t hartid = clint->hartid_base + (addr >> 2);
        CPUState *cpu = cpu_by_arch_id(hartid);
        CPURISCVState *env = cpu ? cpu_env(cpu) : NULL;
        if (!env) {
            error_report("clint-swi: invalid hartid: %zu", hartid);
        } else if ((addr & 0x3) == 0) {
            if (value & 0x1) {
                qemu_irq_raise(clint->irqs[clint->num_harts + hartid - clint->hartid_base]);
            } else {
                if (!clint->sswi) {
                    qemu_irq_lower(clint->irqs[clint->num_harts + hartid - clint->hartid_base]);
                }
            }
            return;
        }
    /* MTIMER */
    } else if (addr >= clint->timecmp_base &&
        addr < (clint->timecmp_base + (clint->num_harts << 3))) {
        size_t hartid = clint->hartid_base +
                        ((addr - clint->timecmp_base) >> 3);
        CPUState *cpu = cpu_by_arch_id(hartid);
        CPURISCVState *env = cpu ? cpu_env(cpu) : NULL;
        if (!env) {
            error_report("clint-mtimer: invalid hartid: %zu", hartid);
        } else if ((addr & 0x7) == 0) {
            if (size == 4) {
                /* timecmp_lo for RV32/RV64 */
                uint64_t timecmp_hi = clint->timecmp[hartid] >> 32;
                sifive_clint_mtimer_write_timecmp(clint, RISCV_CPU(cpu), hartid,
                    timecmp_hi << 32 | (value & 0xFFFFFFFF));
            } else {
                /* timecmp for RV64 */
                sifive_clint_mtimer_write_timecmp(clint, RISCV_CPU(cpu), hartid,
                                                  value);
            }
        } else if ((addr & 0x7) == 4) {
            if (size == 4) {
                /* timecmp_hi for RV32/RV64 */
                uint64_t timecmp_lo = clint->timecmp[hartid];
                sifive_clint_mtimer_write_timecmp(clint, RISCV_CPU(cpu), hartid,
                    value << 32 | (timecmp_lo & 0xFFFFFFFF));
            } else {
                error_report("clint-mtimer: invalid timecmp_hi write: %08x",
                              (uint32_t)addr);
            }
        } else {
            error_report("clint-mtimer: invalid timecmp write: %08x",
                          (uint32_t)addr);
        }
        return;
    } else if (addr == clint->time_base || addr == clint->time_base + 4) {
        uint64_t rtc_r = cpu_riscv_read_rtc_raw(clint->timebase_freq);

        if (addr == clint->time_base) {
            if (size == 4) {
                /* time_lo for RV32/RV64 */
                clint->time_delta = ((rtc_r & ~0xFFFFFFFFULL) | value) - rtc_r;
            } else {
                /* time for RV64 */
                clint->time_delta = value - rtc_r;
            }
        } else {
            if (size == 4) {
                /* time_hi for RV32/RV64 */
                clint->time_delta = (value << 32 | (rtc_r & 0xFFFFFFFF)) - rtc_r;
            } else {
                error_report("clint-mtimer: invalid time_hi write: %08x",
                              (uint32_t)addr);
                return;
            }
        }

        /* Check if timer interrupt is triggered for each hart. */
        for (i = 0; i < clint->num_harts; i++) {
            CPUState *cpu = cpu_by_arch_id(clint->hartid_base + i);
            CPURISCVState *env = cpu ? cpu_env(cpu) : NULL;
            if (!env) {
                continue;
            }
            sifive_clint_mtimer_write_timecmp(clint, RISCV_CPU(cpu),
                                              clint->hartid_base + i,
                                              clint->timecmp[i]);
        }
        return;
    }

    error_report("clint-mtimer: invalid write: %08x", (uint32_t)addr);
}

static const MemoryRegionOps sifive_clint_ops = {
    .read = sifive_clint_read,
    .write = sifive_clint_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    }
};

static const Property sifive_clint_properties[] = {
    DEFINE_PROP_UINT32("hartid-base", SifiveClintState,
        hartid_base, 0),
    DEFINE_PROP_UINT32("num-harts", SifiveClintState, num_harts, 1),
    DEFINE_PROP_UINT32("timecmp-base", SifiveClintState,
        timecmp_base, SIFIVE_CLINT_DEFAULT_MTIMECMP),
    DEFINE_PROP_UINT32("time-base", SifiveClintState,
        time_base, SIFIVE_CLINT_DEFAULT_MTIME),
    DEFINE_PROP_UINT32("aperture-size", SifiveClintState,
        aperture_size, SIFIVE_CLINT_DEFAULT_MTIMER_SIZE),
    DEFINE_PROP_UINT32("timebase-freq", SifiveClintState,
        timebase_freq, SIFIVE_CLINT_DEFAULT_TIMEBASE_FREQ),
    DEFINE_PROP_UINT32("sswi", SifiveClintState, sswi, false)
};

static void sifive_clint_realize(DeviceState *dev, Error **errp)
{
    SifiveClintState *s = SIFIVE_CLINT(dev);
    int i;

    memory_region_init_io(&s->mmio, OBJECT(dev), &sifive_clint_ops,
                          s, TYPE_SIFIVE_CLINT, s->aperture_size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);

    /* MTIMER + SWI */
    s->irqs = g_new(qemu_irq, s->num_harts * 2);
    qdev_init_gpio_out(dev, s->irqs, s->num_harts * 2);

    /* SWI */
    /* Claim software interrupt bits */
    for (i = 0; i < s->num_harts; i++) {
        RISCVCPU *cpu = RISCV_CPU(qemu_get_cpu(s->hartid_base + i));
        /* We don't claim mip.SSIP because it is writable by software */
        if (riscv_cpu_claim_interrupts(cpu, s->sswi ? 0 : MIP_MSIP) < 0) {
            error_report("MSIP already claimed");
            exit(1);
        }
    }

    /* MTIMER */
    s->timers = g_new0(QEMUTimer *, s->num_harts);
    s->timecmp = g_new0(uint64_t, s->num_harts);
    /* Claim timer interrupt bits */
    for (i = 0; i < s->num_harts; i++) {
        CPUState *cpu = cpu_by_arch_id(s->hartid_base + i);
        RISCVCPU *rvcpu = RISCV_CPU(cpu);
        CPURISCVState *env = cpu ? cpu_env(cpu) : NULL;

        /* SWI */
        qdev_connect_gpio_out(dev, s->num_harts + i,
                             qdev_get_gpio_in(DEVICE(rvcpu),
                                 (s->sswi) ? IRQ_S_SOFT : IRQ_M_SOFT));

        /* MTIMER */
        if (riscv_cpu_claim_interrupts(rvcpu, MIP_MTIP) < 0) {
            error_report("MTIP already claimed");
            exit(1);
        }

        sifive_clint_mtimer_callback *cb =
            g_new0(sifive_clint_mtimer_callback, 1);

        if (!env) {
            g_free(cb);
            continue;
        }

        cb->s = s;
        cb->num = i;
        s->timers[i] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  &sifive_clint_mtimer_cb, cb);
        s->timecmp[i] = 0;

        qdev_connect_gpio_out(dev, i,
                              qdev_get_gpio_in(DEVICE(rvcpu), IRQ_M_TIMER));
    }
}

static void sifive_clint_reset_enter(Object *obj, ResetType type)
{
    /*
     * According to RISC-V ACLINT spec:
     *   - On MTIMER device reset, the MTIME register is cleared to zero.
     *   - On MTIMER device reset, the MTIMECMP registers are in unknown state.
     */
    SifiveClintState *clint = SIFIVE_CLINT(obj);

    /*
     * Clear mtime register by writing to 0 it.
     * Pending mtime interrupts will also be cleared at the same time.
     */
    sifive_clint_write(clint, clint->time_base, 0, 8);
    
    if (!clint->sswi) {
        for (unsigned i = 0; i < clint->num_harts; i++) {
            /* Clear MSIP registers by lowering software interrupts. */
            qemu_irq_lower(clint->irqs[clint->num_harts + i]);
        }
    }
}

static const VMStateDescription vmstate_riscv = {
    .name = "riscv_mtimer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
            VMSTATE_VARRAY_UINT32(timecmp, SifiveClintState,
                                  num_harts, 0,
                                  vmstate_info_uint64, uint64_t),
            VMSTATE_END_OF_LIST()
        }
};

static void sifive_clint_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = sifive_clint_realize;
    device_class_set_props(dc, sifive_clint_properties);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    rc->phases.enter = sifive_clint_reset_enter;
    dc->vmsd = &vmstate_riscv;
}

static const TypeInfo sifive_clint_info = {
    .name          = TYPE_SIFIVE_CLINT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SifiveClintState),
    .class_init    = sifive_clint_class_init,
};

/*
 * Create ACLINT MTIMER device.
 */
DeviceState *sifive_clint_create(hwaddr addr, hwaddr size,
    uint32_t hartid_base, uint32_t num_harts,
    uint32_t timecmp_base, uint32_t time_base, uint32_t timebase_freq,
    bool provide_rdtime, bool sswi)
{
    int i;
    DeviceState *dev = qdev_new(TYPE_SIFIVE_CLINT);

    assert(num_harts <= SIFIVE_CLINT_MAX_HARTS);
    assert(!(addr & 0x7));
    assert(!(timecmp_base & 0x7));
    assert(!(time_base & 0x7));

    qdev_prop_set_uint32(dev, "hartid-base", hartid_base);
    qdev_prop_set_uint32(dev, "num-harts", num_harts);
    qdev_prop_set_uint32(dev, "timecmp-base", timecmp_base);
    qdev_prop_set_uint32(dev, "time-base", time_base);
    qdev_prop_set_uint32(dev, "aperture-size", size);
    qdev_prop_set_uint32(dev, "timebase-freq", timebase_freq);
    qdev_prop_set_uint32(dev, "sswi", sswi ? true : false);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);

    for (i = 0; i < num_harts; i++) {
        CPUState *cpu = cpu_by_arch_id(hartid_base + i);
        CPURISCVState *env = cpu ? cpu_env(cpu) : NULL;
        if (provide_rdtime) {
            riscv_cpu_set_rdtime_fn(env, cpu_riscv_read_rtc, dev);
        }
    }


    return dev;
}

static void sifive_clint_register_types(void)
{
    type_register_static(&sifive_clint_info);
}

type_init(sifive_clint_register_types)
