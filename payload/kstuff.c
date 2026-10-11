/* SPDX-License-Identifier: GPL-3.0-or-later
 * kstuff.c - running a syscall inside another process.
 *
 * Adapted from PoorDS4 by ItsBlurf (GPL-3.0-or-later), which documents
 * kstuff-lite's remote-syscall trap. The bridge below loads a magic value
 * into the syscall-number register and jumps at the syscall instruction
 * inside libkernel's getpid wrapper, which kstuff watches.
 */
#include "kstuff.h"
#include "log.h"

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define KEKCALL_PROBE          UINT64_C(0xffffffff00000027)
#define KEKCALL_REMOTE_SYSCALL UINT64_C(0x500000027)

/* Target page size inside a title. Mappings must be a multiple of it. */
#define TARGET_PAGE 0x4000u

/* Seven arguments: six go in registers the kernel expects, and the seventh
 * lands on the stack, from where the bridge moves it into rax. */
void *p_ga_kekcall;
__asm__(".text\n"
        ".global ga_kekcall\n"
        ".type ga_kekcall,@function\n"
        "ga_kekcall:\n"
        "mov 8(%rsp), %rax\n"
        "jmp *p_ga_kekcall(%rip)\n");
extern uint64_t ga_kekcall(uint64_t, uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t);

static void bridge_init(void)
{
    /* +7 is the syscall instruction inside getpid's wrapper. */
    p_ga_kekcall = (uint8_t *)(uintptr_t)getpid + 7;
}

int ga_kstuff_available(void)
{
    bridge_init();
    return ga_kekcall(0, 0, 0, 0, 0, 0, KEKCALL_PROBE) == 0;
}

int64_t ga_kstuff_syscall(pid_t pid, uint64_t number, const uint64_t args[6])
{
    if (pid <= 0 || !args)
        return -1;
    bridge_init();
    return (int64_t)ga_kekcall((uint64_t)(uint32_t)pid, number,
                               (uint64_t)(uintptr_t)args, 0, 0, 0,
                               KEKCALL_REMOTE_SYSCALL);
}

int ga_kstuff_mmap(pid_t pid, size_t len, intptr_t *out_addr)
{
    if (!out_addr || !len)
        return -1;
    len = (len + TARGET_PAGE - 1) & ~(size_t)(TARGET_PAGE - 1);

    uint64_t args[6] = { 0, (uint64_t)len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, UINT64_MAX, 0 };
    int64_t addr = ga_kstuff_syscall(pid, SYS_mmap, args);

    if (addr <= 0 || (uint64_t)addr > (uint64_t)INTPTR_MAX) {
        ga_log("remote mmap of %zu bytes in pid %d failed (%lld)",
               len, (int)pid, (long long)addr);
        return -1;
    }

    /* The game must not have this paged out from under an in-flight call. */
    uint64_t lock[6] = { (uint64_t)addr, (uint64_t)len, 0, 0, 0, 0 };
    if (ga_kstuff_syscall(pid, SYS_mlock, lock) != 0) {
        ga_log("remote mlock failed; unmapping");
        ga_kstuff_munmap(pid, (intptr_t)addr, len);
        return -1;
    }

    *out_addr = (intptr_t)addr;
    return 0;
}

int ga_kstuff_munmap(pid_t pid, intptr_t addr, size_t len)
{
    len = (len + TARGET_PAGE - 1) & ~(size_t)(TARGET_PAGE - 1);
    uint64_t args[6] = { (uint64_t)addr, (uint64_t)len, 0, 0, 0, 0 };
    return ga_kstuff_syscall(pid, SYS_munmap, args) == 0 ? 0 : -1;
}

int ga_kstuff_mprotect(pid_t pid, intptr_t addr, size_t len, int prot)
{
    uint64_t args[6] = { (uint64_t)addr, (uint64_t)len,
                         (uint64_t)(uint32_t)prot, 0, 0, 0 };
    return ga_kstuff_syscall(pid, SYS_mprotect, args) == 0 ? 0 : -1;
}
