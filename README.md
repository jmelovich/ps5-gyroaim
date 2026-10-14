# GyroAim for PS5

Gyro aiming in any game on a jailbroken PS5.

A resident service folds the DualSense's gyroscope into the control a game
actually reads, usually the right stick, while a condition you choose is met,
usually holding the left trigger. Each rotation can turn the view as you move
it or hold the control over while you keep the pad tilted, so the same tool
aims and steers. Tune it from a phone while the game runs.

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
  other, or one rotation can drive both.
- **Aiming or steering, per rotation.** Read as a speed it turns the view
  while you keep moving; read as an angle from rest it holds the control over
  until you come back, which is what driving, skating and flying want.
- **A held angle is measured against gravity**, so it cannot creep. Turning a
  pad lying flat is the one rotation gravity cannot see, and the page says so
  rather than leaving it to be discovered.
- **Works in camera-velocity space**, so the game's own deadzone and response
  curve are inverted rather than fought. This is the part that makes aiming
  feel proportional instead of mushy near the centre and twitchy further out.
- **Gated on a trigger, a named button, or always.** A button condition picks
  from a chip per button, or takes the whole set from whatever you are
  holding.
- Strength per rotation, in degrees of view per degree of hand movement, with
  an acceleration multiplier that comes in over a speed band.
- Drift calibration that learns the gyro's resting offset while the pad is
  still, judged by how small the offset is rather than by whether you happened
  to be aiming.
- Tiered smoothing and tightening, so fine aim is steady and flicks are not.
- Flick carry, so a fast movement that asks for more than full deflection
  still completes instead of being clipped.
- Player space, taking yaw around gravity, so a tilted pad still turns level.
- Turn-rate rumble.
- **Per-game profiles, chosen automatically and layered** over your general
  settings, so a profile inherits what it does not mention rather than
  borrowing it from whatever was played last.
- **A reset to the defaults**, from the page, back to plain aiming on the
  right stick with the left trigger.
- 50 settings, all live from the portal.
- Only one service can run at a time, and the panel replaces an older one
  rather than leaving two installed.
- Installs and removes cleanly; the game keeps running either way.

Not built yet: the touchpad as a target.

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
the deflection being sent, which makes the effect of each setting visible. It
also shows how far each rotation is from rest and whether gravity is what that
is measured against, which is the difference between an angle that holds and
one that creeps.

For steering rather than aiming, switch a rotation to being held and set how
far from rest counts as full. Rest is taken whenever the condition engages, so
pressing the trigger says "straight ahead is here"; there is a button for
setting it by hand, and a switch to stop it being retaken.

## Tests

```sh
make -C tests check
```

All of them run on the host with no console attached, and each exists because
of a bug it would have caught.

- **config** builds the settings for a game from real files, after per-game
  profiles were found leaking into one another.
- **curve** checks the response-curve tables are genuine inverses.
- **transform** runs the injected transform against a stand-in pad, which is
  the only practical way to test sign conventions and the only way to prove a
  held angle does not creep without waiting half an hour.
- **qr** decodes the code the panel draws.
- **page** runs the portal's own script against a stub DOM and presses its
  controls, after a section rendered empty with the script parsing cleanly.
  Skipped when `node` is missing.

## The two pieces

**The service** is a payload. It does the work, runs as root outside any
sandbox, and survives game launches.

**The control panel** is a native title, `PPSA99510`. It cannot do the
service's job, because launching a game tears a title down, so instead it
installs the service, starts and stops it, says whether it is running, and
puts a QR code on screen so a phone can reach the settings. Closing the panel
leaves the service running.

The panel ships the service inside its own package, so it works on a console
that has never had either, and it keeps the two in step: on launch it replaces
a running service that differs from the one it carries, and refreshes the copy
that starts with the console. Updating the app therefore updates the service,
rather than leaving an older one running with nothing saying so.

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
for every axis, which is what a direct approach would have cost. Each row also
carries whether that rotation is read as a speed or held as an angle.

The condition shows only the settings it uses: a trigger brings up which
trigger and its threshold, a button brings up a chip for each button instead.

**Keep these for this game** writes a profile the service picks up on its own
next time that game starts. **Reset to defaults** goes back to plain aiming,
and asks twice, since there is no getting a tuned profile back.

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
$EDITOR docs/releases/v1.0.3.md          # what changed, in a few lines
tools/release.sh 1.0.3
git push origin main v1.0.3
```

That builds, tests, checks the package against the catalog's rules, stamps the
version in both files it lives in, commits and tags. GitHub then assembles
the title and opens a draft release with the archive attached, for you to read
and publish.

The notes come from `docs/releases/<tag>.md`, with the install and warning
sections generated around it by `tools/release-notes.sh`, which you can run
yourself to see exactly what will be posted. Keeping them in the repo is what
lets them be reviewed, and what stops them being lost when a tag is re-cut.
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
