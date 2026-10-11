/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>
#include <sys/types.h>

/* The libScePad entry points the service cares about, in a fixed order so a
 * resolved address can be looked up by kind. */
enum {
    GA_FN_READ_STATE = 0,
    GA_FN_READ_STATE_EXT,
    GA_FN_READ,
    GA_FN_READ_EXT,
    GA_FN_SET_VIBRATION,
    GA_FN_SET_MOTION_SENSOR,
    GA_FN_SET_DEADBAND,
    GA_FN_GET_CONTROLLER_INFO,
    GA_FN_COUNT
};

extern const char *const ga_fn_names[GA_FN_COUNT];

typedef struct {
    pid_t    pid;
    uint32_t libpad_handle;
    intptr_t libpad_base;
    intptr_t fn[GA_FN_COUNT];     /* resolved export addresses, 0 if absent */
} ga_target_t;

/* Logs which of a list of candidate module names the process has loaded.
 * Read-only; a diagnostic for firmware and naming questions. */
void ga_hook_probe_modules(pid_t pid);

/* Reads the game's loaded libScePad and resolves the exports above. Kernel
 * reads only: nothing in the game is written or stopped.
 * Returns 0 on success. */
int ga_hook_inspect(pid_t pid, ga_target_t *out);
