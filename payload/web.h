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
    int         save_as_title;  /* portal asked to keep these per game */
    int         reload_request; /* portal asked to reread the file */
    int         quit_request;   /* portal asked the service to stop */
    int         rest_request;   /* portal asked to make here the rest position */
    int         uninstall;      /* 1 keep settings, 2 take those too */
    int         kstuff;         /* can the transform be installed at all */
    char        path[GA_PATH_MAX];   /* where settings are being kept */

    ga_args_t   live;           /* last telemetry read */
    int         have_live;
    int         attached;
    char        title[GA_TITLE_ID_MAX];
    char        status[128];
} ga_web_state_t;

/* Takes over the already-bound listening socket and serves the portal.
 * Returns 0 if the thread started. */
int ga_web_start(int listen_fd, ga_web_state_t *state);
