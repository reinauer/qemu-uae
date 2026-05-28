/*
 * QEMU Slirp glue code for use with UAE
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
#include "qemu/sockets.h"
#include "qemu-uae.h"

#include "uae/log.h"
#include "uae/qemu.h"

/* Note: Slirp has been moved to a separate subproject in modern QEMU.
 * This file may need significant adaptation depending on how slirp
 * is integrated in the build. For now, we provide stub implementations
 * that can be filled in once the build system is set up. */

UAE_DEFINE_IMPORT_FUNCTION(uae_slirp_output)

#ifdef CONFIG_SLIRP
#include "slirp/libslirp.h"

static Slirp *slirp;

void qemu_uae_slirp_init(void)
{
    uae_log("QEMU: Initializing Slirp\n");

    /* Note: slirp_init API has changed in modern libslirp.
     * This needs to be adapted for the new API. */
    uae_log("QEMU: Slirp initialization not yet implemented for modern QEMU\n");
}

void qemu_uae_slirp_input(const uint8_t *pkt, int pkt_len)
{
    uae_log("QEMU: qemu_uae_slirp_input pkt_len %d\n", pkt_len);
    if (slirp == NULL) {
        return;
    }
    slirp_input(slirp, pkt, pkt_len);
}

#else /* !CONFIG_SLIRP */

void qemu_uae_slirp_init(void)
{
    uae_log("QEMU: Slirp support not compiled in\n");
}

void qemu_uae_slirp_input(const uint8_t *pkt, int pkt_len)
{
    uae_log("QEMU: Slirp support not compiled in\n");
}

#endif /* CONFIG_SLIRP */

/* This function is called by slirp when it has data to send */
void slirp_output(void *opaque, const uint8_t *pkt, int pkt_len)
{
    uae_log("QEMU: slirp_output pkt_len %d\n", pkt_len);
    if (uae_slirp_output == NULL) {
        return;
    }
    uae_slirp_output(pkt, pkt_len);
}
