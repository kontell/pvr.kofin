[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL%20v2+-blue.svg)](LICENSE.md)

# Kofin PVR for Jellyfin

Kodi PVR client addon for [Jellyfin](https://jellyfin.org) Live TV. Provides Kodi PVR integration for live TV channels, EPG, recordings, and timers from a Jellyfin server.

Play catchup/ archive content from EPG in same manner as IPTV simple client.

Forked from [pvr.iptvsimple](https://github.com/kodi-pvr/pvr.iptvsimple), replacing M3U/XMLTV data sources with Jellyfin REST API calls.

**Requires:** Kodi 21 "Omega" or later, Jellyfin 10.9.x or later with Live TV configured.

## Features

- Play Jellyfin Live TV channels from Kodi EPG
- Play Catchup/ Archive from EPG (direct play only)
- Play in-progress recordings from beginning
- Force re-muxing (for stream sharing)
- Optional transcoding
- Pause live tv (local or server buffer)
- One-shot and series recording timers
- inputstream.ffmpegdirect / inputstream.adaptive support
- Login with username/ password or quick connect code

## Installation

Install via the [Kontell Repository](https://github.com/kontell/repository.kontell).

## Configuration

- After install go to: Add-ons -> My add-ons -> PVR clients -> Kofin PVR for Jellyfin -> Configure
- Enter server details and logon
- Reenter settings and restart add-on
- Kodi PVR settings can be found in: Settings -> PVR & Live TV
- Kofin settings can be accessed from: Settings -> PVR & Live TV -> General -> Client specific settings

### Server address

- The server address may be a bare host or IP (e.g. `192.168.1.10`) - `http` and port `8096` are assumed when omitted. **Use `https://` when connecting over the internet**; the addon warns once per session if a plain-HTTP connection targets a non-private host.
- Reverse-proxy **sub-paths are not supported** (e.g. `https://host/jellyfin`) - use a dedicated host and optional port only.
- On login the addon stores a Jellyfin access token (not your password) in Kodi's addon settings. Like all Kodi addon settings it is stored **in plaintext** under `userdata/addon_data/pvr.kofin/` - be aware of this when sharing Kodi backups or your addon_data folder. Logging out revokes the token on the server.

## Catchup
If supported by your IPTV provider catchup works by using inputstream.ffmpegdirect to play directly from the provider (transcoding settings are irrelevant for catchup playback). To use it you must upload a reference playlist that provides the relevant catchup tags which are omitted by Jellyfin. Refer to IPTV Simple Client for detailed catchup documentation.

## Reference Playlist

An optional M3U reference playlist can be configured to apply per-channel properties that Jellyfin doesn't provide. Channels are matched by name (case-insensitive) between the playlist and Jellyfin. The same `#KODIPROP:`, `#EXTVLCOPT:`, and `#EXTVLCOPT--` directives supported by IPTV Simple Client are honored, along with standard M3U tags like `group-title=` for channel grouping.

In addition to standard KodiProps, the following Kofin-specific properties can be set per channel:

| Property | Values | Description |
|----------|--------|-------------|
| `kofin-force-direct-play` | `true` / `false` | Force direct play for this channel |
| `kofin-force-remux` | `true` / `false` | Force remuxing for this channel (overrides global setting) |
| `kofin-force-transcode` | `true` / `false` | Force transcoding for this channel (overrides global setting) |
| `kofin-bitrate-limit` | kbps (e.g. `4000`) | Set a bitrate limit for this channel (0 or omitted = unlimited) |
| `kofin-disable-pvr` | `true` / `false` | Disable recording for this channel — hides the record option in the EPG |

## Supported platforms

| Platform | Kodi 21 (Omega) | Kodi 22 (Piers) |
|----------|----------------|-----------------|
| Linux x86_64 | yes | yes |
| Linux armv7 (Pi 2+) | yes | yes |
| Linux aarch64 (Pi 3+) | yes | yes |
| Android ARM32 | yes | yes |
| Android ARM64 | yes | yes |
| Windows x86_64 | yes | yes |

## License

This project is licensed under the [GPL-2.0-or-later](LICENSE.md) license.
