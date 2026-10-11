# Releasing

One command cuts a release, and GitHub builds it:

```sh
tools/release.sh 0.6.0
git push origin main v0.6.0
```

`release.sh` builds, runs the tests and the submission checks, stamps the
version in both places it lives, commits that, and tags it. The workflow then
assembles the title, attaches the archive to a **draft** release, and leaves
publishing to you.

## Why the versions are stamped before tagging

Three places carry a version and they have to agree:

| | |
| --- | --- |
| The release tag | `v0.6.0`, what the catalog lists |
| `app/sce_sys/param.json` | `contentVersion` `00.006.000`, how a console spots an update |
| `payload/version.h` | `GA_VERSION` `0.6.0`, what the control panel shows |

The catalog reads `contentVersion` from the **committed** `param.json` at the
release tag, not from the built archive. If it were stamped during the build,
the committed value would stay behind and the listing would show a release no
console ever offers as an update. So the bump has to be in the commit the tag
points at, which is the order `release.sh` uses. The workflow then runs
`tools/set-content-version.sh --check` and refuses the release if they disagree.

`contentVersion` must be `NN.NNN.NNN` and must only ever rise. The mapping is
`v1.2.3` to `01.002.003`, so a major above 99 or a minor or patch above 999 is
refused rather than silently truncated into a version that moves backwards.

## Checking before you tag

```sh
make release          # build, test, and check against the catalog's rules
tools/check-release.sh
```

`check-release.sh` applies the catalog's own rules to what was just built: the
app folder at the top of the archive, `eboot.bin` and `sce_sys/` present, a
title id that is not the boilerplate default, a well-formed `contentVersion`,
no firmware floor, a square PNG icon of at least 256 pixels and under 2 MiB, a
licence file for GitHub to detect, and nothing borrowed left in the package.
The catalog runs the equivalent when the pull request opens, by which point a
release is already published, so it is worth knowing beforehand.

## Listing it in the homebrew catalog

A record at `apps/PPSA99510.json` in
[the catalog](https://github.com/blackbearreloaded/ps5-homebrew-catalog):

```json
{
  "titleid": "PPSA99510",
  "name": "GyroAim",
  "kind": "app",
  "description": "Gyro aiming in any game, with the controller's gyroscope driving the stick and the settings on your phone.",
  "license": "GPL-3.0",
  "author": "jmelovich",
  "version": "0.6.0",
  "source_repo": "https://github.com/jmelovich/ps5-gyroaim",
  "artifact_url": "https://github.com/jmelovich/ps5-gyroaim/releases/download/v0.6.0/PPSA99510.zip",
  "sha256": "<printed by make release>",
  "icon_url": "https://raw.githubusercontent.com/jmelovich/ps5-gyroaim/v0.6.0/app/sce_sys/icon0.png"
}
```

`version` must be the tag or the tag without its leading `v`. Pin `icon_url` to
the tag so the icon cannot change under the listing; the catalog also looks for
`param.json` beside that icon, which is where ours is.

The pull request has to come from the account that owns the repository, and may
change only that one file.

### The one extra step this app needs

The catalog's scan flags any app that ships a payload running outside the
sandbox, and GyroAim ships exactly that: `gyroaim.elf`, inside the title, which
is the whole point of it. Approved payloads are listed by SHA-256 in the
catalog's `helpers/approved.json`, as ProsperoStore's own helpers are, so a
maintainer has to read the service and accept its hash. `make release` prints
that hash.

The scan will also note that the app connects to the payload loader on
`127.0.0.1:9021`. That is correct: it is how the panel starts the service.

## Release notes

The workflow writes a first draft covering installation, what the console
needs, and the settings page, with a compare link to the previous tag. Edit it
before publishing. The catalog turns release notes into the app's notes on the
website and in the store, so put what changed first, under short headings with
bullet lists. Images, links and HTML are not carried over.
