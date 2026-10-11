# GyroAim for PS5

Gyro aiming in any game on a jailbroken PS5.

A resident service folds the DualSense's gyroscope into the control a game
actually reads, usually the right stick, while a condition you choose is met,
usually holding the left trigger. Tune it from a phone while the game runs.

Experimental homebrew for the author's own console. It writes into a running
game's memory: **save your progress before testing.**

## What works

Verified on a PS5 CFI-2015, firmware 13.20, with kstuff-lite 1.11 and
ShadowMount+ 1.7beta4.

- Runs as a resident payload outside any sandbox and survives game launches.
- Finds a running title, resolves its pad library, locates the import slots
  that reach it, and installs the transform. Both a retail title and a
  homebrew one are confirmed.
- Reads the real gyro and writes the stick, with a self test that proves the
  arithmetic without anyone holding the controller.
- Gyro to right or left stick, gated on a trigger, a button, or always.
- Sensitivity, inversion, axis choice, trigger threshold and stick deadzone,
  all live from the portal, saved to `/data/gyroaim/settings.txt`.
- Installs and removes cleanly; the game keeps running either way.

Not built yet: the response curve, smoothing and drift calibration; rumble
feedback; per-title profiles; the touchpad as a target; the native app.

## Build and run

```sh
tools/setup-deps.sh                     # fetches the pinned payload SDK
make -C payload deploy PS5_HOST=<ip>    # builds and sends it to the loader
```

The console needs an ELF loader on port 9021 and kstuff loaded.

To start it at boot, put `gyroaim.elf` in `/data/pldmgr/payloads/gyroaim/`
and add that filename to `/data/pldmgr/autoload.txt`.

## Using it

Open `http://<console ip>:6080` from a phone or PC on the same network. The
page shows live gyro rates and the stick values going in and out, so you can
see the transform working before you can feel it.

**There is no authentication.** Anyone on your network can open the portal and
change the settings.

For the aiming to feel right, the game's own settings matter as much as these:
turn the in-game stick deadzone as low as it goes, pick a linear response
curve, turn off stick acceleration, and set sensitivity high enough that the
camera is not the limit.

## Honest limits

The camera can never turn faster than the game's own maximum stick speed, and
a stick is 8 bits per axis, so fine aim has a floor. Emulating a gyro through
a stick is good, not mouse-equivalent.

For games played online, this changes the input the game receives. That is
plausibly against their rules, and a crash mid-match is a real possibility.

## Credit

The remote-syscall mechanism and the import-slot redirection technique come
from [PoorDS4](https://github.com/ItsBlurf/PoorDS4) by ItsBlurf and
[Ghostcontrol](https://github.com/StonedModder) by StonedModder, both
GPL-3.0-or-later. The gyro design follows Jibb Smart's work at
[GyroWiki](http://gyrowiki.jibbsmart.com) and JoyShockMapper.

Built on the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk).

GPL-3.0-or-later. See [PLAN.md](PLAN.md) for the architecture and
[docs/CONSOLE_NOTES.md](docs/CONSOLE_NOTES.md) for hardware findings.
