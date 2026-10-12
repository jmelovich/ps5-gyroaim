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
    GA_FN_SET_VIBRATION_MODE,
    GA_FN_SET_MOTION_SENSOR,
    GA_FN_SET_DEADBAND,
    GA_FN_GET_CONTROLLER_INFO,
    GA_FN_COUNT
};

extern const char *const ga_fn_names[GA_FN_COUNT];

/* Identifies the page the service and the injected stub share. Declared here
 * rather than with the page's layout because the slot scan needs it to tell
 * one of our own gateways from an ordinary pointer. */
#define GA_MAGIC          0x4f524947u   /* "GYRO" */
#define GA_LAYOUT_VERSION 4u

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

/* Asks the long scans to give up early. Set from a signal handler so a
 * shutdown or a takeover is not held up by a scan: reading a large module
 * eight bytes at a time can run for minutes, and while it does, the service
 * answers nothing. */
extern volatile int ga_hook_cancel;

/* A module larger than this is not swept for slot values. The sweep exists
 * for modules built without a PLT, which are small; a large one has a
 * relocation table and is found that way instead. Sweeping a hundreds of
 * megabyte executable was what made the service unresponsive. */
#define GA_SCAN_MAX_BYTES  (48u * 1024u * 1024u)

#define GA_MAX_SLOTS 24

typedef struct {
    intptr_t slot;        /* address of the import slot in the game */
    intptr_t original;    /* what it points at now */
    uint32_t protection;  /* current page protection, for restoring later */
    int      kind;        /* GA_FN_* */
} ga_slot_t;

/* An installation left behind in a game by an earlier run of the service.
 *
 * Removing hooks restores the slots, but that only happens if the service gets
 * to shut down. Killed, crashed, or displaced mid-shutdown by a replacement,
 * it leaves the game pointing at gateways that no longer have anyone feeding
 * them. The slot then holds our gateway rather than Sony's export, so a plain
 * scan no longer recognises it as a pad slot at all, and the service can never
 * attach to that game again. The stub itself is still wired in, which is why
 * aiming can carry on working while the service reports that it failed.
 */
typedef struct {
    intptr_t args;          /* the shared page the gateways point at */
    intptr_t gateway;       /* one gateway, enough to locate the code */
    uint32_t version;       /* its layout version, which must match ours */
    unsigned slots;         /* how many slots point into it */
} ga_orphan_t;

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
                       unsigned max, ga_orphan_t *orphan);


/* Both slot finders below take an optional `orphan`. They test a slot whose
 * value matches no export for being one of our gateways, which is nearly free
 * because the candidate set is already narrow, and far cheaper than sweeping a
 * module for it: a large game's module holds millions of plausible pointers,
 * and following each one costs two kernel reads.
 */

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
                       unsigned max, ga_orphan_t *orphan);
