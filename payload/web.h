/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "config.h"
#include "proc.h"
#include "stub.h"

#include <pthread.h>

/* State shared between the service loop and the portal thread. */
typedef struct {
    pthread_mutex_t lock;
    int             listen_fd;

    ga_config_t cfg;
    int         cfg_dirty;      /* portal changed something */

    ga_args_t   live;           /* last telemetry read */
    int         have_live;
    int         attached;
    char        title[GA_TITLE_ID_MAX];
    char        status[128];
} ga_web_state_t;

/* Takes over the already-bound listening socket and serves the portal.
 * Returns 0 if the thread started. */
int ga_web_start(int listen_fd, ga_web_state_t *state);
