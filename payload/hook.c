/* SPDX-License-Identifier: GPL-3.0-or-later
 * hook.c - looking into a running game.
 *
 * Everything here is read-only. It establishes that the game has libScePad
 * loaded and that its pad entry points can be resolved from outside, which is
 * the groundwork for redirecting the game's import slots later.
 */
#include "hook.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#endif

const char *const ga_fn_names[GA_FN_COUNT] = {
    "scePadReadState",
    "scePadReadStateExt",
    "scePadRead",
    "scePadReadExt",
    "scePadSetVibration",
    "scePadSetVibrationMode",
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

/* ---- finding the game's import slots ---------------------------------- */

#ifdef __PROSPERO__

#include <elf.h>

/* The runtime-linker object, as the kernel holds it. Offsets are asserted;
 * they have held from firmware 6.02 through 13.x. */
typedef struct {
    uint64_t next;                      /* 0x00 */
    uint64_t path;                      /* 0x08 */
    uint64_t unknown0[2];               /* 0x10 */
    uint32_t refcount;                  /* 0x20 */
    uint32_t alignment0;                /* 0x24 */
    uint64_t handle;                    /* 0x28 */
    uint64_t mapbase;                   /* 0x30 */
    uint64_t mapsize;                   /* 0x38 */
    uint64_t sections;                  /* 0x40 */
    uint64_t section_count;             /* 0x48 */
    uint64_t unknown1;                  /* 0x50 */
    uint8_t  reserved[0x148 - 0x58];    /* 0x58 */
    uint64_t dynsec;                    /* 0x148 */
} ga_rtld_object_t;

_Static_assert(__builtin_offsetof(ga_rtld_object_t, handle) == 0x28, "");
_Static_assert(__builtin_offsetof(ga_rtld_object_t, mapbase) == 0x30, "");
_Static_assert(__builtin_offsetof(ga_rtld_object_t, sections) == 0x40, "");
_Static_assert(__builtin_offsetof(ga_rtld_object_t, dynsec) == 0x148, "");

typedef struct {
    uint64_t list_next;                 /* 0x00 */
    uint64_t list_prev;                 /* 0x08 */
    uint64_t sysvec;                    /* 0x10 */
    uint32_t refcount;                  /* 0x18 */
    uint32_t alignment0;                /* 0x1c */
    uint64_t size;                      /* 0x20 */
    uint64_t symtab;                    /* 0x28 */
    uint64_t symtab_size;               /* 0x30 */
    uint64_t strtab;                    /* 0x38 */
    uint64_t strtab_size;               /* 0x40 */
    uint64_t plt_rela;                  /* 0x48 */
    uint64_t plt_rela_size;             /* 0x50 */
} ga_rtld_dynsec_t;

_Static_assert(__builtin_offsetof(ga_rtld_dynsec_t, symtab) == 0x28, "");
_Static_assert(__builtin_offsetof(ga_rtld_dynsec_t, plt_rela) == 0x48, "");

int ga_hook_verbose = 0;

#define OBJ_LIST_SCAN_LO   0x300
#define OBJ_LIST_SCAN_HI   0x600
#define OBJ_LIST_MAX       512
#define RELA_MAX           200000

/* The object list and the structures it points at live in kernel memory, but
 * a slot is an address in the game. Read whichever way works. */
static int read_any(pid_t pid, intptr_t addr, void *buf, size_t len)
{
    /* Kernel addresses have the top bit set, so they are negative as a
     * signed intptr_t. Only zero means "no address". */
    if (!addr)
        return -1;
    if (kernel_copyout(addr, buf, len) == 0)
        return 0;
    return kernel_proc_copyout(pid, addr, buf, len);
}

static int object_sane(const ga_rtld_object_t *o)
{
    return o->mapbase != 0 &&
           o->mapsize >= sizeof(uint64_t) &&
           o->mapsize < 0x100000000ull &&
           o->dynsec != 0;
}

/* Walks the list at `head` and reports whether it contains `want_mapbase`. */
static int list_contains(pid_t pid, uint64_t head, uint64_t want_mapbase,
                         unsigned *out_count)
{
    uint64_t at = head;
    unsigned n = 0;
    int hit = 0;

    while (at && n < OBJ_LIST_MAX) {
        ga_rtld_object_t o;
        if (read_any(pid, (intptr_t)at, &o, sizeof(o)) != 0)
            return 0;
        n++;
        if (o.mapbase == want_mapbase)
            hit = 1;
        at = o.next;
    }
    if (out_count)
        *out_count = n;
    return hit && n > 1;
}

/* The offset of the object list inside struct proc is firmware specific, so
 * find it by looking for the list that contains the library we already
 * resolved independently. Self-validating beats a hardcoded constant. */
static int find_object_list(const ga_target_t *t, uint64_t *out_head)
{
    intptr_t proc = kernel_get_proc(t->pid);

    if (!proc) {
        ga_log("slots: kernel_get_proc(%d) failed", (int)t->pid);
        return -1;
    }

    for (unsigned off = OBJ_LIST_SCAN_LO; off <= OBJ_LIST_SCAN_HI; off += 8) {
        uint64_t p = 0, head = 0;
        unsigned count = 0;

        if (kernel_copyout(proc + off, &p, sizeof(p)) != 0 || !p)
            continue;
        /* The field may be the list head itself or a pointer to it. */
        if (list_contains(t->pid, p, (uint64_t)t->libpad_base, &count)) {
            head = p;
        } else if (kernel_copyout((intptr_t)p, &head, sizeof(head)) == 0 &&
                   head &&
                   list_contains(t->pid, head, (uint64_t)t->libpad_base,
                                 &count)) {
            /* head already set */
        } else {
            continue;
        }

        ga_log("slots: object list at proc+0x%x, %u objects, contains "
               "libScePad", off, count);
        *out_head = head;
        return 0;
    }

    ga_log("slots: could not find an object list containing libScePad "
           "(base 0x%lx) in proc 0x%lx", (long)t->libpad_base, (long)proc);
    return -1;
}

static void object_path(pid_t pid, const ga_rtld_object_t *o, char *buf,
                        size_t n)
{
    buf[0] = '\0';
    if (!o->path)
        return;
    if (read_any(pid, (intptr_t)o->path, buf, n - 1) != 0) {
        buf[0] = '\0';
        return;
    }
    buf[n - 1] = '\0';
}

/* Only the game's own modules are candidates. Patching a system module or one
 * of ShadowMount's overlay libraries would reach beyond this game's lifetime. */
static int object_is_games_own(const ga_rtld_object_t *o, const char *path)
{
    if (o->handle == 0)
        return 1;                       /* the executable itself */
    if (!path[0])
        return 0;
    if (strstr(path, "fakelib") || strstr(path, "/system/") ||
        strstr(path, "/system_ex/") || strstr(path, "/common/"))
        return 0;
    return strstr(path, "app0") != NULL || strstr(path, "shadowmnt") != NULL;
}

int ga_hook_find_slots(const ga_target_t *t, ga_slot_t *out, unsigned max)
{
    uint64_t head = 0;
    unsigned found = 0, objects = 0, relas = 0;

    if (!t || !out || !max)
        return -1;
    if (find_object_list(t, &head) != 0)
        return -1;

    uint64_t at = head;
    while (at && objects < OBJ_LIST_MAX && found < max) {
        ga_rtld_object_t o;
        char path[192];

        if (read_any(t->pid, (intptr_t)at, &o, sizeof(o)) != 0)
            break;
        objects++;
        at = o.next;

        object_path(t->pid, &o, path, sizeof(path));

        ga_rtld_dynsec_t ds;
        int have_ds = o.dynsec &&
                      read_any(t->pid, (intptr_t)o.dynsec, &ds,
                               sizeof(ds)) == 0;
        if (!have_ds)
            memset(&ds, 0, sizeof(ds));

        int sane = object_sane(&o);
        int own = object_is_games_own(&o, path);

        if (ga_hook_verbose)
            ga_log("  obj %2u h=%llu base=0x%llx size=0x%llx sane=%d own=%d "
                   "ds=%d rela=0x%llx/%llu %s",
                   objects, (unsigned long long)o.handle,
                   (unsigned long long)o.mapbase,
                   (unsigned long long)o.mapsize, sane, own, have_ds,
                   (unsigned long long)ds.plt_rela,
                   (unsigned long long)ds.plt_rela_size,
                   path[0] ? path : "<the executable>");

        if (!sane || !own || !have_ds)
            continue;
        if (!ds.plt_rela || !ds.plt_rela_size ||
            ds.plt_rela_size > 0x02000000 ||
            ds.plt_rela_size % sizeof(Elf64_Rela) != 0)
            continue;

        uint64_t count = ds.plt_rela_size / sizeof(Elf64_Rela);
        if (count > RELA_MAX)
            continue;

        ga_log("slots: scanning %llu relocations of %s (base 0x%llx)",
               (unsigned long long)count, path[0] ? path : "<the executable>",
               (unsigned long long)o.mapbase);

        for (uint64_t i = 0; i < count && found < max; i++) {
            Elf64_Rela rela;
            uint64_t value = 0;

            if (read_any(t->pid, (intptr_t)(ds.plt_rela + i * sizeof(rela)),
                         &rela, sizeof(rela)) != 0)
                continue;
            relas++;

            if (rela.r_offset > o.mapsize - sizeof(uint64_t))
                continue;
            intptr_t slot = (intptr_t)(o.mapbase + rela.r_offset);

            if (kernel_proc_copyout(t->pid, slot, &value, sizeof(value)) != 0)
                continue;

            for (int k = 0; k < GA_FN_COUNT; k++) {
                if (!t->fn[k] || value != (uint64_t)t->fn[k])
                    continue;
                int prot = kernel_get_vmem_protection(t->pid, slot,
                                                      sizeof(uint64_t));
                out[found].slot = slot;
                out[found].original = (intptr_t)value;
                out[found].protection = prot > 0 ? (uint32_t)prot : 0;
                out[found].kind = k;
                ga_log("  %-38s slot 0x%lx -> 0x%llx prot 0x%x",
                       ga_fn_names[k], (long)slot,
                       (unsigned long long)value, out[found].protection);
                found++;
                break;
            }
        }
    }

    ga_log("slots: %u objects walked, %u relocations read, %u pad slot(s) found",
           objects, relas, found);
    return (int)found;
}

/* ---- diagnostics ------------------------------------------------------- */

/* Two things the scan's output cast doubt on: whether reading successive
 * chunks really returns successive memory, and whether the protection query
 * works at all. Both are cheap to settle directly. */
void ga_hook_verify_reads(const ga_target_t *t, const intptr_t *addrs,
                          unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        uint64_t v = 0;
        int rc = kernel_proc_copyout(t->pid, addrs[i], &v, sizeof(v));
        int prot = kernel_get_vmem_protection(t->pid, addrs[i],
                                              sizeof(uint64_t));
        ga_log("verify: 0x%lx -> rc=%d value=0x%llx prot=%d",
               (long)addrs[i], rc, (unsigned long long)v, prot);
    }

    /* Does a chunked read advance? Compare a 32-byte window read on its own
     * against the same window read as part of a larger block. */
    uint8_t small[32], big[0x4000];
    intptr_t probe = (intptr_t)t->fn[GA_FN_READ_STATE];
    (void)probe;
    if (n) {
        intptr_t page = addrs[0] & ~(intptr_t)0x3fff;
        if (kernel_proc_copyout(t->pid, page + 0xdf8, small,
                                sizeof(small)) == 0 &&
            kernel_proc_copyout(t->pid, page, big, sizeof(big)) == 0) {
            int same = memcmp(small, big + 0xdf8, sizeof(small)) == 0;
            ga_log("verify: chunked read %s the direct read at 0x%lx",
                   same ? "agrees with" : "DISAGREES with", (long)page);
        }
        /* And are two adjacent pages actually different memory? */
        uint8_t a[64], b[64];
        if (kernel_proc_copyout(t->pid, page, a, sizeof(a)) == 0 &&
            kernel_proc_copyout(t->pid, page + 0x4000, b, sizeof(b)) == 0)
            ga_log("verify: pages 0x%lx and 0x%lx are %s",
                   (long)page, (long)(page + 0x4000),
                   memcmp(a, b, sizeof(a)) == 0 ? "IDENTICAL" : "different");
    }
}

/* ---- finding slots by value ------------------------------------------- */

#define SCAN_CHUNK 0x4000

int ga_hook_scan_slots(const ga_target_t *t, ga_slot_t *out, unsigned max)
{
    uint64_t head = 0;
    unsigned found = 0, objects = 0, scanned = 0;
    uint8_t *chunk;

    if (!t || !out || !max)
        return -1;
    if (find_object_list(t, &head) != 0)
        return -1;
    if (!(chunk = malloc(SCAN_CHUNK)))
        return -1;

    uint64_t at = head;
    while (at && objects < OBJ_LIST_MAX && found < max) {
        ga_rtld_object_t o;
        char path[192];

        if (read_any(t->pid, (intptr_t)at, &o, sizeof(o)) != 0)
            break;
        objects++;
        at = o.next;

        if (!object_sane(&o))
            continue;
        object_path(t->pid, &o, path, sizeof(path));
        if (!object_is_games_own(&o, path))
            continue;

        scanned++;
        ga_log("scan: %s, 0x%llx bytes at 0x%llx",
               path[0] ? path : "<the executable>",
               (unsigned long long)o.mapsize,
               (unsigned long long)o.mapbase);

        for (uint64_t off = 0; off < o.mapsize && found < max;
             off += SCAN_CHUNK) {
            size_t len = SCAN_CHUNK;
            if (off + len > o.mapsize)
                len = (size_t)(o.mapsize - off);
            len &= ~(size_t)7;
            if (!len)
                break;

            intptr_t base = (intptr_t)(o.mapbase + off);

            /* A partial read can report success while leaving the tail of
             * the buffer holding the previous chunk, which then shows up as
             * a run of phantom hits one page apart. Zeroing first makes any
             * unwritten byte harmless, since no export address is zero. */
            memset(chunk, 0, len);
            if (kernel_proc_copyout(t->pid, base, chunk, len) != 0)
                continue;       /* unmapped or unreadable: not a slot */

            for (size_t i = 0; i + 8 <= len && found < max; i += 8) {
                uint64_t value;
                memcpy(&value, chunk + i, sizeof(value));
                if (!value)
                    continue;

                for (int k = 0; k < GA_FN_COUNT; k++) {
                    if (!t->fn[k] || value != (uint64_t)t->fn[k])
                        continue;

                    intptr_t slot = base + (intptr_t)i;

                    /* Confirm the hit with its own read. Hits are rare, so
                     * this costs nothing and rules out buffer artefacts. */
                    uint64_t confirm = 0;
                    if (kernel_proc_copyout(t->pid, slot, &confirm,
                                            sizeof(confirm)) != 0 ||
                        confirm != value)
                        break;

                    int prot = kernel_get_vmem_protection(t->pid, slot,
                                                          sizeof(uint64_t));
                    if (prot < 0)
                        break;      /* not really mapped */
                    /* A real import slot is writable data. A match in a
                     * read-only page is a constant, not a slot. */
                    /* Do not require the page to be writable: a GOT behind
                     * RELRO is read-only until we mprotect it, which is what
                     * the write path does anyway. */
                    ga_log("  %-38s at 0x%lx prot %d",
                           ga_fn_names[k], (long)slot, prot);

                    out[found].slot = slot;
                    out[found].original = (intptr_t)value;
                    out[found].protection = prot > 0 ? (uint32_t)prot : 0;
                    out[found].kind = k;
                    found++;
                    break;
                }
            }
        }
    }

    free(chunk);
    ga_log("scan: %u object(s) scanned of %u walked, %u pad slot(s) found",
           scanned, objects, found);
    return (int)found;
}

#else

int ga_hook_find_slots(const ga_target_t *t, ga_slot_t *out, unsigned max)
{
    (void)t; (void)out; (void)max;
    return -1;
}

int ga_hook_scan_slots(const ga_target_t *t, ga_slot_t *out, unsigned max)
{
    (void)t; (void)out; (void)max;
    return -1;
}

#endif
