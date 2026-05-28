/*
 * PowerPC CPU library code for use with UAE
 * Copyright 2014 Frode Solheim <frode@fs-uae.net>
 * Adapted for QEMU 11.0 by Stefan Reinauer
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/ppc/ppc.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "system/system.h"
#include "system/cpus.h"
#include "system/runstate.h"
#include "system/cpu-timers.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "exec/tb-flush.h"
#include "cpu.h"
#include "helper_regs.h"
#include "qemu-uae.h"

#include "uae/log.h"
#include "uae/ppc.h"
#include "uae/qemu.h"

#define BUSFREQ 66000000UL
#define TBFREQ 16600000UL
#define MAX_MEMORY_REGIONS 128

static struct {
    CPUPPCState *env;
    PowerPCCPU *cpu;
    int cpu_state;
    QemuThread pause_thread;
    uint32_t hid1;
} state;

static uint64_t indirect_read(void *opaque, hwaddr addr, unsigned size)
{
    addr += (uintptr_t) opaque;
    if (size == 8) {
        uint64_t retval = 0;
        uae_ppc_io_mem_read64(addr, &retval);
        return retval;
    } else {
        uint32_t retval = 0;
        uae_ppc_io_mem_read(addr, &retval, size);
        return retval;
    }
}

static void indirect_write(void *opaque, hwaddr addr, uint64_t data,
                           unsigned size)
{
    addr += (uintptr_t) opaque;
    if (size == 8) {
        uae_ppc_io_mem_write64(addr, data);
    } else {
        uae_ppc_io_mem_write(addr, data, size);
    }
}

static const MemoryRegionOps indirect_ops = {
    .read = indirect_read,
    .write = indirect_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

/** Deprecated, use qemu_uae_version instead */
void PPCAPI ppc_cpu_version(int *major, int *minor, int *revision)
{
    return qemu_uae_version(major, minor, revision);
}

static void qemu_uae_machine_reset(void *opaque)
{
    PowerPCCPU *cpu = opaque;
    cpu_reset(CPU(cpu));
}

static bool qemu_uae_machine_init(const char *model)
{
    ObjectClass *oc;
    Object *cpuobj;
    Error *err = NULL;

    uae_log("QEMU: Initializing PPC CPU model %s\n", model);

    /* Find the CPU class */
    oc = cpu_class_by_name(TYPE_POWERPC_CPU, model);
    if (!oc) {
        uae_log("QEMU: Unknown CPU model %s\n", model);
        return false;
    }

    /* Create CPU object */
    cpuobj = object_new(object_class_get_name(oc));
    state.cpu = POWERPC_CPU(cpuobj);
    state.env = &state.cpu->env;

    /* Realize the CPU */
    qdev_realize(DEVICE(cpuobj), NULL, &err);
    if (err) {
        uae_log("QEMU: Failed to realize CPU: %s\n", error_get_pretty(err));
        error_free(err);
        object_unref(cpuobj);
        return false;
    }

    qemu_register_reset(qemu_uae_machine_reset, state.cpu);
    return true;
}

static bool initialize(const char *model)
{
    static bool initialized = false;
    if (initialized) {
        return false;
    }
    initialized = true;

    qemu_uae_mutex_lock();

    /* Create CPU */
    if (!qemu_uae_machine_init(model)) {
        qemu_uae_mutex_unlock();
        return false;
    }

    /* Doesn't really do anything except in KVM mode */
    cpu_synchronize_all_post_init();

    /* Exception vector base at 0xfff00000, instead of 0x00000000 */
    hreg_store_msr(state.env, 1 << MSR_EP, 0);

    /* Log CPU model identifier */
    uae_log("QEMU: CPU PVR 0x%08x\n", state.env->spr[SPR_PVR]);

    qemu_uae_mutex_unlock();
    return true;
}

bool PPCAPI ppc_cpu_init(const char* model, uint32_t hid1)
{
    /* In case qemu_uae_init hasn't been called by the user yet */
    qemu_uae_init();

    const char *qemu_model = model;
    if (strcasecmp(model, "603ev") == 0) {
        qemu_model = "603e7v1";
    } else if (strcasecmp(model, "604e") == 0) {
        qemu_model = "604e_v2.4";
    }
    uae_log("QEMU: ppc_cpu_init_with_model %s => %s\n", model, qemu_model);
    state.hid1 = hid1;
    return initialize(qemu_model);
}

bool PPCAPI qemu_uae_ppc_init(const char* model, uint32_t hid1)
{
    return ppc_cpu_init(model, hid1);
}

bool PPCAPI qemu_uae_ppc_in_cpu_thread(void)
{
    return qemu_cpu_is_self(CPU(state.cpu));
}

static void qemu_uae_lock_if_needed(void)
{
    if (qemu_uae_ppc_in_cpu_thread() == false) {
        qemu_uae_mutex_lock();
    }
}

static void qemu_uae_unlock_if_needed(void)
{
    if (qemu_uae_ppc_in_cpu_thread() == false) {
        qemu_uae_mutex_unlock();
    }
}

struct UAEregion
{
    struct MemoryRegion *region;
    hwaddr addr;
    unsigned int size;
};

static struct UAEregion added_regions[MAX_MEMORY_REGIONS + 1];

static void ppc_cpu_map_add(PPCMemoryRegion *r)
{
    int i;
    for (i = 0; i < MAX_MEMORY_REGIONS; i++) {
        MemoryRegion *mem;
        struct UAEregion *region = &added_regions[i];
        if (region->region != NULL)
            continue;
        uae_log("QEMU: %02d %08x [+%8x]  =>  %p  \"%s\")\n",
                i, r->start, r->size, r->memory, r->name);
        mem = g_new(MemoryRegion, 1);
        if (r->memory) {
            memory_region_init_ram_ptr(mem, NULL, r->name, r->size, r->memory);
        } else {
            memory_region_init_io(mem, NULL, &indirect_ops,
                                  (void *) (uintptr_t) r->start, r->name,
                                  r->size);
        }
        memory_region_add_subregion(get_system_memory(), r->start, mem);
        region->region = mem;
        region->addr = r->start;
        region->size = r->size;
        return;
    }
}

static void ppc_cpu_map_memory_single(PPCMemoryRegion *r)
{
    int i;
    uae_log("QEMU: Map single memory region %08x + %08x '%s' (%p):\n",
            r->start, r->size, r->name, r->memory);

    if (r->alias == 0xffffffff) {
        for (i = 0; i < MAX_MEMORY_REGIONS; i++) {
            struct UAEregion *mem = &added_regions[i];
            if (mem->region == NULL)
                continue;
            if (mem->addr >= r->start && mem->addr < r->start + r->size) {
                uae_log("QEMU: region %02d %08x %08x deleted\n", i,
                        (uint32_t)mem->addr,
                        mem->size);
                memory_region_del_subregion(get_system_memory(), mem->region);
                object_unparent(OBJECT(mem->region));
                mem->region = NULL;
            }
        }
        return;
    }

    for (i = 0; i < MAX_MEMORY_REGIONS; i++) {
        struct UAEregion *mem = &added_regions[i];
        if (mem->region == NULL)
            continue;
        if (mem->addr == r->start && mem->size == r->size) {
            uae_log("QEMU: region %02d replaced\n", i);
            memory_region_del_subregion(get_system_memory(), mem->region);
            if (r->memory) {
                memory_region_init_ram_ptr(mem->region, NULL, r->name, r->size,
                                           r->memory);
            } else {
                memory_region_init_io(
                    mem->region, NULL, &indirect_ops,
                    (void *) (uintptr_t) r->start, r->name, r->size);
            }
            memory_region_add_subregion(
                get_system_memory(), r->start, mem->region);
            return;
        }
    }
    ppc_cpu_map_add(r);
}

static void ppc_cpu_map_memory_multi(PPCMemoryRegion *regions, int count)
{
    int i;

    uae_log("QEMU: Map memory regions:\n");
    if (count >= MAX_MEMORY_REGIONS) {
        uae_log("QEMU: Too many memory regions!\n");
        return;
    }

    /* Remove all existing memory regions */
    for (i = 0; i < MAX_MEMORY_REGIONS; i++) {
        struct UAEregion *mem = &added_regions[i];
        if (mem->region == NULL)
            continue;
        memory_region_del_subregion(get_system_memory(), mem->region);
        object_unparent(OBJECT(mem->region));
        mem->region = NULL;
    }

    /* Create new memory subregions */
    for (i = 0; i < count; i++)
        ppc_cpu_map_add(&regions[i]);
}

void PPCAPI ppc_cpu_map_memory(PPCMemoryRegion *regions, int count)
{
    qemu_uae_lock_if_needed();
    if (count >= 0)
        ppc_cpu_map_memory_multi(regions, count);
    else
        ppc_cpu_map_memory_single(regions);
    qemu_uae_unlock_if_needed();
}

void PPCAPI qemu_uae_ppc_external_interrupt(bool enable)
{
    ppc_set_irq(state.cpu, PPC_INTERRUPT_EXT, enable ? 1 : 0);
}

int PPCAPI qemu_uae_lock(int type)
{
    int result = 0;
    if (type == QEMU_UAE_LOCK_TRYLOCK) {
        result = qemu_uae_mutex_trylock();
    } else if (type == QEMU_UAE_LOCK_TRYLOCK_CANCEL) {
        qemu_uae_mutex_trylock_cancel();
    } else if (type == QEMU_UAE_LOCK_ACQUIRE) {
        qemu_uae_mutex_lock();
    } else if (type == QEMU_UAE_LOCK_RELEASE) {
        qemu_uae_mutex_unlock();
    }
    return result;
}

void PPCAPI ppc_cpu_run_continuous(void)
{
    uae_log("QEMU: Running main loop\n");
    qemu_uae_mutex_lock();

    cpu_enable_ticks();
    runstate_set(RUN_STATE_RUNNING);
    vm_state_notify(1, RUN_STATE_RUNNING);
    resume_all_vcpus();

    qemu_uae_set_started();

    /* The main loop iteration unlocks and relocks the iothread lock */
    main_loop();
}

void PPCAPI ppc_cpu_reset(void)
{
    uae_log("QEMU: Reset CPU\n");
    cpu_reset(CPU(state.cpu));
    state.env->spr[SPR_HID1] = state.hid1;
    uae_log("QEMU: NIP = 0x%08x\n", state.env->nip);

    uae_log("QEMU: Flushing all JIT translation blocks\n");
    queue_tb_flush(CPU(state.cpu));
}

static void qemu_uae_log_cpu_state(void)
{
    int flags = 0;
    uae_log("QEMU: PPC CPU dump:\n");
    cpu_dump_state(CPU(state.cpu), stderr, flags);
}

void PPCAPI ppc_cpu_set_state(int set_state)
{
    uae_log("QEMU: Set state %d\n", set_state);
    qemu_uae_lock_if_needed();
    if (set_state == PPC_CPU_STATE_PAUSED) {
        pause_all_vcpus();
        state.cpu_state = PPC_CPU_STATE_PAUSED;
        uae_log("QEMU: Paused!\n");
        qemu_uae_log_cpu_state();
    } else if (set_state == PPC_CPU_STATE_RUNNING) {
        resume_all_vcpus();
        state.cpu_state = PPC_CPU_STATE_RUNNING;
        uae_log("QEMU: Resumed!\n");
    }
    qemu_uae_unlock_if_needed();
}

/* Storage for callback functions set by UAE */

uae_log_function PPCAPI uae_log = NULL;
uae_ppc_io_mem_read_function PPCAPI uae_ppc_io_mem_read = NULL;
uae_ppc_io_mem_write_function PPCAPI uae_ppc_io_mem_write = NULL;
uae_ppc_io_mem_read64_function PPCAPI uae_ppc_io_mem_read64 = NULL;
uae_ppc_io_mem_write64_function PPCAPI uae_ppc_io_mem_write64 = NULL;
