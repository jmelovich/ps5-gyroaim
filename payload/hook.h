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

/* Log every dynamic-linker object while hunting for slots. */
extern int ga_hook_verbose;

#define GA_MAX_SLOTS 24

typedef struct {
    intptr_t slot;        /* address of the import slot in the game */
    intptr_t original;    /* what it points at now */
    uint32_t protection;  /* current page protection, for restoring later */
    int      kind;        /* GA_FN_* */
} ga_slot_t;

/* Walks the game's dynamic-linker objects and finds the import slots that
 * point at the pad entry points in `target`. Read-only.
 *
 * Slots are identified by comparing each relocation's current value against
 * the already-resolved export addresses, which is both simpler and more
 * conclusive than decoding symbol names.
 *
 * Returns the number of slots found, or -1 if the object list could not be
 * located at all.
 */
int ga_hook_find_slots(const ga_target_t *target, ga_slot_t *out,
                       unsigned max);

/* Reads back specific addresses one at a time and reports whether chunked
 * reads agree, to tell a real duplicate from a read artefact. */
void ga_hook_verify_reads(const ga_target_t *target, const intptr_t *addrs,
                          unsigned n);

/* Finds the same slots by scanning the game's own modules for the export
 * addresses themselves, eight-byte aligned.
 *
 * Needed because a module built with -fno-plt has no PLT relocations at all
 * and reaches its imports straight through the GOT, which is how homebrew
 * built with this SDK behaves. Retail titles populate the relocation table,
 * so the two methods cover different targets and agree where both apply.
 *
 * Returns the number of slots found. */
int ga_hook_scan_slots(const ga_target_t *target, ga_slot_t *out,
                       unsigned max);
