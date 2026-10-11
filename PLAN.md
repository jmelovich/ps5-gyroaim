# GyroAim for PS5 — implementation plan

Gyro aiming in any game on a jailbroken PS5. A background service transforms the
DualSense's gyroscope into whatever control the game actually reads, usually the
right stick, while a condition you choose is met, usually the left trigger being
held. Configurable from a phone while the game is running.

Status: **plan, awaiting approval.** Nothing is implemented yet.

---

## 1. What we are building

Three pieces:

| Piece | Form | Job |
| --- | --- | --- |
| **The service** | PS5 payload ELF, resident, runs as root outside any sandbox | Reads nothing itself; installs a small transform into the running game so the game's own pad reads come back with the gyro folded in. Hosts the web portal, owns the settings. |
| **The portal** | HTTP server inside the payload, phone or PC browser | Live tuning while a game runs. The primary configuration surface. |
| **The app** | Native PS5 title, separate, thin | Start and stop the service, pick a profile, show status, from the console itself without a second device. |

### Required behaviour

- Gyro maps to a **target**: right stick (default), left stick, or synthetic
  touchpad contact.
- **Activation condition** gates it: any button, or L2/R2 past an analog
  threshold, in modes hold / inverted-hold / toggle / always. The button keeps
  its normal function in the game.
- **Tuning**: sensitivity per axis, response curve, acceleration, smoothing,
  deadzone compensation, axis inversion, gyro space.
- **Rumble feedback** proportional to turn rate, so you can feel that the gyro
  is live. Optional.
- **Per-title profiles**, auto-selected by the running title ID.

---

## 2. Verified baseline

Measured on the console at `192.168.0.192`, not assumed:

- **CFI-2015 A01X, firmware 13.200.006.**
- Resident payloads, autoloaded by pldmgr from `/data/pldmgr/payloads/` in this
  order: etaHEN 2.5B, kstuff-lite 1.11, ShadowMount+ 1.7beta4, ftpsrv, ps5upload,
  orbit_store, shsrv, klogsrv.
- Ports open: 2121 FTP, 2323 shell, 3232 kernel log, **9021 ELF loader**,
  10101 ShadowMount+ API. Port 9020 closed.
- `ps` confirms payloads run as **uid 0, outside any title sandbox**, as ordinary
  long-lived processes alongside a running game. That is the model the service needs.
- SDK already on this machine at
  `steamlink-ps5/.deps/native/ps5-payload-sdk` (ps5-payload-dev v0.42), with
  `toolchain/prospero.mk` for payload builds. It exposes everything required:
  `kernel_copyout` / `kernel_copyin`, `kernel_proc_copyin` / `copyout`,
  `kernel_mprotect(pid, …)`, `kernel_dynlib_handle` / `dlsym` / `resolve` /
  `mapbase_addr`, `kernel_get_proc`, `kernel_get_fw_version`.

Deploying a payload is one command: `nc -w 5 <host> 9021 < gyroaim.elf`.
Persistence is a file in `/data/pldmgr/payloads/` plus a line in
`/data/pldmgr/autoload.txt`.

---

## 3. Why the service must be a payload, and the app only a front end

A PS5 **title** cannot do this job. Launching a game tears the title down, and a
title lives in a sandbox. A **payload** survives game launches, runs as root, and
can reach another process's memory. So the service is a payload and the native app
is nothing more than a remote control that talks to it over localhost.

This splits cleanly and means the app is optional and can come last.

---

## 4. How the game's input actually gets changed

Every PS5 game reads its controller through `libScePad`, calling
`scePadReadState` or `scePadRead` each frame. Those calls arrive as **PLT import
slots** in the game's own module. Redirect a slot and you own the call.

The approach, proven on hardware by PoorDS4 and PuckbridgePS5 on firmwares 6.02
through 13.60:

1. **Find the game.** `sysctl` over `kinfo_proc`, match the process named
   `eboot.bin`.
2. **Find its pad imports.** Walk the game's dynamic-linker object list through
   kernel reads (`kernel_get_proc`, then the shared-object list at `proc+0x3e8`),
   read each object's PLT relocations, and match symbol NIDs against
   `scePadReadState`, `scePadReadStateExt`, `scePadRead`, `scePadReadExt`,
   `scePadSetVibration`. Only the eboot and `/app0/` modules are touched, never
   system or fakelib modules.
3. **Get memory inside the game.** A fresh anonymous mapping is the one thing
   kernel read/write cannot create, so use kstuff's remote-syscall bridge to run
   `mmap` and `mprotect` in the game's context. kstuff-lite 1.11 is already
   resident on the console.
4. **Write the transform there.** A self-contained, position-independent stub
   block plus one 16-byte gateway per hooked symbol
   (`lea rdx,[rip+args]; jmp stub`), then flip the import slot to the gateway with
   a protect / write / restore sequence.
5. **The stub calls Sony's original first**, then rewrites the returned
   `ScePadData` in place before the game ever sees it.

The game is never stopped, never ptraced, and no thread is borrowed.

### Safety properties, all of them load-bearing

- **Fail closed.** Any ABI check that does not match aborts the install. A
  partially applied hook set rolls back slot by slot.
- **Staleness.** A counter in the shared page is bumped on every read and reset
  when the payload publishes settings. Past the threshold the stub stops
  modifying and the game sees pure Sony data. So if the payload dies, aiming
  reverts within a fraction of a second instead of freezing mid-turn.
- **Magic and layout version** in the shared page, so a stub left behind by an
  older build is recognised and stripped rather than reused.
- **Clean unhook** on game exit, settings change, and before rest mode.

### Why not the alternatives

- **Virtual DualSense** (`scePadVirtualDeviceAddDevice` / `InsertData`, the
  Ghostpad path): safer, touches no game memory, but the real controller still
  feeds the game, so you get doubled input, and suppressing the real pad needs the
  in-game hook anyway. It also cannot read the gyro of a pad the game owns without
  injecting a reader into a system process, which adds a second hop of latency.
  Rejected as the primary path; it stays a fallback idea for a no-hooks diagnostic mode.
- **Kernel-level pad driver hook:** would apply to every process at once and need
  no per-game work, but means custom kernel patches tied to one firmware. Far more
  fragile than redirecting a user-space import slot. Rejected.

---

## 5. Where the maths runs, and why that matters

The gyro and the sticks arrive in the **same 120-byte struct**, at the moment the
game asks for it. So the transform belongs **inside the stub**, in the game's own
thread. Nothing is queued, nothing is published frame to frame, and the aim adds
**zero frames of latency**. That is strictly better than computing a stick value
in the payload and shipping it over, which is what the existing projects do
because they are bridging a separate physical controller.

The stub must therefore stay small and free of libc and libm. Division of labour:

**Stub**, every pad read: subtract bias, convert to player space, tighten,
smooth, apply sensitivity, convert rate to deflection, compensate the game's
deadzone, merge with the physical stick, write the target control, append a
sample to a telemetry ring.

**Payload**, about 100 Hz: read the telemetry ring, run stillness detection and
update the bias, serve the portal, load and save profiles, **precompute every
curve as a lookup table** so the stub needs no `powf`, drive rumble, watch for
title changes, install and remove hooks.

`sqrtf` is fine in the stub, it compiles to a single SSE instruction. Everything
transcendental becomes a 64-entry table plus linear interpolation.

Per-pad-handle state (the smoothing ring, the overflow carry, last timestamp)
lives in the shared page. The struct carries a microsecond `timestamp`, so `dt`
comes from the data itself, and state only advances when the timestamp changes.
That makes repeated reads in one frame harmless, and batched `scePadRead` calls
are walked in timestamp order so integration stays correct.

---

## 6. ScePadData layout

120 bytes. Offsets below are cross-checked against three independent
implementations, and `connected` at `0x4C` is confirmed by rpcs3-PS5, which pins
everything above it.

| Offset | Field |
| --- | --- |
| `0x00` | `buttons` u32 |
| `0x04` | left stick x, y u8 |
| `0x06` | **right stick x, y** u8 — the default output |
| `0x08` | L2, R2 u8 |
| `0x0A` | padding u16 |
| `0x0C` | `orientation` quaternion, 4 floats |
| `0x1C` | `acceleration` vec3 floats, in g |
| `0x28` | **`angularVelocity`** vec3 floats, in rad/s — the input |
| `0x34` | touch: finger count, then 2 contacts of x u16, y u16, id u8 at `0x3C` and `0x44` |
| `0x4C` | `connected` |
| `0x50` | `timestamp` µs |
| `0x58` | extension[16], then count, reserved |

Button bits: `L3 0x2 · R3 0x4 · OPTIONS 0x8 · UP 0x10 · RIGHT 0x20 · DOWN 0x40 ·
LEFT 0x80 · L2 0x100 · R2 0x200 · L1 0x400 · R1 0x800 · TRIANGLE 0x1000 ·
CIRCLE 0x2000 · CROSS 0x4000 · SQUARE 0x8000 · TOUCH 0x100000 ·
INTERCEPTED 0x80000000`.

**One genuine ambiguity.** Two of the three sources order the block
orientation → acceleration → angularVelocity, matching the official PS4 SDK; one
has acceleration and angular velocity swapped. The Sony order is almost certainly
right, and it is trivial to settle on hardware: at rest, acceleration has
magnitude about 1.0 and angular velocity about 0. Phase 1 settles it, and
`padlayout.h` carries static assertions so it can never drift silently.

All three mapping targets the user asked for live in this one struct, which is why
"map the gyro to anything" is cheap rather than three separate mechanisms.

---

## 7. The gyro pipeline

Drawn from Jibb Smart's gyro work, JoyShockMapper, and Steam Input's
gyro-to-joystick camera mode. The hard part is that a stick is a *velocity*
request passed through the game's own deadzone and curve, so the pipeline has to
invert the game's processing to stay proportional.

```
raw angularVelocity (rad/s)  ->  deg/s
  minus bias                              drift, from stillness detection
  player space                            yaw from gravity-weighted Y and Z,
                                          pitch local; relax factor 1.41
  tighten                                 rates under ~5 deg/s shrink to zero
  tiered smoothing                        only slow motion is averaged, so
                                          flicks stay instant
  sensitivity (+ optional acceleration)   camera degrees per controller degree
  -> desired camera deg/s
  merge with the physical stick           in velocity space, not stick space,
                                          so the game's curve isn't applied twice
  overflow carry                          a fast flick asking for more than full
                                          deflection is delivered just after,
                                          capped, so the turn completes
  divide by max turn rate                 the game's own full-stick speed
  unpower, then anti-deadzone             invert the game's curve and deadzone
  -> stick fraction, quantised to u8 last
```

Two things the user must set per game, and the portal should walk them through:

- **Max turn rate**, measured by holding the stick fully right and timing one
  full revolution in game. This is what makes "sensitivity 1.0 means a 1:1 real
  world turn" actually true.
- **In-game settings**: stick deadzone as low as allowed, linear response curve,
  no stick acceleration, sensitivity high enough that the ceiling is comfortable.

**Honest limitation to document, not hide.** The camera can never turn faster
than the game's own maximum stick speed, and the stick is 8 bits per axis, so
there is a floor on fine aim. Deflection emulation is good, not mouse-equivalent.

**Calibration.** Bias is learned from stillness: watch the min/max spread of gyro
and accelerometer over a window, treat a quiet window as still, average it, and
ease the bias toward that. Critically, **never learn drift while the gyro is
actively aiming**, because a slow steady turn around gravity looks exactly like
sitting still to the accelerometer. A manual "hold still and press this" calibrate
is also offered, because every automatic detector is wrong occasionally.

**Two calls the stub must make once per handle**, or the gyro fields may simply be
zero: `scePadSetMotionSensorState(handle, true)` to turn the motion sensor on for
games that never ask for it, and `scePadSetAngularVelocityDeadbandState(handle,
false)` to remove Sony's small-rate deadband, which is exactly the hard threshold
good gyro controls must not have. Both resolve from `libScePad` like the rest.

---

## 8. Rumble feedback

The stub already hooks `scePadSetVibration` and holds the address of Sony's
original plus the game's live pad handle. So turn-rate rumble is injected by
calling the original from the game's own thread with our own level, rate-limited
to changes only, and mixed so it never fights the game's own vibration.

This specific trick is **not something the existing projects do** — they only
observe the game's rumble and forward it to a different controller. Treat it as
unverified until Phase 3 proves it on hardware. The fallback, if calling out from
the stub misbehaves, is for the payload to drive rumble through its own pad handle.

---

## 9. Configuration

**Model.** A profile holds the target, the activation condition, and all tuning.
One global default plus per-title overrides keyed by title ID, auto-selected when
the running title changes. Titles are named by reading `/user/appmeta/<ID>/param.json`.

**Storage.** Plain text under `/data/gyroaim/`, readable and editable over FTP,
so a bad setting is always recoverable without the console's UI.

**Surfaces.**

1. **Portal**, the primary one. Plain socket HTTP server in the payload, no
   library, serving one embedded page plus a small JSON API. Live readouts of
   measured degrees per second and emitted stick values make tuning fast. Default
   port **8080**, chosen to avoid Puckbridge's 8090 and ShadowMount+'s 10101. No
   authentication, and the docs will say so plainly: anyone on the LAN can open it.
2. **Native app**, Phase 4. A thin title that talks to the same JSON API over
   localhost. The build pipeline is already proven next door in steamlink-ps5:
   `prospero-clang`, `ps5-native-tool` to link and fake-sign, `sce_sys/param.json`
   with `requiredSystemSoftwareVersion` zeroed, an icon, CPU rendering presented
   through `libSceVideoOut`. Title ID **PPSA99510**, free in the homebrew catalog.
3. **On-pad**, stretch. A button chord toggles the gyro or cycles profiles, with
   `sceKernelSendNotificationRequest` toasts as feedback, so no second device is
   needed mid-game.

---

## 10. Repository layout

```
payload/            the service
  main.c            lifecycle, title watch, install and remove
  bridge/           import-hook machinery
  stub.c            the injected block, compiled here and copied into the game
  gyro.c            pipeline, lookup tables, stillness detection
  profile.c         settings model, load and save
  web.c  web/       portal and JSON API
  rumble.c
  padlayout.h       offsets plus static assertions
app/                native title, Phase 4
tools/              deploy, shell, klog capture
docs/               HOOKING · PADDATA · GYRO · TUNING
```

---

## 11. Phases, each with a hardware gate

Riskiest unknowns are deliberately first. Phase 1 can invalidate the whole
approach, and it comes before any feature work.

**Phase 0 — scaffolding.** Pin the SDK, Makefile from `prospero.mk`, deploy and
relaunch scripts, a payload that logs to the kernel log and does nothing else.
*Gate:* it stays alive across launching and quitting a game.

**Phase 1 — probe, read-only.** Find the game, scan its pad imports, install a
stub that **modifies nothing** and only logs. *Gate:* a kernel-log dump of real
gyro values captured from inside a running game, which simultaneously answers:
does the remote-syscall path work on 13.20 with kstuff-lite 1.11; do hooks install
and roll back cleanly; **is `angularVelocity` actually non-zero in a game's
buffer**; is the accel/gyro offset order the SDK one; how often does the game read
the pad.

**Phase 2 — minimum viable aim.** Right stick only, fixed sensitivity, hold L2,
no configuration. *Gate:* gyro aiming demonstrably works in one game.

**Phase 3 — the real thing.** Full pipeline with tables and stillness
calibration, all three targets, every activation mode, profiles, portal, rumble.
*Gate:* tuning from a phone mid-game, and aim that feels right.

**Phase 4 — native app.** *Gate:* starts, stops and configures the service from
the console.

**Phase 5 — polish.** pldmgr autoload, rest-mode quiesce and re-inject, on-pad
menu, documentation.

---

## 12. Risks and open unknowns

| | |
| --- | --- |
| **Writing into a live game can crash it** | Unavoidable for this approach. Mitigated by ABI validation, fail-closed installs, rollback, staleness, and never touching system modules. Save your game before testing. This is experimental homebrew. |
| **Gyro may be zero in games that never enable motion** | Most likely outcome is that forcing `scePadSetMotionSensorState` fixes it. If a game somehow blocks that, the fallback is reading gyro via a reader injected into a system process, at a latency cost. **Phase 1 answers this.** |
| **Firmware coupling** | The kernel offsets come from the SDK and the remote-syscall path from kstuff. Both cover 13.x, console is 13.20. A console update can break the service until the SDK catches up. Record the firmware in every bug report. |
| **PS4 titles** | Unverified. They run under a compatibility layer and may present `libScePad` differently. Treat PS5 titles as the target and test PS4 ones opportunistically. |
| **Stub-injected rumble** | Novel, unverified. Section 8 has the fallback. |
| **Online games** | This changes the input a game receives. For competitive online titles that is plausibly against their rules, and a crash mid-match is a real possibility. Worth stating in the README; it stays the user's call. |

---

## 13. Licensing and provenance

The import-hook machinery is the intricate, firmware-validated part, and a
working implementation exists: **PoorDS4** by ItsBlurf, GPL-3.0-or-later,
building on **Ghostcontrol** by StonedModder, and already adapted once in
**PuckbridgePS5**.

**Recommendation: vendor that bridge layer, clearly attributed, and license the
whole project GPL-3.0-or-later.** Writing the same mechanism from scratch would
mean rediscovering firmware-specific structure offsets on hardware for no
benefit. Everything above the bridge — the gyro pipeline, settings, portal, app —
is ours and new.

Taking an unencumbered licence instead means reimplementing the bridge
independently, which is a realistic but materially slower route. Flagging it as a
decision rather than quietly picking.

Builds stay on this console. Nothing redistributable gets published.

---

## 14. Decisions I would like confirmed

1. **Vendor PoorDS4's bridge under GPL-3.0** as recommended, or write the hook
   machinery from scratch for a freer licence?
2. **Portal on port 8080**, no authentication, LAN-visible. Fine?
3. **Phase order.** Phase 1 is pure risk reduction and produces nothing you can
   use. Worth it, in my view, because it is the only honest way to find out
   whether the gyro is even reachable before building a pipeline on top of it.
4. **Native app last.** The portal does everything it does, sooner.
