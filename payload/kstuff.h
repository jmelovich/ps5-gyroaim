/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Running a syscall inside another process.
 *
 * Kernel read and write can change a game's memory but cannot create a new
 * mapping in it, which the injected code needs. kstuff exposes a trap that
 * runs a syscall on behalf of another process; this is the thin wrapper.
 *
 * The mechanism and its magic numbers come from PoorDS4 by ItsBlurf
 * (GPL-3.0-or-later), which in turn documents kstuff-lite's interface.
 */

/* True if kstuff is loaded and answers its probe. */
int ga_kstuff_available(void);

/* Runs `number` with `args` in the context of `pid`. Returns the syscall's
 * return value, or a negative value on failure. */
int64_t ga_kstuff_syscall(pid_t pid, uint64_t number, const uint64_t args[6]);

/* Anonymous read/write mapping of `len` bytes inside `pid`, wired so it
 * cannot be paged out under the game's own allocator. 0 on success. */
int ga_kstuff_mmap(pid_t pid, size_t len, intptr_t *out_addr);

int ga_kstuff_munmap(pid_t pid, intptr_t addr, size_t len);
int ga_kstuff_mprotect(pid_t pid, intptr_t addr, size_t len, int prot);
