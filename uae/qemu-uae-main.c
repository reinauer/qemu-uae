/*
 * QEMU integration code for use with UAE
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
#include "qemu/main-loop.h"
#include "qemu/accel.h"
#include "qemu-version.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "exec/cpu-common.h"
#include "hw/core/boards.h"
#include "system/system.h"
#include "system/cpus.h"
#include "system/runstate.h"
#include "system/cpu-timers.h"
#include "qemu-uae.h"

#include "uae/log.h"
#include "uae/ppc.h"
#include "uae/qemu.h"

#ifdef UAE
#error UAE should not be defined here
#endif

/* Increase this when changes are not backwards compatible */
#define VERSION_MAJOR 3

/* Increase this when important changes are made */
#define VERSION_MINOR 8

/* Just increase this when the update is insignificant */
#define VERSION_REVISION 2

#if QEMU_UAE_VERSION_MAJOR != VERSION_MAJOR
#error Major version mismatch between UAE and QEMU-UAE
#endif

#if QEMU_UAE_VERSION_MINOR != VERSION_MINOR
#warning Minor version mismatch between UAE and QEMU-UAE
#endif

static struct {
    volatile bool started;
    bool exit_main_loop;
} state;

static bool qemu_uae_create_machine(void)
{
    static const char *const containers[] = {
        "unattached",
        "peripheral",
        "peripheral-anon",
    };
    ObjectClass *machine_class;
    AccelClass *accel_class;
    AccelState *accel;
    int ret;

    if (current_machine) {
        return true;
    }

    machine_class = object_class_by_name(MACHINE_TYPE_NAME("none"));
    if (!machine_class) {
        uae_log("QEMU: failed to find none machine class\n");
        return false;
    }

    current_machine = MACHINE(object_new_with_class(machine_class));
    object_property_add_child(object_get_root(), "machine",
                              OBJECT(current_machine));
    for (int i = 0; i < G_N_ELEMENTS(containers); i++) {
        object_property_add_new_container(OBJECT(current_machine),
                                          containers[i]);
    }
    cpu_exec_init_all();

    accel_class = accel_find("tcg");
    if (!accel_class) {
        uae_log("QEMU: failed to find TCG accelerator\n");
        return false;
    }

    accel = ACCEL(object_new_with_class(OBJECT_CLASS(accel_class)));
    ret = accel_init_machine(accel, current_machine);
    if (ret < 0) {
        uae_log("QEMU: failed to initialize TCG accelerator: %d\n", ret);
        return false;
    }
    accel_init_interfaces(accel_class);
    return true;
}

void qemu_uae_set_started(void)
{
    state.started = true;
}

void qemu_uae_wait_until_started(void)
{
    while (!state.started) {
        qemu_uae_mutex_unlock();
        g_usleep(10);
        qemu_uae_mutex_lock();
    }
}

void PPCAPI qemu_uae_version(int *major, int *minor, int *revision)
{
    *major = VERSION_MAJOR;
    *minor = VERSION_MINOR;
    *revision = VERSION_REVISION;
}

static bool initialize(void)
{
    int major, minor, revision;
    Error *err = NULL;

    qemu_uae_version(&major, &minor, &revision);
    uae_log("QEMU: Initialize QEMU-UAE (QEMU %s + API %d.%d.%d)\n",
            QEMU_FULL_VERSION, major, minor, revision);

    uae_log("QEMU: qemu_init_subsystems\n");
    qemu_init_subsystems();

    /* qemu_init_main_loop installs signals */
    /* FIXME: could conflict with UAE */
    qemu_init_main_loop(&err);
    if (err) {
        uae_log("QEMU: qemu_init_main_loop failed: %s\n", error_get_pretty(err));
        error_free(err);
        qemu_uae_mutex_unlock();
        return false;
    }

    if (!qemu_uae_create_machine()) {
        qemu_uae_mutex_unlock();
        return false;
    }

    qemu_uae_mutex_unlock();
    return true;
}

static void qemu_uae_main(void)
{
    uae_log("QEMU: Running main loop\n");
    qemu_uae_mutex_lock();

    cpu_enable_ticks();
    runstate_set(RUN_STATE_RUNNING);
    vm_state_notify(1, RUN_STATE_RUNNING);

    qemu_uae_set_started();

    /* The main loop iteration unlocks and relocks the iothread lock */
    main_loop();
}

static void *main_thread_function(void *arg)
{
    uae_log("QEMU: Main thread running\n");
    qemu_uae_main();
    return NULL;
}

static QemuThread main_thread;

void PPCAPI qemu_uae_init(void)
{
    static bool initialized = false;
    if (initialized) {
        return;
    }
    uae_log("QEMU: Initializing\n");
    initialize();
    initialized = true;
}

void PPCAPI qemu_uae_start(void)
{
    static bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;
    uae_log("QEMU: Starting main loop\n");
    qemu_thread_create(&main_thread, "QEMU Main", main_thread_function,
                       NULL, QEMU_THREAD_DETACHED);
}

bool PPCAPI qemu_uae_main_loop_should_exit(void)
{
    return state.exit_main_loop;
}
