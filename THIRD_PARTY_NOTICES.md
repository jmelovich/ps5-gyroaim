# Third-party notices

GyroAim is GPL-3.0-or-later. What it builds on, and under what terms.

## Code included in the built title

**ps5-native-app-boilerplate** by BlackBearReloaded, GPL-3.0-or-later.
<https://github.com/blackbearreloaded/ps5-native-app-boilerplate>

The title build pipeline: the cross-toolchain wrapper, the linker script, the
host tool that converts an ELF into a fake-signed executable and assembles the
package, and the clean-room `libc.prx` runtime module that ships inside the
title. The control panel's CPU renderer and bitmap font, `demo_renderer.cpp`,
is also theirs, copied into `app/src/vendor/` by `tools/setup-app-deps.sh`.

Pinned at commit `7928889d33261833bf87d23269444d830d68ff48`.

**QR-Code-generator** by Project Nayuki, MIT.
<https://github.com/nayuki/QR-Code-generator>

The QR encoder behind the settings code, `qrcodegen.c`, pinned at `v1.8.0` and
fetched by content hash into `app/src/vendor/`.

## Code the service is built against

**ps5-payload-dev SDK** by John Törnblom, GPL-3.0-or-later.
<https://github.com/ps5-payload-dev/sdk>

The cross toolchain, the C runtime, and the kernel helpers the service uses to
read another process's memory and resolve its imports. Pinned at `v0.42`.

## Techniques, not code

No code was taken from these, but the approach would not exist without them
and the debt is worth stating.

**PoorDS4** by ItsBlurf and **Ghostcontrol** by StonedModder, both
GPL-3.0-or-later, documented kstuff's remote-syscall trap and the practice of
redirecting a game's own pad import slots rather than patching shared code.
This project's implementation is independent and adds a value-scan path for
modules built without a PLT, but the mechanism is theirs.

**GyroWiki** and **JoyShockMapper** by Jibb Smart and Electronicks are where
the aiming design comes from: player space, tiered smoothing, tightening in
place of a cutoff, and the idea that a stick is a velocity request that the
game's own curve must be inverted against.

## Not included

Nothing from Sony ships in this repository or in a built package. The SDK's
library stubs are link-time only, and no firmware, keys or system modules are
redistributed.
