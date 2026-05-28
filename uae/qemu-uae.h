#ifndef QEMU_UAE_H
#define QEMU_UAE_H

#include "qemu/osdep.h"
#include "qemu/thread.h"

/* Forward declaration */
struct CPUState;

/* cpus.c - BQL (Big QEMU Lock) wrappers for UAE */

void qemu_uae_mutex_lock(void);
void qemu_uae_mutex_unlock(void);
int qemu_uae_mutex_trylock(void);
void qemu_uae_mutex_trylock_cancel(void);

/* system/runstate.c or main-loop */

void main_loop(void);

/* qemu-uae-cpu.c */

bool qemu_uae_main_loop_should_exit(void);

/* qemu-uae-main.c */

void qemu_uae_set_started(void);
void qemu_uae_wait_until_started(void);

#endif /* QEMU_UAE_H */
