# AGENTS.md

Guidance for coding agents working in this repository.

## Kodi knowledge lives in kodi-drive

Shared Kodi knowledge is **not** in this file. It lives in
[`kontell/kodi-drive`](https://github.com/kontell/kodi-drive): use its
`kodi-drive:*` skills, or read its `README.md`.

Directly relevant here: `kodi-pvr-addon`, `kodi-binary-settings`,
`kodi-binary-build`, `kodi-android-ndk`, `kodi-versions-abi`,
`kodi-addon-release`, `kodi-inputstream`, and `jellyfin-client` under `adjacent/`.

**Do not add generally-useful Kodi findings here.** If you learn something that
would help someone working on a different add-on, it belongs in
`kontell/kodi-drive` — contribute it before you finish the session.

This file holds only: what this project is, its build and release commands, and
decisions specific to *this* add-on.

## Project overview

Kodi PVR add-on for Jellyfin Live TV. C++17 shared library implementing the Kodi
PVR client interface for channels, EPG, recordings and timers. Forked from
`pvr.iptvsimple` with M3U/XMLTV/catchup/VOD stripped and replaced by Jellyfin API
calls. Jellyfin 10.9.x+.

**Add-on ID:** `pvr.kofin`

### Where this lives

The add-on is developed and released by Team Kodi in
[`kodi-pvr/pvr.kofin`](https://github.com/kodi-pvr/pvr.kofin), branch `Piers`
for Kodi 22, and shipped by the official Kodi repository for every platform
except Linux. This repository, `kontell/pvr.kofin`, is a fork of it that
publishes what the official repository does not:

| Branch | Kodi | What the fork does with it |
|---|---|---|
| `Omega` | 21 | fork-only (kodi-pvr has no Omega). Bugfix-only; released here for every platform |
| `Piers` | 22 (and 23) | upstream's `Piers` plus the `kontell-*` workflows; released here for Linux only |
| `master` | 23 | paused. Not released; Kodi 23 is served the Piers Linux build |

**This file lives on the fork's `Omega` branch only.** kodi-pvr removed it from
Piers, and the fork keeps Piers identical to upstream apart from its workflows.

Remotes in a local checkout: `origin` may still be `kodi-pvr/pvr.kofin`. Add the
fork as its own remote and push fork branches and **every release tag** there,
never to kodi-pvr. `gh pr create` from a fork defaults to the parent repository,
so pass `-R kontell/pvr.kofin` for a fork PR.

## Build

Standard Kodi add-on build system (`cmake/addons`); needs a Kodi source tree for
the target version. jsoncpp is the only external dependency and builds
automatically. (pugixml, zlib and lzma were dropped in 0.12.0 with the dead fork
code they served.)

```bash
./scripts/build.sh --os <linux|android> --arch <x86_64|armv7|aarch64> \
                   --kodi <21|22> --kodi-src <path> [--ndk <path>]

# On a Mac: osx x86_64|arm64, ios aarch64, tvos aarch64
./scripts/build.sh --os osx --arch arm64 --kodi <21|22> --kodi-src <path>
```

Use `--kodi 21` on Omega and `--kodi 22` on Piers.

Kodi's `cmake/addons` resolves the add-on source as
`<parent of this checkout>/pvr.kofin`, so a worktree or copy built with
`build.sh` must itself be named `pvr.kofin`.

Linux build deps: `kodi-addons-dev`, `libjsoncpp-dev`, `m4`, `autoconf`,
`automake`, `libtool`, `autopoint`.

No unit tests — testing is manual against a running Jellyfin server on Android TV
and Linux.

## Architecture

```
CIptvSimpleAddon (addon.cpp)        Kodi entry point, creates the PVR instance
  └─ IptvSimple                     implements the PVR callbacks, owns the models
       ├─ ConnectionManager         health-check loop (/System/Ping)
       ├─ Channels / ChannelGroups  in-memory storage, from pvr.iptvsimple
       └─ jellyfin/
            ├─ JellyfinClient           HTTP REST via kodi::vfs::CFile
            ├─ JellyfinChannelLoader    channels, EPG, stream URLs, device profile
            ├─ JellyfinRecordingManager timers (4 types), series timers, recordings
            └─ JellyfinAuth             login / logout / Quick Connect
```

Single instance, global settings — categorized `settings.xml` (`version="1"`,
`<section>`/`<category>`/`<group>`). No `instance-settings.xml`.

## Decisions specific to this add-on

- **Stream properties, not a demuxer.** `GetChannelStreamProperties()` returns URL
  plus inputstream config; `StreamUtils` (from iptvsimple) picks ffmpegdirect or
  adaptive.
- **`service.py` is the sole close authority** for live streams and session
  reporting, moved out of C++ in `69f4c47`. It also finalises the outgoing session
  on a seamless zap from `onAVStarted`, so a shared `LiveStreamId` is never
  double-closed. `JellyfinChannelLoader::CloseLiveStream()` only clears local state.
- **Catchup is pinned to the reference-playlist streaming URL** (ffmpegdirect,
  or Tempo on Omega only), including via the play-as-live path, which routes through
  `GetChannelStreamProperties()`. Jellyfin 12 no longer DirectPlays HLS/DASH
  tuner sources, so catchup without a playlist URL is not available.
- **`master` has no inputstream.tempo** (23.18.1). SyncPlay fine sync on Kodi 23
  is Kodi's own tempo and `presentationtime` (kontell/xbmc #16, #19, #25 and
  kontell/inputstream.ffmpegdirect #1), so the claim carries no tempo route.
- **Piers has neither inputstream.tempo nor the SyncPlay provider** (22.19.0).
  The tempo removal is master's PR #83 cherry-picked, so the two branches read
  the same in `GetChannelStreamProperties()`: a stored `inputStream=3` is read
  as ffmpegdirect, playlist entries pinning `inputstream.tempo` are mapped to
  `inputstream.ffmpegdirect`, and live tuning takes the timeshift buffer on
  every channel. Piers alone also dropped the provider from `service.py`: no
  claim, no registration, no delegated start, so a kofin SyncPlay group cannot
  follow live TV, catchup or recordings from this add-on on Kodi 22. master
  keeps the provider for Kodi 23; Omega keeps both tempo and the provider.
- **Device profile emits two TranscodingProfiles** — fMP4 for AV1, TS otherwise.
- **LAN server discovery lives in Python** (`resources/scripts/discover.py`), not
  beside `JellyfinAuth`. The binary settings ABI has no action callback and no way
  to open the add-on's own settings dialog, so only Python can reopen settings with
  the discovered address filled in; and Windows is a release target, so C++ sockets
  would need a Winsock shim for every Windows artifact. It enters through
  `trigger.py` like the buttons, but is not one of its allowlisted setting pokes —
  discovery writes `jellyfinServerAddress` itself and needs no C++ at all.
- **Settings buttons call `RunScript(pvr.kofin,<action>)`**, with no install
  path, on both channels since 22.18.0 and 21.18.0. `addon.xml.in` declares
  `trigger.py` as an `xbmc.python.library` extension, which is what an id-form
  `RunScript` resolves to, so `trigger.py` is
  the add-on's one script entry point and dispatches on `sys.argv[1]`. Mechanism
  and evidence are in kodi-drive (`kodi-binary-settings`, `kodi-addon-manifest`).
- **A discovered address carrying a path is refused, not stored.** Both address
  normalisers keep only host and port (`InstanceSettings::GetJellyfinBaseUrl`,
  `service.py::normalize_base_url`), so a Jellyfin published under a path would
  leave a settings field that reads correctly and does not work.
- **Credentials are never auto-cleared on a validation failure** (0.12.0).
- **`RewriteLocalhost()`** fixes stream URLs where Jellyfin returns `127.0.0.1`.
- **`WebUtils::RedactUrl()`** masks `ApiKey`/`token`/`secret`/`x-emby-token`
  before logging (0.12.0).
- **In-progress recording cross-references are built only from `InProgress`
  timers** (0.12.0), so a later same-named future timer cannot hijack the EPG link.
- **Recording deletion:** `DELETE /Items/{id}`, falling back to
  `DELETE /LiveTv/Recordings/{id}`.

**Verify Jellyfin response shapes against a live server before writing code that
consumes them** — field names in code or docs are not evidence. Credentials for a
test server go in kodi-drive's `targets.env`, never in this file.

## Key files

| Path | |
|---|---|
| `pvr.kofin/addon.xml.in` | metadata template, version substituted by CMake |
| `pvr.kofin/resources/settings.xml` | categorized settings, Server/Playback/Advanced |
| `.../strings.po` | localized strings, IDs 30600+ |
| `src/iptvsimple/InstanceSettings.h` | settings getters, bitrate table, codec helpers |
| `pvr.kofin/changelog.txt` | top paragraph becomes the release body |
| `scripts/check-abi.sh` | asserts the ABI floor of the built `.so` |
| `.github/workflows/` | Omega: `ci`, `release`, `drift`. Piers: `kontell-ci`, `kontell-release`, `kontell-drift` beside upstream's own |

## Release

| | `Omega` | `Piers` |
|---|---|---|
| Kodi | 21 | 22, and 23 via `kodi23/` |
| Version | `21.y.z`, bumped here | `22.y.z`, **upstream's**, never bumped here |
| Tag | `v21.y.z` | `v22.y.z`, on the fork only (upstream tags `22.y.z-Piers`) |
| Kodi source pin | `21.3-Omega` | `22.0b1-Piers` |
| Workflows | `ci.yml`, `release.yml`, `drift.yml` | `kontell-ci.yml`, `kontell-release.yml`, `kontell-drift.yml` |
| Zips | 9: Linux ×3, Android ×2, macOS ×2, Windows i686 and x86_64 | 3: Linux x86_64, armv7, aarch64 |
| repository.kontell | `omega/` | `piers/` and `kodi23/` |

### Omega

Bugfix-only. Fixes are ported on demand from kodi-pvr's Piers, through a pull
request against the fork's `Omega`.

1. Bump `version=` in `addon.xml.in`, keeping the major at 21.
2. Prepend a `changelog.txt` entry; its top paragraph becomes the release body.
3. Commit and push to `Omega` on the fork.
4. `git tag v21.Y.Z && git push <fork remote> v21.Y.Z`.
5. `release.yml` asserts the tag matches `addon.xml.in` **and** that its major
   is 21, then builds 9 zips and refuses to draft unless all are present.
6. Review the draft, then **publish it yourself** — see `kodi-addon-release` for
   why a workflow cannot.
7. Run repository.kontell's `tools/update.py pvr.kofin`.

### Piers

The version, changelog and code are upstream's; the fork adds Linux zips of
what kodi-pvr has released. Upstream tags each release `22.Y.Z-Piers`, and its
Piers branch may already have moved past that tag, so a fork release is built
from **upstream's tag**, not from the tip of the fork's `Piers`: one commit on
top of the tag that adds the `kontell-*` workflows, kept on a `release/22.Y.Z`
branch and tagged `v22.Y.Z`. The source is then byte-for-byte what upstream released.

Below, `<upstream>` is the remote for kodi-pvr/pvr.kofin and `<fork>` the one
for kontell/pvr.kofin.

```bash
git fetch <upstream> --tags
git fetch <fork>

# Start from exactly what upstream released
git checkout --detach 22.Y.Z-Piers

# Add only the fork's workflows on top
git checkout <fork>/Piers -- .github/workflows/kontell-ci.yml \
                             .github/workflows/kontell-release.yml \
                             .github/workflows/kontell-drift.yml
git commit -m "kontell Linux release of 22.Y.Z-Piers"

# Must list only those three files
git diff --stat 22.Y.Z-Piers HEAD

# Put the commit on a fork branch first, then tag it and push the tag
git push <fork> HEAD:refs/heads/release/22.Y.Z
git tag v22.Y.Z
git push <fork> v22.Y.Z
git checkout -
```

The `release/22.Y.Z` branch is not optional. With the release commit on no
branch, `kontell-release.yml` built all three zips and then failed to draft:
creating the release returned `403 Resource not accessible by integration`,
though the job's token had `contents: write`. Pushing the same commit to a
branch and re-running only the failed job drafted it (v22.20.1). If a tag was
pushed without the branch, push the branch and run
`gh run rerun <run id> -R kontell/pvr.kofin --failed`; nothing needs retagging.

When `git diff --stat 22.Y.Z-Piers <fork>/Piers` already lists only the three
`kontell-*` files, the fork's `Piers` is that release plus the workflows, and
tagging `<fork>/Piers` directly is equivalent.

1. Build the release commit, push it to `release/22.Y.Z`, then push its tag,
   as above.
2. `kontell-release.yml` asserts the tag matches `addon.xml.in`, that its major
   is 22, and that `changelog.txt` leads with that version, then builds the three
   Linux zips and drafts.
3. Publish the draft, then run repository.kontell's `tools/update.py pvr.kofin`,
   which places the zips in `piers/` and copies them into `kodi23/`. It reads
   published releases only, so it does not matter that the tag is on no branch.

The fork's `Piers` branch itself is kept as upstream plus the `kontell-*` files,
synced with `gh repo sync kontell/pvr.kofin -b Piers`; it is where the workflows
are changed and where pull requests run CI. A sync that cannot fast-forward or
merge cleanly means something else was committed there.

The zips are not byte-identical to Team Kodi's builds, only their source is:
upstream's builder compiles against the head of xbmc's Piers branch, the fork
against the `22.0b1-Piers` pin in `ubuntu:22.04`. The official repository ships
no Linux build to compare with.

Every `kontell-*` job is gated on `github.repository == 'kontell/pvr.kofin'`, so a
pull request from the fork to kodi-pvr does not run them there. Upstream's own
workflows on Piers stay enabled: two run only on manual dispatch and the
translation sync is gated to kodi-pvr. Do not disable upstream's `release.yml`
("Make Release") on the fork — GitHub identifies a workflow by its file path, so
that would disable Omega's `release.yml` too.

### master

Paused. Do not tag it: a `v23` tag would still run master's `release.yml`. Piers
builds install on Kodi 23 for now, so repository.kontell copies them into
`kodi23/`. `kontell-drift.yml` checks the Piers pin against xbmc `master` weekly;
when that check fails, Kodi 23 no longer accepts the Piers build, and `kodi23/`
needs either a Kodi 23 release again or pvr.kofin dropped from it.

### Pins and drift

The major carries the Kodi version, which is how repository.kontell decides
between `omega/` and `piers/`. The tag-major guard is the one that earns its keep:
a tag pushed on the wrong branch would otherwise produce a complete, plausible
release filed into the wrong directory.

Pinned deliberately: the **Kodi source ref** (not a branch) and the **build
container** `ubuntu:22.04` (not a runner label). The drift workflows watch
upstream weekly so the pin staying still is a decision rather than neglect. The
reasoning for both is in `kodi-versions-abi` and `kodi-binary-build`. A drift
workflow's schedule fires on the default branch only, which is Piers, so
Omega's `drift.yml` runs when dispatched with `--ref Omega`.

Local builds are for test installs only — production zips come from the release
workflows.
