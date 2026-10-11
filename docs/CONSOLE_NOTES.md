# Working notes for this console

Measured on a PS5 CFI-2015 A01X, firmware 13.200.006. Re-check after any
firmware change.

## Payloads all share one process name

The ELF loader on port 9021 names every payload it loads `payload.elf`,
regardless of the filename. etaHEN, the store payload and this service are
indistinguishable by name in the process table.

**Consequence:** never find "other copies of ourselves" by process name. The
service uses its listening port as the single-instance lock and only signals a
pid it wrote to `/data/gyroaim/pid` itself. An early version matched on the name
and killed two unrelated payloads.

A reboot is not a way out of a mistake like that: rebooting loses the jailbreak,
which then has to be re-run from the browser exploit. pldmgr re-autoloads
everything only after the console is jailbroken again.

## Finding a running title

Titles appear as `eboot.bin`. The title id comes from `sceKernelGetAppInfo`,
not from the process table.

Sony's `app_info` struct is **packed**. The SDK sample declares it with natural
alignment, which places `title_id` four bytes late and reads `99350` instead of
`PPSA99350`. `proc.c` sidesteps the question by finding the id by its shape,
four capitals followed by five digits.

## Process table

Use the real `struct kinfo_proc` from `<sys/user.h>` with `sysctl KERN_PROC`.
Hand-counted field offsets happen to work but are fragile; `ki_comm` and
`ki_pid` are right there.

## Resident payloads at boot

From `/data/pldmgr/autoload.txt`, in order: etaHEN 2.5B, kstuff-lite 1.11,
ShadowMount+ 1.7beta4, ftpsrv, ps5upload, orbit_store, shsrv, klogsrv.

Ports: 2121 FTP, 2323 shell, 3232 kernel log, 9021 ELF loader, 10101
ShadowMount+ API. The service takes 6080.
