# Releasing

One command cuts a release, and GitHub builds it:

```sh
$EDITOR docs/releases/v1.0.3.md          # what changed, in a few lines
tools/release.sh 1.0.3
git push origin main v1.0.3
```

`release.sh` builds, runs the tests and the submission checks, stamps the
version in both files it lives in, commits that, and tags it. The workflow
then assembles the title, attaches the archive to a **draft** release, and
leaves publishing to you.

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

Generate the record rather than typing it, so the digest and the URLs cannot
be mistyped:

```sh
tools/catalog-record.sh v1.0.0
```

It reads the title id from `param.json` and the digest from the published
release, and prints `apps/PPSA99510.json` ready to paste into a fork of
[the catalog](https://github.com/blackbearreloaded/ps5-homebrew-catalog).

Two things have to be true first, and the script says so if they are not:

- **The release is published, not a draft.** The catalog cannot download a
  draft's asset.
- **The repository is public.** `source_repo` and `icon_url` have to resolve.

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

Notes have two parts. What is worth saying about a particular release goes in
`docs/releases/<tag>.md`, written before cutting the tag. The install section,
what the console needs, the settings page and the compare link are the same
every time and are generated around it.

`tools/release-notes.sh <tag>` prints the result, so the notes can be read
before anyone publishes them. The workflow calls the same script, so what you
see locally is what gets posted.

Keeping them in the repo matters for two reasons. Notes written straight into
the draft cannot be reviewed like anything else, and they are lost the moment
a tag is re-cut, which happens whenever a problem is found before publishing.

The catalog turns release notes into the app's notes on the website and in the
store, so lead with one sentence on what the app is for, then what changed,
one sentence a bullet. Images, links and HTML are not carried over.
