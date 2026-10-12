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
- **Any rotation to any control.** Turning, tilting and leaning each route to
  the right stick's two axes, the left stick's two axes, or several at once,
  in either direction. So the view can be on one stick and movement on the
  other, or one rotation can drive both. Gated on a trigger, a button, or
  always.
- **Works in camera-velocity space**, so the game's own deadzone and response
  curve are inverted rather than fought. This is the part that makes aiming
  feel proportional instead of mushy near the centre and twitchy further out.
- Strength per rotation, in degrees of view per degree of hand movement, with
  an acceleration multiplier that comes in over a speed band.
- Drift calibration that learns the gyro's resting offset while the pad is
  still, and knows not to trust a window in which you were aiming.
- Tiered smoothing and tightening, so fine aim is steady and flicks are not.
- Flick carry, so a fast movement that asks for more than full deflection
  still completes instead of being clipped.
- Player space, taking yaw around gravity, so a tilted pad still turns level.
- Turn-rate rumble, and per-game profiles chosen automatically.
- 33 settings, all live from the portal.
- Installs and removes cleanly; the game keeps running either way.

Not built yet: the touchpad as a target, and the native app.

## Tuning it

Two numbers matter more than the rest, and both describe the game rather than
you:

- **Full-stick turn rate.** Hold the stick fully right, time one complete
  revolution, and enter the degrees per second. Until this is right,
  sensitivity is an arbitrary number; once it is, sensitivity 1.0 means the
  camera turns exactly as far as your hands do.
- **Stick deadzone and response curve.** Most games ignore the first 10 to 20
  percent of travel and bend what is left. Get these close and small
  movements move the camera by the right amount.

The live panel shows your hand's rate, the camera speed being asked for, and
the deflection being sent, which makes the effect of each setting visible.

## Tests

```sh
make -C tests check
```

Both run on the host with no console. One checks the response-curve tables are
genuine inverses; the other runs the injected transform itself against a
stand-in pad, which is the only practical way to test sign conventions.

## The two pieces

**The service** is a payload. It does the work, runs as root outside any
sandbox, and survives game launches.

**The control panel** is a native title, `PPSA99510`. It cannot do the
service's job, because launching a game tears a title down, so instead it
installs the service, starts and stops it, says whether it is running, and
puts a QR code on screen so a phone can reach the settings. Closing the panel
leaves the service running.

The panel ships the service inside its own package, so it works on a console
that has never had either.

## Build and run

The control panel, which bundles the service:

```sh
tools/setup-deps.sh                     # the payload SDK
tools/setup-app-deps.sh                 # the title pipeline and QR encoder
make -C app deploy PS5_HOST=<ip>        # builds both, installs the title
```

Launch GyroAim from the console's home screen and choose **start the
service**. Everything after that is the panel or the phone.

The panel's last item, set apart at the bottom, removes the service from the
console: it stops it, deletes the copy that starts at boot, and takes its line
out of pldmgr's list, leaving every other payload alone. It asks first, and
offers to keep your tuning and per-game profiles or to remove those too. The
app always keeps its own copy, so start it again whenever you like.

Removing the app itself does not remove the service, which is why that button
exists.

The service on its own, if you would rather not install a title:

```sh
make -C payload deploy PS5_HOST=<ip>    # straight to the ELF loader
```

The console needs an ELF loader on port 9021 and kstuff loaded. The panel says
so plainly if kstuff is missing, since the transform cannot be installed
without it.

**Starting with the console** is a button in the panel. It copies the payload
to `/data/pldmgr/payloads/gyroaim/` and adds one line to
`/data/pldmgr/autoload.txt`, leaving every other payload alone. It needs
ShadowMount+ new enough to mount `/data` into a title, which the panel checks.

## Using it

Open `http://<console ip>:6080` from a phone or PC on the same network.

Every number has both a slider and a box you can type into, and sections
collapse so the page is not a wall of controls.

Routing is a three by four grid: a row per rotation, a column per control.
Tapping a square cycles it between blank, on, and inverted. That keeps the
whole mapping visible at a glance and avoids a separate copy of every slider
for every axis, which is what a direct approach would have cost.

Two live graphs show what is happening: the game's stick curve with a dot
tracing where you are on it, and hand speed against view speed.

**There is no authentication.** Anyone on your network can open the portal and
change the settings.

For the aiming to feel right, the game's own settings matter as much as these:
turn the in-game stick deadzone as low as it goes, pick a linear response
curve, turn off stick acceleration, and set sensitivity high enough that the
camera is not the limit.

## Releasing

```sh
tools/release.sh 0.6.0
git push origin main v0.6.0
```

That builds, tests, checks the package against the catalog's rules, stamps the
version in the three places it lives, commits and tags. GitHub then assembles
the title and opens a draft release with the archive attached.
[docs/RELEASING.md](docs/RELEASING.md) has the details, including why the
version has to be committed before the tag.

## Publishing it

The control panel is a normal native title, so it can be listed in the
[PS5 homebrew catalog](https://github.com/blackbearreloaded/ps5-homebrew-catalog)
and installed from a store with one button. `tools/check-release.sh` applies
the catalog's rules to the built package, and runs as part of `make release`.

One thing the listing needs beyond the usual record: the bundled service
approved by a maintainer. The catalog's scan flags any app shipping a payload
that runs outside the sandbox, and approved ones are listed by hash, as
ProsperoStore's own helpers are. That is a review step, not a code change.
[docs/RELEASING.md](docs/RELEASING.md) has the record template.

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
