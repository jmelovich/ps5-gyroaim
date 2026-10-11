/* SPDX-License-Identifier: GPL-3.0-or-later
 * hook.c - looking into a running game.
 *
 * Everything here is read-only. It establishes that the game has libScePad
 * loaded and that its pad entry points can be resolved from outside, which is
 * the groundwork for redirecting the game's import slots later.
 */
#include "hook.h"
#include "log.h"

#include <string.h>

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#endif

const char *const ga_fn_names[GA_FN_COUNT] = {
    "scePadReadState",
    "scePadReadStateExt",
    "scePadRead",
    "scePadReadExt",
    "scePadSetVibration",
    "scePadSetMotionSensorState",
    "scePadSetAngularVelocityDeadbandState",
    "scePadGetControllerInformation",
};

/* The runtime module name is not obviously the stub library's name, and a
 * failed lookup is indistinguishable from "the walk does not work on this
 * firmware". Trying a list of candidates settles both at once. */
static const char *const k_libpad_names[] = {
    "libScePad.sprx", "libScePad", "libScePad.prx", "libScePad.so",
};

void ga_hook_probe_modules(pid_t pid)
{
#ifdef __PROSPERO__
    static const char *const probe[] = {
        "libScePad.sprx", "libScePad", "libScePad.prx", "libScePad.so",
        "libkernel.sprx", "libkernel", "libSceLibcInternal.sprx",
        "libSceLibcInternal", "libSceSystemService.sprx",
        "libSceUserService.sprx", "libSceGnmDriver.sprx", "eboot.bin",
    };

    ga_log("module probe for pid %d:", (int)pid);
    for (unsigned i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
        uint32_t h = 0;
        if (kernel_dynlib_handle(pid, probe[i], &h) == 0) {
            intptr_t base = kernel_dynlib_mapbase_addr(pid, h);
            ga_log("  %-26s handle %u base 0x%lx", probe[i], h, (long)base);
        }
    }
    ga_log("module probe: done (names not listed were not found)");
#else
    (void)pid;
#endif
}

int ga_hook_inspect(pid_t pid, ga_target_t *out)
{
#ifndef __PROSPERO__
    (void)pid;
    (void)out;
    return -1;
#else
    if (!out || pid <= 0)
        return -1;

    memset(out, 0, sizeof(*out));
    out->pid = pid;

    const char *found = NULL;
    for (unsigned i = 0; i < sizeof(k_libpad_names) / sizeof(k_libpad_names[0]);
         i++)
        if (kernel_dynlib_handle(pid, k_libpad_names[i],
                                 &out->libpad_handle) == 0) {
            found = k_libpad_names[i];
            break;
        }
    if (!found)
        return -1;
    ga_log("inspect: libScePad is \"%s\" in pid %d", found, (int)pid);

    out->libpad_base = kernel_dynlib_mapbase_addr(pid, out->libpad_handle);
    if (out->libpad_base <= 0) {
        ga_log("inspect: libScePad handle %u has no map base",
               out->libpad_handle);
        return -1;
    }

    ga_log("inspect: pid %d libScePad handle %u base 0x%lx",
           (int)pid, out->libpad_handle, (long)out->libpad_base);

    unsigned resolved = 0;
    for (int i = 0; i < GA_FN_COUNT; i++) {
        out->fn[i] = kernel_dynlib_dlsym(pid, out->libpad_handle,
                                         ga_fn_names[i]);
        if (out->fn[i] > 0) {
            resolved++;
            ga_log("  %-38s 0x%lx (+0x%lx)", ga_fn_names[i],
                   (long)out->fn[i], (long)(out->fn[i] - out->libpad_base));
        } else {
            ga_log("  %-38s not exported", ga_fn_names[i]);
        }
    }

    if (!out->fn[GA_FN_READ_STATE] && !out->fn[GA_FN_READ]) {
        ga_log("inspect: neither scePadReadState nor scePadRead resolved; "
               "nothing to hook");
        return -1;
    }

    ga_log("inspect: %u/%d exports resolved", resolved, GA_FN_COUNT);
    return 0;
#endif
}
