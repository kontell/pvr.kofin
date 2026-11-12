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
calls. Targets Kodi 21 Omega, Kodi 22 Piers and Kodi 23 (pre-release, no
codename yet), Jellyfin 10.9.x+.

**Add-on ID:** `pvr.kofin`

## Build

Standard Kodi add-on build system (`cmake/addons`); needs a Kodi source tree for
the target version. jsoncpp is the only external dependency and builds
automatically. (pugixml, zlib and lzma were dropped in 0.12.0 with the dead fork
code they served.)

```bash
./scripts/build.sh --os <linux|android> --arch <x86_64|armv7|aarch64> \
                   --kodi <21|22|23> --kodi-src <path> [--ndk <path>]

# On a Mac: osx x86_64|arm64, ios aarch64, tvos aarch64
./scripts/build.sh --os osx --arch arm64 --kodi <21|22|23> --kodi-src <path>
```

`--kodi 23` is accepted on `master` only.

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
| `.github/workflows/` | `ci`, `release`, `drift`, `notify-repo` |

## Release

**This file is kept the same on all three channel branches.**

| | `Omega` | `Piers` | `master` |
|---|---|---|---|
| Kodi | 21 | 22 | 23 |
| Version / tags | `21.y.z` / `v21.y.z` | `22.y.z` / `v22.y.z` | `23.y.z` / `v23.y.z` |
| Kodi source pin | `21.3-Omega` | `22.0b1-Piers` | xbmc master `fe8292d` |
| Repository dir | `omega/` | `piers/` | `kodi23/` |
| Policy | bugfix-only, ported on demand | ported from master | where development happens; pre-releases only |

Development targets `master`: branch new work from it and open pull requests
against it, then backport to Piers and, where wanted, Omega. A backport keeps
to what the older branch has; Piers lacks the SyncPlay provider, for one.

`master` is pinned to a commit because Kodi 23 has no tag yet; move the pin to
the first `23.0` tag when one appears. Its releases are published as GitHub
pre-releases, which repository.kontell places in `dev/kodi23/`.

1. Bump `version=` in `addon.xml.in`, keeping the branch's major.
2. Prepend a `changelog.txt` entry; its top paragraph becomes the release body.
3. Commit and push to the channel branch.
4. `git tag vNN.Y.Z && git push origin vNN.Y.Z`.
5. `release.yml` asserts the tag matches `addon.xml.in` **and** that its major
   matches the branch, then builds 10 zips on Piers and master or 9 on Omega and
   refuses to draft unless all are present.
6. Review the draft, then **publish it yourself** — see `kodi-addon-release` for
   why a workflow cannot.
7. `repository.kontell` places the zips and verifies the Worker serves the new
   checksum.

The major carries the Kodi version, which is how `repository.kontell` decides
between `omega/` and `piers/`. The tag-major guard is the one that earns its keep:
a tag pushed on the wrong branch would otherwise produce a complete, plausible
release filed into the wrong directory.

A release carries Linux ×3, Android ×2, macOS ×2, Windows i686 and x86_64, and on
Piers Windows arm64, which Kodi 21 does not build. repository.kontell requires the
nine common to both channels and places the tenth without requiring it. `ci.yml`
builds more than a release ships: iOS, tvOS and a Windows Store compile as well,
which together cover every target Team Kodi's builder compiles. Upstream's tip is
the xbmc branch named for the channel (`Omega`, `Piers`), and xbmc `master` for
Kodi 23; `drift.yml` and CI's non-blocking tip job both read it. `drift.yml`'s
schedule fires on the default branch only, which is Piers, so on master and
Omega it runs when dispatched with `--ref`.

Pinned deliberately: the **Kodi source ref** (not a branch) and the **build
container** `ubuntu:22.04` (not a runner label). `drift.yml` watches upstream
weekly so the pin staying still is a decision rather than neglect. The reasoning
for both is in `kodi-versions-abi` and `kodi-binary-build`.

Local builds are for test installs only — production zips come from the release
workflow.
