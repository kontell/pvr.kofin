# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Kodi PVR addon for Jellyfin Live TV. C++17 shared library implementing the Kodi PVR client interface for live TV channels, EPG, recordings, and timers from a Jellyfin server. Forked from pvr.iptvsimple with M3U/XMLTV/catchup/VOD stripped and replaced by Jellyfin API calls. Targets Kodi 21 "Omega" and Kodi 22 "Piers", Jellyfin 10.9.x+.

**Addon ID:** `pvr.kofin`

## Reference Implementation

The playback pipeline (DeviceProfile, PlaybackInfo requests, stream URL construction, transcode/remux/direct play logic) should match the behaviour of the forked jellyfin-kodi Python addon at `/media/bluecon/docs/IT/kofin/jellyfin-kodi/`. That addon is the authoritative reference for how Jellyfin API calls should be structured — compare against it when debugging playback or stream issues.

## Build Commands

Uses the standard Kodi addon build system (`cmake/addons`). Requires a Kodi source tree for the target version. Dependencies (jsoncpp, pugixml, zlib, lzma) are built automatically.

**Prerequisites:** The addon must be registered in the Kodi source's addon definitions:
```bash
mkdir -p <kodi-source>/cmake/addons/addons/pvr.kofin
echo "pvr.kofin https://github.com/kontell/pvr.kofin main" > <kodi-source>/cmake/addons/addons/pvr.kofin/pvr.kofin.txt
echo "all" > <kodi-source>/cmake/addons/addons/pvr.kofin/platforms.txt
```

**Linux system build deps:** `kodi-addons-dev`, `libjsoncpp-dev`, `libpugixml-dev`, `zlib1g-dev`, `liblzma-dev`, `m4`, `autoconf`, `automake`, `libtool`, `autopoint`.

```bash
# Linux x86_64 (Kodi v21 Omega)
# Kodi v21 source at /media/bluecon/docs/IT/kofin/kodi-omega-full/
mkdir -p build && cd build
cmake -DADDONS_TO_BUILD=pvr.kofin \
  -DADDON_SRC_PREFIX=/media/bluecon/docs/IT/kofin \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=../../kodi-omega-full/build/addons \
  -DPACKAGE_ZIP=1 \
  ../../kodi-omega-full/cmake/addons
make -j$(nproc)
# Output: kodi-omega-full/build/addons/pvr.kofin/

# Android ARM32 (Kodi v22 Piers)
# Kodi v22 source at /media/bluecon/docs/IT/kofin/kodi-piers-full/
# NDK r25c at /media/bluecon/docs/IT/kofin/android-ndk-r25c/
mkdir -p build-android && cd build-android
cmake -DADDONS_TO_BUILD=pvr.kofin \
  -DADDON_SRC_PREFIX=/media/bluecon/docs/IT/kofin \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=../../kodi-piers-full/build/addons-android \
  -DPACKAGE_ZIP=1 \
  -DCMAKE_TOOLCHAIN_FILE=/media/bluecon/docs/IT/kofin/android-ndk-r25c/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=armeabi-v7a -DANDROID_PLATFORM=android-21 \
  -DCPU=armv7a \
  ../../kodi-piers-full/cmake/addons
make -j$(nproc)

# Android ARM64 (Kodi v22 Piers)
# Requires a wrapper toolchain to force ANDROID_ABI through ExternalProject:
cat > /tmp/android-arm64-toolchain.cmake << 'EOF'
set(ANDROID_ABI arm64-v8a CACHE STRING "" FORCE)
set(ANDROID_PLATFORM android-21 CACHE STRING "" FORCE)
include(/media/bluecon/docs/IT/kofin/android-ndk-r25c/build/cmake/android.toolchain.cmake)
EOF
mkdir -p build-android64 && cd build-android64
cmake -DADDONS_TO_BUILD=pvr.kofin \
  -DADDON_SRC_PREFIX=/media/bluecon/docs/IT/kofin \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=../../kodi-piers-full/build/addons-android64 \
  -DPACKAGE_ZIP=1 \
  -DCMAKE_TOOLCHAIN_FILE=/tmp/android-arm64-toolchain.cmake \
  -DCPU=arm64-v8a \
  ../../kodi-piers-full/cmake/addons
make -j$(nproc)
```

**Packaging:** Create installable zips from the install prefix:
```bash
cd <kodi-source>/build/addons[-android[-64]]
zip -r /media/bluecon/docs/IT/kofin/builds/pvr.kofin-<ver>-<platform>.zip pvr.kofin/
```

No unit test suite — testing is manual against a running Jellyfin server on Android TV and Linux.

## Settings

**Uses the old flat Kodi settings format** (`settings.xml` with `type="text"`, `type="number"`, etc.). Does NOT use `instance-settings.xml` or multi-instance support. Settings are read via `kodi::addon::GetSettingString()` / `GetSettingInt()` / `GetSettingBoolean()` in `InstanceSettings::ReadSettings()`. Settings are persisted via `kodi::addon::SetSettingString()`.

Action buttons (Login, Logout, Test Connection) use `RunScript($CWD/resources/scripts/trigger.py,<buttonId>)` with `option="close"` to close the settings dialog first, then trigger the C++ `SetSetting()` callback via a Python helper that calls `addon.setSetting(buttonId, 'trigger')`. The addon-level `SetSetting()` in `addon.cpp` forwards to `IptvSimple::OnSettingChanged()`, which checks for the `"trigger"` value to distinguish real button clicks from settings-save noise.

## Architecture

```
CIptvSimpleAddon (addon.cpp)         — Kodi addon entry point, creates PVR instance
  └─ IptvSimple (IptvSimple.cpp)     — Implements all Kodi PVR callbacks, owns data models
       ├─ ConnectionManager           — Connection health check loop (pings /System/Ping)
       ├─ Channels / ChannelGroups    — In-memory channel/group storage (from pvr.iptvsimple)
       ├─ Providers                   — Provider data model (from pvr.iptvsimple)
       └─ jellyfin/                   — Jellyfin API integration
            ├─ JellyfinClient          — HTTP REST client (GET/POST/DELETE via kodi::vfs::CFile)
            ├─ JellyfinChannelLoader   — Channels, EPG, live stream URL resolution
            └─ JellyfinRecordingManager — Timers (3 types), series timers, recordings
```

### Key Design Decisions

- **Single instance, global settings**: No multi-instance PVR support. Uses `settings.xml` (old flat format), NOT `instance-settings.xml`. Settings read via `kodi::addon::GetSettingXxx()`.
- **Stream properties approach** (not demuxer): `GetChannelStreamProperties()` returns URL + inputstream config to Kodi. StreamUtils from iptvsimple handles inputstream selection (ffmpegdirect or adaptive).
- **Live stream lifecycle**: `GetLiveStreamUrl()` POSTs to `/Items/{id}/PlaybackInfo` with `AutoOpenLiveStream=true`, tracks `LiveStreamId`. `CloseLiveStream()` sends `POST /LiveStreams/Close` with JSON body on a detached thread. The server handles session/dashboard tracking automatically from `AutoOpenLiveStream` — no explicit `Sessions/Playing` reports needed. Note: Kodi v21 does NOT call `CloseLiveStream()` during normal playback stop when using `GetChannelStreamProperties()`; it only fires from the destructor or when switching channels via `GetItemStreamUrl()`.
- **Device profile**: Remux mode (unlimited bitrate): single TS TranscodingProfile with all non-AV1 codecs. Transcode mode (limited bitrate): single profile with only the preferred codec in the correct container (AV1→fMP4, others→TS). DirectPlayProfiles only populated when force remux is off AND bitrate is unlimited (Jellyfin ignores MaxStreamingBitrate for Protocol=Http live TV DirectPlay decisions).
- **Timer types**: 3 types: `TIMER_ONCE_EPG`, `TIMER_ONCE_CREATED_BY_SERIES` (read-only child), `TIMER_SERIES`.
- **Authentication**: Username/password (`POST /Users/AuthenticateByName`) or Quick Connect (initiated from Login settings button). Access token + user ID persisted to settings. On startup, validates stored token only — no automatic username/password retry (user must log in again via settings if token expires).
- **HTTP via Kodi VFS**: `kodi::vfs::CFile` for all HTTP — POST data must be Base64-encoded via the `postdata` protocol option.
- **Kodi v21/v22 compatibility**: `GetChannelStreamProperties` signature differs (v22 adds `PVR_SOURCE` param). CMake detects PVR API version from headers and sets `-DKODI_PVR_API_V9` for v22+. Code uses `#ifdef KODI_PVR_API_V9`.

### Recording Playback

**Stream pipeline:** `GetRecordingStreamProperties()` POSTs to `/Items/{id}/PlaybackInfo` with a recording-specific device profile (`BuildRecordingDeviceProfile()`) that forces remux into TS container at unlimited bitrate (~1Gbps = codec copy, no re-encoding). The resulting HLS URL is played via `inputstream.adaptive`. This is completely independent of live TV transcode/inputstream settings.

**Why remux, not static stream:** Jellyfin's `/Videos/{id}/stream?static=true` returns the raw recording file. Neither ffmpegdirect nor Kodi's default inputstream could seek in it (VLC also failed). The remuxed HLS stream has proper segment indices that inputstream.adaptive can seek through.

**Why inputstream.adaptive, not ffmpegdirect:** ffmpegdirect with `stream_mode=catchup` reported `LengthStream: -1` and failed to seek. With `stream_mode=default` it still couldn't seek. Jellyfin's HLS playlists use `#EXT-X-PLAYLIST-TYPE:EVENT` (no `#EXT-X-ENDLIST`) which ffmpegdirect treats as live/growing. inputstream.adaptive handles HLS segment-based seeking correctly.

**Two code paths in Kodi v21:**

1. **Home screen widgets / EPG actions:** `CPVRPlaybackState::StartPlayback()` calls `GetRecordingStreamProperties()`. If it returns `PVR_STREAM_PROPERTY_STREAMURL` + `PVR_STREAM_PROPERTY_INPUTSTREAM`, Kodi uses the specified inputstream. `OpenRecordedStream()` is never called.

2. **PVR Recordings section (window 10701):** Kodi does NOT call `GetRecordingStreamProperties()`. It opens `pvr://recordings/...` directly via `CInputStreamPVRRecording` → `OpenRecordedStream()`. Implemented via `OpenRecordedStreamImpl()` which HTTP-streams the recording file bytes via `/Videos/{id}/stream?static=true` using `kodi::vfs::CFile`.

**In-progress recordings:** Detected via `/LiveTv/Recordings?IsInProgress=true`. Timer name → channel UID / ProgramId cross-reference provides EPG linking (`SetEPGEventId`/`SetChannelUid`). `Process()` loop polls timers/recordings every 60 seconds and triggers Kodi UI updates.

**Container logic:** The recording device profile always requests TS container. For live TV, `BuildDeviceProfile()` creates a single TranscodingProfile per mode: remux (TS, all non-AV1 codecs) or transcode (preferred codec only, AV1→fMP4/others→TS). The server sets `SegmentContainer` correctly based on the profile. `PostProcessTranscodingUrl()` only recalculates bitrates and rewrites `stream`/`master` → `live` in the URL path.

### Jellyfin API Gotchas

- EPG: use `MaxStartDate` (not `MaxEndDate`) for time filtering
- Stream URLs: Jellyfin may return `127.0.0.1` or `localhost` — `RewriteLocalhost()` fixes this
- Recording deletion: `DELETE /Items/{id}` (primary), fallback `DELETE /LiveTv/Recordings/{id}`
- **Jellyfin API key for direct testing:** `https://jelly.konell.xyz` — append `?api_key=dac2156d1aa14643af37e1ddde87d963` to any endpoint. UserId: `215f5fc3f7ff4a5581e8518b28203a4f`.

## Key Files

- `pvr.kofin/addon.xml.in` — addon metadata template (version substituted by CMake)
- `pvr.kofin/resources/settings.xml` — user-facing settings (old flat format, categories for Server/Playback/Advanced)
- `pvr.kofin/resources/language/resource.language.en_gb/strings.po` — localized strings (IDs 30600+)
- `src/iptvsimple/InstanceSettings.h` — all settings with getters, bitrate table, codec name helpers
- `pvr.kofin/changelog.txt` — top paragraph is used as the GitHub release body
- `.github/workflows/build.yml` — compile-check on every push (GCC/Clang × Omega/Piers)
- `.github/workflows/release.yml` — on `v*` tag: builds 10-way matrix and drafts a GitHub release

## Release Process

Releases are cut by pushing a `v<version>` tag. GitHub Actions handles everything else.

1. Bump `version="X.Y.Z"` in `pvr.kofin/addon.xml.in`.
2. Prepend a new top entry to `pvr.kofin/changelog.txt`:
   ```
   v0.3.1
   - <bullet>
   - <bullet>

   v0.3.0
   ...
   ```
   The top paragraph (up to the first blank line) becomes the release body.
3. Commit (`Bump to X.Y.Z: <summary>`) and push to `main`. `build.yml` verifies it compiles.
4. Tag and push: `git tag vX.Y.Z && git push origin vX.Y.Z`.
5. `release.yml` cross-compiles 10 zips (Linux x86_64/armv7/aarch64 + Android armv7/aarch64, each × Kodi 21/22) and publishes a **draft** release on GitHub.
6. Review draft on GitHub (`gh release view vX.Y.Z`), edit if needed, publish.

Manual local builds into `/media/bluecon/docs/IT/kofin/builds/` are for test installs only — production zips come from the Release workflow.

## Android Packaging

The Android zip is built manually (no CI yet). Package structure:
```
pvr.kofin/
  addon.xml          — generated from addon.xml.in with library_android="pvr.kofin.so"
  pvr.kofin.so       — stripped ARM32 binary
  icon.png
  resources/
    settings.xml
    language/resource.language.en_gb/strings.po
    data/             — genre mappings, provider mappings
```

addon.xml must have `library_android="pvr.kofin.so"` (not `library_android/armeabi-v7a`). Kodi v22 requires `kodi.binary.global.main` version 2.0.3 and `kodi.binary.instance.pvr` version 9.2.0.

Output goes to `/media/bluecon/docs/IT/kofin/builds/`.
