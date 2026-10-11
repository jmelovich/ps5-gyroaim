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

## Finding a game's pad import slots

Two methods, because they cover different targets.

**Relocation walk.** The runtime-linker object list sits at `proc+0x3e8` on
firmware 13.20, which `hook.c` establishes by looking for the list that
contains a library it resolved independently, rather than hardcoding the
offset. Each object's PLT relocation table gives slot addresses.

**Value scan.** Homebrew built with this SDK is compiled `-fno-plt` and has
**no PLT relocations at all** (`plt_rela_size` is 0), reaching its imports
straight through the GOT. For those, scanning the module's own mapped range
for the already-resolved export addresses finds the slots directly. Retail
titles populate the relocation table, so both paths are needed.

Two traps found while building this:

- `kernel_get_proc` and the linker structures return **kernel** pointers,
  which are negative as a signed `intptr_t`. Test for zero, never `<= 0`.
- `kernel_proc_copyout` can report success while leaving part of the buffer
  untouched. Zero the buffer before each read and confirm every hit with its
  own small read, or stale data shows up as phantom hits one page apart.

Import slots live behind RELRO and report protection `PROT_READ` only, so
writing one means raising the protection, writing, and restoring it.

Measured against PPSA99350: three slots, contiguous, for the three pad
functions that title actually calls.

## The pad sample layout, settled on hardware

Reading real data out of a running game resolved the one ambiguity in the
public sources. At rest the controller reports:

    offset 0x1c   -0.043  0.968  0.226     magnitude 0.995
    offset 0x28   -0.001  0.001  0.000

So `0x1c` is acceleration in g and `0x28` is angular velocity in rad/s,
matching the official SDK order. The source that swaps them is wrong.

## Writing code into a game

The stub is copied byte for byte into the target, so it must reference
nothing outside itself. The compiler will quietly break this: writing `-x` on
a float makes it load a sign mask from a constant pool in `.rodata`, reached
by a RIP-relative load that points into open space once copied. Float and
string literals do the same.

`make check-stub` fails the build if the stub's section has any relocation at
all, which is the only reliable way to catch it. Axis inversion is folded
into the sign of the sensitivity so the stub never negates a float.

Other measured details:

- Remote `mmap` goes through kstuff, since kernel read and write cannot
  create a mapping. Target page size is 0x4000.
- Import slots are `PROT_READ`, so writing one means raising protection,
  writing, and restoring it.
- The mapping is deliberately never unmapped on removal: a game thread can be
  inside the stub, and pulling the code out from under it would crash the
  game. It dies with the process anyway.
- `kernel_proc_copyin` is used in small chunks and read back to verify, since
  a partial transfer would leave half a function in the game.

## Keeping the compiler out of .rodata

Removing float literals from the stub is not enough. Clang will fold two
adjacent operations into one SSE instruction and park the operand in a vector
literal, so `(int)x - 128` done twice in a row becomes a packed subtract with
`-128` in `.rodata`. Chasing those one at a time does not converge.

The fix is structural: the stub is compiled `-fno-vectorize
-fno-slp-vectorize`, which keeps small integers as immediates where they
belong. `-fno-math-errno` also matters, so `sqrtf` becomes one instruction
instead of a call. `make check-stub` fails the build on any relocation in the
stub's section, which is the only reliable guard.

Constants that genuinely are needed live in `ga_consts_t` on the shared page,
filled in once before injection.

## The portal's socket needs SO_REUSEADDR

The listening socket doubles as the single-instance lock, so two processes
must never listen at once. That argues for omitting `SO_REUSEADDR`, and an
early version did.

It was wrong. Closed portal requests leave connections in `TIME_WAIT` on that
port, and without `SO_REUSEADDR` a replacement instance cannot bind for about
a minute afterwards, which is exactly when a replacement is most likely. The
old instance would stand down and the new one would then refuse to start,
leaving no service running at all.

`SO_REUSEADDR` waives only the `TIME_WAIT` wait. Two simultaneous listeners
would need `SO_REUSEPORT`, which must never be set here.

## Two coordinate systems, one subtraction

A stick's vertical axis grows downward: above centre means pushed down.
Camera pitch grows upward: positive means looking up. The transform works in
camera space, so the physical stick's vertical contribution has to be
**subtracted** when it is folded in, not added.

Adding it inverted the right stick's up and down, and only while the gyro was
engaged, because the sample passes through untouched otherwise. Nothing in the
telemetry looked wrong; it took someone holding the controller.

`tests/transform_test.c` now runs the stub on the host with a stand-in for
Sony's read function and checks that a stick push survives the round trip
through velocity space. Reinstating the bug makes it fail with "pushed down
stays down: got 56, wanted 200", which is exactly what the report described.
