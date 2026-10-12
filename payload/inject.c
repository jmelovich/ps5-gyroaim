/* SPDX-License-Identifier: GPL-3.0-or-later
 * inject.c - putting the transform inside the game, and taking it out.
 *
 * The sequence, and why it is in this order:
 *   map  - a fresh anonymous page in the game, via kstuff, since kernel
 *          read and write cannot create a mapping
 *   code - copy the stub block, then one small gateway per hooked function
 *          whose only job is to put the shared page's address in the register
 *          the stub expects before jumping to it
 *   seal - make the code read and execute only
 *   args - fill in the shared page, with the transform parked
 *   slot - redirect each import slot, raising page protection to write and
 *          putting it back afterwards
 *
 * A failure after the first slot is redirected rolls the earlier ones back.
 */
#include "config.h"
#include "inject.h"
#include "kstuff.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#endif

#define TARGET_PAGE   0x4000u
#define GATEWAY_SIZE  16u
#define STUB_MAX      0x4000u
#define WRITE_CHUNK   0x800u

#define ALIGN_UP(v, a) (((v) + (a) - 1) & ~((a) - 1))

/* Where a process's injected mapping is remembered between service restarts.
 *
 * Removing hooks deliberately leaves the mapping in place, because a game
 * thread can be inside the stub and pulling the code out from under it would
 * crash the game. That means each install would otherwise map another 32 KB
 * inside the game, and a title's memory budget is finite: a long-running game
 * eventually refuses the allocation and the transform cannot be installed at
 * all. Remembering the address lets a restarted service point the slots back
 * at the code already sitting there. */
#define MAP_RECORD_DIR "/data/gyroaim"

static void map_record_path(pid_t pid, char *out, size_t n)
{
    snprintf(out, n, "%s/map-%d.txt", MAP_RECORD_DIR, (int)pid);
}

void ga_inject_forget(pid_t pid)
{
    char path[128];

    map_record_path(pid, path, sizeof(path));
    unlink(path);
}

/* Which stub serves which function, and which register carries the shared
 * page: the stub takes it as the argument after the real ones, so it lands in
 * rdx for a two-argument function and rcx for a three-argument one. */
enum { REG_RDX = 0, REG_RCX = 1 };

static const struct {
    int      kind;
    void    *stub;
    int      reg;
} k_stubs[] = {
    { GA_FN_READ_STATE,     (void *)ga_stub_read_state,     REG_RDX },
    { GA_FN_READ_STATE_EXT, (void *)ga_stub_read_state_ext, REG_RDX },
    { GA_FN_READ,           (void *)ga_stub_read,           REG_RCX },
};
#define N_STUBS (sizeof(k_stubs) / sizeof(k_stubs[0]))

/* lea <reg>, [rip + disp]; jmp <stub>  */
static int make_gateway(uint8_t out[GATEWAY_SIZE], intptr_t gateway,
                        intptr_t args, intptr_t stub, int reg)
{
    int64_t lea_disp = (int64_t)args - (gateway + 7);
    int64_t jmp_rel = (int64_t)stub - (gateway + 12);

    if (lea_disp < INT32_MIN || lea_disp > INT32_MAX ||
        jmp_rel < INT32_MIN || jmp_rel > INT32_MAX)
        return -1;

    memset(out, 0x90, GATEWAY_SIZE);
    out[0] = 0x48;
    out[1] = 0x8d;
    out[2] = (reg == REG_RCX) ? 0x0d : 0x15;
    int32_t d = (int32_t)lea_disp;
    memcpy(out + 3, &d, sizeof(d));
    out[7] = 0xe9;
    int32_t r = (int32_t)jmp_rel;
    memcpy(out + 8, &r, sizeof(r));
    return 0;
}

#ifdef __PROSPERO__

static int write_game(pid_t pid, intptr_t addr, const void *src, size_t len)
{
    const uint8_t *p = src;

    /* Large transfers are less reliable than several small ones, and a
     * partial write here would leave half a function in the game. */
    for (size_t done = 0; done < len; done += WRITE_CHUNK) {
        size_t n = len - done < WRITE_CHUNK ? len - done : WRITE_CHUNK;
        if (kernel_proc_copyin(pid, p + done, addr + (intptr_t)done, n) != 0)
            return -1;
    }
    return 0;
}

static int verify_game(pid_t pid, intptr_t addr, const void *src, size_t len)
{
    uint8_t buf[WRITE_CHUNK];
    const uint8_t *p = src;

    for (size_t done = 0; done < len; done += WRITE_CHUNK) {
        size_t n = len - done < WRITE_CHUNK ? len - done : WRITE_CHUNK;
        memset(buf, 0, n);
        if (kernel_proc_copyout(pid, addr + (intptr_t)done, buf, n) != 0)
            return -1;
        if (memcmp(buf, p + done, n) != 0)
            return -1;
    }
    return 0;
}

/* Writes one pointer into a page that is normally read-only. */
static int write_slot(pid_t pid, intptr_t slot, uint64_t value, uint32_t prot)
{
    intptr_t page = slot & ~(intptr_t)(TARGET_PAGE - 1);
    int rc = -1;

    if (ga_kstuff_mprotect(pid, page, TARGET_PAGE,
                           PROT_READ | PROT_WRITE) != 0) {
        ga_log("inject: could not make 0x%lx writable", (long)page);
        return -1;
    }
    if (kernel_proc_copyin(pid, &value, slot, sizeof(value)) == 0)
        rc = 0;

    /* Always put the protection back, even if the write failed. */
    if (ga_kstuff_mprotect(pid, page, TARGET_PAGE,
                           prot ? (int)prot : PROT_READ) != 0)
        ga_log("inject: warning, could not restore protection on 0x%lx",
               (long)page);
    return rc;
}

static void map_record_save(const ga_inject_t *in)
{
    char path[128];
    FILE *f;

    map_record_path(in->pid, path, sizeof(path));
    if (!(f = fopen(path, "w")))
        return;
    fprintf(f, "code=0x%lx\nargs=0x%lx\nlen=%zu\n",
            (long)in->code, (long)in->args, in->code_len);
    fclose(f);
}

/* Returns 0 if a mapping from an earlier install is still usable. The stub
 * bytes are compared before trusting it, so a rebuilt service never reuses
 * somebody else's or an older build's code. */
static int map_record_reuse(pid_t pid, const void *stub, size_t stub_len,
                            ga_inject_t *out)
{
    char path[128];
    FILE *f;
    long code = 0, args = 0;
    size_t len = 0;

    map_record_path(pid, path, sizeof(path));
    if (!(f = fopen(path, "r")))
        return -1;
    if (fscanf(f, "code=0x%lx args=0x%lx len=%zu", &code, &args, &len) != 3) {
        fclose(f);
        unlink(path);
        return -1;
    }
    fclose(f);

    if (!code || !args || !len) {
        unlink(path);
        return -1;
    }

    if (verify_game(pid, (intptr_t)code, stub, stub_len) != 0) {
        ga_log("inject: the mapping remembered at 0x%lx no longer holds this "
               "build's code; mapping fresh memory", code);
        unlink(path);
        return -1;
    }

    uint32_t header[2] = { 0, 0 };
    if (kernel_proc_copyout(pid, (intptr_t)args, header, sizeof(header)) != 0 ||
        header[0] != GA_MAGIC) {
        unlink(path);
        return -1;
    }

    out->pid = pid;
    out->code = (intptr_t)code;
    out->args = (intptr_t)args;
    out->code_len = len;
    ga_log("inject: reusing the mapping already in this game at 0x%lx", code);
    return 0;
}

int ga_inject_install(const ga_target_t *t, const ga_slot_t *slots,
                      unsigned slot_count, ga_inject_t *out)
{
    if (!t || !slots || !slot_count || !out)
        return -1;
    memset(out, 0, sizeof(*out));
    out->pid = t->pid;

    if (!ga_kstuff_available()) {
        ga_log("inject: kstuff remote syscalls unavailable");
        return -1;
    }

    /* The block to copy runs from the first stub to the end marker. */
    uintptr_t begin = (uintptr_t)ga_stub_read_state;
    uintptr_t end = (uintptr_t)ga_stub_end;

    if (end <= begin || end - begin > STUB_MAX) {
        ga_log("inject: stub block looks wrong (%ld bytes)",
               (long)(end - begin));
        return -1;
    }
    for (unsigned i = 0; i < N_STUBS; i++) {
        uintptr_t s = (uintptr_t)k_stubs[i].stub;
        if (s < begin || s >= end) {
            ga_log("inject: stub %u is outside the block; check the section "
                   "attribute", i);
            return -1;
        }
    }

    /* Which kinds we have a stub for. Checked before anything is mapped: an
     * earlier version mapped memory in the game, wrote the code, and only
     * then discovered it could serve none of the slots, so every retry
     * churned the game's address space for nothing. */
    int served[GA_FN_COUNT];
    memset(served, 0, sizeof(served));
    for (unsigned i = 0; i < N_STUBS; i++)
        served[k_stubs[i].kind] = 1;

    unsigned usable = 0;
    for (unsigned i = 0; i < slot_count; i++)
        if (slots[i].kind >= 0 && slots[i].kind < GA_FN_COUNT &&
            served[slots[i].kind] && t->fn[slots[i].kind] > 0)
            usable++;
    if (!usable) {
        ga_log("inject: none of the %u slot(s) found is one we can serve",
               slot_count);
        return -1;
    }

    size_t stub_len = end - begin;
    size_t gw_off = ALIGN_UP(stub_len, 16u);
    size_t code_len = ALIGN_UP(gw_off + N_STUBS * GATEWAY_SIZE, TARGET_PAGE);
    size_t args_len = ALIGN_UP(sizeof(ga_args_t), TARGET_PAGE);

    intptr_t map = 0;
    int fresh = map_record_reuse(t->pid, (const void *)begin, stub_len,
                                 out) != 0;

    if (fresh) {
        if (ga_kstuff_mmap(t->pid, code_len + args_len, &map) != 0)
            return -1;
        out->code = map;
        out->code_len = code_len;
        out->args = map + (intptr_t)code_len;
        ga_log("inject: %zu bytes of code at 0x%lx, shared page at 0x%lx",
               stub_len, (long)out->code, (long)out->args);
    }

    /* Gateway addresses follow from the code base, so they are the same
     * whether the code was just written or was already there. */
    intptr_t gw_for_kind[GA_FN_COUNT];
    memset(gw_for_kind, 0, sizeof(gw_for_kind));
    for (unsigned i = 0; i < N_STUBS; i++)
        gw_for_kind[k_stubs[i].kind] =
            out->code + (intptr_t)gw_off + (intptr_t)(i * GATEWAY_SIZE);

    if (fresh) {
        if (write_game(t->pid, out->code, (const void *)begin, stub_len) != 0 ||
            verify_game(t->pid, out->code, (const void *)begin,
                        stub_len) != 0) {
            ga_log("inject: copying the stub failed");
            goto fail_unmap;
        }

        for (unsigned i = 0; i < N_STUBS; i++) {
            intptr_t gw = gw_for_kind[k_stubs[i].kind];
            intptr_t stub = out->code +
                            (intptr_t)((uintptr_t)k_stubs[i].stub - begin);
            uint8_t bytes[GATEWAY_SIZE];

            if (make_gateway(bytes, gw, out->args, stub,
                             k_stubs[i].reg) != 0 ||
                write_game(t->pid, gw, bytes, sizeof(bytes)) != 0) {
                ga_log("inject: gateway %u failed", i);
                goto fail_unmap;
            }
        }

        if (ga_kstuff_mprotect(t->pid, out->code, code_len,
                               PROT_READ | PROT_EXEC) != 0) {
            ga_log("inject: could not make the stub executable");
            goto fail_unmap;
        }
    }

    /* The shared page, with the transform parked. */
    ga_args_t *a = calloc(1, sizeof(*a));
    if (!a)
        goto fail_unmap;

    a->magic = GA_MAGIC;
    a->version = GA_LAYOUT_VERSION;
    /* The stub cannot reach a constant in .rodata, so its arithmetic
     * constants are handed over here and never change afterwards. */
    ga_args_init_consts(a);
    for (int k = 0; k < GA_FN_COUNT; k++)
        a->orig[k] = (uint64_t)(t->fn[k] > 0 ? t->fn[k] : 0);
    a->enabled = 0;
    a->activation = GA_ACT_NEVER;
    a->age = GA_STALE_READS + 1;

    unsigned n = 0;
    for (unsigned i = 0; i < slot_count && n < GA_MAX_SLOTS; i++) {
        if (!gw_for_kind[slots[i].kind])
            continue;               /* no stub serves this one */
        a->hook_slot[n] = (uint64_t)slots[i].slot;
        a->hook_orig[n] = (uint64_t)slots[i].original;
        a->hook_prot[n] = slots[i].protection;
        n++;
    }
    a->hook_count = n;

    if (write_game(t->pid, out->args, a, sizeof(*a)) != 0) {
        ga_log("inject: writing the shared page failed");
        free(a);
        goto fail_unmap;
    }

    /* Point the slots at the gateways, newest failure rolls the rest back. */
    unsigned done = 0;
    for (unsigned i = 0; i < n; i++) {
        intptr_t slot = (intptr_t)a->hook_slot[i];
        uint64_t current = 0;

        /* Make sure nobody else moved it since the scan. */
        if (kernel_proc_copyout(t->pid, slot, &current, sizeof(current)) != 0 ||
            current != a->hook_orig[i]) {
            ga_log("inject: slot 0x%lx changed under us", (long)slot);
            goto fail_rollback;
        }

        int kind = -1;
        for (unsigned j = 0; j < slot_count; j++)
            if ((uint64_t)slots[j].slot == a->hook_slot[i])
                kind = slots[j].kind;
        if (kind < 0 || !gw_for_kind[kind])
            goto fail_rollback;

        if (write_slot(t->pid, slot, (uint64_t)gw_for_kind[kind],
                       a->hook_prot[i]) != 0) {
            ga_log("inject: could not redirect slot 0x%lx", (long)slot);
            goto fail_rollback;
        }
        ga_log("inject: %s now goes through 0x%lx",
               ga_fn_names[kind], (long)gw_for_kind[kind]);
        done++;
    }

    out->hooked = done;
    out->installed = 1;
    map_record_save(out);
    free(a);
    ga_log("inject: installed, %u slot(s) redirected, transform parked",
           done);
    return 0;

fail_rollback:
    for (unsigned i = 0; i < done; i++)
        write_slot(t->pid, (intptr_t)a->hook_slot[i], a->hook_orig[i],
                   a->hook_prot[i]);
    free(a);
fail_unmap:
    /* Only safe to drop a mapping we just made: nothing in the game points at
     * it yet. A reused one may still be referenced. */
    if (fresh && map)
        ga_kstuff_munmap(t->pid, map, code_len + args_len);
    memset(out, 0, sizeof(*out));
    return -1;
}

int ga_inject_adopt(const ga_target_t *t, const ga_orphan_t *orphan,
                    ga_inject_t *out)
{
#ifndef __PROSPERO__
    (void)t; (void)orphan; (void)out;
    return -1;
#else
    ga_args_t *a;

    if (!t || !orphan || !out || !orphan->args)
        return -1;

    if (orphan->version != GA_LAYOUT_VERSION) {
        ga_log("orphan: its layout is version %u and ours is %u, so its "
               "fields cannot be trusted; this game needs restarting to "
               "clear it", orphan->version, GA_LAYOUT_VERSION);
        return -1;
    }

    if (!(a = calloc(1, sizeof(*a))))
        return -1;

    if (kernel_proc_copyout(t->pid, orphan->args, a, sizeof(*a)) != 0 ||
        a->magic != GA_MAGIC || a->version != GA_LAYOUT_VERSION) {
        ga_log("orphan: its shared page could not be read back");
        free(a);
        return -1;
    }
    if (!a->hook_count || a->hook_count > GA_MAX_SLOTS) {
        ga_log("orphan: it claims %u hooked slot(s), which cannot be right",
               a->hook_count);
        free(a);
        return -1;
    }

    /* Every slot it claims must still point into it, or something else has
     * been at the game since and guessing would be worse than starting over. */
    unsigned confirmed = 0;
    for (unsigned i = 0; i < a->hook_count; i++) {
        uint64_t value = 0;
        if (kernel_proc_copyout(t->pid, (intptr_t)a->hook_slot[i], &value,
                                sizeof(value)) != 0)
            continue;
        if (value != a->hook_orig[i])
            confirmed++;        /* still redirected somewhere, i.e. at us */
    }
    if (!confirmed) {
        ga_log("orphan: none of its slots is still redirected");
        free(a);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->pid = t->pid;
    out->args = orphan->args;
    out->hooked = confirmed;
    out->installed = 1;
    /* The code stays where it is; nothing here unmaps it, and the restore
     * table needed to undo the hooks is on the page itself. */
    out->code = 0;
    out->code_len = 0;

    ga_log("orphan: taken over, %u of %u slot(s) still redirected; the "
           "transform is live again without touching the game", confirmed,
           a->hook_count);
    free(a);
    return 0;
#endif
}

int ga_inject_publish(const ga_inject_t *in, const ga_args_t *s)
{
    if (!in || !in->installed || !s)
        return -1;

    /* Settings only, never the telemetry or the restore table, and the age
     * counter last so the stub sees fresh settings before it is unparked. */
    size_t off = __builtin_offsetof(ga_args_t, enabled);
    size_t len = __builtin_offsetof(ga_args_t, settings_seq) +
                 sizeof(s->settings_seq) - off;

    if (kernel_proc_copyin(in->pid, (const uint8_t *)s + off,
                           in->args + (intptr_t)off, len) != 0)
        return -1;

    uint32_t zero = 0;
    return kernel_proc_copyin(in->pid, &zero,
                              in->args +
                              (intptr_t)__builtin_offsetof(ga_args_t, age),
                              sizeof(zero));
}

int ga_inject_read(const ga_inject_t *in, ga_args_t *out)
{
    if (!in || !in->installed || !out)
        return -1;
    if (kernel_proc_copyout(in->pid, in->args, out, sizeof(*out)) != 0)
        return -1;
    return out->magic == GA_MAGIC ? 0 : -1;
}

int ga_inject_remove(ga_inject_t *in)
{
    ga_args_t *a;
    unsigned restored = 0;

    if (!in || !in->installed)
        return -1;
    if (!(a = calloc(1, sizeof(*a))))
        return -1;

    if (ga_inject_read(in, a) != 0) {
        ga_log("remove: cannot read the shared page back");
        free(a);
        return -1;
    }

    /* Park it first, so the stub stops touching input even if a slot write
     * fails below. */
    uint32_t off = 0;
    kernel_proc_copyin(in->pid, &off,
                       in->args + (intptr_t)__builtin_offsetof(ga_args_t,
                                                               enabled),
                       sizeof(off));

    for (unsigned i = 0; i < a->hook_count && i < GA_MAX_SLOTS; i++)
        if (write_slot(in->pid, (intptr_t)a->hook_slot[i], a->hook_orig[i],
                       a->hook_prot[i]) == 0)
            restored++;

    ga_log("remove: %u/%u slot(s) restored", restored, a->hook_count);

    /* The mapping is deliberately left in place. A thread could be inside the
     * stub right now, and unmapping code out from under it would crash the
     * game; one spare page costs nothing and dies with the process. */
    free(a);
    in->installed = 0;
    return restored == a->hook_count ? 0 : -1;
}

#else

int ga_inject_install(const ga_target_t *t, const ga_slot_t *s, unsigned n,
                      ga_inject_t *o)
{
    (void)t; (void)s; (void)n; (void)o;
    return -1;
}
int ga_inject_publish(const ga_inject_t *i, const ga_args_t *s)
{
    (void)i; (void)s;
    return -1;
}
int ga_inject_read(const ga_inject_t *i, ga_args_t *o)
{
    (void)i; (void)o;
    return -1;
}
int ga_inject_remove(ga_inject_t *i)
{
    (void)i;
    return -1;
}

#endif
