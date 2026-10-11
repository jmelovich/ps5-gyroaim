/* SPDX-License-Identifier: GPL-3.0-or-later
 * libc_stubs.c - the few libc symbols the console does not give a title.
 *
 * The boilerplate's own notes describe this pattern: some libc entry points
 * either are not exported or resolve into a module a native title never
 * loads, and the link fails or the call faults on an unresolved address.
 */
#include <stdio.h>

int sceKernelDebugOutText(int channel, const char *text);

/* assert() lowers to this. Compiling assertions out with NDEBUG would hide a
 * real failure instead, and a failed assertion in the QR encoder is something
 * worth seeing in the kernel log. */
void __assert(const char *function, const char *file, int line,
              const char *expression)
{
    char message[256];

    snprintf(message, sizeof(message),
             "[GAPANEL] assertion failed: %s, at %s:%d in %s\n",
             expression ? expression : "?", file ? file : "?", line,
             function ? function : "?");
    sceKernelDebugOutText(0, message);

    /* Stopping here beats carrying on with state the code already decided is
     * impossible. The player closes the app from the console's own menu. */
    for (;;)
        ;
}
