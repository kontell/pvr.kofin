# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Kodi PVR addon for Jellyfin Live TV. C++17 shared library implementing the Kodi PVR client interface for live TV channels, EPG, recordings, and timers from a Jellyfin server. Forked from pvr.iptvsimple with M3U/XMLTV/catchup/VOD stripped and replaced by Jellyfin API calls. Targets Kodi 21 "Omega" and Kodi 22 "Piers", Jellyfin 10.9.x+.

**Addon ID:** `pvr.kofin`

## Reference Implementation

The playback pipeline (DeviceProfile, PlaybackInfo requests, stream URL construction, transcode/remux/direct play logic) should match the behaviour of the forked jellyfin-kodi Python addon at `/media/bluecon/docs/IT/kofin/jellyfin-kodi/`. That addon is the authoritative reference for how Jellyfin API calls should be structured — compare against it when debugging playback or stream issues.

## Build Commands

```bash
# Local Linux build
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# Android ARM32 cross-compilation (requires NDK r25c + pre-built deps)
mkdir -p build-android && cd build-android
cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=/media/bluecon/docs/IT/kofin/android-ndk-r25c/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=armeabi-v7a -DANDROID_PLATFORM=android-21 \
  -DCMAKE_BUILD_TYPE=Release \
  -DKODI_INCLUDE_DIR=/media/bluecon/docs/IT/kofin/kodi-piers/xbmc/addons/kodi-dev-kit/include \
  -DJSONCPP_ROOT=/media/bluecon/docs/IT/kofin/android-deps \
  -DCMAKE_PREFIX_PATH=/media/bluecon/docs/IT/kofin/android-deps
cmake --build . --config Release
```

Build dependencies: `kodi-addons-dev`, `libjsoncpp-dev`, `libpugixml-dev`, `zlib1g-dev`, `liblzma-dev`.

No unit test suite — testing is manual against a running Jellyfin server on Android TV.

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
- **Live stream lifecycle**: `GetLiveStreamUrl()` POSTs to `/Items/{id}/PlaybackInfo` with `AutoOpenLiveStream=true`, tracks `LiveStreamId`. `CloseLiveStream()` sends `POST /LiveStreams/Close`.
- **Device profile**: HLS with fMP4 segments (AV1) or MPEG-TS segments (H264/HEVC), codec preferences from settings (H264/H265/AV1, AAC/AC3/MP3/Opus), configurable max bitrate. The device profile and PlaybackInfo request should mirror the jellyfin-kodi Python addon at `/media/bluecon/docs/IT/kofin/jellyfin-kodi/`.
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

**Container logic:** The recording device profile always requests TS container. For live TV, the `PostProcessTranscodingUrl()` only sets `SegmentContainer=mp4` (fMP4) when the actual stream codec is AV1 (checked from the URL's `VideoCodec=` param), not based on the preferred codec setting. H264/HEVC streams stay in TS regardless of the preferred codec preference.

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
- `VERSION` — single-line version string, read by CMakeLists.txt

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
