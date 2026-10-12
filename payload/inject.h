/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "hook.h"
#include "stub.h"

typedef struct {
    pid_t    pid;
    intptr_t code;         /* where the copied stub block lives in the game */
    size_t   code_len;
    intptr_t args;         /* the shared page */
    unsigned hooked;       /* slots redirected */
    int      installed;
} ga_inject_t;

/* Copies the transform into the game and points its pad import slots at it.
 * Leaves the transform parked (enabled = 0) so nothing changes until the
 * service says so. Returns 0 on success. */
int ga_inject_install(const ga_target_t *target, const ga_slot_t *slots,
                      unsigned slot_count, ga_inject_t *out);

/* Takes over an installation an earlier run left in the game, rather than
 * installing a second one beside it. The layout version has to match ours,
 * since everything past the magic word moves between builds.
 * Returns 0 on success. */
int ga_inject_adopt(const ga_target_t *target, const ga_orphan_t *orphan,
                    ga_inject_t *out);

/* Pushes the current settings and resets the staleness counter. Call it
 * regularly: if it stops, the stub stops modifying input. */
int ga_inject_publish(const ga_inject_t *in, const ga_args_t *settings);

/* Reads back what the stub last saw and did. */
int ga_inject_read(const ga_inject_t *in, ga_args_t *out);

/* Puts every slot back the way it was. The injected mapping stays where it
 * is, deliberately; see the note in inject.c. */
int ga_inject_remove(ga_inject_t *in);

/* Forgets the mapping remembered for a process. Call it when the game exits,
 * so a later game with the same pid cannot inherit a stale address. */
void ga_inject_forget(pid_t pid);
