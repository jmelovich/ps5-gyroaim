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
#include <time.h>

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
volatile int ga_hook_cancel = 0;

/* How much of a module is read at a time while looking for slots. */
#define SCAN_CHUNK         0x4000

/* How many unrecognised slot values may be followed while looking for a
 * gateway left behind by an earlier run. */
/* Each test costs a read of the game's memory, so there is a ceiling. The
 * filter below spends them only on addresses that could actually hold a
 * gateway, which is few enough that this is generous. */
#define ORPHAN_TEST_CAP    6000u

/* Where each loaded module sits, so an address inside one can be dismissed
 * without a read. */
#define RANGE_MAX          64u
typedef struct {
    uint64_t base;
    uint64_t size;
} ga_range_t;

static void note_orphan(pid_t pid, uint64_t value, ga_orphan_t *orphan,
                        unsigned *tests);

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

int ga_hook_find_slots(const ga_target_t *t, ga_slot_t *out, unsigned max,
                       ga_orphan_t *orphan)
{
    uint64_t head = 0;
    unsigned found = 0, objects = 0, relas = 0, tests = 0;

    if (!t || !out || !max)
        return -1;
    if (orphan)
        memset(orphan, 0, sizeof(*orphan));
    if (find_object_list(t, &head) != 0)
        return -1;

    uint64_t at = head;
    while (at && objects < OBJ_LIST_MAX && found < max && !ga_hook_cancel) {
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

        for (uint64_t i = 0; i < count && found < max && !ga_hook_cancel;
             i++) {
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

            int matched = 0;
            for (int k = 0; k < GA_FN_COUNT; k++) {
                if (!t->fn[k] || value != (uint64_t)t->fn[k])
                    continue;
                matched = 1;
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
            if (!matched)
                note_orphan(t->pid, value, orphan, &tests);
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

/* ---- an installation left behind -------------------------------------- *
 *
 * Our gateways have a fixed shape, 16 bytes of
 *
 *     48 8d 1x dd dd dd dd      lea rdx or rcx, [rip + disp]
 *     e9 rr rr rr rr            jmp stub
 *
 * so a slot value that is not a known export can be tested for it, and the
 * displacement leads straight to the shared page. The magic word there is what
 * confirms the find; checking only the magic and the version keeps this
 * working against a page written by a different build, whose fields may have
 * moved.
 */
static int gateway_args(pid_t pid, intptr_t value, intptr_t *out_args,
                        uint32_t *out_version)
{
    uint8_t code[16];
    int32_t disp;
    uint32_t header[2];

    if (kernel_proc_copyout(pid, value, code, sizeof(code)) != 0)
        return 0;
    if (code[0] != 0x48 || code[1] != 0x8d || code[7] != 0xe9)
        return 0;
    if (code[2] != 0x15 && code[2] != 0x0d)      /* rdx or rcx */
        return 0;

    memcpy(&disp, code + 3, sizeof(disp));
    intptr_t args = value + 7 + disp;

    if (kernel_proc_copyout(pid, args, header, sizeof(header)) != 0)
        return 0;
    if (header[0] != GA_MAGIC)
        return 0;

    *out_args = args;
    *out_version = header[1];
    return 1;
}

/* Could this value be a gateway we left behind?
 *
 * Deciding costs a read of the game's memory, and a module holds millions of
 * eight-byte values, so nearly all of them have to be dismissed without one.
 * Two facts do it. A gateway is laid out on a sixteen-byte stride, and it
 * lives on a page this service mapped, which is not part of any module the
 * game loaded. Pointers into a module are the module's own business.
 *
 * Without this the budget went on the first few kilobytes of whatever module
 * came first, and a gateway sitting past that was never examined. A hundred
 * and seventy megabyte executable made that a certainty, and the service
 * could then never take a game back after its own restart: the slots held
 * gateways it could not recognise, so the game had to be restarted by hand.
 */
static int could_be_gateway(uint64_t value, const ga_range_t *ranges,
                            unsigned count)
{
    if (!value || value >= (uint64_t)INTPTR_MAX)
        return 0;
    if (value & 15u)
        return 0;
    if (value < 0x10000u)              /* no mapping down there */
        return 0;
    for (unsigned i = 0; i < count; i++)
        if (value >= ranges[i].base && value < ranges[i].base + ranges[i].size)
            return 0;
    return 1;
}

/* Walks the linker's object list once and records where every module sits. */
static unsigned collect_ranges(const ga_target_t *t, uint64_t head,
                               ga_range_t *out, unsigned max)
{
    unsigned count = 0, objects = 0;
    uint64_t at = head;

    while (at && objects < OBJ_LIST_MAX && count < max) {
        ga_rtld_object_t o;

        if (read_any(t->pid, (intptr_t)at, &o, sizeof(o)) != 0)
            break;
        objects++;
        at = o.next;
        if (!object_sane(&o))
            continue;
        out[count].base = o.mapbase;
        out[count].size = o.mapsize;
        count++;
    }
    return count;
}

/* Tests one slot value for being a gateway we left behind, with a cap so a
 * pointer-dense module cannot turn this into a sweep. */
static void note_orphan(pid_t pid, uint64_t value, ga_orphan_t *orphan,
                        unsigned *tests)
{
    intptr_t args = 0;
    uint32_t version = 0;

    if (!orphan || orphan->args || *tests >= ORPHAN_TEST_CAP)
        return;
    if (!value || value >= (uint64_t)INTPTR_MAX)
        return;

    (*tests)++;
    if (!gateway_args(pid, (intptr_t)value, &args, &version))
        return;

    orphan->args = args;
    orphan->gateway = (intptr_t)value;
    orphan->version = version;
    orphan->slots = 1;
    ga_log("orphan: a slot still points at a gateway from an earlier run; "
           "shared page 0x%lx, layout version %u", (long)args, version);
}

/* ---- finding slots by value ------------------------------------------- */

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

int ga_hook_scan_slots(const ga_target_t *t, ga_slot_t *out, unsigned max,
                       ga_orphan_t *orphan, ga_scan_cursor_t *cursor)
{
    uint64_t head = 0;
    unsigned found = 0, objects = 0, scanned = 0;
    /* Carried across passes, so the orphan hunt cannot keep restarting its
     * budget and crowding out the sweep. */
    unsigned local_tests = 0;
    unsigned *tests_at = cursor ? &cursor->orphan_tests : &local_tests;
    uint64_t deadline = now_ms() + GA_SCAN_BUDGET_MS;
    uint64_t resume_at = cursor ? cursor->offset : 0;
    uint64_t resume_in = cursor ? cursor->mapbase : 0;
    int reached = resume_in == 0;      /* nothing to skip past */
    int ran_out = 0;
    uint64_t paused_size = 0;
    uint8_t *chunk;

    if (!t || !out || !max)
        return -1;
    if (orphan)
        memset(orphan, 0, sizeof(*orphan));
    if (cursor)
        cursor->done = 0;
    if (find_object_list(t, &head) != 0)
        return -1;
    if (!(chunk = malloc(SCAN_CHUNK)))
        return -1;

    ga_range_t ranges[RANGE_MAX];
    unsigned nranges = collect_ranges(t, head, ranges, RANGE_MAX);

    uint64_t at = head;
    while (at && objects < OBJ_LIST_MAX && found < max && !ga_hook_cancel) {
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

        /* Carry on from where the last pass stopped. */
        if (!reached) {
            if (o.mapbase != resume_in)
                continue;
            reached = 1;
        }

        uint64_t from = (o.mapbase == resume_in) ? resume_at : 0;
        if (from == 0) {
            scanned++;
            ga_log("scan: %s, 0x%llx bytes at 0x%llx",
                   path[0] ? path : "<the executable>",
                   (unsigned long long)o.mapsize,
                   (unsigned long long)o.mapbase);
        }

        for (uint64_t off = from; off < o.mapsize && found < max &&
                                  !ga_hook_cancel;
             off += SCAN_CHUNK) {
            /* Checked per chunk, so a large module is read across several
             * passes instead of holding the service for minutes. */
            if (now_ms() >= deadline) {
                if (cursor) {
                    cursor->mapbase = o.mapbase;
                    cursor->offset = off;
                }
                paused_size = o.mapsize;
                ran_out = 1;
                break;
            }
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

                /* A value pointing into any loaded module is that module's
                 * own data. A gateway lives on a page we mapped. */
                if (could_be_gateway(value, ranges, nranges)) {
                    int known = 0;
                    for (int k = 0; k < GA_FN_COUNT; k++)
                        if (t->fn[k] && value == (uint64_t)t->fn[k])
                            known = 1;
                    if (!known)
                        note_orphan(t->pid, value, orphan, tests_at);
                }

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

        if (ran_out)
            break;      /* leave the cursor pointing at this object */
    }

    free(chunk);

    if (ran_out) {
        ga_log("scan: paused at 0x%llx of 0x%llx in the module at 0x%llx, "
               "%u found so far; carrying on next time",
               (unsigned long long)(cursor ? cursor->offset : 0),
               (unsigned long long)paused_size,
               (unsigned long long)(cursor ? cursor->mapbase : 0), found);
    } else {
        if (cursor) {
            cursor->mapbase = 0;
            cursor->offset = 0;
            cursor->done = 1;
        }
        ga_log("scan: %u object(s) scanned of %u walked, %u pad slot(s) found, "
               "%u gateway candidate(s) tested", scanned, objects, found,
               *tests_at);
    }
    return (int)found;
}

#else

int ga_hook_find_slots(const ga_target_t *t, ga_slot_t *out, unsigned max,
                       ga_orphan_t *orphan)
{
    (void)t; (void)out; (void)max; (void)orphan;
    return -1;
}

int ga_hook_scan_slots(const ga_target_t *t, ga_slot_t *out, unsigned max,
                       ga_orphan_t *orphan, ga_scan_cursor_t *cursor)
{
    (void)t; (void)out; (void)max; (void)orphan; (void)cursor;
    return -1;
}

#endif
