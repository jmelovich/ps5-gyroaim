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

## Rumble needs legacy motor emulation

`scePadSetVibration` does nothing at all on a DualSense until the pad is put
into legacy motor emulation with `scePadSetVibrationMode(handle, 2)`. The pad
has voice-coil actuators rather than rotary motors, and without that call the
motor levels are simply ignored, with no error.

The mode is re-applied every couple of seconds while rumble is wanted, because
the game sets the mode too and whichever call came last wins. Switching a pad
into this mode can change how the game's own haptics feel, which is why
turn-rate rumble is off by default.

Letting go of the trigger has to send a zero level explicitly, or the motors
stay at whatever the last aiming frame asked for.

## Make needs header dependencies

A missing one cost an hour. Adding an entry to an enum in `hook.h` rebuilt the
objects whose `.c` files had changed and left `inject.o` alone, so one half of
the binary thought a shared struct was eight bytes shorter than the other
half. The symptoms were absurd: a field in the telemetry read back as an
import slot address, and an array index ran off the end of a table.

`CFLAGS` now carries `-MMD -MP` and the makefile includes the generated `.d`
files. Touching a header rebuilds everything that includes it.

Any build that shares a struct between code compiled into two different
contexts needs this. There is no runtime check that would have caught it,
because both halves were individually self-consistent.

## What a title can and cannot do

Measured while building the control panel.

**Works from inside the sandbox**, no elevation of any kind:

- Plain BSD sockets. Connecting to `127.0.0.1:9021` and writing a payload is
  how the panel starts the service; the homebrew catalog documents this as a
  standard route out of the sandbox and much of the catalog uses it.
- Reading `/app0`, and reading and writing `/data`, including creating
  directories under `/data/pldmgr/payloads` and appending to `autoload.txt`.
  All of those are world-writable. `/data` is only visible because
  ShadowMount+ mounts it; check for it rather than assuming.

**Faults on an unresolved address:** anything living in
`libScePosixForWebKit`, which a native title never loads. `sceNetCtlGetInfo`
is one, and `getaddrinfo` and friends are others per the boilerplate's own
notes. The symptom is a page fault on a read instruction, with no useful
message.

To get the console's own address without it: point a UDP socket at a
documented test address and ask `getsockname` which local address the kernel
would use. Nothing is sent, because connecting a datagram socket only picks a
route. Format the result by hand rather than with `inet_ntop`, which is one
more symbol that may not be there.

`assert()` lowers to `__assert`, which the console does not provide, so a
vendored library that asserts will not link. A small stand-in that logs and
stops beats compiling assertions out.

**The service must skip the panel's own title.** Every title is `eboot.bin`,
so the control panel looks exactly like a game and the transform was duly
installed into it. Holding the activation button would then steer its menu
with the gyro.

Nothing on the console reads a title's stderr. The panel logs to the kernel
log with `sceKernelDebugOutText`, which is the only way to see what a title is
doing from another machine.

## Uninstalling is split between the two pieces

Each half does the part it is positioned for.

The **panel** removes the copy under `/data/pldmgr/payloads/gyroaim` and its
line in `autoload.txt`. Those are plain files, and the panel may be the only
thing running.

The **service** clears its own directory, because a title cannot read one:
`opendir` is exported to a title and then refused, and the files there cannot
be listed ahead of time anyway, since profiles are named after whichever games
were played and the mapping notes after process ids. A payload runs outside the
sandbox with the ordinary system libc, so it just works there.

If the service is not running and settings are to go, the panel starts the
bundled copy for the sole purpose of having it clear up and exit.

Settings are kept unless asked for explicitly. Someone removing the service to
reinstall it should not lose an evening of tuning.

## The service cannot read the pad itself

Worth stating because it rules out a simpler design. A payload cannot open the
controller: `scePadOpen` returns `0x809b0081` and `scePadGetHandle` returns
`0x80920008`, whether or not a title is running, because the payload is not a
registered pad client. Ghostpad works around it by injecting into SceShellCore
and reading at a few hertz, which is far too slow for aiming.

That is why the transform lives inside the game, where the pad data is already
being delivered and the gyro arrives in the same struct as the sticks. The
in-game stub is not a workaround for something neater; it is the only place
the data exists at full rate.

`pad.h` keeps the sample layout and its static assertions, which are the
authoritative record of the offsets and fail the build if anyone disturbs them.

## Only one service may run at a time, and it matters more than it looks

The SDK's kernel read and write primitives are not safe to use concurrently.
PuckbridgePS5 serialises them with a lock for that reason. Two *processes*
cannot share a lock, so two copies of the service both walking a game's memory
can corrupt the primitive between them.

That is not theoretical. Two instances were once running at once, and one of
them ended up spinning in state RUN, ignoring both SIGTERM and SIGKILL, still
answering on its port. An unkillable process holding the lock port is as bad as
it sounds: nothing can replace it until the console is restarted.

Four things let that happen, all now fixed:

- The portal served one connection at a time with a blocking read and no
  timeout, so a client that connected and sent nothing stopped it dead.
- The value sweep ran unbounded over a module. On a multi-gigabyte game it
  takes minutes, during which the service checks no signal and cannot be
  replaced, and a replacement gives up and leaves two running.
- A departing instance removed the pidfile even when a newer instance owned
  it, after which nothing could tell who was in charge.
- Nothing checked whether this instance was still the one in charge.

The rules that follow: the sweep is capped by module size and gives up when
asked, the pidfile is only removed by its owner, and every instance checks each
tick that the pidfile still names it and stands down if not.

## An orphaned gateway makes a game unattachable

If the service does not get to restore the slots, the game keeps pointing at
gateways with nobody feeding them. A slot then holds our gateway rather than
Sony's export, so the scan no longer recognises it as a pad slot and that game
can never be attached to again, while the stub carries on running. Aiming can
keep working while the service reports that it failed, which is exactly how
this was first noticed.

The finders now test an unrecognised slot value for the gateway shape,
`48 8d 1x <disp32> e9 <rel32>`, and follow the displacement to look for the
magic word. Finding one means an installation is already there and can be
taken over rather than fought. The test is capped so a pointer-dense module
cannot turn it into a sweep of its own.

## Routing is a matrix, and the pipeline runs per stick

Three rotations, yaw, pitch and roll, each route to four destinations: the two
axes of each stick. A cell is +1, -1 or 0, so a rotation can drive several
controls and a control can be fed by several rotations.

The pipeline therefore runs once per stick rather than once overall. Each stick
gets its own wanted velocity vector, its own merge with whatever the physical
stick is asking for, and its own flick carry. Smoothing and tightening stay on
the three rotations, which is where they belong: how steady a hand is does not
depend on where its movement is routed.

A stick with nothing routed to it is left exactly as the game delivered it. A
stick with anything routed to it is owned as a pair, because the game's
deadzone and curve act on how far it is pushed rather than on one axis, so an
unrouted axis of a routed stick makes a round trip through the curve. The
round-trip error is under half a percent, so it passes through intact.

Signs live in the matrix as floats the stub multiplies by, since it still
cannot negate one.
