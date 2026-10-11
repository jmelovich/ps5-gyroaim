/* SPDX-License-Identifier: GPL-3.0-or-later
 * proc.c - finding the running game.
 *
 * The process table comes from sysctl KERN_PROC with the real kinfo_proc from
 * <sys/user.h>, and title ids come from sceKernelGetAppInfo, which is how the
 * SDK's own ps does it. Both beat hand-counted struct offsets.
 */
#include "proc.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>

/* Sony's per-process application info.
 *
 * The SDK sample declares this with natural alignment, which puts title_id
 * four bytes late and yields "99350" instead of "PPSA99350" - the struct is
 * actually packed. Rather than bet on either layout, read the block and find
 * the title id by its shape, which is unambiguous: four capitals then five
 * digits. The size is generous so a layout change cannot read short. */
#define APP_INFO_SIZE 0x80

extern int sceKernelGetAppInfo(pid_t pid, void *info);

/* "PPSA99350" and friends. */
static int titleid_shape(const char *s)
{
    for (int i = 0; i < 4; i++)
        if (s[i] < 'A' || s[i] > 'Z')
            return 0;
    for (int i = 4; i < 9; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    return 1;
}

int ga_proc_title_id(pid_t pid, char out[GA_TITLE_ID_MAX])
{
    uint8_t info[APP_INFO_SIZE];

    if (!out)
        return -1;
    out[0] = '\0';
    if (pid <= 0)
        return -1;

    memset(info, 0, sizeof(info));
    if (sceKernelGetAppInfo(pid, info) != 0)
        return -1;

    /* Only the first few words can hold it; stop well before unknown2. */
    for (unsigned off = 0; off + 9 <= 0x20; off++) {
        if (!titleid_shape((const char *)info + off))
            continue;
        memcpy(out, info + off, 9);
        out[9] = '\0';
        return 0;
    }
    return -1;
}

unsigned ga_proc_list(ga_proc_t *out, unsigned max)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
    size_t len = 0;
    uint8_t *buf;
    unsigned n = 0;

    if (!out || !max)
        return 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0 || len == 0)
        return 0;
    /* The table can grow between sizing it and reading it. */
    len += len / 8 + 4096;
    if (!(buf = malloc(len)))
        return 0;
    if (sysctl(mib, 4, buf, &len, NULL, 0) != 0) {
        free(buf);
        return 0;
    }

    const uint8_t *end = buf + len;
    for (uint8_t *p = buf; p + sizeof(struct kinfo_proc) <= end && n < max;) {
        const struct kinfo_proc *ki = (const struct kinfo_proc *)p;
        int size = ki->ki_structsize;

        if (size < (int)sizeof(struct kinfo_proc) ||
            (size_t)size > (size_t)(end - p))
            break;

        if (ki->ki_pid > 0) {
            out[n].pid = ki->ki_pid;
            snprintf(out[n].name, sizeof(out[n].name), "%s", ki->ki_comm);
            if (ga_proc_title_id(ki->ki_pid, out[n].title_id) != 0)
                out[n].title_id[0] = '\0';
            n++;
        }
        p += (size_t)size;
    }

    free(buf);
    return n;
}

unsigned ga_proc_find(const char *name, pid_t *out, unsigned max)
{
    ga_proc_t *all;
    unsigned n, found = 0;
    pid_t self = getpid();

    if (!name || !out || !max)
        return 0;
    if (!(all = malloc(sizeof(*all) * GA_PROC_MAX)))
        return 0;

    n = ga_proc_list(all, GA_PROC_MAX);
    for (unsigned i = 0; i < n && found < max; i++) {
        if (all[i].pid == self || strcmp(all[i].name, name) != 0)
            continue;
        int seen = 0;
        for (unsigned j = 0; j < found; j++)
            if (out[j] == all[i].pid)
                seen = 1;
        if (!seen)
            out[found++] = all[i].pid;
    }

    free(all);
    return found;
}

int ga_proc_alive(pid_t pid)
{
    ga_proc_t *all;
    unsigned n;
    int alive = 0;

    if (pid <= 0)
        return 0;
    if (!(all = malloc(sizeof(*all) * GA_PROC_MAX)))
        return 0;
    n = ga_proc_list(all, GA_PROC_MAX);
    for (unsigned i = 0; i < n; i++)
        if (all[i].pid == pid) {
            alive = 1;
            break;
        }
    free(all);
    return alive;
}

int ga_proc_self_name(char *out, size_t n)
{
    ga_proc_t *all;
    unsigned count;
    pid_t self = getpid();
    int rc = -1;

    if (!out || !n)
        return -1;
    out[0] = '\0';
    if (!(all = malloc(sizeof(*all) * GA_PROC_MAX)))
        return -1;

    count = ga_proc_list(all, GA_PROC_MAX);
    for (unsigned i = 0; i < count; i++)
        if (all[i].pid == self) {
            snprintf(out, n, "%s", all[i].name);
            rc = 0;
            break;
        }

    free(all);
    return rc;
}

int ga_proc_self_check(void)
{
    ga_proc_t *all;
    unsigned n;
    pid_t self = getpid();
    const char *found = NULL;

    if (!(all = malloc(sizeof(*all) * GA_PROC_MAX)))
        return -1;

    n = ga_proc_list(all, GA_PROC_MAX);
    for (unsigned i = 0; i < n; i++)
        if (all[i].pid == self)
            found = all[i].name;

    if (!n) {
        ga_log("process table unreadable: sysctl returned nothing");
        free(all);
        return -1;
    }
    if (!found) {
        ga_log("process table has %u entries but not our own pid %d",
               n, (int)self);
        free(all);
        return -1;
    }

    ga_log("process table ok: %u processes, pid %d is \"%s\"",
           n, (int)self, found);
    free(all);
    return 0;
}
