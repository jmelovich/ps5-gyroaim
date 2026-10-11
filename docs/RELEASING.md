# Releasing

```sh
make release
```

Builds the service and the title from clean, then prints the archive and its
SHA-256. The archive is `app/dist/PPSA99510.zip`, about 200 KB.

## What goes in a release

Attach `PPSA99510.zip` to a GitHub release tagged with the version. The
service's version is `GA_VERSION` in `payload/version.h`, which is what the
control panel displays; bump it before tagging.

The title's own `contentVersion` in `app/sce_sys/param.json` is separate and
only matters to the console's installer. Bump it when the package changes in a
way the console should treat as an update.

## Listing it in the homebrew catalog

A record at `apps/PPSA99510.json` in
[the catalog](https://github.com/blackbearreloaded/ps5-homebrew-catalog),
following the shape every other record uses:

```json
{
  "titleid": "PPSA99510",
  "name": "GyroAim",
  "kind": "app",
  "description": "Gyro aiming in any game. A resident service turns the DualSense's gyroscope into stick input, tuned from a phone while you play.",
  "license": "GPL-3.0",
  "author": "<your name>",
  "version": "<the release tag>",
  "source_repo": "<this repository>",
  "artifact_url": "<the release asset's URL>",
  "sha256": "<printed by make release>",
  "icon_url": "<raw URL of app/sce_sys/icon0.png at that tag>"
}
```

One extra step this app needs that most do not. The catalog's release scan
flags any app shipping a payload that runs outside the sandbox, and GyroAim
ships exactly that: `gyroaim.elf`, inside the title, which is the whole point.
Approved payloads are listed by SHA-256 in the catalog's
`helpers/approved.json`, as ProsperoStore's own helpers are, so a maintainer
has to read the service and accept its hash. That is a review step rather than
anything to change here.

Expect the scan to also note that the app reaches the payload loader on
`127.0.0.1:9021`. That is correct and is how the panel starts the service.
